/**
 * @file contract_test.cc
 * @brief Acceptance checks for the invoke-only revocable contract lease of ABI v2.
 *
 * The file owns its main() and links against the host translation units, so it can be built as:
 *   g++ -std=c++17 -Wall -Wextra -Werror -Iinc tests/contract_test.cc src/context.cc src/events.cc
 *       src/host.cc src/order.cc src/plug.cc -ldl -pthread \
 *       -o build/invoke-v2/contract/contract_test
 *
 * Every fixture is an in-process static factory handed to u42::host::add(); no shared object, no
 * framework and no private header is involved. Plugins observe the host only through the public
 * ABI in 42u/abi.hpp, so all business traffic runs consumer -> host icalls -> provider iinvoke and
 * never touches a provider interface pointer.
 *
 * Covered contracts, in the order the cases run:
 *   1. Contract family/major mismatch and an insufficient offered minor are refused with
 *      unsupported; a malformed required contract is invalid_argument; refused acquire calls
 *      clear the borrow and leave no lock behind (the provider still unloads).
 *   2. A compatible minimum minor is accepted, and explicit protocol discovery works without
 *      locking anything.
 *   3. Reloading the same protocol under the same identity never lets the old lease or binding
 *      follow: they report stale, the consumer's on_revoke clears its session, and the rebuilt
 *      session belongs to the new generation while the previous session token is dead.
 *   4. An incompatible major upgrade under the same identity refuses the old expectation and
 *      accepts the explicitly stated new contract.
 *   5. Multiple leases are independent: returning one keeps the other lock, unbind never returns a
 *      lease, release invalidates the bindings it authorized, and zero/wrong-owner/wrong-thread
 *      calls are refused without disturbing a live lease.
 *   6. During an in-flight call, a release of the same lease from inside provider.invoke and from
 *      inside the output writer both report busy and lose no credential; the later return succeeds.
 *   7. Binding is impossible without a lease: the ABI shapes and signatures are pinned at compile
 *      time, and the host queries the provider for invoke_iid only, never for business iids.
 *   8. An unload requested from inside a plugin callback is accepted as deferred and completed at
 *      the next safe point; the admin revoker runs under the depth guard and cannot structurally
 *      re-enter the host.
 *   9. A failing one-shot native call and a failing leased call leave no dangling lease: the
 *      provider still unloads, and the lease a consumer already holds stays usable.
 *
 * @note NDEBUG is defined deliberately: CHECK must keep reporting failures even when assert() has
 *       been compiled out, so a diagnostic can never vanish in release builds.
 * @note The fixtures live in this file and are inspected only through their own factory/instance
 *       objects, never through a host-internal record or context.
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

namespace abi = u42::abi::v2;

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
 * With NDEBUG defined, assert() collapses to ((void)0), so every failure would silently
 * disappear. The expansion text is checked because a runtime probe cannot detect it.
 */
inline constexpr std::string_view u42_check_expansion = U42_EXPAND_STRING(CHECK(0 == 1));
static_assert(u42_check_expansion.find("fail_check") != std::string_view::npos,
              "CHECK must call fail_check directly; defining it as assert() would disable it "
              "under NDEBUG");

/* ------------------------------------------------------------------ *
 * Compile-time ABI shape and signature proofs.
 * ------------------------------------------------------------------ */

static_assert(sizeof(abi::borrow) == 8 && alignof(abi::borrow) == 8,
              "borrow must be exactly one 8-byte credential");
static_assert(sizeof(abi::token) == 8 && sizeof(abi::binding) == 8);
static_assert(std::is_standard_layout_v<abi::borrow> && std::is_trivially_copyable_v<abi::borrow>);
static_assert(std::is_aggregate_v<abi::borrow>);
// A second storage-bearing member (for example a provider pointer) cannot fit in 8 bytes, and a
// standard-layout struct may not overlap members, so the declared shape excludes it.
static_assert(std::is_same_v<decltype(abi::borrow{}.credential), abi::token>);
static_assert(!std::is_same_v<decltype(abi::borrow{}.credential), void*>);

static_assert(sizeof(abi::contract) == 24 && alignof(abi::contract) == 8);
static_assert(sizeof(abi::caps_desc) == 40 && alignof(abi::caps_desc) == 8);

/**
 * @brief Detect an icalls shortcut that would bind by provider identity instead of a lease.
 *
 * @tparam T Candidate interface type.
 * @note The trait is true only when `bind_name(const char*, const char*, binding*)` is callable.
 *       The frozen ABI has no such overload, so the static assertion below pins that a caller
 *       cannot bypass icaps::acquire() by naming a plug_id.
 */
template <class T, class = void>
struct icalls_bind_with_plug_id : std::false_type {};
template <class T>
struct icalls_bind_with_plug_id<T, std::void_t<decltype(std::declval<T&>().bind_name(
                                          std::declval<const char*>(), std::declval<const char*>(),
                                          std::declval<abi::binding*>()))>> : std::true_type {};
static_assert(!icalls_bind_with_plug_id<abi::icalls>::value,
              "icalls must not expose a plug_id-based bind shortcut; a lease credential is the "
              "only way to bind");

/** @brief Detect the removed 3-argument host::bind that took a plug_id instead of a credential. */
template <class T, class = void>
struct host_bind_with_plug_id : std::false_type {};
template <class T>
struct host_bind_with_plug_id<T, std::void_t<decltype(std::declval<T&>().bind(
                                        std::declval<const std::string&>(),
                                        std::declval<const std::string&>(),
                                        std::declval<abi::binding*>()))>> : std::true_type {};
static_assert(!host_bind_with_plug_id<u42::host>::value,
              "host::bind must take a lease credential, never a plug_id");

