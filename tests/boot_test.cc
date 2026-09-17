/**
 * @file boot_test.cc
 * @brief Allocation-failure and rollback regression tests for host::boot().
 *
 * The normal build of this file is a standalone test executable with a complete global
 * new/delete failure injector. The same source is also compiled into six small real plugin
 * libraries by defining exactly one of the following macros:
 *
 * - U42_BOOT_PROVIDER_FIXTURE
 * - U42_BOOT_CREATED_A_FIXTURE
 * - U42_BOOT_CREATED_B_FIXTURE
 * - U42_BOOT_BATCH_GOOD_FIXTURE
 * - U42_BOOT_BATCH_FAIL_FIXTURE
 * - U42_BOOT_BATCH_TAIL_FIXTURE
 *
 * Executable usage:
 *
 * @code
 * boot_test <provider_library> <created_a_library> <created_b_library> <batch_directory>
 * @endcode
 *
 * The batch directory must contain the good, failing and tail fixture libraries and no other
 * plugin candidates. The provider and two created fixture paths are staged independently so the
 * test can distinguish pre-existing dynamic records from a newly scanned boot batch.
 *
 * @note The provider fixture shares no interface type with the test binary. It publishes a plain
 *       data contract and exposes only the host-private iinvoke through iplug::query, so the
 *       consumer acquires a lease, binds the method and reaches the provider exclusively through
 *       icalls - the only business path ABI v2 allows.
 * @note This test replaces the process-wide allocation functions and must remain an independent
 *       binary, like withdraw_test. It deliberately includes the private engine contract to
 *       construct the otherwise-unobservable pre-existing Created dynamic-record state.
 */

#include <42u/abi.hpp>

#include <cstdint>

/**
 * @brief Data-only contract shared by the provider fixture and the test binary.
 *
 * @note Both sides see exactly these constants because the same source file is compiled once per
 *       fixture macro and once as the test executable, so no header, interface type or virtual
 *       class crosses the DSO boundary.
 */
namespace boot_contract {

namespace abi = u42::abi::v2;

/** @brief Test-only protocol family announced by the provider fixture. */
inline constexpr abi::iid provider_iid{0x424f4f545f50524fULL, 1};
/** @brief Protocol the provider announces and the consumer requires: major 1, minor 0. */
inline constexpr abi::contract provider_contract{provider_iid, 1, 0};
/** @brief Same family and major with a higher minimum minor, which the provider must refuse. */
inline constexpr abi::contract provider_contract_minor2{provider_iid, 1, 1};
/** @brief Numeric identifier of the fixture's single marker method. */
inline constexpr abi::method_id marker_method_id = 1;
/** @brief Published name of the fixture's marker method. */
inline constexpr char marker_method_name[] = "marker";
/** @brief Human-readable summary of the fixture's marker method. */
inline constexpr char marker_method_description[] = "Return the fixed marker JSON object.";
/** @brief Input schema of the fixture's marker method. */
inline constexpr char marker_input_schema[] = "{\"type\":\"object\"}";
/** @brief Output schema of the fixture's marker method. */
inline constexpr char marker_output_schema[] = "{\"type\":\"object\"}";
/** @brief Exact JSON object a successful invoke() must deliver. */
inline constexpr char marker_json[] = "{\"marker\":\"42b007c0ffee1234\"}";

} // namespace boot_contract

#if defined(U42_BOOT_PROVIDER_FIXTURE) || defined(U42_BOOT_CREATED_A_FIXTURE) ||                 \
    defined(U42_BOOT_CREATED_B_FIXTURE) || defined(U42_BOOT_BATCH_GOOD_FIXTURE) ||              \
    defined(U42_BOOT_BATCH_FAIL_FIXTURE) || defined(U42_BOOT_BATCH_TAIL_FIXTURE)

#include <new>

namespace {

namespace abi = u42::abi::v2;

#if defined(U42_BOOT_PROVIDER_FIXTURE)
constexpr char fixture_id[] = "com.example.boot.provider";
constexpr bool fixture_provides_marker = true;
constexpr abi::status fixture_start_status = abi::ok;
#elif defined(U42_BOOT_CREATED_A_FIXTURE)
constexpr char fixture_id[] = "com.example.boot.created.a";
constexpr bool fixture_provides_marker = false;
constexpr abi::status fixture_start_status = abi::ok;
#elif defined(U42_BOOT_CREATED_B_FIXTURE)
constexpr char fixture_id[] = "com.example.boot.created.b";
constexpr bool fixture_provides_marker = false;
constexpr abi::status fixture_start_status = abi::ok;
#elif defined(U42_BOOT_BATCH_GOOD_FIXTURE)
constexpr char fixture_id[] = "com.example.boot.batch.a-good";
constexpr bool fixture_provides_marker = false;
constexpr abi::status fixture_start_status = abi::ok;
#elif defined(U42_BOOT_BATCH_FAIL_FIXTURE)
constexpr char fixture_id[] = "com.example.boot.batch.m-fail";
constexpr bool fixture_provides_marker = false;
constexpr abi::status fixture_start_status = abi::failed;
#else
constexpr char fixture_id[] = "com.example.boot.batch.z-tail";
constexpr bool fixture_provides_marker = false;
constexpr abi::status fixture_start_status = abi::ok;
#endif

constexpr char fixture_version[] = "boot-test-2";
const abi::plug_desc fixture_description{sizeof(abi::plug_desc), 0, fixture_id, fixture_version,
                                         0, 0, nullptr, 0, nullptr};

/**
 * @brief One fixture instance.
 *
 * @note Only the provider variant publishes a business contract and implements iinvoke. The
 *       other variants disclose the legal empty capability set (no contract, no methods), so
 *       they publish no business authority at all and are never acquirable.
 */
class fixture_plugin final : public abi::iplug, public abi::iinvoke {
public:
    abi::status U42_CALL init(abi::ictx* ctx) noexcept override
    {
        if (ctx == nullptr) return abi::invalid_argument;
        if (caps_ != nullptr) return abi::invalid_state;
        void* raw = nullptr;
        const abi::status outcome = ctx->query(&abi::caps_iid, &raw);
        if (outcome != abi::ok) return outcome;
        caps_ = static_cast<abi::icaps*>(raw);
        return caps_ != nullptr ? abi::ok : abi::failed;
    }

