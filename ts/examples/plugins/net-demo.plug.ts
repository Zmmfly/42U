/**
 * @file net-demo.plug.ts
 * @brief Example consumer of the std.net standard device: one-shot TCP check via the
 *        lease + gateway path.
 *
 * @details Demonstrates "everything is a plugin": the demo never imports std.net code and
 *          holds no provider reference. It watches capability facts in init(), borrows a
 *          version-qualified lease on std.net when an in-range offer arrives, and its
 *          'check' method forwards {host, port} as a 'tcp.probe' call through the host
 *          invoke gateway. In fake environments the injected PluginServices (caps/calls)
 *          are test doubles, so no real network is touched; on a real rack the probe runs
 *          inside the std.net instance. onRevoke() clears the cached lease before
 *          returning the credential; stop() unsubscribes and releases the lease.
 */

import { acceptsVersion, definePlugin, exactVersion, RackError } from '../../src/sdk/index.ts';
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

// ---------------------------------------------------------------------------
// Duplicated data contract with std.net: constants only, no shared classes.
// ---------------------------------------------------------------------------

/** @brief Identity of the only provider this consumer depends on. */
const NET_PLUG_ID = 'std.net';

/** @brief The only std.net version this consumer accepts. */
const NET_VERSION = { major: 1, minor: 0, patch: 0 } as const;

/** @brief Explicit closed range [1.0.0, 1.0.0]; the host adds no policy of its own. */
const NET_RANGE = exactVersion(NET_VERSION);

/** @brief Provider method reached through the gateway, duplicated from std.net's contract. */
const TCP_PROBE_METHOD = 'tcp.probe';

/** @brief Probe timeout this demo passes on every check; mirrors the std.net default. */
const PROBE_TIMEOUT_MS = 2000;

// ---------------------------------------------------------------------------
// Own published contract.
// ---------------------------------------------------------------------------

/** @brief Canonical plugin identity announced to the host. */
const DEMO_PLUG_ID = 'example.net-demo';

/** @brief Business version published by this consumer. */
const DEMO_VERSION = { major: 1, minor: 0, patch: 0 } as const;

/** @brief Numeric method id of the check method. */
const CHECK_METHOD_ID = 1;

/** @brief Published name of the check method. */
const CHECK_METHOD_NAME = 'check';

/** @brief Methods announced through the capability service during start(). */
const CHECK_METHODS: readonly MethodDesc[] = [
  {
    id: CHECK_METHOD_ID,
    name: CHECK_METHOD_NAME,
    description: 'Probe {host, port} through a leased std.net provider and return its result.',
    inputSchema: {
      type: 'object',
      properties: {
        host: { type: 'string' },
        port: { type: 'integer', minimum: 1, maximum: 65535 },
      },
      required: ['host', 'port'],
    },
    outputSchema: {
      type: 'object',
      properties: {
        ok: { type: 'boolean' },
        latencyMs: { type: 'integer', minimum: 0 },
      },
      required: ['ok'],
    },
  },
];

/** @brief Immutable factory metadata, including the hard init edge on the provider. */
const DEMO_DESC: PluginDescriptor = {
  plugId: DEMO_PLUG_ID,
  version: DEMO_VERSION,
  // Hard init edge: the std.net provider must precede this plugin.
  init: { priority: 10, after: [NET_PLUG_ID] },
};

/**
 * @brief Create one net-demo consumer instance wired for watch/lease/invoke/revocation.
 *
 * @return A RackPlugin built with the SDK helper; each call yields an independent instance.
 */
