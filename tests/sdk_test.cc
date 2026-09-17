/**
 * @file sdk_test.cc
 * @brief Self-contained checks for the header-only helpers in 42u/sdk.hpp; no test framework.
 *
 * 42u/sdk.hpp is header-only and 42u/abi.hpp only declares the factory entry point, so this file
 * owns its main() and links against nothing but the standard library - no src/ translation unit
 * and no plugin library is needed:
 *   g++ -std=c++17 -Wall -Wextra -Werror -Iinc tests/sdk_test.cc -o build/invoke-v2/sdk/sdk_test
 *
 * The checks cover both layers that can regress silently:
 *   - Runtime behaviour of query/view/string_writer and of the credential-only lease RAII.
 *   - Compile-time shape proofs: the lease exposes no get(), no operator-> and no business
 *     pointer at all, and abi::v2::borrow is exactly one 8-byte credential.
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
#include <type_traits>
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

/** @brief A business type the SDK must never hand out; only used for negative shape checks. */
struct fake_iface {
    int value = 0;
};

/**
 * @brief The two business protocol constants this test drives the fake host service with.
 *
 * They mirror examples/echo.hpp: a plugin-wide contract is an id plus a major/minor pair, and
 * nothing about it is an object pointer.
 */
inline constexpr abi::v2::contract echo_protocol{{0x6563686f34325532ULL, 1}, 1, 0};
inline constexpr abi::v2::contract echo_protocol_minor2{{0x6563686f34325532ULL, 1}, 1, 2};
inline constexpr abi::v2::contract echo_protocol_major2{{0x6563686f34325532ULL, 1}, 2, 0};
inline constexpr abi::v2::contract other_protocol{{0x636f6e7334325532ULL, 1}, 1, 0};
inline constexpr abi::v2::contract zero_id_protocol{{0, 0}, 1, 0};

/**
 * @brief Compare two business contracts field by field.
 *
 * The ABI freezes the contract fields, not an operator==; tests need the comparison spelled out so
 * a wrong field cannot hide behind a defaulted one.
 *
 * @param left First contract to compare.
 * @param right Second contract to compare.
 * @return True only when identity, major and minor all match.
 */
constexpr bool same_contract(abi::v2::contract left, abi::v2::contract right) noexcept
{
    return left.id == right.id && left.major == right.major && left.minor == right.minor;
}

// The contract helpers the v2 ABI ships are constexpr; pin the exact rules this suite relies on.
static_assert(abi::v2::abi_major == 2, "the SDK helper layer targets ABI v2 only");
static_assert(abi::v2::valid_contract(echo_protocol), "a named, versioned protocol is valid");
static_assert(!abi::v2::valid_contract(abi::v2::contract{}), "an empty contract is not usable");
static_assert(!abi::v2::valid_contract(zero_id_protocol), "a zero identity is not usable");
static_assert(abi::v2::compatible_contract(echo_protocol, echo_protocol),
              "a provider satisfies an identical requirement");
static_assert(!abi::v2::compatible_contract(echo_protocol, echo_protocol_minor2),
              "an insufficient offered minor must be rejected");
static_assert(!abi::v2::compatible_contract(echo_protocol, echo_protocol_major2),
              "a different major must be rejected");
static_assert(!abi::v2::compatible_contract(echo_protocol, other_protocol),
              "a different protocol family must be rejected");

/**
 * @brief Detects a callable member function named get() on T.
 * @tparam T Type to inspect; never instantiated, only used in an unevaluated context.
 */
template <class T, class = void>
struct has_get : std::false_type {};
template <class T>
struct has_get<T, std::void_t<decltype(std::declval<const T&>().get())>> : std::true_type {};

/** @brief Detects a member operator-> on T. */
template <class T, class = void>
struct has_operator_arrow : std::false_type {};
template <class T>
struct has_operator_arrow<T, std::void_t<decltype(std::declval<const T&>().operator->())>>
    : std::true_type {};

