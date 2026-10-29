/**
 * @file logging.test.ts
 * @brief Tests for the std.log standard plugin, its demo consumer and formatDiagEntry.
 *
 * @details All PluginServices are self-written fakes; no host implementation, loader or other
 *          agent module is imported. External effects (clock, stderr, file appender,
 *          environment) are injected through createWithDeps(); the two real-filesystem cases
 *          use a temporary directory and are cleaned up in afterAll.
 */
import { afterAll, describe, expect, it } from 'vitest';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { isRackError, RackError, statusOf } from '../../src/sdk/index.ts';
import type {
  CapEvent,
  CapSink,
  CapsDesc,
  EventSink,
  LeaseCredential,
  LeaseHandle,
  PluginServices,
  Revoker,
  Subscription,
  VersionRange,
} from '../../src/sdk/index.ts';
import type { JsonValue } from '../../src/core/json.ts';
import {
  create,
  createWithDeps,
  formatDiagEntry,
} from '../../src/plugins/logging/index.ts';
import type { LoggingDeps, QueryResultDoc, StatsResultDoc } from '../../src/plugins/logging/index.ts';
import { create as createDemo } from '../../examples/plugins/logging-demo.plug.ts';
import type { RackPlugin } from '../../src/sdk/index.ts';

// ---------------------------------------------------------------------------
// Self-written fake PluginServices: records capability publications, watchers,
// lease acquisitions/releases, gateway calls and diagnostics.
// ---------------------------------------------------------------------------

/** @brief One recorded gateway call. */
interface RecordedCall {
  credential: LeaseCredential;
  methodName: string;
  args: JsonValue;
}

/**
 * @brief Build a fake PluginServices bundle backed by plain records.
 *
 * @param plugId plugId reported through the services view.
 * @return The services object plus record arrays and a capability delivery helper.
 */
