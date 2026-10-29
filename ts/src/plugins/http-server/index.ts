/**
 * @file std.http-server: HTTP entry bridge onto the rack invoke gateway.
 *
 * @brief A built-in standard device that listens with node:http and translates every incoming
 *        HTTP request into a one-shot lease borrow plus a gateway call of a target plugin
 *        method, returning the lease immediately afterwards.
 *
 * @details This mirrors the C++ design where the management plane borrows, calls and returns a
 *          lease for every invocation: no provider references and no long-lived credentials
 *          cross plugin boundaries. Routing (method + path with `:param` extraction) is pure
 *          local state; forwarding is `caps.acquire -> calls.call -> release` per request.
 *          Transport-level failures (no route, wrong method, oversized body, target
 *          unavailable) map onto real HTTP statuses, while business failures of the target
 *          method are delivered as HTTP 200 with an `{ok:false}` envelope so callers can tell
 *          the two apart.
 */

import { createServer } from 'node:http';
import type { IncomingMessage, ServerResponse } from 'node:http';
import { definePlugin, isPluginVersion, RackError, statusOf, validVersionRange } from '../../sdk/index.ts';
import type {
  CapsDesc,
  JsonObject,
  JsonValue,
  LeaseCredential,
  LeaseHandle,
  MethodDesc,
  PluginServices,
  RackPlugin,
  Revoker,
  VersionRange,
} from '../../sdk/index.ts';

/** @brief Canonical plugin identity of this standard device. */
const HTTP_SERVER_PLUG_ID = 'std.http-server';

/** @brief Business version of this device. */
const HTTP_SERVER_VERSION = { major: 1, minor: 0, patch: 0 } as const;

/** @brief Numeric method ids of the published management methods. */
const ROUTE_ADD_ID = 1;
const ROUTE_REMOVE_ID = 2;
const ROUTES_LIST_ID = 3;
const STATS_ID = 4;

/** @brief Method names matching the numeric ids above; part of the data-only contract. */
const ROUTE_ADD_NAME = 'route.add';
const ROUTE_REMOVE_NAME = 'route.remove';
const ROUTES_LIST_NAME = 'routes.list';
const STATS_NAME = 'stats';

/** @brief Methods announced through the capability service during start(). */
const HTTP_SERVER_METHODS: readonly MethodDesc[] = [
  {
    id: ROUTE_ADD_ID,
    name: ROUTE_ADD_NAME,
    description: 'Register an HTTP route forwarded to a target plugin method.',
    inputSchema: { type: 'object' },
    outputSchema: { type: 'object', properties: { routeId: { type: 'string' } } },
  },
  {
    id: ROUTE_REMOVE_ID,
    name: ROUTE_REMOVE_NAME,
    description: 'Remove a previously registered route by id.',
    inputSchema: { type: 'object', properties: { routeId: { type: 'string' } } },
    outputSchema: { type: 'null' },
  },
  {
    id: ROUTES_LIST_ID,
    name: ROUTES_LIST_NAME,
    description: 'List all registered routes with their forwarding targets.',
    inputSchema: { type: 'null' },
    outputSchema: { type: 'object', properties: { routes: { type: 'array' } } },
  },
  {
    id: STATS_ID,
    name: STATS_NAME,
    description: 'Return request counters: total requests, error outcomes and active requests.',
    inputSchema: { type: 'null' },
    outputSchema: { type: 'object' },
  },
];

/** @brief Method id to name resolution for id-based invoke dispatch. */
const METHOD_IDS: ReadonlyMap<number, string> = new Map<number, string>([
  [ROUTE_ADD_ID, ROUTE_ADD_NAME],
  [ROUTE_REMOVE_ID, ROUTE_REMOVE_NAME],
  [ROUTES_LIST_ID, ROUTES_LIST_NAME],
  [STATS_ID, STATS_NAME],
]);

/** @brief Default lease range used when a route adds no explicit versionRange: [0.0.0, MAX]. */
const FULL_RANGE: VersionRange = {
  minimum: { major: 0, minor: 0, patch: 0 },
  maximum: {
    major: Number.MAX_SAFE_INTEGER,
    minor: Number.MAX_SAFE_INTEGER,
    patch: Number.MAX_SAFE_INTEGER,
  },
};

