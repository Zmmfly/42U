/**
 * @file lease_test.cc
 * @brief ABI v3 acceptance for version-qualified leases and credential-direct invocation.
 *
 * The file owns its own main() and links against the host translation units, so it can be built as:
 *   g++ -std=c++17 -Wall -Wextra -Werror -Iinc tests/lease_test.cc src/context.cc src/events.cc
 *       src/host.cc src/order.cc src/plug.cc -ldl -pthread \
 *       -o build/invoke-v3/lease/lease_test
 *
 * ABI v3 removed the business contract/protocol/binding layer: a caller states an inclusive numeric
 * plugin-version range in icaps::acquire(), receives one {credential, actual version} borrow and then
 * invokes published methods directly through icalls::call_name()/call_id() with that credential.
 * This file is the dedicated acceptance for that path and for the version rules that replaced the
 * contract negotiation. Nothing here declares, stores or emulates a binding: the helpers only
 * forward a credential, and the compile-time checks below prove that the removed members
 * (icalls::bind_name/bind_id/unbind/call, host::bind/unbind/protocol, caps_desc::protocol, ...) do
 * not exist. There is no business "family" identifier either: a provider is identified by plug_id
 * and its declared numeric plugin_version.
 *
 * Every fixture is an in-process static factory handed to u42::host::add(); no shared object, no
 * framework and no private header is involved. Plugins observe the host only through the public ABI
 * in 42u/abi.hpp, so all business traffic runs consumer -> host icalls -> provider iinvoke and no
 * provider interface pointer is ever handed to a consumer.
 *
 * The eleven v2 contract cases are carried over equivalently:
 *   1. Version mismatch is refused without a lease: a reversed range is invalid_argument, an actual
 *      version outside the range is unsupported, both clear the borrow, and discovery creates no
 *      lock. Endpoint-inclusive ranges, exact versions, the numeric order 1.10.0 > 1.2.0, the uint32
 *      maximum boundary and a legal 0.0.0 whose success is carried by the credential are checked.
 *   2. An explicitly requested range that crosses a major boundary is not second-guessed and
 *      returns the actual version for the caller to branch on.
 *   3. Reloading the same version never lets the old credential or the old business session follow:
 *      the old credential stays stale, the session must be rebuilt, and reused business session IDs
 *      are legal in the new generation (the returned version is a copy of the recorded one).
 *   4. An upgrade to a new major refuses the old expectation and accepts the explicitly stated one.
 *   5. Multiple leases are independent: returning one keeps the other lock, and repeated name and
 *      numeric calls on one lease need no new credential and never consume or release it.
 *   6. Zero, wrong-owner and off-thread credentials are refused without disturbing a live lease,
 *      including the native rule that wrong_thread leaves a native output argument untouched.
 *   7. A release attempted from inside provider.invoke and from inside the result writer reports
 *      busy and loses no credential.
 *   8. The host queries the provider for invoke_iid only, and a consumer without business methods is
 *      never queried at all.
 *   9. An unload requested from inside a plugin callback is accepted as deferred; an administration
 *      revoker runs under the depth guard and cannot structurally re-enter the host.
 *  10. A failing one-shot call leaves no residual lease, and a failing leased call keeps the lease.
 *  11. The mock respects the lifecycle gates: acquire and calls attempted while Initializing or
 *      Starting are refused with no lock, and an instance that is no longer Active cannot call.
 *
 * @note NDEBUG is defined deliberately: CHECK must keep reporting failures even when assert() has
 *       been compiled out, so a diagnostic can never vanish in release builds.
 * @note The fixtures are inspected only through their own factory/instance objects, never through a
 *       host-internal record or context, and they never keep a binding-like lookup table.
 */

#define NDEBUG 1

#include <42u/host.hpp>
#include <42u/sdk.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace abi = u42::abi::v3;

/* ------------------------------------------------------------------ *
 * Check machinery: independent of assert() and therefore of NDEBUG.
 * ------------------------------------------------------------------ */

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

/**
 * @brief Compile-time guard: CHECK must expand to a call of fail_check, never to assert().
 *
 * With NDEBUG defined, assert() collapses to ((void)0), so every failure would silently disappear.
 * The expansion text is checked because a runtime probe cannot detect it.
 */
inline constexpr std::string_view u42_check_expansion = U42_EXPAND_STRING(CHECK(0 == 1));
static_assert(u42_check_expansion.find("fail_check") != std::string_view::npos,
              "CHECK must call fail_check directly; defining it as assert() would disable it "
              "under NDEBUG");

/* ------------------------------------------------------------------ *
 * Compile-time ABI shape and signature proofs.
 * ------------------------------------------------------------------ */

/** @brief Two-field ABI value: one credential plus the version the host actually granted. */
static_assert(sizeof(abi::borrow) == 24 && alignof(abi::borrow) == 8,
              "borrow is one 8-byte credential plus a 12-byte version, padded to 24");
static_assert(offsetof(abi::borrow, credential) == 0 && offsetof(abi::borrow, version) == 8);
static_assert(std::is_standard_layout_v<abi::borrow> && std::is_trivially_copyable_v<abi::borrow>);
static_assert(std::is_aggregate_v<abi::borrow>);
static_assert(std::is_same_v<decltype(abi::borrow{}.credential), abi::token>);
static_assert(std::is_same_v<decltype(abi::borrow{}.version), abi::plugin_version>);
static_assert(!std::is_pointer_v<decltype(abi::borrow{}.credential)> &&
              !std::is_pointer_v<decltype(abi::borrow{}.version)>);
// A standard-layout struct may not overlap its members, so credential (8) + version (12) + 4 bytes
// of padding is the whole object: there is no room left for a provider pointer. A third member
// would not fit at all, which is the compile-time proof that a borrow smuggles no provider address.
static_assert(sizeof(abi::borrow) - sizeof(abi::token) - sizeof(abi::plugin_version) == 4,
              "borrow has exactly the credential and the version as members, plus padding");

static_assert(sizeof(abi::plugin_version) == 12 && alignof(abi::plugin_version) == 4);
static_assert(sizeof(abi::version_range) == 24 && alignof(abi::version_range) == 4);
static_assert(abi::abi_major == 3);

// The descriptor and announcement carry the typed version only: no const char* version and no
// business contract/protocol field hides in the ABI structures.
static_assert(sizeof(abi::plug_desc) == 64 && alignof(abi::plug_desc) == 8);
static_assert(offsetof(abi::plug_desc, plug_id) == 8 && offsetof(abi::plug_desc, version) == 16);
static_assert(offsetof(abi::plug_desc, priority) == 28 && offsetof(abi::plug_desc, before) == 40);
static_assert(offsetof(abi::plug_desc, after_count) == 48 && offsetof(abi::plug_desc, after) == 56);
static_assert(std::is_same_v<decltype(abi::plug_desc{}.version), abi::plugin_version>);
static_assert(sizeof(abi::caps_desc) == 16 && offsetof(abi::caps_desc, methods) == 8);
static_assert(sizeof(abi::cap_event) == 40 && offsetof(abi::cap_event, version) == 12 &&
              offsetof(abi::cap_event, capabilities) == 24);
static_assert(std::is_same_v<decltype(abi::cap_event{}.version), abi::plugin_version>);

/**
 * @brief Declare a trait that is true only when T still exposes a member with this name.
 *
 * @param member Member name to probe; a removed member makes the specialization disappear.
 * @note This is how the absence of the v2 binding layer is pinned at compile time: a re-introduced
 *       ABI member, or a test helper that kept the old layer, breaks the build here.
 */
#define U42_HAS_MEMBER(member)                                                      \
    template <class T, class = void>                                                \
    struct has_member_##member : std::false_type {};                                 \
    template <class T>                                                              \
    struct has_member_##member<T, std::void_t<decltype(&T::member)>> : std::true_type {}

U42_HAS_MEMBER(bind_name);
U42_HAS_MEMBER(bind_id);
U42_HAS_MEMBER(unbind);
U42_HAS_MEMBER(call);
U42_HAS_MEMBER(bind);
U42_HAS_MEMBER(protocol);
#undef U42_HAS_MEMBER

static_assert(!has_member_bind_name<abi::icalls>::value,
              "icalls must not expose bind_name; a credential is the only way to invoke");
static_assert(!has_member_bind_id<abi::icalls>::value,
              "icalls must not expose bind_id; numeric lookup uses call_id with a credential");
static_assert(!has_member_unbind<abi::icalls>::value, "icalls must not expose unbind");
static_assert(!has_member_call<abi::icalls>::value,
              "icalls must not expose a binding-based call; use call_name/call_id");
static_assert(!has_member_bind<abi::icaps>::value,
              "icaps must not expose bind; acquire returns the borrow instead");
static_assert(!has_member_bind<u42::host>::value, "host::bind was removed in ABI v3");
static_assert(!has_member_unbind<u42::host>::value, "host::unbind was removed in ABI v3");
static_assert(!has_member_protocol<u42::host>::value,
              "host::protocol was replaced by host::version in ABI v3");
static_assert(!has_member_protocol<abi::caps_desc>::value,
              "caps_desc must not carry a business protocol; the version lives in plug_desc");

/** @brief Detect the removed convenience call that accepted a plug_id without a version range. */
template <class T, class = void>
struct host_call_without_range : std::false_type {};
template <class T>
struct host_call_without_range<T, std::void_t<decltype(std::declval<T&>().call(
                                           std::declval<const std::string&>(),
                                           std::declval<const std::string&>(),
                                           std::declval<abi::bytes>(),
                                           std::declval<std::string*>()))>> : std::true_type {};
static_assert(!host_call_without_range<u42::host>::value,
              "the one-shot host call must state the explicitly accepted version range");

// Exact signatures: an added overload with the same name would make these declarations ambiguous or
// change the type, which is the intended compile-time proof.
static_assert(std::is_same_v<decltype(&abi::icalls::call_name),
                             abi::status (U42_CALL abi::icalls::*)(abi::token, const char*, abi::bytes,
                                                                   abi::iwriter*) noexcept>);
static_assert(std::is_same_v<decltype(&abi::icalls::call_id),
                             abi::status (U42_CALL abi::icalls::*)(abi::token, abi::method_id,
                                                                   abi::bytes,
                                                                   abi::iwriter*) noexcept>);
static_assert(std::is_same_v<decltype(&abi::icaps::acquire),
                             abi::status (U42_CALL abi::icaps::*)(const char*,
                                                                  const abi::version_range*,
                                                                  abi::irevoker*,
                                                                  abi::borrow*) noexcept>);
static_assert(std::is_same_v<decltype(&abi::icaps::release),
                             abi::status (U42_CALL abi::icaps::*)(abi::token) noexcept>);
static_assert(std::is_same_v<decltype(&u42::host::acquire),
                             abi::status (u42::host::*)(const std::string&, const abi::version_range&,
                                                        abi::irevoker*, abi::borrow*)>);
static_assert(std::is_same_v<decltype(&u42::host::release),
                             abi::status (u42::host::*)(abi::token)>);
static_assert(std::is_same_v<decltype(&u42::host::version),
                             abi::status (u42::host::*)(const std::string&, abi::plugin_version*)>);

/** @brief Detect a lease helper that would hand out a provider pointer. */
template <class T, class = void>
struct has_arrow_operator : std::false_type {};
template <class T>
struct has_arrow_operator<T, std::void_t<decltype(std::declval<T&>().operator->())>>
    : std::true_type {};
template <class T, class = void>
struct has_get_member : std::false_type {};
template <class T>
struct has_get_member<T, std::void_t<decltype(std::declval<T&>().get())>> : std::true_type {};
static_assert(!has_arrow_operator<u42::sdk::lease>::value,
              "sdk::lease must not dereference to a business interface");
static_assert(!has_get_member<u42::sdk::lease>::value,
              "sdk::lease must not expose a raw borrowed interface");
static_assert(std::is_same_v<decltype(std::declval<const u42::sdk::lease&>().credential()),
                             abi::token>);
static_assert(std::is_same_v<decltype(std::declval<const u42::sdk::lease&>().version()),
                             abi::plugin_version>);
static_assert(std::is_same_v<decltype(std::declval<const u42::sdk::lease&>().matches(abi::token{})),
                             bool>);

