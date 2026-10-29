/**
 * @file index.ts
 * @brief Public entry point of the 42U rack (TS edition).
 *
 * @details One import target for embedders and tooling: the rack host (createRack and the
 *          Rack-related types), the frozen status vocabulary (RackError/RackStatus), the
 *          version utilities, the JSON data plane, the plugin SDK (definePlugin/typedApi) and
 *          the standard plugin catalog. Plugin authors should prefer src/sdk/index.ts, which
 *          never pulls in a host implementation; this module is the embedder-facing face.
 */

// ---------------------------------------------------------------------------
// Host: the rack facade plus its diagnostics view.
// ---------------------------------------------------------------------------

export { createRack } from './core/host.ts';
export type { RackDiagRecord, RackWithDiagnostics } from './core/host.ts';

// Host-facing types from the frozen contract.
export type {
  CapEntry,
  CapEvent,
  CapSink,
  CapsDesc,
  CallService,
  CapabilityService,
  DiagLevel,
  DiagService,
  DiscoveredFile,
  EventService,
  EventSink,
  InitOrderSpec,
  InstanceInfo,
  InstanceState,
  InvokeRequest,
  LeaseCredential,
  LeaseHandle,
  MethodDesc,
  PluginDescriptor,
  PluginModule,
  PluginServices,
  PluginVersion,
  Rack,
  RackEvent,
  RackOptions,
  RackPlugin,
  Revoker,
  Subscription,
  VersionRange,
} from './core/types.ts';
export { RACK_CONTRACT_VERSION } from './core/types.ts';

// ---------------------------------------------------------------------------
// Status vocabulary and version utilities.
// ---------------------------------------------------------------------------

export { RackError, isRackError, statusOf } from './core/status.ts';
export type { RackStatus } from './core/status.ts';

export {
  acceptsVersion,
  assertPluginVersion,
  exactVersion,
  formatVersion,
  isPluginVersion,
  validVersionRange,
  versionEquals,
  versionLess,
} from './core/version.ts';

// ---------------------------------------------------------------------------
// JSON data plane.
// ---------------------------------------------------------------------------

export type { JsonArray, JsonObject, JsonValue } from './core/json.ts';
export { estimateJsonBytes, isJsonValue } from './core/json.ts';

// ---------------------------------------------------------------------------
// Plugin SDK (also available standalone from src/sdk/index.ts).
// ---------------------------------------------------------------------------

export { definePlugin, typedApi } from './sdk/index.ts';
export type { PluginHooks } from './sdk/index.ts';

// ---------------------------------------------------------------------------
// Standard plugin catalog (std.* devices).
// ---------------------------------------------------------------------------

export { addStandardPlugins, standardPlugins } from './plugins/index.ts';
export type { StandardPluginSpec } from './plugins/index.ts';
