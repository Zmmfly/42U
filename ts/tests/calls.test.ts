/**
 * @file Tests for the invoke gateway: shared name/id path, validation chain, limits, reentrancy.
 */
import { afterEach, describe, expect, it } from 'vitest';
import { createInvokeGateway } from '../src/core/calls.ts';
import { RackError, type RackStatus } from '../src/core/status.ts';
import type { JsonValue } from '../src/core/json.ts';
import type {
  CapEntry,
  InvokeGateway,
  InvokeRequest,
  LeaseCredential,
  LeaseHandle,
  LeaseManager,
  MethodDesc,
  PluginVersion,
  ProviderEndpoint,
  Revoker,
  VersionRange,
} from '../src/core/types.ts';

const V1: PluginVersion = { major: 1, minor: 0, patch: 0 };

const md = (id: number, name: string): MethodDesc => ({ id, name });

const entry = (plugId: string, generation: number, methods: readonly MethodDesc[]): CapEntry => ({
  plugId,
  generation,
  version: V1,
  caps: { methods },
});

interface FakeLeaseRecord {
  readonly credential: LeaseCredential;
  readonly consumerId: string;
  readonly providerPlugId: string;
  readonly providerGeneration: number;
  readonly version: PluginVersion;
  inFlight: number;
  live: boolean;
}

/** @brief Minimal in-memory LeaseManager double: mint / beginCall / endCall / release counters. */
class FakeLeaseManager implements LeaseManager {
  private readonly records = new Map<object, FakeLeaseRecord>();
  private nextToken = 0;
  guards = 0;
  ends = 0;

  mint(consumerId: string, providerPlugId: string, providerGeneration: number): LeaseCredential {
    return this.mintRecord(consumerId, providerPlugId, providerGeneration).credential;
  }

  private mintRecord(
    consumerId: string,
    providerPlugId: string,
    providerGeneration: number,
  ): FakeLeaseRecord {
    const handle = { token: this.nextToken++ };
    const record: FakeLeaseRecord = {
      credential: { kind: 'u42.lease.credential', handle },
      consumerId,
      providerPlugId,
      providerGeneration,
      version: { major: 1, minor: 0, patch: 0 },
      inFlight: 0,
      live: true,
    };
    this.records.set(handle, record);
    return record;
  }

  acquire(consumerId: string, plugId: string, _range: VersionRange, _revoker: Revoker): LeaseHandle {
    const record = this.mintRecord(consumerId, plugId, 1);
    return {
      credential: record.credential,
      plugId,
      version: record.version,
      release: async () => this.release(record.credential, consumerId),
    };
  }

  release(credential: LeaseCredential, consumerId: string): 'ok' | 'busy' | 'stale' {
    const record = this.records.get(credential.handle);
    if (record === undefined || !record.live) return 'stale';
    if (record.consumerId !== consumerId) return 'stale';
    if (record.inFlight > 0) return 'busy';
    record.live = false;
    return 'ok';
  }

  beginCall(credential: LeaseCredential, consumerId: string) {
    const record = this.records.get(credential.handle);
    if (record === undefined || !record.live) {
      throw new RackError('stale', 'the lease credential is no longer valid');
    }
    if (record.consumerId !== consumerId) {
      throw new RackError('invalid_argument', 'the lease credential belongs to another consumer');
    }
    record.inFlight++;
    this.guards++;
    const manager = this;
    let ended = false;
    return {
      lease: {
        credential: record.credential,
        consumerId,
        providerPlugId: record.providerPlugId,
        providerGeneration: record.providerGeneration,
        version: record.version,
      },
      endCall(): void {
        if (ended) return;
        ended = true;
        manager.ends++;
        record.inFlight--;
      },
    };
  }

  async revokeProvider(providerPlugId: string, generation: number): Promise<'ok' | 'failed'> {
    for (const record of this.records.values()) {
      if (record.providerPlugId === providerPlugId && record.providerGeneration === generation) {
        record.live = false;
      }
    }
    return 'ok';
  }

  releaseAllFrom(consumerId: string): void {
    for (const record of this.records.values()) {
      if (record.consumerId === consumerId) record.live = false;
    }
  }