/* ------------------------------------------------------------------ *
 * Constexpr version helpers: inclusive endpoints, ordering and bounds.
 * ------------------------------------------------------------------ */

/** @brief Shorthand for a plugin version triple, usable in constexpr and runtime checks. */
constexpr abi::plugin_version pv(std::uint32_t major, std::uint32_t minor,
                                 std::uint32_t patch) noexcept
{
    return abi::plugin_version{major, minor, patch};
}

/** @brief Shorthand for an inclusive range between two version triples. */
constexpr abi::version_range between(abi::plugin_version minimum, abi::plugin_version maximum) noexcept
{
    return abi::version_range{minimum, maximum};
}

constexpr abi::plugin_version kFull = pv(UINT32_MAX, UINT32_MAX, UINT32_MAX);

static_assert(abi::version_less(pv(1, 2, 0), pv(1, 10, 0)),
              "versions are numeric triples, not text: 1.2.0 precedes 1.10.0");
static_assert(!abi::version_less(pv(1, 10, 0), pv(1, 9, 9)) &&
              abi::version_less(pv(1, 9, 9), pv(1, 10, 0)));
static_assert(abi::version_less(pv(1, UINT32_MAX, UINT32_MAX), pv(2, 0, 0)),
              "no overflow at the uint32 maximum");
static_assert(abi::valid_version_range(between(pv(0, 0, 0), pv(0, 0, 0))));
static_assert(abi::valid_version_range(between(pv(1, 0, 0), kFull)));
static_assert(!abi::valid_version_range(between(pv(2, 0, 0), pv(1, 0, 0))));
static_assert(abi::accepts_version(abi::exact_version(pv(1, 5, 2)), pv(1, 5, 2)));
static_assert(!abi::accepts_version(abi::exact_version(pv(1, 5, 2)), pv(1, 5, 3)));
static_assert(abi::accepts_version(between(pv(1, 0, 0), pv(1, 5, 2)), pv(1, 0, 0)),
              "the minimum endpoint is inclusive");
static_assert(abi::accepts_version(between(pv(1, 0, 0), pv(1, 5, 2)), pv(1, 5, 2)),
              "the maximum endpoint is inclusive");
static_assert(!abi::accepts_version(between(pv(1, 0, 0), pv(1, 5, 2)), pv(1, 5, 3)));
static_assert(!abi::accepts_version(between(pv(2, 0, 0), pv(1, 0, 0)), pv(1, 0, 0)),
              "a reversed range accepts nothing");
static_assert(abi::accepts_version(between(pv(0, 0, 0), pv(0, 0, 0)), pv(0, 0, 0)),
              "0.0.0 is a legal version, never an error marker");
static_assert(abi::accepts_version(between(pv(1, 0, 0), kFull), kFull));
static_assert(abi::accepts_version(between(pv(1, 0, 0), pv(3, 0, 0)), pv(2, 7, 3)),
              "a range that crosses a major boundary is valid when the caller states it");
/* ------------------------------------------------------------------ *
 * Shared helpers.
 * ------------------------------------------------------------------ */

/** @brief Borrow a string as ABI bytes without copying it. */
abi::bytes as_bytes(const std::string& text) noexcept
{
    if (text.empty()) return abi::bytes{nullptr, 0};
    return abi::bytes{text.data(), static_cast<std::uint64_t>(text.size())};
}

/** @brief Copy borrowed ABI bytes into host-owned text. */
std::string text_of(abi::bytes data)
{
    if (data.data == nullptr || data.size == 0) return std::string();
    return std::string(static_cast<const char*>(data.data), static_cast<std::size_t>(data.size));
}

/** @brief Write one text chunk through a borrowed writer. */
abi::status write_text(abi::iwriter* writer, const std::string& text)
{
    if (writer == nullptr) return abi::failed;
    return writer->write(as_bytes(text));
}

/** @brief True when a refusal cleared the whole borrow output. */
bool cleared(const abi::borrow& value) noexcept
{
    return value.credential.value == 0 && value.version == abi::plugin_version{};
}

/** @brief A poisoned borrow output: every refused acquire() must clear both fields. */
abi::borrow poisoned() noexcept
{
    return abi::borrow{abi::token{0xDEADBEEF}, pv(9, 9, 9)};
}

/** @brief Parse the "S<number>" session token a provider writes from its open method. */
bool parse_session(const std::string& text, std::uint64_t& out) noexcept
{
    if (text.size() < 2 || text[0] != 'S') return false;
    std::uint64_t value = 0;
    for (std::size_t index = 1; index < text.size(); ++index) {
        const char digit = text[index];
        if (digit < '0' || digit > '9') return false;
        value = value * 10 + static_cast<std::uint64_t>(digit - '0');
    }
    out = value;
    return true;
}

/* ------------------------------------------------------------------ *
 * Provider fixture: iplug + iinvoke with a typed version and a session table.
 * ------------------------------------------------------------------ */

/** @brief Method identities announced by every fake provider. */
enum : abi::method_id { open_method = 1, read_method = 2, fail_method = 3, who_method = 4 };

/**
 * @brief Re-entrant probes a provider runs at the start of its own invoke().
 *
 * The probes model a hostile provider: it tries to return the consumer's lease from inside the call,
 * and it tries to unload or restart the host while it is on the stack. The test reads back the
 * recorded statuses.
 */
struct invoke_probe {
    /** @brief Capability service the probe should call release() on; null disables the probe. */
    abi::icaps* release_caps = nullptr;
    /** @brief Credential the release probe must not be able to return. */
    abi::token release_token{};
    bool release_ran = false;
    abi::status release_status = abi::ok;
    /** @brief Host facade used by the structural probe; null disables it. */
    u42::host* structural_host = nullptr;
    std::string deferred_target;
    std::string unknown_target{"lease.no.such.plugin"};
    abi::iplug_fty* add_target = nullptr;
    bool structural_ran = false;
    abi::status deferred_status = abi::ok;
    abi::status unknown_status = abi::ok;
    abi::status shutdown_status = abi::ok;
    abi::status start_status = abi::ok;
    abi::status add_status = abi::ok;
};

class provider_factory;

/**
 * @brief One fake provider instance: lifecycle, invoke gateway and a per-generation session map.
 */
class fake_provider final : public abi::iplug, public abi::iinvoke {
public:
    /** @brief Bind to the library-owned factory that describes this instance. */
    explicit fake_provider(provider_factory& owner) noexcept : owner_(owner) {}
    fake_provider(const fake_provider&) = delete;
    fake_provider& operator=(const fake_provider&) = delete;

    // iplug
    abi::status U42_CALL init(abi::ictx* ctx) noexcept override;
    abi::status U42_CALL start() noexcept override;
    abi::status U42_CALL stop() noexcept override;
    void U42_CALL destroy() noexcept override;
    abi::status U42_CALL query(const abi::iid* type, void** out) noexcept override;
    // iinvoke
    abi::status U42_CALL invoke(abi::method_id method, abi::bytes args,
                               abi::iwriter* result) noexcept override;

    /** @brief Probe configuration the test mutates directly between calls. */
    invoke_probe probe;

    // Observations.
    std::uint64_t init_calls = 0;
    std::uint64_t start_calls = 0;
    std::uint64_t stop_calls = 0;
    std::uint64_t destroy_calls = 0;
    bool destroyed = false;
    std::uint64_t invoke_calls = 0;
    std::uint64_t query_invoke_calls = 0;
    std::uint64_t query_other_calls = 0;
    std::uint64_t query_null_calls = 0;

private:
    /** @brief Run the configured entry probes; each is consumed so it cannot repeat. */
    void run_entry_probes();

    provider_factory& owner_;
    abi::ictx* ctx_ = nullptr;
    // Deliberately instance-local: a reload may reuse the same business session number.
    std::uint64_t next_session_ = 0;
    std::map<std::uint64_t, std::string> sessions_;
};

/**
 * @brief Library-owned static factory with immutable descriptor storage.
 *
 * One factory describes exactly one plug_id, its typed plugin_version and a fixture-local tag. The
 * tag makes the instance that served a call observable, so a credential can be proven to select its
 * recorded target rather than caller-side data.
 */
class provider_factory final : public abi::iplug_fty {
public:
    /**
     * @brief Build one provider type.
     *
     * @param plug_id Stable plugin identity; the descriptor borrows this storage.
     * @param version Published numeric plugin version copied from this descriptor.
     * @param tag Fixture-local text the provider echoes from its who/read methods.
     */
    provider_factory(std::string plug_id, abi::plugin_version version, std::string tag);

    abi::status U42_CALL describe(const abi::plug_desc** out) noexcept override;
    abi::status U42_CALL create(abi::iplug** out) noexcept override;

    /** @brief Most recently created instance, or null before create(). */
    fake_provider* instance() const noexcept { return instance_.get(); }
    /** @brief Version this factory declared through its descriptor. */
    abi::plugin_version declared_version() const noexcept { return version_; }
    /** @brief Fixture-local tag used to identify the instance that served a call. */
    const std::string& tag() const noexcept { return tag_; }

    std::uint64_t create_calls = 0;

private:
    std::string plug_id_;
    abi::plugin_version version_{};
    std::string tag_;
    abi::plug_desc desc_{};
    std::unique_ptr<fake_provider> instance_;
};

provider_factory::provider_factory(std::string plug_id, abi::plugin_version version, std::string tag)
    : plug_id_(std::move(plug_id)), version_(version), tag_(std::move(tag))
{
    desc_.struct_size = sizeof(abi::plug_desc);
    desc_.reserved = 0;
    desc_.plug_id = plug_id_.c_str();
    desc_.version = version_;
    desc_.priority = 0;
    desc_.before_count = 0;
    desc_.before = nullptr;
    desc_.after_count = 0;
    desc_.after = nullptr;
}

abi::status U42_CALL provider_factory::describe(const abi::plug_desc** out) noexcept
{
    if (out == nullptr) return abi::invalid_argument;
    *out = &desc_;
    return abi::ok;
}

abi::status U42_CALL provider_factory::create(abi::iplug** out) noexcept
{
    ++create_calls;
    if (out == nullptr) return abi::invalid_argument;
    *out = nullptr;
    if (instance_ != nullptr) {
        if (!instance_->destroyed) return abi::invalid_state;
        instance_.reset(); // A reload creates a fresh generation under the same identity.
    }
    try {
        instance_ = std::make_unique<fake_provider>(*this);
    } catch (...) {
        return abi::failed;
    }
    *out = instance_.get();
    return abi::ok;
}

abi::status U42_CALL fake_provider::init(abi::ictx* ctx) noexcept
{
    ++init_calls;
    ctx_ = ctx;
    return ctx == nullptr ? abi::invalid_argument : abi::ok;
}

abi::status U42_CALL fake_provider::start() noexcept
{
    ++start_calls;
    if (ctx_ == nullptr) return abi::invalid_state;
    abi::icaps* caps = nullptr;
    void* raw = nullptr;
    if (ctx_->query(&abi::caps_iid, &raw) != abi::ok || raw == nullptr) return abi::failed;
    caps = static_cast<abi::icaps*>(raw);

    static const abi::method_desc methods[4] = {
        {open_method, "open", "Open a provider session", "{}", "{}"},
        {read_method, "read", "Read the named session", "{}", "{}"},
        {fail_method, "fail", "Fail after partial output", "{}", "{}"},
        {who_method, "who", "Report the serving instance tag", "{}", "{}"},
    };
    abi::caps_desc announcement{};
    announcement.method_count = 4;
    announcement.methods = methods;
    return caps->announce(&announcement);
}

abi::status U42_CALL fake_provider::stop() noexcept
{
    ++stop_calls;
    sessions_.clear(); // The provider's generation-local state dies with the instance.
    return abi::ok;
}

void U42_CALL fake_provider::destroy() noexcept
{
    ++destroy_calls;
    destroyed = true;
    ctx_ = nullptr;
}

abi::status U42_CALL fake_provider::query(const abi::iid* type, void** out) noexcept
{
    if (out == nullptr || type == nullptr) {
        ++query_null_calls;
        return abi::invalid_argument;
    }
    *out = nullptr;
    if (*type == abi::invoke_iid) {
        ++query_invoke_calls;
        *out = static_cast<abi::iinvoke*>(this);
        return abi::ok;
    }
    // The host must never ask a business plugin for another host service through this hook.
    ++query_other_calls;
    return abi::unsupported;
}

