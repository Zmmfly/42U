/**
 * @file Tests for the std.http-server plugin: route CRUD plus the HTTP -> gateway bridge.
 *
 * @brief Runs a real node:http listener on an ephemeral port (listen port 0) driven by a
 *        self-written node:http client. PluginServices are faked with plain records so every
 *        acquire/call/release can be asserted; no host implementation is imported.
 */
import * as http from 'node:http';
import * as net from 'node:net';
import { describe, expect, it } from 'vitest';
import { create, createWithDeps } from '../../src/plugins/http-server/index.ts';
import type { HttpListenAddress } from '../../src/plugins/http-server/index.ts';
import { create as createDemoPlugin } from '../../examples/plugins/http-server-demo.plug.ts';
import { isRackError, RackError, statusOf } from '../../src/sdk/index.ts';
import type {
  CapsDesc,
  CapEvent,
  JsonValue,
  LeaseCredential,
  PluginServices,
  RackPlugin,
  RackStatus,
  Revoker,
  VersionRange,
} from '../../src/sdk/index.ts';

// ---------------------------------------------------------------------------
// Self-written fake PluginServices: records publications, lease acquisitions,
// handle releases, gateway calls and diagnostics.
// ---------------------------------------------------------------------------

/** @brief One recorded acquire attempt with its minted credential. */
interface RecordedAcquisition {
  plugId: string;
  range: VersionRange;
  credential: LeaseCredential;
}

/** @brief One recorded gateway call. */
interface RecordedCall {
  credential: LeaseCredential;
  methodName: string;
  args: JsonValue;
}

/** @brief Hooks steering the fake services. */
interface FakeConfig {
  /** @brief Throw here to simulate acquire failures (unsupported/not_found/...). */
  acquire?: () => void;
  /** @brief Produce the gateway call result; throw to simulate target business errors. */
  call?: (entry: RecordedCall) => JsonValue | Promise<JsonValue>;
}

/**
 * @brief Build a fake PluginServices bundle backed by plain records.
 * @param config Optional acquire/call steering hooks.
 * @return The services object plus record arrays for assertions.
 */
function makeFakeServices(config: FakeConfig = {}) {
  const published: CapsDesc[] = [];
  const acquisitions: RecordedAcquisition[] = [];
  const handleReleases: LeaseCredential[] = [];
  const capsReleases: LeaseCredential[] = [];
  const calls: RecordedCall[] = [];
  const diagnostics: Array<{ level: string; message: string }> = [];
  let credentialSeq = 0;

  const services: PluginServices = {
    plugId: 'test.http-server-harness',
    events: {
      subscribe() {
        return { unsubscribe() { /* unused by this plugin */ } };
      },
      publish() {
        // std.http-server publishes no ordinary events.
      },
    },
    caps: {
      publish(caps: CapsDesc): void {
        published.push(caps);
      },
      watch() {
        return { unsubscribe() { /* unused by this plugin */ } };
      },
      snapshot(): readonly CapEvent[] {
        return [];
      },
      acquire(plugId: string, range: VersionRange, _revoker: Revoker) {
        config.acquire?.();
        credentialSeq += 1;
        const credential: LeaseCredential = {
          kind: 'u42.lease.credential',
          handle: { seq: credentialSeq },
        };
        acquisitions.push({ plugId, range, credential });
        return {
          credential,
          plugId,
          version: { major: 1, minor: 0, patch: 0 },
          release(): Promise<'ok'> {
            handleReleases.push(credential);
            return Promise.resolve('ok');
          },
        };
      },
      release(credential: LeaseCredential): 'ok' | 'busy' | 'stale' {
        capsReleases.push(credential);
        return 'ok';
      },
    },
    calls: {
      async call(credential: LeaseCredential, methodName: string, args: JsonValue): Promise<JsonValue> {
        const entry: RecordedCall = { credential, methodName, args };
        calls.push(entry);
        return config.call ? await config.call(entry) : null;
      },
      callById(): Promise<JsonValue> {
        return Promise.reject(new RackError('not_found', 'callById is not used by these tests'));
      },
    },
    diag: {
      report(level: string, message: string): void {
        diagnostics.push({ level, message });
      },
    },
  };

  return { services, published, acquisitions, handleReleases, capsReleases, calls, diagnostics };
}

