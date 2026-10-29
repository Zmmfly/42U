/**
 * @file index.ts
 * @brief std.rpc: the JSON-RPC 2.0 rear-panel network port of the 42U rack (server bridge + client).
 *
 * @details One plugin identity plays two roles:
 *          - Server bridge: leases `std.http-server` within [1.0.0, 1.MAX], registers one
 *            `POST /rpc` route through its `route.add` gateway method (`{path, httpMethod,
 *            targetPlugId, targetMethod, versionRange?}` -> `{routeId}`), and answers
 *            JSON-RPC 2.0 requests by one-shot leasing the target plugin named in
 *            `<plugId>.<methodName>` and invoking it through the rack gateway. The server
 *            dispatches `{query, params, headers, body}` documents (body = parsed JSON or
 *            null) and wraps the returned value into `{ok:true, result}`; non-POST or
 *            foreign-path requests are answered 404/405 by the server itself.
 *          - Client: publishes `call` and `batch` methods performing outgoing JSON-RPC 2.0
 *            requests over HTTP with a configurable timeout - the seed of rack-to-rack
 *            interconnect. Both bare JSON-RPC responses and the rack `{ok}` transport
 *            envelope of a std.http-server-hosted peer are understood.
 *
 *          All external effects (outgoing HTTP) go through an injectable fetch implementation
 *          (createWithDeps); create() binds the global fetch. No npm dependency is added.
 */

import {
  acceptsVersion,
  definePlugin,
  isJsonValue,
  isPluginVersion,
  isRackError,
  RackError,
  statusOf,
  validVersionRange,
} from '../../sdk/index.ts';
import type {
  CapEvent,
  CapSink,
  InvokeRequest,
  JsonObject,
  JsonValue,
  LeaseCredential,
  LeaseHandle,
  MethodDesc,
  PluginServices,
  RackPlugin,
  Revoker,
  Subscription,
  VersionRange,
} from '../../sdk/index.ts';
import type { RackStatus } from '../../sdk/index.ts';

// ---------------------------------------------------------------------------
// Identity, leased peer and published contract.
// ---------------------------------------------------------------------------

/** @brief Canonical plugin identity announced to the host. */
const RPC_PLUG_ID = 'std.rpc';

/** @brief Business version of this plugin. */
const RPC_VERSION = { major: 1, minor: 0, patch: 0 } as const;

/** @brief Identity of the standard HTTP server this bridge mounts its route on. */
const HTTP_SERVER_PLUG_ID = 'std.http-server';

/** @brief HTTP path registered on the leased std.http-server. */
const RPC_HTTP_PATH = '/rpc';

/** @brief Gateway method of std.http-server that registers one route. */
const HTTP_ROUTE_ADD_METHOD = 'route.add';

/** @brief Gateway method of std.http-server that unregisters one route. */
const HTTP_ROUTE_REMOVE_METHOD = 'route.remove';

/**
 * @brief Interval the bridge pins for one-shot leases of itself (its own major).
 *
 * @note Kept as a plain literal (not the VersionRange interface) so it stays assignable to
 *       the gateway's JsonValue args without a cast.
 */
const SELF_VERSION_RANGE = {
  minimum: { major: 1, minor: 0, patch: 0 },
  maximum: { major: 1, minor: Number.MAX_SAFE_INTEGER, patch: Number.MAX_SAFE_INTEGER },
} as const;

/** @brief Accepted std.http-server interval: [1.0.0, 1.MAX]. */
const HTTP_SERVER_RANGE: VersionRange = {
  minimum: { major: 1, minor: 0, patch: 0 },
  maximum: { major: 1, minor: Number.MAX_SAFE_INTEGER, patch: Number.MAX_SAFE_INTEGER },
};

/** @brief Default one-shot target interval: every valid plugin version. */
const ANY_VERSION_RANGE: VersionRange = {
  minimum: { major: 0, minor: 0, patch: 0 },
  maximum: { major: Number.MAX_SAFE_INTEGER, minor: Number.MAX_SAFE_INTEGER, patch: Number.MAX_SAFE_INTEGER },
};

/** @brief Numeric method id of the server bridge entry called by std.http-server. */
const BRIDGE_METHOD_ID = 1;

/** @brief Published name of the server bridge entry. */
const BRIDGE_METHOD_NAME = 'bridge';

/** @brief Numeric method id of the single remote call client method. */
const CALL_METHOD_ID = 2;

/** @brief Published name of the single remote call client method. */
const CALL_METHOD_NAME = 'call';

/** @brief Numeric method id of the batch client method. */
const BATCH_METHOD_ID = 3;

/** @brief Published name of the batch client method. */
const BATCH_METHOD_NAME = 'batch';