/** @brief Hard cap on the request body before the request is rejected with 413. */
const MAX_BODY_BYTES = 4 * 1024 * 1024;

/** @brief HTTP methods accepted in route.add and matched against incoming requests. */
const SUPPORTED_HTTP_METHODS: ReadonlySet<string> = new Set<string>([
  'GET',
  'POST',
  'PUT',
  'DELETE',
  'PATCH',
  'HEAD',
  'OPTIONS',
]);

// ---------------------------------------------------------------------------
// Injection surface (tests and embedders)
// ---------------------------------------------------------------------------

/** @brief Address the listener actually bound to; port 0 resolves to the ephemeral port. */
export interface HttpListenAddress {
  readonly port: number;
  readonly host: string;
}

/**
 * @brief Structural subset of node:http's Server this plugin needs; injectable for tests.
 * @note The default implementation is a real `node:http` server.
 */
export interface HttpServerLike {
  listen(port: number, host: string, callback?: () => void): unknown;
  close(callback?: (error?: Error | null) => void): unknown;
  address(): { port: number } | string | null;
  on(event: 'error', listener: (error: Error) => void): unknown;
  on(event: 'listening', listener: () => void): unknown;
  closeIdleConnections?(): void;
}

/** @brief Factory creating the underlying server for one request listener. */
export type HttpServerFactory = (
  listener: (request: IncomingMessage, response: ServerResponse) => void,
) => HttpServerLike;

/** @brief Constructor-time dependencies; every field is optional. */
export interface HttpServerDeps {
  /** @brief Listen overrides; fields left out fall back to env vars and then to defaults. */
  readonly listen?: { readonly port?: number; readonly host?: string };
  /** @brief Server factory; defaults to node:http's createServer. */
  readonly createServer?: HttpServerFactory;
  /** @brief Notified once listening with the bound address (ephemeral port when 0 was given). */
  readonly onListening?: (address: HttpListenAddress) => void;
}

/** @brief Literal or `:name` path placeholder of a compiled route pattern. */
interface PathSegment {
  readonly kind: 'literal' | 'param';
  readonly text?: string;
  readonly name?: string;
}

/** @brief Internal registered route: identity fields plus the compiled path pattern. */
interface RouteEntry {
  readonly routeId: string;
  readonly path: string;
  readonly httpMethod: string;
  readonly targetPlugId: string;
  readonly targetMethod: string;
  readonly versionRange: VersionRange;
  readonly segments: readonly PathSegment[];
}

/** @brief Result of matching a request path+method against the route table. */
type RouteLookup =
  | { readonly kind: 'match'; readonly route: RouteEntry; readonly params: Record<string, string> }
  | { readonly kind: 'method-mismatch'; readonly allowed: string[] }
  | { readonly kind: 'no-path' };

