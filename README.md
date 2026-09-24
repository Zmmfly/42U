# 42U 插件框架

42U 是一个以 **42U 标准机柜** 为设计类比的 C++ 插件框架：宿主（host）负责扫描、加载并统一编排插件，插件像机柜内的设备一样通过约定的 ABI 接入，再借助宿主提供的事件、能力与调用服务协作。插件之间不共享业务类或接口指针，业务调用统一经宿主网关（invoke-only）。消费者用显式的**插件版本闭区间**借用租约，宿主返回**凭据 + 实际版本**；凭据只用于按方法名或数字 ID 直接调用，用完主动归还，或在收到撤销通知后归还。“42U”描述的是模块化、标准化接入与可组合的设计理念，不是可容纳插件数量的硬上限。

- 构建工具：xmake
- 语言标准：C++（当前构建配置为 C++17）
- 项目版本：`0.3.0`（ABI v3）
- 设计文档：[docs/42U插件框架设计.md](docs/42U插件框架设计.md)
- 纯宿主 CLI：[docs/42U纯宿主CLI设计.md](docs/42U纯宿主CLI设计.md)
- 迁移指南：[docs/42U-v3迁移指南.md](docs/42U-v3迁移指南.md)

## 仓库布局

| 路径 | 内容 |
| --- | --- |
| `inc/42u/abi.hpp` | ABI v3 声明：状态码、框架服务接口 ID（`iid`）、插件版本（`plugin_version`/`version_range`）、跨界结构、宿主服务与生命周期纯虚接口、入口与布局断言；v1、v2 主版本被明确拒绝 |
| `inc/42u/cli.hpp` | 独立版本化的 CLI manifest v1 与只读 `iconfig` 扩展 ABI；不包含 CLI11 类型 |
| `inc/42u/host.hpp` | 宿主公共门面 `u42::host`：基础生命周期、调用网关，以及 discovery 批次的配置冻结与 `adopt()` |
| `inc/42u/plug.hpp` | 动态库映射、确定性候选扫描，以及不创建实例的 `discovered_plugin` |
| `inc/42u/order.hpp` | 初始化排序规划 `u42::plan_order`（优先级、`before`/`after`、环检测） |
| `inc/42u/sdk.hpp` | 插件侧源级 RAII 辅助（非模板凭据租约：凭据 + 实际版本、字节视图、受限输出写入）；不进入 ABI，不保存任何提供者指针 |
| `src/` | 宿主实现：生命周期引擎、上下文与服务实现、事件、库加载、排序 |
| `host/include/42u/cli_host.hpp` | `42uhost` 私有前端的 owned catalog、解析结果和应用接口 |
| `host/src/` | 纯宿主 CLI：manifest 校验、受限 CLI11 动态解析、配置与 handler 编排 |
| `examples/` | 示例插件（`echo`、`consumer`），各自独立构建为 `.u42.so` |
| `tests/` | 不依赖外部测试框架的检查 |
| `docs/` | 设计文档与设计讨论稿 |

## 构建与测试

```bash
# Linux / GCC 构建与测试示例；ABI v3 的权威验证结果见文末“验证状态”
xmake f -m debug --toolchain=gcc --sanitizer=none -o build -y
xmake -j 8 && xmake test -v
```

核心静态库、插件 ABI 和测试夹具不依赖第三方运行库或测试框架。`42uhost_cli`
单独使用锁定的 header-only CLI11 `2.7.2`。可选运行时检查：

```bash
xmake f -m debug --sanitizer=address -o build/asan -y
xmake -j 8 && xmake test -v

# 恢复默认构建目录与关闭检测器
xmake f -m debug --sanitizer=none -o build -y
```

产物：

- `42uhost`：宿主命令行工具（目标名 `42uhost`）。
- 示例插件动态库：`build/<plat>/<arch>/<mode>/plugins/echo.u42.so` 与 `build/<plat>/<arch>/<mode>/plugins/consumer.u42.so`；debug 模式、Linux x86_64 下即 `build/linux/x86_64/debug/plugins/`。