/** @brief Methods announced through the capability service during start(). */
const RPC_METHODS: readonly MethodDesc[] = [
  {
    id: BRIDGE_METHOD_ID,
    name: BRIDGE_METHOD_NAME,
    description: 'Server bridge entry: execute one JSON-RPC 2.0 request or batch against rack providers.',
    inputSchema: {
      type: 'object',
      properties: {
        query: { type: 'object', description: 'Unread by the bridge; forwarded by std.http-server.' },
        params: { type: 'object', description: 'Unread by the bridge; path parameters.' },
        headers: { type: 'object', description: 'Unread by the bridge; lower-cased header table.' },
        body: { description: 'Parsed JSON request document; null for unparseable/empty bodies.' },
      },
      required: ['body'],
    },
    outputSchema: {},
  },
  {
    id: CALL_METHOD_ID,
    name: CALL_METHOD_NAME,
    description: 'Perform one remote JSON-RPC 2.0 method call over HTTP and return its result.',
    inputSchema: {
      type: 'object',
      properties: {
        url: { type: 'string' },
        method: { type: 'string', description: 'Remote method as "<plugId>.<methodName>".' },
        params: {},
        timeoutMs: { type: 'number' },
      },
      required: ['url', 'method'],
    },
    outputSchema: {},
  },
  {
    id: BATCH_METHOD_ID,
    name: BATCH_METHOD_NAME,
    description: 'Post one JSON-RPC 2.0 batch and return aligned per-entry results.',
    inputSchema: {
      type: 'object',
      properties: {
        url: { type: 'string' },
        calls: {
          type: 'array',
          items: {
            type: 'object',
            properties: { method: { type: 'string' }, params: {} },
            required: ['method'],
          },
        },
        timeoutMs: { type: 'number' },
      },
      required: ['url', 'calls'],
    },
    outputSchema: { type: 'array' },
  },
];

// ---------------------------------------------------------------------------
// JSON-RPC 2.0 protocol constants.
// ---------------------------------------------------------------------------

/** @brief Protocol version string required in every request. */
const JSONRPC_VERSION = '2.0';

/** @brief Invalid JSON was received by the server. */
const CODE_PARSE_ERROR = -32700;

/** @brief The JSON sent is not a valid Request object. */
const CODE_INVALID_REQUEST = -32600;

/** @brief The method does not exist / is not found. */
const CODE_METHOD_NOT_FOUND = -32601;

/** @brief Invalid method parameter(s). */
const CODE_INVALID_PARAMS = -32602;

/** @brief Internal JSON-RPC error. */
const CODE_INTERNAL_ERROR = -32603;

/** @brief Default client timeout in milliseconds. */
const DEFAULT_TIMEOUT_MS = 10000;

// ---------------------------------------------------------------------------
// Injected dependencies.
// ---------------------------------------------------------------------------

/** @brief Minimal fetch-like request description the client path produces. */
export interface RpcFetchInit {
  readonly method: 'POST';
  readonly headers: Readonly<Record<string, string>>;
  readonly body: string;
  readonly signal: AbortSignal;
}

/** @brief Minimal fetch-like function; inject a fake in tests via createWithDeps(). */
export type RpcFetchImpl = (url: string, init: RpcFetchInit) => Promise<Response>;

// ---------------------------------------------------------------------------
// Small argument and JSON helpers.
// ---------------------------------------------------------------------------

/**
 * @brief Render any thrown value as a short human-readable string.
 *
 * @param error Value caught in a catch clause.
 * @return The error message or a stringified fallback.
 */
function describeError(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

/**
 * @brief Narrow a JSON value to a JSON object.
 *
 * @param value Candidate args document.
 * @param what Name of the checked document for the error message.
 * @return The same value typed as JsonObject.
 * @throws RackError 'invalid_argument' when the value is not a JSON object.
 */
function expectArgsObject(value: JsonValue, what: string): JsonObject {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw new RackError('invalid_argument', `${what} must be a JSON object`);
  }
  return value as JsonObject;
}

/**
 * @brief Extract a mandatory non-empty string field of an args object.
 *
 * @param record Args document already narrowed to JsonObject.
 * @param field Field name; also used verbatim in the error message.
 * @return The validated string.
 * @throws RackError 'invalid_argument' when the field is missing or not a non-empty string.
 */
function expectStringField(record: JsonObject, field: string): string {
  const value = record[field];
  if (typeof value !== 'string' || value.length === 0) {
    throw new RackError('invalid_argument', `${field} must be a non-empty string`);
  }
  return value;
}

/**
 * @brief Read the optional positive integer timeout field.
 *
 * @param value Candidate timeoutMs value.
 * @return The given value, or DEFAULT_TIMEOUT_MS when absent.
 * @throws RackError 'invalid_argument' for a non-positive or non-integer value.
 */
function readTimeoutMs(value: JsonValue | undefined): number {
  if (value === undefined) return DEFAULT_TIMEOUT_MS;
  if (typeof value !== 'number' || !Number.isSafeInteger(value) || value <= 0) {
    throw new RackError('invalid_argument', 'timeoutMs must be a positive integer when present');
  }
  return value;
}

/**
 * @brief Normalize an optional params document the way the wire protocol treats it.
 *
 * @param value Candidate params value.
 * @return `{}` when absent or null, the value itself otherwise.
 */