function makeFakeServices(plugId: string) {
  const publishedCaps: CapsDesc[] = [];
  const capWatchers: CapSink[] = [];
  const acquisitions: Array<{ plugId: string; range: VersionRange; revoker: Revoker }> = [];
  const releases: LeaseCredential[] = [];
  const calls: RecordedCall[] = [];
  const diagnostics: Array<{ level: string; message: string }> = [];
  let credentialSeq = 0;

  const noopUnsubscribe = (): void => undefined;
  const services: PluginServices = {
    plugId,
    events: {
      subscribe(_name: string, _sink: EventSink): Subscription {
        return { unsubscribe: noopUnsubscribe };
      },
      publish() {
        // Plugins under test publish no ordinary events.
      },
    },
    caps: {
      publish(caps: CapsDesc): void {
        publishedCaps.push(caps);
      },
      watch(sink: CapSink): Subscription {
        capWatchers.push(sink);
        return { unsubscribe: noopUnsubscribe };
      },
      snapshot(): readonly CapEvent[] {
        return [];
      },
      acquire(providerId: string, range: VersionRange, revoker: Revoker): LeaseHandle {
        acquisitions.push({ plugId: providerId, range, revoker });
        credentialSeq += 1;
        const credential: LeaseCredential = {
          kind: 'u42.lease.credential',
          handle: { seq: credentialSeq },
        };
        return {
          credential,
          plugId: providerId,
          version: { major: 1, minor: 0, patch: 0 },
          release(): Promise<'ok' | 'busy' | 'stale'> {
            releases.push(credential);
            return Promise.resolve('ok');
          },
        };
      },
      release(credential: LeaseCredential): 'ok' | 'busy' | 'stale' {
        releases.push(credential);
        return 'ok';
      },
    },
    calls: {
      call(credential: LeaseCredential, methodName: string, args: JsonValue): Promise<JsonValue> {
        calls.push({ credential, methodName, args });
        return Promise.resolve({ accepted: true });
      },
      callById(credential: LeaseCredential, methodId: number, args: JsonValue): Promise<JsonValue> {
        calls.push({ credential, methodName: `#${methodId}`, args });
        return Promise.resolve({ accepted: true });
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
    async deliverCapability(event: CapEvent): Promise<void> {
      for (const sink of [...capWatchers]) await sink.onCapability(event);
    },
  };
}

// ---------------------------------------------------------------------------
// Shared helpers.
// ---------------------------------------------------------------------------

/** @brief Deterministic clock: first call returns start + step. */
function fakeClock(start = 1000, step = 10): () => number {
  let current = start;
  return () => {
    current += step;
    return current;
  };
}

/** @brief Silent stderr replacement keeping test output clean. */
const silentStderr = (): void => undefined;

/**
 * @brief Invoke one method on a plugin and normalize sync/async results.
 *
 * @param plugin Plugin under test.
 * @param methodId Numeric method selector.
 * @param methodName Name selector.
 * @param args Request args document.
 * @return The invoke result.
 */
async function invoke(
  plugin: RackPlugin,
  methodId: number,
  methodName: string,
  args: JsonValue,
): Promise<JsonValue> {
  if (plugin.invoke === undefined) throw new Error('plugin under test has no invoke');
  return await plugin.invoke({ methodId, methodName, args });
}

/**
 * @brief Assert that one invoke dispatch throws a RackError with the expected status.
 *
 * @param plugin Plugin under test.
 * @param methodId Numeric method selector.
 * @param methodName Name selector.
 * @param status Expected rack status.
 * @param args Request args document.
 */
async function expectInvokeError(
  plugin: RackPlugin,
  methodId: number,
  methodName: string,
  status: ReturnType<typeof statusOf>,
  args: JsonValue = null,
): Promise<void> {
  try {
    await invoke(plugin, methodId, methodName, args);
    expect.unreachable();
  } catch (error) {
    expect(isRackError(error)).toBe(true);
    expect(statusOf(error)).toBe(status);
  }
}

/**
 * @brief Build, init and start one std.log instance with hermetic deps.
 *
 * @param deps Effect injections; a deterministic clock and a silent stderr are added.
 * @return The started plugin and the fake services bundle.
 */
async function bootLogging(deps: LoggingDeps = {}) {
  const fake = makeFakeServices('std.log');
  const plugin = createWithDeps({ now: fakeClock(), stderr: silentStderr, ...deps });
  await plugin.init(fake.services);
  await plugin.start();
  return { plugin, fake };
}

/** @brief Query the plugin and cast the result to the query document shape. */
async function query(
  plugin: RackPlugin,
  args: JsonValue = {},
): Promise<QueryResultDoc> {
  return (await invoke(plugin, 2, 'query', args)) as QueryResultDoc;
}

/** @brief Log one entry and assert acceptance. */
async function log(
  plugin: RackPlugin,
  args: JsonValue,
): Promise<void> {
  await expect(invoke(plugin, 1, 'log', args)).resolves.toEqual({ accepted: true });
}

// ---------------------------------------------------------------------------
// std.log descriptor, capability disclosure and dispatch guards.
// ---------------------------------------------------------------------------

describe('std.log capability disclosure and dispatch', () => {
  it('announces identity 1.0.0 and exactly the log/query/stats methods on start()', async () => {
    const { plugin, fake } = await bootLogging();
    expect(plugin.desc.plugId).toBe('std.log');
    expect(plugin.desc.version).toEqual({ major: 1, minor: 0, patch: 0 });
    expect(fake.publishedCaps).toHaveLength(1);
    const methods = fake.publishedCaps[0]?.methods ?? [];
    expect(methods.map((method) => [method.id, method.name])).toEqual([
      [1, 'log'],
      [2, 'query'],
      [3, 'stats'],
    ]);
    await plugin.stop();
  });

  it('dispatches by method name and by method id identically', async () => {
    const { plugin } = await bootLogging({ env: { U42_LOG_RING: '8' } });
    await log(plugin, { level: 'info', message: 'by-name' });
    const byName = await invoke(plugin, 999, 'query', null);
    const byId = await invoke(plugin, 2, 'totally-unknown-name', null);
    const nameDoc = byName as QueryResultDoc;
    const idDoc = byId as QueryResultDoc;
    expect(nameDoc.total).toBe(1);
    expect(idDoc.total).toBe(1);
    expect(nameDoc.entries[0]?.message).toBe('by-name');
    expect(idDoc.entries[0]?.message).toBe('by-name');
    await plugin.stop();
  });

  it('rejects unknown methods with not_found', async () => {
    const { plugin } = await bootLogging();
    await expectInvokeError(plugin, 77, 'flush', 'not_found');
    await plugin.stop();
  });

  it('rejects invoke after stop with invalid_state', async () => {
    const { plugin } = await bootLogging();
    await log(plugin, { level: 'info', message: 'before stop' });
    await plugin.stop();
    await expectInvokeError(plugin, 3, 'stats', 'invalid_state');
    await expectInvokeError(plugin, 1, 'log', 'invalid_state', { level: 'info', message: 'x' });
  });

  it('create() defaults to the real effects and the frozen default capacity', async () => {
    const fake = makeFakeServices('std.log');
    const plugin = create();
    await plugin.init(fake.services);
    await plugin.start();
    const stats = (await invoke(plugin, 3, 'stats', null)) as StatsResultDoc;
    expect(stats.capacity).toBe(4096);
    expect(stats.used).toBe(0);
    await plugin.stop();
  });
});

// ---------------------------------------------------------------------------
// log/query/stats roundtrip and argument validation.
// ---------------------------------------------------------------------------

describe('std.log log/query/stats roundtrip', () => {
  it('stores entries with the frozen five-field shape and returns them ts ascending', async () => {
    const { plugin } = await bootLogging();
    await log(plugin, { level: 'info', message: 'first', fields: { code: 1 }, plugId: 'std.net' });
    await log(plugin, { level: 'warn', message: 'second' });
    await log(plugin, { level: 'error', message: 'third', fields: null, plugId: null });

    const result = await query(plugin);
    expect(result.total).toBe(3);
    expect(result.entries).toHaveLength(3);
    const [first, second, third] = result.entries;
    expect(Object.keys(first ?? {}).sort()).toEqual(['fields', 'level', 'message', 'plugId', 'ts']);
    expect(first?.level).toBe('info');
    expect(first?.message).toBe('first');
    expect(first?.fields).toEqual({ code: 1 });
    expect(first?.plugId).toBe('std.net');
    expect(second?.fields).toBeNull();
    expect(second?.plugId).toBeNull();
    expect(third?.level).toBe('error');
    // Deterministic clock: ts strictly increasing and returned oldest first.
    expect(first?.ts).toBeLessThan(second?.ts ?? Number.NaN);
    expect(second?.ts).toBeLessThan(third?.ts ?? Number.NaN);
    expect(Object.isFrozen(first)).toBe(true);
    await plugin.stop();
  });

  it('accepts null query args as an empty filter document', async () => {
    const { plugin } = await bootLogging();
    await log(plugin, { level: 'info', message: 'only' });
    const result = await query(plugin, null);
    expect(result.total).toBe(1);
    expect(result.entries[0]?.message).toBe('only');
    await plugin.stop();
  });

  it('reports byLevel counts, capacity and used slots', async () => {
    const { plugin } = await bootLogging({ env: { U42_LOG_RING: '16' } });
    await log(plugin, { level: 'info', message: 'a' });
    await log(plugin, { level: 'info', message: 'b' });
    await log(plugin, { level: 'warn', message: 'c' });
    await log(plugin, { level: 'error', message: 'd' });
    const stats = (await invoke(plugin, 3, 'stats', null)) as StatsResultDoc;
    expect(stats).toEqual({
      byLevel: { info: 2, warn: 1, error: 1 },
      capacity: 16,
      used: 4,
    });
    await plugin.stop();
  });

  it('rejects malformed log args with invalid_argument', async () => {
    const { plugin } = await bootLogging();
    await expectInvokeError(plugin, 1, 'log', 'invalid_argument', null);
    await expectInvokeError(plugin, 1, 'log', 'invalid_argument', ['info']);
    await expectInvokeError(plugin, 1, 'log', 'invalid_argument', { message: 'no level' });
    await expectInvokeError(plugin, 1, 'log', 'invalid_argument', {
      level: 'debug',
      message: 'bad level',
    });
    await expectInvokeError(plugin, 1, 'log', 'invalid_argument', { level: 'info' });
    await expectInvokeError(plugin, 1, 'log', 'invalid_argument', {
      level: 'info',
      message: 7,
    });
    await expectInvokeError(plugin, 1, 'log', 'invalid_argument', {
      level: 'info',
      message: 'm',
      fields: 'not-an-object',
    });
    await expectInvokeError(plugin, 1, 'log', 'invalid_argument', {
      level: 'info',
      message: 'm',
      plugId: 42,
    });
    const stats = (await invoke(plugin, 3, 'stats', null)) as StatsResultDoc;
    expect(stats.used).toBe(0);
    await plugin.stop();
  });

  it('rejects malformed query filters with invalid_argument', async () => {
    const { plugin } = await bootLogging();
    await expectInvokeError(plugin, 2, 'query', 'invalid_argument', { level: 'debug' });
    await expectInvokeError(plugin, 2, 'query', 'invalid_argument', { since: 'yesterday' });
    await expectInvokeError(plugin, 2, 'query', 'invalid_argument', { limit: 0 });
    await expectInvokeError(plugin, 2, 'query', 'invalid_argument', { limit: 2.5 });
    await expectInvokeError(plugin, 2, 'query', 'invalid_argument', 'not-an-object');
    await plugin.stop();
  });
});

// ---------------------------------------------------------------------------
// Ring buffer eviction and environment capacity resolution.
// ---------------------------------------------------------------------------

describe('std.log ring eviction and U42_LOG_RING', () => {
  it('keeps only the newest capacity entries (capacity 3)', async () => {
    const { plugin } = await bootLogging({ env: { U42_LOG_RING: '3' } });
    for (const message of ['m1', 'm2', 'm3', 'm4']) {
      await log(plugin, { level: 'info', message });
    }
    const stats = (await invoke(plugin, 3, 'stats', null)) as StatsResultDoc;
    expect(stats.capacity).toBe(3);
    expect(stats.used).toBe(3);
    const result = await query(plugin);
    expect(result.total).toBe(3);
    expect(result.entries.map((entry) => entry.message)).toEqual(['m2', 'm3', 'm4']);
    await plugin.stop();
  });

  it('falls back to the 4096 default for unset or malformed values', async () => {
    for (const raw of [undefined, 'abc', '0', '-4', '2.5', '']) {
      const env: Record<string, string | undefined> = {};
      if (raw !== undefined) env.U42_LOG_RING = raw;
      const { plugin } = await bootLogging({ env });
      const stats = (await invoke(plugin, 3, 'stats', null)) as StatsResultDoc;
      expect(stats.capacity).toBe(4096);
      await plugin.stop();
    }
  });

  it('honours a positive integer override', async () => {
    const { plugin } = await bootLogging({ env: { U42_LOG_RING: '1' } });
    await log(plugin, { level: 'info', message: 'only-one' });
    await log(plugin, { level: 'info', message: 'evicts-first' });
    const result = await query(plugin);
    expect(result.entries.map((entry) => entry.message)).toEqual(['evicts-first']);
    await plugin.stop();
  });
});

// ---------------------------------------------------------------------------
// Query filters: level, since, limit.
// ---------------------------------------------------------------------------

describe('std.log query filters', () => {
  /**
   * @brief Seed five entries with known levels and timestamps.
   *
   * @param plugin Started plugin under test.
   * @param clock The clock instance injected into the plugin.
   * @return The five entry timestamps in insertion order.
   */
  async function seed(plugin: RackPlugin, clock: () => number): Promise<number[]> {
    const stamps: number[] = [];
    const push = async (level: string, message: string): Promise<void> => {
      stamps.push(clock());
      await log(plugin, { level, message });
    };
    await push('info', 'i1');
    await push('warn', 'w1');
    await push('error', 'e1');
    await push('warn', 'w2');
    await push('info', 'i2');
    return stamps;
  }

  it('filters by level', async () => {
    const clock = fakeClock();
    const { plugin } = await bootLogging({ now: clock });
    await seed(plugin, clock);
    const result = await query(plugin, { level: 'warn' });
    expect(result.total).toBe(2);
    expect(result.entries.map((entry) => entry.message)).toEqual(['w1', 'w2']);
    await plugin.stop();
  });

  it('filters by since (inclusive epoch ms) and combines with level', async () => {
    const clock = fakeClock();
    const { plugin } = await bootLogging({ now: clock });
    const stamps = await seed(plugin, clock);
    const sinceTs = stamps[2] ?? 0;
    const sinceResult = await query(plugin, { since: sinceTs });
    expect(sinceResult.total).toBe(3);
    expect(sinceResult.entries.map((entry) => entry.message)).toEqual(['e1', 'w2', 'i2']);
    const combined = await query(plugin, { level: 'warn', since: sinceTs });
    expect(combined.entries.map((entry) => entry.message)).toEqual(['w2']);
    await plugin.stop();
  });

  it('applies limit to the newest window while keeping ts order and full total', async () => {
    const clock = fakeClock();
    const { plugin } = await bootLogging({ now: clock });
    await seed(plugin, clock);
    const limited = await query(plugin, { limit: 2 });
    expect(limited.total).toBe(5);
    expect(limited.entries.map((entry) => entry.message)).toEqual(['w2', 'i2']);
    await plugin.stop();
  });

  it('defaults the window to the newest 100 entries', async () => {
    const { plugin } = await bootLogging();
    for (let index = 0; index < 150; index++) {
      await log(plugin, { level: 'info', message: `entry-${index}` });
    }
    const result = await query(plugin);
    expect(result.total).toBe(150);
    expect(result.entries).toHaveLength(100);
    expect(result.entries[0]?.message).toBe('entry-50');
    expect(result.entries[99]?.message).toBe('entry-149');
    await plugin.stop();
  });
});

// ---------------------------------------------------------------------------
// Console sink (stderr JSON lines).
// ---------------------------------------------------------------------------

describe('std.log console sink', () => {
  it('writes one JSON line per accepted entry to the injected stderr', async () => {
    const lines: string[] = [];
    const { plugin } = await bootLogging({ stderr: (chunk) => lines.push(chunk) });
    await log(plugin, { level: 'warn', message: 'to-stderr', plugId: 'std.net' });
    expect(lines).toHaveLength(1);
    const line = lines[0] ?? '';
    expect(line.endsWith('\n')).toBe(true);
    const parsed = JSON.parse(line) as Record<string, unknown>;
    expect(parsed.level).toBe('warn');
    expect(parsed.message).toBe('to-stderr');
    expect(parsed.plugId).toBe('std.net');
    expect(parsed.fields).toBeNull();
    expect(typeof parsed.ts).toBe('number');
    await plugin.stop();
  });

  it('never rejects a log call when the stderr writer throws', async () => {
    const { plugin } = await bootLogging({
      stderr: () => {
        throw new Error('stderr broken');
      },
    });
    await log(plugin, { level: 'error', message: 'still accepted' });
    const result = await query(plugin);
    expect(result.total).toBe(1);
    await plugin.stop();
  });
});

// ---------------------------------------------------------------------------
// File sink with injected appenders (ordering, degradation).
// ---------------------------------------------------------------------------

describe('std.log file sink with injected appender', () => {
  it('appends one JSON line per entry in order when U42_LOG_FILE is set', async () => {
    const appends: Array<{ path: string; data: string }> = [];
    const { plugin } = await bootLogging({
      env: { U42_LOG_FILE: '/virtual/u42.log' },
      appendFile: async (path, data) => {
        appends.push({ path, data });
      },
    });
    await log(plugin, { level: 'info', message: 'one' });
    await log(plugin, { level: 'error', message: 'two' });
    await plugin.stop(); // stop() flushes the append chain
    expect(appends).toHaveLength(2);
    expect(appends.map((entry) => entry.path)).toEqual(['/virtual/u42.log', '/virtual/u42.log']);
    const first = JSON.parse(appends[0]?.data ?? '{}') as Record<string, unknown>;
    expect(first.message).toBe('one');
    const second = JSON.parse(appends[1]?.data ?? '{}') as Record<string, unknown>;
    expect(second.level).toBe('error');
    expect((appends[1]?.data ?? '').endsWith('\n')).toBe(true);
  });

  it('degrades silently to memory-only after the first append failure', async () => {
    let attempts = 0;
    const { plugin } = await bootLogging({
      env: { U42_LOG_FILE: '/virtual/denied.log' },
      appendFile: async () => {
        attempts += 1;
        throw new RackError('failed', 'simulated EACCES');
      },
    });
    await log(plugin, { level: 'info', message: 'first' });
    await log(plugin, { level: 'info', message: 'second' });
    const memoryOnly = await query(plugin);
    expect(memoryOnly.total).toBe(2);
    await plugin.stop();
    expect(attempts).toBe(1); // disabled after the first failure
  });

  it('keeps accepting entries when the appender throws synchronously', async () => {
    let attempts = 0;
    const { plugin } = await bootLogging({
      env: { U42_LOG_FILE: '/virtual/sync-throw.log' },
      appendFile: () => {
        attempts += 1;
        throw new Error('sync append failure');
      },
    });
    await log(plugin, { level: 'warn', message: 'survives' });
    const result = await query(plugin);
    expect(result.total).toBe(1);
    await plugin.stop();
    expect(attempts).toBe(1);
  });

  it('stays memory-only when U42_LOG_FILE is unset or empty', async () => {
    const appends: string[] = [];
    const deps: LoggingDeps = {
      appendFile: async (_path, data) => {
        appends.push(data);
      },
    };
    for (const env of [{}, { U42_LOG_FILE: '' }] as Array<Record<string, string | undefined>>) {
      const { plugin } = await bootLogging({ ...deps, env });
      await log(plugin, { level: 'info', message: 'no-file' });
      await plugin.stop();
    }
    expect(appends).toHaveLength(0);
  });
});

// ---------------------------------------------------------------------------
// File sink against the real filesystem (temporary directory).
// ---------------------------------------------------------------------------

describe('std.log file sink on the real filesystem', () => {
  const tempDirs: string[] = [];

  /** @brief Create a unique temporary directory tracked for afterAll cleanup. */
  async function makeTempDir(): Promise<string> {
    const dir = await mkdtemp(join(tmpdir(), 'u42-logging-test-'));
    tempDirs.push(dir);
    return dir;
  }

  afterAll(async () => {
    for (const dir of tempDirs) await rm(dir, { recursive: true, force: true });
  });

  it('persists JSON lines to the file named by U42_LOG_FILE', async () => {
    const dir = await makeTempDir();
    const filePath = join(dir, 'app.log');
    const clock = fakeClock();
    const fake = makeFakeServices('std.log');
    const plugin = createWithDeps({ now: clock, stderr: silentStderr, env: { U42_LOG_FILE: filePath } });
    await plugin.init(fake.services);
    await plugin.start();
    await log(plugin, { level: 'info', message: 'one', plugId: 'std.http-client' });
    await log(plugin, { level: 'warn', message: 'two' });
    await log(plugin, { level: 'error', message: 'three' });
    await plugin.stop();

    const text = await readFile(filePath, 'utf8');
    const lines = text.split('\n').filter((line) => line.length > 0);
    expect(lines).toHaveLength(3);
    const parsed = lines.map((line) => JSON.parse(line) as Record<string, unknown>);
    expect(parsed.map((entry) => entry.message)).toEqual(['one', 'two', 'three']);
    expect(parsed[0]?.plugId).toBe('std.http-client');
    expect(parsed.every((entry) => typeof entry.ts === 'number')).toBe(true);
  });

  it('accepts entries and keeps them in memory when the file path is invalid', async () => {
    const dir = await makeTempDir();
    // A path whose parent is a regular file: append fails with ENOTDIR everywhere.
    const blocker = join(dir, 'blocker');
    await writeFile(blocker, 'not a directory', 'utf8');
    const badPath = join(blocker, 'u42.log');

    const { plugin } = await bootLogging({ env: { U42_LOG_FILE: badPath } });
    await log(plugin, { level: 'info', message: 'degrades' });
    await log(plugin, { level: 'info', message: 'still buffered' });
    const result = await query(plugin);
    expect(result.total).toBe(2);
    await expect(plugin.stop()).resolves.toBeUndefined();
  });
});

// ---------------------------------------------------------------------------
// stop() resource release.
// ---------------------------------------------------------------------------

describe('std.log stop()', () => {
  it('clears the buffer and survives a second stop', async () => {
    const { plugin } = await bootLogging();
    await log(plugin, { level: 'info', message: 'will be dropped' });
    await plugin.stop();
    await expect(plugin.stop()).resolves.toBeUndefined();
  });
});

// ---------------------------------------------------------------------------
// formatDiagEntry diagnostic bridge utility.
// ---------------------------------------------------------------------------

describe('formatDiagEntry', () => {
  it('maps a DiagService report onto log args', async () => {
    expect(formatDiagEntry('warn', 'plain')).toEqual({ level: 'warn', message: 'plain' });
    expect(formatDiagEntry('info', 'with object', { code: 7, retry: false })).toEqual({
      level: 'info',
      message: 'with object',
      fields: { code: 7, retry: false },
    });
    // Non-object detail values are wrapped so nothing is lost.
    expect(formatDiagEntry('error', 'with text', 'boom')).toEqual({
      level: 'error',
      message: 'with text',
      fields: { detail: 'boom' },
    });
    expect(formatDiagEntry('error', 'null detail', null)).toEqual({
      level: 'error',
      message: 'null detail',
    });
    expect(formatDiagEntry('info', 'no detail', undefined)).toEqual({
      level: 'info',
      message: 'no detail',
    });
  });

  it('produces documents the log method accepts', async () => {
    const { plugin } = await bootLogging();
    await log(plugin, formatDiagEntry('warn', 'bridged', { source: 'host' }));
    const result = await query(plugin);
    expect(result.entries[0]?.fields).toEqual({ source: 'host' });
    expect(result.entries[0]?.level).toBe('warn');
    await plugin.stop();
  });
});

// ---------------------------------------------------------------------------
// logging-demo example consumer (fake services only).
// ---------------------------------------------------------------------------

/** @brief std.log availability fact for the given provider version. */
function stdLogAvailable(major = 1, minor = 0, patch = 0): CapEvent {
  return {
    plugId: 'std.log',
    available: true,
    version: { major, minor, patch },
    capabilities: {
      methods: [
        { id: 1, name: 'log' },
        { id: 2, name: 'query' },
        { id: 3, name: 'stats' },
      ],
    },
  };
}

/** @brief std.log withdrawal fact. */
function stdLogWithdrawn(): CapEvent {
  return {
    plugId: 'std.log',
    available: false,
    version: { major: 1, minor: 0, patch: 0 },
    capabilities: { methods: [] },
  };
}

describe('logging-demo consumer', () => {
  it('announces example.logging-demo with a hard init edge on std.log', () => {
    const demo = createDemo();
    expect(demo.desc.plugId).toBe('example.logging-demo');
    expect(demo.desc.version).toEqual({ major: 1, minor: 0, patch: 0 });
    expect(demo.desc.init.after).toEqual(['std.log']);
  });

  it('boot-logs through the gateway when the lease is held before start()', async () => {
    const fake = makeFakeServices('example.logging-demo');
    const demo = createDemo();
    await demo.init(fake.services);
    await fake.deliverCapability(stdLogAvailable());
    expect(fake.acquisitions).toHaveLength(1);
    expect(fake.acquisitions[0]?.plugId).toBe('std.log');
    expect(fake.acquisitions[0]?.range).toEqual({
      minimum: { major: 1, minor: 0, patch: 0 },
      maximum: { major: 1, minor: 0, patch: 0 },
    });

    await demo.start();
    // The boot entries are deferred to the next macrotask (Active-consumer rule).
    await new Promise<void>((resolve) => setImmediate(resolve));
    expect(fake.publishedCaps).toHaveLength(1);
    expect(fake.publishedCaps[0]?.methods.map((method) => method.name)).toEqual(['run']);
    const bootCalls = fake.calls.filter((call) => call.methodName === 'log');
    expect(bootCalls).toHaveLength(3);
    const fieldsCall = bootCalls.find(
      (call) => (call.args as { fields?: { rack?: string } }).fields?.rack === '42U',
    );
    expect(fieldsCall).toBeDefined();
    expect(bootCalls.every((call) => (call.args as { plugId?: string }).plugId === 'example.logging-demo')).toBe(true);

    const runResult = await invoke(demo, 1, 'run', { message: 'hello rack' });
    expect(runResult).toEqual({ logged: true });
    const allCalls = fake.calls.filter((call) => call.methodName === 'log');
    expect(allCalls).toHaveLength(4);
    const last = allCalls[allCalls.length - 1]?.args as {
      level?: string;
      message?: string;
      fields?: { source?: string };
    };
    expect(last.level).toBe('info');
    expect(last.message).toBe('hello rack');
    expect(last.fields?.source).toBe('example.logging-demo');

    await demo.stop();
    expect(fake.releases).toHaveLength(1);
  });

  it('boot-logs from the capability callback when availability arrives after start()', async () => {
    const fake = makeFakeServices('example.logging-demo');
    const demo = createDemo();
    await demo.init(fake.services);
    await demo.start();
    expect(fake.calls).toHaveLength(0);
    await fake.deliverCapability(stdLogAvailable());
    const bootCalls = fake.calls.filter((call) => call.methodName === 'log');
    expect(bootCalls).toHaveLength(3);
    await demo.stop();
  });

  it('reports run as not logged without a usable std.log lease', async () => {
    const fake = makeFakeServices('example.logging-demo');
    const demo = createDemo();
    await demo.init(fake.services);
    await demo.start();
    const result = await invoke(demo, 1, 'run', { message: 'no provider' });
    expect(result).toEqual({ logged: false, reason: 'std.log lease not held' });
    expect(fake.calls).toHaveLength(0);
    await demo.stop();
  });

  it('stops logging business after a std.log withdrawal fact', async () => {
    const fake = makeFakeServices('example.logging-demo');
    const demo = createDemo();
    await demo.init(fake.services);
    await fake.deliverCapability(stdLogAvailable());
    await demo.start();
    await fake.deliverCapability(stdLogWithdrawn());
    const result = await invoke(demo, 1, 'run', { message: 'after withdrawal' });
    expect(result).toEqual({ logged: false, reason: 'std.log lease not held' });
    await demo.stop();
  });

  it('ignores std.log versions outside the accepted range', async () => {
    const fake = makeFakeServices('example.logging-demo');
    const demo = createDemo();
    await demo.init(fake.services);
    await fake.deliverCapability(stdLogAvailable(2, 0, 0));
    expect(fake.acquisitions).toHaveLength(0);
    await demo.start();
    const result = await invoke(demo, 1, 'run', { message: 'wrong version' });
    expect(result).toEqual({ logged: false, reason: 'std.log lease not held' });
    await demo.stop();
  });

  it('validates run args and rejects unknown methods', async () => {
    const fake = makeFakeServices('example.logging-demo');
    const demo = createDemo();
    await demo.init(fake.services);
    await demo.start();
    await expectInvokeError(demo, 1, 'run', 'invalid_argument', null);
    await expectInvokeError(demo, 1, 'run', 'invalid_argument', { message: 7 });
    await expectInvokeError(demo, 9, 'nope', 'not_found', null);
    await demo.stop();
  });
});
