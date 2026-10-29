/**
 * @file Tests for the plugin-side SDK: definePlugin validation plus the three example plugins.
 *
 * @brief All PluginServices fakes are self-written records; no host implementation or other
 *        agent module is imported.
 */
import { describe, expect, it } from 'vitest';
import { definePlugin, typedApi } from '../src/sdk/index.ts';
import type { PluginDescriptor, PluginHooks, RackPlugin } from '../src/sdk/index.ts';
import { isRackError, RackError, statusOf } from '../src/core/status.ts';
import type { RackStatus } from '../src/core/status.ts';
import type { JsonValue } from '../src/core/json.ts';
import type {
  CallService,
  CapEvent,
  CapSink,
  CapsDesc,
  EventSink,
  InitOrderSpec,
  LeaseCredential,
  LeaseHandle,
  PluginServices,
  Revoker,
  Subscription,
  VersionRange,
} from '../src/core/types.ts';
import { create as createEcho } from '../examples/plugins/echo.plug.ts';
import { create as createStorage } from '../examples/plugins/storage.plug.ts';
import { create as createConsumer } from '../examples/plugins/consumer.plug.ts';

// ---------------------------------------------------------------------------
// Self-written fake PluginServices: records publications, subscriptions, lease
// acquisitions, gateway calls and diagnostics.
// ---------------------------------------------------------------------------

/** @brief One recorded CapabilityService.acquire attempt. */
interface RecordedAcquire {
  plugId: string;
  range: VersionRange;
  revoker: Revoker;
}

/** @brief One recorded gateway call. */
interface RecordedCall {
  credential: LeaseCredential;
  methodName: string;
  args: JsonValue;
}

/**
 * @brief Build a fake PluginServices bundle backed by plain records.
 *
 * @param options.acquireError Optional error thrown by every acquire attempt (after recording).
 * @return The services object plus record arrays and event/capability delivery helpers.
 */
function makeFakeServices(options?: { acquireError?: RackError }) {
  const publishedCaps: CapsDesc[] = [];
  const capWatchers: CapSink[] = [];
  const eventSinks = new Map<string, EventSink[]>();
  const acquisitions: RecordedAcquire[] = [];
  const releases: LeaseCredential[] = [];
  const calls: RecordedCall[] = [];
  const diagnostics: Array<{ level: string; message: string }> = [];
  const mintedHandles: LeaseHandle[] = [];
  let credentialSeq = 0;

  const services: PluginServices = {
    plugId: 'test.consumer',
    events: {
      subscribe(name: string, sink: EventSink): Subscription {
        const existing = eventSinks.get(name) ?? [];
        existing.push(sink);
        eventSinks.set(name, existing);
        let live = true;
        return {
          unsubscribe() {
            if (!live) return;
            live = false;
            const remaining = (eventSinks.get(name) ?? []).filter((entry) => entry !== sink);
            eventSinks.set(name, remaining);
          },
        };
      },
      publish() {
        // The plugins under test publish no ordinary events; nothing to deliver.
      },
    },
    caps: {
      publish(caps: CapsDesc): void {
        publishedCaps.push(caps);
      },
      watch(sink: CapSink): Subscription {
        capWatchers.push(sink);
        let live = true;
        return {
          unsubscribe() {
            if (!live) return;
            live = false;
            const index = capWatchers.indexOf(sink);
            if (index >= 0) capWatchers.splice(index, 1);
          },
        };
      },
      snapshot(): readonly CapEvent[] {
        return [];
      },
      acquire(plugId: string, range: VersionRange, revoker: Revoker): LeaseHandle {
        acquisitions.push({ plugId, range, revoker });
        if (options?.acquireError) throw options.acquireError;
        credentialSeq += 1;
        const credential: LeaseCredential = {
          kind: 'u42.lease.credential',
          handle: { seq: credentialSeq },
        };
        const handle: LeaseHandle = {
          credential,
          plugId,
          version: { major: 1, minor: 2, patch: 0 },
          release(): Promise<'ok' | 'busy' | 'stale'> {
            releases.push(credential);
            return Promise.resolve('ok');
          },
        };
        mintedHandles.push(handle);
        return handle;
      },
      release(credential: LeaseCredential): 'ok' | 'busy' | 'stale' {
        releases.push(credential);
        return 'ok';
      },
    },
    calls: {
      call(credential: LeaseCredential, methodName: string, args: JsonValue): Promise<JsonValue> {
        calls.push({ credential, methodName, args });
        return Promise.resolve(null);
      },
      callById(credential: LeaseCredential, methodId: number, args: JsonValue): Promise<JsonValue> {
        calls.push({ credential, methodName: `#${methodId}`, args });
        return Promise.resolve(null);
      },
    },
    diag: {
      report(level: string, message: string): void {
        diagnostics.push({ level, message });
      },
    },
  };

  return {
    services,
    publishedCaps,
    acquisitions,
    releases,
    calls,
    diagnostics,
    mintedHandles,
    watcherCount(): number {
      return capWatchers.length;
    },
    sinkCount(name: string): number {
      return (eventSinks.get(name) ?? []).length;
    },
    async deliverEvent(name: string, payload: JsonValue): Promise<void> {
      for (const sink of [...(eventSinks.get(name) ?? [])]) await sink.onEvent({ name, payload });
    },
    async deliverCapability(event: CapEvent): Promise<void> {
      for (const sink of [...capWatchers]) await sink.onCapability(event);
    },
  };
}