function normalizeParams(value: JsonValue | undefined): JsonValue {
  return value === undefined || value === null ? {} : value;
}

/**
 * @brief Structural test for a plain JSON object (not an array, not null).
 *
 * @param value Candidate of JsonValue origin.
 * @return true for plain objects.
 */
function isPlainJsonObject(value: JsonValue | undefined): value is JsonObject {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

/**
 * @brief Structural test for a versionRange request extension field.
 *
 * @param value Candidate value of unknown origin.
 * @return true when both endpoints are well-formed version triples.
 */
function isVersionRangeValue(value: unknown): value is VersionRange {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) return false;
  const record = value as Record<string, unknown>;
  return isPluginVersion(record['minimum']) && isPluginVersion(record['maximum']);
}

/**
 * @brief Structural test for a JSON-RPC request id.
 *
 * @param value Candidate id.
 * @return true for string, finite number or null.
 */
function isRequestId(value: JsonValue | undefined): value is string | number | null {
  if (value === null || typeof value === 'string') return true;
  return typeof value === 'number' && Number.isFinite(value);
}

// ---------------------------------------------------------------------------
// JSON-RPC error mapping.
// ---------------------------------------------------------------------------

/**
 * @brief Map a rack status onto a JSON-RPC error code.
 *
 * @param status Status of the RackError caught while acquiring or invoking.
 * @return -32601 for not_found, -32602 for invalid_argument, -32603 otherwise.
 */
function statusToCode(status: RackStatus): number {
  if (status === 'not_found') return CODE_METHOD_NOT_FOUND;
  if (status === 'invalid_argument') return CODE_INVALID_PARAMS;
  return CODE_INTERNAL_ERROR;
}

/**
 * @brief Map a remote JSON-RPC error code onto a rack status.
 *
 * @param code Remote error code.
 * @return not_found for -32601, invalid_argument for -32602, failed otherwise.
 */
function codeToStatus(code: number): RackStatus {
  if (code === CODE_METHOD_NOT_FOUND) return 'not_found';
  if (code === CODE_INVALID_PARAMS) return 'invalid_argument';
  return 'failed';
}

/**
 * @brief Build a JSON-RPC success envelope.
 *
 * @param id Request id echoed verbatim.
 * @param result Gateway result value.
 * @return The `{jsonrpc, id, result}` response object.
 */
function successEnvelope(id: JsonValue, result: JsonValue): JsonObject {
  return { jsonrpc: JSONRPC_VERSION, id, result };
}

/**
 * @brief Build a JSON-RPC error envelope.
 *
 * @param id Request id echoed verbatim (null when undeterminable).
 * @param code JSON-RPC error code.
 * @param message Human-readable error description.
 * @param data Optional rack status carried in the error data field.
 * @return The `{jsonrpc, id, error}` response object.
 */
function errorEnvelope(id: JsonValue, code: number, message: string, data?: RackStatus): JsonObject {
  const error: JsonObject = data === undefined ? { code, message } : { code, message, data };
  return { jsonrpc: JSONRPC_VERSION, id, error };
}

/**
 * @brief Parse raw request text without throwing.
 *
 * @param text Raw HTTP request body.
 * @return The parsed JSON value, or a failure marker for unparseable or non-JSON text.
 */
function tryParseJson(text: string): { readonly ok: true; readonly value: JsonValue } | { readonly ok: false } {
  try {
    const parsed: unknown = JSON.parse(text);
    if (!isJsonValue(parsed)) return { ok: false };
    return { ok: true, value: parsed };
  } catch {
    return { ok: false };
  }
}

// ---------------------------------------------------------------------------
// Server-side request checking and execution.
// ---------------------------------------------------------------------------

/** @brief One validated JSON-RPC request ready for gateway execution. */
interface CheckedRpcRequest {
  readonly id: string | number | null;
  readonly plugId: string;
  readonly methodName: string;
  readonly params: JsonValue;
  readonly range: VersionRange;
}

/** @brief Discriminated result of request validation. */
type RequestCheck =
  | { readonly ok: true; readonly request: CheckedRpcRequest }
  | {
      readonly ok: false;
      readonly id: string | number | null;
      readonly code: number;
      readonly message: string;
    };

/**
 * @brief Validate one incoming JSON-RPC request object.
 *
 * @param raw Parsed request document.
 * @return The checked request parts, or the protocol error to answer with.
 * @note ids are mandatory (notifications are not supported); `method` is split into
 *       `<plugId>.<methodName>` at the LAST dot, so dotted plug ids like `std.rpc` work
 *       while method names must stay dot-free; `params` must be an object or array when
 *       present; `versionRange` is this rack's extension and must be a valid closed interval.
 */
