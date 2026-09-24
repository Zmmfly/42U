#pragma once

#include <42u/cli.hpp>
#include <42u/host.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/**
 * @file cli_host.hpp
 * @brief CLI-frontend-neutral owned catalog, invocation, and application declarations.
 *
 * The types in this header own all text copied from plugin manifests. They deliberately expose
 * no CLI11 type: a frontend may use CLI11 internally without making it part of the 42U library,
 * plugin SDK, ABI, fixtures, or callers of this header.
 */
namespace u42::cli {

/** @brief Process exit code for successful help, version, or command execution. */
inline constexpr int exit_ok = 0;
/** @brief Process exit code for discovery, declaration, lifecycle, command, or cleanup failure. */
inline constexpr int exit_failure = 1;
/** @brief Process exit code for invalid user command-line input. */
inline constexpr int exit_usage = 2;

/** @brief Host-owned copy of one root or command-local parameter declaration. */
struct parameter {
    std::string plugin_id;
    std::string parameter_id;
    std::string long_name;
    std::string help;
    std::string value_name;
    std::string config_key;
    v1::flag_bits flags = 0;
    v1::parameter_kind kind = v1::text;
    std::uint32_t positional_index = v1::no_position;
    std::vector<std::string> default_values;
    std::vector<std::string> allowed_values;
};

/** @brief Host-owned copy of one plugin-declared top-level command. */
struct command {
    std::string plugin_id;
    std::string command_id;
    std::string name;
    std::string help;
    v1::flag_bits flags = 0;
    abi::v3::method_id handler = 0;
    std::vector<parameter> parameters;
};

/**
 * @brief Validated, deep-copied command surface assembled from discovered plugin manifests.
 *
 * @note add() enforces v1 sizes, reserved fields, known kinds/flags, reserved host names, stable
 *       identifiers, root/command scope rules, and global collisions. It must not retain any
 *       plugin-owned view after returning.
 */
class catalog {
public:
    /** @brief Construct an empty declaration catalog. */
    catalog();
    /** @brief Destroy all host-owned declaration copies. */
    ~catalog();
    /** @brief Copy a complete host-owned catalog. */
    catalog(const catalog&);
    /** @brief Replace this catalog with a complete host-owned copy. */
    catalog& operator=(const catalog&);
    /** @brief Transfer all owned declarations from another catalog. */
    catalog(catalog&&) noexcept;
    /** @brief Replace this catalog by transferring another catalog's declarations. */
    catalog& operator=(catalog&&) noexcept;

    /**
     * @brief Validate and copy one plugin's complete manifest contribution atomically.
     *
     * @param plugin_id Stable identity copied from the same library's v3 factory descriptor.
     * @param value Required borrowed manifest returned by u42_get_cli_manifest().
     * @param[out] error Optional diagnostic, cleared before validation and populated on failure.
     * @return ok when committed; otherwise invalid_argument, unsupported, duplicate,
     *         limit_exceeded, or failed. Failure leaves the catalog unchanged.
     */
    abi::v3::status add(std::string plugin_id, const v1::manifest* value,
                        std::string* error = nullptr);

    /** @brief Remove every copied declaration. */
    void clear() noexcept;

    /** @brief Return root parameters in deterministic discovery/declaration order. */
    const std::vector<parameter>& root_parameters() const noexcept;

