/**
 * @file 42U rack frozen contracts (TS edition).
 *
 * @brief Every public type of the TS rack. This file is the equivalent of the C++ `abi.hpp` +
 *        `host.hpp` surface and is FROZEN: implementing agents must not modify it. Report needed
 *        changes to the orchestrator instead of editing.
 *
 * @note Faithful adaptation of the 42U rack ideas (docs/42U插件框架设计.md, ABI v3):
 *       - Host-orchestrated rack; plugins are devices plugged into standardized slots.
 *       - invoke-only: the sole business path is consumer -> host gateway -> provider invoke.
 *       - Version-qualified leases: acquire(plugId, closedRange) -> credential + actual version.
 *       - Capability disclosure on start(), broadcast withdrawal on unload, snapshot+watch.
 *       - Queued bounded event bus with reserved `u42.*` names.
 *       - Deterministic init ordering (priority + before/after, cycle rejection).
 *       - Explicit unload protocol with revocation, quiescence and generation-stale credentials.
 *       TS-native replacements: JSON data plane, async/await, dynamic import() loading,
 *       RackError instead of status returns, no binary ABI concerns.
 */

import type { JsonValue } from './json.ts';
import type { RackStatus } from './status.ts';

/** @brief Convenience re-export of the JSON data plane so consumers can import it from the
 *         frozen contract module directly. */
export type { JsonValue } from './json.ts';

/** @brief Framework contract version of the TS rack (parallel to the C++ abi_major idea). */
export const RACK_CONTRACT_VERSION = 1;

// ---------------------------------------------------------------------------
// Identity, versions and capabilities
// ---------------------------------------------------------------------------

/**
 * @brief Numeric plugin business version: the provider's own compatibility promise.
 * @note Distinct from RACK_CONTRACT_VERSION and from host-private instance generations. All
 *       triples including 0.0.0 are valid; major-compatibility is promised by the provider and
 *       only honoured when a caller explicitly writes a range that crosses a major boundary.
 */
export interface PluginVersion {
  readonly major: number;
  readonly minor: number;
  readonly patch: number;
}

/** @brief Explicit inclusive version range [minimum, maximum]; both endpoints included. */
export interface VersionRange {
  readonly minimum: PluginVersion;
  readonly maximum: PluginVersion;
}

/**
 * @brief Host-minted opaque lease credential; consumers must never fabricate or decode it.
 * @note Unknown credentials fail the lease table lookup with 'stale'; credentials never survive
 *       a provider reload, even when the disclosed version is identical.
 */
export interface LeaseCredential {
  readonly kind: 'u42.lease.credential';
  /** @brief Host-private payload; treat as opaque. */
  readonly handle: object;
}

/** @brief Initialization declaration attached to a plugin descriptor. */
export interface InitOrderSpec {
  /** @brief Signed ordering hint; smaller initializes first. Default 0. */
  readonly priority?: number;
  /** @brief Hard edges: this plugin initializes before the listed plug ids. */
  readonly before?: readonly string[];
  /** @brief Hard edges: this plugin initializes after the listed plug ids. */
  readonly after?: readonly string[];
}

/** @brief Immutable plugin identity, business version and init constraints. */
export interface PluginDescriptor {
  /** @brief Stable non-empty UTF-8 identity, e.g. "com.example.storage". */
  readonly plugId: string;
  readonly version: PluginVersion;
  readonly init: InitOrderSpec;
}

/** @brief One disclosed synchronous method; names and ids are unique within a plugin. */
export interface MethodDesc {
  readonly id: number;
  readonly name: string;
  readonly description?: string;
  /** @brief Optional JSON-schema-shaped description of the args document. */
  readonly inputSchema?: JsonValue;
  /** @brief Optional JSON-schema-shaped description of the result document. */
  readonly outputSchema?: JsonValue;
}

/** @brief Complete method announcement of one instance; frozen for the instance lifetime. */
export interface CapsDesc {
  readonly methods: readonly MethodDesc[];
}

