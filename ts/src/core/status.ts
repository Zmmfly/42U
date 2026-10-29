/**
 * @file Rack status codes and the typed rack error.
 *
 * @brief Shared failure vocabulary of the whole framework, mirroring the C++ ABI v3 statuses.
 *
 * @note The C++ `wrong_thread` status is dropped: the TS rack runs on the single Node event loop
 *       and serializes all callbacks instead. Every other status keeps its original meaning:
 *       - `ok` success; never appears on a thrown RackError.
 *       - `invalid_argument` malformed input (reversed version range, reserved event name...).
 *       - `unsupported` version outside the caller's explicit closed range, or an incompatible
 *         plugin module shape.
 *       - `not_found` unknown plug id, method, subscription or credential table entry.
 *       - `duplicate` conflicting plug id or subscription token.
 *       - `invalid_state` operation not allowed in the current instance state.
 *       - `busy` in-flight work blocks the operation now; retry later.
 *       - `stale` credential/subscription from a previous generation; never recovers.
 *       - `limit_exceeded` queue capacity or payload/output size bound hit.
 *       - `failed` generic failure (plugin threw, diagnostics recorded).
 *       - `cycle` initialization constraints form a cycle.
 *       - `deferred` unload request accepted and queued, not completed.
 */

/** @brief Machine-readable failure vocabulary shared by host, SDK and plugins. */
export type RackStatus =
  | 'invalid_argument'
  | 'unsupported'
  | 'not_found'
  | 'duplicate'
  | 'invalid_state'
  | 'busy'
  | 'stale'
  | 'limit_exceeded'
  | 'failed'
  | 'cycle'
  | 'deferred';

/**
 * @brief The only error type thrown across rack boundaries.
 *
 * @param status One of the RackStatus values; never 'ok'.
 * @param message Human-readable diagnostic.
 * @param detail Optional structured context (JSON-compatible).
 *
 * @note Plugin lifecycle/invoke implementations may throw anything; the host converts foreign
 *       errors to RackError('failed', ...) and records them through diagnostics. Treat RackError
 *       as the ABI error channel: catching it and inspecting `.status` replaces C++ status codes.
 */
export class RackError extends Error {
  readonly status: RackStatus;
  readonly detail?: unknown;

  constructor(status: RackStatus, message: string, detail?: unknown) {
    super(`[${status}] ${message}`);
    this.name = 'RackError';
    this.status = status;
    this.detail = detail;
  }
}

/**
 * @brief Narrow an unknown thrown value to RackError.
 * @param value Anything caught in a catch clause.
 * @return true when value is a RackError.
 */
export function isRackError(value: unknown): value is RackError {
  return value instanceof RackError;
}

/**
 * @brief Map any thrown value onto the rack status vocabulary.
 * @param value Anything caught in a catch clause.
 * @return The RackError status, or 'failed' for foreign errors.
 */
export function statusOf(value: unknown): RackStatus {
  return isRackError(value) ? value.status : 'failed';
}