    abi::status U42_CALL start() noexcept override
    {
        if (fixture_start_status != abi::ok) return fixture_start_status;
        if (caps_ == nullptr || started_) return abi::invalid_state;
        try {
            // The local array only has to survive this call: announce() copies every method it
            // accepts, so no fixture-side storage outlives start().
            const abi::method_desc methods[]{
                {boot_contract::marker_method_id, boot_contract::marker_method_name,
                 boot_contract::marker_method_description, boot_contract::marker_input_schema,
                 boot_contract::marker_output_schema},
            };
            abi::caps_desc capabilities{};
            if (fixture_provides_marker) {
                capabilities.method_count = 1;
                capabilities.methods = methods;
                capabilities.protocol = boot_contract::provider_contract;
            }
            const abi::status outcome = caps_->announce(&capabilities);
            if (outcome == abi::ok) started_ = true;
            return outcome;
        } catch (...) {
            return abi::failed;
        }
    }

    abi::status U42_CALL stop() noexcept override
    {
        started_ = false;
        return abi::ok;
    }

    void U42_CALL destroy() noexcept override { delete this; }

    /**
     * @brief Return the host-private iinvoke interface, never a cross-plugin business query.
     *
     * @param type Requested interface identifier.
     * @param out Cleared first; receives the invoker for a provider fixture.
     * @return ok when invoke_iid was requested by a provider, otherwise unsupported.
     */
    abi::status U42_CALL query(const abi::iid* type, void** out) noexcept override
    {
        if (out == nullptr) return abi::invalid_argument;
        *out = nullptr;
        if (type == nullptr) return abi::invalid_argument;
        if (fixture_provides_marker && *type == abi::invoke_iid) {
            *out = static_cast<abi::iinvoke*>(this);
            return abi::ok;
        }
        return abi::unsupported;
    }

    /**
     * @brief Deliver the frozen marker JSON for the single published method.
     *
     * @param method Requested provider-local method identifier.
     * @param args Borrowed input; the fixture accepts and ignores any payload.
     * @param result Required caller-owned writer.
     * @return ok after the marker was written, invalid_argument for a null writer,
     *         unsupported for a non-provider variant, not_found for an unannounced method, or
     *         invalid_state before start().
     */
    abi::status U42_CALL invoke(abi::method_id method, abi::bytes args,
                                abi::iwriter* result) noexcept override
    {
        (void)args;
        if (result == nullptr) return abi::invalid_argument;
        if (!fixture_provides_marker) return abi::unsupported;
        if (method != boot_contract::marker_method_id) return abi::not_found;
        if (!started_) return abi::invalid_state;
        try {
            return result->write(abi::bytes{boot_contract::marker_json,
                                            static_cast<std::uint64_t>(
                                                sizeof(boot_contract::marker_json) - 1)});
        } catch (...) {
            return abi::failed;
        }
    }

private:
    abi::icaps* caps_ = nullptr;
    bool started_ = false;
};

/** @brief Library-owned factory for one compile-time-selected fixture identity. */
class fixture_factory final : public abi::iplug_fty {
public:
    abi::status U42_CALL describe(const abi::plug_desc** out) noexcept override
    {
        if (out == nullptr) return abi::invalid_argument;
        *out = &fixture_description;
        return abi::ok;
    }

    abi::status U42_CALL create(abi::iplug** out) noexcept override
    {
        if (out == nullptr) return abi::invalid_argument;
        *out = nullptr;
        try {
            *out = new fixture_plugin();
            return abi::ok;
        } catch (...) {
            return abi::failed;
        }
    }
};

fixture_factory fixture_factory_singleton;

} // namespace

extern "C" U42_EXPORT abi::status U42_CALL u42_get_factory(std::uint32_t major,
                                                            abi::iplug_fty** out) noexcept
{
    if (out == nullptr) return abi::invalid_argument;
    *out = nullptr;
    if (major != abi::abi_major) return abi::unsupported;
    *out = &fixture_factory_singleton;
    return abi::ok;
}

#else

#define NDEBUG 1

#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

// The test needs to stage real libraries without starting them. Pre-including every dependency
// confines this visibility change to u42::host itself; internal.hpp sees host.hpp's pragma-once.
#define private public
#include <42u/host.hpp>
#undef private

#include "../src/internal.hpp"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string_view>
#include <utility>

