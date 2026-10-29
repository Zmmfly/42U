/**
 * @file Capability registry: publication, withdrawal and the atomic snapshot+watch pair.
 *
 * @brief Host-side capability table of the TS rack, implementing the frozen CapabilityRegistry
 *        port of src/core/types.ts.
 *
 * @note Faithful adaptation of the C++ capability bookkeeping (docs/42U插件框架设计.md chapter 7
 *       and the announce/watch/commit/withdraw paths of src/context.cc):
 *       - register() mirrors a successful start(): the method set is copied host-side and frozen
 *         for the instance lifetime, then the availability fact is broadcast to every watcher.
 *       - withdraw() removes exactly one (plugId, generation) pair; unknown or stale generations
 *         are ignored idempotently, and watchers receive an empty method set with the withdrawn
 *         entry's original version.
 *       - watch() is the atomic "snapshot + subsequent changes" operation of the C++ control
 *         thread: the sink first synchronously receives the current facts ordered by plugId
 *         UTF-8 bytes, then follows every later change in order.
 *       TS notification adaptation: notices are delivered synchronously at the register() /
 *       withdraw() call site. The C++ "safe scheduling point" is realized by the host choosing
 *       when to call register() (only after the plugin's start() returned), which is
 *       semantically equivalent to the C++ controlled notice path where no sink is ever invoked
 *       from inside a plugin's start() stack.
 */

import { Buffer } from 'node:buffer';
import { RackError } from './status.ts';
import { isPluginVersion } from './version.ts';
import { isJsonValue } from './json.ts';
import type { JsonValue } from './json.ts';
import type { CapEntry, CapEvent, CapabilityRegistry, CapSink, CapsDesc, MethodDesc, Subscription } from './types.ts';

/**
 * @brief Construction options of createCapabilityRegistry.
 */
export interface CapabilityRegistryOptions {
  /**
   * @brief Observer for sink failures escaped from capability callbacks.
   *
   * @param watcherId Identifier passed to watch() for the failing sink.
   * @param error Value thrown synchronously or rejected asynchronously by the sink.
   *
   * @note Invoked on a best-effort basis; exceptions it throws are swallowed so error
   *       reporting can never cascade back into registry operations. When omitted, sink
   *       failures are silently swallowed.
   */
  onSinkError?: (watcherId: string, error: unknown) => void;
}

/** @brief Shared deeply-frozen empty method set carried by every withdrawal fact. */
const EMPTY_CAPS: CapsDesc = Object.freeze({ methods: Object.freeze<MethodDesc[]>([]) });

/** @brief One live watch registration; stays in place until unsubscribed. */
interface WatchRecord {
  readonly watcherId: string;
  readonly sink: CapSink;
  active: boolean;
}

/**
 * @brief Structural validation of one capability entry before it enters the table.
 *
 * @param entry Candidate of unknown runtime shape (host internals and tests).
 * @throws RackError 'invalid_argument' for a malformed entry: non-object, empty plugId,
 *         negative or fractional generation, malformed version triple, missing method array,
 *         malformed method records, negative or fractional method ids, empty method names,
 *         non-string descriptions, non-JSON schemas, or method ids/names repeated within the
 *         entry.
 */
function assertValidCapEntry(entry: unknown): asserts entry is CapEntry {
  if (typeof entry !== 'object' || entry === null) {
    throw new RackError('invalid_argument', 'capability entry must be an object', { entry });
  }
  const record = entry as Record<string, unknown>;
  const plugId = record['plugId'];
  if (typeof plugId !== 'string' || plugId.length === 0) {
    throw new RackError('invalid_argument', 'capability entry requires a non-empty plugId', { plugId });
  }
  const generation = record['generation'];
  if (typeof generation !== 'number' || !Number.isSafeInteger(generation) || generation < 0) {
    throw new RackError('invalid_argument', `capability entry for '${plugId}' requires a non-negative integer generation`, { plugId, generation });
  }
  if (!isPluginVersion(record['version'])) {
    throw new RackError('invalid_argument', `capability entry for '${plugId}' requires a valid version triple`, { plugId });
  }
  const caps = record['caps'];
  if (typeof caps !== 'object' || caps === null || !Array.isArray((caps as Record<string, unknown>)['methods'])) {
    throw new RackError('invalid_argument', `capability entry for '${plugId}' requires a caps object with a methods array`, { plugId });
  }
  const methods = (caps as Record<string, unknown>)['methods'] as unknown[];
  const seenIds = new Set<number>();
  const seenNames = new Set<string>();
  for (const raw of methods) {
    if (typeof raw !== 'object' || raw === null) {
      throw new RackError('invalid_argument', `methods of '${plugId}' must be objects`, { plugId });
    }
    const method = raw as Record<string, unknown>;
    const id = method['id'];
    if (typeof id !== 'number' || !Number.isSafeInteger(id) || id < 0) {
      throw new RackError('invalid_argument', `methods of '${plugId}' require non-negative integer ids`, { plugId, id });
    }
    const name = method['name'];
    if (typeof name !== 'string' || name.length === 0) {
      throw new RackError('invalid_argument', `methods of '${plugId}' require non-empty names`, { plugId, name });
    }
    if (method['description'] !== undefined && typeof method['description'] !== 'string') {
      throw new RackError('invalid_argument', `method '${name}' of '${plugId}' has a non-string description`, { plugId, name });
    }
    if (method['inputSchema'] !== undefined && !isJsonValue(method['inputSchema'])) {
      throw new RackError('invalid_argument', `method '${name}' of '${plugId}' has a non-JSON input schema`, { plugId, name });
    }
    if (method['outputSchema'] !== undefined && !isJsonValue(method['outputSchema'])) {
      throw new RackError('invalid_argument', `method '${name}' of '${plugId}' has a non-JSON output schema`, { plugId, name });
    }
    if (seenIds.has(id)) {
      throw new RackError('invalid_argument', `method id ${id} repeats within '${plugId}'`, { plugId, id });
    }
    if (seenNames.has(name)) {
      throw new RackError('invalid_argument', `method name '${name}' repeats within '${plugId}'`, { plugId, name });
    }
    seenIds.add(id);
    seenNames.add(name);
  }
}

