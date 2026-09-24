#pragma once
#include <42u/abi.hpp>
#include <42u/cli.hpp>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace u42 {
/**
 * @brief Move-only library mapping. Caller must destroy all instances before close/destruction.
 */
class plug {
public:
    plug() noexcept = default;
    ~plug();
    plug(const plug&) = delete;
    plug& operator=(const plug&) = delete;
    plug(plug&& other) noexcept;
    plug& operator=(plug&& other) noexcept;
    /**
     * @brief Load one trusted library, resolve entry, and negotiate ABI before exposing a factory.
     */
    abi::v3::status open(const std::filesystem::path& path, std::string& error);
    /**
     * @brief Release the mapping. No instance, interface or function pointer may still be used.
     */
    void close() noexcept;
    /**
     * @brief Get the borrowed negotiated factory, or null for a closed library.
     */
    abi::v3::iplug_fty* factory() const noexcept { return factory_; }
    /**
     * @brief Get the canonical loaded path.
     */
    const std::filesystem::path& path() const noexcept { return path_; }
private:
    friend class discovered_plugin;

    void* handle_ = nullptr;
    abi::v3::iplug_fty* factory_ = nullptr;
    std::filesystem::path path_;
};

/**
 * @brief Host-owned copy of one plugin factory description.
 *
 * @note All strings and constraint arrays are validated and copied while the plugin library is
 *       mapped. The numeric version and priority are copied verbatim from the ABI descriptor.
 */
struct plugin_description {
    std::string plug_id;                         //!< Stable plugin identity.
    abi::v3::plugin_version version{};           //!< Exact business version advertised by the factory.
    std::int32_t priority = 0;                   //!< Ordering priority advertised by the factory.
    std::vector<std::string> before;             //!< Plugin identities that must start after this plugin.
    std::vector<std::string> after;              //!< Plugin identities that must start before this plugin.
};

/**
 * @brief Move-only result of mapping and describing one plugin without creating an instance.
 *
 * @note A successful object owns exactly one library mapping. Its factory and optional CLI
 *       manifest are borrowed from that mapping. take_library() transfers the mapping to the
 *       runtime without closing or reopening it; factory() and manifest() then return null.
 */
class discovered_plugin {
public:
    /** @brief Construct an empty discovery result. */
    discovered_plugin() noexcept = default;
    /** @brief Destroy the owned mapping when it has not been transferred to the runtime. */
    ~discovered_plugin() = default;
    /** @brief Discovery results uniquely own their library mapping and cannot be copied. */
    discovered_plugin(const discovered_plugin&) = delete;
    /** @brief Discovery results uniquely own their library mapping and cannot be copy-assigned. */
    discovered_plugin& operator=(const discovered_plugin&) = delete;
    /** @brief Transfer a discovery result and its still-owned mapping. */
    discovered_plugin(discovered_plugin&&) noexcept = default;
    /** @brief Replace this result by transferring another result and its mapping. */
    discovered_plugin& operator=(discovered_plugin&&) noexcept = default;

    /**
     * @brief Map, negotiate, describe, and inspect one trusted plugin library without create().
     *
     * @param path Plugin library path, canonicalized before mapping.
     * @param[out] error Human-readable diagnostic, cleared before discovery.
     * @return ok after a complete discovery; otherwise the relevant ABI status. Failure leaves
     *         this object empty and closes every mapping opened by this call.
     *
     * @note The factory describe() hook is called exactly once. A missing CLI manifest symbol is
     *       accepted; an exported symbol must successfully negotiate manifest v1 and return a
     *       non-null manifest. An object that completed discovery, including one whose mapping was
     *       transferred, cannot be opened again.
     */
    abi::v3::status open(const std::filesystem::path& path, std::string& error);

    /**
     * @brief Return the validated host-owned factory description.
     *
     * @return Description copied during discovery, or an empty description before success.
     */
    const plugin_description& description() const noexcept { return description_; }

    /**
     * @brief Return the optional borrowed CLI manifest.
     *
     * @return Library-owned manifest while this object owns the mapping; otherwise null.
     */
    const cli::v1::manifest* manifest() const noexcept
    {
        return library_ != nullptr ? manifest_ : nullptr;
    }

    /**
     * @brief Return the borrowed negotiated ABI v3 factory.
     *
     * @return Library-owned factory while this object owns the mapping; otherwise null.
     */
    abi::v3::iplug_fty* factory() const noexcept
    {
        return library_ != nullptr ? library_->factory() : nullptr;
    }

    /**
     * @brief Return the canonical library path recorded during successful discovery.
     *
     * @return Empty path before success; the canonical path remains available after transfer.
     */
    const std::filesystem::path& path() const noexcept { return path_; }

    /**
     * @brief Transfer the existing mapping to the host runtime without reopening the library.
     *
     * @return Owned mapping, or null when discovery has not succeeded or ownership was transferred.
     * @warning Read factory() and manifest() before this call; both return null afterwards.
     */
    std::unique_ptr<plug> take_library() noexcept { return std::move(library_); }

private:
    std::unique_ptr<plug> library_;
    plugin_description description_;
    std::filesystem::path path_;
    const cli::v1::manifest* manifest_ = nullptr;
};

/**
 * @brief Enumerate canonical .u42.so/.u42.dylib/.u42.dll plugin candidates deterministically.
 * @note Nonrecursive. Deduplicate canonical paths; return errors rather than silently skipping IO failures.
 */
abi::v3::status scan_plugins(const std::filesystem::path& directory,
                            std::vector<std::filesystem::path>& out, std::string& error);
} // namespace u42