/** @brief Full default lease range expected when a route adds no versionRange. */
const FULL_RANGE: VersionRange = {
  minimum: { major: 0, minor: 0, patch: 0 },
  maximum: {
    major: Number.MAX_SAFE_INTEGER,
    minor: Number.MAX_SAFE_INTEGER,
    patch: Number.MAX_SAFE_INTEGER,
  },
};

// ---------------------------------------------------------------------------
// Harness: real node:http listener on an ephemeral port.
// ---------------------------------------------------------------------------

/** @brief A started plugin together with its bound port and fake services. */
interface ServerHandle {
  plugin: RackPlugin;
  port: number;
  fake: ReturnType<typeof makeFakeServices>;
}

/**
 * @brief Build, init and start one std.http-server instance on 127.0.0.1:0.
 * @param fake Fake services bundle injected through init().
 * @return The plugin, the bound ephemeral port and the fake for assertions.
 */
async function startPlugin(fake: ReturnType<typeof makeFakeServices>): Promise<ServerHandle> {
  let address: HttpListenAddress = { port: -1, host: '127.0.0.1' };
  const plugin = createWithDeps({
    listen: { port: 0, host: '127.0.0.1' },
    onListening: (bound: HttpListenAddress) => {
      address = bound;
    },
  });
  await plugin.init(fake.services);
  await plugin.start();
  return { plugin, port: address.port, fake };
}

/**
 * @brief Invoke one plugin management method by name.
 * @param plugin Plugin under test.
 * @param methodName Method name selector.
 * @param args Request args document.
 * @return The invoke result.
 */
async function invokeMethod(plugin: RackPlugin, methodName: string, args: JsonValue): Promise<JsonValue> {
  const invoke = plugin.invoke;
  if (invoke === undefined) throw new Error('plugin does not implement invoke');
  return await invoke({ methodId: 0, methodName, args });
}

/**
 * @brief Assert that one invoke dispatch throws a RackError with the expected status.
 * @param plugin Plugin under test.
 * @param methodName Method name selector.
 * @param args Request args document.
 * @param status Expected RackStatus.
 */
async function expectInvokeStatus(
  plugin: RackPlugin,
  methodName: string,
  args: JsonValue,
  status: RackStatus,
): Promise<void> {
  try {
    await invokeMethod(plugin, methodName, args);
    expect.unreachable();
  } catch (error) {
    expect(isRackError(error)).toBe(true);
    expect(statusOf(error)).toBe(status);
  }
}

// ---------------------------------------------------------------------------
// Self-written HTTP client.
// ---------------------------------------------------------------------------

/** @brief One client response: status, headers and decoded body. */
interface HttpResult {
  status: number;
  headers: http.IncomingHttpHeaders;
  body: string;
  json(): JsonValue;
}

/**
 * @brief Issue one HTTP request against 127.0.0.1 with a fresh connection.
 * @param port Destination port.
 * @param method HTTP method.
 * @param path Request target including the query string.
 * @param options Optional headers and string body.
 * @return Resolves with status, headers and body once the response ends.
 */
function httpRequest(
  port: number,
  method: string,
  path: string,
  options?: { headers?: Record<string, string>; body?: string },
): Promise<HttpResult> {
  return new Promise<HttpResult>((resolve, reject) => {
    const request = http.request(
      {
        host: '127.0.0.1',
        port,
        path,
        method,
        agent: false,
        ...(options?.headers !== undefined ? { headers: options.headers } : {}),
      },
      (response) => {
        const chunks: Buffer[] = [];
        response.on('data', (chunk: Buffer) => chunks.push(chunk));
        response.on('end', () => {
          const body = Buffer.concat(chunks).toString('utf8');
          resolve({
            status: response.statusCode ?? 0,
            headers: response.headers,
            body,
            json: () => JSON.parse(body) as JsonValue,
          });
        });
      },
    );
    request.on('error', (error: Error) => reject(error));
    if (options?.body !== undefined) request.write(options.body);
    request.end();
  });
}

/**
 * @brief Attempt a raw TCP connect and report the error code.
 * @param port Destination port.
 * @return The socket error code, e.g. 'ECONNREFUSED'.
 */