/** @brief Detects a dereference operator (*value) on T. */
template <class T, class = void>
struct has_operator_star : std::false_type {};
template <class T>
struct has_operator_star<T, std::void_t<decltype(*std::declval<const T&>())>> : std::true_type {};

/**
 * @brief Pointer-shaped stand-in the detectors above must recognize.
 *
 * Without this positive control a detector that always reported false would make the lease
 * assertions pass for the wrong reason.
 */
struct pointer_shaped_iface {
    fake_iface* pointer = nullptr;

    fake_iface* get() const noexcept { return pointer; }
    fake_iface* operator->() const noexcept { return pointer; }
    fake_iface& operator*() const noexcept { return *pointer; }
};

static_assert(has_get<pointer_shaped_iface>::value, "detector must see get()");
static_assert(has_operator_arrow<pointer_shaped_iface>::value, "detector must see operator->");
static_assert(has_operator_star<pointer_shaped_iface>::value, "detector must see operator*");

/**
 * @brief Detects whether T can be aggregate-initialized from two credentials.
 *
 * A type that stores only one credential cannot absorb two, so this is the negative half of the
 * "borrow is token-only" proof below; pointer_shaped_two_tokens is the positive control.
 */
template <class T, class = void>
struct accepts_two_tokens : std::false_type {};
template <class T>
struct accepts_two_tokens<T, std::void_t<decltype(T{abi::v2::token{}, abi::v2::token{}})>>
    : std::true_type {};

/** @brief Stand-in with two credential fields, proving accepts_two_tokens is not always false. */
struct two_token_stand_in {
    abi::v2::token first{};
    abi::v2::token second{};
};
static_assert(accepts_two_tokens<two_token_stand_in>::value,
              "detector must accept a two-token aggregate");

/**
 * @brief Compile-time proof that abi::v2::borrow holds exactly one member: the credential.
 *
 * A structured binding only compiles when the declared names match the aggregate's member count,
 * so this stops compiling the moment borrow grows a second field - for example the business
 * pointer an earlier ABI shape carried.
 *
 * @return True when the single bound name really is the credential.
 */
constexpr bool borrow_binds_to_a_single_credential() noexcept
{
    const abi::v2::borrow value{abi::v2::token{7}};
    const auto& [credential] = value;
    return credential.value == 7;
}
static_assert(borrow_binds_to_a_single_credential(),
              "borrow must expose exactly one member, the credential");
static_assert(sizeof(abi::v2::borrow) == 8 && alignof(abi::v2::borrow) == 8,
              "borrow must stay an 8-byte, 8-aligned value");
static_assert(sizeof(abi::v2::borrow) == sizeof(abi::v2::token),
              "borrow must be no wider than its credential");
static_assert(!accepts_two_tokens<abi::v2::borrow>::value,
              "borrow must not absorb a second token");
static_assert(std::is_standard_layout_v<abi::v2::borrow> &&
                  std::is_trivially_copyable_v<abi::v2::borrow>,
              "borrow must stay a plain ABI value");

/**
 * @brief Fake icaps that records borrows and returns instead of revoking asynchronously.
 *
 * The real service cannot be produced inside this test binary, and these checks only care about
 * the protocol/credential bookkeeping the sdk helpers perform on top of it.
 */
struct fake_caps final : abi::v2::icaps {
    /** @brief Record the announced capability set without interpreting it. */
    abi::v2::status U42_CALL announce(const abi::v2::caps_desc* value) noexcept override
    {
        ++announce_count;
        last_desc = value != nullptr ? *value : abi::v2::caps_desc{};
        return announce_status;
    }
    abi::v2::status U42_CALL watch(abi::v2::icap_sink*, abi::v2::token*) noexcept override
    {
        return abi::v2::unsupported;
    }
    abi::v2::status U42_CALL unwatch(abi::v2::token) noexcept override
    {
        return abi::v2::unsupported;
    }

