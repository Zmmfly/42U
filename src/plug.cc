/**
 * @file plug.cc
 * @brief Host-side dynamic library RAII wrapper and deterministic plugin scanning.
 *
 * The mapping never owns plugin instances: instances are created through the borrowed
 * factory and must be destroyed on the plugin side before the mapping is released.
 */
#include <42u/plug.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
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

/** @brief Descriptor string limit shared with host-side factory validation. */
constexpr std::size_t max_string_bytes = 64u * 1024u;
/** @brief Maximum accepted entries in one before/after descriptor array. */
constexpr std::uint32_t max_constraint_entries = 4096u;

/**
 * @brief Render an ABI status as a stable diagnostic word.
 *
 * @param value ABI status code.
 * @return Lowercase status name, or the decimal value for an unknown code.
 */
std::string status_text(abi::v3::status value)
{
    namespace a = abi::v3;
    switch (value) {
        case a::ok: return "ok";
        case a::invalid_argument: return "invalid_argument";
        case a::unsupported: return "unsupported";
        case a::not_found: return "not_found";
        case a::duplicate: return "duplicate";
        case a::invalid_state: return "invalid_state";
        case a::busy: return "busy";
        case a::stale: return "stale";
        case a::limit_exceeded: return "limit_exceeded";
        case a::failed: return "failed";
        case a::wrong_thread: return "wrong_thread";
        case a::cycle: return "cycle";
        case a::deferred: return "deferred";
        default: return std::to_string(value);
    }
}

/**
 * @brief Strictly validate one UTF-8 byte range.
 *
 * @param data First byte of the candidate text.
 * @param size Number of bytes to validate.
 * @return true when the range is well-formed UTF-8.
 */
bool valid_utf8(const char* data, std::size_t size)
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
        std::uint32_t code_point = 0;
        if ((lead & 0xE0u) == 0xC0u) {
            extra = 1;
            code_point = lead & 0x1Fu;
            if (code_point < 2u) return false;
        } else if ((lead & 0xF0u) == 0xE0u) {
            extra = 2;
            code_point = lead & 0x0Fu;
        } else if ((lead & 0xF8u) == 0xF0u) {
            extra = 3;
            code_point = lead & 0x07u;
            if (code_point > 4u) return false;
        } else {
            return false;
        }

        if (index + extra >= size) return false;
        for (std::size_t offset = 1; offset <= extra; ++offset) {
            const unsigned char trail = bytes[index + offset];
            if ((trail & 0xC0u) != 0x80u) return false;
            code_point = (code_point << 6) | (trail & 0x3Fu);
        }
        if (extra == 2 &&
            (code_point < 0x800u || (code_point >= 0xD800u && code_point <= 0xDFFFu))) {
            return false;
        }
        if (extra == 3 && (code_point < 0x10000u || code_point > 0x10FFFFu)) return false;
        index += extra + 1;
    }
    return true;
}

/**
 * @brief Copy one bounded NUL-terminated descriptor string after UTF-8 validation.
 *
 * @param text Borrowed descriptor string; may be null only for optional fields.
 * @param required Whether the field must be present and non-empty.
 * @param field Field name used in diagnostics.
 * @param[out] out Receives the copied text.
 * @param[out] error Receives a field-specific diagnostic on failure.
 * @return ok on success, otherwise invalid_argument.
 */
abi::v3::status read_descriptor_string(const char* text, bool required, const std::string& field,
                                       std::string& out, std::string& error)
{
    namespace a = abi::v3;
    if (text == nullptr) {
        if (!required) {
            out.clear();
            return a::ok;
        }
        error = field + " must not be null";
        return a::invalid_argument;
    }

    const void* terminator = std::memchr(text, '\0', max_string_bytes + 1);
    if (terminator == nullptr) {
        error = field + " is not NUL-terminated within " + std::to_string(max_string_bytes) +
                " bytes";
        return a::invalid_argument;
    }
    const std::size_t size =
        static_cast<std::size_t>(static_cast<const char*>(terminator) - text);
    if (size == 0) {
        if (!required) {
            out.clear();
            return a::ok;
        }
        error = field + " must not be empty";
        return a::invalid_argument;
    }
    if (!valid_utf8(text, size)) {
        error = field + " is not valid UTF-8";
        return a::invalid_argument;
    }
    out.assign(text, size);
    return a::ok;
}

