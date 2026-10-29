/**
 * @file net.test.ts
 * @brief Tests for the std.net standard device and its example consumer net-demo.
 *
 * @details No real network is ever touched: the DNS resolver and the TCP connector are
 *          injected fakes and PluginServices are self-written records, following the fake
 *          paradigm of tests/sdk.test.ts. TCP timing cases use vitest fake timers so the
 *          2000 ms default, the hard timeout and the measured latency stay deterministic.
 */
import { afterEach, describe, expect, it, vi } from 'vitest';
import { create, createWithDeps } from '../../src/plugins/net/index.ts';
import type {
  DnsResolver,
  NetPluginDeps,
  TcpConnection,
  TcpConnector,
} from '../../src/plugins/net/index.ts';
import { create as createNetDemo } from '../../examples/plugins/net-demo.plug.ts';
import { isRackError, statusOf } from '../../src/core/status.ts';
import type { RackError } from '../../src/core/status.ts';
import type { JsonObject, JsonValue } from '../../src/core/json.ts';
import type {
  CapEvent,
  CapSink,
  CapsDesc,
  LeaseCredential,
  LeaseHandle,
  PluginServices,
  RackPlugin,
  Revoker,
  Subscription,
  VersionRange,
} from '../../src/core/types.ts';

afterEach(() => {
  vi.useRealTimers();
});

// ---------------------------------------------------------------------------
// Self-written fake PluginServices (records publications, watchers, acquisitions,
// releases and gateway calls); no host implementation is imported.
// ---------------------------------------------------------------------------

/**
 * @brief Build a fake PluginServices bundle backed by plain records.
 *
 * @param options.callResult Value resolved by every fake gateway call.
 *
 * @return The services object plus record arrays and a capability delivery helper.
 */
function makeFakeServices(options?: { callResult?: JsonValue }) {
  const publishedCaps: CapsDesc[] = [];
  const capWatchers: CapSink[] = [];
  const acquisitions: Array<{ plugId: string; range: VersionRange; revoker: Revoker }> = [];
  const releases: LeaseCredential[] = [];
  const calls: Array<{ credential: LeaseCredential; methodName: string; args: JsonValue }> = [];
  const handles: LeaseHandle[] = [];
  let credentialSeq = 0;

  const services: PluginServices = {
    plugId: 'test.harness',
    events: {
      subscribe(): Subscription {
        return { unsubscribe() {} };
      },
      publish() {
        // The plugins under test publish no ordinary events.
      },
    },
    caps: {
      publish(caps: CapsDesc): void {
        publishedCaps.push(caps);
      },
      watch(sink: CapSink): Subscription {
        capWatchers.push(sink);
        let live = true;
        return {
          unsubscribe() {
            if (!live) return;
            live = false;
            const index = capWatchers.indexOf(sink);
            if (index >= 0) capWatchers.splice(index, 1);
          },
        };
      },
      snapshot(): readonly CapEvent[] {
        return [];
      },
      acquire(plugId: string, range: VersionRange, revoker: Revoker): LeaseHandle {
        acquisitions.push({ plugId, range, revoker });
        credentialSeq += 1;
        const credential: LeaseCredential = {
          kind: 'u42.lease.credential',
          handle: { seq: credentialSeq },
        };
        const handle: LeaseHandle = {
          credential,
          plugId,
          version: { major: 1, minor: 0, patch: 0 },
          release(): Promise<'ok' | 'busy' | 'stale'> {
            releases.push(credential);
            return Promise.resolve('ok');
          },
        };
        handles.push(handle);
        return handle;
      },
      release(credential: LeaseCredential): 'ok' | 'busy' | 'stale' {
        releases.push(credential);
        return 'ok';
      },
    },
    calls: {
      call(credential: LeaseCredential, methodName: string, args: JsonValue): Promise<JsonValue> {
        calls.push({ credential, methodName, args });
        return Promise.resolve(options?.callResult ?? null);
      },
      callById(credential: LeaseCredential, methodId: number, args: JsonValue): Promise<JsonValue> {
        calls.push({ credential, methodName: `#${methodId}`, args });
        return Promise.resolve(options?.callResult ?? null);
      },
    },
    diag: {
      report() {
        // Diagnostics of the plugins under test are not asserted here.
      },
    },
  };

  return {
    services,
    publishedCaps,
    acquisitions,
    releases,
    calls,
    handles,
    async deliverCapability(event: CapEvent): Promise<void> {
      for (const sink of [...capWatchers]) await sink.onCapability(event);
    },
  };
}

