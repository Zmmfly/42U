/**
 * @file Tests for the lease manager: credential minting, generation binding, in-flight call
 *        counting, the revocation protocol and consumer-side cleanup.
 *
 * @brief Each test drives createLeaseManager with a mutable capability table standing in for
 *        the host registry; "reloading" a provider means swapping its entry to a new
 *        generation, optionally with an identical version.
 */
import { describe, expect, it } from 'vitest';
import { createLeaseManager } from '../src/core/lease.ts';
import type {
  CapEntry,
  LeaseCredential,
  LeaseManager,
  PluginVersion,
  Revoker,
  VersionRange,
} from '../src/core/types.ts';
import { isRackError, statusOf } from '../src/core/status.ts';
import type { RackStatus } from '../src/core/status.ts';

const v = (major: number, minor: number, patch: number): PluginVersion => ({ major, minor, patch });
const range = (minimum: PluginVersion, maximum: PluginVersion): VersionRange => ({ minimum, maximum });
const wide = (): VersionRange => range(v(0, 0, 0), v(99, 0, 0));

function entry(plugId: string, generation: number, version: PluginVersion): CapEntry {
  return { plugId, generation, version, caps: { methods: [] } };
}

interface DiagEntry {
  readonly message: string;
  readonly detail?: unknown;
}

function setup(): { table: Map<string, CapEntry>; diags: DiagEntry[]; manager: LeaseManager } {
  const table = new Map<string, CapEntry>();
  const diags: DiagEntry[] = [];
  const manager = createLeaseManager({
    lookup: (plugId) => table.get(plugId),
    onDiag: (message, detail) => {
      diags.push({ message, detail });
    },
  });
  return { table, diags, manager };
}

interface RecordingRevoker extends Revoker {
  readonly calls: LeaseCredential[];
}

function makeRevoker(onRevoke?: (credential: LeaseCredential) => void | Promise<void>): RecordingRevoker {
  const calls: LeaseCredential[] = [];
  return {
    calls,
    onRevoke: (credential) => {
      calls.push(credential);
      return onRevoke?.(credential);
    },
  };
}

function expectRackStatus(action: () => unknown, expected: RackStatus): void {
  try {
    action();
    expect.unreachable();
  } catch (error) {
    expect(isRackError(error)).toBe(true);
    expect(statusOf(error)).toBe(expected);
  }
}

describe('acquire', () => {
  it('mints a credential and returns the by-value actual version', () => {
    const { table, manager } = setup();
    const provider = entry('p.svc', 1, v(1, 2, 3));
    table.set('p.svc', provider);
    const lease = manager.acquire('consumer.a', 'p.svc', range(v(1, 0, 0), v(2, 0, 0)), makeRevoker());
    expect(lease.plugId).toBe('p.svc');
    expect(lease.version).toEqual(v(1, 2, 3));
    expect(lease.version).not.toBe(provider.version);
    expect(manager.activeCount).toBe(1);
    // A late mutation of the provider record must not leak into the held lease.
    (provider as { version: PluginVersion }).version = v(9, 9, 9);
    expect(lease.version).toEqual(v(1, 2, 3));
    // Equal endpoints (exact version) are accepted too.
    const exact = manager.acquire('consumer.a', 'p.svc', range(v(9, 9, 9), v(9, 9, 9)), makeRevoker());
    expect(exact.version).toEqual(v(9, 9, 9));
    expect(manager.activeCount).toBe(2);
  });

  it('rejects a reversed range with invalid_argument', () => {
    const { table, manager } = setup();
    table.set('p.svc', entry('p.svc', 1, v(1, 0, 0)));
    expectRackStatus(
      () => manager.acquire('consumer.a', 'p.svc', range(v(1, 1, 0), v(1, 0, 0)), makeRevoker()),
      'invalid_argument',
    );
  });

  it('rejects a malformed range or revoker with invalid_argument', () => {
    const { table, manager } = setup();
    table.set('p.svc', entry('p.svc', 1, v(1, 0, 0)));
    expectRackStatus(
      () => manager.acquire('consumer.a', 'p.svc', { minimum: v(1, 0, 0) } as unknown as VersionRange, makeRevoker()),
      'invalid_argument',
    );
    expectRackStatus(
      () => manager.acquire('consumer.a', 'p.svc', null as unknown as VersionRange, makeRevoker()),
      'invalid_argument',
    );
    expectRackStatus(
      () => manager.acquire('consumer.a', 'p.svc', wide(), null as unknown as Revoker),
      'invalid_argument',
    );
  });

  it('rejects an unknown or withdrawn provider with not_found', () => {
    const { table, manager } = setup();
    expectRackStatus(() => manager.acquire('consumer.a', 'p.missing', wide(), makeRevoker()), 'not_found');
    table.set('p.svc', entry('p.svc', 1, v(1, 0, 0)));
    table.delete('p.svc'); // withdrawal between registration and acquire
    expectRackStatus(() => manager.acquire('consumer.a', 'p.svc', wide(), makeRevoker()), 'not_found');
  });

  it('rejects a version outside the explicit range with unsupported', () => {
    const { table, manager } = setup();
    table.set('p.svc', entry('p.svc', 1, v(2, 0, 0)));
    expectRackStatus(
      () => manager.acquire('consumer.a', 'p.svc', range(v(1, 0, 0), v(1, 9, 9)), makeRevoker()),
      'unsupported',
    );
    expect(manager.activeCount).toBe(0);
  });
});