// ---------------------------------------------------------------------------
// Shared helpers.
// ---------------------------------------------------------------------------

/** @brief A minimal well-formed descriptor for positive definePlugin cases. */
function validDesc(): PluginDescriptor {
  return { plugId: 'test.plugin', version: { major: 0, minor: 1, patch: 0 }, init: {} };
}

/** @brief Storage availability fact for the given provider version. */
function storageAvailable(major = 1, minor = 2, patch = 0): CapEvent {
  return {
    plugId: 'example.storage',
    available: true,
    version: { major, minor, patch },
    capabilities: {
      methods: [
        { id: 1, name: 'set' },
        { id: 2, name: 'get' },
      ],
    },
  };
}

/** @brief Storage withdrawal fact; methods are empty on withdrawal. */
function storageWithdrawn(): CapEvent {
  return {
    plugId: 'example.storage',
    available: false,
    version: { major: 1, minor: 2, patch: 0 },
    capabilities: { methods: [] },
  };
}

/** @brief Status method request reused across consumer assertions. */
const statusRequest = { methodId: 1, methodName: 'status', args: null } as const;

/**
 * @brief Assert that one invoke dispatch throws a RackError with the expected status.
 *
 * @param plugin Plugin under test.
 * @param methodId Numeric method selector.
 * @param methodName Name selector.
 * @param status Expected RackStatus.
 * @param args Request args document.
 */
async function expectInvokeError(
  plugin: RackPlugin,
  methodId: number,
  methodName: string,
  status: RackStatus,
  args: JsonValue = null,
): Promise<void> {
  try {
    await plugin.invoke?.({ methodId, methodName, args });
    expect.unreachable();
  } catch (error) {
    expect(isRackError(error)).toBe(true);
    expect(statusOf(error)).toBe(status);
  }
}

/**
 * @brief Assert that a definePlugin call rejects with 'invalid_argument'.
 *
 * @param run Callback performing the invalid construction.
 */
async function expectInvalidArgument(run: () => unknown): Promise<void> {
  try {
    await run();
    expect.unreachable();
  } catch (error) {
    expect(isRackError(error)).toBe(true);
    expect(statusOf(error)).toBe('invalid_argument');
  }
}

// ---------------------------------------------------------------------------
// definePlugin: descriptor shape and hook wiring.
// ---------------------------------------------------------------------------

