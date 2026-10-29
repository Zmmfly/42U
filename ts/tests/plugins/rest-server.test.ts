/**
 * @file Tests for std.rest-server: RESTful resource conventions over a leased std.http-server.
 *
 * @brief All PluginServices are self-written fakes: acquire() mints fake LeaseHandles pointing
 *        at std.http-server and the fake CallService records every route.add / route.remove
 *        call. No host implementation or other agent module is imported.
 */
import { describe, expect, it } from 'vitest';
import { create } from '../../src/plugins/rest-server/index.ts';
import { isRackError, RackError, statusOf } from '../../src/core/status.ts';
import type { RackStatus } from '../../src/core/status.ts';
import type { JsonObject, JsonValue } from '../../src/core/json.ts';
import type {
  CapEvent,
  CapSink,
  CapsDesc,
  EventSink,
  LeaseCredential,
  LeaseHandle,
  PluginServices,
  Revoker,
  VersionRange,
} from '../../src/core/types.ts';

// ---------------------------------------------------------------------------
// Self-written fake PluginServices: records publications, capability watchers, lease
// acquisitions, route.add / route.remove gateway calls and diagnostics.
// ---------------------------------------------------------------------------

/** @brief One recorded CapabilityService.acquire attempt. */
interface RecordedAcquire {
  plugId: string;
  range: VersionRange;
  revoker: Revoker;
}

/** @brief One recorded gateway call with its args document. */
interface RecordedCall {
  credential: LeaseCredential;
  args: JsonObject;
}

/**
 * @brief Build a fake PluginServices bundle backed by plain records.
 *
 * @param options.failRouteAddOn When set, the Nth route.add attempt (1-based) rejects with a
 *                               RackError to exercise mount rollback.
 * @return The services object plus record arrays and a capability delivery helper.
 */
function makeFakeHarness(options?: { failRouteAddOn?: number }) {
  const publishedCaps: CapsDesc[] = [];
  const capWatchers: CapSink[] = [];
  const acquisitions: RecordedAcquire[] = [];
  const releases: LeaseCredential[] = [];
  const routeAdds: RecordedCall[] = [];
  const routeRemoves: RecordedCall[] = [];
  const diagnostics: Array<{ level: string; message: string }> = [];
  const mintedHandles: LeaseHandle[] = [];
  let credentialSeq = 0;
  let routeSeq = 0;
  let routeAddAttempts = 0;

  const services: PluginServices = {
    plugId: 'test.rest-server',
    events: {
      subscribe(_name: string, _sink: EventSink) {
        let live = true;
        return {
          unsubscribe() {
            live = false;
          },
        };
      },
      publish() {
        // The plugin under test publishes no ordinary events; nothing to deliver.
      },
    },
    caps: {
      publish(caps: CapsDesc): void {
        publishedCaps.push(caps);
      },
      watch(sink: CapSink) {
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
          version: { major: 1, minor: 2, patch: 0 },
          release(): Promise<'ok' | 'busy' | 'stale'> {
            releases.push(credential);
            return Promise.resolve('ok');
          },
        };
        mintedHandles.push(handle);
        return handle;
      },
      release(credential: LeaseCredential): 'ok' | 'busy' | 'stale' {
        releases.push(credential);
        return 'ok';
      },
    },
    calls: {
      call(credential: LeaseCredential, methodName: string, args: JsonValue): Promise<JsonValue> {
        if (methodName === 'route.add') {
          routeAddAttempts += 1;
          routeAdds.push({ credential, args: args as JsonObject });
          if (options?.failRouteAddOn === routeAddAttempts) {
            return Promise.reject(
              new RackError('failed', `injected route.add failure #${routeAddAttempts}`),
            );
          }
          routeSeq += 1;
          return Promise.resolve({ routeId: `r-${routeSeq}` });
        }
        if (methodName === 'route.remove') {
          routeRemoves.push({ credential, args: args as JsonObject });
          return Promise.resolve({ removed: true });
        }
        return Promise.reject(new RackError('not_found', `unexpected method ${methodName}`));
      },
      callById(credential: LeaseCredential, methodId: number, args: JsonValue): Promise<JsonValue> {
        return Promise.reject(new RackError('not_found', `unexpected method #${methodId}`, args));
      },
    },
    diag: {
      report(level: string, message: string): void {
        diagnostics.push({ level, message });
      },
    },
  };

  return {
    services,
    publishedCaps,
    acquisitions,
    releases,
    routeAdds,
    routeRemoves,
    diagnostics,
    mintedHandles,
    watcherCount(): number {
      return capWatchers.length;
    },
    async deliverCapability(event: CapEvent): Promise<void> {
      for (const sink of [...capWatchers]) await sink.onCapability(event);
    },
  };
}