/** @brief Capability availability or withdrawal fact delivered to cap sinks. */
export interface CapEvent {
  readonly plugId: string;
  readonly available: boolean;
  readonly version: PluginVersion;
  /** @brief Methods on availability; empty object set on withdrawal. */
  readonly capabilities: CapsDesc;
}

/** @brief Host-side capability record for one published provider instance. */
export interface CapEntry {
  readonly plugId: string;
  readonly generation: number;
  readonly version: PluginVersion;
  readonly caps: CapsDesc;
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

/** @brief One delivered event; the payload is valid only during delivery. */
export interface RackEvent {
  readonly name: string;
  readonly payload: JsonValue;
}

/** @brief Receiver interface implemented by subscribers. */
export interface EventSink {
  onEvent(event: RackEvent): void | Promise<void>;
}

/** @brief Capability observation receiver (snapshot first, then ordered changes). */
export interface CapSink {
  onCapability(event: CapEvent): void | Promise<void>;
}

/**
 * @brief Synchronous revocation receiver for one outstanding lease.
 * @note The consumer must stop dependent business inside onRevoke and release the credential
 *       before returning (or retain it and retry when release reports 'busy'). No new business
 *       calls against the revoking provider may be issued after the notification.
 */
export interface Revoker {
  onRevoke(credential: LeaseCredential): void | Promise<void>;
}

/** @brief Subscription handle; unsubscribe is idempotent and owned by the subscribing plugin. */
export interface Subscription {
  unsubscribe(): void;
}

// ---------------------------------------------------------------------------
// Plugin-facing services (the ictx equivalent)
// ---------------------------------------------------------------------------

/**
 * @brief Per-instance event service view.
 * @note publish() enqueues without recursive dispatch and rejects reserved `u42.*` names.
 */
export interface EventService {
  subscribe(name: string, sink: EventSink): Subscription;
  /** @throws RackError 'invalid_argument' for reserved `u42.*` names or oversized payloads,
   *          'limit_exceeded' when the queue is full. */
  publish(name: string, payload: JsonValue): void;
}

/**
 * @brief Per-instance capability service view.
 * @note Capabilities may only be published from within start(); the method set is then frozen
 *       for the instance lifetime. watch() atomically delivers the current snapshot followed by
 *       subsequent changes in order.
 */
export interface CapabilityService {
  /** @throws RackError 'invalid_state' when called outside start(). */
  publish(caps: CapsDesc): void;
  watch(sink: CapSink): Subscription;
  snapshot(): readonly CapEvent[];
  /**
   * @brief Borrow a revocable lease on the current provider instance for plugId.
   * @throws RackError 'invalid_argument' for a reversed range or missing provider,
   *          'unsupported' when the actual version is outside the range,
   *          'not_found' when no active provider publishes for plugId.
   * @return Credential plus the by-value actual version; never a provider reference.
   */
  acquire(plugId: string, range: VersionRange, revoker: Revoker): LeaseHandle;
  /**
   * @brief Return a lease; the only operation that clears it.
   * @retval 'ok' lease returned; 'busy' in-flight call outstanding, lease kept;
   *         'stale' unknown or already returned credential.
   */
  release(credential: LeaseCredential): 'ok' | 'busy' | 'stale';
}

/**
 * @brief Per-instance invoke gateway view; the only business call path.
 * @note Both entries share one implementation (name and id resolve identically). Only Active
 *       consumers may call; Initialized consumers may hold leases but not call.
 */
export interface CallService {
  call(credential: LeaseCredential, methodName: string, args: JsonValue): Promise<JsonValue>;
  callById(credential: LeaseCredential, methodId: number, args: JsonValue): Promise<JsonValue>;
}

/** @brief Diagnostic severity. */
export type DiagLevel = 'info' | 'warn' | 'error';

/** @brief Per-instance diagnostics reporting channel. */
export interface DiagService {
  report(level: DiagLevel, message: string, detail?: JsonValue): void;
}

/** @brief Host services injected into init(); the ictx equivalent. */
export interface PluginServices {
  readonly plugId: string;
  readonly events: EventService;
  readonly caps: CapabilityService;
  readonly calls: CallService;
  readonly diag: DiagService;
}

// ---------------------------------------------------------------------------
// Plugin contract
// ---------------------------------------------------------------------------

/** @brief One dynamic-call request delivered by the host gateway to the provider. */
export interface InvokeRequest {
  readonly methodId: number;
  readonly methodName: string;
  readonly args: JsonValue;
}

/**
 * @brief Plugin instance contract implemented by every rack plugin.
 * @note init() may only subscribe and prepare; start() publishes capabilities; stop() must end
 *       all plugin-side asynchronous work; destroy() releases plugin-owned resources. invoke()
 *       is called only by the host gateway. Throwing anywhere is converted by the host into
 *       'failed' plus a diagnostic record.
 */
export interface RackPlugin {
  readonly desc: PluginDescriptor;
  init(services: PluginServices): void | Promise<void>;
  start(): void | Promise<void>;
  stop(): void | Promise<void>;
  destroy?(): void | Promise<void>;
  /** @brief Dynamic method entry; implement when the plugin publishes any method. */
  invoke?(request: InvokeRequest): JsonValue | Promise<JsonValue>;
}

/** @brief Shape a plugin module must export (default export or named `create`). */
export interface PluginModule {
  create(): RackPlugin;
}

// ---------------------------------------------------------------------------
// Host-side orchestration types
// ---------------------------------------------------------------------------

/** @brief Instance lifecycle states, mirroring the C++ state machine. */
export type InstanceState =
  | 'created'
  | 'initialized'
  | 'active'
  | 'revoking'
  | 'stopped'
  | 'faulted'
  | 'destroyed';

/** @brief Read-only instance snapshot for diagnostics and tests. */
export interface InstanceInfo {
  readonly plugId: string;
  readonly version: PluginVersion;
  readonly state: InstanceState;
  /** @brief Host-private generation surfaced read-only for observability. */
  readonly generation: number;
  readonly sourcePath?: string;
}

/** @brief Bounded rack resources; mirrors host_options of the C++ host. */
export interface RackOptions {
  /** @brief Directories scanned by boot(); non-recursive, deterministic order. */
  readonly pluginDirectories?: readonly string[];
  /** @brief Maximum queued ordinary events. Default 1024. */
  readonly eventCapacity?: number;
  /** @brief Maximum estimated bytes per event payload or call args. Default 1 MiB. */
  readonly payloadLimit?: number;
  /** @brief Maximum estimated bytes per call result. Default 1 MiB. */
  readonly outputLimit?: number;
  /** @brief Maximum events and notices delivered per drain. Default 10000. */
  readonly dispatchBudget?: number;
  /** @brief Host-level diagnostics sink. */
  readonly onDiag?: (level: DiagLevel, message: string, detail?: JsonValue) => void;
}

/** @brief Result of a lease acquisition: credential plus by-value actual version. */
export interface LeaseHandle {
  readonly credential: LeaseCredential;
  readonly plugId: string;
  /** @brief Actual version copied by value from the provider descriptor at acquire time. */
  readonly version: PluginVersion;
  /**
   * @brief Return the lease; resolves 'ok' or 'stale' after clearing, 'busy' while an in-flight
   *        call is outstanding (lease kept, retry later).
   */
  release(): Promise<'ok' | 'busy' | 'stale'>;
}

// ---------------------------------------------------------------------------
// Internal module ports (frozen interconnection between core modules)
// ---------------------------------------------------------------------------

/** @brief Planner input for one pending plugin. */
export interface OrderNodeInput {
  readonly plugId: string;
  readonly priority: number;
  readonly before: readonly string[];
  readonly after: readonly string[];
}

/**
 * @brief Deterministic initialization planner (implemented in core/order.ts).
 * @throws RackError 'invalid_argument' for empty/duplicate ids or self references,
 *         'not_found' for unknown before/after targets,
 *         'cycle' when constraints form a cycle,
 *         'invalid_state' when a `before` target is already initialized (hot-load rule).
 */
export interface OrderPlanner {
  /**
   * @param nodes Pending plugins with their constraints.
   * @param initializedIds Ids already initialized (hot-load): `after` may be satisfied by them,
   *        `before` against them is impossible and must be rejected.
   * @return Indices into nodes in plan order.
   */
  plan(nodes: readonly OrderNodeInput[], initializedIds: ReadonlySet<string>): number[];
}

/**
 * @brief Queued event bus (implemented in core/events.ts).
 * @note Delivery happens only in drain() at safe scheduling points; publish never dispatches
 *       recursively. Reserved `u42.*` names are rejected for plugin publishers.
 */
export interface EventBus {
  subscribe(name: string, sink: EventSink, ownerId: string): Subscription;
  /** @throws RackError 'invalid_argument' (reserved name / non-JSON payload / bad size),
   *          'limit_exceeded' when the queue is full. */
  publish(name: string, payload: JsonValue, publisherId: string): void;
  /** @brief Drop every subscription owned by ownerId (unload-time host action). */
  unsubscribeAll(ownerId: string): void;
  /** @brief Deliver queued events serially; returns the number delivered. */
  drain(budget: number): Promise<number>;
  /** @brief Currently queued event count. */
  readonly pending: number;
}

/**
 * @brief Capability registry (implemented in core/caps.ts).
 * @note register() corresponds to a successful start(); withdraw() broadcasts withdrawal. The
 *       snapshot+watch pair is atomic: watchers see the current facts first, then changes.
 */
export interface CapabilityRegistry {
  /** @throws RackError 'duplicate' when plugId is already registered with the same generation. */
  register(entry: CapEntry): void;
  /** @brief Withdraw by exact generation; unknown or stale generations are ignored. */
  withdraw(plugId: string, generation: number): void;
  /** @brief Atomic snapshot + follow subscription. */
  watch(sink: CapSink, watcherId: string): Subscription & { readonly snapshot: readonly CapEvent[] };
  snapshot(): readonly CapEvent[];
  /** @brief Current entry for lease acquisition; undefined when absent or withdrawn. */
  lookup(plugId: string): CapEntry | undefined;
}

/** @brief Live lease record exposed to the invoke gateway after validation. */
export interface ActiveLeaseInfo {
  readonly credential: LeaseCredential;
  readonly consumerId: string;
  readonly providerPlugId: string;
  readonly providerGeneration: number;
  readonly version: PluginVersion;
}

/** @brief In-flight call guard returned by LeaseManager.beginCall. */
export interface LeaseCallGuard {
  readonly lease: ActiveLeaseInfo;
  /** @brief Decrement the in-flight count; must run in finally. */
  endCall(): void;
}

/**
 * @brief Lease manager (implemented in core/lease.ts).
 * @note Credentials embed the host-private generation; they never match a reloaded provider.
 */
export interface LeaseManager {
  /**
   * @brief Validate the explicit closed range against the provider's actual version and mint a
   *        lease bound to the current provider generation.
   * @throws RackError 'invalid_argument' (reversed range), 'not_found' (no provider entry),
   *          'unsupported' (actual version outside the range).
   */
  acquire(consumerId: string, plugId: string, range: VersionRange, revoker: Revoker): LeaseHandle;
  /**
   * @brief Return a lease; the only operation that clears it.
   * @retval 'ok' returned; 'busy' in-flight call outstanding (kept); 'stale' unknown credential
   *         or wrong owner.
   */
  release(credential: LeaseCredential, consumerId: string): 'ok' | 'busy' | 'stale';
  /**
   * @brief Validate a credential for a business call and increment its in-flight count.
   * @throws RackError 'invalid_argument' (malformed), 'stale' (returned / wrong owner /
   *          provider reloaded), 'busy' re-entering the same provider concurrently.
   * @return Guard whose endCall() must run in finally.
   */
  beginCall(credential: LeaseCredential, consumerId: string): LeaseCallGuard;
  /**
   * @brief Revoke every lease on one provider generation: notify revokers synchronously, then
   *        require credentials returned; unreturned leases block completion.
   * @retval 'ok' all leases returned; 'failed' at least one lease unreturned (isolation).
   */
  revokeProvider(providerPlugId: string, generation: number): Promise<'ok' | 'failed'>;
  /** @brief Host-side cleanup of all outgoing leases held by a stopping consumer. */
  releaseAllFrom(consumerId: string): void;
  /** @brief Whether any lease is still outstanding (blocks provider unload). */
  hasOutstandingFor(providerPlugId: string, generation: number): boolean;
  /** @brief Total outstanding leases. */
  readonly activeCount: number;
}

/** @brief Provider endpoint resolved by the host for one live generation. */
export interface ProviderEndpoint {
  /** @brief Stable per-instance key used for reentrancy tracking. */
  readonly key: string;
  invoke(request: InvokeRequest): Promise<JsonValue> | JsonValue;
}

/**
 * @brief Invoke gateway (implemented in core/calls.ts): consumer -> host -> provider.
 * @note Name and id entries share one validation path; results are size-checked against
 *       outputLimit; provider exceptions become RackError('failed') with diagnostics hook.
 */
export interface InvokeGateway {
  call(
    consumerId: string,
    credential: LeaseCredential,
    method: { readonly byName: string } | { readonly byId: number },
    args: JsonValue,
  ): Promise<JsonValue>;
}

/** @brief Loader-discovered candidate file. */
export interface DiscoveredFile {
  readonly path: string;
}

/**
 * @brief Plugin module loader (implemented in core/loader.ts).
 * @note Modules are ESM files matching `*.plug.ts` / `*.plug.mjs` / `*.plug.js`, exporting a
 *       `create(): RackPlugin` factory as default or named export. Reloads import a fresh URL
 *       (cache-busting query) so a new generation re-evaluates the module; the old module is
 *       left to the GC once the host drops its references — the JS replacement for dlclose().
 */
export interface PluginLoader {
  /** @brief Non-recursive deterministic scan (sorted by path) of one directory. */
  discover(directory: string): Promise<readonly DiscoveredFile[]>;
  /**
   * @brief Import and validate one plugin module for the given generation.
   * @throws RackError 'not_found' (missing file), 'unsupported' (no compatible export),
   *          'invalid_argument' (invalid descriptor), 'failed' (module evaluation threw).
   */
  load(path: string, generation: number): Promise<PluginModule>;
}

/**
 * @brief Rack facade (implemented in core/host.ts): the u42::host equivalent.
 * @note All operations run on the Node event loop. Operations issued from inside plugin
 *       callbacks follow the C++ reentrancy rules: unload returns after queueing ('deferred'
 *       semantics surfaced as normal completion of this call), boot/load/shutdown reject with
 *       RackError('busy'), and the queued unloads run at the next poll()/safe point.
 */
export interface Rack {
  /** @brief Scan configured directories, load all, plan, init all, then start all. */
  boot(): Promise<void>;
  /** @brief Stage an in-process plugin factory for the next start (no module file needed). */
  add(create: () => RackPlugin): void;
  /**
   * @brief Initialize and start every staged-but-not-started instance (deterministic plan).
   * @throws RackError with the first failure; the batch rolls back in reverse plan order.
   */
  start(): Promise<void>;
  /** @brief Hot-load one plugin module file; hot-load `before` constraints may be rejected. */
  load(path: string): Promise<void>;
  /**
   * @brief Full unload protocol for one instance: withdraw caps, notify, revoke leases, verify
   *        quiescence, stop, destroy, drop module references. Unreturned leases leave the
   *        instance isolated ('failed') instead of forcing teardown.
   */
  unload(plugId: string): Promise<void>;
  /** @brief Unload then freshly load the same source; generation bumps, old credentials stale. */
  reload(plugId: string): Promise<void>;
  /** @brief Drain queued events and deferred unloads at a safe scheduling point. */
  poll(): Promise<void>;
  /** @brief Global teardown in reverse initialization order; retained on failure, never forced. */
  shutdown(): Promise<void>;
  /** @brief Deterministic snapshot of known instances. */
  plugins(): readonly InstanceInfo[];
  /** @brief Current version of an active provider without leasing (probe). */
  describe(plugId: string): InstanceInfo | undefined;
}
