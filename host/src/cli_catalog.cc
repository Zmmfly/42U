#include <42u/cli_host.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace u42::cli {
namespace {

namespace a = abi::v3;

constexpr std::size_t max_plugin_id_bytes = 64u * 1024u;

struct copy_budget {
    std::uint64_t used = 0;

    a::status consume(std::uint64_t bytes, const std::string& field, std::string& error)
    {
        if (bytes > v1::max_manifest_copy_bytes - used) {
            error = field + " exceeds the per-plugin manifest copy budget of " +
                    std::to_string(v1::max_manifest_copy_bytes) + " bytes";
            return a::limit_exceeded;
        }
        used += bytes;
        return a::ok;
    }

    template <class T>
    a::status consume_array(std::uint64_t count, const std::string& field, std::string& error)
    {
        constexpr std::uint64_t width = sizeof(T);
        if (count > std::numeric_limits<std::uint64_t>::max() / width) {
            error = field + " byte size overflows uint64_t";
            return a::limit_exceeded;
        }
        return consume(count * width, field, error);
    }
};

/**
 * @brief Validate UTF-8 without accepting overlong forms, surrogates, or out-of-range points.
 */
bool valid_utf8(const char* data, std::size_t size) noexcept
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    std::size_t index = 0;
    while (index < size) {
        const unsigned char lead = bytes[index];
        if (lead < 0x80u) {
            ++index;
            continue;
        }

        std::size_t extra = 0;
        std::uint32_t point = 0;
        if ((lead & 0xE0u) == 0xC0u) {
            extra = 1;
            point = lead & 0x1Fu;
            if (point < 2u) return false;
        } else if ((lead & 0xF0u) == 0xE0u) {
            extra = 2;
            point = lead & 0x0Fu;
        } else if ((lead & 0xF8u) == 0xF0u) {
            extra = 3;
            point = lead & 0x07u;
            if (point > 4u) return false;
        } else {
            return false;
        }

        if (index + extra >= size) return false;
        for (std::size_t offset = 1; offset <= extra; ++offset) {
            const unsigned char trail = bytes[index + offset];
            if ((trail & 0xC0u) != 0x80u) return false;
            point = (point << 6) | (trail & 0x3Fu);
        }
        if (extra == 2 &&
            (point < 0x800u || (point >= 0xD800u && point <= 0xDFFFu)))
            return false;
        if (extra == 3 && (point < 0x10000u || point > 0x10FFFFu)) return false;
        index += extra + 1;
    }
    return true;
}

a::status copy_text(v1::text_view source, std::uint64_t limit, bool required,
                    const std::string& field, copy_budget& budget, std::string& out,
                    std::string& error)
{
    if (!v1::valid_view(source)) {
        error = field + " has a null data pointer with a non-zero size";
        return a::invalid_argument;
    }
    if (source.size > limit) {
        error = field + " is " + std::to_string(source.size) +
                " bytes, exceeding the limit of " + std::to_string(limit);
        return a::limit_exceeded;
    }
    if (source.size == 0) {
        if (required) {
            error = field + " must not be empty";
            return a::invalid_argument;
        }
        out.clear();
        return a::ok;
    }
    if (std::memchr(source.data, '\0', static_cast<std::size_t>(source.size)) != nullptr) {
        error = field + " contains an embedded NUL byte";
        return a::invalid_argument;
    }
    if (!valid_utf8(source.data, static_cast<std::size_t>(source.size))) {
        error = field + " is not valid UTF-8";
        return a::invalid_argument;
    }
    const a::status charged = budget.consume(source.size, field, error);
    if (charged != a::ok) return charged;
    out.assign(source.data, static_cast<std::size_t>(source.size));
    return a::ok;
}

bool ascii_letter(char value) noexcept
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

bool ascii_digit(char value) noexcept
{
    return value >= '0' && value <= '9';
}

