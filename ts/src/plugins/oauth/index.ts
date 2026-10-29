/**
 * @file std.oauth plugin implementation.
 * @brief OAuth 2.0 standard device for the 42U rack: authorization URL building with PKCE S256,
 *        token grants (client_credentials / authorization_code), token caching with proactive
 *        refresh, cache invalidation and token introspection.
 *
 * @details Zero npm dependencies: randomness and SHA-256 come from node:crypto, HTTP runs on an
 *          injectable fetch implementation. Hosts and tests inject both external effects through
 *          createWithDeps({ fetchImpl, now }); the real defaults (global fetch, Date.now) are
 *          exported as defaultFetch / defaultNow. Security red line: client secrets, access
 *          tokens and refresh tokens never appear in diagnostics, error details or log
 *          parameters; anywhere a secret would be shown it is masked as '***'.
 */

import { createHash, randomBytes } from 'node:crypto';
import { definePlugin, isJsonValue, RackError } from '../../sdk/index.ts';
import type {
  InvokeRequest,
  JsonObject,
  JsonValue,
  MethodDesc,
  PluginServices,
  RackPlugin,
} from '../../sdk/index.ts';

/** @brief Canonical plugin identity announced to the host. */
const OAUTH_PLUG_ID = 'std.oauth';

/** @brief Business version published by this provider. */
const OAUTH_VERSION = { major: 1, minor: 0, patch: 0 } as const;

// ---------------------------------------------------------------------------
// Method ids and names (both selectors resolve identically).
// ---------------------------------------------------------------------------

const AUTHORIZE_URL_ID = 1;
const TOKEN_ID = 2;
const INVALIDATE_ID = 3;
const INTROSPECT_ID = 4;

const AUTHORIZE_URL_NAME = 'authorize-url';
const TOKEN_NAME = 'token';
const INVALIDATE_NAME = 'token.invalidate';
const INTROSPECT_NAME = 'introspect';

/** @brief Methods announced through the capability service during start(). */
const OAUTH_METHODS: readonly MethodDesc[] = [
  {
    id: AUTHORIZE_URL_ID,
    name: AUTHORIZE_URL_NAME,
    description: 'Build an authorization endpoint URL with response_type=code and optional PKCE S256.',
    inputSchema: {
      type: 'object',
      required: ['authorizationEndpoint', 'clientId', 'redirectUri'],
      properties: {
        authorizationEndpoint: { type: 'string' },
        clientId: { type: 'string' },
        redirectUri: { type: 'string' },
        scope: { type: 'string' },
        state: { type: 'string' },
        codeVerifier: { type: 'string' },
        pkce: { type: 'boolean', default: true },
      },
    },
    outputSchema: {
      type: 'object',
      required: ['url', 'state'],
      properties: {
        url: { type: 'string' },
        state: { type: 'string' },
        codeVerifier: { type: 'string' },
        codeChallenge: { type: 'string' },
      },
    },
  },
  {
    id: TOKEN_ID,
    name: TOKEN_NAME,
    description: 'Run a client_credentials or authorization_code grant with caching and auto refresh.',
    inputSchema: {
      type: 'object',
      required: ['grantType', 'tokenEndpoint', 'clientId'],
      properties: {
        grantType: { enum: ['client_credentials', 'authorization_code'] },
        tokenEndpoint: { type: 'string' },
        clientId: { type: 'string' },
        clientSecret: { type: 'string' },
        code: { type: 'string' },
        redirectUri: { type: 'string' },
        scope: { type: 'string' },
        pkce: {
          type: 'object',
          required: ['codeVerifier'],
          properties: { codeVerifier: { type: 'string' } },
        },
        refreshEndpoint: { type: 'string' },
        cacheKey: { type: 'string' },
      },
    },
    outputSchema: {
      type: 'object',
      required: ['accessToken', 'tokenType', 'expiresIn'],
      properties: {
        accessToken: { type: 'string' },
        tokenType: { type: 'string' },
        expiresIn: { type: 'number' },
        scope: { type: 'string' },
        cached: { type: 'boolean' },
      },
    },
  },
  {
    id: INVALIDATE_ID,
    name: INVALIDATE_NAME,
    description: 'Drop one cached token entry; unknown keys succeed idempotently.',
    inputSchema: {
      type: 'object',
      required: ['cacheKey'],
      properties: { cacheKey: { type: 'string' } },
    },
    outputSchema: { type: 'null' },
  },
  {
    id: INTROSPECT_ID,
    name: INTROSPECT_NAME,
    description: 'POST a token to an introspection endpoint and return the raw JSON response.',
    inputSchema: {
      type: 'object',
      required: ['introspectionEndpoint', 'token', 'clientId'],
      properties: {
        introspectionEndpoint: { type: 'string' },
        token: { type: 'string' },
        clientId: { type: 'string' },
        clientSecret: { type: 'string' },
      },
    },
    outputSchema: {},
  },
];

