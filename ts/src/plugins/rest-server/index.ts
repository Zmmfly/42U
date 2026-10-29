/**
 * @file index.ts
 * @brief std.rest-server: RESTful resource conventions layered on a leased std.http-server.
 *
 * @details Second showcase of plugins consuming plugins (after example.consumer). This standard
 *          device owns no socket: it borrows one version-qualified lease on std.http-server,
 *          translates each "mount a resource" declaration into up to five conventional CRUD
 *          routes and forwards every registration or removal through the invoke gateway under
 *          the borrowed credential. Target plugins are never referenced directly; std.http-server
 *          leases them per request and delivers the unified {query, params, headers, body}
 *          envelope documented in README.md. When the provider lease is revoked the cached
 *          handle is cleared and returned; a later availability fact re-borrows and re-mounts
 *          the surviving resources onto the new provider generation.
 */

import {
  acceptsVersion,
  definePlugin,
  isPluginVersion,
  isRackError,
  RackError,
  statusOf,
  typedApi,
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
  PluginDescriptor,
  PluginServices,
  PluginVersion,
  RackPlugin,
  Revoker,
  Subscription,
  VersionRange,
} from '../../sdk/index.ts';

// ---------------------------------------------------------------------------
// Duplicated data contract with std.http-server: constants only, no shared classes.
// ---------------------------------------------------------------------------

/** @brief Identity of the only provider this plugin depends on. */
const HTTP_SERVER_PLUG_ID = 'std.http-server';

/**
 * @brief Accepted closed range [1.0.0, 1.MAX_SAFE] for the http-server lease.
 *
 * @note Upper bounds use the largest safe integers so every 1.x.y provider qualifies; the host
 *       still only honours what this explicit range says, exactly like any other borrower.
 */
const HTTP_SERVER_RANGE: VersionRange = {
  minimum: { major: 1, minor: 0, patch: 0 },
  maximum: { major: 1, minor: Number.MAX_SAFE_INTEGER, patch: Number.MAX_SAFE_INTEGER },
};

/** @brief Method names of the http-server route contract duplicated here; see README.md. */
const ROUTE_ADD_METHOD = 'route.add';
const ROUTE_REMOVE_METHOD = 'route.remove';

// ---------------------------------------------------------------------------
// Own published contract.
// ---------------------------------------------------------------------------

/** @brief Canonical plugin identity announced to the host. */
const REST_SERVER_PLUG_ID = 'std.rest-server';

/** @brief Business version published by this standard plugin. */
const REST_SERVER_VERSION = { major: 1, minor: 0, patch: 0 } as const;

/** @brief Numeric method id of resource.mount. */
const RESOURCE_MOUNT_METHOD_ID = 1;

/** @brief Published name of resource.mount. */
const RESOURCE_MOUNT_METHOD = 'resource.mount';

/** @brief Numeric method id of resource.unmount. */
const RESOURCE_UNMOUNT_METHOD_ID = 2;

/** @brief Published name of resource.unmount. */
const RESOURCE_UNMOUNT_METHOD = 'resource.unmount';

/** @brief Numeric method id of mounted.list. */
const MOUNTED_LIST_METHOD_ID = 3;

/** @brief Published name of mounted.list. */
const MOUNTED_LIST_METHOD = 'mounted.list';

/** @brief Methods announced through the capability service during start(). */
const METHODS: readonly MethodDesc[] = [
  {
    id: RESOURCE_MOUNT_METHOD_ID,
    name: RESOURCE_MOUNT_METHOD,
    description: 'Register the conventional CRUD routes of one resource on std.http-server.',
    inputSchema: { type: 'object' },
    outputSchema: { type: 'object' },
  },
  {
    id: RESOURCE_UNMOUNT_METHOD_ID,
    name: RESOURCE_UNMOUNT_METHOD,
    description: 'Remove every route previously mounted for one path prefix.',
    inputSchema: { type: 'object' },
    outputSchema: { type: 'object' },
  },
  {
    id: MOUNTED_LIST_METHOD_ID,
    name: MOUNTED_LIST_METHOD,
    description: 'List the currently mounted resources in mounting order.',
    inputSchema: { type: 'null' },
    outputSchema: { type: 'array' },
  },
];

