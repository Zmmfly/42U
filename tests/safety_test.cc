/**
 * @file safety_test.cc
 * @brief First-pass unload-refusal and isolation safety checks for the 42U host.
 *
 * Self-contained: owns its main(), includes only the public <42u/host.hpp> (which pulls
 * <42u/abi.hpp>), inspects no host internals and never probes an expired bare ABI value or a
 * destroyed context "to see whether it still works".
 *
 * Covered contracts:
 *  - an unreturned consumer borrow pins the provider: unload() fails with busy, stop()/destroy()
 *    stay at zero, both identities stay registered, and a zero or foreign credential is refused
 *    without clearing the correct borrow; an active return then unloads with stop()/destroy() == 1;
 *  - a provider whose stop() returns failed is quarantined: unload never destroys it, a second
 *    unload never retries stop(), and ~host() leaves the pinned graph alive. That graph is
 *    retained on purpose, so the case runs in a forked child leaving through std::_Exit while the
 *    parent checks waitpid strictly; without fork the case prints an explicit skip;
 *  - a credential of an older generation never matches the reloaded instance and never disturbs
 *    the new, live borrow;
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

namespace abi = u42::abi::v1;

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

/** @brief Fake instance: idiag provider and borrow-holding consumer in one type. */
class fake_plug final : public abi::iplug, public abi::idiag {
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
        if (*type != abi::diag_iid) return abi::unsupported;
        last_published = static_cast<abi::idiag*>(this);
        *out = last_published;
        return abi::ok;
    }
    void U42_CALL log(const char*) noexcept override { ++log_calls; }

    /** @brief Borrow idiag from an active provider through this instance's context. */
    abi::status acquire_from(const char* provider)
    {
        if (caps == nullptr || provider == nullptr) return abi::invalid_state;
        lease_ = abi::borrow{};
        const abi::status found = caps->acquire(provider, &abi::diag_iid, &receiver, &lease_);
        if (found != abi::ok) {
            lease_ = abi::borrow{};
            return found;
        }
        cached = static_cast<abi::idiag*>(lease_.ptr);
        return abi::ok;
    }
    /** @brief Return the cached credential through this instance's own context. */
    abi::status return_lease()
    {
        if (caps == nullptr || !holds_lease()) return abi::invalid_state;
        last_release = caps->release(lease_.credential);
        if (last_release == abi::ok) {
            lease_ = abi::borrow{};
            cached = nullptr;
        }
        return last_release;
    }
    bool holds_lease() const noexcept { return lease_.credential.value != 0; }

    std::uint64_t init_calls = 0, start_calls = 0, stop_calls = 0, destroy_calls = 0, log_calls = 0;
    abi::ictx* ctx = nullptr;
    abi::icaps* caps = nullptr;
    abi::borrow lease_{};
    /** Pointer published by query(); a borrow must carry exactly this value. */
    abi::idiag* last_published = nullptr;
    /** Borrowed interface, cleared when its credential is returned and never used afterwards. */
    abi::idiag* cached = nullptr;
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
    if (owner->last_release == abi::ok) {
        owner->lease_ = abi::borrow{};
        owner->cached = nullptr;
    }
}

