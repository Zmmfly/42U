/**
 * @file Entry point of the rack CLI.
 *
 * @brief Wires runCli() to the console and to the default rack factory, which lazily imports
 *        the host implementation from src/core/host.ts.
 *
 * @note Command semantics live in ./cli/commands.ts; this file only adapts process argv,
 *       console streams and the process exit code. Importing this module (for example from
 *       tests) is inert: the CLI only runs when Node launched this exact script.
 */

import { nodeKeepAlive, runCli } from './cli/commands.ts';
import { createDefaultRackFactory } from './cli/rack-access.ts';

/**
 * @brief Decide whether this module is the executed script rather than a plain import.
 *
 * @return true only when Node launched apps/rack-cli.ts directly.
 * @note The suffix fallback tolerates argv spellings whose absolute form differs from
 *       import.meta.filename (symlinks, cwd-relative launches).
 */
function isEntryScript(): boolean {
  const entry = process.argv[1];
  if (entry === undefined) {
    return false;
  }
  return import.meta.filename === entry || entry.endsWith('apps/rack-cli.ts');
}

if (isEntryScript()) {
  process.exitCode = await runCli(process.argv.slice(2), {
    out: (line) => console.log(line),
    err: (line) => console.error(line),
    rackFactory: createDefaultRackFactory({ withStd: process.argv.includes('--with-std') }),
    keepAlive: nodeKeepAlive,
  });
}
