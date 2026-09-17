/**
 * @file sdk_test.cc
 * @brief Self-contained checks for the header-only helpers in 42u/sdk.hpp; no test framework.
 *
 * The header is header-only, so this file owns its main() and links against nothing but the
 * standard library:
 *   g++ -std=c++17 -Wall -Wextra -Werror -Iinc tests/sdk_test.cc -o /tmp/u42-sdk-test
 *
 * @note NDEBUG is defined deliberately: CHECK must keep reporting failures even when assert()
 *       has been compiled out, so a diagnostic can never vanish in release builds.
 */
#define NDEBUG 1

#include <42u/sdk.hpp>

#include <cassert>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace abi = u42::abi;
namespace sdk = u42::sdk;

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
 * disappear. This is checked on the expansion text because a runtime probe cannot detect it: if
 * CHECK had become a no-op, the probe itself would be a no-op as well.
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
#define TEST_CASE(name)                                                        \
    void name();                                                               \
    [[maybe_unused]] const registrar reg_##name(#name, &name);                 \
    void name()

/**
 * @brief Interface stand-in for lease<T>; the tests only need a distinct, writable address.
 */
struct fake_iface {
    int value = 0;
};

/**
 * @brief Fake icaps that records borrows and returns instead of revoking asynchronously.
 *
 * The real service cannot be produced inside this test binary, and these checks only care about
 * the credential bookkeeping the sdk helpers perform on top of it.
 */
struct fake_caps final : abi::v1::icaps {
    abi::v1::status U42_CALL announce(const abi::v1::caps_desc*) noexcept override
    {
        return abi::v1::unsupported;
    }
    abi::v1::status U42_CALL watch(abi::v1::icap_sink*, abi::v1::token*) noexcept override
    {
        return abi::v1::unsupported;
    }
    abi::v1::status U42_CALL unwatch(abi::v1::token) noexcept override
    {
        return abi::v1::unsupported;
    }

    /** @brief Issue the configured borrow and remember who would be revoked. */
    abi::v1::status U42_CALL acquire(const char* plug_id, const abi::v1::iid* type,
                                     abi::v1::irevoker* receiver,
                                     abi::v1::borrow* out) noexcept override
    {
        ++acquire_count;
        last_plug_id = plug_id;
        last_type = type != nullptr ? *type : abi::v1::iid{};
        last_receiver = receiver;
        if (out != nullptr) *out = abi::v1::borrow{&iface, next_credential};
        return abi::v1::ok;
    }

    /** @brief Record one returned credential and report the configured status. */
    abi::v1::status U42_CALL release(abi::v1::token credential) noexcept override
    {
        ++release_count;
        last_released = credential;
        return next_release;
    }

    fake_iface iface{};                         //!< Borrowed object handed out by acquire().
    abi::v1::token next_credential{42};          //!< Credential acquire() issues next.
    abi::v1::status next_release = abi::v1::ok;  //!< Status release() reports next.
    int acquire_count = 0;                       //!< Number of acquire() calls.
    int release_count = 0;                       //!< Number of release() calls.
    abi::v1::token last_released{};              //!< Credential passed to the last release().
    abi::v1::irevoker* last_receiver = nullptr;  //!< Revocation target registered by acquire().
    const char* last_plug_id = nullptr;          //!< Plugin identity passed to acquire().
    abi::v1::iid last_type{};                    //!< Identifier passed to acquire().
};

/**
 * @brief Fake ictx whose query() writes its output on both paths.
 *
 * Writing even on failure is deliberate: it pins that sdk::query() clears the caller's output
 * itself instead of trusting a failing host to leave it alone.
 */
struct fake_ictx final : abi::v1::ictx {
    abi::v1::status next_status = abi::v1::ok;  //!< Status the next query() reports.
    void* next_value = nullptr;                 //!< Value the next query() stores.
    int query_count = 0;                        //!< Number of query() calls seen.
    abi::v1::iid last_type{};                   //!< Identifier the last query() received.

    abi::v1::status U42_CALL query(const abi::v1::iid* type, void** out) noexcept override
    {
        ++query_count;
        if (type != nullptr) last_type = *type;
        if (out != nullptr) *out = next_value;
        return next_status;
    }
};

/**
 * @brief Stable revocation owner: the object registered with acquire() is separate from the
 *        lease it guards, so moving the lease never moves the callback target.
 */
struct revoking_owner final : abi::v1::irevoker {
    sdk::lease<fake_iface>* held = nullptr;  //!< Lease this owner keeps caching.
    int revocations = 0;                     //!< Number of on_revoke() deliveries.

