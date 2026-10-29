/**
 * @file Data-plane utilities for JSON values crossing plugin boundaries.
 *
 * @brief The TS rack replaces the C++ byte views and ABI writer with plain JSON values.
 */

/** @brief JSON-compatible value: the only data plane that crosses plugin boundaries. */
export type JsonValue = string | number | boolean | null | JsonArray | JsonObject;
/** @brief JSON array. */
export type JsonArray = readonly JsonValue[];
/** @brief JSON object. */
export type JsonObject = { readonly [key: string]: JsonValue };

/**
 * @brief Structural check for values that can safely cross the boundary.
 * @param value Candidate payload.
 * @return true for finite JSON values (no undefined, NaN, Infinity, functions, symbols, bigint,
 *         cycles or class instances).
 * @note Finite-only numbers: NaN/Infinity are rejected because they do not survive JSON.
 */
export function isJsonValue(value: unknown): value is JsonValue {
  return checkJson(value, new Set());
}

function checkJson(value: unknown, seen: Set<unknown>): boolean {
  switch (typeof value) {
    case 'string':
    case 'boolean':
      return true;
    case 'number':
      return Number.isFinite(value);
    case 'object':
      if (value === null) return true;
      if (seen.has(value)) return false;
      seen.add(value);
      if (Array.isArray(value)) return value.every((item) => checkJson(item, seen));
      const proto = Object.getPrototypeOf(value);
      if (proto !== Object.prototype && proto !== null) return false;
      return Object.values(value).every((item) => checkJson(item, seen));
    default:
      return false;
  }
}

/**
 * @brief Estimate the serialized size of a JSON value for limit enforcement.
 * @param value Payload to measure.
 * @return Estimated UTF-8 JSON size in bytes (approximate; never underestimates by more than
 *         structural overhead of ~2 bytes per nesting level).
 *
 * @note Replaces the C++ payload_limit/output_limit byte accounting. The estimate uses string
 *       lengths and a small per-node overhead; hosts enforce limits with it before delivery.
 */
export function estimateJsonBytes(value: unknown): number {
  return measure(value, 0);
}

function measure(value: unknown, depth: number): number {
  if (depth > 64) return 0;
  switch (typeof value) {
    case 'string':
      // UTF-8 worst case: 3 bytes per UCS-2 code unit covers BMP; 4-byte pairs still fit.
      return value.length * 3 + 2;
    case 'number':
      return 24;
    case 'boolean':
      return 5;
    case 'object':
      if (value === null) return 4;
      if (Array.isArray(value)) {
        return value.reduce((sum, item) => sum + measure(item, depth + 1) + 1, 2);
      }
      let total = 2;
      for (const [key, item] of Object.entries(value)) {
        total += key.length * 3 + 4 + measure(item, depth + 1) + 1;
      }
      return total;
    default:
      return 8;
  }
}