function connectRefused(port: number): Promise<string> {
  return new Promise<string>((resolve, reject) => {
    const socket = net.connect({ host: '127.0.0.1', port });
    socket.on('error', (error: NodeJS.ErrnoException) => {
      resolve(error.code ?? 'unknown');
    });
    socket.on('connect', () => {
      socket.destroy();
      reject(new Error('unexpected successful connect'));
    });
  });
}

/**
 * @brief Poll a condition until it holds or a deadline expires.
 * @param check Condition evaluated every 10 ms.
 * @param timeoutMs Deadline in milliseconds.
 */
async function waitUntil(check: () => boolean, timeoutMs = 5000): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  while (!check()) {
    if (Date.now() > deadline) throw new Error('waitUntil timed out');
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
}

/** @brief Read one field of a recorded call args document for targeted assertions. */
function callArgs(entry: RecordedCall | undefined): {
  query: Record<string, string>;
  params: Record<string, string>;
  headers: Record<string, JsonValue>;
  body: JsonValue;
} {
  if (entry === undefined) throw new Error('no recorded call');
  return entry.args as unknown as {
    query: Record<string, string>;
    params: Record<string, string>;
    headers: Record<string, JsonValue>;
    body: JsonValue;
  };
}

// ---------------------------------------------------------------------------
// Route management methods.
// ---------------------------------------------------------------------------

describe('std.http-server route management', () => {
  it('exposes the standard descriptor via create()', () => {
    const plugin = create();
    expect(plugin.desc.plugId).toBe('std.http-server');
    expect(plugin.desc.version).toEqual({ major: 1, minor: 0, patch: 0 });
    expect(typeof plugin.invoke).toBe('function');
  });

  it('adds, lists and removes routes with the default full version range', async () => {
    const ctx = await startPlugin(makeFakeServices());
    try {
      const added = await invokeMethod(ctx.plugin, 'route.add', {
        path: '/api/:id',
        httpMethod: 'GET',
        targetPlugId: 'example.target',
        targetMethod: 'fetch',
      });
      expect(added).toEqual({ routeId: 'route-1' });

      const list = await invokeMethod(ctx.plugin, 'routes.list', null);
      expect(list).toEqual({
        routes: [
          {
            routeId: 'route-1',
            path: '/api/:id',
            httpMethod: 'GET',
            targetPlugId: 'example.target',
            targetMethod: 'fetch',
            versionRange: FULL_RANGE,
          },
        ],
      });

      const removed = await invokeMethod(ctx.plugin, 'route.remove', { routeId: 'route-1' });
      expect(removed).toBeNull();
      expect(await invokeMethod(ctx.plugin, 'routes.list', null)).toEqual({ routes: [] });
    } finally {
      await ctx.plugin.stop();
    }
  });

  it('keeps an explicit version range verbatim', async () => {
    const ctx = await startPlugin(makeFakeServices());
    try {
      // Anonymous literal on purpose: JsonObject accepts it via the implicit index signature.
      const range = {
        minimum: { major: 1, minor: 2, patch: 0 },
        maximum: { major: 1, minor: 5, patch: 0 },
      };
      await invokeMethod(ctx.plugin, 'route.add', {
        path: '/v/x',
        httpMethod: 'POST',
        targetPlugId: 'example.target',
        targetMethod: 'work',
        versionRange: range,
      });
      const list = (await invokeMethod(ctx.plugin, 'routes.list', null)) as unknown as {
        routes: Array<{ versionRange: VersionRange }>;
      };
      expect(list.routes[0]?.versionRange).toEqual(range);
    } finally {
      await ctx.plugin.stop();
    }
  });

  it('rejects duplicate routes with duplicate and unknown removals with not_found', async () => {
    const ctx = await startPlugin(makeFakeServices());
    try {
      const route = {
        path: '/api/:id',
        httpMethod: 'GET',
        targetPlugId: 'example.target',
        targetMethod: 'fetch',
      };
      await invokeMethod(ctx.plugin, 'route.add', route);
      await expectInvokeStatus(ctx.plugin, 'route.add', route, 'duplicate');
      await expectInvokeStatus(ctx.plugin, 'route.remove', { routeId: 'nope' }, 'not_found');

      // Same path with a different method or target is not a duplicate.
      await invokeMethod(ctx.plugin, 'route.add', { ...route, httpMethod: 'POST' });
      await invokeMethod(ctx.plugin, 'route.add', { ...route, targetMethod: 'other' });
      const list = (await invokeMethod(ctx.plugin, 'routes.list', null)) as { routes: unknown[] };
      expect(list.routes.length).toBe(3);
    } finally {
      await ctx.plugin.stop();
    }
  });

  it('rejects malformed paths, methods and ranges with invalid_argument', async () => {
    const ctx = await startPlugin(makeFakeServices());
    try {
      const valid = {
        path: '/ok',
        httpMethod: 'GET',
        targetPlugId: 'example.target',
        targetMethod: 'fetch',
      };
      for (const path of ['', '/api//x', 'api/x', '/', '/api/', '/api/:9a', '/api/:id/:id', '/api/foo bar']) {
        await expectInvokeStatus(ctx.plugin, 'route.add', { ...valid, path }, 'invalid_argument');
      }
      await expectInvokeStatus(ctx.plugin, 'route.add', { ...valid, httpMethod: 'FETCH' }, 'invalid_argument');
      await expectInvokeStatus(
        ctx.plugin,
        'route.add',
        { ...valid, versionRange: { minimum: { major: 2, minor: 0, patch: 0 }, maximum: { major: 1, minor: 0, patch: 0 } } },
        'invalid_argument',
      );
      await expectInvokeStatus(
        ctx.plugin,
        'route.add',
        { ...valid, versionRange: { minimum: { major: 1, minor: 0, patch: 0 } } },
        'invalid_argument',
      );
      await expectInvokeStatus(ctx.plugin, 'route.add', null, 'invalid_argument');
      await expectInvokeStatus(ctx.plugin, 'route.add', { ...valid, path: 7 }, 'invalid_argument');
    } finally {
      await ctx.plugin.stop();
    }
  });

  it('publishes its four management methods during start()', async () => {
    const fake = makeFakeServices();
    const ctx = await startPlugin(fake);
    try {
      expect(fake.published.length).toBe(1);
      expect(fake.published[0]?.methods.map((method) => method.name)).toEqual([
        'route.add',
        'route.remove',
        'routes.list',
        'stats',
      ]);
    } finally {
      await ctx.plugin.stop();
    }
  });
});

