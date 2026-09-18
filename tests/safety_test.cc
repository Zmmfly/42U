/**
 * @file safety_test.cc
 * @brief Unload-refusal, version-lease and direct-call isolation checks for the 42U host.
 *
 * Self-contained: owns its main(), includes only the public <42u/host.hpp> (which pulls
 * <42u/abi.hpp>), inspects no host internals and never probes an expired bare ABI value or a
 * destroyed context "to see whether it still works".
 *
 * Migrated to ABI v3 (typed plugin_version lease + direct calls): the contract/binding layer is
 * gone. A provider publishes a numeric plugin_version and a method set, and a consumer holds only
 * a credential-bearing lease, then reaches the provider exclusively through the host's direct call
 * path (icalls::call_name / icalls::call_id) with that credential. No provider pointer crosses the
 * plugin boundary, and one lease may call any published method by name or by numeric id.
 *
 * Covered contracts:
 *  - an unreturned consumer lease pins the provider: unload() fails with busy, stop()/destroy()
 *    stay at zero, both identities stay registered, and a zero or foreign credential is refused
 *    without clearing the correct lease; an active return then unloads with stop()/destroy() == 1
 *    and both the named and the numeric call made with the returned credential report stale;
 *  - a valid published capability set that announces no methods can lease only the provider's
 *    lifetime: acquire() succeeds, a named direct call reports not_found, and the lifetime lock
 *    still blocks unload;
 *  - an inclusive version range that does not contain the published version is refused with
 *    unsupported and leaves no lease behind, a reversed range is rejected with invalid_argument,
 *    and a range crossing a major boundary is honoured only when it really contains the instance;
 *  - 0.0.0 is a legal published version: it can be discovered, leased with an exact range, called
 *    and returned like any other triple, and success is always recognised by the credential;
 *  - the native administration path takes an explicit version range: version() discovers the
 *    current published triple without leasing it, the one-shot call() acquires/direct-calls/
 *    returns synchronously, a zero credential is refused, and a returned credential leaves both
 *    call forms stale;
 *  - a provider whose stop() returns failed is quarantined: unload never destroys it, a second
 *    unload never retries stop(), and ~host() leaves the pinned graph alive. That graph is
 *    retained on purpose, so the case runs in a forked child leaving through std::_Exit while the
 *    parent checks waitpid strictly; without fork the case prints an explicit skip;
 *  - a consumer whose stop() returns failed keeps its outgoing lease, so its provider is never
 *    destroyed either (also checked in a forked child that leaves through std::_Exit);
 *  - a lease of an older generation never matches the reloaded instance - even when the reloaded
 *    provider publishes exactly the same version - and never disturbs the new, live lease;
 *  - the local CHECK stays fatal although this file defines NDEBUG, proven by a forked child
 *    whose expected exit status is a failure.
 *
 * @note NDEBUG is defined deliberately, mirroring tests/runtime_test.cc.
 */
#define NDEBUG 1

#include <42u/host.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#  define U42_SAFETY_FORK 1
#  include <sys/wait.h>
#  include <unistd.h>
#else
#  define U42_SAFETY_FORK 0
#endif