void fake_provider::run_entry_probes()
{
    if (probe.release_caps != nullptr) {
        probe.release_ran = true;
        probe.release_status = probe.release_caps->release(probe.release_token);
        probe.release_caps = nullptr; // Consume the probe so later calls stay clean.
    }
    if (probe.structural_host != nullptr) {
        probe.structural_ran = true;
        u42::host& facade = *probe.structural_host;
        if (!probe.deferred_target.empty()) probe.deferred_status = facade.unload(probe.deferred_target);
        probe.unknown_status = facade.unload(probe.unknown_target);
        probe.shutdown_status = facade.shutdown();
        probe.start_status = facade.start();
        if (probe.add_target != nullptr) probe.add_status = facade.add(probe.add_target);
        probe.structural_host = nullptr;
    }
}

abi::status U42_CALL fake_provider::invoke(abi::method_id method, abi::bytes args,
                                           abi::iwriter* result) noexcept
{
    ++invoke_calls;
    run_entry_probes();
    switch (method) {
        case open_method: {
            const std::uint64_t id = ++next_session_;
            sessions_[id] = "session-" + std::to_string(id);
            return write_text(result, "S" + std::to_string(id));
        }
        case read_method: {
            std::uint64_t id = 0;
            if (!parse_session(text_of(args), id)) return abi::invalid_argument;
            const auto found = sessions_.find(id);
            if (found == sessions_.end()) return abi::not_found;
            return write_text(result, owner_.tag() + "#" + found->second);
        }
        case who_method:
            return write_text(result, owner_.tag());
        case fail_method:
            (void)write_text(result, "partial");
            return abi::failed;
        default:
            return abi::not_found;
    }
}

/* ------------------------------------------------------------------ *
 * Consumer fixture: lease owner, revoker and test-driven business client.
 * ------------------------------------------------------------------ */

/**
 * @brief Capability sink that records the version each notice reported.
 *
 * A notice must carry the version of the instance that queued it, so a later same-identity instance
 * can never rewrite an older notification.
 */
struct cap_sink_probe final : abi::icap_sink {
    struct entry {
        std::string provider;
        bool available = false;
        abi::plugin_version version{};
        std::uint32_t method_count = 0;
    };

    std::vector<entry> entries;

    void U42_CALL on_capability(const abi::cap_event* value) noexcept override
    {
        if (value == nullptr) return;
        try {
            entry row;
            if (value->plug_id != nullptr) row.provider = value->plug_id;
            row.available = value->available != 0;
            row.version = value->version;
            row.method_count = value->capabilities.method_count;
            entries.push_back(std::move(row));
        } catch (...) {
            // A noexcept callback must not unwind into the dispatcher.
        }
    }

    /** @brief Number of recorded notices for one provider identity. */
    std::size_t count_for(const std::string& provider) const noexcept
    {
        std::size_t total = 0;
        for (const entry& row : entries) {
            if (row.provider == provider) ++total;
        }
        return total;
    }

    /** @brief Most recent notice for one provider identity, or null when none was recorded. */
    const entry* last_for(const std::string& provider) const noexcept
    {
        for (auto row = entries.rbegin(); row != entries.rend(); ++row) {
            if (row->provider == provider) return &*row;
        }
        return nullptr;
    }
};

class consumer_factory;

/**
 * @brief One fake consumer instance.
 *
 * All business traffic goes through the host services resolved from its own ictx: acquire() for the
 * borrow and call_name()/call_id() with the credential. The instance keeps no provider pointer and
 * no binding of any kind, so there is no hidden lookup layer for the removed ABI to survive in.
 */
class fake_consumer final : public abi::iplug, public abi::irevoker {
public:
    /** @brief Create an empty consumer; the factory owns its lifetime. */
    fake_consumer() noexcept = default;
    fake_consumer(const fake_consumer&) = delete;
    fake_consumer& operator=(const fake_consumer&) = delete;

    // iplug
    abi::status U42_CALL init(abi::ictx* ctx) noexcept override;
    abi::status U42_CALL start() noexcept override;
    abi::status U42_CALL stop() noexcept override;
    void U42_CALL destroy() noexcept override;
    abi::status U42_CALL query(const abi::iid* type, void** out) noexcept override;
    // irevoker
    void U42_CALL on_revoke(abi::token credential) noexcept override;

    // Test-driven business operations: acquire once, then call with the credential.
    /** @brief Acquire one lease with an explicit range; a new lease drops any old session state. */
    abi::status acquire_lease(const char* provider_id, abi::version_range allowed);
    /** @brief Invoke the "open" method by name through the held lease. */
    abi::status open_session(std::string* out);
    /**
     * @brief Open a session while delivering the provider output through a caller-owned writer.
     *
     * @param writer Borrowed writer receiving the provider output.
     * @param out Target string of that writer; the returned session token is read from it.
     */
    abi::status open_session(abi::iwriter* writer, std::string* out);
    /** @brief Read the currently held session; invalid_state when no session is held. */
    abi::status read_current(std::string* out);
    /** @brief Read an explicitly named session token by method name. */
    abi::status read_id(const std::string& session_id, std::string* out);
    /** @brief Read an explicitly named session token by numeric method id. */
    abi::status read_id_numeric(const std::string& session_id, std::string* out);
    /** @brief Invoke "who" by name through the held lease. */
    abi::status who(std::string* out);
    /** @brief Invoke "who" by numeric id through the held lease. */
    abi::status who_id(std::string* out);
    /** @brief Invoke the failing method through the held lease. */
    abi::status invoke_fail(std::string* out);
    /** @brief Invoke any name with an arbitrary credential through this instance's icalls. */
    abi::status call_name_with(abi::token credential, const char* name, abi::bytes args,
                               std::string* out);
    /** @brief Invoke any id with an arbitrary credential through this instance's icalls. */
    abi::status call_id_with(abi::token credential, abi::method_id method, abi::bytes args,
                             std::string* out);
    /** @brief Return the lease the instance holds. */
    abi::status release_lease();
    /** @brief Return an arbitrary credential through this instance's own icaps. */
    abi::status release_token(abi::token credential);

    // Observations.
    abi::status caps_status = abi::failed;
    abi::status calls_status = abi::failed;
    abi::status diag_status = abi::failed;
    abi::status announce_status = abi::failed;
    abi::status last_acquire = abi::failed;
    abi::status init_watch = abi::failed;
    std::uint64_t init_calls = 0;
    std::uint64_t start_calls = 0;
    std::uint64_t stop_calls = 0;
    std::uint64_t destroy_calls = 0;
    std::uint64_t plugin_query_calls = 0;
    bool destroyed = false;

    /** @brief Provider identity used by the self-probes; set before start() when it matters. */
    std::string probe_provider{"lease.probe.absent"};
    /** @brief Status of the lease attempted while Initializing; refused by the state gate. */
    abi::status init_acquire = abi::ok;
    /** @brief Borrow of that refused attempt; both fields must be cleared. */
    abi::borrow init_borrow{};
    /** @brief Status of the business call attempted while Initializing. */
    abi::status init_call = abi::ok;
    /** @brief Status of the lease attempted while Starting; refused by the state gate. */
    abi::status start_acquire = abi::ok;
    /** @brief Borrow of that refused attempt; both fields must be cleared. */
    abi::borrow start_borrow{};
    /** @brief Status of the business call attempted while Starting. */
    abi::status start_call = abi::ok;
    /** @brief True once stop() probed a call with a live lease it still owned. */
    bool stop_probe_ran = false;
    /** @brief Status of the call attempted from a non-Active instance. */
    abi::status stop_call = abi::ok;
    /** @brief Status of returning the lease from stop(); release stays legal. */
    abi::status stop_release = abi::ok;
    /** @brief Capability notices observed through the consumer's own watch. */
    cap_sink_probe sink;

    /** @brief True once the capability and invocation services resolved. */
    bool ready() const noexcept { return caps_ != nullptr && calls_ != nullptr; }
    /** @brief Credential of the currently held lease, or zero. */
    abi::token token() const noexcept { return lease_.credential; }
    /** @brief Version the host granted with the currently held lease, or 0.0.0. */
    abi::plugin_version lease_version() const noexcept { return lease_.version; }
    bool holds_lease() const noexcept { return lease_.credential.value != 0; }
    bool session_valid() const noexcept { return session_valid_; }
    /** @brief Capability service of this instance, used by the lease-busy probes. */
    abi::icaps* caps_service() const noexcept { return caps_; }
    std::uint64_t revoke_calls() const noexcept { return revoke_calls_; }
    abi::status revoke_release_status() const noexcept { return revoke_release_status_; }
    /** @brief Credential of the most recent on_revoke() call. */
    abi::token revoke_token() const noexcept { return last_revoked_; }

private:
    /** @brief Invoke the "open" method under the held lease, storing the session on success. */
    abi::status open_with(abi::iwriter* writer, std::string* out);
    /** @brief Attempt a lease from a lifecycle phase that must refuse it, clearing the output. */
    void probe_acquire(abi::status& observed, abi::borrow& value) noexcept;
    /** @brief Attempt a business call from a phase that must refuse it. */
    void probe_call(abi::status& observed, abi::token credential) noexcept;

    abi::ictx* ctx_ = nullptr;
    abi::icaps* caps_ = nullptr;
    abi::icalls* calls_ = nullptr;
    abi::idiag* diag_ = nullptr;
    abi::borrow lease_{};
    abi::token watch_token_{};
    abi::version_range probe_range_{pv(1, 0, 0), kFull};
    std::string session_id_;
    bool session_valid_ = false;
    std::uint64_t revoke_calls_ = 0;
    abi::status revoke_release_status_ = abi::ok;
    abi::token last_revoked_{};
};

/** @brief Library-owned static factory for one consumer identity. */
class consumer_factory final : public abi::iplug_fty {
public:
    /** @brief Build one consumer type with immutable metadata. */
    explicit consumer_factory(std::string plug_id, abi::plugin_version version = pv(1, 0, 0));

    abi::status U42_CALL describe(const abi::plug_desc** out) noexcept override;
    abi::status U42_CALL create(abi::iplug** out) noexcept override;

    /** @brief Live instance; the test drives its business operations through it. */
    fake_consumer& instance() const noexcept { return *instance_; }

private:
    std::string plug_id_;
    abi::plugin_version version_{};
    abi::plug_desc desc_{};
    std::unique_ptr<fake_consumer> instance_;
};

consumer_factory::consumer_factory(std::string plug_id, abi::plugin_version version)
    : plug_id_(std::move(plug_id)), version_(version)
{
    desc_.struct_size = sizeof(abi::plug_desc);
    desc_.reserved = 0;
    desc_.plug_id = plug_id_.c_str();
    desc_.version = version_;
    desc_.priority = 10; // Providers publish before consumers run.
    desc_.before_count = 0;
    desc_.before = nullptr;
    desc_.after_count = 0;
    desc_.after = nullptr;
}

abi::status U42_CALL consumer_factory::describe(const abi::plug_desc** out) noexcept
{
    if (out == nullptr) return abi::invalid_argument;
    *out = &desc_;
    return abi::ok;
}

abi::status U42_CALL consumer_factory::create(abi::iplug** out) noexcept
{
    if (out == nullptr) return abi::invalid_argument;
    *out = nullptr;
    if (instance_ != nullptr) {
        if (!instance_->destroyed) return abi::invalid_state;
        instance_.reset();
    }
    try {
        instance_ = std::make_unique<fake_consumer>();
    } catch (...) {
        return abi::failed;
    }
    *out = instance_.get();
    return abi::ok;
}

void fake_consumer::probe_acquire(abi::status& observed, abi::borrow& value) noexcept
{
    // Poisoned output: a refusal must clear credential and version.
    value = abi::borrow{abi::token{0xF00D}, pv(1, 2, 3)};
    observed = caps_->acquire(probe_provider.c_str(), &probe_range_, this, &value);
}

void fake_consumer::probe_call(abi::status& observed, abi::token credential) noexcept
{
    std::string scratch;
    u42::sdk::string_writer writer(&scratch, 64);
    observed = calls_->call_name(credential, "who", abi::bytes{nullptr, 0}, &writer);
}

