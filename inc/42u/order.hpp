#pragma once
#include <42u/abi.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace u42 {
/**
 * @brief Host-owned copy of immutable plugin identity and initialization constraints.
 */
struct order_node {
    std::string plug_id;
    std::int32_t priority = 0;
    std::vector<std::string> before;
    std::vector<std::string> after;
};
/**
 * @brief Plan pending nodes with hard edges, then ascending priority and UTF-8 byte order.
 * @param nodes Pending plugins. Duplicate identities, unknown targets and cycles are rejected.
 * @param initialized Existing initialized identities; after is satisfied, before is impossible.
 * @param out Indices into nodes; cleared on failure.
 * @param error Human-readable failure diagnostic; empty on success.
 * @return ABI status, including duplicate, not_found, invalid_state and cycle.
 */
abi::v3::status plan_order(const std::vector<order_node>& nodes,
                          const std::vector<std::string>& initialized,
                          std::vector<std::size_t>& out, std::string& error);
} // namespace u42
