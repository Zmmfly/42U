#pragma once

#include <42u/abi.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

/**
 * @file cli.hpp
 * @brief Frozen v1 ABI contract for plugin-declared command-line manifests and configuration.
 *
 * This extension is negotiated independently from the base plugin ABI. It contains no CLI11
 * type and does not change @c u42::abi::v3. A host first negotiates the v3 factory, then may look
 * up @c u42_get_cli_manifest and request this manifest major.
 */
namespace u42::cli::v1 {

using status = abi::v3::status;
using method_id = abi::v3::method_id;
using flag_bits = std::uint64_t;
using parameter_kind = std::uint32_t;
using config_source = std::uint32_t;

/** @brief First and only manifest major described by this header. */
inline constexpr std::uint32_t manifest_major = 1;

/** @brief Maximum bytes in a stable parameter, command, or configuration identifier. */
inline constexpr std::uint64_t max_identifier_bytes = 128;
/** @brief Maximum bytes in a visible command or long-option name. */
inline constexpr std::uint64_t max_cli_name_bytes = 64;
/** @brief Maximum bytes in one help string. */
inline constexpr std::uint64_t max_help_bytes = 4096;
/** @brief Maximum bytes in one value-name label. */
inline constexpr std::uint64_t max_value_name_bytes = 64;
/** @brief Maximum bytes in one manifest-declared default or allowed value. */
inline constexpr std::uint64_t max_declared_value_bytes = 64 * 1024;
/** @brief Maximum root parameters contributed by one plugin. */
inline constexpr std::uint64_t max_root_parameters = 256;
/** @brief Maximum top-level commands contributed by one plugin. */
inline constexpr std::uint64_t max_commands = 256;
/** @brief Maximum parameters declared by one command. */
inline constexpr std::uint64_t max_command_parameters = 256;
/** @brief Maximum default values attached to one parameter. */
inline constexpr std::uint64_t max_default_values = 256;
/** @brief Maximum allowed-value literals attached to one parameter. */
inline constexpr std::uint64_t max_allowed_values = 1024;
/** @brief Maximum aggregate bytes copied from one plugin's manifest views. */
inline constexpr std::uint64_t max_manifest_copy_bytes = 4 * 1024 * 1024;

/** @brief Sentinel used by long options, which have no positional index. */
inline constexpr std::uint32_t no_position = UINT32_MAX;

/** @brief Parameter consumes exactly one UTF-8 text value per occurrence. */
inline constexpr parameter_kind text = 1;
/** @brief Parameter consumes no value and is represented as a JSON/config boolean. */
inline constexpr parameter_kind flag = 2;

/** @brief Parameter must occur when its owning command is selected. */
inline constexpr flag_bits parameter_required = flag_bits{1} << 0;
/** @brief Parameter may occur more than once and preserves occurrence order. */
inline constexpr flag_bits parameter_repeatable = flag_bits{1} << 1;
/** @brief Parameter value must be redacted from diagnostics and help-derived echoes. */
inline constexpr flag_bits parameter_sensitive = flag_bits{1} << 2;
/** @brief All parameter flags understood by manifest v1. */
inline constexpr flag_bits parameter_known_flags =
    parameter_required | parameter_repeatable | parameter_sensitive;

/** @brief Manifest v1 currently defines no manifest-wide behavior flags. */
inline constexpr flag_bits manifest_known_flags = 0;
/** @brief Manifest v1 currently defines no command behavior flags. */
inline constexpr flag_bits command_known_flags = 0;

/** @brief Configuration value originated from a plugin-declared default. */
inline constexpr config_source plugin_default = 1;
/** @brief Configuration value originated from an explicit command-line occurrence. */
inline constexpr config_source command_line = 2;

/** @brief Configuration entry contains a sensitive value. */
inline constexpr flag_bits config_sensitive = flag_bits{1} << 0;
/** @brief All configuration-entry flags understood by config interface v1. */
inline constexpr flag_bits config_known_flags = config_sensitive;

/**
 * @brief Borrowed, length-delimited UTF-8 text.
 *
 * @note The bytes need not be NUL-terminated. A null pointer requires size zero. A non-null
 *       pointer with size zero is valid, so an array containing one such view represents an
 *       explicit empty string rather than an absent value.
 */
struct text_view {
    const char* data = nullptr;
    std::uint64_t size = 0;
};

/**
 * @brief Borrowed contiguous array view.
 *
 * @tparam T Immutable element type owned by the provider.
 * @note A null pointer requires size zero. The provider retains ownership of every element and
 *       of all nested views.
 */
template <class T>
struct array_view {
    const T* data = nullptr;
    std::uint64_t size = 0;
};

/**
 * @brief Validate the pointer/length invariant of a borrowed text view.
 *
 * @param value View to inspect.
 * @return True when null is paired with zero length.
 */
constexpr bool valid_view(text_view value) noexcept
{
    return value.data != nullptr || value.size == 0;
}

/**
 * @brief Validate the pointer/count invariant of a borrowed array view.
 *
 * @tparam T Array element type.
 * @param value View to inspect.
 * @return True when null is paired with zero count.
 */
template <class T>
constexpr bool valid_view(array_view<T> value) noexcept
{
    return value.data != nullptr || value.size == 0;
}

/**
 * @brief Test whether a flag word contains only bits understood by its structure version.
 *
 * @param value Flag word supplied by the plugin or host.
 * @param known_mask Bit mask defined by the matching v1 structure.
 * @return True when no unknown bit is set.
 */
constexpr bool valid_flags(flag_bits value, flag_bits known_mask) noexcept
{
    return (value & ~known_mask) == 0;
}

/** @brief Return true when a parameter descriptor contains no unknown flag bit. */
constexpr bool valid_parameter_flags(flag_bits value) noexcept
{
    return valid_flags(value, parameter_known_flags);
}

/** @brief Return true when a command descriptor contains no unknown flag bit. */
constexpr bool valid_command_flags(flag_bits value) noexcept
{
    return valid_flags(value, command_known_flags);
}

/** @brief Return true when a manifest contains no unknown flag bit. */
constexpr bool valid_manifest_flags(flag_bits value) noexcept
{
    return valid_flags(value, manifest_known_flags);
}

/** @brief Return true when a configuration entry contains no unknown flag bit. */
constexpr bool valid_config_flags(flag_bits value) noexcept
{
    return valid_flags(value, config_known_flags);
}

/**
 * @brief Test whether a parameter kind belongs to manifest v1.
 *
 * @param value Numeric kind from a descriptor.
 * @return True for text and flag only.
 */
constexpr bool valid_parameter_kind(parameter_kind value) noexcept
{
    return value == text || value == flag;
}

/**
 * @brief Test whether a configuration source belongs to config interface v1.
 *
 * @param value Numeric source from a configuration entry.
 * @return True for plugin defaults and explicit command-line values only.
 */
constexpr bool valid_config_source(config_source value) noexcept
{
    return value == plugin_default || value == command_line;
}

/**
 * @brief One root option or command-local parameter declaration.
 *
 * @note All views are borrowed from the plugin and remain valid only while its library stays
 *       mapped. The host must validate and deep-copy them before instance creation. @c param_id
 *       is the stable payload/config identity. @c long_name excludes the leading "--" and is
 *       empty only for a positional command parameter. Positional parameters set
 *       @c positional_index to a zero-based fixed position; options use @c no_position.
 *
 * @note Root parameters must be long options, must not set @c parameter_required, and use
 *       @c param_id as their owning plugin's config key. Command-local parameters enter the
 *       handler payload by default; a non-empty @c config_key redirects that parameter into the
 *       owning plugin's configuration snapshot when the command is selected.
 *
 * @note Stable identifiers use ASCII letters, digits, underscore, hyphen, and dot, begin with an
 *       ASCII letter, and are bounded by @c max_identifier_bytes. Visible command/option names
 *       use lowercase ASCII letters, digits, and single hyphens, begin with a lowercase letter,
 *       and are bounded by @c max_cli_name_bytes. The host rejects leading/trailing/consecutive
 *       hyphens, reserved names, embedded NUL bytes, invalid UTF-8, and every view over its
 *       corresponding v1 byte/count limit.
 *
 * @note @c default_values with zero elements means no default. One zero-length text element is
 *       an explicit empty default. @c allowed_values with zero elements means unrestricted text.
 *       Flags have no value name or allowed-value list and are not repeatable in manifest v1.
 *       struct_size must equal sizeof(parameter_desc), reserved must be zero, and unknown flags
 *       make the manifest invalid.
 */
struct parameter_desc {
    std::uint32_t struct_size = sizeof(parameter_desc);
    std::uint32_t reserved = 0;
    flag_bits flags = 0;
    parameter_kind kind = text;
    std::uint32_t positional_index = no_position;
    text_view param_id{};
    text_view long_name{};
    text_view help{};
    text_view value_name{};
    text_view config_key{};
    array_view<text_view> default_values{};
    array_view<text_view> allowed_values{};
};

/**
 * @brief One plugin-owned top-level command and its handler binding.
 *
 * @note @c command_id is stable plugin-local identity; @c name is the globally visible command
 *       token. The handler is a method ID that the same plugin must publish after start(). The
 *       host deep-copies all views during discovery. struct_size must equal sizeof(command_desc),
 *       both reserved fields must be zero, and unknown flags make the manifest invalid.
 */
struct command_desc {
    std::uint32_t struct_size = sizeof(command_desc);
    std::uint32_t reserved = 0;
    flag_bits flags = 0;
    method_id handler = 0;
    std::uint32_t reserved_handler = 0;
    text_view command_id{};
    text_view name{};
    text_view help{};
    array_view<parameter_desc> parameters{};
};

/**
 * @brief Complete immutable command-line declaration contributed by one plugin library.
 *
 * @note The plugin identity is taken from the factory descriptor negotiated from the same
 *       library. All nested strings and arrays are borrowed until the library is unloaded; the
 *       host must deep-copy them before instance creation. struct_size must equal
 *       sizeof(manifest), reserved must be zero, and unknown flags make discovery fail. Counts
 *       and copied bytes are bounded by the @c max_* constants in this header.
 */
struct manifest {
    std::uint32_t struct_size = sizeof(manifest);
    std::uint32_t reserved = 0;
    flag_bits flags = 0;
    array_view<parameter_desc> root_parameters{};
    array_view<command_desc> commands{};
};

/**
 * @brief One immutable value in the calling plugin's frozen configuration snapshot.
 *
 * @note Entry existence distinguishes a configured key from an absent key. @c values preserves
 *       CLI occurrence order; one zero-length value is an explicit empty string. A flag uses one
 *       canonical value, "true" or "false". Views returned by iconfig remain valid until the next
 *       iconfig call on the same service or until the current plugin callback returns, whichever
 *       happens first. Copy data that must survive that boundary.
 *
 * @note struct_size must equal sizeof(config_entry), reserved must be zero, and consumers must
 *       reject unknown flags, kinds, or sources rather than guessing their meaning.
 */
struct config_entry {
    std::uint32_t struct_size = sizeof(config_entry);
    std::uint32_t reserved = 0;
    flag_bits flags = 0;
    text_view key{};
    parameter_kind kind = text;
    config_source source = plugin_default;
    array_view<text_view> values{};
};

/**
 * @brief Read-only access to the calling plugin's own frozen configuration snapshot.
 *
 * @note The host exposes this interface through abi::v3::ictx::query(config_iid). It never
 *       exposes another plugin's snapshot. Output structures and all nested views are borrowed;
 *       callers must copy anything retained across the lifetime stated by config_entry.
 */
struct iconfig {
    /**
     * @brief Return all present configuration entries in stable manifest order.
     *
     * @param[out] out Required output; cleared before validation and on failure.
     * @return ok on success, invalid_argument for null output, or a host state/thread failure.
     */
    virtual status U42_CALL entries(array_view<config_entry>* out) noexcept = 0;

