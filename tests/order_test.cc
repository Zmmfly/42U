/**
 * @file order_test.cc
 * @brief Self-contained checks for u42::plan_order; no external test framework.
 *
 * The file owns its main(), so it links against src/order.cc alone:
 *   g++ -std=c++17 -Wall -Wextra -Werror -Iinc src/order.cc tests/order_test.cc -o /tmp/u42-order-test
 *
 * @note NDEBUG is defined deliberately: CHECK must keep reporting failures even when
 *       assert() has been compiled out, so a diagnostic can never vanish in release builds.
 */
#define NDEBUG 1

#include <42u/order.hpp>

#include <cassert>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// The planner types live in namespace u42; keep the checks readable.
using u42::order_node;
namespace abi = u42::abi;

const char* g_current_test = nullptr;
using check_hook = void (*)(const char* expr, const char* file, int line);
check_hook g_check_hook = nullptr;

/**
 * @brief Report one failed CHECK and stop the process with a non-zero status.
 *
 * @param expr Failed expression text.
 * @param file Source file of the failed CHECK.
 * @param line Source line of the failed CHECK.
 */
[[noreturn]] void fail_check(const char* expr, const char* file, int line)
{
    if (g_check_hook != nullptr) g_check_hook(expr, file, line);
    std::fprintf(stderr, "CHECK failed in %s: %s (%s:%d)\n",
                 g_current_test != nullptr ? g_current_test : "?", expr, file, line);
    std::fflush(stderr);
    std::exit(EXIT_FAILURE);
}

/** @brief Failure reporting that never compiles away, unlike assert(). */
#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) fail_check(#condition, __FILE__, __LINE__);            \
    } while (false)

#define U42_STRINGIFY(...) #__VA_ARGS__
#define U42_EXPAND_STRING(...) U42_STRINGIFY(__VA_ARGS__)

/**
 * @brief Compile-time guard: CHECK must expand to a call of fail_check, never to assert().
 *
 * With NDEBUG defined, assert() collapses to ((void)0), so every failure would silently
 * disappear. This is checked on the expansion text because a runtime probe cannot detect
 * it: if CHECK had become a no-op, the probe itself would be a no-op as well.
 */
inline constexpr std::string_view u42_check_expansion = U42_EXPAND_STRING(CHECK(0 == 1));
static_assert(u42_check_expansion.find("fail_check") != std::string_view::npos,
              "CHECK must call fail_check directly; defining it as assert() disables it "
              "under NDEBUG");

struct test_case {
    const char* name;
    void (*run)();
};

std::vector<test_case>& tests()
{
    static std::vector<test_case> all;
    return all;
}

struct registrar {
    registrar(const char* name, void (*run)()) { tests().push_back(test_case{name, run}); }
};

/** @brief Register one case without any test framework. */
#define TEST_CASE(name)                                                         \
    void name();                                                                \
    [[maybe_unused]] const registrar reg_##name(#name, &name);                  \
    void name()

/** @brief Build one node with explicit constraints. */
order_node make_node(std::string id, std::int32_t priority,
                     std::vector<std::string> before = {}, std::vector<std::string> after = {})
{
    order_node node;
    node.plug_id = std::move(id);
    node.priority = priority;
    node.before = std::move(before);
    node.after = std::move(after);
    return node;
}

/** @brief Render a plan as plugin identities for readable comparisons. */
std::vector<std::string> planned_ids(const std::vector<order_node>& nodes,
                                    const std::vector<std::size_t>& out)
{
    std::vector<std::string> ids;
    ids.reserve(out.size());
    for (std::size_t index : out) {
        CHECK(index < nodes.size());
        ids.push_back(nodes[index].plug_id);
    }
    return ids;
}

/** @brief Join identities for failure output. */
std::string join(const std::vector<std::string>& values)
{
    std::string text;
    for (const std::string& value : values) {
        if (!text.empty()) text += ", ";
        text += value;
    }
    return text;
}

/**
 * @brief Run plan_order and assert status, output hygiene and exact order.
 *
 * @param label Case label used in diagnostics.
 * @param nodes Pending nodes under test.
 * @param initialized Already initialized identities.
 * @param expected Expected ABI status.
 * @param expected_order Expected planned identities when expected is abi::v3::ok.
 * @param needle Optional substring the failure diagnostic must contain.
 */
