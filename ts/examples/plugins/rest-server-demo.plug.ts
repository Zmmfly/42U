/**
 * @file rest-server-demo.plug.ts
 * @brief Example resource provider: mounts one small REST resource on std.rest-server whose
 *        CRUD routes target this plugin's own methods.
 *
 * @details Third hop of the plugin-consumes-plugin chain: std.http-server serves sockets,
 *          std.rest-server borrows it and owns the REST conventions, and this demo borrows
 *          std.rest-server to publish one resource. The mounted resource points back at the
 *          demo itself: when a request reaches GET /demo/things, std.http-server leases this
 *          plugin one-shot and delivers the unified {query, params, headers, body} envelope
 *          documented in src/plugins/rest-server/README.md. The demo stores no provider
 *          reference anywhere: only the opaque lease credential plus the actual version.
 *
 *          Flow: init() watches capabilities. An availability fact whose version passes the
 *          explicit closed range acquires a lease. start() publishes the three resource
 *          methods and mounts the resource when the lease is already held; a later
 *          acquisition mounts as well once active. onRevoke() clears the cached lease and
 *          returns the credential; stop() unmounts the resource best-effort and returns the
 *          lease.
 */

import {
  acceptsVersion,
  definePlugin,
  exactVersion,
  RackError,
  statusOf,
  typedApi,
} from '../../src/sdk/index.ts';
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
  RackPlugin,
  Revoker,
  Subscription,
} from '../../src/sdk/index.ts';

// ---------------------------------------------------------------------------
// Duplicated data contract with std.rest-server: constants only, no shared classes.
// ---------------------------------------------------------------------------

/** @brief Identity of the only provider this demo depends on. */
const REST_SERVER_PLUG_ID = 'std.rest-server';

/** @brief The only rest-server version this demo accepts. */
const REST_SERVER_VERSION = { major: 1, minor: 0, patch: 0 } as const;

/** @brief Explicit closed range [1.0.0, 1.0.0]; the host adds no policy of its own. */
const REST_SERVER_RANGE = exactVersion(REST_SERVER_VERSION);

/** @brief Method names of the rest-server provider, duplicated from its data contract. */
const RESOURCE_MOUNT_METHOD = 'resource.mount';
const RESOURCE_UNMOUNT_METHOD = 'resource.unmount';

/**
 * @brief Typed facade over the rest-server lease; method names mirror its data contract.
 *
 * @note Argument documents stay JSON values; typedApi packs and forwards them through the
 *       gateway, it never hands out a provider reference.
 */
interface RestServerApi {
  'resource.mount'(request: JsonObject): Promise<JsonValue>;
  'resource.unmount'(request: { pathPrefix: string }): Promise<JsonValue>;
}

// ---------------------------------------------------------------------------
// Own published contract: a tiny "things" resource.
// ---------------------------------------------------------------------------

/** @brief Canonical plugin identity announced to the host. */
const DEMO_PLUG_ID = 'example.rest-demo';

/** @brief Business version published by this demo. */
const DEMO_VERSION = { major: 1, minor: 0, patch: 0 } as const;

/** @brief Path prefix of the demonstrated resource. */
const RESOURCE_PATH_PREFIX = '/demo/things';

/** @brief Numeric method ids of the three resource methods. */
const THING_LIST_METHOD_ID = 1;
const THING_GET_METHOD_ID = 2;
const THING_CREATE_METHOD_ID = 3;

/** @brief Published names of the three resource methods. */
const THING_LIST_METHOD = 'things.list';
const THING_GET_METHOD = 'things.get';
const THING_CREATE_METHOD = 'things.create';