// ---------------------------------------------------------------------------
// HTTP forwarding through one-shot leases.
// ---------------------------------------------------------------------------

describe('std.http-server HTTP forwarding', () => {
  it('matches :params, forwards query/headers/body and returns the gateway result', async () => {
    const fake = makeFakeServices({ call: () => ({ served: true }) });
    const ctx = await startPlugin(fake);
    try {
      await invokeMethod(ctx.plugin, 'route.add', {
        path: '/api/:id',
        httpMethod: 'GET',
        targetPlugId: 'example.target',
        targetMethod: 'fetchIt',
      });
      const response = await httpRequest(ctx.port, 'GET', '/api/42?q=hello&lang=zh');
      expect(response.status).toBe(200);
      expect(response.json()).toEqual({ ok: true, result: { served: true } });

      expect(fake.acquisitions.length).toBe(1);
      expect(fake.acquisitions[0]?.plugId).toBe('example.target');
      expect(fake.acquisitions[0]?.range).toEqual(FULL_RANGE);
      expect(fake.calls.length).toBe(1);
      const entry = fake.calls[0];
      expect(entry?.methodName).toBe('fetchIt');
      expect(entry?.credential).toBe(fake.acquisitions[0]?.credential);
      const args = callArgs(entry);
      expect(args.query).toEqual({ q: 'hello', lang: 'zh' });
      expect(args.params).toEqual({ id: '42' });
      expect(args.body).toBeNull();
      expect(String(args.headers['host'] ?? '')).toContain('127.0.0.1');
    } finally {
      await ctx.plugin.stop();
    }
  });

  it('forwards parsed JSON bodies and null for non-JSON bodies', async () => {
    const fake = makeFakeServices();
    const ctx = await startPlugin(fake);
    try {
      await invokeMethod(ctx.plugin, 'route.add', {
        path: '/items',
        httpMethod: 'POST',
        targetPlugId: 'example.store',
        targetMethod: 'create',
      });
      const response = await httpRequest(ctx.port, 'POST', '/items?dry=1', {
        headers: { 'content-type': 'application/json', 'x-demo': 'abc' },
        body: JSON.stringify({ name: 'u42', tags: ['a', 'b'] }),
      });
      expect(response.status).toBe(200);
      expect(response.json()).toEqual({ ok: true, result: null });

      const jsonArgs = callArgs(fake.calls[0]);
      expect(jsonArgs.query).toEqual({ dry: '1' });
      expect(jsonArgs.params).toEqual({});
      expect(jsonArgs.headers['content-type']).toBe('application/json');
      expect(jsonArgs.headers['x-demo']).toBe('abc');
      expect(jsonArgs.body).toEqual({ name: 'u42', tags: ['a', 'b'] });

      await httpRequest(ctx.port, 'POST', '/items', {
        headers: { 'content-type': 'text/plain' },
        body: 'just plain text',
      });
      expect(callArgs(fake.calls[1]).body).toBeNull();
    } finally {
      await ctx.plugin.stop();
    }
  });

  it('borrows and returns one lease per request, with distinct credentials', async () => {
    const fake = makeFakeServices();
    const ctx = await startPlugin(fake);
    try {
      await invokeMethod(ctx.plugin, 'route.add', {
        path: '/api/:id',
        httpMethod: 'GET',
        targetPlugId: 'example.target',
        targetMethod: 'fetch',
      });
      for (const id of ['1', '2', '3']) {
        const response = await httpRequest(ctx.port, 'GET', `/api/${id}`);
        expect(response.status).toBe(200);
      }
      expect(fake.acquisitions.length).toBe(3);
      expect(fake.handleReleases.length).toBe(3);
      expect(new Set(fake.acquisitions.map((entry) => entry.credential)).size).toBe(3);
      expect(fake.capsReleases.length).toBe(0);
    } finally {
      await ctx.plugin.stop();
    }
  });

  it('handles concurrent requests with independent leases and reports active counts', async () => {
    const gates: Array<(value: JsonValue) => void> = [];
    const fake = makeFakeServices({
      call: () => new Promise<JsonValue>((resolve) => gates.push(resolve)),
    });
    const ctx = await startPlugin(fake);
    try {
      await invokeMethod(ctx.plugin, 'route.add', {
        path: '/slow',
        httpMethod: 'GET',
        targetPlugId: 'example.slow',
        targetMethod: 'work',
      });
      const first = httpRequest(ctx.port, 'GET', '/slow');
      const second = httpRequest(ctx.port, 'GET', '/slow');
      await waitUntil(() => fake.calls.length === 2 && gates.length === 2);

      const midStats = await invokeMethod(ctx.plugin, 'stats', null);
      expect(midStats).toEqual({ requests: 2, errors: 0, active: 2, port: expect.any(Number) });
      expect(fake.acquisitions.length).toBe(2);
      expect(fake.handleReleases.length).toBe(0);
      expect(new Set(fake.acquisitions.map((entry) => entry.credential)).size).toBe(2);

      for (const gate of gates) gate({ n: 1 });
      const responses = await Promise.all([first, second]);
      for (const response of responses) {
        expect(response.status).toBe(200);
        expect(response.json()).toEqual({ ok: true, result: { n: 1 } });
      }
      await waitUntil(() => fake.handleReleases.length === 2);
      const endStats = await invokeMethod(ctx.plugin, 'stats', null);
      expect(endStats).toEqual({ requests: 2, errors: 0, active: 0, port: expect.any(Number) });
    } finally {
      gates.splice(0).forEach((gate) => gate(null));
      await ctx.plugin.stop();
    }
  });
});