describe('release', () => {
  it('returns ok, clears the record and refuses double release', () => {
    const { table, manager } = setup();
    table.set('p.svc', entry('p.svc', 1, v(1, 0, 0)));
    const lease = manager.acquire('consumer.a', 'p.svc', wide(), makeRevoker());
    expect(manager.release(lease.credential, 'consumer.a')).toBe('ok');
    expect(manager.activeCount).toBe(0);
    expect(manager.release(lease.credential, 'consumer.a')).toBe('stale');
  });

  it('returns stale for unknown, malformed or foreign-owned credentials', () => {
    const { table, manager } = setup();
    table.set('p.svc', entry('p.svc', 1, v(1, 0, 0)));
    const lease = manager.acquire('consumer.a', 'p.svc', wide(), makeRevoker());
    expect(manager.release({} as LeaseCredential, 'consumer.a')).toBe('stale');
    expect(manager.release({ kind: 'nope', handle: {} } as unknown as LeaseCredential, 'consumer.a')).toBe('stale');
    expect(manager.release(null as unknown as LeaseCredential, 'consumer.a')).toBe('stale');
    expect(manager.release(lease.credential, 'consumer.b')).toBe('stale'); // wrong owner
    expect(manager.activeCount).toBe(1); // still held by consumer.a
    expect(manager.release(lease.credential, 'consumer.a')).toBe('ok'); // right owner still works
  });

  it('returns busy while a call is in flight and keeps the lease', async () => {
    const { table, manager } = setup();
    table.set('p.svc', entry('p.svc', 1, v(1, 0, 0)));
    const lease = manager.acquire('consumer.a', 'p.svc', wide(), makeRevoker());
    const guard = manager.beginCall(lease.credential, 'consumer.a');
    await expect(lease.release()).resolves.toBe('busy');
    expect(manager.activeCount).toBe(1);
    guard.endCall();
    await expect(lease.release()).resolves.toBe('ok');
    expect(manager.activeCount).toBe(0);
  });
});

describe('beginCall', () => {
  it('rejects malformed credentials with invalid_argument', () => {
    const { manager } = setup();
    expectRackStatus(() => manager.beginCall({} as LeaseCredential, 'consumer.a'), 'invalid_argument');
    expectRackStatus(() => manager.beginCall(null as unknown as LeaseCredential, 'consumer.a'), 'invalid_argument');
    expectRackStatus(
      () => manager.beginCall({ kind: 'u42.lease.credential', handle: 7 } as unknown as LeaseCredential, 'consumer.a'),
      'invalid_argument',
    );
  });

  it('rejects unknown, returned or foreign-owned credentials with stale', () => {
    const { table, manager } = setup();
    table.set('p.svc', entry('p.svc', 1, v(1, 0, 0)));
    const forged = { kind: 'u42.lease.credential', handle: Object.freeze({}) } as const;
    expectRackStatus(() => manager.beginCall(forged, 'consumer.a'), 'stale');
    const lease = manager.acquire('consumer.a', 'p.svc', wide(), makeRevoker());
    expectRackStatus(() => manager.beginCall(lease.credential, 'consumer.b'), 'stale'); // wrong owner
    expect(manager.release(lease.credential, 'consumer.a')).toBe('ok');
    expectRackStatus(() => manager.beginCall(lease.credential, 'consumer.a'), 'stale'); // returned
  });

  it('returns guard info bound to the live lease', () => {
    const { table, manager } = setup();
    table.set('p.svc', entry('p.svc', 3, v(1, 2, 3)));
    const lease = manager.acquire('consumer.a', 'p.svc', wide(), makeRevoker());
    const guard = manager.beginCall(lease.credential, 'consumer.a');
    expect(guard.lease.credential).toBe(lease.credential);
    expect(guard.lease.consumerId).toBe('consumer.a');
    expect(guard.lease.providerPlugId).toBe('p.svc');
    expect(guard.lease.providerGeneration).toBe(3);
    expect(guard.lease.version).toEqual(v(1, 2, 3));
    guard.endCall();
  });

  it('invalidates old credentials after a same-version reload', () => {
    const { table, manager } = setup();
    table.set('p.svc', entry('p.svc', 1, v(1, 4, 0)));
    const old = manager.acquire('consumer.a', 'p.svc', range(v(1, 0, 0), v(1, 9, 9)), makeRevoker());
    // Reload: same plugId, same disclosed version, new generation.
    table.set('p.svc', entry('p.svc', 2, v(1, 4, 0)));
    expectRackStatus(() => manager.beginCall(old.credential, 'consumer.a'), 'stale');
    // Withdrawn entirely (lookup gone) is stale as well.
    table.delete('p.svc');
    expectRackStatus(() => manager.beginCall(old.credential, 'consumer.a'), 'stale');
    // Release stays open as the recovery path, then a fresh borrow works.
    expect(manager.release(old.credential, 'consumer.a')).toBe('ok');
    table.set('p.svc', entry('p.svc', 2, v(1, 4, 0)));
    const fresh = manager.acquire('consumer.a', 'p.svc', range(v(1, 0, 0), v(1, 9, 9)), makeRevoker());
    expect(fresh.credential).not.toBe(old.credential);
    const guard = manager.beginCall(fresh.credential, 'consumer.a');
    expect(guard.lease.providerGeneration).toBe(2);
    guard.endCall();
  });
});