/** @brief Immutable factory metadata, including the typed plugin version. */
const REST_SERVER_DESC: PluginDescriptor = {
  plugId: REST_SERVER_PLUG_ID,
  version: REST_SERVER_VERSION,
  // Hard init edge: the http-server provider must precede this plugin.
  init: { priority: 10, after: [HTTP_SERVER_PLUG_ID] },
};

// ---------------------------------------------------------------------------
// Resource conventions.
// ---------------------------------------------------------------------------

/** @brief The five CRUD slots a mounted resource may provide. */
type ResourceSlot = 'list' | 'get' | 'create' | 'update' | 'remove';

/** @brief Slot names in declaration order; also the only accepted methods-object keys. */
const RESOURCE_SLOTS: readonly ResourceSlot[] = ['list', 'get', 'create', 'update', 'remove'];

/** @brief HTTP verbs this convention layer registers. */
type RouteVerb = 'GET' | 'POST' | 'PUT' | 'DELETE';

/** @brief One conventional route: an HTTP verb mapped onto a resource slot. */
interface RouteConvention {
  readonly verb: RouteVerb;
  readonly slot: ResourceSlot;
  /** @brief Whether the route addresses one item ({prefix}/:id) or the whole collection. */
  readonly withId: boolean;
}

/** @brief Convention table in route registration order. */
const CONVENTIONS: readonly RouteConvention[] = [
  { verb: 'GET', slot: 'list', withId: false },
  { verb: 'GET', slot: 'get', withId: true },
  { verb: 'POST', slot: 'create', withId: false },
  { verb: 'PUT', slot: 'update', withId: true },
  { verb: 'DELETE', slot: 'remove', withId: true },
];

/**
 * @brief Typed facade over the http-server lease; method names mirror its data contract.
 *
 * @note Request and result documents stay JSON values; typedApi packs and forwards them through
 *       the gateway and never hands out a provider reference.
 */
interface HttpServerApi {
  'route.add'(request: JsonObject): Promise<JsonValue>;
  'route.remove'(request: JsonObject): Promise<JsonValue>;
}

/** @brief Validated declaration of one mounted resource. */
interface MountSpec {
  /** @brief Normalized path prefix, e.g. "/api/users". */
  readonly pathPrefix: string;
  /** @brief Provider plug id every route of this resource forwards to. */
  readonly targetPlugId: string;
  /** @brief Target method name per provided resource slot; omitted slots mount no route. */
  readonly methods: Readonly<Partial<Record<ResourceSlot, string>>>;
  /** @brief Explicit closed range leased by http-server when calling the target plugin. */
  readonly versionRange?: VersionRange;
}

/** @brief Bookkeeping record of one mounted resource: its declaration plus live route ids. */
interface MountedResource extends MountSpec {
  routeIds: string[];
}

// ---------------------------------------------------------------------------
// Pure argument parsing and JSON helpers.
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
 * @param value Candidate of runtime-unknown shape.
 * @param origin Method label for the diagnostic.
 * @param label Field name included in the diagnostic.
 * @return The value as a JsonObject.
 * @throws RackError 'invalid_argument' for non-object values (arrays and null included).
 */
function asObject(value: JsonValue | undefined, origin: string, label: string): JsonObject {
  if (!isJsonObject(value)) {
    rejectArgument(origin, `${label} must be a JSON object`);
  }
  return value;
}

/**
 * @brief Check whether a string is one of the known resource slots.
 *
 * @param key Candidate methods-object key.
 * @return true for list/get/create/update/remove.
 */
function isResourceSlot(key: string): key is ResourceSlot {
  return (RESOURCE_SLOTS as readonly string[]).includes(key);
}

/**
 * @brief Normalize a path prefix: require a leading slash, trim trailing slashes.
 *
 * @param prefix Raw prefix string; must be non-empty and start with "/".
 * @param origin Method label for the diagnostic.
 * @return The normalized prefix; "/" stays "/".
 * @throws RackError 'invalid_argument' when the prefix does not start with a slash.
 */
