/**
 * @file legacy_v1.cc
 * @brief Negative fixture: a frozen ABI v1 provider that an ABI v2 host must refuse.
 *
 * @details
 * The library deliberately does not include <42u/abi.hpp>. It re-declares the retired v1
 * entry profile with fixed C types - a @c uint32_t requested major and a @c void** factory
 * out-parameter - so it keeps exporting exactly the symbol the first SDK shipped, without
 * depending on any header this repository may still be migrating.
 *
 * The entry answers a v1 negotiation (major 1) with one recognizable static object and
 * refuses every other major with @c unsupported while clearing the output slot, which is what
 * the current profile (major 2) always requests. A v2 loader therefore has to reject the
 * library during negotiation instead of treating the v1 object as a factory.
 *
 * @note This file must not be linked into any test binary and must not be placed in a plugin
 *       directory that boot() scans; it is only ever opened directly to prove the refusal.
 * @warning The static object is intentionally not a valid v2 factory. Never cast the pointer
 *          this entry returns to any u42::abi::v2 type.
 */

#include <cstdint>

#if defined(_WIN32)
#  define U42_LEGACY_EXPORT __declspec(dllexport)
#  define U42_LEGACY_CALL __cdecl
#else
#  define U42_LEGACY_EXPORT __attribute__((visibility("default")))
#  define U42_LEGACY_CALL
#endif

namespace {

/** @brief Frozen v1 status values used by the retired entry profile. */
using legacy_status = std::uint32_t;
inline constexpr legacy_status legacy_ok = 0;
inline constexpr legacy_status legacy_invalid_argument = 1;
inline constexpr legacy_status legacy_unsupported = 2;

/**
 * @brief Recognizable v1-only static object handed out for a major-1 negotiation.
 *
 * @note The magic value is frozen so a probe can verify the identity of the object without
 *       knowing anything about the v2 ABI. "LEGACY1" is stored as its byte sequence.
 */
struct legacy_object {
    /** @brief Frozen identity word, @c 0x004c454741435931 ("LEGACY1"). */
    std::uint64_t magic;
    /** @brief ABI major this object was written for; always 1. */
    std::uint32_t abi_major;
    /** @brief Padding keeps the object's alignment and layout explicit. */
    std::uint32_t reserved;
};

/** @brief Magic word stored in legacy_singleton; mirrored by the integration probe. */
inline constexpr std::uint64_t legacy_object_magic = 0x004c454741435931ULL;

/// @brief The one recognizable object this retired library still serves.
legacy_object legacy_singleton{legacy_object_magic, 1u, 0u};

} // namespace

/**
 * @brief Retired ABI v1 factory entry, fixed to the original C shape.
 *
 * @param major Requested ABI major; only 1 is answered.
 * @param out Receives the static v1 object for major 1, otherwise null.
 * @return @c legacy_ok for major 1, @c legacy_unsupported for every other major, and
 *         @c legacy_invalid_argument for a null output slot.
 */
extern "C" U42_LEGACY_EXPORT legacy_status U42_LEGACY_CALL u42_get_factory(
    std::uint32_t major, void** out) noexcept
{
    if (out == nullptr) return legacy_invalid_argument;
    *out = nullptr;
    if (major == 1u) {
        *out = &legacy_singleton;
        return legacy_ok;
    }
    return legacy_unsupported;
}

/**
 * @brief Address of the recognizable static object.
 *
 * @return Pointer to the one static v1 object, so a probe can compare identity without reading
 *         a structure whose layout the caller does not know.
 */
extern "C" U42_LEGACY_EXPORT void* U42_LEGACY_CALL u42_legacy_object_address() noexcept
{
    return &legacy_singleton;
}

/**
 * @brief Magic word stored in the recognizable static object.
 *
 * @return The frozen @c legacy_object_magic value.
 */
extern "C" U42_LEGACY_EXPORT std::uint64_t U42_LEGACY_CALL u42_legacy_object_magic_word() noexcept
{
    return legacy_object_magic;
}
