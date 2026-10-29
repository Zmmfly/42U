/**
 * @file Tests for the rack host facade (createRack).
 *
 * @brief Assembles the real first-wave core modules and verifies the orchestration
 *        contract: deterministic add()/start() ordering, capability registration and the
 *        atomic watch snapshot, event and gateway flows through real plugins, batch
 *        rollback on init/start failures, the full unload protocol with quarantine,
 *        deferred unloads from callbacks, reload generation invalidation, duplicate
 *        rejection, reverse-order shutdown with isolation, and boot() discovery.
 */
import { describe, expect, it } from 'vitest';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createRack, type RackDiagRecord } from '../src/core/host.ts';
import { isRackError, statusOf, type RackError, type RackStatus } from '../src/core/status.ts';
import type {
  CapEvent,
  InitOrderSpec,
  InvokeRequest,
  LeaseHandle,
  PluginServices,
  PluginVersion,
  RackPlugin,
  Revoker,
  VersionRange,
} from '../src/core/types.ts';
import type { JsonValue } from '../src/core/json.ts';

const hostFixtures = fileURLToPath(new URL('fixtures/host', import.meta.url));
const singleDir = join(hostFixtures, 'single');
const lateDir = join(hostFixtures, 'late');
const dupDir = join(hostFixtures, 'dup');
const emptyDir = join(hostFixtures, 'empty');
const providerPath = join(singleDir, 'ahost_provider.plug.ts');
const latePath = join(lateDir, 'ahost_late.plug.ts');

/** @brief Version triple helper. */
const v = (major: number, minor: number, patch: number): PluginVersion => ({ major, minor, patch });

/** @brief Closed 1.x range accepted by every fixture and tracked plugin (version 1.x). */
const RANGE_V1: VersionRange = { minimum: v(1, 0, 0), maximum: v(1, 9, 9) };

/**
 * @brief Assert that a promise rejects with a RackError of the given status.
 * @param promise Operation under test.
 * @param status Expected RackError status.
 * @return The caught RackError for detail assertions.
 */
async function expectStatus(promise: Promise<unknown>, status: RackStatus): Promise<RackError> {
  let caught: unknown;
  try {
    await promise;
  } catch (error) {
    caught = error;
  }
  expect(caught, `expected rejection with '${status}'`).toBeInstanceOf(Error);
  expect(isRackError(caught)).toBe(true);
  const rackError = caught as RackError;
  expect(rackError.status).toBe(status);
  return rackError;
}

/**
 * @brief Capture the status of a possibly-rejecting operation as a string.
 * @param promise Operation under test.
 * @return The RackError status, or 'ok' when the promise resolved.
 */
async function captureStatus(promise: Promise<unknown>): Promise<string> {
  try {
    await promise;
    return 'ok';
  } catch (error) {
    return statusOf(error);
  }
}

/** @brief Narrow a JSON value to a number for result assertions. */
const asNumber = (value: JsonValue): number => (typeof value === 'number' ? value : Number.NaN);

/** @brief Lifecycle-observing plugin double built from in-process hooks. */
interface TrackedPlugin {
  readonly plugin: RackPlugin;
  initCalls: number;
  startCalls: number;
  stopCalls: number;
  destroyCalls: number;
  /** @brief Services captured during init(); available to the start/stop hooks. */
  services: PluginServices | undefined;
}

/**
 * @brief Build one object plugin whose lifecycle calls are counted and traced.
 *
 * @param plugId Stable plugin identity.
 * @param hooks Optional descriptor fields, a shared `${plugId}:<phase>` trace array and
 *        per-phase callbacks; onStart/onStop also receive the services captured at init.
 *
 * @return The plugin plus mutable call counters.
 */
