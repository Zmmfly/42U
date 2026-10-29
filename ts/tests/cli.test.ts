/**
 * @file Tests for the rack CLI (apps/rack-cli.ts and apps/cli/*).
 *
 * @brief Exercises every command through runCli() against an injected fake Rack that
 *        implements the frozen Rack interface and records the full call sequence. No host
 *        implementation is required: the default dynamic-import factory is never invoked
 *        here, and serve runs through an immediately-stopping keep-alive double.
 */

// The entry module itself must stay inert when imported by a test runner.
import '../apps/rack-cli.ts';

import { describe, expect, it } from 'vitest';
import { runCli, type CliContext } from '../apps/cli/commands.ts';
import { RACK_CONTRACT_VERSION } from '../src/core/types.ts';
import type {
  CapEvent,
  CapSink,
  CapabilityService,
  CallService,
  DiagService,
  EventService,
  EventSink,
  InstanceInfo,
  InstanceState,
  LeaseCredential,
  LeaseHandle,
  PluginServices,
  Rack,
  RackPlugin,
  Revoker,
  Subscription,
  VersionRange,
} from '../src/core/types.ts';
import type { JsonValue } from '../src/core/json.ts';
import { RackError } from '../src/core/status.ts';

// ---------------------------------------------------------------------------
// Fake rack plumbing
// ---------------------------------------------------------------------------

/** @brief Opaque credential factory; the handle payload stays unique per lease. */
let credentialSequence = 0;

/** @brief Capability service double recording acquisitions and returns in rack order. */
class FakeCaps implements CapabilityService {
  readonly acquires: { plugId: string; range: VersionRange }[] = [];
  readonly releases: string[] = [];
  leaseVersion = { major: 1, minor: 2, patch: 0 } as const;
  acquireError: RackError | null = null;
  private readonly log: string[];

  constructor(log: string[]) {
    this.log = log;
  }

  publish(): void {
    throw new RackError('invalid_state', 'the CLI caller publishes nothing');
  }

  watch(_sink: CapSink): Subscription {
    throw new RackError('invalid_state', 'the CLI caller watches nothing');
  }

  snapshot(): readonly CapEvent[] {
    return [];
  }

  acquire(plugId: string, range: VersionRange, _revoker: Revoker): LeaseHandle {
    if (this.acquireError !== null) {
      throw this.acquireError;
    }
    this.acquires.push({ plugId, range });
    this.log.push(`acquire:${plugId}`);
    const credential: LeaseCredential = {
      kind: 'u42.lease.credential',
      handle: { sequence: ++credentialSequence },
    };
    return {
      credential,
      plugId,
      version: this.leaseVersion,
      release: async (): Promise<'ok' | 'busy' | 'stale'> => {
        this.releases.push(plugId);
        this.log.push(`release:${plugId}`);
        return 'ok';
      },
    };
  }

  release(_credential: LeaseCredential): 'ok' | 'busy' | 'stale' {
    return 'ok';
  }
}

/** @brief Call service double recording gateway invocations in rack order. */
class FakeCalls implements CallService {
  readonly invocations: { methodName: string; args: JsonValue }[] = [];
  result: JsonValue = { echoed: true, input: null };
  error: RackError | null = null;
  private readonly log: string[];

  constructor(log: string[]) {
    this.log = log;
  }

  async call(
    _credential: LeaseCredential,
    methodName: string,
    args: JsonValue,
  ): Promise<JsonValue> {
    // Record the attempt first: a throwing provider was still invoked through the gateway.
    this.invocations.push({ methodName, args });
    this.log.push(`call:${methodName}`);
    if (this.error !== null) {
      throw this.error;
    }
    return this.result;
  }

  async callById(): Promise<JsonValue> {
    throw new Error('callById is not exercised by the CLI');
  }
}

/** @brief Configurable per-operation failures injected into the fake rack. */
type Failures = Partial<Record<'boot' | 'shutdown' | 'unload' | 'reload' | 'poll', Error>>;