    /** @brief Release the lease only when the revoked credential is the one it still holds. */
    void U42_CALL on_revoke(abi::v1::token credential) noexcept override
    {
        ++revocations;
        if (held != nullptr && held->matches(credential)) (void)held->reset();
    }
};

TEST_CASE(query_returns_typed_pointer_and_forwards_iid)
{
    fake_caps caps;
    fake_ictx ctx;
    ctx.next_value = &caps;

    abi::v1::icaps* out = nullptr;
    CHECK(sdk::query<abi::v1::icaps>(&ctx, abi::v1::caps_iid, &out) == abi::v1::ok);
    CHECK(out == &caps);
    CHECK(ctx.query_count == 1);
    CHECK(ctx.last_type == abi::v1::caps_iid);

    // The identifier is passed through verbatim; the helper must not substitute a default.
    out = nullptr;
    CHECK(sdk::query<abi::v1::icaps>(&ctx, abi::v1::diag_iid, &out) == abi::v1::ok);
    CHECK(ctx.last_type == abi::v1::diag_iid);
    CHECK(ctx.query_count == 2);

    // A host that reports ok with a null pointer yields an empty output, never a fabricated one.
    ctx.next_value = nullptr;
    out = &caps;
    CHECK(sdk::query<abi::v1::icaps>(&ctx, abi::v1::caps_iid, &out) == abi::v1::ok);
    CHECK(out == nullptr);
}

TEST_CASE(query_failure_clears_output)
{
    fake_caps caps;
    fake_ictx ctx;
    ctx.next_value = &caps;
    ctx.next_status = abi::v1::not_found;

    abi::v1::icaps* out = &caps; // deliberately dirty: a failure must clear it
    CHECK(sdk::query<abi::v1::icaps>(&ctx, abi::v1::caps_iid, &out) == abi::v1::not_found);
    CHECK(out == nullptr);
    CHECK(ctx.query_count == 1);

    // Null context: rejected before any dereference, output still cleared.
    out = &caps;
    CHECK(sdk::query<abi::v1::icaps>(nullptr, abi::v1::caps_iid, &out) ==
          abi::v1::invalid_argument);
    CHECK(out == nullptr);
    CHECK(ctx.query_count == 1);

    // Null output has nowhere to report anything, so it is rejected without calling the host.
    CHECK(sdk::query<abi::v1::icaps>(&ctx, abi::v1::caps_iid, nullptr) ==
          abi::v1::invalid_argument);
    CHECK(ctx.query_count == 1);
}

TEST_CASE(lease_move_transfers_ownership_once)
{
    fake_caps caps;
    abi::v1::borrow borrowed{};
    CHECK(caps.acquire("demo", &abi::v1::diag_iid, nullptr, &borrowed) == abi::v1::ok);
    CHECK(borrowed.ptr == &caps.iface);

    {
        sdk::lease<fake_iface> owner(&caps, borrowed);
        {
            sdk::lease<fake_iface> moved(std::move(owner));
            CHECK(!owner);
            CHECK(owner.get() == nullptr);
            CHECK(owner.credential().value == 0);
            CHECK(static_cast<bool>(moved));
            CHECK(moved.get() == &caps.iface);
            CHECK(moved.credential().value == 42);
            moved->value = 5;
            CHECK(caps.iface.value == 5);
            CHECK(caps.release_count == 0);
        }
        // The moved-to lease returned the credential exactly once...
        CHECK(caps.release_count == 1);
        CHECK(caps.last_released.value == 42);
    }
    // ...and destroying the moved-from lease afterwards adds no second release.
    CHECK(caps.release_count == 1);
}

TEST_CASE(lease_move_assignment_returns_previous_credential)
{
    fake_caps caps;
    abi::v1::borrow first{};
    caps.next_credential = abi::v1::token{7};
    CHECK(caps.acquire("demo", &abi::v1::diag_iid, nullptr, &first) == abi::v1::ok);
    abi::v1::borrow second{};
    caps.next_credential = abi::v1::token{9};
    CHECK(caps.acquire("demo", &abi::v1::diag_iid, nullptr, &second) == abi::v1::ok);

    {
        sdk::lease<fake_iface> target(&caps, first);
        sdk::lease<fake_iface> source(&caps, second);
        target = std::move(source);

        // The credential target already held is returned before source's borrow is adopted.
        CHECK(caps.release_count == 1);
        CHECK(caps.last_released.value == 7);
        CHECK(target.get() == &caps.iface);
        CHECK(target.credential().value == 9);
        CHECK(!source);
        CHECK(source.get() == nullptr);
        CHECK(source.credential().value == 0);

        // Assigning an empty lease still returns the credential the target held.
        sdk::lease<fake_iface> blank;
        target = std::move(blank);
        CHECK(caps.release_count == 2);
        CHECK(caps.last_released.value == 9);
        CHECK(!target);
        CHECK(!blank);
    }
    CHECK(caps.release_count == 2);
    CHECK(caps.last_released.value == 9);
}

