/**
 * @file Tests for the per-instance plugin services view (createPluginServices).
 *
 * @brief Verifies owner binding, the two state gates (capability publication and Active-only
 *        business calls) and pure delegation to the deps ports, using self-written fake ports
 *        that implement the frozen interconnection interfaces.
 */
import { describe, expect, it } from 'vitest';
import type {
  CapEntry,
  CapEvent,
  CapSink,
  CapsDesc,
  CapabilityRegistry,
  DiagLevel,
  EventBus,
  EventSink,
  InvokeGateway,
  LeaseCallGuard,
  LeaseCredential,
  LeaseHandle,
  LeaseManager,
  PluginServices,
  PluginVersion,
  Revoker,
  Subscription,
  VersionRange,
} from '../src/core/types.ts';
import type { JsonValue } from '../src/core/json.ts';
import { createPluginServices } from '../src/core/services.ts';
import { statusOf } from '../src/core/status.ts';

const v = (major: number, minor: number, patch: number): PluginVersion => ({ major, minor, patch });

/** @brief Minimal credential factory; the payload stays opaque by contract. */
const makeCredential = (): LeaseCredential => ({ kind: 'u42.lease.credential', handle: {} });

/** @brief Subscription double counting its unsubscribe() calls. */
class FakeSubscription implements Subscription {
  unsubscribed = 0;

  unsubscribe(): void {
    this.unsubscribed += 1;
  }
}

/** @brief EventBus double recording owner-attributed subscription and publication traffic. */
class FakeBus implements EventBus {
  readonly subscriptions: { name: string; ownerId: string; sink: EventSink }[] = [];
  readonly publications: { name: string; payload: JsonValue; publisherId: string }[] = [];
  lastSubscription: FakeSubscription | undefined;

  subscribe(name: string, sink: EventSink, ownerId: string): Subscription {
    this.subscriptions.push({ name, ownerId, sink });
    this.lastSubscription = new FakeSubscription();
    return this.lastSubscription;
  }

  publish(name: string, payload: JsonValue, publisherId: string): void {
    this.publications.push({ name, payload, publisherId });
  }

  unsubscribeAll(ownerId: string): void {
    // Not reachable through the per-instance view; unload is a host action.
  }

  async drain(budget: number): Promise<number> {
    return 0;
  }

  get pending(): number {
    return 0;
  }
}

/** @brief CapabilityRegistry double recording watches; registration stays host-side. */
class FakeRegistry implements CapabilityRegistry {
  readonly watches: { sink: CapSink; watcherId: string }[] = [];
  readonly registered: CapEntry[] = [];
  readonly withdrawn: { plugId: string; generation: number }[] = [];
  lastWatchSubscription: (FakeSubscription & { readonly snapshot: readonly CapEvent[] }) | undefined;
  snapshotValue: readonly CapEvent[] = [];

  register(entry: CapEntry): void {
    this.registered.push(entry);
  }

  withdraw(plugId: string, generation: number): void {
    this.withdrawn.push({ plugId, generation });
  }

  watch(sink: CapSink, watcherId: string): Subscription & { readonly snapshot: readonly CapEvent[] } {
    this.watches.push({ sink, watcherId });
    this.lastWatchSubscription = Object.assign(new FakeSubscription(), { snapshot: this.snapshotValue });
    return this.lastWatchSubscription;
  }

  snapshot(): readonly CapEvent[] {
    return this.snapshotValue;
  }

  lookup(plugId: string): CapEntry | undefined {
    return undefined;
  }
}

/** @brief LeaseManager double recording consumer-bound acquisitions and releases. */
class FakeLeases implements LeaseManager {
  readonly acquisitions: { consumerId: string; plugId: string; range: VersionRange; revoker: Revoker }[] = [];
  readonly releases: { credential: LeaseCredential; consumerId: string }[] = [];
  readonly acquireResult: LeaseHandle;
  releaseResult: 'ok' | 'busy' | 'stale' = 'ok';

  constructor() {
    this.acquireResult = {
      credential: makeCredential(),
      plugId: 'com.example.provider',
      version: v(1, 0, 0),
      release: () => Promise.resolve('ok'),
    };
  }

  acquire(consumerId: string, plugId: string, range: VersionRange, revoker: Revoker): LeaseHandle {
    this.acquisitions.push({ consumerId, plugId, range, revoker });
    return this.acquireResult;
  }

