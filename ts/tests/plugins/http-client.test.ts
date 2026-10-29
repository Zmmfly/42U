/**
 * @file Tests for the std.http-client standard device and its demo consumer.
 *
 * @brief Every HTTP effect goes through an injected fake fetch (records calls, programmable
 *        or hanging responses) and every PluginServices bundle is a self-written fake; no
 *        network access and no host implementation is involved.
 */
import { afterEach, describe, expect, it, vi } from 'vitest';
import {
  create,
  createWithDeps,
  HTTP_CLIENT_PLUG_ID,
} from '../../src/plugins/http-client/index.ts';
import type { FetchLike, FetchInitLike } from '../../src/plugins/http-client/index.ts';
import { RackError, typedApi } from '../../src/sdk/index.ts';
import type {
  CallService,
  CapEvent,
  CapSink,
  CapsDesc,
  JsonObject,
  JsonValue,
  LeaseCredential,
  LeaseHandle,
  PluginServices,
  RackPlugin,
  RackStatus,
  Revoker,
  VersionRange,
} from '../../src/sdk/index.ts';
import { create as createDemo } from '../../examples/plugins/http-client-demo.plug.ts';

afterEach(() => {
  vi.useRealTimers();
  vi.unstubAllEnvs();
});

// ---------------------------------------------------------------------------
// Fake fetch: records every call, answers from a script or a fallback handler.
// ---------------------------------------------------------------------------

/** @brief One recorded fake fetch call with its full init document and fake clock time. */
interface RecordedFetchCall {
  readonly url: string;
  readonly method: string;
  readonly headers: Record<string, string>;
  readonly body: string | undefined;
  readonly signal: AbortSignal | undefined;
  readonly at: number;
}

/** @brief Programmable handler for one fake fetch call. */
type FetchHandler = (call: RecordedFetchCall) => Promise<Response>;

/**
 * @brief Build a fake fetch implementation with a per-call script and a fallback.
 *
 * @return The fetch function plus the shared call record and programming entries.
 */
function makeFakeFetch(): {
  calls: RecordedFetchCall[];
  enqueue(handler: FetchHandler): void;
  setFallback(handler: FetchHandler): void;
  fetch: FetchLike;
} {
  const calls: RecordedFetchCall[] = [];
  const script: FetchHandler[] = [];
  let fallback: FetchHandler = () => Promise.resolve(jsonResponse(200, {}));
  const fetch: FetchLike = (input: string, init?: FetchInitLike) => {
    const call: RecordedFetchCall = {
      url: input,
      method: init?.method ?? 'GET',
      headers: { ...(init?.headers ?? {}) },
      body: init?.body,
      signal: init?.signal,
      at: Date.now(),
    };
    calls.push(call);
    const handler = script.shift() ?? fallback;
    return handler(call);
  };
  return {
    calls,
    enqueue(handler: FetchHandler): void {
      script.push(handler);
    },
    setFallback(handler: FetchHandler): void {
      fallback = handler;
    },
    fetch,
  };
}

/**
 * @brief Build a JSON response.
 *
 * @param status HTTP status code.
 * @param value Value serialized into the body.
 * @param headers Extra response headers.
 * @return A real Response with content-type application/json.
 */
function jsonResponse(
  status: number,
  value: JsonValue,
  headers: Record<string, string> = {},
): Response {
  return new Response(JSON.stringify(value), {
    status,
    headers: { 'content-type': 'application/json', ...headers },
  });
}

/**
 * @brief Build a plain-text response.
 *
 * @param status HTTP status code.
 * @param body Response body text.
 * @param headers Extra response headers.
 * @return A real Response.
 */
function textResponse(
  status: number,
  body: string,
  headers: Record<string, string> = {},
): Response {
  return new Response(body, { status, headers });
}

/** @brief Handler that pends until its abort signal fires (fake network hang). */
const hangUntilAbort: FetchHandler = (call) =>
  new Promise<Response>((_resolve, reject) => {
    call.signal?.addEventListener('abort', () => {
      reject(call.signal?.reason ?? new Error('aborted'));
    });
  });

/**
 * @brief Handler always rejecting with the given error.
 *
 * @param error Rejection value.
 * @return The handler.
 */
const rejectWith = (error: unknown): FetchHandler => () => Promise.reject(error);

/**
 * @brief Handler always resolving with the given response.
 *
 * @param response Response to return.
 * @return The handler.
 */
const respondWith = (response: Response): FetchHandler => () => Promise.resolve(response);

// ---------------------------------------------------------------------------
// Fake services and shared helpers.
// ---------------------------------------------------------------------------

/**
 * @brief Minimal fake PluginServices for provider-side tests.
 *
 * @return The services object plus the recorded capability publications.
 */
