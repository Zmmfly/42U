/**
 * @file Host test fixture: second of two files declaring the same plug id 'ahost.dup'.
 */
import type { RackPlugin } from '../../../../src/core/types.ts';

const plugin: RackPlugin = {
  desc: { plugId: 'ahost.dup', version: { major: 1, minor: 0, patch: 0 }, init: {} },
  init: () => {},
  start: () => {},
  stop: () => {},
};

export default () => plugin;
