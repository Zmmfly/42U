/**
 * @file rpc.test.ts
 * @brief Tests for std.rpc: the server bridge runs end to end against a real node:http
 *        endpoint on an ephemeral port, the client runs against an injected fetchImpl.
 *
 * @details No host implementation is imported. A self-written fake rack provides
 *          caps.acquire/calls.call dispatching to fake target plugins, and a real
 *          node:http server stands in for std.http-server: it consults the routes recorded
 *          through `route.add`, one-shot leases the route target, forwards the request body
 *          through the fake gateway, and serializes the returned envelope - the
 *          "route -> one-shot lease -> gateway call" behaviour of the real server.
 */
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import {
  createServer,
  type IncomingMessage,
  type Server,
  type ServerResponse,
} from 'node:http';
import { create, createWithDeps } from '../../src/plugins/rpc/index.ts';
import type { RpcFetchImpl, RpcFetchInit } from '../../src/plugins/rpc/index.ts';
import { create as createRpcDemo } from '../../examples/plugins/rpc-demo.plug.ts';
import { isRackError, RackError, statusOf } from '../../src/core/status.ts';
import type { RackStatus } from '../../src/core/status.ts';
import { acceptsVersion } from '../../src/core/version.ts';
import type {
  CapEvent,
  CapSink,
  CapsDesc,
  EventSink,
  JsonValue,
  LeaseCredential,
  LeaseHandle,
  PluginServices,
  PluginVersion,
  RackPlugin,
  Revoker,
  Subscription,
  VersionRange,
} from '../../src/core/types.ts';

// ---------------------------------------------------------------------------
// Fake rack: registry-backed acquire/call records plus a capability watcher pipeline.
// ---------------------------------------------------------------------------

/** @brief One fake provider target registered in the fake rack. */
interface FakeTarget {
  readonly plugId: string;
  readonly version: PluginVersion;
  invoke(methodName: string, args: JsonValue): JsonValue | Promise<JsonValue>;
}

/** @brief One recorded CapabilityService.acquire attempt. */
interface RecordedAcquire {
  readonly plugId: string;
  readonly range: VersionRange;
  readonly credential: LeaseCredential;
  readonly revoker: Revoker;
}

/** @brief One recorded gateway call. */
interface RecordedCall {
  readonly plugId: string;
  readonly methodName: string;
  readonly args: JsonValue;
}

/** @brief One recorded lease release. */
interface RecordedRelease {
  readonly plugId: string;
  readonly credential: LeaseCredential;
}

/** @brief One route registered through route.add on the fake http-server. */
interface RouteEntry {
  readonly routeId: string;
  readonly path: string;
  readonly httpMethod: string;
  readonly targetPlugId: string;
  readonly targetMethod: string;
  readonly versionRange?: VersionRange;
}

/** @brief One recorded capability publication. */
interface RecordedPublish {
  readonly plugId: string;
  readonly caps: CapsDesc;
}

/**
 * @brief Build a fake rack: shared records plus per-consumer PluginServices views.
 *
 * @return Records, helpers and service factories used by every test below.
 */