> 插件目录带平台、架构、模式三层路径，请按本机实际构建目录调整。`42uhost` 只扫描指定目录（不递归）中形如 `*.u42.so`（macOS 为 `*.u42.dylib`，Windows 为 `*.u42.dll`）的候选文件。

## 运行 CLI

```bash
# 插件目录优先来自 U42_PLUGIN_DIR；否则使用真实可执行文件旁的 plugins 目录。
U42_PLUGIN_DIR=/opt/42u/plugins ./build/linux/x86_64/debug/42uhost \
    --log-level debug serve --port 8080 node-a
```

用法：

```text
42uhost --version
42uhost --help
42uhost [plugin-declared-root-options] <command> [command-options] [positionals]
```

| 项目 | 说明 |
| --- | --- |
| `--version` | 宿主保留项；不扫描、不映射插件，打印项目版本 |
| `--help` | 宿主保留项；读取插件 manifest 生成完整帮助，但不 `create/init/start` |
| 根参数 | 由插件声明，必须出现在子命令之前，只进入所属插件的配置快照 |
| 子命令与局部参数 | 由插件声明；局部参数必须出现在所属子命令之后 |
| `command -- <positionals>` | `--` 仅把后续 token 作为当前命令剩余的位置参数，不返回父级，也不再解析选项 |
| 插件目录 | `U42_PLUGIN_DIR` 指定一个目录；未设置时使用真实可执行文件同级的 `plugins` |

约定：

- **先发现、后解析、再创建**：完整帮助需要映射可信插件并读取 manifest；manifest 冲突和 argv 错误都在任何实例创建前失败。
- **插件拥有业务命令面**：宿主只保留 `--help`/`--version`。根参数、一级子命令、局部参数和 handler `method_id` 均由插件声明。
- **配置隔离**：根参数和显式 `config_key` 映射只进入所属插件的只读快照；`iconfig` 没有跨插件查询入口。
- **确定性 payload**：局部参数默认按稳定 `parameter_id` 序列化为 JSON；TEXT 为字符串、repeatable 为数组、FLAG 为 boolean。插件返回结果按原始字节写到 stdout，不补换行、不重排。
- **受限语法**：不支持 short option、前缀匹配、点号子命令寻址、`++` 返回父级、额外位置参数或多命令执行。
- **退出码**：`0` 成功，`1` 发现/声明/启动/执行/清理或输出失败，`2` 用户输入错误。业务成功但 shutdown 失败仍返回 `1`。
- **同步短命令**：一次进程最多执行一个有限时长 handler；常驻任务、流式输出和取消协议尚未实现。

## ABI 与运行模型

### 固定 ABI profile

ABI v3 采用 **C 链接入口 + 受约束的 C++ 纯虚接口**，并把插件间协作收敛为 **invoke-only**：

- 动态库的基础 ABI 必需入口是 `u42_get_factory`：先协商基础 ABI 主版本（当前为 `3`），再返回库内工厂；需要贡献 CLI 命令面的插件可额外导出独立协商的 `u42_get_cli_manifest`。只支持主版本 `1` 或 `2` 的旧库对宿主的版本 `3` 请求返回 `unsupported`，并被明确拒绝加载；v3 不提供 v1/v2 的源兼容别名或同 IID 变签名，旧插件必须先迁移源码再重新编译。
- 跨界类型限定为固定宽度整数、已定义布局的简单结构（含插件版本 `plugin_version{ major, minor, patch }` 与版本范围 `version_range{ minimum, maximum }`）、不透明标识（`token`）、指针与长度视图（`bytes`）、宿主服务与生命周期纯虚接口指针。STL 容器、异常、RTTI 对象、线程对象不跨边界；边界函数为 `noexcept`，异常在产生侧转换为状态码。
- 对象由分配方销毁：宿主调用插件的 `destroy()`，消费者不销毁任何提供者对象（它只持有宿主签发的凭据和按值复制的实际版本）；跨库 `new`/`delete` 不成立。
- 宿主服务接口只有纯虚函数、无数据成员，指针只能通过 `ictx::query` 取得，不能由根对象地址推算。插件之间**不共享业务类、接口指针或提供者 `iinvoke` 指针**：唯一业务路径是 `消费者 → 宿主 icalls → 提供者 iinvoke`，且每次调用都要出示借用凭据；`iplug::query` 是宿主私有管理入口，宿主只用它取得 `invoke_iid`。
- 当前验证 profile 为 **Linux x86_64、64 位原生对齐、Itanium C++ ABI、C++17、GCC 15.2 / Clang 21.1 与本机 libstdc++/glibc**。两种编译器构建的宿主和示例插件已做双向混合加载测试；这不等于所有 GCC/Clang 版本均兼容，也不保证可部署到更旧的运行库。
- Windows、macOS、32 位和其他架构尚未验证。入口的主版本协商不能自动识别所有 ABI profile 差异，部署方仍须保证工具链、架构与运行库匹配。
- **ABI 协商依赖插件遵守契约**：入口必须拒绝自己不支持的 major。宿主不提供针对谎报版本、返回错误类型工厂或任意内存破坏的安全验证；“拒绝 v1/v2”指正确实现旧入口协商的插件。