// ---------------------------------------------------------------------------
// Injectable effect types and real defaults (both exported).
// ---------------------------------------------------------------------------

/** @brief Minimal structural subset of fetch init accepted from hosts and tests. */
export interface FetchInitLike {
  readonly method?: string;
  readonly headers?: Record<string, string>;
  readonly body?: string;
}

/** @brief Minimal structural subset of a fetch response consumed by this plugin. */
export interface FetchResponseLike {
  readonly ok: boolean;
  readonly status: number;
  text(): Promise<string>;
}

/**
 * @brief Injectable HTTP effect; the real global fetch satisfies this shape.
 * @note Kept structural so tests can supply in-memory fakes without undici types.
 */
export type FetchLike = (input: string | URL, init?: FetchInitLike) => Promise<FetchResponseLike>;

/** @brief Injectable clock returning epoch milliseconds. */
export type NowFn = () => number;

/** @brief Real default HTTP effect: the global fetch. */
export const defaultFetch: FetchLike = fetch;

/** @brief Real default clock: the wall clock in epoch milliseconds. */
export const defaultNow: NowFn = () => Date.now();

/** @brief Dependencies injectable through createWithDeps; every entry is optional. */
export interface OAuthDeps {
  readonly fetchImpl?: FetchLike;
  readonly now?: NowFn;
}

// ---------------------------------------------------------------------------
// Tunables.
// ---------------------------------------------------------------------------

/** @brief Tokens expiring within this margin are treated as already expired. */
const EXPIRY_MARGIN_MS = 30_000;

/** @brief Random PKCE verifier entropy: 32 bytes encode to the RFC minimum 43 chars. */
const PKCE_VERIFIER_BYTES = 32;

/** @brief Random state entropy: 16 bytes encode to 22 base64url characters. */
const STATE_BYTES = 16;

/** @brief Replacement shown anywhere a secret would otherwise be surfaced. */
const MASKED_SECRET = '***';

/** @brief RFC 7636 unreserved-character, 43..128 char verifier grammar. */
const PKCE_VERIFIER_PATTERN = /^[A-Za-z0-9._~-]{43,128}$/;

// ---------------------------------------------------------------------------
// Crypto and encoding helpers.
// ---------------------------------------------------------------------------

/**
 * @brief Base64url-encode raw bytes without padding.
 *
 * @param bytes Raw bytes to encode.
 * @return Unpadded base64url string.
 */
function base64url(bytes: Uint8Array): string {
  return Buffer.from(bytes).toString('base64url');
}

/**
 * @brief Compute the PKCE S256 code challenge for a verifier.
 *
 * @param verifier RFC 7636 code verifier (43..128 unreserved characters).
 * @return BASE64URL(SHA256(ASCII(verifier))), unpadded.
 * @note Exported so consumers and tests can cross-check challenges independently.
 */
export function computeCodeChallenge(verifier: string): string {
  return base64url(createHash('sha256').update(verifier, 'ascii').digest());
}

/**
 * @brief Generate a cryptographically random base64url token.
 *
 * @param bytes Entropy in bytes.
 * @return Unpadded base64url string of length ceil(bytes * 4 / 3).
 */
function randomUrlToken(bytes: number): string {
  return base64url(randomBytes(bytes));
}

// ---------------------------------------------------------------------------
// Argument parsing helpers.
// ---------------------------------------------------------------------------

