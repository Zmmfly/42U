/**
 * @file Command parsing, dispatch and handlers for the rack CLI.
 *
 * @brief The whole command surface (version/list/describe/call/unload/reload/poll/serve) plus
 *        exit-code mapping: 0 success, 1 runtime error, 2 usage error. Every rack-touching
 *        command runs boot -> operate -> shutdown through runWithRack, except serve which
 *        keeps the rack alive until the keep-alive wiring stops it.
 *
 * @note Usage errors print the help text to stderr; runtime failures print RackError
 *       status + message (JSON envelope under --json). runCli never calls process.exit: the
 *       entry point applies the returned code.
 */

import { parseArgs } from 'node:util';

import type { JsonValue, LeaseHandle, Rack, Revoker } from '../../src/core/types.ts';
import { RACK_CONTRACT_VERSION } from '../../src/core/types.ts';
import { isRackError, RackError, statusOf } from '../../src/core/status.ts';
import { formatVersion } from '../../src/core/version.ts';
import {
  ANY_VERSION_RANGE,
  type CallerServices,
  type RackFactory,
  runWithRack,
  shutdownBestEffort,
  stageCliCaller,
} from './rack-access.ts';
import {
  asJsonValue,
  emitJson,
  emitPrettyJson,
  errorMessage,
  type OutputPorts,
  renderInstanceTable,
} from './output.ts';

/** @brief CLI version reported by `version`; independent from the rack contract version. */
export const CLI_VERSION = '0.1.0';

/** @brief Plugin directory used when --dir is absent; relative to the working directory. */
export const DEFAULT_PLUGIN_DIR = 'examples/plugins';

/** @brief Heartbeat interval keeping the `serve` event loop alive (milliseconds). */
const HEARTBEAT_INTERVAL_MS = 30_000;

/** @brief Full help text printed by `help` and on every usage error. */
export const HELP_TEXT = `Usage: node apps/rack-cli.ts <command> [args] [--json] [--dir <directory>] [--with-std]

Commands:
  version                            Print CLI and rack contract versions
  list                               Boot, list loaded plugins, shut down
  describe <plugId>                  Print the InstanceInfo JSON of one plugin
  call <plugId> <method> <jsonArgs>  One-shot leased call through the gateway
  unload <plugId>                    Unload one plugin instance
  reload <plugId>                    Reload one plugin instance (new generation)
  poll                               Drain queued events and deferred unloads
  serve                              Boot and keep the rack running until Ctrl+C
  help                               Show this help

Options:
  --json        Machine-readable single-line JSON output (all commands)
  --dir <dir>   Plugin directory (default: ${DEFAULT_PLUGIN_DIR})
  --with-std    Also stage the bundled std.* standard devices before boot

Exit codes: 0 success, 1 runtime error, 2 usage error.`;

/**
 * @brief Internal marker for exit-code-2 invocation errors.
 *
 * @note Carries only a message; the dispatcher prints it plus the help text to stderr.
 */
class UsageError extends Error {}

/** @brief One parsed per-command invocation. */
interface ParsedInvocation {
  readonly command: string;
  readonly positionals: readonly string[];
  readonly json: boolean;
  readonly dir: string;
  readonly withStd: boolean;
}

/** @brief Options accepted by every command.
 *  @note Keys must be kebab-case: Node parseArgs matches the literal de-dashed flag name
 *        ('with-std'), not the camelCase spelling.
 */
const CLI_OPTIONS = {
  json: { type: 'boolean' },
  dir: { type: 'string' },
  'with-std': { type: 'boolean' },
} as const;

/**
 * @brief Parse the arguments following the command word.
 *
 * @param command Command word, used to prefix error messages.
 * @param argv Arguments without the command word.
 * @return Positional arguments plus the --json flag and the resolved --dir value.
 * @throws UsageError when parseArgs rejects the invocation (unknown option, missing value...).
 */
