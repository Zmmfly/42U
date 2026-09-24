/**
 * @file cli_service.cc
 * @brief CLI fixture contributing the serve command and tracing handler execution.
 */

#include <42u/abi.hpp>
#include <42u/cli.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <string_view>

namespace {

namespace abi = u42::abi::v3;
namespace cli = u42::cli::v1;

constexpr char plugin_id[] = "com.example.cli.service";
constexpr char trace_name[] = "cli_service.jsonl";
constexpr abi::method_id serve_method = 42u;
constexpr char missing_handler_environment[] = "U42_TEST_CLI_SERVICE_HANDLER_MISSING";
constexpr char invoke_failure_environment[] = "U42_TEST_CLI_SERVICE_INVOKE_FAILED";
constexpr char stop_failure_environment[] = "U42_TEST_CLI_SERVICE_STOP_FAILED";

/** @brief Convert one string literal to a length-delimited ABI view. */
template <std::size_t Size>
constexpr cli::text_view text(const char (&value)[Size]) noexcept
{
    return {value, Size - 1};
}

/** @brief Escape one UTF-8 value for a compact JSON trace field. */
std::string json_string(std::string_view value)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.push_back('"');
    for (const unsigned char byte : value) {
        switch (byte) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (byte < 0x20u) {
                out += "\\u00";
                out.push_back(hex[(byte >> 4u) & 0x0fu]);
                out.push_back(hex[byte & 0x0fu]);
            } else {
                out.push_back(static_cast<char>(byte));
            }
            break;
        }
    }
    out.push_back('"');
    return out;
}

/** @brief Copy one borrowed CLI value to an owning string. */
bool copy_single_value(const cli::config_entry& entry, std::string& out)
{
    if (entry.values.size != 1u || entry.values.data == nullptr ||
        !cli::valid_view(entry.values.data[0])) {
        return false;
    }
    const cli::text_view value = entry.values.data[0];
    out.assign(value.data == nullptr ? "" : value.data, static_cast<std::size_t>(value.size));
    return true;
}

/** @brief Append one JSON object to this fixture's process trace, when enabled. */
void trace(std::string_view object) noexcept
{
    try {
        const char* const directory = std::getenv("U42_TEST_TRACE_DIR");
        if (directory == nullptr || *directory == '\0') return;
        std::ofstream stream(std::filesystem::path(directory) / trace_name,
                             std::ios::out | std::ios::app);
        if (!stream) return;
        stream << object << '\n';
        stream.flush();
    } catch (...) {
    }
}

/** @brief Test whether one fixture fault-injection environment switch is enabled. */
bool environment_enabled(const char* name) noexcept
{
    const char* const value = std::getenv(name);
    return value != nullptr && std::string_view(value) == "1";
}

const cli::parameter_desc serve_parameters[]{
    {sizeof(cli::parameter_desc),
     0u,
     cli::parameter_required,
     cli::text,
     cli::no_position,
     text("port"),
     text("port"),
     text("Port accepted by the finite serve fixture."),
     text("PORT"),
     {},
     {},
     {}},
    {sizeof(cli::parameter_desc),
     0u,
     cli::parameter_repeatable,
     cli::text,
     cli::no_position,
     text("label"),
     text("label"),
     text("Attach an ordered label."),
     text("LABEL"),
     {},
     {},
     {}},
    {sizeof(cli::parameter_desc),
     0u,
     0u,
     cli::flag,
     cli::no_position,
     text("verbose"),
     text("verbose"),
     text("Enable verbose fixture output."),
     {},
     {},
     {},
     {}},
    {sizeof(cli::parameter_desc),
     0u,
     0u,
     cli::text,
     0u,
     text("target"),
     {},
     text("Target handled by the fixture."),
     text("TARGET"),
     {},
     {},
     {}},
    {sizeof(cli::parameter_desc),
     0u,
     0u,
     cli::text,
     cli::no_position,
     text("mode-option"),
     text("mode"),
     text("Store the selected mode in plugin-local configuration."),
     text("MODE"),
     text("mode"),
     {},
     {}},
};

