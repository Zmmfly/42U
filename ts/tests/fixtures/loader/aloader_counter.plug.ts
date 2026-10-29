/**
 * @file Loader test fixture: increments a process-global counter on every module evaluation so
 *        tests can assert that a new generation re-evaluates the module (ESM cache busting).
 * @note The counter lives on globalThis under a registered symbol because each cache-busted
 *       URL produces an independent module instance with its own module-level bindings.
 */
import type { RackPlugin } from '../../../src/core/types.ts';

const counterKey = Symbol.for('u42.aloader.eval-counter');
const store = globalThis as typeof globalThis & { [key: symbol]: number | undefined };
store[counterKey] = (store[counterKey] ?? 0) + 1;

const plugin: RackPlugin = {
  desc: {
    plugId: 'aloader.counter',
    version: { major: 0, minor: 0, patch: 1 },
    init: {},
  },
  init: () => {},
  start: () => {},
  stop: () => {},
};

export default () => plugin;
