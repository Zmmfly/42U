# std.http-client 标准设备

`std.http-client`（业务版本 1.0.0）是 42U 机架 TS 版的内置标准 HTTP 客户端：对全局 `fetch`
的生产级封装，提供单次尝试超时、有限重试与线性退避、响应体大小守卫。万物皆插件——本设备
与其他插件一样通过「能力披露 + 版本租约 + 调用网关」被消费（参考
`examples/plugins/http-client-demo.plug.ts`）。

- 零新增 npm 依赖：仅使用全局 `fetch`、全局 `AbortController` 与 WHATWG 全局对象
  （`URL` / `TextDecoder` / `TextEncoder`）。
- 外部效应可注入：`createWithDeps({ fetchImpl, maxBytes })` 供测试注入假 fetch；
  默认 `create()` 绑定 `globalThis.fetch`。两者均从本模块导出。
- 数据面全部为 `JsonValue`；错误一律抛 `RackError`。

## 方法（id 连续分配 1..5）

| id | 方法 | 说明 |
| --- | --- | --- |
| 1 | `request` | 全量参数的通用 HTTP 请求 |
| 2 | `get` | `request` 快捷方式，HTTP 方法固定为 GET |
| 3 | `post` | 同上，固定 POST |
| 4 | `put` | 同上，固定 PUT |
| 5 | `del` | 同上，固定 DELETE |

快捷方法的参数与 `request` 相同但**不允许再传 `method`**（传了 → `invalid_argument`），
需要自定义动词时请用 `request`。

### request 参数

| 字段 | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `url` | string | 必填 | 绝对 http(s) URL；非法或非 http(s) → `invalid_argument` |
| `method` | string | `'GET'` | HTTP 动词，大小写不敏感，自动归一为大写；非动词 → `invalid_argument` |
| `headers` | object | 无 | 请求头，键为非空字符串、值为字符串 |
| `json` | JsonValue | 无 | JSON 请求体；自动补 `content-type: application/json`（调用方显式给出 content-type 时不覆盖） |
| `text` | string | 无 | 字符串请求体；与 `json` 互斥 |
| `timeoutMs` | number | `10000` | 单次尝试超时（正整数），覆盖从发起到响应体读完 |
| `retries` | number | `0` | 首次尝试之外的追加次数（非负整数） |
| `retryBackoffMs` | number | `100` | 线性退避基数：第 n 次失败后等待 `n × retryBackoffMs` |
| `expectOk` | boolean | `false` | 为 true 且最终状态非 2xx → 抛 `failed`（见下） |

GET/HEAD 携带 `json`/`text` 请求体 → `invalid_argument`（先于 fetch 拦截）。

### 结果

```json
{ "status": 200, "ok": true, "headers": { "...": "..." }, "json": { } }
```

| 字段 | 说明 |
| --- | --- |
| `status` | HTTP 状态码 |
| `ok` | 是否 2xx |
| `headers` | 响应头对象，键统一小写；多行 `set-cookie` 以 `", "` 连接 |
| `json` | content-type 含 `json` 且可解析时给出解析后的值 |
| `text` | 其余情况给出原始文本（content-type 标了 json 但解析失败时回退为 `text`） |

## 错误映射（RackError）

| status | 触发条件 |
| --- | --- |
| `invalid_argument` | 参数校验失败（url/method/headers/body/数值字段/快捷方法携带 method 等） |
| `not_found` | 未知方法名或方法 id |
| `limit_exceeded` | 响应体累计字节数超过上限（读流即中断；不消耗重试预算） |
| `failed` | 重试预算耗尽后仍网络层失败，detail 含 `attempts`/`method`/`url`/`lastError`；或 `expectOk` 且非 2xx，detail 含 `status` 与 `body`（前 200 字符摘要）；或请求被 `stop()` 中止（detail `reason: 'stop'`） |
| `invalid_state` | `stop()` 之后的防御性拒绝（主要靠宿主状态门） |

## 超时、重试与退避

- 每次尝试独立的 `AbortController`；`timeoutMs` 覆盖该次尝试从发起到**响应体读完**的全过程，
  读体超时同样按网络层失败处理。
- 仅两类情况消耗重试预算：网络层错误（fetch reject，含超时）与 5xx 响应；4xx 与其他状态
  照常返回不重试。
- 退避为线性：第 n 次失败后等待 `n × retryBackoffMs` 再发起下一次尝试。
- 重试放弃的 5xx 响应体会被取消（不读取），以尽快释放连接。

## 响应体大小守卫

- 响应体按流式分块累计计数，默认上限 **4 MiB**（4×1024×1024 字节）。
- 环境变量 `U42_HTTP_MAX_BYTES`（正整数）覆盖默认值；非法或未设置时回退默认。
  `createWithDeps({ maxBytes })` 的显式参数优先级最高。
- 超限时先 `reader.cancel()` 中断读流，再抛 `limit_exceeded`（detail 含 `limitBytes`）。

## stop() 语义

- 中止全部在途请求（abort 信号），取消全部待执行的退避等待，并等待所有在途 invoke
  结算后才返回。
- 之后到达的 invoke 在插件侧防御性抛 `invalid_state`（宿主状态门是第一道屏障）。
- 实例按机架契约不会重新启动。

## 与宿主资源限额的关系

宿主会按 `RackOptions.outputLimit`（默认约 1 MiB，`estimateJsonBytes` 为偏大估算）检查调用
结果；若部署方把本插件的 4 MiB 守卫调大，超大结果仍可能先被宿主拒绝。需要传递大响应体时
请同步调大宿主限额。

## 测试

`tests/plugins/http-client.test.ts` 只使用自写 fake services 与注入的 fake fetch（记录调用、
可编程响应/挂起），不访问网络、不依赖宿主实现：

```sh
cd ts && npx vitest run tests/plugins/http-client.test.ts
```