function makePluginServices(): { services: PluginServices; published: CapsDesc[] } {
  const published: CapsDesc[] = [];
  const services: PluginServices = {
    plugId: 'std.http-client',
    events: { subscribe: () => ({ unsubscribe() {} }), publish() {} },
    caps: {
      publish: (caps: CapsDesc) => {
        published.push(caps);
      },
      watch: () => ({ unsubscribe() {} }),
      snapshot: () => [],
      acquire: () => {
        throw new RackError('invalid_state', 'not used in provider tests');
      },
      release: () => 'ok',
    },
    calls: { call: () => Promise.resolve(null), callById: () => Promise.resolve(null) },
    diag: { report() {} },
  };
  return { services, published };
}

/**
 * @brief Invoke one plugin method by id and name.
 *
 * @param plugin Plugin under test.
 * @param methodId Numeric selector.
 * @param methodName Name selector.
 * @param args Args document.
 * @return The invoke result promise.
 */
function call(
  plugin: RackPlugin,
  methodId: number,
  methodName: string,
  args: JsonValue,
): Promise<JsonValue> {
  const result = plugin.invoke?.({ methodId, methodName, args });
  if (result instanceof Promise) return result;
  return Promise.resolve(result ?? null);
}

/**
 * @brief Assert that a promise rejects with a RackError of the expected status.
 *
 * @param promise Promise under test.
 * @param status Expected RackStatus.
 * @param inspect Optional detail inspection callback.
 * @return The caught RackError.
 */
async function expectRejected(
  promise: Promise<unknown>,
  status: RackStatus,
  inspect?: (error: RackError) => void,
): Promise<RackError> {
  let caught: unknown;
  try {
    await promise;
  } catch (error) {
    caught = error;
  }
  expect(caught).toBeInstanceOf(RackError);
  const rackError = caught as RackError;
  expect(rackError.status).toBe(status);
  inspect?.(rackError);
  return rackError;
}

// ---------------------------------------------------------------------------
// Capability shape.
// ---------------------------------------------------------------------------

describe('std.http-client capability shape', () => {
  it('announces identity, version and the five contiguous methods on start', async () => {
    const fake = makePluginServices();
    const plugin = create();
    expect(plugin.desc.plugId).toBe(HTTP_CLIENT_PLUG_ID);
    expect(plugin.desc.version).toEqual({ major: 1, minor: 0, patch: 0 });
    expect(plugin.desc.init.before).toEqual([]);
    expect(plugin.desc.init.after).toEqual([]);
    await plugin.init(fake.services);
    await plugin.start();
    expect(fake.published).toHaveLength(1);
    const methods = fake.published[0]?.methods ?? [];
    expect(methods.map((method) => [method.id, method.name])).toEqual([
      [1, 'request'],
      [2, 'get'],
      [3, 'post'],
      [4, 'put'],
      [5, 'del'],
    ]);
    for (const method of methods) {
      expect(method.description?.length ?? 0).toBeGreaterThan(0);
    }
  });
});

// ---------------------------------------------------------------------------
// Basic requests.
// ---------------------------------------------------------------------------

