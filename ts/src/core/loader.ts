/**
 * @file Plugin module loader: the TypeScript replacement for the C++ dlopen/dlclose layer.
 *
 * @brief Non-recursive directory discovery, dynamic ESM import with generation-based cache
 *        busting, module shape normalization and plugin descriptor validation.
 *
 * @note ESM semantics honestly replace the C++ shared-library mapping semantics:
 *       - Mapping: dynamic import() evaluates a plugin module once per URL, like dlopen().
 *       - Reload: the same file is imported under a distinct URL (`?u42gen=N`), which misses
 *         the process-wide ESM module cache and therefore re-evaluates the module - the
 *         equivalent of unmapping the old library and mapping a fresh copy of it.
 *       - Unmapping: JavaScript has no dlclose(). Once the host drops every reference to a
 *         module (namespace object, PluginModule wrapper, created instances), the garbage
 *         collector reclaims it; reclamation is eventual, not immediate. This is the
 *         documented semantic equivalent of the C++ RAII mapping release, not an identical
 *         guarantee: module top-level side effects that the module itself kept alive (timers,
 *         globalThis entries) outlive the host's unload protocol until collected. The host's
 *         explicit unload protocol therefore remains the authoritative release sequence.
 */

import type { Dirent } from 'node:fs';
import { readdir, stat } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { isRackError, RackError, type RackStatus } from './status.ts';
import type { JsonValue } from './json.ts';
import type {
  DiscoveredFile,
  PluginLoader,
  PluginModule,
  RackOptions,
  RackPlugin,
} from './types.ts';
import { isPluginVersion } from './version.ts';

/** @brief File suffixes recognized by discovery; every other directory entry is ignored. */
const PLUGIN_SUFFIXES: readonly string[] = ['.plug.ts', '.plug.js', '.plug.mjs'];

/** @brief Cache-busting URL query parameter carrying the host-private instance generation. */
const GENERATION_PARAM = 'u42gen';

/**
 * @brief Options of createPluginLoader().
 */
export interface PluginLoaderOptions {
  /** @brief Optional diagnostics sink; receives 'error' reports whenever a load() fails. */
  readonly onDiag?: RackOptions['onDiag'];
}

/**
 * @brief Create the pluggable physical layer of the rack: discovery plus module mapping.
 *
 * @param options Optional diagnostics sink receiving load failure reports.
 * @return A fresh PluginLoader instance.
 *
 * @note Each loader memoizes one PluginModule per (absolute path, generation) pair: repeated
 *       load() calls with the same pair return the same object without re-importing the file
 *       or re-invoking the plugin factory. Failed loads are not memoized; a retry re-imports.
 * @note The process-wide ESM module cache is intentionally shared: two loaders importing the
 *       same (path, generation) evaluate the module once but each run their own factory.
 */