TEST_CASE(lease_reset_keeps_ownership_on_retryable_failure_then_succeeds)
{
    fake_caps caps;
    caps.next_credential = abi::v1::token{3};
    caps.next_release = abi::v1::busy;
    abi::v1::borrow borrowed{};
    CHECK(caps.acquire("demo", &abi::v1::diag_iid, nullptr, &borrowed) == abi::v1::ok);

    {
        sdk::lease<fake_iface> held(&caps, borrowed);
        CHECK(held.reset() == abi::v1::busy); // the refusal is reported, not swallowed
        CHECK(caps.release_count == 1);
        CHECK(caps.last_released.value == 3);
        // A busy release is retryable: the lease must still own the pointer and the credential,
        // otherwise the borrow the host still tracks would be silently lost.
        CHECK(static_cast<bool>(held));
        CHECK(held.get() == &caps.iface);
        CHECK(held.credential().value == 3);
        CHECK(held.matches(abi::v1::token{3}));

        // The control thread retries once the host can accept the return.
        caps.next_release = abi::v1::ok;
        CHECK(held.reset() == abi::v1::ok);
        CHECK(caps.release_count == 2);
        CHECK(caps.last_released.value == 3);
        CHECK(held.get() == nullptr);
        CHECK(held.credential().value == 0);
        CHECK(!held);

        // Idempotent: the cleared lease has nothing left to return.
        CHECK(held.reset() == abi::v1::ok);
        CHECK(caps.release_count == 2);
    }
    // The destructor of the already reset lease returns nothing either.
    CHECK(caps.release_count == 2);
}

TEST_CASE(lease_destructor_attempts_one_final_release)
{
    fake_caps caps;
    caps.next_credential = abi::v1::token{11};
    caps.next_release = abi::v1::wrong_thread;
    abi::v1::borrow borrowed{};
    CHECK(caps.acquire("demo", &abi::v1::diag_iid, nullptr, &borrowed) == abi::v1::ok);

    {
        sdk::lease<fake_iface> held(&caps, borrowed);
        CHECK(held.reset() == abi::v1::wrong_thread);
        CHECK(static_cast<bool>(held)); // still owned after a retryable refusal
    }
    // Destruction gets one last chance; it cannot retry, so the refused credential is simply not
    // returned again. That is why the class comment demands destruction on the owning thread.
    CHECK(caps.release_count == 2);
    CHECK(caps.last_released.value == 11);
}

TEST_CASE(lease_reset_stale_credential_clears_ownership)
{
    fake_caps caps;
    caps.next_credential = abi::v1::token{21};
    caps.next_release = abi::v1::stale;
    abi::v1::borrow borrowed{};
    CHECK(caps.acquire("demo", &abi::v1::diag_iid, nullptr, &borrowed) == abi::v1::ok);

    sdk::lease<fake_iface> held(&caps, borrowed);
    // A stale credential already belongs to a newer record, so it can never be returned again.
    CHECK(held.reset() == abi::v1::stale);
    CHECK(caps.release_count == 1);
    CHECK(caps.last_released.value == 21);
    CHECK(!held);
    CHECK(held.get() == nullptr);
    CHECK(held.credential().value == 0);
    CHECK(!held.matches(abi::v1::token{21}));
    CHECK(held.reset() == abi::v1::ok); // nothing left to return
    CHECK(caps.release_count == 1);
}

