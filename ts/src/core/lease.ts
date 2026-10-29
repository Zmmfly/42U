/**
 * @file Version-qualified lease manager: credential minting, generation binding, in-flight
 *        call counting and the revocation protocol.
 *
 * @brief Faithful TS port of the C++ 42U lease table (context.cc context::acquire /
 *        context::release / engine::revoke): consumers borrow by plugId plus an explicit
 *        closed version range, receive an opaque credential and the by-value actual version,
 *        and every credential dies with the provider generation it was minted against.
 *
 * @note Concurrency premise: acquire/beginCall/release are synchronous, so on the single Node
 *       event loop each of them is atomic. Only revokeProvider awaits, because revoker
 *       callbacks may be async. The host MUST withdraw the provider capability (making
 *       deps.lookup fail) BEFORE calling revokeProvider: leases acquired during revocation
 *       are then impossible because acquire reports 'not_found'. This module is pure table
 *       bookkeeping: it never exposes provider objects or functions to consumers — the
 *       credential handle is an opaque frozen token doubling as the table key, lease records
 *       stay host-private, and the version returned to consumers is a by-value copy.
 */

import type {
  ActiveLeaseInfo,
  CapEntry,
  LeaseCallGuard,
  LeaseCredential,
  LeaseManager,
  PluginVersion,
  Revoker,
} from './types.ts';
import { RackError } from './status.ts';
import { acceptsVersion, isPluginVersion, validVersionRange } from './version.ts';

/** @brief The only inspectable part of a LeaseCredential; everything else is host-private. */
const CREDENTIAL_KIND = 'u42.lease.credential';

/**
 * @brief Host-private lease record, keyed in the table by its opaque credential handle.
 * @note Map membership is the definition of "not yet returned": release-ok and
 *       releaseAllFrom delete entries, and revokeProvider only proves quiescence through
 *       revokers actually calling release.
 */
interface LeaseRecord {
  readonly consumerId: string;
  readonly providerPlugId: string;
  readonly providerGeneration: number;
  /** @brief Actual version copied by value from the provider entry at acquire time. */
  readonly version: PluginVersion;
  /** @brief Revocation receiver registered by the borrowing consumer. */
  readonly revoker: Revoker;
  /** @brief The exact credential object handed out; revocation reuses it verbatim. */
  readonly credential: LeaseCredential;
  /** @brief Number of beginCall guards still open on this lease. */
  inFlight: number;
  /** @brief Set immediately before deletion; defends lingering record references. */
  returned: boolean;
}

/** @brief Host dependencies injected into createLeaseManager. */
export interface LeaseManagerDeps {
  /**
   * @brief Resolve the current provider entry for a plugId.
   *
   * @param plugId Provider identity.
   * @return The live capability entry, or undefined when absent or withdrawn.
   */
  readonly lookup: (plugId: string) => CapEntry | undefined;
  /**
   * @brief Optional diagnostics sink used when a revoker throws inside revokeProvider.
   *
   * @param message Human-readable diagnostic.
   * @param detail Structured context (provider, generation, consumer, error).
   */
  readonly onDiag?: (message: string, detail?: unknown) => void;
}

/**
 * @brief Structural check for objects posing as lease credentials.
 *
 * @param value Candidate of unknown origin.
 * @return The opaque handle object when well-formed, undefined otherwise.
 */
function credentialHandle(value: LeaseCredential): object | undefined {
  if (typeof value !== 'object' || value === null) return undefined;
  const candidate = value as { kind?: unknown; handle?: unknown };
  if (candidate.kind !== CREDENTIAL_KIND) return undefined;
  if (typeof candidate.handle !== 'object' || candidate.handle === null) return undefined;
  return candidate.handle;
}

/**
 * @brief Runtime check for a usable revocation receiver.
 *
 * @param value Candidate of unknown origin.
 * @return true when value exposes a callable onRevoke.
 */
function isRevoker(value: unknown): value is Revoker {
  return (
    typeof value === 'object' &&
    value !== null &&
    typeof (value as { onRevoke?: unknown }).onRevoke === 'function'
  );
}

/**
 * @brief Copy a version triple by value so later provider mutations cannot leak into leases.
 *
 * @param value Version read from a provider entry.
 * @return A frozen copy that is safe to hand to consumers.
 */
function copyVersion(value: PluginVersion): PluginVersion {
  return Object.freeze({ major: value.major, minor: value.minor, patch: value.patch });
}

/**
 * @brief Forward a diagnostic without ever letting the sink throw into the protocol.
 *
 * @param deps Manager dependencies carrying the optional sink.
 * @param message Human-readable diagnostic.
 * @param detail Structured context.
 */
function reportDiag(deps: LeaseManagerDeps, message: string, detail?: unknown): void {
  try {
    deps.onDiag?.(message, detail);
  } catch {
    // Diagnostics must never propagate failures into the revocation protocol.
  }
}

/**
 * @brief Create the host-side lease manager.
 *
 * @param deps Host dependencies: the capability lookup view and an optional diagnostics sink.
 *
 * @return A LeaseManager tracking every outstanding lease of the rack.
 *
 * @note Credentials embed an opaque frozen handle object used as the table key; lease records
 *       stay host-private and nothing provider-side (objects, functions, entry references) is
 *       ever exposed to consumers. See the file header for the revocation concurrency premise.
 */
