/**
 * @file sdk_test.cc
 * @brief Self-contained checks for the header-only helpers in 42u/sdk.hpp; no test framework.
 *
 * 42u/sdk.hpp is header-only and 42u/abi.hpp only declares the factory entry point, so this file
 * owns its main() and links against nothing but the standard library - no src/ translation unit
 * and no plugin library is needed:
 *   g++ -std=c++17 -Wall -Wextra -Werror -Iinc tests/sdk_test.cc -o build/invoke-v3/sdk/sdk_test
 *
 * The checks cover both layers that can regress silently:
 *   - Runtime behaviour of query/view/string_writer and of the version-qualified lease RAII.
 *   - Compile-time shape proofs: the lease exposes no get(), no operator-> and no business
 *     pointer at all, and abi::borrow is exactly the 24-byte {credential, version} pair.
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

namespace abi = u42::abi::v3; // v3 is the only ABI this helper layer targets; no v1/v2 alias
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
 * @brief Plugin versions and the acceptance ranges this test drives the fake host service with.
 *
 * A business compatibility promise is now the provider's plugin_version plus the closed range the
 * borrower explicitly supplies, mirroring examples/echo.hpp: echo_versions accepts the whole 1.x
 * line, while echo_version is the version the fake provider actually grants.
 */
inline constexpr abi::plugin_version echo_version{1, 0, 0};
inline constexpr abi::plugin_version echo_version_newer{1, 2, 0};
inline constexpr abi::plugin_version echo_version_1_10{1, 10, 0};
inline constexpr abi::plugin_version echo_version_top{
    1, std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint32_t>::max()};
inline constexpr abi::plugin_version echo_version_2{2, 0, 0};
inline constexpr abi::plugin_version max_version{std::numeric_limits<std::uint32_t>::max(),
                                                 std::numeric_limits<std::uint32_t>::max(),
                                                 std::numeric_limits<std::uint32_t>::max()};
inline constexpr abi::version_range echo_versions{echo_version, echo_version_top};
inline constexpr abi::version_range echo_exact = abi::exact_version(echo_version);

/**
 * @brief Compare two plugin versions field by field.
 *
 * The ABI freezes the plugin_version fields, not an operator==; tests need the comparison spelled
 * out so a wrong field cannot hide behind a defaulted operator.
 *
 * @param left First version to compare.
 * @param right Second version to compare.
 * @return True only when major, minor and patch all match.
 */
constexpr bool same_version(abi::plugin_version left, abi::plugin_version right) noexcept
{
    return left.major == right.major && left.minor == right.minor && left.patch == right.patch;
}

/**
 * @brief Compare two inclusive version ranges by both endpoints.
 *
 * @param left First range to compare.
 * @param right Second range to compare.
 * @return True only when minimum and maximum match field for field.
 */
constexpr bool same_range(abi::version_range left, abi::version_range right) noexcept
{
    return same_version(left.minimum, right.minimum) && same_version(left.maximum, right.maximum);
}

// The version helpers the v3 ABI ships are constexpr; pin the exact rules this suite relies on.
static_assert(abi::abi_major == 3, "the SDK helper layer targets ABI v3 only");
static_assert(echo_version == abi::plugin_version{1, 0, 0}, "equality compares all three fields");
static_assert(echo_version != echo_version_newer, "a different minor/patch is not equal");
static_assert(abi::version_less(echo_version_newer, echo_version_1_10),
              "versions compare numerically, so 1.2.0 precedes 1.10.0");
static_assert(!abi::version_less(echo_version_1_10, echo_version_newer),
              "the numeric order is strict and antisymmetric");
static_assert(abi::valid_version_range(echo_versions), "minimum <= maximum is a valid range");
static_assert(abi::valid_version_range(echo_exact), "an exact version is a valid closed range");
static_assert(!abi::valid_version_range(abi::version_range{echo_version_2, echo_version}),
              "a reversed range is invalid");
static_assert(abi::accepts_version(echo_versions, echo_version),
              "the lower closed bound is included");
static_assert(abi::accepts_version(echo_versions, echo_version_top),
              "the upper closed bound is included");
static_assert(!abi::accepts_version(echo_versions, abi::plugin_version{0, UINT32_MAX, UINT32_MAX}),
              "a version just below the lower bound is rejected");
static_assert(!abi::accepts_version(echo_versions, echo_version_2),
              "a version above the upper bound is rejected");
static_assert(abi::accepts_version(echo_exact, echo_version),
              "an exact range accepts its own version");
static_assert(!abi::accepts_version(echo_exact, abi::plugin_version{1, 0, 1}),
              "an exact range rejects any other version");
static_assert(abi::accepts_version(abi::version_range{echo_version, echo_version_2},
                                   echo_version_2),
              "a cross-major range passes only when the caller explicitly supplies it");
static_assert(!abi::accepts_version(abi::version_range{echo_version, echo_version},
                                    echo_version_2),
              "hosts never infer a wider compatibility range on the caller's behalf");
static_assert(abi::accepts_version(abi::exact_version(abi::plugin_version{0, 0, 0}),
                                   abi::plugin_version{0, 0, 0}),
              "0.0.0 is a legal version inside a legal range");