function parseInvocation(command: string, argv: readonly string[]): ParsedInvocation {
  const parsed = (() => {
    try {
      return parseArgs({
        args: [...argv],
        options: CLI_OPTIONS,
        allowPositionals: true,
        strict: true,
      });
    } catch (error) {
      throw new UsageError(`${command}: ${errorMessage(error)}`);
    }
  })();
  return {
    command,
    positionals: parsed.positionals,
    json: parsed.values.json === true,
    dir: parsed.values.dir ?? DEFAULT_PLUGIN_DIR,
    withStd: parsed.values['with-std'] === true,
  };
}

/**
 * @brief Require an exact number of positional arguments.
 *
 * @param command Command word, used to prefix error messages.
 * @param positionals Positional arguments collected by parseInvocation.
 * @param count Expected count.
 * @throws UsageError when the counts differ.
 */
function expectPositionalCount(
  command: string,
  positionals: readonly string[],
  count: number,
): void {
  if (positionals.length !== count) {
    throw new UsageError(
      `${command}: expected ${count} positional argument(s), got ${positionals.length}`,
    );
  }
}

/**
 * @brief Install the keep-alive wiring for `serve`.
 *
 * @param onStop Exactly-once callback to invoke when the operator requests shutdown.
 * @note Injection seam: the default installs a heartbeat timer plus SIGINT/SIGTERM handlers;
 *       tests install an immediate trigger instead of real signals and timers.
 */
export type KeepAlive = (onStop: () => void) => void;

/** @brief Runtime context every command runs against. */
export interface CliContext extends OutputPorts {
  /** @brief Rack source; never invoked by `version` and `help`. */
  readonly rackFactory: RackFactory;
  /** @brief Keep-alive wiring for `serve`; defaults to nodeKeepAlive. */
  readonly keepAlive?: KeepAlive;
}

/**
 * @brief Default keep-alive wiring: a silent heartbeat plus graceful signal handling.
 *
 * @param onStop Invoked exactly once on the first signal; handlers are removed first so a
 *        second Ctrl+C falls back to Node's default termination.
 */
export function nodeKeepAlive(onStop: () => void): void {
  const heartbeat = setInterval(() => undefined, HEARTBEAT_INTERVAL_MS);
  const stop = (): void => {
    clearInterval(heartbeat);
    process.off('SIGINT', stop);
    process.off('SIGTERM', stop);
    onStop();
  };
  process.on('SIGINT', stop);
  process.on('SIGTERM', stop);
}

/** @brief stderr warning sink used for best-effort shutdown and lease notes. */
function warnSink(ctx: CliContext): (message: string) => void {
  return (message: string) => ctx.err(`rack-cli: warning: ${message}`);
}

/** @brief Build a directory-bound rack factory around the context factory. */
function boundFactory(ctx: CliContext, dir: string): RackFactory {
  return () => ctx.rackFactory(dir);
}

/**
 * @brief Render a runtime failure to stderr in the requested shape.
 *
 * @param ctx Output ports.
 * @param command Command word for the JSON error envelope.
 * @param error Thrown value; RackError statuses are surfaced verbatim, foreign errors map to
 *        'failed'.
 * @param json Whether stderr should carry a machine-readable envelope instead of a text line.
 */
function reportError(ctx: CliContext, command: string, error: unknown, json: boolean): void {
  const status = statusOf(error);
  const message = isRackError(error)
    ? error.message.replace(/^\[[^\]]+\]\s*/, '')
    : errorMessage(error);
  if (json) {
    ctx.err(JSON.stringify({ command, error: { status, message } }));
  } else {
    ctx.err(`rack-cli: error [${status}] ${message}`);
  }
}

/**
 * @brief `version`: print CLI and rack contract versions; no rack is constructed.
 *
 * @param ctx Output ports.
 * @param invocation Parsed invocation (only the --json flag is used).
 * @return Exit code 0.
 */
function runVersion(ctx: CliContext, invocation: ParsedInvocation): number {
  if (invocation.json) {
    emitJson(ctx, { cliVersion: CLI_VERSION, contractVersion: RACK_CONTRACT_VERSION });
  } else {
    ctx.out(`rack-cli ${CLI_VERSION}`);
    ctx.out(`rack contract version ${RACK_CONTRACT_VERSION}`);
  }
  return 0;
}

