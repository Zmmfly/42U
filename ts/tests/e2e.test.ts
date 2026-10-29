/**
 * @file e2e.test.ts
 * @brief End-to-end assembly tests: one real rack, real node:http traffic, no mocks.
 *
 * @details Boots the real host from examples/plugins plus all eight standard devices and an
 *          in-process driver plugin, then verifies the machine-checkable subset of the demo
 *          scenarios: instance inventory and states, typedApi one-shot and long-held leases,
 *          the event flow closure (example.data -> consumer -> storage -> read back), a REST
 *          resource mounted through std.rest-server hit by a real HTTP request, both RPC
 *          loopbacks (POST /rpc bridge and the std.rpc client against a mini endpoint),
 *          std.log write+query, hot-pluggability (reload generation bump with stale
 *          credentials, unload with not_found borrows, hot re-load), and a graceful shutdown
 *          that leaves plugins() empty. The listening port is probed from the free
 *          18000-18999 range before boot; the whole suite shares one rack for speed.
 */

import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { createServer } from 'node:net';

import { addStandardPlugins, createRack, exactVersion, statusOf, typedApi } from '../src/index.ts';
import type { RackWithDiagnostics } from '../src/index.ts';
import type {
  CapEvent,
  CapSink,
  JsonObject,
  JsonValue,
  LeaseCredential,
  LeaseHandle,
  MethodDesc,
  PluginServices,
  Rack,
  RackPlugin,
  Revoker,
  VersionRange,
} from '../src/index.ts';

// ---------------------------------------------------------------------------
// Shared constants and small JSON helpers.
// ---------------------------------------------------------------------------

/** @brief Inclusive 1.x range used for long-lived leases in these tests. */
const RANGE_V1: VersionRange = {
  minimum: { major: 1, minor: 0, patch: 0 },
  maximum: { major: 1, minor: Number.MAX_SAFE_INTEGER, patch: Number.MAX_SAFE_INTEGER },
};

/** @brief plugId of the in-process test driver (consumer, event publisher, HTTP target). */
const DRIVER_PLUG_ID = 'e2e.driver';

/** @brief REST prefix mounted on std.rest-server for the real HTTP assertion. */
const REST_PREFIX = '/e2e/rack';

/** @brief POST path registered through route.add for the client loopback. */
const CLIENT_RPC_PATH = '/e2e/rpc';

/**
 * @brief Compare two JSON values structurally via their JSON text.
 *
 * @param left First value.
 * @param right Second value.
 * @return true when both serialize identically.
 */
function jsonEquals(left: JsonValue, right: JsonValue): boolean {
  return JSON.stringify(left) === JSON.stringify(right);
}

/**
 * @brief Narrow a JSON value to a plain object view.
 *
 * @param value Candidate value.
 * @return The value as a JsonObject, or null for anything else.
 */
function asObjectOrNull(value: JsonValue | undefined): JsonObject | null {
  return typeof value === 'object' && value !== null && !Array.isArray(value)
    ? (value as JsonObject)
    : null;
}

/**
 * @brief Probe one free TCP port from the 18000-18999 range.
 *
 * @return A port that accepted and released a probe listener.
 */
async function findFreePort(): Promise<number> {
  for (let attempt = 0; attempt < 16; attempt += 1) {
    const port = 18000 + Math.floor(Math.random() * 1000);
    const free = await new Promise<boolean>((resolve) => {
      const probe = createServer();
      probe.once('error', () => resolve(false));
      probe.listen(port, '127.0.0.1', () => {
        probe.close(() => resolve(true));
      });
    });
    if (free) return port;
  }
  throw new Error('no free port found in the 18000-18999 range');
}

// ---------------------------------------------------------------------------
// The e2e driver plugin.
// ---------------------------------------------------------------------------

/** @brief Control surface the tests use after boot. */
interface TestDriver {
  readonly services: PluginServices;
  readonly capFacts: CapEvent[];
  acquire(plugId: string, range: VersionRange): LeaseHandle;
}

/**
 * @brief Build the in-process driver plugin used by every test below.
 *
 * @return The plugin to stage on the rack plus its control surface.
 * @note Publishes `info` (REST target) and `serve` (JSON-RPC mini endpoint for the std.rpc
 *       client loopback; see the coordinator ruling in apps/demo.ts step 5 — the client
 *       cannot address its own rack's /rpc because the bridge would re-enter std.rpc while
 *       its client method is in flight, which the gateway rejects with 'busy').
 */
