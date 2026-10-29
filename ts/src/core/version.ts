/**
 * @file Plugin business version arithmetic.
 *
 * @brief Numeric semantics of PluginVersion triples and explicit closed VersionRanges.
 *
 * @note Faithful port of the C++ v3 helpers (version_less / valid_version_range /
 *       accepts_version): the host never infers compatibility, it only checks the caller's
 *       explicit inclusive bounds. No string parsing, no prerelease syntax.
 */

import type { PluginVersion, VersionRange } from './types.ts';
import { RackError } from './status.ts';

/**
 * @brief Structural validation of a plugin version triple.
 * @param value Candidate value of unknown origin (module descriptors, tests).
 * @return true for three finite, safe, non-negative integers.
 */
export function isPluginVersion(value: unknown): value is PluginVersion {
  if (typeof value !== 'object' || value === null) return false;
  const record = value as Record<string, unknown>;
  for (const key of ['major', 'minor', 'patch'] as const) {
    const part = record[key];
    if (typeof part !== 'number' || !Number.isSafeInteger(part) || part < 0) return false;
  }
  return true;
}

/**
 * @brief Assert a well-formed version triple.
 * @param value Candidate version.
 * @param label Context included in the thrown error.
 * @throws RackError 'invalid_argument' when the triple is malformed.
 */
export function assertPluginVersion(value: unknown, label: string): asserts value is PluginVersion {
  if (!isPluginVersion(value)) {
    throw new RackError('invalid_argument', `${label} is not a valid plugin version`, { value });
  }
}

/** @brief Numeric equality of two version triples. */
export function versionEquals(left: PluginVersion, right: PluginVersion): boolean {
  return left.major === right.major && left.minor === right.minor && left.patch === right.patch;
}

/**
 * @brief Lexicographic ordering by major, then minor, then patch.
 * @return true when left strictly precedes right.
 */
export function versionLess(left: PluginVersion, right: PluginVersion): boolean {
  if (left.major !== right.major) return left.major < right.major;
  if (left.minor !== right.minor) return left.minor < right.minor;
  return left.patch < right.patch;
}

/**
 * @brief Validate inclusive range ordering.
 * @return true when minimum is not greater than maximum. Equal endpoints are valid (exact
 *         version); 0.0.0 endpoints are valid.
 */
export function validVersionRange(range: VersionRange): boolean {
  return !versionLess(range.maximum, range.minimum);
}

/**
 * @brief Check the caller's explicit version bounds.
 * @param allowed Inclusive accepted range supplied by the borrower.
 * @param actual Version copied by value from the selected provider descriptor.
 * @return true when the range is valid and contains actual, including both endpoints.
 * @note Crossing a major boundary is accepted only because the caller explicitly wrote such a
 *       range; the host adds no policy of its own.
 */
export function acceptsVersion(allowed: VersionRange, actual: PluginVersion): boolean {
  return validVersionRange(allowed) && !versionLess(actual, allowed.minimum) && !versionLess(allowed.maximum, actual);
}

/**
 * @brief Build an exact-version requirement (closed range with equal endpoints).
 * @param value The only version to accept.
 */
export function exactVersion(value: PluginVersion): VersionRange {
  return { minimum: value, maximum: value };
}

/**
 * @brief Render a version as "major.minor.patch" for diagnostics only.
 * @note Display helper; never parsed back.
 */
export function formatVersion(value: PluginVersion): string {
  return `${value.major}.${value.minor}.${value.patch}`;
}
