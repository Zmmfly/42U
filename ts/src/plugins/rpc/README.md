# std.rpc — JSON-RPC 2.0 网口插件

`std.rpc` 是 42U 机架的"后面板网口"：把机架能力以 **JSON-RPC 2.0** 暴露到 HTTP（服务端桥），
并提供调用远端 RPC 的客户端方法——机架间互连的雏形。传输层逻辑无关，兼容 JSON-RPC 生态。

- **plugId**：`std.rpc`
- **版本**：1.0.0
- **依赖**：零新增 npm 依赖（全局 `fetch` + Node 内置）；生产实现经 `createWithDeps(fetchImpl)`
  注入外部效应，测试注入假 fetch。
- **发布方法**：`bridge`（服务端桥入口，供 std.http-server 回调）、`call`、`batch`（客户端）。

## 服务端桥

### 挂载与卸载

- `init()` 订阅能力 watch；`std.http-server` 可用且版本落在 `[1.0.0, 1.MAX]` 时租借
  （仅 Initialized 也允许租借，挂端点属于业务、等待 `start()`）。
- `start()`：发布能力集合，并对已持有的租约执行 `route.add` 注册一条 `POST /rpc` 路由。
  若启动时 http-server 尚不可用，后续可用事件会补挂（热挂载）。
- `stop()`：中止在途客户端请求（AbortController）→ 退订 watch → `route.remove` 卸端点 →
  归还 http-server 租约。Revoker 被触发时清状态并归还凭据。
- 非 POST 或路径不符的请求由 std.http-server 自行 404/405 兜底，本插件不参与。

### 与 std.http-server 的对接契约（已按真实 std.http-server v1.0.0 对齐）

| 方向 | 网关方法 | args | 结果 |
| --- | --- | --- | --- |
| 本插件 → http-server | `route.add` | `{path:'/rpc', httpMethod:'POST', targetPlugId:'std.rpc', targetMethod:'bridge', versionRange:[1.0.0,1.MAX.MAX]}` | `{routeId}`（四元组重复 → `duplicate`） |
| 本插件 → http-server | `route.remove` | `{routeId}` | `null`（未知 routeId → `not_found`，卸载时容忍） |
| http-server → 本插件 | `bridge` | `{query, params, headers, body}`；`body` 为已解析 JSON，坏 JSON/空体为 `null` | JSON-RPC 应答信封，HTTP 侧包为 `200 {ok:true, result}` |

http-server 按路由的 `versionRange` 一次性租借 `std.rpc` 并网关调用 `bridge`；`bridge` 忽略
`query/params/headers`，只消费 `body`（也兼容裸字符串/文本 body 的其他桥接方）。

### JSON-RPC 请求协议

```json
{ "jsonrpc": "2.0", "id": 7, "method": "<plugId>.<methodName>", "params": {}, "versionRange": null }
```

- `id`：必填，string / number / null；不支持 notification（无 id 视为非法请求）。
- `method`：按【最后一个点号】切分为 `<plugId>` 与 `<methodName>`：plugId 可含点号
  （如 `example.storage.get` → `example.storage` + `get`），方法名因此不得含点号；两段均非空。
- `params`：可省略或为 `null`（等价 `{}`）；存在时必须是对象或数组，原样作为网关 args。
- `versionRange`：本机架扩展字段，`{minimum:{major,minor,patch}, maximum:{...}}` 闭区间；
  省略时默认**全区间**（`0.0.0` 到 `MAX_SAFE_INTEGER`）。结构非法或倒序 → `-32602`。

处理流程：一次性租借目标（按请求的 versionRange）→ `calls.call(credential, methodName, params ?? {})`
→ release → 应答 `{jsonrpc:'2.0', id, result}` 或 `{jsonrpc:'2.0', id, error:{code,message,data?}}`。

### 错误码映射

