# 42U Rack — TypeScript Edition

C++ 42U 插件机架的 TypeScript 版：宿主（Rack）像标准机柜一样编排插件设备——统一扫描、
排序、初始化、启动、撤销与卸载；数据经事件总线与 JSON 调用网关流通；插件间业务协作
只走「版本化租约 + 调用网关」一条路，组件可热插拔（load / unload / reload）。
设计思想与 C++ 原版对照见 [docs/design.md](docs/design.md)，冻结契约见
`src/core/types.ts`。

## 快速开始

要求 Node ≥ 24（仓库用可擦除 TS 语法，Node 原生类型擦除直接运行 `.ts`）。

```bash
cd ts
npm install
npm run typecheck    # npx tsc --noEmit（strict + noUncheckedIndexedAccess + verbatimModuleSyntax）
npm test             # npx vitest run，全量单测 + 端到端（tests/e2e.test.ts）
npm run demo         # node apps/demo.ts，8 步端到端演示（见下文）
node apps/rack-cli.ts version
```

公共导入面只有一个入口：

```ts
import { createRack, addStandardPlugins, definePlugin, typedApi, RackError } from './src/index.ts';
```

## 机架模型

```
                 ┌───────────────────────── 42U Rack（机柜 / 宿主）─────────────────────────┐
                 │  编排：boot / load / unload / reload / poll / shutdown（确定性拓扑排序）  │
                 │                                                                     │
   设备（插件）   │  ┌─────────┐ ┌──────────┐ ┌────────┐ ┌────────────┐      ┌────────┐    │
   *.plug.ts ───▶│  │example. │ │example.  │ │std.log │ │std.http-   │ ...  │std.rpc│    │
   或 rack.add() │  │echo     │ │storage   │ │        │ │server      │      │        │    │
                 │  └────┬────┘ └────┬─────┘ └───┬────┘ └─────┬──────┘      └───┬────┘    │
                 │       │           │           │            │                 │         │
                 │   ────┴───────────┴───────────┴────────────┴─────────────────┴────▶    │
                 │   背板 = 事件总线（有界队列，u42.* 保留名，按订阅者撤销）                  │
                 │        + 调用网关（invoke-only：消费者 → 宿主 → 提供者，JSON 数据面）     │
                 │   租约：acquire(plugId, 版本闭区间) → 不透明凭据 + 实际版本               │
                 │        凭据绑定代次；reload 后旧凭据永久 stale，同版本也必须重借          │
                 └───────────────────────────────────────────────────────────────────────┘
```

- **机柜**：`createRack()` 返回的宿主负责全部生命周期；插件只实现约定接口。
- **设备**：普通业务插件（`examples/plugins/`）与随框架发货的标准件（`std.*`）地位相同。
- **背板**：事件总线 + 调用网关。插件之间不共享对象、不传递提供者引用，数据只过 JSON。

## 五分钟插件教程

1）用 SDK 定义一个提供者：声明身份、版本与初始化约束，`start()` 内发布能力，`invoke()`
服务方法（完整示例见 `examples/plugins/echo.plug.ts` / `storage.plug.ts`）：

```ts
import { definePlugin, RackError } from '../src/sdk/index.ts';
import type { InvokeRequest, RackPlugin } from '../src/sdk/index.ts';

export function create(): RackPlugin {
  return definePlugin(
    { plugId: 'example.echo', version: { major: 1, minor: 0, patch: 0 }, init: {} },
    {
      start: (services) => services.caps.publish({
        methods: [{ id: 1, name: 'echo', description: 'Return the args unchanged.' }],
      }),
      invoke: (request: InvokeRequest) => {
        if (request.methodName === 'echo') return request.args;
        throw new RackError('not_found', `unknown method ${request.methodName}`);
      },
    },
  );
}

export default create;
```

2）消费者在 `init()` 内 `caps.watch()` 观察能力事实，版本落在显式闭区间时租借；
业务调用用 `typedApi` 直调（代理转发网关，租约/代次/限额/重入校验全部保留）：

```ts
import { acceptsVersion, exactVersion, typedApi } from '../src/sdk/index.ts';

// init() 内：
const watch = services.caps.watch({
  onCapability(event) {
    if (event.plugId !== 'example.echo' || !event.available) return;
    if (!acceptsVersion(exactVersion({ major: 1, minor: 0, patch: 0 }), event.version)) return;
    lease = services.caps.acquire('example.echo', exactVersion({ major: 1, minor: 0, patch: 0 }), revoker);
  },
});

// Active 之后的业务调用：
const echo = typedApi<{ echo(payload: unknown): Promise<unknown> }>(services.calls, lease);
const result = await echo.echo({ hello: 'rack' });
```

3）装机运行：`*.plug.ts` 放进目录由 `boot()` 扫描，或在进程内 `rack.add(create)`。
注意两点：**init() 只订阅不调用**；**start() 返回前实例还不是 Active，不能发起网关
调用**（需要挂路由/写启动日志的插件把动作推迟到下一宏任务，参考 `std.rpc` 的
`scheduleInitialMount`）。

## std 标准件

8 个标准设备随框架发货（目录 `src/plugins/<name>/`，各有 README），经
`addStandardPlugins(rack)` 一次全部上机，或用 `standardPlugins` 目录按需 `rack.add()`：