function normalizePathPrefix(prefix: string, origin: string): string {
  if (!prefix.startsWith('/')) {
    rejectArgument(origin, 'pathPrefix must start with "/"');
  }
  const trimmed = prefix.replace(/\/+$/, '');
  return trimmed.length === 0 ? '/' : trimmed;
}

/**
 * @brief Copy one version triple into a plain JSON object.
 *
 * @param version Version to copy by value.
 * @return A fresh {major, minor, patch} object safe to embed in gateway documents.
 */
function versionToJson(version: PluginVersion): JsonObject {
  return { major: version.major, minor: version.minor, patch: version.patch };
}

/**
 * @brief Copy one version range into a plain JSON object.
 *
 * @param range Range to copy by value.
 * @return A fresh {minimum, maximum} object safe to embed in gateway documents.
 */
function versionRangeToJson(range: VersionRange): JsonObject {
  return { minimum: versionToJson(range.minimum), maximum: versionToJson(range.maximum) };
}

/**
 * @brief Validate and copy an explicit closed version range from a request document.
 *
 * @param value Candidate value from the args document.
 * @param origin Method label for the diagnostic.
 * @return The copied range.
 * @throws RackError 'invalid_argument' when a triple is malformed or the range is reversed.
 */
function parseVersionRange(value: JsonValue | undefined, origin: string): VersionRange {
  const record = asObject(value, origin, 'versionRange');
  const minimum = record['minimum'];
  const maximum = record['maximum'];
  if (!isPluginVersion(minimum) || !isPluginVersion(maximum)) {
    rejectArgument(origin, 'versionRange must carry valid minimum and maximum triples');
  }
  const range: VersionRange = { minimum, maximum };
  if (!validVersionRange(range)) {
    rejectArgument(origin, 'versionRange minimum must not exceed its maximum');
  }
  return range;
}

/**
 * @brief Build the conventional route path for one convention entry.
 *
 * @param prefix Normalized resource path prefix.
 * @param withId Whether the route addresses one item.
 * @return prefix itself for collection routes, "{prefix}/:id" for item routes.
 */
function conventionPath(prefix: string, withId: boolean): string {
  if (!withId) return prefix;
  return prefix === '/' ? '/:id' : `${prefix}/:id`;
}

/**
 * @brief Validate a resource.mount args document.
 *
 * @param args Raw args delivered by the gateway.
 * @return The validated declaration with a normalized prefix.
 * @throws RackError 'invalid_argument' for a malformed document or an unknown method slot.
 */
function parseMountRequest(args: JsonValue): MountSpec {
  const origin = RESOURCE_MOUNT_METHOD;
  const root = asObject(args, origin, 'args');
  const rawPrefix = root['pathPrefix'];
  if (typeof rawPrefix !== 'string' || rawPrefix.length === 0) {
    rejectArgument(origin, 'pathPrefix must be a non-empty string');
  }
  const pathPrefix = normalizePathPrefix(rawPrefix, origin);
  const rawTarget = root['targetPlugId'];
  if (typeof rawTarget !== 'string' || rawTarget.length === 0) {
    rejectArgument(origin, 'targetPlugId must be a non-empty string');
  }
  const rawMethods = asObject(root['methods'], origin, 'methods');
  const methods: Partial<Record<ResourceSlot, string>> = {};
  for (const key of Object.keys(rawMethods)) {
    if (!isResourceSlot(key)) {
      rejectArgument(origin, `unknown method slot "${key}"`);
    }
    const target = rawMethods[key];
    if (typeof target !== 'string' || target.length === 0) {
      rejectArgument(origin, `methods.${key} must be a non-empty string`);
    }
    methods[key] = target;
  }
  if (RESOURCE_SLOTS.every((slot) => methods[slot] === undefined)) {
    rejectArgument(origin, 'at least one of list/get/create/update/remove must be provided');
  }
  const rawRange = root['versionRange'];
  const versionRange = rawRange === undefined ? undefined : parseVersionRange(rawRange, origin);
  return {
    pathPrefix,
    targetPlugId: rawTarget,
    methods,
    ...(versionRange !== undefined ? { versionRange } : {}),
  };
}