// ---------------------------------------------------------------------------
// Error mapping.
// ---------------------------------------------------------------------------

describe('std.http-server error mapping', () => {
  it('answers 404 for unmatched paths and 405 for wrong methods', async () => {
    const fake = makeFakeServices();
    const ctx = await startPlugin(fake);
    try {
      await invokeMethod(ctx.plugin, 'route.add', {
        path: '/only',
        httpMethod: 'POST',
        targetPlugId: 'example.target',
        targetMethod: 'work',
      });
      const notFound = await httpRequest(ctx.port, 'GET', '/missing');
      expect(notFound.status).toBe(404);
      expect(notFound.json()).toMatchObject({ ok: false, error: { status: 'not_found', message: expect.any(String) } });

      const wrongMethod = await httpRequest(ctx.port, 'GET', '/only');
      expect(wrongMethod.status).toBe(405);
      expect(wrongMethod.headers['allow']).toBe('POST');
      expect(wrongMethod.json()).toMatchObject({
        ok: false,
        error: { status: 'unsupported', message: expect.any(String) },
      });

      expect(fake.acquisitions.length).toBe(0);
      expect(fake.calls.length).toBe(0);
    } finally {
      await ctx.plugin.stop();
    }
  });

  it('answers 413 and never leases when the body exceeds 4 MiB', async () => {
    const fake = makeFakeServices();
    const ctx = await startPlugin(fake);
    try {
      await invokeMethod(ctx.plugin, 'route.add', {
        path: '/big',
        httpMethod: 'POST',
        targetPlugId: 'example.target',
        targetMethod: 'work',
      });
      const oversized = 'x'.repeat(4 * 1024 * 1024 + 1);
      const response = await httpRequest(ctx.port, 'POST', '/big', {
        headers: { 'content-type': 'text/plain' },
        body: oversized,
      });
      expect(response.status).toBe(413);
      expect(response.json()).toMatchObject({
        ok: false,
        error: { status: 'limit_exceeded', message: expect.any(String) },
      });
      expect(fake.acquisitions.length).toBe(0);
      expect(fake.calls.length).toBe(0);
    } finally {
      await ctx.plugin.stop();
    }
  });

  it('answers 503 when acquire reports not_found or unsupported', async () => {
    const ctx = await startPlugin(
      makeFakeServices({ acquire: () => {
        throw new RackError('not_found', 'no provider for example.gone');
      } }),
    );
    try {
      await invokeMethod(ctx.plugin, 'route.add', {
        path: '/gone',
        httpMethod: 'GET',
        targetPlugId: 'example.gone',
        targetMethod: 'work',
      });
      const response = await httpRequest(ctx.port, 'GET', '/gone');
      expect(response.status).toBe(503);
      expect(response.json()).toMatchObject({
        ok: false,
        error: { status: 'not_found', message: expect.stringContaining('example.gone') },
      });
    } finally {
      await ctx.plugin.stop();
    }

    const ctx2 = await startPlugin(
      makeFakeServices({ acquire: () => {
        throw new RackError('unsupported', 'version 2.0.0 outside range');
      } }),
    );
    try {
      await invokeMethod(ctx2.plugin, 'route.add', {
        path: '/away',
        httpMethod: 'GET',
        targetPlugId: 'example.away',
        targetMethod: 'work',
      });
      const response = await httpRequest(ctx2.port, 'GET', '/away');
      expect(response.status).toBe(503);
      expect(response.json()).toMatchObject({ ok: false, error: { status: 'unsupported' } });
    } finally {
      await ctx2.plugin.stop();
    }
  });

  it('delivers target failures as HTTP 200 ok:false envelopes', async () => {
    const fake = makeFakeServices({
      call: () => {
        throw new RackError('failed', 'boom');
      },
    });
    const ctx = await startPlugin(fake);
    try {
      await invokeMethod(ctx.plugin, 'route.add', {
        path: '/api/:id',
        httpMethod: 'GET',
        targetPlugId: 'example.target',
        targetMethod: 'fetch',
      });
      const response = await httpRequest(ctx.port, 'GET', '/api/9');
      expect(response.status).toBe(200);
      expect(response.json()).toEqual({
        ok: false,
        error: { status: 'failed', message: expect.stringContaining('boom') },
      });
      expect(fake.handleReleases.length).toBe(1);
    } finally {
      await ctx.plugin.stop();
    }
  });

  it('counts errors only for requests without an ok:true outcome', async () => {
    const fake = makeFakeServices({
      call: () => {
        throw new Error('kaputt');
      },
    });
    const ctx = await startPlugin(fake);
    try {
      await invokeMethod(ctx.plugin, 'route.add', {
        path: '/api/:id',
        httpMethod: 'GET',
        targetPlugId: 'example.target',
        targetMethod: 'fetch',
      });
      await httpRequest(ctx.port, 'GET', '/api/1'); // ok:false business failure
      await httpRequest(ctx.port, 'GET', '/nope'); // 404
      const stats = await invokeMethod(ctx.plugin, 'stats', null);
      expect(stats).toEqual({ requests: 2, errors: 2, active: 0, port: expect.any(Number) });
    } finally {
      await ctx.plugin.stop();
    }
  });
});

