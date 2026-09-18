/**
 * @file bad_abi.cc
 * @brief Negative fixture: loadable library whose factory entry rejects the ABI major.
 *
 * Built as a standalone shared library that is intentionally not linked against the
 * project sources. The fixture only references the current frozen profile in <42u/abi.hpp>
 * and deliberately rejects it, so u42::plug::open() must surface abi::v3::unsupported, and
 * the exported entry must clear the caller's output slot before reporting the rejection.
 *
 * @note Rejecting the current major (rather than negotiating an older one) keeps this fixture
 *       distinct from tests/fixtures/legacy_v1.cc and legacy_v2.cc, which serve frozen ABI v1
 *       and v2 profiles and are refused from the other direction.
 */
#include <42u/abi.hpp>

#include <cstdint>

extern "C" U42_EXPORT u42::abi::v3::status U42_CALL u42_get_factory(
    std::uint32_t major, u42::abi::v3::iplug_fty** out) noexcept
{
    (void)major;
    // A rejected negotiation must never leave a stale factory behind for the caller.
    if (out != nullptr) *out = nullptr;
    return u42::abi::v3::unsupported;
}
