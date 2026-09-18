/**
 * @file missing_entry.cc
 * @brief Negative fixture: a valid library that exports no u42_get_factory entry.
 *
 * The ABI header is deliberately not included: it declares a dllexport'd u42_get_factory for
 * the current profile (<42u/abi.hpp> now declares it for u42::abi::v3), which would turn this
 * fixture into an unresolved-export link error. The library therefore only carries one
 * unrelated symbol, so the loader failure can be attributed to the missing entry point rather
 * than to a mapping failure.
 *
 * @note The fixture intentionally references no ABI version at all, so it stays valid across
 *       ABI revisions; only its unrelated symbol proves that the library mapped successfully.
 */

#if defined(_WIN32)
#  define U42_FIXTURE_EXPORT __declspec(dllexport)
#  define U42_FIXTURE_CALL __cdecl
#else
#  define U42_FIXTURE_EXPORT __attribute__((visibility("default")))
#  define U42_FIXTURE_CALL
#endif

/**
 * @brief Unrelated symbol that proves the library itself mapped successfully.
 *
 * @return Fixed marker value; the integration test only checks that the symbol resolves.
 */
extern "C" U42_FIXTURE_EXPORT int U42_FIXTURE_CALL u42_fixture_unrelated_symbol() noexcept
{
    return 42;
}
