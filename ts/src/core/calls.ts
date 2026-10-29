/**
 * @file Invoke gateway: the only business call path between rack plugins.
 *
 * @brief Implements the InvokeGateway port: consumer -> host gateway -> provider invoke, with a
 *        single validation chain shared by method-name and numeric-id selectors.
 *
 * @note Faithful adaptation of the C++ `direct_call` path in src/context.cc: every call presents
 *       a lease credential, the provider is re-resolved by leased generation against the
 *       capability table, only disclosed methods may be entered, re-entry into an in-flight
 *       provider instance is rejected with 'busy', and both arguments and results are checked
 *       against the configured byte limits. Providers never receive references to other plugins;
 *       the gateway hands over a by-value InvokeRequest only.
 */

import { estimateJsonBytes, isJsonValue, type JsonValue } from './json.ts';
import { RackError } from './status.ts';
import type {
  CapEntry,
  InvokeGateway,
  LeaseCredential,
  LeaseManager,
  MethodDesc,
  ProviderEndpoint,
} from './types.ts';

/** @brief Default byte limit for call arguments and results (1 MiB, mirroring RackOptions). */
const DEFAULT_BYTE_LIMIT = 1024 * 1024;

/** @brief Method selector accepted by InvokeGateway.call: one disclosed name or one numeric id. */
type MethodSelector = { readonly byName: string } | { readonly byId: number };

/**
 * @brief Construction dependencies of the invoke gateway.
 *
 * @note Wired by the host (core/host.ts): the gateway owns no plugin state of its own. Every
 *       lookup is a callback so the gateway stays independent of registry and loader internals.
 */
export interface InvokeGatewayDeps {
  /** @brief Lease table validating credentials and counting in-flight calls per lease. */
  readonly leases: LeaseManager;
  /**
   * @brief Resolve the provider endpoint of one live generation.
   *
   * @param plugId Provider plug id taken from the validated lease.
   * @param generation Leased provider generation; never a newer or older one.
   *
   * @return The endpoint, or undefined when no instance lives for that exact generation.
   */
  readonly resolveProvider: (plugId: string, generation: number) => ProviderEndpoint | undefined;
  /**
   * @brief Look up the current capability entry of a plug id.
   *
   * @param plugId Provider plug id taken from the validated lease.
   *
   * @return The published entry, or undefined when the provider is withdrawn.
   */
  readonly lookupCaps: (plugId: string) => CapEntry | undefined;
  /** @brief Maximum estimated JSON bytes per call arguments document. Default 1 MiB. */
  readonly payloadLimit?: number;
  /** @brief Maximum estimated JSON bytes per call result. Default 1 MiB. */
  readonly outputLimit?: number;
  /** @brief Optional diagnostics sink notified when a provider invocation throws. */
  readonly onDiag?: (message: string, detail?: unknown) => void;
}

/**
 * @brief Create the invoke gateway: consumer -> host -> provider, the sole business call path.
 *
 * @param deps Host wiring: lease table, capability lookup, endpoint resolver, limits, diag sink.
 *
 * @return An InvokeGateway whose call() accepts both { byName } and { byId } selectors through
 *         one shared implementation; the selector only decides the disclosure lookup.
 *
 * @note Validation chain per call: arguments JSON check and payload limit, lease beginCall,
 *       capability generation re-check, disclosure lookup, endpoint resolution by leased
 *       generation, per-instance reentrancy guard, invoke presence, provider exception
 *       conversion to 'failed' with diagnostics, then result JSON check and output limit.
 *       The host's CallService adapters merely construct the selector object, so names and ids
 *       walk exactly this one path. The in-flight lease counter is closed in a finally on every
 *       exit, and oversized partial results are discarded, never delivered.
 */
