# std.oauth 插件

42U 机架 TS 版内置标准插件：常用 OAuth 2.0 流程与令牌管理，供其他插件租借使用。

- **plugId**：`std.oauth`
- **业务版本**：`1.0.0`
- **依赖**：零新增 npm 依赖（`node:crypto` + 可注入的全局 `fetch`）
- **文件**：`ts/src/plugins/oauth/index.ts`（本目录）

## 能力（start() 发布，stop() 清空令牌缓存）

| id | 方法 | 用途 |
| --- | --- | --- |
| 1 | `authorize-url` | 构造授权码跳转 URL（response_type=code，可选 PKCE S256） |
| 2 | `token` | client_credentials / authorization_code 换取访问令牌（含缓存与自动刷新） |
| 3 | `token.invalidate` | 丢弃一个缓存令牌条目（未知键幂等成功） |
| 4 | `introspect` | 令牌内省，原样返回端点 JSON |

方法同时支持数字 id 与名称选择，两者经网关解析到同一实现。

## authorize-url

入参（`authorizationEndpoint`/`clientId`/`redirectUri` 必填）：

```
{ authorizationEndpoint, clientId, redirectUri, scope?, state?, codeVerifier?, pkce?=true }
```

出参：`{ url, state, codeVerifier?, codeChallenge? }`

- 查询参数按固定顺序拼接：`response_type, client_id, redirect_uri, scope, state[, code_challenge, code_challenge_method]`；已存在于端点 URL 的查询串会被保留，值用 `encodeURIComponent` 编码（空格为 `%20`）。
- `state`/`codeVerifier` 未提供时用 `node:crypto` 随机生成（state 22 字符、verifier 43 字符 base64url）。
- PKCE（RFC 7636，S256）：`codeChallenge = BASE64URL(SHA256(ASCII(codeVerifier)))`；`codeVerifier` 只在本次应答中返回一次，不进缓存、不写日志。
- `pkce:false` 时结果不含 `codeVerifier`/`codeChallenge`，此时传入 `codeVerifier` 视为非法组合。

## token

入参（`grantType`/`tokenEndpoint`/`clientId` 必填）：

```
{ grantType: 'client_credentials' | 'authorization_code', tokenEndpoint, clientId,
  clientSecret?, code?, redirectUri?, scope?, pkce?{codeVerifier}, refreshEndpoint?, cacheKey? }
```

出参：`{ accessToken, tokenType, expiresIn, scope?, cached? }`

- `accessToken` 完整返回（调用方即令牌所有者）；`tokenType` 缺省为 `Bearer`；`scope` 优先取响应值，缺失时回退请求值。
- POST 标准 OAuth2 表单（`application/x-www-form-urlencoded`）；`authorization_code` 自动附带 `code_verifier`。
- `client_credentials` 携带 `code`/`redirectUri`/`pkce`，或 `authorization_code` 缺 `code`，或 grantType 非法 → `invalid_argument`。
- 响应非 2xx → `RackError('failed', { phase, httpStatus, oauthError? })`；`oauthError` 取响应体 `error` 字段。响应体其余内容不进入错误。

### 令牌缓存

- 键：`cacheKey`（显式）或 `JSON.stringify([tokenEndpoint, clientId, scope ?? ''])`（默认推导）。
- 条目记录**绝对过期时刻** `expiresAt = now() + expires_in * 1000`（`now()` 为注入时钟）。
- 有效期内直接返回并带 `cached: true`；距到期不足 30 秒即视为过期。
- 条目持有 `refresh_token` 且已过期时，自动向 `refreshEndpoint ?? 条目端点` 发起 `refresh_token` grant（携带 `client_id` 与本次调用的 `clientSecret`）；刷新成功更新缓存，`cached` 不置位。
- 刷新失败（网络错误或非 2xx）→ 清除该缓存条目并抛 `failed`。
- `expires_in` 缺失或非法按 0 处理，不写入缓存。

## token.invalidate / introspect

- `token.invalidate { cacheKey }`：删除缓存条目，返回 `null`；未知键同样成功（幂等）。
- `introspect { introspectionEndpoint, token, clientId, clientSecret? }`：POST 表单（`token`/`client_id`/`client_secret?`），原样返回端点 JSON；非 2xx → `failed`（同上错误形状）。

## 安全红线

- `clientSecret` / `accessToken` / `refreshToken` 绝不出现在诊断、错误 `detail`、错误消息或日志参数中；任何需要展示的位置一律掩码 `***`。
- 错误 `detail` 只含 `{ phase, httpStatus, oauthError?, reason? }`；诊断记录与错误同源，同样不含表单或响应体原文。
- `codeVerifier` 仅在 `authorize-url` 应答中一次性返回。

## 依赖注入与测试

```ts
import { createWithDeps, defaultFetch, defaultNow } from '../src/plugins/oauth/index.ts';

const plugin = createWithDeps({
  fetchImpl: myFakeFetch, // 默认 defaultFetch = 全局 fetch（已导出）
  now: () => fakeNowMs,   // 默认 defaultNow = Date.now()（已导出）
});
```

- 验收：`cd ts && npx vitest run tests/plugins/oauth.test.ts`，scoped `tsc`（strict + noUncheckedIndexedAccess + verbatimModuleSyntax）零错误。
- 演示（fake fetch + fake clock，无网络）：`cd ts && node examples/plugins/oauth-demo.plug.ts`；亦可作为插件 `example.oauth-demo` 上机架后调用其 `run` 方法。
