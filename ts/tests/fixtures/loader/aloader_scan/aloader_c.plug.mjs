// Scan-directory fixture: discoverable plugin module written as plain ESM JavaScript so the
// *.plug.mjs suffix of the discovery filter is exercised end to end.
export default () => ({
  desc: { plugId: 'aloader.scan.c', version: { major: 0, minor: 0, patch: 3 }, init: {} },
  init: () => {},
  start: () => {},
  stop: () => {},
});
