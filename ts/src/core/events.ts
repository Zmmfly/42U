/**
 * @file Bounded queued event bus.
 *
 * @brief Host-owned FIFO event queue with owner-attributed subscriptions and serial dispatch.
 *
 * @note Faithful adaptation of the C++ events.cc semantics onto the single Node event loop:
 *       publish() only enqueues and never runs a subscriber on the caller's stack; delivery
 *       happens solely inside drain() behind a re-entrancy guard; `u42.*` names stay reserved
 *       for the host; every subscription is attributed to its owner so unload can revoke them
 *       in bulk. One deliberate divergence from the C++ byte copy: publish() keeps the payload
 *       reference instead of copying it, so callers must treat published values as frozen.
 */

import type { EventBus, EventSink, RackEvent, Subscription } from './types.ts';
import type { JsonValue } from './json.ts';
import { estimateJsonBytes, isJsonValue } from './json.ts';
import { RackError } from './status.ts';

/** @brief Default maximum number of queued events. */
const DEFAULT_CAPACITY = 1024;

/** @brief Default maximum estimated payload bytes per event (1 MiB). */
const DEFAULT_PAYLOAD_LIMIT = 1024 * 1024;

/** @brief Event name prefix reserved for host lifecycle traffic. */
const RESERVED_PREFIX = 'u42.';

/**
 * @brief Creation options of createEventBus().
 * @note Every field is optional; omitted fields fall back to the documented defaults.
 */
export interface EventBusOptions {
  /** @brief Maximum queued events; publishing past this bound throws 'limit_exceeded'. */
  readonly capacity?: number;
  /** @brief Maximum estimated bytes per payload (estimateJsonBytes); default 1 MiB. */
  readonly payloadLimit?: number;
  /** @brief Observer invoked with (eventName, error) whenever a sink throws or rejects. */
  readonly onSinkError?: (name: string, error: unknown) => void;
}

/** @brief Internal subscription record attributed to its subscribing owner. */
interface SubscriptionRecord {
  readonly name: string;
  readonly sink: EventSink;
  readonly ownerId: string;
}

/** @brief Internal queued event; one global FIFO across all event names. */
interface QueuedEvent {
  readonly name: string;
  readonly payload: JsonValue;
}

/**
 * @brief Assert a positive safe integer option.
 *
 * @param value Candidate bound value.
 * @param label Option name included in the thrown diagnostic.
 * @throws RackError 'invalid_argument' for zero, negative, fractional or non-finite values.
 */
function assertPositiveInteger(value: number, label: string): void {
  if (!Number.isSafeInteger(value) || value <= 0) {
    throw new RackError('invalid_argument', `events: ${label} must be a positive integer`, value);
  }
}

/**
 * @brief EventBus implementation: bounded FIFO queue plus owner-attributed subscriptions.
 * @note Not exported; createEventBus() is the public entry point. Dispatch never runs on a
 *       publish() stack and never re-enters a running drain() pass.
 */
class BoundedEventBus implements EventBus {
  private readonly capacity: number;
  private readonly payloadLimit: number;
  private readonly onSinkError?: (name: string, error: unknown) => void;
  private readonly subscriptions = new Map<number, SubscriptionRecord>();
  private readonly queue: QueuedEvent[] = [];
  private nextToken = 1;
  private draining = false;

  /**
   * @brief Validate options and fix the bounds used for the bus lifetime.
   *
   * @param options Optional capacity, payloadLimit and sink-failure observer.
   * @throws RackError 'invalid_argument' for a non-positive-integer bound or an onSinkError
   *         that is present but not a function.
   */
  constructor(options?: EventBusOptions) {
    const capacity = options?.capacity ?? DEFAULT_CAPACITY;
    const payloadLimit = options?.payloadLimit ?? DEFAULT_PAYLOAD_LIMIT;
    assertPositiveInteger(capacity, 'capacity');
    assertPositiveInteger(payloadLimit, 'payloadLimit');
    const onSinkError = options?.onSinkError;
    if (onSinkError !== undefined && typeof onSinkError !== 'function') {
      throw new RackError('invalid_argument', 'events: onSinkError must be a function');
    }
    this.capacity = capacity;
    this.payloadLimit = payloadLimit;
    this.onSinkError = onSinkError;
  }

  /**
   * @brief Register one sink for one exact event name.
   *
   * @param name Non-empty event name compared literally; several subscribers per name are
   *        allowed and wildcards are not provided.
   * @param sink Receiver whose onEvent() is called during drain() passes.
   * @param ownerId Non-empty id of the owning instance; the host revokes by this key.
   * @return Subscription handle whose unsubscribe() is idempotent.
   *
   * @throws RackError 'invalid_argument' for an empty name, a sink without an onEvent()
   *         function or an empty ownerId.
   * @note The subscription is attributed to ownerId; unsubscribeAll(ownerId) removes it even
   *       without the handle. Matching is evaluated at dispatch time, so subscriptions only
   *       receive events drained after they were registered.
   */
  subscribe(name: string, sink: EventSink, ownerId: string): Subscription {
    if (typeof name !== 'string' || name.length === 0) {
      throw new RackError('invalid_argument', 'events.subscribe: name is empty', { name });
    }
    if (typeof sink !== 'object' || sink === null || typeof sink.onEvent !== 'function') {
      throw new RackError('invalid_argument', 'events.subscribe: sink must provide onEvent()');
    }
    if (typeof ownerId !== 'string' || ownerId.length === 0) {
      throw new RackError('invalid_argument', 'events.subscribe: ownerId is empty', { ownerId });
    }
    const token = this.nextToken;
    this.nextToken += 1;
    this.subscriptions.set(token, { name, sink, ownerId });
    let closed = false;
    return {
      unsubscribe: () => {
        if (closed) return;
        closed = true;
        this.subscriptions.delete(token);
      },
    };
  }

