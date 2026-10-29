/**
 * @file Host test fixture: file-based provider used by load(), boot() and reload() flows.
 *
 * @brief Publishes 'echo' and 'moduleLoads'; every module evaluation bumps a process-global
 *        counter so generation-based ESM cache busting is observable across a reload.
 */
import type { PluginServices, RackPlugin } from '../../../../src/core/types.ts';

const counterHost = globalThis as typeof globalThis & { __ahostProviderEvaluations?: number };
counterHost.__ahostProviderEvaluations = (counterHost.__ahostProviderEvaluations ?? 0) + 1;

export default (): RackPlugin => {
  let services: PluginServices | undefined;
  return {
    desc: { plugId: 'ahost.provider', version: { major: 1, minor: 2, patch: 0 }, init: {} },
    init: (view: PluginServices): void => {
      services = view;
    },
    start: (): void => {
      services?.caps.publish({
        methods: [
          { id: 1, name: 'echo' },
          { id: 2, name: 'moduleLoads' },
        ],
      });
    },
    stop: (): void => {},
    invoke: (request) => {
      if (request.methodId === 2) {
        return counterHost.__ahostProviderEvaluations ?? 0;
      }
      return request.args;
    },
  };
};
