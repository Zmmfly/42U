# 42U 机架 TypeScript 版设计（契约 v1）

> 本文档是 C++ 版 42U 插件框架在 TypeScript 下的忠实重写规范。目标不是移植 C++ 代码，
> 而是保留 42U 的设计思想，用 JS/TS 的原生机制重新表达。原始思想以
> `docs/42U插件框架设计.md`（ABI v3）与 `docs/thinks/42U插件机架架构设计.md` 为准。
>
> 冻结契约：`src/core/types.ts` + `src/core/status.ts` + `src/core/json.ts`。
> 实现 Agent 不得修改这三个文件；需要变更时上报协调者裁决。

## 1. 保留的核心思想（不可妥协）

1. **机架比喻**：宿主 = 机柜（Rack），插件 = 插进标准槽位的设备。宿主统一编排：
   扫描、加载、排序、初始化、启动、撤销、卸载；插件只通过约定接口接入。
2. **invoke-only**：插件间业务协作的唯一路径是
   `消费者 → 宿主调用网关 → 提供者 invoke`。插件之间不共享业务对象、不传递提供者
   引用；消费者只持有宿主签发的**不透明租约凭据**与按值复制的实际版本。
3. **版本化租约借用**：消费者以 `plugId + 显式版本闭区间` 借用；宿主按区间筛选
   实际版本，返回 `凭据 + 实际版本`。凭据可撤销、用完主动归还；在途调用未结束时
   归还返回 busy。重载后旧凭据永久失效（stale），即使版本完全相同也必须重新借用。
4. **能力披露**：插件在 `start()` 内一次性登记方法集合（方法名、数字 ID、JSON
   schema 描述）；能力集合在实例生命周期内冻结。宿主广播可用/撤回事实；
   `watch()` 提供原子"快照 + 后续变更"订阅。
5. **事件总线数据流通**：普通事件经宿主队列投递，不递归分发、容量有上限；
   `u42.*` 事件名为宿主保留；订阅归属于订阅实例，卸载时统一撤销。
6. **确定性初始化编排**：priority + before/after 硬约束拓扑排序；同优先级按
   plugId 字节序打破平局；环、未知目标、重复 ID 均为启动错误。热加载时
   `after` 可由已初始化实例满足，`before` 面对已初始化实例必须拒绝。
7. **显式卸载协议**：撤能力 → 通知撤销 → 收回租约 → 验证静默（在途调用为 0、
   租约全部归还）→ stop → destroy → 丢弃模块引用。无法证明静默时**隔离**而不是
   强行拆除；绝不通过删除记录伪造安全卸载。
8. **实例代次（generation）**：宿主私有；重载产生新代次，旧凭据、旧订阅一律失效。
9. **插件业务版本 ≠ 框架契约版本**：`PluginVersion`（major.minor.patch，闭区间
   借用）与 `RACK_CONTRACT_VERSION` 互不推断。
10. **生命周期状态机**：created → initialized → active → revoking → stopped →
    (faulted) → destroyed；init 只订阅不广播不调用；start 才登记能力；每实例
    最多一次 init/start，stop 后不可再启动。
11. **同进程治理而非隔离**：框架治理调用与借用关系，不承诺对恶意代码的隔离。
12. **回调内不重入**：插件回调栈内发起的宿主操作按 C++ 规则收敛
    （unload → deferred 排队；boot/load/shutdown → busy）。

## 2. C++ → TS 适配决策表

| C++ 机制 | TS 机制 | 说明 |
| --- | --- | --- |
| C 入口 + 纯虚 ABI + 布局断言 | `RackPlugin` TS 接口 | 无二进制边界，接口即契约 |
| `dlopen`/`dlclose` + RAII 库包装 | 动态 `import()` + 代次参数缓存击穿 | 重载用 `?u42gen=N` 取新 URL 重新求值；旧模块由 GC 回收（JS 无 dlclose，语义等价） |
| 同步单控制线程 | 单事件循环 + async/await | 回调串行化由事件循环天然保证 |
| 状态码返回 | `RackError` 异常（含 `status`） | 生命周期方法可抛任意值，宿主转 `failed` 并记录诊断 |
| 字节视图 + iwriter | JSON 值（`JsonValue`） | 数据面只过 JSON；大小用估算器限制 |
| `wrong_thread` | 删除 | 单线程模型下无意义 |
| plug_id / iid / method_id | plugId / （iid 删除）/ methodId+iid 名 | 框架服务经结构化服务对象注入，不再按 iid 查询 |
| CLI manifest（第二阶段） | 不在本版范围 | TS 版先聚焦机架本体 |
| `call_name`/`call_id` 调用仪式 | 类型化直调代理 `typedApi`（内核网关保留） | 插件作者面对 `storage.set(k, v)` 式直调；代理转发网关完成租约/代次/限额/重入校验 |

### 2.1 关于“去掉 invoke-only”的裁决（用户提出，协调者裁定）

C++ 的 invoke-only 是 ABI 安全的必要约束（禁跨库指针、对象寿命、崩溃隔离），TS 无二进制边界，
仪式感可以去掉。但**纯直连引用被否决**：JS 引用会阻止 GC 回收提供者模块，卸载失去
可执行性，撤销/代次失效/在途排空全部失效，与“可拔插”目标直接冲突。裁定：调用网关
保留为内核机制（拔插治理依赖它），插件作者面对 `typedApi` 类型化直调代理——去掉的是
调用仪式，不是治理本身。

## 3. 模块划分与文件所有权

目录：仓库根 `ts/`，独立 npm 工程。所有相对导入使用 `.ts` 扩展名
（vitest 与 Node 原生 strip-types 双兼容）。

