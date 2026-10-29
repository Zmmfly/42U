/**
 * @file Loader test fixture: `desc.init` violates the InitOrderSpec shape (fractional
 *        priority, non-array `before`); loads must be rejected with
 *        RackError('invalid_argument').
 */
export default () => ({
  desc: {
    plugId: 'aloader.baddesc.init',
    version: { major: 1, minor: 0, patch: 0 },
    init: { priority: 1.5, before: 'not-an-array' },
  },
  init: () => {},
  start: () => {},
  stop: () => {},
});