// ---------------------------------------------------------------------------
// Shared helpers.
// ---------------------------------------------------------------------------

/** @brief std.http-server availability fact for the given provider version. */
function httpAvailable(major = 1, minor = 2, patch = 0): CapEvent {
  return {
    plugId: 'std.http-server',
    available: true,
    version: { major, minor, patch },
    capabilities: {
      methods: [
        { id: 1, name: 'route.add' },
        { id: 2, name: 'route.remove' },
      ],
    },
  };
}

/** @brief std.http-server withdrawal fact; methods are empty on withdrawal. */
function httpWithdrawn(): CapEvent {
  return {
    plugId: 'std.http-server',
    available: false,
    version: { major: 1, minor: 2, patch: 0 },
    capabilities: { methods: [] },
  };
}

/** @brief The full five-slot resource.mount args document reused across assertions. */
const fullMountArgs: JsonObject = {
  pathPrefix: '/api/users',
  targetPlugId: 'example.users',
  methods: {
    list: 'users.list',
    get: 'users.get',
    create: 'users.create',
    update: 'users.update',
    remove: 'users.remove',
  },
  versionRange: {
    minimum: { major: 1, minor: 0, patch: 0 },
    maximum: { major: 1, minor: 9, patch: 9 },
  },
};

/** @brief [verb, path, targetMethod] projection of one route.add record. */
function routeShape(call: RecordedCall): [string, string, string] {
  return [
    String(call.args['httpMethod']),
    String(call.args['path']),
    String(call.args['targetMethod']),
  ];
}

/**
 * @brief Init and start the plugin under test on the given fake harness.
 *
 * @param fake Fake harness built by makeFakeHarness.
 * @return The started plugin instance.
 */
async function startedPlugin(fake: ReturnType<typeof makeFakeHarness>) {
  const plugin = create();
  await plugin.init(fake.services);
  await plugin.start();
  return plugin;
}

/**
 * @brief Init, start and lease the plugin under test (http-server already available).
 *
 * @param fake Fake harness built by makeFakeHarness.
 * @return The started plugin instance holding one lease.
 */
async function leasedPlugin(fake: ReturnType<typeof makeFakeHarness>) {
  const plugin = await startedPlugin(fake);
  await fake.deliverCapability(httpAvailable());
  return plugin;
}

/**
 * @brief Assert that one invoke dispatch throws a RackError with the expected status.
 *
 * @param plugin Plugin under test.
 * @param methodId Numeric method selector.
 * @param methodName Name selector.
 * @param status Expected RackStatus.
 * @param args Request args document.
 */
async function expectInvokeError(
  plugin: Awaited<ReturnType<typeof startedPlugin>>,
  methodId: number,
  methodName: string,
  status: RackStatus,
  args: JsonValue = null,
): Promise<void> {
  try {
    await plugin.invoke?.({ methodId, methodName, args });
    expect.unreachable();
  } catch (error) {
    expect(isRackError(error)).toBe(true);
    expect(statusOf(error)).toBe(status);
  }
}

// ---------------------------------------------------------------------------
// Identity, capability disclosure and lease borrowing.
// ---------------------------------------------------------------------------

describe('std.rest-server identity and capability disclosure', () => {
  it('announces identity, version, init edge and the three methods on start', async () => {
    const fake = makeFakeHarness();
    const plugin = create();
    expect(plugin.desc.plugId).toBe('std.rest-server');
    expect(plugin.desc.version).toEqual({ major: 1, minor: 0, patch: 0 });
    expect(plugin.desc.init.after).toEqual(['std.http-server']);
    await plugin.init(fake.services);
    await plugin.start();
    expect(fake.publishedCaps).toHaveLength(1);
    expect(fake.publishedCaps[0]?.methods.map((method) => [method.id, method.name])).toEqual([
      [1, 'resource.mount'],
      [2, 'resource.unmount'],
      [3, 'mounted.list'],
    ]);
  });

  it('borrows one closed [1.0.0, 1.MAX_SAFE] lease on the http-server availability fact', async () => {
    const fake = makeFakeHarness();
    const plugin = await startedPlugin(fake);
    // An out-of-range offer must not open business.
    await fake.deliverCapability(httpAvailable(2, 0, 0));
    expect(fake.acquisitions).toHaveLength(0);
    await expectInvokeError(plugin, 1, 'resource.mount', 'invalid_state', fullMountArgs);
    // An in-range offer acquires once with the fixed closed range.
    await fake.deliverCapability(httpAvailable());
    expect(fake.acquisitions).toHaveLength(1);
    expect(fake.acquisitions[0]?.plugId).toBe('std.http-server');
    expect(fake.acquisitions[0]?.range.minimum).toEqual({ major: 1, minor: 0, patch: 0 });
    expect(fake.acquisitions[0]?.range.maximum).toEqual({
      major: 1,
      minor: Number.MAX_SAFE_INTEGER,
      patch: Number.MAX_SAFE_INTEGER,
    });
    expect(fake.acquisitions[0]?.revoker).toBeDefined();
    // A still-live lease is never re-borrowed on repeated availability.
    await fake.deliverCapability(httpAvailable());
    expect(fake.acquisitions).toHaveLength(1);
  });

  it('forbids mounting after withdrawal while the credential is kept for revocation', async () => {
    const fake = makeFakeHarness();
    const plugin = await startedPlugin(fake);
    await fake.deliverCapability(httpAvailable());
    await fake.deliverCapability(httpWithdrawn());
    await expectInvokeError(plugin, 1, 'resource.mount', 'invalid_state', fullMountArgs);
    expect(fake.releases).toHaveLength(0);
  });
});