/** @brief Detect the removed 4-argument convenience call that accepted no explicit contract. */
template <class T, class = void>
struct host_call_without_contract : std::false_type {};
template <class T>
struct host_call_without_contract<T, std::void_t<decltype(std::declval<T&>().call(
                                           std::declval<const std::string&>(),
                                           std::declval<const std::string&>(),
                                           std::declval<abi::bytes>(),
                                           std::declval<std::string*>()))>> : std::true_type {};
static_assert(!host_call_without_contract<u42::host>::value,
              "the convenience call must state the explicitly accepted contract");

// Exact signatures: an added overload with the same name would make these declarations ambiguous
// and fail the build, which is the intended compile-time proof.
static_assert(std::is_same_v<decltype(&abi::icalls::bind_name),
                             abi::status (U42_CALL abi::icalls::*)(abi::token, const char*,
                                                                  abi::binding*) noexcept>);
static_assert(std::is_same_v<decltype(&abi::icalls::bind_id),
                             abi::status (U42_CALL abi::icalls::*)(abi::token, abi::method_id,
                                                                  abi::binding*) noexcept>);
static_assert(std::is_same_v<decltype(&abi::icalls::unbind),
                             abi::status (U42_CALL abi::icalls::*)(abi::binding) noexcept>);
static_assert(std::is_same_v<decltype(&abi::icaps::acquire),
                             abi::status (U42_CALL abi::icaps::*)(const char*, const abi::contract*,
                                                                  abi::irevoker*,
                                                                  abi::borrow*) noexcept>);
static_assert(std::is_same_v<decltype(&u42::host::acquire),
                             abi::status (u42::host::*)(const std::string&, const abi::contract&,
                                                        abi::irevoker*, abi::borrow*)>);
static_assert(std::is_same_v<decltype(&u42::host::release), abi::status (u42::host::*)(abi::token)>);
static_assert(std::is_same_v<decltype(&u42::host::unbind),
                             abi::status (u42::host::*)(abi::binding)>);
static_assert(std::is_same_v<decltype(&u42::host::protocol),
                             abi::status (u42::host::*)(const std::string&, abi::contract*)>);

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

/* ------------------------------------------------------------------ *
 * Constexpr contract helpers.
 * ------------------------------------------------------------------ */

constexpr abi::iid kFamilyA{0x1111111100000001ULL, 1};
constexpr abi::iid kFamilyB{0x2222222200000002ULL, 2};
constexpr abi::contract kA24{kFamilyA, 2, 4};
static_assert(abi::valid_contract(kA24));
static_assert(abi::compatible_contract(abi::contract{kFamilyA, 2, 5}, abi::contract{kFamilyA, 2, 4}));
static_assert(abi::compatible_contract(abi::contract{kFamilyA, 2, 4}, abi::contract{kFamilyA, 2, 4}));
static_assert(!abi::compatible_contract(abi::contract{kFamilyA, 2, 3}, abi::contract{kFamilyA, 2, 4}));
static_assert(!abi::compatible_contract(abi::contract{kFamilyA, 3, 0}, abi::contract{kFamilyA, 2, 0}));
static_assert(!abi::compatible_contract(abi::contract{kFamilyB, 2, 9}, abi::contract{kFamilyA, 2, 0}));
static_assert(!abi::valid_contract(abi::contract{abi::iid{0, 0}, 1, 0}));
static_assert(!abi::valid_contract(abi::contract{kFamilyA, 0, 0}));

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

