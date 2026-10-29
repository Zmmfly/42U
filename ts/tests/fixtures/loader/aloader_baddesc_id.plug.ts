/**
 * @file Loader test fixture: `desc.plugId` is an empty string; loads must be rejected with
 *        RackError('invalid_argument').
 */
export default () => ({
  desc: { plugId: '', version: { major: 1, minor: 0, patch: 0 }, init: {} },
  init: () => {},
  start: () => {},
  stop: () => {},
});