    /**
     * @brief Issue the configured credential for the required contract and remember the receiver.
     *
     * The output is cleared first, exactly as the ABI requires, and only a credential is ever
     * written: there is no interface pointer for this service to hand out.
     */
    abi::v2::status U42_CALL acquire(const char* plug_id, const abi::v2::contract* required,
                                     abi::v2::irevoker* receiver,
                                     abi::v2::borrow* out) noexcept override
    {
        ++acquire_count;
        last_plug_id = plug_id;
        last_required = required != nullptr ? *required : abi::v2::contract{};
        last_receiver = receiver;
        if (out != nullptr) *out = abi::v2::borrow{};
        if (out != nullptr && acquire_status == abi::v2::ok) {
            *out = abi::v2::borrow{next_credential};
        }
        return acquire_status;
    }

    /** @brief Record one returned credential and report the configured status. */
    abi::v2::status U42_CALL release(abi::v2::token credential) noexcept override
    {
        ++release_count;
        last_released = credential;
        return next_release;
    }

    abi::v2::token next_credential{42};          //!< Credential acquire() issues next.
    abi::v2::status next_release = abi::v2::ok;  //!< Status release() reports next.
    abi::v2::status acquire_status = abi::v2::ok; //!< Status acquire() reports next.
    abi::v2::status announce_status = abi::v2::ok; //!< Status announce() reports next.
    int acquire_count = 0;                       //!< Number of acquire() calls.
    int release_count = 0;                       //!< Number of release() calls.
    int announce_count = 0;                      //!< Number of announce() calls.
    abi::v2::token last_released{};              //!< Credential passed to the last release().
    abi::v2::irevoker* last_receiver = nullptr;  //!< Revocation target registered by acquire().
    const char* last_plug_id = nullptr;          //!< Plugin identity passed to acquire().
    abi::v2::contract last_required{};           //!< Contract passed to the last acquire().
    abi::v2::caps_desc last_desc{};              //!< Copy of the last announced capability set.
};

/**
 * @brief Fake ictx whose query() writes its output on both paths.
 *
 * Writing even on failure is deliberate: it pins that sdk::query() clears the caller's output
 * itself instead of trusting a failing host to leave it alone.
 */
struct fake_ictx final : abi::v2::ictx {
    abi::v2::status next_status = abi::v2::ok;  //!< Status the next query() reports.
    void* next_value = nullptr;                 //!< Value the next query() stores.
    int query_count = 0;                        //!< Number of query() calls seen.
    abi::v2::iid last_type{};                   //!< Identifier the last query() received.

    abi::v2::status U42_CALL query(const abi::v2::iid* type, void** out) noexcept override
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
struct revoking_owner final : abi::v2::irevoker {
    sdk::lease* held = nullptr;  //!< Lease this owner keeps caching.
    int revocations = 0;         //!< Number of on_revoke() deliveries.

    /** @brief Release the lease only when the revoked credential is the one it still holds. */
    void U42_CALL on_revoke(abi::v2::token credential) noexcept override
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

    abi::v2::icaps* out = nullptr;
    CHECK(sdk::query<abi::v2::icaps>(&ctx, abi::v2::caps_iid, &out) == abi::v2::ok);
    CHECK(out == &caps);
    CHECK(ctx.query_count == 1);
    CHECK(ctx.last_type == abi::v2::caps_iid);

    // The identifier is passed through verbatim; the helper must not substitute a default.
    out = nullptr;
    CHECK(sdk::query<abi::v2::icaps>(&ctx, abi::v2::diag_iid, &out) == abi::v2::ok);
    CHECK(ctx.last_type == abi::v2::diag_iid);
    CHECK(ctx.query_count == 2);