function trackPlugin(
  plugId: string,
  hooks: {
    version?: PluginVersion;
    order?: InitOrderSpec;
    trace?: string[];
    onInit?: (services: PluginServices) => void | Promise<void>;
    onStart?: (services: PluginServices | undefined) => void | Promise<void>;
    onStop?: (services: PluginServices | undefined) => void | Promise<void>;
    onDestroy?: () => void | Promise<void>;
    onInvoke?: (request: InvokeRequest) => JsonValue | Promise<JsonValue>;
  } = {},
): TrackedPlugin {
  const tracked: TrackedPlugin = {
    initCalls: 0,
    startCalls: 0,
    stopCalls: 0,
    destroyCalls: 0,
    services: undefined,
    plugin: {
      desc: { plugId, version: hooks.version ?? v(1, 0, 0), init: hooks.order ?? {} },
      init: async (services: PluginServices): Promise<void> => {
        tracked.initCalls += 1;
        hooks.trace?.push(`${plugId}:init`);
        tracked.services = services;
        await hooks.onInit?.(services);
      },
      start: async (): Promise<void> => {
        tracked.startCalls += 1;
        hooks.trace?.push(`${plugId}:start`);
        await hooks.onStart?.(tracked.services);
      },
      stop: async (): Promise<void> => {
        tracked.stopCalls += 1;
        hooks.trace?.push(`${plugId}:stop`);
        await hooks.onStop?.(tracked.services);
      },
      destroy: async (): Promise<void> => {
        tracked.destroyCalls += 1;
        hooks.trace?.push(`${plugId}:destroy`);
        await hooks.onDestroy?.();
      },
      ...(hooks.onInvoke === undefined
        ? {}
        : {
            invoke: async (request: InvokeRequest): Promise<JsonValue> => hooks.onInvoke?.(request) ?? null,
          }),
    },
  };
  return tracked;
}

