/**
 * @file cli_runtime_test.cc
 * @brief Runtime configuration validation and allocation-safe adopt rollback tests.
 *
 * Build this source as three shared fixtures with U42_CLI_RUNTIME_PRIMARY_FIXTURE,
 * U42_CLI_RUNTIME_SECONDARY_FIXTURE, and U42_CLI_RUNTIME_CONFLICT_FIXTURE, then build it once
 * without a fixture macro as the test executable. The executable expects those three library
 * paths in that order.
 */

#include <42u/abi.hpp>
#include <42u/cli.hpp>

#include <cstdint>

namespace cli_runtime_contract {

namespace abi = u42::abi::v3;
namespace cli = u42::cli::v1;

using allocation_hook = void (*)() noexcept;

/** @brief Convert one string literal to a borrowed ABI text view. */
template <std::size_t Size>
constexpr cli::text_view text(const char (&value)[Size]) noexcept
{
    return {value, Size - 1};
}

} // namespace cli_runtime_contract

#if defined(U42_CLI_RUNTIME_PRIMARY_FIXTURE) ||                                      \
    defined(U42_CLI_RUNTIME_SECONDARY_FIXTURE) ||                                    \
    defined(U42_CLI_RUNTIME_CONFLICT_FIXTURE)

#include <new>

namespace {

namespace abi = u42::abi::v3;
namespace cli = u42::cli::v1;
using cli_runtime_contract::text;

#if defined(U42_CLI_RUNTIME_PRIMARY_FIXTURE)
constexpr char plugin_id[] =
    "com.example.cli.runtime.primary-with-a-long-identity-for-allocation-tracking";
#elif defined(U42_CLI_RUNTIME_SECONDARY_FIXTURE)
constexpr char plugin_id[] =
    "com.example.cli.runtime.secondary-with-a-long-identity-for-allocation-tracking";
#else
constexpr char plugin_id[] = "com.example.cli.runtime.conflict";
#endif

const abi::plug_desc description{sizeof(abi::plug_desc), 0u, plugin_id, {1u, 0u, 0u},
                                 0, 0u, nullptr, 0u, nullptr};

int create_count = 0;
int destroy_count = 0;
cli_runtime_contract::allocation_hook after_create = nullptr;

/** @brief Minimal silent fixture instance. */
class fixture_plugin final : public abi::iplug {
public:
    /** @brief Accept the host context without starting work. */
    abi::status U42_CALL init(abi::ictx* context) noexcept override
    {
        return context == nullptr ? abi::invalid_argument : abi::ok;
    }
    /** @brief Start no work. */
    abi::status U42_CALL start() noexcept override { return abi::ok; }
    /** @brief Stop no work. */
    abi::status U42_CALL stop() noexcept override { return abi::ok; }
    /** @brief Record destruction and release on the allocating side. */
    void U42_CALL destroy() noexcept override
    {
        ++destroy_count;
        delete this;
    }
    /** @brief Publish no optional plugin interface. */
    abi::status U42_CALL query(const abi::iid* type, void** out) noexcept override
    {
        if (out == nullptr) return abi::invalid_argument;
        *out = nullptr;
        return type == nullptr ? abi::invalid_argument : abi::unsupported;
    }
};

/** @brief Factory whose primary variant can arm allocation failure after create succeeds. */
class fixture_factory final : public abi::iplug_fty {
public:
    /** @brief Return immutable fixture metadata. */
    abi::status U42_CALL describe(const abi::plug_desc** out) noexcept override
    {
        if (out == nullptr) return abi::invalid_argument;
        *out = &description;
        return abi::ok;
    }
    /** @brief Create one primary/conflict instance; the secondary fixture fails deliberately. */
    abi::status U42_CALL create(abi::iplug** out) noexcept override
    {
        if (out == nullptr) return abi::invalid_argument;
        *out = nullptr;
#if defined(U42_CLI_RUNTIME_SECONDARY_FIXTURE)
        return abi::failed;
#else
        *out = new (std::nothrow) fixture_plugin();
        if (*out == nullptr) return abi::failed;
        ++create_count;
        if (after_create != nullptr) after_create();
        return abi::ok;
#endif
    }
};

fixture_factory factory;

#if defined(U42_CLI_RUNTIME_PRIMARY_FIXTURE)

constexpr cli::text_view single_defaults[]{text("default")};
constexpr cli::text_view single_allowed[]{text("default"), text("override")};
constexpr cli::text_view repeat_defaults[]{text("a"), text("b")};
constexpr cli::text_view repeat_allowed[]{text("a"), text("b")};
constexpr cli::text_view shared_a_defaults[]{text("x")};
constexpr cli::text_view shared_a_allowed[]{text("x")};
constexpr cli::text_view shared_b_defaults[]{text("y"), text("z")};
constexpr cli::text_view shared_b_allowed[]{text("y"), text("z")};

const cli::parameter_desc root_parameters[]{
    {sizeof(cli::parameter_desc), 0u, 0u, cli::text, cli::no_position,
     text("single"), text("single"), {}, text("VALUE"), {},
     {single_defaults, 1u}, {single_allowed, 2u}},
    {sizeof(cli::parameter_desc), 0u, cli::parameter_repeatable, cli::text,
     cli::no_position, text("repeat"), text("repeat"), {}, text("VALUE"), {},
     {repeat_defaults, 2u}, {repeat_allowed, 2u}},
    {sizeof(cli::parameter_desc), 0u, 0u, cli::text, cli::no_position,
     text("free-text"), text("free-text"), {}, text("VALUE"), {}, {}, {}},
};

const cli::parameter_desc shared_a[]{
    {sizeof(cli::parameter_desc), 0u, 0u, cli::text, cli::no_position,
     text("shared-a"), text("shared-a"), {}, text("VALUE"), text("shared"),
     {shared_a_defaults, 1u}, {shared_a_allowed, 1u}},
};
const cli::parameter_desc shared_b[]{
    {sizeof(cli::parameter_desc), 0u, cli::parameter_repeatable, cli::text,
     cli::no_position, text("shared-b"), text("shared-b"), {}, text("VALUE"),
     text("shared"), {shared_b_defaults, 2u}, {shared_b_allowed, 2u}},
};
const cli::command_desc commands[]{
    {sizeof(cli::command_desc), 0u, 0u, 1u, 0u, text("first"), text("first"), {},
     {shared_a, 1u}},
    {sizeof(cli::command_desc), 0u, 0u, 2u, 0u, text("second"), text("second"), {},
     {shared_b, 1u}},
};
const cli::manifest manifest{sizeof(cli::manifest), 0u, 0u,
                             {root_parameters, 3u}, {commands, 2u}};

#elif defined(U42_CLI_RUNTIME_CONFLICT_FIXTURE)

const cli::parameter_desc text_shared[]{
    {sizeof(cli::parameter_desc), 0u, 0u, cli::text, cli::no_position,
     text("text-shared"), text("text-shared"), {}, text("VALUE"), text("shared"),
     {}, {}},
};
const cli::parameter_desc flag_shared[]{
    {sizeof(cli::parameter_desc), 0u, 0u, cli::flag, cli::no_position,
     text("flag-shared"), text("flag-shared"), {}, {}, text("shared"), {}, {}},
};
const cli::command_desc commands[]{
    {sizeof(cli::command_desc), 0u, 0u, 1u, 0u, text("first"), text("first"), {},
     {text_shared, 1u}},
    {sizeof(cli::command_desc), 0u, 0u, 2u, 0u, text("second"), text("second"), {},
     {flag_shared, 1u}},
};
const cli::manifest manifest{sizeof(cli::manifest), 0u, 0u, {}, {commands, 2u}};

#endif

} // namespace

