/**
 * @file index.ts
 * @brief std.rest-client: RESTful conventions layered on a leased std.http-client.
 *
 * @details Showcase of plugins consuming plugins (same family as example.consumer and
 *          std.rest-server). This standard device owns no HTTP code: it watches for
 *          std.http-client, borrows one version-qualified lease on the closed range
 *          [1.0.0, 1.MAX_SAFE] and performs every request through the invoke gateway under
 *          the borrowed credential via the typedApi proxy. The provider is never referenced
 *          directly; only the opaque credential plus the actual version are held. When the
 *          lease is revoked the cached handle is cleared and returned, and a later
 *          availability fact re-borrows a fresh generation.
 *
 *          Two methods are published: 'resource' turns one {baseUrl, path, method, query,
 *          body, headers, expectOk} document into one std.http-client request call (query
 *          serialized with URLSearchParams, body forwarded as the request json field) and
 *          returns the response's JSON body; 'crud' maps the conventional REST operations
 *          (list/get/create/update/remove) onto resource URLs and is implemented entirely on
 *          top of 'resource'.
 */

import { acceptsVersion, definePlugin, RackError, statusOf, typedApi } from '../../sdk/index.ts';
import type {
  CapEvent,
  CapSink,
  InvokeRequest,
  JsonObject,
  JsonValue,
  LeaseCredential,
  LeaseHandle,
  MethodDesc,
  PluginDescriptor,
  PluginServices,
  PluginVersion,
  RackPlugin,
  Revoker,
  Subscription,
  VersionRange,
} from '../../sdk/index.ts';

// ---------------------------------------------------------------------------
// Duplicated data contract with std.http-client: constants only, no shared classes.
// ---------------------------------------------------------------------------

/** @brief Identity of the only provider this plugin depends on. */
const HTTP_CLIENT_PLUG_ID = 'std.http-client';

/**
 * @brief Accepted closed range [1.0.0, 1.MAX_SAFE] for the http-client lease.
 *
 * @note Upper bounds use the largest safe integers so every 1.x.y provider qualifies; the
 *       host still only honours what this explicit range says, exactly like any other
 *       borrower. A future 2.0.0 provider is out of range until this plugin is updated.
 */
const HTTP_CLIENT_RANGE: VersionRange = {
  minimum: { major: 1, minor: 0, patch: 0 },
  maximum: { major: 1, minor: Number.MAX_SAFE_INTEGER, patch: Number.MAX_SAFE_INTEGER },
};

/** @brief Method name of the http-client request contract duplicated here; see README.md. */
const HTTP_REQUEST_METHOD = 'request';

/**
 * @brief Typed facade over the http-client lease; method names mirror its data contract.
 *
 * @note Request and result documents stay JSON values; typedApi packs and forwards them
 *       through the gateway and never hands out a provider reference.
 */
interface HttpClientApi {
  request(args: JsonObject): Promise<JsonValue>;
}

// ---------------------------------------------------------------------------
// Own published contract.
// ---------------------------------------------------------------------------

/** @brief Canonical plugin identity announced to the host. */
export const REST_CLIENT_PLUG_ID = 'std.rest-client';

/** @brief Business version published by this standard plugin. */
export const REST_CLIENT_VERSION: PluginVersion = { major: 1, minor: 0, patch: 0 };

/** @brief Numeric method id of resource. */
const RESOURCE_METHOD_ID = 1;

/** @brief Published name of resource. */
const RESOURCE_METHOD_METHOD = 'resource';

/** @brief Numeric method id of crud. */
const CRUD_METHOD_ID = 2;

/** @brief Published name of crud. */
const CRUD_METHOD = 'crud';

/** @brief JSON-schema-shaped description of the resource args document. */
const RESOURCE_INPUT_SCHEMA: JsonValue = {
  type: 'object',
  properties: {
    baseUrl: {
      type: 'string',
      description: 'Absolute http(s) base URL; its path is kept as a directory prefix.',
    },
    path: {
      type: 'string',
      description: 'Path appended after the baseUrl path; each segment is percent-encoded.',
    },
    method: { type: 'string', description: 'HTTP verb forwarded to std.http-client; default GET.' },
    query: {
      type: 'object',
      description:
        'String/number/boolean values serialized with URLSearchParams; replaces the baseUrl query.',
    },
    body: { description: 'JSON request body forwarded as the request json field.' },
    headers: { type: 'object', additionalProperties: { type: 'string' } },
    expectOk: { type: 'boolean', description: 'Fail on non-2xx responses; default true.' },
  },
  required: ['baseUrl'],
};

