/**
 * @file Rack host facade: the assembly point and lifecycle brain of the TS rack.
 *
 * @brief Implements the frozen Rack port by wiring the seven first-wave core modules
 *        (planner, event bus, capability registry, lease manager, invoke gateway, plugin
 *        loader, per-instance services) into one orchestrator: deterministic two-phase
 *        start, post-start capability registration, the explicit unload protocol,
 *        generation-stale credentials, deferred unloads from plugin callbacks and
 *        best-effort reverse-order shutdown.
 *
 * @note Faithful adaptation of the C++ u42 host (src/host.cc) onto the single Node event
 *       loop: init() only subscribes, start() registers capabilities only after the plugin
 *       returns, unload proves quiescence (no outstanding leases, zero in-flight calls)
 *       before stop/destroy, unprovable instances are quarantined ('faulted') instead of
 *       force-torn-down, and structural operations issued from inside plugin callbacks
 *       converge: unload defers to a queue consumed by poll(), while boot/start/load/add/
 *       shutdown reject with RackError('busy'). Host-private generations invalidate every
 *       old credential across a reload even when the disclosed version is identical.
 */

import { createCapabilityRegistry } from './caps.ts';
import { createEventBus } from './events.ts';
import { createInvokeGateway } from './calls.ts';
import { createLeaseManager } from './lease.ts';
import { createOrderPlanner } from './order.ts';
import { createPluginLoader } from './loader.ts';
import { createPluginServices } from './services.ts';
import { isPluginVersion } from './version.ts';
import { isJsonValue, type JsonValue } from './json.ts';
import { RackError, statusOf } from './status.ts';
import type {
  CapEntry,
  CapsDesc,
  CapabilityRegistry,
  DiagLevel,
  EventBus,
  InstanceInfo,
  InstanceState,
  InvokeRequest,
  LeaseManager,
  OrderNodeInput,
  OrderPlanner,
  PluginDescriptor,
  PluginServices,
  ProviderEndpoint,
  Rack,
  RackOptions,
  RackPlugin,
} from './types.ts';

/** @brief Default maximum events delivered per drain pass (mirrors RackOptions). */
const DEFAULT_DISPATCH_BUDGET = 10000;

/** @brief Upper bound of the recent-diagnostics ring buffer exposed for tests. */
const MAX_RECENT_DIAGS = 128;

/** @brief Instance states that count as "already initialized" for planning and hot load. */
const INITIALIZED_STATES: ReadonlySet<InstanceState> = new Set<InstanceState>([
  'initialized',
  'active',
  'revoking',
  'faulted',
]);

/** @brief One collected diagnostic record exposed for test assertions. */
export interface RackDiagRecord {
  readonly level: DiagLevel;
  readonly message: string;
  readonly detail?: JsonValue;
}

/** @brief The Rack facade plus the optional recent-diagnostics view (kept for tests). */
export type RackWithDiagnostics = Rack & { readonly recentDiags: readonly RackDiagRecord[] };

/**
 * @brief Host-private bookkeeping for one plugged instance.
 * @note The record disappears from the table exactly when the instance is destroyed; a
 *       'faulted' record stays so the quarantined resources stay attributable.
 */
interface InstanceRecord {
  readonly plugin: RackPlugin;
  /** @brief Descriptor with defaulted init block: init {} and priority 0 when omitted. */
  readonly desc: PluginDescriptor;
  state: InstanceState;
  /** @brief Host-private monotonic generation assigned at instance creation. */
  readonly generation: number;
  readonly sourcePath?: string;
  services?: PluginServices;
  /** @brief Capability set collected from the plugin's start() via the side channel. */
  pendingCaps?: CapsDesc;
  /** @brief Position in the global initialization plan; -1 until initialized. */
  planOrdinal: number;
  /** @brief In-flight gateway invokes on this provider instance (quiescence proof). */
  inFlight: number;
  /** @brief True only while the instance's own start() call is on the stack. */
  publishAllowed: boolean;
}