// ---------------------------------------------------------------------------
// resource.mount: convention translation through the gateway.
// ---------------------------------------------------------------------------

describe('resource.mount', () => {
  it('registers all five conventional routes via route.add under the lease credential', async () => {
    const fake = makeFakeHarness();
    const plugin = await leasedPlugin(fake);
    const result = await plugin.invoke?.({ methodId: 1, methodName: 'resource.mount', args: fullMountArgs });
    expect(result).toEqual({ mounted: true });
    expect(fake.routeAdds.map(routeShape)).toEqual([
      ['GET', '/api/users', 'users.list'],
      ['GET', '/api/users/:id', 'users.get'],
      ['POST', '/api/users', 'users.create'],
      ['PUT', '/api/users/:id', 'users.update'],
      ['DELETE', '/api/users/:id', 'users.remove'],
    ]);
    for (const call of fake.routeAdds) {
      expect(call.args['targetPlugId']).toBe('example.users');
      expect(call.args['versionRange']).toEqual(fullMountArgs['versionRange']);
      expect(call.credential).toBe(fake.mintedHandles[0]?.credential);
    }
  });

  it('mounts only the provided slots when the declaration omits methods', async () => {
    const fake = makeFakeHarness();
    const plugin = await leasedPlugin(fake);
    await plugin.invoke?.({
      methodId: 1,
      methodName: 'resource.mount',
      args: {
        pathPrefix: '/api/books',
        targetPlugId: 'example.books',
        methods: { list: 'books.list', get: 'books.get' },
      },
    });
    expect(fake.routeAdds.map(routeShape)).toEqual([
      ['GET', '/api/books', 'books.list'],
      ['GET', '/api/books/:id', 'books.get'],
    ]);
    // The omitted versionRange is not injected into the gateway documents.
    expect(fake.routeAdds.every((call) => call.args['versionRange'] === undefined)).toBe(true);
  });

  it('normalizes trailing slashes and reports the mount through mounted.list', async () => {
    const fake = makeFakeHarness();
    const plugin = await leasedPlugin(fake);
    await plugin.invoke?.({
      methodId: 1,
      methodName: 'resource.mount',
      args: { ...fullMountArgs, pathPrefix: '/api/users/' },
    });
    expect(fake.routeAdds.map(routeShape)).toEqual([
      ['GET', '/api/users', 'users.list'],
      ['GET', '/api/users/:id', 'users.get'],
      ['POST', '/api/users', 'users.create'],
      ['PUT', '/api/users/:id', 'users.update'],
      ['DELETE', '/api/users/:id', 'users.remove'],
    ]);
    const listing = await plugin.invoke?.({ methodId: 3, methodName: 'mounted.list', args: null });
    expect(listing).toEqual([
      {
        pathPrefix: '/api/users',
        targetPlugId: 'example.users',
        methods: fullMountArgs['methods'],
        versionRange: fullMountArgs['versionRange'],
      },
    ]);
  });

  it('rejects a repeated path prefix with duplicate and no extra route.add calls', async () => {
    const fake = makeFakeHarness();
    const plugin = await leasedPlugin(fake);
    await plugin.invoke?.({ methodId: 1, methodName: 'resource.mount', args: fullMountArgs });
    await expectInvokeError(plugin, 1, 'resource.mount', 'duplicate', fullMountArgs);
    // The normalized form is the duplicate key, not the raw string.
    await expectInvokeError(plugin, 1, 'resource.mount', 'duplicate', {
      ...fullMountArgs,
      pathPrefix: '/api/users/',
    });
    expect(fake.routeAdds).toHaveLength(5);
  });

  it('rejects mounting without a usable lease with invalid_state', async () => {
    const fake = makeFakeHarness();
    const plugin = await startedPlugin(fake);
    await expectInvokeError(plugin, 1, 'resource.mount', 'invalid_state', fullMountArgs);
    expect(fake.routeAdds).toHaveLength(0);
  });

  it('rejects malformed declarations with invalid_argument', async () => {
    const fake = makeFakeHarness();
    const plugin = await leasedPlugin(fake);
    await expectInvokeError(plugin, 1, 'resource.mount', 'invalid_argument', null);
    await expectInvokeError(plugin, 1, 'resource.mount', 'invalid_argument', 'nope');
    await expectInvokeError(plugin, 1, 'resource.mount', 'invalid_argument', {
      ...fullMountArgs,
      pathPrefix: 'api/users',
    });
    await expectInvokeError(plugin, 1, 'resource.mount', 'invalid_argument', {
      ...fullMountArgs,
      targetPlugId: '',
    });
    await expectInvokeError(plugin, 1, 'resource.mount', 'invalid_argument', {
      ...fullMountArgs,
      methods: {},
    });
    await expectInvokeError(plugin, 1, 'resource.mount', 'invalid_argument', {
      ...fullMountArgs,
      methods: { list: 'users.list', patch: 'users.patch' },
    });
    await expectInvokeError(plugin, 1, 'resource.mount', 'invalid_argument', {
      ...fullMountArgs,
      methods: { list: 7 },
    });
    await expectInvokeError(plugin, 1, 'resource.mount', 'invalid_argument', {
      ...fullMountArgs,
      versionRange: {
        minimum: { major: 1, minor: 9, patch: 0 },
        maximum: { major: 1, minor: 0, patch: 0 },
      },
    });
    expect(fake.routeAdds).toHaveLength(0);
    expect(fake.routeRemoves).toHaveLength(0);
  });

  it('rolls back already-added routes when one route.add fails', async () => {
    const fake = makeFakeHarness({ failRouteAddOn: 3 });
    const plugin = await leasedPlugin(fake);
    await expectInvokeError(plugin, 1, 'resource.mount', 'failed', fullMountArgs);
    expect(fake.routeAdds).toHaveLength(3);
    expect(fake.routeRemoves.map((call) => call.args['routeId'])).toEqual(['r-1', 'r-2']);
    expect(await plugin.invoke?.({ methodId: 3, methodName: 'mounted.list', args: null })).toEqual(
      [],
    );
    // The failed prefix is free again: a smaller declaration succeeds (attempts 4 and 5).
    await plugin.invoke?.({
      methodId: 1,
      methodName: 'resource.mount',
      args: { pathPrefix: '/api/users', targetPlugId: 'example.users', methods: { list: 'users.list' } },
    });
    expect(fake.routeAdds).toHaveLength(4);
    expect(await plugin.invoke?.({ methodId: 3, methodName: 'mounted.list', args: null })).toEqual([
      {
        pathPrefix: '/api/users',
        targetPlugId: 'example.users',
        methods: { list: 'users.list' },
      },
    ]);
  });
});

