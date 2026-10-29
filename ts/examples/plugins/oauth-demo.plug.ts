/**
 * @file oauth-demo.plug.ts
 * @brief Loadable demo for the std.oauth plugin: client_credentials fetch plus a cache hit,
 *        driven entirely by a fake fetch and a fake clock (no network access).
 *
 * @details The demo is also a rack-loadable plugin (plugId example.oauth-demo) exposing one
 *          'run' method that executes the scenario and returns the trace document, so it can be
 *          demonstrated through the host gateway. Running the file directly with
 *          `node examples/plugins/oauth-demo.plug.ts` prints the same trace to stdout.
 */

import { pathToFileURL } from 'node:url';
import { createWithDeps } from '../../src/plugins/oauth/index.ts';
import type { FetchLike } from '../../src/plugins/oauth/index.ts';
import { definePlugin } from '../../src/sdk/index.ts';
import type {
  InvokeRequest,
  JsonObject,
  JsonValue,
  MethodDesc,
  PluginServices,
  RackPlugin,
} from '../../src/sdk/index.ts';

/** @brief Fake token endpoint used by the in-memory authorization server. */
const DEMO_TOKEN_ENDPOINT = 'https://auth.demo.example.com/token';

/** @brief Demo client identity. */
const DEMO_CLIENT_ID = 'demo-client';

/** @brief Demo client secret; never printed — masked as '***' in demo output. */
const DEMO_CLIENT_SECRET = 'demo-secret-never-printed';

/** @brief Demo scope string. */
const DEMO_SCOPE = 'demo';

/** @brief Numeric method id of the demo entry point. */
const RUN_METHOD_ID = 1;

/** @brief Method name of the demo entry point. */
const RUN_METHOD_NAME = 'run';

/** @brief Methods announced through the capability service during start(). */
const DEMO_METHODS: readonly MethodDesc[] = [
  {
    id: RUN_METHOD_ID,
    name: RUN_METHOD_NAME,
    description: 'Run the std.oauth client_credentials fetch + cache-hit demo on a fake backend.',
    inputSchema: { type: 'null' },
    outputSchema: {},
  },
];

/**
 * @brief Build the demo harness: a counting fake token server plus a controllable clock.
 *
 * @return The std.oauth instance under demo, the request counter and the clock advancer.
 */
function buildHarness(): {
  readonly oauth: RackPlugin;
  readonly requestCount: () => number;
  readonly advanceClock: (ms: number) => void;
} {
  let calls = 0;
  let clockMs = 0;
  const fetchImpl: FetchLike = async (_input, _init) => {
    calls += 1;
    return new Response(
      JSON.stringify({
        access_token: `demo-access-${calls}`,
        token_type: 'Bearer',
        expires_in: 3600,
        scope: DEMO_SCOPE,
      }),
      { status: 200, headers: { 'content-type': 'application/json' } },
    );
  };
  const oauth = createWithDeps({ fetchImpl, now: () => clockMs });
  return {
    oauth,
    requestCount: () => calls,
    advanceClock: (ms: number) => {
      clockMs += ms;
    },
  };
}

/**
 * @brief Narrow a demo invoke result to a JSON object.
 *
 * @param value Invoke result.
 * @return The value typed as JsonObject.
 */
function asObject(value: JsonValue | undefined): JsonObject {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw new Error('demo expected a JSON object result');
  }
  // The cast is needed because readonly arrays are not narrowed away by Array.isArray.
  return value as JsonObject;
}

/** @brief Reusable token request args for the demo scenario. */
const TOKEN_ARGS: JsonObject = {
  grantType: 'client_credentials',
  tokenEndpoint: DEMO_TOKEN_ENDPOINT,
  clientId: DEMO_CLIENT_ID,
  clientSecret: DEMO_CLIENT_SECRET,
  scope: DEMO_SCOPE,
};

/**
 * @brief Run the demo scenario: fetch a client_credentials token, then hit the cache.
 *
 * @return Trace document with both results, the network call count and the cache-hit verdict.
 */
async function runScenario(): Promise<JsonObject> {
  const harness = buildHarness();
  const first = asObject(
    await harness.oauth.invoke?.({ methodId: 2, methodName: 'token', args: TOKEN_ARGS }),
  );
  // Second identical call inside the validity window must be served from the token cache.
  const second = asObject(
    await harness.oauth.invoke?.({ methodId: 2, methodName: 'token', args: TOKEN_ARGS }),
  );
  return {
    firstCall: first,
    secondCall: second,
    networkTokenRequests: harness.requestCount(),
    cacheHitConfirmed: second['cached'] === true,
  };
}

/**
 * @brief Print the demo trace to stdout with the client secret masked.
 *
 * @return Resolved when the trace has been printed.
 */
async function printDemo(): Promise<void> {
  const trace = await runScenario();
  console.log('std.oauth demo (fake fetch + fake clock, zero network):');
  console.log(`  client_id: ${DEMO_CLIENT_ID}`);
  console.log(`  client_secret: ${'***'}`);
  console.log(JSON.stringify(trace, null, 2));
}

/**
 * @brief Create the demo provider instance.
 *
 * @return A RackPlugin whose single 'run' method executes the demo scenario.
 */
export function create(): RackPlugin {
  let services: PluginServices | null = null;
  return definePlugin(
    { plugId: 'example.oauth-demo', version: { major: 1, minor: 0, patch: 0 }, init: {} },
    {
      init: (injected) => {
        services = injected;
      },
      start: () => {
        services?.caps.publish({ methods: DEMO_METHODS });
      },
      stop: () => {
        // The demo holds no long-lived resources.
      },
      invoke: async (request: InvokeRequest): Promise<JsonValue> => {
        if (request.methodId === RUN_METHOD_ID || request.methodName === RUN_METHOD_NAME) {
          return await runScenario();
        }
        throw new Error(`unknown demo method ${request.methodName} (#${request.methodId})`);
      },
    },
  );
}

// Direct execution: print the demo trace (loader imports skip this branch).
const invokedPath = process.argv[1];
if (invokedPath !== undefined && import.meta.url === pathToFileURL(invokedPath).href) {
  await printDemo();
}

// Loader convention: a create(): RackPlugin factory as default or named export.
export default create;
