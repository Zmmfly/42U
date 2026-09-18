/**
 * @file main.cc
 * @brief Minimal 42u host command line: boot one plugin directory, then list plugins or call once.
 *
 * The front end is deliberately small and non-interactive:
 *
 * - The complete command line is validated before any plugin library is mapped, so a rejected
 *   invocation cannot dlopen() a candidate.
 * - Arguments are forwarded to the plugin verbatim as bounded ABI bytes; this tool contains no
 *   JSON parser and never rewrites the JSON document that the plugin returned.
 * - The provider version is discovered explicitly through u42::host::version() before a call, and
 *   an exact-version range built from it is passed to the one-shot call, because accepting one
 *   discovered version for a single management call says nothing about later releases.
 * - At most one action runs per process, and the rack is always shut down explicitly before the
 *   process exits, so capabilities are withdrawn and instances stop instead of being dropped.
 *
 * Exit codes: 0 for success (also for --help and for an empty command line), 1 for a runtime
 * failure such as an unusable plugin directory, a failing call or a failing shutdown, and 2 for a
 * rejected command line.
 */
#include <42u/host.hpp>

#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

/** @brief Frozen ABI namespace used by this front end. */
namespace abi = u42::abi::v3;

/**
 * @brief Process exit codes; a rejected command line stays separable from a runtime failure.
 */
enum exit_code : int {
    exit_ok = 0,
    exit_failure = 1,
    exit_usage = 2,
};

/**
 * @brief Render one ABI status as a stable diagnostic word.
 *
 * @param value ABI status returned by the rack.
 * @return Lowercase status name, or the decimal value for a code unknown to this build.
 * @note This mirrors the internal status_text() in src/host.cc, which is not exported to the CLI.
 */
std::string status_text(abi::status value)
{
    switch (value) {
        case abi::ok: return "ok";
        case abi::invalid_argument: return "invalid_argument";
        case abi::unsupported: return "unsupported";
        case abi::not_found: return "not_found";
        case abi::duplicate: return "duplicate";
        case abi::invalid_state: return "invalid_state";
        case abi::busy: return "busy";
        case abi::stale: return "stale";
        case abi::limit_exceeded: return "limit_exceeded";
        case abi::failed: return "failed";
        case abi::wrong_thread: return "wrong_thread";
        case abi::cycle: return "cycle";
        case abi::deferred: return "deferred";
    }
    return std::to_string(value);
}

/**
 * @brief Combine an ABI status with the last host diagnostic.
 *
 * @param value Failing ABI status.
 * @param diagnostic Text from u42::host::error(); may be empty for statuses the host does not
 *                   annotate itself, in which case only the status name is reported.
 * @return "<status>" or "<status>: <diagnostic>".
 */
std::string describe(abi::status value, const std::string& diagnostic)
{
    if (diagnostic.empty()) return status_text(value);
    return status_text(value) + ": " + diagnostic;
}

/**
 * @brief View a JSON argument string as bounded ABI bytes without copying it.
 *
 * @param text Caller-owned JSON document; it must outlive the call.
 * @return Borrowed view valid only while the call runs. An empty document yields {nullptr, 0},
 *         which is the exact shape the ABI requires for a zero length.
 */
abi::bytes as_bytes(const std::string& text)
{
    if (text.empty()) return abi::bytes{nullptr, 0};
    return abi::bytes{text.data(), static_cast<std::uint64_t>(text.size())};
}

/**
 * @brief Parse a non-negative decimal method identifier with explicit overflow and sign checks.
 *
 * @param text Candidate argument text; leading '+'/'-', whitespace and trailing junk are rejected
 *             before any numeric conversion, so a negative value can never wrap around.
 * @param out Receives the validated identifier; untouched on failure.
 * @return true when text is exactly a decimal integer in 0..4294967295.
 */
bool parse_method_id(std::string_view text, abi::method_id& out)
{
    if (text.empty()) return false;
    for (const char character : text) {
        if (character < '0' || character > '9') return false;
    }
    std::uint64_t value = 0;
    const char* const first = text.data();
    const char* const last = text.data() + text.size();
    const std::from_chars_result parsed = std::from_chars(first, last, value, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != last) return false;
    if (value > 0xFFFFFFFFull) return false;
    out = static_cast<abi::method_id>(value);
    return true;
}

/**
 * @brief What the process does after the plugin directory has been booted.
 */
enum class action {
    none,
    list,
    call_name,
    call_id,
};

/**
 * @brief Fully validated command line.
 */
struct options {
    std::filesystem::path plugins; ///< Plugin directory; empty when the default applies.
    bool has_plugins = false;      ///< Whether --plugins was given explicitly.
    action selected = action::none;///< Exactly one action, or none for a rejected line.
    std::string plug;              ///< Provider identity for --call/--call-id.
    std::string method;            ///< Method name for --call.
    abi::method_id method_id = 0;  ///< Method number for --call-id.
    std::string json;              ///< Raw JSON argument document; an empty string means none.
};

