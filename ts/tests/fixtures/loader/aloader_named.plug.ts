/**
 * @file Loader test fixture: valid module exporting only a named `create` factory; the init
 *        spec carries a partial constraint set (priority only, no before/after arrays).
 */
import type { RackPlugin } from '../../../src/core/types.ts';

export function create(): RackPlugin {
  return {
    desc: {
      plugId: 'aloader.named',
      version: { major: 0, minor: 1, patch: 0 },
      init: { priority: -5 },
    },
    init: () => {},
    start: () => {},
    stop: () => {},
  };
}