function checkRpcRequest(raw: JsonValue): RequestCheck {
  if (!isPlainJsonObject(raw)) {
    return { ok: false, id: null, code: CODE_INVALID_REQUEST, message: 'request must be a JSON object' };
  }
  const id = isRequestId(raw['id']) ? raw['id'] : null;
  if (raw['jsonrpc'] !== JSONRPC_VERSION) {
    return { ok: false, id, code: CODE_INVALID_REQUEST, message: 'jsonrpc must be exactly "2.0"' };
  }
  if (!isRequestId(raw['id'])) {
    return { ok: false, id: null, code: CODE_INVALID_REQUEST, message: 'id must be a string, number or null' };
  }
  const method = raw['method'];
  if (typeof method !== 'string' || method.length === 0) {
    return { ok: false, id, code: CODE_INVALID_REQUEST, message: 'method must be a non-empty string' };
  }
  const lastDot = method.lastIndexOf('.');
  if (lastDot <= 0 || lastDot === method.length - 1) {
    return {
      ok: false,
      id,
      code: CODE_INVALID_REQUEST,
      message:
        'method must look like "<plugId>.<methodName>" split at the last dot, with both parts non-empty',
    };
  }
  const paramsValue = raw['params'];
  let params: JsonValue;
  if (paramsValue === undefined || paramsValue === null) {
    params = {};
  } else if (typeof paramsValue === 'object') {
    params = paramsValue;
  } else {
    return {
      ok: false,
      id,
      code: CODE_INVALID_REQUEST,
      message: 'params must be an object or array when present',
    };
  }
  const rangeValue = raw['versionRange'];
  let range: VersionRange = ANY_VERSION_RANGE;
  if (rangeValue !== undefined) {
    if (!isVersionRangeValue(rangeValue) || !validVersionRange(rangeValue)) {
      return {
        ok: false,
        id,
        code: CODE_INVALID_PARAMS,
        message: 'versionRange must be a valid closed interval of version triples',
      };
    }
    range = rangeValue;
  }
  return {
    ok: true,
    request: {
      id,
      plugId: method.slice(0, lastDot),
      methodName: method.slice(lastDot + 1),
      params,
      range,
    },
  };
}

/**
 * @brief Execute one validated-or-not JSON-RPC request against the rack gateway.
 *
 * @param raw Parsed request document.
 * @param svc Services of this plugin instance.
 * @return The JSON-RPC response envelope; never throws.
 * @note The target is leased one-shot with the request's versionRange (default: every
 *       version), invoked by name, and released before the envelope is built.
 */
async function executeRpcRequest(raw: JsonValue, svc: PluginServices): Promise<JsonObject> {
  const checked = checkRpcRequest(raw);
  if (!checked.ok) {
    return errorEnvelope(checked.id, checked.code, checked.message);
  }
  const { id, plugId, methodName, params, range } = checked.request;
  try {
    const lease = svc.caps.acquire(plugId, range, NOOP_REVOKER);
    let result: JsonValue;
    try {
      result = await svc.calls.call(lease.credential, methodName, params);
    } finally {
      const outcome = await lease.release();
      if (outcome !== 'ok') {
        svc.diag.report('warn', `one-shot lease release returned "${outcome}"`, { plugId });
      }
    }
    return successEnvelope(id, result);
  } catch (error) {
    const status = statusOf(error);
    return errorEnvelope(
      id,
      statusToCode(status),
      `${plugId}.${methodName} failed: ${describeError(error)}`,
      status,
    );
  }
}

/**
 * @brief Execute a parsed JSON-RPC payload (single request or batch).
 *
 * @param payload Parsed request document.
 * @param svc Services of this plugin instance.
 * @return A single envelope, or one aligned envelope array for batch requests.
 * @note Batch entries execute serially in input order; an empty batch is answered by a
 *       single -32600 envelope per the JSON-RPC 2.0 specification.
 */
async function executeRpcPayload(payload: JsonValue, svc: PluginServices): Promise<JsonValue> {
  if (!Array.isArray(payload)) {
    return executeRpcRequest(payload, svc);
  }
  if (payload.length === 0) {
    return errorEnvelope(null, CODE_INVALID_REQUEST, 'batch must contain at least one request');
  }
  const responses: JsonObject[] = [];
  for (const entry of payload) {
    responses.push(await executeRpcRequest(entry, svc));
  }
  return responses;
}

/** @brief Statelessly shared revoker for one-shot target leases. */
const NOOP_REVOKER: Revoker = { onRevoke() {} };

// ---------------------------------------------------------------------------
// Client-side helpers.
// ---------------------------------------------------------------------------

/** @brief One parsed HTTP exchange result of the client path. */
interface HttpExchange {
  readonly httpStatus: number;
  readonly payload: JsonValue;
}

/** @brief Rack status vocabulary of the frozen contract; used to validate remote statuses. */
const RACK_STATUSES: ReadonlySet<string> = new Set([
  'invalid_argument',
  'unsupported',
  'not_found',
  'duplicate',
  'invalid_state',
  'busy',
  'stale',
  'limit_exceeded',
  'failed',
  'cycle',
  'deferred',
]);

