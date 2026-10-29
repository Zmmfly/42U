/**
 * @file Scan-directory fixture: discoverable plugin module exporting a default factory.
 */
export default () => ({
  desc: { plugId: 'aloader.scan.b', version: { major: 0, minor: 0, patch: 2 }, init: {} },
  init: () => {},
  start: () => {},
  stop: () => {},
});