/** @brief Options shaping one fake rack. */
interface FakeRackOptions {
  readonly instances?: readonly InstanceInfo[];
  readonly failures?: Failures;
  /** @brief Simulate a host whose boot() leaves staged plugins for an explicit start(). */
  readonly deferStagedStart?: boolean;
}

const noopSubscription: Subscription = { unsubscribe: () => undefined };
const noopEvents: EventService = {
  subscribe: (_name: string, _sink: EventSink): Subscription => noopSubscription,
  publish: () => undefined,
};
const noopDiag: DiagService = { report: () => undefined };

/** @brief Fake Rack implementing the frozen interface and recording every operation. */
class FakeRack implements Rack {
  readonly log: string[] = [];
  readonly caps: FakeCaps;
  readonly calls: FakeCalls;
  private readonly instances: readonly InstanceInfo[];
  private readonly failures: Failures;
  private readonly deferStagedStart: boolean;
  private staged: Array<() => RackPlugin> = [];
  private active: RackPlugin[] = [];

  constructor(options: FakeRackOptions = {}) {
    this.caps = new FakeCaps(this.log);
    this.calls = new FakeCalls(this.log);
    this.instances = options.instances ?? [];
    this.failures = options.failures ?? {};
    this.deferStagedStart = options.deferStagedStart ?? false;
  }

  private maybeFail(op: keyof Failures): void {
    const error = this.failures[op];
    if (error !== undefined) {
      throw error;
    }
  }

  private servicesFor(plugId: string): PluginServices {
    return { plugId, events: noopEvents, caps: this.caps, calls: this.calls, diag: noopDiag };
  }

  private async runPending(): Promise<void> {
    const pending = this.staged.splice(0);
    for (const create of pending) {
      const plugin = create();
      this.active.push(plugin);
      await plugin.init(this.servicesFor(plugin.desc.plugId));
    }
    for (const plugin of this.active) {
      await plugin.start();
    }
  }

  async boot(): Promise<void> {
    this.log.push('boot');
    this.maybeFail('boot');
    if (!this.deferStagedStart) {
      await this.runPending();
    }
  }

  add(create: () => RackPlugin): void {
    this.log.push('add');
    this.staged.push(create);
  }

  async start(): Promise<void> {
    this.log.push('start');
    await this.runPending();
  }

  async load(path: string): Promise<void> {
    this.log.push(`load:${path}`);
  }

  async unload(plugId: string): Promise<void> {
    this.log.push(`unload:${plugId}`);
    this.maybeFail('unload');
  }

  async reload(plugId: string): Promise<void> {
    this.log.push(`reload:${plugId}`);
    this.maybeFail('reload');
  }

  async poll(): Promise<void> {
    this.log.push('poll');
    this.maybeFail('poll');
  }

  async shutdown(): Promise<void> {
    this.log.push('shutdown');
    this.maybeFail('shutdown');
    for (const plugin of this.active) {
      await plugin.stop();
    }
  }

  plugins(): readonly InstanceInfo[] {
    return this.instances;
  }

  describe(plugId: string): InstanceInfo | undefined {
    return this.instances.find((info) => info.plugId === plugId);
  }
}

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

/** @brief One CLI run target: fake rack plus captured stdout/stderr. */
interface Harness {
  readonly rack: FakeRack;
  readonly stdout: string[];
  readonly stderr: string[];
  run(argv: readonly string[]): Promise<number>;
}

/** @brief InstanceInfo fixture with a stable shape. */
const info = (
  plugId: string,
  state: InstanceState = 'active',
  generation = 1,
): InstanceInfo => ({
  plugId,
  version: { major: 1, minor: 2, patch: 0 },
  state,
  generation,
  sourcePath: `examples/plugins/${plugId}.plug.ts`,
});

/** @brief Build a harness whose keep-alive stops serve immediately (simulated SIGINT). */
function makeHarness(options: FakeRackOptions = {}): Harness {
  const rack = new FakeRack(options);
  const stdout: string[] = [];
  const stderr: string[] = [];
  const ctx: CliContext = {
    out: (line) => stdout.push(line),
    err: (line) => stderr.push(line),
    rackFactory: () => rack,
    keepAlive: (onStop) => onStop(),
  };
  return { rack, stdout, stderr, run: (argv) => runCli(argv, ctx) };
}