/**
 * @brief Render any thrown value as a JSON-safe diagnostic summary.
 *
 * @param error Anything caught from plugin code or user callbacks.
 * @return A JSON-compatible summary for RackError details and diagnostics.
 */
function summarizeThrown(error: unknown): JsonValue {
  if (error instanceof Error) {
    return { name: error.name, message: error.message };
  }
  if (typeof error === 'string') return error;
  try {
    return String(error);
  } catch {
    return '<unprintable thrown value>';
  }
}

/**
 * @brief Coerce an unknown diagnostic detail into a JSON value.
 *
 * @param value Candidate detail of any origin.
 * @return The value itself when already JSON, a summarized string/object otherwise.
 */
function jsonDetail(value: unknown): JsonValue | undefined {
  if (value === undefined) return undefined;
  return isJsonValue(value) ? value : summarizeThrown(value);
}

/**
 * @brief Normalize a plugin descriptor, defaulting the init block.
 *
 * @param desc Descriptor as produced by the plugin (init may be entirely missing).
 * @return A frozen descriptor with init { priority 0, before [], after [] } defaults applied.
 * @throws RackError 'invalid_argument' for an empty plugId or a malformed version triple.
 */
function normalizeDescriptor(desc: PluginDescriptor): PluginDescriptor {
  if (typeof desc !== 'object' || desc === null) {
    throw new RackError('invalid_argument', 'plugin descriptor must be an object');
  }
  const candidate = desc as { plugId?: unknown; version?: unknown };
  if (typeof candidate.plugId !== 'string' || candidate.plugId.length === 0) {
    throw new RackError('invalid_argument', 'plugin descriptor requires a non-empty plugId');
  }
  if (!isPluginVersion(candidate.version)) {
    throw new RackError('invalid_argument', `plugin '${candidate.plugId}' requires a valid version triple`);
  }
  const init = desc.init ?? {};
  return Object.freeze({
    plugId: desc.plugId,
    version: desc.version,
    init: Object.freeze({
      priority: init.priority ?? 0,
      before: Object.freeze([...(init.before ?? [])]),
      after: Object.freeze([...(init.after ?? [])]),
    }),
  });
}

/**
 * @brief The rack engine: owns every module instance and the whole lifecycle state machine.
 * @note Not exported directly; createRack() is the public entry point.
 */
class RackHost implements RackWithDiagnostics {
  private readonly options: RackOptions;
  private readonly bus: EventBus;
  private readonly registry: CapabilityRegistry;
  private readonly leases: LeaseManager;
  private readonly loader: ReturnType<typeof createPluginLoader>;
  private readonly planner: OrderPlanner;
  private readonly dispatchBudget: number;
  private readonly instances = new Map<string, InstanceRecord>();
  private readonly stagedFactories: (() => RackPlugin)[] = [];
  private readonly deferredUnloads: string[] = [];
  private readonly diagLog: RackDiagRecord[] = [];
  private nextGeneration = 0;
  private nextPlanOrdinal = 0;
  private callbackDepth = 0;
  private shuttingDown = false;

  /**
   * @brief Resolve the gateway endpoint of one live provider generation.
   * @note Returned only while the instance is Active; the invoke wrapper maintains the
   *       per-instance in-flight counter used as the quiescence proof before stop().
   */
  private readonly resolveProvider = (plugId: string, generation: number): ProviderEndpoint | undefined => {
    const record = this.instances.get(plugId);
    if (record === undefined || record.generation !== generation || record.state !== 'active') {
      return undefined;
    }
    const invoke = record.plugin.invoke;
    return {
      key: `${plugId}@${generation}`,
      invoke:
        invoke === undefined
          ? (request: InvokeRequest): never => {
              throw new RackError('failed', `plugin '${plugId}' publishes methods but has no invoke entry point`, {
                plugId,
                method: request.methodName,
              });
            }
          : async (request: InvokeRequest): Promise<JsonValue> => {
              record.inFlight += 1;
              try {
                return await this.runInCallback(() => invoke(request));
              } finally {
                record.inFlight -= 1;
              }
            },
    };
  };