### 接口名单

`inc/42u/abi.hpp` 中的官方框架服务接口（`iid.high` 均为 `0x3432555f41424933`，对应 ABI v3）；`iid` 只命名框架服务，不再标识业务协议 family：

| 常量 | `iid.low` | 服务 | 用途 |
| --- | --- | --- | --- |
| `events_iid` | 1 | `ievents` | 精确名称的事件订阅、取消与投递 |
| `caps_iid` | 2 | `icaps` | 方法披露、能力快照监听、按显式版本范围借用与归还 |
| `calls_iid` | 3 | `icalls` | 按租约凭据以名称或数字 ID 直接同步调用 |
| `diag_iid` | 4 | `idiag` | 宿主诊断输出 |
| `invoke_iid` | 5 | `iinvoke` | 插件提供的动态方法入口，仅宿主经 `iplug::query` 取得 |

插件通过注入的 `ictx::query(&iid, &out)` 取得前四项宿主服务；`invoke_iid` 对应插件提供的入口，由宿主通过 `iplug::query` 获取，不能向 `ictx` 查询，也不向其他插件暴露。根实例接口为 `iplug`（`init`/`start`/`stop`/`destroy` 与 `iplug::query`），工厂为 `iplug_fty`（`describe`/`create`），库内元数据为 `plug_desc`（含 `plugin_version version`），方法描述为 `method_desc`。

CLI manifest v1 另行定义 `config_iid` 和 `iconfig`，不修改上表 ABI v3 的既有 IID 或虚表。插件可选导出 `u42_get_cli_manifest`；未导出的旧 v3 插件继续加载，只是不贡献命令面。

版本与范围约定：

- 公开版本只有两类：插件的类型化业务版本 `plugin_version`（`major.minor.patch`）与框架 ABI 版本 `abi_major`；二者互不推断。实例代次（generation）是宿主私有信息，只封装在不透明凭据中。
- `version_range` 是**闭区间** `[minimum, maximum]`：两端都包含，两端相等表示精确版本，`0.0.0` 合法，`minimum > maximum` 是 `invalid_argument`。宿主只用调用方给出的显式范围筛选，不做字符串范围解析，不支持预发布或构建标签。
- `major` 是否可跨版本兼容由提供者承诺；跨 `major` 的范围只有在调用方**明确**写成这样的区间时才可能被接受，宿主不会自动推断或自动拒绝。
- 一个插件实例披露一份 `plugin_version` 和一份方法集合；方法集合可以为空，表示该实例只能被借用生命周期而没有方法可调用，调用任何未披露方法返回 `not_found`。
- 借用结果 `borrow` 里的版本是宿主**按值复制**的实际版本，不是允许范围的端点，也不是一段可能随库卸载而悬垂的字符串；它与凭据在同一次 `acquire` 中一起返回。

### 线程与信任模型（第一阶段限制）

