/**
 * @file Tests for the std.oauth plugin.
 * @brief Covers authorize-url building (PKCE S256, serialization, randomness), both token
 *        grants, cache hits, the 30s expiry margin with automatic refresh, refresh-failure
 *        cache clearing, endpoint error mapping with secret masking, introspection,
 *        invalidation idempotence, argument validation and the capability shape.
 *
 * @note Every external effect is injected: fetchImpl is an in-memory fake and now() is a fake
 *       clock. No host implementation or other agent module is imported.
 */
import { describe, expect, it } from 'vitest';
import { createHash } from 'node:crypto';
import {
  computeCodeChallenge,
  createWithDeps,
  defaultFetch,
  defaultNow,
} from '../../src/plugins/oauth/index.ts';
import type { FetchLike } from '../../src/plugins/oauth/index.ts';
import { isRackError } from '../../src/core/status.ts';
import type { RackError, RackStatus } from '../../src/core/status.ts';
import type { JsonObject } from '../../src/core/json.ts';
import type {
  CapsDesc,
  JsonValue,
  PluginServices,
  RackPlugin,
  Subscription,
} from '../../src/core/types.ts';

// ---------------------------------------------------------------------------
// RFC 7636 Appendix B reference vector.
// ---------------------------------------------------------------------------

/** @brief Canonical code verifier from RFC 7636 Appendix B. */
const RFC7636_VERIFIER = 'dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk';

/** @brief Canonical S256 code challenge for RFC7636_VERIFIER. */
const RFC7636_CHALLENGE = 'E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM';

// ---------------------------------------------------------------------------
// Self-written fake PluginServices (capability + diagnostics recording only).
// ---------------------------------------------------------------------------

/**
 * @brief Build a fake services bundle that records capability publications and diagnostics.
 *
 * @return The services object plus the recording arrays.
 */
function makeFakeServices(): {
  readonly services: PluginServices;
  readonly publishedCaps: CapsDesc[];
  readonly diagnostics: Array<{ level: string; message: string; detail?: JsonValue }>;
} {
  const publishedCaps: CapsDesc[] = [];
  const diagnostics: Array<{ level: string; message: string; detail?: JsonValue }> = [];
  const services: PluginServices = {
    plugId: 'std.oauth',
    events: {
      subscribe: (): Subscription => ({ unsubscribe() {} }),
      publish() {
        // std.oauth publishes no ordinary events.
      },
    },
    caps: {
      publish(caps: CapsDesc): void {
        publishedCaps.push(caps);
      },
      watch: (): Subscription => ({ unsubscribe() {} }),
      snapshot: () => [],
      acquire: () => {
        throw new Error('not used by std.oauth tests');
      },
      release: () => 'stale',
    },
    calls: {
      call: () => Promise.resolve(null),
      callById: () => Promise.resolve(null),
    },
    diag: {
      report(level: string, message: string, detail?: JsonValue): void {
        diagnostics.push({ level, message, detail });
      },
    },
  };
  return { services, publishedCaps, diagnostics };
}

// ---------------------------------------------------------------------------
// Fake fetch: records every request, answers from an in-memory responder.
// ---------------------------------------------------------------------------

/** @brief One recorded outgoing request. */
interface RecordedRequest {
  readonly url: string;
  readonly body: string;
  readonly headers: Record<string, string>;
}

/** @brief One scripted endpoint answer. */
interface FakeAnswer {
  readonly status: number;
  readonly body: string;
}

/**
 * @brief Build an in-memory fetch implementation.
 *
 * @param responder Maps each recorded request to a scripted answer.
 * @return The fetch-like implementation plus the request log.
 */
function makeFakeFetch(
  responder: (request: RecordedRequest) => FakeAnswer | Promise<FakeAnswer>,
): { readonly fetchImpl: FetchLike; readonly requests: RecordedRequest[] } {
  const requests: RecordedRequest[] = [];
  const fetchImpl: FetchLike = async (input, init) => {
    const request: RecordedRequest = {
      url: String(input),
      body: typeof init?.body === 'string' ? init.body : '',
      headers: init?.headers ?? {},
    };
    requests.push(request);
    const answer = await responder(request);
    return new Response(answer.body, {
      status: answer.status,
      headers: { 'content-type': 'application/json' },
    });
  };
  return { fetchImpl, requests };
}

