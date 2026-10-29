/**
 * @file Per-instance plugin services view (the TS ictx equivalent).
 *
 * @brief Builds the owner-bound, state-gated PluginServices facade the host injects into
 *        init(): a thin delegation over the frozen host ports.
 *
 * @note Faithful adaptation of the C++ src/context.cc rules for the TS rack. The view owns no
 *       state of its own: every operation delegates to the injected ports while binding the
 *       owning plugId, so the host attributes subscriptions, publications, leases, calls and
 *       diagnostics to the right instance. Two state gates mirror the C++ semantics:
 *       - capabilities may only be published while the instance is inside start()
 *         (deps.capsPublishAllowed);
 *       - only Active consumers may issue business calls; Initialized consumers may hold
 *         leases but not call (deps.consumerActive).
 *       Recovery actions such as release() and unsubscribe() stay available in every state.
 */

import type {
  CapEvent,
  CapSink,
  CapsDesc,
  CapabilityRegistry,
  CapabilityService,
  CallService,
  DiagLevel,
  DiagService,
  EventService,
  EventSink,
  EventBus,
  InvokeGateway,
  LeaseCredential,
  LeaseHandle,
  LeaseManager,
  PluginServices,
  Revoker,
  Subscription,
  VersionRange,
} from './types.ts';
import type { JsonValue } from './json.ts';
import { RackError } from './status.ts';

/**
 * @brief Host ports and state gates backing one plugin services view.
 *
 * @note Supplied by the host (core/host.ts). The view adds no port of its own and keeps no
 *       state beyond the captured owner identity, so all authority and bookkeeping stay
 *       host-side.
 */
export interface PluginServicesDeps {
  /** @brief Rack-wide queued event bus; subscriptions and publications are attributed to the owner. */
  readonly bus: EventBus;
  /** @brief Rack-wide capability registry; used only for watch() and snapshot(). */
  readonly registry: CapabilityRegistry;
  /** @brief Rack-wide lease manager backing acquire() and release(). */
  readonly leases: LeaseManager;
  /** @brief Rack-wide invoke gateway backing call() and callById(). */
  readonly calls: InvokeGateway;
  /** @brief Whether the owner is currently inside start() and may publish capabilities. */
  readonly capsPublishAllowed: () => boolean;
  /** @brief Whether the owner is Active and therefore allowed to issue business calls. */
  readonly consumerActive: () => boolean;
  /** @brief Host diagnostics sink receiving owner-prefixed messages. */
  readonly onDiag: (level: DiagLevel, message: string, detail?: JsonValue) => void;
  /**
   * @brief Side channel collecting successful capability announcements; the only registration
   *        path. The host performs the actual registry registration from its side.
   */
  readonly onCapsPublished?: (caps: CapsDesc) => void;
}

/**
 * @brief Construct the per-instance services view injected into a plugin's init().
 *
 * @param deps Host ports and state gates; captured by every returned service object.
 * @param plugId Owning instance identity; becomes services.plugId and is bound into every
 *        delegated operation (subscription owner, event publisher, lease consumer, call
 *        consumer, diagnostics prefix).
 *
 * @return A PluginServices facade over the deps ports.
 *
 * @note Delegation only: no caches, counters or registries are created here. Capability
 *       publication is recorded exclusively through deps.onCapsPublished (the view never
 *       touches registry.register); watch() and snapshot() are pure registry reads shared by
 *       every instance.
 * @throws RackError 'invalid_state' from caps.publish() outside start() and from call() /
 *         callById() while the consumer is not Active; port errors propagate unchanged.
 */
export function createPluginServices(deps: PluginServicesDeps, plugId: string): PluginServices {
  const ownerId = plugId;

  /**
   * @brief Reject a business call issued by a non-Active consumer.
   * @note Initialized consumers may hold leases but must not call; enforced before the
   *       gateway is reached.
   */
  const assertConsumerActive = (): void => {
    if (!deps.consumerActive()) {
      throw new RackError(
        'invalid_state',
        `business calls require an Active consumer; '${ownerId}' is not Active`,
        { plugId: ownerId },
      );
    }
  };

  const events: EventService = {
    subscribe(name: string, sink: EventSink): Subscription {
      return deps.bus.subscribe(name, sink, ownerId);
    },
    publish(name: string, payload: JsonValue): void {
      deps.bus.publish(name, payload, ownerId);
    },
  };

  const caps: CapabilityService = {
    publish(caps: CapsDesc): void {
      if (!deps.capsPublishAllowed()) {
        throw new RackError(
          'invalid_state',
          `capabilities of '${ownerId}' may only be published from within start()`,
          { plugId: ownerId },
        );
      }
      deps.onCapsPublished?.(caps);
    },
    watch(sink: CapSink): Subscription {
      // Adapt away the registry's snapshot payload; plugins get the plain Subscription contract.
      const inner = deps.registry.watch(sink, ownerId);
      return { unsubscribe: () => inner.unsubscribe() };
    },
    snapshot(): readonly CapEvent[] {
      return deps.registry.snapshot();
    },
    acquire(plugId: string, range: VersionRange, revoker: Revoker): LeaseHandle {
      return deps.leases.acquire(ownerId, plugId, range, revoker);
    },
    release(credential: LeaseCredential): 'ok' | 'busy' | 'stale' {
      // Recovery action: available in every state, mirroring the C++ context rules.
      return deps.leases.release(credential, ownerId);
    },
  };

  const calls: CallService = {
    call(credential: LeaseCredential, methodName: string, args: JsonValue): Promise<JsonValue> {
      assertConsumerActive();
      return deps.calls.call(ownerId, credential, { byName: methodName }, args);
    },
    callById(credential: LeaseCredential, methodId: number, args: JsonValue): Promise<JsonValue> {
      assertConsumerActive();
      return deps.calls.call(ownerId, credential, { byId: methodId }, args);
    },
  };

  const diag: DiagService = {
    report(level: DiagLevel, message: string, detail?: JsonValue): void {
      deps.onDiag(level, `[${ownerId}] ${message}`, detail);
    },
  };

  return { plugId: ownerId, events, caps, calls, diag };
}