// ---------------------------------------------------------------------------
// resource.unmount.
// ---------------------------------------------------------------------------

describe('resource.unmount', () => {
  it('removes every recorded route id through route.remove and empties the listing', async () => {
    const fake = makeFakeHarness();
    const plugin = await leasedPlugin(fake);
    await plugin.invoke?.({ methodId: 1, methodName: 'resource.mount', args: fullMountArgs });
    const result = await plugin.invoke?.({
      methodId: 2,
      methodName: 'resource.unmount',
      args: { pathPrefix: '/api/users' },
    });
    expect(result).toEqual({ unmounted: true });
    expect(fake.routeRemoves.map((call) => call.args['routeId'])).toEqual([
      'r-1',
      'r-2',
      'r-3',
      'r-4',
      'r-5',
    ]);
    for (const call of fake.routeRemoves) {
      expect(call.credential).toBe(fake.mintedHandles[0]?.credential);
    }
    expect(await plugin.invoke?.({ methodId: 3, methodName: 'mounted.list', args: null })).toEqual(
      [],
    );
    // Unmounting an unknown but well-formed prefix fails with not_found; a malformed args
    // document fails validation with invalid_argument before the lookup happens.
    await expectInvokeError(plugin, 2, 'resource.unmount', 'not_found', { pathPrefix: '/api/users' });
    await expectInvokeError(plugin, 2, 'resource.unmount', 'not_found', {
      pathPrefix: '/api/unknown',
    });
    await expectInvokeError(plugin, 2, 'resource.unmount', 'invalid_argument', null);
    await expectInvokeError(plugin, 2, 'resource.unmount', 'invalid_argument', {
      pathPrefix: 'api/users',
    });
  });

  it('drops bookkeeping only when no usable lease remains after a revocation', async () => {
    const fake = makeFakeHarness();
    const plugin = await leasedPlugin(fake);
    await plugin.invoke?.({
      methodId: 1,
      methodName: 'resource.mount',
      args: { pathPrefix: '/api/users', targetPlugId: 'example.users', methods: { list: 'users.list' } },
    });
    const credential = fake.mintedHandles[0]?.credential;
    await fake.acquisitions[0]?.revoker.onRevoke(credential as LeaseCredential);
    expect(fake.releases).toEqual([credential]);
    const result = await plugin.invoke?.({
      methodId: 2,
      methodName: 'resource.unmount',
      args: { pathPrefix: '/api/users' },
    });
    expect(result).toEqual({ unmounted: true });
    expect(fake.routeRemoves).toHaveLength(0);
    expect(await plugin.invoke?.({ methodId: 3, methodName: 'mounted.list', args: null })).toEqual(
      [],
    );
  });
});