/** @brief Outcome of reading one request body under the size cap. */
interface BodyRead {
  readonly bytes: Buffer | null;
  readonly tooLarge: boolean;
  readonly failed: boolean;
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

/**
 * @brief Render an unknown thrown value as a message string.
 * @param value Anything caught in a catch clause.
 * @return Error.message for Error instances, otherwise String(value).
 */
function errorMessage(value: unknown): string {
  return value instanceof Error ? value.message : String(value);
}

/**
 * @brief Read the default HTTP server factory: a real node:http server.
 * @param listener Request listener wired to the plugin dispatcher.
 * @return A structural HttpServerLike backed by node:http.
 */
function defaultHttpServerFactory(
  listener: (request: IncomingMessage, response: ServerResponse) => void,
): HttpServerLike {
  return createServer(listener);
}

/**
 * @brief Validate args as a JSON object body.
 * @param args Invoke args of runtime shape.
 * @param method Method name used in the diagnostic message.
 * @throws RackError 'invalid_argument' when args is not a plain JSON object.
 * @return A mutable view of the object fields.
 */
function requireArgsObject(args: JsonValue, method: string): Record<string, JsonValue> {
  if (typeof args !== 'object' || args === null || Array.isArray(args)) {
    throw new RackError('invalid_argument', `std.http-server: ${method} args must be an object`);
  }
  return args as Record<string, JsonValue>;
}

/**
 * @brief Read one non-empty string field from an args object.
 * @param record Args object view.
 * @param key Field name.
 * @throws RackError 'invalid_argument' when the field is missing or not a non-empty string.
 * @return The field value.
 */
function requireStringField(record: Record<string, JsonValue>, key: string): string {
  const value = record[key];
  if (typeof value !== 'string' || value.length === 0) {
    throw new RackError('invalid_argument', `std.http-server: field "${key}" must be a non-empty string`);
  }
  return value;
}

/**
 * @brief Validate an explicit version range document.
 * @param value Candidate `{ minimum, maximum }` object.
 * @throws RackError 'invalid_argument' when endpoints are malformed triples or reversed.
 * @return The validated closed range.
 */
function requireVersionRange(value: JsonValue): VersionRange {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw new RackError('invalid_argument', 'std.http-server: versionRange must be { minimum, maximum }');
  }
  const record = value as Record<string, JsonValue>;
  const minimum = record['minimum'];
  const maximum = record['maximum'];
  if (!isPluginVersion(minimum) || !isPluginVersion(maximum)) {
    throw new RackError('invalid_argument', 'std.http-server: versionRange endpoints must be version triples');
  }
  const range: VersionRange = { minimum, maximum };
  if (!validVersionRange(range)) {
    throw new RackError('invalid_argument', 'std.http-server: versionRange minimum must not exceed maximum');
  }
  return range;
}

/**
 * @brief Compile a route pattern such as `/api/:id` into matchable segments.
 * @param path Route pattern; must start with `/`, contain no empty segment, use `:name`
 *        placeholders with unique names, and literal segments limited to `[A-Za-z0-9_.-]+`.
 * @throws RackError 'invalid_argument' for any malformed pattern.
 * @return The ordered segment list for matching.
 */
function compilePath(path: string): readonly PathSegment[] {
  if (!path.startsWith('/') || path === '/' || path.includes('//') || path.endsWith('/')) {
    throw new RackError('invalid_argument', `std.http-server: invalid route path "${path}"`);
  }
  const segments: PathSegment[] = [];
  const seenParams = new Set<string>();
  for (const part of path.slice(1).split('/')) {
    const paramMatch = /^:([A-Za-z][A-Za-z0-9_]*)$/.exec(part);
    if (paramMatch !== null) {
      const name = paramMatch[1];
      if (name === undefined || seenParams.has(name)) {
        throw new RackError('invalid_argument', `std.http-server: duplicate or invalid path parameter in "${path}"`);
      }
      seenParams.add(name);
      segments.push({ kind: 'param', name });
      continue;
    }
    if (!/^[A-Za-z0-9_.-]+$/.test(part)) {
      throw new RackError('invalid_argument', `std.http-server: invalid route path segment "${part}"`);
    }
    segments.push({ kind: 'literal', text: part });
  }
  return segments;
}

/**
 * @brief Render a route as the JSON info document returned by routes.list.
 * @param route Registered route.
 * @return A fresh JSON object; the internal compiled segments stay private.
 */
function routeInfo(route: RouteEntry): JsonObject {
  return {
    routeId: route.routeId,
    path: route.path,
    httpMethod: route.httpMethod,
    targetPlugId: route.targetPlugId,
    targetMethod: route.targetMethod,
    versionRange: {
      minimum: { ...route.versionRange.minimum },
      maximum: { ...route.versionRange.maximum },
    },
  };
}

/**
 * @brief Read U42_HTTP_PORT with its default.
 * @throws RackError 'invalid_argument' when the env value is not an integer port 0..65535.
 * @return The configured port, or 8080 when unset/empty.
 */
function readEnvPort(): number {
  const raw = process.env['U42_HTTP_PORT'];
  if (raw === undefined || raw === '') return 8080;
  const port = Number(raw);
  if (!Number.isSafeInteger(port) || port < 0 || port > 65535) {
    throw new RackError('invalid_argument', `std.http-server: invalid U42_HTTP_PORT "${raw}"`);
  }
  return port;
}

