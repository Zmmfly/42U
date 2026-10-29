/**
 * @file index.ts
 * @brief std.log: the logging device that ships with the 42U rack (TS edition).
 *
 * @details Not host-builtin: std.log is a regular rack device at plugId 'std.log'. Consumers
 *          reach it only through the host gateway under a version-qualified lease. Accepted
 *          entries land in an in-memory ring buffer (default 4096 slots, overridable through
 *          U42_LOG_RING) and fan out to two best-effort sinks: a stderr JSON-lines console
 *          sink (always on) and an optional append-only file sink (enabled through
 *          U42_LOG_FILE, node:fs/promises). Sink failures never reject a log call; the file
 *          sink silently degrades to memory-only after its first failure.
 *
 *          External effects (clock, environment view, stderr writer, file appender) are
 *          injectable through createWithDeps() for hermetic tests; create() binds the real
 *          Node implementations of the same shapes.
 */

import { appendFile as fsAppendFile } from 'node:fs/promises';

import { definePlugin, isJsonValue, RackError } from '../../sdk/index.ts';
import type {
  DiagLevel,
  InvokeRequest,
  JsonObject,
  JsonValue,
  MethodDesc,
  PluginServices,
  RackPlugin,
} from '../../sdk/index.ts';

// ---------------------------------------------------------------------------
// Published data contract (duplicated by consumers as constants only).
// ---------------------------------------------------------------------------

/** @brief Canonical plugin identity announced to the host. */
const LOG_PLUG_ID = 'std.log';

/** @brief Business version published by this provider. */
const LOG_VERSION = { major: 1, minor: 0, patch: 0 } as const;

/** @brief Numeric method id of the log method. */
const LOG_METHOD_ID = 1;

/** @brief Numeric method id of the query method. */
const QUERY_METHOD_ID = 2;

/** @brief Numeric method id of the stats method. */
const STATS_METHOD_ID = 3;

/** @brief Method names matching the numeric ids above. */
const LOG_METHOD_NAME = 'log';
const QUERY_METHOD_NAME = 'query';
const STATS_METHOD_NAME = 'stats';

/**
 * @brief Methods announced through the capability service during start().
 * @note The set is frozen for the instance lifetime; ids and names are unique.
 */
const LOG_METHODS: readonly MethodDesc[] = [
  {
    id: LOG_METHOD_ID,
    name: LOG_METHOD_NAME,
    description: 'Accept one log entry into the ring buffer and fan it out to the sinks.',
    inputSchema: { type: 'object' },
    outputSchema: { type: 'object' },
  },
  {
    id: QUERY_METHOD_ID,
    name: QUERY_METHOD_NAME,
    description:
      'Filter buffered entries by level and minimum timestamp; returns the newest window in ts order.',
    inputSchema: { type: 'object' },
    outputSchema: { type: 'object' },
  },
  {
    id: STATS_METHOD_ID,
    name: STATS_METHOD_NAME,
    description: 'Report per-level counts, ring capacity and used slots.',
    inputSchema: { type: 'null' },
    outputSchema: { type: 'object' },
  },
];

// ---------------------------------------------------------------------------
// Environment conventions (scoped to this plugin).
// ---------------------------------------------------------------------------

/** @brief Environment variable overriding the ring capacity; default 4096. */
const RING_CAPACITY_ENV = 'U42_LOG_RING';

/** @brief Environment variable enabling the append-only file sink. */
const FILE_PATH_ENV = 'U42_LOG_FILE';

/** @brief Default ring capacity in entries. */
const DEFAULT_RING_CAPACITY = 4096;

/** @brief Default and maximum-semantic query window size. */
const DEFAULT_QUERY_LIMIT = 100;

/** @brief Arg document keys shared by the log and query handlers. */
const LEVEL_KEY = 'level';
const MESSAGE_KEY = 'message';
const FIELDS_KEY = 'fields';
const PLUG_ID_KEY = 'plugId';
const SINCE_KEY = 'since';
const LIMIT_KEY = 'limit';

// ---------------------------------------------------------------------------
// Public document shapes (type aliases: they stay assignable to JsonValue).
// ---------------------------------------------------------------------------

