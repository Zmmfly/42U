/**
 * @file Loader test fixture: the default factory throws when invoked; loads must surface
 *        RackError('failed') instead of returning a plugin.
 */
export default () => {
  throw new Error('aloader: create() exploded');
};