- **同步单线程、可信插件**：宿主所有接口只在控制线程调用，`host` 与 `iplug` 都不会代为跨线程投递；插件内部可以使用工作线程做私有计算，但不得从工作线程回调宿主或其他插件接口，也不得把租约凭据交给工作线程。框架不提供沙箱，也无法强制终止进程内插件。
- **unload 需租约归还确认**：卸载前先撤下能力并逐项发送 `on_revoke`，消费者必须在撤销回调中归还凭据；只要存在未归还的租约、在途调用或 `stop()` 失败，卸载就会被拒绝。同一租约在途调用（含输出交付）期间 `release` 返回 `busy` 并保留租约，不伪造归还。无法证明静默的实例会被**隔离**而不是被强行拆解（表现为进程直到退出仍持有该实例及其库映射）。停止失败的消费者持有的出向租约也必须保留，因此它依赖的提供者可能一并被保留；不能用删除租约记录来伪造安全卸载。租约按实例代次校验：重新加载后旧凭据调用报 `stale`，旧归还凭据不会命中新记录；**即使版本完全相同，重载后也必须重新借用并重建业务 session**。
- **同进程治理，不承诺隔离**：框架运行在单一进程内，只治理调用与借用关系；不提供内存隔离、崩溃隔离、强制超时或强杀，也不支持不可信插件。内存不足时只做尽力清理，可能留下 Created 或隔离记录供重试、关停，不夸大清理结果。
- **生命周期不重入**：插件调用栈中的 `unload()` 仅对已知 ID 返回 `deferred` 并排队，未知 ID 返回 `not_found`；`shutdown()`、`add()`、`boot()`、`load()` 在这种情况下返回 `busy`，`start()` 返回 `invalid_state`，必须由应用在安全点重试。
- **启动批次边界**：全部 `init()` 完成后按计划逐一 `start()`，每个成功启动后的能力通知照常派发；Initialized 消费者可以保存租约，不能调用业务。整个启动批次及其失败回滚不消费延迟卸载队列，因此成功返回时本批插件仍全部 Active。启动返回后由 `poll()`、显式 `unload()` 或其他批次外的安全清理处理排队请求；`deferred` 不代表已完成卸载。失败回滚销毁的实例会连同其请求一起移除，原有实例的请求保留排队。分配失败时回滚仅为尽力清理，可能留下 Created 或隔离记录供重试、关停。`poll()` 中正常状态失败的卸载在存储允许时重新入队供后续重试；内部分配异常仅尽力保留队列，收到 `failed` 后应显式重试 `unload(plug_id)`。预算耗尽时未处理工作也留到下次派发。`boot()` / `load()` 的内部启动遵循同一语义。
- **宿主寿命与异常**：不得在任何宿主操作或插件回调执行期间销毁 `host`；`shutdown()` 的重入保护不使回调中 `delete host` 合法。返回 `status` 的 native 操作把内部异常转换为状态码，诊断可能因分配失败而保持旧值；构造函数、`plugins()` 快照及调用方准备实参的分配仍可能抛异常。
- **插件自行独立构建**：每个示例插件都是独立编译的 C++ 动态库（匹配的 ABI profile），不链接宿主静态库；宿主在运行时 `dlopen` 加载。对象实现留在各自一侧，跨边界只传 ABI 已定义的类型与受寿命约束的宿主服务/生命周期虚接口指针；插件间业务不传任何接口指针。

## 文档

当前（ABI v3）：

- [docs/42U插件框架设计.md](docs/42U插件框架设计.md)：设计主文档（目标与范围、启动流程、初始化编排、按版本借用与凭据直接调用、动态卸载、ABI v3 交付约束、验收标准）。
- [docs/42U纯宿主CLI设计.md](docs/42U纯宿主CLI设计.md)：插件声明根参数、一级子命令、配置快照、发现移交与受限 CLI11 语法。
- [docs/42U纯宿主CLI验证记录-2026-09-24.md](docs/42U纯宿主CLI验证记录-2026-09-24.md)：纯宿主 CLI 的构建、测试、sanitizer 和审核修复记录。
- [docs/42U-v3迁移指南.md](docs/42U-v3迁移指南.md)：从 ABI v2 迁移到 v3 的旧/新对照、三步借用与调用示例及检查清单。
- [docs/42U-v3验证记录-2026-09-19.md](docs/42U-v3验证记录-2026-09-19.md)：本轮完整验证、独立审查、源码指纹和限制。