/** @brief Return the fixture factory for ABI v3. */
extern "C" U42_EXPORT abi::status U42_CALL u42_get_factory(std::uint32_t major,
                                                            abi::iplug_fty** out) noexcept
{
    if (out == nullptr) return abi::invalid_argument;
    *out = nullptr;
    if (major != abi::abi_major) return abi::unsupported;
    *out = &factory;
    return abi::ok;
}

#if defined(U42_CLI_RUNTIME_PRIMARY_FIXTURE) || defined(U42_CLI_RUNTIME_CONFLICT_FIXTURE)
/** @brief Return the fixture CLI manifest. */
extern "C" U42_EXPORT abi::status U42_CALL u42_get_cli_manifest(
    std::uint32_t major, const cli::manifest** out) noexcept
{
    if (out == nullptr) return abi::invalid_argument;
    *out = nullptr;
    if (major != cli::manifest_major) return abi::unsupported;
    *out = &manifest;
    return abi::ok;
}
#endif

/** @brief Return the number of successfully allocated fixture instances. */
extern "C" U42_EXPORT int u42_cli_runtime_create_count() noexcept { return create_count; }

/** @brief Return the number of fixture instances destroyed by the host. */
extern "C" U42_EXPORT int u42_cli_runtime_destroy_count() noexcept { return destroy_count; }

/** @brief Install a test callback invoked after a successful create. */
extern "C" U42_EXPORT void u42_cli_runtime_set_after_create(
    cli_runtime_contract::allocation_hook hook) noexcept
{
    after_create = hook;
}