describe('basic requests through the fake fetch', () => {
  it('performs a default GET and returns status, ok, headers and json', async () => {
    const fake = makeFakeFetch();
    fake.setFallback(() =>
      Promise.resolve(jsonResponse(200, { hello: '42u' }, { 'X-Custom': 'v' })),
    );
    const plugin = createWithDeps({ fetchImpl: fake.fetch });
    const result = await call(plugin, 1, 'request', { url: 'https://example.test/api' });
    expect(result).toEqual({
      status: 200,
      ok: true,
      headers: { 'content-type': 'application/json', 'x-custom': 'v' },
      json: { hello: '42u' },
    });
    expect(fake.calls).toHaveLength(1);
    expect(fake.calls[0]?.url).toBe('https://example.test/api');
    expect(fake.calls[0]?.method).toBe('GET');
    expect(fake.calls[0]?.body).toBeUndefined();
    expect(fake.calls[0]?.signal).toBeInstanceOf(AbortSignal);
  });

  it('sends a POST json body with the automatic content-type header', async () => {
    const fake = makeFakeFetch();
    fake.setFallback(() =>
      Promise.resolve(
        new Response('{"saved":true}', {
          status: 201,
          headers: { 'content-type': 'application/json' },
        }),
      ),
    );
    const plugin = createWithDeps({ fetchImpl: fake.fetch });
    const result = await call(plugin, 3, 'post', {
      url: 'https://example.test/items',
      json: { name: 'rack' },
      headers: { 'X-Token': 'secret' },
    });
    expect(result).toMatchObject({ status: 201, ok: true, json: { saved: true } });
    expect(fake.calls[0]?.method).toBe('POST');
    expect(fake.calls[0]?.body).toBe('{"name":"rack"}');
    expect(fake.calls[0]?.headers).toEqual({
      'X-Token': 'secret',
      'content-type': 'application/json',
    });
  });

  it('keeps an explicit content-type instead of the automatic one', async () => {
    const fake = makeFakeFetch();
    fake.setFallback(() => Promise.resolve(textResponse(200, 'ok: 42')));
    const plugin = createWithDeps({ fetchImpl: fake.fetch });
    const result = await call(plugin, 4, 'put', {
      url: 'https://example.test/raw',
      headers: { 'Content-Type': 'application/vnd.api+json' },
      json: { a: 1 },
    });
    expect(result).toMatchObject({ status: 200, ok: true, text: 'ok: 42' });
    expect(fake.calls[0]?.method).toBe('PUT');
    expect(fake.calls[0]?.body).toBe('{"a":1}');
    expect(fake.calls[0]?.headers?.['Content-Type']).toBe('application/vnd.api+json');
    expect(fake.calls[0]?.headers?.['content-type']).toBeUndefined();
  });

  it('maps del to DELETE and normalizes lower-case methods', async () => {
    const fake = makeFakeFetch();
    fake.setFallback(() => Promise.resolve(textResponse(200, 'ok')));
    const plugin = createWithDeps({ fetchImpl: fake.fetch });
    await call(plugin, 5, 'del', { url: 'https://example.test/1' });
    expect(fake.calls[0]?.method).toBe('DELETE');
    await call(plugin, 1, 'request', { url: 'https://example.test/2', method: 'patch' });
    expect(fake.calls[1]?.method).toBe('PATCH');
  });

  it('returns text for non-JSON bodies and an empty text for empty JSON bodies', async () => {
    const fake = makeFakeFetch();
    const plugin = createWithDeps({ fetchImpl: fake.fetch });
    fake.setFallback(() => Promise.resolve(textResponse(200, 'plain')));
    const text = (await call(plugin, 2, 'get', { url: 'https://example.test/txt' })) as JsonObject;
    expect(text).toMatchObject({ status: 200, ok: true, text: 'plain' });
    expect(text['json']).toBeUndefined();
    fake.setFallback(() =>
      Promise.resolve(
        new Response('', { status: 200, headers: { 'content-type': 'application/json' } }),
      ),
    );
    const empty = (await call(plugin, 2, 'get', { url: 'https://example.test/empty' })) as JsonObject;
    expect(empty).toMatchObject({ status: 200, ok: true, text: '' });
    expect(empty['json']).toBeUndefined();
  });

  it('resolves name and id dispatch identically', async () => {
    const fake = makeFakeFetch();
    fake.setFallback(() => Promise.resolve(jsonResponse(200, { same: true })));
    const plugin = createWithDeps({ fetchImpl: fake.fetch });
    const byId = await call(plugin, 2, 'uninteresting', { url: 'https://example.test/' });
    const byName = await call(plugin, 0, 'get', { url: 'https://example.test/' });
    expect(byId).toEqual(byName);
  });
});

// ---------------------------------------------------------------------------
// Argument validation.
// ---------------------------------------------------------------------------

describe('argument validation', () => {
  it('rejects malformed args with invalid_argument before reaching fetch', async () => {
    const fake = makeFakeFetch();
    const plugin = createWithDeps({ fetchImpl: fake.fetch });
    const bad: JsonValue[] = [
      null,
      'https://example.test/',
      [],
      { method: 'GET' },
      { url: 5 },
      { url: 'not a url' },
      { url: 'ftp://example.test/x' },
      { url: 'https://example.test/', method: 'FETCH' },
      { url: 'https://example.test/', method: 7 },
      { url: 'https://example.test/', json: { a: 1 }, text: 'b' },
      { url: 'https://example.test/', json: { a: 1 }, method: 'GET' },
      { url: 'https://example.test/', text: 'b', method: 'HEAD' },
      { url: 'https://example.test/', headers: 'x' },
      { url: 'https://example.test/', headers: { 'x-y': 3 } },
      { url: 'https://example.test/', headers: { '': 'v' } },
      { url: 'https://example.test/', timeoutMs: 0 },
      { url: 'https://example.test/', timeoutMs: -5 },
      { url: 'https://example.test/', retries: -1 },
      { url: 'https://example.test/', retries: 1.5 },
      { url: 'https://example.test/', retryBackoffMs: -2 },
      { url: 'https://example.test/', expectOk: 'yes' },
      { url: 'https://example.test/', text: 9 },
    ];
    for (const args of bad) {
      await expectRejected(call(plugin, 1, 'request', args), 'invalid_argument');
    }
    await expectRejected(
      call(plugin, 99, 'nope', { url: 'https://example.test/' }),
      'not_found',
    );
    expect(fake.calls).toHaveLength(0);
  });

  it('rejects an explicit method on the shortcut methods', async () => {
    const plugin = createWithDeps({ fetchImpl: makeFakeFetch().fetch });
    await expectRejected(
      call(plugin, 2, 'get', { url: 'https://example.test/', method: 'POST' }),
      'invalid_argument',
    );
    await expectRejected(
      call(plugin, 3, 'post', { url: 'https://example.test/', method: 'GET' }),
      'invalid_argument',
    );
  });

  it('rejects malformed deps at factory time', () => {
    expect(() => createWithDeps({ maxBytes: 0 })).toThrow(RackError);
    expect(() => createWithDeps({ maxBytes: 1.5 })).toThrow(RackError);
    expect(() =>
      createWithDeps({ fetchImpl: 'nope' as unknown as FetchLike }),
    ).toThrow(RackError);
  });
});

