/**
 * @file Host test fixture: declares a `before` constraint on 'ahost.provider'.
 *
 * @brief Used to prove that hot-loading a plugin which must initialize BEFORE an already
 *        initialized instance is rejected with 'invalid_state' by the planner wiring.
 */
import type { RackPlugin } from '../../../../src/core/types.ts';

const plugin: RackPlugin = {
  desc: {
    plugId: 'ahost.late',
    version: { major: 1, minor: 0, patch: 0 },
    init: { before: ['ahost.provider'] },
  },
  init: () => {},
  start: () => {},
  stop: () => {},
};

export default () => plugin;