/**
 * @brief Script a JSON endpoint answer.
 *
 * @param body Value serialized as the response body.
 * @param status HTTP status code; defaults to 200.
 * @return The scripted answer.
 */
function jsonResponse(body: unknown, status = 200): FakeAnswer {
  return { status, body: JSON.stringify(body) };
}

/**
 * @brief Parse a recorded request body as an OAuth form.
 *
 * @param request Recorded request.
 * @return URL-encoded form view of the body.
 */
function formOf(request: RecordedRequest): URLSearchParams {
  return new URLSearchParams(request.body);
}

// ---------------------------------------------------------------------------
// Shared helpers.
// ---------------------------------------------------------------------------

/**
 * @brief Invoke one plugin method through the request shape used by the gateway.
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
  if (plugin.invoke === undefined) throw new Error('plugin does not implement invoke');
  return await plugin.invoke({ methodId, methodName, args });
}

/**
 * @brief Narrow a JSON result to an object.
 *
 * @param value Invoke result.
 * @return The value typed as JsonObject.
 */
function asObject(value: JsonValue): JsonObject {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw new Error(`expected an object result, got ${JSON.stringify(value)}`);
  }
  // The cast is needed because readonly arrays are not narrowed away by Array.isArray.
  return value as JsonObject;
}

/**
 * @brief Assert that a call rejects with the expected RackError status.
 *
 * @param run Callback performing the failing call.
 * @param status Expected RackStatus.
 * @return The caught RackError for further detail assertions.
 */
async function expectFailure(run: () => unknown, status: RackStatus): Promise<RackError> {
  let caught: unknown;
  try {
    await run();
  } catch (error) {
    caught = error;
  }
  expect(caught, 'expected the call to throw').toBeDefined();
  if (!isRackError(caught)) throw new Error('expected a RackError');
  expect(caught.status).toBe(status);
  return caught;
}

/**
 * @brief Assert that none of the given secret strings appears in any container.
 *
 * @param containers Error details, messages or diagnostic records (any shape).
 * @param secrets Secret values that must never be surfaced.
 */
function assertNoSecrets(containers: readonly unknown[], secrets: readonly string[]): void {
  for (const container of containers) {
    const text = typeof container === 'string' ? container : JSON.stringify(container ?? null);
    for (const secret of secrets) {
      if (secret.length > 0) expect(text, `leaked secret in ${text}`).not.toContain(secret);
    }
  }
}

/** @brief Base arguments for a client_credentials token call. */
function ccArgs(overrides: Record<string, string> = {}): JsonObject {
  return {
    grantType: 'client_credentials',
    tokenEndpoint: 'https://auth.example.com/token',
    clientId: 'client-1',
    clientSecret: 'super-secret-value',
    ...overrides,
  };
}

/** @brief Base arguments for an authorize-url call. */
const AUTH_BASE = {
  authorizationEndpoint: 'https://auth.example.com/authorize',
  clientId: 'client-1',
  redirectUri: 'https://app.example.com/cb',
} as const;

// ---------------------------------------------------------------------------
// Capability shape and lifecycle.
// ---------------------------------------------------------------------------