// ---------------------------------------------------------------------------
// Timeout, retry counting and backoff.
// ---------------------------------------------------------------------------

describe('timeout, retry counting and backoff', () => {
  it('retries timeouts with linear backoff and fails with attempts once exhausted', async () => {
    vi.useFakeTimers();
    const fake = makeFakeFetch();
    fake.setFallback(hangUntilAbort);
    const plugin = createWithDeps({ fetchImpl: fake.fetch });
    const outcome = call(plugin, 1, 'request', {
      url: 'https://slow.test/',
      timeoutMs: 100,
      retries: 2,
      retryBackoffMs: 50,
    }).then(
      (value) => ({ ok: true as const, value }),
      (error: unknown) => ({ ok: false as const, error }),
    );
    await vi.advanceTimersByTimeAsync(100);
    expect(fake.calls).toHaveLength(1);
    await vi.advanceTimersByTimeAsync(50);
    expect(fake.calls).toHaveLength(2);
    await vi.advanceTimersByTimeAsync(100);
    expect(fake.calls).toHaveLength(2);
    await vi.advanceTimersByTimeAsync(100);
    expect(fake.calls).toHaveLength(3);
    await vi.advanceTimersByTimeAsync(100);
    const settled = await outcome;
    expect(settled.ok).toBe(false);
    expect(fake.calls).toHaveLength(3);
    const at = fake.calls.map((entry) => entry.at);
    // Attempt 2 starts after the 100ms timeout plus 1x50ms backoff; attempt 3 after 2x50ms.
    expect(at[1]! - at[0]!).toBe(150);
    expect(at[2]! - at[1]!).toBe(200);
    if (!settled.ok) {
      expect(settled.error).toBeInstanceOf(RackError);
      expect((settled.error as RackError).status).toBe('failed');
      expect((settled.error as RackError).detail).toMatchObject({
        attempts: 3,
        method: 'GET',
        url: 'https://slow.test/',
      });
    }
  });

  it('fails a single timeout without retries', async () => {
    const fake = makeFakeFetch();
    fake.setFallback(hangUntilAbort);
    const plugin = createWithDeps({ fetchImpl: fake.fetch });
    await expectRejected(
      call(plugin, 1, 'request', { url: 'https://slow.test/', timeoutMs: 10 }),
      'failed',
      (error) => {
        expect(error.detail).toMatchObject({ attempts: 1 });
      },
    );
    expect(fake.calls).toHaveLength(1);
  });
});

// ---------------------------------------------------------------------------
// Retry policy.
// ---------------------------------------------------------------------------

