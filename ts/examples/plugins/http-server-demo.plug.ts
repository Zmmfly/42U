/**
 * @file http-server-demo.plug.ts
 * @brief Example consumer of std.http-server: registers a route to its own `hit` method.
 *
 * @details Demonstrates the standard bridging pattern from the plugin side: borrow
 *          std.http-server with a one-shot lease, call `route.add` pointing at this plugin's
 *          own `hit` method, and return the lease immediately. Incoming HTTP requests to the
 *          registered path are then forwarded back into this plugin through the host gateway
 *          (never by a direct reference), so the round trip crosses the invoke-only boundary
 *          twice. `hit` counts invocations and echoes the forwarded args for inspection.
 */

import { definePlugin, RackError, statusOf } from '../../src/sdk/index.ts';
import type { JsonValue, MethodDesc, PluginServices, RackPlugin, Revoker, VersionRange } from '../../src/sdk/index.ts';

/** @brief Canonical plugin identity of this example device. */
const DEMO_PLUG_ID = 'example.http-server-demo';

/** @brief Business version of this example device. */
const DEMO_VERSION = { major: 1, minor: 0, patch: 0 } as const;

/** @brief Provider this example consumes through the gateway. */
const HTTP_SERVER_PLUG_ID = 'std.http-server';

/** @brief Explicit lease range for std.http-server: closed [1.0.0, 1.*.*]. */
const HTTP_SERVER_RANGE: VersionRange = {
  minimum: { major: 1, minor: 0, patch: 0 },
  maximum: { major: 1, minor: Number.MAX_SAFE_INTEGER, patch: Number.MAX_SAFE_INTEGER },
};

/** @brief Numeric method id of the hit counter method. */
const HIT_METHOD_ID = 1;

/** @brief Method name matching the numeric id above; part of the data-only contract. */
const HIT_METHOD_NAME = 'hit';

/** @brief Route pattern registered during start(); `:name` exercises path parameter extraction. */
const DEMO_ROUTE_PATH = '/demo/:name';

/** @brief Methods announced through the capability service during start(). */
const DEMO_METHODS: readonly MethodDesc[] = [
  {
    id: HIT_METHOD_ID,
    name: HIT_METHOD_NAME,
    description: 'Count invocations and echo the args document forwarded by std.http-server.',
    inputSchema: { type: 'object' },
    outputSchema: { type: 'object', properties: { hits: { type: 'number' }, received: {} } },
  },
];

/** @brief No-op revoker: the leases here are one-shot and returned right after each call. */
const noRevoker: Revoker = {
  onRevoke(): void {
    // Nothing to stop: the credential is returned in the finally block of each borrow.
  },
};

/**
 * @brief Read the routeId out of a route.add gateway result.
 * @param result Gateway result document, of runtime shape.
 * @return The routeId string, or null when the result carries none.
 */
function readRouteId(result: JsonValue): string | null {
  if (typeof result !== 'object' || result === null || Array.isArray(result)) return null;
  const routeId = (result as Record<string, JsonValue>)['routeId'];
  return typeof routeId === 'string' ? routeId : null;
}

/**
 * @brief Create one http-server-demo instance.
 *
 * @return A RackPlugin built with the SDK helper; each call yields an independent instance.
 */
export function create(): RackPlugin {
  let services: PluginServices | null = null;
  let hits = 0;
  let routeId: string | null = null;
  // Whether start() completed and stop() has not run; gates the deferred registration.
  let active = false;

  /**
   * @brief Register the demo route through one borrowed http-server lease.
   *
   * @note Business calls require an Active consumer, and the frozen host marks an instance
   *       Active only after start() returns; a synchronous route.add from inside start()
   *       is rejected with 'invalid_state'. Failures are reported through diagnostics so a
   *       temporarily unavailable std.http-server never faults the boot batch.
   */
  async function registerRoute(): Promise<void> {
    const svc = services;
    if (svc === null || !active || routeId !== null) return;
    try {
      const handle = svc.caps.acquire(HTTP_SERVER_PLUG_ID, HTTP_SERVER_RANGE, noRevoker);
      try {
        const result = await svc.calls.call(handle.credential, 'route.add', {
          path: DEMO_ROUTE_PATH,
          httpMethod: 'GET',
          targetPlugId: DEMO_PLUG_ID,
          targetMethod: HIT_METHOD_NAME,
        });
        routeId = readRouteId(result);
      } finally {
        await handle.release();
      }
    } catch (error) {
      svc.diag.report('error', 'example.http-server-demo: route registration failed', {
        status: statusOf(error),
      });
    }
  }
  return definePlugin(
    { plugId: DEMO_PLUG_ID, version: DEMO_VERSION, init: { after: [HTTP_SERVER_PLUG_ID] } },
    {
      init: (injected: PluginServices) => {
        services = injected;
      },
      start: async () => {
        const svc = services;
        if (svc === null) {
          throw new RackError('invalid_state', 'example.http-server-demo: init() must run before start()');
        }
        // Publish capabilities first so the route we register can never outrun our own
        // availability as a lease target.
        svc.caps.publish({ methods: DEMO_METHODS });
        active = true;
        // Defer the route.add to the next macrotask: this instance is only Active after
        // start() returns, and business calls before that are rejected by the gateway.
        setImmediate(() => {
          void registerRoute();
        });
      },
      stop: async () => {
        active = false;
        const svc = services;
        if (svc === null || routeId === null) return;
        try {
          const handle = svc.caps.acquire(HTTP_SERVER_PLUG_ID, HTTP_SERVER_RANGE, noRevoker);
          try {
            await svc.calls.call(handle.credential, 'route.remove', { routeId });
          } finally {
            await handle.release();
          }
        } catch {
          // Best effort: std.http-server may already be stopped by shutdown ordering.
        }
        routeId = null;
      },
      invoke: (request) => {
        if (request.methodId === HIT_METHOD_ID || request.methodName === HIT_METHOD_NAME) {
          hits += 1;
          return { hits, received: request.args };
        }
        throw new RackError(
          'not_found',
          `example.http-server-demo: unknown method ${request.methodName} (#${request.methodId})`,
        );
      },
    },
  );
}

// Loader convention: a create(): RackPlugin factory as default or named export.
export default create;
