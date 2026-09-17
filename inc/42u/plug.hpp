#pragma once
#include <42u/abi.hpp>
#include <filesystem>
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
    abi::v1::status open(const std::filesystem::path& path, std::string& error);
    /**
     * @brief Release the mapping. No instance, interface or function pointer may still be used.
     */
    void close() noexcept;
    /**
     * @brief Get the borrowed negotiated factory, or null for a closed library.
     */
    abi::v1::iplug_fty* factory() const noexcept { return factory_; }
    /**
     * @brief Get the canonical loaded path.
     */
    const std::filesystem::path& path() const noexcept { return path_; }
private:
    void* handle_ = nullptr;
    abi::v1::iplug_fty* factory_ = nullptr;
    std::filesystem::path path_;
};
/**
 * @brief Enumerate canonical .u42.so/.u42.dylib/.u42.dll plugin candidates deterministically.
 * @note Nonrecursive. Deduplicate canonical paths; return errors rather than silently skipping IO failures.
 */
abi::v1::status scan_plugins(const std::filesystem::path& directory,
                            std::vector<std::filesystem::path>& out, std::string& error);
} // namespace u42
