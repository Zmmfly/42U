/**
 * @file Loader test fixture: exports neither a default factory nor a named `create`; loads
 *        must be rejected with RackError('unsupported').
 */
export const meta = { plugId: 'aloader.badshape' };