/**
 * @brief Validate and copy one before/after constraint array.
 *
 * @param items Borrowed constraint strings.
 * @param count Declared element count.
 * @param field Field name used in diagnostics.
 * @param[out] out Receives copied constraints.
 * @param[out] error Receives a field-specific diagnostic on failure.
 * @return ok, invalid_argument, or limit_exceeded.
 */
abi::v3::status read_constraints(const char* const* items, std::uint32_t count,
                                 const std::string& field, std::vector<std::string>& out,
                                 std::string& error)
{
    namespace a = abi::v3;
    out.clear();
    if (count == 0) return a::ok;
    if (items == nullptr) {
        error = field + " has a null array pointer with count " + std::to_string(count);
        return a::invalid_argument;
    }
    if (count > max_constraint_entries) {
        error = field + " declares " + std::to_string(count) +
                " entries, exceeding the limit of " + std::to_string(max_constraint_entries);
        return a::limit_exceeded;
    }

    out.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        std::string value;
        const a::status status = read_descriptor_string(
            items[index], true, field + "[" + std::to_string(index) + "]", value, error);
        if (status != a::ok) return status;
        out.push_back(std::move(value));
    }
    return a::ok;
}

/**
 * @brief Validate and deep-copy one factory descriptor.
 *
 * @param value Required borrowed factory descriptor.
 * @param[out] out Receives the complete owned description on success.
 * @param[out] error Receives a descriptor-specific diagnostic on failure.
 * @return ok or the corresponding validation status.
 */
abi::v3::status copy_description(const abi::v3::plug_desc* value, plugin_description& out,
                                 std::string& error)
{
    namespace a = abi::v3;
    if (value == nullptr) {
        error = "factory describe() returned a null plug_desc";
        return a::failed;
    }
    if (value->struct_size != sizeof(a::plug_desc)) {
        error = "plug_desc struct_size is " + std::to_string(value->struct_size) +
                ", expected " + std::to_string(sizeof(a::plug_desc));
        return a::invalid_argument;
    }
    if (value->reserved != 0) {
        error = "plug_desc reserved field must be zero";
        return a::invalid_argument;
    }

    plugin_description copied;
    a::status status = read_descriptor_string(value->plug_id, true, "plug_desc plug_id",
                                              copied.plug_id, error);
    if (status != a::ok) return status;
    copied.version = value->version;
    copied.priority = value->priority;
    status = read_constraints(value->before, value->before_count, "plug_desc before",
                              copied.before, error);
    if (status != a::ok) return status;
    status = read_constraints(value->after, value->after_count, "plug_desc after", copied.after,
                              error);
    if (status != a::ok) return status;

    out = std::move(copied);
    return a::ok;
}

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
abi::v3::entry_fn resolve_entry(void* handle, std::string& error)
{
#if defined(_WIN32)
    const FARPROC symbol = ::GetProcAddress(static_cast<HMODULE>(handle), abi::v3::entry_name);
    if (symbol == nullptr) {
        error = "cannot resolve " + std::string(abi::v3::entry_name) + ": " + last_error_message();
        return nullptr;
    }
    return reinterpret_cast<abi::v3::entry_fn>(symbol);
#else
    ::dlerror(); // Clear stale state so the next diagnostic belongs to this lookup.
    void* symbol = ::dlsym(handle, abi::v3::entry_name);
    const char* diagnostic = ::dlerror();
    if (diagnostic != nullptr) {
        error = "cannot resolve " + std::string(abi::v3::entry_name) + ": " + diagnostic;
        return nullptr;
    }
    if (symbol == nullptr) {
        error = std::string(abi::v3::entry_name) + " resolves to null";
        return nullptr;
    }
    return reinterpret_cast<abi::v3::entry_fn>(symbol);
#endif
}

/** @brief Outcome of looking up the optional CLI manifest symbol. */
enum class optional_symbol_result {
    missing,
    found,
    failed,
};

#if !defined(_WIN32)
/**
 * @brief Classify a POSIX loader diagnostic as an absent optional symbol.
 *
 * Linux/glibc reports "undefined symbol", while Darwin reports "symbol not found". Requiring
 * the requested symbol name prevents unrelated loader failures from being mistaken for an old
 * plugin that simply predates the optional export.
 *
 * @param diagnostic Text returned by dlerror().
 * @param symbol Exact optional symbol requested through dlsym().
 * @return true only for a supported missing-symbol diagnostic naming that symbol.
 */
