/**
 * @file consumer.plug.ts
 * @brief Example consumer: watch capabilities, borrow a version-qualified lease, write data
 *        through the gateway and survive revocation.
 *
 * @details TS counterpart of examples/consumer.cc, retargeted from echo to the storage provider.
 *          The consumer stores no provider reference and no host implementation: it holds only
 *          the opaque lease credential plus the actual version granted by the host. It shares no
 *          business class with the storage provider either — the shared contract is just the
 *          duplicated constants below (identity, accepted version, method names), mirroring the
 *          data-only echo.hpp idea of the C++ examples.
 *
 *          Flow: init() subscribes to the data event and watches capabilities. An availability
 *          notification whose version passes the explicit closed range acquires a lease — legal
 *          while this instance is still only Initialized, because leasing is not business work.
 *          Business set/get calls stay gated on start(): every 'example.data' event is written
 *          through the gateway with set and read back with get. onRevoke() clears the cached
 *          lease first and then returns the credential.
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
  EventSink,
  JsonObject,
  JsonValue,
  LeaseCredential,
  LeaseHandle,
  MethodDesc,
  PluginDescriptor,
  PluginServices,
  RackEvent,
  RackPlugin,
  Revoker,
  Subscription,
} from '../../src/sdk/index.ts';

// ---------------------------------------------------------------------------
// Duplicated data contract with example.storage: constants only, no shared classes.
// ---------------------------------------------------------------------------

/** @brief Identity of the only provider this consumer depends on. */
const STORAGE_PLUG_ID = 'example.storage';

/** @brief The only storage version this consumer accepts. */
const STORAGE_VERSION = { major: 1, minor: 2, patch: 0 } as const;

/** @brief Explicit closed range [1.2.0, 1.2.0]; the host adds no policy of its own. */
const STORAGE_RANGE = exactVersion(STORAGE_VERSION);

/**
 * @brief Method names of the storage provider, duplicated from its own data contract.
 *
 * @note `set` is reached through the typed facade below; `get` additionally through a direct
 *       gateway entry to demonstrate both call styles.
 */
const STORAGE_SET_METHOD = 'set';
const STORAGE_GET_METHOD = 'get';

/**
 * @brief Typed facade over the storage lease: method names mirror the data contract.
 *
 * @note Argument documents stay JSON values; typedApi packs and forwards them through the
 *       gateway, it never hands out a provider reference.
 */
interface StorageApi {
  set(request: { key: string; value: JsonValue }): Promise<JsonValue>;
  get(request: { key: string }): Promise<JsonValue>;
}

// ---------------------------------------------------------------------------
// Own published contract.
// ---------------------------------------------------------------------------

/** @brief Canonical plugin identity announced to the host. */
const CONSUMER_PLUG_ID = 'example.consumer';

/** @brief Business version published by this consumer. */
const CONSUMER_VERSION = { major: 1, minor: 0, patch: 0 } as const;

/** @brief Name of the demonstration data event this consumer subscribes to. */
const DATA_EVENT_NAME = 'example.data';

/** @brief Storage key the received payloads are written through to. */
const DATA_KEY = 'example.data';

/** @brief Numeric method id of the consumer status method. */
const STATUS_METHOD_ID = 1;

/** @brief Published name of the consumer status method. */
const STATUS_METHOD_NAME = 'status';

/** @brief Methods announced through the capability service during start(). */
const STATUS_METHODS: readonly MethodDesc[] = [
  {
    id: STATUS_METHOD_ID,
    name: STATUS_METHOD_NAME,
    description: 'Report processed data events and currently held storage leases.',
    inputSchema: { type: 'null' },
    outputSchema: { type: 'object' },
  },
];

/** @brief Immutable factory metadata, including the typed plugin version. */
const CONSUMER_DESC: PluginDescriptor = {
  plugId: CONSUMER_PLUG_ID,
  version: CONSUMER_VERSION,
  // Hard init edge: the storage provider must precede this plugin.
  init: { priority: 10, after: [STORAGE_PLUG_ID] },
};

/**
 * @brief Create one consumer instance wired for watch/lease/invoke/revocation.
 *
 * @return A RackPlugin built with the SDK helper; each call yields an independent instance.
 */