/** @brief CLI context capturing a thrown factory failure instead of a rack. */
function failingFactoryContext(stdout: string[], stderr: string[], error: Error): CliContext {
  return {
    out: (line) => stdout.push(line),
    err: (line) => stderr.push(line),
    rackFactory: () => {
      throw error;
    },
  };
}

// ---------------------------------------------------------------------------
// version / help
// ---------------------------------------------------------------------------

describe('rack-cli version', () => {
  it('prints CLI and contract versions without touching the rack factory', async () => {
    const stdout: string[] = [];
    const code = await runCli(['version'], failingFactoryContext(stdout, [], new Error('boom')));
    expect(code).toBe(0);
    expect(stdout.join('\n')).toContain('0.1.0');
    expect(stdout.join('\n')).toContain(String(RACK_CONTRACT_VERSION));
  });

  it('emits machine-readable JSON under --json', async () => {
    const harness = makeHarness();
    const code = await harness.run(['version', '--json']);
    expect(code).toBe(0);
    expect(harness.rack.log).toEqual([]);
    expect(JSON.parse(harness.stdout[0] ?? '')).toEqual({
      cliVersion: '0.1.0',
      contractVersion: RACK_CONTRACT_VERSION,
    });
  });
});

describe('rack-cli help', () => {
  it('prints the help text to stdout with exit code 0', async () => {
    const harness = makeHarness();
    const code = await harness.run(['help']);
    expect(code).toBe(0);
    expect(harness.stdout.join('\n')).toContain('Usage: node apps/rack-cli.ts');
    expect(harness.stderr).toEqual([]);
  });
});

// ---------------------------------------------------------------------------
// list / describe
// ---------------------------------------------------------------------------

describe('rack-cli list', () => {
  it('boots, prints a plugId/version/state/generation table and shuts down', async () => {
    const harness = makeHarness({
      instances: [info('example.storage'), info('example.consumer', 'stopped', 2)],
    });
    const code = await harness.run(['list']);
    expect(code).toBe(0);
    const text = harness.stdout.join('\n');
    expect(text).toContain('PLUG ID');
    expect(text).toContain('VERSION');
    expect(text).toContain('STATE');
    expect(text).toContain('GENERATION');
    expect(text).toContain('example.storage');
    expect(text).toContain('1.2.0');
    expect(text).toContain('stopped');
    expect(text).toContain('2');
    expect(harness.rack.log).toEqual(['boot', 'shutdown']);
  });

  it('prints an empty-plugin notice when nothing loaded', async () => {
    const harness = makeHarness();
    const code = await harness.run(['list']);
    expect(code).toBe(0);
    expect(harness.stdout.join('\n')).toContain('no plugins loaded');
  });

  it('emits a JSON array under --json', async () => {
    const harness = makeHarness({ instances: [info('example.storage'), info('std.log')] });
    const code = await harness.run(['list', '--json']);
    expect(code).toBe(0);
    const parsed = JSON.parse(harness.stdout[0] ?? '') as InstanceInfo[];
    expect(parsed).toHaveLength(2);
    expect(parsed[0]).toMatchObject({
      plugId: 'example.storage',
      version: { major: 1, minor: 2, patch: 0 },
      state: 'active',
      generation: 1,
    });
  });

  it('maps boot failure to exit 1 with the RackError status on stderr', async () => {
    const harness = makeHarness({
      failures: { boot: new RackError('failed', 'plugin module exploded') },
    });
    const code = await harness.run(['list']);
    expect(code).toBe(1);
    expect(harness.stderr.join('\n')).toContain('[failed]');
    expect(harness.stderr.join('\n')).toContain('plugin module exploded');
    expect(harness.rack.log).toEqual(['boot', 'shutdown']);
  });
});

