/**
 * @file Tests for the plugin module loader: discovery filtering and ordering, export shape
 *        normalization, descriptor validation, failure mapping to rack statuses, generation
 *        based ESM cache busting and same-generation module reuse.
 */
import { describe, expect, it } from 'vitest';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createPluginLoader } from '../src/core/loader.ts';
import { isRackError, statusOf, type RackStatus } from '../src/core/status.ts';
import type { DiagLevel } from '../src/core/types.ts';

const fixtures = fileURLToPath(new URL('fixtures/loader', import.meta.url));
const scanDir = join(fixtures, 'aloader_scan');

/**
 * @brief Assert that a promise rejects with a RackError of the given status.
 * @param promise Loader call under test.
 * @param status Expected RackError status.
 */
async function expectStatus(promise: Promise<unknown>, status: RackStatus): Promise<void> {
  let caught: unknown;
  try {
    await promise;
  } catch (error) {
    caught = error;
  }
  expect(caught, `expected rejection with '${status}'`).toBeInstanceOf(Error);
  expect(isRackError(caught)).toBe(true);
  expect(statusOf(caught)).toBe(status);
}

/**
 * @brief Process-global evaluation counter shared with `aloader_counter.plug.ts` through a
 *        registered symbol; cache-busted module instances cannot share module-level bindings.
 */
const counterKey = Symbol.for('u42.aloader.eval-counter');
const counterStore = globalThis as typeof globalThis & { [key: symbol]: number | undefined };
const readCounter = (): number => counterStore[counterKey] ?? 0;

const loader = createPluginLoader();

describe('loader.discover', () => {
  it('keeps only *.plug.{ts,js,mjs} regular files, non-recursive, sorted by path', async () => {
    // The scan directory mixes: two matching .ts files, one .mjs, a .txt, a plain .ts, a
    // .plug.tsx, a nested plugin inside `inner/`, and a directory named `zdir.plug.ts`.
    const files = await loader.discover(scanDir);
    expect(files.map((file) => file.path)).toEqual([
      join(scanDir, 'aloader_a.plug.ts'),
      join(scanDir, 'aloader_b.plug.ts'),
      join(scanDir, 'aloader_c.plug.mjs'),
    ]);
  });

  it('rejects a missing directory with not_found', async () => {
    await expectStatus(loader.discover(join(fixtures, 'aloader_no_such_dir')), 'not_found');
  });

  it('rejects a non-directory path with invalid_argument', async () => {
    await expectStatus(loader.discover(join(scanDir, 'notes.txt')), 'invalid_argument');
  });
});

describe('loader.load', () => {
  it('loads a default-exported factory and caches its single invocation', async () => {
    const mod = await loader.load(join(fixtures, 'aloader_default.plug.ts'), 1);
    const plugin = mod.create();
    expect(plugin.desc.plugId).toBe('aloader.default');
    expect(plugin.desc.version).toEqual({ major: 1, minor: 2, patch: 3 });
    expect(plugin.desc.init).toEqual({ priority: 10, before: [], after: [] });
    // The factory runs once per load(); repeat create() calls return the same instance.
    expect(mod.create()).toBe(plugin);
  });

  it('loads a named create() export with a partial init spec', async () => {
    const mod = await loader.load(join(fixtures, 'aloader_named.plug.ts'), 1);
    const plugin = mod.create();
    expect(plugin.desc.plugId).toBe('aloader.named');
    expect(plugin.desc.init).toEqual({ priority: -5 });
  });

  it('prefers the default export when both default and named create() exist', async () => {
    const mod = await loader.load(join(fixtures, 'aloader_both.plug.ts'), 1);
    expect(mod.create().desc.plugId).toBe('aloader.both.default');
  });

  it('loads a .plug.mjs module', async () => {
    const mod = await loader.load(join(scanDir, 'aloader_c.plug.mjs'), 1);
    expect(mod.create().desc.plugId).toBe('aloader.scan.c');
  });

  it('rejects a module without a compatible export with unsupported', async () => {
    await expectStatus(loader.load(join(fixtures, 'aloader_badshape.plug.ts'), 1), 'unsupported');
  });

  it('maps a module evaluation failure to failed and reports it on the diagnostics sink', async () => {
    const diags: Array<{ level: DiagLevel; message: string }> = [];
    const reporting = createPluginLoader({
      onDiag: (level, message) => {
        diags.push({ level, message });
      },
    });
    await expectStatus(reporting.load(join(fixtures, 'aloader_throws.plug.ts'), 1), 'failed');
    expect(diags.some((entry) => entry.level === 'error' && entry.message.includes('aloader_throws'))).toBe(true);
  });

  it('keeps the failed status when the diagnostics sink itself throws', async () => {
    const broken = createPluginLoader({
      onDiag: () => {
        throw new Error('sink exploded');
      },
    });
    await expectStatus(broken.load(join(fixtures, 'aloader_throws.plug.ts'), 2), 'failed');
  });

  it('maps a throwing plugin factory to failed', async () => {
    await expectStatus(loader.load(join(fixtures, 'aloader_create_throws.plug.ts'), 1), 'failed');
  });

  it('rejects malformed descriptors with invalid_argument', async () => {
    for (const file of [
      'aloader_baddesc_missing.plug.ts',
      'aloader_baddesc_id.plug.ts',
      'aloader_baddesc_version.plug.ts',
      'aloader_baddesc_init.plug.ts',
    ]) {
      await expectStatus(loader.load(join(fixtures, file), 1), 'invalid_argument');
    }
  });

  it('rejects a missing file with not_found', async () => {
    await expectStatus(loader.load(join(fixtures, 'aloader_missing.plug.ts'), 1), 'not_found');
  });

  it('rejects a non-integer generation with invalid_argument', async () => {
    await expectStatus(loader.load(join(fixtures, 'aloader_default.plug.ts'), 1.5), 'invalid_argument');
  });

  it('busts the ESM cache across generations: new evaluation and independent instances', async () => {
    const target = join(fixtures, 'aloader_counter.plug.ts');
    const before = readCounter();

    const first = await loader.load(target, 101);
    expect(readCounter()).toBe(before + 1); // module evaluated once for generation 101

    const second = await loader.load(target, 102);
    expect(readCounter()).toBe(before + 2); // generation 102 re-evaluated the module

    expect(second).not.toBe(first);
    expect(second.create()).not.toBe(first.create());
    expect(second.create().desc.plugId).toBe(first.create().desc.plugId);
  });

  it('reuses the cached module for a repeated same-generation load', async () => {
    const target = join(fixtures, 'aloader_counter.plug.ts');
    const first = await loader.load(target, 7);
    const evaluations = readCounter();

    const again = await loader.load(target, 7);
    expect(again).toBe(first);
    expect(again.create()).toBe(first.create());
    expect(readCounter()).toBe(evaluations); // no re-evaluation, no second factory call
  });
});
