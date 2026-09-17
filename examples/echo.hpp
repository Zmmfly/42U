#pragma once

/**
 * @file echo.hpp
 * @brief Shared data contract of the two example plugins.
 *
 * @details
 * The example provider and the example consumer share no business class, no interface pointer and
 * no provider query result. This header therefore declares data only: the protocol each side
 * publishes or expects, the method numbers of those protocols, and nothing else. Business traffic
 * travels through the host gateway, so the provider's C++ type never has to cross the
 * translation-unit boundary: the consumer holds a pointerless lease credential, binds a method
 * with that credential and invokes the provider through icalls.
 *
 * @note The header depends exclusively on the frozen plugin ABI in <42u/abi.hpp> and includes no
 *       SDK wrapper, so an example can be built with a bare compiler command and requires no host
 *       implementation to compile or link.
 */

#include <42u/abi.hpp>

/**
 * @brief Short alias for the frozen ABI namespace.
 *
 * @note Redeclaring the same alias for the same namespace is harmless, so this definition
 *       coexists with any alias of the same name provided elsewhere.
 */
namespace a = u42::abi::v2;

namespace example {

/**
 * @brief Business protocol published by the echo provider.
 *
 * @details
 * The identifier high half spells "echo42U2"; major 1 is the first published contract and minor 0
 * promises no revision older than itself. The provider repeats exactly this contract in its
 * capability announcement, and a consumer must accept an offer only through
 * compatible_contract(offered, echo_contract); the plugin release version is never consulted.
 *
 * Methods, all synchronous with a JSON argument document and a JSON result document:
 * - 1 "echo": copy the request bytes through the caller's writer unchanged.
 * - 2 "fail": copy a partial JSON prefix and then report failure, so a host can observe that
 *   partial output is discarded atomically.
 */
constexpr a::contract echo_contract{{0x6563686f34325532ULL, 1}, 1, 0};

/**
 * @brief Business protocol published by the consumer.
 *
 * @details
 * The identifier high half spells "cons42U2". The consumer discloses only its own optional status
 * method and never acquires this contract itself.
 *
 * Methods, all synchronous with a JSON argument document and a JSON result document:
 * - 1 "status": report {"connected":bool,"revocations":n,"events":n} for the current instance.
 */
constexpr a::contract consumer_contract{{0x636f6e7334325532ULL, 1}, 1, 0};

/// @brief Method number of the provider's verbatim copy method.
constexpr a::method_id echo_method_id = 1;

/// @brief Method number of the provider's deliberate partial-then-fail method.
constexpr a::method_id fail_method_id = 2;

/// @brief Method number of the consumer's status method.
constexpr a::method_id status_method_id = 1;

} // namespace example
