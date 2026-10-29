/**
 * @file demo.ts
 * @brief End-to-end demonstration of the assembled 42U rack (TS edition).
 *
 * @details Boots one real rack from examples/plugins plus all eight standard devices and one
 *          in-process driver plugin, then walks the eight showcase scenarios in order:
 *          capability snapshot+watch, typedApi calls (one-shot and long-held leases), the
 *          event flow (example.data -> consumer -> storage -> read back), a REST resource
 *          mounted through std.rest-server hit by a real HTTP request, an RPC loopback
 *          through std.rpc, std.log write+query, hot-pluggability (unload/reload of
 *          example.echo with stale and not_found proof), and a graceful shutdown. Every step
 *          prints a ✓/✗ line; any failure prints its reason and makes the process exit
 *          with code 1.
 *
 * @note Run from the ts/ directory: `node apps/demo.ts`. The HTTP listener port is probed
 *       from the free 18000-18999 range before boot so parallel runs do not clash.
 */

import { createServer } from 'node:net';

import {
  addStandardPlugins,
  createRack,
  definePlugin,
  exactVersion,
  RackError,
  statusOf,
  typedApi,
} from '../src/index.ts';
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
// Shared constants.
// ---------------------------------------------------------------------------

/** @brief One mebibyte, the unit of the demo rack's payload/output limits. */
const MIB = 1024 * 1024;

/** @brief plugId of the in-process driver that acts as the demo's consumer. */
const DRIVER_PLUG_ID = 'demo.driver';

/** @brief Version range accepting every 1.x.y provider, used for long-lived leases. */
const RANGE_V1: VersionRange = {
  minimum: { major: 1, minor: 0, patch: 0 },
  maximum: { major: 1, minor: Number.MAX_SAFE_INTEGER, patch: Number.MAX_SAFE_INTEGER },
};

/** @brief Path prefix of the REST resource the demo mounts on std.rest-server. */
const DEMO_REST_PREFIX = '/api/rack';

/** @brief POST path registered through std.http-server route.add for the client demo. */
const DEMO_RPC_PATH = '/rpc2';

// ---------------------------------------------------------------------------
// Small JSON helpers for assertion printing.
// ---------------------------------------------------------------------------

/**
 * @brief Render a JSON value compactly for log lines.
 *
 * @param value Value to render.
 * @return Compact JSON text.
 */
function show(value: JsonValue): string {
  return JSON.stringify(value);
}

/**
 * @brief Compare two JSON values structurally.
 *
 * @param left First value.
 * @param right Second value.
 * @return true when both serialize to the same JSON text.
 */
function jsonEquals(left: JsonValue, right: JsonValue): boolean {
  return JSON.stringify(left) === JSON.stringify(right);
}

/**
 * @brief Read one object field as a plain string.
 *
 * @param value Candidate value.
 * @param fallback Text used when the value is not a string.
 * @return The string value or its fallback.
 */
function textOf(value: JsonValue | undefined, fallback: string): string {
  return typeof value === 'string' ? value : fallback;
}

// ---------------------------------------------------------------------------
// The demo driver plugin: consumer, event publisher and REST target in one.
// ---------------------------------------------------------------------------

/** @brief Handles the driver exposes to the demo script after boot. */
interface Driver {
  /** @brief Services injected at init(); the only rack face the demo uses. */
  services: PluginServices;
  /** @brief Capability facts recorded by the driver's watch() subscription. */
  capFacts: CapEvent[];
  /** @brief Acquire a tracked lease whose revoker returns the credential on revoke. */
  acquire(plugId: string, range: VersionRange): LeaseHandle;
}

/**
 * @brief Build the in-process driver plugin plus its control handles.
 *
 * @return The plugin (stage it with rack.add) and the driver surface used by the steps.
 * @note The driver publishes exactly one method (`info`) so the REST scenario can route real
 *       HTTP traffic back into it. Its revoker returns every outstanding tracked lease, so
 *       unloading a provider never gets stuck on the demo's own borrows.
 */