export function createPluginLoader(options?: PluginLoaderOptions): PluginLoader {
  /** Completed and in-flight loads keyed by `${absolutePath}\0${generation}`. */
  const cache = new Map<string, Promise<PluginModule>>();

  /**
   * @brief Report a load failure through the optional sink and build the RackError for it.
   *
   * @param status Rack status describing the failure class.
   * @param message Human-readable diagnostic.
   * @param detail JSON-compatible structured context.
   *
   * @return The RackError to throw at the call site; callers must `throw` it.
   */
  const rackFailure = (status: RackStatus, message: string, detail?: JsonValue): RackError => {
    try {
      options?.onDiag?.('error', message, detail);
    } catch {
      // A broken diagnostics sink must never replace the real load failure.
    }
    return new RackError(status, message, detail);
  };

  /**
   * @brief Scan one directory for plugin module files.
   *
   * @param directory Directory to scan; relative paths resolve against the process cwd.
   *
   * @return Regular files whose name has a non-empty stem plus a `*.plug.ts` / `*.plug.js` /
   *         `*.plug.mjs` suffix, as absolute paths sorted in byte order. Non-recursive;
   *         directories, symlinks and other special entries never qualify (symlinks are not
   *         followed, mirroring the C++ regular-file rule).
   *
   * @throws RackError 'not_found' when the directory does not exist,
   *         'invalid_argument' when the path exists but is not a directory,
   *         'failed' when the directory cannot be read.
   */
  async function discover(directory: string): Promise<readonly DiscoveredFile[]> {
    let entries: Dirent[];
    try {
      entries = await readdir(directory, { withFileTypes: true });
    } catch (error) {
      const code = errnoOf(error);
      if (code === 'ENOENT') {
        throw new RackError('not_found', `plugin directory does not exist: ${directory}`, { directory });
      }
      if (code === 'ENOTDIR') {
        throw new RackError('invalid_argument', `plugin path is not a directory: ${directory}`, { directory });
      }
      throw new RackError('failed', `cannot read plugin directory: ${directory}`, {
        directory,
        errno: code ?? null,
      });
    }

    const found: DiscoveredFile[] = [];
    for (const entry of entries) {
      if (!entry.isFile() || !hasPluginSuffix(entry.name)) continue;
      // Absolute paths keep results stable for later load() calls regardless of host cwd.
      found.push({ path: resolve(directory, entry.name) });
    }
    // Deterministic byte-order sort; all candidates share one parent directory, so this is a
    // file-name ordering (UTF-16 code-unit order coincides with UTF-8 byte order for practical
    // plugin file names).
    found.sort((left, right) => (left.path < right.path ? -1 : left.path > right.path ? 1 : 0));
    return found;
  }

  /**
   * @brief Import and validate one plugin module file for the given instance generation.
   *
   * @param rawPath Module file path; relative paths resolve against the process cwd.
   * @param generation Host-private instance generation baked into the cache-busting URL.
   *
   * @return The validated PluginModule; memoized per (absolute path, generation).
   *
   * @throws RackError 'invalid_argument' for a malformed generation,
   *         'not_found' when the file is missing or not a regular file,
   *         'unsupported' when the module exports no compatible factory,
   *         'invalid_argument' when the produced plugin lacks a valid descriptor,
   *         'failed' when the module evaluation or the factory invocation throws.
   */
  async function load(rawPath: string, generation: number): Promise<PluginModule> {
    if (!Number.isSafeInteger(generation)) {
      throw new RackError('invalid_argument', `generation must be a safe integer: ${String(generation)}`);
    }
    const filePath = resolve(rawPath);
    const cacheKey = `${filePath}\u0000${generation}`;
    let pending = cache.get(cacheKey);
    if (pending === undefined) {
      pending = importModule(filePath, generation).catch((error: unknown) => {
        // Failures are not memoized: a retry re-imports and re-runs the factory.
        cache.delete(cacheKey);
        throw error;
      });
      cache.set(cacheKey, pending);
    }
    return pending;
  }

  /** @brief Import, normalize and validate one module; every failure reports through the sink. */
  async function importModule(filePath: string, generation: number): Promise<PluginModule> {
    // Step 1: the file must exist as a regular file before it is mapped.
    try {
      const info = await stat(filePath);
      if (!info.isFile()) {
        throw rackFailure('not_found', `plugin module path is not a regular file: ${filePath}`, {
          path: filePath,
          generation,
        });
      }
    } catch (error) {
      if (isRackError(error)) throw error; // already reported through the sink
      const code = errnoOf(error);
      if (code === 'ENOENT' || code === 'ENOTDIR') {
        throw rackFailure('not_found', `plugin module file not found: ${filePath}`, {
          path: filePath,
          generation,
        });
      }
      throw rackFailure('failed', `cannot inspect plugin module file: ${filePath}`, {
        path: filePath,
        generation,
        errno: code ?? null,
      });
    }

    // Step 2: map the module under a generation-qualified URL. A new generation misses the ESM
    // cache and re-evaluates the file: the reload equivalent of mapping a fresh library copy.
    const moduleUrl = `${pathToFileURL(filePath).href}?${GENERATION_PARAM}=${generation}`;
    let namespace: unknown;
    try {
      namespace = await import(/* @vite-ignore */ moduleUrl);
    } catch (error) {
      const cause = summarizeThrown(error);
      throw rackFailure('failed', `plugin module evaluation failed for ${filePath}: ${cause}`, {
        path: filePath,
        generation,
        cause,
      });
    }

    // Step 3: normalize the export shape: a default export wins over a named `create`.
    const record: Record<string, unknown> =
      typeof namespace === 'object' && namespace !== null ? (namespace as Record<string, unknown>) : {};
    const defaultExport = record['default'];
    const namedCreate = record['create'];
    const factory: (() => unknown) | undefined =
      typeof defaultExport === 'function'
        ? (defaultExport as () => unknown)
        : typeof namedCreate === 'function'
          ? (namedCreate as () => unknown)
          : undefined;
    if (factory === undefined) {
      throw rackFailure('unsupported', `plugin module ${filePath} exports no compatible factory`, {
        path: filePath,
        generation,
        expected: 'default export or named create() function',
      });
    }

    // Step 4: invoke the factory exactly once per load() and validate the produced plugin.
    let plugin: unknown;
    try {
      plugin = factory();
    } catch (error) {
      const cause = summarizeThrown(error);
      throw rackFailure('failed', `plugin factory create() threw for ${filePath}: ${cause}`, {
        path: filePath,
        generation,
        cause,
      });
    }
    if ((typeof plugin !== 'object' || plugin === null) && typeof plugin !== 'function') {
      throw rackFailure('invalid_argument', `plugin factory for ${filePath} returned no plugin object`, {
        path: filePath,
        generation,
      });
    }
    const problem = descriptorProblem((plugin as Record<string, unknown>)['desc']);
    if (problem !== undefined) {
      throw rackFailure('invalid_argument', `invalid plugin descriptor from ${filePath}: ${problem}`, {
        path: filePath,
        generation,
        problem,
      });
    }

    // Step 5: wrap with a cached create(). Design choice: the factory is invoked exactly once
    // per successful load() and its result is cached, so repeated create() calls return the
    // same instance and validation never re-triggers plugin construction.
    const instance = plugin as RackPlugin;
    return { create: () => instance };
  }

  return { discover, load };
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/**
 * @brief Check a file name for a plugin suffix behind a non-empty stem.
 * @param name File name taken from a directory enumeration.
 * @return true for names like `storage.plug.ts`; a bare `.plug.ts` has no stem and is rejected.
 */
function hasPluginSuffix(name: string): boolean {
  return PLUGIN_SUFFIXES.some((suffix) => name.length > suffix.length && name.endsWith(suffix));
}

/**
 * @brief Extract the errno code of a Node filesystem error.
 * @param value Anything caught from a filesystem call.
 * @return The error code string, or undefined for foreign thrown values.
 */
function errnoOf(value: unknown): string | undefined {
  if (typeof value === 'object' && value !== null) {
    const code = (value as { code?: unknown }).code;
    if (typeof code === 'string') return code;
  }
  return undefined;
}

/**
 * @brief Summarize an arbitrary thrown value for a RackError message.
 * @param value Anything caught in a catch clause.
 * @return `Name: message` for errors, a stringification otherwise.
 */
function summarizeThrown(value: unknown): string {
  if (value instanceof Error) return `${value.name}: ${value.message}`;
  try {
    return String(value);
  } catch {
    return '<unprintable thrown value>';
  }
}

/**
 * @brief Validate one candidate plugin descriptor against the frozen PluginDescriptor shape.
 *
 * @param value The `desc` member produced by a plugin factory, of unknown shape.
 *
 * @return undefined when the descriptor is valid; otherwise a short human-readable problem
 *         used verbatim in the RackError message and diagnostics detail.
 *
 * @note Follows the frozen InitOrderSpec: `init` may be missing entirely (the host fills the
 *       default); when present it must be a plain object whose optional `priority` is a safe
 *       integer and whose optional `before`/`after` are arrays of strings.
 */
function descriptorProblem(value: unknown): string | undefined {
  if ((typeof value !== 'object' || value === null) && typeof value !== 'function') {
    return 'desc is missing or not an object';
  }
  const desc = value as Record<string, unknown>;
  if (!isNonEmptyString(desc['plugId'])) return 'desc.plugId must be a non-empty string';
  if (!isPluginVersion(desc['version'])) return 'desc.version is not a valid plugin version';
  const init = desc['init'];
  if (init !== undefined) {
    if (typeof init !== 'object' || init === null || Array.isArray(init)) {
      return 'desc.init must be an object when present';
    }
    const spec = init as Record<string, unknown>;
    if (spec['priority'] !== undefined && !Number.isSafeInteger(spec['priority'])) {
      return 'desc.init.priority must be an integer';
    }
    if (spec['before'] !== undefined && !isStringArray(spec['before'])) {
      return 'desc.init.before must be an array of strings';
    }
    if (spec['after'] !== undefined && !isStringArray(spec['after'])) {
      return 'desc.init.after must be an array of strings';
    }
  }
  return undefined;
}

/** @brief Narrow to a non-empty string. */
function isNonEmptyString(value: unknown): value is string {
  return typeof value === 'string' && value.length > 0;
}

/** @brief Narrow to a plain array of strings (used for init constraint lists). */
function isStringArray(value: unknown): value is string[] {
  return Array.isArray(value) && value.every((item) => typeof item === 'string');
}