describe('std.oauth capability shape', () => {
  it('publishes the four OAuth methods on start()', async () => {
    const fake = makeFakeServices();
    const plugin = createWithDeps();
    expect(plugin.desc.plugId).toBe('std.oauth');
    expect(plugin.desc.version).toEqual({ major: 1, minor: 0, patch: 0 });
    await plugin.init(fake.services);
    await plugin.start();
    expect(fake.publishedCaps).toHaveLength(1);
    const methods = fake.publishedCaps[0]?.methods ?? [];
    expect(methods.map((method) => [method.id, method.name])).toEqual([
      [1, 'authorize-url'],
      [2, 'token'],
      [3, 'token.invalidate'],
      [4, 'introspect'],
    ]);
    for (const method of methods) {
      expect(typeof method.description).toBe('string');
      expect(method.inputSchema).toBeTypeOf('object');
    }
  });

  it('stop() clears the token cache', async () => {
    const { fetchImpl, requests } = makeFakeFetch(() =>
      jsonResponse({ access_token: 'at-1', token_type: 'Bearer', expires_in: 3600 }),
    );
    const plugin = createWithDeps({ fetchImpl, now: () => 0 });
    await invoke(plugin, 2, 'token', ccArgs());
    await invoke(plugin, 2, 'token', ccArgs()); // served from cache
    expect(requests).toHaveLength(1);
    await plugin.stop();
    await invoke(plugin, 2, 'token', ccArgs());
    expect(requests).toHaveLength(2);
  });
});

// ---------------------------------------------------------------------------
// authorize-url.
// ---------------------------------------------------------------------------

describe('std.oauth authorize-url', () => {
  it('serializes parameters in a fixed order with proper percent-encoding', async () => {
    const plugin = createWithDeps();
    const result = asObject(
      await invoke(plugin, 1, 'authorize-url', {
        authorizationEndpoint: 'https://auth.example.com/authorize',
        clientId: 'client 1',
        redirectUri: 'https://app.example.com/cb?a=1',
        scope: 'read write',
        state: 'st-123',
        pkce: false,
      }),
    );
    expect(result['url']).toBe(
      'https://auth.example.com/authorize?response_type=code&client_id=client%201' +
        '&redirect_uri=https%3A%2F%2Fapp.example.com%2Fcb%3Fa%3D1&scope=read%20write&state=st-123',
    );
    expect(result['state']).toBe('st-123');
    expect('codeVerifier' in result).toBe(false);
    expect('codeChallenge' in result).toBe(false);
  });

  it('derives the S256 challenge for the RFC 7636 fixed verifier vector', () => {
    expect(computeCodeChallenge(RFC7636_VERIFIER)).toBe(RFC7636_CHALLENGE);
  });

  it('uses a supplied codeVerifier and embeds the derived challenge in the URL', async () => {
    const plugin = createWithDeps();
    const result = asObject(
      await invoke(plugin, 1, 'authorize-url', {
        ...AUTH_BASE,
        state: 'st-1',
        codeVerifier: RFC7636_VERIFIER,
      }),
    );
    expect(result['codeVerifier']).toBe(RFC7636_VERIFIER);
    expect(result['codeChallenge']).toBe(RFC7636_CHALLENGE);
    expect(String(result['url'])).toContain(
      `code_challenge=${RFC7636_CHALLENGE}&code_challenge_method=S256`,
    );
  });

  it('generates a random state and PKCE verifier when omitted', async () => {
    const plugin = createWithDeps();
    const first = asObject(await invoke(plugin, 1, 'authorize-url', AUTH_BASE));
    const second = asObject(await invoke(plugin, 1, 'authorize-url', AUTH_BASE));
    const verifier = String(first['codeVerifier']);
    expect(verifier).toMatch(/^[A-Za-z0-9._~-]{43}$/);
    // Independent recomputation with node:crypto must reproduce the plugin's challenge.
    expect(first['codeChallenge']).toBe(createHash('sha256').update(verifier).digest('base64url'));
    expect(String(first['url'])).toContain(`state=${String(first['state'])}`);
    expect(String(first['url'])).toContain('code_challenge_method=S256');
    expect(first['state']).not.toBe(second['state']);
    expect(first['codeVerifier']).not.toBe(second['codeVerifier']);
  });

  it('rejects malformed authorize-url arguments', async () => {
    const plugin = createWithDeps();
    await expectFailure(
      () => invoke(plugin, 1, 'authorize-url', { authorizationEndpoint: AUTH_BASE.authorizationEndpoint }),
      'invalid_argument',
    );
    await expectFailure(() => invoke(plugin, 1, 'authorize-url', 'nope'), 'invalid_argument');
    await expectFailure(
      () => invoke(plugin, 1, 'authorize-url', { ...AUTH_BASE, authorizationEndpoint: 'not a url' }),
      'invalid_argument',
    );
    await expectFailure(
      () => invoke(plugin, 1, 'authorize-url', { ...AUTH_BASE, pkce: 'yes' }),
      'invalid_argument',
    );
    await expectFailure(
      () => invoke(plugin, 1, 'authorize-url', { ...AUTH_BASE, codeVerifier: 'short', pkce: false }),
      'invalid_argument',
    );
    await expectFailure(
      () => invoke(plugin, 1, 'authorize-url', { ...AUTH_BASE, codeVerifier: 'tooshort' }),
      'invalid_argument',
    );
  });
});