static_assert(!abi::accepts_version(echo_versions, abi::plugin_version{0, 0, 0}),
              "0.0.0 is not accepted when it falls outside the requested range");
static_assert(!abi::valid_version_range(abi::version_range{max_version, echo_version}),
              "uint32-max endpoints cannot wrap into a valid range");

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
 * A type whose second member is a version cannot absorb two tokens, so this is the negative half
 * of the "borrow's second field is a version" proof below; two_token_stand_in is the positive
 * control.
 */
template <class T, class = void>
struct accepts_two_tokens : std::false_type {};
template <class T>
struct accepts_two_tokens<T, std::void_t<decltype(T{abi::token{}, abi::token{}})>> : std::true_type {};

/** @brief Stand-in with two credential fields, proving accepts_two_tokens is not always false. */
struct two_token_stand_in {
    abi::token first{};
    abi::token second{};
};
static_assert(accepts_two_tokens<two_token_stand_in>::value,
              "detector must accept a two-token aggregate");

/**
 * @brief Detects whether T can be aggregate-initialized from a credential and a version.
 *
 * @tparam T Type to inspect; never instantiated, only used in an unevaluated context.
 */
template <class T, class = void>
struct accepts_token_and_version : std::false_type {};
template <class T>
struct accepts_token_and_version<T, std::void_t<decltype(T{abi::token{}, abi::plugin_version{}})>> :
    std::true_type {};

/**
 * @brief Compile-time proof that abi::borrow holds exactly two members: credential and version.
 *
 * A structured binding only compiles when the declared names match the aggregate's member count,
 * so this stops compiling if borrow grows or loses a field - for example the token-only shape an
 * earlier ABI carried.
 *
 * @return True when the two bound names really are the credential and the granted version.
 */
constexpr bool borrow_binds_to_credential_and_version() noexcept
{
    const abi::borrow value{abi::token{7}, abi::plugin_version{1, 2, 3}};
    const auto& [credential, version] = value;
    return credential.value == 7 && version.major == 1 && version.minor == 2 && version.patch == 3;
}
static_assert(borrow_binds_to_credential_and_version(),
              "borrow must expose exactly two members, credential and version");
static_assert(sizeof(abi::borrow) == 24 && alignof(abi::borrow) == 8,
              "borrow must stay the 24-byte {credential, version} pair");
static_assert(offsetof(abi::borrow, version) == 8, "the version must follow the credential");
static_assert(accepts_token_and_version<abi::borrow>::value,
              "borrow must accept a credential plus a version");
static_assert(!accepts_two_tokens<abi::borrow>::value,
              "borrow's second member must be a version, not another credential");
static_assert(std::is_same_v<decltype(abi::borrow{}.credential), abi::token>,
              "the first borrow member must be the credential token");
static_assert(std::is_same_v<decltype(abi::borrow{}.version), abi::plugin_version>,
              "the second borrow member must be the plugin version");
static_assert(std::is_standard_layout_v<abi::borrow> &&
                  std::is_trivially_copyable_v<abi::borrow>,
              "borrow must stay a plain ABI value");

/**
 * @brief Fake icaps that records borrows and returns instead of revoking asynchronously.
 *
 * The real service cannot be produced inside this test binary, and these checks only care about
 * the credential/version and ownership bookkeeping the sdk helpers perform on top of it.
 */
struct fake_caps final : abi::icaps {
    /** @brief Record the announced capability set without interpreting it. */
    abi::status U42_CALL announce(const abi::caps_desc* value) noexcept override
    {
        ++announce_count;
        last_desc = value != nullptr ? *value : abi::caps_desc{};
        return announce_status;
    }
    abi::status U42_CALL watch(abi::icap_sink*, abi::token*) noexcept override
    {
        return abi::unsupported;
    }
    abi::status U42_CALL unwatch(abi::token) noexcept override
    {
        return abi::unsupported;
    }

    /**
     * @brief Issue the configured credential/version pair for the requested closed range.
     *
     * The output is cleared first, exactly as the ABI requires, and failure leaves the whole borrow
     * zeroed: a version alone cannot indicate success because success also needs a non-zero
     * credential. A range that does not accept grant_version is refused with unsupported, mirroring
     * the ABI rule that a version mismatch creates no lease at all.
     */
    abi::status U42_CALL acquire(const char* plug_id, const abi::version_range* allowed,
                                 abi::irevoker* receiver, abi::borrow* out) noexcept override
    {
        ++acquire_count;
        last_plug_id = plug_id;
        last_allowed = allowed != nullptr ? *allowed : abi::version_range{};
        last_allowed_was_null = allowed == nullptr;
        last_receiver = receiver;
        if (out != nullptr) *out = abi::borrow{};
        if (acquire_status != abi::ok) return acquire_status;
        if (out == nullptr) return abi::invalid_argument;
        if (allowed == nullptr || !abi::accepts_version(*allowed, grant_version)) {
            return abi::unsupported;
        }
        *out = abi::borrow{next_credential, grant_version};
        return abi::ok;
    }

    /** @brief Record one returned credential and report the configured status. */
    abi::status U42_CALL release(abi::token credential) noexcept override
    {
        ++release_count;
        last_released = credential;
        return next_release;
    }