/**
 * @brief Args document of the 'log' method.
 * @note fields and plugId are optional; absent values are stored as null.
 */
export type LogArgs = {
  readonly level: DiagLevel;
  readonly message: string;
  readonly fields?: JsonObject;
  readonly plugId?: string;
};

/** @brief Args document of the 'query' method; every member is optional. */
export type QueryArgs = {
  readonly level?: DiagLevel;
  readonly since?: number;
  readonly limit?: number;
};

/**
 * @brief One buffered entry as returned by 'query'; the field set is frozen.
 * @note Exactly ts/level/message/fields/plugId; absent optionals are null.
 */
export type LogEntryDoc = {
  readonly ts: number;
  readonly level: DiagLevel;
  readonly message: string;
  readonly fields: JsonObject | null;
  readonly plugId: string | null;
};

/** @brief Result document of the 'query' method. */
export type QueryResultDoc = {
  readonly entries: readonly LogEntryDoc[];
  readonly total: number;
};

/** @brief Result document of the 'stats' method. */
export type StatsResultDoc = {
  readonly byLevel: { readonly info: number; readonly warn: number; readonly error: number };
  readonly capacity: number;
  readonly used: number;
};

/**
 * @brief Injectable external effects of one std.log instance.
 * @note createWithDeps() replaces the whole effect; nothing is merged with the
 *       real environment, so tests stay hermetic.
 */
export interface LoggingDeps {
  /** @brief Timestamp source for entry ts fields (epoch milliseconds). */
  readonly now?: () => number;
  /** @brief Environment view consulted once at instance construction. */
  readonly env?: Readonly<Record<string, string | undefined>>;
  /** @brief Console sink writer; receives one JSON line per accepted entry. */
  readonly stderr?: (chunk: string) => void;
  /**
   * @brief File sink appender invoked with the configured path and one JSON line.
   * @note Rejections (or synchronous throws) degrade the file sink silently.
   */
  readonly appendFile?: (path: string, data: string) => Promise<void>;
}

// ---------------------------------------------------------------------------
// Argument validation helpers.
// ---------------------------------------------------------------------------

/**
 * @brief Check that a value is one of the three log levels.
 *
 * @param value Candidate of runtime-unknown origin.
 * @return true for 'info', 'warn' or 'error'.
 */
function isLevel(value: unknown): value is DiagLevel {
  return value === 'info' || value === 'warn' || value === 'error';
}

/**
 * @brief Narrow a JSON value to a plain object (shallow check, hot-path cheap).
 *
 * @param value Candidate JSON value.
 * @return true for objects that are neither arrays nor null.
 */