/** @brief JSON-schema-shaped description of the crud args document. */
const CRUD_INPUT_SCHEMA: JsonValue = {
  type: 'object',
  properties: {
    baseUrl: { type: 'string', description: 'Absolute http(s) base URL of the API.' },
    resource: { type: 'string', description: 'Resource name placed after the baseUrl path.' },
    id: {
      description: 'Item identifier; string or number, percent-encoded into the path.',
    },
    item: { description: 'Request body for create/update, forwarded as the request json field.' },
    query: {
      type: 'object',
      description: 'String/number/boolean values serialized with URLSearchParams.',
    },
    method: {
      type: 'string',
      description: 'Optional verb override: GET, POST, PUT or DELETE; default inferred.',
    },
  },
  required: ['baseUrl', 'resource'],
};

/** @brief JSON-schema-shaped description of every method result: the response JSON body. */
const TARGET_JSON_OUTPUT_SCHEMA: JsonValue = {
  description: 'The 2xx response JSON body, or null when the response carried no JSON body.',
};

/** @brief Methods announced through the capability service during start(). */
const METHODS: readonly MethodDesc[] = [
  {
    id: RESOURCE_METHOD_ID,
    name: RESOURCE_METHOD_METHOD,
    description:
      'Perform one REST request through std.http-client and return the response JSON body.',
    inputSchema: RESOURCE_INPUT_SCHEMA,
    outputSchema: TARGET_JSON_OUTPUT_SCHEMA,
  },
  {
    id: CRUD_METHOD_ID,
    name: CRUD_METHOD,
    description:
      'Map one conventional CRUD operation (list/get/create/update/remove) onto a resource URL.',
    inputSchema: CRUD_INPUT_SCHEMA,
    outputSchema: TARGET_JSON_OUTPUT_SCHEMA,
  },
];

/** @brief Immutable factory metadata, including the typed plugin version. */
const REST_CLIENT_DESC: PluginDescriptor = {
  plugId: REST_CLIENT_PLUG_ID,
  version: REST_CLIENT_VERSION,
  // Hard init edge: the http-client provider must precede this plugin.
  init: { priority: 10, after: [HTTP_CLIENT_PLUG_ID] },
};

// ---------------------------------------------------------------------------
// Resource and CRUD conventions.
// ---------------------------------------------------------------------------

/**
 * @brief The five conventional CRUD operations of the crud method.
 *
 * @note Mirrors the REST routing conventions of std.rest-server: list GET {res}, get GET
 *       {res}/:id, create POST {res}, update PUT {res}/:id, remove DELETE {res}/:id.
 */
type CrudOp = 'list' | 'get' | 'create' | 'update' | 'remove';

/** @brief HTTP verb each CRUD operation is performed with. */
const CRUD_VERBS: Readonly<Record<CrudOp, string>> = {
  list: 'GET',
  get: 'GET',
  create: 'POST',
  update: 'PUT',
  remove: 'DELETE',
};

/** @brief One fully validated request ready to be forwarded as one http-client call. */
interface ResourceCall {
  /** @brief Final absolute URL: baseUrl path + encoded path + serialized query. */
  readonly url: string;
  /** @brief HTTP verb; forwarded verbatim to std.http-client. */
  readonly method: string;
  /** @brief Header map forwarded as-is; absent means no header field is sent. */
  readonly headers?: JsonObject;
  /** @brief JSON body forwarded as the request json field; absent means no body. */
  readonly body?: JsonValue;
  /** @brief Resolved expectOk flag; always forwarded because the provider default differs. */
  readonly expectOk: boolean;
}

// ---------------------------------------------------------------------------
// Pure argument parsing and URL construction helpers.
// ---------------------------------------------------------------------------

/**
 * @brief Reject a malformed request document.
 *
 * @param origin Method label included in the diagnostic.
 * @param message Rule description.
 * @throws RackError 'invalid_argument' always.
 */
