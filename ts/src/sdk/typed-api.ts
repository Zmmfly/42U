/**
 * @file typed-api.ts
 * @brief Typed direct-call proxy over the invoke gateway.
 *
 * @details This is typed sugar over CallService, not a bypass: every proxy call forwards to
 *          calls.call(handle.credential, methodName, packedArgs), so lease validation,
 *          revocation notification, generation staleness, in-flight draining, reentrancy and
 *          size limits keep their exact gateway semantics. After handle.release() the same
 *          proxy calls reject with 'stale' straight from the gateway.
 *
 *          Plain direct references to a provider object were considered and rejected by the
 *          design ruling (docs/design.md §2.1): a live JS reference would keep the provider
 *          module unreachable for the garbage collector, make unload unenforceable, and void
 *          revocation, generation-staleness and in-flight draining — directly contradicting the
 *          pluggability goal. The proxy therefore holds only the opaque credential plus the
 *          gateway view, never a provider reference.
 */

import type { CallService, JsonValue, LeaseHandle } from '../core/types.ts';

/**
 * @brief Pack positional proxy arguments into the gateway args document.
 *
 * @param args Positional arguments of one proxy call.
 * @return null when no argument was given, the single argument itself, or all arguments as a
 *         JSON array.
 */
function packArgs(args: readonly JsonValue[]): JsonValue {
  if (args.length === 0) return null;
  if (args.length === 1) return args[0] as JsonValue;
  return [...args];
}

/**
 * @brief Create a typed direct-call proxy bound to one lease handle.
 *
 * @param calls Per-instance invoke gateway view.
 *
 * @param handle Lease handle whose credential is forwarded with every call.
 *
 * @return A proxy of type T whose string properties (except 'then') are functions
 *         `(...args: JsonValue[]) => Promise<JsonValue>` forwarding to the gateway; symbol
 *         properties and 'then' resolve to undefined so the proxy is never mistaken for a
 *         thenable.
 *
 * @note The proxy is stateless: each property access returns a fresh forwarder and no call
 *       results are cached, so the gateway remains the single source of truth for every call.
 */
export function typedApi<T extends object>(calls: CallService, handle: LeaseHandle): T {
  const forward = (methodName: string) =>
    (...args: JsonValue[]): Promise<JsonValue> =>
      calls.call(handle.credential, methodName, packArgs(args));
  return new Proxy({} as T, {
    get(_target: T, prop: string | symbol): unknown {
      if (typeof prop === 'symbol' || prop === 'then') return undefined;
      return forward(prop);
    },
  });
}
