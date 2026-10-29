/**
 * @file Scan-directory fixture: discoverable plugin module exporting a default factory.
 */
export default () => ({
  desc: { plugId: 'aloader.scan.a', version: { major: 0, minor: 0, patch: 1 }, init: {} },
  init: () => {},
  start: () => {},
  stop: () => {},
});
