/**
 * @file Tests for std.rest-client: RESTful conventions over a leased std.http-client.
 *
 * @brief All PluginServices are self-written fakes: acquire() mints fake LeaseHandles pointing
 *        at std.http-client and the fake CallService records every request call. No host
 *        implementation or other agent module is imported.
 */
import { describe, expect, it } from 'vitest';
import { create } from '../../src/plugins/rest-client/index.ts';
import { create as createDemo } from '../../examples/plugins/rest-client-demo.plug.ts';
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
  RackPlugin,
  Revoker,
  VersionRange,
} from '../../src/core/types.ts';

// ---------------------------------------------------------------------------
// Self-written fake PluginServices: records publications, capability watchers, lease
// acquisitions, request gateway calls and diagnostics.
// ---------------------------------------------------------------------------

/** @brief One recorded CapabilityService.acquire attempt. */
interface RecordedAcquire {
  plugId: string;
  range: VersionRange;
  revoker: Revoker;
}

/** @brief One recorded gateway call with its credential and args document. */
interface RecordedCall {
  credential: LeaseCredential;
  methodName: string;
  args: JsonValue;
}

/** @brief Programmable results handed back by the fake CallService, in FIFO order. */
type QueuedResult = JsonValue | RackError;

/**
 * @brief Build a fake PluginServices bundle backed by plain records.
 *
 * @return The services object plus record arrays, a result queue and delivery helpers.
 * @note Gateway calls resolve with the next queued result, or a default 200/JSON envelope
 *       once the queue runs dry; queued RackError values reject instead.
 */
function makeFakeServices() {
  const publishedCaps: CapsDesc[] = [];
  const capWatchers: CapSink[] = [];
  const acquisitions: RecordedAcquire[] = [];
  const releases: LeaseCredential[] = [];
  const calls: RecordedCall[] = [];
  const diagnostics: Array<{ level: string; message: string }> = [];
  const mintedHandles: LeaseHandle[] = [];
  const queuedResults: QueuedResult[] = [];
  let credentialSeq = 0;

  const services: PluginServices = {
    plugId: 'test.rest-client',
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
          version: { major: 1, minor: 0, patch: 0 },
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
        calls.push({ credential, methodName, args });
        const next = queuedResults.shift();
        if (next instanceof RackError) return Promise.reject(next);
        if (next !== undefined) return Promise.resolve(next);
        return Promise.resolve({ status: 200, ok: true, headers: {}, json: { default: true } });
      },
      callById(credential: LeaseCredential, methodId: number, args: JsonValue): Promise<JsonValue> {
        calls.push({ credential, methodName: `#${methodId}`, args });
        return Promise.resolve(null);
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
    calls,
    diagnostics,
    mintedHandles,
    queuedResults,
    watcherCount(): number {
      return capWatchers.length;
    },
    enqueue(result: QueuedResult): void {
      queuedResults.push(result);
    },
    async deliverCapability(event: CapEvent): Promise<void> {
      for (const sink of [...capWatchers]) await sink.onCapability(event);
    },
  };
}

// ---------------------------------------------------------------------------
// Shared helpers.
// ---------------------------------------------------------------------------

/** @brief The closed range std.rest-client must borrow std.http-client on. */
const EXPECTED_RANGE: VersionRange = {
  minimum: { major: 1, minor: 0, patch: 0 },
  maximum: { major: 1, minor: Number.MAX_SAFE_INTEGER, patch: Number.MAX_SAFE_INTEGER },
};

/**
 * @brief std.http-client availability fact for the given provider version.
 *
 * @param major Major component of the disclosed provider version.
 * @param minor Minor component of the disclosed provider version.
 * @param patch Patch component of the disclosed provider version.
 */
function httpAvailable(major = 1, minor = 0, patch = 0): CapEvent {
  return {
    plugId: 'std.http-client',
    available: true,
    version: { major, minor, patch },
    capabilities: {
      methods: [
        { id: 1, name: 'request' },
        { id: 2, name: 'get' },
      ],
    },
  };
}

