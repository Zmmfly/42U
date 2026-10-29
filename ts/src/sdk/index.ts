/**
 * @file Public SDK surface for 42U plugin authors.
 *
 * @brief Single import target for plugins: the definePlugin() helper plus the plugin-facing
 *        subset of the frozen contracts (rack types, status errors, version arithmetic and the
 *        JSON data plane).
 *
 * @note Example plugins import only this module and never a host implementation, mirroring the
 *       C++ examples that link nothing but the frozen ABI.
 */

export { definePlugin } from './define-plugin.ts';
export type { PluginHooks } from './define-plugin.ts';
export { typedApi } from './typed-api.ts';

// Frozen contract re-exports: the public face plugin authors may depend on.
export { RACK_CONTRACT_VERSION } from '../core/types.ts';
export type {
  CallService,
  CapabilityService,
  CapEntry,
  CapEvent,
  CapSink,
  CapsDesc,
  DiagLevel,
  DiagService,
  EventService,
  EventSink,
  InitOrderSpec,
  InvokeRequest,
  LeaseCredential,
  LeaseHandle,
  MethodDesc,
  PluginDescriptor,
  PluginModule,
  PluginServices,
  PluginVersion,
  RackEvent,
  RackPlugin,
  Revoker,
  Subscription,
  VersionRange,
} from '../core/types.ts';

export { RackError, isRackError, statusOf } from '../core/status.ts';
export type { RackStatus } from '../core/status.ts';

export {
  acceptsVersion,
  assertPluginVersion,
  exactVersion,
  formatVersion,
  isPluginVersion,
  validVersionRange,
  versionEquals,
  versionLess,
} from '../core/version.ts';

export type { JsonArray, JsonObject, JsonValue } from '../core/json.ts';
export { estimateJsonBytes, isJsonValue } from '../core/json.ts';
