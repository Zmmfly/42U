/**
 * @file cli_e2e_test.cc
 * @brief Black-box process tests for the pure-host CLI and real plugin fixtures.
 *
 * Usage:
 * @code
 * cli_e2e_test <42uhost> <cli_logging_plugin> <cli_service_plugin> <cli_bad_manifest_plugin>
 *              <echo_without_manifest_plugin>
 * @endcode
 */

#define NDEBUG 1

#include <42u/cli_host.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  include <fcntl.h>
#  include <io.h>
#  include <process.h>
#  include <sys/stat.h>
#else
#  include <cerrno>
#  include <fcntl.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

const char* phase = "startup";
fs::path live_workspace;
constexpr char missing_handler_environment[] = "U42_TEST_CLI_SERVICE_HANDLER_MISSING";
constexpr char invoke_failure_environment[] = "U42_TEST_CLI_SERVICE_INVOKE_FAILED";
constexpr char stop_failure_environment[] = "U42_TEST_CLI_SERVICE_STOP_FAILED";
constexpr char unsupported_manifest_environment[] =
    "U42_TEST_CLI_BAD_MANIFEST_UNSUPPORTED";
constexpr const char* fault_environments[]{missing_handler_environment,
                                           invoke_failure_environment,
                                           stop_failure_environment,
                                           unsupported_manifest_environment};

/** @brief One explicit child-process environment assignment. */
using environment_setting = std::pair<std::string, std::string>;
/** @brief Fault-injection environment passed only to one isolated host process. */
using environment_settings = std::vector<environment_setting>;

/** @brief Report one failed check with the active test phase. */
[[noreturn]] void fail_check(const char* expression, const char* file, int line)
{
    std::fprintf(stderr, "CHECK failed [%s]: %s (%s:%d)\n", phase, expression, file, line);
    std::exit(EXIT_FAILURE);
}

/** @brief Runtime assertion that remains active under NDEBUG. */
#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) fail_check(#condition, __FILE__, __LINE__);           \
    } while (false)

/** @brief Temporarily label checks with one e2e scenario name. */
class phase_guard {
public:
    /** @brief Install one phase label until this guard is destroyed. */
    explicit phase_guard(const char* value) noexcept : previous_(phase) { phase = value; }
    /** @brief Restore the enclosing phase label. */
    ~phase_guard() { phase = previous_; }

private:
    const char* previous_;
};

/** @brief Remove the current temporary workspace during normal exit or CHECK failure. */
void cleanup_workspace() noexcept
{
    if (live_workspace.empty()) return;
    std::error_code error;
    fs::remove_all(live_workspace, error);
    live_workspace.clear();
}

/** @brief Process output and normalized process exit code. */
struct process_result {
    int exit_code = -1;
    std::string standard_output;
    std::string standard_error;
};