// ---------------------------------------------------------------------------
// token: client_credentials grant and caching.
// ---------------------------------------------------------------------------

describe('std.oauth token (client_credentials)', () => {
  it('fetches a token with a form-encoded POST and returns the full access token', async () => {
    const { fetchImpl, requests } = makeFakeFetch(() =>
      jsonResponse({ access_token: 'at-1', token_type: 'Bearer', expires_in: 3600, scope: 'api read' }),
    );
    const plugin = createWithDeps({ fetchImpl, now: () => 0 });
    const result = asObject(await invoke(plugin, 2, 'token', ccArgs()));
    expect(result).toEqual({ accessToken: 'at-1', tokenType: 'Bearer', expiresIn: 3600, scope: 'api read' });
    expect(requests).toHaveLength(1);
    const request = requests[0];
    if (request === undefined) throw new Error('missing recorded request');
    expect(request.url).toBe('https://auth.example.com/token');
    expect(request.headers['content-type']).toBe('application/x-www-form-urlencoded');
    const form = formOf(request);
    expect(form.get('grant_type')).toBe('client_credentials');
    expect(form.get('client_id')).toBe('client-1');
    expect(form.get('client_secret')).toBe('super-secret-value');
    expect(form.get('scope')).toBe(null);
  });

  it('falls back to the requested scope when the response omits scope', async () => {
    const { fetchImpl } = makeFakeFetch(() =>
      jsonResponse({ access_token: 'at-1', token_type: 'Bearer', expires_in: 3600 }),
    );
    const plugin = createWithDeps({ fetchImpl, now: () => 0 });
    const result = asObject(await invoke(plugin, 2, 'token', ccArgs({ scope: 'api' })));
    expect(result['scope']).toBe('api');
  });

  it('serves a second identical call from the cache with cached:true', async () => {
    const { fetchImpl, requests } = makeFakeFetch(() =>
      jsonResponse({ access_token: 'at-1', token_type: 'Bearer', expires_in: 3600 }),
    );
    const plugin = createWithDeps({ fetchImpl, now: () => 0 });
    await invoke(plugin, 2, 'token', ccArgs());
    // Second call dispatched by numeric id to prove id/name gateway parity.
    const second = asObject(await invoke(plugin, 2, 'token', ccArgs()));
    expect(second['cached']).toBe(true);
    expect(second['accessToken']).toBe('at-1');
    expect(requests).toHaveLength(1);
  });

  it('caches per tokenEndpoint+clientId+scope combination', async () => {
    const { fetchImpl, requests } = makeFakeFetch(() =>
      jsonResponse({ access_token: 'at-1', token_type: 'Bearer', expires_in: 3600 }),
    );
    const plugin = createWithDeps({ fetchImpl, now: () => 0 });
    await invoke(plugin, 2, 'token', ccArgs());
    const otherScope = asObject(await invoke(plugin, 2, 'token', ccArgs({ scope: 'admin' })));
    expect(otherScope['cached']).toBeUndefined();
    expect(requests).toHaveLength(2);
  });
});

// ---------------------------------------------------------------------------
// token: expiry margin, automatic refresh and refresh failures.
// ---------------------------------------------------------------------------