/** @brief std.net availability fact for the given provider version. */
function netAvailable(major = 1, minor = 0, patch = 0): CapEvent {
  return {
    plugId: 'std.net',
    available: true,
    version: { major, minor, patch },
    capabilities: {
      methods: [
        { id: 1, name: 'dns.resolve' },
        { id: 2, name: 'tcp.probe' },
        { id: 3, name: 'url.normalize' },
      ],
    },
  };
}

// ---------------------------------------------------------------------------
// Shared helpers.
// ---------------------------------------------------------------------------

/**
 * @brief Invoke one method by id and name on a plugin and normalize the result.
 *
 * @param plugin Plugin under test.
 *
 * @param methodId Numeric method selector.
 *
 * @param methodName Name selector.
 *
 * @param args Request args document.
 *
 * @return The settled invoke result as a promise.
 */
function invoke(
  plugin: RackPlugin,
  methodId: number,
  methodName: string,
  args: JsonValue,
): Promise<JsonValue> {
  const result = plugin.invoke?.({ methodId, methodName, args });
  if (result === undefined) throw new Error('plugin under test has no invoke');
  return Promise.resolve(result);
}

/**
 * @brief Run a callback and return the RackError it must throw.
 *
 * @param run Callback expected to throw (sync or async).
 *
 * @return The captured RackError.
 */
async function captureRackError(run: () => unknown): Promise<RackError> {
  try {
    await run();
  } catch (error) {
    expect(isRackError(error)).toBe(true);
    expect(statusOf(error)).not.toBe('ok');
    return error as RackError;
  }
  expect.unreachable();
  throw new Error('unreachable');
}

/**
 * @brief Assert a JSON value is an object and return it as JsonObject.
 *
 * @param value Candidate result document.
 *
 * @return The narrowed object.
 */
function asObject(value: JsonValue): JsonObject {
  expect(typeof value === 'object' && value !== null && !Array.isArray(value)).toBe(true);
  return value as JsonObject;
}

/**
 * @brief Create and start one std.net instance over the given deps with fake services.
 *
 * @param deps Injected effect ports.
 *
 * @return The started plugin.
 */
async function startedNet(deps: NetPluginDeps): Promise<RackPlugin> {
  const fake = makeFakeServices();
  const plugin = createWithDeps(deps);
  await plugin.init(fake.services);
  await plugin.start();
  return plugin;
}

// ---------------------------------------------------------------------------
// Fake DNS resolver.
// ---------------------------------------------------------------------------

/**
 * @brief Build a fake resolver returning fixed per-family results or rejections.
 *
 * @param v4 A record answer for resolve4, or an Error to reject with.
 *
 * @param v6 A record answer for resolve6, or an Error to reject with.
 *
 * @return The resolver plus the recorded requests.
 */
function fakeDns(
  v4: string[] | Error,
  v6: string[] | Error,
): { resolver: DnsResolver; requests: Array<{ family: 4 | 6; host: string }> } {
  const requests: Array<{ family: 4 | 6; host: string }> = [];
  const settle = (value: string[] | Error): Promise<string[]> =>
    value instanceof Error ? Promise.reject(value) : Promise.resolve([...value]);
  const resolver: DnsResolver = {
    resolve4(host: string): Promise<string[]> {
      requests.push({ family: 4, host });
      return settle(v4);
    },
    resolve6(host: string): Promise<string[]> {
      requests.push({ family: 6, host });
      return settle(v6);
    },
  };
  return { resolver, requests };
}

/** @brief DNS-style error carrying a Node resolver code. */
function dnsError(code: string): Error {
  return Object.assign(new Error(`dns query failed (${code})`), { code });
}