/** @brief std.http-client withdrawal fact; methods are empty on withdrawal. */
function httpWithdrawn(): CapEvent {
  return {
    plugId: 'std.http-client',
    available: false,
    version: { major: 1, minor: 0, patch: 0 },
    capabilities: { methods: [] },
  };
}

/** @brief std.rest-client availability fact reused by the demo test. */
function restClientAvailable(): CapEvent {
  return {
    plugId: 'std.rest-client',
    available: true,
    version: { major: 1, minor: 0, patch: 0 },
    capabilities: {
      methods: [
        { id: 1, name: 'resource' },
        { id: 2, name: 'crud' },
      ],
    },
  };
}

/** @brief A typical resource request document reused across forwarding assertions. */
const LIST_THINGS_ARGS: JsonObject = {
  baseUrl: 'https://api.example.com/v1',
  path: '/things',
  query: { page: 2, q: 'hello world' },
};

/**
 * @brief Init and start the plugin under test on the given fake harness.
 *
 * @param fake Fake harness built by makeFakeServices.
 * @return The started plugin instance (no lease yet).
 */
async function startedPlugin(fake: ReturnType<typeof makeFakeServices>): Promise<RackPlugin> {
  const plugin = create();
  await plugin.init(fake.services);
  await plugin.start();
  return plugin;
}

/**
 * @brief Init, start and lease the plugin under test (http-client already available).
 *
 * @param fake Fake harness built by makeFakeServices.
 * @return The started plugin instance holding one lease.
 */
