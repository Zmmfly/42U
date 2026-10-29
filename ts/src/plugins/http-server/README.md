# std.http-server

内置标准插件（`plugId: std.http-server`，版本 1.0.0）：机架的 **HTTP 入口设备**。它用
`node:http` 监听端口，把每一条进入的 HTTP 请求翻译为**一次性租借 + 网关调用**——对应 C++
设计中"管理端每次调用：借用 → 调用 → 归还"的范式，然后立刻归还凭据。请求与目标插件之间
不存在任何直接引用，全部经由宿主调用网关。

## 能力与方法

`start()` 内登记 4 个方法（同时开始监听）：

| 方法 | 参数 | 返回 | 错误 |
| --- | --- | --- | --- |
| `route.add` | `{path, httpMethod, targetPlugId, targetMethod, versionRange?}` | `{routeId}` | 重复路由 `duplicate`；非法 path/方法/区间 `invalid_argument` |
| `route.remove` | `{routeId}` | `null` | 未知 `routeId` → `not_found` |
| `routes.list` | `null` | `{routes:[{routeId, path, httpMethod, targetPlugId, targetMethod, versionRange}]}` | - |
| `stats` | `null` | `{requests, errors, active, port}`；`port` 为实际监听端口（`U42_HTTP_PORT=0` 时为内核分配的临时端口，未监听时为 `null`），供集成方发现监听地址 | - |

- `path` 支持 `:param` 占位（如 `/api/:id`），参数名在一条路由内必须唯一；字面段仅允许
  `[A-Za-z0-9_.-]`；必须以 `/` 开头、不允许空段与尾部 `/`。
- `httpMethod` 限 `GET/POST/PUT/DELETE/PATCH/HEAD/OPTIONS`（区分大小写）。
- `versionRange` 省略时使用全区间 `[0.0.0, MAX_SAFE_INTEGER.*.*]`；显式给出时必须为
  合法的闭区间 `{minimum, maximum}`。
- 重复路由判定：`path + httpMethod + targetPlugId + targetMethod` 四元组相同即重复
  （`versionRange` 不参与判定）。

## 请求处理流程

1. **匹配**：`httpMethod + path` 匹配（`:param` 解码提取）。路径无任何路由 → `404`；
   路径命中但方法不符 → `405`（附 `Allow` 头）。
2. **读体**：请求体上限 4 MiB（先看 `Content-Length`，再累计分片），超限 → `413`，
   之后丢弃剩余字节但不缓冲，保证 413 响应仍能送达客户端。
3. **一次性租借**：`caps.acquire(targetPlugId, range, revoker)` →
   `calls.call(credential, targetMethod, args)` → `handle.release()`。并发请求各自独立
   租借；业务错误路径同样保证归还（`finally`）。
4. **响应**：成功 `200 {ok:true, result}`；目标抛错 `200 {ok:false, error:{status,message}}`。

转发给目标方法的 `args` 恒为：

```json
{
  "query":   { "<键>": "<首个/最后一个值>" },
  "params":  { "<路径参数名>": "<解码值>" },
  "headers": { "<小写头名>": "字符串或字符串数组" },
  "body":    <解析后的 JSON，非 JSON 体或空体为 null>
}
```

- `query` 由 `URLSearchParams` 展开，重复键后者覆盖前者。
- `headers` 直接取 `node:http` 的小写头表；同名多值头保持字符串数组。
- `body` 仅做 `JSON.parse`：非法 JSON 不报错，置为 `null`。

## HTTP 状态码映射（传输错误 vs 业务错误）

| 场景 | HTTP 码 | 信封 |
| --- | --- | --- |
| 路径未匹配任何路由 | 404 | `{ok:false, error:{status:'not_found'}}` |
| 路径命中但方法不符 | 405 | `error.status:'unsupported'`，附 `Allow` |
| 请求体超过 4 MiB | 413 | `error.status:'limit_exceeded'` |
| `acquire` 报 `not_found`/`unsupported` | 503 | 透传实际 rack status |
| `acquire` 其他失败 / 内部错误 | 500 / 400 | 透传实际 rack status |
| **目标方法抛错（业务错误）** | **200** | `{ok:false, error:{status, message}}` |

**取舍**：目标插件的业务拒绝（校验失败、资源不存在等）与桥本身的传输/网关故障是两类
错误。前者统一 `200 + ok:false`，调用方永远可以依赖"非 200 = 桥或租借层出了问题"这一
不变式；业务语义只在信封里表达。`stats.errors` 统计所有未产生 `ok:true` 的请求（含
4xx/5xx 与 `ok:false`）。

## 监听配置

`start()` 按以下优先级确定监听地址：

1. `createWithDeps({listen: {port, host}})` 显式注入（测试传 `port: 0` 用临时端口）；
2. 环境变量 `U42_HTTP_PORT`（默认 `8080`）、`U42_HTTP_HOST`（默认 `127.0.0.1`）；
3. 非法端口值（非 0..65535 整数）在 `start()` 内抛 `invalid_argument`。

`stop()`：关闭服务器（`close()` + `closeIdleConnections()`），并等待全部在途请求结算
（响应写完、租借归还）后返回；已 stop 的实例再次 `start()` 抛 `invalid_state`。路由表
保留在实例内，不随 stop 清空。

## 依赖注入与导出

零新增 npm 依赖（仅 `node:http` 与全局 `URL`）。本模块导出：

- `create()`：默认工厂，真实 `node:http` 服务器 + env 监听配置；
- `createWithDeps(deps)`：可注入 `createServer` 工厂（默认真实）、`listen` 覆盖、
  `onListening` 回调（端口 0 时取回临时端口）；
- 类型 `HttpServerDeps` / `HttpServerLike` / `HttpServerFactory` / `HttpListenAddress`。

测试位于 `tests/plugins/http-server.test.ts`：真实 `node:http` 监听 + 自写 HTTP 客户端，
`PluginServices` 用自写 fake 记录 `acquire/call/release` 断言一次性租借语义。

## 示例

`examples/plugins/http-server-demo.plug.ts` 演示插件侧标准用法：`start()` 中先登记自身
`hit` 能力，再一次性租借 `std.http-server` 调 `route.add` 注册 `GET /demo/:name` 指向
自身 `hit`；`hit` 返回调用计数与收到的 args；`stop()` 尽力移除路由。它通过
`init.after: ['std.http-server']` 保证启动顺序。