describe('rack-cli describe', () => {
  it('prints the InstanceInfo as JSON and shuts down', async () => {
    const harness = makeHarness({ instances: [info('example.storage')] });
    const code = await harness.run(['describe', 'example.storage']);
    expect(code).toBe(0);
    expect(JSON.parse(harness.stdout.join('\n'))).toEqual(info('example.storage'));
    expect(harness.rack.log).toEqual(['boot', 'shutdown']);
  });

  it('prints compact single-line JSON under --json', async () => {
    const harness = makeHarness({ instances: [info('std.log')] });
    const code = await harness.run(['describe', 'std.log', '--json']);
    expect(code).toBe(0);
    expect(harness.stdout).toHaveLength(1);
    expect(JSON.parse(harness.stdout[0] ?? '')).toEqual(info('std.log'));
  });

  it('maps unknown plug ids to exit 1 with a not_found error envelope', async () => {
    const harness = makeHarness({ instances: [info('example.storage')] });
    const code = await harness.run(['describe', 'nope.plugin', '--json']);
    expect(code).toBe(1);
    const envelope = JSON.parse(harness.stderr[0] ?? '') as {
      command: string;
      error: { status: string; message: string };
    };
    expect(envelope.command).toBe('describe');
    expect(envelope.error.status).toBe('not_found');
    expect(envelope.error.message).toContain('nope.plugin');
    expect(harness.rack.log).toEqual(['boot', 'shutdown']);
  });
});

// ---------------------------------------------------------------------------
// call
// ---------------------------------------------------------------------------

describe('rack-cli call', () => {
  it('performs one acquire -> call -> release cycle with the full version range', async () => {
    const harness = makeHarness({ instances: [info('example.storage')] });
    harness.rack.calls.result = { stored: true, key: 'a' };
    const code = await harness.run([
      'call',
      'example.storage',
      'set',
      '{"key":"a","value":42}',
      '--json',
    ]);
    expect(code).toBe(0);
    expect(harness.rack.log).toEqual([
      'add',
      'boot',
      'acquire:example.storage',
      'call:set',
      'release:example.storage',
      'shutdown',
    ]);

    const acquisition = harness.rack.caps.acquires[0];
    expect(acquisition?.plugId).toBe('example.storage');
    expect(acquisition?.range.minimum).toEqual({ major: 0, minor: 0, patch: 0 });
    expect(acquisition?.range.maximum).toEqual({
      major: Number.MAX_SAFE_INTEGER,
      minor: Number.MAX_SAFE_INTEGER,
      patch: Number.MAX_SAFE_INTEGER,
    });

    const invocation = harness.rack.calls.invocations[0];
    expect(invocation?.methodName).toBe('set');
    expect(invocation?.args).toEqual({ key: 'a', value: 42 });
    expect(harness.rack.caps.releases).toEqual(['example.storage']);

    expect(JSON.parse(harness.stdout[0] ?? '')).toEqual({ stored: true, key: 'a' });
  });

  it('pretty-prints the result in text mode', async () => {
    const harness = makeHarness();
    const code = await harness.run(['call', 'std.log', 'query', '{"level":"warn"}']);
    expect(code).toBe(0);
    expect(JSON.parse(harness.stdout.join('\n'))).toEqual({ echoed: true, input: null });
    expect(harness.rack.calls.invocations[0]?.args).toEqual({ level: 'warn' });
  });

  it('releases the lease and shuts down even when the call fails', async () => {
    const harness = makeHarness();
    harness.rack.calls.error = new RackError('failed', 'provider threw');
    const code = await harness.run(['call', 'std.log', 'boom', '{}']);
    expect(code).toBe(1);
    expect(harness.stderr.join('\n')).toContain('[failed]');
    expect(harness.stderr.join('\n')).toContain('provider threw');
    expect(harness.rack.log).toEqual([
      'add',
      'boot',
      'acquire:std.log',
      'call:boom',
      'release:std.log',
      'shutdown',
    ]);
  });

  it('maps acquire failures to exit 1 without invoking or releasing', async () => {
    const harness = makeHarness();
    harness.rack.caps.acquireError = new RackError('not_found', 'no active provider');
    const code = await harness.run(['call', 'ghost.plug', 'get', '{}']);
    expect(code).toBe(1);
    expect(harness.stderr.join('\n')).toContain('[not_found]');
    expect(harness.stderr.join('\n')).toContain('no active provider');
    expect(harness.rack.log).toEqual(['add', 'boot', 'shutdown']);
  });

  it('falls back to an explicit start() on hosts whose boot() defers staged plugins', async () => {
    const harness = makeHarness({ deferStagedStart: true });
    const code = await harness.run(['call', 'std.log', 'query', 'null']);
    expect(code).toBe(0);
    expect(harness.rack.log).toEqual([
      'add',
      'boot',
      'start',
      'acquire:std.log',
      'call:query',
      'release:std.log',
      'shutdown',
    ]);
  });
});