describe('in-flight counting', () => {
  it('keeps release busy until every guard ends; endCall is idempotent', () => {
    const { table, manager } = setup();
    table.set('p.svc', entry('p.svc', 1, v(1, 0, 0)));
    const lease = manager.acquire('consumer.a', 'p.svc', wide(), makeRevoker());
    const first = manager.beginCall(lease.credential, 'consumer.a');
    const second = manager.beginCall(lease.credential, 'consumer.a');
    expect(manager.release(lease.credential, 'consumer.a')).toBe('busy');
    first.endCall();
    expect(manager.release(lease.credential, 'consumer.a')).toBe('busy'); // second still open
    first.endCall(); // repeated endCall must not decrement twice
    expect(manager.release(lease.credential, 'consumer.a')).toBe('busy');
    second.endCall();
    expect(manager.release(lease.credential, 'consumer.a')).toBe('ok');
  });

  it('tolerates endCall after the record vanished (releaseAllFrom during flight)', () => {
    const { table, manager } = setup();
    table.set('p.svc', entry('p.svc', 1, v(1, 0, 0)));
    const lease = manager.acquire('consumer.a', 'p.svc', wide(), makeRevoker());
    const guard = manager.beginCall(lease.credential, 'consumer.a');
    manager.releaseAllFrom('consumer.a');
    expect(() => guard.endCall()).not.toThrow();
    expect(manager.activeCount).toBe(0);
  });
});