// ---------------------------------------------------------------------------
// Fake TCP connector with manually driven connections.
// ---------------------------------------------------------------------------

/** @brief A controllable fake connection plus its state and emitters. */
interface ManualConnection {
  readonly connection: TcpConnection;
  destroyed(): boolean;
  emitConnected(): void;
  emitFailed(error?: unknown): void;
}

/**
 * @brief Build one manually driven fake connection.
 *
 * @param options.keepCallbacksAfterDestroy Simulate an undisciplined peer that keeps
 *        invoking callbacks after destroy(); used to prove late callbacks are ignored.
 *
 * @return The fake connection handle.
 */
function manualConnection(options?: { keepCallbacksAfterDestroy?: boolean }): ManualConnection {
  let connected: (() => void) | null = null;
  let failed: ((error: unknown) => void) | null = null;
  let destroyed = false;
  return {
    connection: {
      onConnected(callback: () => void): void {
        connected = callback;
      },
      onFailed(callback: (error: unknown) => void): void {
        failed = callback;
      },
      destroy(): void {
        destroyed = true;
        if (options?.keepCallbacksAfterDestroy !== true) {
          connected = null;
          failed = null;
        }
      },
    },
    destroyed(): boolean {
      return destroyed;
    },
    emitConnected(): void {
      connected?.();
    },
    emitFailed(error: unknown = new Error('connect ECONNREFUSED')): void {
      failed?.(error);
    },
  };
}

/**
 * @brief Build a fake connector recording every attempt and pending connection.
 *
 * @return The connector plus its records.
 */
function fakeConnector(): {
  connector: TcpConnector;
  pending: ManualConnection[];
  connectRequests: Array<{ host: string; port: number }>;
} {
  const pending: ManualConnection[] = [];
  const connectRequests: Array<{ host: string; port: number }> = [];
  const connector: TcpConnector = {
    connect(host: string, port: number): TcpConnection {
      connectRequests.push({ host, port });
      const manual = manualConnection();
      pending.push(manual);
      return manual.connection;
    },
  };
  return { connector, pending, connectRequests };
}

// ---------------------------------------------------------------------------
// Capability disclosure.
// ---------------------------------------------------------------------------

describe('std.net capability disclosure', () => {
  it('announces identity and exactly the three methods on start', async () => {
    const fake = makeFakeServices();
    const net = create();
    expect(net.desc.plugId).toBe('std.net');
    expect(net.desc.version).toEqual({ major: 1, minor: 0, patch: 0 });
    expect(net.desc.init).toEqual({ before: [], after: [] });
    await net.init(fake.services);
    await net.start();
    expect(fake.publishedCaps).toHaveLength(1);
    const methods = fake.publishedCaps[0]?.methods ?? [];
    expect(methods.map((method) => [method.id, method.name])).toEqual([
      [1, 'dns.resolve'],
      [2, 'tcp.probe'],
      [3, 'url.normalize'],
    ]);
    for (const method of methods) {
      expect(method.inputSchema).toBeDefined();
      expect(method.outputSchema).toBeDefined();
    }
  });

  it('rejects malformed dependency ports with invalid_argument', async () => {
    const badDns = await captureRackError(() =>
      createWithDeps({ dns: {} as DnsResolver }),
    );
    expect(badDns.status).toBe('invalid_argument');
    const badTcp = await captureRackError(() =>
      createWithDeps({ tcp: { connect: 1 } as unknown as TcpConnector }),
    );
    expect(badTcp.status).toBe('invalid_argument');
  });
});

// ---------------------------------------------------------------------------
// dns.resolve
// ---------------------------------------------------------------------------

