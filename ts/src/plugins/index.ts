/**
 * @file index.ts
 * @brief Catalog of the standard devices that ship with the 42U rack (TS edition).
 *
 * @details The eight `std.*` plugins are regular rack devices, not host built-ins ("everything
 *          is a plugin"). This module is their single source of truth for embedders: one
 *          catalog entry per device carrying its plugId, business version, create factory
 *          and a one-line description. `addStandardPlugins(rack)` stages every factory on a
 *          rack in one call so `rack.boot()` plans them together with directory-loaded
 *          plugins (hard init edges such as `after: ['std.http-server']` resolve inside that
 *          single batch).
 */

import type { PluginVersion, Rack, RackPlugin } from '../core/types.ts';
import { create as createLogging } from './logging/index.ts';
import { create as createNet } from './net/index.ts';
import { create as createHttpClient } from './http-client/index.ts';
import { create as createRestClient } from './rest-client/index.ts';
import { create as createHttpServer } from './http-server/index.ts';
import { create as createRestServer } from './rest-server/index.ts';
import { create as createRpc } from './rpc/index.ts';
import { create as createOauth } from './oauth/index.ts';

/**
 * @brief One catalog entry describing a standard plugin that ships with the framework.
 * @note The factories are plain `create(): RackPlugin` module factories; staging one entry
 *       on a rack via `rack.add(entry.create)` yields an independent instance.
 */
export interface StandardPluginSpec {
  /** @brief Canonical plugId announced by the device, e.g. "std.log". */
  readonly plugId: string;
  /** @brief Business version published by the device (duplicated from its own contract). */
  readonly version: PluginVersion;
  /** @brief Factory producing one independent plugin instance. */
  readonly create: () => RackPlugin;
  /** @brief One-line Chinese description of what the device provides. */
  readonly description: string;
}

/** @brief Business version shared by every standard device in this release. */
const V1: PluginVersion = { major: 1, minor: 0, patch: 0 };

/**
 * @brief The standard plugin catalog in stable catalog order.
 * @note Order carries no orchestration meaning: init order is derived from priorities and
 *       hard edges by the host planner, not from this array.
 */
export const standardPlugins: readonly StandardPluginSpec[] = [
  {
    plugId: 'std.log',
    version: V1,
    create: createLogging,
    description: '内存环形缓冲日志设备：log/query/stats，可选控制台与文件 sink。',
  },
  {
    plugId: 'std.net',
    version: V1,
    create: createNet,
    description: '网络工具设备：dns.resolve / tcp.probe / url.normalize。',
  },
  {
    plugId: 'std.http-client',
    version: V1,
    create: createHttpClient,
    description: 'HTTP 客户端设备：带超时、重试与大小守卫的 fetch 封装。',
  },
  {
    plugId: 'std.rest-client',
    version: V1,
    create: createRestClient,
    description: 'RESTful 客户端设备：租借 std.http-client，提供 resource/crud 约定。',
  },
  {
    plugId: 'std.http-server',
    version: V1,
    create: createHttpServer,
    description: 'HTTP 服务设备：node:http 路由按请求一次性租借目标插件并经网关转发。',
  },
  {
    plugId: 'std.rest-server',
    version: V1,
    create: createRestServer,
    description: 'RESTful 资源服务设备：租借 std.http-server，把资源声明翻译成 CRUD 路由。',
  },
  {
    plugId: 'std.rpc',
    version: V1,
    create: createRpc,
    description: 'JSON-RPC 2.0 网口设备：服务端桥挂载 POST /rpc，客户端提供 call/batch。',
  },
  {
    plugId: 'std.oauth',
    version: V1,
    create: createOauth,
    description: 'OAuth 2.0 设备：授权码/客户端凭证/PKCE、令牌缓存与自动刷新。',
  },
];

/**
 * @brief Stage every standard plugin on a rack before boot().
 *
 * @param rack Rack that has not booted yet; every catalog factory is staged via rack.add().
 * @return The same rack, for call chaining.
 * @note Staging must happen before boot()/start() so the standard devices join the same
 *       planning batch as directory-loaded plugins; example plugins carry hard edges such as
 *       `after: ['std.http-server']` that would fail with 'not_found' otherwise.
 */
export function addStandardPlugins(rack: Rack): Rack {
  for (const spec of standardPlugins) {
    rack.add(spec.create);
  }
  return rack;
}