/** @brief Methods announced through the capability service during start(). */
const DEMO_METHODS: readonly MethodDesc[] = [
  {
    id: THING_LIST_METHOD_ID,
    name: THING_LIST_METHOD,
    description: 'List every stored thing; receives the {query, params, headers, body} envelope.',
    inputSchema: { type: 'object' },
    outputSchema: { type: 'array' },
  },
  {
    id: THING_GET_METHOD_ID,
    name: THING_GET_METHOD,
    description: 'Return one thing by params.id or null when absent.',
    inputSchema: { type: 'object' },
    outputSchema: { type: 'object' },
  },
  {
    id: THING_CREATE_METHOD_ID,
    name: THING_CREATE_METHOD,
    description: 'Create one thing from the HTTP request body and return the stored document.',
    inputSchema: { type: 'object' },
    outputSchema: { type: 'object' },
  },
];

/** @brief The single resource declaration mounted on std.rest-server. */
const MOUNT_REQUEST: JsonObject = {
  pathPrefix: RESOURCE_PATH_PREFIX,
  targetPlugId: DEMO_PLUG_ID,
  methods: {
    list: THING_LIST_METHOD,
    get: THING_GET_METHOD,
    create: THING_CREATE_METHOD,
  },
};

/** @brief Immutable factory metadata, including the typed plugin version. */
const DEMO_DESC: PluginDescriptor = {
  plugId: DEMO_PLUG_ID,
  version: DEMO_VERSION,
  // Hard init edge: the rest-server provider must precede this plugin.
  init: { priority: 20, after: [REST_SERVER_PLUG_ID] },
};

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
 * @brief Extract the params object and body value from one gateway envelope.
 *
 * @param args Args document delivered by std.http-server: {query, params, headers, body}.
 * @return The params object (empty object when absent) and the body (null when absent).
 * @throws RackError 'invalid_argument' when the args document is not the expected envelope.
 */
function openEnvelope(args: JsonValue): { params: JsonObject; body: JsonValue } {
  if (!isJsonObject(args)) {
    throw new RackError('invalid_argument', 'expected the {query, params, headers, body} envelope');
  }
  const params = args['params'];
  const body = args['body'];
  return {
    params: isJsonObject(params) ? params : {},
    body: body === undefined ? null : body,
  };
}

/**
 * @brief Create one demo instance that mounts its own small resource on std.rest-server.
 *
 * @return A RackPlugin built with the SDK helper; each call yields an independent instance.
 */