/**
 * @brief Narrow invoke args to a JSON object.
 *
 * @param value Candidate request args document.
 * @param method Method name used in the error message.
 * @return The same value typed as JsonObject.
 * @throws RackError 'invalid_argument' when the value is not a JSON object.
 */
function argsObject(value: JsonValue, method: string): JsonObject {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw new RackError('invalid_argument', `${method} args must be a JSON object`);
  }
  // The cast is needed because readonly arrays are not narrowed away by Array.isArray.
  return value as JsonObject;
}

/**
 * @brief Narrow any JSON value to a JSON object.
 *
 * @param value Candidate value; undefined is tolerated for absent bodies.
 * @return The object, or undefined for non-objects (including null and arrays).
 */
function asJsonObject(value: JsonValue | undefined): JsonObject | undefined {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    return undefined;
  }
  // The cast is needed because readonly arrays are not narrowed away by Array.isArray.
  return value as JsonObject;
}

/**
 * @brief Read a mandatory non-empty string field.
 *
 * @param args Args document already narrowed to an object.
 * @param key Field name (used verbatim in the error message).
 * @return The validated string.
 * @throws RackError 'invalid_argument' when the field is missing, not a string or empty.
 */
function reqString(args: JsonObject, key: string): string {
  const value = args[key];
  if (typeof value !== 'string' || value.length === 0) {
    throw new RackError('invalid_argument', `args.${key} must be a non-empty string`);
  }
  return value;
}

/**
 * @brief Read an optional string field.
 *
 * @param args Args document already narrowed to an object.
 * @param key Field name (used verbatim in the error message).
 * @return The string, or undefined when absent.
 * @throws RackError 'invalid_argument' when the field is present but not a string.
 */
function optString(args: JsonObject, key: string): string | undefined {
  const value = args[key];
  if (value === undefined) return undefined;
  if (typeof value !== 'string') {
    throw new RackError('invalid_argument', `args.${key} must be a string when present`);
  }
  return value;
}

/**
 * @brief Read an optional boolean field.
 *
 * @param args Args document already narrowed to an object.
 * @param key Field name (used verbatim in the error message).
 * @return The boolean, or undefined when absent.
 * @throws RackError 'invalid_argument' when the field is present but not a boolean.
 */
function optBoolean(args: JsonObject, key: string): boolean | undefined {
  const value = args[key];
  if (value === undefined) return undefined;
  if (typeof value !== 'boolean') {
    throw new RackError('invalid_argument', `args.${key} must be a boolean when present`);
  }
  return value;
}

// ---------------------------------------------------------------------------
// URL building.
// ---------------------------------------------------------------------------

/**
 * @brief Percent-encode one query parameter pair with %20 spaces (not '+').
 *
 * @param name Raw parameter name.
 * @param value Raw parameter value.
 * @return Encoded "name=value" pair.
 */
function encodePair(name: string, value: string): string {
  return `${name}=${encodeURIComponent(value)}`;
}

/**
 * @brief Append OAuth parameters to a URL, preserving any existing query string.
 *
 * @param url Parsed target URL; mutated in place.
 * @param params Ordered parameter pairs to append.
 */
function appendQuery(url: URL, params: readonly (readonly [string, string])[]): void {
  const existing = url.search.replace(/^\?/, '');
  const extra = params.map(([name, value]) => encodePair(name, value)).join('&');
  url.search = existing.length > 0 ? `${existing}&${extra}` : extra;
}

// ---------------------------------------------------------------------------
// Token cache entry.
// ---------------------------------------------------------------------------

/**
 * @brief Internal cached token record.
 *
 * @note expiresAt is an absolute epoch-millisecond deadline computed with the injected now();
 *       the refresh token is retained only in memory to enable automatic refresh and is cleared
 *       on stop(), invalidation and failed refreshes.
 */
interface TokenCacheEntry {
  readonly accessToken: string;
  readonly tokenType: string;
  readonly expiresIn: number;
  readonly scope: string | undefined;
  readonly expiresAt: number;
  readonly refreshToken: string | undefined;
  readonly tokenEndpoint: string;
  readonly refreshEndpoint: string | undefined;
  readonly clientId: string;
}

