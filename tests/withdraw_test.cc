/**
 * @file withdraw_test.cc
 * @brief Deterministic allocation-failure checks for u42::detail::engine::withdraw().
 *
 * The host treats a failed capability withdrawal as "the instance is still published": it must
 * not destroy or unmap an instance whose watchers were never told the capability went away.
 * That contract is only observable when the notice queue cannot grow, so this file replaces the
 * global allocation functions with a one-shot, index-addressable failure injector and drives
 * engine::withdraw() directly through ../src/internal.hpp.
 *
 * Build (links the whole library, like the other tests):
 *   g++ -std=c++17 -Wall -Wextra -Werror -Iinc -Isrc src/context.cc src/events.cc src/host.cc \
 *       src/order.cc src/plug.cc tests/withdraw_test.cc -o build/invoke-v2/events/withdraw_test \
 *       -ldl -pthread
 *
 * @note NDEBUG is defined deliberately so CHECK never collapses into assert().
 * @note The injector is disarmed before every CHECK, and replacement operator new/delete are a
 *       complete matching set, so no allocation can be routed to a mismatched deallocator.
 */
#define NDEBUG 1

#include "../src/internal.hpp"

#include <42u/host.hpp>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

/** @brief Total allocation calls observed since process start. */
std::atomic<long> g_new_calls{0};
/** @brief 0-based call index that must throw, or -1 when no index is armed. */
std::atomic<long> g_fail_at{-1};
/** @brief Whether the armed index may still fire. */
std::atomic<bool> g_armed{false};

/**
 * @brief Count one allocation and throw std::bad_alloc when the armed index is reached.
 *
 * @throws std::bad_alloc Exactly once per armed index, then the hook disarms itself.
 * @note Never allocates, so it is safe to call from the replacement allocation functions.
 */
void note_allocation()
{
    const long index = g_new_calls.fetch_add(1, std::memory_order_relaxed);
    if (!g_armed.load(std::memory_order_relaxed)) return;
    if (g_fail_at.load(std::memory_order_relaxed) != index) return;
    g_armed.store(false, std::memory_order_relaxed); // one-shot: the retry must succeed
    throw std::bad_alloc();
}

/**
 * @brief Arm the injector so that the allocation of the given 0-based call index throws.
 *
 * @param fail_index Index measured from g_new_calls; the next call has the current value.
 * @note Called immediately before the operation under test, and only after every fixture has
 *       been built, so the injected index is deterministic.
 */
void arm_fail_at(long fail_index) noexcept
{
    g_fail_at.store(fail_index, std::memory_order_relaxed);
    g_armed.store(true, std::memory_order_relaxed);
}

/** @brief Disarm the injector so diagnostics and teardown allocate normally. */
void disarm_fail() noexcept { g_armed.store(false, std::memory_order_relaxed); }

} // namespace