function createDriverPlugin(): { plugin: RackPlugin; driver: TestDriver } {
  let services: PluginServices | null = null;
  const capFacts: CapEvent[] = [];
  const tracked = new Map<object, LeaseHandle>();

  const revoker: Revoker = {
    async onRevoke(credential: LeaseCredential): Promise<void> {
      const held = tracked.get(credential.handle);
      if (held === undefined) return;
      tracked.delete(credential.handle);
      await held.release();
    },
  };

  function driverServices(): PluginServices {
    if (services === null) throw new Error('e2e.driver has not been initialized');
    return services;
  }

  function trackedAcquire(plugId: string, range: VersionRange): LeaseHandle {
    const handle = driverServices().caps.acquire(plugId, range, revoker);
    tracked.set(handle.credential.handle, handle);
    return handle;
  }

  const methods: readonly MethodDesc[] = [
    { id: 1, name: 'info', description: 'REST target greeting the :id path parameter.' },
    { id: 2, name: 'serve', description: 'JSON-RPC 2.0 mini endpoint for the client loopback.' },
  ];

  const capSink: CapSink = {
    onCapability(event: CapEvent): void {
      capFacts.push(event);
    },
  };

  async function serveJsonRpc(args: JsonValue): Promise<JsonValue> {
    const envelope = asObjectOrNull(args);
    const body = envelope !== null ? envelope['body'] : null;
    const request = asObjectOrNull(body);
    const id: JsonValue = (request !== null ? request['id'] : undefined) ?? null;
    const method = request !== null ? request['method'] : undefined;
    if (request === null || request['jsonrpc'] !== '2.0' || typeof method !== 'string') {
      return { jsonrpc: '2.0', id, error: { code: -32600, message: 'invalid JSON-RPC request' } };
    }
    if (method !== 'example.storage.get') {
      return { jsonrpc: '2.0', id, error: { code: -32601, message: `method ${method} not served` } };
    }
    const params = asObjectOrNull(request['params']);
    const key = params !== null ? params['key'] : undefined;
    if (typeof key !== 'string') {
      return { jsonrpc: '2.0', id, error: { code: -32602, message: 'params.key must be a string' } };
    }
    const handle = trackedAcquire('example.storage', RANGE_V1);
    try {
      const result = await driverServices().calls.call(handle.credential, 'get', { key });
      return { jsonrpc: '2.0', id, result };
    } catch (error) {
      return {
        jsonrpc: '2.0',
        id,
        error: { code: -32603, message: error instanceof Error ? error.message : String(error) },
      };
    } finally {
      tracked.delete(handle.credential.handle);
      await handle.release();
    }
  }

  const plugin: RackPlugin = {
    desc: { plugId: DRIVER_PLUG_ID, version: { major: 1, minor: 0, patch: 0 }, init: {} },
    init: (injected: PluginServices): void => {
      services = injected;
      injected.caps.watch(capSink);
    },
    start: (): void => {
      driverServices().caps.publish({ methods });
    },
    stop: (): void => undefined,
    invoke: (request) => {
      if (request.methodId === 1 || request.methodName === 'info') {
        const envelope = asObjectOrNull(request.args);
        const params = asObjectOrNull(envelope !== null ? envelope['params'] : null) ?? {};
        const raw = params['id'] ?? params['name'];
        const name = typeof raw === 'string' ? raw : 'stranger';
        return { hello: name, servedBy: DRIVER_PLUG_ID };
      }
      if (request.methodId === 2 || request.methodName === 'serve') {
        return serveJsonRpc(request.args);
      }
      throw new Error(`unknown e2e.driver method ${request.methodName}`);
    },
  };

  return {
    plugin,
    driver: {
      get services(): PluginServices {
        return driverServices();
      },
      capFacts,
      acquire: trackedAcquire,
    },
  };
}

// ---------------------------------------------------------------------------
// Typed provider facades (duplicated constants only, JSON in/out).
// ---------------------------------------------------------------------------