| 来源 | JSON-RPC code | `error.data` |
| --- | --- | --- |
| 请求体不可解析/为空（http-server 转发为 `body:null`） | `-32700` | 无 |
| 非法请求（版本/形状/id/method/params） | `-32600` | 无 |
| 租借或调用抛 `not_found` | `-32601` | `'not_found'` |
| 抛 `invalid_argument`（含 versionRange 非法） | `-32602` | `'invalid_argument'` |
| 其余（`failed`/`unsupported`/`stale`…） | `-32603` | 对应 RackStatus 字符串 |

JSON-RPC 层错误以**正常结果** riding 在 `{ok:true, result}` 内返回；只有 `bridge` 自身抛错才会
出现 `{ok:false, error:{status,message}}`（如分派文档无 body → `invalid_argument`）。

### batch

- 请求体为数组：**串行**逐条执行，整体应答一个**等长等序**的信封数组（每条独立成败）。
- 空数组按 JSON-RPC 2.0 规范应答单个 `-32600` 信封。

## 客户端方法

### `call`

```json
{ "url": "http://peer:port/rpc", "method": "<plugId>.<methodName>", "params": {}, "timeoutMs": 10000 }
```

- 发送 `POST url`（`content-type: application/json`），体为
  `{jsonrpc:'2.0', id:<自增>, method, params}`；`params` 省略/为 null 时发 `{}`。
- `timeoutMs` 默认 `10000`，正整数；超时覆盖整个交换（fetch + 读体）。
- 成功 → 返回 `result`。应答兼容两种传输：裸 JSON-RPC 信封，或机架 http-server 的
  `{ok:true, result}` / `{ok:false, error:{status,message}}`（后者直接映射为对应 status 的
  RackError）。远端 JSON-RPC `error` 对象映射回 `RackError`：

| 远端 code | RackError.status |
| --- | --- |
| `-32601` | `not_found` |
| `-32602` | `invalid_argument` |
| 其他 | `failed`（`detail` 携带 `{code, data?}`） |

- 网络失败 / 超时 / 应答非 JSON / 应答形状非法 / 应答 id 不匹配 → `RackError('failed', ...)`。
- 参数校验失败（url/method 缺失、timeoutMs 非法）→ `invalid_argument`，不触网。

### `batch`

```json
{ "url": "...", "calls": [{ "method": "a.b", "params": {} }], "timeoutMs": 10000 }
```

- 一次 POST 发送整个请求数组（id 连续自增），应答必须为等长数组，否则整体 `failed`。
- 返回与输入**等长等序**的结果数组，每项为二选一（顶层判别键，`result` 内嵌套任意值不冲突）：

```json
{ "result": <JsonValue> }
{ "error": { "code": -32601, "message": "...", "data": null, "status": "not_found" } }
```

- `error.status` 是远端 code 回映后的 RackStatus；单条失败不使整体抛错。

## 生命周期与并发

- 单事件循环内串行处理；服务端每请求一次性租借目标并在 finally 归还（release 返回
  `busy`/`stale` 时记 warn 诊断）。
- `stop()` 中止所有在途客户端交换（调用方收到 `failed`）；宿主卸载协议保证 stop 时无在途
  网关调用，插件自身不再追踪。
- 所有错误以 `RackError` 抛出；args 与结果均为 `JsonValue`。

## 可测性

- `createWithDeps(fetchImpl: RpcFetchImpl)`：注入 fetch 假实现（类型随模块导出）。
- `create()`：绑定全局 `fetch`。
- 单测见 `ts/tests/plugins/rpc.test.ts`：真实 node:http 临时端口端到端 + 假网关/假目标插件。

## 示例

```bash
curl -X POST http://127.0.0.1:<port>/rpc \
  -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"example.rpc-demo.ping","params":{"n":1}}'
# -> {"jsonrpc":"2.0","id":1,"result":{"pong":true,"received":{"n":1}}}
```

演示插件见 `ts/examples/plugins/rpc-demo.plug.ts`（`ping` 提供者 + 经自身客户端调用本地端点）。
