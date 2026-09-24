/**
 * @file cli_frontend.cc
 * @brief Restricted CLI11 frontend and pure-host process orchestration.
 */
#if defined(_WIN32) && !defined(NOMINMAX)
#  define NOMINMAX
#endif

#include <42u/cli_host.hpp>
#include <42u/plug.hpp>

#include <CLI/CLI.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#elif defined(__APPLE__)
#  include <mach-o/dyld.h>
#endif

namespace u42::cli {
namespace {

/** @brief Frozen ABI namespace used by the command-line frontend. */
namespace abi = u42::abi::v3;

/**
 * @brief Replace an optional diagnostic without repeating null checks.
 *
 * @param error Optional diagnostic destination.
 * @param text New diagnostic text.
 */
void assign_error(std::string* error, std::string text)
{
    if (error != nullptr) *error = std::move(text);
}

/**
 * @brief Validate one byte sequence as strict UTF-8.
 *
 * @param value Bytes to validate.
 * @return True only for well-formed, shortest-form Unicode scalar values.
 */
bool valid_utf8(std::string_view value) noexcept
{
    std::size_t index = 0;
    while (index < value.size()) {
        const auto lead = static_cast<unsigned char>(value[index]);
        if (lead <= 0x7fU) {
            ++index;
            continue;
        }

        std::size_t trailing = 0;
        std::uint32_t codepoint = 0;
        std::uint32_t minimum = 0;
        if (lead >= 0xc2U && lead <= 0xdfU) {
            trailing = 1;
            codepoint = lead & 0x1fU;
            minimum = 0x80U;
        } else if (lead >= 0xe0U && lead <= 0xefU) {
            trailing = 2;
            codepoint = lead & 0x0fU;
            minimum = 0x800U;
        } else if (lead >= 0xf0U && lead <= 0xf4U) {
            trailing = 3;
            codepoint = lead & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (trailing > value.size() - index - 1U) return false;
        for (std::size_t offset = 1; offset <= trailing; ++offset) {
            const auto byte = static_cast<unsigned char>(value[index + offset]);
            if ((byte & 0xc0U) != 0x80U) return false;
            codepoint = (codepoint << 6U) | (byte & 0x3fU);
        }
        if (codepoint < minimum || codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            return false;
        }
        index += trailing + 1U;
    }
    return true;
}

/**
 * @brief Validate values against one parameter's optional allow-list.
 *
 * @param declaration Parameter declaration.
 * @param values Values to validate.
 * @param[out] error Failure diagnostic.
 * @return True when unrestricted or every value is listed.
 */
bool values_allowed(const parameter& declaration, const std::vector<std::string>& values,
                    std::string& error)
{
    if (declaration.allowed_values.empty()) return true;
    for (const std::string& value : values) {
        if (std::find(declaration.allowed_values.begin(), declaration.allowed_values.end(), value) ==
            declaration.allowed_values.end()) {
            const std::string name = declaration.long_name.empty()
                                         ? declaration.value_name
                                         : "--" + declaration.long_name;
            error = "value for '" + name + "' is not in its allowed set";
            return false;
        }
    }
    return true;
}

/**
 * @brief Find one exact root parameter by visible long name.
 *
 * @param surface Validated catalog.
 * @param name Visible name without leading dashes.
 * @return Matching declaration, or null.
 */
const parameter* find_root(const catalog& surface, std::string_view name) noexcept
{
    for (const parameter& value : surface.root_parameters()) {
        if (value.long_name == name) return &value;
    }
    return nullptr;
}

/**
 * @brief Find one exact top-level command by visible name.
 *
 * @param surface Validated catalog.
 * @param name Command token.
 * @return Matching declaration, or null.
 */
const command* find_command(const catalog& surface, std::string_view name) noexcept
{
    for (const command& value : surface.commands()) {
        if (value.name == name) return &value;
    }
    return nullptr;
}

/**
 * @brief Find one exact long option in a command.
 *
 * @param owner Command declaration.
 * @param name Visible option name without leading dashes.
 * @return Matching declaration, or null.
 */
const parameter* find_local(const command& owner, std::string_view name) noexcept
{
    for (const parameter& value : owner.parameters) {
        if (value.positional_index == v1::no_position && value.long_name == name) return &value;
    }
    return nullptr;
}

/**
 * @brief Detect whether any command owns an exact local option name.
 *
 * @param surface Validated catalog.
 * @param name Visible option name without leading dashes.
 * @return True when at least one command declares the option.
 */
bool any_local(const catalog& surface, std::string_view name) noexcept
{
    for (const command& owner : surface.commands()) {
        if (find_local(owner, name) != nullptr) return true;
    }
    return false;
}

/** @brief One parsed long-option token before CLI11 sees normalized argv. */
struct long_token {
    std::string_view name;
    std::string_view value;
    bool has_equals = false;
};

/**
 * @brief Split one token beginning with two dashes.
 *
 * @param token Complete argv token.
 * @return Name and optional text after the first equals sign.
 */
long_token split_long(std::string_view token) noexcept
{
    const std::string_view body = token.substr(2);
    const std::size_t equals = body.find('=');
    if (equals == std::string_view::npos) return {body, {}, false};
    return {body.substr(0, equals), body.substr(equals + 1), true};
}

/** @brief Result of the 42U grammar pass that precedes CLI11 parsing. */
struct normalized_invocation {
    std::vector<std::string> argv;
    const command* selected = nullptr;
    bool root_help = false;
    bool command_help = false;
    std::unordered_map<const parameter*, std::size_t> occurrences;
    std::unordered_map<const parameter*, std::vector<std::string>> positional_values;
};

/**
 * @brief Record one explicit parameter occurrence and enforce non-repeatability.
 *
 * @param declaration Parameter being consumed.
 * @param[in,out] normalized Preflight result.
 * @param[out] error Failure diagnostic.
 * @return True when the occurrence is allowed.
 */
bool record_occurrence(const parameter& declaration, normalized_invocation& normalized,
                       std::string& error)
{
    std::size_t& count = normalized.occurrences[&declaration];
    ++count;
    if (count > 1 && (declaration.flags & v1::parameter_repeatable) == 0) {
        const std::string name = declaration.long_name.empty()
                                     ? declaration.parameter_id
                                     : "--" + declaration.long_name;
        error = "parameter '" + name + "' may be provided only once";
        return false;
    }
    return true;
}

/**
 * @brief Validate one explicit text value without echoing potentially sensitive bytes.
 *
 * @param declaration Owning parameter declaration.
 * @param value Explicit command-line bytes.
 * @param[out] error Failure diagnostic.
 * @return True when the value is valid UTF-8.
 */
bool validate_text_value(const parameter& declaration, std::string_view value,
                         std::string& error)
{
    if (valid_utf8(value)) return true;
    const std::string name = declaration.long_name.empty()
                                 ? declaration.parameter_id
                                 : "--" + declaration.long_name;
    error = "text value for '" + name + "' is not valid UTF-8";
    return false;
}

/**
 * @brief Return command positionals in their fixed index order.
 *
 * @param owner Command declaration.
 * @return Borrowed parameter pointers ordered by positional_index.
 */
std::vector<const parameter*> ordered_positionals(const command& owner)
{
    std::vector<const parameter*> positionals;
    for (const parameter& declaration : owner.parameters) {
        if (declaration.positional_index != v1::no_position) positionals.push_back(&declaration);
    }
    std::sort(positionals.begin(), positionals.end(), [](const parameter* left,
                                                         const parameter* right) {
        return left->positional_index < right->positional_index;
    });
    return positionals;
}

/**
 * @brief Preserve one raw positional and give CLI11 an option-safe placeholder.
 *
 * @param declaration Positional declaration being filled.
 * @param value Raw command-line value.
 * @param ordinal Zero-based positional ordinal used only for the placeholder.
 * @param[in,out] normalized Preflight result.
 * @param[out] error Failure diagnostic.
 * @return True when the value is valid and recorded.
 */
bool append_positional(const parameter& declaration, std::string_view value,
                       std::size_t ordinal, normalized_invocation& normalized,
                       std::string& error)
{
    if (!validate_text_value(declaration, value, error) ||
        !record_occurrence(declaration, normalized, error)) {
        return false;
    }
    normalized.positional_values[&declaration].emplace_back(value);
    normalized.argv.push_back("u42-positional-value-" + std::to_string(ordinal));
    return true;
}

/**
 * @brief Append one option and its value while preserving explicit empty strings.
 *
 * @param raw Original token.
 * @param parsed Split option token.
 * @param declaration Matched parameter declaration.
 * @param argc Original argument count.
 * @param argv Original argument vector.
 * @param[in,out] index Current token index; advanced when a separate value is consumed.
 * @param[in,out] normalized Normalized argv destination.
 * @param[out] error Failure diagnostic.
 * @return True when the option is structurally valid.
 */
bool append_option(std::string_view raw, const long_token& parsed, const parameter& declaration,
                   int argc, const char* const* argv, int& index,
                   normalized_invocation& normalized, std::string& error)
{
    if (!record_occurrence(declaration, normalized, error)) return false;
    if (declaration.kind == v1::flag) {
        if (parsed.has_equals) {
            error = "flag '--" + declaration.long_name + "' does not accept a value";
            return false;
        }
        normalized.argv.emplace_back(raw);
        return true;
    }

    if (parsed.has_equals) {
        if (!validate_text_value(declaration, parsed.value, error)) return false;
        if (parsed.value.empty()) {
            normalized.argv.push_back("--" + declaration.long_name);
            normalized.argv.emplace_back();
        } else {
            normalized.argv.emplace_back(raw);
        }
        return true;
    }
    if (index + 1 >= argc || argv[index + 1] == nullptr) {
        error = "option '--" + declaration.long_name + "' requires a value";
        return false;
    }
    const std::string_view value = argv[index + 1];
    if (value == "++" || value == "--") {
        error = "standalone '" + std::string(value) +
                "' is not accepted; use --" + declaration.long_name + "=" +
                std::string(value) + " to pass it as data";
        return false;
    }
    if (!validate_text_value(declaration, value, error)) return false;
    normalized.argv.emplace_back(raw);
    normalized.argv.emplace_back(value);
    ++index;
    return true;
}

/**
 * @brief Enforce 42U's strict token grammar and normalize explicit empty option values.
 *
 * @param surface Validated declaration catalog.
 * @param argc Argument count including argv[0].
 * @param argv Borrowed argument vector.
 * @param[out] normalized Preflight selection and normalized argv.
 * @param[out] error User-facing diagnostic.
 * @return ok or invalid_argument for rejected user syntax.
 */
abi::status normalize_argv(const catalog& surface, int argc, const char* const* argv,
                           normalized_invocation& normalized, std::string& error)
{
    normalized = normalized_invocation{};
    error.clear();
    if (argc < 1 || argv == nullptr) {
        error = "argv must contain a program name";
        return abi::invalid_argument;
    }
    normalized.argv.emplace_back(argv[0] != nullptr && *argv[0] != '\0' ? argv[0] : "42u");

    std::size_t positional_count = 0;
    for (int index = 1; index < argc; ++index) {
        if (argv[index] == nullptr) {
            error = "argv contains a null argument";
            return abi::invalid_argument;
        }
        const std::string_view token = argv[index];
        if (token == "++") {
            error = "standalone '++' is not supported";
            return abi::invalid_argument;
        }
        if (token == "--") {
            if (normalized.selected == nullptr) {
                error = "standalone '--' is supported only after a command";
                return abi::invalid_argument;
            }
            const std::vector<const parameter*> positionals =
                ordered_positionals(*normalized.selected);
            if (positional_count >= positionals.size()) {
                error = "standalone '--' is not accepted after all positional parameters are filled";
                return abi::invalid_argument;
            }
            for (++index; index < argc; ++index) {
                if (argv[index] == nullptr) {
                    error = "argv contains a null argument";
                    return abi::invalid_argument;
                }
                if (positional_count >= positionals.size()) {
                    error = "too many positional arguments after '--'";
                    return abi::invalid_argument;
                }
                if (!append_positional(*positionals[positional_count], argv[index],
                                       positional_count, normalized, error)) {
                    return abi::invalid_argument;
                }
                ++positional_count;
            }
            break;
        }
        if (token == "-h") {
            error = "short option '-h' is not supported; use --help";
            return abi::invalid_argument;
        }
        if (token == "--version") {
            error = "--version must be the only argument";
            return abi::invalid_argument;
        }

        if (token.size() > 2 && token.substr(0, 2) == "--") {
            const long_token parsed = split_long(token);
            if (parsed.name.empty() || parsed.name.find('.') != std::string_view::npos) {
                error = "invalid long option syntax: '" + std::string(token) + "'";
                return abi::invalid_argument;
            }
            if (parsed.name == "help") {
                if (parsed.has_equals) {
                    error = "--help does not accept a value";
                    return abi::invalid_argument;
                }
                normalized.argv.emplace_back(token);
                if (normalized.selected == nullptr) {
                    normalized.root_help = true;
                } else {
                    normalized.command_help = true;
                }
                continue;
            }

            if (normalized.selected == nullptr) {
                const parameter* declaration = find_root(surface, parsed.name);
                if (declaration == nullptr) {
                    error = any_local(surface, parsed.name)
                                ? "command-local option '--" + std::string(parsed.name) +
                                      "' must appear after its command"
                                : "unknown root option '--" + std::string(parsed.name) + "'";
                    return abi::invalid_argument;
                }
                if (!append_option(token, parsed, *declaration, argc, argv, index, normalized,
                                   error)) {
                    return abi::invalid_argument;
                }
            } else {
                const parameter* declaration = find_local(*normalized.selected, parsed.name);
                if (declaration == nullptr) {
                    error = find_root(surface, parsed.name) != nullptr
                                ? "root option '--" + std::string(parsed.name) +
                                      "' must appear before the command"
                                : "unknown option '--" + std::string(parsed.name) + "' for command '" +
                                      normalized.selected->name + "'";
                    return abi::invalid_argument;
                }
                if (!append_option(token, parsed, *declaration, argc, argv, index, normalized,
                                   error)) {
                    return abi::invalid_argument;
                }
            }
            continue;
        }

        if (token.size() > 1 && token.front() == '-') {
            error = "short options are not supported: '" + std::string(token) + "'";
            return abi::invalid_argument;
        }

        if (normalized.selected == nullptr) {
            if (normalized.root_help) {
                error = "root --help cannot be combined with a command";
                return abi::invalid_argument;
            }
            normalized.selected = find_command(surface, token);
            if (normalized.selected == nullptr) {
                error = "unknown command '" + std::string(token) + "'";
                return abi::invalid_argument;
            }
            normalized.argv.emplace_back(token);
            continue;
        }

        const std::vector<const parameter*> positionals =
            ordered_positionals(*normalized.selected);
        if (positional_count < positionals.size()) {
            if (!append_positional(*positionals[positional_count], token, positional_count,
                                   normalized, error)) {
                return abi::invalid_argument;
            }
            ++positional_count;
            continue;
        }
        if (find_command(surface, token) != nullptr) {
            error = "only one command may be selected";
        } else {
            error = "unexpected positional argument '" + std::string(token) + "'";
        }
        return abi::invalid_argument;
    }

    if (!normalized.root_help && normalized.selected == nullptr) {
        error = "a plugin-declared command is required";
        return abi::invalid_argument;
    }
    return abi::ok;
}

/**
 * @brief Apply the restricted CLI11 parser settings to one command scope.
 *
 * @param app Root application or one top-level subcommand.
 */
void restrict_cli11(CLI::App& app)
{
    app.set_help_flag("--help", "Show this help and exit");
    app.allow_extras(false);
    app.prefix_command(false);
    app.fallthrough(false);
    app.subcommand_fallthrough(false);
    app.allow_subcommand_prefix_matching(false);
    app.allow_windows_style_options(false);
}

/** @brief CLI11 option associated with one validated manifest parameter. */
struct option_binding {
    const parameter* declaration = nullptr;
    CLI::Option* option = nullptr;
};

/** @brief CLI11 subcommand and option bindings associated with one manifest command. */
struct command_binding {
    const command* declaration = nullptr;
    CLI::App* app = nullptr;
    std::vector<option_binding> options;
};

/**
 * @brief Add one unbound manifest parameter to a CLI11 scope.
 *
 * @param app Target root or command scope.
 * @param declaration Validated parameter declaration.
 * @return Created CLI11 option.
 */
CLI::Option* add_dynamic_option(CLI::App& app, const parameter& declaration)
{
    std::string name;
    if (declaration.positional_index == v1::no_position) {
        name = "--" + declaration.long_name;
    } else {
        name = declaration.parameter_id;
    }

    CLI::Option* option = nullptr;
    if (declaration.kind == v1::flag) {
        option = app.add_flag(name, declaration.help);
        option->disable_flag_override(true);
    } else {
        option = app.add_option(name, declaration.help);
        option->type_name(declaration.value_name);
        option->expected(1);
        if ((declaration.flags & v1::parameter_repeatable) != 0) {
            option->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
        } else {
            option->multi_option_policy(CLI::MultiOptionPolicy::Throw);
        }
    }
    if ((declaration.flags & v1::parameter_required) != 0) option->required();
    return option;
}

/**
 * @brief Append one string as a JSON string literal.
 *
 * @param value Raw UTF-8 text.
 * @param[in,out] out JSON destination.
 */
void append_json_string(std::string_view value, std::string& out)
{
    static constexpr char hex[] = "0123456789abcdef";
    out.push_back('"');
    for (const unsigned char byte : value) {
        switch (byte) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (byte < 0x20U) {
                    out += "\\u00";
                    out.push_back(hex[(byte >> 4U) & 0x0fU]);
                    out.push_back(hex[byte & 0x0fU]);
                } else {
                    out.push_back(static_cast<char>(byte));
                }
                break;
        }
    }
    out.push_back('"');
}

/**
 * @brief Resolve explicit CLI11 results or plugin defaults for one parameter.
 *
 * @param declaration Parameter declaration.
 * @param option Parsed CLI11 option.
 * @param normalized Preflight occurrences and preserved positional values.
 * @return Resolved argument preserving absence, source, and explicit empty strings.
 */
argument resolve_argument(const parameter& declaration, const CLI::Option& option,
                          const normalized_invocation& normalized)
{
    argument resolved;
    resolved.parameter_id = declaration.parameter_id;
    resolved.kind = declaration.kind;
    const auto found = normalized.occurrences.find(&declaration);
    resolved.explicitly_provided =
        found != normalized.occurrences.end() && found->second != 0;
    if (resolved.explicitly_provided) {
        if (declaration.kind == v1::flag) {
            const CLI::results_t& raw = option.results();
            (void)raw;
            resolved.values.emplace_back("true");
        } else if (declaration.positional_index != v1::no_position) {
            const CLI::results_t& placeholders = option.results();
            (void)placeholders;
            resolved.values = normalized.positional_values.at(&declaration);
        } else {
            const CLI::results_t& raw = option.results();
            resolved.values.assign(raw.begin(), raw.end());
        }
    } else {
        resolved.values = declaration.default_values;
    }
    return resolved;
}

/**
 * @brief Find or create one plugin configuration in stable first-declaration order.
 *
 * @param plugin_id Owning plugin identity.
 * @param[in,out] configurations Configuration list.
 * @return Mutable configuration for the requested plugin.
 */
plugin_configuration& ensure_configuration(const std::string& plugin_id,
                                           std::vector<plugin_configuration>& configurations)
{
    for (plugin_configuration& value : configurations) {
        if (value.plugin_id == plugin_id) return value;
    }
    configurations.push_back(plugin_configuration{plugin_id, {}});
    return configurations.back();
}

/**
 * @brief Append one present resolved value to an owning plugin's configuration.
 *
 * @param key Stable configuration key.
 * @param declaration Source declaration.
 * @param resolved Resolved parameter value.
 * @param[in,out] configuration Owning plugin snapshot under construction.
 */
void append_configuration(std::string key, const parameter& declaration,
                          const argument& resolved, plugin_configuration& configuration)
{
    if (resolved.values.empty()) return;
    configuration_value value;
    value.key = std::move(key);
    value.kind = declaration.kind;
    value.source = resolved.explicitly_provided ? v1::command_line : v1::plugin_default;
    value.flags = (declaration.flags & v1::parameter_sensitive) != 0 ? v1::config_sensitive : 0;
    value.values = resolved.values;
    configuration.entries.push_back(std::move(value));
}

/**
 * @brief Serialize selected non-config arguments as deterministic JSON.
 *
 * @param owner Selected command declaration.
 * @param arguments Resolved non-config arguments in declaration order.
 * @return Stable JSON object using parameter_id keys.
 */
std::string make_payload(const command& owner, const std::vector<argument>& arguments)
{
    std::string payload = "{";
    bool first = true;
    for (const argument& value : arguments) {
        const auto declaration = std::find_if(
            owner.parameters.begin(), owner.parameters.end(), [&value](const parameter& candidate) {
                return candidate.parameter_id == value.parameter_id;
            });
        if (declaration == owner.parameters.end() || value.values.empty()) continue;
        if (!first) payload.push_back(',');
        first = false;
        append_json_string(value.parameter_id, payload);
        payload.push_back(':');
        if (value.kind == v1::flag) {
            payload += value.values.front() == "true" ? "true" : "false";
        } else if ((declaration->flags & v1::parameter_repeatable) != 0) {
            payload.push_back('[');
            for (std::size_t index = 0; index < value.values.size(); ++index) {
                if (index != 0) payload.push_back(',');
                append_json_string(value.values[index], payload);
            }
            payload.push_back(']');
        } else {
            append_json_string(value.values.front(), payload);
        }
    }
    payload.push_back('}');
    return payload;
}

/**
 * @brief Convert one ABI status to a stable diagnostic word.
 *
 * @param value ABI status code.
 * @return Symbolic status name, or its decimal value when unknown.
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
 * @brief Combine an ABI status with an optional detailed diagnostic.
 *
 * @param status Failing ABI status.
 * @param detail Optional host or loader diagnostic.
 * @return User-facing status text.
 */
std::string describe_status(abi::status status, const std::string& detail)
{
    if (detail.empty()) return status_text(status);
    return status_text(status) + ": " + detail;
}

/**
 * @brief Borrow one string as ABI bytes for the duration of a synchronous call.
 *
 * @param value Caller-owned string.
 * @return Bounded borrowed bytes.
 */
abi::bytes as_bytes(const std::string& value) noexcept
{
    if (value.empty()) return {nullptr, 0};
    return {value.data(), static_cast<std::uint64_t>(value.size())};
}

/**
 * @brief Write one complete stdout record and verify the flushed stream state.
 *
 * @param value Bytes to write exactly.
 * @param append_newline Whether to append one host-owned newline after the bytes.
 * @param[out] error Failure diagnostic.
 * @return True only when writing and flushing stdout succeed.
 */
bool write_stdout(std::string_view value, bool append_newline, std::string& error)
{
    error.clear();
    try {
        if (value.size() > static_cast<std::size_t>(
                               std::numeric_limits<std::streamsize>::max())) {
            error = "stdout record exceeds the stream size limit";
            return false;
        }
        if (!value.empty()) {
            std::cout.write(value.data(), static_cast<std::streamsize>(value.size()));
        }
        if (append_newline) std::cout.put('\n');
        std::cout.flush();
    } catch (const std::ios_base::failure& failure) {
        error = std::string("stdout write failed: ") + failure.what();
        return false;
    }
    if (!std::cout) {
        error = "stdout write failed";
        return false;
    }
    return true;
}

/**
 * @brief Locate the default plugin directory beside the real executable.
 *
 * @param program argv[0], used only when the platform does not expose the running image path.
 * @return The executable directory joined with @c plugins, or a current-directory fallback.
 */
std::filesystem::path default_plugins_dir(const char* program)
{
#if defined(_WIN32)
    std::vector<wchar_t> image(1024);
    for (;;) {
        const DWORD length =
            ::GetModuleFileNameW(nullptr, image.data(), static_cast<DWORD>(image.size()));
        if (length == 0) break;
        if (length < image.size()) {
            return std::filesystem::path(image.data(), image.data() + length).parent_path() /
                   "plugins";
        }
        if (image.size() > static_cast<std::size_t>(std::numeric_limits<DWORD>::max() / 2U)) {
            break;
        }
        image.resize(image.size() * 2U);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    (void)::_NSGetExecutablePath(nullptr, &size);
    if (size != 0) {
        std::vector<char> image(size);
        if (::_NSGetExecutablePath(image.data(), &size) == 0) {
            std::error_code image_error;
            const std::filesystem::path resolved =
                std::filesystem::canonical(image.data(), image_error);
            if (!image_error && !resolved.empty()) return resolved.parent_path() / "plugins";
        }
    }
#elif defined(__linux__)
    std::error_code self_error;
    const std::filesystem::path self =
        std::filesystem::read_symlink("/proc/self/exe", self_error);
    if (!self_error && !self.empty()) return self.parent_path() / "plugins";
#endif
    if (program != nullptr && *program != '\0') {
        std::error_code path_error;
        const std::filesystem::path resolved =
            std::filesystem::weakly_canonical(program, path_error);
        if (!path_error && !resolved.empty()) return resolved.parent_path() / "plugins";
    }
    std::error_code current_error;
    const std::filesystem::path current = std::filesystem::current_path(current_error);
    return current_error || current.empty() ? std::filesystem::path("plugins")
                                            : current / "plugins";
}

} // namespace

/**
 * @brief Parse normalized argv through a dynamically constructed CLI11 command surface.
 *
 * @param surface Deep-copied and validated declarations.
 * @param argc Argument count including argv[0].
 * @param argv Borrowed command-line vector.
 * @param[out] out Parsed invocation or rendered help.
 * @param[out] error Optional user-facing diagnostic.
 * @return ok, invalid_argument for user syntax, or failed for frontend construction failure.
 */
abi::status parse_invocation(const catalog& surface, int argc, const char* const* argv,
                             parse_result* out, std::string* error)
{
    if (error != nullptr) error->clear();
    if (out == nullptr) {
        assign_error(error, "parse result is null");
        return abi::invalid_argument;
    }
    *out = parse_result{};
    try {
        normalized_invocation normalized;
        std::string diagnostic;
        abi::status status = normalize_argv(surface, argc, argv, normalized, diagnostic);
        if (status != abi::ok) {
            assign_error(error, std::move(diagnostic));
            return status;
        }

        CLI::App app("42U plugin host");
        restrict_cli11(app);
        app.add_flag("--version", "Show version and exit")->disable_flag_override(true);
        app.require_subcommand(1, 1);

        std::vector<option_binding> root_bindings;
        root_bindings.reserve(surface.root_parameters().size());
        for (const parameter& declaration : surface.root_parameters()) {
            root_bindings.push_back({&declaration, add_dynamic_option(app, declaration)});
        }

        std::vector<command_binding> command_bindings;
        command_bindings.reserve(surface.commands().size());
        for (const command& declaration : surface.commands()) {
            CLI::App* subcommand = app.add_subcommand(declaration.name, declaration.help);
            restrict_cli11(*subcommand);
            command_binding binding{&declaration, subcommand, {}};
            binding.options.reserve(declaration.parameters.size());
            for (const parameter& parameter_value : declaration.parameters) {
                if (parameter_value.positional_index == v1::no_position) {
                    binding.options.push_back(
                        {&parameter_value, add_dynamic_option(*subcommand, parameter_value)});
                }
            }
            std::vector<const parameter*> positionals;
            for (const parameter& parameter_value : declaration.parameters) {
                if (parameter_value.positional_index != v1::no_position) {
                    positionals.push_back(&parameter_value);
                }
            }
            std::sort(positionals.begin(), positionals.end(), [](const parameter* left,
                                                                 const parameter* right) {
                return left->positional_index < right->positional_index;
            });
            for (const parameter* parameter_value : positionals) {
                binding.options.push_back(
                    {parameter_value, add_dynamic_option(*subcommand, *parameter_value)});
            }
            command_bindings.push_back(std::move(binding));
        }

        std::vector<const char*> normalized_pointers;
        normalized_pointers.reserve(normalized.argv.size());
        for (const std::string& token : normalized.argv) normalized_pointers.push_back(token.c_str());
        try {
            app.parse(static_cast<int>(normalized_pointers.size()), normalized_pointers.data());
        } catch (const CLI::CallForHelp&) {
            if (normalized.selected == nullptr) {
                out->kind = parse_kind::root_help;
                out->rendered_help = app.help();
            } else {
                out->kind = parse_kind::command_help;
                const auto binding = std::find_if(
                    command_bindings.begin(), command_bindings.end(),
                    [&normalized](const command_binding& candidate) {
                        return candidate.declaration == normalized.selected;
                    });
                if (binding == command_bindings.end()) {
                    assign_error(error, "selected command has no CLI11 binding");
                    return abi::failed;
                }
                out->rendered_help = binding->app->help();
            }
            return abi::ok;
        } catch (const CLI::ParseError& parse_error) {
            assign_error(error, parse_error.what());
            return abi::invalid_argument;
        }

        const std::vector<CLI::App*> selected_apps = app.get_subcommands();
        if (selected_apps.size() != 1 || normalized.selected == nullptr ||
            selected_apps.front()->count() == 0) {
            assign_error(error, "exactly one command must be selected");
            return abi::invalid_argument;
        }
        const auto selected_binding = std::find_if(
            command_bindings.begin(), command_bindings.end(),
            [&normalized](const command_binding& candidate) {
                return candidate.declaration == normalized.selected;
            });
        if (selected_binding == command_bindings.end() || selected_binding->app != selected_apps.front()) {
            assign_error(error, "CLI11 selected a command outside the validated catalog");
            return abi::failed;
        }

        invocation parsed;
        for (const parameter& declaration : surface.root_parameters()) {
            (void)ensure_configuration(declaration.plugin_id, parsed.configurations);
        }
        for (const command& declaration : surface.commands()) {
            (void)ensure_configuration(declaration.plugin_id, parsed.configurations);
        }
        for (const option_binding& binding : root_bindings) {
            argument resolved =
                resolve_argument(*binding.declaration, *binding.option, normalized);
            if (!values_allowed(*binding.declaration, resolved.values, diagnostic)) {
                assign_error(error, std::move(diagnostic));
                return abi::invalid_argument;
            }
            plugin_configuration& configuration =
                ensure_configuration(binding.declaration->plugin_id, parsed.configurations);
            append_configuration(binding.declaration->parameter_id, *binding.declaration, resolved,
                                 configuration);
        }

        selected_command selected;
        selected.plugin_id = normalized.selected->plugin_id;
        selected.command_id = normalized.selected->command_id;
        selected.handler = normalized.selected->handler;
        for (const parameter& declaration : normalized.selected->parameters) {
            const auto binding = std::find_if(
                selected_binding->options.begin(), selected_binding->options.end(),
                [&declaration](const option_binding& candidate) {
                    return candidate.declaration == &declaration;
                });
            if (binding == selected_binding->options.end()) {
                assign_error(error, "selected parameter has no CLI11 binding");
                return abi::failed;
            }
            argument resolved = resolve_argument(declaration, *binding->option, normalized);
            if ((declaration.flags & v1::parameter_required) != 0 &&
                !resolved.explicitly_provided) {
                const std::string name = declaration.long_name.empty()
                                             ? declaration.value_name
                                             : "--" + declaration.long_name;
                assign_error(error, "required parameter '" + name + "' was not provided");
                return abi::invalid_argument;
            }
            if (!values_allowed(declaration, resolved.values, diagnostic)) {
                assign_error(error, std::move(diagnostic));
                return abi::invalid_argument;
            }
            if (!declaration.config_key.empty()) {
                plugin_configuration& configuration =
                    ensure_configuration(declaration.plugin_id, parsed.configurations);
                append_configuration(declaration.config_key, declaration, resolved, configuration);
            } else if (!resolved.values.empty()) {
                selected.arguments.push_back(std::move(resolved));
            }
        }
        selected.payload = make_payload(*normalized.selected, selected.arguments);
        parsed.command = std::move(selected);
        out->kind = parse_kind::execute;
        out->value = std::move(parsed);
        return abi::ok;
    } catch (const std::bad_alloc&) {
        assign_error(error, "allocation failed while constructing the command line");
        return abi::failed;
    } catch (const std::exception& exception) {
        assign_error(error, std::string("command-line frontend failed: ") + exception.what());
        return abi::failed;
    } catch (...) {
        assign_error(error, "command-line frontend failed: unknown exception");
        return abi::failed;
    }
}

/** @brief Hidden state retained across application runs. */
struct application::impl {
    /**
     * @brief Store application construction options.
     *
     * @param value Owned version and runtime bounds.
     */
    explicit impl(application_options value) : options(std::move(value)) {}