#else

#define NDEBUG 1

#include <42u/host.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace abi = u42::abi::v3;
namespace cli = u42::cli::v1;

std::atomic<long> new_calls{0};
std::atomic<long> fail_at{-1};
const char* phase = "startup";

/** @brief Count one allocation and fail exactly once at the armed absolute index. */
void note_allocation()
{
    const long index = new_calls.fetch_add(1, std::memory_order_relaxed);
    long expected = index;
    if (fail_at.compare_exchange_strong(expected, -1, std::memory_order_relaxed)) {
        throw std::bad_alloc();
    }
}

/** @brief Arm the next allocation after a successful fixture create. */
void arm_next_allocation() noexcept
{
    fail_at.store(new_calls.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

/** @brief Disable allocation failure before assertions and teardown. */
void disarm() noexcept { fail_at.store(-1, std::memory_order_relaxed); }

/** @brief Report one failed assertion with its active phase. */
[[noreturn]] void fail_check(const char* expression, const char* file, int line)
{
    disarm();
    std::fprintf(stderr, "CHECK failed [%s]: %s (%s:%d)\n", phase, expression, file, line);
    std::exit(EXIT_FAILURE);
}

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) fail_check(#condition, __FILE__, __LINE__);           \
    } while (false)

/** @brief Temporarily identify one test case in diagnostics. */
class phase_guard {
public:
    /** @brief Install one test name. */
    explicit phase_guard(const char* value) noexcept : previous_(phase) { phase = value; }
    /** @brief Restore the enclosing test name. */
    ~phase_guard() { phase = previous_; }

private:
    const char* previous_;
};

/** @brief Dynamic fixture probes retained while discovery mappings are transferred. */
class fixture_probe {
public:
    using count_fn = int (*)() noexcept;
    using hook_fn = void (*)(cli_runtime_contract::allocation_hook) noexcept;

    /** @brief Open one fixture and resolve its test-only counters/hooks. */
    explicit fixture_probe(const char* path) : handle_(::dlopen(path, RTLD_NOW | RTLD_LOCAL))
    {
        CHECK(handle_ != nullptr);
        creates = reinterpret_cast<count_fn>(::dlsym(handle_, "u42_cli_runtime_create_count"));
        destroys = reinterpret_cast<count_fn>(::dlsym(handle_, "u42_cli_runtime_destroy_count"));
        set_hook = reinterpret_cast<hook_fn>(
            ::dlsym(handle_, "u42_cli_runtime_set_after_create"));
        CHECK(creates != nullptr);
        CHECK(destroys != nullptr);
        CHECK(set_hook != nullptr);
    }
    /** @brief Release the independent probe mapping. */
    ~fixture_probe() { (void)::dlclose(handle_); }

    count_fn creates = nullptr;
    count_fn destroys = nullptr;
    hook_fn set_hook = nullptr;

private:
    void* handle_ = nullptr;
};

/** @brief Discover one plugin and attach the supplied snapshot values. */
u42::plugin_activation activation(const char* path,
                                  std::vector<u42::plugin_config_value> values = {})
{
    u42::discovered_plugin plugin;
    std::string error;
    CHECK(plugin.open(path, error) == abi::ok);
    return {std::move(plugin), std::move(values)};
}

/** @brief Expect adopt validation to fail without creating an instance. */
void expect_rejected(const char* path, fixture_probe& probe,
                     std::vector<u42::plugin_config_value> values,
                     abi::status expected, const char* diagnostic)
{
    const int creates_before = probe.creates();
    u42::host host;
    std::vector<u42::plugin_activation> batch;
    batch.push_back(activation(path, std::move(values)));
    CHECK(host.adopt(std::move(batch)) == expected);
    CHECK(host.error().find(diagnostic) != std::string::npos);
    CHECK(probe.creates() == creates_before);
    CHECK(host.plugins().empty());
}

/** @brief Expect one snapshot to create and then destroy exactly one instance. */
void expect_accepted(const char* path, fixture_probe& probe,
                     std::vector<u42::plugin_config_value> values)
{
    const int creates_before = probe.creates();
    const int destroys_before = probe.destroys();
    u42::host host;
    std::vector<u42::plugin_activation> batch;
    batch.push_back(activation(path, std::move(values)));
    CHECK(host.adopt(std::move(batch)) == abi::ok);
    CHECK(probe.creates() == creates_before + 1);
    CHECK(host.shutdown() == abi::ok);
    CHECK(probe.destroys() == destroys_before + 1);
}

/** @brief Validate UTF-8, cardinality, allowed-value and source/default contracts. */
void test_config_contracts(const char* primary, fixture_probe& probe)
{
    phase_guard guard("config-contracts");
    expect_accepted(primary, probe, {});
    expect_accepted(primary, probe,
                    {{"single", cli::text, cli::plugin_default, 0, {"default"}}});
    expect_accepted(primary, probe,
                    {{"single", cli::text, cli::command_line, 0, {"override"}}});
    expect_accepted(primary, probe,
                    {{"shared", cli::text, cli::command_line, 0, {"y", "z"}}});
    expect_accepted(primary, probe,
                    {{"shared", cli::text, cli::plugin_default, 0, {"y", "z"}}});

    expect_rejected(primary, probe,
                    {{"single", cli::text, cli::command_line, 0, {"default", "override"}}},
                    abi::invalid_argument, "not repeatable");
    expect_rejected(primary, probe,
                    {{"single", cli::text, cli::command_line, 0, {"outside"}}},
                    abi::invalid_argument, "allowed values");
    expect_rejected(primary, probe,
                    {{"single", cli::text, cli::plugin_default, 0, {"override"}}},
                    abi::invalid_argument, "plugin default");
    expect_rejected(primary, probe,
                    {{"free-text", cli::text, cli::command_line, 0,
                      {std::string("\xc0\xaf", 2)}}},
                    abi::invalid_argument, "valid UTF-8");
    expect_rejected(primary, probe,
                    {{"single", cli::text, cli::command_line, 0, {"default"}},
                     {"single", cli::text, cli::command_line, 0, {"override"}}},
                    abi::duplicate, "repeats configuration key");
}

/** @brief Empty snapshots still reject incompatible cross-command config-key declarations. */
void test_empty_snapshot_manifest_validation(const char* conflict, fixture_probe& probe)
{
    phase_guard guard("empty-snapshot-manifest-validation");
    expect_rejected(conflict, probe, {}, abi::invalid_argument,
                    "incompatible kind or sensitivity");
}

/** @brief Fail the first allocation after create and prove the created record is rolled back. */
void test_post_create_oom_rollback(const char* primary, const char* secondary,
                                   fixture_probe& first, fixture_probe& second)
{
    phase_guard guard("post-create-oom-rollback");
    const int creates_before = first.creates();
    const int destroys_before = first.destroys();
    const int second_creates_before = second.creates();

    u42::host host;
    std::vector<u42::plugin_activation> batch;
    batch.reserve(2);
    batch.push_back(activation(primary));
    batch.push_back(activation(secondary));
    first.set_hook(&arm_next_allocation);
    const abi::status outcome = host.adopt(std::move(batch));
    first.set_hook(nullptr);
    disarm();

    CHECK(outcome == abi::failed);
    CHECK(first.creates() == creates_before + 1);
    CHECK(first.destroys() == destroys_before + 1);
    CHECK(second.creates() == second_creates_before);
    CHECK(host.plugins().empty());
    CHECK(host.shutdown() == abi::ok);
}

} // namespace