describe('definePlugin descriptor validation', () => {
  it('accepts a minimal descriptor and freezes a defensive copy', () => {
    const before = ['friend.a'];
    const plugin = definePlugin(
      { plugId: 'test.plugin', version: { major: 1, minor: 0, patch: 0 }, init: { before } },
      {},
    );
    expect(plugin.desc.plugId).toBe('test.plugin');
    expect(plugin.desc.version).toEqual({ major: 1, minor: 0, patch: 0 });
    expect(plugin.desc.init.before).toEqual(['friend.a']);
    expect(Object.isFrozen(plugin.desc)).toBe(true);
    expect(Object.isFrozen(plugin.desc.init)).toBe(true);
    expect(Object.isFrozen(plugin.desc.init.before)).toBe(true);
    before.push('friend.b'); // caller-side mutation must not leak into the plugin
    expect(plugin.desc.init.before).toEqual(['friend.a']);
  });

  it('keeps an explicit integer priority and empty edges', () => {
    const plugin = definePlugin({ ...validDesc(), init: { priority: -3, before: [], after: [] } }, {});
    expect(plugin.desc.init.priority).toBe(-3);
    expect(plugin.desc.init.before).toEqual([]);
    expect(plugin.desc.init.after).toEqual([]);
  });

  it('rejects malformed descriptors and hook bundles with invalid_argument', async () => {
    await expectInvalidArgument(() => definePlugin(null as unknown as PluginDescriptor, {}));
    await expectInvalidArgument(() => definePlugin({ ...validDesc(), plugId: '' }, {}));
    await expectInvalidArgument(() =>
      definePlugin({ ...validDesc(), plugId: 7 as unknown as string }, {}),
    );
    await expectInvalidArgument(() =>
      definePlugin({ plugId: 'test.plugin', init: {} } as unknown as PluginDescriptor, {}),
    );
    await expectInvalidArgument(() =>
      definePlugin(
        {
          ...validDesc(),
          version: { major: '1', minor: 0, patch: 0 } as unknown as PluginDescriptor['version'],
        },
        {},
      ),
    );
    await expectInvalidArgument(() =>
      definePlugin({ ...validDesc(), version: { major: -1, minor: 0, patch: 0 } }, {}),
    );
    await expectInvalidArgument(() =>
      definePlugin({ ...validDesc(), version: { major: 1.5, minor: 0, patch: 0 } }, {}),
    );
    await expectInvalidArgument(() =>
      definePlugin(
        { plugId: 'test.plugin', version: { major: 1, minor: 0, patch: 0 } } as unknown as PluginDescriptor,
        {},
      ),
    );
    await expectInvalidArgument(() =>
      definePlugin({ ...validDesc(), init: null as unknown as InitOrderSpec }, {}),
    );
    await expectInvalidArgument(() =>
      definePlugin({ ...validDesc(), init: { before: 'friend.a' } as unknown as InitOrderSpec }, {}),
    );
    await expectInvalidArgument(() =>
      definePlugin({ ...validDesc(), init: { before: ['a', 2] } as unknown as InitOrderSpec }, {}),
    );
    await expectInvalidArgument(() =>
      definePlugin({ ...validDesc(), init: { after: [null] } as unknown as InitOrderSpec }, {}),
    );
    await expectInvalidArgument(() => definePlugin({ ...validDesc(), init: { priority: 1.5 } }, {}));
    await expectInvalidArgument(() =>
      definePlugin({ ...validDesc(), init: { priority: 'high' } as unknown as InitOrderSpec }, {}),
    );
    await expectInvalidArgument(() => definePlugin(validDesc(), null as unknown as PluginHooks));
    await expectInvalidArgument(() =>
      definePlugin(validDesc(), { start: 1 } as unknown as PluginHooks),
    );
  });
});

describe('definePlugin hook wiring', () => {
  it('routes lifecycle and invoke through the provided hooks', async () => {
    const trace: string[] = [];
    const fake = makeFakeServices();
    const plugin = definePlugin(validDesc(), {
      init: (services) => {
        trace.push(`init:${services.plugId}`);
      },
      start: () => {
        trace.push('start');
      },
      stop: () => {
        trace.push('stop');
      },
      destroy: () => {
        trace.push('destroy');
      },
      invoke: (request) => ({ method: request.methodName, args: request.args }),
    });
    await plugin.init(fake.services);
    await plugin.start();
    expect(await plugin.invoke?.({ methodId: 1, methodName: 'm', args: 42 })).toEqual({
      method: 'm',
      args: 42,
    });
    await plugin.stop();
    await plugin.destroy?.();
    expect(trace).toEqual(['init:test.consumer', 'start', 'stop', 'destroy']);
  });

  it('defaults missing hooks to no-ops and omits optional entries', async () => {
    const fake = makeFakeServices();
    const plugin = definePlugin(validDesc());
    await plugin.init(fake.services);
    await plugin.start();
    await plugin.stop();
    expect(plugin.destroy).toBeUndefined();
    expect(plugin.invoke).toBeUndefined();
  });
});

// ---------------------------------------------------------------------------
// Example echo plugin.
// ---------------------------------------------------------------------------