describe('retry policy', () => {
  it('retries 5xx responses and succeeds on a later attempt', async () => {
    const fake = makeFakeFetch();
    fake.enqueue(() => Promise.resolve(textResponse(500, 'boom')));
    fake.enqueue(() => Promise.resolve(jsonResponse(200, { fine: true })));
    const plugin = createWithDeps({ fetchImpl: fake.fetch });
    const result = await call(plugin, 1, 'request', {
      url: 'https://flaky.test/',
      retries: 1,
      retryBackoffMs: 1,
    });
    expect(result).toMatchObject({ status: 200, ok: true, json: { fine: true } });
    expect(fake.calls).toHaveLength(2);
  });

  it('returns a final 5xx normally once the budget is spent', async () => {
    const fake = makeFakeFetch();
    fake.enqueue(() => Promise.resolve(textResponse(500, 'boom')));
    fake.enqueue(() => Promise.resolve(textResponse(500, 'boom')));
    const plugin = createWithDeps({ fetchImpl: fake.fetch });
    const result = await call(plugin, 1, 'request', {
      url: 'https://down.test/',
      retries: 1,
      retryBackoffMs: 1,
    });
    expect(result).toMatchObject({ status: 500, ok: false, text: 'boom' });
    expect(fake.calls).toHaveLength(2);
  });

  it('never retries 4xx responses', async () => {
    const fake = makeFakeFetch();
    fake.setFallback(() => Promise.resolve(textResponse(404, 'nope')));
    const plugin = createWithDeps({ fetchImpl: fake.fetch });
    const result = await call(plugin, 2, 'get', {
      url: 'https://example.test/missing',
      retries: 3,
      retryBackoffMs: 1,
    });
    expect(result).toMatchObject({ status: 404, ok: false, text: 'nope' });
    expect(fake.calls).toHaveLength(1);
  });

  it('retries network rejections and fails with attempts when exhausted', async () => {
    const fake = makeFakeFetch();
    fake.setFallback(rejectWith(new Error('ECONNRESET')));
    const plugin = createWithDeps({ fetchImpl: fake.fetch });
    await expectRejected(
      call(plugin, 1, 'request', {
        url: 'https://dead.test/',
        retries: 2,
        retryBackoffMs: 1,
      }),
      'failed',
      (error) => {
        expect(error.detail).toMatchObject({ attempts: 3, lastError: 'ECONNRESET' });
      },
    );
    expect(fake.calls).toHaveLength(3);
  });

  it('recovers when a retried network rejection is followed by success', async () => {
    const fake = makeFakeFetch();
    fake.enqueue(rejectWith(new Error('socket hang up')));
    // 204 carries a null body by spec; the empty body resolves to text ''.
    fake.enqueue(respondWith(new Response(null, { status: 204 })));
    const plugin = createWithDeps({ fetchImpl: fake.fetch });
    const result = await call(plugin, 1, 'request', {
      url: 'https://flaky.test/',
      retries: 1,
      retryBackoffMs: 1,
    });
    expect(result).toMatchObject({ status: 204, ok: true, text: '' });
    expect(fake.calls).toHaveLength(2);
  });
});

// ---------------------------------------------------------------------------
// expectOk.
// ---------------------------------------------------------------------------

describe('expectOk', () => {
  it('fails on non-2xx with the status and a body snippet', async () => {
    const fake = makeFakeFetch();
    fake.setFallback(() => Promise.resolve(textResponse(404, 'nope')));
    const plugin = createWithDeps({ fetchImpl: fake.fetch });
    await expectRejected(
      call(plugin, 2, 'get', { url: 'https://example.test/missing', expectOk: true }),
      'failed',
      (error) => {
        expect(error.detail).toMatchObject({ status: 404, body: 'nope' });
      },
    );
  });

  it('passes on 2xx and defaults to false', async () => {
    const fake = makeFakeFetch();
    fake.setFallback(() => Promise.resolve(jsonResponse(200, { ok: true })));
    const plugin = createWithDeps({ fetchImpl: fake.fetch });
    const strict = await call(plugin, 1, 'request', {
      url: 'https://example.test/',
      expectOk: true,
    });
    expect(strict).toMatchObject({ status: 200, ok: true });
    const lenient = (await call(plugin, 1, 'request', {
      url: 'https://example.test/not-there',
      expectOk: false,
    })) as JsonObject;
    expect(lenient['ok']).toBe(true); // expectOk=false never turns a result into an error
  });

  it('spends the retry budget on 5xx before failing expectOk', async () => {
    const fake = makeFakeFetch();
    fake.enqueue(() => Promise.resolve(textResponse(500, 'boom')));
    fake.enqueue(() => Promise.resolve(textResponse(500, 'boom')));
    const plugin = createWithDeps({ fetchImpl: fake.fetch });
    await expectRejected(
      call(plugin, 1, 'request', {
        url: 'https://down.test/',
        retries: 1,
        retryBackoffMs: 1,
        expectOk: true,
      }),
      'failed',
      (error) => {
        expect(error.detail).toMatchObject({ status: 500, body: 'boom' });
      },
    );
    expect(fake.calls).toHaveLength(2);
  });
});

// ---------------------------------------------------------------------------
// Response size guard.
// ---------------------------------------------------------------------------