// Complete replacement allocation set for deterministic one-shot fault injection.
void* operator new(std::size_t size)
{
    note_allocation();
    void* memory = std::malloc(size == 0 ? 1 : size);
    if (memory == nullptr) throw std::bad_alloc();
    return memory;
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    try { return ::operator new(size); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, const std::nothrow_t& value) noexcept
{
    return ::operator new(size, value);
}
void* operator new(std::size_t size, std::align_val_t alignment)
{
    note_allocation();
    const std::size_t align = static_cast<std::size_t>(alignment);
    void* memory = nullptr;
    if (::posix_memalign(&memory, align < sizeof(void*) ? sizeof(void*) : align,
                         size == 0 ? 1 : size) != 0)
        throw std::bad_alloc();
    return memory;
}
void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return ::operator new(size, alignment);
}
void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept
{
    try { return ::operator new(size, alignment); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept
{
    try { return ::operator new(size, alignment); } catch (...) { return nullptr; }
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
void operator delete[](void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
void operator delete(void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept
{
    std::free(memory);
}
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept
{
    std::free(memory);
}

/** @brief Execute all runtime configuration and rollback checks. */
int main(int argc, char** argv)
{
    CHECK(argc == 4);
    fixture_probe primary(argv[1]);
    fixture_probe secondary(argv[2]);
    fixture_probe conflict(argv[3]);

    test_config_contracts(argv[1], primary);
    test_empty_snapshot_manifest_validation(argv[3], conflict);
    test_post_create_oom_rollback(argv[1], argv[2], primary, secondary);
    std::printf("cli_runtime_test: all checks passed\n");
    return EXIT_SUCCESS;
}

#endif