| plugId | 方法 | 一句话 |
| --- | --- | --- |
| `std.log` | `log` / `query` / `stats` | 内存环形缓冲日志，可选控制台/文件 sink |
| `std.net` | `dns.resolve` / `tcp.probe` / `url.normalize` | DNS 解析、TCP 探测与 URL 归一化 |
| `std.http-client` | `request` / `get` / `post` / `put` / `del` | 带超时、重试与大小守卫的 fetch 封装 |
| `std.rest-client` | `resource` / `crud` | 租借 std.http-client 的 RESTful 约定客户端 |
| `std.http-server` | `route.add` / `route.remove` / `routes.list` / `stats` | node:http 路由，按请求一次性租借目标插件转发；`stats.port` 暴露实际监听端口（`U42_HTTP_PORT`，默认 8080；0 = 随机） |
| `std.rest-server` | `resource.mount` / `resource.unmount` / `mounted.list` | 租借 std.http-server，把资源声明翻译成 CRUD 路由 |
| `std.rpc` | `bridge` / `call` / `batch` | JSON-RPC 2.0 网口：桥挂 `POST /rpc`，客户端走向远端机架 |
| `std.oauth` | `authorize-url` / `token` / `token.invalidate` / `introspect` | OAuth 2.0 授权码/客户端凭证/PKCE 与令牌缓存 |

## CLI

`apps/rack-cli.ts` 面向运维：`list / describe / call / unload / reload / poll / serve`，
支持 `--json` 与 `--dir`。完整文档见 [apps/rack-cli.md](apps/rack-cli.md)。

```bash
node apps/rack-cli.ts version
node apps/rack-cli.ts list --dir examples/plugins --json
node apps/rack-cli.ts call example.storage get '{"key":"example.data"}'
node apps/rack-cli.ts reload example.echo
```

注意：`examples/plugins` 里多数示例声明了对 `std.*` 的硬初始化边（`after`），单独 boot
该目录会因目标缺失而 `not_found`；CLI 场景请用包含 `example.echo` / `example.storage` /
`example.consumer` 的自包含子集目录，或参照 demo 在同一机架内加入标准件。

## demo

`node apps/demo.ts`（在 `ts/` 目录下）装配真实机架——`examples/plugins` 全部示例 +
8 个标准件 + 进程内 `demo.driver`——依次演示并打印 ✓/✗：

1. 能力快照与 watch；
2. echo/storage 的 typedApi 直调（一次性租借 + 长持租约）；
3. 事件流通：`example.data` → consumer 消费 → 写穿 storage → 读回核对；
4. REST：经 std.rest-server 挂载 `/api/rack/:id`，真实 HTTP 请求命中；
5. RPC：`POST /rpc` 桥回环 + std.rpc 客户端回环调用 `example.storage.get`；
6. 日志：向 std.log 记两条再 query 核对；
7. 可拔插：reload 代次递增、同版本必须重借；unload 后旧凭据 stale、租借 not_found；
8. 优雅收尾：归还长持租约，shutdown 后 `plugins()` 为空。

任一步失败打印原因并以退出码 1 结束。机器可验证断言见 `tests/e2e.test.ts`。

## 与 C++ 版思想对照

TS 版保留 42U 的核心思想（机架比喻、invoke-only 网关、版本化租约、能力披露、确定性
编排、显式卸载协议、代次失效），用 JS/TS 原生机制替换 C++ 机制（动态 `import()` +
缓存击穿替代 `dlopen/dlclose`、`RackError` 替代状态码、JSON 值替代字节视图、
`typedApi` 直调代理替代调用仪式）。完整裁决与适配决策表见
[docs/design.md](docs/design.md)。

## 已知边界

- **单进程可信插件**：框架治理调用与借用关系，不承诺对恶意代码的隔离（设计思想 11）。
- **payload / output 限额**：事件载荷、调用参数与结果按 `RackOptions.payloadLimit` /
  `outputLimit`（默认各 1 MiB，偏大估算）检查；demo 与 e2e 放宽到 8 MiB。
- **ESM 卸载 = GC 语义**：reload 以 `?u42gen=N` 取新 URL 重新求值模块；旧模块在宿主
  丢弃引用后由 GC 回收，JS 没有 `dlclose`。
- **start() 内不可发起业务调用**：实例在 `start()` 返回后才 Active；网关拒绝
  Initialized 消费者的调用（`invalid_state`）。需要在启动时挂路由/写日志的插件应把
  动作推迟到下一宏任务（`std.rpc`、`example.http-server-demo` 等均已如此）。
- **stop() 内的网关调用同样被拒**：卸载期间实例处于 `revoking`；路由等远端资源随
  提供方代次一起消亡，各插件对 stop 期清理一律按尽力而为处理。
- **std.rpc 客户端不能回环自家 `/rpc`**：`call` 在途时桥（`bridge`）是对同一
  `std.rpc` 实例的并发进入，被网关以 `busy` 拒绝（同提供者在途守卫）。客户端定位是
  机架间互连；同机架回环请直接 `POST /rpc`（demo 双路演示了这两种形态）。