abi::status U42_CALL fake_consumer::init(abi::ictx* ctx) noexcept
{
    ++init_calls;
    ctx_ = ctx;
    if (ctx == nullptr) return abi::invalid_argument;
    void* raw = nullptr;
    caps_status = ctx->query(&abi::caps_iid, &raw);
    caps_ = caps_status == abi::ok ? static_cast<abi::icaps*>(raw) : nullptr;
    raw = nullptr;
    calls_status = ctx->query(&abi::calls_iid, &raw);
    calls_ = calls_status == abi::ok ? static_cast<abi::icalls*>(raw) : nullptr;
    raw = nullptr;
    diag_status = ctx->query(&abi::diag_iid, &raw);
    diag_ = diag_status == abi::ok ? static_cast<abi::idiag*>(raw) : nullptr;
    if (caps_ == nullptr || calls_ == nullptr) return abi::failed;

    // Registering a watch while Initializing is legal: it stores the sink and queues the current
    // snapshot; no callback runs inline here.
    init_watch = caps_->watch(&sink, &watch_token_);

    // The mock obeys the lifecycle gate instead of ignoring it: an Initializing instance may not
    // take a lease and may not run a business call. Both refusals are recorded, and the refused
    // acquire keeps no credential.
    probe_acquire(init_acquire, init_borrow);
    probe_call(init_call, abi::token{0xDEAD0001});
    return abi::ok;
}

abi::status U42_CALL fake_consumer::start() noexcept
{
    ++start_calls;
    if (caps_ == nullptr) return abi::invalid_state;
    // A consumer without business methods announces an empty method set: that is the only legal
    // announcement here, and the host publishes it without querying the instance.
    abi::caps_desc announcement{};
    announce_status = caps_->announce(&announcement);
    // Starting is still not Active: neither a lease nor a call may start here.
    probe_acquire(start_acquire, start_borrow);
    probe_call(start_call, abi::token{0xDEAD0002});
    return announce_status;
}

abi::status U42_CALL fake_consumer::stop() noexcept
{
    ++stop_calls;
    // A non-Active instance may not run business methods, but it must still be able to return the
    // lease it holds. The mock probes both so the test can assert the exact split.
    if (lease_.credential.value != 0 && calls_ != nullptr) {
        stop_probe_ran = true;
        std::string scratch;
        u42::sdk::string_writer writer(&scratch, 64);
        stop_call = calls_->call_name(lease_.credential, "who", abi::bytes{nullptr, 0}, &writer);
        stop_release = release_lease();
    }
    session_valid_ = false;
    session_id_.clear();
    return abi::ok;
}

void U42_CALL fake_consumer::destroy() noexcept
{
    ++destroy_calls;
    destroyed = true;
    ctx_ = nullptr;
}

abi::status U42_CALL fake_consumer::query(const abi::iid*, void** out) noexcept
{
    ++plugin_query_calls; // The host must never query a consumer without business methods.
    if (out != nullptr) *out = nullptr;
    return abi::unsupported;
}

void U42_CALL fake_consumer::on_revoke(abi::token credential) noexcept
{
    ++revoke_calls_;
    last_revoked_ = credential;
    if (caps_ == nullptr || credential.value == 0 || lease_.credential.value != credential.value) {
        revoke_release_status_ = abi::stale;
        return;
    }
    // Stop dependent business and drop the generation-local session first, then return the
    // credential. The old session state is never carried across the revocation.
    session_valid_ = false;
    session_id_.clear();
    const abi::status returned = caps_->release(credential);
    revoke_release_status_ = returned;
    if (returned == abi::ok || returned == abi::stale) lease_ = abi::borrow{};
}

abi::status fake_consumer::acquire_lease(const char* provider_id, abi::version_range allowed)
{
    if (caps_ == nullptr) return abi::invalid_state;
    abi::borrow value = poisoned();
    const abi::status acquired = caps_->acquire(provider_id, &allowed, this, &value);
    last_acquire = acquired;
    if (acquired != abi::ok) {
        // A refused acquire must have cleared the whole output; the local lease is untouched.
        CHECK(cleared(value));
        return acquired;
    }
    lease_ = value;
    session_valid_ = false; // A fresh credential never revives the previous session.
    session_id_.clear();
    return abi::ok;
}

abi::status fake_consumer::open_with(abi::iwriter* writer, std::string* out)
{
    if (calls_ == nullptr) return abi::invalid_state;
    if (lease_.credential.value == 0) return abi::invalid_state;
    const abi::status invoked =
        calls_->call_name(lease_.credential, "open", abi::bytes{nullptr, 0}, writer);
    if (invoked == abi::ok && out != nullptr) {
        session_id_ = *out;
        session_valid_ = true;
    }
    return invoked;
}

abi::status fake_consumer::open_session(std::string* out)
{
    u42::sdk::string_writer writer(out, 4096);
    return open_with(&writer, out);
}

abi::status fake_consumer::open_session(abi::iwriter* writer, std::string* out)
{
    return open_with(writer, out);
}

abi::status fake_consumer::read_current(std::string* out)
{
    if (!session_valid_ || session_id_.empty()) return abi::invalid_state;
    return read_id(session_id_, out);
}

abi::status fake_consumer::call_name_with(abi::token credential, const char* name, abi::bytes args,
                                          std::string* out)
{
    if (calls_ == nullptr) return abi::invalid_state;
    u42::sdk::string_writer writer(out, 4096);
    return calls_->call_name(credential, name, args, &writer);
}

abi::status fake_consumer::call_id_with(abi::token credential, abi::method_id method, abi::bytes args,
                                        std::string* out)
{
    if (calls_ == nullptr) return abi::invalid_state;
    u42::sdk::string_writer writer(out, 4096);
    return calls_->call_id(credential, method, args, &writer);
}

abi::status fake_consumer::read_id(const std::string& session_id, std::string* out)
{
    if (lease_.credential.value == 0) return abi::invalid_state;
    return call_name_with(lease_.credential, "read", as_bytes(session_id), out);
}

abi::status fake_consumer::read_id_numeric(const std::string& session_id, std::string* out)
{
    if (lease_.credential.value == 0) return abi::invalid_state;
    return call_id_with(lease_.credential, read_method, as_bytes(session_id), out);
}

abi::status fake_consumer::who(std::string* out)
{
    return call_name_with(lease_.credential, "who", abi::bytes{nullptr, 0}, out);
}

abi::status fake_consumer::who_id(std::string* out)
{
    return call_id_with(lease_.credential, who_method, abi::bytes{nullptr, 0}, out);
}

abi::status fake_consumer::invoke_fail(std::string* out)
{
    return call_name_with(lease_.credential, "fail", abi::bytes{nullptr, 0}, out);
}

abi::status fake_consumer::release_lease()
{
    if (caps_ == nullptr || lease_.credential.value == 0) return abi::invalid_state;
    const abi::status returned = caps_->release(lease_.credential);
    if (returned == abi::ok || returned == abi::stale) lease_ = abi::borrow{};
    return returned;
}

abi::status fake_consumer::release_token(abi::token credential)
{
    if (caps_ == nullptr) return abi::invalid_state;
    return caps_->release(credential);
}

/* ------------------------------------------------------------------ *
 * Output writer that probes the lease while the result is delivered.
 * ------------------------------------------------------------------ */

/**
 * @brief Caller-owned writer that tries to return a lease during the output callback.
 *
 * The host delivers the provider result through this writer while the lease still counts an
 * in-flight call, so the release attempt must be refused with busy.
 */
class probe_writer final : public abi::iwriter {
public:
    std::string value;
    abi::status result = abi::ok;
    abi::icaps* release_caps = nullptr;
    abi::token release_token{};
    bool release_ran = false;
    abi::status release_status = abi::ok;

    abi::status U42_CALL write(abi::bytes data) noexcept override
    {
        if (release_caps != nullptr) {
            release_ran = true;
            release_status = release_caps->release(release_token);
            release_caps = nullptr; // Consume the probe so later deliveries stay clean.
        }
        if (result != abi::ok) return result;
        if (data.data == nullptr && data.size != 0) return result = abi::invalid_argument;
        try {
            if (data.size != 0) {
                value.append(static_cast<const char*>(data.data), static_cast<std::size_t>(data.size));
            }
        } catch (...) {
            return result = abi::failed;
        }
        return abi::ok;
    }
};

/* ------------------------------------------------------------------ *
 * Administration-side receivers.
 * ------------------------------------------------------------------ */

/**
 * @brief Revoker that deliberately refuses to return credentials.
 *
 * A lease nobody returns is what pins its provider, so this receiver is how the tests observe that
 * an unreturned credential blocks teardown and that only the owning context can release it.
 */
struct ignoring_revoker final : abi::irevoker {
    std::uint64_t on_revoke_calls = 0;
    abi::token last{};

    void U42_CALL on_revoke(abi::token credential) noexcept override
    {
        ++on_revoke_calls;
        last = credential;
        // Deliberately not returned: the owning context must call release() explicitly.
    }
};

/**
 * @brief Revoker that tries to re-enter host structure while it is being called.
 *
 * The admin lease has no plugin record, so revoke() runs this callback under the engine depth guard;
 * every structural operation must be refused instead of running re-entrantly.
 */
struct structural_revoker final : abi::irevoker {
    u42::host* host = nullptr;
    std::string victim;
    abi::iplug_fty* spare = nullptr;
    std::uint64_t calls = 0;
    abi::status shutdown_status = abi::ok;
    abi::status start_status = abi::ok;
    abi::status unload_status = abi::ok;
    abi::status add_status = abi::ok;

    void U42_CALL on_revoke(abi::token credential) noexcept override
    {
        ++calls;
        (void)credential;
        if (host == nullptr) return;
        shutdown_status = host->shutdown();
        start_status = host->start();
        unload_status = host->unload(victim);
        if (spare != nullptr) add_status = host->add(spare);
        // Deliberately not returned: the outer teardown must report busy and retain the lease.
    }
};

/* ------------------------------------------------------------------ *
 * Rack fixture.
 * ------------------------------------------------------------------ */

/**
 * @brief One host plus the in-process factories it borrows.
 *
 * Member order matters: the factories are declared before host_, so host_ is destroyed first and
 * every plugin callback during shutdown still reaches a live factory.
 */
class fixture {
public:
    fixture() = default;

    /** @brief Create, own and register one provider factory. */
    provider_factory& provider(std::string plug_id, abi::plugin_version version, std::string tag)
    {
        providers_.push_back(std::make_unique<provider_factory>(std::move(plug_id), version,
                                                                std::move(tag)));
        return *providers_.back();
    }

    /** @brief Create, own and register one consumer factory. */
    consumer_factory& consumer(std::string plug_id)
    {
        consumers_.push_back(std::make_unique<consumer_factory>(std::move(plug_id)));
        return *consumers_.back();
    }

    /** @brief The rack under test. */
    u42::host& host() { return *host_; }

    /** @brief Run the two-phase start over every record staged so far. */
    abi::status start() { return host_->start(); }

    /** @brief True while the identity is registered with the host. */
    bool alive(const std::string& plug_id) const
    {
        const std::vector<std::string> ids = host_->plugins();
        return std::find(ids.begin(), ids.end(), plug_id) != ids.end();
    }

private:
    std::vector<std::unique_ptr<provider_factory>> providers_;
    std::vector<std::unique_ptr<consumer_factory>> consumers_;
    std::unique_ptr<u42::host> host_ = std::make_unique<u42::host>();
};

/* ------------------------------------------------------------------ *
 * Compile-time proof that the fixtures keep no binding-like member.
 * ------------------------------------------------------------------ */

// The removed layer must not survive inside the test helpers either: a local bind/unbind shim would
// make the acceptance meaningless, so the same member probes are applied to the fixture types.
static_assert(!has_member_bind<fake_consumer>::value &&
              !has_member_unbind<fake_consumer>::value &&
              !has_member_bind_name<fake_consumer>::value,
              "the consumer fixture must not keep a binding-like helper layer");
static_assert(!has_member_bind<fake_provider>::value && !has_member_unbind<fake_provider>::value &&
              !has_member_bind_name<fake_provider>::value,
              "the provider fixture must not keep a binding-like helper layer");

/* ------------------------------------------------------------------ *
 * Cases.
 * ------------------------------------------------------------------ */