/** @brief Compare two contracts field by field. */
bool contract_equal(const abi::contract& left, const abi::contract& right) noexcept
{
    return left.id == right.id && left.major == right.major && left.minor == right.minor;
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
 * Provider fixture: iplug + iinvoke with a stateful session table.
 * ------------------------------------------------------------------ */

/** @brief Method identities announced by every fake provider. */
enum : abi::method_id { open_method = 1, read_method = 2, fail_method = 3 };

/**
 * @brief Re-entrant probes a provider runs at the start of its own invoke().
 *
 * The probes model a hostile provider: it tries to return the consumer's lease from inside the
 * call, and it tries to unload or restart the host while it is on the stack. The test reads back
 * the recorded statuses.
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
    std::string unknown_target{"contract.no.such.plugin"};
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
 */
class provider_factory final : public abi::iplug_fty {
public:
    /**
     * @brief Build one provider type.
     *
     * @param plug_id Stable plugin identity; the descriptor borrows this storage.
     * @param protocol Business contract announced during start().
     */
    provider_factory(std::string plug_id, abi::contract protocol);

    abi::status U42_CALL describe(const abi::plug_desc** out) noexcept override;
    abi::status U42_CALL create(abi::iplug** out) noexcept override;

    /** @brief Most recently created instance, or null before create(). */
    fake_provider* instance() const noexcept { return instance_.get(); }
    const abi::contract& protocol() const noexcept { return protocol_; }

    std::uint64_t create_calls = 0;

private:
    std::string plug_id_;
    std::string version_ = "1.0";
    abi::contract protocol_;
    abi::plug_desc desc_{};
    std::unique_ptr<fake_provider> instance_;
};

provider_factory::provider_factory(std::string plug_id, abi::contract protocol)
    : plug_id_(std::move(plug_id)), protocol_(protocol)
{
    desc_.struct_size = sizeof(abi::plug_desc);
    desc_.reserved = 0;
    desc_.plug_id = plug_id_.c_str();
    desc_.version = version_.c_str();
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

    static const abi::method_desc methods[3] = {
        {open_method, "open", "Open a provider session", "{}", "{}"},
        {read_method, "read", "Read the current session", "{}", "{}"},
        {fail_method, "fail", "Fail after partial output", "{}", "{}"},
    };
    abi::caps_desc announcement{};
    announcement.method_count = 3;
    announcement.methods = methods;
    announcement.protocol = owner_.protocol();
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
            return write_text(result, found->second);
        }
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

class consumer_factory;

/**
 * @brief One fake consumer instance.
 *
 * All business traffic goes through the host services resolved from its own ictx: acquire() for
 * the lease, bind_name() for a method and call() for the invocation. The instance keeps no
 * provider pointer and no provider interface.
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

    // Test-driven business operations.
    /** @brief Acquire one lease and bind the open method of the named provider. */
    abi::status connect(const char* provider_id, const abi::contract& required);
    /** @brief Open a session through the bound method, storing the returned token. */
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
    /** @brief Read an explicitly named session token through a fresh binding. */
    abi::status read_id(const std::string& session_id, std::string* out);
    /** @brief Invoke the failing method through a temporary binding on the held lease. */
    abi::status invoke_fail(std::string* out);
    /** @brief Return the lease the instance holds. */
    abi::status release_lease();
    /** @brief Return an arbitrary credential through this instance's own icaps. */
    abi::status release_token(abi::token credential);
    /** @brief Bind with an arbitrary credential through this instance's own icalls. */
    abi::status bind_credential(abi::token credential, const char* name, abi::binding* out);
    /** @brief Unbind an arbitrary binding through this instance's own icalls. */
    abi::status unbind_binding(abi::binding value);
    /** @brief Invoke an arbitrary binding through this instance's own icalls. */
    abi::status call_binding(abi::binding value, abi::bytes args, std::string* out);

    // Observations.
    abi::status caps_status = abi::failed;
    abi::status calls_status = abi::failed;
    abi::status diag_status = abi::failed;
    abi::status announce_status = abi::failed;
    abi::status last_acquire = abi::failed;
    abi::status last_bind = abi::failed;
    std::uint64_t init_calls = 0;
    std::uint64_t start_calls = 0;
    std::uint64_t stop_calls = 0;
    std::uint64_t destroy_calls = 0;
    std::uint64_t plugin_query_calls = 0;
    bool destroyed = false;

    /** @brief True once the capability and invocation services resolved. */
    bool ready() const noexcept { return caps_ != nullptr && calls_ != nullptr; }
    /** @brief Credential of the currently held lease, or zero. */
    abi::token token() const noexcept { return lease_.credential; }
    /** @brief Binding of the currently held open method, or zero. */
    abi::binding binding() const noexcept { return session_binding_; }
    bool holds_lease() const noexcept { return lease_.credential.value != 0; }
    bool session_valid() const noexcept { return session_valid_; }
    /** @brief Capability service of this instance, used by the lease-busy probes. */
    abi::icaps* caps_service() const noexcept { return caps_; }
    std::uint64_t revoke_calls() const noexcept { return revoke_calls_; }
    abi::status revoke_release_status() const noexcept { return revoke_release_status_; }

private:
    /** @brief Invoke the bound open method, storing the returned session on success. */
    abi::status open_with(abi::iwriter* writer, std::string* out);

    abi::ictx* ctx_ = nullptr;
    abi::icaps* caps_ = nullptr;
    abi::icalls* calls_ = nullptr;
    abi::idiag* diag_ = nullptr;
    abi::borrow lease_{};
    abi::binding session_binding_{};
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
    explicit consumer_factory(std::string plug_id);

    abi::status U42_CALL describe(const abi::plug_desc** out) noexcept override;
    abi::status U42_CALL create(abi::iplug** out) noexcept override;

    /** @brief Live instance; the test drives its business operations through it. */
    fake_consumer& instance() const noexcept { return *instance_; }

private:
    std::string plug_id_;
    std::string version_ = "1.0";
    abi::plug_desc desc_{};
    std::unique_ptr<fake_consumer> instance_;
};

consumer_factory::consumer_factory(std::string plug_id) : plug_id_(std::move(plug_id))
{
    desc_.struct_size = sizeof(abi::plug_desc);
    desc_.reserved = 0;
    desc_.plug_id = plug_id_.c_str();
    desc_.version = version_.c_str();
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
    return abi::ok;
}

abi::status U42_CALL fake_consumer::start() noexcept
{
    ++start_calls;
    if (caps_ == nullptr) return abi::invalid_state;
    // A consumer without business methods announces the all-zero contract of an empty method set.
    abi::caps_desc announcement{};
    announce_status = caps_->announce(&announcement);
    return announce_status;
}

abi::status U42_CALL fake_consumer::stop() noexcept
{
    ++stop_calls;
    session_valid_ = false;
    session_id_.clear();
    session_binding_ = abi::binding{};
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
    // Contract order: stop dependent business and drop the generation-local session first, then
    // return the credential. The failed-backend state is never carried across the revocation.
    session_valid_ = false;
    session_id_.clear();
    session_binding_ = abi::binding{};
    const abi::status returned = caps_->release(credential);
    revoke_release_status_ = returned;
    if (returned == abi::ok || returned == abi::stale) lease_ = abi::borrow{};
}

abi::status fake_consumer::connect(const char* provider_id, const abi::contract& required)
{
    if (caps_ == nullptr || calls_ == nullptr) return abi::invalid_state;
    abi::borrow value{abi::token{0xABCDEF}};
    const abi::status acquired = caps_->acquire(provider_id, &required, this, &value);
    last_acquire = acquired;
    if (acquired != abi::ok) return acquired;
    lease_ = value;
    abi::binding bound{};
    const abi::status linked = calls_->bind_name(lease_.credential, "open", &bound);
    last_bind = linked;
    if (linked != abi::ok) return linked;
    session_binding_ = bound;
    return abi::ok;
}

