/**
 * @file integration_test.cc
 * @brief End-to-end rack test over real plugin DSOs; no third-party test framework.
 *
 * Usage: integration_test <plugins_dir> <bad_abi_library> <missing_entry_library>
 *   plugins_dir           directory holding the echo and consumer plugin DSOs
 *   bad_abi_library       DSO exporting u42_get_factory, which rejects the ABI major
 *   missing_entry_library DSO exporting an unrelated symbol and no u42_get_factory
 *
 * The file owns its main(); it links against the 42u library (u42::host, u42::plug and
 * u42::scan_plugins) and needs the two negative-fixture DSOs present at runtime. Every
 * temporary directory is created below the system temp directory under a unique name and
 * removed again before the check that created it returns.
 *
 * @note NDEBUG is defined deliberately: CHECK must keep reporting failures even when
 *       assert() has been compiled out, so a diagnostic can never vanish in release builds.
 */
#define NDEBUG 1

#include <42u/host.hpp>
#include <42u/plug.hpp>

#include <charconv>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace {

namespace abi = u42::abi::v1;
namespace fs = std::filesystem;

using u42::host;
using u42::plug;

/** @brief Plugin identity and method contract published by the example plugins. */
constexpr const char* kEchoPlugId = "com.example.echo";
constexpr const char* kConsumerPlugId = "com.example.consumer";
constexpr const char* kEchoMethod = "echo";
constexpr const char* kStatusMethod = "status";
constexpr abi::method_id kEchoMethodId = 1;
constexpr abi::method_id kStatusMethodId = 1;
constexpr const char* kEchoPayload = "42u integration payload";
constexpr const char* kFixtureDummySymbol = "u42_fixture_unrelated_symbol";

/** @brief Label of the phase currently running; every failure prints it. */
const char* g_phase = "<startup>";

/**
 * @brief Report a failed check and stop the process with a non-zero status.
 *
 * @param file Source file of the failed check.
 * @param line Source line of the failed check.
 * @param format printf-style diagnostic describing the failure.
 */
[[noreturn]] void failf(const char* file, int line, const char* format, ...)
{
    std::fprintf(stderr, "CHECK failed [%s] at %s:%d: ", g_phase, file, line);
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(stderr, format, arguments);
    va_end(arguments);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    std::exit(EXIT_FAILURE);
}

/** @brief Failure reporting that never compiles away, unlike assert(). */
#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) failf(__FILE__, __LINE__, "%s", #condition);           \
    } while (false)

#define U42_STRINGIFY(...) #__VA_ARGS__
#define U42_EXPAND_STRING(...) U42_STRINGIFY(__VA_ARGS__)

/**
 * @brief Compile-time guard: CHECK must expand to a call of failf, never to assert().
 *
 * With NDEBUG defined, assert() collapses to ((void)0), so every failure would silently
 * disappear. This is checked on the expansion text because a runtime probe cannot detect
 * it: if CHECK had become a no-op, the probe itself would be a no-op as well.
 */
inline constexpr std::string_view u42_check_expansion = U42_EXPAND_STRING(CHECK(0 == 1));
static_assert(u42_check_expansion.find("failf") != std::string_view::npos,
              "CHECK must call failf directly; defining it as assert() disables it under NDEBUG");

/** @brief Mark the current phase for diagnostics and restore the previous label on exit. */
struct phase_guard {
    const char* previous;
    explicit phase_guard(const char* name) : previous(g_phase) { g_phase = name; }
    ~phase_guard() { g_phase = previous; }
    phase_guard(const phase_guard&) = delete;
    phase_guard& operator=(const phase_guard&) = delete;
};

/** @brief Human-readable ABI status for diagnostics. */
const char* status_name(abi::status value) noexcept
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
    default: return "unknown";
    }
}

/**
 * @brief Require an exact ABI status.
 *
 * @param actual Status returned by the call under test.
 * @param expected Required status.
 * @param what Operation label used in the diagnostic.
 */
void expect_status(abi::status actual, abi::status expected, const char* what)
{
    if (actual == expected) return;
    failf(__FILE__, __LINE__, "%s: expected %s(%u) but got %s(%u)", what, status_name(expected),
          static_cast<unsigned>(expected), status_name(actual), static_cast<unsigned>(actual));
}