/**
 * @brief `help`: print the help text to stdout.
 *
 * @param ctx Output ports.
 * @return Exit code 0.
 */
function runHelp(ctx: CliContext): number {
  for (const line of HELP_TEXT.split('\n')) {
    ctx.out(line);
  }
  return 0;
}

/**
 * @brief `list`: boot, print the plugin snapshot as a table (or JSON array), shut down.
 *
 * @param ctx Output ports and rack factory.
 * @param invocation Parsed invocation; --dir selects the plugin directory.
 * @return Exit code.
 */
async function runList(ctx: CliContext, invocation: ParsedInvocation): Promise<number> {
  return runWithRack(
    boundFactory(ctx, invocation.dir),
    (rack) => {
      const instances = rack.plugins();
      if (invocation.json) {
        emitJson(ctx, asJsonValue(instances));
      } else if (instances.length === 0) {
        ctx.out(`no plugins loaded from ${invocation.dir}`);
      } else {
        for (const line of renderInstanceTable(instances)) {
          ctx.out(line);
        }
      }
      return 0;
    },
    warnSink(ctx),
  );
}

/**
 * @brief `describe`: boot, print the InstanceInfo of one plugin as JSON, shut down.
 *
 * @param ctx Output ports and rack factory.
 * @param invocation Parsed invocation with exactly one positional (plugId).
 * @return Exit code.
 */
async function runDescribe(ctx: CliContext, invocation: ParsedInvocation): Promise<number> {
  expectPositionalCount('describe', invocation.positionals, 1);
  const plugId = invocation.positionals[0] ?? '';
  return runWithRack(
    boundFactory(ctx, invocation.dir),
    (rack) => {
      const info = rack.describe(plugId);
      if (info === undefined) {
        throw new RackError('not_found', `unknown plug id '${plugId}'`);
      }
      if (invocation.json) {
        emitJson(ctx, asJsonValue(info));
      } else {
        emitPrettyJson(ctx, asJsonValue(info));
      }
      return 0;
    },
    warnSink(ctx),
  );
}

/**
 * @brief Resolve the CLI caller services after boot, with an explicit-start fallback.
 *
 * @param rack The booted rack carrying the staged CLI caller plugin.
 * @param staged Services promise returned by stageCliCaller().
 * @return The captured capability/call service views.
 * @note Hosts whose boot() already initializes staged in-process plugins resolve immediately.
 *       Hosts that leave staged instances for an explicit start() get exactly that fallback.
 *       A caller that never appears is a host bug and becomes a runtime error.
 */
async function startedCallerServices(
  rack: Rack,
  staged: Promise<CallerServices> | null,
): Promise<CallerServices> {
  if (staged === null) {
    throw new RackError('failed', 'CLI caller plugin was not staged before boot');
  }
  const immediate = await Promise.race([staged, Promise.resolve(null)]);
  if (immediate !== null) {
    return immediate;
  }
  await rack.start();
  return staged;
}

/**
 * @brief Borrow once, invoke once by name, and always return the lease.
 *
 * @param services Capability/call views captured by the CLI caller plugin.
 * @param plugId Provider identity to borrow.
 * @param methodName Method name to invoke through the gateway.
 * @param args JSON argument document forwarded verbatim.
 * @param warn Warning sink for busy/revoked lease returns.
 * @return The provider's JSON result.
 * @note The lease is acquired for the full range [0.0.0, MAX] and released in a finally
 *       block, so failed calls never leak a credential. Revocation between acquire and
 *       release returns the credential from onRevoke as the protocol requires.
 */