/**
 * @brief Structural test for the rack HTTP envelope `{ok, result|error}`.
 *
 * @param value Candidate response payload.
 * @return true for std.http-server transport envelopes (bare JSON-RPC excluded).
 */
function isRackHttpEnvelope(value: JsonValue): value is JsonObject {
  return isPlainJsonObject(value) && 'ok' in value && !('jsonrpc' in value);
}

/**
 * @brief Convert a rack HTTP envelope error into the RackError to throw.
 *
 * @param errorValue `error` field of a `{ok:false}` envelope.
 * @return A RackError carrying the transported status (or 'failed' when absent).
 */
function mapRackHttpError(errorValue: JsonValue): RackError {
  const record = isPlainJsonObject(errorValue) ? errorValue : {};
  const candidate = record['status'];
  const status = typeof candidate === 'string' && RACK_STATUSES.has(candidate) ? candidate : 'failed';
  const message = typeof record['message'] === 'string' && record['message'].length > 0
    ? record['message']
    : 'remote rack http endpoint reported an error';
  return new RackError(status as RackStatus, message, record);
}

/**
 * @brief Unwrap the transport layer of one parsed client response.
 *
 * @param payload Parsed HTTP response body.
 * @return The bare JSON-RPC envelope or batch array beneath a rack `{ok:true}` envelope,
 *         the payload itself when already bare.
 * @throws RackError with the transported status for `{ok:false}` rack envelopes.
 */
function unwrapTransport(payload: JsonValue): JsonValue {
  if (!isRackHttpEnvelope(payload)) return payload;
  if (payload['ok'] !== true) {
    throw mapRackHttpError(payload['error'] ?? null);
  }
  return payload['result'] ?? null;
}

/**
 * @brief Convert a remote JSON-RPC error object into the RackError to throw.
 *
 * @param errorValue Remote `error` field.
 * @return A RackError whose status is mapped from the remote code.
 * @note -32601 maps to not_found, -32602 to invalid_argument, everything else to failed.
 */
function mapRemoteError(errorValue: JsonValue): RackError {
  const record = isPlainJsonObject(errorValue) ? errorValue : {};
  const rawCode = record['code'];
  const code =
    typeof rawCode === 'number' && Number.isSafeInteger(rawCode) ? rawCode : CODE_INTERNAL_ERROR;
  const rawMessage = record['message'];
  const message =
    typeof rawMessage === 'string' && rawMessage.length > 0 ? rawMessage : `remote rpc error ${code}`;
  const data = record['data'];
  const detail: JsonObject = data === undefined ? { code } : { code, data };
  return new RackError(codeToStatus(code), message, detail);
}

/**
 * @brief Normalize one batch response entry for the aligned results array.
 *
 * @param entry One remote response entry.
 * @return `{result}` on success, `{error: {code, message, status}}` on failure, or a
 *         synthetic failed entry for malformed documents.
 * @note The discriminator is the top-level key: `error` marks a failed entry, otherwise
 *       the entry's `result` field carries the value. Result values that themselves
 *       contain an `error` key stay nested under `result`, so the shape is unambiguous.
 */
function normalizeBatchEntry(entry: JsonValue): JsonObject {
  if (isPlainJsonObject(entry)) {
    const errorValue = entry['error'];
    if (isPlainJsonObject(errorValue)) {
      const rawCode = errorValue['code'];
      const code =
        typeof rawCode === 'number' && Number.isSafeInteger(rawCode)
          ? rawCode
          : CODE_INTERNAL_ERROR;
      const rawMessage = errorValue['message'];
      const message =
        typeof rawMessage === 'string' && rawMessage.length > 0
          ? rawMessage
          : `remote rpc error ${code}`;
      const error: JsonObject = { code, message, status: codeToStatus(code) };
      const data = errorValue['data'];
      return { error: data === undefined ? error : { ...error, data } };
    }
    if ('result' in entry) {
      return { result: entry['result'] ?? null };
    }
  }
  return { error: { code: CODE_INTERNAL_ERROR, message: 'malformed batch response entry', status: 'failed' } };
}

/**
 * @brief Unwrap a single JSON-RPC response envelope.
 *
 * @param exchange Parsed exchange result.
 * @param id Request id that was sent.
 * @return The remote result value.
 * @throws RackError with the mapped remote status for error envelopes, 'failed' for
 *         malformed responses or id mismatches.
 */
function unwrapSingleResponse(exchange: HttpExchange, id: number): JsonValue {
  const payload = unwrapTransport(exchange.payload);
  if (isPlainJsonObject(payload)) {
    // A remote error envelope wins over id checking: its mapped status is the useful part.
    const errorValue = payload['error'];
    if (isPlainJsonObject(errorValue)) {
      throw mapRemoteError(errorValue);
    }
    if ('result' in payload) {
      if (payload['id'] !== id) {
        throw new RackError('failed', 'rpc response id does not match the request');
      }
      return payload['result'] ?? null;
    }
  }
  throw new RackError('failed', `malformed rpc response (http ${exchange.httpStatus})`);
}

