/**
 * @file plug.cc
 * @brief Host-side dynamic library RAII wrapper and deterministic plugin scanning.
 *
 * The mapping never owns plugin instances: instances are created through the borrowed
 * factory and must be destroyed on the plugin side before the mapping is released.
 */
#include <42u/plug.hpp>

#include <algorithm>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace u42 {
namespace {

#if defined(_WIN32)
using native_path_string = std::wstring;
inline constexpr const wchar_t* plugin_suffix = L".u42.dll";
#elif defined(__APPLE__)
using native_path_string = std::string;
inline constexpr const char* plugin_suffix = ".u42.dylib";
#else
using native_path_string = std::string;
inline constexpr const char* plugin_suffix = ".u42.so";
#endif

/**
 * @brief Check whether a file name carries this platform's plugin suffix.
 *
 * @param file Candidate path taken from a directory enumeration.
 * @return true only for a name with a non-empty stem followed by the platform suffix.
 */
bool has_plugin_suffix(const std::filesystem::path& file)
{
    const native_path_string name = file.filename().native();
    const native_path_string suffix = plugin_suffix;
    return name.size() > suffix.size() &&
           name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

#if !defined(_WIN32)
/**
 * @brief Read and clear the pending loader diagnostic.
 *
 * @return Diagnostic text; never empty when the preceding loader call failed.
 */
std::string last_dl_error()
{
    const char* text = ::dlerror();
    return text != nullptr ? std::string(text) : std::string("no loader diagnostic available");
}
#endif

#if defined(_WIN32)
/**
 * @brief Format the most recent Windows error as a UTF-8 diagnostic.
 *
 * @return Diagnostic text; falls back to the numeric error code when formatting fails.
 */
std::string last_error_message()
{
    const DWORD code = ::GetLastError();
    std::string text = "Windows error " + std::to_string(code);
    wchar_t* buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (length != 0 && buffer != nullptr) {
        const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length),
                                                nullptr, 0, nullptr, nullptr);
        if (bytes > 0) {
            std::string utf8(static_cast<std::size_t>(bytes), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), utf8.data(), bytes,
                                  nullptr, nullptr);
            const std::size_t end = utf8.find_last_not_of("\r\n ");
            if (end != std::string::npos) utf8.erase(end + 1);
            if (!utf8.empty()) text += ": " + utf8;
        }
    }
    if (buffer != nullptr) ::LocalFree(buffer);
    return text;
}
#endif

/**
 * @brief Map one trusted library with immediate binding and locally scoped symbols.
 *
 * @param path Canonical library path.
 * @param error Human-readable diagnostic; written only on failure.
 * @return Native library handle, or null when the loader rejected the library.
 */
void* map_library(const std::filesystem::path& path, std::string& error)
{
#if defined(_WIN32)
    HMODULE module = ::LoadLibraryW(path.c_str());
    if (module == nullptr) {
        error = "cannot load '" + path.string() + "': " + last_error_message();
        return nullptr;
    }
    return module;
#else
    void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        error = "cannot load '" + path.string() + "': " + last_dl_error();
        return nullptr;
    }
    return handle;
#endif
}

/**
 * @brief Release a native library mapping.
 * @note Only call once no borrowed factory, instance or callback is still referenced.
 *
 * @param handle Handle previously returned by map_library().
 */
void unmap_library(void* handle) noexcept
{
#if defined(_WIN32)
    (void)::FreeLibrary(static_cast<HMODULE>(handle));
#else
    (void)::dlclose(handle);
#endif
}

/**
 * @brief Resolve the exported factory entry point and keep a specific diagnostic.
 *
 * @param handle Mapped library handle.
 * @param error Human-readable diagnostic; written only on failure.
 * @return Entry point, or null when the symbol is missing or resolves to null.
 */
abi::v2::entry_fn resolve_entry(void* handle, std::string& error)
{
#if defined(_WIN32)
    const FARPROC symbol = ::GetProcAddress(static_cast<HMODULE>(handle), abi::v2::entry_name);
    if (symbol == nullptr) {
        error = "cannot resolve " + std::string(abi::v2::entry_name) + ": " + last_error_message();
        return nullptr;
    }
    return reinterpret_cast<abi::v2::entry_fn>(symbol);
#else
    ::dlerror(); // Clear stale state so the next diagnostic belongs to this lookup.
    void* symbol = ::dlsym(handle, abi::v2::entry_name);
    const char* diagnostic = ::dlerror();
    if (diagnostic != nullptr) {
        error = "cannot resolve " + std::string(abi::v2::entry_name) + ": " + diagnostic;
        return nullptr;
    }
    if (symbol == nullptr) {
        error = std::string(abi::v2::entry_name) + " resolves to null";
        return nullptr;
    }
    return reinterpret_cast<abi::v2::entry_fn>(symbol);
#endif
}

} // namespace

