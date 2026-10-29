# TS 版 42U 机架重写（2026-10-30 01:01:32）

## 目标与结果

- 目的：分析既有 C++ 42U 插件机架的设计思想，用 TypeScript 在新目录 `ts/` 重写一份不被 C++ 实现束缚、保留核心设想的框架，具备数据互通与可拔插组件加载/卸载，并按追加要求补齐日志、网络、RPC、REST/HTTP、OAuth 等常用能力，交付"拿来就能用"的成品；全程多 Agent（glm-5.3 max）协作，现有 C++ 代码不动。
- 状态：已完成。`ts/` 独立 npm 工程（Node 24 + vitest + tsc，零运行时依赖），含机架引擎、8 个 std 标准插件、CLI、示例与端到端演示；`npx tsc --noEmit` 零错误、`npx vitest run` 415/415 通过、`node apps/demo.ts` 8/8 幕通过、CLI 真实机架冒烟通过。

## 方案与取舍

- 保留的思想（对照 `docs/42U插件框架设计.md` ABI v3）：宿主统一编排的机架模型、先全量 init 再按计划 start、确定性初始化编排（priority + before/after 拓扑、字节序平局、环检测、热加载规则）、start() 一次性能力披露与原子"快照+watch"、有界事件总线（`u42.*` 保留名、不递归派发）、版本闭区间租约借用（凭据+实际版本、busy/stale、撤销协议）、显式卸载协议（撤能力→收租约→静默验证→stop→destroy→隔离而非强拆）、实例代次（重载旧凭据永久失效，同版本也必须重借）、回调内 unload→deferred。
- TS 适配决策：C 链接+纯虚 ABI → TS 接口契约；dlopen/dlclose → 动态 `import()` + 代次参数击穿 ESM 缓存（旧模块 GC 回收，语义等价并如实文档化）；状态码返回 → `RackError(status)` 异常；字节视图/iwriter → JSON 数据面 + 大小估算限额；`wrong_thread` 删除（单事件循环天然串行）。
- invoke-only 裁决（用户提出弱化）：纯直连引用被否决——JS 引用会阻止 GC 回收提供者模块，卸载失去可执行性；改为内核保留调用网关（拔插治理依赖），插件作者面对 `typedApi` 类型化直调代理，去掉调用仪式而非治理（design.md §2.1）。
- 常用能力形态：全部做成随框架发货的 `std.*` 标准插件而非宿主内置（"万物皆插件"）；插件消费插件（rest-client 租借 http-client、rest-server/rpc 租借 http-server、HTTP/RPC 入口一次性租借目标插件）；零新增 npm 依赖（Node 内置 + 全局 fetch）。
- std.rpc 客户端无法回环自家 /rpc（同实例在途被网关 busy 拒绝，忠实保留"同实例不重入"纪律）：裁决保持网关语义，demo/e2e 双路演示并写入 README 已知边界。
- 模型选择：用户要求 glm-5.5 不存在，经确认改用 `self/glm-5.3` + max 思考等级；工具链经确认 npm + vitest + typescript。

## 工作内容

- 调研：通读设计主文档、架构思考稿、ABI v3 头、宿主门面、示例与 C++ 实现。
- 协调者（本 Agent）搭地基：`ts/` 工程（package/tsconfig/vitest 配置）、冻结契约 `src/core/{types,status,json,version}.ts`、设计文档 `ts/docs/design.md`（含 Agent 分工与协作规则），后续按各波次产出增量维护。
- 第一波 8 Agent 并行：order（拓扑编排）、events（事件总线）、caps（能力注册表）、lease（租约管理）、calls（调用网关）、loader（动态加载）、services（插件服务视图）、sdk（definePlugin/typedApi + echo/storage/consumer 示例）。
- 第二波按用户扩容补满 10 并发槽：a-host（机架引擎 createRack：状态机、boot/start/load/unload/reload/poll/shutdown、deferred 队列、失败回滚）+ 9 个标准件/工具 Agent（log、net、http-client、rest-client、http-server、rest-server、rpc、oauth、rack-cli）。
- 集成波：`src/index.ts` 公共面、`src/plugins/index.ts` 标准件目录、rest-server 与 http-server 的 route.add 实参契约对齐（`httpMethod`）、http-server stats 增加 port、修复 5 处真实装配缺陷（非可擦除语法、start() 期 Active 门冲突的延迟挂载模式）、`apps/demo.ts` 八幕演示、`tests/e2e.test.ts`、README 重写。
- 协调者收官：清理 8 个 `.js` 编译残留与 1 个重复 plugId 的杂散文件；CLI 增加 `--with-std`（含 Node parseArgs 串肉签键行为修正：选项键必须 `'with-std'`）；补 CLI 文档。
- 全程未修改 `ts/` 以外任何文件，未执行 git commit；C++ 版本原样未动。

## 验证

- `cd ts && npx tsc --noEmit` → exit 0，零错误（strict + noUncheckedIndexedAccess + verbatimModuleSyntax 全开）。
- `cd ts && npx vitest run` → Test Files 20 passed (20)，Tests 415 passed (415)。
- `cd ts && node apps/demo.ts` → 演示完成：8/8 步全部通过 ✓（快照/typedApi 直调/事件写穿闭环/REST 真实 HTTP/RPC 双路回环/日志/卸载失效+重载重借/优雅 shutdown）。
- `cd ts && node apps/rack-cli.ts version` → `rack-cli 0.1.0` / `rack contract version 1`。
- `cd ts && U42_HTTP_PORT=18197 node apps/rack-cli.ts list --dir examples/plugins --with-std` → 19 个设备全部 active（8 std + 11 示例）。
- 注：本机 8080 端口被其他服务占用，故涉及 std.http-server 默认端口的命令需 `U42_HTTP_PORT` 指定空闲端口；属环境约束，非代码缺陷。

## 遗留

- `example.rpc-demo` 的 go 方法指向自家 /rpc 会收到 busy 映射失败（README 已知边界），后续可改为指向外部机架或 mini 端点。
- C++ 版"网口=子进程 RPC/MCP 兼容"仅以 std.rpc（JSON-RPC 2.0 over HTTP）落地雏形，子进程隔离与 MCP 协议细节未实现。
- 单进程可信插件模型：无沙箱、无崩溃隔离、无强制超时（与 C++ 第一阶段一致）；跨线程/跨进程投递协议未设计。
- CLI `serve` 的 HTTP/RPC 组合挂载尚未自动化（demo.ts 已示范组合方式）；可按 demo 模式扩展 serve --with-std 的自动挂载。