/**
 * @brief Record a usage error without throwing.
 *
 * @param error Receives the reason.
 * @param message Human-readable reason.
 * @return Always false, so parsing can "return fail(...)" in one line.
 */
bool fail(std::string& error, std::string message)
{
    error = std::move(message);
    return false;
}

/**
 * @brief Validate the whole command line before any plugin library is touched.
 *
 * Unknown options, repeated selectors, conflicting actions, missing operands and non-decimal or
 * out-of-range numbers are all rejected here, so the caller never reaches u42::host::boot() with
 * a command line that could have failed earlier.
 *
 * @param argc Argument count including argv[0].
 * @param argv Raw argument vector, unchanged.
 * @param out Receives the validated selection; only meaningful when true is returned.
 * @param error Receives a human-readable reason when false is returned.
 * @return true when the arguments may be executed, false when they must be rejected.
 */
bool parse_args(int argc, char** argv, options& out, std::string& error)
{
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg = argv[index];
        if (arg == "--plugins") {
            if (out.has_plugins) return fail(error, "--plugins 只能指定一次");
            if (index + 1 >= argc) return fail(error, "--plugins 需要一个目录参数");
            out.plugins = argv[++index];
            if (out.plugins.empty()) return fail(error, "--plugins 的目录不能为空");
            out.has_plugins = true;
        } else if (arg == "--list") {
            if (out.selected != action::none) return fail(error, "--list 与其它动作选项冲突");
            out.selected = action::list;
        } else if (arg == "--call") {
            if (out.selected != action::none) return fail(error, "--call 与其它动作选项冲突");
            if (index + 3 >= argc) return fail(error, "--call 需要 PLUG、METHOD 和 JSON 三个参数");
            out.plug = argv[++index];
            out.method = argv[++index];
            out.json = argv[++index];
            if (out.plug.empty()) return fail(error, "--call 的 PLUG 不能为空");
            if (out.method.empty()) return fail(error, "--call 的 METHOD 不能为空");
            out.selected = action::call_name;
        } else if (arg == "--call-id") {
            if (out.selected != action::none) return fail(error, "--call-id 与其它动作选项冲突");
            if (index + 3 >= argc)
                return fail(error, "--call-id 需要 PLUG、NUMBER 和 JSON 三个参数");
            out.plug = argv[++index];
            const std::string_view number = argv[++index];
            out.json = argv[++index];
            if (out.plug.empty()) return fail(error, "--call-id 的 PLUG 不能为空");
            if (!parse_method_id(number, out.method_id)) {
                return fail(error, "--call-id 的 NUMBER 必须是 0..4294967295 的十进制整数，收到 '" +
                                       std::string(number) + "'");
            }
            out.selected = action::call_id;
        } else {
            return fail(error, "无法识别的选项: '" + std::string(arg) + "'");
        }
    }
    if (out.selected == action::none) return fail(error, "缺少动作选项: 需要 --list 或 --call/--call-id");
    return true;
}

/**
 * @brief Directory used when --plugins is omitted: "<executable dir>/plugins".
 *
 * @param program argv[0] as passed by the caller; may be null or empty.
 * @return Candidate directory next to the running executable. It is not required to exist here:
 *         u42::host::boot() reports a precise error for a missing or unusable directory.
 * @note Linux resolves the real executable through /proc/self/exe, so a PATH- or relative-based
 *       invocation still finds the sibling directory of the installed binary. On other platforms
 *       the documented `xmake run 42uhost` flow is served by canonicalizing argv[0].
 */
std::filesystem::path default_plugins_dir(const char* program)
{
#if defined(__linux__)
    std::error_code self_error;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", self_error);
    if (!self_error && !self.empty()) return self.parent_path() / "plugins";
#endif
    std::error_code path_error;
    std::filesystem::path resolved;
    if (program != nullptr && *program != '\0') resolved = std::filesystem::weakly_canonical(program, path_error);
    if (path_error || resolved.empty()) {
        path_error.clear();
        resolved = std::filesystem::current_path(path_error);
    }
    if (path_error || resolved.empty()) return std::filesystem::path("plugins");
    return resolved.parent_path() / "plugins";
}

/**
 * @brief Write the command summary to one stream.
 *
 * @param out Target stream: stdout for --help, stderr for a rejected command line.
 */