export function createLeaseManager(deps: LeaseManagerDeps): LeaseManager {
  /** @brief Outstanding leases keyed by credential handle object identity. */
  const records = new Map<object, LeaseRecord>();

  const manager: LeaseManager = {
    acquire(consumerId, plugId, range, revoker) {
      if (!isRevoker(revoker)) {
        throw new RackError('invalid_argument', 'acquire requires a revocation receiver');
      }
      if (
        typeof range !== 'object' ||
        range === null ||
        !isPluginVersion(range.minimum) ||
        !isPluginVersion(range.maximum)
      ) {
        throw new RackError('invalid_argument', 'acquire requires a well-formed closed version range', {
          plugId,
        });
      }
      if (!validVersionRange(range)) {
        throw new RackError(
          'invalid_argument',
          'the allowed version range must not have its minimum above its maximum',
          { plugId },
        );
      }
      const entry = deps.lookup(plugId);
      if (entry === undefined) {
        throw new RackError('not_found', `no active provider publishes for '${plugId}'`);
      }
      const actual = copyVersion(entry.version);
      if (!acceptsVersion(range, actual)) {
        throw new RackError('unsupported', `the provider version for '${plugId}' is outside the allowed range`, {
          plugId,
          actual,
        });
      }
      // Mint the credential: an opaque frozen token object doubles as the table key and the
      // record itself never leaves the host. A failed acquire above leaves no partial lease.
      const handle: object = Object.freeze({});
      const credential: LeaseCredential = Object.freeze({ kind: CREDENTIAL_KIND, handle });
      records.set(handle, {
        consumerId,
        providerPlugId: plugId,
        providerGeneration: entry.generation,
        version: actual,
        revoker,
        credential,
        inFlight: 0,
        returned: false,
      });
      return {
        credential,
        plugId,
        version: actual,
        release: () => Promise.resolve(manager.release(credential, consumerId)),
      };
    },

    release(credential, consumerId) {
      const handle = credentialHandle(credential);
      if (handle === undefined) return 'stale'; // malformed credentials can never match a lease
      const record = records.get(handle);
      if (record === undefined || record.returned || record.consumerId !== consumerId) {
        return 'stale';
      }
      if (record.inFlight > 0) return 'busy'; // lease kept; retry once the in-flight call ends
      record.returned = true;
      records.delete(handle);
      return 'ok';
    },

    beginCall(credential, consumerId) {
      const handle = credentialHandle(credential);
      if (handle === undefined) {
        throw new RackError('invalid_argument', 'beginCall requires a lease credential');
      }
      const record = records.get(handle);
      if (record === undefined || record.returned || record.consumerId !== consumerId) {
        throw new RackError('stale', 'the lease credential is no longer valid');
      }
      // Generation re-check against the live registry: a reloaded provider — even one that
      // discloses the identical version — must permanently invalidate old credentials.
      const entry = deps.lookup(record.providerPlugId);
      if (entry === undefined || entry.generation !== record.providerGeneration) {
        throw new RackError(
          'stale',
          `the provider generation for '${record.providerPlugId}' changed; re-acquire the lease`,
        );
      }
      record.inFlight += 1;
      const lease: ActiveLeaseInfo = Object.freeze({
        credential,
        consumerId: record.consumerId,
        providerPlugId: record.providerPlugId,
        providerGeneration: record.providerGeneration,
        version: record.version,
      });
      let ended = false;
      const guard: LeaseCallGuard = {
        lease,
        endCall: () => {
          if (ended) return; // idempotent: a repeated endCall must not double-decrement
          ended = true;
          const live = records.get(handle);
          if (live !== undefined && live.inFlight > 0) live.inFlight -= 1;
        },
      };
      return guard;
    },

    async revokeProvider(providerPlugId, generation) {
      // Snapshot first (Map iteration follows insertion order, so notification order is
      // deterministic); revokers returning their own or earlier credentials mid-loop are
      // handled by the re-lookup below.
      const pending: object[] = [];
      for (const [handle, record] of records) {
        if (record.providerPlugId === providerPlugId && record.providerGeneration === generation) {
          pending.push(handle);
        }
      }
      for (const handle of pending) {
        const record = records.get(handle);
        if (record === undefined) continue; // already returned by an earlier callback
        try {
          await record.revoker.onRevoke(record.credential);
        } catch (error) {
          reportDiag(deps, `a lease revoker for '${providerPlugId}' threw during onRevoke`, {
            providerPlugId,
            generation,
            consumerId: record.consumerId,
            error: error instanceof Error ? error.message : String(error),
          });
        }
      }
      // Re-check after every callback ran: only actually returned leases prove quiescence.
      for (const [, record] of records) {
        if (record.providerPlugId === providerPlugId && record.providerGeneration === generation) {
          return 'failed';
        }
      }
      return 'ok';
    },

    releaseAllFrom(consumerId) {
      // Silent host-side cleanup at consumer stop: no revoker is notified, and leases still
      // in flight simply vanish, which the guards' endCall tolerates by design.
      for (const [handle, record] of records) {
        if (record.consumerId === consumerId) records.delete(handle);
      }
    },

    hasOutstandingFor(providerPlugId, generation) {
      for (const [, record] of records) {
        if (record.providerPlugId === providerPlugId && record.providerGeneration === generation) {
          return true;
        }
      }
      return false;
    },

    get activeCount() {
      return records.size;
    },
  };

  return manager;
}