/**
 * @brief Case 1: inclusive endpoints, exact match, reversed range and out-of-range refusals.
 *
 * Covers the v2 "contract mismatch" case: nothing negotiates a contract here, so the refusals are
 * the invalid_argument of a reversed range and the unsupported of an actual version outside the
 * caller's explicit range. Every refusal leaves the borrow cleared and no lease behind.
 */
void case_version_range_accepts_and_rejects()
{
    g_current_test = "version_range_accepts_and_rejects";
    fixture f;
    provider_factory& provider = f.provider("ver.one", pv(1, 5, 2), "one");
    ignoring_revoker revoker;
    CHECK(f.host().add(&provider) == abi::ok);

    // Staged but not started: nothing is published, so an otherwise legal range is not found and no
    // lease is created.
    abi::borrow premature = poisoned();
    CHECK(f.host().acquire("ver.one", abi::exact_version(pv(1, 5, 2)), &revoker, &premature) ==
          abi::not_found);
    CHECK(cleared(premature));

    CHECK(f.start() == abi::ok);

    const abi::version_range exact = abi::exact_version(pv(1, 5, 2));
    const abi::version_range upward = between(pv(1, 5, 2), pv(2, 0, 0));
    const abi::version_range downward = between(pv(1, 0, 0), pv(1, 5, 2));

    // The exact version, the range minimum and the range maximum are all inclusive.
    abi::borrow lease{};
    CHECK(f.host().acquire("ver.one", exact, &revoker, &lease) == abi::ok);
    CHECK(lease.credential.value != 0 && lease.version == pv(1, 5, 2));
    CHECK(f.host().release(lease.credential) == abi::ok);
    CHECK(f.host().acquire("ver.one", upward, &revoker, &lease) == abi::ok);
    CHECK(lease.version == pv(1, 5, 2));
    CHECK(f.host().release(lease.credential) == abi::ok);
    CHECK(f.host().acquire("ver.one", downward, &revoker, &lease) == abi::ok);
    CHECK(lease.version == pv(1, 5, 2));
    CHECK(f.host().release(lease.credential) == abi::ok);

    // A reversed range is an argument error, not a version mismatch.
    abi::borrow out = poisoned();
    CHECK(f.host().acquire("ver.one", between(pv(2, 0, 0), pv(1, 0, 0)), &revoker, &out) ==
          abi::invalid_argument);
    CHECK(cleared(out));
    // An actual version above the maximum or below the minimum is unsupported, again with no lock.
    out = poisoned();
    CHECK(f.host().acquire("ver.one", between(pv(1, 0, 0), pv(1, 5, 1)), &revoker, &out) ==
          abi::unsupported);
    CHECK(cleared(out));
    out = poisoned();
    CHECK(f.host().acquire("ver.one", between(pv(1, 5, 3), pv(2, 0, 0)), &revoker, &out) ==
          abi::unsupported);
    CHECK(cleared(out));
    out = poisoned();
    CHECK(f.host().acquire("ver.one", abi::exact_version(pv(1, 5, 3)), &revoker, &out) ==
          abi::unsupported);
    CHECK(cleared(out));
    // Required arguments are validated: a null receiver, a null output and an empty identity.
    CHECK(f.host().acquire("ver.one", exact, nullptr, &out) == abi::invalid_argument);
    CHECK(cleared(out));
    CHECK(f.host().acquire("ver.one", exact, &revoker, nullptr) == abi::invalid_argument);
    out = poisoned();
    CHECK(f.host().acquire("", exact, &revoker, &out) == abi::invalid_argument);
    CHECK(cleared(out));

    // None of the refusals produced a lease: the provider unloads without revoking anyone.
    CHECK(f.host().unload("ver.one") == abi::ok);
    CHECK(revoker.on_revoke_calls == 0);
    CHECK(f.host().shutdown() == abi::ok);
}

/** @brief Case 1b: numeric triple ordering and the uint32 maximum boundary. */
void case_version_numeric_order_and_uint32_bounds()
{
    g_current_test = "version_numeric_order_and_uint32_bounds";
    fixture f;
    provider_factory& ten = f.provider("ver.ten", pv(1, 10, 0), "ten");
    provider_factory& ceiling = f.provider("ver.max", pv(1, UINT32_MAX, UINT32_MAX), "max");
    provider_factory& full = f.provider("ver.ceiling", kFull, "ceiling");
    ignoring_revoker revoker;
    CHECK(f.host().add(&ten) == abi::ok);
    CHECK(f.host().add(&ceiling) == abi::ok);
    CHECK(f.host().add(&full) == abi::ok);
    CHECK(f.start() == abi::ok);

    // 1.10.0 sorts after 1.2.0 by numeric triple order, not by text: [1.2.0, 1.10.0] contains it
    // while [1.0.0, 1.9.9] does not.
    abi::borrow lease{};
    CHECK(f.host().acquire("ver.ten", between(pv(1, 2, 0), pv(1, 10, 0)), &revoker, &lease) ==
          abi::ok);
    CHECK(lease.version == pv(1, 10, 0));
    CHECK(f.host().release(lease.credential) == abi::ok);
    abi::borrow out = poisoned();
    CHECK(f.host().acquire("ver.ten", between(pv(1, 0, 0), pv(1, 9, 9)), &revoker, &out) ==
          abi::unsupported);
    CHECK(cleared(out));
    out = poisoned();
    CHECK(f.host().acquire("ver.ten", between(pv(1, 10, 1), pv(1, 20, 0)), &revoker, &out) ==
          abi::unsupported);
    CHECK(cleared(out));

    // The uint32 maximum is a legal minor/patch value and a legal endpoint on both sides, with no
    // overflow in the comparison.
    CHECK(f.host().acquire("ver.max", between(pv(1, 0, 0), pv(1, UINT32_MAX, UINT32_MAX)), &revoker,
                           &lease) == abi::ok);
    CHECK(lease.version == pv(1, UINT32_MAX, UINT32_MAX));
    CHECK(f.host().release(lease.credential) == abi::ok);
    CHECK(f.host().acquire("ver.max", between(pv(0, 0, 0), kFull), &revoker, &lease) == abi::ok);
    CHECK(lease.version == pv(1, UINT32_MAX, UINT32_MAX));
    CHECK(f.host().release(lease.credential) == abi::ok);
    CHECK(f.host().acquire("ver.max", abi::exact_version(pv(1, UINT32_MAX, UINT32_MAX)), &revoker,
                           &lease) == abi::ok);
    CHECK(f.host().release(lease.credential) == abi::ok);
    out = poisoned();
    CHECK(f.host().acquire("ver.max", abi::exact_version(pv(2, 0, 0)), &revoker, &out) ==
          abi::unsupported);
    CHECK(cleared(out));

    // An instance at the absolute maximum triple is accepted only by a range that really contains
    // it: a range ending at 1.MAX.MAX must not match major MAX.
    CHECK(f.host().acquire("ver.ceiling", between(pv(1, 0, 0), kFull), &revoker, &lease) == abi::ok);
    CHECK(lease.version == kFull);
    CHECK(f.host().release(lease.credential) == abi::ok);
    CHECK(f.host().acquire("ver.ceiling", abi::exact_version(kFull), &revoker, &lease) == abi::ok);
    CHECK(f.host().release(lease.credential) == abi::ok);
    out = poisoned();
    CHECK(f.host().acquire("ver.ceiling", between(pv(1, 0, 0), pv(1, UINT32_MAX, UINT32_MAX)),
                           &revoker, &out) == abi::unsupported);
    CHECK(cleared(out));

    CHECK(f.host().unload("ver.ten") == abi::ok);
    CHECK(f.host().unload("ver.max") == abi::ok);
    CHECK(f.host().unload("ver.ceiling") == abi::ok);
    CHECK(f.host().shutdown() == abi::ok);
}

/**
 * @brief Case 1c: a legal 0.0.0 and an explicitly requested cross-major range.
 *
 * Success is credential != 0, never version != 0, and the host does not reject a range that crosses
 * a major boundary when the caller states it; the borrow reports the actual version to branch on.
 */
void case_zero_version_and_cross_major_actual()
{
    g_current_test = "zero_version_and_cross_major_actual";
    fixture f;
    provider_factory& zero = f.provider("ver.zero", pv(0, 0, 0), "zero");
    provider_factory& cross = f.provider("ver.cross", pv(2, 7, 3), "cross");
    ignoring_revoker revoker;
    CHECK(f.host().add(&zero) == abi::ok);
    CHECK(f.host().add(&cross) == abi::ok);
    CHECK(f.start() == abi::ok);

    abi::borrow lease{};
    CHECK(f.host().acquire("ver.zero", abi::exact_version(pv(0, 0, 0)), &revoker, &lease) == abi::ok);
    CHECK(lease.credential.value != 0);
    CHECK(lease.version == pv(0, 0, 0));
    abi::plugin_version seen = poisoned().version;
    CHECK(f.host().version("ver.zero", &seen) == abi::ok);
    CHECK(seen == pv(0, 0, 0));
    // A null output is an argument error even for an otherwise valid call.
    CHECK(f.host().call(lease.credential, std::string("who"), abi::bytes{nullptr, 0}, nullptr) ==
          abi::invalid_argument);
    CHECK(f.host().release(lease.credential) == abi::ok);
    // A refused 0.0.0 acquire is distinguished by its cleared, zero credential.
    abi::borrow out = poisoned();
    CHECK(f.host().acquire("ver.zero", between(pv(0, 0, 1), pv(1, 0, 0)), &revoker, &out) ==
          abi::unsupported);
    CHECK(cleared(out));

    CHECK(f.host().acquire("ver.cross", between(pv(1, 0, 0), pv(3, 0, 0)), &revoker, &lease) ==
          abi::ok);
    CHECK(lease.version == pv(2, 7, 3));
    CHECK(f.host().release(lease.credential) == abi::ok);
    CHECK(f.host().acquire("ver.cross", between(pv(2, 0, 0), pv(3, 0, 0)), &revoker, &lease) ==
          abi::ok);
    CHECK(lease.version == pv(2, 7, 3));
    CHECK(f.host().release(lease.credential) == abi::ok);
    out = poisoned();
    CHECK(f.host().acquire("ver.cross", between(pv(3, 0, 0), pv(3, 5, 0)), &revoker, &out) ==
          abi::unsupported);
    CHECK(cleared(out));
    // The one-shot administration call applies the same explicit range and still has no binding.
    std::string text;
    CHECK(f.host().call("ver.cross", between(pv(1, 0, 0), pv(3, 0, 0)), std::string("who"),
                        abi::bytes{nullptr, 0}, &text) == abi::ok);
    CHECK(text == "cross");

    CHECK(f.host().unload("ver.zero") == abi::ok);
    CHECK(f.host().unload("ver.cross") == abi::ok);
    CHECK(f.host().shutdown() == abi::ok);
}

/**
 * @brief Case 2: version discovery is not a lease and is not a compatibility acceptance.
 *
 * Covers the v2 "explicit discovery" case: the replacement is host::version plus the caller's own
 * range. Discovery reads the recorded metadata only, clears its output on failure and locks nothing.
 */
void case_version_discovery_no_lease()
{
    g_current_test = "version_discovery_no_lease";
    fixture f;
    provider_factory& provider = f.provider("disc.provider", pv(2, 1, 0), "disc");
    consumer_factory& factory = f.consumer("disc.consumer");
    ignoring_revoker revoker;
    CHECK(f.host().add(&provider) == abi::ok);
    CHECK(f.host().add(&factory) == abi::ok);

    // Staged only: nothing is published yet, so discovery reports not_found and clears the output.
    abi::plugin_version seen = pv(9, 9, 9);
    CHECK(f.host().version("disc.provider", &seen) == abi::not_found);
    CHECK(seen == pv(0, 0, 0));
    CHECK(f.host().version("disc.absent", &seen) == abi::not_found);
    CHECK(f.host().version("disc.provider", nullptr) == abi::invalid_argument);

    CHECK(f.start() == abi::ok);
    seen = pv(9, 9, 9);
    CHECK(f.host().version("disc.provider", &seen) == abi::ok);
    CHECK(seen == pv(2, 1, 0));
    // A published Active instance with an empty method set still reports its own version.
    seen = pv(9, 9, 9);
    CHECK(f.host().version("disc.consumer", &seen) == abi::ok);
    CHECK(seen == pv(1, 0, 0));
    // Discovery is not a lock: the provider unloads directly, without any revocation.
    CHECK(f.host().unload("disc.provider") == abi::ok);
    CHECK(revoker.on_revoke_calls == 0);
    CHECK(f.host().shutdown() == abi::ok);
}

