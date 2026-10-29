/**
 * @file Loader test fixture: the factory result carries no `desc` member at all; loads must be
 *        rejected with RackError('invalid_argument').
 */
export default () => ({ init: () => {}, start: () => {}, stop: () => {} });
