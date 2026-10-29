/**
 * @file Tests for plugin version arithmetic (closed ranges, endpoint semantics).
 */
import { describe, expect, it } from 'vitest';
import {
  acceptsVersion,
  assertPluginVersion,
  exactVersion,
  formatVersion,
  isPluginVersion,
  validVersionRange,
  versionEquals,
  versionLess,
} from '../src/core/version.ts';
import { statusOf } from '../src/core/status.ts';

const v = (major: number, minor: number, patch: number) => ({ major, minor, patch });
const range = (minimum: ReturnType<typeof v>, maximum: ReturnType<typeof v>) => ({ minimum, maximum });

describe('version triples', () => {
  it('validates structural triples', () => {
    expect(isPluginVersion(v(0, 0, 0))).toBe(true);
    expect(isPluginVersion(v(1, 2, 3))).toBe(true);
    expect(isPluginVersion({ major: 1, minor: 2 })).toBe(false);
    expect(isPluginVersion({ major: -1, minor: 0, patch: 0 })).toBe(false);
    expect(isPluginVersion({ major: 1.5, minor: 0, patch: 0 })).toBe(false);
    expect(isPluginVersion({ major: Number.NaN, minor: 0, patch: 0 })).toBe(false);
    expect(isPluginVersion(null)).toBe(false);
    expect(isPluginVersion('1.0.0')).toBe(false);
  });

  it('asserts with invalid_argument on malformed triples', () => {
    expect(() => assertPluginVersion({ major: '1' }, 'bad desc')).toThrowError();
    try {
      assertPluginVersion(undefined, 'bad desc');
      expect.unreachable();
    } catch (error) {
      expect(statusOf(error)).toBe('invalid_argument');
    }
  });

  it('compares lexicographically by major, minor, patch', () => {
    expect(versionLess(v(1, 0, 0), v(1, 0, 1))).toBe(true);
    expect(versionLess(v(1, 0, 9), v(1, 1, 0))).toBe(true);
    expect(versionLess(v(1, 9, 9), v(2, 0, 0))).toBe(true);
    expect(versionLess(v(2, 0, 0), v(1, 99, 99))).toBe(false);
    expect(versionLess(v(1, 0, 0), v(1, 0, 0))).toBe(false);
  });

  it('compares numeric equality including 0.0.0', () => {
    expect(versionEquals(v(0, 0, 0), v(0, 0, 0))).toBe(true);
    expect(versionEquals(v(1, 2, 3), v(1, 2, 3))).toBe(true);
    expect(versionEquals(v(1, 2, 3), v(1, 2, 4))).toBe(false);
  });

  it('formats for diagnostics', () => {
    expect(formatVersion(v(1, 2, 3))).toBe('1.2.3');
    expect(formatVersion(v(0, 0, 0))).toBe('0.0.0');
  });
});

describe('closed version ranges', () => {
  it('accepts equal endpoints and 0.0.0 bounds', () => {
    expect(validVersionRange(range(v(0, 0, 0), v(0, 0, 0)))).toBe(true);
    expect(validVersionRange(range(v(1, 0, 0), v(1, 0, 0)))).toBe(true);
    expect(validVersionRange(range(v(1, 0, 0), v(2, 0, 0)))).toBe(true);
  });

  it('rejects reversed endpoints', () => {
    expect(validVersionRange(range(v(1, 1, 0), v(1, 0, 0)))).toBe(false);
    expect(validVersionRange(range(v(2, 0, 0), v(1, 9, 9)))).toBe(false);
  });

  it('contains both endpoints inclusively', () => {
    const allowed = range(v(1, 0, 0), v(1, 2, 0));
    expect(acceptsVersion(allowed, v(1, 0, 0))).toBe(true);
    expect(acceptsVersion(allowed, v(1, 2, 0))).toBe(true);
    expect(acceptsVersion(allowed, v(1, 1, 5))).toBe(true);
    expect(acceptsVersion(allowed, v(1, 2, 1))).toBe(false);
    expect(acceptsVersion(allowed, v(0, 9, 9))).toBe(false);
  });

  it('accepts major crossings only when explicitly written', () => {
    const crossing = range(v(1, 0, 0), v(2, 3, 0));
    expect(acceptsVersion(crossing, v(2, 0, 0))).toBe(true);
    expect(acceptsVersion(crossing, v(2, 3, 0))).toBe(true);
    const sameMajor = range(v(1, 0, 0), v(1, Number.MAX_SAFE_INTEGER, Number.MAX_SAFE_INTEGER));
    expect(acceptsVersion(sameMajor, v(2, 0, 0))).toBe(false);
  });

  it('treats reversed ranges as never matching', () => {
    const reversed = range(v(1, 1, 0), v(1, 0, 0));
    expect(acceptsVersion(reversed, v(1, 0, 0))).toBe(false);
    expect(acceptsVersion(reversed, v(1, 1, 0))).toBe(false);
  });

  it('builds exact-version requirements', () => {
    const exact = exactVersion(v(1, 4, 2));
    expect(validVersionRange(exact)).toBe(true);
    expect(acceptsVersion(exact, v(1, 4, 2))).toBe(true);
    expect(acceptsVersion(exact, v(1, 4, 3))).toBe(false);
  });
});