/**
 * @brief Case 2b: the borrow version is a copy of the record, not a handle on it.
 *
 * Mutating the returned borrow or the range that was passed in must not reach the host record, the
 * version it reports, or the target a credential selects.
 */
void case_borrow_version_is_caller_copy()
{
    g_current_test = "borrow_version_is_caller_copy";
    fixture f;
    provider_factory& first = f.provider("copy.a", pv(1, 4, 0), "A");
    provider_factory& second = f.provider("copy.b", pv(1, 4, 0), "B");
    ignoring_revoker revoker;
    CHECK(f.host().add(&first) == abi::ok);
    CHECK(f.host().add(&second) == abi::ok);
    CHECK(f.start() == abi::ok);

    abi::version_range allowed = between(pv(1, 0, 0), kFull);
    abi::borrow lease_a{};
    abi::borrow lease_b{};
    CHECK(f.host().acquire("copy.a", allowed, &revoker, &lease_a) == abi::ok);
    CHECK(f.host().acquire("copy.b", allowed, &revoker, &lease_b) == abi::ok);
    CHECK(lease_a.version == pv(1, 4, 0) && lease_b.version == pv(1, 4, 0));

    std::string text;
    CHECK(f.host().call(lease_a.credential, std::string("who"), abi::bytes{nullptr, 0}, &text) ==
          abi::ok);
    CHECK(text == "A");
    CHECK(f.host().call(lease_b.credential, std::string("who"), abi::bytes{nullptr, 0}, &text) ==
          abi::ok);
    CHECK(text == "B");

    allowed = abi::exact_version(pv(9, 9, 9));
    lease_a.version = pv(8, 8, 8);
    lease_b.version = pv(7, 7, 7);
    abi::plugin_version seen = pv(9, 9, 9);
    CHECK(f.host().version("copy.a", &seen) == abi::ok);
    CHECK(seen == pv(1, 4, 0));
    seen = pv(9, 9, 9);
    CHECK(f.host().version("copy.b", &seen) == abi::ok);
    CHECK(seen == pv(1, 4, 0));
    CHECK(first.declared_version() == pv(1, 4, 0));
    CHECK(f.host().call(lease_a.credential, std::string("who"), abi::bytes{nullptr, 0}, &text) ==
          abi::ok);
    CHECK(text == "A");
    CHECK(f.host().call(lease_b.credential, std::string("who"), abi::bytes{nullptr, 0}, &text) ==
          abi::ok);
    CHECK(text == "B");
    // A fresh acquire for the same identity still returns the actual recorded version.
    abi::borrow fresh{};
    CHECK(f.host().acquire("copy.a", abi::exact_version(pv(1, 4, 0)), &revoker, &fresh) == abi::ok);
    CHECK(fresh.version == pv(1, 4, 0));
    CHECK(fresh.credential.value != lease_a.credential.value);
    CHECK(f.host().release(fresh.credential) == abi::ok);
    CHECK(f.host().release(lease_a.credential) == abi::ok);
    CHECK(f.host().release(lease_b.credential) == abi::ok);

    CHECK(f.host().unload("copy.a") == abi::ok);
    CHECK(f.host().unload("copy.b") == abi::ok);
    CHECK(f.host().shutdown() == abi::ok);
}

/**
 * @brief Case 3: a same-version reload rebuilds the session and keeps the old credential stale.
 *
 * Covers the v2 "same protocol reload" case. The version numbers are unchanged, so only the
 * generation check and the rebuild requirement can distinguish the instances.
 */
void case_same_version_reload_rebuilds_session()
{
    g_current_test = "same_version_reload_rebuilds_session";
    fixture f;
    provider_factory& provider = f.provider("reload.provider", pv(1, 0, 0), "P");
    consumer_factory& factory = f.consumer("reload.consumer");
    CHECK(f.host().add(&provider) == abi::ok);
    CHECK(f.host().add(&factory) == abi::ok);
    CHECK(f.start() == abi::ok);

    fake_consumer& consumer = factory.instance();
    CHECK(consumer.ready());
    CHECK(consumer.init_watch == abi::ok);
    // The availability notice carries the version of the instance that queued it.
    CHECK(consumer.sink.count_for("reload.provider") == 1);
    const cap_sink_probe::entry* announced = consumer.sink.last_for("reload.provider");
    CHECK(announced != nullptr && announced->available);
    CHECK(announced->version == pv(1, 0, 0));
    CHECK(announced->method_count == 4u);

    CHECK(consumer.acquire_lease("reload.provider", abi::exact_version(pv(1, 0, 0))) == abi::ok);
    CHECK(consumer.holds_lease());
    CHECK(consumer.lease_version() == pv(1, 0, 0));
    std::string first;
    CHECK(consumer.open_session(&first) == abi::ok);
    CHECK(first == "S1");
    std::string payload;
    CHECK(consumer.read_current(&payload) == abi::ok);
    CHECK(payload == "P#session-1");
    std::string who_tag;
    CHECK(consumer.who(&who_tag) == abi::ok);
    CHECK(who_tag == "P");

    const abi::token old_token = consumer.token();
    CHECK(old_token.value != 0);

    // Unloading the provider while the consumer still holds a lease revokes first.
    CHECK(f.host().unload("reload.provider") == abi::ok);
    CHECK(consumer.revoke_calls() == 1);
    CHECK(consumer.revoke_token().value == old_token.value);
    CHECK(consumer.revoke_release_status() == abi::ok);
    CHECK(!consumer.holds_lease());
    CHECK(!consumer.session_valid());
    CHECK(consumer.token().value == 0);
    CHECK(consumer.read_current(&payload) == abi::invalid_state);
    CHECK(consumer.release_token(old_token) == abi::stale);
    std::string stale_out;
    CHECK(consumer.call_name_with(old_token, "who", abi::bytes{nullptr, 0}, &stale_out) ==
          abi::stale);
    CHECK(stale_out.empty());
    // The withdrawal notice reports the same version as the availability notice.
    CHECK(consumer.sink.count_for("reload.provider") == 2);
    const cap_sink_probe::entry* withdrawn = consumer.sink.last_for("reload.provider");
    CHECK(withdrawn != nullptr && !withdrawn->available && withdrawn->version == pv(1, 0, 0));

    // Reload the identical identity and version: a new generation must rebuild the session even
    // though every version number is unchanged.
    provider_factory& reloaded = f.provider("reload.provider", pv(1, 0, 0), "P2");
    CHECK(f.host().add(&reloaded) == abi::ok);
    CHECK(f.host().start() == abi::ok);
    CHECK(f.alive("reload.provider"));
    CHECK(consumer.sink.count_for("reload.provider") == 3);
    const cap_sink_probe::entry* again = consumer.sink.last_for("reload.provider");
    CHECK(again != nullptr && again->available && again->version == pv(1, 0, 0));

    CHECK(consumer.acquire_lease("reload.provider", abi::exact_version(pv(1, 0, 0))) == abi::ok);
    CHECK(consumer.token().value != old_token.value);
    CHECK(!consumer.session_valid());
    CHECK(consumer.read_current(&payload) == abi::invalid_state);
    // The new instance has no sessions: the old business ID is unknown before reconstruction.
    CHECK(consumer.read_id(first, &payload) == abi::not_found);
    std::string second;
    CHECK(consumer.open_session(&second) == abi::ok);
    CHECK(second == first); // Business IDs may repeat; the host must not rely on their uniqueness.
    std::string rebuilt;
    CHECK(consumer.read_current(&rebuilt) == abi::ok);
    CHECK(rebuilt == "P2#session-1");
    // The old credential stays stale even after the new session was opened.
    CHECK(consumer.release_token(old_token) == abi::stale);
    CHECK(consumer.call_id_with(old_token, who_method, abi::bytes{nullptr, 0}, &stale_out) ==
          abi::stale);
    CHECK(consumer.call_name_with(old_token, "who", abi::bytes{nullptr, 0}, &stale_out) ==
          abi::stale);

    CHECK(consumer.release_lease() == abi::ok);
    CHECK(f.host().unload("reload.provider") == abi::ok);
    CHECK(f.host().shutdown() == abi::ok);
}

/** @brief Case 4: a major upgrade refuses the old expectation only. */
void case_major_upgrade_reload()
{
    g_current_test = "major_upgrade_reload";
    fixture f;
    provider_factory& version_one = f.provider("major.provider", pv(1, 0, 0), "one");
    ignoring_revoker revoker;
    CHECK(f.host().add(&version_one) == abi::ok);
    CHECK(f.start() == abi::ok);

    abi::plugin_version seen = pv(9, 9, 9);
    CHECK(f.host().version("major.provider", &seen) == abi::ok);
    CHECK(seen == pv(1, 0, 0));
    abi::borrow lease{};
    CHECK(f.host().acquire("major.provider", abi::exact_version(pv(1, 0, 0)), &revoker, &lease) ==
          abi::ok);
    CHECK(f.host().release(lease.credential) == abi::ok);
    CHECK(f.host().unload("major.provider") == abi::ok);

    provider_factory& version_two = f.provider("major.provider", pv(2, 0, 0), "two");
    CHECK(f.host().add(&version_two) == abi::ok);
    CHECK(f.host().start() == abi::ok);
    seen = pv(9, 9, 9);
    CHECK(f.host().version("major.provider", &seen) == abi::ok);
    CHECK(seen == pv(2, 0, 0));

    // The previously accepted exact version is refused after the upgrade: the host never silently
    // widens the caller's expectation.
    abi::borrow out = poisoned();
    CHECK(f.host().acquire("major.provider", abi::exact_version(pv(1, 0, 0)), &revoker, &out) ==
          abi::unsupported);
    CHECK(cleared(out));
    // Explicitly accepting the new version works, and the actual version is still reported so the
    // caller can branch on the major change.
    abi::borrow fresh{};
    CHECK(f.host().acquire("major.provider", abi::exact_version(pv(2, 0, 0)), &revoker, &fresh) ==
          abi::ok);
    CHECK(fresh.version == pv(2, 0, 0));
    std::string text;
    CHECK(f.host().call(fresh.credential, std::string("who"), abi::bytes{nullptr, 0}, &text) ==
          abi::ok);
    CHECK(text == "two");
    CHECK(f.host().release(fresh.credential) == abi::ok);
    CHECK(f.host().acquire("major.provider", between(pv(1, 0, 0), pv(3, 0, 0)), &revoker, &lease) ==
          abi::ok);
    CHECK(lease.version == pv(2, 0, 0));
    CHECK(f.host().release(lease.credential) == abi::ok);

    CHECK(f.host().unload("major.provider") == abi::ok);
    CHECK(f.host().shutdown() == abi::ok);
}

/** @brief Case 5: two leases have independent lifetimes and locks. */
void case_multi_lease_lifetime()
{
    g_current_test = "multi_lease_lifetime";
    fixture f;
    provider_factory& provider = f.provider("lock.provider", pv(1, 0, 0), "L");
    ignoring_revoker revoker;
    CHECK(f.host().add(&provider) == abi::ok);
    CHECK(f.start() == abi::ok);

    const abi::version_range allowed = abi::exact_version(pv(1, 0, 0));
    abi::borrow first{};
    abi::borrow second{};
    CHECK(f.host().acquire("lock.provider", allowed, &revoker, &first) == abi::ok);
    CHECK(f.host().acquire("lock.provider", allowed, &revoker, &second) == abi::ok);
    CHECK(first.credential.value != 0 && second.credential.value != 0);
    CHECK(first.credential.value != second.credential.value);

    std::string text;
    CHECK(f.host().call(first.credential, std::string("who"), abi::bytes{nullptr, 0}, &text) ==
          abi::ok);
    CHECK(text == "L");
    CHECK(f.host().call(second.credential, std::string("who"), abi::bytes{nullptr, 0}, &text) ==
          abi::ok);
    CHECK(text == "L");

    // Returning the first lease invalidates only it; the second one still locks the provider.
    CHECK(f.host().release(first.credential) == abi::ok);
    CHECK(f.host().call(first.credential, std::string("who"), abi::bytes{nullptr, 0}, &text) ==
          abi::stale);
    CHECK(text.empty());
    CHECK(f.host().call(second.credential, std::string("who"), abi::bytes{nullptr, 0}, &text) ==
          abi::ok);
    CHECK(text == "L");
    CHECK(f.host().unload("lock.provider") == abi::busy);
    CHECK(f.alive("lock.provider"));
    CHECK(revoker.on_revoke_calls == 1);
    CHECK(revoker.last.value == second.credential.value);

    CHECK(f.host().release(second.credential) == abi::ok);
    CHECK(f.host().unload("lock.provider") == abi::ok);
    CHECK(!f.alive("lock.provider"));
    CHECK(f.host().shutdown() == abi::ok);
}