function createFakeRack() {
  const targets = new Map<string, FakeTarget>();
  const acquires: RecordedAcquire[] = [];
  const releases: RecordedRelease[] = [];
  const calls: RecordedCall[] = [];
  const publishes: RecordedPublish[] = [];
  const routes: RouteEntry[] = [];
  const routeRemovals: Array<{ routeId: string }> = [];
  const watchers: Array<{ owner: string; sink: CapSink }> = [];
  let credentialSeq = 0;
  let routeSeq = 0;

  /** @brief Version interval accepting every valid plugin version. */
  const anyRange: VersionRange = {
    minimum: { major: 0, minor: 0, patch: 0 },
    maximum: {
      major: Number.MAX_SAFE_INTEGER,
      minor: Number.MAX_SAFE_INTEGER,
      patch: Number.MAX_SAFE_INTEGER,
    },
  };

  /**
   * @brief Mint a lease credential for a registered in-range provider.
   *
   * @throws RackError 'not_found' for unknown plug ids, 'unsupported' outside the range.
   */
  function acquire(plugId: string, range: VersionRange, revoker: Revoker): LeaseHandle {
    const target = targets.get(plugId);
    if (target === undefined) throw new RackError('not_found', `no provider for ${plugId}`);
    if (!acceptsVersion(range, target.version)) {
      throw new RackError('unsupported', `${plugId} version outside the requested range`);
    }
    credentialSeq += 1;
    const credential: LeaseCredential = {
      kind: 'u42.lease.credential',
      handle: { seq: credentialSeq, plugId },
    };
    acquires.push({ plugId, range, credential, revoker });
    return {
      credential,
      plugId,
      version: target.version,
      release: async () => {
        releases.push({ plugId, credential });
        return 'ok' as const;
      },
    };
  }

  /**
   * @brief Dispatch one gateway call: intercept the fake server's route methods, forward
   *        everything else to the registered target's invoke.
   */
  async function call(credential: LeaseCredential, methodName: string, args: JsonValue): Promise<JsonValue> {
    const handle = credential.handle as { plugId: string };
    const plugId = handle.plugId;
    calls.push({ plugId, methodName, args });
    if (plugId === 'std.http-server' && methodName === 'route.add') {
      const record = args as unknown as {
        path: string;
        httpMethod: string;
        targetPlugId: string;
        targetMethod: string;
        versionRange?: VersionRange;
      };
      const duplicate = routes.some(
        (route) =>
          route.path === record.path &&
          route.httpMethod === record.httpMethod &&
          route.targetPlugId === record.targetPlugId &&
          route.targetMethod === record.targetMethod,
      );
      if (duplicate) {
        throw new RackError(
          'duplicate',
          `route ${record.httpMethod} ${record.path} -> ${record.targetPlugId}.${record.targetMethod} already registered`,
        );
      }
      routeSeq += 1;
      const routeId = `route-${routeSeq}`;
      routes.push({
        routeId,
        path: record.path,
        httpMethod: record.httpMethod,
        targetPlugId: record.targetPlugId,
        targetMethod: record.targetMethod,
        versionRange: record.versionRange,
      });
      return { routeId };
    }
    if (plugId === 'std.http-server' && methodName === 'route.remove') {
      const record = args as unknown as { routeId: string | number };
      const index = routes.findIndex((route) => route.routeId === record.routeId);
      if (index < 0) throw new RackError('not_found', `no route ${String(record.routeId)}`);
      const removed = routes[index];
      routeRemovals.push({ routeId: removed?.routeId ?? String(record.routeId) });
      routes.splice(index, 1);
      return null;
    }
    const target = targets.get(plugId);
    if (target === undefined) throw new RackError('not_found', `no provider for ${plugId}`);
    return target.invoke(methodName, args);
  }

  /**
   * @brief Build a PluginServices view whose consumer identity is plugId.
   *
   * @param plugId Consumer identity stamped on the returned services.
   * @return A self-written fake services bundle backed by the shared records.
   */
  function servicesFor(plugId: string): PluginServices {
    return {
      plugId,
      events: {
        subscribe(_name: string, _sink: EventSink): Subscription {
          return { unsubscribe() {} };
        },
        publish() {
          // The plugins under test publish no ordinary events; nothing to deliver.
        },
      },
      caps: {
        publish(caps: CapsDesc): void {
          publishes.push({ plugId, caps });
        },
        watch(sink: CapSink): Subscription {
          const entry = { owner: plugId, sink };
          watchers.push(entry);
          let live = true;
          return {
            unsubscribe() {
              if (!live) return;
              live = false;
              const index = watchers.indexOf(entry);
              if (index >= 0) watchers.splice(index, 1);
            },
          };
        },
        snapshot(): readonly CapEvent[] {
          return [];
        },
        acquire,
        release(credential: LeaseCredential): 'ok' | 'busy' | 'stale' {
          const handle = credential.handle as { plugId: string };
          releases.push({ plugId: handle.plugId, credential });
          return 'ok';
        },
      },
      calls: {
        call,
        callById(credential: LeaseCredential, methodId: number, args: JsonValue): Promise<JsonValue> {
          return call(credential, `#${methodId}`, args);
        },
      },
      diag: {
        report(level: string, message: string): void {
          diagnostics.push({ level, message });
        },
      },
    };
  }

  const diagnostics: Array<{ level: string; message: string }> = [];

  /**
   * @brief Deliver one capability fact to every live watcher, in order.
   *
   * @param event Availability or withdrawal fact.
   */
  async function deliverCapability(event: CapEvent): Promise<void> {
    for (const watcher of [...watchers]) await watcher.sink.onCapability(event);
  }

  return {
    targets,
    acquires,
    releases,
    calls,
    publishes,
    routes,
    routeRemovals,
    diagnostics,
    anyRange,
    acquire,
    call,
    register(target: FakeTarget): void {
      targets.set(target.plugId, target);
    },
    servicesFor,
    deliverCapability,
    watcherCount(): number {
      return watchers.length;
    },
    acquireCount(plugId: string): number {
      return acquires.filter((entry) => entry.plugId === plugId).length;
    },
    releaseCount(plugId: string): number {
      return releases.filter((entry) => entry.plugId === plugId).length;
    },
  };
}

// ---------------------------------------------------------------------------
// Real node:http stand-in for std.http-server.
// ---------------------------------------------------------------------------

/** @brief Handle to the running test HTTP endpoint. */
interface HttpBridge {
  readonly baseUrl: string;
  close(): Promise<void>;
}

/**
 * @brief Read the full request body as UTF-8 text.
 *
 * @param req Incoming HTTP request.
 * @return The raw body text.
 */
function readRequestBody(req: IncomingMessage): Promise<string> {
  return new Promise((resolve, reject) => {
    const chunks: Buffer[] = [];
    req.on('data', (chunk: Buffer) => {
      chunks.push(chunk);
    });
    req.on('end', () => {
      resolve(Buffer.concat(chunks).toString('utf8'));
    });
    req.on('error', (error: Error) => {
      reject(error);
    });
  });
}

/** @brief Shared no-op revoker for the one-shot leases the fake server mints. */
const NOOP_REVOKER: Revoker = { onRevoke() {} };

/**
 * @brief Parse raw request text the way std.http-server does: null when unparseable.
 *
 * @param text Raw HTTP request body.
 * @return The parsed JSON value or null.
 */