  /**
   * @brief Assemble the rack from the first-wave core modules.
   *
   * @param options Optional rack bounds, plugin directories and diagnostics sink.
   * @throws RackError 'invalid_argument' propagated from module construction for malformed bounds.
   */
  constructor(options?: RackOptions) {
    this.options = options ?? {};
    this.dispatchBudget = this.options.dispatchBudget ?? DEFAULT_DISPATCH_BUDGET;
    this.bus = createEventBus({
      capacity: this.options.eventCapacity,
      payloadLimit: this.options.payloadLimit,
      onSinkError: (name, error) => {
        this.diag('error', `an event sink failed for '${name}'`, { name, cause: summarizeThrown(error) });
      },
    });
    this.registry = createCapabilityRegistry({
      onSinkError: (watcherId, error) => {
        this.diag('error', `a capability sink of '${watcherId}' failed`, {
          watcherId,
          cause: summarizeThrown(error),
        });
      },
    });
    this.leases = createLeaseManager({
      lookup: (plugId) => this.registry.lookup(plugId),
      onDiag: (message, detail) => {
        this.diag('error', message, jsonDetail(detail));
      },
    });
    this.loader = createPluginLoader({
      onDiag: (level, message, detail) => {
        this.diag(level, message, jsonDetail(detail));
      },
    });
    this.planner = createOrderPlanner();
    this.gateway = createInvokeGateway({
      leases: this.leases,
      resolveProvider: this.resolveProvider,
      lookupCaps: (plugId) => this.registry.lookup(plugId),
      payloadLimit: this.options.payloadLimit,
      outputLimit: this.options.outputLimit,
      onDiag: (message, detail) => {
        this.diag('error', message, jsonDetail(detail));
      },
    });
  }

  private readonly gateway: ReturnType<typeof createInvokeGateway>;

  /** @brief Snapshot of the most recent diagnostics (bounded ring), newest last. */
  get recentDiags(): readonly RackDiagRecord[] {
    return [...this.diagLog];
  }

  /**
   * @brief Scan the configured directories, create every plugin instance, then start them.
   * @throws RackError 'invalid_state' after shutdown, 'busy' from a callback; loader,
   *         duplicate and start failures propagate after the batch is cleaned up.
   * @note Empty and missing directories list boot successfully with zero plugins.
   */
  async boot(): Promise<void> {
    this.assertEntryOpen('boot()');
    const directories = this.options.pluginDirectories ?? [];
    const created: InstanceRecord[] = [];
    try {
      for (const directory of directories) {
        const files = await this.loader.discover(directory);
        for (const file of files) {
          const generation = this.nextGeneration + 1;
          const module = await this.loader.load(file.path, generation);
          this.nextGeneration = generation;
          created.push(this.createRecord(module.create(), file.path, generation));
        }
      }
    } catch (error) {
      await this.discardAll(created);
      throw error;
    }
    await this.start();
  }

  /**
   * @brief Stage an in-process plugin factory for the next start() call.
   *
   * @param create Factory producing the plugin instance; invoked once during start().
   * @throws RackError 'invalid_state' after shutdown, 'busy' from inside a plugin callback.
   */
  add(create: () => RackPlugin): void {
    this.assertEntryOpen('add()');
    this.stagedFactories.push(create);
  }