    /** @brief Return commands in deterministic discovery/declaration order. */
    const std::vector<command>& commands() const noexcept;

private:
    std::vector<std::string> plugin_ids_;
    std::vector<parameter> root_parameters_;
    std::vector<command> commands_;
};

/** @brief One resolved parameter value after defaults and explicit CLI occurrences are merged. */
struct argument {
    std::string parameter_id;
    v1::parameter_kind kind = v1::text;
    bool explicitly_provided = false;
    std::vector<std::string> values;
};

/** @brief One host-owned configuration entry for a specific plugin snapshot. */
struct configuration_value {
    std::string key;
    v1::parameter_kind kind = v1::text;
    v1::config_source source = v1::plugin_default;
    v1::flag_bits flags = 0;
    std::vector<std::string> values;
};

/** @brief Complete frozen configuration owned by one plugin identity. */
struct plugin_configuration {
    std::string plugin_id;
    std::vector<configuration_value> entries;
};

/** @brief Selected plugin-owned command and the deterministic request prepared for its handler. */
struct selected_command {
    std::string plugin_id;
    std::string command_id;
    abi::v3::method_id handler = 0;
    std::vector<argument> arguments;
    std::string payload;
};

/**
 * @brief Frontend-neutral result of parsing one process invocation.
 *
 * @note configurations contains only each owning plugin's values. command is absent for help or
 *       version rendering and present for executable business invocations. The first CLI version
 *       permits exactly one selected command.
 */
struct invocation {
    std::vector<plugin_configuration> configurations;
    std::optional<selected_command> command;
};

/** @brief High-level outcome selected by the restricted CLI parser. */
enum class parse_kind {
    root_help,
    command_help,
    execute,
};

/** @brief Parsed invocation or host-rendered help, with no CLI11 type in the interface. */
struct parse_result {
    parse_kind kind = parse_kind::execute;
    invocation value;
    std::string rendered_help;
};

/**
 * @brief Parse one argv against a validated catalog and build owned configuration/payload data.
 *
 * @param surface Deep-copied declaration catalog.
 * @param argc Number of argv elements, including argv[0].
 * @param argv Borrowed process argument array.
 * @param[out] out Required parse result, cleared before parsing and on failure.
 * @param[out] error Optional user-facing diagnostic, cleared first.
 * @return ok on executable input or a help request; invalid_argument for user input; otherwise
 *         a host-side construction failure. The application handles --version before discovery.
 *
 * @note The implementation may use CLI11, but its public declaration and result types may not.
 */
abi::v3::status parse_invocation(const catalog& surface, int argc,
                                 const char* const* argv, parse_result* out,
                                 std::string* error = nullptr);

/** @brief Construction options for the pure-host command-line application. */
struct application_options {
    /** @brief Version text printed by the host-owned root --version option. */
    std::string version;
    /** @brief Resource bounds applied to the underlying plugin runtime. */
    host_options runtime{};
};

/**
 * @brief End-to-end pure-host CLI application with an implementation-hidden frontend/runtime.
 *
 * @note run() owns discovery, manifest copying, parsing, configuration freezing, lifecycle,
 *       selected-handler invocation, shutdown, diagnostics, and process-code mapping. Business
 *       root parameters and commands come only from plugin manifests. --version does not discover
 *       plugins; help may discover manifests but never creates instances.
 */
class application {
public:
    /**
     * @brief Construct an application with host version text and runtime bounds.
     *
     * @param options Owned construction options.
     */
    explicit application(application_options options = {});
    /** @brief Destroy the hidden frontend/runtime state. */
    ~application();
    /** @brief Applications own process/runtime state and cannot be copied. */
    application(const application&) = delete;
    /** @brief Applications own process/runtime state and cannot be copy-assigned. */
    application& operator=(const application&) = delete;
    /** @brief Transfer the hidden application state. */
    application(application&&) noexcept;
    /** @brief Replace this application by transferring hidden state. */
    application& operator=(application&&) noexcept;

    /**
     * @brief Execute one process invocation synchronously on the constructing control thread.
     *
     * @param argc Number of argv elements, including argv[0].
     * @param argv Borrowed process argument array.
     * @return exit_ok for success, exit_failure for discovery/declaration/runtime/cleanup failure,
     *         or exit_usage for user input error. ABI and frontend-specific status codes never
     *         escape as process codes.
     */
    int run(int argc, const char* const* argv);

    /**
     * @brief Return the latest application diagnostic.
     *
     * @return Borrowed text owned by the application and replaced by a later run operation.
     */
    const std::string& error() const noexcept;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace u42::cli