void expect_plan(const char* label, const std::vector<order_node>& nodes,
                 const std::vector<std::string>& initialized, abi::v3::status expected,
                 const std::vector<std::string>& expected_order, const char* needle = nullptr)
{
    // Deliberately dirty: a failure must clear it, a success must replace it.
    std::vector<std::size_t> out(nodes.size() + 2, 999);
    std::string error = "stale diagnostic";
    const abi::v3::status actual = u42::plan_order(nodes, initialized, out, error);
    if (actual != expected) {
        std::fprintf(stderr, "%s: status %u != %u, error='%s'\n", label, actual, expected,
                     error.c_str());
        std::fflush(stderr);
        std::exit(EXIT_FAILURE);
    }
    if (expected == abi::v3::ok) {
        CHECK(error.empty());
        const std::vector<std::string> got = planned_ids(nodes, out);
        if (got != expected_order) {
            std::fprintf(stderr, "%s: expected [%s] but got [%s]\n", label,
                         join(expected_order).c_str(), join(got).c_str());
            std::fflush(stderr);
            std::exit(EXIT_FAILURE);
        }
    } else {
        CHECK(out.empty());
        CHECK(!error.empty());
        if (needle != nullptr && error.find(needle) == std::string::npos) {
            std::fprintf(stderr, "%s: error '%s' does not mention '%s'\n", label, error.c_str(),
                         needle);
            std::fflush(stderr);
            std::exit(EXIT_FAILURE);
        }
    }
}

TEST_CASE(empty_input_is_ok)
{
    expect_plan("empty", {}, {}, abi::v3::ok, {});
}

TEST_CASE(priority_ascending)
{
    const std::vector<order_node> nodes = {
        make_node("low", 5), make_node("high", -1), make_node("mid", 0)};
    expect_plan("priority", nodes, {}, abi::v3::ok, {"high", "mid", "low"});
}

TEST_CASE(priority_tie_uses_utf8_byte_order)
{
    // Byte order: A(0x41) < a(0x61) < ab < z(0x7a) < é(0xc3 0xa9). A signed char
    // comparison would sort the two-byte sequence first, so this pins unsigned bytes.
    const std::vector<order_node> ascending = {make_node("A", 3), make_node("a", 3),
                                              make_node("ab", 3), make_node("z", 3),
                                              make_node("\xC3\xA9", 3)};
    const std::vector<std::string> expected = {"A", "a", "ab", "z", "\xC3\xA9"};
    expect_plan("tie-insertion-order", ascending, {}, abi::v3::ok, expected);

    const std::vector<order_node> reversed = {make_node("\xC3\xA9", 3), make_node("z", 3),
                                             make_node("ab", 3), make_node("a", 3),
                                             make_node("A", 3)};
    expect_plan("tie-reversed-input", reversed, {}, abi::v3::ok, expected);
}

TEST_CASE(hard_edges_override_priority)
{
    std::vector<order_node> nodes;
    nodes.push_back(make_node("log", 1, {}, {"svc"}));
    nodes.push_back(make_node("svc", 100));
    nodes.push_back(make_node("app", 50, {"svc"}));
    // app must precede svc, svc must precede log, whatever the priorities say.
    expect_plan("hard-edges", nodes, {}, abi::v3::ok, {"app", "svc", "log"});

    std::vector<order_node> mirrored;
    mirrored.push_back(make_node("a", 10, {"b"}));
    mirrored.push_back(make_node("b", 0, {}, {"a"}));
    expect_plan("hard-edge-declared-twice", mirrored, {}, abi::v3::ok, {"a", "b"});
}

TEST_CASE(duplicate_edges_are_deduplicated)
{
    std::vector<order_node> nodes;
    nodes.push_back(make_node("a", 5, {"b", "b", "b"}));
    nodes.push_back(make_node("b", 0, {}, {"a", "a"}));
    // Both endpoints declare the same hard edge, several times over. The plan must stay a
    // single a-before-b constraint: a dedup bug that records an edge once but counts it
    // more often (or the reverse) would strand b and report a bogus cycle here.
    expect_plan("duplicate-edges", nodes, {}, abi::v3::ok, {"a", "b"});
}

