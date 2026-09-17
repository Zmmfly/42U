# 42U 插件框架

42U 是一个以 **42U 标准机柜** 为设计类比的 C++ 插件框架：宿主（host）负责扫描、加载并统一编排插件，插件像机柜内的设备一样通过约定的 ABI 接入，再借助宿主提供的事件、能力与调用服务协作。“42U”描述的是模块化、标准化接入与可组合的设计理念，不是可容纳插件数量的硬上限。

- 构建工具：xmake
- 语言标准：C++（当前构建配置为 C++17）
- 设计文档：[docs/42U插件框架设计.md](docs/42U插件框架设计.md)

## 仓库布局

| 路径 | 内容 |
| --- | --- |
| `inc/42u/abi.hpp` | 首版 ABI v1 声明：状态码、接口 ID（`iid`）、跨界结构、纯虚接口、入口与布局断言；尚不承诺历史发布版本兼容 |
| `inc/42u/host.hpp` | 宿主公共门面 `u42::host`：`boot`/`start`/`load`/`unload`/`shutdown`/`poll`/`bind`/`unbind`/`call`/`plugins`/`error` |
| `inc/42u/plug.hpp` | 动态库映射 `u42::plug` 与确定性候选扫描 `u42::scan_plugins` |
| `inc/42u/order.hpp` | 初始化排序规划 `u42::plan_order`（优先级、`before`/`after`、环检测） |
| `inc/42u/sdk.hpp` | 插件侧源级 RAII 辅助（借用租约、字节视图等）；不进入 ABI |
| `src/` | 宿主实现：生命周期引擎、上下文与服务实现、事件、库加载、排序 |
| `host/src/main.cc` | 最小 CLI `42uhost`：启动一个插件目录，然后列出插件或调用一次 |
| `examples/` | 示例插件（`echo`、`consumer`），各自独立构建为 `.u42.so` |
| `tests/` | 不依赖外部测试框架的检查 |
| `docs/` | 设计文档与设计讨论稿 |

## 构建与测试

```bash
# 本轮已验证的 Linux / GCC 配置；显式重置持久配置中的检测器和输出目录
xmake f -m debug --toolchain=gcc --sanitizer=none -o build -y
xmake -j 8 && xmake test -v
```