  release(credential: LeaseCredential, consumerId: string): 'ok' | 'busy' | 'stale' {
    this.releases.push({ credential, consumerId });
    return this.releaseResult;
  }

  beginCall(credential: LeaseCredential, consumerId: string): LeaseCallGuard {
    throw new Error('beginCall is not reachable through the per-instance view');
  }

  async revokeProvider(providerPlugId: string, generation: number): Promise<'ok' | 'failed'> {
    return 'ok';
  }

  releaseAllFrom(consumerId: string): void {
    // Host-side unload action; not reachable through the per-instance view.
  }

  hasOutstandingFor(providerPlugId: string, generation: number): boolean {
    return false;
  }

  get activeCount(): number {
    return 0;
  }
}

/** @brief InvokeGateway double recording the exact method selectors it receives. */
class FakeGateway implements InvokeGateway {
  readonly received: {
    consumerId: string;
    credential: LeaseCredential;
    method: { readonly byName: string } | { readonly byId: number };
    args: JsonValue;
  }[] = [];
  result: JsonValue = 'gateway-result';

  async call(
    consumerId: string,
    credential: LeaseCredential,
    method: { readonly byName: string } | { readonly byId: number },
    args: JsonValue,
  ): Promise<JsonValue> {
    this.received.push({ consumerId, credential, method, args });
    return this.result;
  }
}

/** @brief One wired harness: real view over fake ports plus mutable state gates. */
interface Harness {
  readonly services: PluginServices;
  readonly flags: { capsAllowed: boolean; active: boolean };
  readonly bus: FakeBus;
  readonly registry: FakeRegistry;
  readonly leases: FakeLeases;
  readonly gateway: FakeGateway;
  readonly diag: { level: DiagLevel; message: string; detail: JsonValue | undefined }[];
  readonly published: CapsDesc[];
}

function makeHarness(plugId = 'com.example.consumer'): Harness {
  const bus = new FakeBus();
  const registry = new FakeRegistry();
  const leases = new FakeLeases();
  const gateway = new FakeGateway();
  const flags = { capsAllowed: false, active: false };
  const diag: { level: DiagLevel; message: string; detail: JsonValue | undefined }[] = [];
  const published: CapsDesc[] = [];
  const services = createPluginServices(
    {
      bus,
      registry,
      leases,
      calls: gateway,
      capsPublishAllowed: () => flags.capsAllowed,
      consumerActive: () => flags.active,
      onDiag: (level, message, detail) => {
        diag.push({ level, message, detail });
      },
      onCapsPublished: (caps) => {
        published.push(caps);
      },
    },
    plugId,
  );
  return { services, flags, bus, registry, leases, gateway, diag, published };
}