/** @brief Library-owned fake factory with stable descriptor storage. */
class fake_factory final : public abi::iplug_fty {
public:
    /** @brief Build one type; @p after lists identities that must start before this plugin. */
    explicit fake_factory(std::string plug_id, std::vector<std::string> after = {})
        : plug_id_(std::move(plug_id)), after_(std::move(after))
    {
        for (const std::string& value : after_) after_ptrs_.push_back(value.c_str());
        desc_.struct_size = sizeof(abi::plug_desc);
        desc_.plug_id = plug_id_.c_str();
        desc_.version = "1.0";
        desc_.after_count = static_cast<std::uint32_t>(after_ptrs_.size());
        desc_.after = after_ptrs_.empty() ? nullptr : after_ptrs_.data();
        interfaces_.push_back(abi::diag_iid);
        caps_.struct_size = sizeof(abi::caps_desc);
        caps_.interface_count = 1;
        caps_.interfaces = interfaces_.data();
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
    abi::plug_desc desc_{};
    std::vector<abi::iid> interfaces_;
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
 * @brief An unreturned borrow pins the provider; an active return releases the pin.
 *
 * Stays in the ordinary process (credential returned, both instances destroyed), so this case
 * must also pass under ASan/LSan.
 */
TEST_CASE(unreturned_borrow_pins_the_provider_until_the_return)
{
    u42::host rack;
    fake_factory provider("safety1.provider");
    fake_factory consumer("safety1.consumer", {"safety1.provider"});
    CHECK_STATUS(rack.add(&provider), abi::ok);
    CHECK_STATUS(rack.add(&consumer), abi::ok);
    CHECK_STATUS(rack.start(), abi::ok);

    fake_plug* borrower = consumer.instance();
    CHECK_STATUS(borrower->acquire_from("safety1.provider"), abi::ok);
    CHECK(borrower->holds_lease());
    CHECK(borrower->cached == provider.instance()->last_published);
    borrower->cached->log("live"); // Used only while its credential is live.
    CHECK(provider.instance()->log_calls == 1);
    const abi::token held = borrower->lease_.credential;

    consumer.flags.swallow_revocation = true; // on_revoke deliberately keeps the credential.
    CHECK_STATUS(rack.unload("safety1.provider"), abi::busy);
    CHECK(!rack.error().empty());
    CHECK(provider.instance()->stop_calls == 0);
    CHECK(provider.instance()->destroy_calls == 0);
    CHECK(borrower->receiver.calls == 1); // The host did try to revoke.
    CHECK(has_plugin(rack, "safety1.provider"));
    CHECK(has_plugin(rack, "safety1.consumer"));

    // A zero credential and another context's credential must not clear the correct borrow.
    CHECK_STATUS(borrower->caps->release(abi::token{0}), abi::invalid_argument);
    CHECK_STATUS(provider.instance()->caps->release(held), abi::invalid_argument);
    CHECK(borrower->lease_.credential.value == held.value);
    CHECK_STATUS(rack.unload("safety1.provider"), abi::busy); // Still pinned.
    CHECK(provider.instance()->stop_calls == 0);
    CHECK(provider.instance()->destroy_calls == 0);

    // The control thread returns the credential: the pin is released.
    CHECK_STATUS(borrower->return_lease(), abi::ok);
    CHECK(!borrower->holds_lease());
    CHECK(borrower->cached == nullptr);
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

/** @brief A credential of the previous generation must not touch the reloaded instance. */
TEST_CASE(old_credential_does_not_match_the_new_instance)
{
    u42::host rack;
    fake_factory provider("safety3.provider");
    fake_factory consumer("safety3.consumer", {"safety3.provider"});
    CHECK_STATUS(rack.add(&provider), abi::ok);
    CHECK_STATUS(rack.add(&consumer), abi::ok);
    CHECK_STATUS(rack.start(), abi::ok);

    fake_plug* borrower = consumer.instance();
    CHECK_STATUS(borrower->acquire_from("safety3.provider"), abi::ok);
    const abi::token first = borrower->lease_.credential;
    CHECK(first.value != 0);
    // The consumer returns normally during revocation, so the first unload can complete.
    CHECK_STATUS(rack.unload("safety3.provider"), abi::ok);
    CHECK(provider.instance()->destroy_calls == 1);

    // Reload the same identity: new instance, new generation, and a deliberately kept borrow.
    consumer.flags.swallow_revocation = true;
    CHECK_STATUS(rack.add(&provider), abi::ok);
    CHECK_STATUS(rack.start(), abi::ok);
    CHECK_STATUS(borrower->acquire_from("safety3.provider"), abi::ok);
    const abi::token second = borrower->lease_.credential;
    CHECK(second.value != first.value);

    CHECK_STATUS(borrower->caps->release(first), abi::stale); // Old credential, new instance.
    CHECK(borrower->lease_.credential.value == second.value); // Correct borrow untouched.
    CHECK_STATUS(rack.unload("safety3.provider"), abi::busy);
    CHECK(provider.instance()->stop_calls == 0);
    CHECK(provider.instance()->destroy_calls == 0);

    CHECK_STATUS(borrower->return_lease(), abi::ok);
    CHECK_STATUS(rack.unload("safety3.provider"), abi::ok);
    CHECK(provider.instance()->stop_calls == 1);
    CHECK(provider.instance()->destroy_calls == 1);
    CHECK_STATUS(rack.shutdown(), abi::ok);
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