describe('std.net dns.resolve', () => {
  it('merges resolve4 and resolve6 results, deduplicated, v4 first', async () => {
    const fake = fakeDns(
      ['93.184.216.34', '1.1.1.1'],
      ['93.184.216.34', '2606:2800:220:1:248:1893:25c8:1946'],
    );
    const net = await startedNet({ dns: fake.resolver });
    const result = await invoke(net, 1, 'dns.resolve', { host: 'example.com' });
    expect(result).toEqual({
      addresses: ['93.184.216.34', '1.1.1.1', '2606:2800:220:1:248:1893:25c8:1946'],
    });
    expect(fake.requests).toEqual([
      { family: 4, host: 'example.com' },
      { family: 6, host: 'example.com' },
    ]);
  });

  it('accepts hosts whose records exist for one family only', async () => {
    const fake = fakeDns(['93.184.216.34'], dnsError('ENODATA'));
    const net = await startedNet({ dns: fake.resolver });
    const result = await invoke(net, 1, 'dns.resolve', { host: 'v4-only.example' });
    expect(result).toEqual({ addresses: ['93.184.216.34'] });
    // Both empty record sets are still a successful answer.
    const empty = fakeDns([], []);
    const net2 = await startedNet({ dns: empty.resolver });
    expect(await invoke(net2, 1, 'dns.resolve', { host: 'localhost' })).toEqual({
      addresses: [],
    });
  });

  it('maps a total lookup failure to failed with the resolver code', async () => {
    const fake = fakeDns(dnsError('ENOTFOUND'), dnsError('EAI_AGAIN'));
    const net = await startedNet({ dns: fake.resolver });
    const error = await captureRackError(() =>
      invoke(net, 1, 'dns.resolve', { host: 'missing.example' }),
    );
    expect(error.status).toBe('failed');
    expect(error.detail).toEqual({ code: 'ENOTFOUND' });
    // Rejections without a code still surface 'failed', just without a code detail.
    const codeless = fakeDns(new Error('resolver exploded'), new Error('resolver exploded'));
    const net2 = await startedNet({ dns: codeless.resolver });
    const error2 = await captureRackError(() =>
      invoke(net2, 1, 'dns.resolve', { host: 'broken.example' }),
    );
    expect(error2.status).toBe('failed');
    expect(error2.detail).toBeUndefined();
  });

  it('accepts structurally valid hostnames and rejects invalid ones', async () => {
    const fake = fakeDns([], []);
    const net = await startedNet({ dns: fake.resolver });
    for (const host of ['localhost', 'example.com.', 'api-v2.example.co.uk', '127.0.0.1']) {
      expect(await invoke(net, 1, 'dns.resolve', { host })).toEqual({ addresses: [] });
    }
    expect(fake.requests).toHaveLength(8);
    for (const host of ['', 'exa mple.com', '-bad.example', 'bad-.example', 'a..b', 'under_score.example', '::1']) {
      const error = await captureRackError(() => invoke(net, 1, 'dns.resolve', { host }));
      expect(error.status).toBe('invalid_argument');
    }
    for (const args of [
      null,
      'example.com',
      [],
      { host: 42 },
      { host: null },
      {},
    ] as JsonValue[]) {
      const error = await captureRackError(() => invoke(net, 1, 'dns.resolve', args));
      expect(error.status).toBe('invalid_argument');
    }
    expect(fake.requests).toHaveLength(8); // no rejected case reached the resolver
  });
});

// ---------------------------------------------------------------------------
// tcp.probe
// ---------------------------------------------------------------------------