| 模块 | 文件 | 职责 | 归属 Agent |
| --- | --- | --- | --- |
| 契约（冻结） | `src/core/types.ts` `status.ts` `json.ts` | 全部公共类型、状态码、数据面工具 | 协调者（已完成） |
| 版本（冻结层实现） | `src/core/version.ts` + `tests/version.test.ts` | 三元组比较、闭区间校验、acceptsVersion、exactVersion | 协调者（已完成） |
| 编排 | `src/core/order.ts` + `tests/order.test.ts` | 拓扑排序、环检测、热加载规则 | a-order |
| 事件 | `src/core/events.ts` + `tests/events.test.ts` | 有界队列总线、保留名、按 owner 撤销 | a-events |
| 能力 | `src/core/caps.ts` + `tests/caps.test.ts` | 登记/撤回、原子快照+watch | a-caps |
| 租约 | `src/core/lease.ts` + `tests/lease.test.ts` | 凭据铸造、代次绑定、在途计数、撤销协议 | a-lease |
| 调用 | `src/core/calls.ts` + `tests/calls.test.ts` | 名称/ID 同路径网关、校验链、限额 | a-calls |
| 加载 | `src/core/loader.ts` + `tests/loader.test.ts` | 目录发现、动态 import、形状校验、缓存击穿 | a-loader |
| 宿主 | `src/core/host.ts` + `tests/host.test.ts` | 状态机、boot/start/load/unload/reload/poll/shutdown、模块装配 | a-host（第二波） |
| SDK | `src/sdk/*.ts` + `tests/sdk.test.ts` + `examples/plugins/*.plug.ts` | definePlugin 辅助、LeaseHandle 包装、示例插件 | a-sdk |
| 集成 | `src/index.ts` `apps/demo.ts` `tests/e2e.test.ts` `README.md` | 公共导出、端到端场景、全量验证 | a-integration（末波） |

### 3.1 内置标准插件（std.*，随框架发货，非宿主内置）

“万物皆插件”：常用能力全部以标准设备形态上机架，彼此也经租约+网关协作。
全部零新增 npm 依赖（Node 内置 + 全局 fetch）；外部效应经 createWithDeps 注入可测。

| 插件 | plugId | 职责 | 归属 |
| --- | --- | --- | --- |
| 日志 | `std.log` | 内存环形缓冲+控制台/文件 sink，log/query/stats | a-logging |
| 网络工具 | `std.net` | dns.resolve / tcp.probe / url.normalize | a-netutil |
| HTTP 客户端 | `std.http-client` | fetch 封装：超时/重试/大小限额，get/post/put/del | a-http-client |
| REST 客户端 | `std.rest-client` | 租借 std.http-client，RESTful 约定与 CRUD | a-rest-client |
| HTTP 服务器 | `std.http-server` | node:http 路由→一次性租借目标插件→网关调用 | a-http-server |
| REST 服务器 | `std.rest-server` | 租借 std.http-server，资源 CRUD 路由约定 | a-rest-server |
| RPC 网口 | `std.rpc` | JSON-RPC 2.0：服务端桥(挂 http-server) + 客户端 | a-rpc |
| OAuth | `std.oauth` | 授权码/客户端凭证/PKCE，令牌缓存与刷新 | a-oauth |
| 机架 CLI | `apps/rack-cli.ts` | list/describe/call/unload/reload/poll/serve | a-cli |

## 4. 关键时序（宿主职责）

boot：发现（确定性排序）→ 逐个 import + 形状校验 + 重复 plugId 拒绝 → 建实例
（created）→ 计划（拓扑）→ 逐个 init（注入服务；失败则逆序回滚已 init，销毁全部）
→ 全部 init 成功后逐个 start（能力登记 + 广播；Initialized 消费者可收通知、可存
租约、不可调用）→ 全部 Active。

unload(plugId)：撤下能力表 → 广播撤回 → revokeProvider（逐个 onRevoke；未归还
→ 隔离 faulted，卸载失败）→ 验证在途调用为 0 → stop（撤销其订阅、释放其出向
租约）→ destroy → 丢弃模块引用。回调栈内请求 → deferred 队列，poll() 消费。

reload：完整 unload 成功后，以同一 sourcePath、`gen+1` 重新 import；消费者重新
发现、重新借用、重建会话。

shutdown：关闭业务入口 → 按计划逆序对每个实例执行 unload 协议；失败者保留
（隔离），不强行拆除。

## 5. 工程规范

- Node 24 原生可运行：**只准用可擦除 TS 语法**（禁 enum / namespace / 构造器
  参数属性 / const 参数属性）。
- `strict` + `noUncheckedIndexedAccess` + `verbatimModuleSyntax` 全开。
- 注释遵循仓库规范：Doxygen 风格 JSDoc（@brief/@param/@return/@note），公共接口
  必须有注释；用英文，简洁说明意图与约束。
- 每个模块自带 vitest 测试；验收命令：
  `cd ts && npx tsc --noEmit && npx vitest run tests/<本模块>.test.ts`。
- 测试不得依赖其他 Agent 未完成的文件；只允许导入冻结契约与本模块文件。
- 单测中对未来 async 的容忍：契约允许同步或异步返回的实现，测试用
  `await Promise.resolve(...)` 归一。

## 6. 协作规则

- 只写自己名下的文件；不改动 `ts/` 以外的任何内容（读 C++ 源与文档作参考允许）。
- 契约需要变更：完成当前可在现有契约下的工作，然后 `to_agent` 上报协调者。
- 完成后报告：实现要点、测试结果（命令 + 通过数）、对契约的疑点。