export function create(): RackPlugin {
  // Services borrowed during init(); kept because callbacks keep firing after init returns.
  let services: PluginServices | null = null;
  let capWatch: Subscription | null = null;
  // Pointerless lease on one rest-server generation; null while no credential is held.
  let lease: LeaseHandle | null = null;
  // Whether the last accepted notification still reports a usable provider.
  let restUsable = false;
  // Whether start() completed; mounting is gated on this flag.
  let active = false;
  // Whether the demo resource is currently registered on the rest-server.
  let mounted = false;
  // The tiny resource state served through the gateway.
  const things = new Map<string, JsonObject>();
  let nextThingId = 1;

  /**
   * @brief Stop dependent business and return the credential when the host revokes the lease.
   *
   * @param credential Host credential being revoked.
   * @note Unrelated or already returned credentials are ignored. The cached lease is cleared
   *       before the return is attempted; mounted is cleared as well because the routes died
   *       with the revoked provider generation and a re-borrow re-mounts the resource.
   */
  const revoker: Revoker = {
    async onRevoke(credential: LeaseCredential): Promise<void> {
      if (lease === null || lease.credential.handle !== credential.handle) return;
      restUsable = false;
      mounted = false;
      const held = lease;
      lease = null;
      const returned = await held.release();
      if (returned === 'busy') {
        services?.diag.report(
          'warn',
          'rest-server lease revoke returned busy; the return is still owed',
        );
      }
    },
  };

  /**
   * @brief Mount the demo resource once, best-effort.
   *
   * @param svc Services whose gateway view carries the mount call.
   * @note Failures are reported through diagnostics instead of thrown so a missing provider
   *       cannot fault the demo; mounted stays false and a later re-borrow retries.
   */
  async function mountResource(svc: PluginServices): Promise<void> {
    const handle = lease;
    if (mounted || handle === null || !restUsable) return;
    try {
      const rest = typedApi<RestServerApi>(svc.calls, handle);
      await rest[RESOURCE_MOUNT_METHOD](MOUNT_REQUEST);
      mounted = true;
    } catch (error) {
      svc.diag.report('warn', 'demo resource mount failed', { status: statusOf(error) });
    }
  }

  /**
   * @brief Accept a rest-server offer only when its version passes the explicit range.
   *
   * @param event Capability availability or withdrawal fact.
   * @note A live lease is never re-borrowed; acquisition failures are reported, not thrown,
   *       so one bad notification cannot fault the whole demo.
   */
  const capSink: CapSink = {
    async onCapability(event: CapEvent): Promise<void> {
      if (event.plugId !== REST_SERVER_PLUG_ID) return;
      if (!event.available) {
        restUsable = false;
        return;
      }
      if (!acceptsVersion(REST_SERVER_RANGE, event.version)) {
        restUsable = false;
        return;
      }
      if (lease !== null) {
        restUsable = true;
        return;
      }
      const svc = services;
      if (svc === null) return;
      try {
        lease = svc.caps.acquire(REST_SERVER_PLUG_ID, REST_SERVER_RANGE, revoker);
        restUsable = true;
        if (active) await mountResource(svc);
      } catch (error) {
        svc.diag.report('warn', 'rest-server lease acquisition failed', {
          status: statusOf(error),
        });
      }
    },
  };

  return definePlugin(DEMO_DESC, {
    init: (injected) => {
      services = injected;
      // Watch first: the snapshot may already carry the provider fact, leasing is not business.
      capWatch = injected.caps.watch(capSink);
    },
    start: async () => {
      services?.caps.publish({ methods: DEMO_METHODS });
      active = true;
      const svc = services;
      if (svc !== null) {
        // Mounting is business work; this instance only becomes Active after start()
        // returns, so the initial mount is deferred to the next macrotask.
        setImmediate(() => {
          void mountResource(svc);
        });
      }
    },
    stop: async () => {
      active = false;
      restUsable = false;
      capWatch?.unsubscribe();
      capWatch = null;
      const svc = services;
      const held = lease;
      lease = null;
      if (svc !== null && held !== null && mounted) {
        try {
          const rest = typedApi<RestServerApi>(svc.calls, held);
          await rest[RESOURCE_UNMOUNT_METHOD]({ pathPrefix: RESOURCE_PATH_PREFIX });
        } catch (error) {
          svc.diag.report('warn', 'demo resource unmount failed', { status: statusOf(error) });
        }
      }
      mounted = false;
      if (held !== null) await held.release();
    },
    invoke: (request: InvokeRequest): JsonValue | Promise<JsonValue> => {
      const { methodId, methodName, args } = request;
      if (methodId === THING_LIST_METHOD_ID || methodName === THING_LIST_METHOD) {
        openEnvelope(args); // shape check only; listing ignores the envelope content
        return [...things.values()];
      }
      if (methodId === THING_GET_METHOD_ID || methodName === THING_GET_METHOD) {
        const { params } = openEnvelope(args);
        const id = params['id'];
        if (typeof id !== 'string') {
          throw new RackError('invalid_argument', 'things.get requires a string params.id');
        }
        return things.get(id) ?? null;
      }
      if (methodId === THING_CREATE_METHOD_ID || methodName === THING_CREATE_METHOD) {
        const { body } = openEnvelope(args);
        if (!isJsonObject(body)) {
          throw new RackError('invalid_argument', 'things.create requires a JSON object body');
        }
        const id = `t${nextThingId}`;
        nextThingId += 1;
        // Copy field by field: spreading an index-signature type drags Object.prototype
        // members into the inferred type, which would not stay a JsonValue.
        const record: { [key: string]: JsonValue } = {};
        for (const [key, value] of Object.entries(body)) {
          record[key] = value;
        }
        record['id'] = id;
        const stored: JsonObject = record;
        things.set(id, stored);
        return stored;
      }
      throw new RackError(
        'not_found',
        `unknown rest-demo method ${methodName} (#${methodId})`,
      );
    },
  });
}

// Loader convention: a create(): RackPlugin factory as default or named export.
export default create;