a::status validate_stable_id(const std::string& value, const std::string& field,
                             std::string& error)
{
    if (value.empty() || !ascii_letter(value.front())) {
        error = field + " must begin with an ASCII letter";
        return a::invalid_argument;
    }
    for (const char character : value) {
        if (!ascii_letter(character) && !ascii_digit(character) && character != '_' &&
            character != '-' && character != '.') {
            error = field +
                    " may contain only ASCII letters, digits, underscore, hyphen, and dot";
            return a::invalid_argument;
        }
    }
    return a::ok;
}

a::status validate_visible_name(const std::string& value, const std::string& field,
                                std::string& error)
{
    if (value.empty() || value.front() < 'a' || value.front() > 'z') {
        error = field + " must begin with a lowercase ASCII letter";
        return a::invalid_argument;
    }
    bool previous_hyphen = false;
    for (const char character : value) {
        const bool hyphen = character == '-';
        if (!hyphen && !(character >= 'a' && character <= 'z') && !ascii_digit(character)) {
            error = field + " may contain only lowercase ASCII letters, digits, and hyphens";
            return a::invalid_argument;
        }
        if (hyphen && previous_hyphen) {
            error = field + " contains consecutive hyphens";
            return a::invalid_argument;
        }
        previous_hyphen = hyphen;
    }
    if (value.back() == '-') {
        error = field + " must not end with a hyphen";
        return a::invalid_argument;
    }
    return a::ok;
}

bool reserved_name(const std::string& value) noexcept
{
    return value == "help" || value == "version";
}

std::string root_ref(const parameter& value)
{
    return "plugin '" + value.plugin_id + "' declaration '" + value.parameter_id + "'";
}

std::string command_ref(const command& value)
{
    return "plugin '" + value.plugin_id + "' declaration '" + value.command_id + "'";
}

std::string local_ref(const command& owner, const parameter& value)
{
    return "plugin '" + value.plugin_id + "' declaration '" + owner.command_id + "/" +
           value.parameter_id + "'";
}

a::status reserved_conflict(const std::string& name, const std::string& declaration,
                            std::string& error)
{
    error = declaration + " conflicts with plugin '<host>' declaration '" + name + "'";
    return a::duplicate;
}

a::status copy_values(v1::array_view<v1::text_view> source, std::uint64_t count_limit,
                      const std::string& field, copy_budget& budget,
                      std::vector<std::string>& out, std::string& error)
{
    if (!v1::valid_view(source)) {
        error = field + " has a null array pointer with a non-zero count";
        return a::invalid_argument;
    }
    if (source.size > count_limit) {
        error = field + " declares " + std::to_string(source.size) +
                " entries, exceeding the limit of " + std::to_string(count_limit);
        return a::limit_exceeded;
    }
    a::status result = budget.consume_array<v1::text_view>(source.size, field, error);
    if (result != a::ok) return result;

    out.clear();
    out.reserve(static_cast<std::size_t>(source.size));
    for (std::uint64_t index = 0; index < source.size; ++index) {
        std::string copied;
        result = copy_text(source.data[index], v1::max_declared_value_bytes, false,
                           field + "[" + std::to_string(index) + "]", budget, copied, error);
        if (result != a::ok) return result;
        out.push_back(std::move(copied));
    }
    return a::ok;
}

