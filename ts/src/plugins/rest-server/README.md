# std.rest-server（RESTful 资源服务器）

`std.rest-server` 是 42U 机架 TS 版的内置标准插件（`std.*`），是继 `example.consumer`
之后第二个「插件消费插件」示范：它自己不监听任何端口，而是**租借 `std.http-server`**，
把「挂载一个 RESTful 资源」的声明翻译成一组约定 CRUD 路由，全部经宿主调用网关
（`typedApi`）注册与移除。

- plugId：`std.rest-server`
- 版本：1.0.0
- 初始化约束：`after: ['std.http-server']`（硬边）
- 零新增 npm 依赖；不修改 `package.json`

## 发布的方法

| ID | 方法 | 作用 |
| --- | --- | --- |
| 1 | `resource.mount` | 把一个资源声明翻译成约定路由并注册到 std.http-server |
| 2 | `resource.unmount` | 移除某个 pathPrefix 下已注册的全部路由 |
| 3 | `mounted.list` | 按挂载顺序列出当前挂载的资源清单 |

所有 args 与结果都是 JSON 值（`JsonValue`）；错误一律抛 `RackError`。

## resource.mount

```json
{
  "pathPrefix": "/api/users",
  "targetPlugId": "example.users",
  "methods": {
    "list": "users.list",
    "get": "users.get",
    "create": "users.create",
    "update": "users.update",
    "remove": "users.remove"
  },
  "versionRange": { "minimum": { "major": 1, "minor": 0, "patch": 0 }, "maximum": { "major": 1, "minor": 9, "patch": 9 } }
}
```

- `pathPrefix`：必须以 `/` 开头；尾部 `/` 会被归一化去掉（`/api/users/` 等价于
  `/api/users`）。重复判定按归一化后的前缀。
- `methods`：五个槽位（`list`/`get`/`create`/`update`/`remove`）全部可选，值是
  **目标插件的方法名**；至少提供一个；不允许未知槽位。未提供的槽位不挂路由。
- `versionRange`：可选。转发给 std.http-server，作为它**一次性租借目标插件**时使用的
  显式闭区间。
- 结果：`{ "mounted": true }`。

### 约定路由表

| HTTP 方法 | 路径 | 槽位（目标方法） |
| --- | --- | --- |
| GET | `{prefix}` | `list` |
| GET | `{prefix}/:id` | `get` |
| POST | `{prefix}` | `create` |
| PUT | `{prefix}/:id` | `update` |
| DELETE | `{prefix}/:id` | `remove` |

注册顺序固定为上表顺序；route.add 的顺序即 list → get → create → update → remove。
挂载是原子语义：任何一条 `route.add` 失败都会先回滚本次已添加的路由再抛出原始错误，
该前缀不进入挂载台账，可立即重试。

### 错误

| 状态 | 触发条件 |
| --- | --- |
| `invalid_argument` | args 结构非法：pathPrefix 不以 `/` 开头、targetPlugId 为空、methods 为空对象/含未知槽位/方法名不是非空字符串、versionRange 反向或结构非法 |
| `invalid_state` | 当前没有可用的 std.http-server 租约（未租借、已被撤销或已撤回） |
| `duplicate` | 归一化后的 pathPrefix 已挂载（含仍在注册途中的并发挂载） |
| `failed` | route.add 返回的结果里没有可用的 `routeId`（回滚后抛出） |
| 其他 | 由网关透传（例如 `stale`、`limit_exceeded`），透传前已完成回滚 |

## resource.unmount

```json
{ "pathPrefix": "/api/users" }
```

- 按记录的 routeId 集合逐条调用 `route.remove`，全部移除后从台账删除并返回
  `{ "unmounted": true }`。
- 未知前缀 → `not_found`。
- `route.remove` 返回 `not_found` 视为「对端本来就没有这条路由」，跳过并继续。
- 其他移除错误会原样抛出；台账保留未移除完的 routeId，重试即可续传。
- 若当前没有可用租约（例如 http-server 已被撤销/重载）：路由已随提供方代次一起消亡，
  此时只清除本地台账并返回成功。

## mounted.list

args 任意（建议 `null`）；返回 JSON 数组，按挂载顺序：

```json
[
  {
    "pathPrefix": "/api/users",
    "targetPlugId": "example.users",
    "methods": { "list": "users.list", "get": "users.get" },
    "versionRange": { "minimum": { "major": 1, "minor": 0, "patch": 0 }, "maximum": { "major": 1, "minor": 9, "patch": 9 } }
  }
]
```

`versionRange` 仅在挂载时提供才出现；`methods` 只包含提供的槽位。

## 目标插件的 args 契约（重要）

rest-server 从不直接调用目标插件。路由命中时由 **std.http-server** 一次性租借
`targetPlugId` 并按 `versionRange`（挂载时提供才有）调用对应目标方法，args 统一封装为：

```json
{
  "query":   { "page": "2" },
  "params":  { "id": "t1" },
  "headers": { "content-type": "application/json" },
  "body":    { "name": "first thing" }
}
```

- `query`：查询字符串键值对象（值为字符串）。
- `params`：路径参数对象；条目路由（`get`/`update`/`remove`）含 `id`，值为字符串。
- `headers`：请求头对象。
- `body`：**create/update 的 HTTP 请求体（解析后的 JSON 值）直接作为 `body` 字段**；
  无请求体时为 `null`。list/get/remove 一般也为 `null`。
- 目标方法的返回值作为 HTTP 响应内容；错误如何映射为状态码由 std.http-server 决定。
- 目标插件必须发布 methods 里声明的同名方法，并按上述封装解析 args
  （示例见 `ts/examples/plugins/rest-server-demo.plug.ts` 的 `things.*`）。

## 与 std.http-server 的协作协议

> 注意：std.http-server 的真实 `route.add` 契约为
> `{ path, httpMethod, targetPlugId, targetMethod, versionRange? }`（见
> `src/plugins/http-server/index.ts`），本插件转发时逐字段对齐。

| 方法 | args | 结果 |
| --- | --- | --- |
| `route.add` | `{ path, httpMethod, targetPlugId, targetMethod, versionRange? }` | `{ routeId }` |
| `route.remove` | `{ routeId }` | `{ removed: true }` |

租借与生命周期，风格与 `example.consumer` 一致：

- `init()` 内 `caps.watch()`；`std.http-server` 可用且版本落在显式闭区间
  `[1.0.0, 1.MAX_SAFE_INTEGER]` 时 `acquire()`（初始化阶段持租约合法，业务调用仍需
  Active）。
- `onRevoke()`：先清空缓存的租约再归还凭据；已挂载资源**保留台账**，路由随提供方
  代次一起消亡。
- 下一次可用事实到达时重新 `acquire()`（重借），并自动把台账中的资源**重新挂载**到
  新代次上；单个资源重挂失败只记诊断，不影响其他资源。
- 能力撤回（withdrawal）：禁止新业务（`resource.mount` → `invalid_state`），凭据保留
  等待 `onRevoke()`。
- `stop()`：先经网关移除全部已挂载路由，再归还租约并取消 watch 订阅。

## 相关文件

- 实现：`ts/src/plugins/rest-server/index.ts`
- 测试：`ts/tests/plugins/rest-server.test.ts`（全部使用 fake services）
- 示例：`ts/examples/plugins/rest-server-demo.plug.ts`（挂载一个指向自身方法的小资源）