constexpr bool is_missing_optional_symbol(std::string_view diagnostic,
                                          std::string_view symbol) noexcept
{
    const bool names_target = !symbol.empty() && diagnostic.find(symbol) != std::string_view::npos;
    const bool reports_missing =
        diagnostic.find("undefined symbol") != std::string_view::npos ||
        diagnostic.find("symbol not found") != std::string_view::npos ||
        diagnostic.find("Symbol not found") != std::string_view::npos;
    return names_target && reports_missing;
}

static_assert(is_missing_optional_symbol(
    "plugin.u42.so: undefined symbol: u42_get_cli_manifest", "u42_get_cli_manifest"));
static_assert(is_missing_optional_symbol(
    "dlsym(0x1, u42_get_cli_manifest): symbol not found", "u42_get_cli_manifest"));
static_assert(!is_missing_optional_symbol(
    "dlsym failed because the loader handle is invalid", "u42_get_cli_manifest"));
static_assert(!is_missing_optional_symbol(
    "plugin.u42.so: undefined symbol: another_export", "u42_get_cli_manifest"));
#endif

/**
 * @brief Resolve the optional CLI manifest entry from an existing library mapping.
 *
 * @param handle Existing mapping that already supplied the ABI v3 factory.
 * @param[out] out Receives the entry when found and is cleared otherwise.
 * @param[out] error Receives a loader diagnostic only for a lookup failure.
 * @return missing when the plugin contributes no CLI manifest, found on success, or failed for
 *         a platform loader error other than an absent optional symbol.
 */
optional_symbol_result resolve_manifest_entry(void* handle, cli::v1::entry_fn& out,
                                              std::string& error)
{
    out = nullptr;
#if defined(_WIN32)
    ::SetLastError(ERROR_SUCCESS);
    const FARPROC symbol =
        ::GetProcAddress(static_cast<HMODULE>(handle), cli::v1::entry_name);
    if (symbol == nullptr) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_SUCCESS || code == ERROR_PROC_NOT_FOUND) {
            return optional_symbol_result::missing;
        }
        ::SetLastError(code);
        error = "cannot resolve optional " + std::string(cli::v1::entry_name) + ": " +
                last_error_message();
        return optional_symbol_result::failed;
    }
    out = reinterpret_cast<cli::v1::entry_fn>(symbol);
#else
    ::dlerror();
    void* symbol = ::dlsym(handle, cli::v1::entry_name);
    const char* diagnostic = ::dlerror();
    if (diagnostic != nullptr) {
        const std::string_view loader_error(diagnostic);
        if (is_missing_optional_symbol(loader_error, cli::v1::entry_name)) {
            return optional_symbol_result::missing;
        }
        error = "cannot resolve optional " + std::string(cli::v1::entry_name) + ": " +
                std::string(loader_error);
        return optional_symbol_result::failed;
    }
    if (symbol == nullptr) {
        error = std::string(cli::v1::entry_name) + " resolves to null";
        return optional_symbol_result::failed;
    }
    out = reinterpret_cast<cli::v1::entry_fn>(symbol);
#endif
    return optional_symbol_result::found;
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

abi::v3::status plug::open(const std::filesystem::path& path, std::string& error)
{
    error.clear();
    if (handle_ != nullptr) {
        error = "library already open: '" + path_.string() + "'";
        return abi::v3::invalid_state;
    }
    if (path.empty()) {
        error = "plugin library path is empty";
        return abi::v3::invalid_argument;
    }

    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::canonical(path, ec);
    if (ec) {
        error = "cannot resolve plugin library '" + path.string() + "': " + ec.message();
        return ec == std::errc::no_such_file_or_directory ? abi::v3::not_found : abi::v3::failed;
    }

    std::string diagnostic;
    void* handle = map_library(canonical, diagnostic);
    if (handle == nullptr) {
        error = diagnostic;
        return abi::v3::failed;
    }

    abi::v3::entry_fn entry = resolve_entry(handle, diagnostic);
    if (entry == nullptr) {
        error = diagnostic;
        unmap_library(handle);
        return abi::v3::failed;
    }

    abi::v3::iplug_fty* factory = nullptr;
    const abi::v3::status negotiated = entry(abi::v3::abi_major, &factory);
    if (negotiated != abi::v3::ok) {
        error = "ABI negotiation failed for '" + canonical.string() + "': requested major " +
                std::to_string(abi::v3::abi_major) + ", library returned status " +
                std::to_string(negotiated);
        unmap_library(handle);
        return abi::v3::unsupported;
    }
    if (factory == nullptr) {
        error = "u42_get_factory returned a null factory for '" + canonical.string() + "'";
        unmap_library(handle);
        return abi::v3::failed;
    }

    handle_ = handle;
    factory_ = factory;
    path_ = std::move(canonical);
    return abi::v3::ok;
}