describe('std.oauth token expiry and refresh', () => {
  it('keeps serving from cache until 30s before the absolute expiry', async () => {
    let clockMs = 0;
    const { fetchImpl, requests } = makeFakeFetch(() =>
      jsonResponse({ access_token: 'at-1', token_type: 'Bearer', expires_in: 100, refresh_token: 'rt-1' }),
    );
    const plugin = createWithDeps({ fetchImpl, now: () => clockMs });
    await invoke(plugin, 2, 'token', ccArgs());
    clockMs = 69_000; // 31s of validity remain: above the 30s margin.
    const hit = asObject(await invoke(plugin, 2, 'token', ccArgs()));
    expect(hit['cached']).toBe(true);
    expect(requests).toHaveLength(1);
  });

  it('auto-refreshes via the refresh_token grant once expiry is within 30s', async () => {
    let clockMs = 0;
    const { fetchImpl, requests } = makeFakeFetch((request) => {
      if (formOf(request).get('grant_type') === 'refresh_token') {
        return jsonResponse({ access_token: 'at-2', token_type: 'Bearer', expires_in: 100 });
      }
      return jsonResponse({ access_token: 'at-1', token_type: 'Bearer', expires_in: 100, refresh_token: 'rt-1' });
    });
    const plugin = createWithDeps({ fetchImpl, now: () => clockMs });
    expect(asObject(await invoke(plugin, 2, 'token', ccArgs()))['accessToken']).toBe('at-1');
    clockMs = 71_000; // 29s remain: treated as expired.
    const refreshed = asObject(await invoke(plugin, 2, 'token', ccArgs()));
    expect(refreshed['accessToken']).toBe('at-2');
    expect(refreshed['cached']).toBeUndefined();
    expect(requests).toHaveLength(2);
    const refreshForm = formOf(requests[1] ?? failTest('missing refresh request'));
    expect(refreshForm.get('grant_type')).toBe('refresh_token');
    expect(refreshForm.get('refresh_token')).toBe('rt-1');
    expect(refreshForm.get('client_id')).toBe('client-1');
    expect(refreshForm.get('client_secret')).toBe('super-secret-value');
    // Without further clock movement the refreshed entry is served from cache.
    const third = asObject(await invoke(plugin, 2, 'token', ccArgs()));
    expect(third['cached']).toBe(true);
    expect(third['accessToken']).toBe('at-2');
  });

  it('refreshes against refreshEndpoint when provided', async () => {
    let clockMs = 0;
    const { fetchImpl, requests } = makeFakeFetch((request) => {
      if (request.url === 'https://auth.example.com/refresh') {
        return jsonResponse({ access_token: 'at-2', token_type: 'Bearer', expires_in: 100 });
      }
      return jsonResponse({ access_token: 'at-1', token_type: 'Bearer', expires_in: 100, refresh_token: 'rt-1' });
    });
    const plugin = createWithDeps({ fetchImpl, now: () => clockMs });
    const args = ccArgs({ refreshEndpoint: 'https://auth.example.com/refresh' });
    await invoke(plugin, 2, 'token', args);
    clockMs = 71_000;
    expect(asObject(await invoke(plugin, 2, 'token', args))['accessToken']).toBe('at-2');
    expect(requests[1]?.url).toBe('https://auth.example.com/refresh');
  });

  it('clears the cache and throws failed when the refresh grant is rejected', async () => {
    let clockMs = 0;
    const { fetchImpl, requests } = makeFakeFetch((request) => {
      if (formOf(request).get('grant_type') === 'refresh_token') {
        return jsonResponse({ error: 'invalid_grant' }, 400);
      }
      return jsonResponse({ access_token: 'at-1', token_type: 'Bearer', expires_in: 100, refresh_token: 'rt-1' });
    });
    const plugin = createWithDeps({ fetchImpl, now: () => clockMs });
    await invoke(plugin, 2, 'token', ccArgs());
    clockMs = 71_000;
    const error = await expectFailure(() => invoke(plugin, 2, 'token', ccArgs()), 'failed');
    const detail = asObject((error.detail ?? null) as JsonValue);
    expect(detail['phase']).toBe('refresh');
    expect(detail['httpStatus']).toBe(400);
    expect(detail['oauthError']).toBe('invalid_grant');
    assertNoSecrets([error.detail, error.message], ['super-secret-value', 'rt-1', 'at-1']);
    // The cache entry was dropped: the next call runs a fresh client_credentials grant.
    const fresh = asObject(await invoke(plugin, 2, 'token', ccArgs()));
    expect(fresh['accessToken']).toBe('at-1');
    expect(requests).toHaveLength(3);
    expect(formOf(requests[2] ?? failTest('missing third request')).get('grant_type')).toBe('client_credentials');
  });
});

