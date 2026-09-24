/**
 * @file cli_abi_test.cc
 * @brief Standalone compile-time and mock verification of the frozen CLI extension contract.
 *
 * @code
 *   g++ -std=c++17 -Wall -Wextra -Werror -Iinc -Ihost/include tests/cli_abi_test.cc -o cli_abi_test
 * @endcode
 *
 * The test links only the standard library. It pins the manifest/config layout, entry signature,
 * flag validation, borrowed-view rules, host-side application pimpl shape, and exact config IID
 * query behavior without requiring CLI11, xmake, the host runtime, or a plugin DSO.
 */
#define NDEBUG 1

#include <42u/cli.hpp>
#include <42u/cli_host.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

namespace abi = u42::abi::v3;
namespace cli = u42::cli::v1;

/** @brief Report one failed runtime contract check. */
[[noreturn]] void fail_check(const char* expression, const char* file, int line)
{
    std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", expression, file, line);
    std::exit(EXIT_FAILURE);
}

/** @brief Runtime assertion that remains active under NDEBUG. */
#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) fail_check(#condition, __FILE__, __LINE__);           \
    } while (false)

/** @brief Compare a borrowed text view with a literal without requiring NUL termination. */
bool equals(cli::text_view actual, std::string_view expected)
{
    return actual.size == expected.size() &&
           (actual.size == 0 ||
            std::string_view(actual.data, static_cast<std::size_t>(actual.size)) == expected);
}

/** @brief Minimal read-only config service used to exercise both virtual signatures. */
class mock_config final : public cli::iconfig {
public:
    mock_config()
    {
        values_[0] = cli::text_view{"debug", 5};
        entry_.key = cli::text_view{"log-level", 9};
        entry_.kind = cli::text;
        entry_.source = cli::command_line;
        entry_.values = cli::array_view<cli::text_view>{values_, 1};
        entries_[0] = entry_;
    }

    abi::status U42_CALL entries(cli::array_view<cli::config_entry>* out) noexcept override
    {
        if (out == nullptr) return abi::invalid_argument;
        *out = {};
        *out = cli::array_view<cli::config_entry>{entries_, 1};
        return abi::ok;
    }

    abi::status U42_CALL get(cli::text_view key, cli::config_entry* out) noexcept override
    {
        if (out == nullptr) return abi::invalid_argument;
        *out = {};
        if (!cli::valid_view(key) || key.size == 0) return abi::invalid_argument;
        if (!equals(key, "log-level")) return abi::not_found;
        *out = entry_;
        return abi::ok;
    }

private:
    cli::text_view values_[1]{};
    cli::config_entry entry_{};
    cli::config_entry entries_[1]{};
};

/** @brief Context mock exposing only the official config service IID. */
class mock_context final : public abi::ictx {
public:
    explicit mock_context(cli::iconfig* config) noexcept : config_(config) {}

    abi::status U42_CALL query(const abi::iid* type, void** out) noexcept override
    {
        if (out == nullptr) return abi::invalid_argument;
        *out = nullptr;
        if (type == nullptr) return abi::invalid_argument;
        if (*type != cli::config_iid) return abi::unsupported;
        *out = config_;
        return abi::ok;
    }

private:
    cli::iconfig* config_ = nullptr;
};

using expected_entry = abi::status (U42_CALL *)(std::uint32_t, const cli::manifest**) noexcept;
using expected_entries = abi::status (U42_CALL cli::iconfig::*)(
    cli::array_view<cli::config_entry>*) noexcept;
using expected_get = abi::status (U42_CALL cli::iconfig::*)(
    cli::text_view, cli::config_entry*) noexcept;
using expected_parse = abi::status (*)(const u42::cli::catalog&, int,
                                       const char* const*, u42::cli::parse_result*,
                                       std::string*);

static_assert(cli::manifest_major == 1);
static_assert(cli::max_identifier_bytes == 128);
static_assert(cli::max_cli_name_bytes == 64);
static_assert(cli::max_manifest_copy_bytes == 4 * 1024 * 1024);
static_assert(std::is_same_v<cli::entry_fn, expected_entry>);
static_assert(std::is_same_v<decltype(&u42_get_cli_manifest), expected_entry>);
static_assert(std::is_same_v<decltype(&cli::iconfig::entries), expected_entries>);
static_assert(std::is_same_v<decltype(&cli::iconfig::get), expected_get>);
static_assert(std::is_same_v<decltype(&u42::cli::parse_invocation), expected_parse>);

static_assert(cli::valid_parameter_kind(cli::text));
static_assert(cli::valid_parameter_kind(cli::flag));
static_assert(!cli::valid_parameter_kind(0));
static_assert(!cli::valid_parameter_kind(3));
static_assert(cli::valid_config_source(cli::plugin_default));
static_assert(cli::valid_config_source(cli::command_line));
static_assert(!cli::valid_config_source(0));
static_assert(!cli::valid_config_source(3));

static_assert(cli::valid_flags(cli::parameter_required | cli::parameter_sensitive,
                               cli::parameter_known_flags));