// ---------------------------------------------------------------------------
// stop() semantics.
// ---------------------------------------------------------------------------

describe('std.http-server stop()', () => {
  it('closes the listener so further connections are refused', async () => {
    const ctx = await startPlugin(makeFakeServices());
    await ctx.plugin.stop();
    expect(await connectRefused(ctx.port)).toBe('ECONNREFUSED');
    try {
      await ctx.plugin.start();
      expect.unreachable();
    } catch (error) {
      expect(isRackError(error)).toBe(true);
      expect(statusOf(error)).toBe('invalid_state');
    }
  });

  it('waits for in-flight requests to settle before returning', async () => {
    const gates: Array<(value: JsonValue) => void> = [];
    const fake = makeFakeServices({
      call: () => new Promise<JsonValue>((resolve) => gates.push(resolve)),
    });
    const ctx = await startPlugin(fake);
    await invokeMethod(ctx.plugin, 'route.add', {
      path: '/slow',
      httpMethod: 'GET',
      targetPlugId: 'example.slow',
      targetMethod: 'work',
    });
    const responsePromise = httpRequest(ctx.port, 'GET', '/slow');
    await waitUntil(() => fake.calls.length === 1 && gates.length === 1);

    const stopPromise = ctx.plugin.stop();
    gates.splice(0).forEach((gate) => gate({ done: true }));
    const [response] = await Promise.all([responsePromise, stopPromise]);
    expect(response.status).toBe(200);
    expect(response.json()).toEqual({ ok: true, result: { done: true } });
    expect(fake.handleReleases.length).toBe(1);
    expect(await invokeMethod(ctx.plugin, 'stats', null)).toEqual({ requests: 1, errors: 0, active: 0, port: null });
    expect(await connectRefused(ctx.port)).toBe('ECONNREFUSED');
  });
});