function rejectArgument(origin: string, message: string): never {
  throw new RackError('invalid_argument', `${origin}: ${message}`);
}

/**
 * @brief Check whether a value is a JSON object (not null and not an array).
 *
 * @param value Candidate of runtime-unknown shape.
 * @return true for plain JSON objects.
 * @note Hand-written instead of inline checks because Array.isArray cannot narrow the
 *       readonly JsonArray out of JsonValue unions under noUncheckedIndexedAccess.
 */
function isJsonObject(value: unknown): value is JsonObject {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

/**
 * @brief Narrow a JSON value to a JSON object.
 *
 * @param value Candidate args document.
 * @param origin Method label included in the diagnostic.
 * @return The value as a JsonObject.
 * @throws RackError 'invalid_argument' for non-object values (arrays and null included).
 */
function asObject(value: JsonValue, origin: string): JsonObject {
  if (!isJsonObject(value)) {
    rejectArgument(origin, 'args must be a JSON object');
  }
  return value;
}

/**
 * @brief Serialize one query object into URLSearchParams.
 *
 * @param query Query object with string, number or boolean values.
 * @param origin Method label included in the diagnostic.
 * @return The serialized search string without the leading '?'.
 * @throws RackError 'invalid_argument' when a value is null, an object or an array.
 * @note Number and boolean values are coerced with String(); key order follows insertion
 *       order, which keeps the produced URL deterministic.
 */
function toSearchParams(query: JsonObject, origin: string): URLSearchParams {
  const params = new URLSearchParams();
  for (const [key, value] of Object.entries(query)) {
    if (typeof value === 'string' || typeof value === 'number' || typeof value === 'boolean') {
      params.append(key, String(value));
    } else {
      rejectArgument(origin, `args.query["${key}"] must be a string, number or boolean`);
    }
  }
  return params;
}

/**
 * @brief Percent-encode one path while preserving its segment separators.
 *
 * @param path Raw path; a leading slash is added when missing.
 * @return The encoded path starting with '/'.
 * @note Each segment between '/' separators is encoded with encodeURIComponent so ids like
 *       "a b" become "a%20b" while the structure stays intact.
 */
function encodePath(path: string): string {
  const withSlash = path.startsWith('/') ? path : `/${path}`;
  return withSlash
    .split('/')
    .map((segment) => encodeURIComponent(segment))
    .join('/');
}

/**
 * @brief Build the final target URL from the base URL, encoded path and query.
 *
 * @param baseUrl Absolute base URL; its pathname is kept as a directory prefix.
 * @param encodedPath Optional already percent-encoded path appended after the base pathname;
 *        callers own the encoding so segments like "a/b" can stay one "a%2Fb" segment.
 * @param query Optional query object that replaces the baseUrl query when provided.
 * @param origin Method label included in the diagnostic.
 * @return The normalized absolute URL string.
 * @throws RackError 'invalid_argument' when the baseUrl does not parse as a URL.
 * @note Trailing slashes of the base pathname are trimmed so "https://x/" and "https://x"
 *       produce the same target. An absent query keeps the baseUrl query; a provided (even
 *       empty) query object replaces it.
 */
function buildTargetUrl(
  baseUrl: string,
  encodedPath: string | undefined,
  query: JsonObject | undefined,
  origin: string,
): string {
  let url: URL;
  try {
    url = new URL(baseUrl);
  } catch {
    rejectArgument(origin, `args.baseUrl is not a valid URL: ${baseUrl}`);
  }
  const directory = url.pathname.replace(/\/+$/, '');
  url.pathname = `${directory}${encodedPath ?? ''}`;
  if (query !== undefined) {
    url.search = toSearchParams(query, origin).toString();
  }
  return url.href;
}

/**
 * @brief Validate the shared query field of both methods.
 *
 * @param raw Raw args field value; undefined means "no query".
 * @param origin Method label included in the diagnostic.
 * @return The query object, or undefined when absent.
 * @throws RackError 'invalid_argument' when present but not a JSON object of scalar values.
 */
function parseQuery(raw: JsonValue | undefined, origin: string): JsonObject | undefined {
  if (raw === undefined) return undefined;
  if (!isJsonObject(raw)) {
    rejectArgument(origin, 'args.query must be a JSON object');
  }
  // Validate value shapes eagerly so malformed queries fail before any gateway call.
  toSearchParams(raw, origin);
  return raw;
}

/**
 * @brief Validate the headers field of the resource method.
 *
 * @param raw Raw args field value; undefined means "no headers".
 * @param origin Method label included in the diagnostic.
 * @return The header map, or undefined when absent.
 * @throws RackError 'invalid_argument' when present but not an object of non-empty string
 *         names to string values.
 */
function parseHeaders(raw: JsonValue | undefined, origin: string): JsonObject | undefined {
  if (raw === undefined) return undefined;
  if (!isJsonObject(raw)) {
    rejectArgument(origin, 'args.headers must be a JSON object');
  }
  for (const [key, value] of Object.entries(raw)) {
    if (key.length === 0) {
      rejectArgument(origin, 'args.headers names must be non-empty strings');
    }
    if (typeof value !== 'string') {
      rejectArgument(origin, `args.headers["${key}"] must be a string`);
    }
  }
  return raw;
}

/**
 * @brief Validate and resolve one resource request document.
 *
 * @param args Raw resource args: {baseUrl, path?, method?, query?, body?, headers?, expectOk?}.
 * @return The fully resolved request ready for one http-client call.
 * @throws RackError 'invalid_argument' for every shape violation documented in the README.
 */
function parseResourceRequest(args: JsonValue): ResourceCall {
  const origin = 'resource';
  const obj = asObject(args, origin);

  const baseUrl = obj['baseUrl'];
  if (typeof baseUrl !== 'string' || baseUrl.length === 0) {
    rejectArgument(origin, 'args.baseUrl must be a non-empty string');
  }
  const path = obj['path'];
  if (path !== undefined && (typeof path !== 'string' || path.length === 0)) {
    rejectArgument(origin, 'args.path must be a non-empty string');
  }
  const method = obj['method'];
  if (method !== undefined && (typeof method !== 'string' || method.length === 0)) {
    rejectArgument(origin, 'args.method must be a non-empty string');
  }
  const expectOkRaw = obj['expectOk'];
  if (expectOkRaw !== undefined && typeof expectOkRaw !== 'boolean') {
    rejectArgument(origin, 'args.expectOk must be a boolean');
  }
  const headers = parseHeaders(obj['headers'], origin);
  const hasBody = Object.hasOwn(obj, 'body');

  return {
    url: buildTargetUrl(
      baseUrl,
      path === undefined ? undefined : encodePath(path),
      parseQuery(obj['query'], origin),
      origin,
    ),
    method: method ?? 'GET',
    ...(headers !== undefined ? { headers } : {}),
    ...(hasBody ? { body: obj['body'] } : {}),
    expectOk: expectOkRaw ?? true,
  };
}

/** @brief Field requirements of each CRUD operation; drives precise validation errors. */
interface CrudOpRule {
  /** @brief Whether the operation addresses one item with an id path segment. */
  readonly withId: boolean;
  /** @brief Whether the operation sends the item as the request body. */
  readonly withItem: boolean;
}

/** @brief Convention table: which fields each CRUD operation requires. */
const CRUD_RULES: Readonly<Record<CrudOp, CrudOpRule>> = {
  list: { withId: false, withItem: false },
  get: { withId: true, withItem: false },
  create: { withId: false, withItem: true },
  update: { withId: true, withItem: true },
  remove: { withId: true, withItem: false },
};

/**
 * @brief Resolve the CRUD operation from the optional verb override and field presence.
 *
 * @param method Uppercased verb when args.method was provided, otherwise undefined.
 * @param hasId Whether args carried an id.
 * @param hasItem Whether args carried an item.
 * @param origin Method label included in the diagnostic.
 * @return The selected CRUD operation.
 * @throws RackError 'invalid_argument' when the verb is not one of GET/POST/PUT/DELETE or
 *         the provided fields contradict the selected convention.
 * @note Without an explicit verb the operation is inferred: item presence selects
 *       create/update (POST/PUT), absence selects list/get (GET). Deletion therefore needs
 *       an explicit method "DELETE" because "id without item" already means get.
 */
function resolveCrudOp(
  method: string | undefined,
  hasId: boolean,
  hasItem: boolean,
  origin: string,
): CrudOp {
  if (method === 'GET' || method === undefined) {
    if (method === 'GET' && hasItem) {
      rejectArgument(origin, 'method GET cannot carry an item body');
    }
    if (method === undefined && hasItem) return hasId ? 'update' : 'create';
    return hasId ? 'get' : 'list';
  }
  if (method === 'POST' || method === 'PUT') {
    const op: CrudOp = method === 'POST' ? 'create' : 'update';
    if (!hasItem) {
      rejectArgument(origin, `method ${method} requires an item body`);
    }
    return op;
  }
  if (method === 'DELETE') return 'remove';
  rejectArgument(origin, 'args.method must be one of GET, POST, PUT or DELETE');
}

/**
 * @brief Validate and resolve one crud request document into a resource request.
 *
 * @param args Raw crud args: {baseUrl, resource, id?, item?, query?, method?}.
 * @return The fully resolved request ready for one http-client call.
 * @throws RackError 'invalid_argument' for every shape violation documented in the README.
 */
function parseCrudRequest(args: JsonValue): ResourceCall {
  const origin = 'crud';
  const obj = asObject(args, origin);

  const baseUrl = obj['baseUrl'];
  if (typeof baseUrl !== 'string' || baseUrl.length === 0) {
    rejectArgument(origin, 'args.baseUrl must be a non-empty string');
  }
  const resource = obj['resource'];
  if (typeof resource !== 'string' || resource.length === 0) {
    rejectArgument(origin, 'args.resource must be a non-empty string');
  }
  const idRaw = obj['id'];
  if (
    idRaw !== undefined &&
    !((typeof idRaw === 'string' && idRaw.length > 0) || (typeof idRaw === 'number' && Number.isFinite(idRaw)))
  ) {
    rejectArgument(origin, 'args.id must be a non-empty string or a finite number');
  }
  const methodRaw = obj['method'];
  if (methodRaw !== undefined && (typeof methodRaw !== 'string' || methodRaw.length === 0)) {
    rejectArgument(origin, 'args.method must be a non-empty string');
  }
  const method = methodRaw === undefined ? undefined : methodRaw.toUpperCase();
  const hasId = idRaw !== undefined;
  const hasItem = Object.hasOwn(obj, 'item');
  const op = resolveCrudOp(method, hasId, hasItem, origin);

  const rule = CRUD_RULES[op];
  if (rule.withId && !hasId) {
    rejectArgument(origin, `${op} requires an id`);
  }
  if (!rule.withId && hasId) {
    rejectArgument(origin, `${op} does not take an id`);
  }
  if (rule.withItem && !hasItem) {
    rejectArgument(origin, `${op} requires an item body`);
  }
  if (!rule.withItem && hasItem) {
    rejectArgument(origin, `${op} does not take an item body`);
  }

  const id = idRaw === undefined ? undefined : String(idRaw);
  // Each part is encoded as a single segment: an id containing "/" stays one %2F segment.
  const encodedPath =
    id === undefined
      ? `/${encodeURIComponent(resource)}`
      : `/${encodeURIComponent(resource)}/${encodeURIComponent(id)}`;
  const query = parseQuery(obj['query'], origin);
  return {
    url: buildTargetUrl(baseUrl, encodedPath, query, origin),
    method: CRUD_VERBS[op],
    ...(hasItem && rule.withItem ? { body: obj['item'] } : {}),
    expectOk: true,
  };
}

// ---------------------------------------------------------------------------
// Instance factory.
// ---------------------------------------------------------------------------

/**
 * @brief Create one std.rest-client instance wired for watch/lease/invoke/revocation.
 *
 * @return A RackPlugin built with the SDK helper; each call yields an independent instance.
 * @note stop() unsubscribes the capability watch and actively returns the lease; invoke
 *       without a usable lease rejects with RackError 'invalid_state'.
 */
export function create(): RackPlugin {
  // Services borrowed during init(); kept because callbacks keep firing after init returns.
  let services: PluginServices | null = null;
  let capWatch: Subscription | null = null;
  // Pointerless lease on one http-client generation; null while no credential is held.
  let lease: LeaseHandle | null = null;
  // Whether the last accepted notification still reports a usable provider.
  let clientUsable = false;

  /**
   * @brief Stop dependent business and return the credential when the host revokes the lease.
   *
   * @param credential Host credential being revoked.
   * @note Unrelated or already returned credentials are ignored. The cached lease is cleared
   *       before the return is attempted, so no new request can be issued after the
   *       notification; a refused ('busy') return is surfaced through diagnostics. The next
   *       accepted availability fact re-borrows a fresh provider generation.
   */
  const revoker: Revoker = {
    async onRevoke(credential: LeaseCredential): Promise<void> {
      if (lease === null || lease.credential.handle !== credential.handle) return;
      clientUsable = false;
      const held = lease;
      lease = null;
      const returned = await held.release();
      if (returned === 'busy') {
        services?.diag.report(
          'warn',
          'http-client lease revoke returned busy; the return is still owed',
        );
      }
    },
  };

  /**
   * @brief Accept an http-client offer only when its version passes the explicit range.
   *
   * @param event Capability availability or withdrawal fact.
   * @note A withdrawal forbids new business but keeps the credential for onRevoke(). A live
   *       lease is never re-borrowed; acquisition failures are reported, not thrown, so one
   *       bad notification cannot fault the whole REST client.
   */
  const capSink: CapSink = {
    onCapability(event: CapEvent): void | Promise<void> {
      if (event.plugId !== HTTP_CLIENT_PLUG_ID) return;
      if (!event.available) {
        clientUsable = false;
        return;
      }
      if (!acceptsVersion(HTTP_CLIENT_RANGE, event.version)) {
        clientUsable = false;
        return;
      }
      if (lease !== null) {
        clientUsable = true;
        return;
      }
      const svc = services;
      if (svc === null) return;
      try {
        lease = svc.caps.acquire(HTTP_CLIENT_PLUG_ID, HTTP_CLIENT_RANGE, revoker);
        clientUsable = true;
      } catch (error) {
        services?.diag.report('warn', 'http-client lease acquisition failed', {
          status: statusOf(error),
        });
      }
    },
  };

  /**
   * @brief Perform one resolved request through the gateway and extract the target JSON.
   *
   * @param call Fully validated request produced by parseResourceRequest / parseCrudRequest.
   * @return The 2xx response JSON body, or null when the response carried no JSON body.
   * @throws RackError 'invalid_state' when no usable http-client lease is held, or whatever
   *         the gateway throws (non-2xx failures surface as 'failed' from expectOk).
   */
  async function performRequest(call: ResourceCall): Promise<JsonValue> {
    const svc = services;
    const handle = lease;
    if (svc === null || handle === null || !clientUsable) {
      throw new RackError('invalid_state', 'std.http-client 未上线');
    }
    // The provider defaults expectOk to false; the resolved value is always forwarded.
    const args: Record<string, JsonValue> = {
      url: call.url,
      method: call.method,
      expectOk: call.expectOk,
    };
    if (call.headers !== undefined) args['headers'] = call.headers;
    if (call.body !== undefined) args['json'] = call.body;
    const http = typedApi<HttpClientApi>(svc.calls, handle);
    const envelope = await http.request(args as JsonObject);
    if (isJsonObject(envelope) && envelope['json'] !== undefined) {
      return envelope['json'];
    }
    return null;
  }

  return definePlugin(REST_CLIENT_DESC, {
    init: (injected) => {
      services = injected;
      // Watch first: the snapshot may already carry the provider fact, leasing is not business.
      capWatch = injected.caps.watch(capSink);
    },
    start: () => {
      services?.caps.publish({ methods: METHODS });
    },
    stop: async () => {
      clientUsable = false;
      capWatch?.unsubscribe();
      capWatch = null;
      const held = lease;
      lease = null;
      if (held !== null) await held.release();
    },
    invoke: (request: InvokeRequest): JsonValue | Promise<JsonValue> => {
      const { methodId, methodName, args } = request;
      if (methodId === RESOURCE_METHOD_ID || methodName === RESOURCE_METHOD_METHOD) {
        return performRequest(parseResourceRequest(args));
      }
      if (methodId === CRUD_METHOD_ID || methodName === CRUD_METHOD) {
        return performRequest(parseCrudRequest(args));
      }
      throw new RackError(
        'not_found',
        `unknown rest-client method ${methodName} (#${methodId})`,
      );
    },
  });
}

// Loader convention: a create(): RackPlugin factory as default or named export.
export default create;