/** @brief Read a complete small text file. */
std::string read_text(const fs::path& path)
{
    std::ifstream stream(path, std::ios::in | std::ios::binary);
    if (!stream) return {};
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

/** @brief Read one JSONL trace as an ordered vector of lines. */
std::vector<std::string> read_lines(const fs::path& path)
{
    std::ifstream stream(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(stream, line)) lines.push_back(line);
    return lines;
}

/** @brief Extract the fixed event field emitted by a CLI fixture trace line. */
std::string event_name(const std::string& line)
{
    constexpr std::string_view prefix = "\"event\":\"";
    const std::size_t start = line.find(prefix);
    if (start == std::string::npos) return {};
    const std::size_t value = start + prefix.size();
    const std::size_t end = line.find('"', value);
    return end == std::string::npos ? std::string{} : line.substr(value, end - value);
}

/** @brief Require one fixture trace to contain only its discovery event. */
void expect_manifest_only(const fs::path& path)
{
    const std::vector<std::string> lines = read_lines(path);
    CHECK(lines.size() == 1u);
    CHECK(event_name(lines[0]) == "manifest");
}

/** @brief Require one fixture trace to have an exact lifecycle event sequence. */
void expect_events(const fs::path& path, const std::vector<std::string>& expected)
{
    const std::vector<std::string> lines = read_lines(path);
    CHECK(lines.size() == expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        CHECK(event_name(lines[index]) == expected[index]);
    }
}

/** @brief Return the first trace line carrying one event name. */
const std::string& find_event(const std::vector<std::string>& lines, std::string_view event)
{
    for (const std::string& line : lines) {
        if (event_name(line) == event) return line;
    }
    fail_check("trace event exists", __FILE__, __LINE__);
}

#if defined(_WIN32)

/** @brief Save one process environment value for restoration after a Windows spawn. */
std::optional<std::string> environment_value(const char* key)
{
    const char* const value = std::getenv(key);
    return value == nullptr ? std::nullopt : std::optional<std::string>(value);
}

/** @brief Restore one environment value changed for a Windows child process. */
void restore_environment(const char* key, const std::optional<std::string>& value)
{
    (void)_putenv_s(key, value.has_value() ? value->c_str() : "");
}

/** @brief Spawn one host process on Windows with redirected output and fixture environment. */
int spawn_host(const fs::path& host, const std::vector<std::string>& arguments,
               const fs::path& plugin_directory, const fs::path& trace_directory,
               const fs::path& stdout_path, const fs::path& stderr_path,
               const environment_settings& environment)
{
    const int output = _open(stdout_path.string().c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC,
                             _S_IREAD | _S_IWRITE);
    const int errors = _open(stderr_path.string().c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC,
                             _S_IREAD | _S_IWRITE);
    if (output < 0 || errors < 0) return -1;
    const int saved_output = _dup(_fileno(stdout));
    const int saved_errors = _dup(_fileno(stderr));
    if (saved_output < 0 || saved_errors < 0) return -1;

    const std::optional<std::string> old_plugins = environment_value("U42_PLUGIN_DIR");
    const std::optional<std::string> old_trace = environment_value("U42_TEST_TRACE_DIR");
    std::vector<std::optional<std::string>> old_faults;
    old_faults.reserve(sizeof(fault_environments) / sizeof(fault_environments[0]));
    for (const char* name : fault_environments) {
        old_faults.push_back(environment_value(name));
        (void)_putenv_s(name, "");
    }
    (void)_putenv_s("U42_PLUGIN_DIR", plugin_directory.string().c_str());
    (void)_putenv_s("U42_TEST_TRACE_DIR", trace_directory.string().c_str());
    for (const environment_setting& setting : environment) {
        (void)_putenv_s(setting.first.c_str(), setting.second.c_str());
    }
    (void)_dup2(output, _fileno(stdout));
    (void)_dup2(errors, _fileno(stderr));

    std::vector<std::string> storage;
    storage.reserve(arguments.size() + 1u);
    storage.push_back(host.string());
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<const char*> argv;
    argv.reserve(storage.size() + 1u);
    for (const std::string& value : storage) argv.push_back(value.c_str());
    argv.push_back(nullptr);
    const intptr_t child = _spawnv(_P_WAIT, storage[0].c_str(), argv.data());

    (void)_dup2(saved_output, _fileno(stdout));
    (void)_dup2(saved_errors, _fileno(stderr));
    _close(saved_output);
    _close(saved_errors);
    _close(output);
    _close(errors);
    restore_environment("U42_PLUGIN_DIR", old_plugins);
    restore_environment("U42_TEST_TRACE_DIR", old_trace);
    for (std::size_t index = 0;
         index < sizeof(fault_environments) / sizeof(fault_environments[0]); ++index) {
        restore_environment(fault_environments[index], old_faults[index]);
    }
    return child < 0 ? -1 : static_cast<int>(child);
}

#else

/** @brief Spawn one host process on POSIX with redirected output and fixture environment. */
int spawn_host(const fs::path& host, const std::vector<std::string>& arguments,
               const fs::path& plugin_directory, const fs::path& trace_directory,
               const fs::path& stdout_path, const fs::path& stderr_path,
               const environment_settings& environment)
{
    const pid_t child = ::fork();
    if (child < 0) return -1;
    if (child == 0) {
        int output_flags = O_WRONLY;
        if (stdout_path != fs::path("/dev/full")) output_flags |= O_CREAT | O_TRUNC;
        const int output = ::open(stdout_path.c_str(), output_flags, 0600);
        const int errors = ::open(stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (output < 0 || errors < 0 || ::dup2(output, STDOUT_FILENO) < 0 ||
            ::dup2(errors, STDERR_FILENO) < 0) {
            std::_Exit(126);
        }
        ::close(output);
        ::close(errors);
        if (::setenv("U42_PLUGIN_DIR", plugin_directory.c_str(), 1) != 0 ||
            ::setenv("U42_TEST_TRACE_DIR", trace_directory.c_str(), 1) != 0) {
            std::_Exit(126);
        }
        for (const char* name : fault_environments) {
            if (::unsetenv(name) != 0) std::_Exit(126);
        }
        for (const environment_setting& setting : environment) {
            if (::setenv(setting.first.c_str(), setting.second.c_str(), 1) != 0) {
                std::_Exit(126);
            }
        }

        std::vector<std::string> storage;
        storage.reserve(arguments.size() + 1u);
        storage.push_back(host.string());
        storage.insert(storage.end(), arguments.begin(), arguments.end());
        std::vector<char*> argv;
        argv.reserve(storage.size() + 1u);
        for (std::string& value : storage) argv.push_back(value.data());
        argv.push_back(nullptr);
        ::execv(host.c_str(), argv.data());
        std::_Exit(errno == ENOENT ? 127 : 126);
    }

    int status = 0;
    pid_t reaped = 0;
    do {
        reaped = ::waitpid(child, &status, 0);
    } while (reaped < 0 && errno == EINTR);
    if (reaped != child) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

#endif

/** @brief Temporary directories and isolated plugin sets used by all process cases. */
class workspace {
public:
    /** @brief Create isolated normal and invalid plugin directories from fixture DSOs. */
    workspace(const fs::path& logging, const fs::path& service, const fs::path& invalid,
              const fs::path& without_manifest)
    {
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = fs::temp_directory_path() / ("u42_cli_e2e_" + std::to_string(ticks));
        normal_plugins_ = root_ / "normal_plugins";
        invalid_plugins_ = root_ / "invalid_plugins";
        std::error_code error;
        fs::create_directories(normal_plugins_, error);
        if (error) throw std::runtime_error("cannot create normal plugin directory");
        fs::create_directories(invalid_plugins_, error);
        if (error) throw std::runtime_error("cannot create invalid plugin directory");

        copy_plugin(logging, normal_plugins_);
        copy_plugin(service, normal_plugins_);
        copy_plugin(without_manifest, normal_plugins_);
        copy_plugin(invalid, invalid_plugins_);
        live_workspace = root_;
        std::atexit(cleanup_workspace);
    }

    /** @brief Remove all copied plugins, traces, and captured process output. */
    ~workspace() { cleanup_workspace(); }

    /** @brief Directory containing the logging and service fixtures. */
    const fs::path& normal_plugins() const noexcept { return normal_plugins_; }
    /** @brief Directory containing only the invalid-manifest fixture. */
    const fs::path& invalid_plugins() const noexcept { return invalid_plugins_; }

    /** @brief Create one empty trace directory for an isolated process case. */
    fs::path make_trace(const char* name) const
    {
        const fs::path result = root_ / (std::string("trace_") + name);
        std::error_code error;
        fs::create_directories(result, error);
        if (error) throw std::runtime_error("cannot create trace directory");
        return result;
    }

    /** @brief Run one host invocation and capture both output streams. */
    process_result run(const fs::path& host, const fs::path& plugins, const fs::path& trace,
                       const std::vector<std::string>& arguments,
                       environment_settings environment = {})
    {
        const std::string stem = "process_" + std::to_string(counter_++);
        const fs::path output = root_ / (stem + ".out");
        const fs::path errors = root_ / (stem + ".err");
        process_result result;
        result.exit_code =
            spawn_host(host, arguments, plugins, trace, output, errors, environment);
        result.standard_output = read_text(output);
        result.standard_error = read_text(errors);
        return result;
    }

    /** @brief Run one invocation with stdout redirected to an existing output device. */
    process_result run_with_stdout(const fs::path& host, const fs::path& plugins,
                                   const fs::path& trace,
                                   const std::vector<std::string>& arguments,
                                   const fs::path& output_device,
                                   environment_settings environment = {})
    {
        const std::string stem = "process_" + std::to_string(counter_++);
        const fs::path errors = root_ / (stem + ".err");
        process_result result;
        result.exit_code =
            spawn_host(host, arguments, plugins, trace, output_device, errors, environment);
        result.standard_error = read_text(errors);
        return result;
    }

private:
    /** @brief Copy one fixture while preserving its recognized plugin filename suffix. */
    static void copy_plugin(const fs::path& source, const fs::path& directory)
    {
        std::error_code error;
        fs::copy_file(source, directory / source.filename(), fs::copy_options::overwrite_existing,
                      error);
        if (error) throw std::runtime_error("cannot copy plugin fixture: " + error.message());
    }

    fs::path root_;
    fs::path normal_plugins_;
    fs::path invalid_plugins_;
    std::size_t counter_ = 0;
};

/** @brief Verify --version returns before plugin discovery. */
void version_case(workspace& files, const fs::path& host)
{
    phase_guard guard("version does not load plugins");
    const fs::path trace = files.make_trace("version");
    const process_result result = files.run(host, files.normal_plugins(), trace, {"--version"});
    CHECK(result.exit_code == u42::cli::exit_ok);
    CHECK(!result.standard_output.empty());
    CHECK(result.standard_error.empty());
    CHECK(fs::directory_iterator(trace) == fs::directory_iterator());
}

/** @brief Verify root help discovers manifests but never creates instances. */
void help_case(workspace& files, const fs::path& host)
{
    phase_guard guard("help does not create");
    const fs::path trace = files.make_trace("help");
    const process_result result = files.run(host, files.normal_plugins(), trace, {"--help"});
    CHECK(result.exit_code == u42::cli::exit_ok);
    CHECK(result.standard_output.find("--log-level") != std::string::npos);
    CHECK(result.standard_output.find("serve") != std::string::npos);
    CHECK(result.standard_error.empty());
    expect_manifest_only(trace / "cli_logging.jsonl");
    expect_manifest_only(trace / "cli_service.jsonl");
}

/** @brief Verify user parse failures return 2 before any plugin instance is created. */
void parse_error_case(workspace& files, const fs::path& host)
{
    phase_guard guard("parse error does not create");
    const fs::path trace = files.make_trace("parse_error");
    const process_result result =
        files.run(host, files.normal_plugins(), trace, {"serve", "--port"});
    CHECK(result.exit_code == u42::cli::exit_usage);
    CHECK(result.standard_output.empty());
    CHECK(!result.standard_error.empty());
    expect_manifest_only(trace / "cli_logging.jsonl");
    expect_manifest_only(trace / "cli_service.jsonl");
}

/** @brief Verify malformed UTF-8 is rejected before any plugin instance is created. */
void utf8_error_case(workspace& files, const fs::path& host)
{
    phase_guard guard("invalid UTF-8 does not create");
    const std::string invalid("\xC3\x28", 2u);

    const fs::path config_trace = files.make_trace("invalid_utf8_config");
    const process_result config = files.run(
        host, files.normal_plugins(), config_trace,
        {"--log-level", invalid, "serve", "--port", "8080", "node-a"});
    CHECK(config.exit_code == u42::cli::exit_usage);
    CHECK(config.standard_output.empty());
    CHECK(config.standard_error.find("UTF-8") != std::string::npos);
    expect_manifest_only(config_trace / "cli_logging.jsonl");
    expect_manifest_only(config_trace / "cli_service.jsonl");

    const fs::path payload_trace = files.make_trace("invalid_utf8_payload");
    const process_result payload = files.run(
        host, files.normal_plugins(), payload_trace,
        {"serve", "--port", "8080", "--label", invalid, "node-a"});
    CHECK(payload.exit_code == u42::cli::exit_usage);
    CHECK(payload.standard_output.empty());
    CHECK(payload.standard_error.find("UTF-8") != std::string::npos);
    expect_manifest_only(payload_trace / "cli_logging.jsonl");
    expect_manifest_only(payload_trace / "cli_service.jsonl");
}

/** @brief Verify an invalid manifest returns 1 and never reaches factory create. */
void invalid_manifest_case(workspace& files, const fs::path& host)
{
    phase_guard guard("invalid manifest does not create");
    const fs::path trace = files.make_trace("bad_manifest");
    const process_result result = files.run(host, files.invalid_plugins(), trace, {"--help"});
    CHECK(result.exit_code == u42::cli::exit_failure);
    CHECK(result.standard_output.empty());
    CHECK(result.standard_error.find("manifest") != std::string::npos);
    expect_manifest_only(trace / "cli_bad_manifest.jsonl");
}

/** @brief Verify a rejected manifest version exits 1 before factory create. */
void unsupported_manifest_case(workspace& files, const fs::path& host)
{
    phase_guard guard("unsupported manifest does not create");
    const fs::path trace = files.make_trace("unsupported_manifest");
    const process_result result = files.run(
        host, files.invalid_plugins(), trace, {"--help"},
        {{unsupported_manifest_environment, "1"}});
    CHECK(result.exit_code == u42::cli::exit_failure);
    CHECK(result.standard_output.empty());
    CHECK(result.standard_error.find("unsupported") != std::string::npos);
    expect_manifest_only(trace / "cli_bad_manifest.jsonl");
}

/**
 * @brief Verify full activation, including one compatible v3 plugin with no CLI manifest.
 */
void success_case(workspace& files, const fs::path& host)
{
    phase_guard guard("successful command");
    const fs::path trace = files.make_trace("success");
    const process_result result = files.run(
        host, files.normal_plugins(), trace,
        {"--log-level", "debug", "serve", "--port", "8080", "--label", "a", "--label",
         "b", "--verbose", "node-a", "--mode", "fast"});
    CHECK(result.exit_code == u42::cli::exit_ok);
    CHECK(result.standard_output == "{\"handled\":true}");
    CHECK(result.standard_error.empty());

    const fs::path logging_path = trace / "cli_logging.jsonl";
    const fs::path service_path = trace / "cli_service.jsonl";
    expect_events(logging_path, {"manifest", "create", "init", "start", "stop", "destroy"});
    expect_events(service_path,
                  {"manifest", "create", "init", "start", "invoke", "stop", "destroy"});

    const std::vector<std::string> logging = read_lines(logging_path);
    const std::string& logging_init = find_event(logging, "init");
    CHECK(logging_init.find("\"log_level\":\"debug\"") != std::string::npos);
    CHECK(logging_init.find("\"source\":2") != std::string::npos);

    const std::vector<std::string> service = read_lines(service_path);
    const std::string& service_init = find_event(service, "init");
    CHECK(service_init.find("\"config_entry_count\":1") != std::string::npos);
    CHECK(service_init.find("\"mode\":\"fast\"") != std::string::npos);

    const std::string& invoke = find_event(service, "invoke");
    CHECK(invoke.find(
              "\"payload\":{\"port\":\"8080\",\"label\":[\"a\",\"b\"],"
              "\"verbose\":true,\"target\":\"node-a\"}") != std::string::npos);
    CHECK(invoke.find("\"config\":{\"mode\":\"fast\",\"source\":2}") !=
          std::string::npos);
    CHECK(invoke.find("\"log_level_status\":3") != std::string::npos);
}

/** @brief Verify a manifest-bound handler missing from capabilities fails before invoke. */
void missing_handler_case(workspace& files, const fs::path& host)
{
    phase_guard guard("missing handler");
    const fs::path trace = files.make_trace("missing_handler");
    const process_result result = files.run(
        host, files.normal_plugins(), trace,
        {"serve", "--port", "8080", "node-a"},
        {{missing_handler_environment, "1"}});
    CHECK(result.exit_code == u42::cli::exit_failure);
    CHECK(result.standard_output.empty());
    CHECK(result.standard_error.find("execute command 'serve'") != std::string::npos);
    CHECK(result.standard_error.find("not_found") != std::string::npos);

    expect_events(trace / "cli_logging.jsonl",
                  {"manifest", "create", "init", "start", "stop", "destroy"});
    const fs::path service_path = trace / "cli_service.jsonl";
    expect_events(service_path, {"manifest", "create", "init", "start", "stop", "destroy"});
    const std::vector<std::string> service = read_lines(service_path);
    CHECK(find_event(service, "start").find("\"handler_announced\":false") !=
          std::string::npos);
}

/** @brief Verify invoke and stop failures are both diagnosed and failed stop is quarantined. */
void invoke_and_stop_failure_case(workspace& files, const fs::path& host)
{
    phase_guard guard("invoke plus stop failure");
    const fs::path trace = files.make_trace("invoke_stop_failure");
    const process_result result = files.run(
        host, files.normal_plugins(), trace,
        {"serve", "--port", "8080", "node-a"},
        {{invoke_failure_environment, "1"}, {stop_failure_environment, "1"}});
    CHECK(result.exit_code == u42::cli::exit_failure);
    CHECK(result.standard_output.empty());
    const std::size_t invoke_error = result.standard_error.find("execute command 'serve'");
    const std::size_t shutdown_error = result.standard_error.find("shutdown");
    CHECK(invoke_error != std::string::npos);
    CHECK(shutdown_error != std::string::npos);
    CHECK(invoke_error < shutdown_error);
    CHECK(result.standard_error.find("quarantined") != std::string::npos);

    expect_events(trace / "cli_logging.jsonl",
                  {"manifest", "create", "init", "start", "stop", "destroy"});
    const fs::path service_path = trace / "cli_service.jsonl";
    expect_events(service_path, {"manifest", "create", "init", "start", "invoke", "stop"});
    const std::vector<std::string> service = read_lines(service_path);
    CHECK(find_event(service, "stop").find("\"status\":\"failed\"") !=
          std::string::npos);
}

/** @brief Verify -- preserves option-shaped positional bytes through the real handler call. */
void positional_escape_case(workspace& files, const fs::path& host)
{
    phase_guard guard("escaped positional");
    const fs::path trace = files.make_trace("escaped_positional");
    const process_result result = files.run(
        host, files.normal_plugins(), trace,
        {"serve", "--port", "8080", "--", "--node"});
    CHECK(result.exit_code == u42::cli::exit_ok);
    CHECK(result.standard_output == "{\"handled\":true}");
    CHECK(result.standard_error.empty());

    const std::vector<std::string> service = read_lines(trace / "cli_service.jsonl");
    const std::string& invoke = find_event(service, "invoke");
    CHECK(invoke.find("\"target\":\"--node\"") != std::string::npos);
}

#if !defined(_WIN32)
/** @brief Verify stdout device failures change version, help, and result exits to failure. */
void stdout_failure_case(workspace& files, const fs::path& host)
{
    phase_guard guard("stdout failure");
    const fs::path full = "/dev/full";
    if (!fs::exists(full)) return;

    const fs::path version_trace = files.make_trace("stdout_version");
    const process_result version = files.run_with_stdout(
        host, files.normal_plugins(), version_trace, {"--version"}, full);
    CHECK(version.exit_code == u42::cli::exit_failure);
    CHECK(version.standard_error.find("write version") != std::string::npos);
    CHECK(fs::directory_iterator(version_trace) == fs::directory_iterator());

    const fs::path help_trace = files.make_trace("stdout_help");
    const process_result help = files.run_with_stdout(
        host, files.normal_plugins(), help_trace, {"--help"}, full);
    CHECK(help.exit_code == u42::cli::exit_failure);
    CHECK(help.standard_error.find("write help") != std::string::npos);
    expect_manifest_only(help_trace / "cli_logging.jsonl");
    expect_manifest_only(help_trace / "cli_service.jsonl");

    const fs::path result_trace = files.make_trace("stdout_result");
    const process_result result = files.run_with_stdout(
        host, files.normal_plugins(), result_trace,
        {"serve", "--port", "8080", "node-a"}, full);
    CHECK(result.exit_code == u42::cli::exit_failure);
    CHECK(result.standard_error.find("write command result") != std::string::npos);
    expect_events(result_trace / "cli_logging.jsonl",
                  {"manifest", "create", "init", "start", "stop", "destroy"});
    expect_events(result_trace / "cli_service.jsonl",
                  {"manifest", "create", "init", "start", "invoke", "stop", "destroy"});
}
#endif

} // namespace

/** @brief Run all real-process pure-host CLI scenarios. */
int main(int argc, char** argv)
{
    try {
        if (argc < 6) {
            std::fprintf(stderr,
                         "usage: %s <42uhost> <cli_logging_plugin> <cli_service_plugin> "
                         "<cli_bad_manifest_plugin> <echo_without_manifest_plugin>\n",
                         argv[0]);
            return EXIT_FAILURE;
        }

        const fs::path host = fs::absolute(argv[1]);
        const fs::path logging = fs::absolute(argv[2]);
        const fs::path service = fs::absolute(argv[3]);
        const fs::path invalid = fs::absolute(argv[4]);
        const fs::path without_manifest = fs::absolute(argv[5]);
        CHECK(fs::is_regular_file(host));
        CHECK(fs::is_regular_file(logging));
        CHECK(fs::is_regular_file(service));
        CHECK(fs::is_regular_file(invalid));
        CHECK(fs::is_regular_file(without_manifest));

        workspace files(logging, service, invalid, without_manifest);
        version_case(files, host);
        help_case(files, host);
        parse_error_case(files, host);
        utf8_error_case(files, host);
        invalid_manifest_case(files, host);
        unsupported_manifest_case(files, host);
        success_case(files, host);
        missing_handler_case(files, host);
        invoke_and_stop_failure_case(files, host);
        positional_escape_case(files, host);
#if !defined(_WIN32)
        stdout_failure_case(files, host);
#endif
        std::fprintf(stdout, "cli_e2e_test: all checks passed\n");
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "cli_e2e_test: unexpected exception: %s\n", error.what());
    } catch (...) {
        std::fprintf(stderr, "cli_e2e_test: unexpected non-standard exception\n");
    }
    cleanup_workspace();
    return EXIT_FAILURE;
}
