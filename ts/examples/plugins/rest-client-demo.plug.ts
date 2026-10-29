/**
 * @file rest-client-demo.plug.ts
 * @brief Demo consumer of the std.rest-client standard device: borrows a version-qualified
 *        lease and walks one resource through all five CRUD conventions.
 *
 * @details Consumer end of the plugin-consumes-plugin chain: std.http-client performs the
 *          fetches, std.rest-client borrows it and owns the REST conventions, and this demo
 *          borrows std.rest-client to exercise them. Like the other examples it shares no
 *          class with the provider — only the duplicated constants imported from the plugin
 *          module (identity, version, method name). The demo-crud invoke performs exactly
 *          five gateway round-trips: create, list, get, update and remove, in that order,
 *          and reports the returned target JSON of each step. It is fully hermetic under
 *          fake services: nothing but the gateway view is ever touched.
 *
 *          Flow: init() watches capabilities. An availability fact whose version passes the
 *          explicit closed range acquires a lease. start() publishes the demo method;
 *          onRevoke() clears the cached lease and returns the credential; stop() returns
 *          the lease.
 */

import { acceptsVersion, definePlugin, exactVersion, RackError, statusOf, typedApi } from '../../src/sdk/index.ts';
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
import { REST_CLIENT_PLUG_ID, REST_CLIENT_VERSION } from '../../src/plugins/rest-client/index.ts';

// ---------------------------------------------------------------------------
// Duplicated data contract with std.rest-client: constants only, no shared classes.
// ---------------------------------------------------------------------------

/** @brief The only std.rest-client version this demo accepts. */
const ACCEPTED_RANGE = exactVersion(REST_CLIENT_VERSION);

/** @brief Crud method name of the provider, duplicated from its published contract. */
const PROVIDER_CRUD_METHOD = 'crud';

/**
 * @brief Typed facade over the rest-client lease.
 *
 * @note Argument documents stay JSON values; typedApi packs and forwards them through the
 *       gateway, it never hands out a provider reference.
 */
interface RestClientApi {
  crud(args: JsonObject): Promise<JsonValue>;
}

// ---------------------------------------------------------------------------
// Own published contract.
// ---------------------------------------------------------------------------

/** @brief Canonical plugin identity announced to the host. */
const DEMO_PLUG_ID = 'demo.rest-client';

/** @brief Business version published by this demo. */
const DEMO_VERSION = { major: 1, minor: 0, patch: 0 } as const;

/** @brief Resource name the five demonstrated operations act on. */
const DEMO_RESOURCE = 'things';

/** @brief Item id used by the get/update/remove steps. */
const DEMO_ITEM_ID = 't1';

/** @brief Numeric method id of the demonstration method. */
const DEMO_CRUD_METHOD_ID = 1;

/** @brief Published name of the demonstration method. */
const DEMO_CRUD_METHOD_NAME = 'demo-crud';

/** @brief Methods announced through the capability service during start(). */
const DEMO_METHODS: readonly MethodDesc[] = [
  {
    id: DEMO_CRUD_METHOD_ID,
    name: DEMO_CRUD_METHOD_NAME,
    description:
      'Run create, list, get, update and remove on one resource through std.rest-client.',
    inputSchema: {
      type: 'object',
      properties: {
        baseUrl: { type: 'string', description: 'Absolute http(s) base URL of the API.' },
      },
      required: ['baseUrl'],
    },
    outputSchema: { type: 'array', description: 'One {op, result} entry per CRUD step.' },
  },
];

