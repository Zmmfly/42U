/**
 * @file Loader test fixture: throws during module evaluation; loads must surface
 *        RackError('failed') carrying a summary of the original error.
 */
throw new Error('aloader: module evaluation exploded');
