/**
 * @file cli_logging.cc
 * @brief CLI fixture contributing one root logging option and tracing its lifecycle.
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

constexpr char plugin_id[] = "com.example.cli.logging";
constexpr char trace_name[] = "cli_logging.jsonl";

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

constexpr cli::text_view log_level_defaults[]{text("info")};

const cli::parameter_desc root_parameters[]{
    {sizeof(cli::parameter_desc),
     0u,
     0u,
     cli::text,
     cli::no_position,
     text("log-level"),
     text("log-level"),
     text("Set the logging persistence level."),
     text("LEVEL"),
     {},
     {log_level_defaults, 1u},
     {}},
};

const cli::manifest logging_manifest{sizeof(cli::manifest),
                                     0u,
                                     0u,
                                     {root_parameters, 1u},
                                     {}};

const abi::plug_desc logging_description{sizeof(abi::plug_desc),
                                         0u,
                                         plugin_id,
                                         {1u, 0u, 0u},
                                         0,
                                         0u,
                                         nullptr,
                                         0u,
                                         nullptr};

/** @brief Logging fixture instance reading its owner-local root configuration during init. */
class logging_plugin final : public abi::iplug {
public:
    /** @brief Query config and capability services and record the resolved log level. */
    abi::status U42_CALL init(abi::ictx* context) noexcept override
    {
        if (context == nullptr || caps_ != nullptr) return abi::invalid_argument;

        void* raw_config = nullptr;
        abi::status status = context->query(&cli::config_iid, &raw_config);
        if (status != abi::ok || raw_config == nullptr) {
            trace("{\"event\":\"init\",\"status\":" + std::to_string(status) + "}");
            return status == abi::ok ? abi::failed : status;
        }
        auto* const config = static_cast<cli::iconfig*>(raw_config);
        cli::config_entry entry{};
        status = config->get(text("log-level"), &entry);
        if (status != abi::ok || entry.values.size != 1u || entry.values.data == nullptr ||
            !cli::valid_view(entry.values.data[0])) {
            trace("{\"event\":\"init\",\"status\":" + std::to_string(status) + "}");
            return status == abi::ok ? abi::failed : status;
        }

        const cli::text_view value = entry.values.data[0];
        const std::string level(value.data == nullptr ? "" : value.data,
                                static_cast<std::size_t>(value.size));
        trace("{\"event\":\"init\",\"log_level\":" + json_string(level) +
              ",\"source\":" + std::to_string(entry.source) + "}");

        void* raw_caps = nullptr;
        status = context->query(&abi::caps_iid, &raw_caps);
        if (status != abi::ok) return status;
        caps_ = static_cast<abi::icaps*>(raw_caps);
        return caps_ != nullptr ? abi::ok : abi::failed;
    }

    /** @brief Publish an empty capability set and record startup. */
    abi::status U42_CALL start() noexcept override
    {
        if (caps_ == nullptr || started_) return abi::invalid_state;
        const abi::caps_desc capabilities{};
        const abi::status status = caps_->announce(&capabilities);
        if (status == abi::ok) {
            started_ = true;
            trace("{\"event\":\"start\"}");
        }
        return status;
    }

    /** @brief Record orderly shutdown. */
    abi::status U42_CALL stop() noexcept override
    {
        started_ = false;
        trace("{\"event\":\"stop\"}");
        return abi::ok;
    }

    /** @brief Record destruction and free the instance on its allocating side. */
    void U42_CALL destroy() noexcept override
    {
        trace("{\"event\":\"destroy\"}");
        delete this;
    }

    /** @brief This fixture publishes no invocation interface. */
    abi::status U42_CALL query(const abi::iid* type, void** out) noexcept override
    {
        if (out == nullptr) return abi::invalid_argument;
        *out = nullptr;
        return type == nullptr ? abi::invalid_argument : abi::unsupported;
    }

private:
    abi::icaps* caps_ = nullptr;
    bool started_ = false;
};

/** @brief Static factory for the logging fixture. */
class logging_factory final : public abi::iplug_fty {
public:
    /** @brief Return immutable factory metadata. */
    abi::status U42_CALL describe(const abi::plug_desc** out) noexcept override
    {
        if (out == nullptr) return abi::invalid_argument;
        *out = &logging_description;
        return abi::ok;
    }

    /** @brief Allocate one uninitialized logging instance. */
    abi::status U42_CALL create(abi::iplug** out) noexcept override
    {
        if (out == nullptr) return abi::invalid_argument;
        *out = new (std::nothrow) logging_plugin();
        if (*out == nullptr) return abi::failed;
        trace("{\"event\":\"create\"}");
        return abi::ok;
    }
};

logging_factory factory;

} // namespace

/** @brief Negotiate the ABI v3 logging fixture factory. */
extern "C" U42_EXPORT abi::status U42_CALL u42_get_factory(std::uint32_t major,
                                                            abi::iplug_fty** out) noexcept
{
    if (out == nullptr) return abi::invalid_argument;
    *out = nullptr;
    if (major != abi::abi_major) return abi::unsupported;
    *out = &factory;
    return abi::ok;
}

/** @brief Negotiate the logging fixture's CLI manifest v1. */
extern "C" U42_EXPORT abi::status U42_CALL u42_get_cli_manifest(
    std::uint32_t major, const cli::manifest** out) noexcept
{
    if (out == nullptr) return abi::invalid_argument;
    *out = nullptr;
    if (major != cli::manifest_major) return abi::unsupported;
    *out = &logging_manifest;
    trace("{\"event\":\"manifest\"}");
    return abi::ok;
}