a::status validate_value_constraints(const parameter& value, const std::string& declaration,
                                     std::string& error)
{
    if (value.kind == v1::flag) {
        if ((value.flags & v1::parameter_repeatable) != 0) {
            error = declaration + " declares a repeatable flag";
            return a::invalid_argument;
        }
        if (!value.value_name.empty() || !value.default_values.empty() ||
            !value.allowed_values.empty()) {
            error = declaration +
                    " is a flag and must not declare value_name, defaults, or allowed values";
            return a::invalid_argument;
        }
        return a::ok;
    }

    if ((value.flags & v1::parameter_repeatable) == 0 && value.default_values.size() > 1) {
        error = declaration + " is not repeatable but declares multiple default values";
        return a::invalid_argument;
    }
    if ((value.flags & v1::parameter_required) != 0 && !value.default_values.empty()) {
        error = declaration + " is required and must not declare a default value";
        return a::invalid_argument;
    }
    for (std::size_t index = 0; index < value.allowed_values.size(); ++index) {
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            if (value.allowed_values[index] == value.allowed_values[earlier]) {
                error = declaration + " repeats an allowed value";
                return a::duplicate;
            }
        }
    }
    if (!value.allowed_values.empty()) {
        for (const std::string& item : value.default_values) {
            if (std::find(value.allowed_values.begin(), value.allowed_values.end(), item) ==
                value.allowed_values.end()) {
                error = declaration + " has a default value outside its allowed values";
                return a::invalid_argument;
            }
        }
    }
    return a::ok;
}

a::status copy_parameter(const std::string& plugin_id, const v1::parameter_desc& source,
                         bool root, const std::string& command_id, copy_budget& budget,
                         parameter& out, std::string& error)
{
    const std::string field =
        root ? "root parameter" : "command '" + command_id + "' parameter";
    if (source.struct_size != sizeof(v1::parameter_desc)) {
        error = field + " has struct_size " + std::to_string(source.struct_size) +
                ", expected " + std::to_string(sizeof(v1::parameter_desc));
        return a::invalid_argument;
    }
    if (source.reserved != 0) {
        error = field + " has a non-zero reserved field";
        return a::invalid_argument;
    }
    if (!v1::valid_parameter_flags(source.flags)) {
        error = field + " contains unknown parameter flags";
        return a::unsupported;
    }
    if (!v1::valid_parameter_kind(source.kind)) {
        error = field + " contains unsupported parameter kind " + std::to_string(source.kind);
        return a::unsupported;
    }

    out = parameter{};
    out.plugin_id = plugin_id;
    out.flags = source.flags;
    out.kind = source.kind;
    out.positional_index = source.positional_index;

    a::status result = copy_text(source.param_id, v1::max_identifier_bytes, true,
                                 field + ".param_id", budget, out.parameter_id, error);
    if (result != a::ok) return result;
    result = validate_stable_id(out.parameter_id, field + ".param_id", error);
    if (result != a::ok) return result;

    const std::string declaration = root_ref(out);
    result = copy_text(source.long_name, v1::max_cli_name_bytes, false,
                       declaration + ".long_name", budget, out.long_name, error);
    if (result != a::ok) return result;
    result = copy_text(source.help, v1::max_help_bytes, false, declaration + ".help", budget,
                       out.help, error);
    if (result != a::ok) return result;
    result = copy_text(source.value_name, v1::max_value_name_bytes, false,
                       declaration + ".value_name", budget, out.value_name, error);
    if (result != a::ok) return result;
    result = copy_text(source.config_key, v1::max_identifier_bytes, false,
                       declaration + ".config_key", budget, out.config_key, error);
    if (result != a::ok) return result;
    if (!out.config_key.empty()) {
        result = validate_stable_id(out.config_key, declaration + ".config_key", error);
        if (result != a::ok) return result;
    }
    result = copy_values(source.default_values, v1::max_default_values,
                         declaration + ".default_values", budget, out.default_values, error);
    if (result != a::ok) return result;
    result = copy_values(source.allowed_values, v1::max_allowed_values,
                         declaration + ".allowed_values", budget, out.allowed_values, error);
    if (result != a::ok) return result;

    if (root) {
        if (out.long_name.empty() || out.positional_index != v1::no_position) {
            error = declaration + " must be a root long option";
            return a::invalid_argument;
        }
        if ((out.flags & v1::parameter_required) != 0) {
            error = declaration + " is a root parameter and must not be required";
            return a::invalid_argument;
        }
        if (!out.config_key.empty()) {
            error = declaration + " is a root parameter and must not declare config_key";
            return a::invalid_argument;
        }
    } else {
        const bool positional = out.positional_index != v1::no_position;
        if (positional == !out.long_name.empty()) {
            error = declaration +
                    " must declare exactly one of long_name or positional_index";
            return a::invalid_argument;
        }
        if (positional && out.kind != v1::text) {
            error = declaration + " is positional and must have text kind";
            return a::invalid_argument;
        }
        if (positional && (out.flags & v1::parameter_repeatable) != 0) {
            error = declaration + " is positional and must not be repeatable";
            return a::invalid_argument;
        }
    }

    if (!out.long_name.empty()) {
        result = validate_visible_name(out.long_name, declaration + ".long_name", error);
        if (result != a::ok) return result;
        if (reserved_name(out.long_name))
            return reserved_conflict("--" + out.long_name, declaration, error);
    }
    return validate_value_constraints(out, declaration, error);
}