describe('example echo plugin', () => {
  it('announces identity and both methods on start', async () => {
    const fake = makeFakeServices();
    const echo = createEcho();
    expect(echo.desc.plugId).toBe('example.echo');
    expect(echo.desc.version).toEqual({ major: 1, minor: 0, patch: 0 });
    await echo.init(fake.services);
    await echo.start();
    expect(fake.publishedCaps).toHaveLength(1);
    const methods = fake.publishedCaps[0]?.methods ?? [];
    expect(methods.map((method) => [method.id, method.name])).toEqual([
      [1, 'echo'],
      [2, 'fail'],
    ]);
  });

  it('echoes the args document verbatim', async () => {
    const echo = createEcho();
    const args = { hello: ['42u', 42, true, null], nested: { deep: -1 } };
    expect(await echo.invoke?.({ methodId: 1, methodName: 'echo', args })).toEqual(args);
    // Name dispatch resolves identically to the numeric entry.
    expect(await echo.invoke?.({ methodId: 0, methodName: 'echo', args: 'plain' })).toBe('plain');
  });

  it('fails deliberately on the fail method and rejects unknown methods', async () => {
    const echo = createEcho();
    await expectInvokeError(echo, 2, 'fail', 'failed');
    await expectInvokeError(echo, 99, 'nope', 'not_found');
  });
});

// ---------------------------------------------------------------------------
// Example storage plugin.
// ---------------------------------------------------------------------------

describe('example storage plugin', () => {
  it('announces set/get on start and keeps state across invoke calls', async () => {
    const fake = makeFakeServices();
    const storage = createStorage();
    expect(storage.desc.plugId).toBe('example.storage');
    expect(storage.desc.version).toEqual({ major: 1, minor: 2, patch: 0 });
    await storage.init(fake.services);
    await storage.start();
    expect(fake.publishedCaps[0]?.methods.map((method) => [method.id, method.name])).toEqual([
      [1, 'set'],
      [2, 'get'],
    ]);
    expect(
      await storage.invoke?.({ methodId: 1, methodName: 'set', args: { key: 'answer', value: 42 } }),
    ).toBeNull();
    expect(await storage.invoke?.({ methodId: 2, methodName: 'get', args: { key: 'answer' } })).toBe(42);
    expect(await storage.invoke?.({ methodId: 2, methodName: 'get', args: { key: 'missing' } })).toBeNull();
    // Overwrite demonstrates the instance-internal Map is the single source of truth.
    await storage.invoke?.({ methodId: 1, methodName: 'set', args: { key: 'answer', value: 'new' } });
    expect(await storage.invoke?.({ methodId: 2, methodName: 'get', args: { key: 'answer' } })).toBe('new');
  });

  it('isolates state between two instances', async () => {
    const first = createStorage();
    const second = createStorage();
    await first.invoke?.({ methodId: 1, methodName: 'set', args: { key: 'k', value: [1, 2] } });
    expect(await second.invoke?.({ methodId: 2, methodName: 'get', args: { key: 'k' } })).toBeNull();
  });

  it('rejects malformed arguments and unknown methods', async () => {
    const storage = createStorage();
    await expectInvokeError(storage, 1, 'set', 'invalid_argument', null);
    await expectInvokeError(storage, 1, 'set', 'invalid_argument', { key: 5, value: 1 });
    await expectInvokeError(storage, 1, 'set', 'invalid_argument', { key: 'k' });
    await expectInvokeError(storage, 2, 'get', 'invalid_argument', 'k');
    await expectInvokeError(storage, 2, 'get', 'invalid_argument', []);
    await expectInvokeError(storage, 7, 'wipe', 'not_found');
  });
});

// ---------------------------------------------------------------------------
// Example consumer plugin.
// ---------------------------------------------------------------------------

