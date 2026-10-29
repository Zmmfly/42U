/**
 * @file logging-demo.plug.ts
 * @brief Example consumer of std.log: borrow a lease, boot-log after start() and forward
 *        'run' messages through the gateway.
 *
 * @details The demo shares no business class with the logging provider: the shared contract
 *          is the duplicated constants below (identity, accepted version, method names),
 *          mirroring the data-only contract idea of the other examples. init() watches
 *          capabilities; an availability fact whose version passes the explicit closed
 *          range acquires a lease (legal while only Initialized). start() publishes the
 *          'run' method and records a few demonstration entries through the gateway; each
 *          'run' {message} invocation forwards exactly one log call. Without a usable
 *          std.log lease every write is skipped and reported as not logged.
 */

import {
  acceptsVersion,
  definePlugin,
  exactVersion,
  RackError,
  statusOf,
} from '../../src/sdk/index.ts';
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
// Duplicated data contract with std.log: constants only, no shared classes.
// ---------------------------------------------------------------------------

/** @brief Identity of the only provider this consumer depends on. */
const LOG_PLUG_ID = 'std.log';

/** @brief The only std.log version this demo accepts. */
const LOG_VERSION = { major: 1, minor: 0, patch: 0 } as const;

/** @brief Explicit closed range [1.0.0, 1.0.0]; the host adds no policy of its own. */
const LOG_RANGE = exactVersion(LOG_VERSION);

/** @brief Name of the log method, duplicated from the std.log data contract. */
const LOG_METHOD = 'log';

/** @brief Arg key of the run method message. */
const MESSAGE_ARG_KEY = 'message';

/**
 * @brief Narrow a JSON value to a plain object (not an array, not null).
 *
 * @param value Candidate JSON value.
 * @return true for JSON objects.
 */