export function createInvokeGateway(deps: InvokeGatewayDeps): InvokeGateway {
  const payloadLimit = deps.payloadLimit ?? DEFAULT_BYTE_LIMIT;
  const outputLimit = deps.outputLimit ?? DEFAULT_BYTE_LIMIT;
  /** Keys of provider instances with a call currently in flight (reentrancy guard). */
  const inFlightProviders = new Set<string>();

  const call = async (
    consumerId: string,
    credential: LeaseCredential,
    method: MethodSelector,
    args: JsonValue,
  ): Promise<JsonValue> => {
    if (!isJsonValue(args)) {
      throw new RackError('invalid_argument', 'call arguments must be a JSON value');
    }
    if (estimateJsonBytes(args) > payloadLimit) {
      throw new RackError('limit_exceeded', 'call arguments exceed the payload limit', { payloadLimit });
    }

    const guard = deps.leases.beginCall(credential, consumerId);
    try {
      const lease = guard.lease;
      const capsEntry = deps.lookupCaps(lease.providerPlugId);
      if (capsEntry === undefined || capsEntry.generation !== lease.providerGeneration) {
        throw new RackError(
          'stale',
          `the leased provider '${lease.providerPlugId}' is withdrawn or was reloaded`,
          {
            plugId: lease.providerPlugId,
            leasedGeneration: lease.providerGeneration,
            publishedGeneration: capsEntry?.generation,
          },
        );
      }

      const disclosed = findMethod(capsEntry, method);
      if (disclosed === undefined) {
        throw new RackError(
          'not_found',
          `the provider '${lease.providerPlugId}' does not publish ${describeSelector(method)}`,
          { plugId: lease.providerPlugId },
        );
      }

      const endpoint = deps.resolveProvider(lease.providerPlugId, lease.providerGeneration);
      if (endpoint === undefined) {
        throw new RackError(
          'stale',
          `no live endpoint for '${lease.providerPlugId}' generation ${lease.providerGeneration}`,
        );
      }
      if (inFlightProviders.has(endpoint.key)) {
        throw new RackError(
          'busy',
          `the provider instance '${endpoint.key}' is already executing a call`,
        );
      }
      inFlightProviders.add(endpoint.key);
      try {
        // Defensive assembly check: a published provider without an invoke entry point is a
        // host-side wiring mistake, reported as 'failed' before any plugin code runs.
        if (typeof (endpoint as { readonly invoke?: unknown }).invoke !== 'function') {
          throw new RackError(
            'failed',
            `the provider endpoint '${endpoint.key}' exposes methods but has no invoke entry point`,
          );
        }

        let result: unknown;
        try {
          result = await endpoint.invoke({
            methodId: disclosed.id,
            methodName: disclosed.name,
            args,
          });
        } catch (error) {
          const summary = summarizeThrown(error);
          const message = `the provider '${lease.providerPlugId}' threw in method '${disclosed.name}'`;
          deps.onDiag?.(message, summary);
          throw new RackError('failed', message, summary);
        }
        if (!isJsonValue(result)) {
          throw new RackError(
            'invalid_argument',
            `the method '${disclosed.name}' of '${lease.providerPlugId}' returned a non-JSON value`,
          );
        }
        if (estimateJsonBytes(result) > outputLimit) {
          // Oversized partial results are discarded, never delivered.
          throw new RackError(
            'limit_exceeded',
            `the result of '${disclosed.name}' of '${lease.providerPlugId}' exceeds the output limit`,
            { outputLimit },
          );
        }
        return result;
      } finally {
        inFlightProviders.delete(endpoint.key);
      }
    } finally {
      guard.endCall();
    }
  };

  return { call };
}

/**
 * @brief Resolve one disclosed method descriptor.
 *
 * @param capsEntry Capability entry of the leased provider generation.
 * @param method Name or numeric id selector; the only place the two entries differ.
 *
 * @return The matching MethodDesc, or undefined when the method is not disclosed.
 */
function findMethod(capsEntry: CapEntry, method: MethodSelector): MethodDesc | undefined {
  if ('byName' in method) {
    return capsEntry.caps.methods.find((candidate) => candidate.name === method.byName);
  }
  return capsEntry.caps.methods.find((candidate) => candidate.id === method.byId);
}

/**
 * @brief Render a method selector for error messages.
 *
 * @param method Name or numeric id selector.
 *
 * @return A short human-readable description of the requested method.
 */
function describeSelector(method: MethodSelector): string {
  return 'byName' in method ? `method '${method.byName}'` : `method id ${method.byId}`;
}

/**
 * @brief Summarize a foreign thrown value for diagnostics and the RackError detail.
 *
 * @param error Anything a provider invoke threw or rejected with.
 *
 * @return A JSON-safe summary: name/message for Errors, otherwise a string rendering.
 */
function summarizeThrown(error: unknown): { readonly name: string; readonly message: string } | string {
  if (error instanceof Error) {
    return { name: error.name, message: error.message };
  }
  return typeof error === 'string' ? error : String(error);
}