// ---------------------------------------------------------------------------
// Plugin construction
// ---------------------------------------------------------------------------

/**
 * @brief Build a std.http-server plugin instance with injectable dependencies.
 *
 * @param deps Optional listen overrides, server factory and listening callback. The default
 *        factory is a real node:http server; the listen address defaults to
 *        `U42_HTTP_PORT`/`U42_HTTP_HOST` (127.0.0.1:8080).
 * @return A RackPlugin implementing route.add / route.remove / routes.list / stats and the
 *         HTTP forwarding bridge.
 */
export function createWithDeps(deps: HttpServerDeps = {}): RackPlugin {
  const factory = deps.createServer ?? defaultHttpServerFactory;
  let services: PluginServices | null = null;
  let server: HttpServerLike | null = null;
  let stopped = false;
  let routeSeq = 0;
  const routes = new Map<string, RouteEntry>();
  const stats = { requests: 0, errors: 0, active: 0 };
  /** @brief Port the listener actually bound to (ephemeral port when 0 was configured). */
  let listeningPort: number | null = null;
  const pending = new Set<Promise<void>>();

  const revoker: Revoker = {
    onRevoke(credential: LeaseCredential): void {
      // One-shot borrowing: return the credential as soon as revocation arrives. The post-call
      // handle.release() then harmlessly reports 'stale'; a 'busy' lease is kept and still
      // returned by that post-call release.
      if (services !== null) services.caps.release(credential);
    },
  };

  // ----- management method handlers -----------------------------------------

  const handlers: Readonly<Record<string, (args: JsonValue) => JsonValue>> = {
    [ROUTE_ADD_NAME]: (args: JsonValue): JsonValue => {
      const record = requireArgsObject(args, ROUTE_ADD_NAME);
      const path = requireStringField(record, 'path');
      const httpMethod = requireStringField(record, 'httpMethod');
      const targetPlugId = requireStringField(record, 'targetPlugId');
      const targetMethod = requireStringField(record, 'targetMethod');
      if (!SUPPORTED_HTTP_METHODS.has(httpMethod)) {
        throw new RackError(
          'invalid_argument',
          `std.http-server: httpMethod "${httpMethod}" is not one of ${[...SUPPORTED_HTTP_METHODS].join(', ')}`,
        );
      }
      const rawRange = record['versionRange'];
      const versionRange = rawRange === undefined || rawRange === null ? FULL_RANGE : requireVersionRange(rawRange);
      const segments = compilePath(path);
      for (const route of routes.values()) {
        if (
          route.path === path &&
          route.httpMethod === httpMethod &&
          route.targetPlugId === targetPlugId &&
          route.targetMethod === targetMethod
        ) {
          throw new RackError(
            'duplicate',
            `std.http-server: route ${httpMethod} ${path} -> ${targetPlugId}.${targetMethod} already registered`,
          );
        }
      }
      routeSeq += 1;
      const routeId = `route-${routeSeq}`;
      routes.set(routeId, { routeId, path, httpMethod, targetPlugId, targetMethod, versionRange, segments });
      return { routeId };
    },
    [ROUTE_REMOVE_NAME]: (args: JsonValue): JsonValue => {
      const record = requireArgsObject(args, ROUTE_REMOVE_NAME);
      const routeId = requireStringField(record, 'routeId');
      if (!routes.delete(routeId)) {
        throw new RackError('not_found', `std.http-server: unknown routeId "${routeId}"`);
      }
      return null;
    },
    [ROUTES_LIST_NAME]: (): JsonValue => ({
      routes: [...routes.values()].map((route): JsonObject => routeInfo(route)),
    }),
    [STATS_NAME]: (): JsonValue => ({
      requests: stats.requests,
      errors: stats.errors,
      active: stats.active,
      port: listeningPort,
    }),
  };

  /**
   * @brief Resolve the invoke handler by method name, then by numeric id.
   * @param methodName Name selector from the gateway request.
   * @param methodId Numeric selector from the gateway request.
   * @return The handler, or undefined for unknown methods.
   */
  function resolveHandler(
    methodName: string,
    methodId: number,
  ): ((args: JsonValue) => JsonValue) | undefined {
    const byName = handlers[methodName];
    if (byName !== undefined) return byName;
    const nameById = METHOD_IDS.get(methodId);
    return nameById !== undefined ? handlers[nameById] : undefined;
  }

  // ----- HTTP request handling ----------------------------------------------

  /**
   * @brief Send one JSON response; never throws, even on an already-gone socket.
   * @param response Server response stream.
   * @param statusCode HTTP status code.
   * @param payload JSON body document.
   * @param extraHeaders Additional headers (e.g. Allow on 405).
   * @return void.
   */
  function sendJson(
    response: ServerResponse,
    statusCode: number,
    payload: JsonValue,
    extraHeaders?: Record<string, string>,
  ): void {
    try {
      const headers: Record<string, string> = { 'content-type': 'application/json; charset=utf-8' };
      if (extraHeaders !== undefined) Object.assign(headers, extraHeaders);
      response.writeHead(statusCode, headers);
      response.end(JSON.stringify(payload));
    } catch {
      // The socket is gone; there is nothing left to deliver.
    }
  }

  /**
   * @brief Count and send one transport-level error response.
   * @param response Server response stream.
   * @param statusCode HTTP status code.
   * @param status Rack status carried in the error envelope.
   * @param message Human-readable diagnostic.
   * @param extraHeaders Additional headers.
   * @return void.
   */
  function respondError(
    response: ServerResponse,
    statusCode: number,
    status: string,
    message: string,
    extraHeaders?: Record<string, string>,
  ): void {
    stats.errors += 1;
    sendJson(response, statusCode, { ok: false, error: { status, message } }, extraHeaders);
  }

  /**
   * @brief Match a request path pattern against a compiled route.
   * @param route Registered route.
   * @param pathname Percent-encoded request pathname.
   * @return The extracted parameters, or null when the shapes differ.
   */
  function matchSegments(route: RouteEntry, pathname: string): Record<string, string> | null {
    if (!pathname.startsWith('/') || pathname === '/' || pathname.endsWith('/')) return null;
    const parts = pathname.slice(1).split('/');
    if (parts.length !== route.segments.length) return null;
    const params: Record<string, string> = {};
    for (let index = 0; index < parts.length; index += 1) {
      const part = parts[index];
      const segment = route.segments[index];
      if (part === undefined || segment === undefined) return null;
      let decoded: string;
      try {
        decoded = decodeURIComponent(part);
      } catch {
        return null;
      }
      if (segment.kind === 'literal') {
        if (decoded !== segment.text) return null;
      } else {
        params[segment.name ?? ''] = decoded;
      }
    }
    return params;
  }

  /**
   * @brief Look a request method+path up in the route table.
   * @param pathname Request pathname.
   * @param httpMethod Request method.
   * @return The unique match, the allowed methods on a pure method mismatch, or no-path.
   */
  function lookupRoute(pathname: string, httpMethod: string): RouteLookup {
    const allowed: string[] = [];
    let pathMatched = false;
    for (const route of routes.values()) {
      const params = matchSegments(route, pathname);
      if (params === null) continue;
      pathMatched = true;
      if (route.httpMethod === httpMethod) return { kind: 'match', route, params };
      if (!allowed.includes(route.httpMethod)) allowed.push(route.httpMethod);
    }
    return pathMatched ? { kind: 'method-mismatch', allowed } : { kind: 'no-path' };
  }

  /**
   * @brief Read the request body under the 4 MiB cap.
   * @param request Incoming request stream.
   * @return The buffered bytes (null when absent/failed/oversized), the too-large flag and the
   *         stream-failure flag. Oversized requests are drained, not buffered, so the 413
   *         response still reaches the client.
   */
  function readRequestBody(request: IncomingMessage): Promise<BodyRead> {
    const lengthHeader = request.headers['content-length'];
    const declared = typeof lengthHeader === 'string' ? Number(lengthHeader) : Number.NaN;
    if (Number.isFinite(declared) && declared > MAX_BODY_BYTES) {
      request.resume();
      return Promise.resolve<BodyRead>({ bytes: null, tooLarge: true, failed: false });
    }
    return new Promise<BodyRead>((resolve) => {
      const chunks: Buffer[] = [];
      let total = 0;
      let settled = false;
      const finish = (result: BodyRead): void => {
        if (settled) return;
        settled = true;
        resolve(result);
      };
      request.on('data', (chunk: Buffer) => {
        if (settled) return;
        total += chunk.length;
        if (total > MAX_BODY_BYTES) {
          finish({ bytes: null, tooLarge: true, failed: false });
          request.resume(); // discard the remainder without buffering it
          return;
        }
        chunks.push(chunk);
      });
      request.on('end', () => finish({ bytes: Buffer.concat(chunks), tooLarge: false, failed: false }));
      request.on('error', () => finish({ bytes: null, tooLarge: false, failed: true }));
    });
  }

  /**
   * @brief Parse the body bytes as JSON.
   * @param bytes Buffered request body, or null when absent.
   * @return The parsed JSON value; null for an empty body or any non-JSON body.
   */
  function parseJsonBody(bytes: Buffer | null): JsonValue {
    if (bytes === null || bytes.length === 0) return null;
    try {
      return JSON.parse(bytes.toString('utf8')) as JsonValue;
    } catch {
      return null;
    }
  }

  /**
   * @brief Collect request headers into a JSON-safe record.
   * @param request Incoming request.
   * @return Node's header map without undefined entries; repeated headers stay arrays.
   */
  function collectHeaders(request: IncomingMessage): Record<string, JsonValue> {
    const headers: Record<string, JsonValue> = {};
    for (const [name, value] of Object.entries(request.headers)) {
      if (value !== undefined) headers[name] = value;
    }
    return headers;
  }

  /**
   * @brief Forward one matched HTTP request through a one-shot lease and the gateway.
   * @param request Incoming request stream.
   * @param response Server response stream.
   * @return Resolved once the response (and the lease release) has been handled.
   */
  async function dispatchHttpRequest(request: IncomingMessage, response: ServerResponse): Promise<void> {
    const method = request.method ?? '';
    const url = new URL(request.url ?? '/', 'http://std.http-server.invalid');
    const lookup = lookupRoute(url.pathname, method);
    if (lookup.kind === 'no-path') {
      respondError(response, 404, 'not_found', `no route matches ${url.pathname}`);
      return;
    }
    if (lookup.kind === 'method-mismatch') {
      respondError(
        response,
        405,
        'unsupported',
        `method ${method || '(none)'} not allowed for ${url.pathname}; allowed: ${lookup.allowed.join(', ')}`,
        { allow: lookup.allowed.join(', ') },
      );
      return;
    }
    const route = lookup.route;
    const bodyRead = await readRequestBody(request);
    if (bodyRead.failed) {
      respondError(response, 400, 'failed', 'request stream failed before the body completed');
      return;
    }
    if (bodyRead.tooLarge) {
      respondError(response, 413, 'limit_exceeded', `request body exceeds the ${MAX_BODY_BYTES} byte limit`);
      return;
    }
    const svc = services;
    if (svc === null) {
      respondError(response, 500, 'invalid_state', 'std.http-server is not initialized');
      return;
    }
    const args: JsonObject = {
      query: Object.fromEntries(url.searchParams.entries()),
      params: lookup.params,
      headers: collectHeaders(request),
      body: parseJsonBody(bodyRead.bytes),
    };
    let handle: LeaseHandle;
    try {
      handle = svc.caps.acquire(route.targetPlugId, route.versionRange, revoker);
    } catch (error) {
      const status = statusOf(error);
      if (status === 'not_found' || status === 'unsupported') {
        respondError(response, 503, status, `target ${route.targetPlugId} unavailable: ${errorMessage(error)}`);
      } else {
        respondError(response, 500, status, `lease acquisition failed: ${errorMessage(error)}`);
      }
      return;
    }
    try {
      const result = await svc.calls.call(handle.credential, route.targetMethod, args);
      sendJson(response, 200, { ok: true, result });
    } catch (error) {
      // Business failure: the transport worked. Deliberately answered as HTTP 200 with an
      // {ok:false} envelope so a target's business refusal is never confused with a gateway
      // or transport problem (404/405/413/503/500).
      stats.errors += 1;
      sendJson(response, 200, { ok: false, error: { status: statusOf(error), message: errorMessage(error) } });
    } finally {
      // One-shot lease: return the credential immediately after every call, success or not.
      await handle.release();
    }
  }

  /**
   * @brief Connection entry: bookkeeping plus an awaited, never-rejecting dispatch task.
   * @param request Incoming request stream.
   * @param response Server response stream.
   * @return void.
   */
  function handleConnection(request: IncomingMessage, response: ServerResponse): void {
    stats.requests += 1;
    stats.active += 1;
    const task = (async () => {
      try {
        await dispatchHttpRequest(request, response);
      } catch (error) {
        respondError(response, 500, 'failed', `internal error: ${errorMessage(error)}`);
      } finally {
        stats.active -= 1;
      }
    })();
    pending.add(task);
    void task.then(
      () => pending.delete(task),
      () => pending.delete(task),
    );
  }

  // ----- lifecycle ------------------------------------------------------------

  return definePlugin(
    { plugId: HTTP_SERVER_PLUG_ID, version: HTTP_SERVER_VERSION, init: {} },
    {
      init: (injected: PluginServices) => {
        services = injected;
      },
      start: async () => {
        const svc = services;
        if (svc === null) {
          throw new RackError('invalid_state', 'std.http-server: init() must run before start()');
        }
        if (server !== null) {
          throw new RackError('invalid_state', 'std.http-server: already listening');
        }
        if (stopped) {
          throw new RackError('invalid_state', 'std.http-server: instance was already stopped once');
        }
        const port = deps.listen?.port ?? readEnvPort();
        if (!Number.isSafeInteger(port) || port < 0 || port > 65535) {
          throw new RackError('invalid_argument', `std.http-server: invalid listen port ${String(port)}`);
        }
        const host = deps.listen?.host ?? process.env['U42_HTTP_HOST'] ?? '127.0.0.1';
        if (typeof host !== 'string' || host.length === 0) {
          throw new RackError('invalid_argument', 'std.http-server: listen host must be a non-empty string');
        }
        const bound = factory(handleConnection);
        server = bound;
        let rejectListen: ((error: Error) => void) | null = null;
        bound.on('error', (error: Error): void => {
          if (rejectListen !== null) {
            const reject = rejectListen;
            rejectListen = null;
            reject(error);
            return;
          }
          svc.diag.report('error', 'std.http-server: runtime server error', error.message);
        });
        await new Promise<void>((resolve, reject) => {
          rejectListen = reject;
          bound.listen(port, host, () => {
            rejectListen = null;
            resolve();
          });
        }).catch((error: unknown) => {
          server = null;
          throw error;
        });
        const address = bound.address();
        const boundPort = typeof address === 'object' && address !== null ? address.port : port;
        listeningPort = boundPort;
        deps.onListening?.({ port: boundPort, host });
        svc.caps.publish({ methods: HTTP_SERVER_METHODS } satisfies CapsDesc);
      },
      stop: async () => {
        stopped = true;
        listeningPort = null;
        if (server !== null) {
          const closing = server;
          server = null;
          closing.closeIdleConnections?.();
          await new Promise<void>((resolve) => {
            closing.close(() => resolve());
          });
        }
        // Wait until every in-flight request has settled (response written, lease returned).
        while (pending.size > 0) {
          await Promise.all([...pending]);
        }
      },
      invoke: (request) => {
        const handler = resolveHandler(request.methodName, request.methodId);
        if (handler === undefined) {
          throw new RackError(
            'not_found',
            `std.http-server: unknown method ${request.methodName} (#${request.methodId})`,
          );
        }
        return handler(request.args);
      },
    },
  );
}

/**
 * @brief Build a std.http-server plugin with production defaults (real node:http server).
 * @return A RackPlugin instance; listen address comes from env vars or their defaults.
 */
export function create(): RackPlugin {
  return createWithDeps();
}

// Loader convention: a create(): RackPlugin factory as default or named export.
export default create;