describe('response size guard', () => {
  it('rejects oversized text bodies with limit_exceeded', async () => {
    const fake = makeFakeFetch();
    fake.setFallback(() => Promise.resolve(textResponse(200, 'x'.repeat(100))));
    const plugin = createWithDeps({ fetchImpl: fake.fetch, maxBytes: 16 });
    await expectRejected(
      call(plugin, 2, 'get', { url: 'https://big.test/' }),
      'limit_exceeded',
      (error) => {
        expect(error.detail).toMatchObject({ limitBytes: 16, status: 200 });
      },
    );
  });

  it('accepts a body exactly at the limit', async () => {
    const fake = makeFakeFetch();
    fake.setFallback(() => Promise.resolve(textResponse(200, '0123456789')));
    const plugin = createWithDeps({ fetchImpl: fake.fetch, maxBytes: 10 });
    const result = await call(plugin, 2, 'get', { url: 'https://edge.test/' });
    expect(result).toMatchObject({ status: 200, text: '0123456789' });
  });

  it('interrupts the underlying stream when the guard trips', async () => {
    const cancelled: string[] = [];
    const stream = new ReadableStream<Uint8Array>({
      start(controller) {
        controller.enqueue(new TextEncoder().encode('0123456789'));
        controller.enqueue(new TextEncoder().encode('abcdefghij'));
      },
      cancel(reason) {
        cancelled.push(String(reason));
      },
    });
    const fake = makeFakeFetch();
    fake.setFallback(() => Promise.resolve(new Response(stream, { status: 200 })));
    const plugin = createWithDeps({ fetchImpl: fake.fetch, maxBytes: 10 });
    await expectRejected(call(plugin, 2, 'get', { url: 'https://stream.test/' }), 'limit_exceeded');
    expect(cancelled).toHaveLength(1);
  });

  it('does not consume the retry budget on guard failures', async () => {
    const fake = makeFakeFetch();
    fake.setFallback(() => Promise.resolve(textResponse(200, 'x'.repeat(100))));
    const plugin = createWithDeps({ fetchImpl: fake.fetch, maxBytes: 8 });
    await expectRejected(
      call(plugin, 1, 'request', { url: 'https://big.test/', retries: 3, retryBackoffMs: 1 }),
      'limit_exceeded',
    );
    expect(fake.calls).toHaveLength(1);
  });

  it('honours U42_HTTP_MAX_BYTES and falls back on invalid values', async () => {
    vi.stubEnv('U42_HTTP_MAX_BYTES', '8');
    const fake = makeFakeFetch();
    fake.setFallback(() => Promise.resolve(textResponse(200, '123456789')));
    const plugin = createWithDeps({ fetchImpl: fake.fetch });
    await expectRejected(call(plugin, 2, 'get', { url: 'https://env.test/' }), 'limit_exceeded');
    vi.stubEnv('U42_HTTP_MAX_BYTES', 'not-a-number');
    const lenient = createWithDeps({ fetchImpl: fake.fetch });
    const result = await call(lenient, 2, 'get', { url: 'https://env.test/' });
    expect(result).toMatchObject({ status: 200, text: '123456789' });
  });
});

// ---------------------------------------------------------------------------
// typedApi packing compatibility.
// ---------------------------------------------------------------------------

describe('typedApi packing compatibility', () => {
  it('accepts single-object packing and rejects array or null packing', async () => {
    const fake = makeFakeFetch();
    fake.setFallback(() => Promise.resolve(jsonResponse(200, { packed: true })));
    const plugin = createWithDeps({ fetchImpl: fake.fetch });
    const forwarded: Array<{ methodName: string; args: JsonValue }> = [];
    const calls: CallService = {
      call: (_credential, methodName, args) => {
        forwarded.push({ methodName, args });
        return Promise.resolve(
          plugin.invoke?.({ methodId: 0, methodName, args }) ?? null,
        );
      },
      callById: (_credential, methodId, args) => {
        forwarded.push({ methodName: `#${methodId}`, args });
        return Promise.resolve(plugin.invoke?.({ methodId, methodName: '', args }) ?? null);
      },
    };
    const handle: LeaseHandle = {
      credential: { kind: 'u42.lease.credential', handle: { seq: 1 } },
      plugId: 'std.http-client',
      version: { major: 1, minor: 0, patch: 0 },
      release: () => Promise.resolve('ok'),
    };
    const api = typedApi<{
      get: (args: JsonValue) => Promise<JsonValue>;
      request: (...args: JsonValue[]) => Promise<JsonValue>;
    }>(calls, handle);

    const result = await api.get({ url: 'https://pack.test/a' });
    expect(result).toMatchObject({ status: 200, json: { packed: true } });
    expect(forwarded).toHaveLength(1);
    expect(forwarded[0]?.methodName).toBe('get');
    expect(forwarded[0]?.args).toEqual({ url: 'https://pack.test/a' });

    // Multiple positional proxy arguments pack into a JSON array → not an args object.
    await expectRejected(
      api.request({ url: 'https://pack.test/a' }, { extra: 1 }),
      'invalid_argument',
    );
    // Zero arguments pack to null → likewise rejected.
    await expectRejected(api.request(), 'invalid_argument');
    expect(fake.calls).toHaveLength(1);
  });
});