首版不依赖第三方运行库或测试框架，仅使用 C++ 标准库和系统动态加载接口。可选运行时检查：

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
# Linux 示例：按方法名调用 echo 示例插件的 echo 方法
xmake run 42uhost --plugins build/linux/x86_64/debug/plugins --call com.example.echo echo '{"hello":"42u"}'
```

用法：

```text
42uhost [--plugins DIR] (--list | --call PLUG METHOD JSON | --call-id PLUG NUMBER JSON)
42uhost [--help]
```

| 选项 | 说明 |
| --- | --- |
| `--plugins DIR` | 插件目录；省略时使用**可执行文件同级的 `plugins` 目录**（Linux 下经 `/proc/self/exe` 解析真实可执行文件路径） |
| `--list` | 启动后逐行打印已就绪插件的标识；目录内没有候选插件时不输出内容并返回 0 |
| `--call PLUG METHOD JSON` | 按方法名调用一次；`JSON` 传空字符串（`''`）表示无参数 |
| `--call-id PLUG NUMBER JSON` | 按方法数字 ID 调用一次；`NUMBER` 为 `0..4294967295` 的十进制无符号整数 |
| `--help`、`-h` | 打印用法并返回 0；完全不带参数运行也同样打印用法并返回 0 |

约定：

- **先校验、后加载**：未知选项、重复或冲突的动作、参数不足、带符号或非十进制、溢出的数字，都在参数解析阶段拒绝；只有命令行通过校验后才会加载插件动态库。
- **不解析 JSON**：`JSON` 参数按原始字节转发给插件；调用成功后插件返回的 JSON 原样写到 stdout（仅在其不以换行结尾时补一个换行），本工具不做校验、重排或包装。
- **失败语义**：错误以 `error: <stage>: <status>: <host 诊断>` 形式写到 stderr（例如 `boot` 目录不存在）；退出码 `2` 表示命令行被拒绝，`1` 表示 `boot`、调用或 `shutdown` 失败，`0` 表示成功。
- **显式 shutdown**：每次成功 `boot` 之后都会调用 `shutdown()`，`shutdown` 失败同样以非零退出；宿主析构只作为兜底，不是正常清理路径。
- **单动作、非交互**：一次进程只执行 `--list` 或一次调用，不做交互式会话。动态卸载/重载通过 C++ 的 `host::unload()` 与 `host::load()` 使用，CLI 暂无 `--reload` 选项。

## ABI 与运行模型

### 固定 ABI profile

ABI v1 采用 **C 链接入口 + 受约束的 C++ 纯虚接口**：

- 动态库只导出一个入口 `u42_get_factory`：先协商基础 ABI 主版本（当前为 `1`），再返回库内工厂；不支持时返回错误，不返回可调用对象。
- 跨界类型限定为固定宽度整数、已定义布局的简单结构、不透明标识（`token`、`binding`）、指针与长度视图（`bytes`）、纯虚接口指针。STL 容器、异常、RTTI 对象、线程对象不跨边界；边界函数为 `noexcept`，异常在产生侧转换为状态码。
- 对象由分配方销毁：宿主调用插件的 `destroy()`，消费者不销毁借用接口；跨库 `new`/`delete` 不成立。
- 接口只有纯虚函数、无数据成员；接口指针只能通过 `ictx::query` 或 `icaps::acquire` 取得，不能由根对象地址推算。
- 当前验证 profile 为 **Linux x86_64、64 位原生对齐、Itanium C++ ABI、C++17、GCC 15.2 / Clang 21.1 与本机 libstdc++/glibc**。两种编译器构建的宿主和示例插件已做双向混合加载测试；这不等于所有 GCC/Clang 版本均兼容，也不保证可部署到更旧的运行库。
- Windows、macOS、32 位和其他架构尚未验证。入口的主版本协商不能自动识别所有 ABI profile 差异，部署方仍须保证工具链、架构与运行库匹配。

### 接口名单

`inc/42u/abi.hpp` 中的官方服务接口（`iid.high` 均为 `0x3432555f41424931`）：

| 常量 | `iid.low` | 服务 | 用途 |
| --- | --- | --- | --- |
| `events_iid` | 1 | `ievents` | 精确名称的事件订阅、取消与投递 |
| `caps_iid` | 2 | `icaps` | 能力登记、快照监听、接口借用与归还 |
| `calls_iid` | 3 | `icalls` | 按名称或数字 ID 绑定并同步调用方法 |
| `diag_iid` | 4 | `idiag` | 宿主诊断输出 |
| `invoke_iid` | 5 | `iinvoke` | 插件侧可选的动态方法接口 |

插件通过注入的 `ictx::query(&iid, &out)` 取得前四项宿主服务；`invoke_iid` 对应插件提供的接口，由宿主通过 `iplug::query` 获取，不能向 `ictx` 查询。根实例接口为 `iplug`（`init`/`start`/`stop`/`destroy` 与能力查询），工厂为 `iplug_fty`（`describe`/`create`），库内元数据为 `plug_desc`，方法描述为 `method_desc`。

### 线程与信任模型（第一阶段限制）

- **同步单线程、可信插件**：宿主所有接口只在控制线程调用，`host` 与 `iplug` 都不会代为跨线程投递；插件内部可以使用工作线程做私有计算，但不得从工作线程回调宿主或其他插件接口，也不得把借用指针交给工作线程。框架不提供沙箱，也无法强制终止进程内插件。
- **unload 需借用确认**：卸载前先撤下能力并逐项撤销借用，消费者必须在撤销回调中归还凭据；只要存在未归还的借用、在途调用或 `stop()` 失败，卸载就会被拒绝。无法证明静默的实例会被**隔离**而不是被强行拆解（表现为进程直到退出仍持有该实例及其库映射）。停止失败的消费者持有的借用也必须保留，因此它依赖的提供者可能一并被保留；不能用删除借用记录来伪造安全卸载。方法绑定按实例代次校验：重新加载后旧绑定调用报错，旧归还凭据不会命中新记录。
- **生命周期不重入**：插件调用栈中的 `unload()` 仅对已知 ID 返回 `deferred` 并排队，未知 ID 返回 `not_found`；`shutdown()`、`add()`、`boot()`、`load()` 在这种情况下返回 `busy`，`start()` 返回 `invalid_state`，必须由应用在安全点重试。
- **启动批次边界**：全部 `init()` 完成后按计划逐一 `start()`，每个成功启动后的能力通知照常派发；Initialized 消费者可以保存借用，不能调用业务。整个启动批次及其失败回滚不消费延迟卸载队列，因此成功返回时本批插件仍全部 Active。启动返回后由 `poll()`、显式 `unload()` 或其他批次外的安全清理处理排队请求；`deferred` 不代表已完成卸载。失败回滚销毁的实例会连同其请求一起移除，原有实例的请求保留排队。分配失败时回滚仅为尽力清理，可能留下 Created 或隔离记录供重试、关停。`poll()` 中正常状态失败的卸载在存储允许时重新入队供后续重试；内部分配异常仅尽力保留队列，收到 `failed` 后应显式重试 `unload(plug_id)`。预算耗尽时未处理工作也留到下次派发。`boot()` / `load()` 的内部启动遵循同一语义。
- **宿主寿命与异常**：不得在任何宿主操作或插件回调执行期间销毁 `host`；`shutdown()` 的重入保护不使回调中 `delete host` 合法。返回 `status` 的 native 操作把内部异常转换为状态码，诊断可能因分配失败而保持旧值；构造函数、`plugins()` 快照及调用方准备实参的分配仍可能抛异常。
- **插件自行独立构建**：每个示例插件都是独立编译的 C++ 动态库（匹配的 ABI profile），不链接宿主静态库；宿主在运行时 `dlopen` 加载。对象实现留在各自一侧，跨边界只传 ABI 已定义的类型与受寿命约束的虚接口指针。

## 文档

- [docs/42U插件框架设计.md](docs/42U插件框架设计.md)：设计主文档（目标与范围、启动流程、初始化编排、能力与借用、动态卸载、ABI 交付约束、验收标准）。
- `docs/thinks/`：本地探索区，讨论稿被 Git 忽略，不作为发布规范。

## 验证状态

2026-09-17 对本轮收尾后的工作树重新验证：**GCC Debug、GCC ASan（开启正常退出泄漏检测）、GCC UBSan、Clang Release 均为 10/10 测试目标通过**。GCC 宿主加载 Clang 插件及反向混合加载的完整集成测试均通过，CLI 列举、名称调用、数字调用和错误出口也已验证。

重入回归现有 294 项检查，包含启动中自卸载、同批前后插件卸载、失败批次的请求隔离、暂被借用阻塞的延迟卸载重试和派发预算边界。新增独立 `boot_test` 覆盖旧身份快照的 11 个分配失败点、双故障下的补充回滚，以及 native 参数拒绝的异常边界；`withdraw_test` 覆盖撤回通知的 14 个分配失败点。这两个全局分配故障注入测试必须保持为独立可执行文件。

停止失败场景按设计保留资源，测试在独立子进程验证不销毁，再由进程退出回收；这与正常退出的泄漏检查分开处理。验证命令、源码指纹、验收项对应和边界详见[本轮验证记录](docs/42U首版验证记录-2026-09-17.md)。

尚无历史 SDK 发布产物，未完成跨历史版本兼容性认证；Windows、macOS、32 位及其他架构亦未验证，不代表所有首版验收项已经覆盖。当前开发配置已恢复为 GCC Debug、无 sanitizer、输出到 `build/`。