namespace {

namespace abi = u42::abi::v3;

/* ------------------------------------------------------------------ *
 * Harness: NDEBUG-proof CHECK and a self-contained main.
 * ------------------------------------------------------------------ */

std::size_t g_failures = 0;
const char* g_case = "<none>";
/** @brief True in the forked child, where a failure must not run exit-time leak reporting. */
[[maybe_unused]] bool g_in_fork_child = false;

/** @brief Terminate a failing case with a non-zero status. */
[[noreturn]] void terminate_failure()
{
#if U42_SAFETY_FORK
    if (g_in_fork_child) std::_Exit(EXIT_FAILURE); // Bypass atexit: the pinned graph is intentional.
#endif
    std::exit(EXIT_FAILURE);
}

[[noreturn]] void fail_check(const char* expression, const char* file, int line)
{
    ++g_failures;
    std::fprintf(stderr, "CHECK failure in %s: %s (%s:%d)\n", g_case, expression, file, line);
    std::fflush(stderr);
    terminate_failure();
}

[[noreturn]] void fail_status(const char* call, abi::status actual, abi::status expected,
                              const char* file, int line)
{
    ++g_failures;
    std::fprintf(stderr, "CHECK failure in %s: %s returned %u, expected %u (%s:%d)\n", g_case, call,
                 static_cast<unsigned>(actual), static_cast<unsigned>(expected), file, line);
    std::fflush(stderr);
    terminate_failure();
}

/** @brief Fatal check that never compiles away, unlike assert(). */
#define CHECK(condition)                                                         \
    do {                                                                         \
        if (!(condition)) fail_check(#condition, __FILE__, __LINE__);             \
    } while (false)

/** @brief Fatal check for an exact ABI status; reports the observed value on mismatch. */
#define CHECK_STATUS(call, expected)                                             \
    do {                                                                         \
        const abi::status u42_actual_ = (call);                                  \
        if (u42_actual_ != (expected)) {                                         \
            fail_status(#call, u42_actual_, (expected), __FILE__, __LINE__);      \
        }                                                                        \
    } while (false)

/** @brief Fatal check that a call failed; every non-ok status is accepted. */
#define CHECK_FAILS(call)                                                        \
    do {                                                                         \
        const abi::status u42_actual_ = (call);                                  \
        if (u42_actual_ == abi::ok) {                                            \
            fail_status(#call, u42_actual_, abi::busy, __FILE__, __LINE__);       \
        }                                                                        \
    } while (false)

#define U42_STRINGIFY_(...) #__VA_ARGS__
#define U42_STRINGIFY(...) U42_STRINGIFY_(__VA_ARGS__)

/** @brief Compile-time guard: CHECK calls fail_check, so NDEBUG cannot disable it. */
inline constexpr std::string_view u42_check_expansion = U42_STRINGIFY(CHECK(0 == 1));
static_assert(u42_check_expansion.find("fail_check") != std::string_view::npos,
              "CHECK must call fail_check directly; assert() would be removed by NDEBUG");

struct test_case {
    const char* name;
    void (*run)();
};

std::vector<test_case>& registry()
{
    static std::vector<test_case> all;
    return all;
}

struct registrar {
    registrar(const char* name, void (*run)()) { registry().push_back({name, run}); }
};

/** @brief Register one case without a test framework. */
#define TEST_CASE(name)                                                          \
    void name();                                                                 \
    [[maybe_unused]] const registrar reg_##name(#name, &name);                   \
    void name()

/* ------------------------------------------------------------------ *
 * Version and method constants used by every fake provider.
 * ------------------------------------------------------------------ */

/** @brief Version the fakes publish unless a case asks for another triple. */
inline constexpr abi::plugin_version k_provider_version{1, 4, 0};
/** @brief The all-zero triple; legal as a published version and never a failure marker. */
inline constexpr abi::plugin_version k_zero_version{0, 0, 0};
/** @brief Exact requirement matching @ref k_provider_version. */
inline constexpr abi::version_range k_provider_exact = abi::exact_version(k_provider_version);
/** @brief Numeric ids of the two methods every callable fake provider discloses. */
inline constexpr abi::method_id k_ping_method = 1;
inline constexpr abi::method_id k_echo_method = 2;

/**
 * @brief Minimal host-owned output sink required by icalls::call_name()/call_id().
 *
 * @note The ABI never hands a std::string across the boundary: the plugin writes into this
 *       borrowed writer and the string stays on the control side. A failed write is sticky so a
 *       partial result is never mistaken for a complete one.
 */
struct sink_writer final : abi::iwriter {
    std::string value;
    abi::status U42_CALL write(abi::bytes data) noexcept override
    {
        if (result_ != abi::ok) return result_;
        if (data.size != 0 && data.data == nullptr) return result_ = abi::invalid_argument;
        if (data.size == 0) return abi::ok;
        try {
            value.append(static_cast<const char*>(data.data), static_cast<std::size_t>(data.size));
        } catch (...) {
            return result_ = abi::failed;
        }
        return abi::ok;
    }

private:
    abi::status result_ = abi::ok;
};

/**
 * @brief Stable administration-side revocation receiver for the native host path.
 *
 * @note The registered address must stay valid until the lease is returned, so it is a named
 *       local in the case and never a temporary. It returns the credential it recognizes so a
 *       provider unload can complete.
 */
struct host_revoker final : abi::irevoker {
    u42::host* host = nullptr;
    [[maybe_unused]] std::uint64_t calls = 0;
    explicit host_revoker(u42::host* value) noexcept : host(value) {}
    void U42_CALL on_revoke(abi::token credential) noexcept override
    {
        ++calls;
        if (host == nullptr) return;
        try {
            (void)host->release(credential);
        } catch (...) {
        }
    }
};

/* ------------------------------------------------------------------ *
 * In-process fakes; the factories always outlive the host under test.
 * ------------------------------------------------------------------ */

/** @brief Live switches read by a fake instance; owned by its factory. */
struct fake_flags {
    /** Status the next stop() returns; abi::failed makes the instance quarantinable. */
    abi::status stop_status = abi::ok;
    /** When true, on_revoke deliberately keeps the credential instead of returning it. */
    bool swallow_revocation = false;
};

class fake_plug;

/** @brief Revocation receiver owned by the fake consumer for its whole instance lifetime. */
struct fake_revoker final : abi::irevoker {
    fake_plug* owner = nullptr;
    std::uint64_t calls = 0;
    abi::token last{};

    explicit fake_revoker(fake_plug* value) noexcept : owner(value) {}
    void U42_CALL on_revoke(abi::token credential) noexcept override;
};

/**
 * @brief Fake instance: business provider (iplug + iinvoke) and lease consumer in one type.
 *
 * @note As a provider it announces a typed version and a method set and answers query(invoke_iid)
 *       so the host can reach iinvoke; as a consumer it leases a version range through its own
 *       icaps and calls published methods directly through its own icalls with the returned
 *       credential, holding nothing but that credential.
 */
class fake_plug final : public abi::iplug, public abi::iinvoke {
public:
    fake_plug(const fake_flags& flags, const abi::caps_desc& announced) noexcept
        : flags_(flags), announced_(announced)
    {
    }

    abi::status U42_CALL init(abi::ictx* value) noexcept override
    {
        ++init_calls;
        ctx = value;
        if (ctx == nullptr) return abi::invalid_argument;
        void* raw = nullptr;
        if (ctx->query(&abi::caps_iid, &raw) != abi::ok || raw == nullptr) return abi::failed;
        caps = static_cast<abi::icaps*>(raw);
        raw = nullptr;
        if (ctx->query(&abi::calls_iid, &raw) != abi::ok || raw == nullptr) return abi::failed;
        calls = static_cast<abi::icalls*>(raw);
        return abi::ok;
    }
    abi::status U42_CALL start() noexcept override
    {
        ++start_calls;
        return caps == nullptr ? abi::invalid_state : caps->announce(&announced_);
    }
    abi::status U42_CALL stop() noexcept override
    {
        ++stop_calls;
        return flags_.stop_status;
    }
    void U42_CALL destroy() noexcept override { ++destroy_calls; }
    abi::status U42_CALL query(const abi::iid* type, void** out) noexcept override
    {
        if (out == nullptr || type == nullptr) return abi::invalid_argument;
        *out = nullptr;
        // Only the host-private invoke gateway is ever provided; no cross-plugin business iid.
        if (*type != abi::invoke_iid) return abi::unsupported;
        *out = static_cast<abi::iinvoke*>(this);
        return abi::ok;
    }
    abi::status U42_CALL invoke(abi::method_id method, abi::bytes args,
                                abi::iwriter* result) noexcept override
    {
        (void)args;
        (void)result;
        ++invoke_calls;
        last_method = method;
        return abi::ok;
    }

    /** @brief Lease the provider under this suite's exact published version. */
    abi::status acquire_from(const char* provider) { return acquire_range(provider, k_provider_exact); }
    /** @brief Lease the provider under an explicit required inclusive version range. */
    abi::status acquire_range(const char* provider, const abi::version_range& allowed)
    {
        if (caps == nullptr || provider == nullptr) return abi::invalid_state;
        lease_ = abi::borrow{};
        const abi::status found = caps->acquire(provider, &allowed, &receiver, &lease_);
        if (found != abi::ok) {
            lease_ = abi::borrow{};
            return found;
        }
        return abi::ok;
    }
    /** @brief Direct named call using the credential currently held by this instance. */
    abi::status call_name(const char* name) { return call_name_value(lease_.credential, name); }
    /** @brief Direct numeric call using the credential currently held by this instance. */
    abi::status call_id(abi::method_id method) { return call_id_value(lease_.credential, method); }
    /** @brief Direct named call with an arbitrary (possibly stale) credential. */
    abi::status call_name_value(abi::token credential, const char* name)
    {
        if (calls == nullptr) return abi::invalid_state;
        sink_writer writer;
        return calls->call_name(credential, name, abi::bytes{nullptr, 0}, &writer);
    }
    /** @brief Direct numeric call with an arbitrary (possibly stale) credential. */
    abi::status call_id_value(abi::token credential, abi::method_id method)
    {
        if (calls == nullptr) return abi::invalid_state;
        sink_writer writer;
        return calls->call_id(credential, method, abi::bytes{nullptr, 0}, &writer);
    }
    /** @brief Return the held credential through this instance's own icaps. */
    abi::status return_lease()
    {
        if (caps == nullptr || !holds_lease()) return abi::invalid_state;
        last_release = caps->release(lease_.credential);
        if (last_release == abi::ok) lease_ = abi::borrow{};
        return last_release;
    }
    bool holds_lease() const noexcept { return lease_.credential.value != 0; }

    std::uint64_t init_calls = 0, start_calls = 0, stop_calls = 0, destroy_calls = 0;
    std::uint64_t invoke_calls = 0;
    abi::method_id last_method = 0;
    abi::ictx* ctx = nullptr;
    abi::icaps* caps = nullptr;
    abi::icalls* calls = nullptr;
    abi::borrow lease_{};
    abi::status last_release = abi::failed;
    fake_revoker receiver{this};

private:
    friend struct fake_revoker;
    const fake_flags& flags_;
    const abi::caps_desc& announced_;
};

void U42_CALL fake_revoker::on_revoke(abi::token credential) noexcept
{
    ++calls;
    last = credential;
    if (owner == nullptr || owner->caps == nullptr) return;
    if (owner->flags_.swallow_revocation) return; // Deliberately keeps the credential.
    if (owner->lease_.credential.value != credential.value) return; // Not this credential.
    owner->last_release = owner->caps->release(credential);
    if (owner->last_release == abi::ok) owner->lease_ = abi::borrow{};
}

/** @brief Library-owned fake factory with stable descriptor storage. */
class fake_factory final : public abi::iplug_fty {
public:
    /**
     * @brief Build one type.
     *
     * @param plug_id Stable identity; the descriptor borrows this storage.
     * @param after Identities that must start before this plugin.
     * @param with_method When true the instance announces the two probe methods, so it can serve
     *                    business calls; otherwise it publishes a valid, empty method set (a pure
     *                    lifetime-lock provider).
     * @param version Plugin business version the descriptor publishes.
     */
    fake_factory(std::string plug_id, std::vector<std::string> after = {}, bool with_method = false,
                 abi::plugin_version version = k_provider_version)
        : plug_id_(std::move(plug_id)), after_(std::move(after)), with_method_(with_method),
          version_(version)
    {
        for (const std::string& value : after_) after_ptrs_.push_back(value.c_str());
        desc_.struct_size = sizeof(abi::plug_desc);
        desc_.plug_id = plug_id_.c_str();
        desc_.version = version_;
        desc_.after_count = static_cast<std::uint32_t>(after_ptrs_.size());
        desc_.after = after_ptrs_.empty() ? nullptr : after_ptrs_.data();
        method_desc_.id = k_ping_method;
        method_desc_.name = "ping";
        method_desc_.description = "safety probe";
        method_desc_.input_schema = "{}";
        method_desc_.output_schema = "{}";
        echo_desc_.id = k_echo_method;
        echo_desc_.name = "echo";
        echo_desc_.description = "safety probe";
        echo_desc_.input_schema = "{}";
        echo_desc_.output_schema = "{}";
        methods_ = {method_desc_, echo_desc_};
        caps_.struct_size = sizeof(abi::caps_desc);
        caps_.method_count = with_method_ ? static_cast<std::uint32_t>(methods_.size()) : 0u;
        caps_.methods = with_method_ ? methods_.data() : nullptr;
    }

    abi::status U42_CALL describe(const abi::plug_desc** out) noexcept override
    {
        if (out == nullptr) return abi::invalid_argument;
        *out = &desc_;
        return abi::ok;
    }
    abi::status U42_CALL create(abi::iplug** out) noexcept override
    {
        if (out == nullptr) return abi::invalid_argument;
        *out = nullptr;
        try {
            instance_ = std::make_unique<fake_plug>(flags, caps_);
        } catch (...) {
            return abi::failed;
        }
        *out = instance_.get();
        return abi::ok;
    }
    fake_plug* instance() const noexcept { return instance_.get(); }

    fake_flags flags;

private:
    std::string plug_id_;
    std::vector<std::string> after_;
    std::vector<const char*> after_ptrs_;
    bool with_method_ = false;
    abi::plugin_version version_{};
    abi::plug_desc desc_{};
    abi::method_desc method_desc_{};
    abi::method_desc echo_desc_{};
    std::vector<abi::method_desc> methods_;
    abi::caps_desc caps_{};
    std::unique_ptr<fake_plug> instance_;
};

bool has_plugin(const u42::host& rack, const char* plug_id)
{
    const std::vector<std::string> ids = rack.plugins();
    return std::find(ids.begin(), ids.end(), std::string(plug_id)) != ids.end();
}

/* ------------------------------------------------------------------ *
 * Cases.
 * ------------------------------------------------------------------ */

/**
 * @brief An unreturned consumer lease pins the provider; an active return releases the pin.
 *
 * The lease is exercised the v3 way: one credential issues direct calls of two different published
 * methods (by name and by numeric id), so the "live borrow works" proof is a real provider invoke
 * rather than a pointer call. The credential is returned in the ordinary process, so this case
 * must also pass under LSan.
 */
TEST_CASE(unreturned_borrow_pins_the_provider_until_the_return)
{
    u42::host rack;
    fake_factory provider("safety1.provider", {}, true);
    fake_factory consumer("safety1.consumer", {"safety1.provider"});
    CHECK_STATUS(rack.add(&provider), abi::ok);
    CHECK_STATUS(rack.add(&consumer), abi::ok);
    CHECK_STATUS(rack.start(), abi::ok);

    fake_plug* borrower = consumer.instance();
    CHECK_STATUS(borrower->acquire_from("safety1.provider"), abi::ok);
    CHECK(borrower->holds_lease());
    CHECK(borrower->lease_.version == k_provider_version); // The actual version comes with the lease.
    CHECK_STATUS(borrower->call_name("ping"), abi::ok);    // The only business path is a direct call.
    CHECK_STATUS(borrower->call_id(k_echo_method), abi::ok);
    CHECK(provider.instance()->invoke_calls == 2);         // Same credential, two published methods.
    CHECK(provider.instance()->last_method == k_echo_method);
    CHECK_STATUS(borrower->call_name("missing"), abi::not_found);
    CHECK_STATUS(borrower->call_id(99), abi::not_found);
    CHECK(provider.instance()->invoke_calls == 2);         // Unpublished names never reach it.
    const abi::token held = borrower->lease_.credential;
    CHECK(held.value != 0);

    consumer.flags.swallow_revocation = true; // on_revoke deliberately keeps the credential.
    CHECK_STATUS(rack.unload("safety1.provider"), abi::busy);
    CHECK(!rack.error().empty());
    CHECK(provider.instance()->stop_calls == 0);
    CHECK(provider.instance()->destroy_calls == 0);
    CHECK(borrower->receiver.calls == 1); // The host did try to revoke.
    CHECK(has_plugin(rack, "safety1.provider"));
    CHECK(has_plugin(rack, "safety1.consumer"));

    // A zero credential and another context's credential must not clear the correct lease.
    CHECK_STATUS(borrower->caps->release(abi::token{0}), abi::invalid_argument);
    CHECK_STATUS(provider.instance()->caps->release(held), abi::invalid_argument);
    CHECK(borrower->lease_.credential.value == held.value);
    CHECK_STATUS(rack.unload("safety1.provider"), abi::busy); // Still pinned.
    CHECK(provider.instance()->stop_calls == 0);
    CHECK(provider.instance()->destroy_calls == 0);

    // The control thread returns the credential: the pin is released.
    CHECK_STATUS(borrower->return_lease(), abi::ok);
    CHECK(!borrower->holds_lease());
    // The returned credential authorizes no method any more, by name or by numeric id.
    CHECK_STATUS(borrower->call_name_value(held, "ping"), abi::stale);
    CHECK_STATUS(borrower->call_id_value(held, k_ping_method), abi::stale);
    CHECK_STATUS(rack.unload("safety1.provider"), abi::ok);
    CHECK(provider.instance()->stop_calls == 1);
    CHECK(provider.instance()->destroy_calls == 1);
    CHECK(!has_plugin(rack, "safety1.provider"));
    // The already returned credential no longer identifies anything.
    CHECK_STATUS(borrower->caps->release(held), abi::stale);

    CHECK_STATUS(rack.shutdown(), abi::ok);
    CHECK(consumer.instance()->stop_calls == 1);
    CHECK(consumer.instance()->destroy_calls == 1);
}

/**
 * @brief A valid published capability set with no methods leases only the lifetime.
 */
TEST_CASE(valid_capabilities_without_methods_lease_only_the_lifetime)
{
    u42::host rack;
    fake_factory provider("safety4.provider", {}, false); // Valid version, empty method set.
    fake_factory consumer("safety4.consumer", {"safety4.provider"});
    CHECK_STATUS(rack.add(&provider), abi::ok);
    CHECK_STATUS(rack.add(&consumer), abi::ok);
    CHECK_STATUS(rack.start(), abi::ok);

    fake_plug* borrower = consumer.instance();
    // A published version is leaseable even with no methods; the lease pins the lifetime only.
    CHECK_STATUS(borrower->acquire_from("safety4.provider"), abi::ok);
    CHECK(borrower->holds_lease());
    CHECK(borrower->lease_.version == k_provider_version);
    CHECK_STATUS(borrower->call_name("ping"), abi::not_found); // Nothing was published to call.
    CHECK_STATUS(borrower->call_id(k_ping_method), abi::not_found);
    CHECK(provider.instance()->invoke_calls == 0);
    // Keep the credential across the revocation attempt so the lifetime lock is the only pin.
    consumer.flags.swallow_revocation = true;
    CHECK_STATUS(rack.unload("safety4.provider"), abi::busy);
    CHECK(provider.instance()->stop_calls == 0);
    CHECK(provider.instance()->destroy_calls == 0);

    CHECK_STATUS(borrower->return_lease(), abi::ok);
    CHECK_STATUS(rack.unload("safety4.provider"), abi::ok);
    CHECK(provider.instance()->stop_calls == 1);
    CHECK_STATUS(rack.shutdown(), abi::ok);
}

/**
 * @brief A version range that does not contain the published triple is refused without a lease.
 */
TEST_CASE(out_of_range_versions_leave_no_lease_behind)
{
    u42::host rack;
    fake_factory provider("safety5.provider", {}, false); // Publishes 1.4.0.
    fake_factory consumer("safety5.consumer", {"safety5.provider"});
    CHECK_STATUS(rack.add(&provider), abi::ok);
    CHECK_STATUS(rack.add(&consumer), abi::ok);
    CHECK_STATUS(rack.start(), abi::ok);
    fake_plug* borrower = consumer.instance();

    // Lower bound above the published 1.4.0.
    CHECK_STATUS(borrower->acquire_range("safety5.provider", abi::version_range{{1, 4, 1}, {1, 9, 9}}),
                 abi::unsupported);
    // Upper bound below the published 1.4.0.
    CHECK_STATUS(borrower->acquire_range("safety5.provider", abi::version_range{{0, 1, 0}, {1, 3, 9}}),
                 abi::unsupported);
    // An explicit range that crosses a major boundary must still contain the instance.
    CHECK_STATUS(borrower->acquire_range("safety5.provider", abi::version_range{{2, 0, 0}, {3, 0, 0}}),
                 abi::unsupported);
    // A reversed range is rejected as an argument error, not as an incompatibility.
    CHECK_STATUS(borrower->acquire_range("safety5.provider", abi::version_range{{1, 5, 0}, {1, 4, 0}}),
                 abi::invalid_argument);
    CHECK(!borrower->holds_lease());
    CHECK(borrower->lease_.credential.value == 0);
    CHECK(borrower->lease_.version == abi::plugin_version{});

    // A cross-major range is honoured when the caller explicitly includes the published version.
    CHECK_STATUS(borrower->acquire_range("safety5.provider", abi::version_range{{0, 9, 0}, {2, 0, 0}}),
                 abi::ok);
    CHECK(borrower->lease_.version == k_provider_version); // The actual version, not an endpoint.
    CHECK_STATUS(borrower->return_lease(), abi::ok);

    // No refusal leaked a lease: the provider is still unloadable without any return.
    CHECK_STATUS(rack.unload("safety5.provider"), abi::ok);
    CHECK(provider.instance()->destroy_calls == 1);
    CHECK_STATUS(rack.shutdown(), abi::ok);
}

/**
 * @brief 0.0.0 is a legal published version, never an "no capabilities" marker.
 */
TEST_CASE(zero_version_is_a_valid_published_version)
{
    u42::host rack;
    fake_factory provider("safety6.provider", {}, true, k_zero_version);
    fake_factory consumer("safety6.consumer", {"safety6.provider"});
    CHECK_STATUS(rack.add(&provider), abi::ok);
    CHECK_STATUS(rack.add(&consumer), abi::ok);
    CHECK_STATUS(rack.start(), abi::ok);

    abi::plugin_version published{};
    CHECK_STATUS(rack.version("safety6.provider", &published), abi::ok);
    CHECK(published == k_zero_version); // Discovery reports the real triple, zero included.

    fake_plug* borrower = consumer.instance();
    // 0.0.0 is compared numerically: a range that starts at 0.0.1 still excludes it.
    CHECK_STATUS(borrower->acquire_range("safety6.provider", abi::version_range{{0, 0, 1}, {1, 0, 0}}),
                 abi::unsupported);
    CHECK(!borrower->holds_lease());
    // An exact 0.0.0 requirement succeeds; the credential, not the version, proves success.
    CHECK_STATUS(borrower->acquire_range("safety6.provider", abi::exact_version(k_zero_version)),
                 abi::ok);
    CHECK(borrower->holds_lease());
    CHECK(borrower->lease_.credential.value != 0);
    CHECK(borrower->lease_.version == k_zero_version);
    CHECK_STATUS(borrower->call_name("ping"), abi::ok);
    CHECK_STATUS(borrower->call_id(k_ping_method), abi::ok);
    CHECK(provider.instance()->invoke_calls == 2);
    const abi::token held = borrower->lease_.credential;
    CHECK(held.value != 0);
    CHECK_STATUS(borrower->return_lease(), abi::ok);
    CHECK(!borrower->holds_lease());
    // The zero version travels with the credential, so only the credential proves success.
    CHECK_STATUS(borrower->call_name_value(held, "ping"), abi::stale);
    CHECK_STATUS(borrower->call_id_value(held, k_ping_method), abi::stale);

    CHECK_STATUS(rack.unload("safety6.provider"), abi::ok);
    CHECK(provider.instance()->destroy_calls == 1);
    CHECK_STATUS(rack.shutdown(), abi::ok);
}

/**
 * @brief The native administration path takes an explicit version range and leaves no stale ok.
 */
TEST_CASE(native_calls_require_an_explicit_version_range)
{
    u42::host rack;
    fake_factory provider("safety8.provider", {}, true);
    CHECK_STATUS(rack.add(&provider), abi::ok);
    CHECK_STATUS(rack.start(), abi::ok);

    // Discovery reports the current published triple without acquiring a lease or calling the plugin.
    abi::plugin_version discovered{};
    CHECK_STATUS(rack.version("safety8.provider", &discovered), abi::ok);
    CHECK(discovered == k_provider_version);
    CHECK_STATUS(rack.version("safety8.absent", &discovered), abi::not_found);
    CHECK(discovered == abi::plugin_version{}); // Cleared on failure.
    CHECK(provider.instance()->invoke_calls == 0);

    // The one-shot call acquires, directly calls and returns synchronously under the range.
    std::string output;
    CHECK_STATUS(rack.call("safety8.provider", k_provider_exact, "ping", abi::bytes{nullptr, 0}, &output),
                 abi::ok);
    CHECK(provider.instance()->invoke_calls == 1);
    // The numeric one-shot form shares the same lease-checked path.
    CHECK_STATUS(rack.call("safety8.provider", k_provider_exact, k_echo_method, abi::bytes{nullptr, 0}, &output),
                 abi::ok);
    CHECK(provider.instance()->invoke_calls == 2);

    // A range that excludes the published version is refused before any invocation.
    CHECK_STATUS(rack.call("safety8.provider", abi::version_range{{2, 0, 0}, {3, 0, 0}}, "ping",
                           abi::bytes{nullptr, 0}, &output),
                 abi::unsupported);
    CHECK(provider.instance()->invoke_calls == 2);

    // Direct calls never accept an unowned credential, and a returned lease stays returned.
    CHECK_STATUS(rack.call(abi::token{0}, "ping", abi::bytes{nullptr, 0}, &output), abi::invalid_argument);
    host_revoker revoker(&rack);
    abi::borrow lease{};
    CHECK_STATUS(rack.acquire("safety8.provider", k_provider_exact, &revoker, &lease), abi::ok);
    CHECK(lease.version == k_provider_version);
    CHECK_STATUS(rack.call(lease.credential, "ping", abi::bytes{nullptr, 0}, &output), abi::ok);
    CHECK_STATUS(rack.release(lease.credential), abi::ok);
    CHECK_STATUS(rack.release(lease.credential), abi::stale);
    CHECK_STATUS(rack.call(lease.credential, "ping", abi::bytes{nullptr, 0}, &output), abi::stale);
    CHECK_STATUS(rack.call(lease.credential, k_ping_method, abi::bytes{nullptr, 0}, &output), abi::stale);

    CHECK_STATUS(rack.shutdown(), abi::ok);
    CHECK(provider.instance()->destroy_calls == 1);
}

#if U42_SAFETY_FORK
/**
 * @brief Run one case body in a forked child and validate its exit status strictly.
 *
 * The child leaves through std::_Exit, so a deliberately retained host graph cannot be reported
 * by a leak checker as a false positive.
 *
 * @param label Child case name used by diagnostics.
 * @param body Body executed inside the child.
 * @param expect_clean true expects EXIT_SUCCESS, false expects a fatal non-zero exit.
 */
void run_in_child(const char* label, void (*body)(), bool expect_clean)
{
    if (std::fflush(nullptr) != 0) CHECK(false);
    const pid_t child = fork();
    if (child < 0) {
        ++g_failures;
        std::fprintf(stderr, "CHECK failure in %s: fork() failed with errno %d; case not run\n",
                     g_case, errno);
        std::fflush(stderr);
        return;
    }
    if (child == 0) {
        g_case = label;
        g_in_fork_child = true;
        body();
        if (std::fflush(nullptr) != 0) std::_Exit(EXIT_FAILURE);
        std::_Exit(EXIT_SUCCESS);
    }

    int status = 0;
    pid_t reaped = 0;
    do {
        reaped = waitpid(child, &status, 0);
    } while (reaped < 0 && errno == EINTR);
    if (reaped != child) {
        ++g_failures;
        std::fprintf(stderr, "CHECK failure in %s: waitpid failed with errno %d\n", g_case, errno);
        std::fflush(stderr);
        return;
    }
    const bool exited = WIFEXITED(status) != 0;
    const int code = exited ? WEXITSTATUS(status) : -1;
    if (expect_clean ? !(exited && code == EXIT_SUCCESS) : !(exited && code != EXIT_SUCCESS)) {
        ++g_failures;
        std::fprintf(stderr, "CHECK failure in %s: child %s (exited=%d code=%d signal=%d)\n",
                     g_case, label, exited ? 1 : 0, code,
                     WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        std::fflush(stderr);
        return;
    }
    std::fprintf(stderr, "[child] %s: checks passed; left via _Exit\n", label);
    std::fflush(stderr);
}

/**
 * @brief Quarantine body of the failed-stop case; runs only inside the forked child.
 *
 * The host deliberately keeps a quarantined instance, its context and its mapping alive.
 */
void quarantined_failed_stop_body()
{
    fake_factory provider("safety2.provider");
    provider.flags.stop_status = abi::failed;
    {
        u42::host rack;
        CHECK_STATUS(rack.add(&provider), abi::ok);
        CHECK_STATUS(rack.start(), abi::ok);
        CHECK(provider.instance()->stop_calls == 0);

        CHECK_FAILS(rack.unload("safety2.provider"));
        CHECK(provider.instance()->stop_calls == 1);
        CHECK(provider.instance()->destroy_calls == 0);
        CHECK(has_plugin(rack, "safety2.provider"));

        CHECK_FAILS(rack.unload("safety2.provider"));
        CHECK(provider.instance()->stop_calls == 1); // stop() is never retried automatically.
        CHECK(provider.instance()->destroy_calls == 0);

        // Leaving this scope runs ~host(): shutdown must not destroy a quarantined instance.
    }
    CHECK(provider.instance()->stop_calls == 1);
    CHECK(provider.instance()->destroy_calls == 0);
}

/**
 * @brief A failed consumer stop keeps its outgoing lease, which keeps the provider pinned.
 *
 * The consumer's own stop() returns failed while it still holds an outgoing lease on the
 * provider. The unload is refused, the lease - and therefore the provider's outgoing borrow -
 * survives, and the retained graph is only released by std::_Exit.
 */
void failed_consumer_stop_body()
{
    fake_factory provider("safety7.provider");
    fake_factory consumer("safety7.consumer", {"safety7.provider"});
    consumer.flags.stop_status = abi::failed;
    {
        u42::host rack;
        CHECK_STATUS(rack.add(&provider), abi::ok);
        CHECK_STATUS(rack.add(&consumer), abi::ok);
        CHECK_STATUS(rack.start(), abi::ok);
        fake_plug* borrower = consumer.instance();
        CHECK_STATUS(borrower->acquire_from("safety7.provider"), abi::ok);
        CHECK(borrower->holds_lease());

        CHECK_FAILS(rack.unload("safety7.consumer")); // stop() failed: the consumer is retained.
        CHECK(consumer.instance()->stop_calls == 1);
        CHECK(consumer.instance()->destroy_calls == 0);
        CHECK(borrower->holds_lease()); // Its outgoing borrow was not cleared.
        CHECK(provider.instance()->stop_calls == 0);
        CHECK(provider.instance()->destroy_calls == 0);

        // Leaving this scope runs ~host(); the retained graph must not be reported as a leak, so
        // the child leaves through std::_Exit instead of the normal exit path.
    }
}

/** @brief Child body that must fail: it proves CHECK is still fatal under NDEBUG. */
void ndebug_probe_body()
{
    CHECK(1 == 0); // Expected: terminates this child with EXIT_FAILURE.
    std::_Exit(EXIT_SUCCESS); // Only reachable when CHECK was compiled away.
}
#endif

/**
 * @brief A provider whose stop() failed stays quarantined, even at host destruction.
 *
 * The retained graph is intentional, so the child leaves through std::_Exit and the parent only
 * accepts a clean exit status from waitpid.
 */
TEST_CASE(failed_stop_is_quarantined_even_at_host_destruction)
{
#if U42_SAFETY_FORK
    run_in_child("child:failed_stop_is_quarantined", &quarantined_failed_stop_body, true);
#else
    std::fprintf(stderr,
                 "[skip] %s: no fork/waitpid on this platform; the quarantined failed-stop case "
                 "was NOT executed\n",
                 g_case);
    std::fflush(stderr);
#endif
}

/**
 * @brief A consumer whose stop() failed keeps its outgoing lease, so its provider is pinned.
 */
TEST_CASE(failed_consumer_stop_retains_its_outgoing_lease)
{
#if U42_SAFETY_FORK
    run_in_child("child:failed_consumer_stop_retains_its_outgoing_lease", &failed_consumer_stop_body,
                 true);
#else
    std::fprintf(stderr,
                 "[skip] %s: no fork/waitpid on this platform; the failed-consumer-stop case "
                 "was NOT executed\n",
                 g_case);
    std::fflush(stderr);
#endif
}

/** @brief A lease of the previous generation must not touch the reloaded instance. */
TEST_CASE(old_credential_does_not_match_the_new_instance)
{
    u42::host rack;
    fake_factory provider("safety3.provider", {}, true);
    fake_factory consumer("safety3.consumer", {"safety3.provider"});
    CHECK_STATUS(rack.add(&provider), abi::ok);
    CHECK_STATUS(rack.add(&consumer), abi::ok);
    CHECK_STATUS(rack.start(), abi::ok);

    fake_plug* borrower = consumer.instance();
    CHECK_STATUS(borrower->acquire_from("safety3.provider"), abi::ok);
    CHECK_STATUS(borrower->call_name("ping"), abi::ok);
    const abi::token first = borrower->lease_.credential;
    CHECK(first.value != 0);
    CHECK_STATUS(borrower->call_id(k_echo_method), abi::ok);
    CHECK(provider.instance()->invoke_calls == 2);
    // The consumer returns normally during revocation, so the first unload can complete.
    CHECK_STATUS(rack.unload("safety3.provider"), abi::ok);
    CHECK(provider.instance()->destroy_calls == 1);
    // That unload returned the lease: the old credential is stale for both call forms.
    CHECK_STATUS(borrower->call_name_value(first, "ping"), abi::stale);
    CHECK_STATUS(borrower->call_id_value(first, k_ping_method), abi::stale);

    // Reload the same identity with the same version and the same method set: new generation.
    consumer.flags.swallow_revocation = true;
    CHECK_STATUS(rack.add(&provider), abi::ok);
    CHECK_STATUS(rack.start(), abi::ok);
    CHECK_STATUS(borrower->acquire_from("safety3.provider"), abi::ok);
    const abi::token second = borrower->lease_.credential;
    CHECK(second.value != first.value);
    CHECK(provider.instance()->invoke_calls == 0); // Fresh instance, fresh counters.

    CHECK_STATUS(borrower->caps->release(first), abi::stale); // Old credential, new instance.
    CHECK(borrower->lease_.credential.value == second.value); // Correct lease untouched.
    CHECK_STATUS(rack.unload("safety3.provider"), abi::busy);
    CHECK(provider.instance()->stop_calls == 0);
    CHECK(provider.instance()->destroy_calls == 0);

    CHECK_STATUS(borrower->return_lease(), abi::ok);
    CHECK_STATUS(rack.unload("safety3.provider"), abi::ok);
    CHECK(provider.instance()->stop_calls == 1);
    CHECK(provider.instance()->destroy_calls == 1);
    CHECK_STATUS(rack.shutdown(), abi::ok);
}

/**
 * @brief The NDEBUG-proof CHECK must still be fatal; a forked child proves it empirically.
 *
 * A clean non-zero child exit is the expected observation. A zero status would mean CHECK no
 * longer stops a case, and a signal would mean it terminated the child abnormally.
 */
TEST_CASE(check_stays_fatal_under_ndebug)
{
#if U42_SAFETY_FORK
    run_in_child("child:ndebug_check_probe (the CHECK failure below is expected)",
                 &ndebug_probe_body, false);
#else
    std::fprintf(stderr,
                 "[skip] %s: no fork/waitpid on this platform; the NDEBUG CHECK probe was NOT "
                 "executed\n",
                 g_case);
    std::fflush(stderr);
#endif
}

} // namespace

int main()
{
    for (const test_case& entry : registry()) {
        g_case = entry.name;
        const std::size_t mark = g_failures;
        entry.run();
        std::fprintf(stderr, "%s %s\n", g_failures == mark ? "ok  " : "FAIL", entry.name);
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "%zu CHECK failure(s)\n", g_failures);
        std::fflush(stderr);
        return EXIT_FAILURE;
    }
    std::fprintf(stderr, "all safety cases passed\n");
    std::fflush(stderr);
    return EXIT_SUCCESS;
}