a::status validate_local_uniqueness(const command& value, std::string& error)
{
    struct positional_declaration {
        std::uint32_t index;
        const parameter* value;
    };

    std::vector<positional_declaration> positions;
    for (std::size_t index = 0; index < value.parameters.size(); ++index) {
        const parameter& current = value.parameters[index];
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            const parameter& known = value.parameters[earlier];
            if (current.parameter_id == known.parameter_id) {
                error = local_ref(value, current) + " conflicts with " + local_ref(value, known) +
                        " because the stable parameter id is repeated";
                return a::duplicate;
            }
            if (!current.long_name.empty() && current.long_name == known.long_name) {
                error = local_ref(value, current) + " conflicts with " + local_ref(value, known) +
                        " on local option '--" + current.long_name + "'";
                return a::duplicate;
            }
            if (current.positional_index != v1::no_position &&
                current.positional_index == known.positional_index) {
                error = local_ref(value, current) + " conflicts with " + local_ref(value, known) +
                        " on positional index " + std::to_string(current.positional_index);
                return a::duplicate;
            }
            if (!current.config_key.empty() && current.config_key == known.config_key) {
                error = local_ref(value, current) + " conflicts with " + local_ref(value, known) +
                        " on plugin configuration key '" + current.config_key + "'";
                return a::duplicate;
            }
        }
        if (current.positional_index != v1::no_position)
            positions.push_back({current.positional_index, &current});
    }

    std::sort(positions.begin(), positions.end(),
              [](const positional_declaration& left, const positional_declaration& right) {
                  return left.index < right.index;
              });
    const parameter* first_optional = nullptr;
    for (std::size_t index = 0; index < positions.size(); ++index) {
        if (positions[index].index != index) {
            error = command_ref(value) +
                    " has non-contiguous positional indices; expected index " +
                    std::to_string(index) + " but found " +
                    std::to_string(positions[index].index);
            return a::invalid_argument;
        }
        const bool required =
            (positions[index].value->flags & v1::parameter_required) != 0;
        if (!required && first_optional == nullptr) first_optional = positions[index].value;
        if (required && first_optional != nullptr) {
            error = local_ref(value, *positions[index].value) + " conflicts with " +
                    local_ref(value, *first_optional) +
                    " because required positional parameters must form a prefix";
            return a::invalid_argument;
        }
    }
    return a::ok;
}

