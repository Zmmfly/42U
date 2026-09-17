/**
 * @file order.cc
 * @brief Deterministic initialization planning for pending plugins.
 *
 * The planner is a pure function over immutable node descriptions: it validates
 * identities, derives hard ordering edges, checks them against already initialized
 * identities and emits a deterministic order. It reads and mutates no host state.
 */
#include <42u/order.hpp>

#include <algorithm>
#include <cstddef>
#include <map>
#include <new>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace u42 {
namespace {

/**
 * @brief Compare two strings by their UTF-8 bytes interpreted as unsigned values.
 *
 * @param left First string.
 * @param right Second string.
 * @return A negative value when left sorts first, zero when equal, positive otherwise.
 * @note Plain std::string ordering depends on whether char is signed, so multi-byte
 *       UTF-8 sequences would otherwise sort inconsistently across platforms.
 */
int compare_utf8_bytes(const std::string& left, const std::string& right)
{
    const std::size_t shared = left.size() < right.size() ? left.size() : right.size();
    for (std::size_t i = 0; i < shared; ++i) {
        const unsigned char l = static_cast<unsigned char>(left[i]);
        const unsigned char r = static_cast<unsigned char>(right[i]);
        if (l != r) return l < r ? -1 : 1;
    }
    if (left.size() == right.size()) return 0;
    return left.size() < right.size() ? -1 : 1;
}

/** @brief Comparator giving associative containers the same unsigned byte order. */
struct utf8_less {
    bool operator()(const std::string& left, const std::string& right) const
    {
        return compare_utf8_bytes(left, right) < 0;
    }
};

/**
 * @brief Planner body; the public entry point only backstops allocation failures.
 *
 * @param nodes Pending plugins, indexed as reported through out.
 * @param initialized Already initialized identities.
 * @param out Receives indices into nodes in planned order.
 * @param error Receives a specific diagnostic on failure.
 * @return abi::v2::ok on success, otherwise the first detected violation.
 */
abi::v2::status plan_order_impl(const std::vector<order_node>& nodes,
                               const std::vector<std::string>& initialized,
                               std::vector<std::size_t>& out, std::string& error)
{
    const std::size_t count = nodes.size();
    std::map<std::string, std::size_t, utf8_less> pending;
    std::set<std::string, utf8_less> settled;

    for (std::size_t i = 0; i < count; ++i) {
        const std::string& id = nodes[i].plug_id;
        if (id.empty()) {
            error = "pending node " + std::to_string(i) + " has an empty plug_id";
            return abi::v2::invalid_argument;
        }
        if (!pending.emplace(id, i).second) {
            error = "duplicate pending plug_id '" + id + "'";
            return abi::v2::duplicate;
        }
    }
    for (std::size_t i = 0; i < initialized.size(); ++i) {
        const std::string& id = initialized[i];
        if (id.empty()) {
            error = "initialized entry " + std::to_string(i) + " has an empty plug_id";
            return abi::v2::invalid_argument;
        }
        if (!settled.insert(id).second) {
            error = "duplicate initialized plug_id '" + id + "'";
            return abi::v2::duplicate;
        }
        if (pending.find(id) != pending.end()) {
            error = "plug_id '" + id + "' is both pending and initialized";
            return abi::v2::duplicate;
        }
    }

    std::vector<std::vector<std::size_t>> successors(count);
    std::vector<std::size_t> indegree(count, 0);
    std::set<std::pair<std::size_t, std::size_t>> edges;
    // A hard edge may be declared from either endpoint; keep exactly one copy so that
    // duplicated declarations cannot inflate indegree and fake a cycle.
    const auto add_edge = [&](std::size_t from, std::size_t to) {
        if (edges.insert(std::make_pair(from, to)).second) {
            successors[from].push_back(to);
            ++indegree[to];
        }
    };

    for (std::size_t i = 0; i < count; ++i) {
        const order_node& node = nodes[i];
        for (const std::string& target : node.before) {
            if (target == node.plug_id) {
                error = "plugin '" + node.plug_id + "' lists itself in before";
                return abi::v2::cycle;
            }
            if (settled.find(target) != settled.end()) {
                error = "plugin '" + node.plug_id + "' cannot start before already initialized '" +
                        target + "'";
                return abi::v2::invalid_state;
            }
            const auto found = pending.find(target);
            if (found == pending.end()) {
                error = "plugin '" + node.plug_id + "' declares unknown before target '" + target +
                        "'";
                return abi::v2::not_found;
            }
            add_edge(i, found->second);
        }
        for (const std::string& target : node.after) {
            if (target == node.plug_id) {
                error = "plugin '" + node.plug_id + "' lists itself in after";
                return abi::v2::cycle;
            }
            // An initialized target already satisfies the constraint; it adds no edge.
            if (settled.find(target) != settled.end()) continue;
            const auto found = pending.find(target);
            if (found == pending.end()) {
                error = "plugin '" + node.plug_id + "' declares unknown after target '" + target +
                        "'";
                return abi::v2::not_found;
            }
            add_edge(found->second, i);
        }
    }

    std::vector<std::size_t> ready;
    ready.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (indegree[i] == 0) ready.push_back(i);
    }

    std::vector<bool> planned(count, false);
    out.reserve(count);
    while (!ready.empty()) {
        std::size_t best = 0;
        for (std::size_t candidate = 1; candidate < ready.size(); ++candidate) {
            const order_node& lhs = nodes[ready[candidate]];
            const order_node& rhs = nodes[ready[best]];
            if (lhs.priority < rhs.priority) {
                best = candidate;
            } else if (lhs.priority == rhs.priority &&
                       compare_utf8_bytes(lhs.plug_id, rhs.plug_id) < 0) {
                best = candidate;
            }
        }
        const std::size_t index = ready[best];
        ready.erase(ready.begin() + static_cast<std::vector<std::size_t>::difference_type>(best));
        planned[index] = true;
        out.push_back(index);
        for (std::size_t next : successors[index]) {
            if (--indegree[next] == 0) ready.push_back(next);
        }
    }

    if (out.size() != count) {
        std::vector<std::string> blocked;
        for (std::size_t i = 0; i < count; ++i) {
            if (!planned[i]) blocked.push_back(nodes[i].plug_id);
        }
        std::sort(blocked.begin(), blocked.end(), utf8_less{});
        std::string list;
        for (const std::string& id : blocked) {
            if (!list.empty()) list += ", ";
            list += "'" + id + "'";
        }
        error = "dependency cycle; unplannable pending plugins: " + list;
        out.clear();
        return abi::v2::cycle;
    }
    return abi::v2::ok;
}

} // namespace

abi::v2::status plan_order(const std::vector<order_node>& nodes,
                          const std::vector<std::string>& initialized,
                          std::vector<std::size_t>& out, std::string& error)
{
    out.clear();
    error.clear();
    try {
        return plan_order_impl(nodes, initialized, out, error);
    } catch (const std::bad_alloc&) {
        out.clear();
        error = "out of memory while planning the plugin order";
        return abi::v2::failed;
    } catch (...) {
        out.clear();
        error = "unexpected exception while planning the plugin order";
        return abi::v2::failed;
    }
}
} // namespace u42
