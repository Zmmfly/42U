/**
 * @file Rack construction and lifecycle seam for the rack CLI.
 *
 * @brief Everything the CLI needs to reach a real rack without statically importing the host
 *        implementation: a lazy RackFactory (dynamic import of src/core/host.ts), the
 *        boot -> operate -> shutdown runner, and the in-process caller plugin that backs
 *        one-shot leased calls.
 *
 * @note src/core/host.ts is authored in parallel (a-host) and may not exist yet. It is only
 *       imported dynamically through a non-literal specifier, so tsc never requires the file
 *       to be present and the CLI reports a friendly error until createRack lands.
 */

import type {
  CapabilityService,
  CallService,
  PluginServices,
  Rack,
  RackOptions,
  RackPlugin,
  VersionRange,
} from '../../src/core/types.ts';
import { RackError } from '../../src/core/status.ts';
import { addStandardPlugins } from '../../src/plugins/index.ts';
import { errorMessage } from './output.ts';

/**
 * @brief Source of a Rack for one CLI invocation.
 *
 * @param dir Plugin directory selected with --dir; factories may ignore it.
 * @return A live rack, or a promise for one.
 * @note Injection seam: tests return fake racks implementing the frozen Rack interface; the
 *       default factory lazily imports the a-host implementation. Zero-argument functions are
 *       valid factories and ignore the directory.
 */
export type RackFactory = (dir?: string) => Promise<Rack> | Rack;

/** @brief Capability and call service views the CLI caller plugin captured from init(). */
export interface CallerServices {
  readonly caps: CapabilityService;
  readonly calls: CallService;
}

/** @brief plugId of the CLI's in-process helper instance; publishes no capabilities. */
export const CLI_CALLER_PLUG_ID = 'cli.caller';

/**
 * @brief Stage the CLI caller plugin on a rack before boot.
 *
 * @param rack Rack that has not booted yet.
 * @return Promise resolving with the services injected into the caller's init(); it resolves
 *         while boot() (or the explicit start() fallback) initializes staged instances.
 * @note The caller is a regular in-process plugin: it publishes nothing and subscribes to
 *       nothing, it merely keeps the injected caps/calls views so the CLI can borrow and
 *       invoke like any other consumer. The frozen Rack facade offers no service accessor,
 *       so staging is the only contract-faithful path to caps/calls.
 */
export function stageCliCaller(rack: Rack): Promise<CallerServices> {
  let ready: (services: CallerServices) => void = () => undefined;
  const services = new Promise<CallerServices>((resolve) => {
    ready = resolve;
  });
  rack.add(() => {
    const plugin: RackPlugin = {
      desc: {
        plugId: CLI_CALLER_PLUG_ID,
        version: { major: 1, minor: 0, patch: 0 },
        init: {},
      },
      init: (injected: PluginServices): void => {
        ready({ caps: injected.caps, calls: injected.calls });
      },
      start: (): void => undefined,
      stop: (): void => undefined,
    };
    return plugin;
  });
  return services;
}

/**
 * @brief Inclusive version range covering every legal plugin version: [0.0.0, MAX.MAX.MAX].
 *
 * @note The CLI accepts any provider version; the host still binds the lease to the actual
 *       disclosed version and to the current generation.
 */
export const ANY_VERSION_RANGE: VersionRange = {
  minimum: { major: 0, minor: 0, patch: 0 },
  maximum: {
    major: Number.MAX_SAFE_INTEGER,
    minor: Number.MAX_SAFE_INTEGER,
    patch: Number.MAX_SAFE_INTEGER,
  },
};

/**
 * @brief Shut a rack down, tolerating failures.
 *
 * @param rack Rack to shut down; shutdown() is attempted exactly once.
 * @param warn Optional warning sink used when shutdown() itself throws.
 * @note Best effort by design: a failed teardown must never mask the operation's result.
 */
export async function shutdownBestEffort(
  rack: Rack,
  warn?: (message: string) => void,
): Promise<void> {
  try {
    await rack.shutdown();
  } catch (error) {
    warn?.(`rack shutdown failed: ${errorMessage(error)}`);
  }
}

/**
 * @brief Run one operation against a freshly booted rack, then shut the rack down.
 *
 * @param factory Rack source; may throw or reject (host not ready).
 * @param fn Operation executed after boot(); its return value becomes the runner result.
 * @param warn Optional stderr sink for best-effort shutdown warnings.
 * @return Whatever fn returns.
 * @note boot() failures are rethrown after a quiet best-effort shutdown. When fn succeeds or
 *       throws, shutdown() still runs; its own failure is only reported through warn so the
 *       operation's result or error always surfaces unchanged.
 */
export async function runWithRack<T>(
  factory: RackFactory,
  fn: (rack: Rack) => Promise<T> | T,
  warn?: (message: string) => void,
): Promise<T> {
  const rack = await factory();
  try {
    await rack.boot();
  } catch (error) {
    await shutdownBestEffort(rack);
    throw error;
  }
  try {
    return await fn(rack);
  } finally {
    await shutdownBestEffort(rack, warn);
  }
}

/**
 * @brief Module specifier of the a-host implementation.
 *
 * @note Deliberately imported through a variable so the specifier is non-literal for tsc:
 *       TypeScript then types the dynamic import as any instead of requiring the module to
 *       exist. The path stays relative to this file (apps/cli/) and keeps its .ts extension.
 */
const HOST_MODULE_SPECIFIER = '../../src/core/host.ts';

/**
 * @brief Build the default rack factory backed by the real host implementation.
 *
 * @param options Optional factory behaviour switches.
 * @return A factory that dynamically imports src/core/host.ts on first use and hands the
 *         selected plugin directory to createRack via RackOptions.pluginDirectories.
 * @note Friendly RackError('failed') until the a-host module exists and exports createRack.
 * @note With options.withStd the factory stages every bundled std.* standard device via
 *       rack.add() before boot, so directory plugins may declare std.* init edges.
 */
export function createDefaultRackFactory(options?: { withStd?: boolean }): RackFactory {
  return async (dir?: string): Promise<Rack> => {
    let hostModule: { createRack?: unknown };
    try {
      hostModule = (await import(HOST_MODULE_SPECIFIER as string)) as { createRack?: unknown };
    } catch (error) {
      throw new RackError(
        'failed',
        `rack host implementation is not available yet ` +
          `(expected createRack in ts/src/core/host.ts): ${errorMessage(error)}`,
      );
    }
    const createRack = hostModule.createRack;
    if (typeof createRack !== 'function') {
      throw new RackError('failed', 'ts/src/core/host.ts loaded but exports no createRack function');
    }
    const rackOptions: RackOptions = dir === undefined ? {} : { pluginDirectories: [dir] };
    const rack = (createRack as (options?: RackOptions) => Rack)(rackOptions);
    if (options?.withStd === true) {
      addStandardPlugins(rack);
    }
    return rack;
  };
}