// ---------------------------------------------------------------------------
// token: authorization_code grant.
// ---------------------------------------------------------------------------

describe('std.oauth token (authorization_code)', () => {
  it('sends code, redirect_uri and code_verifier for the authorization_code grant', async () => {
    const { fetchImpl, requests } = makeFakeFetch(() =>
      jsonResponse({ access_token: 'at-code', token_type: 'Bearer', expires_in: 3600 }),
    );
    const plugin = createWithDeps({ fetchImpl, now: () => 0 });
    const result = asObject(
      await invoke(plugin, 2, 'token', {
        grantType: 'authorization_code',
        tokenEndpoint: 'https://auth.example.com/token',
        clientId: 'client-1',
        clientSecret: 'super-secret-value',
        code: 'the-auth-code',
        redirectUri: 'https://app.example.com/cb',
        pkce: { codeVerifier: RFC7636_VERIFIER },
      }),
    );
    expect(result).toEqual({ accessToken: 'at-code', tokenType: 'Bearer', expiresIn: 3600 });
    const form = formOf(requests[0] ?? failTest('missing request'));
    expect(form.get('grant_type')).toBe('authorization_code');
    expect(form.get('code')).toBe('the-auth-code');
    expect(form.get('code_verifier')).toBe(RFC7636_VERIFIER);
    expect(form.get('redirect_uri')).toBe('https://app.example.com/cb');
    expect(form.get('client_id')).toBe('client-1');
    expect(form.get('client_secret')).toBe('super-secret-value');
  });

  it('omits code_verifier when no pkce object is given', async () => {
    const { fetchImpl, requests } = makeFakeFetch(() =>
      jsonResponse({ access_token: 'at-code', token_type: 'Bearer', expires_in: 3600 }),
    );
    const plugin = createWithDeps({ fetchImpl, now: () => 0 });
    await invoke(plugin, 2, 'token', {
      grantType: 'authorization_code',
      tokenEndpoint: 'https://auth.example.com/token',
      clientId: 'client-1',
      code: 'the-auth-code',
    });
    expect(formOf(requests[0] ?? failTest('missing request')).get('code_verifier')).toBe(null);
  });
});

// ---------------------------------------------------------------------------
// token: endpoint error mapping and masking.
// ---------------------------------------------------------------------------