describe('std.net tcp.probe', () => {
  it('settles ok:true with latencyMs on connect and destroys the connection', async () => {
    const fake = fakeConnector();
    const net = await startedNet({ tcp: fake.connector });
    const pending = invoke(net, 2, 'tcp.probe', { host: 'example.com', port: 443 });
    const manual = fake.pending[0];
    expect(manual).toBeDefined();
    expect(manual?.destroyed()).toBe(false);
    manual?.emitConnected();
    const result = asObject(await pending);
    expect(result['ok']).toBe(true);
    expect(typeof result['latencyMs']).toBe('number');
    expect(result['latencyMs'] as number).toBeGreaterThanOrEqual(0);
    expect(manual?.destroyed()).toBe(true);
    expect(fake.connectRequests).toEqual([{ host: 'example.com', port: 443 }]);
  });

  it('settles ok:false without latencyMs on an immediate refusal', async () => {
    const fake = fakeConnector();
    const net = await startedNet({ tcp: fake.connector });
    const pending = invoke(net, 2, 'tcp.probe', { host: 'closed.example', port: 81 });
    fake.pending[0]?.emitFailed(
      Object.assign(new Error('connect ECONNREFUSED'), { code: 'ECONNREFUSED' }),
    );
    expect(await pending).toEqual({ ok: false });
    expect(fake.pending[0]?.destroyed()).toBe(true);
  });

  it('destroys the connection and settles ok:false when the hard timeout fires', async () => {
    vi.useFakeTimers();
    const fake = fakeConnector();
    const net = await startedNet({ tcp: fake.connector });
    const pending = invoke(net, 2, 'tcp.probe', {
      host: 'slow.example',
      port: 80,
      timeoutMs: 100,
    });
    const manual = fake.pending[0];
    await vi.advanceTimersByTimeAsync(99);
    expect(manual?.destroyed()).toBe(false);
    await vi.advanceTimersByTimeAsync(1);
    expect(await pending).toEqual({ ok: false });
    expect(manual?.destroyed()).toBe(true);
  });

  it('defaults timeoutMs to 2000', async () => {
    vi.useFakeTimers();
    const fake = fakeConnector();
    const net = await startedNet({ tcp: fake.connector });
    const pending = invoke(net, 2, 'tcp.probe', { host: 'slow.example', port: 80 });
    const manual = fake.pending[0];
    await vi.advanceTimersByTimeAsync(1999);
    expect(manual?.destroyed()).toBe(false);
    await vi.advanceTimersByTimeAsync(1);
    expect(await pending).toEqual({ ok: false });
    expect(manual?.destroyed()).toBe(true);
  });

  it('reports the elapsed latency of a delayed successful connect', async () => {
    vi.useFakeTimers();
    let handle: ManualConnection | null = null;
    const connector: TcpConnector = {
      connect(host: string, port: number): TcpConnection {
        expect(host).toBe('delay.example');
        expect(port).toBe(8080);
        handle = manualConnection();
        setTimeout(() => handle?.emitConnected(), 50);
        return handle.connection;
      },
    };
    const net = await startedNet({ tcp: connector });
    const pending = invoke(net, 2, 'tcp.probe', {
      host: 'delay.example',
      port: 8080,
      timeoutMs: 5000,
    });
    await vi.advanceTimersByTimeAsync(50);
    expect(await pending).toEqual({ ok: true, latencyMs: 50 });
  });

  it('accepts port boundaries and rejects invalid ports, hosts and timeouts', async () => {
    const fake = fakeConnector();
    const net = await startedNet({ tcp: fake.connector });
    for (const port of [0, -1, 65536, 1.5, '80', null]) {
      const error = await captureRackError(() =>
        invoke(net, 2, 'tcp.probe', { host: 'example.com', port: port as JsonValue }),
      );
      expect(error.status).toBe('invalid_argument');
    }
    for (const args of [
      null,
      'x',
      [],
      {},
      { port: 80 },
      { host: 'example.com' },
      { host: '', port: 80 },
      { host: 'exa mple.com', port: 80 },
      { host: 'example.com', port: 80, timeoutMs: 0 },
      { host: 'example.com', port: 80, timeoutMs: -5 },
      { host: 'example.com', port: 80, timeoutMs: 2.5 },
      { host: 'example.com', port: 80, timeoutMs: 'soon' },
    ] as JsonValue[]) {
      const error = await captureRackError(() => invoke(net, 2, 'tcp.probe', args));
      expect(error.status).toBe('invalid_argument');
    }
    expect(fake.pending).toHaveLength(0);
    // IPv6 literals are valid probe hosts; port boundaries reach the connector.
    const first = invoke(net, 2, 'tcp.probe', { host: '::1', port: 1 });
    const second = invoke(net, 2, 'tcp.probe', { host: 'example.com', port: 65535 });
    expect(fake.connectRequests).toEqual([
      { host: '::1', port: 1 },
      { host: 'example.com', port: 65535 },
    ]);
    fake.pending[0]?.emitConnected();
    fake.pending[1]?.emitConnected();
    expect(asObject(await first)['ok']).toBe(true);
    expect(asObject(await second)['ok']).toBe(true);
  });

  it('stop() aborts in-flight probes, settles them ok:false and waits for settlement', async () => {
    const fake = fakeConnector();
    const net = await startedNet({ tcp: fake.connector });
    const first = invoke(net, 2, 'tcp.probe', { host: 'a.example', port: 80 });
    const second = invoke(net, 2, 'tcp.probe', { host: 'b.example', port: 81 });
    expect(fake.pending).toHaveLength(2);
    await net.stop();
    expect(await first).toEqual({ ok: false });
    expect(await second).toEqual({ ok: false });
    expect(fake.pending.every((manual) => manual.destroyed())).toBe(true);
    const error = await captureRackError(() =>
      invoke(net, 2, 'tcp.probe', { host: 'c.example', port: 82 }),
    );
    expect(error.status).toBe('invalid_state');
  });

  it('ignores late connection callbacks after stop() has settled a probe', async () => {
    const sloppy = manualConnection({ keepCallbacksAfterDestroy: true });
    const connector: TcpConnector = {
      connect(): TcpConnection {
        return sloppy.connection;
      },
    };
    const net = await startedNet({ tcp: connector });
    const pending = invoke(net, 2, 'tcp.probe', { host: 'x.example', port: 80 });
    await net.stop();
    expect(await pending).toEqual({ ok: false });
    // A peer that keeps firing after destroy() must not resurrect or crash the probe.
    sloppy.emitConnected();
    sloppy.emitFailed(new Error('ECONNRESET'));
    expect(await pending).toEqual({ ok: false });
  });

  it('rejects unknown methods with not_found', async () => {
    const net = await startedNet({});
    const error = await captureRackError(() => invoke(net, 99, 'nope', null));
    expect(error.status).toBe('not_found');
  });
});