function isPlainObject(value: JsonValue): value is JsonObject {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

/**
 * @brief Narrow invoke args to a JSON object.
 *
 * @param args Request args document.
 * @param method Method name used in error messages.
 * @param allowNull Whether a JSON null is accepted as an empty document.
 * @return The args as a plain JSON object.
 * @throws RackError 'invalid_argument' when args is not a JSON object.
 */
function asObjectArg(args: JsonValue, method: string, allowNull: boolean): JsonObject {
  const doc: JsonValue = allowNull && args === null ? {} : args;
  if (!isPlainObject(doc)) {
    throw new RackError('invalid_argument', `std.log ${method} args must be a JSON object`);
  }
  return doc;
}

/**
 * @brief Check that a value is a plain JSON object (not an array, not null).
 *
 * @param value Candidate of runtime-unknown origin.
 * @return true for JSON objects whose whole tree is JSON-safe.
 */
function isJsonObjectValue(value: unknown): value is JsonObject {
  return (
    typeof value === 'object' &&
    value !== null &&
    !Array.isArray(value) &&
    isJsonValue(value)
  );
}

// ---------------------------------------------------------------------------
// Ring buffer.
// ---------------------------------------------------------------------------

/**
 * @brief Fixed-capacity ring buffer of frozen log entries.
 * @note push() is O(1) with no array shifting and no entry copies; entries()
 *       returns an oldest-first snapshot, which equals ts order because the
 *       clock is monotonic per instance and insertion order is preserved.
 */
class LogRing {
  /** @brief Number of retained entries; fixed at construction (public readonly). */
  readonly capacity: number;
  private readonly slots: (LogEntryDoc | undefined)[];
  private head = 0;
  private count = 0;

  /**
   * @brief Construct a ring for a fixed capacity.
   *
   * @param capacity Number of retained entries; callers must pass >= 1.
   */
  constructor(capacity: number) {
    // Explicit assignment instead of a parameter property: constructor parameter
    // properties are not erasable TypeScript and break Node's strip-only mode.
    this.capacity = capacity;
    this.slots = new Array<LogEntryDoc | undefined>(capacity);
  }

  /**
   * @brief Append one entry, evicting the oldest when full.
   *
   * @param entry Frozen normalized entry; stored by reference, never copied.
   */
  push(entry: LogEntryDoc): void {
    const writeIndex = (this.head + this.count) % this.capacity;
    this.slots[writeIndex] = entry;
    if (this.count === this.capacity) {
      // Buffer full: writeIndex === head, the oldest entry was just overwritten.
      this.head = (this.head + 1) % this.capacity;
    } else {
      this.count++;
    }
  }

  /**
   * @brief Snapshot the retained entries.
   *
   * @return Entries ordered oldest to newest (ts ascending).
   */
  entries(): LogEntryDoc[] {
    const out: LogEntryDoc[] = [];
    for (let index = 0; index < this.count; index++) {
      const entry = this.slots[(this.head + index) % this.capacity];
      if (entry !== undefined) out.push(entry);
    }
    return out;
  }

  /** @brief Drop every entry and release the stored references. */
  clear(): void {
    this.slots.fill(undefined);
    this.head = 0;
    this.count = 0;
  }

  /** @brief Number of currently retained entries. */
  get used(): number {
    return this.count;
  }
}

// ---------------------------------------------------------------------------
// Environment resolution.
// ---------------------------------------------------------------------------

/**
 * @brief Resolve the ring capacity from the environment view.
 *
 * @param envView Environment map read once at construction time.
 * @return The positive safe integer in U42_LOG_RING, or the 4096 default when
 *         unset, empty or malformed (non-integer, zero, negative).
 */
function resolveCapacity(envView: Readonly<Record<string, string | undefined>>): number {
  const raw = envView[RING_CAPACITY_ENV];
  if (raw === undefined) return DEFAULT_RING_CAPACITY;
  const value = Number(raw);
  if (!Number.isSafeInteger(value) || value <= 0) return DEFAULT_RING_CAPACITY;
  return value;
}

/**
 * @brief Resolve the file sink path from the environment view.
 *
 * @param envView Environment map read once at construction time.
 * @return The configured path, or null when the file sink is disabled.
 */
function resolveFilePath(envView: Readonly<Record<string, string | undefined>>): string | null {
  const raw = envView[FILE_PATH_ENV];
  return typeof raw === 'string' && raw.length > 0 ? raw : null;
}

// ---------------------------------------------------------------------------
// Default effects (real Node implementations).
// ---------------------------------------------------------------------------

/**
 * @brief Default console sink: one write per JSON line to stderr.
 *
 * @param chunk Complete JSON line including the trailing newline.
 */
function defaultStderr(chunk: string): void {
  process.stderr.write(chunk);
}

/**
 * @brief Default file sink appender backed by node:fs/promises.
 *
 * @param path Configured log file path.
 * @param data Complete JSON line including the trailing newline.
 * @return Completion promise; failures are handled by the caller's chain.
 */
async function defaultAppendFile(path: string, data: string): Promise<void> {
  await fsAppendFile(path, data, 'utf8');
}

// ---------------------------------------------------------------------------
// Diagnostic bridge utility.
// ---------------------------------------------------------------------------

/**
 * @brief Shape a DiagService report into args for the 'log' method.
 *
 * @param level Diagnostic severity, reused as the log level.
 * @param message Diagnostic human-readable message.
 * @param detail Optional structured diagnostic payload.
 * @return A JSON object matching the 'log' args contract: detail is used as
 *         `fields` verbatim when it is a JSON object, wrapped as
 *         `{ detail: ... }` for any other JSON value, and omitted when null
 *         or absent.
 * @note Pure function with no side effects; designed for host-side bridging
 *       of DiagService reports onto the rack logging device.
 */
export function formatDiagEntry(level: DiagLevel, message: string, detail?: JsonValue): JsonObject {
  if (detail === undefined || detail === null) return { level, message };
  if (isJsonObjectValue(detail)) return { level, message, fields: detail };
  return { level, message, fields: { detail } };
}

// ---------------------------------------------------------------------------
// Plugin factory.
// ---------------------------------------------------------------------------

/**
 * @brief Create one std.log instance with injected external effects.
 *
 * @param deps Effects to bind; every member falls back to the real Node
 *        implementation when omitted. The environment view replaces
 *        process.env entirely, so partial injections stay hermetic.
 * @return A RackPlugin built with the SDK helper; independent per call.
 * @note U42_LOG_RING and U42_LOG_FILE are read once here, never per call, so
 *       the log hot path stays allocation-lean: one normalized frozen entry,
 *       one JSON serialization shared by both sinks.
 */
export function createWithDeps(deps: LoggingDeps = {}): RackPlugin {
  const now = deps.now ?? Date.now;
  const envView: Readonly<Record<string, string | undefined>> = deps.env ?? process.env;
  const writeStderr = deps.stderr ?? defaultStderr;
  const appendFile = deps.appendFile ?? defaultAppendFile;

  const capacity = resolveCapacity(envView);
  const filePath = resolveFilePath(envView);
  const ring = new LogRing(capacity);

  let services: PluginServices | null = null;
  let running = false;
  let fileEnabled = filePath !== null;
  // Serialized file appends: keeps line order stable and prevents interleaving.
  let fileChain: Promise<void> = Promise.resolve();

  /**
   * @brief Fan one accepted entry out to the console and file sinks.
   *
   * @param entry Frozen normalized entry already stored in the ring.
   * @note Best effort by contract: any sink failure is swallowed. The first
   *       file failure disables the file sink permanently (memory-only
   *       degradation) and never rejects the log call.
   */
  const dispatchSinks = (entry: LogEntryDoc): void => {
    let line: string;
    try {
      line = JSON.stringify(entry) + '\n';
    } catch {
      // Entries are validated JSON before storage; this guard is absolute.
      return;
    }
    try {
      writeStderr(line);
    } catch {
      // Console sink is best effort; never propagate writer failures.
    }
    const path = filePath;
    if (fileEnabled && path !== null) {
      fileChain = fileChain
        .then(() => {
          if (fileEnabled) return appendFile(path, line);
          return undefined;
        })
        .catch(() => {
          // Silent degradation to memory-only logging.
          fileEnabled = false;
        });
    }
  };

  /**
   * @brief Serve the 'log' method: validate, store, fan out.
   *
   * @param args Request args document.
   * @return `{ accepted: true }` once the entry is buffered.
   * @throws RackError 'invalid_argument' for a malformed args document.
   */
  const handleLog = (args: JsonValue): JsonObject => {
    const doc = asObjectArg(args, LOG_METHOD_NAME, false);
    const level = doc[LEVEL_KEY];
    if (!isLevel(level)) {
      throw new RackError('invalid_argument', 'std.log log.level must be one of info|warn|error');
    }
    const message = doc[MESSAGE_KEY];
    if (typeof message !== 'string') {
      throw new RackError('invalid_argument', 'std.log log.message must be a string');
    }
    const rawFields = doc[FIELDS_KEY];
    let fields: JsonObject | null = null;
    if (rawFields !== undefined && rawFields !== null) {
      if (!isJsonObjectValue(rawFields)) {
        throw new RackError(
          'invalid_argument',
          'std.log log.fields must be a JSON object when present',
        );
      }
      fields = rawFields;
    }
    const rawPlugId = doc[PLUG_ID_KEY];
    let plugId: string | null = null;
    if (rawPlugId !== undefined && rawPlugId !== null) {
      if (typeof rawPlugId !== 'string') {
        throw new RackError(
          'invalid_argument',
          'std.log log.plugId must be a string when present',
        );
      }
      plugId = rawPlugId;
    }
    // Hot path: exactly one normalized frozen entry, stored by reference.
    const entry: LogEntryDoc = Object.freeze({ ts: now(), level, message, fields, plugId });
    ring.push(entry);
    dispatchSinks(entry);
    return { accepted: true };
  };

  /**
   * @brief Serve the 'query' method: filter the buffer, newest window first.
   *
   * @param args Request args document; null is accepted as "no filters".
   * @return Matching entries (ts ascending, at most `limit` newest of them)
   *         plus the total match count before the limit was applied.
   * @throws RackError 'invalid_argument' for malformed filter values.
   */
  const handleQuery = (args: JsonValue): QueryResultDoc => {
    const doc = asObjectArg(args, QUERY_METHOD_NAME, true);
    const rawLevel = doc[LEVEL_KEY];
    let level: DiagLevel | undefined;
    if (rawLevel !== undefined && rawLevel !== null) {
      if (!isLevel(rawLevel)) {
        throw new RackError(
          'invalid_argument',
          'std.log query.level must be one of info|warn|error when present',
        );
      }
      level = rawLevel;
    }
    const rawSince = doc[SINCE_KEY];
    let since: number | undefined;
    if (rawSince !== undefined && rawSince !== null) {
      if (typeof rawSince !== 'number' || !Number.isFinite(rawSince)) {
        throw new RackError(
          'invalid_argument',
          'std.log query.since must be a finite epoch-ms number when present',
        );
      }
      since = rawSince;
    }
    const rawLimit = doc[LIMIT_KEY];
    let limit = DEFAULT_QUERY_LIMIT;
    if (rawLimit !== undefined && rawLimit !== null) {
      if (typeof rawLimit !== 'number' || !Number.isSafeInteger(rawLimit) || rawLimit <= 0) {
        throw new RackError(
          'invalid_argument',
          'std.log query.limit must be a positive integer when present',
        );
      }
      limit = rawLimit;
    }
    const matched = ring.entries().filter((entry) => {
      if (level !== undefined && entry.level !== level) return false;
      if (since !== undefined && entry.ts < since) return false;
      return true;
    });
    const total = matched.length;
    const start = total > limit ? total - limit : 0;
    return { entries: matched.slice(start), total };
  };

  /**
   * @brief Serve the 'stats' method.
   *
   * @return Per-level counts over the current buffer, capacity and used slots.
   */
  const handleStats = (): StatsResultDoc => {
    const byLevel = { info: 0, warn: 0, error: 0 };
    for (const entry of ring.entries()) {
      byLevel[entry.level] += 1;
    }
    return { byLevel, capacity, used: ring.used };
  };

  return definePlugin(
    { plugId: LOG_PLUG_ID, version: LOG_VERSION, init: {} },
    {
      init: (injected) => {
        services = injected;
      },
      start: () => {
        services?.caps.publish({ methods: LOG_METHODS });
        running = true;
      },
      stop: async () => {
        running = false;
        // Drain in-flight appends first: queued chain links still see the sink
        // enabled, so stop() never drops already-accepted lines.
        await fileChain;
        fileEnabled = false;
        ring.clear();
        services = null;
      },
      invoke: (request: InvokeRequest): JsonValue => {
        if (!running) {
          throw new RackError('invalid_state', 'std.log instance is not running');
        }
        if (request.methodName === LOG_METHOD_NAME || request.methodId === LOG_METHOD_ID) {
          return handleLog(request.args);
        }
        if (request.methodName === QUERY_METHOD_NAME || request.methodId === QUERY_METHOD_ID) {
          return handleQuery(request.args);
        }
        if (request.methodName === STATS_METHOD_NAME || request.methodId === STATS_METHOD_ID) {
          return handleStats();
        }
        throw new RackError(
          'not_found',
          `unknown std.log method ${request.methodName} (#${request.methodId})`,
        );
      },
    },
  );
}

/**
 * @brief Create one std.log instance bound to the real Node effects.
 *
 * @return A RackPlugin reading U42_LOG_RING / U42_LOG_FILE from process.env.
 */
export function create(): RackPlugin {
  return createWithDeps();
}

// Loader convention: a create(): RackPlugin factory as default or named export.
export default create;
