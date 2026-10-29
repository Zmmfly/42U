# std.log 标准日志插件

随框架发货的标准日志设备（`std.log`，版本 1.0.0）。它不是宿主内置功能，而是机架上的
一台 `std.*` 设备：其他插件经能力披露 + 版本化租约 + 调用网关使用它，与任何第三方
插件的地位完全相同。

- 实现：`ts/src/plugins/logging/index.ts`
- 演示消费者：`ts/examples/plugins/logging-demo.plug.ts`
- 测试：`ts/tests/plugins/logging.test.ts`

## 能力与方法

`start()` 时一次性披露以下方法集合（实例生命周期内冻结）：

| 方法 | ID | args | 结果 |
| --- | --- | --- | --- |
| `log` | 1 | `{level:'info'\|'warn'\|'error', message:string, fields?:object, plugId?:string}` | `{accepted:true}` |
| `query` | 2 | `{level?, since?(epoch ms, 含), limit?=100}` | `{entries:[...], total}` |
| `stats` | 3 | 无（`null`） | `{byLevel:{info,warn,error}, capacity, used}` |

语义要点：

- **条目字段冻结**：`query` 返回的每个条目恒为 `{ts, level, message, fields, plugId}`；
  `log` 省略的 `fields`/`plugId` 在条目中为 `null`。条目对象本身被冻结。
- **环形缓冲**：默认 4096 条，写满后驱逐最旧条目；`log` 热路径仅构造一个冻结条目并
  按引用存入，不做多余拷贝。
- **query 过滤**：`level` 精确匹配、`since` 为 `ts >= since`（含端点）、`limit` 取
  **最新的 limit 条**（默认 100），返回结果仍按 `ts` 升序；`total` 是应用 `limit`
  之前的匹配总数，便于判断是否截断。
- **plugId 自报**：调用方在 `log.plugId` 中声明自己；框架网关不注入调用者身份。
- **错误**：args 不合法抛 `RackError('invalid_argument', ...)`，未知方法抛
  `RackError('not_found', ...)`，`stop()` 之后的调用抛
  `RackError('invalid_state', ...)`。

## Sink 行为

每条被接受的日志按顺序写入两个尽力而为的 sink，任何 sink 失败都不影响
`{accepted:true}` 的返回：

1. **控制台 sink**（常开）：向 `stderr` 写一行 JSON（即条目本身加换行）。
2. **文件 sink**（可选）：`U42_LOG_FILE` 设置时用 `node:fs/promises` 追加同一行
   JSON。追加按调用顺序串行化；首次失败后静默降级为仅内存，之后不再重试。

## 环境变量（仅本插件范围内约定）

| 变量 | 缺省 | 说明 |
| --- | --- | --- |
| `U42_LOG_RING` | `4096` | 环形缓冲容量；非法值（非正整数、非数字）回退默认 |
| `U42_LOG_FILE` | 未设置 | 文件 sink 路径；未设置或空字符串表示禁用 |

两个变量都在实例构造时读取一次，不在 `log` 热路径上重复解析。

## 消费方式（插件作者视角）

消费者不导入本模块的任何运行时对象，只复制数据契约常量（plugId、接受版本、方法名），
然后按 watch → acquire → call 流程使用：

```ts
import { exactVersion, typedApi } from '../src/sdk/index.ts';

const LOG_RANGE = exactVersion({ major: 1, minor: 0, patch: 0 });
// init() 中：caps.watch(...) 收到 available 且版本过区间后
const handle = services.caps.acquire('std.log', LOG_RANGE, revoker);
// start() 之后的业务路径：
const logger = typedApi<{ log(args: JsonObject): Promise<JsonValue> }>(services.calls, handle);
await logger.log({ level: 'warn', message: 'disk nearly full', plugId: 'my.plugin' });
```

`logging-demo.plug.ts` 是完整参考：初始化时 watch、按精确版本 1.0.0 借用租约、
`start()` 后经网关记录三条演示日志，`run {message}` 方法再触发一次 `log` 调用；
租约被撤销时停止业务并归还凭据。

## 宿主诊断桥接：formatDiagEntry

模块导出纯工具 `formatDiagEntry(level, message, detail?)`，把
`DiagService.report(level, message, detail?)` 的参数整形成 `log` 方法的 args
形状，供宿主把诊断记录桥接到机架日志设备：

- `detail` 是 JSON 对象 → 直接作为 `fields`；
- `detail` 是其他 JSON 值 → 包装为 `{ detail: <值> }`，不丢信息；
- `detail` 缺省或 `null` → 不带 `fields`。

```ts
import { formatDiagEntry } from '../src/plugins/logging/index.ts';
// 桥接：把宿主诊断转成 std.log 调用参数
const args = formatDiagEntry('warn', 'provider start failed', { plugId: 'std.net' });
await gateway.call(credential, 'log', args);
```

## 测试注入：createWithDeps

`create()` 绑定真实实现（`Date.now`、`process.env`、`process.stderr`、
`node:fs/promises.appendFile`）。单测使用 `createWithDeps(deps)` 注入外部效应，
注入的环境视图**整体替换** `process.env`（不合并），保证测试密闭：

```ts
const plugin = createWithDeps({
  now: () => 1234,                          // 确定时间戳
  env: { U42_LOG_RING: '3', U42_LOG_FILE: tmpPath },
  stderr: () => undefined,                  // 静音控制台 sink
  appendFile: async (path, data) => { /* 记录追加 */ },
});
```

`stop()` 会等待文件追加链排空、清空环形缓冲并释放服务引用；`destroy()` 无额外资源
需要释放，因此未提供。