  /**
   * @brief Initialize and start every staged or otherwise pending instance.
   *
   * @throws RackError with the first failure ('invalid_argument' / 'not_found' / 'cycle' /
   *         'invalid_state' from the planner, 'failed' for plugin or registration errors,
   *         'duplicate' for clashing ids); the whole batch rolls back in reverse plan order.
   */
  async start(): Promise<void> {
    this.assertEntryOpen('start()');

    // Materialize staged factories first; any failure destroys only what this call created.
    const factories = this.stagedFactories.splice(0, this.stagedFactories.length);
    const materialized: InstanceRecord[] = [];
    for (const factory of factories) {
      let plugin: RackPlugin;
      try {
        plugin = factory();
      } catch (error) {
        await this.discardAll(materialized);
        throw new RackError('failed', 'a staged plugin factory threw during create()', {
          cause: summarizeThrown(error),
        });
      }
      try {
        materialized.push(this.createRecord(plugin));
      } catch (error) {
        await this.discardAll(materialized);
        throw error;
      }
    }

    const pending = [...this.instances.values()].filter((record) => record.state === 'created');
    if (pending.length === 0) return;

    const initializedIds = new Set<string>();
    for (const record of this.instances.values()) {
      if (INITIALIZED_STATES.has(record.state)) initializedIds.add(record.desc.plugId);
    }

    let order: number[];
    try {
      const nodes: OrderNodeInput[] = pending.map((record) => ({
        plugId: record.desc.plugId,
        priority: record.desc.init.priority ?? 0,
        before: record.desc.init.before ?? [],
        after: record.desc.init.after ?? [],
      }));
      order = this.planner.plan(nodes, initializedIds);
    } catch (error) {
      // Planning failed: the whole pending batch is abandoned and destroyed.
      await this.discardAll(pending);
      throw error;
    }
    const plan = order.map((index) => pending[index]!);

    // Phase one: init everything in plan order before anything starts.
    const initialized: InstanceRecord[] = [];
    for (const record of plan) {
      try {
        await this.initInstance(record);
        initialized.push(record);
      } catch (error) {
        for (let index = initialized.length - 1; index >= 0; index -= 1) {
          await this.rollbackInstance(initialized[index]!, false);
        }
        for (const rest of plan) {
          if (rest.state === 'created') await this.destroyRecord(rest);
        }
        throw error;
      }
    }

    // Phase two: start in plan order; register capabilities only after start() returned.
    for (const record of plan) {
      try {
        await this.startInstance(record);
      } catch (error) {
        for (let index = plan.length - 1; index >= 0; index -= 1) {
          const target = plan[index]!;
          if (!this.instances.has(target.desc.plugId)) continue;
          if (target.state === 'created') {
            await this.destroyRecord(target);
          } else {
            await this.rollbackInstance(target, true);
          }
        }
        throw error;
      }
      // Batch capability notices were already delivered synchronously by the registry;
      // this drain only dispatches queued ordinary events. Deferred unloads are never
      // consumed while a startup batch is running.
      await this.runInCallback(() => this.bus.drain(this.dispatchBudget));
    }
  }

  /**
   * @brief Hot-load one plugin module file and start it as a single-instance batch.
   *
   * @param path Module file path; relative paths resolve against the process cwd.
   * @throws RackError 'not_found' / 'unsupported' / 'invalid_argument' / 'failed' from the
   *         loader, 'duplicate' when the plugId already exists (the original is kept),
   *         'invalid_state' when a hot-load `before` constraint hits an initialized plugin.
   */
  async load(path: string): Promise<void> {
    this.assertEntryOpen('load()');
    const generation = this.nextGeneration + 1;
    const module = await this.loader.load(path, generation);
    this.nextGeneration = generation;
    this.createRecord(module.create(), path, generation);
    await this.start();
  }

  /**
   * @brief Run the full unload protocol for one instance.
   *
   * @param plugId Identity to unload.
   * @throws RackError 'not_found' for unknown ids, 'invalid_state' after shutdown or for a
   *         quarantined instance, 'failed' when leases were not returned or stop() threw
   *         (the instance stays isolated as 'faulted'), 'busy' while calls are in flight
   *         (the instance stays 'revoking' and the unload can be retried).
   * @note Inside a plugin callback the request is queued and the promise resolves normally
   *       ('deferred' semantics); poll() executes the queued requests.
   */
  async unload(plugId: string): Promise<void> {
    if (this.shuttingDown) {
      throw new RackError('invalid_state', `the rack is shutting down; '${plugId}' is handled by shutdown()`, {
        plugId,
      });
    }
    if (!this.instances.has(plugId)) {
      throw new RackError('not_found', `unknown plugin '${plugId}'`, { plugId });
    }
    if (this.callbackDepth > 0) {
      this.queueDeferredUnload(plugId);
      return;
    }
    await this.runUnloadProtocol(plugId);
  }

