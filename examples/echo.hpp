#pragma once

/**
 * @file echo.hpp
 * @brief Shared data contract of the two example plugins.
 *
 * @details
 * The example provider and the example consumer share no business class, no interface pointer and
 * no provider query result. This header therefore declares data only: the numeric plugin version
 * each side publishes or expects, the inclusive version range the consumer accepts for the
 * provider, the method numbers of the published methods, and nothing else. Business traffic
 * travels through the host gateway, so the provider's C++ type never has to cross the
 * translation-unit boundary: the consumer holds a pointerless lease credential plus the actual
 * version the host granted for it and invokes the provider through icalls.
 *
 * The published methods, all synchronous with a JSON argument document and a JSON result document:
 * - 1 "echo": copy the request bytes through the caller's writer unchanged.
 * - 2 "fail": copy a partial JSON prefix and then report failure, so a host can observe that
 *   partial output is discarded atomically.
 *
 * @note The header depends exclusively on the frozen plugin ABI in <42u/abi.hpp> and includes no
 *       SDK wrapper, so an example can be built with a bare compiler command and requires no host
 *       implementation to compile or link.
 */

#include <42u/abi.hpp>

#include <cstdint>

/**
 * @brief Short alias for the frozen ABI namespace.
 *
 * @note Redeclaring the same alias for the same namespace is harmless, so this definition
 *       coexists with any alias of the same name provided elsewhere.
 */
namespace a = u42::abi::v3;

namespace example {

/// @brief Numeric plugin version published by the echo provider.
constexpr a::plugin_version echo_version{1u, 0u, 0u};

/// @brief Numeric plugin version published by the consumer.
constexpr a::plugin_version consumer_version{1u, 0u, 0u};

/**
 * @brief Inclusive version range the consumer accepts for the echo provider.
 *
 * @details
 * The consumer accepts every published version on major 1, up to the unsigned maximum for minor
 * and patch. This is the caller's explicit policy, not an inferred compatibility promise: the host
 * only filters against the range it is given and returns the provider's actual version so the
 * consumer can adapt its own handling if needed.
 */
constexpr a::version_range echo_versions{{1u, 0u, 0u}, {1u, UINT32_MAX, UINT32_MAX}};

/// @brief Method number of the provider's verbatim copy method.
constexpr a::method_id echo_method_id = 1;

/// @brief Method number of the provider's deliberate partial-then-fail method.
constexpr a::method_id fail_method_id = 2;

/// @brief Method number of the consumer's status method.
constexpr a::method_id status_method_id = 1;

} // namespace example