  /**
   * @brief Enqueue one event without dispatching anything.
   *
   * @param name Non-empty event name outside the reserved `u42.*` prefix.
   * @param payload JSON-compatible value; the bus stores the reference without copying.
   * @param publisherId Publishing instance id; attribution only, the bus applies no owner
   *        filter on the publish path.
   * @return void; the event is queued for the next drain() pass.
   *
   * @throws RackError 'invalid_argument' for an empty or reserved name or a non-JSON payload.
   * @throws RackError 'limit_exceeded' when estimateJsonBytes(payload) exceeds payloadLimit or
   *         the queue already holds capacity events.
   * @note Unlike the C++ byte copy, the queue keeps the caller's payload reference: after a
   *       successful publish the caller must not mutate the value, and subscribers receive it
   *       read-only for the duration of their callback only.
   */
  publish(name: string, payload: JsonValue, publisherId: string): void {
    void publisherId;
    if (typeof name !== 'string' || name.length === 0) {
      throw new RackError('invalid_argument', 'events.publish: name is empty', { name });
    }
    if (name.startsWith(RESERVED_PREFIX)) {
      throw new RackError('invalid_argument', `events.publish: '${RESERVED_PREFIX}*' names are reserved for the host`, { name });
    }
    if (!isJsonValue(payload)) {
      throw new RackError('invalid_argument', 'events.publish: payload is not a JSON value', { name });
    }
    if (estimateJsonBytes(payload) > this.payloadLimit) {
      throw new RackError('limit_exceeded', 'events.publish: the payload exceeds the configured limit', { name });
    }
    if (this.queue.length >= this.capacity) {
      throw new RackError('limit_exceeded', 'events.publish: the event queue is full', { name });
    }
    this.queue.push({ name, payload });
  }

  /**
   * @brief Drop every subscription owned by ownerId (host unload-time action).
   *
   * @param ownerId Owner whose subscriptions are revoked.
   * @return void; unknown owners and repeated calls are silent no-ops.
   */
  unsubscribeAll(ownerId: string): void {
    for (const [token, record] of this.subscriptions) {
      if (record.ownerId === ownerId) {
        this.subscriptions.delete(token);
      }
    }
  }

  /**
   * @brief Deliver queued events serially in global FIFO order across all names.
   *
   * @param budget Maximum number of events to dequeue in this pass; non-finite or non-positive
   *        values deliver nothing.
   * @return The number of events dequeued and dispatched; events without matching subscribers
   *         are consumed and counted.
   *
   * @note Async sinks are awaited one at a time; each sink failure (throw or rejection) is
   *       routed to onSinkError (or swallowed when absent) and never stops later deliveries.
   * @note Calling drain() from inside a sink callback, or while another pass is in flight,
   *       resolves to 0 instead of recursing. Events published during delivery join the queue
   *       tail and may be picked up by this same pass while budget remains. Events left when
   *       the budget runs out stay queued for the next pass.
   */
  async drain(budget: number): Promise<number> {
    if (this.draining) return 0;
    const limit = Number.isFinite(budget) && budget > 0 ? Math.floor(budget) : 0;
    if (limit === 0) return 0;
    this.draining = true;
    let delivered = 0;
    try {
      while (delivered < limit) {
        const entry = this.queue.shift();
        if (entry === undefined) break;
        delivered += 1;
        await this.deliver(entry);
      }
    } finally {
      this.draining = false;
    }
    return delivered;
  }

  /** @brief Currently queued event count across all names. */
  get pending(): number {
    return this.queue.length;
  }

  /**
   * @brief Deliver one dequeued event to every still-registered subscriber of its name.
   *
   * @param entry Dequeued event moved out of the queue by drain().
   * @note Matching subscription tokens are snapshotted before the first callback and
   *       re-checked before each call, so callbacks may freely subscribe, unsubscribe or
   *       unsubscribeAll without invalidating this pass; newly added subscribers only see
   *       later events.
   */
  private async deliver(entry: QueuedEvent): Promise<void> {
    const tokens: number[] = [];
    for (const [token, record] of this.subscriptions) {
      if (record.name === entry.name) tokens.push(token);
    }
    const event: RackEvent = { name: entry.name, payload: entry.payload };
    for (const token of tokens) {
      const record = this.subscriptions.get(token);
      if (record === undefined) continue;
      try {
        await record.sink.onEvent(event);
      } catch (error) {
        this.reportSinkError(entry.name, error);
      }
    }
  }

  /**
   * @brief Forward one sink failure to the configured observer, best effort.
   *
   * @param name Name of the event whose delivery failed.
   * @param error Value thrown or rejection reason produced by the sink.
   */
  private reportSinkError(name: string, error: unknown): void {
    const handler = this.onSinkError;
    if (handler === undefined) return;
    try {
      handler(name, error);
    } catch {
      // Best effort only: a failing error reporter must not break dispatch.
    }
  }
}

/**
 * @brief Create a bounded queued event bus.
 *
 * @param options Optional bounds and failure observer; defaults are capacity 1024,
 *        payloadLimit 1 MiB and no onSinkError (sink failures are swallowed).
 * @return A fresh EventBus; independent of every other bus instance.
 *
 * @note The returned bus never dispatches synchronously from publish(); the host drives
 *       delivery through drain() at safe scheduling points.
 */
export function createEventBus(options?: EventBusOptions): EventBus {
  return new BoundedEventBus(options);
}