function parseBodyLikeHttpServer(text: string): JsonValue {
  try {
    const parsed: unknown = JSON.parse(text);
    return typeof parsed === 'number' && !Number.isFinite(parsed) ? null : (parsed as JsonValue);
  } catch {
    return null;
  }
}

/**
 * @brief Start a real HTTP server on an ephemeral port mimicking std.http-server.
 *
 * @param fake Fake rack whose records and dispatch back the mounted routes.
 * @return The base URL plus a close() that severs all connections.
 * @note Dispatch mirrors the real server: match httpMethod+path against route.add
 *       registrations (404/405 otherwise), one-shot lease the route target with the
 *       route's versionRange, forward `{query, params, headers, body}` (body = parsed
 *       JSON or null), release, and answer `200 {ok:true, result}` — or
 *       `200 {ok:false, error:{status, message}}` when the target throws.
 */
async function startHttpBridge(fake: ReturnType<typeof createFakeRack>): Promise<HttpBridge> {
  const server: Server = createServer((req: IncomingMessage, res: ServerResponse) => {
    void dispatch(req, res);
  });

  async function dispatch(req: IncomingMessage, res: ServerResponse): Promise<void> {
    try {
      const url = new URL(req.url ?? '/', 'http://127.0.0.1');
      const route = fake.routes.find((entry) => entry.path === url.pathname);
      if (route === undefined) {
        res.statusCode = 404;
        res.setHeader('content-type', 'application/json');
        res.end(JSON.stringify({ ok: false, error: { status: 'not_found', message: 'no route' } }));
        return;
      }
      if (req.method !== route.httpMethod) {
        res.statusCode = 405;
        res.setHeader('allow', route.httpMethod);
        res.setHeader('content-type', 'application/json');
        res.end(JSON.stringify({ ok: false, error: { status: 'unsupported', message: 'method not allowed' } }));
        return;
      }
      const body = parseBodyLikeHttpServer(await readRequestBody(req));
      const query: Record<string, string> = {};
      for (const [key, value] of url.searchParams.entries()) query[key] = value;
      const headers: Record<string, string | string[]> = {};
      for (const [name, value] of Object.entries(req.headers)) {
        headers[name] = value as string | string[];
      }
      const lease = fake.acquire(route.targetPlugId, route.versionRange ?? fake.anyRange, NOOP_REVOKER);
      try {
        const result = await fake.call(lease.credential, route.targetMethod, {
          query,
          params: {},
          headers,
          body,
        });
        res.statusCode = 200;
        res.setHeader('content-type', 'application/json');
        res.end(JSON.stringify({ ok: true, result }));
      } finally {
        await lease.release();
      }
    } catch (error) {
      res.statusCode = 200;
      res.setHeader('content-type', 'application/json');
      res.end(
        JSON.stringify({
          ok: false,
          error: {
            status: statusOf(error),
            message: error instanceof Error ? error.message : String(error),
          },
        }),
      );
    }
  }

  await new Promise<void>((resolve) => {
    server.listen(0, '127.0.0.1', resolve);
  });
  const address = server.address();
  if (address === null || typeof address === 'string') {
    throw new Error('test server has no port address');
  }
  return {
    baseUrl: `http://127.0.0.1:${address.port}`,
    close(): Promise<void> {
      server.closeAllConnections();
      return new Promise((resolve) => {
        server.close(() => {
          resolve();
        });
      });
    },
  };
}

// ---------------------------------------------------------------------------
// Shared fixtures and helpers.
// ---------------------------------------------------------------------------

/** @brief Version of the fake std.http-server. */
const HTTP_SERVER_VERSION: PluginVersion = { major: 1, minor: 0, patch: 0 };

/** @brief Version of the fake target provider. */
const FAKE_TARGET_VERSION: PluginVersion = { major: 2, minor: 1, patch: 3 };

/** @brief The real global fetch, wrapped to satisfy the injectable signature. */
const realFetch: RpcFetchImpl = (url, init) => fetch(url, init);