describe('example consumer plugin', () => {
  it('subscribes in init, publishes its status method on start, counts events without a lease', async () => {
    const fake = makeFakeServices();
    const consumer = createConsumer();
    expect(consumer.desc.plugId).toBe('example.consumer');
    expect(consumer.desc.version).toEqual({ major: 1, minor: 0, patch: 0 });
    expect(consumer.desc.init.after).toEqual(['example.storage']);
    await consumer.init(fake.services);
    expect(fake.watcherCount()).toBe(1);
    expect(fake.sinkCount('example.data')).toBe(1);
    await consumer.start();
    expect(fake.publishedCaps[0]?.methods.map((method) => [method.id, method.name])).toEqual([
      [1, 'status'],
    ]);
    await fake.deliverEvent('example.data', { n: 1 });
    expect(await consumer.invoke?.(statusRequest)).toEqual({ eventsProcessed: 1, leasesHeld: 0 });
    expect(fake.calls).toHaveLength(0);
  });

  it('borrows an exact [1.2.0,1.2.0] storage lease after the availability event', async () => {
    const fake = makeFakeServices();
    const consumer = createConsumer();
    await consumer.init(fake.services);
    await consumer.start();
    // Out-of-range offer must not open business.
    await fake.deliverCapability(storageAvailable(1, 3, 0));
    expect(fake.acquisitions).toHaveLength(0);
    expect(await consumer.invoke?.(statusRequest)).toEqual({ eventsProcessed: 0, leasesHeld: 0 });
    // In-range offer acquires once with the fixed closed range.
    await fake.deliverCapability(storageAvailable());
    expect(fake.acquisitions).toHaveLength(1);
    expect(fake.acquisitions[0]?.plugId).toBe('example.storage');
    expect(fake.acquisitions[0]?.range.minimum).toEqual({ major: 1, minor: 2, patch: 0 });
    expect(fake.acquisitions[0]?.range.maximum).toEqual({ major: 1, minor: 2, patch: 0 });
    expect(fake.acquisitions[0]?.revoker).toBeDefined();
    expect(await consumer.invoke?.(statusRequest)).toEqual({ eventsProcessed: 0, leasesHeld: 1 });
    // A still-live lease is never re-borrowed on repeated availability.
    await fake.deliverCapability(storageAvailable());
    expect(fake.acquisitions).toHaveLength(1);
  });

  it('forwards data events through the gateway as set/get under the lease credential', async () => {
    const fake = makeFakeServices();
    const consumer = createConsumer();
    await consumer.init(fake.services);
    await consumer.start();
    await fake.deliverCapability(storageAvailable());
    await fake.deliverEvent('example.data', { hello: '42u' });
    // Two typed proxy calls (set, get) plus one direct gateway call, all under one credential.
    expect(fake.calls).toHaveLength(3);
    expect(fake.calls[0]?.methodName).toBe('set');
    expect(fake.calls[0]?.args).toEqual({ key: 'example.data', value: { hello: '42u' } });
    expect(fake.calls[1]?.methodName).toBe('get');
    expect(fake.calls[1]?.args).toEqual({ key: 'example.data' });
    expect(fake.calls[2]?.methodName).toBe('get');
    expect(fake.calls[2]?.args).toEqual({ key: 'example.data' });
    for (const call of fake.calls) {
      expect(call.credential).toBe(fake.mintedHandles[0]?.credential);
    }
    expect(await consumer.invoke?.(statusRequest)).toEqual({ eventsProcessed: 1, leasesHeld: 1 });
  });

  it('stops forwarding after withdrawal while the credential is kept for revocation', async () => {
    const fake = makeFakeServices();
    const consumer = createConsumer();
    await consumer.init(fake.services);
    await consumer.start();
    await fake.deliverCapability(storageAvailable());
    await fake.deliverCapability(storageWithdrawn());
    await fake.deliverEvent('example.data', 1);
    expect(fake.calls).toHaveLength(0);
    // Withdrawal forbids business but keeps the lease for the coming onRevoke().
    expect(await consumer.invoke?.(statusRequest)).toEqual({ eventsProcessed: 1, leasesHeld: 1 });
  });

  it('clears state and returns the credential on revoke', async () => {
    const fake = makeFakeServices();
    const consumer = createConsumer();
    await consumer.init(fake.services);
    await consumer.start();
    await fake.deliverCapability(storageAvailable());
    const revoker = fake.acquisitions[0]?.revoker;
    const credential = fake.mintedHandles[0]?.credential;
    expect(credential).toBeDefined();
    await revoker?.onRevoke(credential as LeaseCredential);
    expect(fake.releases).toEqual([credential]);
    // After the revocation the cached lease is gone: events no longer reach storage.
    await fake.deliverEvent('example.data', 2);
    expect(fake.calls).toHaveLength(0);
    expect(await consumer.invoke?.(statusRequest)).toEqual({ eventsProcessed: 1, leasesHeld: 0 });
    // An unrelated credential is ignored.
    await revoker?.onRevoke({ kind: 'u42.lease.credential', handle: { seq: 'other' } });
    expect(fake.releases).toHaveLength(1);
  });

  it('survives a failing acquisition with a diagnostic', async () => {
    const fake = makeFakeServices({
      acquireError: new RackError('unsupported', 'no active provider'),
    });
    const consumer = createConsumer();
    await consumer.init(fake.services);
    await consumer.start();
    await fake.deliverCapability(storageAvailable());
    expect(fake.acquisitions).toHaveLength(1);
    expect(fake.diagnostics.some((entry) => entry.level === 'warn')).toBe(true);
    expect(await consumer.invoke?.(statusRequest)).toEqual({ eventsProcessed: 0, leasesHeld: 0 });
  });

  it('releases subscriptions and the lease on stop', async () => {
    const fake = makeFakeServices();
    const consumer = createConsumer();
    await consumer.init(fake.services);
    await consumer.start();
    await fake.deliverCapability(storageAvailable());
    const credential = fake.mintedHandles[0]?.credential;
    await consumer.stop();
    expect(fake.watcherCount()).toBe(0);
    expect(fake.sinkCount('example.data')).toBe(0);
    expect(fake.releases).toEqual([credential]);
  });
});