/** @brief example.echo data contract. */
interface EchoApi {
  echo(payload: JsonValue): Promise<JsonValue>;
}

/** @brief example.storage data contract. */
interface StorageApi {
  set(request: { key: string; value: JsonValue }): Promise<JsonValue>;
  get(request: { key: string }): Promise<JsonValue>;
}

/** @brief example.consumer data contract. */
interface ConsumerApi {
  status(): Promise<JsonValue>;
}

/** @brief std.http-server data contract subset used here. */
interface HttpServerApi {
  stats(): Promise<JsonValue>;
  'route.add'(request: JsonObject): Promise<JsonValue>;
}

/** @brief std.rest-server data contract subset used here. */
interface RestServerApi {
  'resource.mount'(request: JsonObject): Promise<JsonValue>;
}

/** @brief std.rpc client data contract. */
interface RpcApi {
  call(request: { url: string; method: string; params: JsonValue; timeoutMs: number }): Promise<JsonValue>;
}

/** @brief std.log data contract. */
interface LogApi {
  log(request: JsonObject): Promise<JsonValue>;
  query(request: JsonObject): Promise<JsonValue>;
}

// ---------------------------------------------------------------------------
// Shared rack state.
// ---------------------------------------------------------------------------

/** @brief The rack under test, created once in beforeAll. */
let rack: RackWithDiagnostics;

/** @brief The driver control surface resolved after boot. */
let driver: TestDriver;

/** @brief The long-held storage lease, released during shutdown. */
let storageLease: LeaseHandle;

/** @brief The port std.http-server actually listens on. */
let httpPort = 0;

/** @brief Wall-clock budget guard: the whole suite must stay well under 30 seconds. */
const suiteStartedAt = Date.now();

/**
 * @brief One-shot helper performing a gateway call expected to reject.
 *
 * @param run Call body.
 * @return The RackError status of the rejection, or 'ok' when it resolved.
 */
async function rejectionStatus(run: () => Promise<unknown>): Promise<string> {
  try {
    await run();
    return 'ok';
  } catch (error) {
    return statusOf(error);
  }
}

beforeAll(async () => {
  const port = await findFreePort();
  process.env['U42_HTTP_PORT'] = String(port);
  rack = createRack({
    pluginDirectories: ['examples/plugins'],
    payloadLimit: 8 * 1024 * 1024,
    outputLimit: 8 * 1024 * 1024,
  });
  const { plugin, driver: control } = createDriverPlugin();
  driver = control;
  addStandardPlugins(rack as Rack);
  rack.add(() => plugin);
  await rack.boot();
  // Let the deferred business scheduled from plugin start() callbacks (rpc mount, demo
  // routes) land before the assertions begin.
  await new Promise<void>((resolve) => setImmediate(resolve));
}, 30_000);

afterAll(async () => {
  const held = storageLease as LeaseHandle | undefined;
  if (held !== undefined) {
    try {
      await held.release();
    } catch {
      // Best effort: shutdown revokes outstanding leases anyway.
    }
  }
  await rack.shutdown();
});

// ---------------------------------------------------------------------------
// Scenarios.
// ---------------------------------------------------------------------------

