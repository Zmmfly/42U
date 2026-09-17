#pragma once

/**
 * @file echo.hpp
 * @brief Shared contract of the example echo provider plugin.
 *
 * @details
 * This header is the only artifact shared by the two example plugins. It depends
 * exclusively on the frozen plugin ABI in <42u/abi.hpp> and intentionally does not
 * include any SDK wrapper header, so each example can be built with a bare compiler
 * command and requires no host implementation.
 */

#include <42u/abi.hpp>

/**
 * @brief Short alias for the frozen ABI namespace.
 *
 * @note Redeclaring the same alias for the same namespace is harmless, so this
 *       definition coexists with an SDK-provided alias of the same name.
 */
namespace a = u42::abi::v1;

namespace example {

/**
 * @brief Identity of the echo provider interface contract.
 *
 * @note The value is frozen forever. It identifies the immutable interface
 *       contract, not any particular provider instance.
 */
constexpr a::iid echo_iid{0x6563686f34325531ULL, 1};

/**
 * @brief Synchronous echo interface published by the example provider.
 *
 * @note An implementation is borrowed through icaps::acquire and stays usable
 *       only while the host borrowing credential is held. A consumer must clear
 *       its cached pointer and return the credential in irevoker::on_revoke.
 */
struct iecho {
    /**
     * @brief Return the caller-supplied JSON payload unchanged.
     *
     * @param input Borrowed request bytes; a null data pointer is accepted only
     *              together with a zero size.
     * @param out Caller-owned output writer; never retained after the call.
     * @retval ok The whole payload was accepted by the writer.
     * @retval invalid_argument A required argument was null or inconsistent.
     * @retval failed The writer rejected the write or threw across the ABI.
     *
     * @note The provider neither parses nor retains the payload; it only copies
     *       the bytes through the writer, which preserves them exactly.
     */
    virtual a::status U42_CALL echo(a::bytes input, a::iwriter* out) noexcept = 0;

protected:
    ~iecho() = default;
};

} // namespace example