void plug::close() noexcept
{
    if (handle_ == nullptr) return;
    factory_ = nullptr;
    void* handle = std::exchange(handle_, nullptr);
    path_.clear();
    unmap_library(handle);
}

abi::v3::status discovered_plugin::open(const std::filesystem::path& path, std::string& error)
{
    namespace a = abi::v3;

    error.clear();
    if (library_ != nullptr || !path_.empty()) {
        error = "plugin already discovered: '" + path_.string() + "'";
        return a::invalid_state;
    }

    auto library = std::make_unique<plug>();
    a::status status = library->open(path, error);
    if (status != a::ok) return status;

    a::iplug_fty* const discovered_factory = library->factory();
    if (discovered_factory == nullptr) {
        error = "discovered library has no negotiated factory: '" + library->path().string() +
                "'";
        return a::failed;
    }

    const a::plug_desc* borrowed_description = nullptr;
    const a::status described = discovered_factory->describe(&borrowed_description);
    if (described != a::ok) {
        error = "factory describe() failed (" + status_text(described) + ")";
        return described;
    }

    plugin_description owned_description;
    status = copy_description(borrowed_description, owned_description, error);
    if (status != a::ok) return status;

    cli::v1::entry_fn manifest_entry = nullptr;
    const optional_symbol_result manifest_symbol =
        resolve_manifest_entry(library->handle_, manifest_entry, error);
    if (manifest_symbol == optional_symbol_result::failed) return a::failed;

    const cli::v1::manifest* borrowed_manifest = nullptr;
    if (manifest_symbol == optional_symbol_result::found) {
        const a::status negotiated =
            manifest_entry(cli::v1::manifest_major, &borrowed_manifest);
        if (negotiated != a::ok) {
            error = "CLI manifest negotiation failed for '" + library->path().string() +
                    "': requested major " + std::to_string(cli::v1::manifest_major) +
                    ", library returned status " + status_text(negotiated);
            return negotiated;
        }
        if (borrowed_manifest == nullptr) {
            error = std::string(cli::v1::entry_name) + " returned a null manifest for '" +
                    library->path().string() + "'";
            return a::failed;
        }
    }

    path_ = library->path();
    description_ = std::move(owned_description);
    manifest_ = borrowed_manifest;
    library_ = std::move(library);
    return a::ok;
}

abi::v3::status scan_plugins(const std::filesystem::path& directory,
                            std::vector<std::filesystem::path>& out, std::string& error)
{
    namespace fs = std::filesystem;

    out.clear();
    error.clear();

    if (directory.empty()) {
        error = "plugin directory is empty";
        return abi::v3::invalid_argument;
    }

    std::error_code ec;
    if (!fs::exists(directory, ec)) {
        if (ec) {
            error = "cannot inspect plugin directory '" + directory.string() + "': " + ec.message();
            return abi::v3::failed;
        }
        error = "plugin directory does not exist: '" + directory.string() + "'";
        return abi::v3::not_found;
    }
    if (!fs::is_directory(directory, ec)) {
        if (ec) {
            error = "cannot inspect plugin directory '" + directory.string() + "': " + ec.message();
            return abi::v3::failed;
        }
        error = "plugin path is not a directory: '" + directory.string() + "'";
        return abi::v3::invalid_argument;
    }

    std::vector<fs::path> candidates;
    const fs::directory_iterator end;
    for (fs::directory_iterator it(directory, ec); it != end; it.increment(ec)) {
        if (ec) {
            error = "cannot read plugin directory '" + directory.string() + "': " + ec.message();
            return abi::v3::failed;
        }

        const fs::directory_entry& entry = *it;
        const bool regular = entry.is_regular_file(ec);
        if (ec) {
            error = "cannot inspect plugin candidate '" + entry.path().string() + "': " +
                    ec.message();
            return abi::v3::failed;
        }
        if (!regular || !has_plugin_suffix(entry.path())) continue;

        fs::path canonical = fs::canonical(entry.path(), ec);
        if (ec) {
            error = "cannot resolve plugin candidate '" + entry.path().string() + "': " +
                    ec.message();
            return abi::v3::failed;
        }
        candidates.push_back(std::move(canonical));
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const fs::path& left, const fs::path& right) {
                  return left.native() < right.native();
              });
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    out = std::move(candidates);
    return abi::v3::ok;
}

} // namespace u42