TEST_CASE(lease_move_assignment_refusal_keeps_both_sides_owned)
{
    fake_caps caps;
    abi::v1::borrow first{};
    caps.next_credential = abi::v1::token{31};
    CHECK(caps.acquire("demo", &abi::v1::diag_iid, nullptr, &first) == abi::v1::ok);
    abi::v1::borrow second{};
    caps.next_credential = abi::v1::token{32};
    CHECK(caps.acquire("demo", &abi::v1::diag_iid, nullptr, &second) == abi::v1::ok);

    caps.next_release = abi::v1::failed;
    sdk::lease<fake_iface> target(&caps, first);
    sdk::lease<fake_iface> source(&caps, second);

    target = std::move(source);
    // The assignment could not return target's old credential, so it must not have moved the
    // borrow either: both sides keep exactly what they owned and nothing is silently dropped.
    CHECK(caps.release_count == 1);
    CHECK(caps.last_released.value == 31);
    CHECK(target.credential().value == 31);
    CHECK(target.get() == &caps.iface);
    CHECK(source.credential().value == 32);
    CHECK(source.get() == &caps.iface);

    // Retrying the same assignment once the host accepts the return completes the transfer.
    caps.next_release = abi::v1::ok;
    target = std::move(source);
    CHECK(caps.release_count == 2);
    CHECK(caps.last_released.value == 31);
    CHECK(target.credential().value == 32);
    CHECK(target.get() == &caps.iface);
    CHECK(!source);
    CHECK(source.get() == nullptr);
    CHECK(source.credential().value == 0);
}

TEST_CASE(lease_without_owner_cannot_return_credential)
{
    fake_iface object;
    sdk::lease<fake_iface> orphan(nullptr, abi::v1::borrow{&object, abi::v1::token{5}});
    CHECK(orphan.get() == &object);
    CHECK(orphan.matches(abi::v1::token{5}));
    // Without an icaps there is no release to call and no ok/stale to observe, so ownership must
    // survive: an unstoppable credential is reported instead of being dropped as if returned.
    CHECK(orphan.reset() == abi::v1::invalid_state);
    CHECK(static_cast<bool>(orphan));
    CHECK(orphan.get() == &object);
    CHECK(orphan.credential().value == 5);
    CHECK(orphan.reset() == abi::v1::invalid_state);

    sdk::lease<fake_iface> empty;
    CHECK(!empty);
    CHECK(empty.reset() == abi::v1::ok);
    CHECK(!empty.matches(abi::v1::token{0}));

    // A lease that owns a pointer but no credential has nothing to hand back: reset() reports ok
    // and clears the pointer without calling release() at all.
    fake_caps caps;
    sdk::lease<fake_iface> credentialless(&caps, abi::v1::borrow{&object, abi::v1::token{}});
    CHECK(static_cast<bool>(credentialless));
    CHECK(credentialless.reset() == abi::v1::ok);
    CHECK(!credentialless);
    CHECK(caps.release_count == 0);
}

TEST_CASE(lease_matches_credential_for_explicit_revocation)
{
    fake_caps caps;
    caps.next_credential = abi::v1::token{77};
    revoking_owner owner;
    abi::v1::borrow borrowed{};
    CHECK(caps.acquire("demo", &abi::v1::diag_iid, &owner, &borrowed) == abi::v1::ok);
    // The registered target is the stable owner, never the movable lease itself.
    CHECK(caps.last_receiver == &owner);
    CHECK(caps.last_plug_id != nullptr);
    CHECK(owner.revocations == 0);

    sdk::lease<fake_iface> held(&caps, borrowed);
    owner.held = &held;

    CHECK(held.matches(abi::v1::token{77}));
    CHECK(!held.matches(abi::v1::token{78}));
    CHECK(!held.matches(abi::v1::token{0}));

    // A revocation for a different credential leaves this lease intact.
    owner.on_revoke(abi::v1::token{78});
    CHECK(caps.release_count == 0);
    CHECK(static_cast<bool>(held));

    // A revoker that runs while the host refuses the return keeps the lease owning the borrow,
    // so the same revocation can be replayed on the control thread until release() accepts it.
    caps.next_release = abi::v1::busy;
    owner.on_revoke(abi::v1::token{77});
    CHECK(caps.release_count == 1);
    CHECK(held.matches(abi::v1::token{77}));
    caps.next_release = abi::v1::ok;

    // The matching revocation returns the credential and empties the lease.
    owner.on_revoke(abi::v1::token{77});
    CHECK(caps.release_count == 2);
    CHECK(caps.last_released.value == 77);
    CHECK(!held);
    CHECK(!held.matches(abi::v1::token{77}));

    // Replaying the same revocation cannot release twice.
    owner.on_revoke(abi::v1::token{77});
    CHECK(owner.revocations == 4);
    CHECK(caps.release_count == 2);

    owner.held = nullptr;
    CHECK(caps.release_count == 2);
}