    abi::token next_credential{42};              //!< Credential acquire() issues next.
    abi::plugin_version grant_version{1, 0, 0};  //!< Actual provider version acquire() grants.
    abi::status next_release = abi::ok;          //!< Status release() reports next.
    abi::status acquire_status = abi::ok;        //!< Status acquire() reports next.
    abi::status announce_status = abi::ok;       //!< Status announce() reports next.
    int acquire_count = 0;                       //!< Number of acquire() calls.
    int release_count = 0;                       //!< Number of release() calls.
    int announce_count = 0;                      //!< Number of announce() calls.
    abi::token last_released{};                  //!< Credential passed to the last release().
    abi::irevoker* last_receiver = nullptr;      //!< Revocation target registered by acquire().
    const char* last_plug_id = nullptr;          //!< Plugin identity passed to acquire().
    abi::version_range last_allowed{};           //!< Range passed to the last acquire().
    bool last_allowed_was_null = false;          //!< Whether the last acquire() got a null range.
    abi::caps_desc last_desc{};                  //!< Copy of the last announced capability set.
};

/**
 * @brief Fake ictx whose query() writes its output on both paths.
 *
 * Writing even on failure is deliberate: it pins that sdk::query() clears the caller's output
 * itself instead of trusting a failing host to leave it alone.
 */
struct fake_ictx final : abi::ictx {
    abi::status next_status = abi::ok;  //!< Status the next query() reports.
    void* next_value = nullptr;                 //!< Value the next query() stores.
    int query_count = 0;                        //!< Number of query() calls seen.
    abi::iid last_type{};                   //!< Identifier the last query() received.

    abi::status U42_CALL query(const abi::iid* type, void** out) noexcept override
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
struct revoking_owner final : abi::irevoker {
    sdk::lease* held = nullptr;  //!< Lease this owner keeps caching.
    int revocations = 0;         //!< Number of on_revoke() deliveries.

    /** @brief Release the lease only when the revoked credential is the one it still holds. */
    void U42_CALL on_revoke(abi::token credential) noexcept override
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

    abi::icaps* out = nullptr;
    CHECK(sdk::query<abi::icaps>(&ctx, abi::caps_iid, &out) == abi::ok);
    CHECK(out == &caps);
    CHECK(ctx.query_count == 1);
    CHECK(ctx.last_type == abi::caps_iid);

    // The identifier is passed through verbatim; the helper must not substitute a default.
    out = nullptr;
    CHECK(sdk::query<abi::icaps>(&ctx, abi::diag_iid, &out) == abi::ok);
    CHECK(ctx.last_type == abi::diag_iid);
    CHECK(ctx.query_count == 2);