function isArgsObject(value: JsonValue): value is JsonObject {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

// ---------------------------------------------------------------------------
// Own published contract.
// ---------------------------------------------------------------------------

/** @brief Canonical plugin identity announced to the host. */
const DEMO_PLUG_ID = 'example.logging-demo';

/** @brief Business version published by this demo. */
const DEMO_VERSION = { major: 1, minor: 0, patch: 0 } as const;

/** @brief Numeric method id of the demonstration run method. */
const RUN_METHOD_ID = 1;

/** @brief Published name of the demonstration run method. */
const RUN_METHOD_NAME = 'run';

/** @brief Methods announced through the capability service during start(). */
const RUN_METHODS: readonly MethodDesc[] = [
  {
    id: RUN_METHOD_ID,
    name: RUN_METHOD_NAME,
    description: 'Forward one message to std.log through the gateway.',
    inputSchema: { type: 'object' },
    outputSchema: { type: 'object' },
  },
];

/** @brief Immutable factory metadata, including the hard init edge on std.log. */
const DEMO_DESC: PluginDescriptor = {
  plugId: DEMO_PLUG_ID,
  version: DEMO_VERSION,
  // Hard init edge: the logging provider must precede this demo.
  init: { priority: 10, after: [LOG_PLUG_ID] },
};

/**
 * @brief Create one logging-demo instance wired for watch/lease/invoke/revocation.
 *
 * @return A RackPlugin built with the SDK helper; each call yields an independent instance.
 */
export function create(): RackPlugin {
  // Services borrowed during init(); kept because callbacks keep firing after init returns.
  let services: PluginServices | null = null;
  let capWatch: Subscription | null = null;
  // Pointerless lease on one std.log generation; null while no credential is held.
  let lease: LeaseHandle | null = null;
  // Whether the last accepted notification still reports a usable provider.
  let logUsable = false;
  // Whether start() completed; business calls are gated on this flag.
  let active = false;
  // Ensures the boot entries are recorded once per lease+start combination.
  let bootLogged = false;

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
      logUsable = false;
      const held = lease;
      lease = null;
      const returned = await held.release();
      if (returned === 'busy') {
        services?.diag.report('warn', 'std.log lease revoke returned busy; return still owed');
      }
    },
  };

  /**
   * @brief Forward one entry to std.log through the gateway.
   *
   * @param level Log level of the entry.
   * @param message Human-readable message.
   * @param fields Optional structured fields.
   * @return Completion; gateway failures are reported through diagnostics, never thrown.
   */
  const writeEntry = (
    level: 'info' | 'warn' | 'error',
    message: string,
    fields?: JsonObject,
  ): Promise<void> => {
    const svc = services;
    const held = lease;
    if (svc === null || held === null) return Promise.resolve();
    const args: JsonObject =
      fields === undefined
        ? { level, message, plugId: DEMO_PLUG_ID }
        : { level, message, fields, plugId: DEMO_PLUG_ID };
    return svc.calls
      .call(held.credential, LOG_METHOD, args)
      .then(() => undefined)
      .catch((error: unknown) => {
        svc.diag.report('warn', 'std.log write failed', { status: statusOf(error) });
      });
  };

  /**
   * @brief Record the demonstration boot entries once business is allowed.
   *
   * @note Called from start() when the lease is already held, or from the capability
   *       callback when availability arrives while the instance is already Active.
   */
  const writeBootEntries = async (): Promise<void> => {
    await writeEntry('info', 'logging-demo started');
    await writeEntry('info', 'logging-demo boot fields', { rack: '42U', slots: 42 });
    await writeEntry('warn', 'logging-demo boot warning entry');
  };

  /**
   * @brief Accept a std.log offer only when the version passes the explicit range.
   *
   * @param event Capability availability or withdrawal fact.
   * @note A withdrawal forbids new business but keeps the credential for onRevoke(). A live
   *       lease is never re-borrowed; acquisition failures are reported, not thrown.
   */
  const capSink: CapSink = {
    async onCapability(event: CapEvent): Promise<void> {
      if (event.plugId !== LOG_PLUG_ID) return;
      if (!event.available) {
        logUsable = false;
        return;
      }
      if (!acceptsVersion(LOG_RANGE, event.version)) {
        logUsable = false;
        return;
      }
      if (lease !== null) {
        logUsable = true;
        return;
      }
      const svc = services;
      if (svc === null) return;
      try {
        lease = svc.caps.acquire(LOG_PLUG_ID, LOG_RANGE, revoker);
        logUsable = true;
      } catch (error) {
        services?.diag.report('warn', 'std.log lease acquisition failed', {
          status: statusOf(error),
        });
        return;
      }
      if (active && !bootLogged) {
        bootLogged = true;
        await writeBootEntries();
      }
    },
  };

  return definePlugin(DEMO_DESC, {
    init: (injected) => {
      services = injected;
      // Watch first: the snapshot may already carry the provider fact.
      capWatch = injected.caps.watch(capSink);
    },
    start: async () => {
      services?.caps.publish({ methods: RUN_METHODS });
      active = true;
      if (!bootLogged && lease !== null && logUsable) {
        bootLogged = true;
        // Business calls require an Active consumer, which this instance only becomes
        // after start() returns; the boot entries are deferred to the next macrotask.
        setImmediate(() => {
          writeBootEntries().catch((error: unknown) => {
            services?.diag.report('error', 'logging-demo boot entries failed', {
              status: statusOf(error),
            });
          });
        });
      }
    },
    stop: async () => {
      active = false;
      logUsable = false;
      bootLogged = false;
      capWatch?.unsubscribe();
      capWatch = null;
      const held = lease;
      lease = null;
      if (held !== null) await held.release();
      services = null;
    },
    invoke: async (request) => {
      if (request.methodName === RUN_METHOD_NAME || request.methodId === RUN_METHOD_ID) {
        const args = request.args;
        if (!isArgsObject(args)) {
          throw new RackError('invalid_argument', 'logging-demo run args must be a JSON object');
        }
        const message = args[MESSAGE_ARG_KEY];
        if (typeof message !== 'string') {
          throw new RackError('invalid_argument', 'logging-demo run.message must be a string');
        }
        const svc = services;
        const held = lease;
        if (!active || svc === null || held === null || !logUsable) {
          const skipped: JsonObject = { logged: false, reason: 'std.log lease not held' };
          return skipped;
        }
        await writeEntry('info', message, { source: DEMO_PLUG_ID });
        const done: JsonObject = { logged: true };
        return done;
      }
      throw new RackError(
        'not_found',
        `unknown logging-demo method ${request.methodName} (#${request.methodId})`,
      );
    },
  });
}

// Loader convention: a create(): RackPlugin factory as default or named export.
export default create;