TEST_CASE(view_borrows_string_data)
{
    const abi::v1::bytes none = sdk::view(std::string_view{});
    CHECK(none.data == nullptr);
    CHECK(none.size == 0);

    const std::string text = "hello";
    const abi::v1::bytes raw = sdk::view(std::string_view{text});
    CHECK(raw.data == text.data()); // same address: the view is borrowed, not copied
    CHECK(raw.size == 5u);

    // A view with a non-null but empty range still normalizes to {nullptr, 0}, because the ABI
    // requires a null pointer to carry size zero.
    const std::string_view empty_tail(text.data() + text.size(), 0);
    CHECK(empty_tail.data() != nullptr);
    const abi::v1::bytes tail = sdk::view(empty_tail);
    CHECK(tail.data == nullptr);
    CHECK(tail.size == 0);
}

TEST_CASE(writer_appends_through_the_abi_interface)
{
    std::string out;
    sdk::string_writer writer(&out, 8);
    abi::v1::iwriter* abi_writer = &writer; // the ABI only ever sees iwriter*

    CHECK(abi_writer->write(sdk::view("ab")) == abi::v1::ok);
    CHECK(out == "ab");
    CHECK(abi_writer->write(sdk::view("cd")) == abi::v1::ok);
    CHECK(out == "abcd");
    CHECK(writer.status() == abi::v1::ok);

    // A zero-length chunk is legal even with a null pointer and must change nothing.
    CHECK(abi_writer->write(abi::v1::bytes{nullptr, 0}) == abi::v1::ok);
    CHECK(out == "abcd");
}

TEST_CASE(writer_rejects_over_limit_atomically_and_stickily)
{
    std::string out;
    sdk::string_writer writer(&out, 4);
    CHECK(writer.write(sdk::view("abcde")) == abi::v1::limit_exceeded);
    CHECK(out.empty()); // no truncated prefix is published
    CHECK(writer.status() == abi::v1::limit_exceeded);
    CHECK(writer.write(sdk::view("ab")) == abi::v1::limit_exceeded); // sticky
    CHECK(out.empty());

    // The cap counts the whole target: filling it exactly is allowed, one byte more is not.
    std::string exact;
    sdk::string_writer cumulative(&exact, 4);
    CHECK(cumulative.write(sdk::view("abcd")) == abi::v1::ok);
    CHECK(cumulative.write(sdk::view("e")) == abi::v1::limit_exceeded);
    CHECK(exact == "abcd");
    CHECK(cumulative.status() == abi::v1::limit_exceeded);

    // A target that already exceeds the cap must not become writable: the remaining room must
    // not underflow to a huge value.
    std::string over = "abcdef";
    sdk::string_writer clamped(&over, 2);
    CHECK(clamped.write(abi::v1::bytes{nullptr, 0}) == abi::v1::ok); // zero bytes never exceed
    CHECK(over == "abcdef");
    CHECK(clamped.write(sdk::view("g")) == abi::v1::limit_exceeded);
    CHECK(over == "abcdef");
}

TEST_CASE(writer_rejects_null_target_and_malformed_bytes)
{
    sdk::string_writer detached(nullptr, 16);
    CHECK(detached.write(sdk::view("x")) == abi::v1::invalid_argument);
    CHECK(detached.status() == abi::v1::invalid_argument);
    CHECK(detached.write(abi::v1::bytes{nullptr, 0}) == abi::v1::invalid_argument); // sticky

    std::string out;
    sdk::string_writer writer(&out, 8);
    CHECK(writer.write(abi::v1::bytes{nullptr, 3}) == abi::v1::invalid_argument);
    CHECK(out.empty());
    CHECK(writer.status() == abi::v1::invalid_argument);
    CHECK(writer.write(sdk::view("ok")) == abi::v1::invalid_argument); // sticky
    CHECK(out.empty());
}

TEST_CASE(writer_converts_thrown_exception_into_status)
{
    std::string out;
    const char marker = 'x';
    sdk::string_writer writer(&out, std::numeric_limits<std::size_t>::max());

    // No buffer can hold this many bytes, so appending must throw (std::length_error or
    // std::bad_alloc); write() is noexcept and has to turn that into a status instead of
    // unwinding into the ABI.
    CHECK(writer.write(abi::v1::bytes{&marker, std::numeric_limits<std::uint64_t>::max()}) ==
          abi::v1::failed);
    CHECK(writer.status() == abi::v1::failed);
    CHECK(out.empty());
    CHECK(writer.write(sdk::view("small")) == abi::v1::failed); // sticky
    CHECK(out.empty());
}

std::jmp_buf* g_jump_target = nullptr;
const char* g_reported_expr = nullptr;

TEST_CASE(check_is_not_disabled_by_ndebug)
{
    // NDEBUG (defined at the top of this file) removes assert(); the static_assert above already
    // pins that CHECK is not assert(). Here the failure path must be reachable as well.
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