  /**
   * @brief Unload one file-based instance and freshly load the same source.
   *
   * @param plugId Identity to reload; must have been loaded from a module file.
   * @throws RackError 'invalid_state' for in-process (add()-ed) plugins without a source
   *         path, plus every unload() and load() failure; a failure after the unload leaves
   *         the plugin absent from the rack.
   * @note The fresh module is evaluated under a new generation URL, so every old credential
   *       dies (stale) even when the disclosed version is identical.
   */
  async reload(plugId: string): Promise<void> {
    this.assertEntryOpen('reload()');
    const record = this.instances.get(plugId);
    if (record === undefined) {
      throw new RackError('not_found', `unknown plugin '${plugId}'`, { plugId });
    }
    if (record.sourcePath === undefined) {
      throw new RackError('invalid_state', `plugin '${plugId}' has no source path and cannot be reloaded`, {
        plugId,
      });
    }
    const sourcePath = record.sourcePath;
    await this.runUnloadProtocol(plugId);
    const generation = this.nextGeneration + 1;
    const module = await this.loader.load(sourcePath, generation);
    this.nextGeneration = generation;
    this.createRecord(module.create(), sourcePath, generation);
    await this.start();
  }

  /**
   * @brief Drain queued events, execute deferred unload requests, then drain again.
   * @throws RackError 'busy' when called from inside a plugin callback.
   * @note Failed deferred requests leave the queue unless retryable: quarantined
   *       ('faulted') and vanished instances drop their request, 'busy' ones stay queued.
   */
  async poll(): Promise<void> {
    if (this.callbackDepth > 0) {
      throw new RackError('busy', 'poll() cannot run while a plugin callback is on the stack');
    }
    await this.runInCallback(() => this.bus.drain(this.dispatchBudget));
    const queue = [...this.deferredUnloads];
    for (const plugId of queue) {
      if (!this.deferredUnloads.includes(plugId)) continue;
      if (!this.instances.has(plugId)) {
        this.dropDeferredUnload(plugId);
        continue;
      }
      try {
        await this.runUnloadProtocol(plugId);
      } catch (error) {
        const status = statusOf(error);
        if (status === 'busy') {
          this.queueDeferredUnload(plugId);
        } else {
          this.dropDeferredUnload(plugId);
          this.diag(status === 'failed' ? 'error' : 'warn', `a deferred unload of '${plugId}' failed (${status})`, {
            plugId,
            status,
          });
        }
      }
    }
    await this.runInCallback(() => this.bus.drain(this.dispatchBudget));
  }

  /**
   * @brief Tear the whole rack down in reverse initialization order, best effort.
   * @throws RackError 'busy' when called from inside a plugin callback.
   * @note Never throws for plugin failures: instances that cannot prove quiescence stay
   *       isolated ('faulted') with their resources, recorded through diagnostics.
   */
  async shutdown(): Promise<void> {
    if (this.shuttingDown) return;
    if (this.callbackDepth > 0) {
      throw new RackError('busy', 'shutdown cannot run while a plugin callback is on the stack');
    }
    this.shuttingDown = true;
    this.deferredUnloads.length = 0;
    const ordered = [...this.instances.values()].sort((left, right) => right.planOrdinal - left.planOrdinal);
    for (const record of ordered) {
      const plugId = record.desc.plugId;
      try {
        await this.runUnloadProtocol(plugId);
      } catch (error) {
        this.diag('error', `shutdown could not unload '${plugId}' (${statusOf(error)}); instance retained`, {
          plugId,
          status: statusOf(error),
        });
      }
    }
    if (this.instances.size > 0) {
      const retained = [...this.instances.keys()].sort();
      this.diag('error', `shutdown finished with quarantined instances: ${retained.join(', ')}`, { retained });
    }
  }