    // A host that reports ok with a null pointer yields an empty output, never a fabricated one.
    ctx.next_value = nullptr;
    out = &caps;
    CHECK(sdk::query<abi::icaps>(&ctx, abi::caps_iid, &out) == abi::ok);
    CHECK(out == nullptr);
}

TEST_CASE(query_failure_clears_output)
{
    fake_caps caps;
    fake_ictx ctx;
    ctx.next_value = &caps;
    ctx.next_status = abi::not_found;

    abi::icaps* out = &caps; // deliberately dirty: a failure must clear it
    CHECK(sdk::query<abi::icaps>(&ctx, abi::caps_iid, &out) == abi::not_found);
    CHECK(out == nullptr);
    CHECK(ctx.query_count == 1);

    // Null context: rejected before any dereference, output still cleared.
    out = &caps;
    CHECK(sdk::query<abi::icaps>(nullptr, abi::caps_iid, &out) ==
          abi::invalid_argument);
    CHECK(out == nullptr);
    CHECK(ctx.query_count == 1);

    // Null output has nowhere to report anything, so it is rejected without calling the host.
    CHECK(sdk::query<abi::icaps>(&ctx, abi::caps_iid, nullptr) ==
          abi::invalid_argument);
    CHECK(ctx.query_count == 1);
}

TEST_CASE(lease_move_transfers_ownership_once)
{
    fake_caps caps;
    caps.next_credential = abi::token{42};
    caps.grant_version = echo_version_newer;
    abi::borrow borrowed{};
    CHECK(caps.acquire("demo", &echo_versions, nullptr, &borrowed) == abi::ok);
    // The whole borrow: the credential plus the version the host actually selected.
    CHECK(borrowed.credential.value == 42);
    CHECK(same_version(borrowed.version, echo_version_newer));
    CHECK(same_range(caps.last_allowed, echo_versions));
    CHECK(!caps.last_allowed_was_null);

    {
        sdk::lease owner(&caps, borrowed);
        CHECK(same_version(owner.version(), echo_version_newer));
        {
            sdk::lease moved(std::move(owner));
            CHECK(!owner);
            CHECK(owner.credential().value == 0);
            CHECK(same_version(owner.version(), abi::plugin_version{}));
            CHECK(static_cast<bool>(moved));
            CHECK(moved.credential().value == 42);
            CHECK(moved.matches(abi::token{42}));
            CHECK(same_version(moved.version(), echo_version_newer));
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
    abi::borrow first{};
    caps.next_credential = abi::token{7};
    caps.grant_version = echo_version;
    CHECK(caps.acquire("demo", &echo_versions, nullptr, &first) == abi::ok);
    abi::borrow second{};
    caps.next_credential = abi::token{9};
    caps.grant_version = echo_version_newer;
    CHECK(caps.acquire("demo", &echo_versions, nullptr, &second) == abi::ok);

    {
        sdk::lease target(&caps, first);
        sdk::lease source(&caps, second);
        target = std::move(source);

        // The credential target already held is returned before source's borrow is adopted.
        CHECK(caps.release_count == 1);
        CHECK(caps.last_released.value == 7);
        CHECK(target.credential().value == 9);
        CHECK(target.matches(abi::token{9}));
        // The granted version travels with the credential, not with the lease object.
        CHECK(same_version(target.version(), echo_version_newer));
        CHECK(!source);
        CHECK(source.credential().value == 0);
        CHECK(same_version(source.version(), abi::plugin_version{}));

        // Assigning an empty lease still returns the credential the target held.
        sdk::lease blank;
        target = std::move(blank);
        CHECK(caps.release_count == 2);
        CHECK(caps.last_released.value == 9);
        CHECK(!target);
        CHECK(same_version(target.version(), abi::plugin_version{}));
        CHECK(!blank);
    }
    CHECK(caps.release_count == 2);
    CHECK(caps.last_released.value == 9);
}

TEST_CASE(lease_reset_keeps_ownership_on_retryable_failure_then_succeeds)
{
    fake_caps caps;
    caps.next_credential = abi::token{3};
    caps.next_release = abi::busy;
    caps.grant_version = echo_version_newer;
    abi::borrow borrowed{};
    CHECK(caps.acquire("demo", &echo_versions, nullptr, &borrowed) == abi::ok);

    {
        sdk::lease held(&caps, borrowed);
        CHECK(held.reset() == abi::busy); // the refusal is reported, not swallowed
        CHECK(caps.release_count == 1);
        CHECK(caps.last_released.value == 3);
        // A busy release is retryable: the lease must still own the credential and remember the
        // granted version, otherwise the borrow the host still tracks would be silently lost.
        CHECK(static_cast<bool>(held));
        CHECK(held.credential().value == 3);
        CHECK(held.matches(abi::token{3}));
        CHECK(same_version(held.version(), echo_version_newer));

        // The control thread retries once the host can accept the return.
        caps.next_release = abi::ok;
        CHECK(held.reset() == abi::ok);
        CHECK(caps.release_count == 2);
        CHECK(caps.last_released.value == 3);
        CHECK(held.credential().value == 0);
        CHECK(same_version(held.version(), abi::plugin_version{}));
        CHECK(!held);

        // Idempotent: the cleared lease has nothing left to return.
        CHECK(held.reset() == abi::ok);
        CHECK(caps.release_count == 2);
    }
    // The destructor of the already reset lease returns nothing either.
    CHECK(caps.release_count == 2);
}

TEST_CASE(lease_destructor_attempts_one_final_release)
{
    fake_caps caps;
    caps.next_credential = abi::token{11};
    caps.next_release = abi::wrong_thread;
    caps.grant_version = echo_version_newer;

    abi::borrow borrowed{};
    CHECK(caps.acquire("demo", &echo_versions, nullptr, &borrowed) == abi::ok);

    {
        sdk::lease held(&caps, borrowed);
        CHECK(held.reset() == abi::wrong_thread);
        CHECK(static_cast<bool>(held)); // still owned after a retryable refusal
        CHECK(held.credential().value == 11);
        CHECK(same_version(held.version(), echo_version_newer));
    }
    // Destruction gets one last chance; it cannot retry, so the refused credential is simply not
    // returned again. That is why the class comment demands destruction on the owning thread.
    CHECK(caps.release_count == 2);
    CHECK(caps.last_released.value == 11);
}

TEST_CASE(lease_reset_stale_credential_clears_ownership)
{
    fake_caps caps;
    caps.next_credential = abi::token{21};
    caps.next_release = abi::stale;
    caps.grant_version = echo_version_newer;
    abi::borrow borrowed{};
    CHECK(caps.acquire("demo", &echo_versions, nullptr, &borrowed) == abi::ok);

    sdk::lease held(&caps, borrowed);
    CHECK(same_version(held.version(), echo_version_newer));
    // A stale credential already belongs to a newer record, so it can never be returned again.
    CHECK(held.reset() == abi::stale);
    CHECK(caps.release_count == 1);
    CHECK(caps.last_released.value == 21);
    CHECK(!held);
    CHECK(held.credential().value == 0);
    CHECK(same_version(held.version(), abi::plugin_version{})); // the version goes with it
    CHECK(!held.matches(abi::token{21}));
    CHECK(held.reset() == abi::ok); // nothing left to return
    CHECK(caps.release_count == 1);
}

TEST_CASE(lease_move_assignment_refusal_keeps_both_sides_owned)
{
    fake_caps caps;
    abi::borrow first{};
    caps.next_credential = abi::token{31};
    caps.grant_version = echo_version;
    CHECK(caps.acquire("demo", &echo_versions, nullptr, &first) == abi::ok);
    abi::borrow second{};
    caps.next_credential = abi::token{32};
    caps.grant_version = echo_version_newer;
    CHECK(caps.acquire("demo", &echo_versions, nullptr, &second) == abi::ok);

    caps.next_release = abi::failed;
    sdk::lease target(&caps, first);
    sdk::lease source(&caps, second);

    target = std::move(source);
    // The assignment could not return target's old credential, so it must not have moved the
    // borrow either: both sides keep exactly what they owned and nothing is silently dropped.
    CHECK(caps.release_count == 1);
    CHECK(caps.last_released.value == 31);
    CHECK(target.credential().value == 31);
    CHECK(source.credential().value == 32);
    CHECK(same_version(target.version(), echo_version));
    CHECK(same_version(source.version(), echo_version_newer));
    CHECK(static_cast<bool>(target));
    CHECK(static_cast<bool>(source));

    // Retrying the same assignment once the host accepts the return completes the transfer.
    caps.next_release = abi::ok;
    target = std::move(source);
    CHECK(caps.release_count == 2);
    CHECK(caps.last_released.value == 31);
    CHECK(target.credential().value == 32);
    CHECK(same_version(target.version(), echo_version_newer));
    CHECK(!source);
    CHECK(source.credential().value == 0);
    CHECK(same_version(source.version(), abi::plugin_version{}));
}

TEST_CASE(lease_without_owner_cannot_return_credential)
{
    // Without an icaps there is no release to call and no ok/stale to observe, so ownership must
    // survive: an unstoppable credential is reported instead of being dropped as if returned.
    sdk::lease orphan(nullptr, abi::borrow{abi::token{5}, abi::plugin_version{3, 4, 5}});
    CHECK(static_cast<bool>(orphan));
    CHECK(orphan.credential().value == 5);
    CHECK(orphan.matches(abi::token{5}));
    CHECK(same_version(orphan.version(), abi::plugin_version{3, 4, 5}));
    CHECK(orphan.reset() == abi::invalid_state);
    CHECK(static_cast<bool>(orphan));
    CHECK(orphan.credential().value == 5);
    CHECK(same_version(orphan.version(), abi::plugin_version{3, 4, 5}));
    CHECK(orphan.reset() == abi::invalid_state);

    sdk::lease empty;
    CHECK(!empty);
    CHECK(empty.reset() == abi::ok);
    CHECK(!empty.matches(abi::token{0}));
    CHECK(same_version(empty.version(), abi::plugin_version{}));

    // A zero credential is not a borrow at all: the host tracks nothing, so reset() reports ok,
    // never calls release(), and still drops any stale version a caller put into the value.
    fake_caps caps;
    sdk::lease credentialless(&caps, abi::borrow{abi::token{}, abi::plugin_version{1, 0, 0}});
    CHECK(!credentialless);
    CHECK(same_version(credentialless.version(), abi::plugin_version{1, 0, 0}));
    CHECK(credentialless.reset() == abi::ok);
    CHECK(same_version(credentialless.version(), abi::plugin_version{}));
    CHECK(caps.release_count == 0);
}

TEST_CASE(lease_matches_credential_for_explicit_revocation)
{
    fake_caps caps;
    caps.next_credential = abi::token{77};
    caps.grant_version = echo_version_newer;
    revoking_owner owner;
    abi::borrow borrowed{};
    CHECK(caps.acquire("demo", &echo_versions, &owner, &borrowed) == abi::ok);
    // The registered target is the stable owner, never the movable lease itself.
    CHECK(caps.last_receiver == &owner);
    CHECK(caps.last_plug_id != nullptr);
    CHECK(same_range(caps.last_allowed, echo_versions));
    CHECK(owner.revocations == 0);

    sdk::lease held(&caps, borrowed);
    owner.held = &held;
    CHECK(same_version(held.version(), echo_version_newer));

    CHECK(held.matches(abi::token{77}));
    CHECK(!held.matches(abi::token{78}));
    CHECK(!held.matches(abi::token{0}));

    // A revocation for a different credential leaves this lease intact.
    owner.on_revoke(abi::token{78});
    CHECK(caps.release_count == 0);
    CHECK(static_cast<bool>(held));

    // A revoker that runs while the host refuses the return keeps the lease owning the borrow,
    // so the same revocation can be replayed on the control thread until release() accepts it.
    caps.next_release = abi::busy;
    owner.on_revoke(abi::token{77});
    CHECK(caps.release_count == 1);
    CHECK(held.matches(abi::token{77}));
    CHECK(same_version(held.version(), echo_version_newer)); // the refusal kept the version too
    caps.next_release = abi::ok;

    // The matching revocation returns the credential and empties the lease.
    owner.on_revoke(abi::token{77});
    CHECK(caps.release_count == 2);
    CHECK(caps.last_released.value == 77);
    CHECK(!held);
    CHECK(same_version(held.version(), abi::plugin_version{}));
    CHECK(!held.matches(abi::token{77}));

    // Replaying the same revocation cannot release twice.
    owner.on_revoke(abi::token{77});
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
    static_assert(!std::is_constructible_v<sdk::lease, abi::icaps*, fake_iface*>,
                  "a lease must never adopt a business interface pointer");
    // The lease is exactly the owning icaps plus the {credential, version} borrow: 8 + 24 bytes on
    // the 64-bit profile. There is no room for a business pointer, which is the property this
    // migration is about, and the granted version is part of the value rather than inferred later.
    static_assert(sizeof(sdk::lease) == sizeof(void*) + sizeof(abi::borrow),
                  "lease must store only icaps* + the borrow");
#if UINTPTR_MAX == UINT64_MAX
    static_assert(sizeof(sdk::lease) == 32 && alignof(sdk::lease) == 8,
                  "on the 64-bit profile the lease must be exactly 32 bytes");
#endif
    static_assert(!std::is_copy_constructible_v<sdk::lease> &&
                      !std::is_copy_assignable_v<sdk::lease>,
                  "a credential is owned by exactly one lease");
    static_assert(std::is_move_constructible_v<sdk::lease> &&
                      std::is_move_assignable_v<sdk::lease>,
                  "a lease must still transfer ownership");
    // sdk::lease is used above without template arguments: a typed lease<...> no longer exists.
    static_assert(std::is_class_v<sdk::lease>, "lease is the non-template RAII handle");
    static_assert(noexcept(std::declval<const sdk::lease&>().version()),
                  "version() must be a non-throwing getter");
    static_assert(std::is_same_v<decltype(std::declval<const sdk::lease&>().version()),
                                 abi::plugin_version>,
                  "version() must report the borrowed plugin_version");

    fake_caps caps;
    sdk::lease held(&caps, abi::borrow{abi::token{64}, abi::plugin_version{2, 0, 0}});
    CHECK(static_cast<bool>(held));
    CHECK(held.credential().value == 64);
    CHECK(same_version(held.version(), abi::plugin_version{2, 0, 0}));
    CHECK(caps.release_count == 0);
    CHECK(held.reset() == abi::ok);
    CHECK(caps.release_count == 1);
}

TEST_CASE(borrow_is_the_credential_and_version_pair)
{
    static_assert(sizeof(abi::borrow) == 24 && alignof(abi::borrow) == 8,
                  "borrow must stay the 24-byte, 8-aligned pair");
    static_assert(offsetof(abi::borrow, version) == 8, "the version must follow the credential");
    static_assert(std::is_same_v<decltype(abi::borrow{}.credential), abi::token>,
                  "the first borrow member must be the credential token");
    static_assert(std::is_same_v<decltype(abi::borrow{}.version), abi::plugin_version>,
                  "the second borrow member must be the plugin version");
    static_assert(!accepts_two_tokens<abi::borrow>::value,
                  "borrow must not absorb a second token");
    static_assert(borrow_binds_to_credential_and_version(),
                  "borrow must expose exactly two members");

    const abi::borrow value{abi::token{7}, abi::plugin_version{1, 2, 3}};
    CHECK(value.credential.value == 7);
    CHECK(same_version(value.version, abi::plugin_version{1, 2, 3}));

    // acquire() hands out the same pair; the lease can then be built from it.
    fake_caps caps;
    caps.next_credential = abi::token{13};
    caps.grant_version = echo_version_newer;
    abi::borrow issued{};
    CHECK(caps.acquire("demo", &echo_versions, nullptr, &issued) == abi::ok);
    CHECK(issued.credential.value == 13);
    CHECK(same_version(issued.version, echo_version_newer));
    sdk::lease held(&caps, issued);
    CHECK(held.credential().value == 13);
    CHECK(same_version(held.version(), echo_version_newer));
    CHECK(held.reset() == abi::ok);
    CHECK(caps.last_released.value == 13);
}

TEST_CASE(version_helpers_cover_closed_bounds)
{
    // The static_asserts above pin the constexpr rules; these checks exercise the same helpers at
    // runtime so a helper that only worked in a constant expression would still be caught.
    const abi::version_range allowed = abi::version_range{{1, 0, 0}, {1, 9, 9}};
    CHECK(abi::valid_version_range(allowed));
    CHECK(abi::accepts_version(allowed, abi::plugin_version{1, 0, 0})); // lower bound included
    CHECK(abi::accepts_version(allowed, abi::plugin_version{1, 9, 9})); // upper bound included
    CHECK(abi::accepts_version(allowed, abi::plugin_version{1, 5, 0})); // interior
    CHECK(!abi::accepts_version(allowed, abi::plugin_version{0, 9, 9}));
    CHECK(!abi::accepts_version(allowed, abi::plugin_version{1, 9, 10}));
    CHECK(!abi::accepts_version(allowed, abi::plugin_version{2, 0, 0}));

    const abi::version_range exact = abi::exact_version(abi::plugin_version{1, 2, 3});
    CHECK(abi::valid_version_range(exact));
    CHECK(abi::accepts_version(exact, abi::plugin_version{1, 2, 3}));
    CHECK(!abi::accepts_version(exact, abi::plugin_version{1, 2, 4}));
    CHECK(!abi::accepts_version(exact, abi::plugin_version{1, 3, 3}));

    // A reversed range is invalid and can never accept anything.
    const abi::version_range reversed = abi::version_range{{2, 0, 0}, {1, 9, 9}};
    CHECK(!abi::valid_version_range(reversed));
    CHECK(!abi::accepts_version(reversed, abi::plugin_version{1, 5, 0}));
    CHECK(!abi::accepts_version(reversed, abi::plugin_version{2, 0, 0}));

    // uint32 boundaries are ordinary values, so max endpoints are inclusive.
    const abi::plugin_version top{std::numeric_limits<std::uint32_t>::max(),
                                  std::numeric_limits<std::uint32_t>::max(),
                                  std::numeric_limits<std::uint32_t>::max()};
    CHECK(abi::accepts_version(abi::version_range{top, top}, top));
    CHECK(!abi::accepts_version(echo_versions, top)); // 2.x lies outside the requested 1.x range
    CHECK(!abi::version_less(top, top));

    // Numeric, not textual: 1.10.0 sorts after 1.2.0.
    CHECK(abi::version_less(abi::plugin_version{1, 2, 0}, abi::plugin_version{1, 10, 0}));
    CHECK(!abi::version_less(abi::plugin_version{1, 10, 0}, abi::plugin_version{1, 2, 0}));
    CHECK(abi::version_less(abi::plugin_version{1, 10, 0}, abi::plugin_version{2, 0, 0}));

    // A cross-major range is honored only because the caller explicitly asked for it.
    const abi::version_range across = abi::version_range{{1, 0, 0}, {2, 0, 0}};
    CHECK(abi::accepts_version(across, abi::plugin_version{2, 0, 0}));
    CHECK(!abi::accepts_version(echo_versions, abi::plugin_version{2, 0, 0}));

    // 0.0.0 is a legal version, not a failure marker.
    const abi::version_range zero = abi::exact_version(abi::plugin_version{0, 0, 0});
    CHECK(abi::valid_version_range(zero));
    CHECK(abi::accepts_version(zero, abi::plugin_version{0, 0, 0}));
    CHECK(!abi::accepts_version(zero, abi::plugin_version{0, 0, 1}));
}

TEST_CASE(lease_reports_actual_version_and_failure_clears_borrow)
{
    fake_caps caps;
    caps.next_credential = abi::token{5};
    caps.grant_version = abi::plugin_version{0, 0, 0};

    // 0.0.0 is a legal granted version: ownership is decided by the credential alone, so the lease
    // is non-empty even though version() reads 0.0.0.
    abi::borrow zero_borrow{};
    const abi::version_range zero_range = abi::exact_version(abi::plugin_version{0, 0, 0});
    CHECK(caps.acquire("demo", &zero_range, nullptr, &zero_borrow) == abi::ok);
    CHECK(zero_borrow.credential.value == 5);
    CHECK(same_version(zero_borrow.version, abi::plugin_version{0, 0, 0}));

    {
        sdk::lease held(&caps, zero_borrow);
        CHECK(static_cast<bool>(held));
        CHECK(held.credential().value == 5);
        CHECK(same_version(held.version(), abi::plugin_version{0, 0, 0}));

        // A retryable refusal must not forget the granted version along with the token.
        caps.next_release = abi::busy;
        CHECK(held.reset() == abi::busy);
        CHECK(static_cast<bool>(held));
        CHECK(same_version(held.version(), abi::plugin_version{0, 0, 0}));

        caps.next_release = abi::ok;
        CHECK(held.reset() == abi::ok);
        CHECK(!held);
        CHECK(same_version(held.version(), abi::plugin_version{}));
    }

    // A failed acquire writes neither a credential nor a version into a dirty output.
    caps.acquire_status = abi::failed;
    abi::borrow failed{abi::token{77}, abi::plugin_version{3, 2, 1}};
    CHECK(caps.acquire("demo", &echo_versions, nullptr, &failed) == abi::failed);
    CHECK(failed.credential.value == 0);
    CHECK(same_version(failed.version, abi::plugin_version{}));

    // The cleared output yields an empty lease: reset() reports ok and never calls release().
    const int releases_before = caps.release_count;
    sdk::lease empty(&caps, failed);
    CHECK(!empty);
    CHECK(same_version(empty.version(), abi::plugin_version{}));
    CHECK(empty.reset() == abi::ok);
    CHECK(caps.release_count == releases_before);
}

TEST_CASE(fake_icaps_carries_only_methods_and_caps_desc)
{
    static_assert(sizeof(abi::caps_desc) == 16 && offsetof(abi::caps_desc, methods) == 8,
                  "caps_desc must expose only a method count and a method table");
    static_assert(sizeof(abi::method_desc) == 40, "a method description stays five words wide");

    static const abi::method_desc methods[] = {
        {1, "echo", "repeat the payload", "{}", "{}"},
    };
    abi::caps_desc desc{};
    desc.method_count = 1;
    desc.methods = methods;

    fake_caps caps;
    CHECK(caps.announce(&desc) == abi::ok);
    CHECK(caps.announce_count == 1);
    CHECK(caps.last_desc.method_count == 1);
    CHECK(caps.last_desc.methods == methods);

    // acquire() is range-based now: the requested closed range is passed through verbatim, and the
    // issued borrow carries the credential plus the version the provider actually offers.
    caps.grant_version = echo_version_newer;
    abi::borrow borrowed{};
    CHECK(caps.acquire("echo", &echo_versions, nullptr, &borrowed) == abi::ok);
    CHECK(same_range(caps.last_allowed, echo_versions));
    CHECK(!caps.last_allowed_was_null);
    CHECK(caps.last_plug_id != nullptr);
    CHECK(borrowed.credential.value != 0);
    CHECK(same_version(borrowed.version, echo_version_newer));

    // A range that does not accept the provider version is refused with unsupported, and the whole
    // borrow is cleared: the fake mirrors the ABI rule of clearing a legal output first.
    abi::borrow refused{abi::token{99}, abi::plugin_version{9, 9, 9}};
    CHECK(caps.acquire("echo", &echo_exact, nullptr, &refused) == abi::unsupported);
    CHECK(same_range(caps.last_allowed, echo_exact));
    CHECK(!caps.last_allowed_was_null);
    CHECK(refused.credential.value == 0);
    CHECK(same_version(refused.version, abi::plugin_version{}));
    sdk::lease none(&caps, refused);
    CHECK(!none);

    // A null required range is rejected without creating a lease either.
    CHECK(caps.acquire("echo", nullptr, nullptr, &refused) == abi::unsupported);
    CHECK(caps.last_allowed_was_null);
    CHECK(refused.credential.value == 0);
    CHECK(same_version(refused.version, abi::plugin_version{}));
}

TEST_CASE(view_borrows_string_data)
{
    const abi::bytes none = sdk::view(std::string_view{});
    CHECK(none.data == nullptr);
    CHECK(none.size == 0);

    const std::string text = "hello";
    const abi::bytes raw = sdk::view(std::string_view{text});
    CHECK(raw.data == text.data()); // same address: the view is borrowed, not copied
    CHECK(raw.size == 5u);

    // A view with a non-null but empty range still normalizes to {nullptr, 0}, because the ABI
    // requires a null pointer to carry size zero.
    const std::string_view empty_tail(text.data() + text.size(), 0);
    CHECK(empty_tail.data() != nullptr);
    const abi::bytes tail = sdk::view(empty_tail);
    CHECK(tail.data == nullptr);
    CHECK(tail.size == 0);
}

TEST_CASE(writer_appends_through_the_abi_interface)
{
    std::string out;
    sdk::string_writer writer(&out, 8);
    abi::iwriter* abi_writer = &writer; // the ABI only ever sees iwriter*

    CHECK(abi_writer->write(sdk::view("ab")) == abi::ok);
    CHECK(out == "ab");
    CHECK(abi_writer->write(sdk::view("cd")) == abi::ok);
    CHECK(out == "abcd");
    CHECK(writer.status() == abi::ok);

    // A zero-length chunk is legal even with a null pointer and must change nothing.
    CHECK(abi_writer->write(abi::bytes{nullptr, 0}) == abi::ok);
    CHECK(out == "abcd");
}

TEST_CASE(writer_rejects_over_limit_atomically_and_stickily)
{
    std::string out;
    sdk::string_writer writer(&out, 4);
    CHECK(writer.write(sdk::view("abcde")) == abi::limit_exceeded);
    CHECK(out.empty()); // no truncated prefix is published
    CHECK(writer.status() == abi::limit_exceeded);
    CHECK(writer.write(sdk::view("ab")) == abi::limit_exceeded); // sticky
    CHECK(out.empty());

    // The cap counts the whole target: filling it exactly is allowed, one byte more is not.
    std::string exact;
    sdk::string_writer cumulative(&exact, 4);
    CHECK(cumulative.write(sdk::view("abcd")) == abi::ok);
    CHECK(cumulative.write(sdk::view("e")) == abi::limit_exceeded);
    CHECK(exact == "abcd");
    CHECK(cumulative.status() == abi::limit_exceeded);

    // A target that already exceeds the cap must not become writable: the remaining room must
    // not underflow to a huge value.
    std::string over = "abcdef";
    sdk::string_writer clamped(&over, 2);
    CHECK(clamped.write(abi::bytes{nullptr, 0}) == abi::ok); // zero bytes never exceed
    CHECK(over == "abcdef");
    CHECK(clamped.write(sdk::view("g")) == abi::limit_exceeded);
    CHECK(over == "abcdef");
}

TEST_CASE(writer_rejects_null_target_and_malformed_bytes)
{
    sdk::string_writer detached(nullptr, 16);
    CHECK(detached.write(sdk::view("x")) == abi::invalid_argument);
    CHECK(detached.status() == abi::invalid_argument);
    CHECK(detached.write(abi::bytes{nullptr, 0}) == abi::invalid_argument); // sticky

    std::string out;
    sdk::string_writer writer(&out, 8);
    CHECK(writer.write(abi::bytes{nullptr, 3}) == abi::invalid_argument);
    CHECK(out.empty());
    CHECK(writer.status() == abi::invalid_argument);
    CHECK(writer.write(sdk::view("ok")) == abi::invalid_argument); // sticky
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
    CHECK(writer.write(abi::bytes{&marker, std::numeric_limits<std::uint64_t>::max()}) ==
          abi::failed);
    CHECK(writer.status() == abi::failed);
    CHECK(out.empty());
    CHECK(writer.write(sdk::view("small")) == abi::failed); // sticky
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