// ---------------------------------------------------------------------------
// unload / reload / poll
// ---------------------------------------------------------------------------

describe('rack-cli unload', () => {
  it('boots, unloads the target and shuts down', async () => {
    const harness = makeHarness({ instances: [info('example.storage')] });
    const code = await harness.run(['unload', 'example.storage']);
    expect(code).toBe(0);
    expect(harness.stdout.join('\n')).toContain('unloaded example.storage');
    expect(harness.rack.log).toEqual(['boot', 'unload:example.storage', 'shutdown']);
  });

  it('still shuts down when unload rejects', async () => {
    const harness = makeHarness({
      failures: { unload: new RackError('invalid_state', 'lease still outstanding') },
    });
    const code = await harness.run(['unload', 'std.net', '--json']);
    expect(code).toBe(1);
    const envelope = JSON.parse(harness.stderr[0] ?? '') as {
      error: { status: string; message: string };
    };
    expect(envelope.error.status).toBe('invalid_state');
    expect(envelope.error.message).toContain('lease still outstanding');
    expect(harness.rack.log).toEqual(['boot', 'unload:std.net', 'shutdown']);
  });
});

describe('rack-cli reload', () => {
  it('emits an ok envelope under --json', async () => {
    const harness = makeHarness();
    const code = await harness.run(['reload', 'std.log', '--json']);
    expect(code).toBe(0);
    expect(JSON.parse(harness.stdout[0] ?? '')).toEqual({
      command: 'reload',
      plugId: 'std.log',
      ok: true,
    });
    expect(harness.rack.log).toEqual(['boot', 'reload:std.log', 'shutdown']);
  });

  it('prints a reload notice in text mode', async () => {
    const harness = makeHarness();
    const code = await harness.run(['reload', 'std.log']);
    expect(code).toBe(0);
    expect(harness.stdout.join('\n')).toContain('reloaded std.log');
  });
});

describe('rack-cli poll', () => {
  it('boots, drains and shuts down', async () => {
    const harness = makeHarness();
    const code = await harness.run(['poll']);
    expect(code).toBe(0);
    expect(harness.stdout.join('\n')).toContain('poll complete');
    expect(harness.rack.log).toEqual(['boot', 'poll', 'shutdown']);
  });

  it('emits an ok envelope under --json', async () => {
    const harness = makeHarness();
    const code = await harness.run(['poll', '--json']);
    expect(code).toBe(0);
    expect(JSON.parse(harness.stdout[0] ?? '')).toEqual({ command: 'poll', ok: true });
  });
});

// ---------------------------------------------------------------------------
// serve
// ---------------------------------------------------------------------------