/** @brief Require abi::v1::ok. */
void expect_ok(abi::status actual, const char* what)
{
    expect_status(actual, abi::ok, what);
}

/**
 * @brief Wrap a string as borrowed ABI bytes for the duration of one call.
 *
 * @param value Bytes borrowed only until the enclosing call returns.
 * @return Null-or-data descriptor matching value.size().
 */
abi::bytes bytes_of(const std::string& value) noexcept
{
    return abi::bytes{value.data(), static_cast<std::uint64_t>(value.size())};
}

/** @brief Check whether a plugin identity is present in a host listing. */
bool has_plugin(const std::vector<std::string>& ids, const char* plug_id)
{
    for (const std::string& id : ids) {
        if (id == plug_id) return true;
    }
    return false;
}

/**
 * @brief Build a process-unique temporary token without platform-specific process APIs.
 *
 * @return Token that is unique for the lifetime of the running process.
 */
std::string unique_token()
{
    static std::uint64_t counter = 0;
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::to_string(ticks) + "_" + std::to_string(counter++);
}

/** @brief Temporary directories this process still owns, cleaned before it terminates. */
std::vector<fs::path>& live_temp_dirs()
{
    static std::vector<fs::path> dirs;
    return dirs;
}

/**
 * @brief Remove every temporary directory the process still owns.
 *
 * failf() terminates through std::exit(), which skips automatic objects but still runs
 * atexit handlers, so a failing check must not leave its directory behind. The registry is
 * registered first, so it is destroyed after this handler has already run.
 */
void cleanup_live_temp_dirs() noexcept
{
    for (const fs::path& directory : live_temp_dirs()) {
        try {
            std::error_code ec;
            fs::remove_all(directory, ec);
        } catch (...) {
            // A diagnostic run must never terminate inside its own exit handler.
        }
    }
    live_temp_dirs().clear();
}

/**
 * @brief Unique temporary directory whose contents are removed on destruction or exit.
 *
 * @note Creation throws std::runtime_error; the destructor only performs best-effort
 *       removal and never throws, so unwinding cannot terminate the test.
 */
class temp_directory {
public:
    explicit temp_directory(const char* tag)
        : root_(fs::temp_directory_path() /
                fs::path(std::string("u42_it_") + tag + "_" + unique_token()))
    {
        std::error_code ec;
        fs::remove_all(root_, ec);
        ec.clear();
        fs::create_directories(root_, ec);
        if (ec) {
            throw std::runtime_error("cannot create temporary directory '" + root_.string() +
                                     "': " + ec.message());
        }
        // Registered once: an empty registry means this is the process's first directory.
        if (live_temp_dirs().empty()) std::atexit(cleanup_live_temp_dirs);
        live_temp_dirs().push_back(root_);
    }