/** @brief Immutable factory metadata; initializes after the provider it consumes. */
const DEMO_DESC: PluginDescriptor = {
  plugId: DEMO_PLUG_ID,
  version: DEMO_VERSION,
  init: { priority: 20, after: [REST_CLIENT_PLUG_ID] },
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
 * @brief Create one demo consumer instance wired for watch/lease/invoke/revocation.
 *
 * @return A RackPlugin built with the SDK helper; each call yields an independent instance.
 */
export function create(): RackPlugin {
  // Services borrowed during init(); kept because callbacks keep firing after init returns.
  let services: PluginServices | null = null;
  let capWatch: Subscription | null = null;
  // Pointerless lease on one rest-client generation; null while no credential is held.
  let lease: LeaseHandle | null = null;
  // Whether the last accepted notification still reports a usable provider.
  let usable = false;

  /**
   * @brief Stop dependent business and return the credential when the host revokes the lease.
   *
   * @param credential Host credential being revoked.
   * @note Unrelated or already returned credentials are ignored; a refused ('busy') return
   *       is surfaced through diagnostics.
   */
  const revoker: Revoker = {
    async onRevoke(credential: LeaseCredential): Promise<void> {
      if (lease === null || lease.credential.handle !== credential.handle) return;
      usable = false;
      const held = lease;
      lease = null;
      const returned = await held.release();
      if (returned === 'busy') {
        services?.diag.report(
          'warn',
          'rest-client lease revoke returned busy; the return is still owed',
        );
      }
    },
  };

  /**
   * @brief Accept a std.rest-client offer only when its version passes the explicit range.
   *
   * @param event Capability availability or withdrawal fact.
   * @note A withdrawal forbids new business but keeps the credential for onRevoke(); a live
   *       lease is never re-borrowed; acquisition failures are reported, not thrown.
   */
  const capSink: CapSink = {
    onCapability(event: CapEvent): void | Promise<void> {
      if (event.plugId !== REST_CLIENT_PLUG_ID) return;
      if (!event.available) {
        usable = false;
        return;
      }
      if (!acceptsVersion(ACCEPTED_RANGE, event.version)) return;
      if (lease !== null) {
        usable = true;
        return;
      }
      const svc = services;
      if (svc === null) return;
      try {
        lease = svc.caps.acquire(REST_CLIENT_PLUG_ID, ACCEPTED_RANGE, revoker);
        usable = true;
      } catch (error) {
        services?.diag.report('warn', 'rest-client lease acquisition failed', {
          status: statusOf(error),
        });
      }
    },
  };

  /**
   * @brief Run the five CRUD conventions against one resource through the leased provider.
   *
   * @param baseUrl Absolute base URL of the demonstrated API.
   * @return One {op, result} entry per step, in create → list → get → update → remove order.
   * @throws RackError 'invalid_state' when no usable rest-client lease is held, or whatever
   *         a step throws through the gateway.
   */
  async function runCrudWalk(baseUrl: string): Promise<JsonValue> {
    const svc = services;
    const handle = lease;
    if (svc === null || handle === null || !usable) {
      throw new RackError(
        'invalid_state',
        'std.rest-client lease is not currently held; wait for its capability event',
      );
    }
    const rest = typedApi<RestClientApi>(svc.calls, handle);
    const steps: Array<{ op: string; result: JsonValue }> = [];
    steps.push({
      op: 'create',
      result: await rest.crud({
        baseUrl,
        resource: DEMO_RESOURCE,
        item: { name: 'demo thing' },
      }),
    });
    steps.push({
      op: 'list',
      result: await rest.crud({ baseUrl, resource: DEMO_RESOURCE, query: { page: 1 } }),
    });
    steps.push({
      op: 'get',
      result: await rest.crud({ baseUrl, resource: DEMO_RESOURCE, id: DEMO_ITEM_ID }),
    });
    steps.push({
      op: 'update',
      result: await rest.crud({
        baseUrl,
        resource: DEMO_RESOURCE,
        id: DEMO_ITEM_ID,
        item: { name: 'renamed' },
      }),
    });
    steps.push({
      op: 'remove',
      result: await rest.crud({
        baseUrl,
        resource: DEMO_RESOURCE,
        id: DEMO_ITEM_ID,
        method: 'DELETE',
      }),
    });
    return steps;
  }

  return definePlugin(DEMO_DESC, {
    init: (injected) => {
      services = injected;
      // Watch first: the snapshot may already carry the provider fact, leasing is not business.
      capWatch = injected.caps.watch(capSink);
    },
    start: () => {
      services?.caps.publish({ methods: DEMO_METHODS });
    },
    stop: async () => {
      usable = false;
      capWatch?.unsubscribe();
      capWatch = null;
      const held = lease;
      lease = null;
      if (held !== null) await held.release();
    },
    invoke: (request: InvokeRequest): JsonValue | Promise<JsonValue> => {
      const { methodId, methodName, args } = request;
      if (methodId !== DEMO_CRUD_METHOD_ID && methodName !== DEMO_CRUD_METHOD_NAME) {
        throw new RackError(
          'not_found',
          `unknown rest-client-demo method ${methodName} (#${methodId})`,
        );
      }
      if (!isJsonObject(args)) {
        throw new RackError('invalid_argument', 'demo-crud args must be a JSON object');
      }
      const baseUrl = args['baseUrl'];
      if (typeof baseUrl !== 'string' || baseUrl.length === 0) {
        throw new RackError('invalid_argument', 'args.baseUrl must be a non-empty string');
      }
      return runCrudWalk(baseUrl);
    },
  });
}

// Loader convention: a create(): RackPlugin factory as default or named export.
export default create;