namespace {

/** @brief Total replacement-new calls observed since process start. */
std::atomic<long> g_new_calls{0};
/** @brief First absolute 0-based allocation index that throws, or -1 while unused. */
std::atomic<long> g_fail_at{-1};
/** @brief Optional second absolute allocation index, used to interrupt best-effort cleanup. */
std::atomic<long> g_fail_at_second{-1};
/** @brief Whether either selected allocation may still fail. */
std::atomic<bool> g_armed{false};
/** @brief Number of armed failures that have fired in the current injection. */
std::atomic<int> g_failure_hits{0};

/** @brief Count one allocation and throw when either armed absolute index is reached. */
void note_allocation()
{
    const long index = g_new_calls.fetch_add(1, std::memory_order_relaxed);
    if (!g_armed.load(std::memory_order_relaxed)) return;
    if (g_fail_at.load(std::memory_order_relaxed) == index) {
        g_fail_at.store(-1, std::memory_order_relaxed);
        if (g_fail_at_second.load(std::memory_order_relaxed) < 0) {
            g_armed.store(false, std::memory_order_relaxed);
        }
        g_failure_hits.fetch_add(1, std::memory_order_relaxed);
        throw std::bad_alloc();
    }
    if (g_fail_at_second.load(std::memory_order_relaxed) == index) {
        g_fail_at_second.store(-1, std::memory_order_relaxed);
        if (g_fail_at.load(std::memory_order_relaxed) < 0) {
            g_armed.store(false, std::memory_order_relaxed);
        }
        g_failure_hits.fetch_add(1, std::memory_order_relaxed);
        throw std::bad_alloc();
    }
}

/** @brief Arm a one-shot failure at an absolute allocation index. */
void arm_fail_at(long index) noexcept
{
    g_fail_at.store(index, std::memory_order_relaxed);
    g_fail_at_second.store(-1, std::memory_order_relaxed);
    g_failure_hits.store(0, std::memory_order_relaxed);
    g_armed.store(true, std::memory_order_relaxed);
}

/** @brief Arm two allocation failures; the second remains active after the first throws. */
void arm_fail_pair(long first, long second) noexcept
{
    g_fail_at.store(first, std::memory_order_relaxed);
    g_fail_at_second.store(second, std::memory_order_relaxed);
    g_failure_hits.store(0, std::memory_order_relaxed);
    g_armed.store(true, std::memory_order_relaxed);
}

/** @brief Disable allocation failure before assertions and teardown. */
void disarm_fail() noexcept
{
    g_armed.store(false, std::memory_order_relaxed);
    g_fail_at.store(-1, std::memory_order_relaxed);
    g_fail_at_second.store(-1, std::memory_order_relaxed);
}

} // namespace

// Complete matching global replacement set. The test is intentionally a separate executable.
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
                         size != 0 ? size : 1) != 0) {
        throw std::bad_alloc();
    }
    return memory;
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return ::operator new(size, alignment);
}