// ---------------------------------------------------------------------------
// typedApi direct-call proxy.
// ---------------------------------------------------------------------------

/** @brief The exact-version range used by the typedApi fixtures below. */
const exactRange = (): VersionRange => ({
  minimum: { major: 1, minor: 2, patch: 0 },
  maximum: { major: 1, minor: 2, patch: 0 },
});

describe('typedApi direct-call proxy', () => {
  it('forwards string properties with packed args and the lease credential', async () => {
    const fake = makeFakeServices();
    const handle = fake.services.caps.acquire('example.storage', exactRange(), { onRevoke() {} });
    type Ping = (...args: JsonValue[]) => Promise<JsonValue>;
    const api = typedApi<{ ping: Ping }>(fake.services.calls, handle);
    expect(typeof api.ping).toBe('function');
    await api.ping();
    await api.ping({ a: 1 });
    await api.ping(1, 'x', null);
    expect(fake.calls.map((call) => [call.methodName, call.args])).toEqual([
      ['ping', null],
      ['ping', { a: 1 }],
      ['ping', [1, 'x', null]],
    ]);
    expect(fake.calls.every((call) => call.credential === handle.credential)).toBe(true);
  });

  it('resolves symbols and "then" to undefined and stays stateless', () => {
    const fake = makeFakeServices();
    const handle = fake.services.caps.acquire('example.storage', exactRange(), { onRevoke() {} });
    const api = typedApi<Record<string, (() => Promise<JsonValue>) | undefined>>(
      fake.services.calls,
      handle,
    );
    expect(api['then']).toBeUndefined();
    expect((api as unknown as Record<symbol, unknown>)[Symbol.toPrimitive]).toBeUndefined();
    expect(api['ping']).not.toBe(api['ping']); // fresh forwarder per access, nothing cached
  });

  it('rejects with stale from the gateway after the lease is released', async () => {
    let live = true;
    const recorded: Array<{ methodName: string; args: JsonValue }> = [];
    const calls: CallService = {
      call(_credential, methodName, args) {
        if (!live) return Promise.reject(new RackError('stale', 'lease already returned'));
        recorded.push({ methodName, args });
        return Promise.resolve(null);
      },
      callById(_credential, methodId, args) {
        if (!live) return Promise.reject(new RackError('stale', 'lease already returned'));
        recorded.push({ methodName: `#${methodId}`, args });
        return Promise.resolve(null);
      },
    };
    const handle: LeaseHandle = {
      credential: { kind: 'u42.lease.credential', handle: { seq: 'stale-test' } },
      plugId: 'example.storage',
      version: { major: 1, minor: 2, patch: 0 },
      release() {
        live = false;
        return Promise.resolve('ok');
      },
    };
    const api = typedApi<{ ping: (...args: JsonValue[]) => Promise<JsonValue> }>(calls, handle);
    await api.ping('before');
    await handle.release();
    await expect(api.ping('after')).rejects.toThrowError(RackError);
    expect(recorded).toEqual([{ methodName: 'ping', args: 'before' }]);
  });
});
