/**
 * @file echo.plug.ts
 * @brief Example echo provider: verbatim JSON echo plus a deliberate failure method.
 *
 * @details TS counterpart of examples/echo.cc. The provider owns no state besides the host
 *          services borrowed in init(): start() announces exactly two methods and invoke()
 *          serves them. A consumer reaches these methods only through the host gateway under a
 *          version-qualified lease; this module exports no business type for anyone to import.
 */

import { definePlugin, RackError } from '../../src/sdk/index.ts';
import type { MethodDesc, PluginServices, RackPlugin } from '../../src/sdk/index.ts';

/** @brief Canonical plugin identity announced to the host. */
const ECHO_PLUG_ID = 'example.echo';

/** @brief Business version published by this provider. */
const ECHO_VERSION = { major: 1, minor: 0, patch: 0 } as const;

/** @brief Numeric method id of the verbatim echo method. */
const ECHO_METHOD_ID = 1;

/** @brief Numeric method id of the deliberate failure method. */
const FAIL_METHOD_ID = 2;

/** @brief Method names matching the numeric ids above; part of the data-only contract. */
const ECHO_METHOD_NAME = 'echo';
const FAIL_METHOD_NAME = 'fail';

/** @brief Methods announced through the capability service during start(). */
const ECHO_METHODS: readonly MethodDesc[] = [
  {
    id: ECHO_METHOD_ID,
    name: ECHO_METHOD_NAME,
    description: 'Return the request JSON value unchanged.',
    inputSchema: { type: 'object' },
    outputSchema: { type: 'object' },
  },
  {
    id: FAIL_METHOD_ID,
    name: FAIL_METHOD_NAME,
    description: 'Always fail on purpose; demonstrates host error conversion.',
    inputSchema: { type: 'object' },
    outputSchema: { type: 'object' },
  },
];

/**
 * @brief Create one echo provider instance.
 *
 * @return A RackPlugin built with the SDK helper; each call yields an independent instance.
 */
export function create(): RackPlugin {
  let services: PluginServices | null = null;
  return definePlugin(
    { plugId: ECHO_PLUG_ID, version: ECHO_VERSION, init: {} },
    {
      init: (injected) => {
        services = injected;
      },
      start: () => {
        services?.caps.publish({ methods: ECHO_METHODS });
      },
      stop: () => {
        // No threads, callbacks or leases to unwind; capability withdrawal is host-driven.
      },
      invoke: (request) => {
        if (request.methodId === ECHO_METHOD_ID || request.methodName === ECHO_METHOD_NAME) {
          return request.args;
        }
        if (request.methodId === FAIL_METHOD_ID || request.methodName === FAIL_METHOD_NAME) {
          // Deliberate failure demo; the host converts it into 'failed' plus a diagnostic.
          throw new RackError('failed', 'example.echo fail method always fails by design');
        }
        throw new RackError(
          'not_found',
          `unknown echo method ${request.methodName} (#${request.methodId})`,
        );
      },
    },
  );
}

// Loader convention: a create(): RackPlugin factory as default or named export.
export default create;