// ---------------------------------------------------------------------------
// stop semantics.
// ---------------------------------------------------------------------------

describe('stop semantics', () => {
  it('aborts in-flight requests, waits for settlement and rejects them', async () => {
    const fake = makeFakeFetch();
    fake.setFallback(hangUntilAbort);
    const fixture = makePluginServices();
    const plugin = createWithDeps({ fetchImpl: fake.fetch });
    await plugin.init(fixture.services);
    await plugin.start();
    const pending = call(plugin, 1, 'request', { url: 'https://hang.test/' });
    const stopping = plugin.stop();
    const error = await expectRejected(pending, 'failed');
    expect((error.detail as { reason?: string }).reason).toBe('stop');
    await stopping;
  });

  it('clears pending retry plans instead of waiting for the backoff', async () => {
    const fake = makeFakeFetch();
    fake.setFallback(hangUntilAbort);
    const plugin = createWithDeps({ fetchImpl: fake.fetch });
    const pending = call(plugin, 1, 'request', {
      url: 'https://slow.test/',
      timeoutMs: 5,
      retries: 5,
      retryBackoffMs: 60_000,
    });
    await new Promise<void>((resolve) => {
      globalThis.setTimeout(resolve, 30);
    });
    expect(fake.calls).toHaveLength(1);
    await plugin.stop();
    await expectRejected(pending, 'failed');
    expect(fake.calls).toHaveLength(1);
  });

  it('rejects new invokes defensively after stop', async () => {
    const plugin = createWithDeps({ fetchImpl: makeFakeFetch().fetch });
    await plugin.stop();
    await expectRejected(
      (async () =>
        plugin.invoke?.({ methodId: 1, methodName: 'request', args: { url: 'https://example.test/' } }))(),
      'invalid_state',
    );
  });

  it('resolves immediately and idempotently when nothing is in flight', async () => {
    const plugin = create();
    await plugin.stop();
    await plugin.stop();
  });
});

// ---------------------------------------------------------------------------
// Demo consumer plugin.
// ---------------------------------------------------------------------------

/**
 * @brief Fake PluginServices for the demo consumer: records publications, watchers,
 *        lease acquisitions, credential releases and gateway calls.
 *
 * @param programmedResult Result returned by every fake gateway call.
 * @return The services object plus its record arrays and delivery helpers.
 */
function makeDemoFixture(programmedResult: JsonValue): {
  services: PluginServices;
  published: CapsDesc[];
  acquisitions: Array<{ plugId: string; range: VersionRange; revoker: Revoker }>;
  releases: LeaseCredential[];
  gatewayCalls: Array<{ methodName: string; args: JsonValue }>;
  minted: LeaseHandle[];
  watcherCount(): number;
  deliverCapability(event: CapEvent): Promise<void>;
} {
  const published: CapsDesc[] = [];
  const watchers: CapSink[] = [];
  const acquisitions: Array<{ plugId: string; range: VersionRange; revoker: Revoker }> = [];
  const releases: LeaseCredential[] = [];
  const gatewayCalls: Array<{ methodName: string; args: JsonValue }> = [];
  const minted: LeaseHandle[] = [];
  let credentialSeq = 0;
  const services: PluginServices = {
    plugId: 'demo.http-client',
    events: { subscribe: () => ({ unsubscribe() {} }), publish() {} },
    caps: {
      publish: (caps: CapsDesc) => {
        published.push(caps);
      },
      watch: (sink: CapSink) => {
        watchers.push(sink);
        return {
          unsubscribe() {
            const index = watchers.indexOf(sink);
            if (index >= 0) watchers.splice(index, 1);
          },
        };
      },
      snapshot: () => [],
      acquire: (plugId: string, range: VersionRange, revoker: Revoker) => {
        acquisitions.push({ plugId, range, revoker });
        credentialSeq += 1;
        const credential: LeaseCredential = {
          kind: 'u42.lease.credential',
          handle: { seq: credentialSeq },
        };
        const handle: LeaseHandle = {
          credential,
          plugId,
          version: { major: 1, minor: 0, patch: 0 },
          release: () => {
            releases.push(credential);
            return Promise.resolve('ok');
          },
        };
        minted.push(handle);
        return handle;
      },
      release: () => 'ok',
    },
    calls: {
      call: (_credential, methodName, args) => {
        gatewayCalls.push({ methodName, args });
        return Promise.resolve(programmedResult);
      },
      callById: (_credential, methodId, args) => {
        gatewayCalls.push({ methodName: `#${methodId}`, args });
        return Promise.resolve(programmedResult);
      },
    },
    diag: { report() {} },
  };
  return {
    services,
    published,
    acquisitions,
    releases,
    gatewayCalls,
    minted,
    watcherCount: () => watchers.length,
    deliverCapability: async (event: CapEvent) => {
      for (const sink of [...watchers]) await sink.onCapability(event);
    },
  };
}