const cli::command_desc commands[]{
    {sizeof(cli::command_desc),
     0u,
     0u,
     serve_method,
     0u,
     text("serve"),
     text("serve"),
     text("Execute one finite service request."),
     {serve_parameters, 5u}},
};

const cli::manifest service_manifest{sizeof(cli::manifest), 0u, 0u, {}, {commands, 1u}};

const abi::plug_desc service_description{sizeof(abi::plug_desc),
                                         0u,
                                         plugin_id,
                                         {1u, 0u, 0u},
                                         0,
                                         0u,
                                         nullptr,
                                         0u,
                                         nullptr};

const abi::method_desc serve_description{serve_method,
                                         "serve",
                                         "Handle one generated CLI JSON payload.",
                                         "{\"type\":\"object\"}",
                                         "{\"type\":\"object\"}"};

/** @brief Service fixture implementing one published numeric command handler. */
class service_plugin final : public abi::iplug, public abi::iinvoke {
public:
    /** @brief Capture owner-local config and capability services before startup. */
    abi::status U42_CALL init(abi::ictx* context) noexcept override
    {
        if (context == nullptr || caps_ != nullptr || config_ != nullptr) {
            return abi::invalid_argument;
        }

        void* raw_config = nullptr;
        abi::status status = context->query(&cli::config_iid, &raw_config);
        if (status != abi::ok || raw_config == nullptr) return status == abi::ok ? abi::failed : status;
        config_ = static_cast<cli::iconfig*>(raw_config);

        cli::array_view<cli::config_entry> entries{};
        status = config_->entries(&entries);
        if (status != abi::ok) return status;
        cli::config_entry mode{};
        const abi::status mode_status = config_->get(text("mode"), &mode);
        std::string mode_value;
        if (mode_status == abi::ok && !copy_single_value(mode, mode_value)) return abi::failed;
        trace("{\"event\":\"init\",\"config_entry_count\":" +
              std::to_string(entries.size) + ",\"mode_status\":" +
              std::to_string(mode_status) +
              (mode_status == abi::ok ? ",\"mode\":" + json_string(mode_value) : "") + "}");

        void* raw_caps = nullptr;
        status = context->query(&abi::caps_iid, &raw_caps);
        if (status != abi::ok) return status;
        caps_ = static_cast<abi::icaps*>(raw_caps);
        return caps_ != nullptr ? abi::ok : abi::failed;
    }

    /** @brief Announce the numeric serve handler, or an empty set for the missing-handler case. */
    abi::status U42_CALL start() noexcept override
    {
        if (caps_ == nullptr || config_ == nullptr || started_) return abi::invalid_state;
        const bool omit_handler = environment_enabled(missing_handler_environment);
        const abi::caps_desc capabilities{sizeof(abi::caps_desc),
                                          omit_handler ? 0u : 1u,
                                          omit_handler ? nullptr : &serve_description};
        const abi::status status = caps_->announce(&capabilities);
        if (status == abi::ok) {
            started_ = true;
            trace(std::string("{\"event\":\"start\",\"handler_announced\":") +
                  (omit_handler ? "false}" : "true}"));
        }
        return status;
    }

    /** @brief Record shutdown and optionally fail so the host must quarantine this instance. */
    abi::status U42_CALL stop() noexcept override
    {
        started_ = false;
        const bool fail = environment_enabled(stop_failure_environment);
        trace(std::string("{\"event\":\"stop\",\"status\":\"") +
              (fail ? "failed\"}" : "ok\"}"));
        return fail ? abi::failed : abi::ok;
    }

    /** @brief Record destruction and free the instance on its allocating side. */
    void U42_CALL destroy() noexcept override
    {
        trace("{\"event\":\"destroy\"}");
        delete this;
    }