/**
 * @brief Case 5b: one lease serves repeated name and numeric calls and is never consumed.
 *
 * This replaces the removed unbind test: there is no per-method binding to create or retire, so the
 * credential alone must carry every call. Both the administration lease and a plugin-owned lease
 * are checked, including failure paths, which must leave the credential owned.
 */
void case_direct_calls_do_not_consume_lease()
{
    g_current_test = "direct_calls_do_not_consume_lease";
    fixture f;
    provider_factory& admin_provider = f.provider("calls.provider", pv(3, 2, 1), "C");
    provider_factory& consumer_provider = f.provider("calls.backend", pv(1, 0, 0), "B");
    consumer_factory& factory = f.consumer("calls.consumer");
    ignoring_revoker revoker;
    CHECK(f.host().add(&admin_provider) == abi::ok);
    CHECK(f.host().add(&consumer_provider) == abi::ok);
    CHECK(f.host().add(&factory) == abi::ok);
    CHECK(f.start() == abi::ok);

    abi::borrow lease{};
    CHECK(f.host().acquire("calls.provider", abi::exact_version(pv(3, 2, 1)), &revoker, &lease) ==
          abi::ok);
    const abi::token held = lease.credential;
    std::string text;
    CHECK(f.host().call(held, std::string("who"), abi::bytes{nullptr, 0}, &text) == abi::ok);
    CHECK(text == "C");
    CHECK(f.host().call(held, static_cast<abi::method_id>(who_method), abi::bytes{nullptr, 0},
                        &text) == abi::ok);
    CHECK(text == "C");
    CHECK(f.host().call(held, std::string("open"), abi::bytes{nullptr, 0}, &text) == abi::ok);
    CHECK(text == "S1");
    const std::string session_id("S1");
    CHECK(f.host().call(held, static_cast<abi::method_id>(read_method), as_bytes(session_id),
                        &text) == abi::ok);
    CHECK(text == "C#session-1");
    CHECK(f.host().call(held, std::string("read"), as_bytes(session_id), &text) == abi::ok);
    CHECK(text == "C#session-1");
    CHECK(lease.credential.value == held.value);

    // Failed calls also leave the lease owned: an unknown name, an unknown id, a malformed argument
    // view and oversized arguments are refused without consuming the credential.
    CHECK(f.host().call(held, std::string("absent"), abi::bytes{nullptr, 0}, &text) == abi::not_found);
    CHECK(text.empty());
    CHECK(f.host().call(held, static_cast<abi::method_id>(99), abi::bytes{nullptr, 0}, &text) ==
          abi::not_found);
    CHECK(f.host().call(held, std::string("read"), abi::bytes{nullptr, 1}, &text) ==
          abi::invalid_argument);
    CHECK(f.host().call(held, std::string("read"),
                        abi::bytes{static_cast<const void*>("S1"), 1024u * 1024u + 1u}, &text) ==
          abi::limit_exceeded);
    CHECK(f.host().call(abi::token{}, std::string("who"), abi::bytes{nullptr, 0}, &text) ==
          abi::invalid_argument);
    CHECK(text.empty());
    CHECK(f.host().release(held) == abi::ok); // Still owned, so the return must succeed.

    // The same holds for a plugin-owned lease: repeated name and numeric calls, then a successful
    // return that proves no call consumed the credential.
    fake_consumer& consumer = factory.instance();
    CHECK(consumer.acquire_lease("calls.backend", abi::exact_version(pv(1, 0, 0))) == abi::ok);
    const abi::token consumer_token = consumer.token();
    std::string who;
    CHECK(consumer.who(&who) == abi::ok);
    CHECK(who == "B");
    who.clear(); // The writer appends into a caller-owned target; reset it between assertions.
    CHECK(consumer.who_id(&who) == abi::ok);
    CHECK(who == "B");
    std::string session;
    CHECK(consumer.open_session(&session) == abi::ok);
    CHECK(session == "S1");
    std::string payload;
    CHECK(consumer.read_id(session, &payload) == abi::ok);
    CHECK(payload == "B#session-1");
    payload.clear();
    CHECK(consumer.read_id_numeric(session, &payload) == abi::ok);
    CHECK(payload == "B#session-1");
    CHECK(consumer.token().value == consumer_token.value);
    CHECK(consumer.release_lease() == abi::ok);

    CHECK(f.host().unload("calls.provider") == abi::ok);
    CHECK(f.host().unload("calls.backend") == abi::ok);
    CHECK(f.host().shutdown() == abi::ok);
}

/** @brief Case 6: zero, cross-context and off-thread credentials are refused safely. */
void case_owner_zero_thread_validation()
{
    g_current_test = "owner_zero_thread_validation";
    fixture f;
    provider_factory& provider = f.provider("owner.provider", pv(1, 0, 0), "O");
    consumer_factory& factory = f.consumer("owner.consumer");
    ignoring_revoker revoker;
    CHECK(f.host().add(&provider) == abi::ok);
    CHECK(f.host().add(&factory) == abi::ok);
    CHECK(f.start() == abi::ok);

    fake_consumer& consumer = factory.instance();
    CHECK(consumer.acquire_lease("owner.provider", abi::exact_version(pv(1, 0, 0))) == abi::ok);
    std::string text;
    CHECK(consumer.open_session(&text) == abi::ok);
    CHECK(consumer.read_current(&text) == abi::ok);
    const abi::token consumer_token = consumer.token();
    CHECK(consumer_token.value != 0);

    abi::borrow admin{};
    CHECK(f.host().acquire("owner.provider", abi::exact_version(pv(1, 0, 0)), &revoker, &admin) ==
          abi::ok);
    CHECK(admin.credential.value != 0 && admin.credential.value != consumer_token.value);

    // Zero credentials, a null output and a null output write target are argument errors.
    CHECK(f.host().release(abi::token{}) == abi::invalid_argument);
    CHECK(f.host().call(abi::token{}, std::string("who"), abi::bytes{nullptr, 0}, &text) ==
          abi::invalid_argument);
    CHECK(text.empty());
    CHECK(f.host().call(admin.credential, std::string("who"), abi::bytes{nullptr, 0}, nullptr) ==
          abi::invalid_argument);
    CHECK(consumer.release_token(abi::token{}) == abi::invalid_argument);
    CHECK(consumer.call_name_with(abi::token{}, "who", abi::bytes{nullptr, 0}, &text) ==
          abi::invalid_argument);
    CHECK(text.empty());
    CHECK(consumer.call_id_with(abi::token{}, who_method, abi::bytes{nullptr, 0}, &text) ==
          abi::invalid_argument);

    // A credential of another context is refused on both sides and is never returned by the wrong
    // owner; both leases stay usable.
    CHECK(f.host().release(consumer_token) == abi::invalid_argument);
    CHECK(f.host().call(consumer_token, std::string("who"), abi::bytes{nullptr, 0}, &text) ==
          abi::invalid_argument);
    CHECK(text.empty());
    CHECK(consumer.release_token(admin.credential) == abi::invalid_argument);
    CHECK(consumer.call_name_with(admin.credential, "who", abi::bytes{nullptr, 0}, &text) ==
          abi::invalid_argument);
    CHECK(consumer.call_id_with(admin.credential, who_method, abi::bytes{nullptr, 0}, &text) ==
          abi::invalid_argument);
    CHECK(consumer.token().value == consumer_token.value);
    CHECK(f.host().call(admin.credential, std::string("who"), abi::bytes{nullptr, 0}, &text) ==
          abi::ok);
    CHECK(text == "O");
    CHECK(consumer.read_current(&text) == abi::ok);

    // Another thread is refused by both the native facade and a plugin service. The native facade
    // leaves every native output argument untouched on that refusal.
    abi::status off_release = abi::ok;
    abi::status off_service = abi::ok;
    abi::status off_call = abi::ok;
    abi::status off_version = abi::ok;
    abi::plugin_version off_seen = pv(4, 4, 4);
    abi::borrow off_acquire = poisoned();
    abi::status off_acquire_status = abi::ok;
    std::thread other([&] {
        off_release = f.host().release(admin.credential);
        off_service = consumer.release_token(consumer_token);
        off_call = consumer.call_name_with(consumer_token, "who", abi::bytes{nullptr, 0}, &text);
        off_version = f.host().version("owner.provider", &off_seen);
        off_acquire_status = f.host().acquire("owner.provider", abi::exact_version(pv(1, 0, 0)),
                                              &revoker, &off_acquire);
    });
    other.join();
    CHECK(off_release == abi::wrong_thread);
    CHECK(off_service == abi::wrong_thread);
    CHECK(off_call == abi::wrong_thread);
    CHECK(off_version == abi::wrong_thread);
    CHECK(off_acquire_status == abi::wrong_thread);
    CHECK(off_seen == pv(4, 4, 4)); // wrong_thread does not clear a native output argument.
    CHECK(off_acquire.credential.value == 0xDEADBEEF && off_acquire.version == pv(9, 9, 9));
    CHECK(consumer.token().value == consumer_token.value);
    CHECK(consumer.read_current(&text) == abi::ok);
    CHECK(f.host().call(admin.credential, std::string("who"), abi::bytes{nullptr, 0}, &text) ==
          abi::ok);
    CHECK(revoker.on_revoke_calls == 0);

    CHECK(f.host().release(admin.credential) == abi::ok);
    CHECK(consumer.release_lease() == abi::ok);
    CHECK(f.host().unload("owner.provider") == abi::ok);
    CHECK(f.host().shutdown() == abi::ok);
}

/** @brief Case 7: an in-flight call pins its lease, from invoke and from output delivery. */
void case_lease_busy_in_flight()
{
    g_current_test = "lease_busy_in_flight";
    fixture f;
    provider_factory& provider_factory_ref = f.provider("busy.provider", pv(1, 0, 0), "U");
    consumer_factory& factory = f.consumer("busy.consumer");
    CHECK(f.host().add(&provider_factory_ref) == abi::ok);
    CHECK(f.host().add(&factory) == abi::ok);
    CHECK(f.start() == abi::ok);

    fake_consumer& consumer = factory.instance();
    CHECK(consumer.acquire_lease("busy.provider", abi::exact_version(pv(1, 0, 0))) == abi::ok);
    const abi::token held = consumer.token();
    CHECK(held.value != 0);

    // The provider tries to return the consumer's lease from inside invoke(), and the writer tries
    // the same while the result is delivered. Both attempts must be refused with busy.
    fake_provider& provider = *provider_factory_ref.instance();
    provider.probe.release_caps = consumer.caps_service();
    provider.probe.release_token = held;

    probe_writer writer;
    writer.release_caps = consumer.caps_service();
    writer.release_token = held;

    CHECK(consumer.open_session(&writer, &writer.value) == abi::ok);
    CHECK(provider.probe.release_ran);
    CHECK(provider.probe.release_status == abi::busy);
    CHECK(writer.release_ran);
    CHECK(writer.release_status == abi::busy);
    CHECK(writer.result == abi::ok);
    CHECK(writer.value == "S1");

    // The credential survived both refusals, and the lease is still usable.
    CHECK(consumer.token().value == held.value);
    std::string payload;
    CHECK(consumer.read_current(&payload) == abi::ok);
    // Only after the call has fully returned can the owning context return the lease.
    CHECK(consumer.release_lease() == abi::ok);
    CHECK(consumer.token().value == 0);
    CHECK(consumer.release_token(held) == abi::stale);

    CHECK(f.host().unload("busy.provider") == abi::ok);
    CHECK(f.host().shutdown() == abi::ok);
}