    ~temp_directory()
    {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    temp_directory(const temp_directory&) = delete;
    temp_directory& operator=(const temp_directory&) = delete;

    /** @brief Absolute path of the temporary directory. */
    const fs::path& path() const noexcept { return root_; }

    /** @brief Remove the directory now and require that nothing survives. */
    void remove_and_verify()
    {
        std::error_code ec;
        fs::remove_all(root_, ec);
        if (ec) {
            failf(__FILE__, __LINE__, "cannot remove temporary directory '%s': %s",
                  root_.string().c_str(), ec.message().c_str());
        }
        CHECK(!fs::exists(root_));
    }

private:
    fs::path root_;
};

// ---------------------------------------------------------------------------
// Minimal read-only JSON field probing for plugin status payloads.
// ---------------------------------------------------------------------------

/** @brief Skip JSON whitespace. */
std::size_t skip_ws(std::string_view text, std::size_t index) noexcept
{
    while (index < text.size() && (text[index] == ' ' || text[index] == '\t' ||
                                   text[index] == '\n' || text[index] == '\r')) {
        ++index;
    }
    return index;
}

/**
 * @brief Read one quoted JSON string starting at a quotation mark.
 *
 * @param text Whole JSON text.
 * @param quote Index of the opening quotation mark.
 * @param out Content between the quotes, without escape processing.
 * @return Index just past the closing quotation mark.
 */
std::size_t read_string(std::string_view text, std::size_t quote, std::string_view& out) noexcept
{
    std::size_t index = quote + 1;
    const std::size_t begin = index;
    while (index < text.size()) {
        if (text[index] == '\\') {
            index += 2;
            continue;
        }
        if (text[index] == '"') break;
        ++index;
    }
    out = text.substr(begin, index - begin);
    return index < text.size() ? index + 1 : index;
}

/**
 * @brief Find the end of one JSON value: past a string, past a nested container, or at
 *        the next top-level comma/brace/bracket.
 *
 * @param text Whole JSON text.
 * @param begin Index of the first value character.
 * @return Index just past the value (or past the end when the value is truncated).
 */
std::size_t scan_value_end(std::string_view text, std::size_t begin) noexcept
{
    if (begin >= text.size()) return begin;
    const char first = text[begin];
    if (first == '"') {
        std::string_view ignored;
        return read_string(text, begin, ignored);
    }
    if (first == '{' || first == '[') {
        int depth = 0;
        for (std::size_t index = begin; index < text.size(); ++index) {
            const char current = text[index];
            if (current == '"') {
                std::string_view ignored;
                index = read_string(text, index, ignored) - 1;
                continue;
            }
            if (current == '{' || current == '[') {
                ++depth;
            } else if (current == '}' || current == ']') {
                if (--depth == 0) return index + 1;
            }
        }
        return text.size();
    }
    std::size_t index = begin;
    while (index < text.size() && text[index] != ',' && text[index] != '}' && text[index] != ']') {
        ++index;
    }
    while (index > begin && (text[index - 1] == ' ' || text[index - 1] == '\t' ||
                            text[index - 1] == '\n' || text[index - 1] == '\r')) {
        --index;
    }
    return index;
}

/**
 * @brief Look up one top-level object member and return its raw value text.
 *
 * @param text Whole JSON object text.
 * @param key Member name without quotes.
 * @return Raw value without surrounding whitespace, or an empty view when absent.
 */
std::string_view json_value(std::string_view text, std::string_view key) noexcept
{
    std::size_t index = text.find('{');
    if (index == std::string_view::npos) return {};
    ++index;
    while (index < text.size()) {
        index = skip_ws(text, index);
        if (index >= text.size() || text[index] == '}') return {};
        if (text[index] != '"') return {};
        std::string_view name;
        index = skip_ws(text, read_string(text, index, name));
        if (index >= text.size() || text[index] != ':') return {};
        index = skip_ws(text, index + 1);
        const std::size_t end = scan_value_end(text, index);
        if (name == key) return text.substr(index, end - index);
        index = skip_ws(text, end);
        if (index < text.size() && text[index] == ',') {
            ++index;
            continue;
        }
        return {};
    }
    return {};
}

/** @brief Read a boolean member; false when the member is absent or not a boolean. */
bool json_bool(std::string_view text, std::string_view key, bool& out) noexcept
{
    const std::string_view raw = json_value(text, key);
    if (raw == "true") {
        out = true;
        return true;
    }
    if (raw == "false") {
        out = false;
        return true;
    }
    return false;
}

/**
 * @brief Read a non-negative counter member, accepting a number or an array of items.
 *
 * @param text Whole JSON object text.
 * @param key Member name without quotes.
 * @param out Parsed number, or the number of top-level array items.
 * @return true when the member exists and has a supported shape.
 */
bool json_count(std::string_view text, std::string_view key, std::uint64_t& out) noexcept
{
    const std::string_view raw = json_value(text, key);
    if (raw.empty()) return false;
    if (raw.front() == '[') {
        if (raw.back() != ']') return false;
        std::uint64_t items = 0;
        bool has_content = false;
        int depth = 0;
        for (std::size_t index = 1; index + 1 < raw.size(); ++index) {
            const char current = raw[index];
            if (current == '"') {
                std::string_view ignored;
                index = read_string(raw, index, ignored) - 1;
                has_content = true;
                continue;
            }
            if (current == '{' || current == '[') {
                ++depth;
                has_content = true;
            } else if (current == '}' || current == ']') {
                --depth;
            } else if (current == ',' && depth == 0) {
                ++items;
            } else if (current != ' ' && current != '\t' && current != '\n' && current != '\r') {
                has_content = true;
            }
        }
        out = has_content ? items + 1 : 0;
        return true;
    }
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    if (parsed.ec != std::errc() || parsed.ptr != raw.data() + raw.size()) return false;
    out = value;
    return true;
}

/** @brief Decoded consumer status fields required by the example contract. */
struct consumer_view {
    bool connected = false;
    std::uint64_t revocations = 0;
    std::uint64_t events = 0;
};

/**
 * @brief Parse the consumer status payload and require all documented fields.
 *
 * @param json Raw method output.
 * @return Decoded connected/revocations/events fields.
 */
consumer_view parse_consumer_status(const std::string& json)
{
    consumer_view view;
    const bool parsed_connected = json_bool(json, "connected", view.connected);
    const bool parsed_revocations = json_count(json, "revocations", view.revocations);
    const bool parsed_events = json_count(json, "events", view.events);
    if (!parsed_connected || !parsed_revocations || !parsed_events) {
        failf(__FILE__, __LINE__,
              "consumer status JSON lacks connected/revocations/events: '%s'", json.c_str());
    }
    return view;
}

/**
 * @brief Invoke the consumer's status method through one lookup path.
 *
 * @param rack Started host holding the consumer.
 * @param by_id true to resolve the method by numeric ID, false to use its name.
 * @param label Operation label used in diagnostics.
 * @return Decoded status payload.
 */
consumer_view read_consumer_status(host& rack, bool by_id, const char* label)
{
    std::string out;
    // The status method takes no mandatory argument; an empty object is accepted as the
    // fallback spelling so the plugin may validate the JSON argument either way.
    abi::status called = by_id ? rack.call(kConsumerPlugId, kStatusMethodId, abi::bytes{}, &out)
                               : rack.call(kConsumerPlugId, kStatusMethod, abi::bytes{}, &out);
    if (called != abi::ok) {
        out.clear();
        called = by_id ? rack.call(kConsumerPlugId, kStatusMethodId, bytes_of(std::string("{}")), &out)
                       : rack.call(kConsumerPlugId, kStatusMethod, bytes_of(std::string("{}")), &out);
    }
    expect_ok(called, label);
    return parse_consumer_status(out);
}

/**
 * @brief Poll the host until the consumer reports the expected connection state.
 *
 * @param rack Started host.
 * @param expected_connected Required connected flag.
 * @param rounds Maximum number of poll rounds before the final requirement.
 * @param label Operation label used in diagnostics.
 * @return Last observed consumer status.
 */
consumer_view pump_until_connected(host& rack, bool expected_connected, int rounds, const char* label)
{
    consumer_view view = read_consumer_status(rack, false, label);
    for (int round = 0; round < rounds && view.connected != expected_connected; ++round) {
        expect_ok(rack.poll(), "poll()");
        view = read_consumer_status(rack, false, label);
    }
    if (view.connected != expected_connected) {
        failf(__FILE__, __LINE__, "%s: consumer connected=%s, expected %s", label,
              view.connected ? "true" : "false", expected_connected ? "true" : "false");
    }
    return view;
}

// ---------------------------------------------------------------------------
// Native loader probes for the negative fixtures.
// ---------------------------------------------------------------------------

/** @brief Map one library directly, without the u42::plug wrapper. */
void* map_native(const fs::path& path)
{
#if defined(_WIN32)
    return reinterpret_cast<void*>(::LoadLibraryW(path.c_str()));
#else
    return ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

/** @brief Release a mapping returned by map_native(). */
void unmap_native(void* handle) noexcept
{
#if defined(_WIN32)
    if (handle != nullptr) (void)::FreeLibrary(static_cast<HMODULE>(handle));
#else
    if (handle != nullptr) (void)::dlclose(handle);
#endif
}

/** @brief Resolve a native symbol, or return null when it is absent. */
void* native_symbol(void* handle, const char* name)
{
#if defined(_WIN32)
    return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(handle), name));
#else
    ::dlerror(); // Clear stale state so the next diagnostic belongs to this lookup.
    void* symbol = ::dlsym(handle, name);
    return ::dlerror() != nullptr ? nullptr : symbol;
#endif
}