    // A host that reports ok with a null pointer yields an empty output, never a fabricated one.
    ctx.next_value = nullptr;
    out = &caps;
    CHECK(sdk::query<abi::v2::icaps>(&ctx, abi::v2::caps_iid, &out) == abi::v2::ok);
    CHECK(out == nullptr);
}

TEST_CASE(query_failure_clears_output)
{
    fake_caps caps;
    fake_ictx ctx;
    ctx.next_value = &caps;
    ctx.next_status = abi::v2::not_found;

    abi::v2::icaps* out = &caps; // deliberately dirty: a failure must clear it
    CHECK(sdk::query<abi::v2::icaps>(&ctx, abi::v2::caps_iid, &out) == abi::v2::not_found);
    CHECK(out == nullptr);
    CHECK(ctx.query_count == 1);

    // Null context: rejected before any dereference, output still cleared.
    out = &caps;
    CHECK(sdk::query<abi::v2::icaps>(nullptr, abi::v2::caps_iid, &out) ==
          abi::v2::invalid_argument);
    CHECK(out == nullptr);
    CHECK(ctx.query_count == 1);

    // Null output has nowhere to report anything, so it is rejected without calling the host.
    CHECK(sdk::query<abi::v2::icaps>(&ctx, abi::v2::caps_iid, nullptr) ==
          abi::v2::invalid_argument);
    CHECK(ctx.query_count == 1);
}

TEST_CASE(lease_move_transfers_ownership_once)
{
    fake_caps caps;
    caps.next_credential = abi::v2::token{42};
    abi::v2::borrow borrowed{};
    CHECK(caps.acquire("demo", &echo_protocol, nullptr, &borrowed) == abi::v2::ok);
    // The whole borrow: a credential. There is no pointer for a lease to carry around.
    CHECK(borrowed.credential.value == 42);
    CHECK(same_contract(caps.last_required, echo_protocol));

    {
        sdk::lease owner(&caps, borrowed);
        {
            sdk::lease moved(std::move(owner));
            CHECK(!owner);
            CHECK(owner.credential().value == 0);
            CHECK(static_cast<bool>(moved));
            CHECK(moved.credential().value == 42);
            CHECK(moved.matches(abi::v2::token{42}));
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
    abi::v2::borrow first{};
    caps.next_credential = abi::v2::token{7};
    CHECK(caps.acquire("demo", &echo_protocol, nullptr, &first) == abi::v2::ok);
    abi::v2::borrow second{};
    caps.next_credential = abi::v2::token{9};
    CHECK(caps.acquire("demo", &echo_protocol, nullptr, &second) == abi::v2::ok);

    {
        sdk::lease target(&caps, first);
        sdk::lease source(&caps, second);
        target = std::move(source);

        // The credential target already held is returned before source's borrow is adopted.
        CHECK(caps.release_count == 1);
        CHECK(caps.last_released.value == 7);
        CHECK(target.credential().value == 9);
        CHECK(target.matches(abi::v2::token{9}));
        CHECK(!source);
        CHECK(source.credential().value == 0);

        // Assigning an empty lease still returns the credential the target held.
        sdk::lease blank;
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
    caps.next_credential = abi::v2::token{3};
    caps.next_release = abi::v2::busy;
    abi::v2::borrow borrowed{};
    CHECK(caps.acquire("demo", &echo_protocol, nullptr, &borrowed) == abi::v2::ok);

    {
        sdk::lease held(&caps, borrowed);
        CHECK(held.reset() == abi::v2::busy); // the refusal is reported, not swallowed
        CHECK(caps.release_count == 1);
        CHECK(caps.last_released.value == 3);
        // A busy release is retryable: the lease must still own the credential, otherwise the
        // borrow the host still tracks would be silently lost.
        CHECK(static_cast<bool>(held));
        CHECK(held.credential().value == 3);
        CHECK(held.matches(abi::v2::token{3}));

        // The control thread retries once the host can accept the return.
        caps.next_release = abi::v2::ok;
        CHECK(held.reset() == abi::v2::ok);
        CHECK(caps.release_count == 2);
        CHECK(caps.last_released.value == 3);
        CHECK(held.credential().value == 0);
        CHECK(!held);

        // Idempotent: the cleared lease has nothing left to return.
        CHECK(held.reset() == abi::v2::ok);
        CHECK(caps.release_count == 2);
    }
    // The destructor of the already reset lease returns nothing either.
    CHECK(caps.release_count == 2);
}

TEST_CASE(lease_destructor_attempts_one_final_release)
{
    fake_caps caps;
    caps.next_credential = abi::v2::token{11};
    caps.next_release = abi::v2::wrong_thread;

    abi::v2::borrow borrowed{};
    CHECK(caps.acquire("demo", &echo_protocol, nullptr, &borrowed) == abi::v2::ok);

    {
        sdk::lease held(&caps, borrowed);
        CHECK(held.reset() == abi::v2::wrong_thread);
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
    caps.next_credential = abi::v2::token{21};
    caps.next_release = abi::v2::stale;
    abi::v2::borrow borrowed{};
    CHECK(caps.acquire("demo", &echo_protocol, nullptr, &borrowed) == abi::v2::ok);

    sdk::lease held(&caps, borrowed);
    // A stale credential already belongs to a newer record, so it can never be returned again.
    CHECK(held.reset() == abi::v2::stale);
    CHECK(caps.release_count == 1);
    CHECK(caps.last_released.value == 21);
    CHECK(!held);
    CHECK(held.credential().value == 0);
    CHECK(!held.matches(abi::v2::token{21}));
    CHECK(held.reset() == abi::v2::ok); // nothing left to return
    CHECK(caps.release_count == 1);
}

TEST_CASE(lease_move_assignment_refusal_keeps_both_sides_owned)
{
    fake_caps caps;
    abi::v2::borrow first{};
    caps.next_credential = abi::v2::token{31};
    CHECK(caps.acquire("demo", &echo_protocol, nullptr, &first) == abi::v2::ok);
    abi::v2::borrow second{};
    caps.next_credential = abi::v2::token{32};
    CHECK(caps.acquire("demo", &echo_protocol, nullptr, &second) == abi::v2::ok);

    caps.next_release = abi::v2::failed;
    sdk::lease target(&caps, first);
    sdk::lease source(&caps, second);

    target = std::move(source);
    // The assignment could not return target's old credential, so it must not have moved the
    // borrow either: both sides keep exactly what they owned and nothing is silently dropped.
    CHECK(caps.release_count == 1);
    CHECK(caps.last_released.value == 31);
    CHECK(target.credential().value == 31);
    CHECK(source.credential().value == 32);
    CHECK(static_cast<bool>(target));
    CHECK(static_cast<bool>(source));

    // Retrying the same assignment once the host accepts the return completes the transfer.
    caps.next_release = abi::v2::ok;
    target = std::move(source);
    CHECK(caps.release_count == 2);
    CHECK(caps.last_released.value == 31);
    CHECK(target.credential().value == 32);
    CHECK(!source);
    CHECK(source.credential().value == 0);
}

TEST_CASE(lease_without_owner_cannot_return_credential)
{
    // Without an icaps there is no release to call and no ok/stale to observe, so ownership must
    // survive: an unstoppable credential is reported instead of being dropped as if returned.
    sdk::lease orphan(nullptr, abi::v2::borrow{abi::v2::token{5}});
    CHECK(static_cast<bool>(orphan));
    CHECK(orphan.credential().value == 5);
    CHECK(orphan.matches(abi::v2::token{5}));
    CHECK(orphan.reset() == abi::v2::invalid_state);
    CHECK(static_cast<bool>(orphan));
    CHECK(orphan.credential().value == 5);
    CHECK(orphan.reset() == abi::v2::invalid_state);

    sdk::lease empty;
    CHECK(!empty);
    CHECK(empty.reset() == abi::v2::ok);
    CHECK(!empty.matches(abi::v2::token{0}));

    // A zero credential is not a borrow at all: the host tracks nothing, so reset() reports ok and
    // never calls release().
    fake_caps caps;
    sdk::lease credentialless(&caps, abi::v2::borrow{abi::v2::token{}});
    CHECK(!credentialless);
    CHECK(credentialless.reset() == abi::v2::ok);
    CHECK(caps.release_count == 0);
}

TEST_CASE(lease_matches_credential_for_explicit_revocation)
{
    fake_caps caps;
    caps.next_credential = abi::v2::token{77};
    revoking_owner owner;
    abi::v2::borrow borrowed{};
    CHECK(caps.acquire("demo", &echo_protocol, &owner, &borrowed) == abi::v2::ok);
    // The registered target is the stable owner, never the movable lease itself.
    CHECK(caps.last_receiver == &owner);
    CHECK(caps.last_plug_id != nullptr);
    CHECK(same_contract(caps.last_required, echo_protocol));
    CHECK(owner.revocations == 0);

    sdk::lease held(&caps, borrowed);
    owner.held = &held;

    CHECK(held.matches(abi::v2::token{77}));
    CHECK(!held.matches(abi::v2::token{78}));
    CHECK(!held.matches(abi::v2::token{0}));

    // A revocation for a different credential leaves this lease intact.
    owner.on_revoke(abi::v2::token{78});
    CHECK(caps.release_count == 0);
    CHECK(static_cast<bool>(held));

    // A revoker that runs while the host refuses the return keeps the lease owning the borrow,
    // so the same revocation can be replayed on the control thread until release() accepts it.
    caps.next_release = abi::v2::busy;
    owner.on_revoke(abi::v2::token{77});
    CHECK(caps.release_count == 1);
    CHECK(held.matches(abi::v2::token{77}));
    caps.next_release = abi::v2::ok;

    // The matching revocation returns the credential and empties the lease.
    owner.on_revoke(abi::v2::token{77});
    CHECK(caps.release_count == 2);
    CHECK(caps.last_released.value == 77);
    CHECK(!held);
    CHECK(!held.matches(abi::v2::token{77}));

    // Replaying the same revocation cannot release twice.
    owner.on_revoke(abi::v2::token{77});
    CHECK(owner.revocations == 4);
    CHECK(caps.release_count == 2);

    owner.held = nullptr;
    CHECK(caps.release_count == 2);
}

TEST_CASE(lease_is_not_a_dereferenceable_business_pointer)
{
    // Compile-time proof: an accidental typed lease cannot come back without breaking this build.
    // The detectors are validated by the pointer_shaped_iface positive control above, so these
    // negatives cannot pass merely because detection is broken.
    static_assert(!has_get<sdk::lease>::value, "lease must not expose get()");
    static_assert(!has_operator_arrow<sdk::lease>::value, "lease must not expose operator->");
    static_assert(!has_operator_star<sdk::lease>::value, "lease must not be dereferenceable");
    static_assert(!std::is_pointer_v<sdk::lease>, "lease is an RAII handle, not a pointer");
    static_assert(!std::is_convertible_v<sdk::lease, void*>,
                  "lease must not decay to a raw pointer");
    static_assert(!std::is_constructible_v<sdk::lease, fake_iface*>,
                  "no typed lease shortcut exists");
    static_assert(!std::is_constructible_v<sdk::lease, abi::v2::icaps*, fake_iface*>,
                  "a lease must never adopt a business interface pointer");
    // Two words at most: the owning icaps plus the credential. There is no room for a business
    // pointer, which is the property this migration is about.
    static_assert(sizeof(sdk::lease) == 2 * sizeof(void*),
                  "lease must store only icaps* + credential");
    static_assert(!std::is_copy_constructible_v<sdk::lease> &&
                      !std::is_copy_assignable_v<sdk::lease>,
                  "a credential is owned by exactly one lease");
    static_assert(std::is_move_constructible_v<sdk::lease> &&
                      std::is_move_assignable_v<sdk::lease>,
                  "a lease must still transfer ownership");
    // sdk::lease is used above without template arguments: a typed lease<...> no longer exists.
    static_assert(std::is_class_v<sdk::lease>, "lease is the non-template RAII handle");

    fake_caps caps;
    sdk::lease held(&caps, abi::v2::borrow{abi::v2::token{64}});
    CHECK(static_cast<bool>(held));
    CHECK(held.credential().value == 64);
    CHECK(caps.release_count == 0);
    CHECK(held.reset() == abi::v2::ok);
    CHECK(caps.release_count == 1);
}

TEST_CASE(borrow_is_an_eight_byte_credential_only)
{
    static_assert(sizeof(abi::v2::borrow) == 8 && alignof(abi::v2::borrow) == 8,
                  "borrow must stay an 8-byte, 8-aligned value");
    static_assert(sizeof(abi::v2::borrow) == sizeof(abi::v2::token),
                  "borrow must be no wider than its credential");
    static_assert(std::is_same_v<decltype(abi::v2::borrow{}.credential), abi::v2::token>,
                  "borrow's single member must be the credential token");
    static_assert(!accepts_two_tokens<abi::v2::borrow>::value,
                  "borrow must not absorb a second token");
    static_assert(borrow_binds_to_a_single_credential(),
                  "borrow must expose exactly one member");

    const abi::v2::borrow value{abi::v2::token{7}};
    CHECK(value.credential.value == 7);

    // acquire() hands out the same credential-only value; the lease can then be built from it.
    fake_caps caps;
    caps.next_credential = abi::v2::token{13};
    abi::v2::borrow issued{};
    CHECK(caps.acquire("demo", &echo_protocol, nullptr, &issued) == abi::v2::ok);
    CHECK(issued.credential.value == 13);
    sdk::lease held(&caps, issued);
    CHECK(held.credential().value == 13);
    CHECK(held.reset() == abi::v2::ok);
    CHECK(caps.last_released.value == 13);
}

TEST_CASE(fake_icaps_carries_only_v2_contracts_and_caps_desc)
{
    static_assert(sizeof(abi::v2::caps_desc) == 40 && offsetof(abi::v2::caps_desc, protocol) == 16,
                  "caps_desc must expose only counts, methods and a protocol");
    static_assert(sizeof(abi::v2::contract) == 24 && offsetof(abi::v2::contract, minor) == 20,
                  "contract must stay id + major + minor");

    static const abi::v2::method_desc methods[] = {
        {1, "echo", "repeat the payload", "{}", "{}"},
    };
    abi::v2::caps_desc desc{};
    desc.method_count = 1;
    desc.methods = methods;
    desc.protocol = echo_protocol;

    fake_caps caps;
    CHECK(caps.announce(&desc) == abi::v2::ok);
    CHECK(caps.announce_count == 1);
    CHECK(caps.last_desc.method_count == 1);
    CHECK(caps.last_desc.methods == methods);
    CHECK(same_contract(caps.last_desc.protocol, echo_protocol));

    // acquire() is protocol-based now: the required contract is passed through verbatim, and the
    // issued borrow still carries nothing but a credential.
    abi::v2::borrow borrowed{};
    CHECK(caps.acquire("echo", &echo_protocol, nullptr, &borrowed) == abi::v2::ok);
    CHECK(same_contract(caps.last_required, echo_protocol));
    CHECK(caps.last_plug_id != nullptr);
    CHECK(borrowed.credential.value != 0);

    // A refused lease is refused by contract compatibility, and the fake mirrors the ABI rule of
    // clearing a legal output first: no credential survives a failed acquire.
    caps.acquire_status = abi::v2::unsupported;
    abi::v2::borrow refused{abi::v2::token{99}};
    CHECK(caps.acquire("echo", &echo_protocol_minor2, nullptr, &refused) == abi::v2::unsupported);
    CHECK(same_contract(caps.last_required, echo_protocol_minor2));
    CHECK(refused.credential.value == 0);
    sdk::lease none(&caps, refused);
    CHECK(!none);
}

TEST_CASE(view_borrows_string_data)
{
    const abi::v2::bytes none = sdk::view(std::string_view{});
    CHECK(none.data == nullptr);
    CHECK(none.size == 0);

    const std::string text = "hello";
    const abi::v2::bytes raw = sdk::view(std::string_view{text});
    CHECK(raw.data == text.data()); // same address: the view is borrowed, not copied
    CHECK(raw.size == 5u);

    // A view with a non-null but empty range still normalizes to {nullptr, 0}, because the ABI
    // requires a null pointer to carry size zero.
    const std::string_view empty_tail(text.data() + text.size(), 0);
    CHECK(empty_tail.data() != nullptr);
    const abi::v2::bytes tail = sdk::view(empty_tail);
    CHECK(tail.data == nullptr);
    CHECK(tail.size == 0);
}

TEST_CASE(writer_appends_through_the_abi_interface)
{
    std::string out;
    sdk::string_writer writer(&out, 8);
    abi::v2::iwriter* abi_writer = &writer; // the ABI only ever sees iwriter*

    CHECK(abi_writer->write(sdk::view("ab")) == abi::v2::ok);
    CHECK(out == "ab");
    CHECK(abi_writer->write(sdk::view("cd")) == abi::v2::ok);
    CHECK(out == "abcd");
    CHECK(writer.status() == abi::v2::ok);

    // A zero-length chunk is legal even with a null pointer and must change nothing.
    CHECK(abi_writer->write(abi::v2::bytes{nullptr, 0}) == abi::v2::ok);
    CHECK(out == "abcd");
}

TEST_CASE(writer_rejects_over_limit_atomically_and_stickily)
{
    std::string out;
    sdk::string_writer writer(&out, 4);
    CHECK(writer.write(sdk::view("abcde")) == abi::v2::limit_exceeded);
    CHECK(out.empty()); // no truncated prefix is published
    CHECK(writer.status() == abi::v2::limit_exceeded);
    CHECK(writer.write(sdk::view("ab")) == abi::v2::limit_exceeded); // sticky
    CHECK(out.empty());

    // The cap counts the whole target: filling it exactly is allowed, one byte more is not.
    std::string exact;
    sdk::string_writer cumulative(&exact, 4);
    CHECK(cumulative.write(sdk::view("abcd")) == abi::v2::ok);
    CHECK(cumulative.write(sdk::view("e")) == abi::v2::limit_exceeded);
    CHECK(exact == "abcd");
    CHECK(cumulative.status() == abi::v2::limit_exceeded);

    // A target that already exceeds the cap must not become writable: the remaining room must
    // not underflow to a huge value.
    std::string over = "abcdef";
    sdk::string_writer clamped(&over, 2);
    CHECK(clamped.write(abi::v2::bytes{nullptr, 0}) == abi::v2::ok); // zero bytes never exceed
    CHECK(over == "abcdef");
    CHECK(clamped.write(sdk::view("g")) == abi::v2::limit_exceeded);
    CHECK(over == "abcdef");
}

TEST_CASE(writer_rejects_null_target_and_malformed_bytes)
{
    sdk::string_writer detached(nullptr, 16);
    CHECK(detached.write(sdk::view("x")) == abi::v2::invalid_argument);
    CHECK(detached.status() == abi::v2::invalid_argument);
    CHECK(detached.write(abi::v2::bytes{nullptr, 0}) == abi::v2::invalid_argument); // sticky

    std::string out;
    sdk::string_writer writer(&out, 8);
    CHECK(writer.write(abi::v2::bytes{nullptr, 3}) == abi::v2::invalid_argument);
    CHECK(out.empty());
    CHECK(writer.status() == abi::v2::invalid_argument);
    CHECK(writer.write(sdk::view("ok")) == abi::v2::invalid_argument); // sticky
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
    CHECK(writer.write(abi::v2::bytes{&marker, std::numeric_limits<std::uint64_t>::max()}) ==
          abi::v2::failed);
    CHECK(writer.status() == abi::v2::failed);
    CHECK(out.empty());
    CHECK(writer.write(sdk::view("small")) == abi::v2::failed); // sticky
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