async function oneShotCall(
  services: CallerServices,
  plugId: string,
  methodName: string,
  args: JsonValue,
  warn: (message: string) => void,
): Promise<JsonValue> {
  let revoked = false;
  const revoker: Revoker = {
    onRevoke: (credential) => {
      revoked = true;
      // Best-effort immediate return; the finally below stays as the safety net.
      services.caps.release(credential);
    },
  };
  let lease: LeaseHandle | null = null;
  try {
    lease = services.caps.acquire(plugId, ANY_VERSION_RANGE, revoker);
    return await services.calls.call(lease.credential, methodName, args);
  } finally {
    if (lease !== null) {
      const returned = await lease.release();
      if (returned === 'busy') {
        warn(`lease for '${plugId}' returned busy; the lease stays outstanding`);
      } else if (returned === 'ok' && revoked) {
        warn(`lease for '${plugId}' was revoked during the call`);
      }
    }
  }
}

/**
 * @brief `call`: boot with a staged CLI caller, perform one leased invoke, print the JSON
 *        result, release the lease and shut down.
 *
 * @param ctx Output ports and rack factory.
 * @param invocation Parsed invocation with exactly three positionals
 *        (plugId, methodName, jsonArgs).
 * @return Exit code.
 * @throws UsageError when jsonArgs is not valid JSON.
 */
async function runCall(ctx: CliContext, invocation: ParsedInvocation): Promise<number> {
  expectPositionalCount('call', invocation.positionals, 3);
  const plugId = invocation.positionals[0] ?? '';
  const methodName = invocation.positionals[1] ?? '';
  const rawArgs = invocation.positionals[2] ?? '';
  let args: JsonValue;
  try {
    args = JSON.parse(rawArgs) as JsonValue;
  } catch (error) {
    throw new UsageError(`call: <jsonArgs> must be valid JSON: ${errorMessage(error)}`);
  }

  let staged: Promise<CallerServices> | null = null;
  const factory: RackFactory = async (dir?: string) => {
    const rack = await ctx.rackFactory(dir ?? invocation.dir);
    staged = stageCliCaller(rack);
    return rack;
  };

  return runWithRack(
    factory,
    async (rack) => {
      const services = await startedCallerServices(rack, staged);
      const warn = warnSink(ctx);
      const result = await oneShotCall(services, plugId, methodName, args, warn);
      if (invocation.json) {
        emitJson(ctx, asJsonValue(result));
      } else {
        emitPrettyJson(ctx, asJsonValue(result));
      }
      return 0;
    },
    warnSink(ctx),
  );
}

/**
 * @brief `unload`: boot, run the full unload protocol for one plugin, shut down.
 *
 * @param ctx Output ports and rack factory.
 * @param invocation Parsed invocation with exactly one positional (plugId).
 * @return Exit code.
 */
async function runUnload(ctx: CliContext, invocation: ParsedInvocation): Promise<number> {
  expectPositionalCount('unload', invocation.positionals, 1);
  const plugId = invocation.positionals[0] ?? '';
  return runWithRack(
    boundFactory(ctx, invocation.dir),
    async (rack) => {
      await rack.unload(plugId);
      if (invocation.json) {
        emitJson(ctx, { command: 'unload', plugId, ok: true });
      } else {
        ctx.out(`unloaded ${plugId}`);
      }
      return 0;
    },
    warnSink(ctx),
  );
}

/**
 * @brief `reload`: boot, unload and freshly load one plugin (generation bumps), shut down.
 *
 * @param ctx Output ports and rack factory.
 * @param invocation Parsed invocation with exactly one positional (plugId).
 * @return Exit code.
 */
async function runReload(ctx: CliContext, invocation: ParsedInvocation): Promise<number> {
  expectPositionalCount('reload', invocation.positionals, 1);
  const plugId = invocation.positionals[0] ?? '';
  return runWithRack(
    boundFactory(ctx, invocation.dir),
    async (rack) => {
      await rack.reload(plugId);
      if (invocation.json) {
        emitJson(ctx, { command: 'reload', plugId, ok: true });
      } else {
        ctx.out(`reloaded ${plugId}`);
      }
      return 0;
    },
    warnSink(ctx),
  );
}

/**
 * @brief `poll`: boot, drain queued events and deferred unloads, shut down.
 *
 * @param ctx Output ports and rack factory.
 * @param invocation Parsed invocation (positionals are rejected).
 * @return Exit code.
 */