/**
 * @brief Require both negative fixtures to fail inside the loader itself.
 *
 * @param bad_abi_library Fixture that rejects the negotiated ABI major.
 * @param missing_entry_library Fixture that exports no factory entry.
 */
void loader_negative_checks(const fs::path& bad_abi_library, const fs::path& missing_entry_library)
{
    phase_guard guard("loader negatives");

    if (!fs::exists(bad_abi_library)) {
        failf(__FILE__, __LINE__, "bad-ABI fixture is missing: '%s'", bad_abi_library.string().c_str());
    }
    if (!fs::exists(missing_entry_library)) {
        failf(__FILE__, __LINE__, "missing-entry fixture is missing: '%s'",
              missing_entry_library.string().c_str());
    }

    {
        // Direct probe: the entry must reject the major and clear a poisoned output slot.
        void* handle = map_native(bad_abi_library);
        if (handle == nullptr) {
            failf(__FILE__, __LINE__, "cannot map bad-ABI fixture '%s'", bad_abi_library.string().c_str());
        }
        const auto entry = reinterpret_cast<abi::entry_fn>(native_symbol(handle, abi::entry_name));
        CHECK(entry != nullptr);
        abi::iplug_fty* poison = reinterpret_cast<abi::iplug_fty*>(static_cast<std::uintptr_t>(1));
        const abi::status negotiated = entry(abi::abi_major, &poison);
        expect_status(negotiated, abi::unsupported, "bad-ABI entry(abi_major)");
        CHECK(poison == nullptr);
        unmap_native(handle);
    }
    {
        // Direct probe: the library maps, carries its unrelated symbol, but no entry.
        void* handle = map_native(missing_entry_library);
        if (handle == nullptr) {
            failf(__FILE__, __LINE__, "cannot map missing-entry fixture '%s'",
                  missing_entry_library.string().c_str());
        }
        CHECK(native_symbol(handle, abi::entry_name) == nullptr);
        CHECK(native_symbol(handle, kFixtureDummySymbol) != nullptr);
        unmap_native(handle);
    }
    {
        plug library;
        std::string error;
        expect_status(library.open(bad_abi_library, error), abi::unsupported, "plug::open(bad ABI)");
        CHECK(!error.empty());
        CHECK(library.factory() == nullptr);
        CHECK(library.path().empty());
    }
    {
        plug library;
        std::string error;
        expect_status(library.open(missing_entry_library, error), abi::failed, "plug::open(missing entry)");
        CHECK(!error.empty());
        CHECK(error.find(abi::entry_name) != std::string::npos);
        CHECK(library.factory() == nullptr);
        CHECK(library.path().empty());
    }
}

