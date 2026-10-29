# rack-cli 使用文档

`apps/rack-cli.ts` 是 42U TS 版机架的命令行入口，面向运维与开发，直接管理一个真实机架：
列出插件、查看实例、按名调用、卸载/重载、事件排空与常驻服务。零第三方依赖，仅使用
Node 内置能力（`node:util` 的 `parseArgs`、`console`、`process`）。

## 运行方式

```bash
# 在 ts/ 目录下（Node ≥ 20.11，仓库使用 Node 24 原生类型擦除直接运行 .ts）
node apps/rack-cli.ts <command> [args] [--json] [--dir <directory>] [--with-std]
```

- `--dir`：插件目录，默认 `examples/plugins`（相对当前工作目录），对所有操作机架的命令可用。
- `--with-std`：boot 前额外装载随框架发货的全部 std.* 标准件（日志/网络/HTTP 客户端与服务器/REST/RPC/OAuth）；目录插件声明 std.* 初始化硬边时必带。
- `--json`：所有命令都支持，输出单行机器可读 JSON。
- 退出码：`0` 成功，`1` 运行错误（RackError status + message），`2` 用法错误（help 打印到 stderr）。

## 命令表

| 命令 | 参数 | 作用 |
| --- | --- | --- |
| `version` | | 打印 CLI 版本（0.1.0）与 `RACK_CONTRACT_VERSION`；不构造机架 |
| `list` | `[--dir <dir>]` | boot → `plugins()` 快照（表格：plugId/version/state/generation）→ shutdown |
| `describe` | `<plugId>` | boot → `describe(plugId)` 的 InstanceInfo JSON（未知 ID 报 `not_found`，退出码 1）→ shutdown |
| `call` | `<plugId> <methodName> <jsonArgs>` | boot（内置 CLI caller 插件）→ 一次性租借（全区间 `[0.0.0, MAX]`）→ `calls.call` 按名调用 → 打印 JSON 结果 → release → shutdown |
| `unload` | `<plugId>` | boot → 完整卸载协议 → shutdown |
| `reload` | `<plugId>` | boot → 卸载并以同一 sourcePath 重新加载（代次 +1，旧凭据失效）→ shutdown |
| `poll` | | boot → 排空队列事件与 deferred 卸载 → shutdown |
| `serve` | `[--dir <dir>]` | boot 后常驻运行：30s 心跳保活，SIGINT/SIGTERM 优雅 shutdown；HTTP/RPC 挂载由集成阶段组合，此处只保活机架 |
| `help` | | 打印帮助到 stdout |

## 输出形状

| 命令 | 文本模式 | `--json` 模式（单行） |
| --- | --- | --- |
| `version` | 两行：`rack-cli 0.1.0`、`rack contract version 1` | `{"cliVersion":"0.1.0","contractVersion":1}` |
| `list` | 对齐表格（含表头）或 `no plugins loaded ...` | InstanceInfo 数组 `[{plugId,version,state,generation,sourcePath},...]` |
| `describe` | 缩进多行 JSON | InstanceInfo 对象（单行） |
| `call` | 缩进多行 JSON | **裸调用结果**（便于 `\| jq` 管道） |
| `unload`/`reload` | `unloaded <plugId>` / `reloaded <plugId>` | `{"command":"unload","plugId":"...","ok":true}` |
| `poll` | `poll complete` | `{"command":"poll","ok":true}` |
| `serve` | 启动横幅 + 插件清单 + `rack shut down` | 启动一行 `{"command":"serve","status":"running","dir":...,"plugins":[...]}`，优雅退出后一行 `{"command":"serve","status":"stopped"}` |

运行错误统一输出到 **stderr**：文本模式为 `rack-cli: error [<status>] <message>`；
`--json` 模式为单行 `{"command":"<cmd>","error":{"status":"<status>","message":"..."}}`。
用法错误总是把帮助文本打印到 stderr，退出码 2。

## 示例

```bash
cd ts

# 查看版本（a-host 未就绪时也可用）
node apps/rack-cli.ts version
node apps/rack-cli.ts version --json

# 列出默认目录中的插件
node apps/rack-cli.ts list
node apps/rack-cli.ts list --dir examples/plugins --json

# 查看单个实例
node apps/rack-cli.ts describe example.storage
node apps/rack-cli.ts describe example.storage --json

# 一次性租借调用（参数是 JSON 文档）
node apps/rack-cli.ts call example.storage get '{"key":"example.data"}'
node apps/rack-cli.ts call example.storage set '{"key":"k","value":[1,2,3]}' --json

# 热插拔与事件排空
node apps/rack-cli.ts reload example.storage
node apps/rack-cli.ts unload example.storage
node apps/rack-cli.ts poll

# 常驻服务（Ctrl+C 优雅关停；第二个 Ctrl+C 强制退出）
node apps/rack-cli.ts serve
node apps/rack-cli.ts serve --dir examples/plugins --json
```

## `call` 的一次性租借语义

- 冻结的 `Rack` 门面不暴露 caps/calls 服务视图，CLI 通过 `rack.add()` 暂存一个内置
  caller 插件（`cli.caller`，不发布任何能力），在 `init()` 中捕获注入的
  `PluginServices`，从而像普通消费者一样租借与调用。
- 若宿主的 `boot()` 不启动暂存实例，CLI 会退回显式 `start()`，两种宿主实现都兼容。
- 租借区间为全区间 `[0.0.0, MAX_SAFE_INTEGER.MAX_SAFE_INTEGER.MAX_SAFE_INTEGER]`，
  由宿主绑定实际披露版本与当前代次。
- 调用失败也会在 finally 中归还租约；租借/调用失败打印 RackError 的 status 与
  message，退出码 1。

## 宿主未就绪时的行为

CLI 不静态导入 `src/core/host.ts`（由 a-host 并行实现，可能尚未存在）。默认工厂在
首次使用时动态 `import('../src/core/host.ts')` 并取 `createRack`：

- 文件就绪并导出 `createRack`：正常工作，`--dir` 映射为
  `RackOptions.pluginDirectories`。
- 尚未就绪：输出友好的 `failed` 错误（stderr），退出码 1。`version` 与 `help`
  完全不触达宿主，任何时候可用。

## 实现结构与测试

- `apps/rack-cli.ts`：入口（entry 判定、console/process 适配、退出码写回）。
- `apps/cli/commands.ts`：命令解析、分发、退出码映射、`serve` 保活（`nodeKeepAlive`
  可注入，测试用立即停止的替身模拟 SIGINT）。
- `apps/cli/rack-access.ts`：`RackFactory`（可注入 fake Rack）、`runWithRack`
  （boot → 操作 → 尽力 shutdown → 返回结果）、CLI caller 暂存、默认动态导入工厂。
- `apps/cli/output.ts`：行式输出端口、表格/JSON 渲染。
- `tests/cli.test.ts`：全部注入 fake Rack（实现冻结 `Rack` 接口并记录调用序列），
  覆盖各命令输出、`--json` 字段、call 的 acquire→call→release 顺序与参数、
  错误映射退出码、用法错误 exit 2、serve 注入点。

验证命令：

```bash
cd ts
npx vitest run tests/cli.test.ts
npx tsc --noEmit  # 全仓类型检查（含 apps/rack-cli.ts apps/cli/*.ts）
```