  /** @brief Deterministic snapshot of every known instance, ordered by plan position. */
  plugins(): readonly InstanceInfo[] {
    return [...this.instances.values()]
      .sort((left, right) => left.planOrdinal - right.planOrdinal)
      .map((record) => this.snapshotOf(record));
  }

  /**
   * @brief Probe one instance's identity, version, state and generation without leasing.
   * @param plugId Identity to look up.
   * @return The frozen snapshot, or undefined for unknown ids.
   */
  describe(plugId: string): InstanceInfo | undefined {
    const record = this.instances.get(plugId);
    return record === undefined ? undefined : this.snapshotOf(record);
  }

  // -------------------------------------------------------------------------
  // Internals
  // -------------------------------------------------------------------------

  /** @brief Reject a structural entry point after shutdown or from inside a callback. */
  private assertEntryOpen(operation: string): void {
    if (this.shuttingDown) {
      throw new RackError('invalid_state', `the rack is shut down; ${operation} is closed`);
    }
    if (this.callbackDepth > 0) {
      throw new RackError('busy', `${operation} cannot run while a plugin callback is on the stack`);
    }
  }

  /**
   * @brief Run one plugin-facing operation with the callback depth raised.
   * @note The depth stays elevated across awaits, so reentrant host operations observe it.
   */
  private async runInCallback<T>(operation: () => T | Promise<T>): Promise<T> {
    this.callbackDepth += 1;
    try {
      return await operation();
    } finally {
      this.callbackDepth -= 1;
    }
  }

  /** @brief Queue one deferred unload request, deduplicated by identity. */
  private queueDeferredUnload(plugId: string): void {
    if (!this.deferredUnloads.includes(plugId)) this.deferredUnloads.push(plugId);
  }

  /** @brief Drop every queued unload request naming one identity. */
  private dropDeferredUnload(plugId: string): void {
    const index = this.deferredUnloads.indexOf(plugId);
    if (index >= 0) this.deferredUnloads.splice(index, 1);
  }

  /** @brief Record one diagnostic and forward it to the optional host sink. */
  private diag(level: DiagLevel, message: string, detail?: unknown): void {
    const safeDetail = jsonDetail(detail);
    this.diagLog.push({ level, message, ...(safeDetail !== undefined ? { detail: safeDetail } : {}) });
    if (this.diagLog.length > MAX_RECENT_DIAGS) {
      this.diagLog.splice(0, this.diagLog.length - MAX_RECENT_DIAGS);
    }
    try {
      this.options.onDiag?.(level, message, safeDetail);
    } catch {
      // A failing diagnostics sink must never break host operations.
    }
  }

  /** @brief Build the frozen InstanceInfo snapshot of one record. */
  private snapshotOf(record: InstanceRecord): InstanceInfo {
    return Object.freeze<InstanceInfo>({
      plugId: record.desc.plugId,
      version: record.desc.version,
      state: record.state,
      generation: record.generation,
      ...(record.sourcePath !== undefined ? { sourcePath: record.sourcePath } : {}),
    });
  }

  /**
   * @brief Create and register one instance record in the 'created' state.
   * @throws RackError 'invalid_argument' for a malformed descriptor, 'duplicate' when the
   *         plugId is already present (the existing instance is untouched).
   */
  private createRecord(plugin: RackPlugin, sourcePath?: string, generation?: number): InstanceRecord {
    const desc = normalizeDescriptor(plugin.desc);
    const existing = this.instances.get(desc.plugId);
    if (existing !== undefined) {
      throw new RackError('duplicate', `plugin id '${desc.plugId}' is already present in the rack`, {
        plugId: desc.plugId,
        existingState: existing.state,
      });
    }
    const assigned = generation ?? this.nextGeneration + 1;
    this.nextGeneration = Math.max(this.nextGeneration, assigned);
    const record: InstanceRecord = {
      plugin,
      desc,
      state: 'created',
      generation: assigned,
      ...(sourcePath !== undefined ? { sourcePath } : {}),
      services: undefined,
      pendingCaps: undefined,
      planOrdinal: -1,
      inFlight: 0,
      publishAllowed: false,
    };
    this.instances.set(desc.plugId, record);
    return record;
  }