a::status copy_command(const std::string& plugin_id, const v1::command_desc& source,
                       copy_budget& budget, command& out, std::string& error)
{
    if (source.struct_size != sizeof(v1::command_desc)) {
        error = "command has struct_size " + std::to_string(source.struct_size) +
                ", expected " + std::to_string(sizeof(v1::command_desc));
        return a::invalid_argument;
    }
    if (source.reserved != 0 || source.reserved_handler != 0) {
        error = "command has a non-zero reserved field";
        return a::invalid_argument;
    }
    if (!v1::valid_command_flags(source.flags)) {
        error = "command contains unknown command flags";
        return a::unsupported;
    }
    if (!v1::valid_view(source.parameters)) {
        error = "command has a null parameter array with a non-zero count";
        return a::invalid_argument;
    }
    if (source.parameters.size > v1::max_command_parameters) {
        error = "command declares " + std::to_string(source.parameters.size) +
                " parameters, exceeding the limit of " +
                std::to_string(v1::max_command_parameters);
        return a::limit_exceeded;
    }

    out = command{};
    out.plugin_id = plugin_id;
    out.flags = source.flags;
    out.handler = source.handler;
    a::status result = copy_text(source.command_id, v1::max_identifier_bytes, true,
                                 "command.command_id", budget, out.command_id, error);
    if (result != a::ok) return result;
    result = validate_stable_id(out.command_id, "command.command_id", error);
    if (result != a::ok) return result;

    const std::string declaration = command_ref(out);
    result = copy_text(source.name, v1::max_cli_name_bytes, true, declaration + ".name", budget,
                       out.name, error);
    if (result != a::ok) return result;
    result = validate_visible_name(out.name, declaration + ".name", error);
    if (result != a::ok) return result;
    if (reserved_name(out.name)) return reserved_conflict(out.name, declaration, error);
    result = copy_text(source.help, v1::max_help_bytes, false, declaration + ".help", budget,
                       out.help, error);
    if (result != a::ok) return result;
    result = budget.consume_array<v1::parameter_desc>(source.parameters.size,
                                                      declaration + ".parameters", error);
    if (result != a::ok) return result;

    out.parameters.reserve(static_cast<std::size_t>(source.parameters.size));
    for (std::uint64_t index = 0; index < source.parameters.size; ++index) {
        parameter copied;
        result = copy_parameter(plugin_id, source.parameters.data[index], false, out.command_id,
                                budget, copied, error);
        if (result != a::ok) return result;
        out.parameters.push_back(std::move(copied));
    }
    return validate_local_uniqueness(out, error);
}

struct top_level_declaration {
    const std::string* plugin_id;
    const std::string* stable_id;
    std::string reference;
};

void append_root_ids(const std::vector<parameter>& roots,
                     std::vector<top_level_declaration>& out)
{
    for (const parameter& value : roots)
        out.push_back({&value.plugin_id, &value.parameter_id, root_ref(value)});
}

void append_command_ids(const std::vector<command>& commands,
                        std::vector<top_level_declaration>& out)
{
    for (const command& value : commands)
        out.push_back({&value.plugin_id, &value.command_id, command_ref(value)});
}

a::status validate_stable_ids(const std::vector<top_level_declaration>& existing,
                              const std::vector<top_level_declaration>& added,
                              const std::string& kind, std::string& error)
{
    for (std::size_t index = 0; index < added.size(); ++index) {
        for (const top_level_declaration& known : existing) {
            if (*added[index].plugin_id == *known.plugin_id &&
                *added[index].stable_id == *known.stable_id) {
                error = added[index].reference + " conflicts with " + known.reference +
                        " because the plugin-local " + kind + " id is repeated";
                return a::duplicate;
            }
        }
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            if (*added[index].plugin_id == *added[earlier].plugin_id &&
                *added[index].stable_id == *added[earlier].stable_id) {
                error = added[index].reference + " conflicts with " + added[earlier].reference +
                        " because the plugin-local " + kind + " id is repeated";
                return a::duplicate;
            }
        }
    }
    return a::ok;
}

a::status validate_top_level_ids(const std::vector<parameter>& existing_roots,
                                 const std::vector<command>& existing_commands,
                                 const std::vector<parameter>& added_roots,
                                 const std::vector<command>& added_commands,
                                 std::string& error)
{
    std::vector<top_level_declaration> existing;
    std::vector<top_level_declaration> added;
    existing.reserve(existing_roots.size());
    added.reserve(added_roots.size());
    append_root_ids(existing_roots, existing);
    append_root_ids(added_roots, added);
    a::status result = validate_stable_ids(existing, added, "root parameter", error);
    if (result != a::ok) return result;

    existing.clear();
    added.clear();
    existing.reserve(existing_commands.size());
    added.reserve(added_commands.size());
    append_command_ids(existing_commands, existing);
    append_command_ids(added_commands, added);
    return validate_stable_ids(existing, added, "command", error);
}