describe('std.rpc', () => {
  let fake: ReturnType<typeof createFakeRack>;
  let httpBridge: HttpBridge;

  beforeEach(async () => {
    fake = createFakeRack();
    httpBridge = await startHttpBridge(fake);
  });

  afterEach(async () => {
    if (httpBridge !== undefined) await httpBridge.close();
  });

  /** @brief std.http-server availability fact for the given version. */
  function httpServerAvailable(version: PluginVersion = HTTP_SERVER_VERSION): CapEvent {
    return {
      plugId: 'std.http-server',
      available: true,
      version,
      capabilities: { methods: [{ id: 1, name: 'route.add' }, { id: 2, name: 'route.remove' }] },
    };
  }

  /** @brief std.http-server withdrawal fact. */
  function httpServerWithdrawn(): CapEvent {
    return {
      plugId: 'std.http-server',
      available: false,
      version: HTTP_SERVER_VERSION,
      capabilities: { methods: [] },
    };
  }

  /** @brief Register the fake target provider with echo/boom/bad behaviours. */
  function registerFakeTarget(): void {
    fake.register({
      plugId: 'fake.target',
      version: FAKE_TARGET_VERSION,
      invoke(methodName: string, args: JsonValue): JsonValue {
        if (methodName === 'echo') return { echoed: args };
        if (methodName === 'boom') throw new RackError('failed', 'fake.target boom fails by design');
        if (methodName === 'bad') throw new RackError('invalid_argument', 'fake.target rejects these args');
        throw new RackError('not_found', `fake.target has no method ${methodName}`);
      },
    });
  }

  /**
   * @brief Create, init and start the plugin under test with the http-server fact delivered.
   *
   * @param fetchImpl Outgoing fetch implementation; defaults to the global fetch.
   * @return The started plugin instance.
   */
  async function startRpcPlugin(fetchImpl: RpcFetchImpl = realFetch): Promise<RackPlugin> {
    const plugin = createWithDeps(fetchImpl);
    fake.register({
      plugId: 'std.rpc',
      version: { major: 1, minor: 0, patch: 0 },
      invoke(methodName: string, args: JsonValue): Promise<JsonValue> | JsonValue {
        const result = plugin.invoke?.({ methodId: -1, methodName, args });
        if (result === undefined) throw new RackError('not_found', `std.rpc has no method ${methodName}`);
        return result;
      },
    });
    fake.register({
      plugId: 'std.http-server',
      version: HTTP_SERVER_VERSION,
      invoke: () => null,
    });
    await plugin.init(fake.servicesFor('std.rpc'));
    await fake.deliverCapability(httpServerAvailable());
    await plugin.start();
    // The initial POST /rpc mount is deferred to the next macrotask: business calls
    // require an Active consumer, which the instance only becomes after start() returns.
    await new Promise<void>((resolve) => setImmediate(resolve));
    return plugin;
  }

  /**
   * @brief POST one raw body to the local /rpc endpoint and unwrap the rack transport.
   *
   * @param body Raw request text.
   * @return HTTP status plus the JSON-RPC envelope beneath `{ok:true, result}`.
   */
  async function postRpc(body: string): Promise<{ status: number; json: JsonValue }> {
    const response = await fetch(`${httpBridge.baseUrl}/rpc`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body,
    });
    const text = await response.text();
    const parsed = JSON.parse(text) as { ok?: boolean; result?: JsonValue };
    const json = parsed.ok === true ? (parsed.result ?? null) : (parsed as unknown as JsonValue);
    return { status: response.status, json };
  }

  /**
   * @brief Invoke one method on a plugin through the gateway request shape.
   *
   * @param plugin Plugin under test.
   * @param methodName Name selector.
   * @param args Request args document.
   * @return The invoke result.
   */
  async function invokeRpc(plugin: RackPlugin, methodName: string, args: JsonValue): Promise<JsonValue> {
    const result = plugin.invoke?.({ methodId: -1, methodName, args });
    if (result === undefined) throw new Error(`plugin has no invoke for ${methodName}`);
    return result;
  }

  /**
   * @brief Assert that a promise rejects with a RackError of the expected status.
   *
   * @param run Callback performing the failing operation.
   * @param status Expected RackStatus.
   * @param pattern Optional message pattern.
   * @return The caught RackError for further assertions.
   */
  async function expectRackError(
    run: () => Promise<unknown>,
    status: RackStatus,
    pattern?: RegExp,
  ): Promise<RackError> {
    try {
      await run();
      expect.unreachable();
    } catch (error) {
      expect(isRackError(error)).toBe(true);
      const rackError = error as RackError;
      expect(rackError.status).toBe(status);
      expect(statusOf(error)).toBe(status);
      if (pattern !== undefined) expect(rackError.message).toMatch(pattern);
      return rackError;
    }
    throw new Error('unreachable');
  }

  /** @brief Read the error code of a JSON-RPC envelope, or undefined. */
  function errorCodeOf(payload: JsonValue | undefined): number | undefined {
    if (typeof payload !== 'object' || payload === null || Array.isArray(payload)) return undefined;
    const error = (payload as { error?: { code?: number } }).error;
    return typeof error?.code === 'number' ? error.code : undefined;
  }

  // -------------------------------------------------------------------------
  // Server bridge over a real node:http endpoint.
  // -------------------------------------------------------------------------

  describe('server bridge over real node:http', () => {
    it('leases std.http-server in [1.0.0,1.MAX] and registers one POST /rpc route on start', async () => {
      await startRpcPlugin();
      expect(fake.routes).toHaveLength(1);
      expect(fake.routes[0]).toMatchObject({
        path: '/rpc',
        httpMethod: 'POST',
        targetPlugId: 'std.rpc',
        targetMethod: 'bridge',
      });
      // The route pins the bridge's own major for the server's one-shot dispatch leases.
      expect(fake.routes[0]?.versionRange).toEqual({
        minimum: { major: 1, minor: 0, patch: 0 },
        maximum: { major: 1, minor: Number.MAX_SAFE_INTEGER, patch: Number.MAX_SAFE_INTEGER },
      });
      expect(fake.acquireCount('std.http-server')).toBe(1);
      const range = fake.acquires.find((entry) => entry.plugId === 'std.http-server')?.range;
      expect(range?.minimum).toEqual({ major: 1, minor: 0, patch: 0 });
      expect(range?.maximum).toEqual({
        major: 1,
        minor: Number.MAX_SAFE_INTEGER,
        patch: Number.MAX_SAFE_INTEGER,
      });
      // The capability set is published exactly once on start.
      const published = fake.publishes.filter((entry) => entry.plugId === 'std.rpc');
      expect(published).toHaveLength(1);
      expect(published[0]?.caps.methods.map((method) => [method.id, method.name])).toEqual([
        [1, 'bridge'],
        [2, 'call'],
        [3, 'batch'],
      ]);
    });

    it('answers a successful call with a result envelope and exactly one one-shot target lease', async () => {
      registerFakeTarget();
      await startRpcPlugin();
      const response = await postRpc(
        JSON.stringify({ jsonrpc: '2.0', id: 7, method: 'fake.target.echo', params: { a: 1 } }),
      );
      expect(response.status).toBe(200);
      expect(response.json).toEqual({ jsonrpc: '2.0', id: 7, result: { echoed: { a: 1 } } });
      // One-shot lease accounting: one acquire and one release for the single request.
      expect(fake.acquireCount('fake.target')).toBe(1);
      expect(fake.releaseCount('fake.target')).toBe(1);
      const targetCall = fake.calls.find((entry) => entry.plugId === 'fake.target');
      expect(targetCall?.methodName).toBe('echo');
      expect(targetCall?.args).toEqual({ a: 1 });
      // The http-server stand-in itself one-shot leases std.rpc for the dispatch.
      expect(fake.acquireCount('std.rpc')).toBe(1);
      expect(fake.releaseCount('std.rpc')).toBe(1);
    });

    it('maps rack failures onto JSON-RPC error codes with the RackStatus as data', async () => {
      registerFakeTarget();
      await startRpcPlugin();
      const boom = await postRpc(JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'fake.target.boom' }));
      expect(boom.json).toEqual({
        jsonrpc: '2.0',
        id: 1,
        error: { code: -32603, message: expect.any(String), data: 'failed' },
      });
      const bad = await postRpc(JSON.stringify({ jsonrpc: '2.0', id: 2, method: 'fake.target.bad', params: {} }));
      expect(bad.json).toEqual({
        jsonrpc: '2.0',
        id: 2,
        error: { code: -32602, message: expect.any(String), data: 'invalid_argument' },
      });
      const missingMethod = await postRpc(
        JSON.stringify({ jsonrpc: '2.0', id: 3, method: 'fake.target.missing' }),
      );
      expect(missingMethod.json).toEqual({
        jsonrpc: '2.0',
        id: 3,
        error: { code: -32601, message: expect.any(String), data: 'not_found' },
      });
      const unknownPlug = await postRpc(JSON.stringify({ jsonrpc: '2.0', id: 4, method: 'no.such', params: {} }));
      expect(unknownPlug.json).toEqual({
        jsonrpc: '2.0',
        id: 4,
        error: { code: -32601, message: expect.any(String), data: 'not_found' },
      });
    });

    it('answers -32700 for unparseable bodies and -32600 for invalid requests', async () => {
      registerFakeTarget();
      await startRpcPlugin();
      const parseError = await postRpc('{oops');
      expect(parseError.json).toEqual({
        jsonrpc: '2.0',
        id: null,
        error: { code: -32700, message: expect.any(String) },
      });
      const wrongVersion = await postRpc(JSON.stringify({ jsonrpc: '1.0', id: 1, method: 'fake.target.echo' }));
      expect(errorCodeOf(wrongVersion.json)).toBe(-32600);
      const noDot = await postRpc(JSON.stringify({ jsonrpc: '2.0', id: 2, method: 'nodot' }));
      expect(errorCodeOf(noDot.json)).toBe(-32600);
      const scalarParams = await postRpc(
        JSON.stringify({ jsonrpc: '2.0', id: 3, method: 'fake.target.echo', params: 5 }),
      );
      expect(errorCodeOf(scalarParams.json)).toBe(-32600);
      const missingId = await postRpc(JSON.stringify({ jsonrpc: '2.0', method: 'fake.target.echo' }));
      expect(errorCodeOf(missingId.json)).toBe(-32600);
      // A batch of non-objects answers one -32600 envelope per entry.
      const batchOfGarbage = await postRpc(JSON.stringify([1, 2]));
      expect(Array.isArray(batchOfGarbage.json)).toBe(true);
      const entries = (batchOfGarbage.json as readonly JsonValue[] | null) ?? [];
      expect(entries).toHaveLength(2);
      expect(errorCodeOf(entries[0])).toBe(-32600);
      expect(errorCodeOf(entries[1])).toBe(-32600);
      // An empty batch is answered by a single -32600 envelope per the spec.
      const emptyBatch = await postRpc('[]');
      expect(errorCodeOf(emptyBatch.json)).toBe(-32600);
    });

    it('executes batches serially and answers one aligned envelope per entry', async () => {
      registerFakeTarget();
      await startRpcPlugin();
      const batch = [
        { jsonrpc: '2.0', id: 'a', method: 'fake.target.echo', params: [1, 2] },
        { jsonrpc: '2.0', id: 'b', method: 'fake.target.boom' },
        { jsonrpc: '2.0', id: 'c', method: 'fake.target.missing' },
      ];
      const response = await postRpc(JSON.stringify(batch));
      expect(Array.isArray(response.json)).toBe(true);
      const envelopes = (response.json as readonly JsonValue[] | null) ?? [];
      expect(envelopes).toHaveLength(3);
      expect(envelopes[0]).toEqual({ jsonrpc: '2.0', id: 'a', result: { echoed: [1, 2] } });
      expect(errorCodeOf(envelopes[1])).toBe(-32603);
      expect(errorCodeOf(envelopes[2])).toBe(-32601);
      // Serial execution order is visible through the gateway record.
      expect(
        fake.calls.filter((entry) => entry.plugId === 'fake.target').map((entry) => entry.methodName),
      ).toEqual(['echo', 'boom', 'missing']);
      // Three one-shot leases, each released.
      expect(fake.acquireCount('fake.target')).toBe(3);
      expect(fake.releaseCount('fake.target')).toBe(3);
    });

    it('forwards an explicit versionRange and defaults to the full interval', async () => {
      registerFakeTarget();
      await startRpcPlugin();
      // Default: every valid version accepted; the fake target is 2.1.3.
      await postRpc(JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'fake.target.echo' }));
      const defaultAcquire = fake.acquires.find((entry) => entry.plugId === 'fake.target');
      expect(defaultAcquire?.range.minimum).toEqual({ major: 0, minor: 0, patch: 0 });
      expect(defaultAcquire?.range.maximum.major).toBe(Number.MAX_SAFE_INTEGER);
      // Explicit exact interval is forwarded verbatim and succeeds against 2.1.3.
      const exact = {
        minimum: { major: 2, minor: 1, patch: 3 },
        maximum: { major: 2, minor: 1, patch: 3 },
      };
      const exactResponse = await postRpc(
        JSON.stringify({ jsonrpc: '2.0', id: 4, method: 'fake.target.echo', versionRange: exact }),
      );
      expect(exactResponse.json).toEqual({
        jsonrpc: '2.0',
        id: 4,
        result: { echoed: {} },
      });
      expect(fake.acquires.filter((entry) => entry.plugId === 'fake.target')[1]?.range).toEqual(exact);
      // Explicit out-of-range interval: acquisition rejects 'unsupported' -> -32603.
      const range = { minimum: { major: 3, minor: 0, patch: 0 }, maximum: { major: 3, minor: 0, patch: 0 } };
      const response = await postRpc(
        JSON.stringify({ jsonrpc: '2.0', id: 2, method: 'fake.target.echo', versionRange: range }),
      );
      expect(errorCodeOf(response.json)).toBe(-32603);
      expect((response.json as { error?: { data?: JsonValue } }).error?.data).toBe('unsupported');
      // A malformed extension field is an argument problem: -32602.
      const badRange = await postRpc(
        JSON.stringify({
          jsonrpc: '2.0',
          id: 3,
          method: 'fake.target.echo',
          versionRange: { minimum: { major: 9, minor: 0, patch: 0 } },
        }),
      );
      expect(errorCodeOf(badRange.json)).toBe(-32602);
    });

    it('lets the HTTP server answer 404/405 outside POST /rpc', async () => {
      registerFakeTarget();
      await startRpcPlugin();
      const before = fake.acquires.length;
      const wrongMethod = await fetch(`${httpBridge.baseUrl}/rpc`, { method: 'GET' });
      expect(wrongMethod.status).toBe(405);
      const wrongPath = await fetch(`${httpBridge.baseUrl}/other`, { method: 'POST', body: '{}' });
      expect(wrongPath.status).toBe(404);
      // No dispatch (hence no lease) happened for either request.
      expect(fake.acquires.length).toBe(before);
    });

    it('stop() unmounts the endpoint, returns the lease and unsubscribes', async () => {
      registerFakeTarget();
      const plugin = await startRpcPlugin();
      const serverCredential = fake.acquires.find((entry) => entry.plugId === 'std.http-server')?.credential;
      const mountedRouteId = fake.routes[0]?.routeId;
      await plugin.stop();
      expect(fake.routeRemovals).toEqual([{ routeId: mountedRouteId }]);
      expect(fake.routes).toEqual([]);
      expect(fake.releaseCount('std.http-server')).toBe(1);
      expect(fake.releases.map((entry) => entry.credential)).toEqual([serverCredential]);
      expect(fake.watcherCount()).toBe(0);
      const after = await fetch(`${httpBridge.baseUrl}/rpc`, { method: 'POST', body: '{}' });
      expect(after.status).toBe(404);
    });

    it('mounts later when std.http-server becomes available after start()', async () => {
      const plugin = createWithDeps(realFetch);
      fake.register({
        plugId: 'std.rpc',
        version: { major: 1, minor: 0, patch: 0 },
        invoke: () => null,
      });
      fake.register({ plugId: 'std.http-server', version: HTTP_SERVER_VERSION, invoke: () => null });
      await plugin.init(fake.servicesFor('std.rpc'));
      await plugin.start();
      expect(fake.routes).toEqual([]);
      await fake.deliverCapability(httpServerAvailable());
      expect(fake.routes).toHaveLength(1);
      expect(fake.routes[0]).toMatchObject({
        path: '/rpc',
        httpMethod: 'POST',
        targetPlugId: 'std.rpc',
        targetMethod: 'bridge',
      });
    });

    it('keeps the lease on withdrawal for the revoker and re-acquires on return', async () => {
      await startRpcPlugin();
      const firstAcquire = fake.acquires.find((entry) => entry.plugId === 'std.http-server');
      const firstCredential = firstAcquire?.credential;
      await fake.deliverCapability(httpServerWithdrawn());
      // Withdrawal forbids new business but keeps the credential for onRevoke().
      expect(fake.releaseCount('std.http-server')).toBe(0);
      await firstAcquire?.revoker.onRevoke(firstCredential as LeaseCredential);
      expect(fake.releaseCount('std.http-server')).toBe(1);
      // The revoked generation's route table is gone; simulate the replaced server.
      fake.routes.length = 0;
      // The server coming back re-opens the lease and re-mounts while still active.
      await fake.deliverCapability(httpServerAvailable());
      expect(fake.acquireCount('std.http-server')).toBe(2);
      expect(fake.routes).toHaveLength(1);
      expect(fake.routes[0]).toMatchObject({
        path: '/rpc',
        httpMethod: 'POST',
        targetPlugId: 'std.rpc',
        targetMethod: 'bridge',
      });
    });
  });

  // -------------------------------------------------------------------------
  // Client methods with an injected fetchImpl.
  // -------------------------------------------------------------------------

  describe('client methods with an injected fetchImpl', () => {
    /** @brief One captured outgoing request. */
    interface CapturedRequest {
      readonly url: string;
      readonly init: RpcFetchInit;
    }

    /** @brief Build a JSON HTTP response. */
    function jsonResponse(payload: unknown, status = 200): Response {
      return new Response(JSON.stringify(payload), {
        status,
        headers: { 'content-type': 'application/json' },
      });
    }

    /**
     * @brief Create, init and start a plugin wired to the given fetch implementation.
     *
     * @param fetchImpl Fake or real fetch implementation.
     * @return The started plugin instance.
     */
    async function startClient(fetchImpl: RpcFetchImpl): Promise<RackPlugin> {
      const plugin = createWithDeps(fetchImpl);
      await plugin.init(fake.servicesFor('std.rpc'));
      await plugin.start();
      return plugin;
    }

    it('call posts a JSON-RPC envelope and returns the remote result', async () => {
      const captured: CapturedRequest[] = [];
      const plugin = await startClient(async (url, init) => {
        captured.push({ url, init });
        return jsonResponse({ jsonrpc: '2.0', id: 1, result: 'ok' });
      });
      const result = await invokeRpc(plugin, 'call', {
        url: 'http://peer/rpc',
        method: 'fake.target.echo',
        params: { a: 1 },
      });
      expect(result).toBe('ok');
      expect(captured).toHaveLength(1);
      expect(captured[0]?.url).toBe('http://peer/rpc');
      expect(captured[0]?.init.method).toBe('POST');
      expect(captured[0]?.init.headers['content-type']).toBe('application/json');
      expect(JSON.parse(captured[0]?.init.body ?? '')).toEqual({
        jsonrpc: '2.0',
        id: 1,
        method: 'fake.target.echo',
        params: { a: 1 },
      });
    });

    it('maps remote error codes back onto RackError statuses', async () => {
      const notFound = await startClient(async () =>
        jsonResponse({ jsonrpc: '2.0', id: 1, error: { code: -32601, message: 'nope' } }),
      );
      await expectRackError(
        () => invokeRpc(notFound, 'call', { url: 'http://peer/rpc', method: 'a.b' }),
        'not_found',
      );
      const invalid = await startClient(async () =>
        jsonResponse({ jsonrpc: '2.0', id: 1, error: { code: -32602, message: 'bad args', data: 'x' } }),
      );
      const invalidError = await expectRackError(
        () => invokeRpc(invalid, 'call', { url: 'http://peer/rpc', method: 'a.b' }),
        'invalid_argument',
      );
      expect(invalidError.detail).toEqual({ code: -32602, data: 'x' });
      const other = await startClient(async () =>
        jsonResponse({ jsonrpc: '2.0', id: 1, error: { code: -32700, message: 'parse' } }),
      );
      await expectRackError(
        () => invokeRpc(other, 'call', { url: 'http://peer/rpc', method: 'a.b' }),
        'failed',
      );
    });

    it('maps network failures to failed', async () => {
      const plugin = await startClient(async () => {
        throw new TypeError('fetch failed');
      });
      await expectRackError(
        () => invokeRpc(plugin, 'call', { url: 'http://peer/rpc', method: 'a.b' }),
        'failed',
        /fetch failed/,
      );
    });

    it('aborts hanging requests at timeoutMs even when fetch ignores the signal', async () => {
      const plugin = await startClient(() => new Promise<Response>(() => {}));
      await expectRackError(
        () => invokeRpc(plugin, 'call', { url: 'http://peer/rpc', method: 'a.b', timeoutMs: 25 }),
        'failed',
        /timed out/,
      );
    });

    it('rejects malformed responses with failed', async () => {
      const notJson = await startClient(async () => new Response('not json', { status: 200 }));
      await expectRackError(
        () => invokeRpc(notJson, 'call', { url: 'http://peer/rpc', method: 'a.b' }),
        'failed',
        /not valid JSON/,
      );
      const arrayPayload = await startClient(async () => jsonResponse([1, 2]));
      await expectRackError(
        () => invokeRpc(arrayPayload, 'call', { url: 'http://peer/rpc', method: 'a.b' }),
        'failed',
        /malformed/,
      );
      const idMismatch = await startClient(async () =>
        jsonResponse({ jsonrpc: '2.0', id: 99, result: 'x' }),
      );
      await expectRackError(
        () => invokeRpc(idMismatch, 'call', { url: 'http://peer/rpc', method: 'a.b' }),
        'failed',
        /id/,
      );
    });

    it('rejects invalid client arguments without touching the network', async () => {
      let hits = 0;
      const plugin = await startClient(async () => {
        hits += 1;
        return jsonResponse({ jsonrpc: '2.0', id: 1, result: null });
      });
      await expectRackError(() => invokeRpc(plugin, 'call', { method: 'a.b' }), 'invalid_argument');
      await expectRackError(
        () => invokeRpc(plugin, 'call', { url: 'u', method: 'a.b', timeoutMs: 0 }),
        'invalid_argument',
      );
      await expectRackError(() => invokeRpc(plugin, 'batch', { url: 'u', calls: [] }), 'invalid_argument');
      await expectRackError(
        () => invokeRpc(plugin, 'batch', { url: 'u', calls: [{ params: {} }] }),
        'invalid_argument',
      );
      expect(hits).toBe(0);
    });

    it('batch posts one array and returns aligned per-entry results', async () => {
      const captured: CapturedRequest[] = [];
      const plugin = await startClient(async (url, init) => {
        captured.push({ url, init });
        return jsonResponse([
          { jsonrpc: '2.0', id: 1, result: 'first' },
          { jsonrpc: '2.0', id: 2, error: { code: -32601, message: 'missing method' } },
          { jsonrpc: '2.0', id: 3, result: { nested: true } },
        ]);
      });
      const results = await invokeRpc(plugin, 'batch', {
        url: 'http://peer/rpc',
        calls: [{ method: 'a.one' }, { method: 'a.two', params: { n: 1 } }, { method: 'a.three' }],
      });
      expect(results).toEqual([
        { result: 'first' },
        { error: { code: -32601, message: 'missing method', status: 'not_found' } },
        { result: { nested: true } },
      ]);
      const body = JSON.parse(captured[0]?.init.body ?? '') as Array<{ id: number; method: string; params: JsonValue }>;
      expect(body).toHaveLength(3);
      expect(body.map((entry) => entry.id)).toEqual([1, 2, 3]);
      expect(body[1]?.method).toBe('a.two');
      expect(body[1]?.params).toEqual({ n: 1 });
    });

    it('loops back through the real local endpoint with the global fetch', async () => {
      registerFakeTarget();
      const plugin = await startRpcPlugin(realFetch);
      const result = await invokeRpc(plugin, 'call', {
        url: `${httpBridge.baseUrl}/rpc`,
        method: 'fake.target.echo',
        params: { loop: true },
      });
      expect(result).toEqual({ echoed: { loop: true } });
    });
  });

  // -------------------------------------------------------------------------
  // Example rpc-demo plugin.
  // -------------------------------------------------------------------------

  describe('rpc-demo plugin', () => {
    it('answers ping directly and rejects unknown methods', async () => {
      const demo = createRpcDemo();
      expect(demo.desc.plugId).toBe('example.rpc-demo');
      expect(demo.desc.init.after).toEqual(['std.rpc']);
      await demo.init(fake.servicesFor('example.rpc-demo'));
      await demo.start();
      expect(await invokeRpc(demo, 'ping', { n: 1 })).toEqual({ pong: true, received: { n: 1 } });
      await expectRackError(() => invokeRpc(demo, 'nope', null), 'not_found');
    });

    it('round-trips demo through std.rpc against the real local endpoint', async () => {
      registerFakeTarget();
      await startRpcPlugin(realFetch);
      const demo = createRpcDemo();
      fake.register({
        plugId: 'example.rpc-demo',
        version: { major: 1, minor: 0, patch: 0 },
        invoke(methodName: string, args: JsonValue): Promise<JsonValue> | JsonValue {
          const result = demo.invoke?.({ methodId: -1, methodName, args });
          if (result === undefined) throw new RackError('not_found', `demo has no method ${methodName}`);
          return result;
        },
      });
      await demo.init(fake.servicesFor('example.rpc-demo'));
      await fake.deliverCapability({
        plugId: 'std.rpc',
        available: true,
        version: { major: 1, minor: 0, patch: 0 },
        capabilities: {
          methods: [
            { id: 1, name: 'bridge' },
            { id: 2, name: 'call' },
            { id: 3, name: 'batch' },
          ],
        },
      });
      await demo.start();
      const result = await invokeRpc(demo, 'demo', { url: `${httpBridge.baseUrl}/rpc` });
      expect(result).toEqual({
        url: `${httpBridge.baseUrl}/rpc`,
        method: 'example.rpc-demo.ping',
        result: { pong: true, received: {} },
      });
    });

    it('rejects demo invocations before a std.rpc lease exists', async () => {
      const demo = createRpcDemo();
      await demo.init(fake.servicesFor('example.rpc-demo'));
      await demo.start();
      await expectRackError(
        () => invokeRpc(demo, 'demo', { url: 'http://peer/rpc' }),
        'invalid_state',
      );
    });
  });
});