TEST_CASE(self_edges_are_cycles)
{
    expect_plan("self-before", {make_node("solo", 0, {"solo"})}, {}, abi::v3::cycle, {}, "'solo'");
    expect_plan("self-after", {make_node("solo", 0, {}, {"solo"})}, {}, abi::v3::cycle, {},
                "'solo'");
    const std::vector<order_node> both = {make_node("solo", 0, {"solo"}, {"solo"}),
                                         make_node("other", 1)};
    expect_plan("self-both", both, {}, abi::v3::cycle, {}, "itself");
}

TEST_CASE(mutual_edges_are_cycles)
{
    const std::vector<order_node> before_cycle = {make_node("a", 0, {"b"}),
                                                 make_node("b", 1, {"a"})};
    expect_plan("mutual-before", before_cycle, {}, abi::v3::cycle, {}, "cycle");

    const std::vector<order_node> after_cycle = {make_node("a", 0, {}, {"b"}),
                                                make_node("b", 1, {}, {"a"})};
    expect_plan("mutual-after", after_cycle, {}, abi::v3::cycle, {}, "cycle");
}

TEST_CASE(graph_cycle_clears_output)
{
    std::vector<order_node> nodes;
    nodes.push_back(make_node("x", 0, {"y"}));
    nodes.push_back(make_node("y", 0, {"z"}));
    nodes.push_back(make_node("z", 0, {"x"}));
    nodes.push_back(make_node("solo", -100)); // plannable alone, still reported as a failure
    expect_plan("graph-cycle", nodes, {}, abi::v3::cycle, {}, "cycle");

    const std::vector<order_node> trailing = {make_node("a", 0), make_node("b", 0, {"c"}),
                                             make_node("c", 0, {"b"})};
    expect_plan("graph-cycle-tail", trailing, {}, abi::v3::cycle, {}, "cycle");
}

TEST_CASE(unknown_targets_are_not_found)
{
    expect_plan("unknown-before", {make_node("a", 0, {"ghost"})}, {}, abi::v3::not_found, {},
                "'ghost'");
    expect_plan("unknown-after", {make_node("a", 0, {}, {"ghost"})}, {}, abi::v3::not_found, {},
                "'ghost'");
    const std::vector<order_node> mixed = {make_node("a", 0), make_node("b", 0, {}, {"ghost"})};
    expect_plan("unknown-after-mixed", mixed, {}, abi::v3::not_found, {}, "'ghost'");
    expect_plan("empty-target", {make_node("a", 0, {""})}, {}, abi::v3::not_found, {}, "'a'");
}

TEST_CASE(initialized_constraints_are_enforced)
{
    // before an initialized plugin is impossible during hot load.
    const std::vector<order_node> impossible = {make_node("ready", 0, {}, {"init"}),
                                                make_node("late", 0, {"init"})};
    expect_plan("before-initialized", impossible, {"init"}, abi::v3::invalid_state, {}, "'init'");

    // after an initialized plugin is already satisfied and adds no edge.
    const std::vector<order_node> satisfied = {make_node("b", 1, {}, {"init"}),
                                               make_node("a", 5)};
    expect_plan("after-initialized", satisfied, {"init"}, abi::v3::ok, {"b", "a"});

    const std::vector<order_node> only_after = {make_node("a", 0, {}, {"init"})};
    expect_plan("after-initialized-only", only_after, {"init"}, abi::v3::ok, {"a"});
}

TEST_CASE(duplicate_identities_are_rejected)
{
    const std::vector<order_node> pending_duplicate = {make_node("dup", 0), make_node("dup", 1)};
    expect_plan("duplicate-pending", pending_duplicate, {}, abi::v3::duplicate, {}, "'dup'");

    const std::vector<order_node> triple = {make_node("dup", 0), make_node("dup", 1),
                                            make_node("dup", 2)};
    expect_plan("duplicate-pending-triple", triple, {}, abi::v3::duplicate, {}, "'dup'");

    const std::vector<order_node> single = {make_node("x", 0)};
    expect_plan("duplicate-initialized", single, {"init", "init"}, abi::v3::duplicate, {},
                "'init'");
    expect_plan("pending-and-initialized", single, {"x"}, abi::v3::duplicate, {}, "'x'");
}

