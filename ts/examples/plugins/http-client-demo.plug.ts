/**
 * @file http-client-demo.plug.ts
 * @brief Demo consumer of the std.http-client standard device: borrows a version-qualified
 *        lease and answers fetch-json with one gateway round-trip.
 *
 * @details The demo owns no HTTP code: it watches capabilities, acquires an exact [1.0.0,1.0.0]
 *          lease on std.http-client once that provider appears, and forwards every fetch-json
 *          {url} invoke through the typedApi proxy as one request call (GET, expectOk). Like
 *          the other examples it shares no class with the provider — only the duplicated
 *          constants imported from the plugin module (identity, version, method name). The
 *          one-shot call means the credential is held across calls but every invoke performs a
 *          single gateway round-trip; revocation and withdrawal clear the lease immediately.
 */

import { acceptsVersion, definePlugin, exactVersion, RackError, statusOf, typedApi } from '../../src/sdk/index.ts';
import type {
  CapEvent,
  CapSink,
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
import { HTTP_CLIENT_PLUG_ID, HTTP_CLIENT_VERSION } from '../../src/plugins/http-client/index.ts';

// ---------------------------------------------------------------------------
// Duplicated data contract with std.http-client: constants only, no shared classes.
// ---------------------------------------------------------------------------

/** @brief The only std.http-client version this demo accepts. */
const ACCEPTED_RANGE = exactVersion(HTTP_CLIENT_VERSION);

/** @brief Request method name of the provider, duplicated from its published contract. */
const PROVIDER_REQUEST_METHOD = 'request';

/**
 * @brief Typed facade over the http-client lease.
 *
 * @note Argument documents stay JSON values; typedApi packs and forwards them through the
 *       gateway, it never hands out a provider reference.
 */
interface HttpClientApi {
  request(args: JsonObject): Promise<JsonObject>;
}

// ---------------------------------------------------------------------------
// Own published contract.
// ---------------------------------------------------------------------------

/** @brief Canonical plugin identity announced to the host. */
const DEMO_PLUG_ID = 'demo.http-client';

/** @brief Business version published by this demo. */
const DEMO_VERSION = { major: 1, minor: 0, patch: 0 } as const;

/** @brief Per-attempt timeout the demo asks the provider for. */
const DEMO_TIMEOUT_MS = 8000;

/** @brief Numeric method id of the demonstration method. */
const FETCH_JSON_METHOD_ID = 1;

/** @brief Published name of the demonstration method. */
const FETCH_JSON_METHOD_NAME = 'fetch-json';

/** @brief Methods announced through the capability service during start(). */
const DEMO_METHODS: readonly MethodDesc[] = [
  {
    id: FETCH_JSON_METHOD_ID,
    name: FETCH_JSON_METHOD_NAME,
    description: 'One-shot GET through std.http-client; returns the provider result document.',
    inputSchema: {
      type: 'object',
      properties: { url: { type: 'string', description: 'Absolute http(s) URL to fetch.' } },
      required: ['url'],
    },
    outputSchema: { type: 'object' },
  },
];

/** @brief Immutable factory metadata; initializes after the provider it consumes. */
const DEMO_DESC: PluginDescriptor = {
  plugId: DEMO_PLUG_ID,
  version: DEMO_VERSION,
  init: { after: [HTTP_CLIENT_PLUG_ID] },
};

/**
 * @brief Create one demo consumer instance wired for watch/lease/invoke/revocation.
 *
 * @return A RackPlugin built with the SDK helper; each call yields an independent instance.
 */
export function create(): RackPlugin {
  // Services borrowed during init(); kept because callbacks keep firing after init returns.
  let services: PluginServices | null = null;
  let capWatch: Subscription | null = null;
  // Pointerless lease on one std.http-client generation; null while no credential is held.
  let lease: LeaseHandle | null = null;
  // Whether the last accepted notification still reports a usable provider.
  let usable = false;

  /**
   * @brief Stop business and return the credential when the host revokes the lease.
   *
   * @param credential Host credential being revoked.
   * @note Unrelated or already returned credentials are ignored; a refused ('busy') return is
   *       surfaced through diagnostics.
   */
  const revoker: Revoker = {
    async onRevoke(credential: LeaseCredential): Promise<void> {
      if (lease === null || lease.credential.handle !== credential.handle) return;
      usable = false;
      const held = lease;
      lease = null;
      const returned = await held.release();
      if (returned === 'busy') {
        services?.diag.report('warn', 'http-client lease revoke returned busy; the return is still owed');
      }
    },
  };

  /**
   * @brief Accept a std.http-client offer only when its version passes the explicit range.
   *
   * @param event Capability availability or withdrawal fact.
   * @note A withdrawal forbids new business but keeps the credential for onRevoke(); a live
   *       lease is never re-borrowed; acquisition failures are reported, not thrown.
   */
  const capSink: CapSink = {
    onCapability(event: CapEvent): void | Promise<void> {
      if (event.plugId !== HTTP_CLIENT_PLUG_ID) return;
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
        lease = svc.caps.acquire(HTTP_CLIENT_PLUG_ID, ACCEPTED_RANGE, revoker);
        usable = true;
      } catch (error) {
        services?.diag.report('warn', 'http-client lease acquisition failed', {
          status: statusOf(error),
        });
      }
    },
  };

  return definePlugin(DEMO_DESC, {
    init: (injected) => {
      services = injected;
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
    invoke: async (request) => {
      if (
        request.methodId === FETCH_JSON_METHOD_ID ||
        request.methodName === FETCH_JSON_METHOD_NAME
      ) {
        const rawArgs = request.args;
        if (typeof rawArgs !== 'object' || rawArgs === null || Array.isArray(rawArgs)) {
          throw new RackError('invalid_argument', 'fetch-json args must be a JSON object');
        }
        // The final cast is needed because the readonly JsonArray union member is not narrowed
        // away by Array.isArray (readonly arrays are not assignable to any[]).
        const obj = rawArgs as JsonObject;
        const url = obj['url'];
        if (typeof url !== 'string' || url.length === 0) {
          throw new RackError('invalid_argument', 'args.url must be a non-empty string');
        }
        const svc = services;
        const held = lease;
        if (svc === null || held === null || !usable) {
          throw new RackError(
            'invalid_state',
            'std.http-client lease is not currently held; wait for its capability event',
          );
        }
        // One-shot demonstration call: a single gateway round-trip per invoke.
        const http = typedApi<HttpClientApi>(svc.calls, held);
        return await http.request({
          url,
          method: 'GET',
          timeoutMs: DEMO_TIMEOUT_MS,
          expectOk: true,
        });
      }
      throw new RackError(
        'not_found',
        `unknown demo method ${request.methodName} (#${request.methodId})`,
      );
    },
  });
}

// Loader convention: a create(): RackPlugin factory as default or named export.
export default create;
