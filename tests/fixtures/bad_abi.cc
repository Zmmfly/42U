/**
 * @file bad_abi.cc
 * @brief Negative fixture: loadable library whose factory entry rejects the ABI major.
 *
 * Built as a standalone shared library that is intentionally not linked against the
 * project sources. u42::plug::open() must surface abi::v1::unsupported, and the exported
 * entry must clear the caller's output slot before reporting the rejection.
 */
#include <42u/abi.hpp>

#include <cstdint>

extern "C" U42_EXPORT u42::abi::v1::status U42_CALL u42_get_factory(
    std::uint32_t major, u42::abi::v1::iplug_fty** out) noexcept
{
    (void)major;
    // A rejected negotiation must never leave a stale factory behind for the caller.
    if (out != nullptr) *out = nullptr;
    return u42::abi::v1::unsupported;
}