export function create(): RackPlugin {
  // Services borrowed during init(); kept because callbacks keep firing after init returns.
  let services: PluginServices | null = null;
  let capWatch: Subscription | null = null;
  let eventSub: Subscription | null = null;
  // Pointerless lease on one storage generation; null while no credential is held.
  let lease: LeaseHandle | null = null;
  // Whether the last accepted notification still reports a usable provider.
  let storageUsable = false;
  // Whether start() completed; business calls are gated on this flag.
  let active = false;
  let eventsProcessed = 0;

  /**
   * @brief Stop business and return the credential when the host revokes the lease.
   *
   * @param credential Host credential being revoked.
   * @note Unrelated or already returned credentials are ignored. The cached lease is cleared
   *       before the return is attempted, so no new business can be issued after the
   *       notification; a refused ('busy') return is surfaced through diagnostics.
   */
  const revoker: Revoker = {
    async onRevoke(credential: LeaseCredential): Promise<void> {
      if (lease === null || lease.credential.handle !== credential.handle) return;
      storageUsable = false;
      const held = lease;
      lease = null;
      const returned = await held.release();
      if (returned === 'busy') {
        services?.diag.report(
          'warn',
          'storage lease revoke returned busy; the return is still owed',
        );
      }
    },
  };

  /**
   * @brief Accept a storage offer only when the notification version passes the explicit range.
   *
   * @param event Capability availability or withdrawal fact.
   * @note A withdrawal forbids new business but keeps the credential for onRevoke(). A live
   *       lease is never re-borrowed; acquisition failures are reported, not thrown, so one
   *       bad notification cannot fault the whole consumer.
   */
  const capSink: CapSink = {
    onCapability(event: CapEvent): void | Promise<void> {
      if (event.plugId !== STORAGE_PLUG_ID) return;
      if (!event.available) {
        storageUsable = false;
        return;
      }
      if (!acceptsVersion(STORAGE_RANGE, event.version)) {
        storageUsable = false;
        return;
      }
      if (lease !== null) {
        storageUsable = true;
        return;
      }
      const svc = services;
      if (svc === null) return;
      try {
        lease = svc.caps.acquire(STORAGE_PLUG_ID, STORAGE_RANGE, revoker);
        storageUsable = true;
      } catch (error) {
        services?.diag.report('warn', 'storage lease acquisition failed', {
          status: statusOf(error),
        });
      }
    },
  };

  /**
   * @brief Count the data event and write it through to storage while leased and Active.
   *
   * @param event Delivered rack event; the payload is valid only during delivery.
   * @note Events are counted even without a lease. Business happens only when this instance is
   *       Active, the last accepted notification still reports availability and a lease is
   *       held. Two call styles coexist on purpose: the typedApi proxy for set/get and one
   *       direct numeric gateway entry — both travel the same gateway under the same
   *       credential, and the results are discarded either way.
   */
  const eventSink: EventSink = {
    async onEvent(event: RackEvent): Promise<void> {
      if (event.name !== DATA_EVENT_NAME) return;
      eventsProcessed++;
      const svc = services;
      const held = lease;
      if (!active || !storageUsable || svc === null || held === null) return;
      try {
        // Typed direct-call style: storage.set(...) / storage.get(...) via the proxy.
        const storage = typedApi<StorageApi>(svc.calls, held);
        await storage.set({ key: DATA_KEY, value: event.payload });
        await storage.get({ key: DATA_KEY });
        // Classic gateway style kept side by side: the name entry resolves identically.
        await svc.calls.call(held.credential, STORAGE_GET_METHOD, { key: DATA_KEY });
      } catch (error) {
        svc.diag.report('warn', 'storage write-through failed', { status: statusOf(error) });
      }
    },
  };

  return definePlugin(CONSUMER_DESC, {
    init: (injected) => {
      services = injected;
      // Watch first, then subscribe: the watch snapshot may already carry the provider fact.
      capWatch = injected.caps.watch(capSink);
      eventSub = injected.events.subscribe(DATA_EVENT_NAME, eventSink);
    },
    start: () => {
      services?.caps.publish({ methods: STATUS_METHODS });
      active = true;
    },
    stop: async () => {
      active = false;
      storageUsable = false;
      eventSub?.unsubscribe();
      eventSub = null;
      capWatch?.unsubscribe();
      capWatch = null;
      const held = lease;
      lease = null;
      if (held !== null) await held.release();
    },
    invoke: (request) => {
      if (request.methodId === STATUS_METHOD_ID || request.methodName === STATUS_METHOD_NAME) {
        const status: JsonObject = {
          eventsProcessed,
          leasesHeld: lease === null ? 0 : 1,
        };
        return status;
      }
      throw new RackError(
        'not_found',
        `unknown consumer method ${request.methodName} (#${request.methodId})`,
      );
    },
  });
}

// Loader convention: a create(): RackPlugin factory as default or named export.
export default create;
