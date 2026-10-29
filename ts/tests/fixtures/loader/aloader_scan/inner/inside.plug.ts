/**
 * @file Scan-directory fixture: plugin file inside a subdirectory; discovery is non-recursive
 *        and must not report it.
 */
export default () => ({
  desc: { plugId: 'aloader.scan.nested', version: { major: 0, minor: 0, patch: 4 }, init: {} },
  init: () => {},
  start: () => {},
  stop: () => {},
});