  /**
   * @brief Inject the services view and run one plugin's init().
   * @throws RackError 'failed' when init() throws; the record stays 'created' so the batch
   *         rollback destroys it without calling stop().
   */
  private async initInstance(record: InstanceRecord): Promise<void> {
    const plugId = record.desc.plugId;
    const services = createPluginServices(
      {
        bus: this.bus,
        registry: this.registry,
        leases: this.leases,
        calls: this.gateway,
        capsPublishAllowed: () => record.publishAllowed,
        consumerActive: () => record.state === 'active',
        onDiag: (level, message, detail) => {
          this.diag(level, message, detail);
        },
        onCapsPublished: (caps) => {
          if (record.pendingCaps === undefined) {
            record.pendingCaps = caps;
          } else {
            this.diag('warn', `plugin '${plugId}' published capabilities more than once; the first set is kept`, {
              plugId,
            });
          }
        },
      },
      plugId,
    );
    record.services = services;
    try {
      await this.runInCallback(() => record.plugin.init(services));
    } catch (error) {
      throw new RackError('failed', `init of plugin '${plugId}' failed`, {
        plugId,
        phase: 'init',
        cause: summarizeThrown(error),
      });
    }
    record.state = 'initialized';
    record.planOrdinal = this.nextPlanOrdinal;
    this.nextPlanOrdinal += 1;
  }

  /**
   * @brief Run one plugin's start() and register the announced capabilities afterwards.
   * @throws RackError 'failed' when start() throws or the registration is rejected; the
   *         uncommitted capability set is dropped so nothing is registered for it.
   */
  private async startInstance(record: InstanceRecord): Promise<void> {
    const plugId = record.desc.plugId;
    record.publishAllowed = true;
    try {
      try {
        await this.runInCallback(() => record.plugin.start());
      } catch (error) {
        throw new RackError('failed', `start of plugin '${plugId}' failed`, {
          plugId,
          phase: 'start',
          cause: summarizeThrown(error),
        });
      }
      record.state = 'active';
      const caps = record.pendingCaps;
      if (caps !== undefined) {
        const entry: CapEntry = {
          plugId,
          generation: record.generation,
          version: record.desc.version,
          caps,
        };
        try {
          await this.runInCallback(() => {
            this.registry.register(entry);
          });
        } catch (error) {
          throw new RackError('failed', `capability registration of plugin '${plugId}' failed`, {
            plugId,
            phase: 'register',
            cause: summarizeThrown(error),
          });
        }
      }
    } finally {
      record.publishAllowed = false;
    }
  }

  /**
   * @brief Tear one instance down during a batch rollback (reverse plan order).
   * @param withdrawCaps Whether to withdraw a registered capability set first.
   * @note Best effort: stop()/destroy() failures are recorded and the record is dropped
   *       regardless, because the batch has already failed and the operator was informed.
   */
  private async rollbackInstance(record: InstanceRecord, withdrawCaps: boolean): Promise<void> {
    const plugId = record.desc.plugId;
    if (withdrawCaps) {
      await this.runInCallback(() => {
        this.registry.withdraw(plugId, record.generation);
      });
    }
    try {
      await this.runInCallback(() => record.plugin.stop());
    } catch (error) {
      this.diag('error', `stop() of '${plugId}' threw during rollback; the instance is destroyed anyway`, {
        plugId,
        cause: summarizeThrown(error),
      });
    }
    this.leases.releaseAllFrom(plugId);
    this.bus.unsubscribeAll(plugId);
    await this.destroyRecord(record);
  }

  /** @brief Destroy one record and remove it from the table (never calls stop()). */
  private async destroyRecord(record: InstanceRecord): Promise<void> {
    const plugId = record.desc.plugId;
    try {
      await this.runInCallback(() => record.plugin.destroy?.());
    } catch (error) {
      this.diag('error', `destroy() of '${plugId}' threw; the record is dropped regardless`, {
        plugId,
        cause: summarizeThrown(error),
      });
    }
    record.state = 'destroyed';
    this.instances.delete(plugId);
    this.dropDeferredUnload(plugId);
  }

