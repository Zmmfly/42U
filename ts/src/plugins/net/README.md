# std.net —— 标准网络工具插件

`std.net` 是随框架发货的标准设备（`ts/src/plugins/net/`，见 `ts/docs/design.md` §3.1），
对外提供 DNS 解析、TCP 连通性探测与 URL 归一化三类能力。**零新增 npm 依赖**：仅使用
Node 内置的 `node:dns/promises`、`node:net` 与全局 `URL`。

- plugId：`std.net`
- 业务版本：`1.0.0`
- 初始化约束：无（`init: {}`），可被任何宿主编排

## 方法契约

| id | 方法 | 输入 | 输出 |
| --- | --- | --- | --- |
| 1 | `dns.resolve` | `{host}` | `{addresses: string[]}` |
| 2 | `tcp.probe` | `{host, port, timeoutMs?=2000}` | `{ok: boolean, latencyMs?: number}` |
| 3 | `url.normalize` | `{url, base?}` | `{url, host, pathname, searchParams}` |

### dns.resolve

- 并发执行 `resolve4` + `resolve6`，结果合并去重（v4 在前，保持首次出现顺序）。
- `host` 必须是结构合法的主机名（RFC 1123 标签：字母/数字开头结尾、连字符居中、
  总长 ≤ 253，允许一个 FQDN 尾点）；IPv6 字面量按非法参数处理（`resolve4/6` 只接受
  主机名）。非法 → `invalid_argument`。
- **任一家族查询成功即成功**：只有 IPv4 记录（AAAA 查询返回 ENODATA 等）的主机照常
  返回 v4 地址；两族查询全部失败 → `RackError('failed')`，`detail.code` 携带第一个
  （A 记录）失败的解析器错误码，如 `ENOTFOUND`。

### tcp.probe

- 真实实现用 `net.createConnection({host, port})` 建连；定时器一到立即 `destroy()`
  并结算 `{ok:false}`。连接成功、被拒、超时、stop 中止四条路径都会立即销毁连接。
- 成功返回 `{ok:true, latencyMs}`；其余情况只返回 `{ok:false}`（不带 `latencyMs`），
  永不 reject。
- 参数校验（均 → `invalid_argument`）：`port` 必须是 `[1, 65535]` 内的安全整数；
  `timeoutMs` 可选，给出时必须是正整数（缺省 2000）；`host` 接受主机名与 IPv6 字面量
  （如 `::1`、`fe80::1%eth0`），畸形字面量会在连接阶段以 `ok:false` 结算。

### url.normalize

- 用 WHATWG `URL` 解析（可带 `base`），返回归一化后的 `{url, host, pathname,
  searchParams}`；`searchParams` 是普通对象，重复查询键取最后一个值。
- `url` 缺失/非字符串/解析失败，或 `base` 非字符串 → `invalid_argument`。

## 错误映射

| 场景 | 状态 |
| --- | --- |
| 参数缺失/类型/取值非法、URL 解析失败 | `invalid_argument` |
| DNS 两族查询全部失败 | `failed`（`detail.code` = 解析器错误码） |
| 未知方法名/方法 id | `not_found` |
| start() 之前或 stop() 之后调用 | `invalid_state` |

## 生命周期与并发纪律

- `start()` 一次性发布上表三个方法（能力集合实例生命周期内冻结）。
- `stop()` 通过实例级 `AbortController` 立即中止全部在途探测：每条在途探测以
  `{ok:false}` 结算、其连接立即销毁、其超时定时器清除；`stop()` 等待全部探测结算
  完毕后才返回。并发探测仅做内部计数，用于让 `stop()` 等待排空。
- `stop()` 之后不会再有任何探测回调：定时器已清除、连接监听已摘除（真实 socket
  destroy 前先移除全部监听并挂一个惰性 error 吸收器，避免未捕获异常），晚到的
  回调也被已结算标志挡下。
- 探测 promise 永不 reject：失败即 `{ok:false}`，符合"探测是测量，不是异常"的语义。

## 依赖注入与测试

- `create()`：真实实现（`node:dns/promises` + `node:net` + 全局 `URL`）。
- `createWithDeps(deps)`：`deps.dns`（`resolve4/resolve6`）与 `deps.tcp`
  （`connect(host, port) → {onConnected, onFailed, destroy}`）均可注入假实现，
  供 `ts/tests/plugins/net.test.ts` 在完全不触网的情况下验证行为。
- 两者均从本模块导出（`create` 同时是默认导出，满足加载器约定）。

## 验收

```bash
cd ts && npx vitest run tests/plugins/net.test.ts
npx tsc --noEmit --strict --noUncheckedIndexedAccess --verbatimModuleSyntax \
  --target ES2023 --module NodeNext --moduleResolution NodeNext --types node \
  --allowImportingTsExtensions --skipLibCheck \
  src/plugins/net/index.ts tests/plugins/net.test.ts examples/plugins/net-demo.plug.ts
```

（`--allowImportingTsExtensions`/`--skipLibCheck` 是本仓库 `.ts` 相对导入约定所需的
补充开关，与根 `tsconfig.json` 一致。）