    /**
     * @brief Look up one exact stable configuration key.
     *
     * @param key Required non-empty borrowed key; null requires zero size.
     * @param[out] out Required output; reset to a default entry before lookup and on failure.
     * @return ok on success, not_found when the key is absent, or an argument/state/thread error.
     */
    virtual status U42_CALL get(text_view key, config_entry* out) noexcept = 0;

protected:
    ~iconfig() = default;
};

/** @brief Official context-service identifier for iconfig v1 (ASCII tag "42U_CLI1"). */
inline constexpr abi::v3::iid config_iid{0x3432555f434c4931ULL, 1};

/** @brief Function-pointer type of the independently negotiated CLI manifest entry. */
using entry_fn = status (U42_CALL *)(std::uint32_t, const manifest**) noexcept;

/** @brief Exact dynamic symbol name of the CLI manifest entry. */
inline constexpr const char* entry_name = "u42_get_cli_manifest";

static_assert(sizeof(text_view) == sizeof(array_view<text_view>));
static_assert(std::is_standard_layout_v<text_view> && std::is_trivially_copyable_v<text_view>);
static_assert(std::is_standard_layout_v<parameter_desc> &&
              std::is_trivially_copyable_v<parameter_desc>);
static_assert(std::is_standard_layout_v<command_desc> &&
              std::is_trivially_copyable_v<command_desc>);
static_assert(std::is_standard_layout_v<manifest> && std::is_trivially_copyable_v<manifest>);
static_assert(std::is_standard_layout_v<config_entry> &&
              std::is_trivially_copyable_v<config_entry>);

#if UINTPTR_MAX == UINT64_MAX
static_assert(sizeof(text_view) == 16 && alignof(text_view) == 8);
static_assert(sizeof(parameter_desc) == 136 && alignof(parameter_desc) == 8);
static_assert(offsetof(parameter_desc, flags) == 8);
static_assert(offsetof(parameter_desc, param_id) == 24);
static_assert(offsetof(parameter_desc, default_values) == 104);
static_assert(sizeof(command_desc) == 88 && alignof(command_desc) == 8);
static_assert(offsetof(command_desc, handler) == 16);
static_assert(offsetof(command_desc, command_id) == 24);
static_assert(offsetof(command_desc, parameters) == 72);
static_assert(sizeof(manifest) == 48 && alignof(manifest) == 8);
static_assert(offsetof(manifest, root_parameters) == 16);
static_assert(sizeof(config_entry) == 56 && alignof(config_entry) == 8);
static_assert(offsetof(config_entry, key) == 16);
static_assert(offsetof(config_entry, values) == 40);
static_assert(sizeof(iconfig) == sizeof(void*));
#endif

} // namespace u42::cli::v1

/**
 * @brief Negotiate and return one library-owned immutable CLI manifest.
 *
 * @param major Requested CLI manifest major, currently u42::cli::v1::manifest_major.
 * @param[out] out Required output; implementations clear it before validation and on failure.
 * @return abi::v3::ok when supported, abi::v3::unsupported for another major, or
 *         abi::v3::invalid_argument for a null output.
 *
 * @note Plugin definitions add U42_EXPORT; host-side declarations do not export this symbol.
 */
extern "C" u42::abi::v3::status U42_CALL u42_get_cli_manifest(
    std::uint32_t major, const u42::cli::v1::manifest** out) noexcept;