a::status validate_root_names(const std::vector<parameter>& existing,
                              const std::vector<parameter>& added, std::string& error)
{
    for (std::size_t index = 0; index < added.size(); ++index) {
        for (const parameter& known : existing) {
            if (added[index].long_name == known.long_name) {
                error = root_ref(added[index]) + " conflicts with " + root_ref(known) +
                        " on root option '--" + added[index].long_name + "'";
                return a::duplicate;
            }
        }
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            if (added[index].long_name == added[earlier].long_name) {
                error = root_ref(added[index]) + " conflicts with " + root_ref(added[earlier]) +
                        " on root option '--" + added[index].long_name + "'";
                return a::duplicate;
            }
        }
    }
    return a::ok;
}

a::status validate_command_names(const std::vector<command>& existing,
                                 const std::vector<command>& added, std::string& error)
{
    for (std::size_t index = 0; index < added.size(); ++index) {
        for (const command& known : existing) {
            if (added[index].name == known.name) {
                error = command_ref(added[index]) + " conflicts with " + command_ref(known) +
                        " on command name '" + added[index].name + "'";
                return a::duplicate;
            }
        }
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            if (added[index].name == added[earlier].name) {
                error = command_ref(added[index]) + " conflicts with " +
                        command_ref(added[earlier]) + " on command name '" +
                        added[index].name + "'";
                return a::duplicate;
            }
        }
    }
    return a::ok;
}

a::status validate_shadow_pair(const parameter& root, const command& owner,
                               const parameter& local, std::string& error)
{
    if (local.long_name.empty() || root.long_name != local.long_name) return a::ok;
    error = root_ref(root) + " conflicts with " + local_ref(owner, local) +
            " because root option '--" + root.long_name + "' may not be shadowed locally";
    return a::duplicate;
}

a::status validate_root_local_shadowing(const std::vector<parameter>& existing_roots,
                                        const std::vector<command>& existing_commands,
                                        const std::vector<parameter>& added_roots,
                                        const std::vector<command>& added_commands,
                                        std::string& error)
{
    for (const parameter& root : added_roots) {
        for (const command& owner : existing_commands) {
            for (const parameter& local : owner.parameters) {
                const a::status result = validate_shadow_pair(root, owner, local, error);
                if (result != a::ok) return result;
            }
        }
        for (const command& owner : added_commands) {
            for (const parameter& local : owner.parameters) {
                const a::status result = validate_shadow_pair(root, owner, local, error);
                if (result != a::ok) return result;
            }
        }
    }
    for (const parameter& root : existing_roots) {
        for (const command& owner : added_commands) {
            for (const parameter& local : owner.parameters) {
                const a::status result = validate_shadow_pair(root, owner, local, error);
                if (result != a::ok) return result;
            }
        }
    }
    return a::ok;
}

