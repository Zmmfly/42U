# 仓库说明与协作规范

## 仓库说明

### 项目目标

- 使用 C++ 编写一个概念上类似 **42U 标准机柜**的插件框架。
- 以机柜作为设计类比：框架提供统一的插件容纳与集成方式，插件如同机柜内的设备，通过约定的接口接入并协作。
- “42U 标准机柜”描述的是模块化、标准化接入和可组合的设计理念，不意味着框架固定只能容纳 42 个插件；具体容量与接口设计由后续需求确定。

### 技术栈与构建工具

- **开发语言**：C++。
- **构建工具**：xmake，用于项目配置与编译构建。
- 后续新增或调整构建配置时，以 xmake 为统一入口；具体构建命令以仓库实际配置为准。

## 代码注释规范

### 注释风格与范围

- 使用 **Doxygen** 风格编写文档注释，参考 `main.c` 示例，采用 `/** ... */` 多行注释，每行以 ` *` 开头。
- 文档注释放在所描述的声明或定义之前，使用 `@brief`、`@param`、`@return` 等 `@` 形式的标签。
- 公共类、插件接口和公开函数应提供文档注释；内部实现按复杂度补充，重点解释设计意图、约束和非显而易见的行为，避免复述代码。
- 接口说明优先放在头文件声明处，避免在声明与实现中重复维护同一份文档；仅在源文件定义的函数可直接在定义前添加注释。
- 修改接口或行为时同步更新注释，确保注释与实现一致。

### 常用标签

- `@brief`：简要说明类、接口或函数的用途。
- `@param`：按声明顺序说明每个参数的含义；必要时使用 `@param[in]`、`@param[out]` 或 `@param[in,out]` 标明方向，并说明空值、范围和所有权约束。
- `@return`：说明返回值的实际含义，不只填写返回类型；返回类型为 `void` 的函数以及构造、析构函数不添加此标签。
- `@retval`：需要逐项说明返回状态或错误码时使用。
- `@tparam`：说明模板参数的用途与约束。
- `@note`、`@warning`：按需说明生命周期、线程安全、调用顺序或使用风险。
- `@throws`：函数可能抛出异常时，说明异常类型及触发条件。

### 格式示例

- `@brief` 与参数说明之间留一行空注释行，保持结构清晰。
- 插件接口应按需补充资源所有权、生命周期和并发调用约束，不编造实现尚未保证的行为。

```cpp
/**
 * @brief Main entry.
 *
 * @param argc Number of command-line arguments.
 * @param argv Array of command-line argument strings.
 * @return 0 on successful completion.
 */
int main(int argc, char** argv)
{
    return 0;
}
```

## 1. Git 提交基本原则

- 每次提交只解决一个明确问题，保持改动原子化，便于审查、回滚和追踪。
- 不将功能开发、缺陷修复和无关格式调整混在同一次提交中。
- 提交前检查差异，只暂存与本次任务相关的文件或代码片段，保留他人未提交的改动。
- 仅在用户明确要求或授权时执行提交、推送等操作；修改文件不代表自动获得提交授权。

## 2. 提交消息格式

首行采用 Conventional Commits 风格，正文采用 Markdown 小节和列表：

```markdown
<type>[可选 scope][可选 !]: <英文简短描述>

## English

### Context
- <用英文说明原有行为、问题或需求，以及本次修改的动机和目标>

### Changes
- <用英文说明修改内容及原因>

### Validation
- <用英文说明实际验证结果或未验证原因>

## 中文

### 上下文
- <用中文说明原有行为、问题或需求，以及本次修改的动机和目标>

### 变更
- <用中文说明修改内容及原因>

### 验证
- <用中文说明实际验证结果或未验证原因>

[可选页脚]
```

- `type` 使用小写英文，类型参见下一节。
- `scope` 用圆括号包裹，表示受影响的模块，例如 `auth`、`api`、`build`；无明确模块时可省略。
- 冒号后保留一个空格。
- 提交消息首行固定使用英文，准确说明做了什么，避免 `update files`、`change code` 等含糊表述。
- 标题尽量不超过 72 个字符，末尾不加句号。
- 正文必须使用英中双语：先写完整英文正文，再写完整中文正文，两部分之间留一个空行。
- 正文必须采用 Markdown 格式，以 `## English` 和 `## 中文` 分隔两个语言区块。
- 每个语言区块必须依次包含上下文、变更和验证小节：英文使用 `### Context`、`### Changes`、`### Validation`，中文使用 `### 上下文`、`### 变更`、`### 验证`；按需添加影响范围、迁移说明等小节。
- 上下文小节必须说明修改前的行为或现状、遇到的问题或需求，以及本次修改的动机和目标，让读者仅凭提交消息即可理解为什么需要此次改动，不依赖聊天记录或外部任务链接。
- 上下文涉及缺陷时，应按需说明触发条件及影响；涉及方案选择时，应说明关键约束或取舍。只记录已知事实，不编造背景，不仅写“按要求修改”等空泛描述。
- 小节内容使用 `- ` 无序列表逐条编写，每条聚焦一个要点，不使用连续散文代替小节和条目。
- 英中两部分的小节和条目应对应、信息一致；先完成所有英文小节，再编写所有中文小节，不按语言交替排列。
- 正文与标题之间留一个空行，说明修改原因、实现要点及必要的影响范围，不重复罗列差异。
- 页脚置于双语正文之后，可关联任务或问题，例如 `Refs: #123`；只有确实解决问题时才使用 `Closes: #123`。