abi::status fake_consumer::open_with(abi::iwriter* writer, std::string* out)
{
    if (session_binding_.value == 0) return abi::invalid_state;
    const abi::status invoked = calls_->call(session_binding_, abi::bytes{nullptr, 0}, writer);
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

abi::status fake_consumer::read_id(const std::string& session_id, std::string* out)
{
    if (lease_.credential.value == 0) return abi::invalid_state;
    abi::binding bound{};
    const abi::status linked = calls_->bind_name(lease_.credential, "read", &bound);
    if (linked != abi::ok) return linked;
    u42::sdk::string_writer writer(out, 4096);
    const abi::status invoked = calls_->call(bound, as_bytes(session_id), &writer);
    const abi::status unbound = calls_->unbind(bound);
    if (invoked != abi::ok) return invoked;
    return unbound;
}

abi::status fake_consumer::invoke_fail(std::string* out)
{
    if (lease_.credential.value == 0) return abi::invalid_state;
    abi::binding bound{};
    const abi::status linked = calls_->bind_name(lease_.credential, "fail", &bound);
    if (linked != abi::ok) return linked;
    u42::sdk::string_writer writer(out, 4096);
    const abi::status invoked = calls_->call(bound, abi::bytes{nullptr, 0}, &writer);
    const abi::status unbound = calls_->unbind(bound);
    if (invoked != abi::ok) return invoked;
    return unbound;
}

abi::status fake_consumer::release_lease()
{
    if (lease_.credential.value == 0) return abi::invalid_state;
    const abi::status returned = caps_->release(lease_.credential);
    if (returned == abi::ok || returned == abi::stale) lease_ = abi::borrow{};
    return returned;
}

abi::status fake_consumer::release_token(abi::token credential)
{
    if (caps_ == nullptr) return abi::invalid_state;
    return caps_->release(credential);
}

abi::status fake_consumer::bind_credential(abi::token credential, const char* name, abi::binding* out)
{
    if (calls_ == nullptr) return abi::invalid_state;
    return calls_->bind_name(credential, name, out);
}

abi::status fake_consumer::unbind_binding(abi::binding value)
{
    if (calls_ == nullptr) return abi::invalid_state;
    return calls_->unbind(value);
}

abi::status fake_consumer::call_binding(abi::binding value, abi::bytes args, std::string* out)
{
    if (calls_ == nullptr) return abi::invalid_state;
    u42::sdk::string_writer writer(out, 4096);
    return calls_->call(value, args, &writer);
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
 * The admin lease has no plugin record, so revoke() runs this callback under the engine depth
 * guard; every structural operation must be refused instead of running re-entrantly.
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
    provider_factory& provider(std::string plug_id, abi::contract protocol)
    {
        providers_.push_back(std::make_unique<provider_factory>(std::move(plug_id), protocol));
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
 * Cases.
 * ------------------------------------------------------------------ */

/** @brief Case 1: name/major mismatch and an insufficient minor are unsupported, with no lock. */
void case_contract_mismatch_no_lock()
{
    g_current_test = "contract_mismatch_no_lock";
    fixture f;
    const abi::contract offered{kFamilyA, 2, 5};
    provider_factory& provider = f.provider("mismatch.provider", offered);
    ignoring_revoker revoker;

    CHECK(f.host().add(&provider) == abi::ok);
    // Staged but not started: nothing is published yet, so the lease is refused without a lock.
    abi::borrow premature{abi::token{0xDEAD}};
    CHECK(f.host().acquire("mismatch.provider", offered, &revoker, &premature) == abi::not_found);
    CHECK(premature.credential.value == 0);

    CHECK(f.start() == abi::ok);

    abi::borrow out{abi::token{0xDEADBEEF}};
    // A different protocol family is incompatible, not an argument error.
    CHECK(f.host().acquire("mismatch.provider", abi::contract{kFamilyB, 2, 0}, &revoker, &out) ==
          abi::unsupported);
    CHECK(out.credential.value == 0);
    // Same family, different major: incompatible.
    out = abi::borrow{abi::token{0xDEADBEEF}};
    CHECK(f.host().acquire("mismatch.provider", abi::contract{kFamilyA, 3, 0}, &revoker, &out) ==
          abi::unsupported);
    CHECK(out.credential.value == 0);
    // Same family and major, but the provider's minor is below the required minimum.
    out = abi::borrow{abi::token{0xDEADBEEF}};
    CHECK(f.host().acquire("mismatch.provider", abi::contract{kFamilyA, 2, 6}, &revoker, &out) ==
          abi::unsupported);
    CHECK(out.credential.value == 0);
    // A required protocol that is not a valid contract is invalid_argument, not unsupported.
    out = abi::borrow{abi::token{0xDEADBEEF}};
    CHECK(f.host().acquire("mismatch.provider", abi::contract{abi::iid{0, 0}, 2, 0}, &revoker, &out) ==
          abi::invalid_argument);
    CHECK(out.credential.value == 0);
    out = abi::borrow{abi::token{0xDEADBEEF}};
    CHECK(f.host().acquire("mismatch.provider", abi::contract{kFamilyA, 0, 0}, &revoker, &out) ==
          abi::invalid_argument);
    CHECK(out.credential.value == 0);

    // None of the refusals produced a lease: the provider unloads directly and never revoked.
    CHECK(f.host().unload("mismatch.provider") == abi::ok);
    CHECK(revoker.on_revoke_calls == 0);
    CHECK(!f.alive("mismatch.provider"));
    CHECK(f.host().shutdown() == abi::ok);
}

/** @brief Case 2: explicit discovery and a lower compatible minor that actually works. */
void case_discovery_and_compatible_minor()
{
    g_current_test = "discovery_and_compatible_minor";
    fixture f;
    const abi::contract offered{abi::iid{0x3333333300000003ULL, 7}, 4, 3};
    provider_factory& provider = f.provider("discovery.provider", offered);
    ignoring_revoker revoker;
    CHECK(f.host().add(&provider) == abi::ok);
    CHECK(f.start() == abi::ok);

    // Discovery reports the published contract without acquiring anything.
    abi::contract discovered{abi::iid{9, 9}, 9, 9};
    CHECK(f.host().protocol("discovery.provider", &discovered) == abi::ok);
    CHECK(contract_equal(discovered, offered));
    abi::contract missing{abi::iid{9, 9}, 9, 9};
    CHECK(f.host().protocol("discovery.absent", &missing) == abi::not_found);
    CHECK(missing.id.high == 0 && missing.id.low == 0 && missing.major == 0 && missing.minor == 0);

    // A lower minimum minor and the exact minor are both compatible.
    abi::borrow lease{};
    CHECK(f.host().acquire("discovery.provider", abi::contract{offered.id, 4, 1}, &revoker, &lease) ==
          abi::ok);
    CHECK(lease.credential.value != 0);
    abi::binding bound{};
    CHECK(f.host().bind(lease.credential, std::string("open"), &bound) == abi::ok);
    std::string text;
    CHECK(f.host().call(bound, abi::bytes{nullptr, 0}, &text) == abi::ok);
    CHECK(text.rfind("S", 0) == 0);
    CHECK(f.host().unbind(bound) == abi::ok);
    CHECK(f.host().release(lease.credential) == abi::ok);
    CHECK(f.host().acquire("discovery.provider", offered, &revoker, &lease) == abi::ok);
    CHECK(f.host().release(lease.credential) == abi::ok);

    CHECK(f.host().unload("discovery.provider") == abi::ok);
    CHECK(f.host().shutdown() == abi::ok);
}

/** @brief Case 3: reload of the same protocol never lets old state follow. */
void case_same_protocol_reload_state()
{
    g_current_test = "same_protocol_reload_state";
    fixture f;
    const abi::contract proto{abi::iid{0x4444444400000004ULL, 1}, 1, 0};
    provider_factory& provider = f.provider("reload.provider", proto);
    consumer_factory& factory = f.consumer("reload.consumer");
    CHECK(f.host().add(&provider) == abi::ok);
    CHECK(f.host().add(&factory) == abi::ok);
    CHECK(f.start() == abi::ok);

    fake_consumer& consumer = factory.instance();
    CHECK(consumer.ready());
    CHECK(consumer.connect("reload.provider", proto) == abi::ok);
    CHECK(consumer.holds_lease());
    std::string first;
    CHECK(consumer.open_session(&first) == abi::ok);
    CHECK(consumer.session_valid());
    CHECK(first.rfind("S", 0) == 0);
    std::string payload;
    CHECK(consumer.read_current(&payload) == abi::ok);
    CHECK(payload.rfind("session-", 0) == 0);

    const abi::token old_token = consumer.token();
    const abi::binding old_binding = consumer.binding();
    CHECK(old_token.value != 0 && old_binding.value != 0);

    // Unload the provider while the consumer still holds a lease: the host revokes first.
    CHECK(f.host().unload("reload.provider") == abi::ok);
    CHECK(consumer.revoke_calls() == 1);
    CHECK(consumer.revoke_release_status() == abi::ok);
    CHECK(!consumer.holds_lease());
    CHECK(!consumer.session_valid());
    CHECK(consumer.token().value == 0);
    // The consumer must rebuild: no session and no usable binding survives the revocation.
    CHECK(consumer.read_current(&payload) == abi::invalid_state);
    CHECK(consumer.release_token(old_token) == abi::stale);
    abi::binding scratch{abi::binding{0xFEED}};
    CHECK(consumer.bind_credential(old_token, "open", &scratch) == abi::stale);
    CHECK(scratch.value == 0);
    CHECK(consumer.call_binding(old_binding, abi::bytes{nullptr, 0}, &payload) == abi::stale);
    CHECK(consumer.unbind_binding(old_binding) == abi::stale);

    // Reload the same identity and protocol: a fresh generation with a fresh session table.
    provider_factory& reloaded = f.provider("reload.provider", proto);
    CHECK(f.host().add(&reloaded) == abi::ok);
    CHECK(f.host().start() == abi::ok);
    CHECK(f.alive("reload.provider"));

    CHECK(consumer.connect("reload.provider", proto) == abi::ok);
    CHECK(consumer.session_valid() == false); // Reconnecting does not carry the old session.
    CHECK(consumer.read_current(&payload) == abi::invalid_state);
    // Before explicit reconstruction the new provider has no session, even under a fresh lease.
    CHECK(consumer.read_id(first, &payload) == abi::not_found);
    std::string second;
    CHECK(consumer.open_session(&second) == abi::ok);
    CHECK(second == first); // Business IDs may repeat: the framework must not rely on uniqueness.
    CHECK(consumer.read_current(&payload) == abi::ok);
    // Identical business bytes do not revive the old lease or binding after the new session opens.
    CHECK(consumer.release_token(old_token) == abi::stale);
    CHECK(consumer.bind_credential(old_token, "open", &scratch) == abi::stale);
    CHECK(consumer.call_binding(old_binding, as_bytes(first), &payload) == abi::stale);
    // A fresh lease can legitimately use that reused ID; interpreting JSON is the protocol's job.
    CHECK(consumer.read_id(first, &payload) == abi::ok);

    CHECK(consumer.release_lease() == abi::ok);
    CHECK(f.host().unload("reload.provider") == abi::ok);
    CHECK(f.host().shutdown() == abi::ok);
}

/** @brief Case 4: an incompatible major upgrade refuses the old expectation only. */
void case_major_upgrade_reload()
{
    g_current_test = "major_upgrade_reload";
    fixture f;
    const abi::iid family{0x5555555500000005ULL, 2};
    provider_factory& version_one = f.provider("major.provider", abi::contract{family, 1, 0});
    ignoring_revoker revoker;
    CHECK(f.host().add(&version_one) == abi::ok);
    CHECK(f.start() == abi::ok);

    abi::contract seen{};
    CHECK(f.host().protocol("major.provider", &seen) == abi::ok);
    CHECK(seen.major == 1 && seen.id == family);
    abi::borrow lease{};
    CHECK(f.host().acquire("major.provider", abi::contract{family, 1, 0}, &revoker, &lease) == abi::ok);
    CHECK(f.host().release(lease.credential) == abi::ok);
    CHECK(f.host().unload("major.provider") == abi::ok);

    provider_factory& version_two = f.provider("major.provider", abi::contract{family, 2, 0});
    CHECK(f.host().add(&version_two) == abi::ok);
    CHECK(f.host().start() == abi::ok);
    CHECK(f.host().protocol("major.provider", &seen) == abi::ok);
    CHECK(seen.major == 2);

    // The previously accepted expectation is refused after the upgrade.
    abi::borrow stale_out{abi::token{0xDEAD}};
    CHECK(f.host().acquire("major.provider", abi::contract{family, 1, 0}, &revoker, &stale_out) ==
          abi::unsupported);
    CHECK(stale_out.credential.value == 0);
    // Explicitly accepting the new contract succeeds and is fully usable.
    abi::borrow fresh{};
    CHECK(f.host().acquire("major.provider", abi::contract{family, 2, 0}, &revoker, &fresh) == abi::ok);
    abi::binding bound{};
    CHECK(f.host().bind(fresh.credential, std::string("open"), &bound) == abi::ok);
    std::string text;
    CHECK(f.host().call(bound, abi::bytes{nullptr, 0}, &text) == abi::ok);
    CHECK(f.host().unbind(bound) == abi::ok);
    CHECK(f.host().release(fresh.credential) == abi::ok);

    CHECK(f.host().unload("major.provider") == abi::ok);
    CHECK(f.host().shutdown() == abi::ok);
}

/** @brief Case 5: leases, bindings and locks have independent lifetimes. */
void case_multi_lease_lifetime()
{
    g_current_test = "multi_lease_lifetime";
    fixture f;
    const abi::contract proto{abi::iid{0x6666666600000006ULL, 1}, 1, 0};
    provider_factory& provider = f.provider("lock.provider", proto);
    ignoring_revoker revoker;
    CHECK(f.host().add(&provider) == abi::ok);
    CHECK(f.start() == abi::ok);

    abi::borrow first{};
    abi::borrow second{};
    CHECK(f.host().acquire("lock.provider", proto, &revoker, &first) == abi::ok);
    CHECK(f.host().acquire("lock.provider", proto, &revoker, &second) == abi::ok);
    CHECK(first.credential.value != 0 && second.credential.value != 0);
    CHECK(first.credential.value != second.credential.value);

    std::string text;
    abi::binding bound{};
    CHECK(f.host().bind(first.credential, std::string("open"), &bound) == abi::ok);
    CHECK(f.host().call(bound, abi::bytes{nullptr, 0}, &text) == abi::ok);
    // unbind removes only the binding; the lease behind it stays alive and can bind again.
    CHECK(f.host().unbind(bound) == abi::ok);
    CHECK(f.host().call(bound, abi::bytes{nullptr, 0}, &text) == abi::stale);
    CHECK(f.host().unbind(bound) == abi::stale);
    abi::binding rebound{};
    CHECK(f.host().bind(first.credential, std::string("open"), &rebound) == abi::ok);
    CHECK(f.host().unbind(rebound) == abi::ok);

    // The second lease serves independently.
    CHECK(f.host().bind(second.credential, std::string("read"), &bound) == abi::ok);
    CHECK(f.host().unbind(bound) == abi::ok);

    // Returning one lease neither unlocks the other nor invalidates its live lease.
    CHECK(f.host().release(first.credential) == abi::ok);
    CHECK(f.host().bind(first.credential, std::string("open"), &rebound) == abi::stale);
    CHECK(f.host().unload("lock.provider") == abi::busy);
    CHECK(f.alive("lock.provider"));
    CHECK(revoker.on_revoke_calls == 1);
    CHECK(revoker.last.value == second.credential.value);

    // The last lease is still the owner's to return.
    CHECK(f.host().release(second.credential) == abi::ok);
    CHECK(f.host().bind(second.credential, std::string("open"), &rebound) == abi::stale);
    CHECK(f.host().unload("lock.provider") == abi::ok);
    CHECK(!f.alive("lock.provider"));
    CHECK(f.host().shutdown() == abi::ok);
}

/** @brief Case 5b: zero, cross-context and off-thread credential handling. */
void case_owner_zero_thread_validation()
{
    g_current_test = "owner_zero_thread_validation";
    fixture f;
    const abi::contract proto{abi::iid{0x7777777700000007ULL, 1}, 1, 0};
    provider_factory& provider = f.provider("owner.provider", proto);
    consumer_factory& factory = f.consumer("owner.consumer");
    ignoring_revoker revoker;
    CHECK(f.host().add(&provider) == abi::ok);
    CHECK(f.host().add(&factory) == abi::ok);
    CHECK(f.start() == abi::ok);

    fake_consumer& consumer = factory.instance();
    CHECK(consumer.connect("owner.provider", proto) == abi::ok);
    std::string text;
    CHECK(consumer.open_session(&text) == abi::ok);
    CHECK(consumer.read_current(&text) == abi::ok);
    const abi::token consumer_token = consumer.token();
    const abi::binding consumer_binding = consumer.binding();
    CHECK(consumer_token.value != 0 && consumer_binding.value != 0);

    abi::borrow admin{};
    CHECK(f.host().acquire("owner.provider", proto, &revoker, &admin) == abi::ok);
    abi::binding admin_binding{};
    CHECK(f.host().bind(admin.credential, std::string("open"), &admin_binding) == abi::ok);

    // Zero credentials and bindings are argument errors and never clear a real value.
    abi::binding scratch{abi::binding{0xFEED}};
    CHECK(f.host().release(abi::token{}) == abi::invalid_argument);
    CHECK(f.host().bind(abi::token{}, std::string("open"), &scratch) == abi::invalid_argument);
    CHECK(scratch.value == 0);
    CHECK(f.host().unbind(abi::binding{}) == abi::invalid_argument);
    CHECK(f.host().call(abi::binding{}, abi::bytes{nullptr, 0}, &text) == abi::invalid_argument);
    CHECK(consumer.bind_credential(abi::token{}, "open", &scratch) == abi::invalid_argument);
    CHECK(scratch.value == 0);
    CHECK(consumer.release_token(abi::token{}) == abi::invalid_argument);
    CHECK(consumer.unbind_binding(abi::binding{}) == abi::invalid_argument);
    CHECK(consumer.call_binding(abi::binding{}, abi::bytes{nullptr, 0}, &text) == abi::invalid_argument);

    // A credential of another context is refused on both sides and is never returned by the
    // wrong owner.
    scratch = abi::binding{0xFEED};
    CHECK(f.host().bind(consumer_token, std::string("open"), &scratch) == abi::invalid_argument);
    CHECK(scratch.value == 0);
    CHECK(f.host().unbind(consumer_binding) == abi::invalid_argument);
    CHECK(f.host().call(consumer_binding, abi::bytes{nullptr, 0}, &text) == abi::invalid_argument);
    CHECK(f.host().release(consumer_token) == abi::invalid_argument);
    CHECK(consumer.bind_credential(admin.credential, "open", &scratch) == abi::invalid_argument);
    CHECK(consumer.unbind_binding(admin_binding) == abi::invalid_argument);
    CHECK(consumer.call_binding(admin_binding, abi::bytes{nullptr, 0}, &text) == abi::invalid_argument);
    CHECK(consumer.release_token(admin.credential) == abi::invalid_argument);
    CHECK(consumer.token().value == consumer_token.value);
    CHECK(f.host().call(admin_binding, abi::bytes{nullptr, 0}, &text) == abi::ok);
    CHECK(consumer.read_current(&text) == abi::ok);

    // Both the native facade and the plugin service reject another thread with wrong_thread.
    abi::status off_native = abi::ok;
    abi::status off_service = abi::ok;
    std::thread other([&] {
        off_native = f.host().release(admin.credential);
        off_service = consumer.release_token(consumer_token);
    });
    other.join();
    CHECK(off_native == abi::wrong_thread);
    CHECK(off_service == abi::wrong_thread);
    // Neither refusal touched the live leases.
    CHECK(consumer.token().value == consumer_token.value);
    CHECK(consumer.read_current(&text) == abi::ok);
    CHECK(f.host().bind(admin.credential, std::string("open"), &scratch) == abi::ok);
    CHECK(f.host().unbind(scratch) == abi::ok);

    CHECK(f.host().release(admin.credential) == abi::ok);
    CHECK(consumer.release_lease() == abi::ok);
    CHECK(f.host().unload("owner.provider") == abi::ok);
    CHECK(f.host().shutdown() == abi::ok);
}

/** @brief Case 6: an in-flight call pins its lease, from invoke and from output delivery. */
void case_lease_busy_in_flight()
{
    g_current_test = "lease_busy_in_flight";
    fixture f;
    const abi::contract proto{abi::iid{0x8888888800000008ULL, 1}, 1, 0};
    provider_factory& provider_factory_ref = f.provider("busy.provider", proto);
    consumer_factory& factory = f.consumer("busy.consumer");
    CHECK(f.host().add(&provider_factory_ref) == abi::ok);
    CHECK(f.host().add(&factory) == abi::ok);
    CHECK(f.start() == abi::ok);

    fake_consumer& consumer = factory.instance();
    CHECK(consumer.connect("busy.provider", proto) == abi::ok);
    const abi::token held = consumer.token();
    CHECK(held.value != 0);

    // The provider tries to return the consumer's lease from inside invoke(), the writer tries the
    // same while the result is delivered. Both attempts must be refused with busy.
    fake_provider& provider = *provider_factory_ref.instance();
    provider.probe.release_caps = consumer.caps_service();
    provider.probe.release_token = held;

    probe_writer writer;
    writer.release_caps = consumer.caps_service();
    writer.release_token = held;

    std::string session;
    // The probe writer appends into its own target, so the session token is read back from there.
    CHECK(consumer.open_session(&writer, &writer.value) == abi::ok);
    session = writer.value;
    CHECK(provider.probe.release_ran);
    CHECK(provider.probe.release_status == abi::busy);
    CHECK(writer.release_ran);
    CHECK(writer.release_status == abi::busy);
    CHECK(writer.result == abi::ok);
    CHECK(!session.empty());

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

/** @brief Case 7: the host queries invoke_iid only; binding always needs a lease. */
void case_query_isolated_to_invoke()
{
    g_current_test = "query_isolated_to_invoke";
    fixture f;
    const abi::contract proto{abi::iid{0x9999999900000009ULL, 1}, 1, 0};
    provider_factory& provider_factory_ref = f.provider("query.provider", proto);
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
    CHECK(consumer.connect("query.provider", proto) == abi::ok);
    std::string session;
    std::string payload;
    CHECK(consumer.open_session(&session) == abi::ok);
    CHECK(consumer.read_current(&payload) == abi::ok);
    // Lease acquisition, binding and invocation are host bookkeeping; no plugin query runs.
    CHECK(provider.query_invoke_calls == 1);
    CHECK(provider.query_other_calls == 0);
    // The consumer announces no business capability, so the host never queries it at all.
    CHECK(consumer.plugin_query_calls == 0);

    CHECK(consumer.release_lease() == abi::ok);
    CHECK(f.host().unload("query.provider") == abi::ok);
    CHECK(f.host().shutdown() == abi::ok);
}

/** @brief Case 8: an unload requested from inside a callback is deferred to a safe point. */
void case_callback_unload_deferred()
{
    g_current_test = "callback_unload_deferred";
    fixture f;
    const abi::contract proto{abi::iid{0xAAAAAAAA0000000AULL, 1}, 1, 0};
    provider_factory& provider_factory_ref = f.provider("defer.provider", proto);
    provider_factory& victim_factory = f.provider("defer.victim", proto);
    consumer_factory& factory = f.consumer("defer.consumer");
    CHECK(f.host().add(&provider_factory_ref) == abi::ok);
    CHECK(f.host().add(&victim_factory) == abi::ok);
    CHECK(f.host().add(&factory) == abi::ok);
    CHECK(f.start() == abi::ok);
    CHECK(f.alive("defer.victim"));

    fake_consumer& consumer = factory.instance();
    CHECK(consumer.connect("defer.provider", proto) == abi::ok);

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

/** @brief Case 8b: the admin revoker runs under the depth guard and cannot re-enter structure. */
void case_admin_revoker_depth_guard()
{
    g_current_test = "admin_revoker_depth_guard";
    fixture f;
    const abi::contract proto{abi::iid{0xBBBBBBBB0000000BULL, 1}, 1, 0};
    provider_factory& provider_factory_ref = f.provider("revoke.provider", proto);
    provider_factory& victim_factory = f.provider("revoke.victim", proto);
    provider_factory& spare_factory = f.provider("revoke.spare", proto);
    CHECK(f.host().add(&provider_factory_ref) == abi::ok);
    CHECK(f.host().add(&victim_factory) == abi::ok);
    CHECK(f.start() == abi::ok);

    structural_revoker revoker;
    revoker.host = &f.host();
    revoker.victim = "revoke.victim";
    revoker.spare = &spare_factory;

    abi::borrow lease{};
    CHECK(f.host().acquire("revoke.provider", proto, &revoker, &lease) == abi::ok);

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

    // Returning the credential at a safe point completes both the retried teardown and the
    // deferred request the callback queued.
    CHECK(f.host().release(lease.credential) == abi::ok);
    CHECK(f.host().unload("revoke.provider") == abi::ok);
    CHECK(!f.alive("revoke.provider"));
    CHECK(!f.alive("revoke.victim"));
    CHECK(f.host().shutdown() == abi::ok);
}

/** @brief Case 9: a failing temporary call and a failing leased call leave no dangling lease. */
void case_failed_call_no_dangling_lease()
{
    g_current_test = "failed_call_no_dangling_lease";
    fixture f;
    const abi::contract proto{abi::iid{0xCCCCCCCC0000000CULL, 1}, 1, 5};
    provider_factory& one_shot_factory = f.provider("oneshot.provider", proto);
    provider_factory& leased_factory = f.provider("leased.provider", proto);
    consumer_factory& consumer_factory_ref = f.consumer("oneshot.consumer");
    CHECK(f.host().add(&one_shot_factory) == abi::ok);
    CHECK(f.host().add(&leased_factory) == abi::ok);
    CHECK(f.host().add(&consumer_factory_ref) == abi::ok);
    CHECK(f.start() == abi::ok);

    // The one-shot administration call states its contract explicitly, and every failure path
    // clears the output and returns its temporary lease and binding.
    std::string out("dirty");
    CHECK(f.host().call("oneshot.provider", abi::contract{proto.id, 2, 0}, std::string("open"),
                        abi::bytes{nullptr, 0}, &out) == abi::unsupported);
    CHECK(out.empty());
    CHECK(f.host().call("oneshot.provider", proto, std::string("absent"), abi::bytes{nullptr, 0},
                        &out) == abi::not_found);
    CHECK(out.empty());
    out = "dirty";
    CHECK(f.host().call("oneshot.provider", proto, std::string("fail"), abi::bytes{nullptr, 0},
                        &out) == abi::failed);
    CHECK(out.empty()); // The partial output of the failing invocation is discarded.
    CHECK(f.host().call("oneshot.provider", proto, std::string("open"), abi::bytes{nullptr, 0},
                        &out) == abi::ok);
    CHECK(out.rfind("S", 0) == 0);
    // No borrowing is left behind: the provider unloads directly.
    CHECK(f.host().unload("oneshot.provider") == abi::ok);

    // A failing leased call keeps the consumer's own lease, which stays usable until returned.
    fake_consumer& consumer = consumer_factory_ref.instance();
    CHECK(consumer.connect("leased.provider", proto) == abi::ok);
    const abi::token held = consumer.token();
    std::string failed("");
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
    case_contract_mismatch_no_lock();
    case_discovery_and_compatible_minor();
    case_same_protocol_reload_state();
    case_major_upgrade_reload();
    case_multi_lease_lifetime();
    case_owner_zero_thread_validation();
    case_lease_busy_in_flight();
    case_query_isolated_to_invoke();
    case_callback_unload_deferred();
    case_admin_revoker_depth_guard();
    case_failed_call_no_dangling_lease();

    std::printf("all contract lease acceptance checks passed\n");
    return 0;
}
