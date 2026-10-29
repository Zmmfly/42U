/**
 * @file Loader test fixture: valid module exporting a default plugin factory with a complete
 *        descriptor (explicit init priority plus empty constraint arrays).
 */
import type { RackPlugin } from '../../../src/core/types.ts';

const plugin: RackPlugin = {
  desc: {
    plugId: 'aloader.default',
    version: { major: 1, minor: 2, patch: 3 },
    init: { priority: 10, before: [], after: [] },
  },
  init: () => {},
  start: () => {},
  stop: () => {},
};

export default () => plugin;