// ---------------------------------------------------------------------------
// url.normalize
// ---------------------------------------------------------------------------

describe('std.net url.normalize', () => {
  it('normalizes an absolute URL with credentials, port, path, query and fragment', async () => {
    const net = await startedNet({});
    const result = await invoke(net, 3, 'url.normalize', {
      url: 'HTTPS://user:pw@Example.com:8443/p/a/./t%68/../th?q=1&r=2#frag',
    });
    expect(result).toEqual({
      url: 'https://user:pw@example.com:8443/p/a/th?q=1&r=2#frag',
      host: 'example.com:8443',
      pathname: '/p/a/th',
      searchParams: { q: '1', r: '2' },
    });
  });

  it('resolves a relative URL against a base and defaults missing parts', async () => {
    const net = await startedNet({});
    expect(await invoke(net, 3, 'url.normalize', { url: '/guide/?q=vite', base: 'https://cn.vitejs.dev/' })).toEqual({
      url: 'https://cn.vitejs.dev/guide/?q=vite',
      host: 'cn.vitejs.dev',
      pathname: '/guide/',
      searchParams: { q: 'vite' },
    });
    expect(await invoke(net, 3, 'url.normalize', { url: 'https://example.com' })).toEqual({
      url: 'https://example.com/',
      host: 'example.com',
      pathname: '/',
      searchParams: {},
    });
    // Repeated query keys keep the last value.
    expect(
      asObject(await invoke(net, 3, 'url.normalize', { url: 'https://example.com/?a=1&a=2' }))['searchParams'],
    ).toEqual({ a: '2' });
  });

  it('maps malformed URLs and args to invalid_argument', async () => {
    const net = await startedNet({});
    for (const args of [
      null,
      'https://example.com',
      [],
      {},
      { url: 42 },
      { url: '' },
      { url: 'not a url' },
      { url: '/x', base: '::not-a-base::' },
      { url: 'https://example.com', base: 7 },
    ] as JsonValue[]) {
      const error = await captureRackError(() => invoke(net, 3, 'url.normalize', args));
      expect(error.status).toBe('invalid_argument');
    }
  });
});