/**
 * @brief Recursively copy one JSON value into a deeply frozen representation.
 *
 * @param value Validated JSON value (schemas were checked with isJsonValue beforehand).
 * @return An independent copy in which every array and object is frozen.
 */
function frozenJsonCopy(value: JsonValue): JsonValue {
  if (value === null || typeof value !== 'object') return value;
  if (Array.isArray(value)) {
    return Object.freeze(value.map(frozenJsonCopy));
  }
  const copy: { [key: string]: JsonValue } = {};
  for (const [key, item] of Object.entries(value)) {
    if (item !== undefined) copy[key] = frozenJsonCopy(item);
  }
  return Object.freeze(copy);
}

/**
 * @brief Copy and freeze one method description so callers cannot mutate the table later.
 *
 * @param method Validated method record.
 * @return A frozen private copy; optional fields are copied only when present.
 */
function frozenMethod(method: MethodDesc): MethodDesc {
  const copy: { id: number; name: string; description?: string; inputSchema?: JsonValue; outputSchema?: JsonValue } = {
    id: method.id,
    name: method.name,
  };
  if (method.description !== undefined) copy.description = method.description;
  if (method.inputSchema !== undefined) copy.inputSchema = frozenJsonCopy(method.inputSchema);
  if (method.outputSchema !== undefined) copy.outputSchema = frozenJsonCopy(method.outputSchema);
  return Object.freeze(copy);
}

/**
 * @brief Build the frozen table record stored and exposed by lookup().
 *
 * @param entry Validated entry supplied by the host.
 * @return A deeply frozen private copy; the caller keeps no alias into the table.
 */
function frozenEntry(entry: CapEntry): CapEntry {
  const version = Object.freeze({ major: entry.version.major, minor: entry.version.minor, patch: entry.version.patch });
  const methods = Object.freeze(entry.caps.methods.map(frozenMethod));
  const caps = Object.freeze({ methods });
  return Object.freeze({ plugId: entry.plugId, generation: entry.generation, version, caps });
}

/**
 * @brief Build the availability fact for one stored entry.
 *
 * @param entry Frozen table record.
 * @return A frozen CapEvent sharing the entry's frozen version and capability data.
 */
function availableEvent(entry: CapEntry): CapEvent {
  return Object.freeze({
    plugId: entry.plugId,
    available: true,
    version: entry.version,
    capabilities: entry.caps,
  });
}

/**
 * @brief Create a capability registry instance.
 *
 * @param options Optional construction options; see CapabilityRegistryOptions.
 * @return A fresh independent registry with an empty table and no watchers.
 *
 * @note Notifications are delivered synchronously at the register()/withdraw() call site; the
 *       C++ "safe scheduling point" semantics come from the host's choice of call timing
 *       (register only after the plugin's start() returned). See the file header.
 */