// ---------------------------------------------------------------------------
// Directory and identity negatives.
// ---------------------------------------------------------------------------

/** @brief Require scan/boot to reject empty and absent directories and to stay clean. */
void directory_negative_checks()
{
    phase_guard guard("directory negatives");

    {
        temp_directory empty("empty");
        std::vector<fs::path> out{fs::path("pre-existing-candidate")};
        std::string error = "stale diagnostic";
        expect_ok(u42::scan_plugins(empty.path(), out, error), "scan_plugins(empty directory)");
        CHECK(out.empty());
        CHECK(error.empty());

        host rack;
        expect_ok(rack.boot(empty.path()), "boot(empty directory)");
        CHECK(rack.plugins().empty());
        expect_ok(rack.shutdown(), "shutdown() after empty boot");
        empty.remove_and_verify();
    }
    {
        const fs::path missing = fs::temp_directory_path() /
                                 fs::path("u42_it_missing_" + unique_token());
        std::error_code ec;
        fs::remove_all(missing, ec);
        CHECK(!fs::exists(missing));

        std::vector<fs::path> out{fs::path("pre-existing-candidate")};
        std::string error = "stale diagnostic";
        expect_status(u42::scan_plugins(missing, out, error), abi::not_found,
                      "scan_plugins(missing directory)");
        CHECK(out.empty());
        CHECK(!error.empty());

        host rack;
        CHECK(rack.boot(missing) != abi::ok);
        CHECK(!rack.error().empty());
        CHECK(rack.plugins().empty());
        expect_ok(rack.shutdown(), "shutdown() after failed boot");

        plug library;
        std::string open_error;
        expect_status(library.open(missing, open_error), abi::not_found, "plug::open(missing path)");
        CHECK(!open_error.empty());
        CHECK(library.factory() == nullptr);
    }
}