    /** @brief Expose only the host-private invocation interface. */
    abi::status U42_CALL query(const abi::iid* type, void** out) noexcept override
    {
        if (out == nullptr) return abi::invalid_argument;
        *out = nullptr;
        if (type == nullptr) return abi::invalid_argument;
        if (*type != abi::invoke_iid) return abi::unsupported;
        *out = static_cast<abi::iinvoke*>(this);
        return abi::ok;
    }

    /** @brief Trace the generated payload, local config, and isolated root-key lookup. */
    abi::status U42_CALL invoke(abi::method_id method, abi::bytes args,
                                abi::iwriter* result) noexcept override
    {
        if (result == nullptr || (args.data == nullptr && args.size != 0u)) {
            return abi::invalid_argument;
        }
        if (method != serve_method) return abi::not_found;
        if (!started_ || config_ == nullptr) return abi::invalid_state;

        try {
            const std::string payload(args.data == nullptr ? "" : static_cast<const char*>(args.data),
                                      static_cast<std::size_t>(args.size));
            cli::array_view<cli::config_entry> entries{};
            abi::status status = config_->entries(&entries);
            if (status != abi::ok) return status;

            cli::config_entry mode{};
            const abi::status mode_status = config_->get(text("mode"), &mode);
            std::string mode_value;
            if (mode_status == abi::ok && !copy_single_value(mode, mode_value)) return abi::failed;

            cli::config_entry foreign{};
            const abi::status log_level_status = config_->get(text("log-level"), &foreign);

            std::string line = "{\"event\":\"invoke\",\"payload\":";
            line += payload.empty() ? "null" : payload;
            line += ",\"config_entry_count\":" + std::to_string(entries.size);
            line += ",\"config\":";
            if (mode_status == abi::ok) {
                line += "{\"mode\":" + json_string(mode_value) + ",\"source\":" +
                        std::to_string(mode.source) + "}";
            } else {
                line += "{}";
            }
            line += ",\"log_level_status\":" + std::to_string(log_level_status) + "}";
            trace(line);

            if (environment_enabled(invoke_failure_environment)) return abi::failed;

            constexpr char response[] = "{\"handled\":true}";
            return result->write({response, sizeof(response) - 1u});
        } catch (...) {
            return abi::failed;
        }
    }

private:
    abi::icaps* caps_ = nullptr;
    cli::iconfig* config_ = nullptr;
    bool started_ = false;
};

/** @brief Static factory for the service fixture. */
class service_factory final : public abi::iplug_fty {
public:
    /** @brief Return immutable factory metadata. */
    abi::status U42_CALL describe(const abi::plug_desc** out) noexcept override
    {
        if (out == nullptr) return abi::invalid_argument;
        *out = &service_description;
        return abi::ok;
    }

    /** @brief Allocate one uninitialized service instance. */
    abi::status U42_CALL create(abi::iplug** out) noexcept override
    {
        if (out == nullptr) return abi::invalid_argument;
        *out = new (std::nothrow) service_plugin();
        if (*out == nullptr) return abi::failed;
        trace("{\"event\":\"create\"}");
        return abi::ok;
    }
};

service_factory factory;

} // namespace

/** @brief Negotiate the ABI v3 service fixture factory. */
extern "C" U42_EXPORT abi::status U42_CALL u42_get_factory(std::uint32_t major,
                                                            abi::iplug_fty** out) noexcept
{
    if (out == nullptr) return abi::invalid_argument;
    *out = nullptr;
    if (major != abi::abi_major) return abi::unsupported;
    *out = &factory;
    return abi::ok;
}

/** @brief Negotiate the service fixture's CLI manifest v1. */
extern "C" U42_EXPORT abi::status U42_CALL u42_get_cli_manifest(
    std::uint32_t major, const cli::manifest** out) noexcept
{
    if (out == nullptr) return abi::invalid_argument;
    *out = nullptr;
    if (major != cli::manifest_major) return abi::unsupported;
    *out = &service_manifest;
    trace("{\"event\":\"manifest\"}");
    return abi::ok;
}
