/**
 * @file Plugin-side SDK helper for implementing the RackPlugin contract.
 *
 * @brief definePlugin() validates a plugin descriptor, freezes a defensive copy of it and binds
 *        optional lifecycle/invoke hooks into a ready-to-return RackPlugin object.
 *
 * @details Thin adapter over the frozen contracts in src/core: plugin authors declare identity,
 *          business version and init constraints, implement only the hooks they need, and get a
 *          host-compatible plugin object in return. The helper adds no behaviour of its own; the
 *          host still orchestrates scanning, ordering, lifecycle and the invoke gateway.
 */

import type {
  InvokeRequest,
  PluginDescriptor,
  PluginServices,
  RackPlugin,
} from '../core/types.ts';
import type { JsonValue } from '../core/json.ts';
import { RackError } from '../core/status.ts';
import { assertPluginVersion } from '../core/version.ts';

/**
 * @brief Optional plugin-side hooks wrapped by definePlugin().
 *
 * @note Every hook is optional. The returned RackPlugin substitutes a no-op for the mandatory
 *       init/start/stop entries and omits destroy/invoke entirely when they are not provided.
 *       Hooks may be synchronous or asynchronous; the host awaits both.
 */
export interface PluginHooks {
  /** @brief Called once with the per-instance host services; subscribe and prepare only. */
  init?(services: PluginServices): void | Promise<void>;
  /** @brief Called once when the instance becomes Active; publish capabilities here. */
  start?(): void | Promise<void>;
  /** @brief Called once on teardown; end all plugin-side asynchronous work here. */
  stop?(): void | Promise<void>;
  /** @brief Final cleanup after stop; releases plugin-owned resources. */
  destroy?(): void | Promise<void>;
  /** @brief Dynamic method entry served through the host invoke gateway. */
  invoke?(request: InvokeRequest): JsonValue | Promise<JsonValue>;
}

/**
 * @brief Check that a value is an array containing only strings.
 *
 * @param value Candidate of runtime-unknown origin.
 * @return true for arrays whose every element is a string.
 */
function isStringArray(value: unknown): boolean {
  return Array.isArray(value) && value.every((entry) => typeof entry === 'string');
}

/**
 * @brief Throw the standard definePlugin rejection.
 *
 * @param message Human-readable description of the violated rule.
 * @throws RackError 'invalid_argument' always.
 */
function fail(message: string): never {
  throw new RackError('invalid_argument', `definePlugin: ${message}`);
}

/**
 * @brief Validate the shape of a plugin descriptor defensively.
 *
 * @param desc Candidate descriptor of runtime-unknown origin.
 * @throws RackError 'invalid_argument' when desc is not an object, plugId is not a non-empty
 *         string, version is not a valid triple, init is not an object, init.before/after are
 *         not string arrays, or an explicit init.priority is not a safe integer.
 */
function validateDescriptor(desc: PluginDescriptor): void {
  if (typeof desc !== 'object' || desc === null) fail('descriptor must be an object');
  if (typeof desc.plugId !== 'string' || desc.plugId.length === 0) {
    fail('plugId must be a non-empty string');
  }
  assertPluginVersion(desc.version, 'version');
  if (typeof desc.init !== 'object' || desc.init === null) {
    fail('init must be an InitOrderSpec object');
  }
  if (desc.init.priority !== undefined &&
      (typeof desc.init.priority !== 'number' || !Number.isSafeInteger(desc.init.priority))) {
    fail('init.priority must be an integer when present');
  }
  if (desc.init.before !== undefined && !isStringArray(desc.init.before)) {
    fail('init.before must be an array of plug id strings');
  }
  if (desc.init.after !== undefined && !isStringArray(desc.init.after)) {
    fail('init.after must be an array of plug id strings');
  }
}

/**
 * @brief Validate that every present hook is a function.
 *
 * @param impl Hook bundle of runtime-unknown origin.
 * @throws RackError 'invalid_argument' when impl is not an object or a present hook is not a
 *         function.
 */
function validateHooks(impl: PluginHooks): void {
  if (typeof impl !== 'object' || impl === null) fail('hooks must be an object');
  for (const key of ['init', 'start', 'stop', 'destroy', 'invoke'] as const) {
    const hook = impl[key];
    if (hook !== undefined && typeof hook !== 'function') {
      fail(`hook "${key}" must be a function when present`);
    }
  }
}

/**
 * @brief Build a RackPlugin from a validated descriptor and optional hooks.
 *
 * @param desc Plugin identity, business version and init-order constraints.
 * @param impl Optional lifecycle and invoke hooks; omitted hooks become no-ops.
 * @return A RackPlugin holding a deep-frozen defensive copy of desc plus the wrapped hooks.
 * @throws RackError 'invalid_argument' when desc or impl is malformed.
 * @note The caller's objects are neither frozen nor retained: desc is copied and deep-frozen, so
 *       later mutations by the plugin author cannot alter the identity announced to the host.
 */
export function definePlugin(desc: PluginDescriptor, impl: PluginHooks = {}): RackPlugin {
  validateDescriptor(desc);
  validateHooks(impl);

  const frozenDesc: PluginDescriptor = Object.freeze({
    plugId: desc.plugId,
    version: Object.freeze({
      major: desc.version.major,
      minor: desc.version.minor,
      patch: desc.version.patch,
    }),
    init: Object.freeze({
      ...(desc.init.priority !== undefined ? { priority: desc.init.priority } : {}),
      before: Object.freeze([...(desc.init.before ?? [])]),
      after: Object.freeze([...(desc.init.after ?? [])]),
    }),
  });

  const { init, start, stop, destroy, invoke } = impl;
  const plugin: RackPlugin = {
    desc: frozenDesc,
    init: (services: PluginServices) => init?.(services),
    start: () => start?.(),
    stop: () => stop?.(),
    ...(destroy ? { destroy: () => destroy?.() } : {}),
    ...(invoke ? { invoke: (request: InvokeRequest) => invoke?.(request) } : {}),
  };
  return plugin;
}