  hasOutstandingFor(providerPlugId: string, generation: number): boolean {
    for (const record of this.records.values()) {
      if (
        record.live &&
        record.providerPlugId === providerPlugId &&
        record.providerGeneration === generation
      ) {
        return true;
      }
    }
    return false;
  }

  get activeCount(): number {
    let count = 0;
    for (const record of this.records.values()) {
      if (record.live) count++;
    }
    return count;
  }
}

interface DiagEntry {
  readonly message: string;
  readonly detail?: unknown;
}

interface ResolveProbe {
  readonly plugId: string;
  readonly generation: number;
}

interface Harness {
  readonly gateway: InvokeGateway;
  readonly leases: FakeLeaseManager;
  readonly caps: Map<string, CapEntry>;
  readonly providers: Map<string, ProviderEndpoint>;
  readonly requests: InvokeRequest[];
  readonly diags: DiagEntry[];
  readonly resolved: ResolveProbe[];
}

function makeHarness(limits?: { readonly payloadLimit?: number; readonly outputLimit?: number }): Harness {
  const leases = new FakeLeaseManager();
  const caps = new Map<string, CapEntry>();
  const providers = new Map<string, ProviderEndpoint>();
  const requests: InvokeRequest[] = [];
  const diags: DiagEntry[] = [];
  const resolved: ResolveProbe[] = [];
  const gateway = createInvokeGateway({
    leases,
    lookupCaps: (plugId) => caps.get(plugId),
    resolveProvider: (plugId, generation) => {
      resolved.push({ plugId, generation });
      return providers.get(`${plugId}#${generation}`);
    },
    payloadLimit: limits?.payloadLimit,
    outputLimit: limits?.outputLimit,
    onDiag: (message, detail) => {
      diags.push({ message, detail });
    },
  });
  return { gateway, leases, caps, providers, requests, diags, resolved };
}

function publish(
  h: Harness,
  plugId: string,
  generation: number,
  methods: readonly MethodDesc[],
  invoke: (request: InvokeRequest) => JsonValue | Promise<JsonValue>,
): void {
  h.caps.set(plugId, entry(plugId, generation, methods));
  h.providers.set(`${plugId}#${generation}`, {
    key: `${plugId}#${generation}`,
    invoke: (request: InvokeRequest) => {
      h.requests.push(request);
      return invoke(request);
    },
  });
}

async function expectFailure(promise: Promise<unknown>, status: RackStatus): Promise<RackError> {
  try {
    await promise;
  } catch (error) {
    if (!(error instanceof RackError)) {
      throw new Error(`expected a RackError('${status}'), got: ${String(error)}`);
    }
    expect(error.status).toBe(status);
    return error;
  }
  throw new Error(`expected the call to reject with '${status}', but it resolved`);
}