void print_usage(std::ostream& out)
{
    out << "用法:\n"
           "  42uhost [--plugins DIR] (--list | --call PLUG METHOD JSON | --call-id PLUG NUMBER JSON)\n"
           "  42uhost [--help]\n"
           "\n"
           "选项:\n"
           "  --plugins DIR            插件目录；省略时使用可执行文件同级的 plugins 目录\n"
           "  --list                   启动后逐行打印已就绪插件的标识（无候选插件时不输出内容）\n"
           "  --call PLUG METHOD JSON  按方法名调用一次；JSON 传空字符串表示无参数\n"
           "  --call-id PLUG NUMBER    按方法数字 ID 调用一次；NUMBER 为 0..4294967295 的十进制整数\n"
           "  --help, -h               打印本帮助并返回 0\n"
           "\n"
           "行为:\n"
           "  - 命令行先被完整校验，之后才会加载任何插件动态库。\n"
           "  - 调用成功时，插件返回的 JSON 原样写到 stdout（仅在其不以换行结尾时补一个换行）。\n"
           "  - 调用前先用 u42::host::version() 发现提供者当前版本，再把 exact_version(actual) 传给一次性调用；\n"
           "    本次命令按发现结果原样接受，不代表长期消费者自动接受后续升级。\n"
           "  - 失败时 stderr 输出 'error: <stage>: <status>: <host 诊断>'。\n"
           "  - 结束后显式 shutdown；shutdown 失败同样以非零退出。\n"
           "\n"
           "退出码: 0 成功（含无参数与 --help）；1 运行时失败（boot/shutdown/调用失败）；2 命令行被拒绝\n";
}

/**
 * @brief Boot the rack, run the single selected action, then shut the rack down explicitly.
 *
 * @param opts Validated command line.
 * @param program argv[0], used only to locate the default plugin directory.
 * @return Process exit code; non-zero as soon as booting, the action or shutdown fails.
 * @note A failed boot has already rolled its batch back, so the constructor's teardown covers the
 *       remaining resources; a successful boot is always followed by an explicit shutdown.
 * @note A call action first discovers the current version through u42::host::version() and passes
 *       exact_version(actual) to the one-shot host call. That exact range is accepted only for
 *       this single command and proves nothing about later releases, because this front end is a
 *       one-shot management caller rather than a stateful consumer.
 */
int execute(const options& opts, const char* program)
{
    const std::filesystem::path directory =
        opts.has_plugins ? opts.plugins : default_plugins_dir(program);

    u42::host rack;
    const abi::status booted = rack.boot(directory);
    if (booted != abi::ok) {
        std::cerr << "error: boot " << directory.string() << ": " << describe(booted, rack.error())
                  << '\n';
        return exit_failure;
    }

    int code = exit_ok;
    if (opts.selected == action::list) {
        for (const std::string& id : rack.plugins()) std::cout << id << '\n';
        std::cout.flush();
    } else {
        // Ask the rack for the provider's current version, then require exactly that version for
        // the one-shot call: no cached or inferred range is ever substituted here.
        abi::plugin_version actual{};
        const abi::status discovered = rack.version(opts.plug, &actual);
        if (discovered != abi::ok) {
            std::cerr << "error: version " << opts.plug << ": "
                      << describe(discovered, rack.error()) << '\n';
            code = exit_failure;
        } else {
            const abi::version_range required = abi::exact_version(actual);
            const abi::bytes args = as_bytes(opts.json);
            std::string result;
            abi::status called = abi::failed;
            if (opts.selected == action::call_name) {
                called = rack.call(opts.plug, required, opts.method, args, &result);
            } else {
                called = rack.call(opts.plug, required, opts.method_id, args, &result);
            }
            if (called != abi::ok) {
                std::cerr << "error: call " << opts.plug << ": " << describe(called, rack.error())
                          << '\n';
                code = exit_failure;
            } else {
                // The plugin owns the JSON shape; this front end only forwards those exact bytes.
                std::cout << result;
                if (result.empty() || result.back() != '\n') std::cout << '\n';
                std::cout.flush();
            }
        }
    }

    const abi::status stopped = rack.shutdown();
    if (stopped != abi::ok) {
        std::cerr << "error: shutdown: " << describe(stopped, rack.error()) << '\n';
        code = exit_failure;
    }
    return code;
}

} // namespace

/**
 * @brief Program entry.
 *
 * An empty command line and --help print the usage and succeed, because running the tool without
 * arguments cannot do anything useful. Any other command line is validated strictly, and the
 * whole body is guarded so an unexpected exception becomes a diagnostic plus a non-zero exit
 * instead of a terminate().
 *
 * @param argc Number of command-line arguments, including the program name.
 * @param argv Argument strings; the vector is only read.
 * @return 0 on success, 1 for a runtime failure, 2 for a rejected command line.
 */
int main(int argc, char** argv)
{
    try {
        if (argc <= 1) {
            print_usage(std::cout);
            return exit_ok;
        }
        for (int index = 1; index < argc; ++index) {
            const std::string_view arg = argv[index];
            if (arg == "--help" || arg == "-h") {
                print_usage(std::cout);
                return exit_ok;
            }
        }

        options parsed;
        std::string reason;
        if (!parse_args(argc, argv, parsed, reason)) {
            std::cerr << "error: " << reason << '\n';
            std::cerr << "提示: 运行 '42uhost --help' 查看用法。\n";
            return exit_usage;
        }

        const char* const program = argv[0] != nullptr ? argv[0] : "";
        return execute(parsed, program);
    } catch (const std::exception& unexpected) {
        std::cerr << "error: 未捕获的异常: " << unexpected.what() << '\n';
        return exit_failure;
    } catch (...) {
        std::cerr << "error: 未捕获的未知异常\n";
        return exit_failure;
    }
}