/**
 * @brief Case 11: the mock obeys the lifecycle gates and a non-Active instance cannot call.
 *
 * An Initializing or Starting instance may not acquire a lease and may not call: the mock performs
 * both attempts once and records the exact refusals instead of ignoring them. The reachable
 * non-Active window is stop(), where the instance still owns its lease and must be able to return it
 * while a business call is refused.
 */
void case_lifecycle_state_gate()
{
    g_current_test = "lifecycle_state_gate";
    fixture f;
    provider_factory& provider = f.provider("gate.provider", pv(1, 0, 0), "G");
    consumer_factory& factory = f.consumer("gate.consumer");
    CHECK(f.host().add(&provider) == abi::ok);
    CHECK(f.host().add(&factory) == abi::ok);
    // Probe against a real identity, so only the state gate can explain a refusal.
    factory.instance().probe_provider = "gate.provider";
    CHECK(f.start() == abi::ok);

    fake_consumer& consumer = factory.instance();
    CHECK(consumer.ready());
    // Initializing: the lease and the call were both refused, and the refused acquire left no lock.
    CHECK(consumer.init_acquire == abi::invalid_state);
    CHECK(cleared(consumer.init_borrow));
    CHECK(consumer.init_call == abi::stale);
    CHECK(!consumer.holds_lease());
    // Starting: the same gate, while the only legitimate work (announcing) succeeded.
    CHECK(consumer.start_acquire == abi::invalid_state);
    CHECK(cleared(consumer.start_borrow));
    CHECK(consumer.start_call == abi::stale);
    CHECK(consumer.announce_status == abi::ok);
    CHECK(!consumer.holds_lease());

    CHECK(consumer.acquire_lease("gate.provider", abi::exact_version(pv(1, 0, 0))) == abi::ok);
    CHECK(consumer.holds_lease());
    std::string who;
    CHECK(consumer.who(&who) == abi::ok);
    CHECK(who == "G");

    // retire() marks the record revoking before stop(), so the lease is still owned while a call is
    // refused and the return remains legal.
    CHECK(f.host().unload("gate.consumer") == abi::ok);
    CHECK(consumer.stop_calls == 1);
    CHECK(consumer.stop_probe_ran);
    CHECK(consumer.stop_call == abi::invalid_state);
    CHECK(consumer.stop_release == abi::ok);
    CHECK(consumer.destroy_calls == 1 && consumer.destroyed);
    CHECK(!f.alive("gate.consumer"));
    CHECK(f.alive("gate.provider"));
    // The lease went away with its owner, so the provider is no longer pinned.
    CHECK(f.host().unload("gate.provider") == abi::ok);
    CHECK(f.host().shutdown() == abi::ok);
}

/** @brief Case 8: the host queries invoke_iid only; invocation always needs a lease. */
void case_query_isolated_to_invoke()
{
    g_current_test = "query_isolated_to_invoke";
    fixture f;
    provider_factory& provider_factory_ref = f.provider("query.provider", pv(1, 2, 3), "Q");
    consumer_factory& factory = f.consumer("query.consumer");
    CHECK(f.host().add(&provider_factory_ref) == abi::ok);
    CHECK(f.host().add(&factory) == abi::ok);
    CHECK(f.start() == abi::ok);

    fake_provider& provider = *provider_factory_ref.instance();
    // Publishing an instance with methods resolves invoke_iid exactly once and nothing else.
    CHECK(provider.query_invoke_calls == 1);
    CHECK(provider.query_other_calls == 0);
    CHECK(provider.query_null_calls == 0);

    fake_consumer& consumer = factory.instance();
    CHECK(consumer.acquire_lease("query.provider", abi::exact_version(pv(1, 2, 3))) == abi::ok);
    std::string session;
    std::string payload;
    CHECK(consumer.open_session(&session) == abi::ok);
    CHECK(consumer.read_current(&payload) == abi::ok);
    // Acquiring a lease and calling a method are host bookkeeping; no plugin query runs.
    CHECK(provider.query_invoke_calls == 1);
    CHECK(provider.query_other_calls == 0);
    // The consumer announces no business method, so the host never queries it at all.
    CHECK(consumer.plugin_query_calls == 0);

    CHECK(consumer.release_lease() == abi::ok);
    CHECK(f.host().unload("query.provider") == abi::ok);
    CHECK(f.host().shutdown() == abi::ok);
}

/** @brief Case 9: an unload requested from inside a callback is deferred to a safe point. */
void case_callback_unload_deferred()
{
    g_current_test = "callback_unload_deferred";
    fixture f;
    provider_factory& provider_factory_ref = f.provider("defer.provider", pv(1, 0, 0), "D");
    provider_factory& victim_factory = f.provider("defer.victim", pv(1, 0, 0), "V");
    consumer_factory& factory = f.consumer("defer.consumer");
    CHECK(f.host().add(&provider_factory_ref) == abi::ok);
    CHECK(f.host().add(&victim_factory) == abi::ok);
    CHECK(f.host().add(&factory) == abi::ok);
    CHECK(f.start() == abi::ok);
    CHECK(f.alive("defer.victim"));

    fake_consumer& consumer = factory.instance();
    CHECK(consumer.acquire_lease("defer.provider", abi::exact_version(pv(1, 0, 0))) == abi::ok);

    fake_provider& provider = *provider_factory_ref.instance();
    provider.probe.structural_host = &f.host();
    provider.probe.deferred_target = "defer.victim";
    provider.probe.unknown_target = "defer.absent";
    provider.probe.add_target = &victim_factory;

    std::string session;
    CHECK(consumer.open_session(&session) == abi::ok);
    CHECK(provider.probe.structural_ran);
    // A known identity is accepted but only queued; an unknown one is refused and never remembered.
    CHECK(provider.probe.deferred_status == abi::deferred);
    CHECK(provider.probe.unknown_status == abi::not_found);
    // Structural host operations cannot run while a plugin call is on the stack.
    CHECK(provider.probe.shutdown_status == abi::busy);
    CHECK(provider.probe.start_status == abi::invalid_state);
    CHECK(provider.probe.add_status == abi::busy);
    CHECK(f.alive("defer.victim"));

    // The next safe point consumes the deferred request without disturbing the caller.
    CHECK(f.host().poll() == abi::ok);
    CHECK(!f.alive("defer.victim"));
    CHECK(f.alive("defer.provider"));

    CHECK(consumer.release_lease() == abi::ok);
    CHECK(f.host().unload("defer.provider") == abi::ok);
    CHECK(f.host().shutdown() == abi::ok);
}

/** @brief Case 9b: the admin revoker runs under the depth guard and cannot re-enter structure. */
void case_admin_revoker_depth_guard()
{
    g_current_test = "admin_revoker_depth_guard";
    fixture f;
    provider_factory& provider_factory_ref = f.provider("revoke.provider", pv(1, 0, 0), "R");
    provider_factory& victim_factory = f.provider("revoke.victim", pv(1, 0, 0), "RV");
    provider_factory& spare_factory = f.provider("revoke.spare", pv(1, 0, 0), "RS");
    CHECK(f.host().add(&provider_factory_ref) == abi::ok);
    CHECK(f.host().add(&victim_factory) == abi::ok);
    CHECK(f.start() == abi::ok);

    structural_revoker revoker;
    revoker.host = &f.host();
    revoker.victim = "revoke.victim";
    revoker.spare = &spare_factory;

    abi::borrow lease{};
    CHECK(f.host().acquire("revoke.provider", abi::exact_version(pv(1, 0, 0)), &revoker, &lease) ==
          abi::ok);

    // The unreturned lease pins the provider, and the revocation callback cannot structurally
    // re-enter the host while it observes the lease table.
    CHECK(f.host().unload("revoke.provider") == abi::busy);
    CHECK(revoker.calls == 1);
    CHECK(revoker.shutdown_status == abi::busy);
    CHECK(revoker.start_status == abi::invalid_state);
    CHECK(revoker.unload_status == abi::deferred);
    CHECK(revoker.add_status == abi::busy);
    CHECK(f.alive("revoke.provider"));
    CHECK(f.alive("revoke.victim"));
    CHECK(!f.alive("revoke.spare"));

    // Returning the credential at a safe point completes both the retried teardown and the deferred
    // request the callback queued.
    CHECK(f.host().release(lease.credential) == abi::ok);
    CHECK(f.host().unload("revoke.provider") == abi::ok);
    CHECK(!f.alive("revoke.provider"));
    CHECK(!f.alive("revoke.victim"));
    CHECK(f.host().shutdown() == abi::ok);
}

/** @brief Case 10: a failing temporary call and a failing leased call leave no dangling lease. */
void case_failed_call_no_dangling_lease()
{
    g_current_test = "failed_call_no_dangling_lease";
    fixture f;
    provider_factory& one_shot_factory = f.provider("oneshot.provider", pv(1, 0, 0), "OS");
    provider_factory& leased_factory = f.provider("leased.provider", pv(1, 0, 0), "LS");
    consumer_factory& consumer_factory_ref = f.consumer("oneshot.consumer");
    CHECK(f.host().add(&one_shot_factory) == abi::ok);
    CHECK(f.host().add(&leased_factory) == abi::ok);
    CHECK(f.host().add(&consumer_factory_ref) == abi::ok);
    CHECK(f.start() == abi::ok);

    const abi::version_range allowed = abi::exact_version(pv(1, 0, 0));
    // The one-shot administration call states its version range explicitly, and every failure path
    // clears the output and returns its temporary lease.
    std::string out("dirty");
    CHECK(f.host().call("oneshot.provider", between(pv(2, 0, 0), pv(2, 5, 0)), std::string("open"),
                        abi::bytes{nullptr, 0}, &out) == abi::unsupported);
    CHECK(out.empty());
    CHECK(f.host().call("oneshot.provider", between(pv(1, 5, 0), pv(1, 0, 0)), std::string("open"),
                        abi::bytes{nullptr, 0}, &out) == abi::invalid_argument);
    CHECK(out.empty());
    CHECK(f.host().call("oneshot.provider", allowed, std::string("absent"), abi::bytes{nullptr, 0},
                        &out) == abi::not_found);
    CHECK(out.empty());
    CHECK(f.host().call("oneshot.provider", allowed, std::string("fail"), abi::bytes{nullptr, 0},
                        &out) == abi::failed);
    CHECK(out.empty()); // The partial output of the failing invocation is discarded.
    CHECK(f.host().call("oneshot.provider", allowed, static_cast<abi::method_id>(open_method),
                        abi::bytes{nullptr, 0}, &out) == abi::ok);
    CHECK(out == "S1");
    // No borrowing is left behind: the provider unloads directly.
    CHECK(f.host().unload("oneshot.provider") == abi::ok);

    // A failing leased call keeps the consumer's own lease, which stays usable until returned.
    fake_consumer& consumer = consumer_factory_ref.instance();
    CHECK(consumer.acquire_lease("leased.provider", allowed) == abi::ok);
    const abi::token held = consumer.token();
    CHECK(held.value != 0);
    std::string failed;
    CHECK(consumer.invoke_fail(&failed) == abi::failed);
    CHECK(failed.empty());
    CHECK(consumer.token().value == held.value);
    std::string session;
    CHECK(consumer.open_session(&session) == abi::ok);
    std::string payload;
    CHECK(consumer.read_current(&payload) == abi::ok);
    CHECK(consumer.release_lease() == abi::ok);
    CHECK(f.host().unload("leased.provider") == abi::ok);
    CHECK(f.host().shutdown() == abi::ok);
}

} // namespace

int main()
{
    case_version_range_accepts_and_rejects();
    case_version_numeric_order_and_uint32_bounds();
    case_zero_version_and_cross_major_actual();
    case_version_discovery_no_lease();
    case_borrow_version_is_caller_copy();
    case_same_version_reload_rebuilds_session();
    case_major_upgrade_reload();
    case_multi_lease_lifetime();
    case_direct_calls_do_not_consume_lease();
    case_owner_zero_thread_validation();
    case_lease_busy_in_flight();
    case_lifecycle_state_gate();
    case_query_isolated_to_invoke();
    case_callback_unload_deferred();
    case_admin_revoker_depth_guard();
    case_failed_call_no_dangling_lease();

    std::printf("all lease/version acceptance checks passed\n");
    return 0;
}