describe('rack-cli serve', () => {
  it('boots, lists plugins, shuts down gracefully on the simulated signal', async () => {
    const harness = makeHarness({ instances: [info('example.storage'), info('std.log')] });
    const code = await harness.run(['serve']);
    expect(code).toBe(0);
    const text = harness.stdout.join('\n');
    expect(text).toContain('rack is serving plugins from examples/plugins');
    expect(text).toContain('example.storage');
    expect(text).toContain('std.log');
    expect(text).toContain('rack shut down');
    expect(harness.rack.log).toEqual(['boot', 'shutdown']);
  });

  it('emits running and stopped JSON documents under --json', async () => {
    const harness = makeHarness({ instances: [info('example.storage')] });
    const code = await harness.run(['serve', '--dir', 'custom/plugs', '--json']);
    expect(code).toBe(0);
    const running = JSON.parse(harness.stdout[0] ?? '') as {
      command: string;
      status: string;
      dir: string;
      plugins: InstanceInfo[];
    };
    expect(running.command).toBe('serve');
    expect(running.status).toBe('running');
    expect(running.dir).toBe('custom/plugs');
    expect(running.plugins).toEqual([info('example.storage')]);
    const stopped = JSON.parse(harness.stdout[1] ?? '') as { command: string; status: string };
    expect(stopped).toEqual({ command: 'serve', status: 'stopped' });
  });

  it('never installs keep-alive wiring when boot fails', async () => {
    const rack = new FakeRack({ failures: { boot: new RackError('cycle', 'init cycle') } });
    const stdout: string[] = [];
    const stderr: string[] = [];
    let keepAliveInstalled = false;
    const code = await runCli(['serve'], {
      out: (line) => stdout.push(line),
      err: (line) => stderr.push(line),
      rackFactory: () => rack,
      keepAlive: () => {
        keepAliveInstalled = true;
      },
    });
    expect(code).toBe(1);
    expect(keepAliveInstalled).toBe(false);
    expect(stderr.join('\n')).toContain('[cycle]');
    expect(rack.log).toEqual(['boot', 'shutdown']);
  });
});

// ---------------------------------------------------------------------------
// Directory forwarding, usage and error mapping
// ---------------------------------------------------------------------------

describe('rack-cli --dir handling', () => {
  it('forwards --dir to the rack factory and defaults to examples/plugins', async () => {
    const rack = new FakeRack();
    const seenDirs: Array<string | undefined> = [];
    const ctx: CliContext = {
      out: () => undefined,
      err: () => undefined,
      rackFactory: (dir?: string) => {
        seenDirs.push(dir);
        return rack;
      },
    };
    expect(await runCli(['list', '--dir', 'custom/plugs'], ctx)).toBe(0);
    expect(await runCli(['list'], ctx)).toBe(0);
    expect(seenDirs).toEqual(['custom/plugs', 'examples/plugins']);
  });
});

describe('rack-cli usage errors', () => {
  it.each([
    [[], 'missing command'],
    [['frobnicate'], "unknown command 'frobnicate'"],
    [['describe'], 'expected 1 positional argument(s), got 0'],
    [['call', 'a', 'b'], 'expected 3 positional argument(s), got 2'],
    [['call', 'a', 'b', 'not-json'], 'must be valid JSON'],
    [['list', '--bogus'], 'Unknown option'],
    [['poll', 'extra'], 'expected 0 positional argument(s), got 1'],
  ])('maps %j to exit 2 with help on stderr', async (argv, fragment) => {
    const harness = makeHarness();
    const code = await harness.run(argv);
    expect(code).toBe(2);
    const stderr = harness.stderr.join('\n');
    expect(stderr).toContain(fragment);
    expect(stderr).toContain('Usage: node apps/rack-cli.ts');
    expect(harness.rack.log).toEqual([]);
    expect(harness.stdout).toEqual([]);
  });
});

describe('rack-cli error mapping', () => {
  it('maps a missing host implementation to a friendly runtime error', async () => {
    const stdout: string[] = [];
    const stderr: string[] = [];
    const code = await runCli(
      ['list'],
      failingFactoryContext(stdout, stderr, new Error('Cannot find module')),
    );
    expect(code).toBe(1);
    expect(stderr.join('\n')).toContain('[failed]');
    expect(stdout).toEqual([]);
  });

  it('maps shutdown failures to a warning while keeping the operation result', async () => {
    const harness = makeHarness({
      instances: [info('example.storage')],
      failures: { shutdown: new RackError('failed', 'isolated instance retained') },
    });
    const code = await harness.run(['unload', 'example.storage']);
    expect(code).toBe(0);
    expect(harness.stdout.join('\n')).toContain('unloaded example.storage');
    expect(harness.stderr.join('\n')).toContain('rack shutdown failed');
    expect(harness.rack.log).toEqual(['boot', 'unload:example.storage', 'shutdown']);
  });
});