async function leasedPlugin(fake: ReturnType<typeof makeFakeServices>): Promise<RackPlugin> {
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
  plugin: RackPlugin,
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

/** @brief The args document of the last recorded gateway call, as a JSON object. */
function lastCallArgs(fake: ReturnType<typeof makeFakeServices>): JsonObject {
  const last = fake.calls.at(-1);
  if (last === undefined) throw new Error('no gateway call recorded');
  return last.args as JsonObject;
}

// ---------------------------------------------------------------------------
// Capability watching and lease timing.
// ---------------------------------------------------------------------------

describe('watch and lease timing', () => {
  it('watches std.http-client during init and acquires once it appears', async () => {
    const fake = makeFakeServices();
    const plugin = create();
    await plugin.init(fake.services);
    // Watching is not business work: the subscription exists right after init.
    expect(fake.watcherCount()).toBe(1);
    expect(fake.acquisitions).toHaveLength(0);
    expect(fake.publishedCaps).toHaveLength(0);

    await fake.deliverCapability(httpAvailable(1, 0, 0));
    expect(fake.acquisitions).toHaveLength(1);
    expect(fake.acquisitions[0]?.plugId).toBe('std.http-client');
    expect(fake.acquisitions[0]?.range).toEqual(EXPECTED_RANGE);
  });

  it('starts publishing exactly the resource and crud methods', async () => {
    const fake = makeFakeServices();
    await startedPlugin(fake);
    expect(fake.publishedCaps).toHaveLength(1);
    const methods = fake.publishedCaps[0]?.methods ?? [];
    expect(methods.map((method) => [method.id, method.name])).toEqual([
      [1, 'resource'],
      [2, 'crud'],
    ]);
  });

  it('never re-borrows while the live lease is still accepted', async () => {
    const fake = makeFakeServices();
    await leasedPlugin(fake);
    await fake.deliverCapability(httpAvailable(1, 2, 3));
    await fake.deliverCapability(httpAvailable(1, 9, 9));
    expect(fake.acquisitions).toHaveLength(1);
  });

  it('keeps the credential but blocks business on withdrawal', async () => {
    const fake = makeFakeServices();
    const plugin = await leasedPlugin(fake);
    await fake.deliverCapability(httpWithdrawn());
    expect(fake.acquisitions).toHaveLength(1);
    expect(fake.releases).toHaveLength(0);
    await expectInvokeError(plugin, 1, 'resource', 'invalid_state', LIST_THINGS_ARGS);
  });
});

// ---------------------------------------------------------------------------
// Version filtering of the closed range [1.0.0, 1.MAX_SAFE].
// ---------------------------------------------------------------------------

describe('version range filtering', () => {
  it('rejects versions outside the closed range without borrowing', async () => {
    const fake = makeFakeServices();
    const plugin = await startedPlugin(fake);
    await fake.deliverCapability(httpAvailable(0, 9, 9));
    expect(fake.acquisitions).toHaveLength(0);
    await fake.deliverCapability(httpAvailable(2, 0, 0));
    expect(fake.acquisitions).toHaveLength(0);
    // Out-of-range facts also mark a not-yet-borrowed provider unusable.
    await expectInvokeError(plugin, 1, 'resource', 'invalid_state', LIST_THINGS_ARGS);
  });

  it('borrows on both range endpoints and any 1.x.y between them', async () => {
    const fake = makeFakeServices();
    await startedPlugin(fake);
    await fake.deliverCapability(httpAvailable(1, 0, 0));
    expect(fake.acquisitions).toHaveLength(1);
  });

  it('borrows every 1.x.y up to the maximum safe triple', async () => {
    const fake = makeFakeServices();
    await startedPlugin(fake);
    await fake.deliverCapability(httpAvailable(1, Number.MAX_SAFE_INTEGER, 3));
    expect(fake.acquisitions).toHaveLength(1);
  });

  it('marks a held lease unusable when an out-of-range fact arrives', async () => {
    const fake = makeFakeServices();
    const plugin = await leasedPlugin(fake);
    await fake.deliverCapability(httpAvailable(2, 0, 0));
    await expectInvokeError(plugin, 1, 'resource', 'invalid_state', LIST_THINGS_ARGS);
  });
});

// ---------------------------------------------------------------------------
// typedApi forwarding: method name, packed args and credential.
// ---------------------------------------------------------------------------

describe('typedApi forwarding to std.http-client request', () => {
  it('forwards the method name, packed args and borrowed credential', async () => {
    const fake = makeFakeServices();
    const plugin = await leasedPlugin(fake);
    const handle = fake.mintedHandles[0];
    if (handle === undefined) throw new Error('no lease minted');

    const result = await plugin.invoke?.({
      methodId: 1,
      methodName: 'resource',
      args: LIST_THINGS_ARGS,
    });
    expect(fake.calls).toHaveLength(1);
    expect(fake.calls[0]?.credential).toBe(handle.credential);
    expect(fake.calls[0]?.methodName).toBe('request');
    expect(fake.calls[0]?.args).toEqual({
      url: 'https://api.example.com/v1/things?page=2&q=hello+world',
      method: 'GET',
      expectOk: true,
    });
    // The target JSON body of the fake envelope is returned.
    expect(result).toEqual({ default: true });
  });

  it('resolves the crud method through the numeric id entry as well', async () => {
    const fake = makeFakeServices();
    const plugin = await leasedPlugin(fake);
    await plugin.invoke?.({
      methodId: 2,
      methodName: 'wrong-name-on-purpose',
      args: { baseUrl: 'https://api.example.com/v1', resource: 'things' },
    });
    expect(fake.calls).toHaveLength(1);
    expect(fake.calls[0]?.methodName).toBe('request');
    expect(lastCallArgs(fake)['url']).toBe('https://api.example.com/v1/things');
  });

  it('forwards headers, json body and expectOk=false verbatim', async () => {
    const fake = makeFakeServices();
    const plugin = await leasedPlugin(fake);
    await plugin.invoke?.({
      methodId: 1,
      methodName: 'resource',
      args: {
        baseUrl: 'https://api.example.com/v1',
        path: '/things',
        method: 'PATCH',
        body: { name: 'first', tags: ['a', null] },
        headers: { 'x-trace': 't1' },
        expectOk: false,
      },
    });
    expect(fake.calls[0]?.args).toEqual({
      url: 'https://api.example.com/v1/things',
      method: 'PATCH',
      expectOk: false,
      headers: { 'x-trace': 't1' },
      json: { name: 'first', tags: ['a', null] },
    });
  });

  it('returns null when the response envelope carries no JSON body', async () => {
    const fake = makeFakeServices();
    const plugin = await leasedPlugin(fake);
    fake.enqueue({ status: 204, ok: true, headers: {}, text: '' });
    const result = await plugin.invoke?.({
      methodId: 1,
      methodName: 'resource',
      args: { baseUrl: 'https://api.example.com/v1', path: '/things', method: 'DELETE' },
    });
    expect(result).toBeNull();
  });

  it('propagates gateway failures such as expectOk failures untouched', async () => {
    const fake = makeFakeServices();
    const plugin = await leasedPlugin(fake);
    fake.enqueue(new RackError('failed', 'GET https://api.example.com returned HTTP 500'));
    try {
      await plugin.invoke?.({ methodId: 1, methodName: 'resource', args: LIST_THINGS_ARGS });
      expect.unreachable();
    } catch (error) {
      expect(statusOf(error)).toBe('failed');
    }
  });
});

// ---------------------------------------------------------------------------
// Query serialization and URL construction.
// ---------------------------------------------------------------------------

describe('query serialization and URL construction', () => {
  it('serializes string, number and boolean query values with URLSearchParams', async () => {
    const fake = makeFakeServices();
    const plugin = await leasedPlugin(fake);
    await plugin.invoke?.({
      methodId: 1,
      methodName: 'resource',
      args: {
        baseUrl: 'https://api.example.com/v1/things',
        query: { page: 2, q: 'hello world', deep: true, active: false, size: 0 },
      },
    });
    expect(lastCallArgs(fake)['url']).toBe(
      'https://api.example.com/v1/things?page=2&q=hello+world&deep=true&active=false&size=0',
    );
  });

  it('percent-encodes each path segment while preserving separators', async () => {
    const fake = makeFakeServices();
    const plugin = await leasedPlugin(fake);
    await plugin.invoke?.({
      methodId: 1,
      methodName: 'resource',
      args: { baseUrl: 'https://api.example.com/v1/', path: 'user groups/a b' },
    });
    expect(lastCallArgs(fake)['url']).toBe('https://api.example.com/v1/user%20groups/a%20b');
  });

  it('keeps the baseUrl query when no query object is provided', async () => {
    const fake = makeFakeServices();
    const plugin = await leasedPlugin(fake);
    await plugin.invoke?.({
      methodId: 1,
      methodName: 'resource',
      args: { baseUrl: 'https://api.example.com/v1?token=abc', path: '/things' },
    });
    expect(lastCallArgs(fake)['url']).toBe('https://api.example.com/v1/things?token=abc');
  });

  it('replaces the baseUrl query when a query object is provided', async () => {
    const fake = makeFakeServices();
    const plugin = await leasedPlugin(fake);
    await plugin.invoke?.({
      methodId: 1,
      methodName: 'resource',
      args: { baseUrl: 'https://api.example.com/v1?token=abc', path: '/things', query: {} },
    });
    expect(lastCallArgs(fake)['url']).toBe('https://api.example.com/v1/things');
  });

  it('rejects malformed URLs and non-scalar query values', async () => {
    const fake = makeFakeServices();
    const plugin = await leasedPlugin(fake);
    await expectInvokeError(
      plugin,
      1,
      'resource',
      'invalid_argument',
      { baseUrl: 'not a url' },
    );
    await expectInvokeError(plugin, 1, 'resource', 'invalid_argument', {
      baseUrl: 'https://api.example.com',
      query: { bad: { nested: true } },
    });
    await expectInvokeError(plugin, 1, 'resource', 'invalid_argument', {
      baseUrl: 'https://api.example.com',
      query: { bad: ['a'] },
    });
    await expectInvokeError(plugin, 1, 'resource', 'invalid_argument', {
      baseUrl: 'https://api.example.com',
      headers: { 'x-trace': 7 },
    });
    await expectInvokeError(plugin, 1, 'resource', 'invalid_argument', {
      baseUrl: 'https://api.example.com',
      expectOk: 'yes',
    });
    await expectInvokeError(plugin, 1, 'resource', 'invalid_argument', 'plain string');
  });
});

// ---------------------------------------------------------------------------
// crud: the five conventional operations.
// ---------------------------------------------------------------------------

describe('crud conventional mapping', () => {
  it('lists the collection with GET on the resource path', async () => {
    const fake = makeFakeServices();
    const plugin = await leasedPlugin(fake);
    await plugin.invoke?.({
      methodId: 2,
      methodName: 'crud',
      args: { baseUrl: 'https://api.example.com/v1', resource: 'things', query: { page: '2' } },
    });
    expect(fake.calls[0]?.args).toEqual({
      url: 'https://api.example.com/v1/things?page=2',
      method: 'GET',
      expectOk: true,
    });
  });

  it('gets one item with GET on the resource/id path', async () => {
    const fake = makeFakeServices();
    const plugin = await leasedPlugin(fake);
    await plugin.invoke?.({
      methodId: 2,
      methodName: 'crud',
      args: { baseUrl: 'https://api.example.com/v1', resource: 'things', id: 't1' },
    });
    expect(fake.calls[0]?.args).toEqual({
      url: 'https://api.example.com/v1/things/t1',
      method: 'GET',
      expectOk: true,
    });
  });

  it('creates with POST and the item as the json body', async () => {
    const fake = makeFakeServices();
    const plugin = await leasedPlugin(fake);
    await plugin.invoke?.({
      methodId: 2,
      methodName: 'crud',
      args: { baseUrl: 'https://api.example.com/v1', resource: 'things', item: { name: 'first' } },
    });
    expect(fake.calls[0]?.args).toEqual({
      url: 'https://api.example.com/v1/things',
      method: 'POST',
      expectOk: true,
      json: { name: 'first' },
    });
  });

  it('updates with PUT on the resource/id path and the item as the json body', async () => {
    const fake = makeFakeServices();
    const plugin = await leasedPlugin(fake);
    await plugin.invoke?.({
      methodId: 2,
      methodName: 'crud',
      args: {
        baseUrl: 'https://api.example.com/v1',
        resource: 'things',
        id: 't1',
        item: { name: 'renamed' },
      },
    });
    expect(fake.calls[0]?.args).toEqual({
      url: 'https://api.example.com/v1/things/t1',
      method: 'PUT',
      expectOk: true,
      json: { name: 'renamed' },
    });
  });

  it('removes with an explicit DELETE verb on the resource/id path', async () => {
    const fake = makeFakeServices();
    const plugin = await leasedPlugin(fake);
    await plugin.invoke?.({
      methodId: 2,
      methodName: 'crud',
      args: { baseUrl: 'https://api.example.com/v1', resource: 'things', id: 't1', method: 'DELETE' },
    });
    expect(fake.calls[0]?.args).toEqual({
      url: 'https://api.example.com/v1/things/t1',
      method: 'DELETE',
      expectOk: true,
    });
  });

  it('percent-encodes resource names and ids, including numeric ids', async () => {
    const fake = makeFakeServices();
    const plugin = await leasedPlugin(fake);
    await plugin.invoke?.({
      methodId: 2,
      methodName: 'crud',
      args: { baseUrl: 'https://api.example.com/v1', resource: 'user groups', id: 7 },
    });
    expect(lastCallArgs(fake)['url']).toBe('https://api.example.com/v1/user%20groups/7');
    await plugin.invoke?.({
      methodId: 2,
      methodName: 'crud',
      args: { baseUrl: 'https://api.example.com/v1', resource: 'things', id: 'a/b&c' },
    });
    expect(lastCallArgs(fake)['url']).toBe('https://api.example.com/v1/things/a%2Fb%26c');
  });

  it('honours explicit verbs case-insensitively and keeps lowercase defaults off', async () => {
    const fake = makeFakeServices();
    const plugin = await leasedPlugin(fake);
    await plugin.invoke?.({
      methodId: 2,
      methodName: 'crud',
      args: {
        baseUrl: 'https://api.example.com/v1',
        resource: 'things',
        id: 't1',
        method: 'delete',
      },
    });
    expect(lastCallArgs(fake)['method']).toBe('DELETE');
  });

  it('rejects field combinations that contradict the conventions', async () => {
    const fake = makeFakeServices();
    const plugin = await leasedPlugin(fake);
    // POST with an id has no conventional URL.
    await expectInvokeError(plugin, 2, 'crud', 'invalid_argument', {
      baseUrl: 'https://api.example.com/v1',
      resource: 'things',
      id: 't1',
      item: { name: 'x' },
      method: 'POST',
    });
    // PUT requires both id and item.
    await expectInvokeError(plugin, 2, 'crud', 'invalid_argument', {
      baseUrl: 'https://api.example.com/v1',
      resource: 'things',
      id: 't1',
      method: 'PUT',
    });
    // DELETE requires an id.
    await expectInvokeError(plugin, 2, 'crud', 'invalid_argument', {
      baseUrl: 'https://api.example.com/v1',
      resource: 'things',
      method: 'DELETE',
    });
    // GET cannot carry an item body.
    await expectInvokeError(plugin, 2, 'crud', 'invalid_argument', {
      baseUrl: 'https://api.example.com/v1',
      resource: 'things',
      item: { name: 'x' },
      method: 'GET',
    });
    // Unknown verbs are not part of the CRUD conventions.
    await expectInvokeError(plugin, 2, 'crud', 'invalid_argument', {
      baseUrl: 'https://api.example.com/v1',
      resource: 'things',
      method: 'PATCH',
    });
    // Missing required basics.
    await expectInvokeError(plugin, 2, 'crud', 'invalid_argument', {
      baseUrl: 'https://api.example.com/v1',
    });
    await expectInvokeError(plugin, 2, 'crud', 'invalid_argument', {
      baseUrl: 'https://api.example.com/v1',
      resource: 'things',
      id: '',
    });
  });

  it('rejects unknown methods with not_found', async () => {
    const fake = makeFakeServices();
    const plugin = await leasedPlugin(fake);
    await expectInvokeError(plugin, 9, 'nope', 'not_found');
  });
});

// ---------------------------------------------------------------------------
// Missing lease, revocation and stop.
// ---------------------------------------------------------------------------

describe('lease lifecycle', () => {
  it('rejects both methods with invalid_state while std.http-client is offline', async () => {
    const fake = makeFakeServices();
    const plugin = await startedPlugin(fake);
    await expectInvokeError(plugin, 1, 'resource', 'invalid_state', LIST_THINGS_ARGS);
    await expectInvokeError(plugin, 2, 'crud', 'invalid_state', {
      baseUrl: 'https://api.example.com/v1',
      resource: 'things',
    });
    expect(fake.calls).toHaveLength(0);
  });

  it('clears the lease on revocation and re-borrows on the next accepted fact', async () => {
    const fake = makeFakeServices();
    const plugin = await leasedPlugin(fake);
    const first = fake.mintedHandles[0];
    const revoker = fake.acquisitions[0]?.revoker;
    if (first === undefined || revoker === undefined) throw new Error('no lease minted');

    await revoker.onRevoke(first.credential);
    expect(fake.releases).toEqual([first.credential]);
    await expectInvokeError(plugin, 1, 'resource', 'invalid_state', LIST_THINGS_ARGS);

    // The next availability fact borrows a fresh credential...
    await fake.deliverCapability(httpAvailable(1, 1, 0));
    expect(fake.acquisitions).toHaveLength(2);
    const second = fake.mintedHandles[1];
    if (second === undefined) throw new Error('no second lease minted');
    // ...and business calls travel under the new credential.
    await plugin.invoke?.({ methodId: 1, methodName: 'resource', args: LIST_THINGS_ARGS });
    expect(fake.calls.at(-1)?.credential).toBe(second.credential);
    expect(fake.calls).toHaveLength(1);
  });

  it('ignores revocations of unrelated credentials', async () => {
    const fake = makeFakeServices();
    const plugin = await leasedPlugin(fake);
    const revoker = fake.acquisitions[0]?.revoker;
    if (revoker === undefined) throw new Error('no lease minted');
    await revoker.onRevoke({
      kind: 'u42.lease.credential',
      handle: { seq: 'unrelated' },
    });
    expect(fake.releases).toHaveLength(0);
    // The held lease still works.
    await plugin.invoke?.({ methodId: 1, methodName: 'resource', args: LIST_THINGS_ARGS });
    expect(fake.calls).toHaveLength(1);
  });

  it('unsubscribes and returns the lease on stop', async () => {
    const fake = makeFakeServices();
    const plugin = await leasedPlugin(fake);
    const handle = fake.mintedHandles[0];
    if (handle === undefined) throw new Error('no lease minted');
    await plugin.stop();
    expect(fake.releases).toEqual([handle.credential]);
    expect(fake.watcherCount()).toBe(0);
    // Late availability facts reach nobody; nothing is borrowed again.
    await fake.deliverCapability(httpAvailable());
    expect(fake.acquisitions).toHaveLength(1);
    expect(fake.calls).toHaveLength(0);
  });
});

// ---------------------------------------------------------------------------
// Example demo plugin: five crud calls in a fake service environment.
// ---------------------------------------------------------------------------

describe('rest-client-demo example', () => {
  it('runs create, list, get, update and remove through the leased rest-client', async () => {
    const fake = makeFakeServices();
    fake.enqueue({ id: 't1', name: 'demo thing' });
    fake.enqueue([{ id: 't1', name: 'demo thing' }]);
    fake.enqueue({ id: 't1', name: 'demo thing' });
    fake.enqueue({ id: 't1', name: 'renamed' });
    fake.enqueue(null);

    const demo = createDemo();
    await demo.init(fake.services);
    await demo.start();
    await fake.deliverCapability(restClientAvailable());
    expect(fake.acquisitions).toHaveLength(1);

    const report = await demo.invoke?.({
      methodId: 1,
      methodName: 'demo-crud',
      args: { baseUrl: 'https://api.example.com/v1' },
    });
    expect(report).toEqual([
      { op: 'create', result: { id: 't1', name: 'demo thing' } },
      { op: 'list', result: [{ id: 't1', name: 'demo thing' }] },
      { op: 'get', result: { id: 't1', name: 'demo thing' } },
      { op: 'update', result: { id: 't1', name: 'renamed' } },
      { op: 'remove', result: null },
    ]);
    expect(fake.calls.map((call) => call.methodName)).toEqual([
      'crud',
      'crud',
      'crud',
      'crud',
      'crud',
    ]);
    // The demo talks to std.rest-client's crud boundary, so the recorded documents are the
    // conventional crud args themselves; URL construction is covered one hop further down.
    expect(fake.calls.map((call) => call.args)).toEqual([
      { baseUrl: 'https://api.example.com/v1', resource: 'things', item: { name: 'demo thing' } },
      { baseUrl: 'https://api.example.com/v1', resource: 'things', query: { page: 1 } },
      { baseUrl: 'https://api.example.com/v1', resource: 'things', id: 't1' },
      {
        baseUrl: 'https://api.example.com/v1',
        resource: 'things',
        id: 't1',
        item: { name: 'renamed' },
      },
      { baseUrl: 'https://api.example.com/v1', resource: 'things', id: 't1', method: 'DELETE' },
    ]);
  });

  it('validates its baseUrl argument and lease state', async () => {
    const fake = makeFakeServices();
    const demo = createDemo();
    await demo.init(fake.services);
    await demo.start();
    try {
      await demo.invoke?.({ methodId: 1, methodName: 'demo-crud', args: { baseUrl: '' } });
      expect.unreachable();
    } catch (error) {
      expect(statusOf(error)).toBe('invalid_argument');
    }
    try {
      await demo.invoke?.({
        methodId: 1,
        methodName: 'demo-crud',
        args: { baseUrl: 'https://api.example.com/v1' },
      });
      expect.unreachable();
    } catch (error) {
      expect(statusOf(error)).toBe('invalid_state');
    }
  });
});