plug::~plug()
{
    close();
}

plug::plug(plug&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)),
      factory_(std::exchange(other.factory_, nullptr)),
      path_(std::move(other.path_))
{
}

plug& plug::operator=(plug&& other) noexcept
{
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, nullptr);
        factory_ = std::exchange(other.factory_, nullptr);
        path_ = std::move(other.path_);
    }
    return *this;
}

abi::v2::status plug::open(const std::filesystem::path& path, std::string& error)
{
    error.clear();
    if (handle_ != nullptr) {
        error = "library already open: '" + path_.string() + "'";
        return abi::v2::invalid_state;
    }
    if (path.empty()) {
        error = "plugin library path is empty";
        return abi::v2::invalid_argument;
    }

    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::canonical(path, ec);
    if (ec) {
        error = "cannot resolve plugin library '" + path.string() + "': " + ec.message();
        return ec == std::errc::no_such_file_or_directory ? abi::v2::not_found : abi::v2::failed;
    }

    std::string diagnostic;
    void* handle = map_library(canonical, diagnostic);
    if (handle == nullptr) {
        error = diagnostic;
        return abi::v2::failed;
    }

    abi::v2::entry_fn entry = resolve_entry(handle, diagnostic);
    if (entry == nullptr) {
        error = diagnostic;
        unmap_library(handle);
        return abi::v2::failed;
    }

    abi::v2::iplug_fty* factory = nullptr;
    const abi::v2::status negotiated = entry(abi::v2::abi_major, &factory);
    if (negotiated != abi::v2::ok) {
        error = "ABI negotiation failed for '" + canonical.string() + "': requested major " +
                std::to_string(abi::v2::abi_major) + ", library returned status " +
                std::to_string(negotiated);
        unmap_library(handle);
        return abi::v2::unsupported;
    }
    if (factory == nullptr) {
        error = "u42_get_factory returned a null factory for '" + canonical.string() + "'";
        unmap_library(handle);
        return abi::v2::failed;
    }

    handle_ = handle;
    factory_ = factory;
    path_ = std::move(canonical);
    return abi::v2::ok;
}

void plug::close() noexcept
{
    if (handle_ == nullptr) return;
    factory_ = nullptr;
    void* handle = std::exchange(handle_, nullptr);
    path_.clear();
    unmap_library(handle);
}

abi::v2::status scan_plugins(const std::filesystem::path& directory,
                            std::vector<std::filesystem::path>& out, std::string& error)
{
    namespace fs = std::filesystem;

    out.clear();
    error.clear();

    if (directory.empty()) {
        error = "plugin directory is empty";
        return abi::v2::invalid_argument;
    }

    std::error_code ec;
    if (!fs::exists(directory, ec)) {
        if (ec) {
            error = "cannot inspect plugin directory '" + directory.string() + "': " + ec.message();
            return abi::v2::failed;
        }
        error = "plugin directory does not exist: '" + directory.string() + "'";
        return abi::v2::not_found;
    }
    if (!fs::is_directory(directory, ec)) {
        if (ec) {
            error = "cannot inspect plugin directory '" + directory.string() + "': " + ec.message();
            return abi::v2::failed;
        }
        error = "plugin path is not a directory: '" + directory.string() + "'";
        return abi::v2::invalid_argument;
    }

    std::vector<fs::path> candidates;
    const fs::directory_iterator end;
    for (fs::directory_iterator it(directory, ec); it != end; it.increment(ec)) {
        if (ec) {
            error = "cannot read plugin directory '" + directory.string() + "': " + ec.message();
            return abi::v2::failed;
        }

        const fs::directory_entry& entry = *it;
        const bool regular = entry.is_regular_file(ec);
        if (ec) {
            error = "cannot inspect plugin candidate '" + entry.path().string() + "': " +
                    ec.message();
            return abi::v2::failed;
        }
        if (!regular || !has_plugin_suffix(entry.path())) continue;

        fs::path canonical = fs::canonical(entry.path(), ec);
        if (ec) {
            error = "cannot resolve plugin candidate '" + entry.path().string() + "': " +
                    ec.message();
            return abi::v2::failed;
        }
        candidates.push_back(std::move(canonical));
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const fs::path& left, const fs::path& right) {
                  return left.native() < right.native();
              });
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    out = std::move(candidates);
    return abi::v2::ok;
}

} // namespace u42