/**
 * @brief Require boot to reject two libraries that declare the same plugin identity.
 *
 * @param echo_library Source library copied twice into a private directory.
 */
void duplicate_identity_checks(const fs::path& echo_library)
{
    phase_guard guard("duplicate identity");

    const std::string name = echo_library.filename().string();
    const std::size_t marker = name.find(".u42.");
    if (marker == std::string::npos) {
        failf(__FILE__, __LINE__, "echo library '%s' does not carry the .u42 suffix", name.c_str());
    }
    const std::string suffix = name.substr(marker);

    temp_directory directory("duplicate");
    const fs::path first = directory.path() / (std::string("dup_a") + suffix);
    const fs::path second = directory.path() / (std::string("dup_b") + suffix);
    std::error_code ec;
    fs::copy_file(echo_library, first, fs::copy_options::none, ec);
    if (ec) {
        failf(__FILE__, __LINE__, "cannot copy '%s' to '%s': %s", echo_library.string().c_str(),
              first.string().c_str(), ec.message().c_str());
    }
    ec.clear();
    fs::copy_file(echo_library, second, fs::copy_options::none, ec);
    if (ec) {
        failf(__FILE__, __LINE__, "cannot copy '%s' to '%s': %s", echo_library.string().c_str(),
              second.string().c_str(), ec.message().c_str());
    }

    // Sanity: the staged directory really presents two distinct candidates.
    std::vector<fs::path> candidates;
    std::string scan_error;
    expect_ok(u42::scan_plugins(directory.path(), candidates, scan_error), "scan_plugins(duplicate dir)");
    CHECK(candidates.size() == 2);

    host rack;
    const abi::status booted = rack.boot(directory.path());
    if (booted == abi::ok) {
        failf(__FILE__, __LINE__, "boot accepted two libraries declaring plug id '%s'", kEchoPlugId);
    }
    if (booted != abi::duplicate && booted != abi::failed) {
        failf(__FILE__, __LINE__, "boot duplicate rejection returned %s(%u)", status_name(booted),
              static_cast<unsigned>(booted));
    }
    CHECK(!rack.error().empty());
    CHECK(rack.plugins().empty());
    expect_ok(rack.shutdown(), "shutdown() after duplicate rejection");
    directory.remove_and_verify();
}

/** @brief Plugin DSO paths resolved from the boot directory. */
struct plugin_paths {
    fs::path echo;
    fs::path consumer;
};

/**
 * @brief Identify the echo and consumer DSOs by loading each candidate and reading its description.
 *
 * @param directory Directory passed as the first command-line argument.
 * @return Canonical library paths for both example plugins.
 */
plugin_paths discover_plugins(const fs::path& directory)
{
    phase_guard guard("discover plugins");

    std::vector<fs::path> candidates;
    std::string error;
    expect_ok(u42::scan_plugins(directory, candidates, error), "scan_plugins(plugins_dir)");
    CHECK(!candidates.empty());

    plugin_paths found;
    for (const fs::path& candidate : candidates) {
        plug library;
        std::string open_error;
        if (library.open(candidate, open_error) != abi::ok) {
            std::fprintf(stderr, "integration_test: skipping unloadable candidate '%s': %s\n",
                         candidate.string().c_str(), open_error.c_str());
            continue;
        }
        const abi::plug_desc* description = nullptr;
        if (library.factory()->describe(&description) != abi::ok || description == nullptr) {
            std::fprintf(stderr, "integration_test: skipping candidate without description '%s'\n",
                         candidate.string().c_str());
            continue;
        }
        const std::string plug_id = description->plug_id != nullptr ? description->plug_id : "";
        if (plug_id == kEchoPlugId) found.echo = candidate;
        else if (plug_id == kConsumerPlugId) found.consumer = candidate;
    }

    if (found.echo.empty()) {
        failf(__FILE__, __LINE__, "no candidate in '%s' describes plug id '%s'", directory.string().c_str(),
              kEchoPlugId);
    }
    if (found.consumer.empty()) {
        failf(__FILE__, __LINE__, "no candidate in '%s' describes plug id '%s'",
              directory.string().c_str(), kConsumerPlugId);
    }
    return found;
}

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------