/**
 * @brief Validate a resource.unmount args document.
 *
 * @param args Raw args delivered by the gateway.
 * @return The normalized path prefix to unmount.
 * @throws RackError 'invalid_argument' for a malformed document.
 */
function parseUnmountRequest(args: JsonValue): string {
  const origin = RESOURCE_UNMOUNT_METHOD;
  const root = asObject(args, origin, 'args');
  const rawPrefix = root['pathPrefix'];
  if (typeof rawPrefix !== 'string' || rawPrefix.length === 0) {
    rejectArgument(origin, 'pathPrefix must be a non-empty string');
  }
  return normalizePathPrefix(rawPrefix, origin);
}

/**
 * @brief Extract the route id from one route.add result document.
 *
 * @param result Result returned by std.http-server through the gateway.
 * @return The route id rendered as a string.
 * @throws RackError 'failed' when the result carries no string or number routeId.
 */
function extractRouteId(result: JsonValue): string {
  if (!isJsonObject(result)) {
    throw new RackError('failed', 'route.add result must be a JSON object with a routeId');
  }
  const routeId = result['routeId'];
  if (typeof routeId !== 'string' && typeof routeId !== 'number') {
    throw new RackError('failed', 'route.add result must carry a string or number routeId');
  }
  return String(routeId);
}

// ---------------------------------------------------------------------------
// Plugin instance.
// ---------------------------------------------------------------------------

/**
 * @brief Create one RESTful resource server instance wired for lease/invoke/revocation.
 *
 * @return A RackPlugin built with the SDK helper; each call yields an independent instance.
 */