a::status validate_configuration_keys(const std::vector<parameter>& roots,
                                      const std::vector<command>& commands,
                                      std::string& error)
{
    for (const parameter& root : roots) {
        for (const command& owner : commands) {
            for (const parameter& local : owner.parameters) {
                if (!local.config_key.empty() && root.plugin_id == local.plugin_id &&
                    root.parameter_id == local.config_key) {
                    error = root_ref(root) + " conflicts with " + local_ref(owner, local) +
                            " on plugin configuration key '" + root.parameter_id + "'";
                    return a::duplicate;
                }
            }
        }
    }

    for (std::size_t left_index = 0; left_index < commands.size(); ++left_index) {
        const command& left_owner = commands[left_index];
        for (std::size_t right_index = left_index + 1; right_index < commands.size();
             ++right_index) {
            const command& right_owner = commands[right_index];
            if (left_owner.plugin_id != right_owner.plugin_id) continue;
            for (const parameter& left : left_owner.parameters) {
                if (left.config_key.empty()) continue;
                for (const parameter& right : right_owner.parameters) {
                    if (left.config_key != right.config_key) continue;
                    const bool left_sensitive =
                        (left.flags & v1::parameter_sensitive) != 0;
                    const bool right_sensitive =
                        (right.flags & v1::parameter_sensitive) != 0;
                    if (left.kind == right.kind && left_sensitive == right_sensitive) continue;
                    error = local_ref(left_owner, left) + " conflicts with " +
                            local_ref(right_owner, right) +
                            " because configuration key '" + left.config_key +
                            "' is reused across commands with incompatible kind or sensitivity";
                    return a::invalid_argument;
                }
            }
        }
    }
    return a::ok;
}

a::status validate_global_conflicts(const std::vector<parameter>& existing_roots,
                                    const std::vector<command>& existing_commands,
                                    const std::vector<parameter>& added_roots,
                                    const std::vector<command>& added_commands,
                                    std::string& error)
{
    a::status result = validate_top_level_ids(existing_roots, existing_commands, added_roots,
                                              added_commands, error);
    if (result != a::ok) return result;
    result = validate_root_names(existing_roots, added_roots, error);
    if (result != a::ok) return result;
    result = validate_command_names(existing_commands, added_commands, error);
    if (result != a::ok) return result;
    return validate_root_local_shadowing(existing_roots, existing_commands, added_roots,
                                         added_commands, error);
}

} // namespace

catalog::catalog() = default;
catalog::~catalog() = default;
catalog::catalog(const catalog&) = default;
catalog& catalog::operator=(const catalog&) = default;
catalog::catalog(catalog&&) noexcept = default;
catalog& catalog::operator=(catalog&&) noexcept = default;