export function createCapabilityRegistry(options?: CapabilityRegistryOptions): CapabilityRegistry {
  const onSinkError = options?.onSinkError;
  const table = new Map<string, CapEntry>();
  const watchers: WatchRecord[] = [];

  /**
   * @brief Forward one sink failure to onSinkError, swallowing everything.
   * @param watcherId Identity of the failing watcher.
   * @param error Escaped value.
   */
  function reportSinkError(watcherId: string, error: unknown): void {
    if (!onSinkError) return;
    try {
      onSinkError(watcherId, error);
    } catch {
      // Error reporting must never cascade back into registry operations.
    }
  }

  /**
   * @brief Deliver one fact to one sink, isolating sync throws and async rejections.
   * @param record Target watch registration.
   * @param event Immutable fact to deliver.
   */
  function deliver(record: WatchRecord, event: CapEvent): void {
    try {
      const outcome = record.sink.onCapability(event);
      if (outcome instanceof Promise) {
        void outcome.catch((error: unknown) => {
          reportSinkError(record.watcherId, error);
        });
      }
    } catch (error) {
      reportSinkError(record.watcherId, error);
    }
  }

  /**
   * @brief Deliver one fact to every currently active watcher.
   * @param event Immutable fact to deliver.
   * @note Iterates a copy of the watcher list: sinks may unsubscribe themselves or others
   *       from inside the callback without disturbing this pass.
   */
  function notifyActiveWatchers(event: CapEvent): void {
    for (const record of [...watchers]) {
      if (record.active) deliver(record, event);
    }
  }

  /**
   * @brief Current availability facts ordered by plugId UTF-8 bytes.
   * @return A fresh array of fresh frozen event objects sharing the frozen table data.
   */
  function currentEvents(): CapEvent[] {
    const rows: { key: Buffer; event: CapEvent }[] = [];
    for (const entry of table.values()) {
      rows.push({ key: Buffer.from(entry.plugId, 'utf8'), event: availableEvent(entry) });
    }
    rows.sort((left, right) => Buffer.compare(left.key, right.key));
    return rows.map((row) => row.event);
  }

  return Object.freeze({
    /**
     * @brief Publish one instance's frozen method set and announce availability.
     *
     * @param entry Host-side capability record; deeply copied and frozen on acceptance.
     * @throws RackError 'invalid_argument' for a malformed entry (see assertValidCapEntry),
     *         'duplicate' when plugId is currently registered with any generation.
     *
     * @note The table is updated first, then every active watcher is notified synchronously;
     *       a sink re-entering the registry from its callback therefore observes the new state.
     */
    register(entry: CapEntry): void {
      assertValidCapEntry(entry);
      if (table.has(entry.plugId)) {
        throw new RackError('duplicate', `plugin '${entry.plugId}' already has published capabilities`, { plugId: entry.plugId });
      }
      const stored = frozenEntry(entry);
      table.set(stored.plugId, stored);
      notifyActiveWatchers(availableEvent(stored));
    },

    /**
     * @brief Remove one provider from the table and announce withdrawal.
     *
     * @param plugId Provider identity.
     * @param generation Exact instance generation that published the entry.
     *
     * @note Only an exact (plugId, generation) match withdraws; unknown ids and stale
     *       generations are ignored silently, making the call idempotent. Watchers receive
     *       an {available:false} fact with the withdrawn entry's original version and an
     *       empty method set.
     */
    withdraw(plugId: string, generation: number): void {
      const current = table.get(plugId);
      if (current === undefined || current.generation !== generation) return;
      table.delete(plugId);
      notifyActiveWatchers(
        Object.freeze({
          plugId: current.plugId,
          available: false,
          version: current.version,
          capabilities: EMPTY_CAPS,
        }),
      );
    },

    /**
     * @brief Atomically deliver the current snapshot and subscribe to later changes.
     *
     * @param sink Receiver for capability facts; must provide onCapability().
     * @param watcherId Diagnostic identity used in onSinkError reports.
     * @return Subscription handle carrying the delivered snapshot; unsubscribe() is idempotent
     *         and stops all further notifications.
     * @throws RackError 'invalid_argument' when the sink has no onCapability() function.
     *
     * @note The snapshot (ordered by plugId UTF-8 bytes) is delivered synchronously before
     *       the sink joins the subscriber list, so no fact can be observed twice. Snapshot
     *       delivery failures are reported like any other sink failure and never prevent
     *       the subscription itself.
     */
    watch(sink: CapSink, watcherId: string): Subscription & { readonly snapshot: readonly CapEvent[] } {
      if (typeof sink !== 'object' || sink === null || typeof sink.onCapability !== 'function') {
        throw new RackError('invalid_argument', 'watch requires a capability sink with onCapability()', { watcherId });
      }
      const record: WatchRecord = { watcherId, sink, active: true };
      const snapshotEvents = currentEvents();
      for (const event of snapshotEvents) {
        deliver(record, event);
      }
      watchers.push(record);
      return Object.freeze({
        snapshot: snapshotEvents,
        unsubscribe: (): void => {
          record.active = false;
        },
      });
    },

    /**
     * @brief Current availability facts, sorted by plugId UTF-8 byte order.
     * @return A fresh array of fresh event objects sharing the frozen capability data.
     */
    snapshot(): readonly CapEvent[] {
      return currentEvents();
    },

    /**
     * @brief Current table record for one provider.
     *
     * @param plugId Provider identity.
     * @return The frozen stored entry, or undefined when absent or withdrawn.
     */
    lookup(plugId: string): CapEntry | undefined {
      return table.get(plugId);
    },
  });
}