export function create(): RackPlugin {
  // Services borrowed during init(); kept because callbacks keep firing after init returns.
  let services: PluginServices | null = null;
  let capWatch: Subscription | null = null;
  // Pointerless lease on one http-server generation; null while no credential is held.
  let lease: LeaseHandle | null = null;
  // Whether the last accepted notification still reports a usable provider.
  let serverUsable = false;
  // Whether start() completed; remounting after a re-borrow is gated on this flag.
  let active = false;
  // Mounted resources keyed by normalized path prefix, in mounting order.
  const mounts = new Map<string, MountedResource>();
  // Prefixes whose route.add sequence is still in flight; guards concurrent duplicate mounts.
  const mountingPrefixes = new Set<string>();

  /**
   * @brief Stop dependent business and return the credential when the host revokes the lease.
   *
   * @param credential Host credential being revoked.
   * @note Unrelated or already returned credentials are ignored. The cached lease is cleared
   *       before the return is attempted, so no new business can be issued after the
   *       notification; mounted resources stay recorded and are re-mounted after a re-borrow.
   *       The routes themselves die with the revoked provider generation.
   */
  const revoker: Revoker = {
    async onRevoke(credential: LeaseCredential): Promise<void> {
      if (lease === null || lease.credential.handle !== credential.handle) return;
      serverUsable = false;
      const held = lease;
      lease = null;
      const returned = await held.release();
      if (returned === 'busy') {
        services?.diag.report(
          'warn',
          'http-server lease revoke returned busy; the return is still owed',
        );
      }
    },
  };

  /**
   * @brief Register the conventional routes of one resource declaration.
   *
   * @param api Typed gateway facade bound to the current lease.
   * @param spec Validated resource declaration.
   * @return Route ids in convention order.
   * @throws Whatever route.add throws; routes already added in this sequence are rolled back
   *         with best-effort route.remove calls before the error is rethrown.
   */
  async function addConventionRoutes(api: HttpServerApi, spec: MountSpec): Promise<string[]> {
    const added: string[] = [];
    try {
      for (const convention of CONVENTIONS) {
        const targetMethod = spec.methods[convention.slot];
        if (targetMethod === undefined) continue;
        const result = await api[ROUTE_ADD_METHOD]({
          // Real std.http-server route.add contract: {path, httpMethod, targetPlugId,
          // targetMethod, versionRange?} (see src/plugins/http-server/index.ts).
          path: conventionPath(spec.pathPrefix, convention.withId),
          httpMethod: convention.verb,
          targetPlugId: spec.targetPlugId,
          targetMethod,
          ...(spec.versionRange !== undefined
            ? { versionRange: versionRangeToJson(spec.versionRange) }
            : {}),
        });
        added.push(extractRouteId(result));
      }
    } catch (error) {
      for (const routeId of added) {
        try {
          await api[ROUTE_REMOVE_METHOD]({ routeId });
        } catch (rollbackError) {
          services?.diag.report('warn', 'route rollback after a failed mount failed', {
            pathPrefix: spec.pathPrefix,
            routeId,
            status: statusOf(rollbackError),
          });
        }
      }
      throw error;
    }
    return added;
  }

  /**
   * @brief Re-register every recorded resource onto a freshly borrowed http-server generation.
   *
   * @param handle The newly acquired lease handle.
   * @note Called after a re-borrow; each resource is re-mounted independently so one failing
   *       resource cannot block the others. A resource whose re-mount fails keeps its record
   *       with its previous (now stale) route ids and is reported through diagnostics.
   */
  async function remountAll(handle: LeaseHandle): Promise<void> {
    const svc = services;
    if (svc === null) return;
    const api = typedApi<HttpServerApi>(svc.calls, handle);
    for (const record of [...mounts.values()]) {
      try {
        record.routeIds = await addConventionRoutes(api, record);
      } catch (error) {
        svc.diag.report('warn', 'resource re-mount onto the new http-server lease failed', {
          pathPrefix: record.pathPrefix,
          status: statusOf(error),
        });
      }
    }
  }

  /**
   * @brief Accept an http-server offer only when its version passes the explicit range.
   *
   * @param event Capability availability or withdrawal fact.
   * @note A withdrawal forbids new business but keeps the credential for onRevoke(). A live
   *       lease is never re-borrowed; acquisition failures are reported, not thrown, so one
   *       bad notification cannot fault the whole resource server.
   */
  const capSink: CapSink = {
    async onCapability(event: CapEvent): Promise<void> {
      if (event.plugId !== HTTP_SERVER_PLUG_ID) return;
      if (!event.available) {
        serverUsable = false;
        return;
      }
      if (!acceptsVersion(HTTP_SERVER_RANGE, event.version)) {
        serverUsable = false;
        return;
      }
      if (lease !== null) {
        serverUsable = true;
        return;
      }
      const svc = services;
      if (svc === null) return;
      try {
        const handle = svc.caps.acquire(HTTP_SERVER_PLUG_ID, HTTP_SERVER_RANGE, revoker);
        lease = handle;
        serverUsable = true;
        if (active) await remountAll(handle);
      } catch (error) {
        svc.diag.report('warn', 'http-server lease acquisition failed', {
          status: statusOf(error),
        });
      }
    },
  };

  /**
   * @brief resource.mount: translate one declaration into the conventional routes.
   *
   * @param args Validated by parseMountRequest.
   * @return {mounted: true} once every provided slot is registered.
   * @throws RackError 'invalid_state' without a usable lease, 'duplicate' for a known prefix,
   *         or whatever the gateway throws (after rolling back partial registrations).
   */
  async function mountResource(args: JsonValue): Promise<JsonObject> {
    const spec = parseMountRequest(args);
    const svc = services;
    const handle = lease;
    if (svc === null || handle === null || !serverUsable) {
      throw new RackError(
        'invalid_state',
        'no usable std.http-server lease; resource cannot be mounted',
      );
    }
    if (mounts.has(spec.pathPrefix) || mountingPrefixes.has(spec.pathPrefix)) {
      throw new RackError('duplicate', `pathPrefix ${spec.pathPrefix} is already mounted`);
    }
    mountingPrefixes.add(spec.pathPrefix);
    try {
      const api = typedApi<HttpServerApi>(svc.calls, handle);
      const routeIds = await addConventionRoutes(api, spec);
      const record: MountedResource = { ...spec, routeIds };
      mounts.set(spec.pathPrefix, record);
      return { mounted: true };
    } finally {
      mountingPrefixes.delete(spec.pathPrefix);
    }
  }

  /**
   * @brief resource.unmount: remove every route of one prefix.
   *
   * @param args Validated by parseUnmountRequest.
   * @return {unmounted: true} once the prefix is fully removed.
   * @throws RackError 'not_found' for an unknown prefix, or whatever route.remove throws; the
   *         record keeps its not-yet-removed route ids so the call can be retried.
   * @note Without a usable lease the routes already died with the revoked provider generation,
   *       so only the local bookkeeping is dropped.
   */
  async function unmountResource(args: JsonValue): Promise<JsonObject> {
    const pathPrefix = parseUnmountRequest(args);
    const record = mounts.get(pathPrefix);
    if (record === undefined) {
      throw new RackError('not_found', `no resource mounted at ${pathPrefix}`);
    }
    const svc = services;
    const handle = lease;
    if (svc === null || handle === null || !serverUsable) {
      mounts.delete(pathPrefix);
      return { unmounted: true };
    }
    const api = typedApi<HttpServerApi>(svc.calls, handle);
    while (record.routeIds.length > 0) {
      const routeId = record.routeIds.shift();
      if (routeId === undefined) break;
      try {
        await api[ROUTE_REMOVE_METHOD]({ routeId });
      } catch (error) {
        if (isRackError(error) && error.status === 'not_found') {
          // The route is already gone on the provider side; dropping it locally is correct.
          continue;
        }
        record.routeIds.unshift(routeId);
        throw error;
      }
    }
    if (record.routeIds.length === 0) {
      mounts.delete(pathPrefix);
    }
    return { unmounted: true };
  }

  /**
   * @brief mounted.list: report the mounted resources in mounting order.
   *
   * @return JSON array of {pathPrefix, targetPlugId, methods, versionRange?} objects.
   */
  function listMounts(): JsonValue {
    const listing: JsonValue[] = [];
    for (const record of mounts.values()) {
      listing.push({
        pathPrefix: record.pathPrefix,
        targetPlugId: record.targetPlugId,
        methods: { ...record.methods } as JsonObject,
        ...(record.versionRange !== undefined
          ? { versionRange: versionRangeToJson(record.versionRange) }
          : {}),
      });
    }
    return listing;
  }

  return definePlugin(REST_SERVER_DESC, {
    init: (injected) => {
      services = injected;
      // Watch first: the snapshot may already carry the provider fact, leasing is not business.
      capWatch = injected.caps.watch(capSink);
    },
    start: () => {
      services?.caps.publish({ methods: METHODS });
      active = true;
    },
    stop: async () => {
      active = false;
      const wasUsable = serverUsable;
      serverUsable = false;
      capWatch?.unsubscribe();
      capWatch = null;
      const svc = services;
      const held = lease;
      lease = null;
      // Unmount every resource through the live lease before returning it; when the provider
      // is already withdrawn the routes died with it and only bookkeeping remains.
      if (svc !== null && held !== null && wasUsable) {
        const api = typedApi<HttpServerApi>(svc.calls, held);
        for (const record of mounts.values()) {
          for (const routeId of record.routeIds) {
            try {
              await api[ROUTE_REMOVE_METHOD]({ routeId });
            } catch (error) {
              svc.diag.report('warn', 'route removal during stop failed', {
                pathPrefix: record.pathPrefix,
                routeId,
                status: statusOf(error),
              });
            }
          }
        }
      }
      mounts.clear();
      if (held !== null) await held.release();
    },
    invoke: (request: InvokeRequest): JsonValue | Promise<JsonValue> => {
      const { methodId, methodName, args } = request;
      if (methodId === RESOURCE_MOUNT_METHOD_ID || methodName === RESOURCE_MOUNT_METHOD) {
        return mountResource(args);
      }
      if (methodId === RESOURCE_UNMOUNT_METHOD_ID || methodName === RESOURCE_UNMOUNT_METHOD) {
        return unmountResource(args);
      }
      if (methodId === MOUNTED_LIST_METHOD_ID || methodName === MOUNTED_LIST_METHOD) {
        return listMounts();
      }
      throw new RackError(
        'not_found',
        `unknown rest-server method ${methodName} (#${methodId})`,
      );
    },
  });
}

// Loader convention: a create(): RackPlugin factory as default or named export.
export default create;