    application_options options;
    std::string error;
};

/**
 * @brief Construct a pure-host CLI application.
 *
 * @param options Owned version text and runtime bounds.
 */
application::application(application_options options)
    : impl_(std::make_unique<impl>(std::move(options)))
{
}

/** @brief Destroy the hidden application state. */
application::~application() = default;

/**
 * @brief Transfer application state.
 *
 * @param other Source application.
 */
application::application(application&& other) noexcept = default;

/**
 * @brief Replace this application by transferring another state.
 *
 * @param other Source application.
 * @return This application.
 */
application& application::operator=(application&& other) noexcept = default;

/**
 * @brief Execute discovery, parsing, activation, one handler, and explicit shutdown.
 *
 * @param argc Number of command-line arguments, including argv[0].
 * @param argv Borrowed argument vector.
 * @return exit_ok, exit_failure, or exit_usage according to the public process contract.
 */
int application::run(int argc, const char* const* argv)
{
    if (!impl_) return exit_failure;
    impl_->error.clear();
    std::unique_ptr<host> rack;
    try {
        const auto record_error = [this](const std::string& stage, abi::status status,
                                         const std::string& detail) {
            const std::string diagnostic = stage + ": " + describe_status(status, detail);
            std::cerr << "error: " << diagnostic << '\n';
            if (impl_->error.empty()) {
                impl_->error = diagnostic;
            } else {
                impl_->error += "\n" + diagnostic;
            }
        };

        if (argc == 2 && argv != nullptr && argv[1] != nullptr &&
            std::string_view(argv[1]) == "--version") {
            std::string output_error;
            if (!write_stdout(impl_->options.version, true, output_error)) {
                record_error("write version", abi::failed, output_error);
                return exit_failure;
            }
            return exit_ok;
        }

        std::filesystem::path directory;
        const char* const configured_directory = std::getenv("U42_PLUGIN_DIR");
        if (configured_directory != nullptr) {
            if (*configured_directory == '\0') {
                record_error("plugin directory", abi::invalid_argument,
                             "U42_PLUGIN_DIR is set but empty");
                return exit_failure;
            }
            directory = configured_directory;
        } else {
            const char* const program =
                argc > 0 && argv != nullptr && argv[0] != nullptr ? argv[0] : "";
            directory = default_plugins_dir(program);
        }

        std::vector<std::filesystem::path> candidates;
        std::string diagnostic;
        abi::status status = scan_plugins(directory, candidates, diagnostic);
        if (status != abi::ok) {
            record_error("scan '" + directory.string() + "'", status, diagnostic);
            return exit_failure;
        }

        catalog surface;
        std::vector<plugin_activation> activations;
        activations.reserve(candidates.size());
        std::set<std::string> identities;
        for (const std::filesystem::path& candidate : candidates) {
            discovered_plugin discovered;
            status = discovered.open(candidate, diagnostic);
            if (status != abi::ok) {
                record_error("discover '" + candidate.string() + "'", status, diagnostic);
                return exit_failure;
            }
            const std::string& plugin_id = discovered.description().plug_id;
            if (!identities.insert(plugin_id).second) {
                record_error("discover '" + candidate.string() + "'", abi::duplicate,
                             "plugin identity '" + plugin_id + "' was discovered more than once");
                return exit_failure;
            }
            if (discovered.manifest() != nullptr) {
                status = surface.add(plugin_id, discovered.manifest(), &diagnostic);
                if (status != abi::ok) {
                    record_error("manifest for '" + plugin_id + "'", status, diagnostic);
                    return exit_failure;
                }
            }
            plugin_activation activation;
            activation.plugin = std::move(discovered);
            activations.push_back(std::move(activation));
        }

        parse_result parsed;
        status = parse_invocation(surface, argc, argv, &parsed, &diagnostic);
        if (status != abi::ok) {
            record_error("command line", status, diagnostic);
            return status == abi::invalid_argument ? exit_usage : exit_failure;
        }
        if (parsed.kind == parse_kind::root_help || parsed.kind == parse_kind::command_help) {
            const bool append_newline =
                parsed.rendered_help.empty() || parsed.rendered_help.back() != '\n';
            if (!write_stdout(parsed.rendered_help, append_newline, diagnostic)) {
                record_error("write help", abi::failed, diagnostic);
                return exit_failure;
            }
            return exit_ok;
        }
        if (!parsed.value.command.has_value()) {
            record_error("command line", abi::failed,
                         "successful execution parse did not select a command");
            return exit_failure;
        }

        for (const plugin_configuration& configuration : parsed.value.configurations) {
            const auto activation = std::find_if(
                activations.begin(), activations.end(),
                [&configuration](const plugin_activation& candidate) {
                    return candidate.plugin.description().plug_id == configuration.plugin_id;
                });
            if (activation == activations.end()) {
                record_error("configuration", abi::failed,
                             "no discovered plugin owns configuration for '" +
                                 configuration.plugin_id + "'");
                return exit_failure;
            }
            activation->config.reserve(configuration.entries.size());
            for (const configuration_value& source : configuration.entries) {
                plugin_config_value value;
                value.key = source.key;
                value.kind = source.kind;
                value.source = source.source;
                value.flags = source.flags;
                value.values = source.values;
                activation->config.push_back(std::move(value));
            }
        }

        const selected_command& selected = *parsed.value.command;
        rack = std::make_unique<host>(impl_->options.runtime);
        int code = exit_ok;
        bool command_succeeded = false;
        std::string result;

        status = rack->adopt(std::move(activations));
        if (status != abi::ok) {
            record_error("adopt plugins", status, rack->error());
            code = exit_failure;
        } else {
            status = rack->start();
            if (status != abi::ok) {
                record_error("start plugins", status, rack->error());
                code = exit_failure;
            } else {
                abi::plugin_version actual{};
                status = rack->version(selected.plugin_id, &actual);
                if (status != abi::ok) {
                    record_error("resolve handler owner '" + selected.plugin_id + "'", status,
                                 rack->error());
                    code = exit_failure;
                } else {
                    status = rack->call(selected.plugin_id, abi::exact_version(actual),
                                        selected.handler, as_bytes(selected.payload), &result);
                    if (status != abi::ok) {
                        record_error("execute command '" + selected.command_id + "'", status,
                                     rack->error());
                        code = exit_failure;
                    } else {
                        command_succeeded = true;
                    }
                }
            }
        }

        const abi::status shutdown_status = rack->shutdown();
        if (shutdown_status != abi::ok) {
            record_error("shutdown", shutdown_status, rack->error());
            code = exit_failure;
        }
        if (command_succeeded && !result.empty() &&
            !write_stdout(result, false, diagnostic)) {
            record_error("write command result", abi::failed, diagnostic);
            code = exit_failure;
        }
        return code;
    } catch (const std::bad_alloc&) {
        impl_->error = "application: allocation failed";
    } catch (const std::exception& exception) {
        impl_->error = std::string("application: unexpected exception: ") + exception.what();
    } catch (...) {
        impl_->error = "application: unknown exception";
    }
    if (rack) {
        const abi::status shutdown_status = rack->shutdown();
        if (shutdown_status != abi::ok) {
            try {
                impl_->error += "\nshutdown: " +
                                describe_status(shutdown_status, rack->error());
            } catch (...) {
            }
        }
    }
    std::cerr << "error: " << impl_->error << '\n';
    return exit_failure;
}

/**
 * @brief Return the latest application diagnostic.
 *
 * @return Borrowed diagnostic text owned by the application.
 */
const std::string& application::error() const noexcept
{
    static const std::string empty;
    return impl_ ? impl_->error : empty;
}

} // namespace u42::cli