/**
 * @brief Availability fact for std.http-client with the given provider version.
 *
 * @param major Provider major version.
 * @param minor Provider minor version.
 * @param patch Provider patch version.
 * @return CapEvent announcing the provider as available.
 */
function httpClientAvailable(major = 1, minor = 0, patch = 0): CapEvent {
  return {
    plugId: 'std.http-client',
    available: true,
    version: { major, minor, patch },
    capabilities: { methods: [{ id: 1, name: 'request' }] },
  };
}

describe('demo plugin http-client-demo', () => {
  it('announces fetch-json and initializes after std.http-client', async () => {
    const fixture = makeDemoFixture(null);
    const demo = createDemo();
    expect(demo.desc.plugId).toBe('demo.http-client');
    expect(demo.desc.init.after).toEqual(['std.http-client']);
    await demo.init(fixture.services);
    await demo.start();
    expect(fixture.published[0]?.methods.map((method) => [method.id, method.name])).toEqual([
      [1, 'fetch-json'],
    ]);
  });

  it('borrows an exact-range lease only for the accepted version', async () => {
    const fixture = makeDemoFixture(null);
    const demo = createDemo();
    await demo.init(fixture.services);
    await demo.start();
    await fixture.deliverCapability(httpClientAvailable(2, 0, 0));
    expect(fixture.acquisitions).toHaveLength(0);
    await fixture.deliverCapability(httpClientAvailable());
    expect(fixture.acquisitions).toHaveLength(1);
    expect(fixture.acquisitions[0]?.plugId).toBe('std.http-client');
    expect(fixture.acquisitions[0]?.range).toEqual({
      minimum: { major: 1, minor: 0, patch: 0 },
      maximum: { major: 1, minor: 0, patch: 0 },
    });
    await fixture.deliverCapability(httpClientAvailable());
    expect(fixture.acquisitions).toHaveLength(1); // a live lease is never re-borrowed
  });

  it('performs one gateway request call per fetch-json invoke', async () => {
    const programmed: JsonObject = {
      status: 200,
      ok: true,
      headers: {},
      json: { demo: true },
    };
    const fixture = makeDemoFixture(programmed);
    const demo = createDemo();
    await demo.init(fixture.services);
    await demo.start();
    await fixture.deliverCapability(httpClientAvailable());
    const result = await demo.invoke?.({
      methodId: 1,
      methodName: 'fetch-json',
      args: { url: 'https://demo.test/x' },
    });
    expect(result).toEqual(programmed);
    expect(fixture.gatewayCalls).toHaveLength(1);
    expect(fixture.gatewayCalls[0]?.methodName).toBe('request');
    expect(fixture.gatewayCalls[0]?.args).toEqual({
      url: 'https://demo.test/x',
      method: 'GET',
      timeoutMs: 8000,
      expectOk: true,
    });
  });

  it('rejects fetch-json without a held lease and validates args', async () => {
    const fixture = makeDemoFixture(null);
    const demo = createDemo();
    await demo.init(fixture.services);
    await demo.start();
    await expectRejected(
      call(demo, 1, 'fetch-json', { url: 'https://demo.test/x' }),
      'invalid_state',
    );
    await expectRejected(call(demo, 1, 'fetch-json', null), 'invalid_argument');
    await expectRejected(call(demo, 1, 'fetch-json', {}), 'invalid_argument');
    await expectRejected(call(demo, 7, 'nope', null), 'not_found');
  });

  it('clears the lease on revocation and stops forwarding', async () => {
    const fixture = makeDemoFixture(null);
    const demo = createDemo();
    await demo.init(fixture.services);
    await demo.start();
    await fixture.deliverCapability(httpClientAvailable());
    const revoker = fixture.acquisitions[0]?.revoker;
    const credential = fixture.minted[0]?.credential;
    expect(credential).toBeDefined();
    await revoker?.onRevoke(credential as LeaseCredential);
    expect(fixture.releases).toEqual([credential]);
    await expectRejected(
      call(demo, 1, 'fetch-json', { url: 'https://demo.test/x' }),
      'invalid_state',
    );
  });

  it('unsubscribes and releases the lease on stop', async () => {
    const fixture = makeDemoFixture(null);
    const demo = createDemo();
    await demo.init(fixture.services);
    await demo.start();
    await fixture.deliverCapability(httpClientAvailable());
    const credential = fixture.minted[0]?.credential;
    await demo.stop();
    expect(fixture.watcherCount()).toBe(0);
    expect(fixture.releases).toEqual([credential]);
  });
});
