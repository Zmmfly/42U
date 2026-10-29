/**
 * @file rpc-demo.plug.ts
 * @brief Example rack-to-rack demo: a ping provider plus a client round trip through std.rpc.
 *
 * @details The demo plugin plays both ends of the RPC port on one rack: `ping` is a plain
 *          provider method reachable through the gateway (so a remote rack can call
 *          `example.rpc-demo.ping`), while `demo` leases `std.rpc`, drives its `call`
 *          client method against a caller-supplied endpoint URL, and reports the full round
 *          trip. In tests this runs against a real local node:http endpoint standing in for
 *          a peer rack, closing the loop: demo -> gateway -> std.rpc.call -> HTTP ->
 *          std.rpc bridge -> gateway -> demo.ping.
 *
 *          The shared data contract with std.rpc is duplicated as constants below (no
 *          shared classes), mirroring the data-only contracts of the other examples.
 */

import {
  acceptsVersion,
  definePlugin,
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
  VersionRange,
} from '../../src/sdk/index.ts';

// ---------------------------------------------------------------------------
// Duplicated data contract with std.rpc: constants only, no shared classes.
// ---------------------------------------------------------------------------

/** @brief Identity of the RPC port provider this demo depends on. */
const RPC_PLUG_ID = 'std.rpc';

/** @brief Accepted std.rpc interval [1.0.0, 1.MAX]. */
const RPC_RANGE: VersionRange = {
  minimum: { major: 1, minor: 0, patch: 0 },
  maximum: { major: 1, minor: Number.MAX_SAFE_INTEGER, patch: Number.MAX_SAFE_INTEGER },
};

/** @brief Gateway method of std.rpc used by the demo round trip. */
const RPC_CALL_METHOD = 'call';

// ---------------------------------------------------------------------------
// Own published contract.
// ---------------------------------------------------------------------------

/** @brief Canonical plugin identity announced to the host. */
const DEMO_PLUG_ID = 'example.rpc-demo';

/** @brief Business version published by this demo. */
const DEMO_VERSION = { major: 1, minor: 0, patch: 0 } as const;

/** @brief Numeric method id of the ping provider method. */
const PING_METHOD_ID = 1;

/** @brief Published name of the ping provider method. */
const PING_METHOD_NAME = 'ping';

/** @brief Numeric method id of the client round-trip method. */
const DEMO_METHOD_ID = 2;

/** @brief Published name of the client round-trip method. */
const DEMO_METHOD_NAME = 'demo';

/** @brief Methods announced through the capability service during start(). */
const DEMO_METHODS: readonly MethodDesc[] = [
  {
    id: PING_METHOD_ID,
    name: PING_METHOD_NAME,
    description: 'Answer with a pong document echoing the received args.',
    inputSchema: {},
    outputSchema: { type: 'object' },
  },
  {
    id: DEMO_METHOD_ID,
    name: DEMO_METHOD_NAME,
    description: 'Call one remote JSON-RPC method through the leased std.rpc client.',
    inputSchema: {
      type: 'object',
      properties: {
        url: { type: 'string', description: 'Remote endpoint URL, e.g. the local /rpc route.' },
        method: { type: 'string', description: 'Defaults to example.rpc-demo.ping.' },
        params: {},
      },
      required: ['url'],
    },
    outputSchema: { type: 'object' },
  },
];

/** @brief Immutable factory metadata, including the hard init edge behind std.rpc. */
const DEMO_DESC: PluginDescriptor = {
  plugId: DEMO_PLUG_ID,
  version: DEMO_VERSION,
  init: { priority: 10, after: [RPC_PLUG_ID] },
};

/**
 * @brief Typed facade over the std.rpc lease; mirrors the std.rpc `call` data contract.
 *
 * @note typedApi packs and forwards the args document through the gateway; no provider
 *       reference ever crosses the plugin boundary.
 */
interface RpcClientApi {
  call(request: JsonObject): Promise<JsonValue>;
}

// ---------------------------------------------------------------------------
// Local argument helpers (examples import only the SDK).
// ---------------------------------------------------------------------------

/**
 * @brief Narrow a JSON value to a JSON object.
 *
 * @param value Candidate args document.
 * @param what Name of the checked document for the error message.
 * @return The same value typed as JsonObject.
 * @throws RackError 'invalid_argument' when the value is not a JSON object.
 */
function expectObject(value: JsonValue, what: string): JsonObject {
  // The final cast is needed because the readonly JsonArray union member is not narrowed
  // away by Array.isArray (readonly arrays are not assignable to any[]).
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw new RackError('invalid_argument', `${what} must be a JSON object`);
  }
  return value as JsonObject;
}

/**
 * @brief Extract a mandatory non-empty string field.
 *
 * @param value Candidate field value.
 * @param what Field description used verbatim in the error message.
 * @return The validated string.
 * @throws RackError 'invalid_argument' when the value is missing or not a non-empty string.
 */