/**
 * @brief Exercise boot, name/ID parity, unload, reload, stale bindings and shutdown.
 *
 * @param plugins_dir Directory holding both example plugins.
 * @param paths Resolved plugin library paths.
 */
void integration_lifecycle(const fs::path& plugins_dir, const plugin_paths& paths)
{
    phase_guard guard("lifecycle");

    host rack;
    expect_ok(rack.boot(plugins_dir), "boot(plugins_dir)");
    const std::vector<std::string> ids = rack.plugins();
    CHECK(has_plugin(ids, kEchoPlugId));
    CHECK(has_plugin(ids, kConsumerPlugId));

    const std::string payload = kEchoPayload;
    std::string echoed_by_name;
    std::string echoed_by_id;
    expect_ok(rack.call(kEchoPlugId, kEchoMethod, bytes_of(payload), &echoed_by_name),
              "call(echo, name)");
    expect_ok(rack.call(kEchoPlugId, kEchoMethodId, bytes_of(payload), &echoed_by_id),
              "call(echo, id)");
    CHECK(echoed_by_name == payload);
    CHECK(echoed_by_id == payload);

    const consumer_view booted = read_consumer_status(rack, false, "consumer status after boot");
    const consumer_view booted_by_id = read_consumer_status(rack, true, "consumer status by id after boot");
    CHECK(booted.connected);
    CHECK(booted_by_id.connected == booted.connected);
    CHECK(booted_by_id.revocations == booted.revocations);
    CHECK(booted_by_id.events == booted.events);

    abi::binding old_binding{};
    expect_ok(rack.bind(kEchoPlugId, kEchoMethod, &old_binding), "bind(echo, name)");
    CHECK(old_binding.value != 0);
    std::string live_output;
    expect_ok(rack.call(old_binding, bytes_of(payload), &live_output), "call(old binding)");
    CHECK(live_output == payload);

    expect_ok(rack.unload(kEchoPlugId), "unload(echo)");
    CHECK(!has_plugin(rack.plugins(), kEchoPlugId));
    const consumer_view parked = pump_until_connected(rack, false, 8, "consumer status after unload");
    CHECK(parked.revocations == 1);

    std::string stale_output = "must be cleared";
    expect_status(rack.call(old_binding, bytes_of(payload), &stale_output), abi::stale,
                  "call(old binding) after unload");
    CHECK(stale_output.empty());
    stale_output = "must be cleared";
    CHECK(rack.call(kEchoPlugId, kEchoMethod, bytes_of(payload), &stale_output) != abi::ok);

    expect_ok(rack.load(paths.echo), "load(echo)");
    CHECK(has_plugin(rack.plugins(), kEchoPlugId));
    const consumer_view resumed = pump_until_connected(rack, true, 8, "consumer status after reload");
    CHECK(resumed.revocations >= parked.revocations);

    stale_output = "must be cleared";
    expect_status(rack.call(old_binding, bytes_of(payload), &stale_output), abi::stale,
                  "call(old binding) after reload");
    CHECK(stale_output.empty());

    abi::binding new_binding{};
    expect_ok(rack.bind(kEchoPlugId, kEchoMethodId, &new_binding), "bind(echo, id) after reload");
    CHECK(new_binding.value != 0);
    std::string rebound_output;
    expect_ok(rack.call(new_binding, bytes_of(payload), &rebound_output), "call(new binding)");
    CHECK(rebound_output == payload);
    expect_ok(rack.unbind(new_binding), "unbind(new binding)");
    CHECK(rack.call(new_binding, bytes_of(payload), &rebound_output) != abi::ok);

    expect_ok(rack.shutdown(), "shutdown()");
    CHECK(rack.plugins().empty());
    std::string shutdown_output = "must be cleared";
    CHECK(rack.call(kEchoPlugId, kEchoMethod, bytes_of(payload), &shutdown_output) != abi::ok);
    CHECK(shutdown_output.empty());

    // Once the rack is shut down, the host rejects business calls uniformly before it
    // re-examines individual bindings, so this single call accepts either rejection. The
    // pre-shutdown unload/reload checks above stay strict: only stale is acceptable there.
    shutdown_output = "must be cleared";
    const abi::status after_shutdown = rack.call(old_binding, bytes_of(payload), &shutdown_output);
    if (after_shutdown != abi::stale && after_shutdown != abi::invalid_state) {
        failf(__FILE__, __LINE__,
              "call(old binding) after shutdown: expected stale(%u) or invalid_state(%u) but "
              "got %s(%u)",
              static_cast<unsigned>(abi::stale), static_cast<unsigned>(abi::invalid_state),
              status_name(after_shutdown), static_cast<unsigned>(after_shutdown));
    }
    CHECK(shutdown_output.empty());
}