describe('end-to-end rack assembly', () => {
  it('boots examples + standard devices + driver, all Active, with capability facts', () => {
    const plugins = rack.plugins();
    expect(plugins.length).toBe(20);
    for (const info of plugins) {
      expect(info.state).toBe('active');
    }
    const ids = new Set(plugins.map((info) => info.plugId));
    for (const expected of [
      'example.echo',
      'example.storage',
      'example.consumer',
      'example.http-server-demo',
      'example.rest-demo',
      'example.rpc-demo',
      'std.log',
      'std.net',
      'std.http-client',
      'std.rest-client',
      'std.http-server',
      'std.rest-server',
      'std.rpc',
      'std.oauth',
      DRIVER_PLUG_ID,
    ]) {
      expect(ids.has(expected), `missing ${expected}`).toBe(true);
    }
    const snapshot = driver.services.caps.snapshot().filter((fact) => fact.available);
    expect(snapshot.length).toBeGreaterThanOrEqual(15);
    for (const fact of snapshot) {
      expect(fact.capabilities.methods.length).toBeGreaterThan(0);
    }
    expect(driver.capFacts.length).toBeGreaterThanOrEqual(snapshot.length);
  });

  it('serves typedApi calls under one-shot and long-held leases', async () => {
    const echoHandle = driver.acquire('example.echo', exactVersion({ major: 1, minor: 0, patch: 0 }));
    const echoPayload: JsonObject = { ping: true, n: 7 };
    try {
      const echoed = await typedApi<EchoApi>(driver.services.calls, echoHandle).echo(echoPayload);
      expect(jsonEquals(echoed, echoPayload)).toBe(true);
    } finally {
      expect(await echoHandle.release()).toBe('ok');
    }

    storageLease = driver.acquire('example.storage', RANGE_V1);
    expect(storageLease.version).toEqual({ major: 1, minor: 2, patch: 0 });
    const value: JsonObject = { suite: 'e2e' };
    const storage = typedApi<StorageApi>(driver.services.calls, storageLease);
    await storage.set({ key: 'e2e.typed', value });
    expect(jsonEquals(await storage.get({ key: 'e2e.typed' }), value)).toBe(true);
  });

  it('closes the event flow: publish example.data, consumer writes through to storage', async () => {
    const payload: JsonObject = { source: 'e2e', seq: 42 };
    driver.services.events.publish('example.data', payload);
    await rack.poll();

    const consumerHandle = driver.acquire('example.consumer', exactVersion({ major: 1, minor: 0, patch: 0 }));
    let status: JsonValue;
    try {
      status = await typedApi<ConsumerApi>(driver.services.calls, consumerHandle).status();
    } finally {
      await consumerHandle.release();
    }
    const statusRecord = asObjectOrNull(status);
    expect(statusRecord?.['eventsProcessed']).toBe(1);

    const stored = await typedApi<StorageApi>(driver.services.calls, storageLease).get({
      key: 'example.data',
    });
    expect(jsonEquals(stored, payload)).toBe(true);
  });

  it('serves a real HTTP request through a rest-server mounted route', async () => {
    const statsHandle = driver.acquire('std.http-server', RANGE_V1);
    let stats: JsonValue;
    try {
      stats = await typedApi<HttpServerApi>(driver.services.calls, statsHandle).stats();
    } finally {
      await statsHandle.release();
    }
    const statsRecord = asObjectOrNull(stats);
    const port = statsRecord?.['port'];
    expect(typeof port).toBe('number');
    httpPort = typeof port === 'number' ? port : 0;
    expect(httpPort).toBeGreaterThan(0);

    const restHandle = driver.acquire('std.rest-server', RANGE_V1);
    try {
      const mounted = await typedApi<RestServerApi>(driver.services.calls, restHandle)['resource.mount']({
        pathPrefix: REST_PREFIX,
        targetPlugId: DRIVER_PLUG_ID,
        methods: { get: 'info' },
      });
      expect(asObjectOrNull(mounted)?.['mounted']).toBe(true);
    } finally {
      await restHandle.release();
    }

    const response = await fetch(`http://127.0.0.1:${httpPort}${REST_PREFIX}/42u`);
    expect(response.status).toBe(200);
    const body = (await response.json()) as JsonValue;
    const record = asObjectOrNull(body);
    expect(record?.['ok']).toBe(true);
    expect(asObjectOrNull(record?.['result'])).toEqual({ hello: '42u', servedBy: DRIVER_PLUG_ID });
  });

  it('loops back through the rpc bridge and the rpc client over real HTTP', async () => {
    const value: JsonObject = { roundtrip: true, via: 'std.rpc' };
    await typedApi<StorageApi>(driver.services.calls, storageLease).set({ key: 'e2e.rpc', value });

    // Bridge loopback: a raw JSON-RPC POST against std.rpc's own /rpc route.
    const posted = await fetch(`http://127.0.0.1:${httpPort}/rpc`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({
        jsonrpc: '2.0',
        id: 7,
        method: 'example.storage.get',
        params: { key: 'e2e.rpc' },
      }),
    });
    expect(posted.status).toBe(200);
    const transport = asObjectOrNull((await posted.json()) as JsonValue);
    expect(transport?.['ok']).toBe(true);
    const envelope = asObjectOrNull(transport?.['result']);
    expect(asObjectOrNull(envelope?.['result'] ?? null)).toEqual(value);

    // Client loopback: std.rpc.call against the mini endpoint mounted via route.add.
    const routeHandle = driver.acquire('std.http-server', RANGE_V1);
    try {
      await typedApi<HttpServerApi>(driver.services.calls, routeHandle)['route.add']({
        path: CLIENT_RPC_PATH,
        httpMethod: 'POST',
        targetPlugId: DRIVER_PLUG_ID,
        targetMethod: 'serve',
      });
    } finally {
      await routeHandle.release();
    }
    const rpcHandle = driver.acquire('std.rpc', RANGE_V1);
    let clientResult: JsonValue;
    try {
      clientResult = await typedApi<RpcApi>(driver.services.calls, rpcHandle).call({
        url: `http://127.0.0.1:${httpPort}${CLIENT_RPC_PATH}`,
        method: 'example.storage.get',
        params: { key: 'e2e.rpc' },
        timeoutMs: 5000,
      });
    } finally {
      await rpcHandle.release();
    }
    expect(jsonEquals(clientResult, value)).toBe(true);
  });

  it('records and queries log entries through std.log', async () => {
    const logHandle = driver.acquire('std.log', RANGE_V1);
    let query: JsonValue;
    try {
      const logger = typedApi<LogApi>(driver.services.calls, logHandle);
      await logger.log({ level: 'info', message: 'e2e 记录一条', plugId: DRIVER_PLUG_ID });
      await logger.log({ level: 'warn', message: 'e2e 再记一条', plugId: DRIVER_PLUG_ID });
      query = await logger.query({ limit: 10 });
    } finally {
      await logHandle.release();
    }
    const record = asObjectOrNull(query);
    const entries = record?.['entries'];
    expect(Array.isArray(entries)).toBe(true);
    expect((entries as JsonValue[]).length).toBeGreaterThanOrEqual(2);
    const messages = (entries as JsonValue[])
      .map((entry) => asObjectOrNull(entry)?.['message'])
      .filter((message): message is string => typeof message === 'string');
    expect(messages).toContain('e2e 记录一条');
    expect(messages).toContain('e2e 再记一条');
  });

  it('enforces hot-pluggability: reload bumps generations, stale credentials never recover', async () => {
    const ECHO = exactVersion({ major: 1, minor: 0, patch: 0 });

    // reload() = unload + fresh load of the same source; the generation must bump.
    const reloadHandle = driver.acquire('example.echo', ECHO);
    const before = rack.describe('example.echo');
    await rack.reload('example.echo');
    const after = rack.describe('example.echo');
    expect(before).toBeDefined();
    expect(after).toBeDefined();
    expect(after!.generation).toBeGreaterThan(before!.generation);
    expect(await rejectionStatus(() =>
      driver.services.calls.call(reloadHandle.credential, 'echo', null),
    )).toBe('stale');
    const fresh = driver.acquire('example.echo', ECHO);
    const echoed = await typedApi<EchoApi>(driver.services.calls, fresh).echo({ reloaded: true });
    expect(jsonEquals(echoed, { reloaded: true })).toBe(true);
    expect(await fresh.release()).toBe('ok');

    // unload() removes the instance: stale calls and not_found borrows until a hot re-load.
    const unloadHandle = driver.acquire('example.echo', ECHO);
    await rack.unload('example.echo');
    expect(await rejectionStatus(() =>
      driver.services.calls.call(unloadHandle.credential, 'echo', null),
    )).toBe('stale');
    expect(await rejectionStatus(() => Promise.resolve(driver.acquire('example.echo', ECHO)))).toBe(
      'not_found',
    );
    await rack.load('examples/plugins/echo.plug.ts');
    const hot = driver.acquire('example.echo', ECHO);
    expect(jsonEquals(await typedApi<EchoApi>(driver.services.calls, hot).echo({ hot: true }), { hot: true })).toBe(true);
    expect(await hot.release()).toBe('ok');
  });

  it('shuts down gracefully: reverse-order unload leaves plugins() empty', async () => {
    expect(storageLease).toBeDefined();
    await storageLease.release();
    await rack.shutdown();
    expect(rack.plugins()).toEqual([]);
    expect(Date.now() - suiteStartedAt).toBeLessThan(30_000);
  });
});