function createDriverPlugin(): { plugin: RackPlugin; driver: Driver } {
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

  /**
   * @brief Access the injected services; fails fast before init() has run.
   *
   * @return The services captured during init().
   */
  function driverServices(): PluginServices {
    if (services === null) throw new Error('demo.driver has not been initialized yet');
    return services;
  }

  /**
   * @brief Acquire a tracked lease; every handle is returned by its revoker on revoke.
   *
   * @param plugId Provider identity.
   * @param range Explicit closed version range.
   * @return The minted lease handle.
   */
  function trackedAcquire(plugId: string, range: VersionRange): LeaseHandle {
    const handle = driverServices().caps.acquire(plugId, range, revoker);
    tracked.set(handle.credential.handle, handle);
    return handle;
  }

  const infoMethods: readonly MethodDesc[] = [
    {
      id: 1,
      name: 'info',
      description: 'REST demo target: greet the :id path parameter of the mounted route.',
      inputSchema: { type: 'object' },
      outputSchema: { type: 'object' },
    },
    {
      id: 2,
      name: 'serve',
      description: 'JSON-RPC 2.0 mini endpoint for the std.rpc client loopback demo.',
      inputSchema: { type: 'object' },
      outputSchema: { type: 'object' },
    },
  ];

  const capSink: CapSink = {
    onCapability(event: CapEvent): void {
      capFacts.push(event);
    },
  };

  /**
   * @brief Serve one JSON-RPC 2.0 request by forwarding example.storage.get through the gateway.
   *
   * @param args The {query, params, headers, body} envelope of std.http-server; body is the
   *        already-parsed JSON-RPC request document.
   * @return A JSON-RPC 2.0 response envelope; never throws.
   * @note Coordinator ruling: the std.rpc client cannot loop back to its own rack's /rpc
   *       (the bridge would re-enter std.rpc while its client method is in flight, which the
   *       gateway rejects with 'busy'). This mini endpoint lets the demo exercise the real
   *       client against the same rack without touching the gateway semantics.
   */
  async function serveJsonRpc(args: JsonValue): Promise<JsonValue> {
    const body =
      typeof args === 'object' && args !== null && !Array.isArray(args)
        ? (args as JsonObject)['body']
        : null;
    const request =
      typeof body === 'object' && body !== null && !Array.isArray(body) ? (body as JsonObject) : null;
    const id: JsonValue = (request !== null ? request['id'] : undefined) ?? null;
    const method = request !== null ? request['method'] : undefined;
    if (request === null || request['jsonrpc'] !== '2.0' || typeof method !== 'string') {
      return { jsonrpc: '2.0', id, error: { code: -32600, message: 'invalid JSON-RPC request' } };
    }
    if (method !== 'example.storage.get') {
      return { jsonrpc: '2.0', id, error: { code: -32601, message: `method ${method} not served` } };
    }
    const params = request['params'];
    const key =
      typeof params === 'object' && params !== null && !Array.isArray(params)
        ? (params as JsonObject)['key']
        : undefined;
    if (typeof key !== 'string') {
      return { jsonrpc: '2.0', id, error: { code: -32602, message: 'params.key must be a string' } };
    }
    const svc = driverServices();
    const handle = trackedAcquire('example.storage', RANGE_V1);
    try {
      const result = await svc.calls.call(handle.credential, 'get', { key });
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

  const plugin: RackPlugin = definePlugin(
    { plugId: DRIVER_PLUG_ID, version: { major: 1, minor: 0, patch: 0 }, init: {} },
    {
      init: (injected: PluginServices) => {
        services = injected;
        injected.caps.watch(capSink);
      },
      start: () => {
        services?.caps.publish({ methods: infoMethods });
      },
      stop: () => undefined,
      invoke: (request) => {
        if (request.methodId === 1 || request.methodName === 'info') {
          const args = request.args;
          const params =
            typeof args === 'object' && args !== null && !Array.isArray(args)
              ? ((args as JsonObject)['params'] ?? {})
              : {};
          // std.rest-server's get convention addresses items as {prefix}/:id, so the path
          // parameter arrives as params.id (fallback kept for other bridge shapes).
          const record =
            typeof params === 'object' && params !== null && !Array.isArray(params)
              ? (params as JsonObject)
              : {};
          const raw = record['id'] ?? record['name'];
          const name = textOf(raw, 'stranger');
          return { hello: name, servedBy: DRIVER_PLUG_ID };
        }
        if (request.methodId === 2 || request.methodName === 'serve') {
          return serveJsonRpc(request.args);
        }
        throw new RackError('not_found', `demo.driver: unknown method ${request.methodName} (#${request.methodId})`);
      },
    },
  );

  const driver: Driver = {
    get services(): PluginServices {
      return driverServices();
    },
    capFacts,
    acquire(plugId: string, range: VersionRange): LeaseHandle {
      return trackedAcquire(plugId, range);
    },
  };

  return { plugin, driver };
}

// ---------------------------------------------------------------------------
// Typed facades over the providers the demo borrows (constants only, JSON in/out).
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

/** @brief std.http-server data contract (the stats entry used by the demo). */
interface HttpServerStatsApi {
  stats(): Promise<JsonValue>;
}

/** @brief std.http-server route.add data contract used for the client demo endpoint. */
interface HttpServerRouteApi {
  'route.add'(request: JsonObject): Promise<JsonValue>;
}

/** @brief std.rest-server data contract (mount/unmount/list). */
interface RestServerApi {
  'resource.mount'(request: JsonObject): Promise<JsonValue>;
  'mounted.list'(): Promise<JsonValue>;
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

/**
 * @brief Unwrap the JSON-RPC result from one {ok:true, result} transport envelope.
 *
 * @param body Parsed HTTP response body.
 * @return The JSON-RPC result field, or the whole body when the shape is unexpected.
 */
function unwrapRpcEnvelope(body: JsonValue): JsonValue {
  if (typeof body !== 'object' || body === null || Array.isArray(body)) return body;
  const record = body as JsonObject;
  const result = record['result'];
  if (record['ok'] === true && typeof result === 'object' && result !== null && !Array.isArray(result)) {
    const envelope = result as JsonObject;
    return 'result' in envelope ? (envelope['result'] as JsonValue) : body;
  }
  return body;
}

// ---------------------------------------------------------------------------
// Port probing.
// ---------------------------------------------------------------------------

/**
 * @brief Probe one free TCP port from the 18000-18999 range.
 *
 * @return A port that accepted and released a probe listener.
 * @throws Error when no candidate port was free after several attempts.
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
// Step runner: ✓/✗ output with fail-fast semantics.
// ---------------------------------------------------------------------------

/** @brief Collected failure of the first failed step, printed at the end. */
let failure: string | null = null;

/**
 * @brief Run one labeled demo step.
 *
 * @param label Chinese step headline printed with the ✓/✗ marker.
 * @param fn Step body; a rejection fails the whole demo.
 * @return The step's return value.
 */
async function step<T>(label: string, fn: () => Promise<T> | T): Promise<T> {
  if (failure !== null) throw new Error(failure);
  try {
    const value = await fn();
    console.log(`✓ ${label}`);
    return value;
  } catch (error) {
    console.log(`✗ ${label}`);
    failure = `${label} 失败: ${error instanceof Error ? error.message : String(error)}`;
    throw new Error(failure);
  }
}

// ---------------------------------------------------------------------------
// Main walk.
// ---------------------------------------------------------------------------

/**
 * @brief Boot the demo rack, run the eight scenarios, then shut the rack down.
 * @return Resolves when the walk finished; process.exitCode carries the outcome.
 */
async function main(): Promise<void> {
  console.log('=== 42U 机架 TS 版 · 端到端演示 ===');
  const port = await findFreePort();
  process.env['U42_HTTP_PORT'] = String(port);
  console.log(`HTTP 监听端口: ${port}（U42_HTTP_PORT）`);

  const rack: Rack = createRack({
    pluginDirectories: ['examples/plugins'],
    payloadLimit: 8 * MIB,
    outputLimit: 8 * MIB,
  });
  const { plugin: driverPlugin, driver } = createDriverPlugin();
  addStandardPlugins(rack);
  rack.add(() => driverPlugin);

  let listeningPort = port;
  let storageLease: LeaseHandle | null = null;

  try {
    await step('0. 启动机架：examples/plugins + 8 个标准件 + demo.driver', async () => {
      await rack.boot();
      const plugins = rack.plugins();
      const states = new Set(plugins.map((info) => info.state));
      if (states.size !== 1 || !states.has('active')) {
        throw new Error(`启动后存在非 active 实例: ${show([...states])}`);
      }
      console.log(`  已装载 ${plugins.length} 个设备，全部 active`);
    });

    await step('① 能力快照与 watch：Providers 披露与方法计数', () => {
      const snapshot = driver.services.caps.snapshot();
      const available = snapshot.filter((fact) => fact.available);
      const required = ['example.echo', 'example.storage', 'example.consumer', 'std.log', 'std.http-server', 'std.rest-server', 'std.rpc'];
      for (const plugId of required) {
        const fact = available.find((entry) => entry.plugId === plugId);
        if (fact === undefined) throw new Error(`快照缺少可用提供者 ${plugId}`);
        if (fact.capabilities.methods.length === 0) throw new Error(`${plugId} 未披露任何方法`);
      }
      console.log(`  快照可用提供者 ${available.length} 个；watch 累计收到 ${driver.capFacts.length} 条能力事实`);
    });

    await step('② typedApi 直调：echo 一次性租借 + storage 长持租约', async () => {
      const echoPayload: JsonObject = { ping: '你好，42U', n: 42 };
      const echoHandle = driver.acquire('example.echo', exactVersion({ major: 1, minor: 0, patch: 0 }));
      try {
        const echo = typedApi<EchoApi>(driver.services.calls, echoHandle);
        const echoed = await echo.echo(echoPayload);
        if (!jsonEquals(echoed, echoPayload)) {
          throw new Error(`echo 返回不符: ${show(echoed)}`);
        }
      } finally {
        const returned = await echoHandle.release();
        if (returned !== 'ok') throw new Error(`echo 租约归还结果为 ${returned}`);
      }
      const demoValue: JsonObject = { from: 'demo', at: 'boot' };
      storageLease = driver.acquire('example.storage', RANGE_V1);
      if (storageLease.version.major !== 1 || storageLease.version.minor !== 2) {
        const got: JsonObject = {
          major: storageLease.version.major,
          minor: storageLease.version.minor,
          patch: storageLease.version.patch,
        };
        throw new Error(`storage 实际版本不符: ${show(got)}`);
      }
      const storage = typedApi<StorageApi>(driver.services.calls, storageLease);
      await storage.set({ key: 'demo.hello', value: demoValue });
      const readBack = await storage.get({ key: 'demo.hello' });
      if (!jsonEquals(readBack, demoValue)) throw new Error(`storage 读回不符: ${show(readBack)}`);
      console.log(`  echo 回显一致；storage 1.2.0 长持租约写入并读回 ${show(demoValue)}`);
    });

    await step('③ 事件流通：example.data → consumer 消费 → 写穿 storage → 读回核对', async () => {
      const payload: JsonObject = { source: 'demo', seq: 1 };
      driver.services.events.publish('example.data', payload);
      await rack.poll();
      const consumerHandle = driver.acquire('example.consumer', exactVersion({ major: 1, minor: 0, patch: 0 }));
      let status: JsonValue;
      try {
        const consumer = typedApi<ConsumerApi>(driver.services.calls, consumerHandle);
        status = await consumer.status();
      } finally {
        await consumerHandle.release();
      }
      const eventsProcessed =
        typeof status === 'object' && status !== null && !Array.isArray(status)
          ? (status as JsonObject)['eventsProcessed']
          : undefined;
      if (eventsProcessed !== 1) throw new Error(`consumer.status = ${show(status)}`);
      const stored = await typedApi<StorageApi>(driver.services.calls, storageLease!).get({ key: 'example.data' });
      if (!jsonEquals(stored, payload)) throw new Error(`storage 中 example.data = ${show(stored)}`);
      console.log(`  consumer 处理 1 条事件，storage 读回载荷一致: ${show(stored)}`);
    });

    await step('④ REST：经 std.rest-server 挂载 /api/rack/:id → 真实 HTTP 命中', async () => {
      const statsHandle = driver.acquire('std.http-server', RANGE_V1);
      let statsResult: JsonValue;
      try {
        statsResult = await typedApi<HttpServerStatsApi>(driver.services.calls, statsHandle).stats();
      } finally {
        await statsHandle.release();
      }
      const statsPort =
        typeof statsResult === 'object' && statsResult !== null && !Array.isArray(statsResult)
          ? (statsResult as JsonObject)['port']
          : undefined;
      if (typeof statsPort !== 'number' || statsPort <= 0) throw new Error(`stats = ${show(statsResult)}`);
      listeningPort = statsPort;

      const restHandle = driver.acquire('std.rest-server', RANGE_V1);
      try {
        const rest = typedApi<RestServerApi>(driver.services.calls, restHandle);
        await rest['resource.mount']({
          pathPrefix: DEMO_REST_PREFIX,
          targetPlugId: DRIVER_PLUG_ID,
          methods: { get: 'info' },
        });
      } finally {
        await restHandle.release();
      }

      const response = await fetch(`http://127.0.0.1:${listeningPort}${DEMO_REST_PREFIX}/42u`);
      const body = (await response.json()) as JsonValue;
      if (response.status !== 200) throw new Error(`HTTP ${response.status}`);
      if (typeof body !== 'object' || body === null || Array.isArray(body)) throw new Error(`响应体 ${show(body)}`);
      const record = body as JsonObject;
      if (record['ok'] !== true) throw new Error(`响应信封 ${show(body)}`);
      const result = record['result'];
      if (
        typeof result !== 'object' ||
        result === null ||
        (result as JsonObject)['hello'] !== '42u' ||
        (result as JsonObject)['servedBy'] !== DRIVER_PLUG_ID
      ) {
        throw new Error(`响应结果 ${show(result ?? null)}`);
      }
      console.log(`  GET ${DEMO_REST_PREFIX}/42u -> ${show(result)}`);
    });

    await step('⑤ RPC：POST /rpc 桥回环 + std.rpc 客户端回环调用 example.storage.get', async () => {
      const rpcPayload: JsonObject = { roundtrip: true, via: 'std.rpc' };
      const storage = typedApi<StorageApi>(driver.services.calls, storageLease!);
      await storage.set({ key: 'demo.rpc', value: rpcPayload });

      // Path A: raw HTTP POST against std.rpc's own bridge route; the bridge one-shot leases
      // example.storage and answers with a bare JSON-RPC envelope under the {ok} transport.
      const posted = await fetch(`http://127.0.0.1:${listeningPort}/rpc`, {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({
          jsonrpc: '2.0',
          id: 41,
          method: 'example.storage.get',
          params: { key: 'demo.rpc' },
        }),
      });
      const postedBody = (await posted.json()) as JsonValue;
      const postedResult = unwrapRpcEnvelope(postedBody);
      if (posted.status !== 200 || !jsonEquals(postedResult, rpcPayload)) {
        throw new Error(`POST /rpc -> ${posted.status} ${show(postedBody)}`);
      }

      // Path B: the std.rpc client over real HTTP. Coordinator ruling: the client cannot
      // address its own rack's /rpc (the bridge would re-enter std.rpc while its client
      // method is in flight, rejected with 'busy'), so the demo points the client at its own
      // mini JSON-RPC endpoint mounted through std.http-server route.add.
      const routeHandle = driver.acquire('std.http-server', RANGE_V1);
      try {
        await typedApi<HttpServerRouteApi>(driver.services.calls, routeHandle)['route.add']({
          path: DEMO_RPC_PATH,
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
        const rpc = typedApi<RpcApi>(driver.services.calls, rpcHandle);
        clientResult = await rpc.call({
          url: `http://127.0.0.1:${listeningPort}${DEMO_RPC_PATH}`,
          method: 'example.storage.get',
          params: { key: 'demo.rpc' },
          timeoutMs: 5000,
        });
      } finally {
        await rpcHandle.release();
      }
      if (!jsonEquals(clientResult, rpcPayload)) throw new Error(`客户端回环结果 ${show(clientResult)}`);
      console.log(`  桥回环与客户端回环均命中: ${show(rpcPayload)}`);
    });

    await step('⑥ 日志：向 std.log 记两条并 query 核对', async () => {
      const logHandle = driver.acquire('std.log', RANGE_V1);
      let queryResult: JsonValue;
      try {
        const logger = typedApi<LogApi>(driver.services.calls, logHandle);
        await logger.log({ level: 'info', message: '演示日志 1：机架装配完成', plugId: DRIVER_PLUG_ID });
        await logger.log({ level: 'warn', message: '演示日志 2：配额接近上限（演示）', plugId: DRIVER_PLUG_ID });
        queryResult = await logger.query({ limit: 10 });
      } finally {
        await logHandle.release();
      }
      const entries =
        typeof queryResult === 'object' && queryResult !== null && !Array.isArray(queryResult)
          ? (queryResult as JsonObject)['entries']
          : undefined;
      if (!Array.isArray(entries) || entries.length < 2) throw new Error(`query 结果 ${show(queryResult)}`);
      const messages = entries
        .filter((entry): entry is JsonObject => typeof entry === 'object' && entry !== null && !Array.isArray(entry))
        .map((entry) => textOf(entry['message'], ''));
      if (!messages.includes('演示日志 1：机架装配完成') || !messages.includes('演示日志 2：配额接近上限（演示）')) {
        throw new Error(`query 条目 ${show(messages)}`);
      }
      console.log(`  query 返回 ${entries.length} 条，两条演示日志均在环内`);
    });

    await step('⑦ 可拔插：reload 同版本必须重借；unload 后旧凭据 stale、租借 not_found', async () => {
      const ECHO_RANGE = exactVersion({ major: 1, minor: 0, patch: 0 });

      // reload() is one operation: full unload plus a fresh load of the same source. The
      // held lease is revoked (and returned by the revoker) and the generation bumps.
      const reloadHandle = driver.acquire('example.echo', ECHO_RANGE);
      const before = rack.describe('example.echo');
      await rack.reload('example.echo');
      const after = rack.describe('example.echo');
      if (before === undefined || after === undefined || after.generation <= before.generation) {
        throw new Error('重载后代次未递增');
      }
      let reloadStale = '';
      try {
        await driver.services.calls.call(reloadHandle.credential, 'echo', { ping: 'stale?' });
        reloadStale = 'ok';
      } catch (error) {
        reloadStale = statusOf(error);
      }
      if (reloadStale !== 'stale') throw new Error(`重载后旧凭据调用结果为 ${reloadStale}，应为 stale`);
      const freshHandle = driver.acquire('example.echo', ECHO_RANGE);
      try {
        const echo = typedApi<EchoApi>(driver.services.calls, freshHandle);
        const echoed = await echo.echo({ reloaded: true });
        if (!jsonEquals(echoed, { reloaded: true })) throw new Error(`重借后回显 ${show(echoed)}`);
      } finally {
        await freshHandle.release();
      }

      // unload() removes the instance entirely: old credentials go stale, new borrows are
      // not_found until the same source is hot-loaded again.
      const unloadHandle = driver.acquire('example.echo', ECHO_RANGE);
      await rack.unload('example.echo');
      let unloadStale = '';
      try {
        await driver.services.calls.call(unloadHandle.credential, 'echo', { ping: 'gone' });
        unloadStale = 'ok';
      } catch (error) {
        unloadStale = statusOf(error);
      }
      if (unloadStale !== 'stale') throw new Error(`卸载后旧凭据调用结果为 ${unloadStale}，应为 stale`);
      let acquireStatus = '';
      try {
        driver.acquire('example.echo', ECHO_RANGE);
        acquireStatus = 'ok';
      } catch (error) {
        acquireStatus = statusOf(error);
      }
      if (acquireStatus !== 'not_found') throw new Error(`卸载后租借结果为 ${acquireStatus}，应为 not_found`);
      await rack.load('examples/plugins/echo.plug.ts');
      const hotHandle = driver.acquire('example.echo', ECHO_RANGE);
      try {
        const echo = typedApi<EchoApi>(driver.services.calls, hotHandle);
        const echoed = await echo.echo({ hot: 'load' });
        if (!jsonEquals(echoed, { hot: 'load' })) throw new Error(`热装后回显 ${show(echoed)}`);
      } finally {
        await hotHandle.release();
      }
      console.log(
        `  reload 代次 ${before.generation} -> ${after.generation}，同版本重借成功；unload 后旧凭据 stale、租借 not_found，load 热装回后可用`,
      );
    });

    await step('⑧ 优雅收尾：归还长持租约并 shutdown', async () => {
      const returned = await storageLease!.release();
      if (returned !== 'ok') throw new Error(`长持租约归还结果为 ${returned}`);
      storageLease = null;
      await rack.shutdown();
      const remaining = rack.plugins();
      if (remaining.length !== 0) {
        throw new Error(`shutdown 后残留实例: ${remaining.map((info) => info.plugId).join(', ')}`);
      }
      console.log('  长持租约已归还；机架已按逆初始化顺序全部卸载，plugins() 为空');
    });

    console.log('=== 演示完成：8/8 步全部通过 ✓ ===');
  } catch (error) {
    console.error(`\n演示失败：${error instanceof Error ? error.message : String(error)}`);
    process.exitCode = 1;
    const held = storageLease as LeaseHandle | null;
    if (held !== null) {
      try {
        await held.release();
      } catch {
        // Best effort: shutdown revokes outstanding leases anyway.
      }
    }
    try {
      await rack.shutdown();
    } catch {
      // Best effort teardown; the failure reason above is what matters.
    }
  }
}

await main();