/**
 * @brief Count mapping table entries whose line contains a substring.
 *
 * @param needle Substring expected inside mapping paths.
 * @param count Number of matching entries.
 * @return false when no process mapping table is available, so the probe is skipped.
 */
bool count_mapped(const std::string& needle, std::size_t& count)
{
#if defined(__linux__)
    std::FILE* table = std::fopen("/proc/self/maps", "r");
    if (table == nullptr) return false;
    char line[4096];
    count = 0;
    while (std::fgets(line, sizeof(line), table) != nullptr) {
        if (std::strstr(line, needle.c_str()) != nullptr) ++count;
    }
    std::fclose(table);
    return true;
#else
    (void)needle;
    (void)count;
    return false;
#endif
}

/**
 * @brief Repeat the full boot/unload/reload/shutdown cycle and watch for resource growth.
 *
 * @param plugins_dir Directory holding both example plugins.
 * @param paths Resolved plugin library paths.
 * @param rounds Number of complete cycles to run.
 */
void multi_round_checks(const fs::path& plugins_dir, const plugin_paths& paths, int rounds)
{
    phase_guard guard("multi round");

    const std::string needle = paths.echo.filename().string();
    std::size_t warm_mappings = 0;
    bool mapping_probe = false;

    for (int round = 0; round < rounds; ++round) {
        host rack;
        expect_ok(rack.boot(plugins_dir), "boot(plugins_dir) in round");
        CHECK(has_plugin(rack.plugins(), kEchoPlugId));
        const consumer_view live = pump_until_connected(rack, true, 8, "consumer status in round");
        CHECK(live.connected);

        expect_ok(rack.unload(kEchoPlugId), "unload(echo) in round");
        const consumer_view parked = pump_until_connected(rack, false, 8, "consumer status after unload in round");
        CHECK(parked.revocations == 1);

        expect_ok(rack.load(paths.echo), "load(echo) in round");
        const consumer_view resumed = pump_until_connected(rack, true, 8, "consumer status after reload in round");
        CHECK(resumed.connected);

        expect_ok(rack.shutdown(), "shutdown() in round");
        CHECK(rack.plugins().empty());

        // Baseline after the first complete cycle: later rounds must not add mappings.
        if (round == 0) mapping_probe = count_mapped(needle, warm_mappings);
    }

    std::size_t final_mappings = 0;
    if (mapping_probe && count_mapped(needle, final_mappings)) {
        // Mappings that keep piling up, round after round, indicate a loader leak.
        if (final_mappings > warm_mappings) {
            failf(__FILE__, __LINE__, "echo mappings grew from %zu to %zu over %d rounds",
                  warm_mappings, final_mappings, rounds);
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc < 4) {
            std::fprintf(stderr,
                         "usage: %s <plugins_dir> <bad_abi_library> <missing_entry_library>\n",
                         argv[0]);
            return EXIT_FAILURE;
        }

        const fs::path plugins_dir = argv[1];
        loader_negative_checks(fs::path(argv[2]), fs::path(argv[3]));
        const plugin_paths paths = discover_plugins(plugins_dir);
        directory_negative_checks();
        duplicate_identity_checks(paths.echo);
        integration_lifecycle(plugins_dir, paths);
        multi_round_checks(plugins_dir, paths, 3);

        std::fprintf(stdout, "integration_test: all checks passed\n");
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "integration_test: unexpected exception: %s\n", error.what());
    } catch (...) {
        std::fprintf(stderr, "integration_test: unexpected non-standard exception\n");
    }
    return EXIT_FAILURE;
}