async function runPoll(ctx: CliContext, invocation: ParsedInvocation): Promise<number> {
  expectPositionalCount('poll', invocation.positionals, 0);
  return runWithRack(
    boundFactory(ctx, invocation.dir),
    async (rack) => {
      await rack.poll();
      if (invocation.json) {
        emitJson(ctx, { command: 'poll', ok: true });
      } else {
        ctx.out('poll complete');
      }
      return 0;
    },
    warnSink(ctx),
  );
}

/**
 * @brief `serve`: boot and keep the rack alive until the keep-alive wiring stops it.
 *
 * @param ctx Output ports, rack factory and keep-alive wiring.
 * @param invocation Parsed invocation; --dir selects the plugin directory.
 * @return Exit code.
 * @note HTTP/RPC front-ends are composed on top in a later integration stage; serve only
 *       keeps the booted rack resident. Under --json it prints one running document at
 *       startup and one stopped document after graceful shutdown.
 */
async function runServe(ctx: CliContext, invocation: ParsedInvocation): Promise<number> {
  const rack = await ctx.rackFactory(invocation.dir);
  try {
    await rack.boot();
  } catch (error) {
    await shutdownBestEffort(rack);
    throw error;
  }
  const instances = rack.plugins();
  if (invocation.json) {
    emitJson(ctx, {
      command: 'serve',
      status: 'running',
      dir: invocation.dir,
      plugins: asJsonValue(instances),
    });
  } else {
    ctx.out(`rack is serving plugins from ${invocation.dir} (press Ctrl+C to stop)`);
    for (const info of instances) {
      ctx.out(
        `  ${info.plugId} ${formatVersion(info.version)} ${info.state} (generation ${info.generation})`,
      );
    }
    if (instances.length === 0) {
      ctx.out('  (no plugins loaded)');
    }
  }

  const keepAlive = ctx.keepAlive ?? nodeKeepAlive;
  await new Promise<void>((resolve) => {
    keepAlive(() => {
      void (async () => {
        await shutdownBestEffort(rack, warnSink(ctx));
        if (invocation.json) {
          emitJson(ctx, { command: 'serve', status: 'stopped' });
        } else {
          ctx.out('rack shut down');
        }
        resolve();
      })();
    });
  });
  return 0;
}

/**
 * @brief Execute one CLI invocation and report its exit code.
 *
 * @param argv Arguments without the executable and script path (process.argv.slice(2)).
 * @param ctx Output ports, rack factory and optional keep-alive wiring.
 * @return 0 success, 1 runtime error, 2 usage error.
 * @note Never calls process.exit and never installs signal handlers outside serve's injected
 *       keep-alive; the entry point decides how to apply the returned code.
 */
export async function runCli(argv: readonly string[], ctx: CliContext): Promise<number> {
  const command = argv[0];
  const rest = argv.slice(1);
  try {
    switch (command) {
      case 'version':
      case '-v':
      case '--version':
        return runVersion(ctx, parseInvocation('version', rest));
      case 'help':
      case '-h':
      case '--help':
        parseInvocation('help', rest);
        return runHelp(ctx);
      case 'list':
        return await runList(ctx, parseInvocation('list', rest));
      case 'describe':
        return await runDescribe(ctx, parseInvocation('describe', rest));
      case 'call':
        return await runCall(ctx, parseInvocation('call', rest));
      case 'unload':
        return await runUnload(ctx, parseInvocation('unload', rest));
      case 'reload':
        return await runReload(ctx, parseInvocation('reload', rest));
      case 'poll':
        return await runPoll(ctx, parseInvocation('poll', rest));
      case 'serve':
        return await runServe(ctx, parseInvocation('serve', rest));
      case undefined:
        throw new UsageError('missing command');
      default:
        throw new UsageError(`unknown command '${command}'`);
    }
  } catch (error) {
    if (error instanceof UsageError) {
      ctx.err(`rack-cli: ${error.message}`);
      for (const line of HELP_TEXT.split('\n')) {
        ctx.err(line);
      }
      return 2;
    }
    reportError(ctx, command ?? 'cli', error, argv.includes('--json'));
    return 1;
  }
}