describe('createPluginServices', () => {
  it('exposes the owning plugId', () => {
    const harness = makeHarness('com.example.owner');
    expect(harness.services.plugId).toBe('com.example.owner');
  });

  it('binds event subscriptions and publications to the owner', () => {
    const harness = makeHarness('com.example.events');
    const sink: EventSink = { onEvent: () => undefined };

    const subscription = harness.services.events.subscribe('app.tick', sink);
    expect(harness.bus.subscriptions).toEqual([{ name: 'app.tick', ownerId: 'com.example.events', sink }]);
    expect(subscription).toBe(harness.bus.lastSubscription);

    harness.services.events.publish('app.tick', { count: 1 });
    expect(harness.bus.publications).toEqual([
      { name: 'app.tick', payload: { count: 1 }, publisherId: 'com.example.events' },
    ]);
  });

  it('rejects capability publication outside start() and records it when allowed', () => {
    const harness = makeHarness('com.example.caps');
    const caps: CapsDesc = { methods: [{ id: 1, name: 'run' }] };

    try {
      harness.services.caps.publish(caps);
      expect.unreachable();
    } catch (error) {
      expect(statusOf(error)).toBe('invalid_state');
    }
    expect(harness.published).toEqual([]);
    expect(harness.registry.registered).toEqual([]);

    harness.flags.capsAllowed = true;
    harness.services.caps.publish(caps);
    expect(harness.published).toEqual([caps]);
    // The side channel is the only registration path; the view never registers on its own.
    expect(harness.registry.registered).toEqual([]);
  });

  it('tolerates a host that supplies no onCapsPublished side channel', () => {
    const harness = makeHarness('com.example.caps');
    const services = createPluginServices(
      {
        bus: harness.bus,
        registry: harness.registry,
        leases: harness.leases,
        calls: harness.gateway,
        capsPublishAllowed: () => true,
        consumerActive: () => true,
        onDiag: () => undefined,
      },
      'com.example.caps',
    );
    expect(() => services.caps.publish({ methods: [] })).not.toThrow();
  });

  it('passes watch and snapshot straight through the registry', () => {
    const harness = makeHarness('com.example.watcher');
    const capEvents: readonly CapEvent[] = [
      {
        plugId: 'com.example.provider',
        available: true,
        version: v(1, 2, 3),
        capabilities: { methods: [] },
      },
    ];
    harness.registry.snapshotValue = capEvents;
    const sink: CapSink = { onCapability: () => undefined };

    const subscription = harness.services.caps.watch(sink);
    expect(harness.registry.watches).toEqual([{ sink, watcherId: 'com.example.watcher' }]);
    expect(harness.services.caps.snapshot()).toBe(capEvents);

    subscription.unsubscribe();
    expect(harness.registry.lastWatchSubscription?.unsubscribed).toBe(1);
    // The registry-issued handle is adapted down to the plain Subscription contract.
    expect('snapshot' in subscription).toBe(false);
  });

  it('acquires leases with this plugin as the consumer', () => {
    const harness = makeHarness('com.example.consumer');
    const revoker: Revoker = { onRevoke: () => undefined };
    const range: VersionRange = { minimum: v(1, 0, 0), maximum: v(1, 9, 9) };

    const handle = harness.services.caps.acquire('com.example.provider', range, revoker);
    expect(harness.leases.acquisitions).toEqual([
      { consumerId: 'com.example.consumer', plugId: 'com.example.provider', range, revoker },
    ]);
    expect(handle).toBe(harness.leases.acquireResult);
  });

  it('releases credentials under the owner identity in every state', () => {
    const harness = makeHarness('com.example.consumer');
    const credential = makeCredential();
    harness.leases.releaseResult = 'busy';

    expect(harness.services.caps.release(credential)).toBe('busy');
    expect(harness.leases.releases).toEqual([{ credential, consumerId: 'com.example.consumer' }]);

    // Recovery stays available regardless of the state gates.
    harness.flags.capsAllowed = false;
    harness.flags.active = false;
    harness.leases.releaseResult = 'ok';
    expect(harness.services.caps.release(makeCredential())).toBe('ok');
  });

  it('rejects business calls from a non-Active consumer', async () => {
    const harness = makeHarness('com.example.caller');
    const credential = makeCredential();
    harness.flags.active = false;

    const attempts: (() => unknown)[] = [
      () => harness.services.calls.call(credential, 'run', null),
      () => harness.services.calls.callById(credential, 7, null),
    ];
    for (const attempt of attempts) {
      try {
        await attempt();
        expect.unreachable();
      } catch (error) {
        expect(statusOf(error)).toBe('invalid_state');
      }
    }
    expect(harness.gateway.received).toEqual([]);
  });

  it('forwards Active calls by name and by id through the gateway', async () => {
    const harness = makeHarness('com.example.caller');
    harness.flags.active = true;
    const credential = makeCredential();

    const byName = await harness.services.calls.call(credential, 'run', { x: 1 });
    const byId = await harness.services.calls.callById(credential, 7, [1, 2]);

    expect(byName).toBe('gateway-result');
    expect(byId).toBe('gateway-result');
    expect(harness.gateway.received).toEqual([
      { consumerId: 'com.example.caller', credential, method: { byName: 'run' }, args: { x: 1 } },
      { consumerId: 'com.example.caller', credential, method: { byId: 7 }, args: [1, 2] },
    ]);
  });

  it('prefixes diagnostics with the owning plugId', () => {
    const harness = makeHarness('com.example.diag');

    harness.services.diag.report('warn', 'disk low', { free: 12 });
    harness.services.diag.report('error', 'boom');

    expect(harness.diag).toEqual([
      { level: 'warn', message: '[com.example.diag] disk low', detail: { free: 12 } },
      { level: 'error', message: '[com.example.diag] boom', detail: undefined },
    ]);
  });
});