function expectString(value: JsonValue | undefined, what: string): string {
  if (typeof value !== 'string' || value.length === 0) {
    throw new RackError('invalid_argument', `${what} must be a non-empty string`);
  }
  return value;
}

// ---------------------------------------------------------------------------
// Factory.
// ---------------------------------------------------------------------------

/**
 * @brief Create one rpc-demo instance wired for watch/lease/client round trips.
 *
 * @return A RackPlugin built with the SDK helper; each call yields an independent instance.
 */
export function create(): RackPlugin {
  let services: PluginServices | null = null;
  let capWatch: Subscription | null = null;
  // Pointerless lease on one std.rpc generation; null while no credential is held.
  let lease: LeaseHandle | null = null;
  // Whether the last accepted notification still reports a usable std.rpc.
  let rpcUsable = false;
  // Whether start() completed; business calls are gated on this flag.
  let active = false;

  /**
   * @brief Stop business and return the credential when the host revokes the lease.
   *
   * @param credential Host credential being revoked.
   * @note Unrelated or already returned credentials are ignored; a refused ('busy')
   *       return is surfaced through diagnostics.
   */
  const revoker: Revoker = {
    async onRevoke(credential: LeaseCredential): Promise<void> {
      const held = lease;
      if (held === null || held.credential.handle !== credential.handle) return;
      rpcUsable = false;
      lease = null;
      const outcome = await held.release();
      if (outcome !== 'ok') {
        services?.diag.report('warn', `std.rpc lease release returned "${outcome}" on revoke`);
      }
    },
  };

  /**
   * @brief Borrow std.rpc whenever a usable generation shows up.
   *
   * @param event Capability availability or withdrawal fact.
   * @note Acquisition failures are reported, not thrown, so one bad notification cannot
   *       fault the demo.
   */
  const capSink: CapSink = {
    onCapability(event: CapEvent): void {
      if (event.plugId !== RPC_PLUG_ID) return;
      if (!event.available || !acceptsVersion(RPC_RANGE, event.version)) {
        rpcUsable = false;
        return;
      }
      rpcUsable = true;
      if (lease === null && services !== null) {
        try {
          lease = services.caps.acquire(RPC_PLUG_ID, RPC_RANGE, revoker);
        } catch (error) {
          services.diag.report('warn', 'std.rpc lease acquisition failed', {
            status: statusOf(error),
          });
        }
      }
    },
  };

  /**
   * @brief Drive one remote call through the leased std.rpc client.
   *
   * @param args `{url, method?, params?}`; the method defaults to this plugin's ping.
   * @return `{url, method, result}` describing the completed round trip.
   * @throws RackError 'invalid_state' before start() or without a usable std.rpc lease;
   *         gateway/RackError passthrough from std.rpc otherwise.
   */
  async function runDemo(args: JsonValue): Promise<JsonValue> {
    const svc = services;
    const held = lease;
    if (!active || !rpcUsable || svc === null || held === null) {
      throw new RackError('invalid_state', 'demo requires an active lease on std.rpc');
    }
    const record = expectObject(args, 'demo args');
    const url = expectString(record['url'], 'demo args.url');
    const methodValue = record['method'];
    const method =
      typeof methodValue === 'string' && methodValue.length > 0
        ? methodValue
        : `${DEMO_PLUG_ID}.${PING_METHOD_NAME}`;
    const paramsValue = record['params'];
    const params = paramsValue === undefined || paramsValue === null ? {} : paramsValue;
    const rpc = typedApi<RpcClientApi>(svc.calls, held);
    const result = await rpc.call({ url, method, params });
    return { url, method, result };
  }

  return definePlugin(DEMO_DESC, {
    init: (injected) => {
      services = injected;
      capWatch = injected.caps.watch(capSink);
    },
    start: () => {
      services?.caps.publish({ methods: DEMO_METHODS });
      active = true;
    },
    stop: async () => {
      active = false;
      rpcUsable = false;
      capWatch?.unsubscribe();
      capWatch = null;
      const held = lease;
      lease = null;
      if (held !== null) await held.release();
    },
    invoke: (request: InvokeRequest): Promise<JsonValue> => {
      if (request.methodId === PING_METHOD_ID || request.methodName === PING_METHOD_NAME) {
        return Promise.resolve({ pong: true, received: request.args });
      }
      if (request.methodId === DEMO_METHOD_ID || request.methodName === DEMO_METHOD_NAME) {
        return runDemo(request.args);
      }
      throw new RackError(
        'not_found',
        `unknown rpc-demo method ${request.methodName} (#${request.methodId})`,
      );
    },
  });
}

// Loader convention: a create(): RackPlugin factory as default or named export.
export default create;