describe('invoke gateway', () => {
  let harnessRef: Harness | undefined;

  afterEach(() => {
    if (harnessRef === undefined) return;
    expect(harnessRef.leases.ends, 'every opened beginCall guard must be closed').toBe(
      harnessRef.leases.guards,
    );
  });

  const fresh = (limits?: { readonly payloadLimit?: number; readonly outputLimit?: number }): Harness => {
    harnessRef = makeHarness(limits);
    return harnessRef;
  };

  it('routes name and id selectors through the same shared path', async () => {
    const harness = fresh();
    publish(harness, 'provider.a', 1, [md(7, 'calc.add'), md(8, 'calc.mul')], (request) => ({
      echoed: { methodId: request.methodId, methodName: request.methodName, args: request.args },
    }));
    const credential = harness.leases.mint('consumer.one', 'provider.a', 1);
    const args = { x: 1 };

    const byName = await harness.gateway.call('consumer.one', credential, { byName: 'calc.add' }, args);
    const sameById = await harness.gateway.call('consumer.one', credential, { byId: 7 }, args);
    const byId = await harness.gateway.call('consumer.one', credential, { byId: 8 }, args);

    expect(byName).toEqual({ echoed: { methodId: 7, methodName: 'calc.add', args: { x: 1 } } });
    // The id selector resolved the full disclosure too: identical request for the same method.
    expect(sameById).toEqual(byName);
    expect(byId).toEqual({ echoed: { methodId: 8, methodName: 'calc.mul', args: { x: 1 } } });
    expect(harness.requests).toHaveLength(3);
  });

  it('rejects undisclosed methods with not_found', async () => {
    const harness = fresh();
    publish(harness, 'provider.a', 1, [md(1, 'a.work')], () => 'ok');
    const credential = harness.leases.mint('consumer', 'provider.a', 1);

    await expectFailure(harness.gateway.call('consumer', credential, { byName: 'a.missing' }, null), 'not_found');
    await expectFailure(harness.gateway.call('consumer', credential, { byId: 999 }, null), 'not_found');

    const empty = fresh();
    publish(empty, 'provider.b', 1, [], () => 'ok');
    const credB = empty.leases.mint('consumer', 'provider.b', 1);
    await expectFailure(empty.gateway.call('consumer', credB, { byId: 1 }, null), 'not_found');
    expect(empty.requests).toHaveLength(0);
  });

  it('propagates lease-layer credential failures untouched', async () => {
    const harness = fresh();
    publish(harness, 'provider.a', 1, [md(1, 'a.work')], () => 'ok');
    const credential = harness.leases.mint('consumer', 'provider.a', 1);

    const released = harness.leases.mint('consumer', 'provider.a', 1);
    expect(harness.leases.release(released, 'consumer')).toBe('ok');
    await expectFailure(harness.gateway.call('consumer', released, { byName: 'a.work' }, null), 'stale');
    await expectFailure(
      harness.gateway.call('someone.else', credential, { byName: 'a.work' }, null),
      'invalid_argument',
    );

    expect(harness.requests).toHaveLength(0);
    expect(harness.resolved).toHaveLength(0);
    // Neither rejected call opened an in-flight guard.
    expect(harness.leases.guards).toBe(0);
  });

  it('validates arguments before opening a lease guard', async () => {
    const harness = fresh({ payloadLimit: 16 });
    publish(harness, 'provider.a', 1, [md(1, 'a.work')], () => 'ok');
    const credential = harness.leases.mint('consumer', 'provider.a', 1);

    await expectFailure(
      harness.gateway.call('consumer', credential, { byName: 'a.work' }, { bad: Number.NaN } as unknown as JsonValue),
      'invalid_argument',
    );
    await expectFailure(
      harness.gateway.call('consumer', credential, { byName: 'a.work' }, undefined as unknown as JsonValue),
      'invalid_argument',
    );
    await expectFailure(
      harness.gateway.call('consumer', credential, { byName: 'a.work' }, 'x'.repeat(20)),
      'limit_exceeded',
    );
    expect(harness.leases.guards).toBe(0);
  });

  it('rejects non-JSON and oversized results and keeps the provider usable', async () => {
    const harness = fresh({ outputLimit: 16 });
    publish(harness, 'provider.a', 1, [md(1, 'a.echo')], (request) =>
      request.args === 'nan' ? (Number.NaN as unknown as JsonValue) : request.args,
    );
    const credential = harness.leases.mint('consumer', 'provider.a', 1);

    await expectFailure(
      harness.gateway.call('consumer', credential, { byName: 'a.echo' }, 'y'.repeat(20)),
      'limit_exceeded',
    );
    await expectFailure(harness.gateway.call('consumer', credential, { byName: 'a.echo' }, 'nan'), 'invalid_argument');

    const ok = await harness.gateway.call('consumer', credential, { byName: 'a.echo' }, 'ok');
    expect(ok).toBe('ok');
    expect(harness.leases.ends).toBe(3);
  });

  it('converts provider exceptions into failed with diagnostics', async () => {
    const harness = fresh();
    const credential = harness.leases.mint('consumer', 'provider.a', 1);

    publish(harness, 'provider.a', 1, [md(3, 'a.boom')], () => {
      throw new Error('boom');
    });
    const failed = await expectFailure(
      harness.gateway.call('consumer', credential, { byId: 3 }, null),
      'failed',
    );
    expect(failed.message).toContain('provider.a');
    expect(failed.message).toContain('a.boom');
    expect(failed.detail).toEqual({ name: 'Error', message: 'boom' });

    publish(harness, 'provider.a', 1, [md(4, 'a.asyncBoom')], async () => {
      throw new Error('async boom');
    });
    await expectFailure(harness.gateway.call('consumer', credential, { byName: 'a.asyncBoom' }, null), 'failed');

    publish(harness, 'provider.a', 1, [md(5, 'a.foreign')], () => {
      throw 'plain oops';
    });
    const foreign = await expectFailure(
      harness.gateway.call('consumer', credential, { byName: 'a.foreign' }, null),
      'failed',
    );
    expect(foreign.detail).toBe('plain oops');

    expect(harness.diags).toHaveLength(3);
    for (const diag of harness.diags) {
      expect(diag.message).toContain('provider.a');
    }
    expect(harness.diags[0]?.detail).toEqual({ name: 'Error', message: 'boom' });

    // A missing onDiag sink must not break the conversion.
    const bare = createInvokeGateway({
      leases: harness.leases,
      lookupCaps: (plugId) => harness.caps.get(plugId),
      resolveProvider: (plugId, generation) => harness.providers.get(`${plugId}#${generation}`),
    });
    publish(harness, 'provider.a', 1, [md(6, 'a.silent')], () => {
      throw new Error('quiet');
    });
    await expectFailure(bare.call('consumer', credential, { byName: 'a.silent' }, null), 'failed');
  });

  it('rejects re-entry into an in-flight provider instance (A calling A)', async () => {
    const harness = fresh();
    const credOuter = harness.leases.mint('consumer.outer', 'provider.a', 1);
    const credInner = harness.leases.mint('provider.a', 'provider.a', 1);
    const gateway = harness.gateway;
    let nested: unknown = 'not-attempted';
    publish(harness, 'provider.a', 1, [md(1, 'a.work')], async (request) => {
      try {
        const value = await gateway.call('provider.a', credInner, { byName: 'a.work' }, null);
        nested = `unexpected-success: ${String(value)}`;
      } catch (error) {
        nested = error;
      }
      return { inner: nested instanceof RackError ? nested.status : 'none', received: request.args };
    });

    const first = await gateway.call('consumer.outer', credOuter, { byName: 'a.work' }, { round: 1 });
    expect(first).toEqual({ inner: 'busy', received: { round: 1 } });
    expect(nested).toBeInstanceOf(RackError);
    expect((nested as RackError).status).toBe('busy');
    // The nested attempt was rejected before reaching any provider invoke.
    expect(harness.requests).toHaveLength(1);

    // The in-flight key was released in the finally, so a later call enters again.
    const second = await gateway.call('consumer.outer', credOuter, { byName: 'a.work' }, { round: 2 });
    expect(second).toEqual({ inner: 'busy', received: { round: 2 } });
  });

  it('allows nested calls into a different provider (A -> B)', async () => {
    const harness = fresh();
    const credA = harness.leases.mint('consumer.outer', 'provider.a', 1);
    const credB = harness.leases.mint('provider.a', 'provider.b', 1);
    const gateway = harness.gateway;

    publish(harness, 'provider.b', 1, [md(5, 'b.double')], (request) => {
      const value = (request.args as { readonly n: number }).n;
      return { doubled: value * 2 };
    });
    publish(harness, 'provider.a', 1, [md(1, 'a.work')], async (request) => {
      const via = await gateway.call('provider.a', credB, { byId: 5 }, { n: 4 });
      return { via, received: request.args };
    });

    const result = await gateway.call('consumer.outer', credA, { byName: 'a.work' }, { tag: 'go' });
    expect(result).toEqual({ via: { doubled: 8 }, received: { tag: 'go' } });
    expect(harness.requests.map((request) => request.methodName)).toEqual(['a.work', 'b.double']);
    expect(harness.leases.guards).toBe(2);
  });

  it('stales when the provider was withdrawn, reloaded or has no endpoint', async () => {
    // Withdrawn from the capability table.
    const withdrawn = fresh();
    publish(withdrawn, 'provider.a', 1, [md(1, 'a.work')], () => 'ok');
    const credW = withdrawn.leases.mint('consumer', 'provider.a', 1);
    withdrawn.caps.delete('provider.a');
    await expectFailure(withdrawn.gateway.call('consumer', credW, { byName: 'a.work' }, null), 'stale');

    // Reloaded: the capability table carries a newer generation than the lease.
    const reloaded = fresh();
    publish(reloaded, 'provider.a', 2, [md(1, 'a.work')], () => 'ok');
    const credR = reloaded.leases.mint('consumer', 'provider.a', 1);
    await expectFailure(reloaded.gateway.call('consumer', credR, { byId: 1 }, null), 'stale');
    expect(reloaded.resolved).toHaveLength(0);

    // Caps intact but no live endpoint for the leased generation.
    const unassembled = fresh();
    unassembled.caps.set('provider.a', entry('provider.a', 1, [md(1, 'a.work')]));
    const credU = unassembled.leases.mint('consumer', 'provider.a', 1);
    await expectFailure(unassembled.gateway.call('consumer', credU, { byName: 'a.work' }, null), 'stale');
    // Endpoint resolution used the leased generation, not some other one.
    expect(unassembled.resolved).toEqual([{ plugId: 'provider.a', generation: 1 }]);
  });

  it('fails on host assembly errors when the endpoint lacks invoke', async () => {
    const harness = fresh();
    harness.caps.set('provider.a', entry('provider.a', 1, [md(1, 'a.work')]));
    harness.providers.set('provider.a#1', { key: 'provider.a#1' } as ProviderEndpoint);
    const credential = harness.leases.mint('consumer', 'provider.a', 1);

    await expectFailure(harness.gateway.call('consumer', credential, { byName: 'a.work' }, null), 'failed');
    expect(harness.requests).toHaveLength(0);
  });

  it('defaults both limits to 1 MiB', async () => {
    const harness = fresh();
    publish(harness, 'provider.a', 1, [md(1, 'a.echo')], (request) =>
      request.args === 'big-result' ? 'r'.repeat(400_000) : request.args,
    );
    const credential = harness.leases.mint('consumer', 'provider.a', 1);

    const ok = await harness.gateway.call('consumer', credential, { byName: 'a.echo' }, 'a'.repeat(300_000));
    expect(typeof ok).toBe('string');
    await expectFailure(
      harness.gateway.call('consumer', credential, { byName: 'a.echo' }, 'b'.repeat(400_000)),
      'limit_exceeded',
    );
    await expectFailure(
      harness.gateway.call('consumer', credential, { byName: 'a.echo' }, 'big-result'),
      'limit_exceeded',
    );
  });

  it('closes every lease guard across all exit paths', async () => {
    const harness = fresh({ outputLimit: 16 });
    const methods = [md(1, 'a.work'), md(2, 'a.boom')];
    publish(harness, 'provider.a', 1, methods, (request) => {
      if (request.methodName === 'a.boom') throw new Error('boom');
      if (request.args === 'big') return 'z'.repeat(20);
      return 'ok';
    });
    const credential = harness.leases.mint('consumer', 'provider.a', 1);

    expect(await harness.gateway.call('consumer', credential, { byName: 'a.work' }, null)).toBe('ok');
    await expectFailure(harness.gateway.call('consumer', credential, { byName: 'a.missing' }, null), 'not_found');
    harness.caps.delete('provider.a');
    await expectFailure(harness.gateway.call('consumer', credential, { byName: 'a.work' }, null), 'stale');
    harness.caps.set('provider.a', entry('provider.a', 1, methods));
    await expectFailure(harness.gateway.call('consumer', credential, { byName: 'a.boom' }, null), 'failed');
    await expectFailure(
      harness.gateway.call('consumer', credential, { byName: 'a.work' }, 'big'),
      'limit_exceeded',
    );

    expect(harness.leases.guards).toBe(5);
    expect(harness.leases.ends).toBe(5);
  });
});
