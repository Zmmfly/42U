# std.rest-client（RESTful 客户端）

`std.rest-client` 是 42U 机架 TS 版的内置标准插件（`std.*`），是「插件消费插件」的又一
示范：它自己不发起任何网络请求，而是**租借 `std.http-client`**，把 RESTful 约定翻译成
一次网关调用（`typedApi` 直调 `request`），由被租借的设备完成真正的 fetch、超时、重试与
大小限额。

- plugId：`std.rest-client`
- 版本：1.0.0
- 初始化约束：`after: ['std.http-client']`（硬边）
- 零新增 npm 依赖；不修改 `package.json`

## 租借策略

- `init()` 时 `caps.watch('std.http-client')`，先订阅再看快照。
- 可用事件先经 `acceptsVersion` 过滤：仅接受闭区间
  `[1.0.0, 1.MAX_SAFE_INTEGER.MAX_SAFE_INTEGER]`（即全部 1.x.y；0.x 与 2.x 不借）。
- 通过过滤且当前无租约时 `acquire`，保存 `LeaseHandle`；已有存活租约时不重复借。
- 撤回事件（`available: false`）只标记不可用，凭据保留给宿主的 `onRevoke()`。
- `Revoker.onRevoke` 先清空本地租约缓存再归还凭据；下次可用事件到达时按新代次重新
  租借。
- `stop()` 主动退订 watch 并归还租约。
- 无可用租约时调用任一方法 → `RackError('invalid_state', 'std.http-client 未上线')`。

## 发布的方法

| ID | 方法 | 作用 |
| --- | --- | --- |
| 1 | `resource` | 一次 REST 请求：拼 URL（path 逐段编码、query 走 URLSearchParams），经 std.http-client `request` 发出，返回响应 JSON 体 |
| 2 | `crud` | 按约定把 list/get/create/update/remove 映射到资源 URL，全部经 `resource` 实现 |

所有 args 与结果都是 JSON 值（`JsonValue`）；错误一律抛 `RackError`。

## resource

```json
{
  "baseUrl": "https://api.example.com/v1",
  "path": "/things/42",
  "method": "GET",
  "query": { "page": 2, "q": "hello world" },
  "body": { "name": "first" },
  "headers": { "x-trace": "t1" },
  "expectOk": true
}
```

- `baseUrl`：必填，绝对 URL；其 pathname 保留为目录前缀，尾部 `/` 会归一化去掉
  （`https://x/v1/` 与 `https://x/v1` 等价）。
- `path`：可选非空字符串；缺少前导 `/` 时自动补上。**逐段 percent-encode**（保留 `/`
  分隔），`a b` → `a%20b`。
- `method`：可选字符串，默认 `GET`；原样转发给 std.http-client（动词合法性由其校验）。
- `query`：可选对象，值为 `string | number | boolean`（其他类型 → `invalid_argument`）；
  经 `URLSearchParams` 序列化后追加。**提供 query 时替换 baseUrl 自带的查询串，未提供
  时保留**；空对象 `{}` 即「清除查询串」。
- `body`：可选任意 JSON 值；**直接作为 std.http-client `request` 的 `json` 字段**。
- `headers`：可选 `string → string` 对象，键须非空。
- `expectOk`：可选布尔，**默认 true**（非 2xx 由 std.http-client 抛
  `RackError('failed', ...)`；置 false 可拿回完整响应文档语义由对端决定）。
- 结果：目标 JSON——成功时返回响应的 JSON 体（对端结果文档中的 `json` 字段）；响应没有
  JSON 体时返回 `null`。

转发到 std.http-client `request` 的 args 恒为：

```json
{ "url": "<拼接结果>", "method": "<GET/POST/...>", "expectOk": true, "headers": {"...": "..."}, "json": "<body，仅当提供>" }
```

## crud

```json
{ "baseUrl": "https://api.example.com/v1", "resource": "things", "id": "t1", "item": { "name": "first" }, "query": { "page": "2" } }
```

约定映射（全部经 `resource` 实现，路径为 `` `${baseUrl路径}/${resource}[/${id}]` ``，
`resource` 与 `id` 均 percent-encode）：

| 操作 | 触发条件 | HTTP | URL |
| --- | --- | --- | --- |
| list | 无 `id` + 无 `item`（默认 GET） | GET | `/${resource}` |
| get | 有 `id` + 无 `item`（默认 GET） | GET | `/${resource}/${id}` |
| create | 无 `id` + 有 `item`（默认推断 POST） | POST | `/${resource}`（`item` 作 `json`） |
| update | 有 `id` + 有 `item`（默认推断 PUT） | PUT | `/${resource}/${id}`（`item` 作 `json`） |
| remove | 显式 `method: "DELETE"` + 有 `id` | DELETE | `/${resource}/${id}` |

- `id`：`非空字符串` 或 `有限数字`，编码进路径。
- `method`：可选显式覆盖，仅接受 `GET/POST/PUT/DELETE`（大小写不敏感）。缺省时由
  `id`/`item` 的存在性推断（见上表）。**删除必须显式 `method: "DELETE"`**：因为
  「有 id 无 item」已表示 get。
- 显式动词与字段组合必须满足上表（例如 POST 带 `id`、GET 带 `item`、DELETE 带 `item`
  均为 `invalid_argument`）。
- `query` 语义与 `resource` 一致；create/update 的 `item` 即请求体。
- `expectOk` 恒为 true（CRUD 约定下非 2xx 即失败）。
- 结果：同 `resource`，目标 JSON（无 JSON 体时为 `null`）。

## 错误

| 状态 | 触发条件 |
| --- | --- |
| `invalid_argument` | args 结构非法：baseUrl/resource 非法、path/method/query/headers/id/item 类型不符、显式动词与字段组合违反约定 |
| `invalid_state` | 当前没有可用的 std.http-client 租约（未租借、已撤回或已撤销） |
| `not_found` | 未知方法名/方法 ID |
| 其他 | 网关透传（例如非 2xx → `failed`、`stale`、`limit_exceeded`） |

## 与 std.rest-server 的对偶关系

`std.rest-server` 把「资源声明」翻译成服务端路由（挂路由的那一侧）；本插件把「资源
访问」翻译成客户端请求（发请求的那一侧）。两者约定同名五操作
list/get/create/update/remove，且都通过 `typedApi` 在租借凭据之下经宿主网关调用提供方，
从不持有提供者引用——这正是 invoke-only 治理在 TS 版的形态（design.md §2.1）。

## 示例

见 `ts/examples/plugins/rest-client-demo.plug.ts`：演示租借 `std.rest-client` 后依次执行
create → list → get → update → remove 五种约定调用（测试用 fake 服务环境驱动）。
