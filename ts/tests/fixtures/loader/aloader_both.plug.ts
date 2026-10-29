/**
 * @file Loader test fixture: exports both a default factory and a named `create`; the loader
 *        must prefer the default export and therefore report `aloader.both.default`.
 */
import type { RackPlugin } from '../../../src/core/types.ts';

const defaultPlugin: RackPlugin = {
  desc: {
    plugId: 'aloader.both.default',
    version: { major: 1, minor: 0, patch: 0 },
    init: {},
  },
  init: () => {},
  start: () => {},
  stop: () => {},
};

const namedPlugin: RackPlugin = {
  desc: {
    plugId: 'aloader.both.named',
    version: { major: 1, minor: 0, patch: 0 },
    init: {},
  },
  init: () => {},
  start: () => {},
  stop: () => {},
};

export default () => defaultPlugin;

export function create(): RackPlugin {
  return namedPlugin;
}
