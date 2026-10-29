/**
 * @file Loader test fixture: `desc.version` is a string instead of a numeric triple; loads
 *        must be rejected with RackError('invalid_argument').
 */
export default () => ({
  desc: { plugId: 'aloader.baddesc.version', version: '1.0.0', init: {} },
  init: () => {},
  start: () => {},
  stop: () => {},
});