describe('std.oauth token error mapping', () => {
  it('maps a 4xx endpoint response to failed with httpStatus and oauthError, no secrets', async () => {
    const fake = makeFakeServices();
    const { fetchImpl } = makeFakeFetch(() =>
      jsonResponse({ error: 'invalid_client', error_description: 'bad credentials' }, 401),
    );
    const plugin = createWithDeps({ fetchImpl, now: () => 0 });
    await plugin.init(fake.services);
    const error = await expectFailure(() => invoke(plugin, 2, 'token', ccArgs()), 'failed');
    const detail = asObject((error.detail ?? null) as JsonValue);
    expect(detail['phase']).toBe('token');
    expect(detail['httpStatus']).toBe(401);
    expect(detail['oauthError']).toBe('invalid_client');
    // Red line: neither the client secret nor any token value may surface anywhere.
    assertNoSecrets([error.detail, error.message, ...fake.diagnostics], ['super-secret-value']);
    expect(fake.diagnostics).toHaveLength(1);
    expect(fake.diagnostics[0]?.level).toBe('warn');
  });

  it('maps a 5xx non-JSON response to failed without oauthError', async () => {
    const { fetchImpl } = makeFakeFetch(() => ({
      status: 500,
      body: '<html>Internal Server Error</html>',
    }));
    const plugin = createWithDeps({ fetchImpl, now: () => 0 });
    const error = await expectFailure(() => invoke(plugin, 2, 'token', ccArgs()), 'failed');
    const detail = asObject((error.detail ?? null) as JsonValue);
    expect(detail['httpStatus']).toBe(500);
    expect(detail['oauthError']).toBeUndefined();
    expect(error.message).toContain('HTTP 500');
    assertNoSecrets([error.detail, error.message], ['super-secret-value']);
  });

  it('fails when a 2xx token response lacks access_token', async () => {
    const { fetchImpl } = makeFakeFetch(() => jsonResponse({ token_type: 'Bearer', expires_in: 3600 }));
    const plugin = createWithDeps({ fetchImpl, now: () => 0 });
    const error = await expectFailure(() => invoke(plugin, 2, 'token', ccArgs()), 'failed');
    expect(asObject((error.detail ?? null) as JsonValue)['reason']).toBe('missing_access_token');
    assertNoSecrets([error.detail, error.message], ['super-secret-value']);
  });

  it('wraps network failures as failed with reason network_error', async () => {
    const fetchImpl: FetchLike = async () => {
      throw new TypeError('fetch failed');
    };
    const plugin = createWithDeps({ fetchImpl, now: () => 0 });
    const error = await expectFailure(() => invoke(plugin, 2, 'token', ccArgs()), 'failed');
    expect(asObject((error.detail ?? null) as JsonValue)['reason']).toBe('network_error');
    assertNoSecrets([error.detail, error.message], ['super-secret-value']);
  });
});

// ---------------------------------------------------------------------------
// introspect.
// ---------------------------------------------------------------------------

describe('std.oauth introspect', () => {
  it('returns the raw endpoint JSON and posts the introspection form', async () => {
    const { fetchImpl, requests } = makeFakeFetch(() =>
      jsonResponse({ active: true, scope: 'api', username: 'alice' }),
    );
    const plugin = createWithDeps({ fetchImpl, now: () => 0 });
    const result = await invoke(plugin, 4, 'introspect', {
      introspectionEndpoint: 'https://auth.example.com/introspect',
      token: 'the-access-token',
      clientId: 'client-1',
      clientSecret: 'super-secret-value',
    });
    expect(result).toEqual({ active: true, scope: 'api', username: 'alice' });
    const request = requests[0] ?? failTest('missing request');
    expect(request.url).toBe('https://auth.example.com/introspect');
    const form = formOf(request);
    expect(form.get('token')).toBe('the-access-token');
    expect(form.get('client_id')).toBe('client-1');
    expect(form.get('client_secret')).toBe('super-secret-value');
  });

  it('fails with masked details when the endpoint rejects introspection', async () => {
    const { fetchImpl } = makeFakeFetch(() => jsonResponse({ error: 'invalid_request' }, 400));
    const plugin = createWithDeps({ fetchImpl, now: () => 0 });
    const error = await expectFailure(
      () =>
        invoke(plugin, 4, 'introspect', {
          introspectionEndpoint: 'https://auth.example.com/introspect',
          token: 'the-access-token',
          clientId: 'client-1',
        }),
      'failed',
    );
    const detail = asObject((error.detail ?? null) as JsonValue);
    expect(detail['httpStatus']).toBe(400);
    expect(detail['oauthError']).toBe('invalid_request');
    assertNoSecrets([error.detail, error.message], ['the-access-token']);
  });

  it('rejects introspection without a token', async () => {
    const plugin = createWithDeps();
    await expectFailure(
      () =>
        invoke(plugin, 4, 'introspect', {
          introspectionEndpoint: 'https://auth.example.com/introspect',
          clientId: 'client-1',
        }),
      'invalid_argument',
    );
  });
});