static_assert(!cli::valid_flags(cli::flag_bits{1} << 63, cli::parameter_known_flags));
static_assert(cli::valid_parameter_flags(cli::parameter_repeatable));
static_assert(!cli::valid_parameter_flags(cli::flag_bits{1} << 63));
static_assert(cli::valid_flags(0, cli::manifest_known_flags));
static_assert(!cli::valid_flags(1, cli::manifest_known_flags));
static_assert(cli::valid_manifest_flags(0));
static_assert(!cli::valid_manifest_flags(1));
static_assert(cli::valid_command_flags(0));
static_assert(!cli::valid_command_flags(1));
static_assert(cli::valid_flags(cli::config_sensitive, cli::config_known_flags));
static_assert(!cli::valid_flags(2, cli::config_known_flags));
static_assert(cli::valid_config_flags(cli::config_sensitive));
static_assert(!cli::valid_config_flags(2));

static_assert(cli::valid_view(cli::text_view{}));
static_assert(!cli::valid_view(cli::text_view{nullptr, 1}));
static_assert(cli::valid_view(cli::array_view<int>{}));
static_assert(!cli::valid_view(cli::array_view<int>{nullptr, 1}));

static_assert(std::is_standard_layout_v<cli::text_view>);
static_assert(std::is_trivially_copyable_v<cli::parameter_desc>);
static_assert(std::is_trivially_copyable_v<cli::command_desc>);
static_assert(std::is_trivially_copyable_v<cli::manifest>);
static_assert(std::is_trivially_copyable_v<cli::config_entry>);

static_assert(offsetof(cli::parameter_desc, struct_size) == 0);
static_assert(offsetof(cli::parameter_desc, reserved) == 4);
static_assert(offsetof(cli::command_desc, struct_size) == 0);
static_assert(offsetof(cli::command_desc, reserved) == 4);
static_assert(offsetof(cli::manifest, struct_size) == 0);
static_assert(offsetof(cli::manifest, reserved) == 4);
static_assert(offsetof(cli::config_entry, struct_size) == 0);
static_assert(offsetof(cli::config_entry, reserved) == 4);

#if UINTPTR_MAX == UINT64_MAX
static_assert(sizeof(cli::text_view) == 16);
static_assert(sizeof(cli::parameter_desc) == 136);
static_assert(offsetof(cli::parameter_desc, param_id) == 24);
static_assert(offsetof(cli::parameter_desc, default_values) == 104);
static_assert(sizeof(cli::command_desc) == 88);
static_assert(offsetof(cli::command_desc, command_id) == 24);
static_assert(sizeof(cli::manifest) == 48);
static_assert(sizeof(cli::config_entry) == 56);
static_assert(offsetof(cli::config_entry, values) == 40);
#endif

static_assert(!std::is_copy_constructible_v<u42::cli::application>);
static_assert(!std::is_copy_assignable_v<u42::cli::application>);
static_assert(std::is_move_constructible_v<u42::cli::application>);
static_assert(std::is_move_assignable_v<u42::cli::application>);
static_assert(sizeof(u42::cli::application) == sizeof(std::unique_ptr<void>));
static_assert(u42::cli::exit_ok == 0);
static_assert(u42::cli::exit_failure == 1);
static_assert(u42::cli::exit_usage == 2);
static_assert(std::is_default_constructible_v<u42::cli::parse_result>);

/** @brief Verify config IID routing, output clearing, and borrowed entry contents. */
void test_mock_query()
{
    mock_config config;
    mock_context context(&config);

    void* raw = reinterpret_cast<void*>(static_cast<std::uintptr_t>(1));
    CHECK(context.query(&cli::config_iid, &raw) == abi::ok);
    CHECK(raw == static_cast<cli::iconfig*>(&config));

    auto* queried = static_cast<cli::iconfig*>(raw);
    cli::array_view<cli::config_entry> entries{reinterpret_cast<cli::config_entry*>(
                                                   static_cast<std::uintptr_t>(1)),
                                               7};
    CHECK(queried->entries(&entries) == abi::ok);
    CHECK(entries.size == 1);
    CHECK(entries.data != nullptr);
    CHECK(equals(entries.data[0].key, "log-level"));
    CHECK(entries.data[0].source == cli::command_line);
    CHECK(entries.data[0].values.size == 1);
    CHECK(equals(entries.data[0].values.data[0], "debug"));

    cli::config_entry found;
    CHECK(queried->get(cli::text_view{"log-level", 9}, &found) == abi::ok);
    CHECK(equals(found.key, "log-level"));
    CHECK(found.kind == cli::text);

    found.key = cli::text_view{"dirty", 5};
    CHECK(queried->get(cli::text_view{"missing", 7}, &found) == abi::not_found);
    CHECK(found.key.data == nullptr);
    CHECK(found.key.size == 0);

    const abi::iid unknown{0x1111222233334444ULL, 0x5555666677778888ULL};
    raw = reinterpret_cast<void*>(static_cast<std::uintptr_t>(1));
    CHECK(context.query(&unknown, &raw) == abi::unsupported);
    CHECK(raw == nullptr);
}

} // namespace

/** @brief Run the dependency-free CLI ABI checks. */
int main()
{
    test_mock_query();
    return EXIT_SUCCESS;
}