// ---------------------------------------------------------------------------
// Example demo plugin.
// ---------------------------------------------------------------------------

describe('http-server-demo example plugin', () => {
  it('registers and removes its route through one-shot leased gateway calls', async () => {
    const fake = makeFakeServices({ call: () => ({ routeId: 'r-1' }) });
    const demo = createDemoPlugin();
    await demo.init(fake.services);
    await demo.start();
    // The route registration is deferred to the next macrotask (Active-consumer rule).
    await new Promise<void>((resolve) => setImmediate(resolve));

    expect(fake.published[0]?.methods.map((method) => method.name)).toEqual(['hit']);
    expect(fake.acquisitions.length).toBe(1);
    expect(fake.acquisitions[0]?.plugId).toBe('std.http-server');
    expect(fake.calls.length).toBe(1);
    expect(fake.calls[0]?.methodName).toBe('route.add');
    expect(fake.calls[0]?.args).toEqual({
      path: '/demo/:name',
      httpMethod: 'GET',
      targetPlugId: 'example.http-server-demo',
      targetMethod: 'hit',
    });
    expect(fake.handleReleases.length).toBe(1);

    const firstHit = await demo.invoke?.({ methodId: 1, methodName: 'hit', args: { params: { name: 'u42' } } });
    expect(firstHit).toEqual({ hits: 1, received: { params: { name: 'u42' } } });
    const secondHit = await demo.invoke?.({ methodId: 1, methodName: 'hit', args: null });
    expect(secondHit).toEqual({ hits: 2, received: null });

    await demo.stop();
    expect(fake.calls[1]?.methodName).toBe('route.remove');
    expect(fake.calls[1]?.args).toEqual({ routeId: 'r-1' });
    expect(fake.handleReleases.length).toBe(2);
  });
});