describe('revokeProvider', () => {
  it('reports ok when every revoker returns its credential', async () => {
    const { table, manager } = setup();
    table.set('p.svc', entry('p.svc', 1, v(1, 0, 0)));
    const revoker = makeRevoker((credential) => {
      manager.release(credential, 'consumer.a');
    });
    const lease = manager.acquire('consumer.a', 'p.svc', wide(), revoker);
    await expect(manager.revokeProvider('p.svc', 1)).resolves.toBe('ok');
    expect(revoker.calls.length).toBe(1);
    expect(revoker.calls[0]).toBe(lease.credential);
    expect(manager.activeCount).toBe(0);
  });

  it('reports failed and keeps the record when a lease stays unreturned', async () => {
    const { table, manager } = setup();
    table.set('p.svc', entry('p.svc', 1, v(1, 0, 0)));
    manager.acquire('consumer.a', 'p.svc', wide(), makeRevoker()); // revoker does nothing
    await expect(manager.revokeProvider('p.svc', 1)).resolves.toBe('failed');
    expect(manager.activeCount).toBe(1);
    expect(manager.hasOutstandingFor('p.svc', 1)).toBe(true);
  });

  it('continues past a throwing revoker and records a diagnostic', async () => {
    const { table, diags, manager } = setup();
    table.set('p.svc', entry('p.svc', 1, v(1, 0, 0)));
    const boom = makeRevoker(() => {
      throw new Error('consumer exploded');
    });
    const good = makeRevoker((credential) => {
      manager.release(credential, 'consumer.b');
    });
    manager.acquire('consumer.a', 'p.svc', wide(), boom);
    manager.acquire('consumer.b', 'p.svc', wide(), good);
    await expect(manager.revokeProvider('p.svc', 1)).resolves.toBe('failed'); // boom's lease kept
    expect(boom.calls.length).toBe(1);
    expect(good.calls.length).toBe(1); // the throw did not stop the loop
    expect(manager.activeCount).toBe(1);
    expect(diags.length).toBe(1);
    expect(diags[0]?.message).toContain('onRevoke');
  });

  it('still reports ok when a throwing revoker released first', async () => {
    const { table, diags, manager } = setup();
    table.set('p.svc', entry('p.svc', 1, v(1, 0, 0)));
    const revoker = makeRevoker((credential) => {
      manager.release(credential, 'consumer.a');
      throw new Error('cleanup glitch');
    });
    manager.acquire('consumer.a', 'p.svc', wide(), revoker);
    await expect(manager.revokeProvider('p.svc', 1)).resolves.toBe('ok');
    expect(manager.activeCount).toBe(0);
    expect(diags.length).toBe(1); // diagnosed, but the lease was returned
  });

  it('notifies only leases of the exact generation', async () => {
    const { table, manager } = setup();
    table.set('p.svc', entry('p.svc', 1, v(1, 0, 0)));
    const revokerGen1 = makeRevoker((credential) => {
      manager.release(credential, 'consumer.a');
    });
    manager.acquire('consumer.a', 'p.svc', wide(), revokerGen1);
    table.set('p.svc', entry('p.svc', 2, v(1, 0, 0))); // reload; gen-1 lease lingers unreturned
    const revokerGen2 = makeRevoker();
    manager.acquire('consumer.b', 'p.svc', wide(), revokerGen2);
    await expect(manager.revokeProvider('p.svc', 1)).resolves.toBe('ok');
    expect(revokerGen1.calls.length).toBe(1);
    expect(revokerGen2.calls.length).toBe(0);
    expect(manager.activeCount).toBe(1); // gen-2 lease untouched
    expect(manager.hasOutstandingFor('p.svc', 2)).toBe(true);
  });

  it('awaits asynchronous revokers before re-checking', async () => {
    const { table, manager } = setup();
    table.set('p.svc', entry('p.svc', 1, v(1, 0, 0)));
    const revoker = makeRevoker(async (credential) => {
      await Promise.resolve();
      manager.release(credential, 'consumer.a');
    });
    manager.acquire('consumer.a', 'p.svc', wide(), revoker);
    await expect(manager.revokeProvider('p.svc', 1)).resolves.toBe('ok');
    expect(manager.activeCount).toBe(0);
  });

  it('reports ok with nothing outstanding', async () => {
    const { table, manager } = setup();
    table.set('p.svc', entry('p.svc', 1, v(1, 0, 0)));
    await expect(manager.revokeProvider('p.svc', 1)).resolves.toBe('ok');
  });

  it('tolerates a missing diagnostics sink', async () => {
    const table = new Map<string, CapEntry>();
    table.set('p.svc', entry('p.svc', 1, v(1, 0, 0)));
    const manager = createLeaseManager({ lookup: (plugId) => table.get(plugId) });
    manager.acquire('consumer.a', 'p.svc', wide(), makeRevoker(() => {
      throw new Error('boom');
    }));
    await expect(manager.revokeProvider('p.svc', 1)).resolves.toBe('failed');
  });
});

describe('releaseAllFrom and outstanding queries', () => {
  it('silently clears only the outgoing leases of one consumer', () => {
    const { table, manager } = setup();
    table.set('p.one', entry('p.one', 1, v(1, 0, 0)));
    table.set('p.two', entry('p.two', 4, v(2, 0, 0)));
    const leaseA1 = manager.acquire('consumer.a', 'p.one', wide(), makeRevoker());
    manager.acquire('consumer.a', 'p.two', range(v(2, 0, 0), v(2, 0, 0)), makeRevoker());
    const leaseB = manager.acquire('consumer.b', 'p.one', wide(), makeRevoker());
    expect(manager.activeCount).toBe(3);
    manager.releaseAllFrom('consumer.a');
    expect(manager.activeCount).toBe(1);
    expect(manager.release(leaseA1.credential, 'consumer.a')).toBe('stale'); // already cleared
    expect(manager.hasOutstandingFor('p.one', 1)).toBe(true); // consumer.b's lease survives
    expect(manager.hasOutstandingFor('p.two', 4)).toBe(false);
    expect(manager.release(leaseB.credential, 'consumer.b')).toBe('ok');
  });

  it('scopes hasOutstandingFor by plugId and generation', () => {
    const { table, manager } = setup();
    table.set('p.svc', entry('p.svc', 1, v(1, 0, 0)));
    expect(manager.hasOutstandingFor('p.svc', 1)).toBe(false);
    const lease = manager.acquire('consumer.a', 'p.svc', wide(), makeRevoker());
    expect(manager.hasOutstandingFor('p.svc', 1)).toBe(true);
    expect(manager.hasOutstandingFor('p.svc', 2)).toBe(false);
    expect(manager.hasOutstandingFor('p.other', 1)).toBe(false);
    manager.release(lease.credential, 'consumer.a');
    expect(manager.hasOutstandingFor('p.svc', 1)).toBe(false);
  });
});