TEST_CASE(empty_identities_are_rejected)
{
    const std::vector<order_node> blank = {make_node("a", 0), make_node("", 1)};
    expect_plan("empty-pending-id", blank, {}, abi::v3::invalid_argument, {}, "empty");

    const std::vector<order_node> valid = {make_node("a", 0)};
    expect_plan("empty-initialized-id", valid, {""}, abi::v3::invalid_argument, {}, "empty");
    expect_plan("empty-pending-first", {make_node("", 0)}, {}, abi::v3::invalid_argument, {},
                "empty");
    // Two blank identities are invalid_argument, not duplicate: emptiness is rejected first.
    const std::vector<order_node> two_blank = {make_node("", 0), make_node("", 0)};
    expect_plan("empty-pending-twice", two_blank, {}, abi::v3::invalid_argument, {}, "empty");
    expect_plan("empty-initialized-twice", valid, {"", ""}, abi::v3::invalid_argument, {},
                "empty");
}

TEST_CASE(ready_set_is_reselected_after_every_step)
{
    std::vector<order_node> nodes;
    nodes.push_back(make_node("a", 5));
    nodes.push_back(make_node("b", 3));
    nodes.push_back(make_node("c", 0, {"d"}));
    nodes.push_back(make_node("d", 2));
    // The minimum is recomputed from the whole ready set after each pick.
    expect_plan("reselection", nodes, {}, abi::v3::ok, {"c", "d", "b", "a"});
}

TEST_CASE(output_and_error_are_hygienic)
{
    const std::vector<order_node> two = {make_node("a", 0), make_node("b", 1)};
    const std::vector<order_node> one = {make_node("solo", 0)};
    std::vector<std::size_t> out;
    std::string error = "stale";
    CHECK(u42::plan_order(two, {}, out, error) == abi::v3::ok);
    CHECK(out.size() == 2);
    CHECK(error.empty());

    error = "stale again";
    CHECK(u42::plan_order(one, {}, out, error) == abi::v3::ok);
    CHECK(out.size() == 1);
    CHECK(out[0] == 0);
    CHECK(error.empty());

    out = {7, 7, 7};
    error = "keep";
    const std::vector<order_node> broken = {make_node("a", 0, {}, {"ghost"})};
    CHECK(u42::plan_order(broken, {}, out, error) == abi::v3::not_found);
    CHECK(out.empty());
    CHECK(!error.empty());

    // A failure must not poison the next call.
    error = "keep";
    CHECK(u42::plan_order(two, {}, out, error) == abi::v3::ok);
    CHECK(out.size() == 2);
    CHECK(error.empty());
}

TEST_CASE(long_chain_stays_linear)
{
    constexpr std::size_t kCount = 128;
    std::vector<order_node> nodes;
    std::vector<std::string> expected;
    for (std::size_t i = 0; i < kCount; ++i) {
        order_node node =
            make_node("p" + std::to_string(i), static_cast<std::int32_t>(kCount - i));
        if (i + 1 < kCount) node.before.push_back("p" + std::to_string(i + 1));
        expected.push_back(node.plug_id);
        nodes.push_back(std::move(node));
    }
    expect_plan("long-chain", nodes, {}, abi::v3::ok, expected);
}

std::jmp_buf* g_jump_target = nullptr;
const char* g_reported_expr = nullptr;

TEST_CASE(check_is_not_disabled_by_ndebug)
{
    // NDEBUG (defined at the top of this file) removes assert(); the static_assert above
    // already pins that CHECK is not assert(). Here the failure path must be reachable.
#if defined(NDEBUG)
    assert(1 == 2);
#endif

    std::jmp_buf jump{};
    g_reported_expr = nullptr;
    g_jump_target = &jump;
    g_check_hook = [](const char* expr, const char* file, int line) {
        (void)file;
        (void)line;
        g_reported_expr = expr;
        ::longjmp(*g_jump_target, 1);
    };
    if (::setjmp(jump) == 0) {
        CHECK(2 + 2 == 5); // must be reported even though NDEBUG is defined
    }
    g_check_hook = nullptr;
    g_jump_target = nullptr;
    CHECK(g_reported_expr != nullptr);
    CHECK(std::string(g_reported_expr) == "2 + 2 == 5");
}

} // namespace

int main()
{
    for (const test_case& test : tests()) {
        g_current_test = test.name;
        test.run();
    }
    std::printf("%zu test cases passed\n", tests().size());
    return 0;
}