// --- replacement global allocation functions -----------------------------------------------
// The complete set is replaced so every allocation performed through operator new is paired
// with a matching operator delete; allocations are taken from malloc and released with free.
void* operator new(std::size_t size)
{
    note_allocation();
    void* memory = std::malloc(size != 0 ? size : 1);
    if (memory == nullptr) throw std::bad_alloc();
    return memory;
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
    note_allocation();
    const std::size_t align = static_cast<std::size_t>(alignment);
    void* memory = nullptr;
    if (::posix_memalign(&memory, align < sizeof(void*) ? sizeof(void*) : align,
                         size != 0 ? size : 1) != 0)
        throw std::bad_alloc();
    return memory;
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return ::operator new(size, alignment);
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    try {
        return ::operator new(size, alignment);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    try {
        return ::operator new(size, alignment);
    } catch (...) {
        return nullptr;
    }
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
void operator delete[](void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
void operator delete(void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }

namespace {

namespace abi = u42::abi::v2;

/** @brief Protocol the provider fixture announces: a valid family carried by every notice. */
constexpr abi::iid provider_protocol_id{0x1234, 0x5678};
constexpr abi::contract provider_protocol{provider_protocol_id, 1u, 0u};
using u42::host_options;
using u42::detail::cap_notice;
using u42::detail::cap_subscription;
using u42::detail::engine;
using u42::detail::phase;
using u42::detail::record;

/** @brief Counts capability events; withdraw must never call it from the calling stack. */
struct sink_probe final : abi::icap_sink {
    int calls = 0;
    void U42_CALL on_capability(const abi::cap_event*) noexcept override { ++calls; }
};

const char* g_current_test = nullptr;

/**
 * @brief Report one failed CHECK and stop the process with a non-zero status.
 *
 * @param expr Failed expression text.
 * @param file Source file of the failed CHECK.
 * @param line Source line of the failed CHECK.
 */
[[noreturn]] void fail_check(const char* expr, const char* file, int line)
{
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

/** @brief Compile-time guard: CHECK must expand to fail_check, never to assert(). */
inline constexpr std::string_view u42_check_expansion = U42_EXPAND_STRING(CHECK(0 == 1));
static_assert(u42_check_expansion.find("fail_check") != std::string_view::npos,
              "CHECK must call fail_check directly; defining it as assert() disables it "
              "under NDEBUG");

/** @brief Provider fixture with one watch per sink, fully built before any fault is armed. */
struct fixture {
    engine runtime;
    record* provider = nullptr;
    std::vector<std::unique_ptr<sink_probe>> sinks;

    /**
     * @brief Build a published (or not yet published) provider plus its capability watchers.
     *
     * @param watch_count Number of registered watches that must each receive a notice.
     * @param published Whether the provider currently offers capabilities.
     * @note Every string is longer than the small-string buffer and the capability set holds one
     *       protocol and one method, so each queued notice performs several real allocations
     *       and the injector has many addressable failure points inside withdraw().
     */
    explicit fixture(std::size_t watch_count, bool published = true) : runtime(host_options{})
    {
        auto item = std::make_unique<record>();
        const std::string id = "com.example.provider.withdrawal";
        item->order.plug_id = id;
        item->generation = 7;
        item->state = phase::active;
        item->published = published;
        u42::detail::owned_method method;
        method.id = 11;
        method.name = "describe.interface.contract";
        method.description = "deterministic allocation probe for withdraw";
        method.input_schema = "{\"type\":\"object\",\"title\":\"probe-input\"}";
        method.output_schema = "{\"type\":\"object\",\"title\":\"probe-output\"}";
        item->capabilities.protocol = provider_protocol;
        item->capabilities.methods.push_back(std::move(method));
        provider = item.get();
        runtime.records.emplace(id, std::move(item));
        for (std::size_t index = 0; index < watch_count; ++index) {
            sinks.push_back(std::make_unique<sink_probe>());
            runtime.watches.emplace(100 + index, cap_subscription{provider, sinks.back().get()});
        }
    }

    std::size_t notice_count() const { return runtime.notices.size(); }
};

const std::string sentinel = "untouched-diagnostic";

/**
 * @brief A wrong-thread withdrawal reports wrong_thread and leaves every piece of state alone.
 */
void test_wrong_thread()
{
    g_current_test = "wrong-thread";
    fixture probe(2);
    probe.runtime.error = sentinel;
    const std::size_t queued = probe.notice_count();

    abi::status outcome = abi::failed;
    std::thread worker([&]() { outcome = probe.runtime.withdraw(*probe.provider); });
    worker.join();

    CHECK(outcome == abi::wrong_thread);
    CHECK(probe.provider->published);
    CHECK(probe.notice_count() == queued);
    CHECK(probe.runtime.error == sentinel); // no diagnostic was written on the failure path

    // Unpublished state does not turn a wrong-thread call into success.
    fixture idle(1, false);
    abi::status idle_outcome = abi::failed;
    std::thread idle_worker([&]() { idle_outcome = idle.runtime.withdraw(*idle.provider); });
    idle_worker.join();
    CHECK(idle_outcome == abi::wrong_thread);
    CHECK(idle.notice_count() == 0);
}

/** @brief Withdrawing an instance that never published is a no-op. */
void test_already_withdrawn()
{
    g_current_test = "already-withdrawn";
    fixture probe(3, false);
    probe.runtime.error = sentinel;
    CHECK(probe.runtime.withdraw(*probe.provider) == abi::ok);
    CHECK(!probe.provider->published);
    CHECK(probe.notice_count() == 0);
    CHECK(probe.runtime.error == sentinel);
}

/**
 * @brief Success withdraws once, keeps pre-existing notices, and repeats idempotently.
 */
void test_success_and_idempotence()
{
    g_current_test = "success-and-idempotence";
    fixture probe(2);
    cap_notice existing{};
    existing.watch = 900;
    existing.provider = "com.example.earlier.notice";
    existing.generation = 3;
    existing.available = true;
    probe.runtime.notices.push_back(std::move(existing));
    const std::size_t queued = probe.notice_count();

    CHECK(probe.runtime.withdraw(*probe.provider) == abi::ok);
    CHECK(!probe.provider->published);
    CHECK(probe.notice_count() == queued + 2);
    CHECK(probe.sinks[0]->calls == 0 && probe.sinks[1]->calls == 0); // queued, never inline
    CHECK(probe.runtime.notices.front().watch == 900);
    CHECK(probe.runtime.notices.front().provider == "com.example.earlier.notice");
    for (std::size_t index = 1; index < probe.notice_count(); ++index) {
        const cap_notice& note = probe.runtime.notices[index];
        CHECK(note.provider == probe.provider->order.plug_id);
        CHECK(note.generation == probe.provider->generation);
        CHECK(!note.available);
        CHECK(note.capabilities.protocol.id == provider_protocol_id);
        CHECK(note.capabilities.protocol.major == provider_protocol.major);
        CHECK(abi::valid_contract(note.capabilities.protocol));
        CHECK(note.capabilities.methods.size() == 1);
    }
    const abi::status repeated = probe.runtime.withdraw(*probe.provider);
    CHECK(repeated == abi::ok);
    CHECK(probe.notice_count() == queued + 2); // no duplicate withdrawal notice
}

/**
 * @brief Every allocation point inside withdraw fails transactionally, then the retry succeeds.
 */
void test_allocation_failure_matrix()
{
    g_current_test = "allocation-failure-matrix";
    long allocation_count = 0;
    {
        fixture measured(2);
        const long before = g_new_calls.load(std::memory_order_relaxed);
        const abi::status outcome = measured.runtime.withdraw(*measured.provider);
        allocation_count = g_new_calls.load(std::memory_order_relaxed) - before;
        CHECK(outcome == abi::ok);
        CHECK(!measured.provider->published);
        CHECK(measured.notice_count() == 2);
    }
    CHECK(allocation_count >= 3); // otherwise the matrix below would prove too little

    long failed_offsets = 0;
    for (long offset = 0; offset <= allocation_count + 1; ++offset) {
        fixture probe(2);
        const std::size_t queued = probe.notice_count();
        probe.runtime.error = sentinel;
        arm_fail_at(g_new_calls.load(std::memory_order_relaxed) + offset);
        const abi::status outcome = probe.runtime.withdraw(*probe.provider);
        disarm_fail(); // never let an armed hook affect the CHECK diagnostics below

        if (outcome == abi::failed) {
            ++failed_offsets;
            CHECK(probe.provider->published);       // still published: the host must not destroy it
            CHECK(probe.notice_count() == queued);  // the queue gained nothing
            CHECK(probe.runtime.error == sentinel); // no allocation happened while reporting
            CHECK(probe.runtime.withdraw(*probe.provider) == abi::ok);
        } else {
            CHECK(outcome == abi::ok);
        }
        if (offset == 0) CHECK(outcome == abi::failed); // the documented fail-next case
        CHECK(!probe.provider->published);
        CHECK(probe.notice_count() == queued + 2);
        CHECK(probe.runtime.withdraw(*probe.provider) == abi::ok);
        CHECK(probe.notice_count() == queued + 2);
        CHECK(probe.provider->order.plug_id == "com.example.provider.withdrawal");
    }
    // Every allocation index used by a successful withdrawal was reachable and transactional;
    // only the offsets past the last allocation leave the call untouched.
    CHECK(failed_offsets == allocation_count);
    std::printf("withdraw_test: %ld allocations per withdrawal, %ld injected failures across %ld offsets\n",
                allocation_count, failed_offsets, allocation_count + 2);
}

} // namespace

int main()
{
    test_wrong_thread();
    test_already_withdrawn();
    test_success_and_idempotence();
    test_allocation_failure_matrix();
    std::printf("withdraw_test: all checks passed\n");
    return EXIT_SUCCESS;
}