// ---------------------------------------------------------------------------
// token.invalidate.
// ---------------------------------------------------------------------------

describe('std.oauth token.invalidate', () => {
  it('invalidates cache entries idempotently, including unknown keys', async () => {
    const { fetchImpl, requests } = makeFakeFetch(() =>
      jsonResponse({ access_token: 'at-1', token_type: 'Bearer', expires_in: 3600 }),
    );
    const plugin = createWithDeps({ fetchImpl, now: () => 0 });
    expect(await invoke(plugin, 3, 'token.invalidate', { cacheKey: 'never-cached' })).toBeNull();
    const args = ccArgs({ cacheKey: 'my-key' });
    await invoke(plugin, 2, 'token', args);
    await invoke(plugin, 2, 'token', args); // cached
    expect(requests).toHaveLength(1);
    expect(await invoke(plugin, 3, 'token.invalidate', { cacheKey: 'my-key' })).toBeNull();
    expect(await invoke(plugin, 3, 'token.invalidate', { cacheKey: 'my-key' })).toBeNull();
    const after = asObject(await invoke(plugin, 2, 'token', args));
    expect(after['cached']).toBeUndefined();
    expect(requests).toHaveLength(2);
  });

  it('rejects invalidation without a cacheKey', async () => {
    const plugin = createWithDeps();
    await expectFailure(() => invoke(plugin, 3, 'token.invalidate', {}), 'invalid_argument');
  });
});

// ---------------------------------------------------------------------------
// Argument validation and unknown methods.
// ---------------------------------------------------------------------------

describe('std.oauth argument validation', () => {
  it('rejects malformed token arguments and invalid grant combinations', async () => {
    const { fetchImpl } = makeFakeFetch(() =>
      jsonResponse({ access_token: 'at-1', token_type: 'Bearer', expires_in: 3600 }),
    );
    const plugin = createWithDeps({ fetchImpl, now: () => 0 });
    await expectFailure(() => invoke(plugin, 2, 'token', 'nope'), 'invalid_argument');
    await expectFailure(
      () => invoke(plugin, 2, 'token', { tokenEndpoint: 'https://auth.example.com/token', clientId: 'client-1' }),
      'invalid_argument',
    );
    await expectFailure(
      () => invoke(plugin, 2, 'token', { ...ccArgs(), tokenEndpoint: '' }),
      'invalid_argument',
    );
    await expectFailure(() => invoke(plugin, 2, 'token', ccArgs({ grantType: 'password' })), 'invalid_argument');
    await expectFailure(
      () =>
        invoke(plugin, 2, 'token', {
          grantType: 'authorization_code',
          tokenEndpoint: 'https://auth.example.com/token',
          clientId: 'client-1',
        }),
      'invalid_argument',
    );
    await expectFailure(() => invoke(plugin, 2, 'token', ccArgs({ code: 'x' })), 'invalid_argument');
    await expectFailure(
      () => invoke(plugin, 2, 'token', ccArgs({ pkce: 'x' })),
      'invalid_argument',
    );
    await expectFailure(
      () => invoke(plugin, 2, 'token', { ...ccArgs(), pkce: { codeVerifier: RFC7636_VERIFIER } }),
      'invalid_argument',
    );
  });

  it('rejects unknown methods with not_found', async () => {
    const plugin = createWithDeps();
    await expectFailure(() => invoke(plugin, 99, 'revoke', {}), 'not_found');
  });
});

// ---------------------------------------------------------------------------
// Exported dependency defaults.
// ---------------------------------------------------------------------------

describe('std.oauth dependency injection defaults', () => {
  it('exports the real global fetch and wall clock as defaults', () => {
    expect(defaultFetch).toBe(fetch);
    expect(Math.abs(defaultNow() - Date.now())).toBeLessThan(5_000);
  });
});

/**
 * @brief Helper that always throws; used to narrow optional request-log entries.
 *
 * @param message Failure description.
 * @return Never returns.
 */
function failTest(message: string): never {
  throw new Error(message);
}