describe('createRack', () => {
  it('starts add()-staged plugins in deterministic before/after order', async () => {
    const rack = createRack();
    const trace: string[] = [];
    // Priorities alone would plan aaa.top, mmm.mid, zzz.base; the hard edges flip it.
    const base = trackPlugin('zzz.base', { order: { priority: 10 }, trace });
    const top = trackPlugin('aaa.top', { order: { priority: 0, after: ['zzz.base'] }, trace });
    const mid = trackPlugin('mmm.mid', { order: { priority: 5, before: ['zzz.base'] }, trace });
    rack.add(() => base.plugin);
    rack.add(() => top.plugin);
    rack.add(() => mid.plugin);

    await rack.start();

    expect(trace).toEqual([
      'mmm.mid:init',
      'zzz.base:init',
      'aaa.top:init',
      'mmm.mid:start',
      'zzz.base:start',
      'aaa.top:start',
    ]);
    expect(rack.plugins().map((info) => info.plugId)).toEqual(['mmm.mid', 'zzz.base', 'aaa.top']);
    expect(rack.plugins().every((info) => info.state === 'active')).toBe(true);
    const generations = rack.plugins().map((info) => info.generation);
    expect(new Set(generations).size).toBe(3);
  });

  it('registers capabilities after start() and serves the atomic watch snapshot', async () => {
    const rack = createRack();
    const capEvents: CapEvent[] = [];
    const provider = trackPlugin('w.provider', {
      version: v(2, 0, 0),
      order: { priority: 0 },
      onStart: (services) => {
        services?.caps.publish({ methods: [{ id: 1, name: 'echo', description: 'echoes its args' }] });
      },
    });
    const watcher = trackPlugin('w.watcher', {
      order: { priority: 1 },
      onInit: (services) => {
        services.caps.watch({
          onCapability: (event) => {
            capEvents.push(event);
          },
        });
      },
    });
    rack.add(() => provider.plugin);
    rack.add(() => watcher.plugin);
    await rack.start();

    // The watcher started after the provider: its first delivery is the snapshot fact.
    expect(capEvents.map((event) => [event.plugId, event.available])).toEqual([['w.provider', true]]);
    expect(capEvents[0]?.version).toEqual(v(2, 0, 0));
    expect(capEvents[0]?.capabilities.methods.map((method) => method.name)).toEqual(['echo']);

    await rack.unload('w.provider');
    expect(capEvents.map((event) => [event.plugId, event.available])).toEqual([
      ['w.provider', true],
      ['w.provider', false],
    ]);
    expect(capEvents[1]?.capabilities.methods).toEqual([]);
    expect(rack.plugins().map((info) => info.plugId)).toEqual(['w.watcher']);
  });

  it('flows events and gateway calls between plugins (name and id paths)', async () => {
    const rack = createRack();
    const seen: { name: string; payload: JsonValue }[] = [];
    const results: Record<string, unknown> = {};
    const provider = trackPlugin('t.provider', {
      order: { priority: 1 },
      onStart: (services) => {
        services?.caps.publish({ methods: [{ id: 7, name: 'echo' }] });
      },
      onInvoke: (request) => ({ method: request.methodName, id: request.methodId, args: request.args }),
    });
    const consumer = trackPlugin('t.consumer', {
      order: { priority: 0 },
      onInit: (services) => {
        services.events.subscribe('t.ready', {
          onEvent: async (event) => {
            seen.push({ name: event.name, payload: event.payload });
            const handle = services.caps.acquire('t.provider', RANGE_V1, { onRevoke: () => undefined });
            results.echo = await services.calls.call(handle.credential, 'echo', { x: 41 });
            results.byId = await services.calls.callById(handle.credential, 7, { via: 'id' });
            try {
              await services.calls.call(handle.credential, 'missing', null);
              results.missing = 'unexpected-success';
            } catch (error) {
              results.missing = statusOf(error);
            }
            results.release = await handle.release();
          },
        });
      },
    });
    const emitter = trackPlugin('t.emitter', {
      order: { priority: 2 },
      onStart: (services) => {
        services?.events.publish('t.ready', { tick: 1 });
      },
    });
    rack.add(() => consumer.plugin);
    rack.add(() => provider.plugin);
    rack.add(() => emitter.plugin);
    await rack.start();

    expect(seen).toEqual([{ name: 't.ready', payload: { tick: 1 } }]);
    expect(results.echo).toEqual({ method: 'echo', id: 7, args: { x: 41 } });
    expect(results.byId).toEqual({ method: 'echo', id: 7, args: { via: 'id' } });
    expect(results.missing).toBe('not_found');
    expect(results.release).toBe('ok');
  });

  it('rolls back the whole batch when an init() throws', async () => {
    const rack = createRack();
    const trace: string[] = [];
    const ok = trackPlugin('f.ok', { order: { priority: 0 }, trace });
    const bad = trackPlugin('f.bad', {
      order: { priority: 1 },
      trace,
      onInit: () => {
        throw new Error('boom');
      },
    });
    rack.add(() => ok.plugin);
    rack.add(() => bad.plugin);

    const error = await expectStatus(rack.start(), 'failed');

    expect((error.detail as { plugId?: string } | undefined)?.plugId).toBe('f.bad');
    expect(trace).toEqual(['f.ok:init', 'f.bad:init', 'f.ok:stop', 'f.ok:destroy', 'f.bad:destroy']);
    expect(ok.stopCalls).toBe(1);
    expect(bad.stopCalls).toBe(0);
    expect(bad.destroyCalls).toBe(1);
    expect(rack.plugins()).toEqual([]);
  });

  it('rolls back started peers when a start() throws', async () => {
    const rack = createRack();
    const trace: string[] = [];
    const capEvents: CapEvent[] = [];
    const watcher = trackPlugin('g.watcher', {
      order: { priority: 0 },
      trace,
      onInit: (services) => {
        services.caps.watch({
          onCapability: (event) => {
            capEvents.push(event);
          },
        });
      },
    });
    const starter = trackPlugin('g.starter', {
      order: { priority: 1 },
      trace,
      onStart: (services) => {
        services?.caps.publish({ methods: [{ id: 1, name: 'run' }] });
      },
    });
    const failing = trackPlugin('g.failing', {
      order: { priority: 2 },
      trace,
      onStart: () => {
        throw new Error('no');
      },
    });
    rack.add(() => watcher.plugin);
    rack.add(() => starter.plugin);
    rack.add(() => failing.plugin);

    const error = await expectStatus(rack.start(), 'failed');

    expect((error.detail as { plugId?: string } | undefined)?.plugId).toBe('g.failing');
    expect(capEvents.map((event) => [event.plugId, event.available])).toEqual([
      ['g.starter', true],
      ['g.starter', false],
    ]);
    expect(trace).toEqual([
      'g.watcher:init',
      'g.starter:init',
      'g.failing:init',
      'g.watcher:start',
      'g.starter:start',
      'g.failing:start',
      'g.failing:stop',
      'g.failing:destroy',
      'g.starter:stop',
      'g.starter:destroy',
      'g.watcher:stop',
      'g.watcher:destroy',
    ]);
    expect(rack.plugins()).toEqual([]);
  });

  it('runs the full unload protocol: unsubscribe, release leases, destroy', async () => {
    const rack = createRack();
    let sinkHits = 0;
    let echoResult: unknown;
    let revokeCalls = 0;
    const provider = trackPlugin('u.provider', {
      order: { priority: 1 },
      onStart: (services) => {
        services?.caps.publish({ methods: [{ id: 1, name: 'echo' }] });
      },
      onInvoke: (request) => ({ echoed: request.args }),
    });
    const consumer = trackPlugin('u.consumer', {
      order: { priority: 0 },
      onInit: (services) => {
        services.events.subscribe('u.chatter', {
          onEvent: async () => {
            sinkHits += 1;
            // The consumer deliberately keeps this lease (revoker retains the credential);
            // unload(consumer) must still release it host-side.
            const handle = services.caps.acquire('u.provider', RANGE_V1, {
              onRevoke: () => {
                revokeCalls += 1;
              },
            });
            echoResult = await services.calls.call(handle.credential, 'echo', { n: 1 });
          },
        });
      },
    });
    const emitter = trackPlugin('u.emitter', {
      order: { priority: 2 },
      onStart: (services) => {
        services?.events.publish('u.chatter', { tick: 1 });
      },
      onStop: (services) => {
        services?.events.publish('u.chatter', { tick: 2 });
      },
    });
    rack.add(() => consumer.plugin);
    rack.add(() => provider.plugin);
    rack.add(() => emitter.plugin);
    await rack.start();

    expect(sinkHits).toBe(1);
    expect(echoResult).toEqual({ echoed: { n: 1 } });

    await rack.unload('u.consumer');
    expect(rack.plugins().map((info) => info.plugId)).toEqual(['u.provider', 'u.emitter']);
    expect(consumer.stopCalls).toBe(1);
    expect(consumer.destroyCalls).toBe(1);

    // Succeeds immediately only because releaseAllFrom dropped the retained lease above.
    await rack.unload('u.provider');
    expect(revokeCalls).toBe(0);
    expect(provider.destroyCalls).toBe(1);

    await rack.unload('u.emitter');
    await rack.poll();
    // The consumer's subscription was revoked with the instance: the second event is dropped.
    expect(sinkHits).toBe(1);
    expect(rack.plugins()).toEqual([]);
  });

  it('isolates a provider whose leases are not returned', async () => {
    const diags: RackDiagRecord[] = [];
    const rack = createRack({ onDiag: (level, message) => diags.push({ level, message }) });
    const capEvents: CapEvent[] = [];
    let revokeCalls = 0;
    const provider = trackPlugin('q.provider', {
      order: { priority: 0 },
      onStart: (services) => {
        services?.caps.publish({ methods: [{ id: 1, name: 'echo' }] });
      },
    });
    const consumer = trackPlugin('q.consumer', {
      order: { priority: 1 },
      onStart: (services) => {
        services?.caps.acquire('q.provider', RANGE_V1, {
          onRevoke: () => {
            revokeCalls += 1;
          },
        });
      },
    });
    const watcher = trackPlugin('q.watcher', {
      order: { priority: 2 },
      onInit: (services) => {
        services.caps.watch({
          onCapability: (event) => {
            capEvents.push(event);
          },
        });
      },
    });
    rack.add(() => provider.plugin);
    rack.add(() => consumer.plugin);
    rack.add(() => watcher.plugin);
    await rack.start();
    expect(capEvents.map((event) => event.available)).toEqual([true]);

    const error = await expectStatus(rack.unload('q.provider'), 'failed');
    expect((error.detail as { plugId?: string } | undefined)?.plugId).toBe('q.provider');
    expect(rack.describe('q.provider')?.state).toBe('faulted');
    expect(capEvents.map((event) => event.available)).toEqual([true, false]);
    expect(revokeCalls).toBe(1);

    await expectStatus(rack.unload('q.provider'), 'invalid_state');

    // Shutdown is best effort: the consumer and watcher go down, the provider is retained.
    await rack.shutdown();
    expect(rack.plugins().map((info) => [info.plugId, info.state])).toEqual([['q.provider', 'faulted']]);
    expect(rack.recentDiags.some((record) => record.level === 'error')).toBe(true);
    expect(diags.some((record) => record.level === 'error' && record.message.includes('q.provider'))).toBe(true);
  });

  it('defers unload requests made inside callbacks and consumes them at poll()', async () => {
    const rack = createRack();
    const statuses: Record<string, string> = {};
    const provider = trackPlugin('d.provider', {
      order: { priority: 0 },
      onStart: (services) => {
        services?.caps.publish({ methods: [{ id: 1, name: 'echo' }] });
      },
    });
    const killer = trackPlugin('d.killer', {
      order: { priority: 1 },
      onInit: (services) => {
        services.events.subscribe('d.go', {
          onEvent: async () => {
            statuses.unknown = await captureStatus(rack.unload('d.nobody'));
            await rack.unload('d.provider');
            statuses.deferred = 'resolved';
            statuses.start = await captureStatus(rack.start());
            try {
              rack.add(() => provider.plugin);
              statuses.add = 'resolved';
            } catch (error) {
              statuses.add = statusOf(error);
            }
          },
        });
      },
    });
    const emitter = trackPlugin('d.emitter', {
      order: { priority: 2 },
      onStart: (services) => {
        services?.events.publish('d.go', { go: true });
      },
    });
    rack.add(() => provider.plugin);
    rack.add(() => killer.plugin);
    rack.add(() => emitter.plugin);
    await rack.start();

    expect(statuses).toEqual({ unknown: 'not_found', deferred: 'resolved', start: 'busy', add: 'busy' });
    expect(rack.describe('d.provider')?.state).toBe('active');

    await rack.poll();
    expect(rack.describe('d.provider')).toBeUndefined();
    expect(provider.stopCalls).toBe(1);
    expect(provider.destroyCalls).toBe(1);
    expect(rack.plugins().map((info) => info.plugId)).toEqual(['d.killer', 'd.emitter']);
  });

  it('invalidates old credentials across a same-version reload while new leases work', async () => {
    const rack = createRack();
    await rack.load(providerPath);
    const before = rack.describe('ahost.provider');
    expect(before?.state).toBe('active');
    expect(before?.sourcePath).toBe(providerPath);

    let mode: 'capture' | 'verify' = 'capture';
    const captured: { m1?: number; stale?: string; m2?: number; revokes: number } = { revokes: 0 };
    let stored: LeaseHandle | undefined;
    const revoker: Revoker = {
      onRevoke: async (credential) => {
        captured.revokes += 1;
        if (stored !== undefined && stored.credential === credential) {
          await stored.release();
        }
      },
    };
    const consumer = trackPlugin('r.consumer', {
      order: { priority: 0 },
      onInit: (services) => {
        services.events.subscribe('r.go', {
          onEvent: async () => {
            if (mode === 'capture') {
              const handle = services.caps.acquire('ahost.provider', RANGE_V1, revoker);
              stored = handle;
              captured.m1 = asNumber(await services.calls.call(handle.credential, 'moduleLoads', null));
            } else {
              if (stored === undefined) throw new Error('capture phase did not run');
              try {
                await services.calls.call(stored.credential, 'moduleLoads', null);
                captured.stale = 'unexpected-success';
              } catch (error) {
                captured.stale = statusOf(error);
              }
              const fresh = services.caps.acquire('ahost.provider', RANGE_V1, revoker);
              stored = fresh;
              captured.m2 = asNumber(await services.calls.call(fresh.credential, 'moduleLoads', null));
            }
          },
        });
      },
    });
    const emitter = trackPlugin('r.emitter', {
      order: { priority: 1 },
      onStart: (services) => {
        services?.events.publish('r.go', { phase: 'start' });
      },
      onStop: (services) => {
        services?.events.publish('r.go', { phase: 'stop' });
      },
    });
    rack.add(() => consumer.plugin);
    rack.add(() => emitter.plugin);
    await rack.start();

    const m1 = captured.m1;
    expect(m1).toBeTypeOf('number');

    mode = 'verify';
    await rack.reload('ahost.provider');

    const after = rack.describe('ahost.provider');
    expect(after?.state).toBe('active');
    expect(after?.generation).toBeGreaterThan(before?.generation ?? 0);
    expect(captured.revokes).toBe(1);

    await rack.unload('r.emitter');
    await rack.poll();

    expect(captured.stale).toBe('stale');
    expect(captured.m2).toBe((m1 ?? 0) + 1);
    await rack.shutdown();
    expect(rack.plugins()).toEqual([]);
  });

  it('rejects duplicate plug ids from add(), load() and boot()', async () => {
    const rack1 = createRack();
    const first = trackPlugin('dup.x');
    const second = trackPlugin('dup.x');
    rack1.add(() => first.plugin);
    rack1.add(() => second.plugin);
    const error = await expectStatus(rack1.start(), 'duplicate');
    expect((error.detail as { plugId?: string } | undefined)?.plugId).toBe('dup.x');
    expect(rack1.plugins()).toEqual([]);
    expect(first.initCalls).toBe(0);

    const rack2 = createRack();
    await rack2.load(providerPath);
    const original = rack2.describe('ahost.provider');
    const clashing = trackPlugin('ahost.provider');
    rack2.add(() => clashing.plugin);
    await expectStatus(rack2.start(), 'duplicate');
    expect(rack2.describe('ahost.provider')).toEqual(original);
    await rack2.shutdown();

    const rack3 = createRack({ pluginDirectories: [dupDir] });
    await expectStatus(rack3.boot(), 'duplicate');
    expect(rack3.plugins()).toEqual([]);
  });

  it('rejects a hot-load before-constraint against an initialized plugin', async () => {
    const rack = createRack();
    await rack.load(providerPath);
    await expectStatus(rack.load(latePath), 'invalid_state');
    expect(rack.describe('ahost.provider')?.state).toBe('active');
    expect(rack.plugins().map((info) => info.plugId)).toEqual(['ahost.provider']);
    await rack.shutdown();
  });

  it('shuts down in reverse plan order, isolating failures without throwing', async () => {
    const diags: RackDiagRecord[] = [];
    const rack = createRack({
      onDiag: (level, message) => diags.push({ level, message }),
    });
    const trace: string[] = [];
    const a = trackPlugin('s.a', { order: { priority: 0 }, trace });
    const b = trackPlugin('s.b', {
      order: { priority: 1 },
      trace,
      onStop: () => {
        throw new Error('refuses to stop');
      },
    });
    const c = trackPlugin('s.c', { order: { priority: 2 }, trace });
    rack.add(() => a.plugin);
    rack.add(() => b.plugin);
    rack.add(() => c.plugin);
    await rack.start();
    expect(rack.plugins().map((info) => info.plugId)).toEqual(['s.a', 's.b', 's.c']);

    await rack.shutdown();

    expect(trace.slice(6)).toEqual([
      's.c:stop',
      's.c:destroy',
      's.b:stop',
      's.a:stop',
      's.a:destroy',
    ]);
    expect(rack.plugins().map((info) => [info.plugId, info.state])).toEqual([['s.b', 'faulted']]);
    expect(b.destroyCalls).toBe(0);
    expect(c.destroyCalls).toBe(1);
    expect(diags.some((record) => record.level === 'error' && record.message.includes("'s.b'"))).toBe(true);

    await expectStatus(rack.boot(), 'invalid_state');
    await expectStatus(rack.load(providerPath), 'invalid_state');
    await expectStatus(rack.unload('s.b'), 'invalid_state');
    await expectStatus(rack.start(), 'invalid_state');
    try {
      rack.add(() => a.plugin);
      expect.unreachable();
    } catch (error) {
      expect(statusOf(error)).toBe('invalid_state');
    }
    await rack.shutdown();
  });

  it('boots configured directories and succeeds on empty ones', async () => {
    const rack = createRack({ pluginDirectories: [singleDir] });
    await rack.boot();
    const info = rack.describe('ahost.provider');
    expect(info?.state).toBe('active');
    expect(info?.sourcePath).toBe(providerPath);
    expect(info?.version).toEqual(v(1, 2, 0));
    expect(rack.plugins().length).toBe(1);
    await rack.shutdown();

    const empty = createRack({ pluginDirectories: [emptyDir] });
    await empty.boot();
    expect(empty.plugins()).toEqual([]);

    const bare = createRack();
    await bare.boot();
    expect(bare.plugins()).toEqual([]);
  });
});
