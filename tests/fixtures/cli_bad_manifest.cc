/**
 * @file cli_bad_manifest.cc
 * @brief Negative CLI fixture exporting a deterministic reserved-name declaration.
 */

#include <42u/abi.hpp>
#include <42u/cli.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <new>
#include <string_view>

namespace {

namespace abi = u42::abi::v3;
namespace cli = u42::cli::v1;

constexpr char plugin_id[] = "com.example.cli.bad-manifest";
constexpr char trace_name[] = "cli_bad_manifest.jsonl";
constexpr char unsupported_environment[] = "U42_TEST_CLI_BAD_MANIFEST_UNSUPPORTED";

/** @brief Convert one string literal to a length-delimited ABI view. */
template <std::size_t Size>
constexpr cli::text_view text(const char (&value)[Size]) noexcept
{
    return {value, Size - 1};
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

/** @brief Test whether manifest-v1 negotiation should be rejected for this process. */
bool reject_manifest_v1() noexcept
{
    const char* const value = std::getenv(unsupported_environment);
    return value != nullptr && std::string_view(value) == "1";
}

const cli::parameter_desc invalid_root[]{
    {sizeof(cli::parameter_desc),
     0u,
     0u,
     cli::flag,
     cli::no_position,
     text("bad-help"),
     text("help"),
     text("Illegally shadows the host-owned help option."),
     {},
     {},
     {},
     {}},
};

const cli::manifest bad_manifest{sizeof(cli::manifest), 0u, 0u, {invalid_root, 1u}, {}};

const abi::plug_desc bad_description{sizeof(abi::plug_desc),
                                     0u,
                                     plugin_id,
                                     {1u, 0u, 0u},
                                     0,
                                     0u,
                                     nullptr,
                                     0u,
                                     nullptr};

/** @brief Instance that should never be created because manifest validation fails first. */
class bad_plugin final : public abi::iplug {
public:
    /** @brief Record an unexpected initialization. */
    abi::status U42_CALL init(abi::ictx* context) noexcept override
    {
        trace("{\"event\":\"init\"}");
        return context == nullptr ? abi::invalid_argument : abi::ok;
    }

    /** @brief Record an unexpected startup. */
    abi::status U42_CALL start() noexcept override
    {
        trace("{\"event\":\"start\"}");
        return abi::ok;
    }

    /** @brief Record an unexpected stop. */
    abi::status U42_CALL stop() noexcept override
    {
        trace("{\"event\":\"stop\"}");
        return abi::ok;
    }

    /** @brief Record destruction and free the instance. */
    void U42_CALL destroy() noexcept override
    {
        trace("{\"event\":\"destroy\"}");
        delete this;
    }

    /** @brief Reject every optional plugin interface. */
    abi::status U42_CALL query(const abi::iid* type, void** out) noexcept override
    {
        if (out == nullptr) return abi::invalid_argument;
        *out = nullptr;
        return type == nullptr ? abi::invalid_argument : abi::unsupported;
    }
};

/** @brief Static factory for the invalid-manifest fixture. */
class bad_factory final : public abi::iplug_fty {
public:
    /** @brief Return otherwise-valid plugin metadata. */
    abi::status U42_CALL describe(const abi::plug_desc** out) noexcept override
    {
        if (out == nullptr) return abi::invalid_argument;
        *out = &bad_description;
        return abi::ok;
    }

    /** @brief Record any erroneous attempt to create this rejected plugin. */
    abi::status U42_CALL create(abi::iplug** out) noexcept override
    {
        if (out == nullptr) return abi::invalid_argument;
        *out = new (std::nothrow) bad_plugin();
        if (*out == nullptr) return abi::failed;
        trace("{\"event\":\"create\"}");
        return abi::ok;
    }
};

bad_factory factory;

} // namespace

/** @brief Negotiate the ABI v3 factory used before the manifest is rejected. */
extern "C" U42_EXPORT abi::status U42_CALL u42_get_factory(std::uint32_t major,
                                                            abi::iplug_fty** out) noexcept
{
    if (out == nullptr) return abi::invalid_argument;
    *out = nullptr;
    if (major != abi::abi_major) return abi::unsupported;
    *out = &factory;
    return abi::ok;
}

/** @brief Return the intentionally invalid reserved-name manifest. */
extern "C" U42_EXPORT abi::status U42_CALL u42_get_cli_manifest(
    std::uint32_t major, const cli::manifest** out) noexcept
{
    if (out == nullptr) return abi::invalid_argument;
    *out = nullptr;
    if (major != cli::manifest_major) return abi::unsupported;
    if (reject_manifest_v1()) {
        trace("{\"event\":\"manifest\",\"status\":\"unsupported\"}");
        return abi::unsupported;
    }
    *out = &bad_manifest;
    trace("{\"event\":\"manifest\"}");
    return abi::ok;
}