// ---------------------------------------------------------------------------
// Revocation, re-borrowing and stop.
// ---------------------------------------------------------------------------

describe('revocation, re-borrowing and stop', () => {
  it('clears the cache on revoke, re-borrows on the next availability and re-mounts', async () => {
    const fake = makeFakeHarness();
    const plugin = await leasedPlugin(fake);
    await plugin.invoke?.({
      methodId: 1,
      methodName: 'resource.mount',
      args: { pathPrefix: '/api/users', targetPlugId: 'example.users', methods: { list: 'users.list', get: 'users.get' } },
    });
    expect(fake.routeAdds).toHaveLength(2);
    // Revoke clears the cached lease and returns the credential; routes die with the provider.
    const firstCredential = fake.mintedHandles[0]?.credential;
    await fake.acquisitions[0]?.revoker.onRevoke(firstCredential as LeaseCredential);
    expect(fake.releases).toEqual([firstCredential]);
    expect(fake.routeRemoves).toHaveLength(0);
    // The next in-range availability re-borrows and re-mounts the surviving resource.
    await fake.deliverCapability(httpAvailable());
    expect(fake.acquisitions).toHaveLength(2);
    expect(fake.routeAdds).toHaveLength(4);
    expect(fake.routeAdds.slice(2).map(routeShape)).toEqual([
      ['GET', '/api/users', 'users.list'],
      ['GET', '/api/users/:id', 'users.get'],
    ]);
    for (const call of fake.routeAdds.slice(2)) {
      expect(call.credential).toBe(fake.mintedHandles[1]?.credential);
    }
    expect(await plugin.invoke?.({ methodId: 3, methodName: 'mounted.list', args: null })).toEqual([
      {
        pathPrefix: '/api/users',
        targetPlugId: 'example.users',
        methods: { list: 'users.list', get: 'users.get' },
      },
    ]);
    // An unrelated credential is ignored by the revoker.
    await fake.acquisitions[0]?.revoker.onRevoke({
      kind: 'u42.lease.credential',
      handle: { seq: 'other' },
    });
    expect(fake.releases).toHaveLength(1);
  });

  it('unmounts every resource and returns the lease on stop', async () => {
    const fake = makeFakeHarness();
    const plugin = await leasedPlugin(fake);
    await plugin.invoke?.({ methodId: 1, methodName: 'resource.mount', args: fullMountArgs });
    await plugin.invoke?.({
      methodId: 1,
      methodName: 'resource.mount',
      args: { pathPrefix: '/api/books', targetPlugId: 'example.books', methods: { list: 'books.list' } },
    });
    const credential = fake.mintedHandles[0]?.credential;
    await plugin.stop();
    expect(fake.routeRemoves.map((call) => call.args['routeId'])).toEqual([
      'r-1',
      'r-2',
      'r-3',
      'r-4',
      'r-5',
      'r-6',
    ]);
    expect(fake.releases).toEqual([credential]);
    expect(fake.watcherCount()).toBe(0);
    expect(await plugin.invoke?.({ methodId: 3, methodName: 'mounted.list', args: null })).toEqual(
      [],
    );
  });

  it('rejects unknown methods with not_found', async () => {
    const fake = makeFakeHarness();
    const plugin = await leasedPlugin(fake);
    await expectInvokeError(plugin, 99, 'resource.patch', 'not_found');
  });
});