历史（不对应当前 ABI，仅作迁移与审计参考）：

- [docs/42U-v2迁移指南.md](docs/42U-v2迁移指南.md)：从 ABI v1 迁移到 v2 的旧/新对照。
- [docs/42U-v2验证记录-2026-09-17.md](docs/42U-v2验证记录-2026-09-17.md)：ABI v2 验证、独立审查、限制和源码标识。
- [`docs/42U首版验证记录-2026-09-17.md`](docs/42U首版验证记录-2026-09-17.md)：ABI v1 历史验证证据。
- `docs/thinks/`：本地探索区，讨论稿被 Git 忽略，不作为发布规范。

## 验证状态

各代 ABI 的验证分别记录，不把历史通过结果沿用到当前 ABI。

### ABI v1 历史验证（2026-09-17）

- GCC Debug、GCC ASan（开启正常退出泄漏检测）、GCC UBSan、Clang Release 均为 10/10 测试目标通过。
- GCC 宿主加载 Clang 插件及反向混合加载的完整集成测试通过；CLI 列举、名称调用、数字调用和错误出口已验证。
- 重入回归 294 项检查；`boot_test` 覆盖旧身份快照的 11 个分配失败点与双故障回滚，`withdraw_test` 覆盖撤回通知的 14 个分配失败点。
- 停止失败场景按设计保留资源，测试在独立子进程验证不销毁；这与正常退出的泄漏检查分开处理。
- 验证命令、源码指纹、验收项对应和边界详见[42U 首版验证记录（2026-09-17）](docs/42U首版验证记录-2026-09-17.md)。

### ABI v2 历史验证（2026-09-17）

该轮 GCC Debug、GCC ASan（正常退出泄漏检测开启）、GCC UBSan、Clang Release 均为 **11/11 测试目标通过**，GCC/Clang 双向混合加载及 CLI 回归通过。保留 294 项重入检查，并新增协议/状态重建、在途归还、无业务指针与旧 ABI 拒绝检查；SDK 为 19 cases，ABI 为 14 cases。上述数字只属于 v2 历史记录，不代表 ABI v3。

详细命令、证据与边界见[42U v2 验证记录](docs/42U-v2验证记录-2026-09-17.md)。

### ABI v3 验证状态（2026-09-19）

GCC Debug、GCC ASan（开启正常退出泄漏检测）、GCC UBSan、Clang Release 均为 **11/11 测试目标通过**。GCC/Clang 双向混合加载、CLI 调用及旧 v1/v2 拒绝检查通过；两个独立审查任务未发现声明范围内可复现的阻塞缺陷。

当前包含 307 项重入检查、16 个 lease/version 专项场景、21 个 SDK cases、14 个 ABI cases；原有运行时 22 cases 等价保留。借用返回实际版本、闭区间边界、同版本重载后旧凭据失效、主动/撤销归还和在途 busy 均有持久回归。boot 测试的 5 个一次性调用分配失败点均恢复且无多余租约。

命令、证据与源码指纹见[本轮 v3 验证记录](docs/42U-v3验证记录-2026-09-19.md)。开发配置已恢复为 GCC Debug、无 sanitizer、输出到 `build/`；不支持 v1/v2 插件与 v3 宿主混用。

### 纯宿主 CLI 验证状态（2026-09-24）

GCC Debug、GCC ASan（`detect_leaks=0`）与 GCC UBSan 均为 **15/15 测试目标通过**。新增持久测试覆盖 manifest ABI、动态 parser 负例、显式空值、配置隔离、原样输出、输出设备失败、帮助/解析错误不创建实例、真实 DSO 生命周期、配置 defensive validation，以及 create 成功后的分配失败整批回滚。

详细命令、审核修复和仍未纳入第一版的边界见[纯宿主 CLI 验证记录（2026-09-24）](docs/42U纯宿主CLI验证记录-2026-09-24.md)。