/**
 * @brief Derive the default cache key from grant identity components.
 *
 * @param tokenEndpoint Token endpoint URL.
 * @param clientId Client identity.
 * @param scope Requested scope string, if any.
 * @return A deterministic JSON-encoded key.
 */
function deriveCacheKey(tokenEndpoint: string, clientId: string, scope: string | undefined): string {
  return JSON.stringify([tokenEndpoint, clientId, scope ?? '']);
}

// ---------------------------------------------------------------------------
// Plugin factory.
// ---------------------------------------------------------------------------

/**
 * @brief Create one std.oauth provider instance with injectable effects.
 *
 * @param deps Optional fetch implementation and clock; omitted entries fall back to the real
 *        global fetch and wall clock.
 * @return A RackPlugin publishing authorize-url / token / token.invalidate / introspect.
 * @note Each call yields an independent instance with its own private token cache; stop()
 *       clears that cache. Errors thrown across the boundary are always RackError.
 */
export function createWithDeps(deps: OAuthDeps = {}): RackPlugin {
  const fetchImpl = deps.fetchImpl ?? defaultFetch;
  const now = deps.now ?? defaultNow;
  const cache = new Map<string, TokenCacheEntry>();
  let services: PluginServices | null = null;

  /**
   * @brief POST one application/x-www-form-urlencoded request and parse the JSON reply.
   *
   * @param endpoint Absolute endpoint URL.
   * @param form Ordered form fields (secrets travel only here, never into errors).
   * @param phase Flow tag used in messages and details ('token' | 'refresh' | 'introspect').
   * @return Response status plus the parsed JSON body when it is valid JSON.
   * @throws RackError 'failed' with detail {phase, reason:'network_error'} when fetch rejects,
   *         or with detail {phase, httpStatus, oauthError?} on non-2xx responses. Details never
   *         contain the request form, so secrets cannot leak.
   */
  async function postForm(
    endpoint: string,
    form: Record<string, string>,
    phase: 'token' | 'refresh' | 'introspect',
  ): Promise<{ readonly status: number; readonly json: JsonValue | undefined }> {
    let response: FetchResponseLike;
    try {
      response = await fetchImpl(endpoint, {
        method: 'POST',
        headers: { 'content-type': 'application/x-www-form-urlencoded', accept: 'application/json' },
        body: new URLSearchParams(form).toString(),
      });
    } catch (error) {
      const cause = error instanceof Error ? error.message : 'unknown error';
      throw new RackError('failed', `oauth ${phase} request to ${endpoint} failed: ${cause}`, {
        phase,
        reason: 'network_error',
      });
    }
    const text = await response.text();
    let json: JsonValue | undefined;
    if (text.trim().length > 0) {
      try {
        const parsed: unknown = JSON.parse(text);
        if (isJsonValue(parsed)) json = parsed;
      } catch {
        json = undefined;
      }
    }
    if (!response.ok) {
      const body = asJsonObject(json);
      const oauthError =
        body !== undefined && typeof body['error'] === 'string' ? body['error'] : undefined;
      const detail: JsonObject = {
        phase,
        httpStatus: response.status,
        ...(oauthError !== undefined ? { oauthError } : {}),
      };
      services?.diag.report('warn', `std.oauth ${phase} request failed with HTTP ${response.status}`, detail);
      throw new RackError('failed', `oauth ${phase} endpoint returned HTTP ${response.status}`, detail);
    }
    return { status: response.status, json };
  }

  /**
   * @brief Turn a successful endpoint reply into a cache entry.
   *
   * @param reply Parsed endpoint reply from postForm.
   * @param ctx Grant context: phase tag, scope/refreshToken fallbacks and cache identity.
   * @return A fully populated cache entry with an absolute expiry deadline.
   * @throws RackError 'failed' with reason 'invalid_response' or 'missing_access_token' when the
   *         body is not a JSON object or lacks a usable access_token.
   */
  function parseTokenReply(
    reply: { readonly status: number; readonly json: JsonValue | undefined },
    ctx: {
      readonly phase: 'token' | 'refresh';
      readonly fallbackScope: string | undefined;
      readonly fallbackRefreshToken: string | undefined;
      readonly tokenEndpoint: string;
      readonly refreshEndpoint: string | undefined;
      readonly clientId: string;
    },
  ): TokenCacheEntry {
    const body = asJsonObject(reply.json);
    if (body === undefined) {
      throw new RackError('failed', `oauth ${ctx.phase} endpoint returned a non-JSON object body`, {
        phase: ctx.phase,
        httpStatus: reply.status,
        reason: 'invalid_response',
      });
    }
    const accessToken = body['access_token'];
    if (typeof accessToken !== 'string' || accessToken.length === 0) {
      throw new RackError('failed', `oauth ${ctx.phase} endpoint response lacks access_token`, {
        phase: ctx.phase,
        httpStatus: reply.status,
        reason: 'missing_access_token',
      });
    }
    const expiresInRaw = body['expires_in'];
    const expiresIn =
      typeof expiresInRaw === 'number' && Number.isFinite(expiresInRaw) && expiresInRaw > 0
        ? expiresInRaw
        : 0;
    const tokenTypeRaw = body['token_type'];
    const tokenType =
      typeof tokenTypeRaw === 'string' && tokenTypeRaw.length > 0 ? tokenTypeRaw : 'Bearer';
    const scopeRaw = body['scope'];
    const scope = typeof scopeRaw === 'string' ? scopeRaw : ctx.fallbackScope;
    const refreshTokenRaw = body['refresh_token'];
    const refreshToken =
      typeof refreshTokenRaw === 'string' && refreshTokenRaw.length > 0
        ? refreshTokenRaw
        : ctx.fallbackRefreshToken;
    return {
      accessToken,
      tokenType,
      expiresIn,
      scope,
      refreshToken,
      expiresAt: now() + expiresIn * 1000,
      tokenEndpoint: ctx.tokenEndpoint,
      refreshEndpoint: ctx.refreshEndpoint,
      clientId: ctx.clientId,
    };
  }

  /**
   * @brief Shape one token reply as the public JSON result document.
   *
   * @param entry Cache entry to project.
   * @param fromCache Whether the entry was served from the cache without network I/O.
   * @return {accessToken, tokenType, expiresIn, scope?, cached?}; accessToken is returned in
   *         full because the caller is the token owner.
   */
  function tokenResult(entry: TokenCacheEntry, fromCache: boolean): JsonObject {
    return {
      accessToken: entry.accessToken,
      tokenType: entry.tokenType,
      expiresIn: entry.expiresIn,
      ...(entry.scope !== undefined ? { scope: entry.scope } : {}),
      ...(fromCache ? { cached: true } : {}),
    };
  }

  /**
   * @brief Handle the authorize-url method.
   *
   * @param rawArgs Request args document.
   * @return {url, state, codeVerifier?, codeChallenge?}; PKCE fields appear only with pkce:true
   *         and the codeVerifier is handed out exactly here, never cached or logged.
   * @throws RackError 'invalid_argument' for malformed or contradictory arguments.
   */
  function authorizeUrl(rawArgs: JsonValue): JsonObject {
    const args = argsObject(rawArgs, AUTHORIZE_URL_NAME);
    const authorizationEndpoint = reqString(args, 'authorizationEndpoint');
    const clientId = reqString(args, 'clientId');
    const redirectUri = reqString(args, 'redirectUri');
    const scope = optString(args, 'scope');
    const givenState = optString(args, 'state');
    const givenVerifier = optString(args, 'codeVerifier');
    const pkce = optBoolean(args, 'pkce') ?? true;
    if (givenVerifier !== undefined && !PKCE_VERIFIER_PATTERN.test(givenVerifier)) {
      throw new RackError(
        'invalid_argument',
        'args.codeVerifier must be 43..128 unreserved characters per RFC 7636',
      );
    }
    if (!pkce && givenVerifier !== undefined) {
      throw new RackError('invalid_argument', 'args.codeVerifier requires pkce:true');
    }
    let url: URL;
    try {
      url = new URL(authorizationEndpoint);
    } catch {
      throw new RackError('invalid_argument', 'args.authorizationEndpoint must be an absolute URL');
    }
    const state = givenState ?? randomUrlToken(STATE_BYTES);
    const params: Array<readonly [string, string]> = [
      ['response_type', 'code'],
      ['client_id', clientId],
      ['redirect_uri', redirectUri],
      ...(scope !== undefined ? [['scope', scope] as const] : []),
      ['state', state],
    ];
    if (!pkce) {
      appendQuery(url, params);
      return { url: url.toString(), state };
    }
    const codeVerifier = givenVerifier ?? randomUrlToken(PKCE_VERIFIER_BYTES);
    const codeChallenge = computeCodeChallenge(codeVerifier);
    params.push(['code_challenge', codeChallenge], ['code_challenge_method', 'S256']);
    appendQuery(url, params);
    return { url: url.toString(), state, codeVerifier, codeChallenge };
  }

  /**
   * @brief Handle the token method: grant execution, caching and automatic refresh.
   *
   * @param rawArgs Request args document.
   * @return Token result document; cached:true marks a cache hit without network I/O.
   * @throws RackError 'invalid_argument' for missing or contradictory parameters;
   *         RackError 'failed' with {phase, httpStatus, oauthError?} when an endpoint rejects
   *         the request (refresh failures also clear the cache entry first).
   */
  async function token(rawArgs: JsonValue): Promise<JsonObject> {
    const args = argsObject(rawArgs, TOKEN_NAME);
    const grantType = reqString(args, 'grantType');
    const tokenEndpoint = reqString(args, 'tokenEndpoint');
    const clientId = reqString(args, 'clientId');
    const clientSecret = optString(args, 'clientSecret');
    const scope = optString(args, 'scope');
    const refreshEndpoint = optString(args, 'refreshEndpoint');
    const cacheKey = optString(args, 'cacheKey');

    let codeVerifier: string | undefined;
    const pkceRaw = args['pkce'];
    if (pkceRaw !== undefined) {
      const pkceArgs = asJsonObject(pkceRaw);
      if (pkceArgs === undefined) {
        throw new RackError('invalid_argument', 'args.pkce must be an object with a codeVerifier string');
      }
      codeVerifier = reqString(pkceArgs, 'codeVerifier');
    }

    let code: string;
    let redirectUri: string | undefined;
    if (grantType === 'client_credentials') {
      code = optString(args, 'code') ?? '';
      redirectUri = optString(args, 'redirectUri');
      if (code.length > 0 || redirectUri !== undefined || codeVerifier !== undefined) {
        throw new RackError(
          'invalid_argument',
          'client_credentials grant must not carry code, redirectUri or pkce',
        );
      }
    } else if (grantType === 'authorization_code') {
      code = reqString(args, 'code');
      redirectUri = optString(args, 'redirectUri');
      if (codeVerifier !== undefined && !PKCE_VERIFIER_PATTERN.test(codeVerifier)) {
        throw new RackError(
          'invalid_argument',
          'pkce.codeVerifier must be 43..128 unreserved characters per RFC 7636',
        );
      }
    } else {
      throw new RackError('invalid_argument', `unsupported grantType '${grantType}'`);
    }

    const key = cacheKey ?? deriveCacheKey(tokenEndpoint, clientId, scope);
    const entry = cache.get(key);
    if (entry !== undefined && entry.expiresAt - EXPIRY_MARGIN_MS > now()) {
      return tokenResult(entry, true);
    }
    if (entry !== undefined && entry.refreshToken !== undefined) {
      const refreshToken = entry.refreshToken;
      try {
        const endpoint = refreshEndpoint ?? entry.refreshEndpoint ?? entry.tokenEndpoint;
        const form: Record<string, string> = {
          grant_type: 'refresh_token',
          refresh_token: refreshToken,
          client_id: entry.clientId,
        };
        if (clientSecret !== undefined) form['client_secret'] = clientSecret;
        if (scope !== undefined) form['scope'] = scope;
        const reply = await postForm(endpoint, form, 'refresh');
        const refreshed = parseTokenReply(reply, {
          phase: 'refresh',
          fallbackScope: entry.scope,
          fallbackRefreshToken: refreshToken,
          tokenEndpoint: entry.tokenEndpoint,
          refreshEndpoint: entry.refreshEndpoint ?? refreshEndpoint,
          clientId: entry.clientId,
        });
        cache.set(key, refreshed);
        return tokenResult(refreshed, false);
      } catch (error) {
        cache.delete(key);
        throw error;
      }
    }
    if (entry !== undefined) cache.delete(key);

    const form: Record<string, string> = { grant_type: grantType, client_id: clientId };
    if (clientSecret !== undefined) form['client_secret'] = clientSecret;
    if (scope !== undefined) form['scope'] = scope;
    if (grantType === 'authorization_code') {
      form['code'] = code;
      if (redirectUri !== undefined) form['redirect_uri'] = redirectUri;
      if (codeVerifier !== undefined) form['code_verifier'] = codeVerifier;
    }
    const reply = await postForm(tokenEndpoint, form, 'token');
    const fresh = parseTokenReply(reply, {
      phase: 'token',
      fallbackScope: scope,
      fallbackRefreshToken: undefined,
      tokenEndpoint,
      refreshEndpoint,
      clientId,
    });
    if (fresh.expiresIn > 0) cache.set(key, fresh);
    return tokenResult(fresh, false);
  }

  /**
   * @brief Handle the token.invalidate method.
   *
   * @param rawArgs Request args document.
   * @return null on success; invalidating an unknown key is an idempotent success.
   * @throws RackError 'invalid_argument' when args.cacheKey is missing or not a string.
   */
  function invalidate(rawArgs: JsonValue): JsonValue {
    const args = argsObject(rawArgs, INVALIDATE_NAME);
    const cacheKey = reqString(args, 'cacheKey');
    cache.delete(cacheKey);
    return null;
  }

  /**
   * @brief Handle the introspect method.
   *
   * @param rawArgs Request args document.
   * @return The endpoint's raw JSON response passed through unmodified.
   * @throws RackError 'failed' with {phase:'introspect', httpStatus, oauthError?} on non-2xx
   *         responses or {reason:'invalid_json'} when a 2xx body is not valid JSON.
   */
  async function introspect(rawArgs: JsonValue): Promise<JsonValue> {
    const args = argsObject(rawArgs, INTROSPECT_NAME);
    const introspectionEndpoint = reqString(args, 'introspectionEndpoint');
    const tokenValue = reqString(args, 'token');
    const clientId = reqString(args, 'clientId');
    const clientSecret = optString(args, 'clientSecret');
    const form: Record<string, string> = { token: tokenValue, client_id: clientId };
    if (clientSecret !== undefined) form['client_secret'] = clientSecret;
    const reply = await postForm(introspectionEndpoint, form, 'introspect');
    if (reply.json === undefined) {
      throw new RackError('failed', 'oauth introspect endpoint returned a non-JSON body', {
        phase: 'introspect',
        httpStatus: reply.status,
        reason: 'invalid_json',
      });
    }
    return reply.json;
  }

  async function dispatch(request: InvokeRequest): Promise<JsonValue> {
    const { methodId, methodName, args } = request;
    if (methodId === AUTHORIZE_URL_ID || methodName === AUTHORIZE_URL_NAME) {
      return authorizeUrl(args);
    }
    if (methodId === TOKEN_ID || methodName === TOKEN_NAME) {
      return await token(args);
    }
    if (methodId === INVALIDATE_ID || methodName === INVALIDATE_NAME) {
      return invalidate(args);
    }
    if (methodId === INTROSPECT_ID || methodName === INTROSPECT_NAME) {
      return await introspect(args);
    }
    throw new RackError(
      'not_found',
      `unknown std.oauth method ${methodName} (#${methodId}); secrets are shown as ${MASKED_SECRET}`,
    );
  }

  return definePlugin(
    { plugId: OAUTH_PLUG_ID, version: OAUTH_VERSION, init: {} },
    {
      init: (injected) => {
        services = injected;
      },
      start: () => {
        services?.caps.publish({ methods: OAUTH_METHODS });
      },
      stop: () => {
        // Drop every cached token (access and refresh) when the device is stopped.
        cache.clear();
      },
      invoke: (request) => dispatch(request),
    },
  );
}

/**
 * @brief Create one std.oauth instance wired to the real global fetch and wall clock.
 *
 * @return A RackPlugin; loader-convention factory used by the host.
 */
export function create(): RackPlugin {
  return createWithDeps();
}

// Loader convention: a create(): RackPlugin factory as default or named export.
export default create;