a::status catalog::add(std::string plugin_id, const v1::manifest* value, std::string* error)
{
    if (error != nullptr) error->clear();
    try {
        std::string diagnostic;
        if (plugin_id.empty()) {
            if (error != nullptr) *error = "plugin_id must not be empty";
            return a::invalid_argument;
        }
        if (plugin_id.size() > max_plugin_id_bytes) {
            diagnostic = "plugin_id is " + std::to_string(plugin_id.size()) +
                         " bytes, exceeding the limit of " +
                         std::to_string(max_plugin_id_bytes);
            if (error != nullptr) *error = diagnostic;
            return a::limit_exceeded;
        }
        if (plugin_id.find('\0') != std::string::npos) {
            if (error != nullptr) *error = "plugin_id contains an embedded NUL byte";
            return a::invalid_argument;
        }
        if (!valid_utf8(plugin_id.data(), plugin_id.size())) {
            if (error != nullptr) *error = "plugin_id is not valid UTF-8";
            return a::invalid_argument;
        }
        if (std::find(plugin_ids_.begin(), plugin_ids_.end(), plugin_id) != plugin_ids_.end()) {
            if (error != nullptr)
                *error = "plugin '" + plugin_id +
                         "' declaration '<manifest>' conflicts with plugin '" + plugin_id +
                         "' declaration '<manifest>' because catalog::add was already called";
            return a::duplicate;
        }
        if (value == nullptr) {
            if (error != nullptr) *error = "manifest must not be null";
            return a::invalid_argument;
        }
        if (value->struct_size != sizeof(v1::manifest)) {
            if (error != nullptr)
                *error = "manifest has struct_size " + std::to_string(value->struct_size) +
                         ", expected " + std::to_string(sizeof(v1::manifest));
            return a::invalid_argument;
        }
        if (value->reserved != 0) {
            if (error != nullptr) *error = "manifest has a non-zero reserved field";
            return a::invalid_argument;
        }
        if (!v1::valid_manifest_flags(value->flags)) {
            if (error != nullptr) *error = "manifest contains unknown manifest flags";
            return a::unsupported;
        }
        if (!v1::valid_view(value->root_parameters) || !v1::valid_view(value->commands)) {
            if (error != nullptr)
                *error = "manifest has a null array pointer with a non-zero count";
            return a::invalid_argument;
        }
        if (value->root_parameters.size > v1::max_root_parameters) {
            if (error != nullptr)
                *error = "manifest root parameter count exceeds " +
                         std::to_string(v1::max_root_parameters);
            return a::limit_exceeded;
        }
        if (value->commands.size > v1::max_commands) {
            if (error != nullptr)
                *error = "manifest command count exceeds " + std::to_string(v1::max_commands);
            return a::limit_exceeded;
        }

        a::status result = a::ok;
        copy_budget budget;
        result = budget.consume_array<v1::parameter_desc>(value->root_parameters.size,
                                                          "manifest.root_parameters", diagnostic);
        if (result == a::ok)
            result = budget.consume_array<v1::command_desc>(value->commands.size,
                                                            "manifest.commands", diagnostic);
        if (result != a::ok) {
            if (error != nullptr) *error = diagnostic;
            return result;
        }

        std::vector<parameter> added_roots;
        std::vector<command> added_commands;
        added_roots.reserve(static_cast<std::size_t>(value->root_parameters.size));
        added_commands.reserve(static_cast<std::size_t>(value->commands.size));
        for (std::uint64_t index = 0; index < value->root_parameters.size; ++index) {
            parameter copied;
            result = copy_parameter(plugin_id, value->root_parameters.data[index], true, {}, budget,
                                    copied, diagnostic);
            if (result != a::ok) {
                if (error != nullptr) *error = diagnostic;
                return result;
            }
            added_roots.push_back(std::move(copied));
        }
        for (std::uint64_t index = 0; index < value->commands.size; ++index) {
            command copied;
            result = copy_command(plugin_id, value->commands.data[index], budget, copied,
                                  diagnostic);
            if (result != a::ok) {
                if (error != nullptr) *error = diagnostic;
                return result;
            }
            added_commands.push_back(std::move(copied));
        }

        result = validate_configuration_keys(added_roots, added_commands, diagnostic);
        if (result == a::ok)
            result = validate_global_conflicts(root_parameters_, commands_, added_roots,
                                               added_commands, diagnostic);
        if (result != a::ok) {
            if (error != nullptr) *error = diagnostic;
            return result;
        }

        std::vector<std::string> committed_plugin_ids = plugin_ids_;
        std::vector<parameter> committed_roots = root_parameters_;
        std::vector<command> committed_commands = commands_;
        committed_plugin_ids.push_back(plugin_id);
        committed_roots.insert(committed_roots.end(), added_roots.begin(), added_roots.end());
        committed_commands.insert(committed_commands.end(), added_commands.begin(),
                                  added_commands.end());
        plugin_ids_.swap(committed_plugin_ids);
        root_parameters_.swap(committed_roots);
        commands_.swap(committed_commands);
        return a::ok;
    } catch (const std::bad_alloc&) {
        if (error != nullptr) {
            try {
                *error = "catalog allocation failed";
            } catch (...) {
            }
        }
        return a::failed;
    } catch (...) {
        if (error != nullptr) {
            try {
                *error = "catalog validation failed with an unexpected exception";
            } catch (...) {
            }
        }
        return a::failed;
    }
}

void catalog::clear() noexcept
{
    plugin_ids_.clear();
    root_parameters_.clear();
    commands_.clear();
}

const std::vector<parameter>& catalog::root_parameters() const noexcept
{
    return root_parameters_;
}

const std::vector<command>& catalog::commands() const noexcept
{
    return commands_;
}

} // namespace u42::cli