  /** @brief Destroy a list of records; used by batch failure cleanup paths. */
  private async discardAll(records: readonly InstanceRecord[]): Promise<void> {
    for (const record of records) {
      await this.destroyRecord(record);
    }
  }

  /**
   * @brief Execute the explicit unload protocol for one identity.
   *
   * @note Sequence: drop queued requests for the identity, discard never-initialized
   *       records without stop(), then withdraw -> revoke leases -> prove zero in-flight
   *       calls -> stop -> release outgoing leases and subscriptions -> destroy -> remove.
   *       Unreturned leases and throwing stop() quarantine the instance as 'faulted';
   *       in-flight calls leave it 'revoking' for a later retry.
   */
  private async runUnloadProtocol(plugId: string): Promise<void> {
    this.dropDeferredUnload(plugId);
    const record = this.instances.get(plugId);
    if (record === undefined) {
      throw new RackError('not_found', `unknown plugin '${plugId}'`, { plugId });
    }
    if (record.state === 'faulted') {
      throw new RackError(
        'invalid_state',
        `plugin '${plugId}' is quarantined after a failed unload; stop() is never retried automatically`,
        { plugId },
      );
    }
    if (record.state === 'created') {
      // Never initialized: nothing is registered, subscribed or leased; discard only.
      await this.destroyRecord(record);
      return;
    }

    record.state = 'revoking';
    await this.runInCallback(() => {
      this.registry.withdraw(plugId, record.generation);
    });
    const revoked = await this.runInCallback(() => this.leases.revokeProvider(plugId, record.generation));
    if (revoked === 'failed') {
      record.state = 'faulted';
      this.diag('error', `unload of '${plugId}' failed: leases were not returned; instance quarantined`, {
        plugId,
      });
      throw new RackError('failed', `borrowings of '${plugId}' were not returned; the instance is quarantined`, {
        plugId,
      });
    }
    if (record.inFlight > 0) {
      // Stay in 'revoking' so a later unload attempt retries the protocol.
      throw new RackError('busy', `plugin '${plugId}' still has in-flight calls; retry the unload later`, {
        plugId,
      });
    }
    try {
      await this.runInCallback(() => record.plugin.stop());
    } catch (error) {
      record.state = 'faulted';
      this.diag('error', `stop() of '${plugId}' threw; instance quarantined`, {
        plugId,
        cause: summarizeThrown(error),
      });
      throw new RackError('failed', `stop() of '${plugId}' failed; the instance is quarantined`, {
        plugId,
        cause: summarizeThrown(error),
      });
    }
    this.leases.releaseAllFrom(plugId);
    this.bus.unsubscribeAll(plugId);
    try {
      await this.runInCallback(() => record.plugin.destroy?.());
    } catch (error) {
      this.diag('error', `destroy() of '${plugId}' threw; the record is dropped regardless`, {
        plugId,
        cause: summarizeThrown(error),
      });
    }
    record.state = 'destroyed';
    this.instances.delete(plugId);
    this.dropDeferredUnload(plugId);
  }
}

/**
 * @brief Create a rack: the host facade assembling the whole 42U plugin framework.
 *
 * @param options Optional plugin directories, resource bounds and a diagnostics sink.
 * @return The Rack facade; it additionally carries a bounded `recentDiags` snapshot view
 *         for test assertions.
 *
 * @note All operations run on the Node event loop. Plugin callbacks (lifecycle methods,
 *       invoke, event delivery, capability notices, onRevoke) are tracked through a depth
 *       counter: unload requests from inside them defer to poll(), every structural entry
 *       point (boot/start/load/add/shutdown) rejects with RackError('busy').
 */
export function createRack(options?: RackOptions): RackWithDiagnostics {
  return new RackHost(options);
}