export function create(): RackPlugin {
  // Services borrowed during init(); kept because callbacks keep firing after init returns.
  let services: PluginServices | null = null;
  let capWatch: Subscription | null = null;
  // Opaque lease on one std.net generation; null while no credential is held.
  let lease: LeaseHandle | null = null;
  // Whether the last accepted notification still reports a usable provider.
  let netUsable = false;
  // Whether start() completed; business calls are gated on this flag.
  let active = false;

  /**
   * @brief Stop business and return the credential when the host revokes the lease.
   *
   * @param credential Host credential being revoked.
   *
   * @note Unrelated or already returned credentials are ignored. The cached lease is
   *       cleared before the return is attempted, so no new business can be issued after
   *       the notification.
   */
  const revoker: Revoker = {
    async onRevoke(credential: LeaseCredential): Promise<void> {
      if (lease === null || lease.credential.handle !== credential.handle) return;
      netUsable = false;
      const held = lease;
      lease = null;
      const returned = await held.release();
      if (returned === 'busy') {
        services?.diag.report(
          'warn',
          'std.net lease revoke returned busy; the return is still owed',
        );
      }
    },
  };

  /**
   * @brief Accept a std.net offer only when the notification version passes the range.
   *
   * @param event Capability availability or withdrawal fact.
   *
   * @note A withdrawal forbids new business but keeps the credential for onRevoke(). A
   *       live lease is never re-borrowed; acquisition failures are reported, not thrown,
   *       so one bad notification cannot fault the whole demo.
   */
  const capSink: CapSink = {
    onCapability(event: CapEvent): void | Promise<void> {
      if (event.plugId !== NET_PLUG_ID) return;
      if (!event.available) {
        netUsable = false;
        return;
      }
      if (!acceptsVersion(NET_RANGE, event.version)) {
        netUsable = false;
        return;
      }
      if (lease !== null) {
        netUsable = true;
        return;
      }
      const svc = services;
      if (svc === null) return;
      try {
        lease = svc.caps.acquire(NET_PLUG_ID, NET_RANGE, revoker);
        netUsable = true;
      } catch (error) {
        services?.diag.report('warn', 'std.net lease acquisition failed', {
          status: error instanceof RackError ? error.status : 'failed',
        });
      }
    },
  };

  return definePlugin(DEMO_DESC, {
    init: (injected) => {
      services = injected;
      // Watch first: the watch snapshot may already carry the provider fact.
      capWatch = injected.caps.watch(capSink);
    },
    start: () => {
      services?.caps.publish({ methods: CHECK_METHODS });
      active = true;
    },
    stop: async () => {
      active = false;
      netUsable = false;
      capWatch?.unsubscribe();
      capWatch = null;
      const held = lease;
      lease = null;
      if (held !== null) await held.release();
    },
    invoke: async (request) => {
      if (request.methodId !== CHECK_METHOD_ID && request.methodName !== CHECK_METHOD_NAME) {
        throw new RackError(
          'not_found',
          `unknown net-demo method ${request.methodName} (#${request.methodId})`,
        );
      }
      const svc = services;
      const held = lease;
      if (!active || !netUsable || svc === null || held === null) {
        throw new RackError('invalid_state', 'net-demo requires a live std.net lease');
      }
      // Argument validation mirrors the provider contract; std.net revalidates anyway.
      const args = request.args;
      if (typeof args !== 'object' || args === null || Array.isArray(args)) {
        throw new RackError('invalid_argument', 'check args must be a JSON object');
      }
      const checkArgs = args as JsonObject;
      const host = checkArgs['host'];
      if (typeof host !== 'string' || host.length === 0) {
        throw new RackError('invalid_argument', 'args.host must be a non-empty string');
      }
      const port = checkArgs['port'];
      if (
        typeof port !== 'number' ||
        !Number.isSafeInteger(port) ||
        port < 1 ||
        port > 65535
      ) {
        throw new RackError('invalid_argument', 'args.port must be an integer in [1, 65535]');
      }
      // The only business path: consumer -> host gateway -> std.net 'tcp.probe'.
      const result: JsonValue = await svc.calls.call(held.credential, TCP_PROBE_METHOD, {
        host,
        port,
        timeoutMs: PROBE_TIMEOUT_MS,
      });
      return result;
    },
  });
}

// Loader convention: a create(): RackPlugin factory as default or named export.
export default create;