// ---------------------------------------------------------------------------
// Example net-demo consumer: lease + gateway against std.net.
// ---------------------------------------------------------------------------

describe('example net-demo plugin', () => {
  it('announces its check method and the hard init edge on std.net', async () => {
    const fake = makeFakeServices();
    const demo = createNetDemo();
    expect(demo.desc.plugId).toBe('example.net-demo');
    expect(demo.desc.version).toEqual({ major: 1, minor: 0, patch: 0 });
    expect(demo.desc.init.after).toEqual(['std.net']);
    await demo.init(fake.services);
    await demo.start();
    expect(fake.publishedCaps[0]?.methods.map((method) => [method.id, method.name])).toEqual([
      [1, 'check'],
    ]);
  });

  it('leases std.net on an in-range offer and forwards check through the gateway', async () => {
    const fake = makeFakeServices({ callResult: { ok: true, latencyMs: 12 } });
    const demo = createNetDemo();
    await demo.init(fake.services);
    await demo.start();
    await fake.deliverCapability(netAvailable());
    expect(fake.acquisitions).toHaveLength(1);
    expect(fake.acquisitions[0]?.plugId).toBe('std.net');
    expect(fake.acquisitions[0]?.range.minimum).toEqual({ major: 1, minor: 0, patch: 0 });
    expect(fake.acquisitions[0]?.range.maximum).toEqual({ major: 1, minor: 0, patch: 0 });
    const result = await invoke(demo, 1, 'check', { host: 'example.com', port: 443 });
    expect(result).toEqual({ ok: true, latencyMs: 12 });
    expect(fake.calls).toEqual([
      {
        credential: fake.handles[0]?.credential,
        methodName: 'tcp.probe',
        args: { host: 'example.com', port: 443, timeoutMs: 2000 },
      },
    ]);
  });

  it('rejects check without a usable lease and with malformed args', async () => {
    const fake = makeFakeServices({ callResult: { ok: false } });
    const demo = createNetDemo();
    await demo.init(fake.services);
    await demo.start();
    // Out-of-range offers never open business.
    await fake.deliverCapability(netAvailable(1, 1, 0));
    expect(fake.acquisitions).toHaveLength(0);
    const noLease = await captureRackError(() =>
      invoke(demo, 1, 'check', { host: 'example.com', port: 443 }),
    );
    expect(noLease.status).toBe('invalid_state');
    // With a lease, malformed args are rejected before any gateway call.
    await fake.deliverCapability(netAvailable());
    for (const args of [
      null,
      'x',
      {},
      { host: 'example.com' },
      { host: '', port: 443 },
      { host: 'example.com', port: 0 },
      { host: 'example.com', port: 65536 },
      { host: 'example.com', port: '443' },
    ] as JsonValue[]) {
      const error = await captureRackError(() => invoke(demo, 1, 'check', args));
      expect(error.status).toBe('invalid_argument');
    }
    expect(fake.calls).toHaveLength(0);
    const unknown = await captureRackError(() => invoke(demo, 7, 'nope', null));
    expect(unknown.status).toBe('not_found');
  });

  it('returns the lease on revoke and on stop', async () => {
    const fake = makeFakeServices();
    const demo = createNetDemo();
    await demo.init(fake.services);
    await demo.start();
    await fake.deliverCapability(netAvailable());
    const credential = fake.handles[0]?.credential;
    expect(credential).toBeDefined();
    const revoker = fake.acquisitions[0]?.revoker;
    await revoker?.onRevoke(credential as LeaseCredential);
    expect(fake.releases).toEqual([credential]);
    // Business is closed after revocation.
    const afterRevoke = await captureRackError(() =>
      invoke(demo, 1, 'check', { host: 'example.com', port: 443 }),
    );
    expect(afterRevoke.status).toBe('invalid_state');
    // A second lifecycle: acquire again, then stop must release the lease.
    await fake.deliverCapability(netAvailable());
    await demo.stop();
    expect(fake.releases).toEqual([credential, fake.handles[1]?.credential]);
  });
});
