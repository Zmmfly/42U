/**
 * @file storage.plug.ts
 * @brief Example key/value provider demonstrating instance state kept across invoke calls.
 *
 * @details The whole store lives in the closure created by create(), so every host-dispatched
 *          invoke() on one instance observes the same Map: instance-internal state survives
 *          across calls without the plugin holding any host or provider reference. Data crosses
 *          the boundary as plain JSON values only.
 */

import { definePlugin, RackError } from '../../src/sdk/index.ts';
import type {
  JsonObject,
  JsonValue,
  MethodDesc,
  PluginServices,
  RackPlugin,
} from '../../src/sdk/index.ts';

/** @brief Canonical plugin identity announced to the host. */
const STORAGE_PLUG_ID = 'example.storage';

/** @brief Business version published by this provider. */
const STORAGE_VERSION = { major: 1, minor: 2, patch: 0 } as const;

/** @brief Numeric method id of the write method. */
const SET_METHOD_ID = 1;

/** @brief Numeric method id of the read method. */
const GET_METHOD_ID = 2;

/** @brief Method names matching the numeric ids above; part of the data-only contract. */
const SET_METHOD_NAME = 'set';
const GET_METHOD_NAME = 'get';

/** @brief Methods announced through the capability service during start(). */
const STORAGE_METHODS: readonly MethodDesc[] = [
  {
    id: SET_METHOD_ID,
    name: SET_METHOD_NAME,
    description: 'Store a JSON value under a string key.',
    inputSchema: {
      type: 'object',
      properties: { key: { type: 'string' }, value: {} },
      required: ['key', 'value'],
    },
    outputSchema: { type: 'null' },
  },
  {
    id: GET_METHOD_ID,
    name: GET_METHOD_NAME,
    description: 'Read a stored JSON value; null when the key is absent.',
    inputSchema: {
      type: 'object',
      properties: { key: { type: 'string' } },
      required: ['key'],
    },
    outputSchema: {},
  },
];

/**
 * @brief Narrow a JSON value to a JSON object.
 *
 * @param value Candidate request args document.
 * @param what Name of the checked document for the error message.
 * @return The same value typed as JsonObject.
 * @throws RackError 'invalid_argument' when the value is not a JSON object.
 */
function expectObject(value: JsonValue, what: string): JsonObject {
  // The final cast is needed because the readonly JsonArray union member is not narrowed
  // away by Array.isArray (readonly arrays are not assignable to any[]).
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw new RackError('invalid_argument', `${what} must be a JSON object`);
  }
  return value as JsonObject;
}

/**
 * @brief Extract the mandatory string key of a set/get args document.
 *
 * @param args Request args already narrowed to a JSON object.
 * @return The validated key.
 * @throws RackError 'invalid_argument' when args.key is missing or not a string.
 */
function expectKey(args: JsonObject): string {
  const key = args['key'];
  if (typeof key !== 'string') {
    throw new RackError('invalid_argument', 'args.key must be a string');
  }
  return key;
}

/**
 * @brief Create one storage provider instance with its own private store.
 *
 * @return A RackPlugin built with the SDK helper; each call yields an independent instance.
 */
export function create(): RackPlugin {
  // Instance-internal state: survives across invoke() calls of this instance only.
  const store = new Map<string, JsonValue>();
  let services: PluginServices | null = null;
  return definePlugin(
    { plugId: STORAGE_PLUG_ID, version: STORAGE_VERSION, init: {} },
    {
      init: (injected) => {
        services = injected;
      },
      start: () => {
        services?.caps.publish({ methods: STORAGE_METHODS });
      },
      stop: () => {
        // Kept intentionally: stored data stays observable until the instance is destroyed.
      },
      destroy: () => {
        store.clear();
      },
      invoke: (request) => {
        if (request.methodId === SET_METHOD_ID || request.methodName === SET_METHOD_NAME) {
          const args = expectObject(request.args, 'set args');
          const value = args['value'];
          if (value === undefined) {
            throw new RackError('invalid_argument', 'set args require a value');
          }
          store.set(expectKey(args), value);
          return null;
        }
        if (request.methodId === GET_METHOD_ID || request.methodName === GET_METHOD_NAME) {
          const args = expectObject(request.args, 'get args');
          // A stored null and a missing key are indistinguishable by contract.
          return store.get(expectKey(args)) ?? null;
        }
        throw new RackError(
          'not_found',
          `unknown storage method ${request.methodName} (#${request.methodId})`,
        );
      },
    },
  );
}

// Loader convention: a create(): RackPlugin factory as default or named export.
export default create;