## 3. 提交类型

| 类型 | 使用场景 |
| --- | --- |
| `feat` | 新增功能 |
| `fix` | 修复缺陷 |
| `docs` | 仅修改文档 |
| `style` | 不影响代码含义的格式调整，如缩进、空格 |
| `refactor` | 不新增功能、不修复缺陷的代码重构 |
| `perf` | 性能优化 |
| `test` | 新增或修改测试 |
| `build` | 构建系统、依赖或打包配置变更 |
| `ci` | 持续集成配置或脚本变更 |
| `chore` | 不属于上述类型的维护工作 |
| `revert` | 撤销已有提交 |

## 4. 不兼容变更

- 引入不兼容变更时，在类型或作用域后添加 `!`。
- 同时在页脚使用 `BREAKING CHANGE:` 说明不兼容点及迁移方式。
- 涉及接口、配置、数据格式或默认行为变更时，应同步更新相关文档。

```markdown
feat(api)!: standardize pagination parameter naming

## English

### Context
- Pagination uses pageSize while other API parameters use snake_case, creating inconsistent naming for clients.
- Standardize the pagination parameter to follow the same naming convention.

### Changes
- Rename pageSize to page_size to keep API naming consistent.

### Migration
- Update clients to use page_size; pageSize is no longer accepted.

### Validation
- Not run: the API test environment is unavailable.

## 中文

### 上下文
- 分页使用 pageSize，而其他 API 参数使用 snake_case，导致调用方遇到不一致的命名。
- 本次修改旨在让分页参数遵循相同的命名规范。

### 变更
- 将分页参数 pageSize 改为 page_size，统一接口命名风格。

### 迁移
- 调用方需改用 page_size；不再接受 pageSize。

### 验证
- 未运行：API 测试环境不可用。

BREAKING CHANGE: pageSize is no longer accepted; use page_size instead.
```

## 5. 提交前检查

- 使用 `git status` 确认工作区状态。
- 使用 `git diff` 和 `git diff --cached` 检查未暂存及已暂存差异，确认提交范围正确。
- 使用 `git diff --cached --check` 检查暂存内容中的空白错误。
- 根据改动范围执行项目已有的格式检查、静态检查、测试或构建；纯文档修改应至少检查内容及 Markdown 格式。
- 无法运行验证时，如实说明原因和未验证项，不声称验证通过。
- 确认未提交密码、令牌、私钥、真实凭据、敏感日志或包含秘密的环境配置文件。
- 不提交无关临时文件、缓存、构建产物和编辑器个人配置，除非项目明确要求纳入版本管理。
- 不使用 `--no-verify` 绕过提交钩子，除非得到明确授权并说明原因。

## 6. 历史与协作

- 不擅自执行 `git commit --amend`、变基或其他重写已有提交历史的操作。
- 不擅自强制推送，也不使用 `git reset --hard` 等方式丢弃未提交改动。
- 需要撤销已共享的提交时，优先通过 `git revert` 创建反向提交，并说明撤销原因。
- 发生冲突时先理解双方修改意图，不直接覆盖他人改动。
- 完成提交后，报告提交哈希、提交摘要及实际验证情况；未经授权不自动推送到远端。

## 7. 常见示例

### 7.1 英文首行示例

以下仅展示首行；实际提交还需补充双语正文。

```text
feat(auth): add token refresh endpoint
fix(config): handle empty configuration files on startup
docs: add Git commit guidelines
refactor(storage): extract shared serialization logic
test(api): cover pagination edge cases
build(deps): upgrade logging library
ci: add tests for merge requests
```

### 7.2 完整双语提交示例

```markdown
docs: add Git commit guidelines

## English

### Context
- The repository has no shared commit guidelines, leaving message structure and validation reporting undefined.
- Establish a consistent format so reviewers can understand the rationale, changes, and validation from each commit message.

### Changes
- Document commit types, message structure, and pre-commit checks.
- Require English subject lines and bilingual bodies with Markdown headings and lists.

### Validation
- Not run: documentation-only change; no automated Markdown check is configured.

## 中文

### 上下文
- 仓库缺少统一的提交规范，尚未明确消息结构和验证情况的记录方式。
- 建立一致的格式，让审查者仅凭提交消息即可了解修改动机、具体变更及验证情况。

### 变更
- 记录提交类型、消息结构及提交前检查要求。
- 要求首行使用英文，双语正文采用 Markdown 小节和列表。

### 验证
- 未运行：仅修改文档，未配置自动化 Markdown 检查。
```

示例中的验证内容仅展示格式，实际提交必须填写真实的验证结果或未验证原因。