void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept
{
    try {
        return ::operator new(size, alignment);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept
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
namespace fs = std::filesystem;
using u42::detail::engine;
using u42::detail::phase;
using u42::detail::record;

constexpr const char* provider_id = "com.example.boot.provider";
constexpr const char* created_a_id = "com.example.boot.created.a";
constexpr const char* created_b_id = "com.example.boot.created.b";
constexpr const char* batch_good_id = "com.example.boot.batch.a-good";
constexpr const char* batch_fail_id = "com.example.boot.batch.m-fail";
constexpr const char* batch_tail_id = "com.example.boot.batch.z-tail";
constexpr const char* consumer_id = "com.example.boot.static-consumer";

const char* g_current_test = "startup";

/** @brief Terminate with a stable diagnostic that does not depend on assert(). */
[[noreturn]] void fail_check(const char* expression, const char* file, int line)
{
    disarm_fail();
    std::fprintf(stderr, "CHECK failed in %s: %s (%s:%d)\n", g_current_test, expression,
                 file, line);
    std::fflush(stderr);
    std::exit(EXIT_FAILURE);
}

/** @brief Assertion that remains enabled under NDEBUG. */
#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) fail_check(#condition, __FILE__, __LINE__);            \
    } while (false)

#define U42_STRINGIFY(...) #__VA_ARGS__
#define U42_EXPAND_STRING(...) U42_STRINGIFY(__VA_ARGS__)
inline constexpr std::string_view u42_check_expansion = U42_EXPAND_STRING(CHECK(0 == 1));
static_assert(u42_check_expansion.find("fail_check") != std::string_view::npos,
              "CHECK must not compile away under NDEBUG");

/** @brief Find one exact identity in a host listing. */
bool contains(const std::vector<std::string>& ids, const char* id)
{
    for (const std::string& value : ids) {
        if (value == id) return true;
    }
    return false;
}

/** @brief Static consumer retaining a real tracked borrow from the dynamic provider. */
class static_consumer final : public abi::iplug, public abi::irevoker {
public:
    explicit static_consumer(class static_consumer_factory& owner) noexcept : owner_(owner) {}

    abi::status U42_CALL init(abi::ictx* ctx) noexcept override
    {
        if (ctx == nullptr) return abi::invalid_argument;
        if (caps_ != nullptr || calls_ != nullptr) return abi::invalid_state;
        void* raw = nullptr;
        const abi::status caps_status = ctx->query(&abi::caps_iid, &raw);
        if (caps_status != abi::ok) return caps_status;
        caps_ = static_cast<abi::icaps*>(raw);
        raw = nullptr;
        const abi::status calls_status = ctx->query(&abi::calls_iid, &raw);
        if (calls_status != abi::ok) return calls_status;
        calls_ = static_cast<abi::icalls*>(raw);
        return caps_ != nullptr && calls_ != nullptr ? abi::ok : abi::failed;
    }

    abi::status U42_CALL start() noexcept override
    {
        active_ = true;
        return abi::ok;
    }

    abi::status U42_CALL stop() noexcept override
    {
        active_ = false;
        if (lease_.credential.value != 0 && caps_ != nullptr) {
            const abi::status released = caps_->release(lease_.credential);
            if (released == abi::ok || released == abi::stale) lease_ = abi::borrow{};
        }
        return abi::ok;
    }

    void U42_CALL destroy() noexcept override;

    abi::status U42_CALL query(const abi::iid* type, void** out) noexcept override
    {
        if (out == nullptr) return abi::invalid_argument;
        *out = nullptr;
        return type == nullptr ? abi::invalid_argument : abi::unsupported;
    }

    void U42_CALL on_revoke(abi::token credential) noexcept override
    {
        if (caps_ == nullptr) return;
        const abi::status released = caps_->release(credential);
        if ((released == abi::ok || released == abi::stale) &&
            credential.value == lease_.credential.value) {
            lease_ = abi::borrow{};
        }
    }

    /**
     * @brief Lease the dynamic provider under the explicitly accepted test protocol.
     *
     * @return ok with a stored credential, unsupported for an incompatible protocol, or the
     *         host status; a refused acquire never changes the held lease.
     */
    abi::status acquire_provider() noexcept
    {
        if (!active_ || caps_ == nullptr) return abi::invalid_state;
        if (lease_.credential.value != 0) return abi::invalid_state;
        abi::borrow next{};
        const abi::status outcome =
            caps_->acquire(provider_id, &boot_contract::provider_contract, this, &next);
        if (outcome == abi::ok) lease_ = next;
        return outcome;
    }

    /**
     * @brief Reach the provider through lease -> bind -> icalls::call and compare the marker.
     *
     * @param out Receives the delivered payload; cleared first.
     * @return ok only when the provider delivered the exact frozen marker JSON.
     * @note No provider pointer is ever stored: the lease credential names the instance and the
     *       binding only exists while that credential does.
     */
    abi::status invoke_provider(std::string& out) noexcept
    {
        out.clear();
        if (calls_ == nullptr || lease_.credential.value == 0) return abi::invalid_state;
        abi::binding target{};
        abi::status outcome =
            calls_->bind_name(lease_.credential, boot_contract::marker_method_name, &target);
        if (outcome != abi::ok) return outcome;
        u42::detail::string_writer writer(1024);
        outcome = calls_->call(target, abi::bytes{nullptr, 0}, &writer);
        const abi::status dropped = calls_->unbind(target);
        if (outcome != abi::ok) return outcome;
        if (dropped != abi::ok) return dropped;
        out = writer.value;
        return out == boot_contract::marker_json ? abi::ok : abi::failed;
    }

    abi::token credential() const noexcept { return lease_.credential; }

private:
    class static_consumer_factory& owner_;
    abi::icaps* caps_ = nullptr;
    abi::icalls* calls_ = nullptr;
    abi::borrow lease_{};
    bool active_ = false;
};

/** @brief Factory whose storage outlives its host and exposes the current test instance. */
class static_consumer_factory final : public abi::iplug_fty {
public:
    static_consumer_factory()
    {
        description_.struct_size = sizeof(description_);
        description_.plug_id = consumer_id;
        description_.version = "boot-test-1";
    }

    abi::status U42_CALL describe(const abi::plug_desc** out) noexcept override
    {
        if (out == nullptr) return abi::invalid_argument;
        *out = &description_;
        return abi::ok;
    }

    abi::status U42_CALL create(abi::iplug** out) noexcept override
    {
        if (out == nullptr) return abi::invalid_argument;
        *out = nullptr;
        try {
            instance_ = new static_consumer(*this);
            *out = instance_;
            return abi::ok;
        } catch (...) {
            instance_ = nullptr;
            return abi::failed;
        }
    }

    void destroyed(static_consumer* value) noexcept
    {
        if (instance_ == value) instance_ = nullptr;
    }

    static_consumer* instance() const noexcept { return instance_; }

private:
    abi::plug_desc description_{};
    static_consumer* instance_ = nullptr;
};

void static_consumer::destroy() noexcept
{
    owner_.destroyed(this);
    delete this;
}

/** @brief Load and stage a real library without starting the resulting record. */
void stage_created(engine& runtime, const fs::path& path)
{
    auto library = std::make_unique<u42::plug>();
    std::string diagnostic;
    CHECK(library->open(path, diagnostic) == abi::ok);
    abi::iplug_fty* factory = library->factory();
    CHECK(factory != nullptr);
    CHECK(runtime.stage(factory, std::move(library)) == abi::ok);
}

/** @brief Scan and stage every real plugin in a directory without calling start_pending(). */
void stage_directory(engine& runtime, const fs::path& directory)
{
    std::vector<fs::path> candidates;
    std::string diagnostic;
    CHECK(u42::scan_plugins(directory, candidates, diagnostic) == abi::ok);
    CHECK(!candidates.empty());
    for (const fs::path& candidate : candidates) stage_created(runtime, candidate);
}

/** @brief Existing active static/dynamic graph with one real outstanding borrow. */
class active_rack {
public:
    explicit active_rack(const fs::path& provider_library)
    {
        CHECK(rack.add(&consumer_factory) == abi::ok);
        CHECK(rack.start() == abi::ok);
        CHECK(rack.load(provider_library) == abi::ok);
        CHECK(consumer_factory.instance() != nullptr);
        CHECK(consumer_factory.instance()->acquire_provider() == abi::ok);
        std::string marker;
        CHECK(consumer_factory.instance()->invoke_provider(marker) == abi::ok);
        CHECK(marker == boot_contract::marker_json);
        CHECK(runtime().leases.size() == 1);
    }

    engine& runtime() { return *rack.engine_; }
    const engine& runtime() const { return *rack.engine_; }
    static_consumer& consumer() { return *consumer_factory.instance(); }

    static_consumer_factory consumer_factory;
    u42::host rack;
};

/** @brief Existing graph plus two dynamic records intentionally left in Created. */
class snapshot_rack final : public active_rack {
public:
    snapshot_rack(const fs::path& provider_library, const fs::path& created_a_library,
                  const fs::path& created_b_library)
        : active_rack(provider_library)
    {
        stage_created(runtime(), created_a_library);
        stage_created(runtime(), created_b_library);
        CHECK(runtime().records.size() == 4);
        CHECK(runtime().records.at(created_a_id)->state == phase::created);
        CHECK(runtime().records.at(created_b_id)->state == phase::created);
    }
};

/** @brief Pointer/generation snapshot proving no old object was replaced or erased. */
struct preserved_state {
    record* consumer = nullptr;
    record* provider = nullptr;
    record* created_a = nullptr;
    record* created_b = nullptr;
    std::uint64_t consumer_generation = 0;
    std::uint64_t provider_generation = 0;
    std::uint64_t created_a_generation = 0;
    std::uint64_t created_b_generation = 0;
    abi::token credential{};
};

preserved_state capture(snapshot_rack& fixture)
{
    engine& runtime = fixture.runtime();
    preserved_state state;
    state.consumer = runtime.records.at(consumer_id).get();
    state.provider = runtime.records.at(provider_id).get();
    state.created_a = runtime.records.at(created_a_id).get();
    state.created_b = runtime.records.at(created_b_id).get();
    state.consumer_generation = state.consumer->generation;
    state.provider_generation = state.provider->generation;
    state.created_a_generation = state.created_a->generation;
    state.created_b_generation = state.created_b->generation;
    state.credential = fixture.consumer().credential();
    return state;
}

/**
 * @brief Require the pre-existing graph, its lease and its real invocation to be untouched.
 *
 * @param fixture Rack that survived the injected allocation failure.
 * @param state Pointer, generation and credential snapshot taken before the failure.
 * @note The invocation travels through the retained credential (lease -> bind -> icalls::call),
 *       so a record that lost its lease, its binding path or its provider invoker fails here
 *       instead of only in a pointer comparison.
 */
void verify_preserved(snapshot_rack& fixture, const preserved_state& state)
{
    engine& runtime = fixture.runtime();
    CHECK(runtime.records.size() == 4);
    CHECK(runtime.records.at(consumer_id).get() == state.consumer);
    CHECK(runtime.records.at(provider_id).get() == state.provider);
    CHECK(runtime.records.at(created_a_id).get() == state.created_a);
    CHECK(runtime.records.at(created_b_id).get() == state.created_b);
    CHECK(state.consumer->generation == state.consumer_generation);
    CHECK(state.provider->generation == state.provider_generation);
    CHECK(state.created_a->generation == state.created_a_generation);
    CHECK(state.created_b->generation == state.created_b_generation);
    CHECK(state.consumer->state == phase::active);
    CHECK(state.provider->state == phase::active);
    CHECK(state.created_a->state == phase::created);
    CHECK(state.created_b->state == phase::created);
    CHECK(state.created_a->library != nullptr);
    CHECK(state.created_b->library != nullptr);
    CHECK(runtime.leases.size() == 1);
    CHECK(runtime.leases.count(state.credential.value) == 1);
    CHECK(runtime.leases.at(state.credential.value).consumer == state.consumer);
    CHECK(runtime.leases.at(state.credential.value).provider == state.provider);
    CHECK(runtime.leases.at(state.credential.value).protocol.id == boot_contract::provider_iid);
    CHECK(runtime.leases.at(state.credential.value).protocol.major ==
          boot_contract::provider_contract.major);
    CHECK(runtime.leases.at(state.credential.value).generation == state.provider_generation);
    CHECK(fixture.consumer().credential().value == state.credential.value);
    std::string marker;
    CHECK(fixture.consumer().invoke_provider(marker) == abi::ok);
    CHECK(marker == boot_contract::marker_json);
    // invoke_provider() unbinds what it bound, so the failure injection left no orphan binding.
    CHECK(runtime.bindings.empty());
}

/**
 * @brief Every injected failure during an otherwise-empty boot preserves the complete old graph.
 *
 * Fail-next necessarily hits construction of the known-ID set. Later offsets cover partial set
 * construction and the empty-directory scan. The test intentionally accepts either failed or ok
 * at offsets past the operation, but the pre-existing Created dynamic records must never vanish.
 */
void test_known_snapshot_oom(const fs::path& provider_library,
                             const fs::path& created_a_library,
                             const fs::path& created_b_library,
                             const fs::path& empty_directory)
{
    g_current_test = "known-snapshot-oom";
    long allocation_count = 0;
    {
        snapshot_rack measured(provider_library, created_a_library, created_b_library);
        const preserved_state before_state = capture(measured);
        const long before = g_new_calls.load(std::memory_order_relaxed);
        CHECK(measured.rack.boot(empty_directory) == abi::ok);
        allocation_count = g_new_calls.load(std::memory_order_relaxed) - before;
        verify_preserved(measured, before_state);
    }
    CHECK(allocation_count >= 4);

    long failed_offsets = 0;
    for (long offset = 0; offset <= allocation_count + 1; ++offset) {
        snapshot_rack fixture(provider_library, created_a_library, created_b_library);
        const preserved_state before_state = capture(fixture);
        arm_fail_at(g_new_calls.load(std::memory_order_relaxed) + offset);
        abi::status outcome = abi::ok;
        bool threw = false;
        try {
            outcome = fixture.rack.boot(empty_directory);
        } catch (...) {
            threw = true;
        }
        disarm_fail();
        CHECK(!threw);
        if (outcome == abi::failed) ++failed_offsets;
        else CHECK(outcome == abi::ok);
        if (offset == 0) CHECK(outcome == abi::failed);
        verify_preserved(fixture, before_state);
    }
    CHECK(failed_offsets >= 2);
    std::printf("boot_test: known snapshot used %ld allocations; %ld injected calls returned failed\n",
                allocation_count, failed_offsets);
}

/** @brief Two start_pending()-relative allocation offsets that defeat its best-effort cleanup. */
struct cleanup_failure_pair {
    long first = -1;
    long second = -1;
};

/**
 * @brief Locate a planner OOM followed by a rollback_created() OOM.
 *
 * The first failure must be caught by plan_order(), so start_pending() returns failed rather than
 * throwing. The second then interrupts rollback_created()'s temporary identity snapshot, leaving
 * at least one newly staged Created record. This is precisely the state the extra boot-level
 * rollback must remove.
 */
cleanup_failure_pair find_incomplete_start_cleanup(const fs::path& provider_library,
                                                   const fs::path& batch_directory)
{
    long allocation_count = 0;
    {
        active_rack measured(provider_library);
        stage_directory(measured.runtime(), batch_directory);
        const long before = g_new_calls.load(std::memory_order_relaxed);
        CHECK(measured.runtime().start_pending() == abi::failed);
        allocation_count = g_new_calls.load(std::memory_order_relaxed) - before;
    }
    CHECK(allocation_count > 0);

    for (long first = 0; first < allocation_count; ++first) {
        for (long gap = 1; gap <= 8; ++gap) {
            active_rack fixture(provider_library);
            stage_directory(fixture.runtime(), batch_directory);
            const long before = g_new_calls.load(std::memory_order_relaxed);
            arm_fail_pair(before + first, before + first + gap);
            abi::status outcome = abi::ok;
            bool threw = false;
            try {
                outcome = fixture.runtime().start_pending();
            } catch (...) {
                threw = true;
            }
            const int failure_hits = g_failure_hits.load(std::memory_order_relaxed);
            disarm_fail();
            if (threw || outcome != abi::failed || failure_hits != 2) continue;
            const engine& runtime = fixture.runtime();
            const bool retained = runtime.records.count(batch_good_id) != 0 ||
                                  runtime.records.count(batch_fail_id) != 0 ||
                                  runtime.records.count(batch_tail_id) != 0;
            if (retained) return cleanup_failure_pair{first, first + gap};
        }
    }
    return {};
}

/**
 * @brief Measure host::boot() allocations before its call to start_pending().
 *
 * This deliberately mirrors the wrapper's known-ID snapshot, scan, open and stage sequence. The
 * returned count translates start_pending()-relative fault offsets into boot()-relative offsets
 * without hard-coding libstdc++ allocation counts or build-directory path lengths.
 */
long measure_boot_prefix(const fs::path& provider_library, const fs::path& batch_directory)
{
    active_rack fixture(provider_library);
    engine& runtime = fixture.runtime();
    const long before = g_new_calls.load(std::memory_order_relaxed);

    std::set<std::string> known;
    for (const auto& entry : runtime.records) known.insert(entry.first);
    std::vector<fs::path> candidates;
    std::string diagnostic;
    CHECK(u42::scan_plugins(batch_directory, candidates, diagnostic) == abi::ok);
    CHECK(!candidates.empty());
    for (const fs::path& candidate : candidates) {
        auto library = std::make_unique<u42::plug>();
        CHECK(library->open(candidate, diagnostic) == abi::ok);
        abi::iplug_fty* factory = library->factory();
        CHECK(runtime.stage(factory, std::move(library)) == abi::ok);
    }
    CHECK(known.size() == 2);
    return g_new_calls.load(std::memory_order_relaxed) - before;
}

/** @brief A failing newly scanned start batch rolls back without touching old instances/leases. */
void test_new_batch_start_failure(const fs::path& provider_library,
                                  const fs::path& batch_directory)
{
    g_current_test = "new-batch-start-failure";
    active_rack fixture(provider_library);
    engine& runtime = fixture.runtime();
    record* const old_consumer = runtime.records.at(consumer_id).get();
    record* const old_provider = runtime.records.at(provider_id).get();
    const std::uint64_t consumer_generation = old_consumer->generation;
    const std::uint64_t provider_generation = old_provider->generation;
    const abi::token credential = fixture.consumer().credential();

    CHECK(fixture.rack.boot(batch_directory) == abi::failed);
    const std::vector<std::string> ids = fixture.rack.plugins();
    CHECK(ids.size() == 2);
    CHECK(contains(ids, consumer_id));
    CHECK(contains(ids, provider_id));
    CHECK(!contains(ids, batch_good_id));
    CHECK(!contains(ids, batch_fail_id));
    CHECK(!contains(ids, batch_tail_id));
    CHECK(runtime.records.at(consumer_id).get() == old_consumer);
    CHECK(runtime.records.at(provider_id).get() == old_provider);
    CHECK(old_consumer->generation == consumer_generation);
    CHECK(old_provider->generation == provider_generation);
    CHECK(old_consumer->state == phase::active);
    CHECK(old_provider->state == phase::active);
    CHECK(runtime.leases.size() == 1);
    CHECK(runtime.leases.count(credential.value) == 1);
    CHECK(runtime.leases.at(credential.value).consumer == old_consumer);
    CHECK(runtime.leases.at(credential.value).provider == old_provider);
    CHECK(fixture.consumer().credential().value == credential.value);
    std::string marker;
    CHECK(fixture.consumer().invoke_provider(marker) == abi::ok);
    CHECK(marker == boot_contract::marker_json);
}

/**
 * @brief The boot wrapper cleans Created records left by an OOM-interrupted planner rollback.
 *
 * Removing the explicit rollback_boot_batch() call after start_pending() makes this check retain
 * at least one batch identity. A normal plugin start failure is intentionally insufficient here,
 * because rollback_pending() already cleans that path without help from the boot wrapper.
 */
void test_boot_fallback_cleanup(const fs::path& provider_library,
                                const fs::path& batch_directory)
{
    g_current_test = "boot-fallback-cleanup";
    const cleanup_failure_pair relative =
        find_incomplete_start_cleanup(provider_library, batch_directory);
    CHECK(relative.first >= 0);
    CHECK(relative.second > relative.first);
    const long prefix = measure_boot_prefix(provider_library, batch_directory);
    CHECK(prefix > 0);

    active_rack fixture(provider_library);
    engine& runtime = fixture.runtime();
    record* const old_consumer = runtime.records.at(consumer_id).get();
    record* const old_provider = runtime.records.at(provider_id).get();
    const abi::token credential = fixture.consumer().credential();
    const long before = g_new_calls.load(std::memory_order_relaxed);
    arm_fail_pair(before + prefix + relative.first,
                  before + prefix + relative.second);
    abi::status outcome = abi::ok;
    bool threw = false;
    try {
        outcome = fixture.rack.boot(batch_directory);
    } catch (...) {
        threw = true;
    }
    const int failure_hits = g_failure_hits.load(std::memory_order_relaxed);
    disarm_fail();

    CHECK(!threw);
    CHECK(failure_hits == 2);
    CHECK(outcome == abi::failed);
    CHECK(runtime.records.size() == 2);
    CHECK(runtime.records.at(consumer_id).get() == old_consumer);
    CHECK(runtime.records.at(provider_id).get() == old_provider);
    CHECK(runtime.records.count(batch_good_id) == 0);
    CHECK(runtime.records.count(batch_fail_id) == 0);
    CHECK(runtime.records.count(batch_tail_id) == 0);
    CHECK(runtime.leases.size() == 1);
    CHECK(runtime.leases.count(credential.value) == 1);
    CHECK(runtime.leases.at(credential.value).consumer == old_consumer);
    CHECK(runtime.leases.at(credential.value).provider == old_provider);
    CHECK(fixture.consumer().credential().value == credential.value);
    std::string marker;
    CHECK(fixture.consumer().invoke_provider(marker) == abi::ok);
    CHECK(marker == boot_contract::marker_json);
    std::printf("boot_test: fallback cleanup used prefix %ld and start offsets %ld/%ld\n",
                prefix, relative.first, relative.second);
}

/** @brief Run one native invalid-argument rejection while fail-next is armed. */
template <typename Operation>
void expect_oom_safe_rejection(u42::host& rack, Operation operation)
{
    // Force the diagnostic string back to SSO capacity so the old engine::fail implementation
    // necessarily allocated for its long message and let bad_alloc escape before entering try.
    std::string{}.swap(rack.engine_->error);
    arm_fail_at(g_new_calls.load(std::memory_order_relaxed));
    abi::status outcome = abi::ok;
    bool threw = false;
    try {
        outcome = operation();
    } catch (...) {
        threw = true;
    }
    disarm_fail();
    CHECK(!threw);
    CHECK(outcome == abi::invalid_argument);
}

/** @brief Revocation target used only to supply a valid receiver to acquire() rejections. */
struct unused_revoker final : abi::irevoker {
    /** @brief Never expected to run: every acquire() below is rejected before a lease exists. */
    void U42_CALL on_revoke(abi::token) noexcept override {}
};

/** @brief Public native parameter checks must remain allocation-failure-safe and non-throwing. */
void test_native_parameter_rejections_do_not_throw()
{
    g_current_test = "native-parameter-rejection-oom";
    u42::host rack;
    std::string output;
    unused_revoker revoker;
    abi::borrow lease{};
    abi::binding binding{};
    const abi::bytes empty{nullptr, 0};
    const abi::bytes invalid{nullptr, 1};

    // ABI v2 native boundary: every entry point rejects a missing required pointer, a zero
    // credential or a zero binding before it consults the engine, so a rejection cannot allocate
    // a diagnostic. The harness swaps the diagnostic back to SSO capacity and arms a failure at
    // the very next allocation, so each lambda has to return invalid_argument without throwing.
    expect_oom_safe_rejection(rack, [&]() { return rack.protocol("missing", nullptr); });
    expect_oom_safe_rejection(
        rack,
        [&]() { return rack.acquire("missing", boot_contract::provider_contract, nullptr, &lease); });
    expect_oom_safe_rejection(
        rack,
        [&]() { return rack.acquire("missing", boot_contract::provider_contract, &revoker, nullptr); });
    expect_oom_safe_rejection(rack, [&]() { return rack.release(abi::token{}); });
    expect_oom_safe_rejection(rack, [&]() { return rack.bind(abi::token{}, "marker", nullptr); });
    expect_oom_safe_rejection(rack, [&]() { return rack.bind(abi::token{}, abi::method_id{1}, nullptr); });
    expect_oom_safe_rejection(rack, [&]() { return rack.bind(abi::token{}, "marker", &binding); });
    expect_oom_safe_rejection(rack, [&]() { return rack.unbind(abi::binding{}); });
    expect_oom_safe_rejection(rack, [&]() { return rack.call(abi::binding{}, empty, nullptr); });
    expect_oom_safe_rejection(rack, [&]() { return rack.call(abi::binding{}, invalid, &output); });
    expect_oom_safe_rejection(rack, [&]() {
        return rack.call("missing", boot_contract::provider_contract, "marker", empty, nullptr);
    });
    expect_oom_safe_rejection(rack, [&]() {
        return rack.call("missing", boot_contract::provider_contract, abi::method_id{1}, empty,
                         nullptr);
    });
}

/**
 * @brief Inject each one-shot call allocation while preserving an unrelated live dependency.
 *
 * This checks temporary receiver lifetime and cleanup in the durable suite rather than relying
 * only on an external audit probe. Arguments are prepared before arming: caller-side string
 * construction is not part of the native exception-to-status boundary.
 */
void test_one_shot_allocation_failures(const fs::path& provider_library)
{
    g_current_test = "one-shot-allocation-failures";
    const std::string id = provider_id;
    const std::string method = boot_contract::marker_method_name;
    const abi::bytes empty{nullptr, 0};
    long allocations = 0;
    {
        active_rack measured(provider_library);
        std::string output = "sentinel";
        const long before = g_new_calls.load(std::memory_order_relaxed);
        CHECK(measured.rack.call(id, boot_contract::provider_contract, method, empty, &output) == abi::ok);
        allocations = g_new_calls.load(std::memory_order_relaxed) - before;
        CHECK(output == boot_contract::marker_json);
    }
    CHECK(allocations > 0);
    long failures = 0;
    for (long offset = 0; offset <= allocations; ++offset) {
        active_rack fixture(provider_library);
        const abi::token existing = fixture.consumer().credential();
        std::string output = "sentinel";
        arm_fail_at(g_new_calls.load(std::memory_order_relaxed) + offset);
        abi::status outcome = abi::ok;
        bool threw = false;
        try {
            outcome = fixture.rack.call(id, boot_contract::provider_contract, method, empty, &output);
        } catch (...) {
            threw = true;
        }
        disarm_fail();
        CHECK(!threw);
        if (outcome == abi::failed) {
            ++failures;
            CHECK(output.empty());
        } else {
            CHECK(outcome == abi::ok);
            CHECK(output == boot_contract::marker_json);
        }
        CHECK(fixture.runtime().leases.size() == 1);
        CHECK(fixture.runtime().leases.count(existing.value) == 1);
        CHECK(fixture.runtime().bindings.empty());
        // Recovery and final revocation would expose a dangling temporary receiver under ASan.
        CHECK(fixture.rack.call(id, boot_contract::provider_contract, method, empty, &output) == abi::ok);
        CHECK(output == boot_contract::marker_json);
        CHECK(fixture.consumer().invoke_provider(output) == abi::ok);
        CHECK(fixture.rack.shutdown() == abi::ok);
        CHECK(fixture.runtime().leases.empty());
        CHECK(fixture.runtime().bindings.empty());
    }
    CHECK(failures == allocations);
    std::printf("boot_test: one-shot call used %ld allocations; %ld injected failures recovered\n",
                allocations, failures);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 5) {
        std::fprintf(stderr,
                     "usage: %s <provider_library> <created_a_library> <created_b_library> "
                     "<batch_directory>\n",
                     argc > 0 ? argv[0] : "boot_test");
        return EXIT_FAILURE;
    }

    const fs::path provider_library = fs::absolute(argv[1]);
    const fs::path created_a_library = fs::absolute(argv[2]);
    const fs::path created_b_library = fs::absolute(argv[3]);
    const fs::path batch_directory = fs::absolute(argv[4]);
    const fs::path empty_directory = batch_directory.parent_path() / "empty";
    std::error_code ec;
    fs::create_directories(empty_directory, ec);
    CHECK(!ec);
    CHECK(fs::is_empty(empty_directory, ec));
    CHECK(!ec);

    test_native_parameter_rejections_do_not_throw();
    test_one_shot_allocation_failures(provider_library);
    test_known_snapshot_oom(provider_library, created_a_library, created_b_library,
                            empty_directory);
    test_new_batch_start_failure(provider_library, batch_directory);
    test_boot_fallback_cleanup(provider_library, batch_directory);
    std::printf("boot_test: all checks passed\n");
    return EXIT_SUCCESS;
}

#endif