// ---------------------------------------------------------------------------
// Plugin factory.
// ---------------------------------------------------------------------------

/**
 * @brief Create one std.rpc instance with an injected fetch implementation.
 *
 * @param fetchImpl Fetch-like function used for every outgoing client request.
 * @return A RackPlugin built with the SDK helper; each call yields an independent instance.
 * @note Inject a fake in tests; create() binds the global fetch for production use.
 */
export function createWithDeps(fetchImpl: RpcFetchImpl): RackPlugin {
  // Services borrowed during init(); kept because callbacks keep firing after init returns.
  let services: PluginServices | null = null;
  let capWatch: Subscription | null = null;
  // Persistent lease on one std.http-server generation; null while no credential is held.
  let serverLease: LeaseHandle | null = null;
  // Whether the last accepted notification still reports a usable std.http-server.
  let serverUsable = false;
  // Whether the POST /rpc route is currently registered on the leased server.
  let mounted = false;
  // routeId returned by the server's route.add; consumed by route.remove on stop.
  let mountedRouteId: string | number | null = null;
  // Whether start() completed; all business is gated on this flag.
  let active = false;
  // Whether the deferred initial mount is already queued; guards duplicate schedules.
  let initialMountScheduled = false;
  // Client request id counter (JSON-RPC ids are per instance lifetime).
  let requestSeq = 0;
  // Abort controllers of in-flight client requests; stop() aborts them all.
  const clientControllers = new Set<AbortController>();

  /**
   * @brief Fail fast when a method is served before init() or after stop().
   *
   * @return The borrowed services.
   * @throws RackError 'invalid_state' when init() has not run.
   */
  function requireServices(): PluginServices {
    if (services === null) {
      throw new RackError('invalid_state', 'std.rpc has not been initialized');
    }
    return services;
  }

  /**
   * @brief Reserve the next `count` client request ids.
   *
   * @param count Number of ids needed by one client method call.
   * @return The first reserved id; the others follow consecutively.
   */
  function allocateRequestIds(count: number): number {
    requestSeq += count;
    return requestSeq - count + 1;
  }

  /**
   * @brief Register the POST /rpc route on the leased std.http-server.
   *
   * @throws RackError propagated from the gateway when registration fails, or 'failed'
   *         when the route.add result carries no usable routeId.
   */
  async function mountRoute(): Promise<void> {
    const svc = services;
    const lease = serverLease;
    if (svc === null || lease === null || mounted) return;
    const result = await svc.calls.call(lease.credential, HTTP_ROUTE_ADD_METHOD, {
      path: RPC_HTTP_PATH,
      httpMethod: 'POST',
      targetPlugId: RPC_PLUG_ID,
      targetMethod: BRIDGE_METHOD_NAME,
      versionRange: SELF_VERSION_RANGE,
    });
    const record = isPlainJsonObject(result) ? result : null;
    const routeId = record !== null ? record['routeId'] : undefined;
    if (typeof routeId !== 'string' && typeof routeId !== 'number') {
      throw new RackError('failed', 'route.add result must carry a string or number routeId');
    }
    mountedRouteId = routeId;
    mounted = true;
  }

  /**
   * @brief Unregister the POST /rpc route from the leased std.http-server.
   *
   * @throws RackError propagated from the gateway when removal fails, except 'not_found'
   *         which means the route already vanished with a replaced server generation.
   */
  async function unmountRoute(): Promise<void> {
    const svc = services;
    const lease = serverLease;
    const routeId = mountedRouteId;
    if (svc === null || lease === null || !mounted || routeId === null) return;
    try {
      await svc.calls.call(lease.credential, HTTP_ROUTE_REMOVE_METHOD, { routeId });
    } catch (error) {
      if (statusOf(error) !== 'not_found') throw error;
    } finally {
      mountedRouteId = null;
      mounted = false;
    }
  }

  /**
   * @brief Queue the initial mount for the next macrotask after start() returned.
   *
   * @note Registering the route is business work through the gateway, and the frozen host
   *       marks an instance Active only after start() returns; a synchronous mount from
   *       inside start() is rejected with 'invalid_state'. The queued callback re-checks
   *       `active` so a stop() before the tick is a no-op.
   */
  function scheduleInitialMount(): void {
    if (initialMountScheduled) return;
    initialMountScheduled = true;
    setImmediate(() => {
      initialMountScheduled = false;
      if (!active || mounted) return;
      mountRoute().catch((error: unknown) => {
        services?.diag.report('error', 'rpc endpoint mount failed', { status: statusOf(error) });
      });
    });
  }

  /**
   * @brief Stop serving and return the credential when the host revokes the server lease.
   *
   * @param credential Host credential being revoked.
   * @note Unrelated or already returned credentials are ignored. The cached lease is
   *       cleared before the return is attempted, so no new mount can happen after the
   *       notification; a refused ('busy') return is surfaced through diagnostics.
   */
  const serverRevoker: Revoker = {
    async onRevoke(credential: LeaseCredential): Promise<void> {
      const held = serverLease;
      if (held === null || held.credential.handle !== credential.handle) return;
      serverUsable = false;
      serverLease = null;
      mounted = false; // the route died with the revoked server generation
      const outcome = await held.release();
      if (outcome !== 'ok') {
        services?.diag.report('warn', `http-server lease release returned "${outcome}" on revoke`);
      }
    },
  };

  /**
   * @brief Borrow std.http-server whenever a usable generation shows up.
   *
   * @param event Capability availability or withdrawal fact.
   * @note Acquisition happens even before start() (leasing is not business); mounting the
   *       route is business and waits for start(). Failures are reported, not thrown, so
   *       one bad notification cannot fault the watch pipeline.
   */
  const capSink: CapSink = {
    async onCapability(event: CapEvent): Promise<void> {
      if (event.plugId !== HTTP_SERVER_PLUG_ID) return;
      if (!event.available || !acceptsVersion(HTTP_SERVER_RANGE, event.version)) {
        serverUsable = false;
        return;
      }
      serverUsable = true;
      if (serverLease === null && services !== null) {
        try {
          serverLease = services.caps.acquire(HTTP_SERVER_PLUG_ID, HTTP_SERVER_RANGE, serverRevoker);
        } catch (error) {
          services.diag.report('warn', 'std.http-server lease acquisition failed', {
            status: statusOf(error),
          });
          return;
        }
      }
      if (active && !mounted) {
        try {
          await mountRoute();
        } catch (error) {
          services?.diag.report('error', 'rpc endpoint mount failed', { status: statusOf(error) });
        }
      }
    },
  };

  /**
   * @brief Perform one HTTP POST and parse the JSON body under a timeout.
   *
   * @param url Target endpoint URL.
   * @param payload JSON-RPC request document (single or batch).
   * @param timeoutMs Whole-exchange timeout in milliseconds.
   * @return The HTTP status plus the parsed response payload.
   * @throws RackError 'failed' on timeout, network failure or a non-JSON body.
   * @note The timer covers fetch and body read; losing promises get a no-op catch so a
   *       late rejection after a timeout win cannot surface as unhandled.
   */
  async function exchange(url: string, payload: JsonValue, timeoutMs: number): Promise<HttpExchange> {
    const controller = new AbortController();
    clientControllers.add(controller);
    let timer: ReturnType<typeof setTimeout> | undefined;
    const timeoutAt = new Promise<never>((_resolve, reject) => {
      timer = setTimeout(() => {
        controller.abort();
        reject(new RackError('failed', `rpc request to ${url} timed out after ${timeoutMs}ms`));
      }, timeoutMs);
    });
    try {
      const fetchPromise = fetchImpl(url, {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify(payload),
        signal: controller.signal,
      });
      fetchPromise.catch(() => {});
      const response = await Promise.race([fetchPromise, timeoutAt]);
      const textPromise = response.text();
      textPromise.catch(() => {});
      const text = await Promise.race([textPromise, timeoutAt]);
      const parsed = tryParseJson(text);
      if (!parsed.ok) {
        throw new RackError('failed', `rpc response from ${url} is not valid JSON`);
      }
      return { httpStatus: response.status, payload: parsed.value };
    } catch (error) {
      if (isRackError(error)) throw error;
      throw new RackError('failed', `rpc request to ${url} failed: ${describeError(error)}`);
    } finally {
      if (timer !== undefined) clearTimeout(timer);
      clientControllers.delete(controller);
    }
  }

  /**
   * @brief Serve the std.http-server bridge dispatch.
   *
   * @param args Dispatch document `{query, params, headers, body}`; `body` is the already
   *        parsed JSON document, or null for unparseable/empty bodies (std.http-server
   *        semantics). The raw request text is also accepted for other bridges.
   * @return The JSON-RPC response envelope (single object or batch array).
   * @throws RackError 'invalid_argument' when the dispatch document carries no usable body.
   */
  async function handleBridge(args: JsonValue): Promise<JsonValue> {
    const svc = requireServices();
    let payload: JsonValue;
    if (typeof args === 'string') {
      const parsed = tryParseJson(args);
      if (!parsed.ok) {
        return errorEnvelope(null, CODE_PARSE_ERROR, 'request body is not valid JSON');
      }
      payload = parsed.value;
    } else if (isPlainJsonObject(args)) {
      const body = args['body'];
      if (body === undefined || body === null) {
        // Unparseable or empty bodies arrive as null from std.http-server.
        return errorEnvelope(null, CODE_PARSE_ERROR, 'request body is not valid JSON');
      }
      if (typeof body === 'string') {
        const parsed = tryParseJson(body);
        if (!parsed.ok) {
          return errorEnvelope(null, CODE_PARSE_ERROR, 'request body is not valid JSON');
        }
        payload = parsed.value;
      } else {
        // Already-parsed JSON document (object, array or scalar).
        payload = body;
      }
    } else {
      throw new RackError(
        'invalid_argument',
        'bridge args must be the raw request text or an object with a body field',
      );
    }
    return executeRpcPayload(payload, svc);
  }

  /**
   * @brief Serve the `call` client method.
   *
   * @param args `{url, method, params?, timeoutMs?}`.
   * @return The remote result value.
   * @throws RackError 'invalid_argument' for malformed args; mapped statuses for remote
   *         error envelopes; 'failed' for network, timeout and malformed responses.
   */
  async function handleCall(args: JsonValue): Promise<JsonValue> {
    const record = expectArgsObject(args, 'call args');
    const url = expectStringField(record, 'url');
    const method = expectStringField(record, 'method');
    const timeoutMs = readTimeoutMs(record['timeoutMs']);
    const params = normalizeParams(record['params']);
    const id = allocateRequestIds(1);
    const envelope: JsonObject = { jsonrpc: JSONRPC_VERSION, id, method, params };
    return unwrapSingleResponse(await exchange(url, envelope, timeoutMs), id);
  }

  /**
   * @brief Serve the `batch` client method.
   *
   * @param args `{url, calls: [{method, params?}...], timeoutMs?}`.
   * @return An array aligned with the input: `{result}` or `{error: {code, message, status}}`.
   * @throws RackError 'invalid_argument' for malformed args; 'failed' when the exchange or
   *         the top-level response shape is broken (per-entry errors do not throw).
   */
  async function handleBatch(args: JsonValue): Promise<JsonValue> {
    const record = expectArgsObject(args, 'batch args');
    const url = expectStringField(record, 'url');
    const callsValue = record['calls'];
    if (!Array.isArray(callsValue) || callsValue.length === 0) {
      throw new RackError('invalid_argument', 'batch args.calls must be a non-empty array');
    }
    const timeoutMs = readTimeoutMs(record['timeoutMs']);
    const firstId = allocateRequestIds(callsValue.length);
    const requests: JsonObject[] = callsValue.map((entry, index) => {
      if (!isPlainJsonObject(entry)) {
        throw new RackError('invalid_argument', `calls[${index}] must be a JSON object`);
      }
      return {
        jsonrpc: JSONRPC_VERSION,
        id: firstId + index,
        method: expectStringField(entry, 'method'),
        params: normalizeParams(entry['params']),
      };
    });
    const result = await exchange(url, requests, timeoutMs);
    const payload = unwrapTransport(result.payload);
    if (!Array.isArray(payload)) {
      throw new RackError('failed', `batch response must be an array (http ${result.httpStatus})`);
    }
    if (payload.length !== requests.length) {
      throw new RackError(
        'failed',
        `batch response length ${payload.length} does not match ${requests.length} requests`,
      );
    }
    return payload.map(normalizeBatchEntry);
  }

  return definePlugin(
    { plugId: RPC_PLUG_ID, version: RPC_VERSION, init: {} },
    {
      init: (injected) => {
        services = injected;
        // Watch first: the snapshot may already carry the std.http-server fact.
        capWatch = injected.caps.watch(capSink);
      },
      start: async () => {
        services?.caps.publish({ methods: RPC_METHODS });
        active = true;
        if (serverUsable && serverLease !== null && !mounted) {
          // Business calls require an Active consumer; defer until start() has returned.
          scheduleInitialMount();
        }
      },
      stop: async () => {
        active = false;
        serverUsable = false;
        // Abort every in-flight client exchange; their callers receive 'failed'.
        for (const controller of clientControllers) controller.abort();
        capWatch?.unsubscribe();
        capWatch = null;
        try {
          await unmountRoute();
        } catch (error) {
          services?.diag.report('warn', 'rpc endpoint unmount failed', { status: statusOf(error) });
        }
        mounted = false;
        const held = serverLease;
        serverLease = null;
        if (held !== null) {
          const outcome = await held.release();
          if (outcome !== 'ok') {
            services?.diag.report('warn', `http-server lease release returned "${outcome}" on stop`);
          }
        }
      },
      invoke: (request: InvokeRequest) => {
        if (!active) {
          throw new RackError('invalid_state', 'std.rpc is not active');
        }
        if (request.methodId === BRIDGE_METHOD_ID || request.methodName === BRIDGE_METHOD_NAME) {
          return handleBridge(request.args);
        }
        if (request.methodId === CALL_METHOD_ID || request.methodName === CALL_METHOD_NAME) {
          return handleCall(request.args);
        }
        if (request.methodId === BATCH_METHOD_ID || request.methodName === BATCH_METHOD_NAME) {
          return handleBatch(request.args);
        }
        throw new RackError(
          'not_found',
          `unknown rpc method ${request.methodName} (#${request.methodId})`,
        );
      },
    },
  );
}

/**
 * @brief Create one std.rpc instance bound to the global fetch.
 *
 * @return A RackPlugin built with the SDK helper; each call yields an independent instance.
 */
export function create(): RackPlugin {
  return createWithDeps((url, init) => fetch(url, init));
}

// Loader convention: a create(): RackPlugin factory as default or named export.
export default create;
