/**
 * @file runtime_test.cc
 * @brief Self-contained contract checks for the 42U host runtime; no external test framework.
 *
 * ABI v2 is invoke-only: plugins never share a business class, a business interface pointer or a
 * provider iinvoke pointer. The only business path is consumer -> host icalls -> provider
 * iinvoke. The fake plugins below therefore announce a data-only protocol contract
 * (abi::contract{id, major, minor}) plus their method list, and every borrow is verified through
 * its opaque credential and a controlled business call, never through a stored pointer.
 *
 * The file owns its main() and only depends on the public headers <42u/abi.hpp> and
 * <42u/host.hpp>, so it stays independent of the host implementation that is still being
 * written. Until the library links, validate it with a syntax-only compile:
 *
 *   g++ -std=c++17 -Wall -Wextra -Werror -Iinc -fsyntax-only tests/runtime_test.cc
 *
 * Structure:
 *  - a configurable in-process fake factory/iplug pair (see @ref fake_factory, @ref fake_plug)
 *    whose lifetime always exceeds the host under test;
 *  - a single global test trace that records lifecycle entry points in call order;
 *  - CHECK for the test body and RECORD for code that runs inside a noexcept plugin
 *    callback: RECORD only counts the failure, the surrounding test (or main) observes it;
 *  - @ref admin_lease: a control-thread administration lease whose revoker outlives the
 *    credential, used to reach the host bind/call entries without repeating acquire/release.
 *
 * Covered contracts (design baseline docs/42U插件框架设计.md, migration contract
 * docs/thinks/invoke-v2-implementation-contract.md):
 *  - all plugins init before the first start; start order equals the initialization plan;
 *  - priority and before/after drive that single shared order;
 *  - stop and destroy run in reverse plan order;
 *  - init failure rolls back, nothing starts, the failed instance is never stopped;
 *  - start failure revokes announced capabilities and stops every initialized instance;
 *  - duplicate plug_id is refused;
 *  - unknown interface queries clear their output; known host services resolve;
 *  - a plugin query exposes only the host-private invoke_iid subobject and no business
 *    interface, and the host reaches the method through that gateway;
 *  - capabilities exist only after a successful start: an initialized consumer may lease and
 *    bind but must not issue a business call;
 *  - a lease is a credential, not a pointer: bindings are created from the credential and a
 *    call goes through the host gateway to the provider's iinvoke;
 *  - name and numeric method binding reach the same implementation;
 *  - unknown method/plugin, null required pointers, zero credentials and stale bindings return a
 *    definite error and clear the caller-owned output;
 *  - unbinding a method never returns the lease; releasing the lease invalidates its bindings;
 *  - partial output of a failed call is discarded, and the output limit stays sticky even if the
 *    plugin ignores the writer failure;
 *  - revoking a lease lets the provider unload; an unreturned lease isolates the provider;
 *  - after a reload the old binding and the old lease credential are stale while the new
 *    generation is usable after a fresh acquire and bind;
 *  - unloading a consumer first returns its outbound leases;
 *  - a borrow cycle is fully returned by shutdown without recursive unloading.
 *
 * Deliberately not covered here:
 *  - a fail-stop instance that is never destroyed (the host intentionally pins unsafe
 *    instances, so an ordinary ASan/LSan case would report the pinned object as a leak);
 *    that needs a dedicated subprocess case and is left out for now;
 *  - boot()/load()/scan_plugins(): they need real .u42.so artefacts, not available in a
 *    syntax-only stage;
 *  - event publish/subscribe and cross-library ABI compatibility, which are outside the
 *    contract set requested for this file.
 *
 * @note NDEBUG is defined deliberately, mirroring tests/order_test.cc: CHECK must keep
 *       reporting failures even when assert() has been compiled out.
 */
#define NDEBUG 1

#include <42u/abi.hpp>
#include <42u/host.hpp>

#include <algorithm>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace abi = u42::abi::v2;

// A lease is a credential and nothing else: these guards fail the build if a business pointer
// ever creeps back into borrow or into the capability announcement.
static_assert(sizeof(abi::borrow) == sizeof(abi::token),
              "a v2 lease must carry only a credential, never a business pointer");
static_assert(alignof(abi::borrow) == alignof(abi::token),
              "a v2 lease must not gain alignment from a hidden pointer");
static_assert(sizeof(abi::caps_desc) == 40,
              "a v2 capability set carries a protocol and methods, not an interface list");

/* ------------------------------------------------------------------ *
 * Test harness: CHECK aborts with a non-zero exit status, RECORD only
 * counts a failure for the surrounding test to observe.
 * ------------------------------------------------------------------ */

const char* g_current_test = nullptr;
std::size_t g_failures = 0;
bool g_quiet = false;
using check_hook = void (*)(const char* expr, const char* file, int line);
check_hook g_check_hook = nullptr;
std::jmp_buf* g_jump_target = nullptr;
const char* g_reported_expression = nullptr;

/**
 * @brief Print one failure; an installed hook runs first so harness tests can intercept it.
 *
 * @param kind Failure channel, "CHECK" or "RECORD".
 * @param expr Text of the failed expression.
 * @param file Source file of the failed expression.
 * @param line Source line of the failed expression.
 * @param detail Optional extra detail, already formatted.
 */
void report_failure(const char* kind, const char* expr, const char* file, int line,
                    const std::string& detail)
{
    if (g_check_hook != nullptr) g_check_hook(expr, file, line);
    if (g_quiet) return;
    std::fprintf(stderr, "%s failure in %s: %s%s%s (%s:%d)\n", kind,
                 g_current_test != nullptr ? g_current_test : "?", expr,
                 detail.empty() ? "" : " -- ", detail.c_str(), file, line);
    std::fflush(stderr);
}

/**
 * @brief Record a fatal CHECK failure and terminate with a non-zero exit status.
 *
 * @param expr Text of the failed expression.
 * @param file Source file of the failed expression.
 * @param line Source line of the failed expression.
 */
[[noreturn]] void fail_check(const char* expr, const char* file, int line)
{
    ++g_failures;
    report_failure("CHECK", expr, file, line, std::string());
    std::exit(EXIT_FAILURE);
}

/** @brief Failure reporting that never compiles away, unlike assert(). */
#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) fail_check(#condition, __FILE__, __LINE__);            \
    } while (false)

/**
 * @brief Count a failure without aborting; only valid outside plugin callbacks.
 *
 * @param expr Text of the failed expression.
 * @param file Source file of the failed expression.
 * @param line Source line of the failed expression.
 * @param detail Optional extra detail.
 */
void record_check(const char* expr, const char* file, int line, const std::string& detail = {})
{
    ++g_failures;
    report_failure("RECORD", expr, file, line, detail);
}

/**
 * @brief Count a plain invariant failure that has no single expression attached.
 *
 * @param what Short category, for example "invariant".
 * @param file Source file of the violated invariant.
 * @param line Source line of the violated invariant.
 * @param detail Human-readable description.
 */
void record_note(const char* what, const char* file, int line, const std::string& detail)
{
    ++g_failures;
    report_failure(what, "<invariant>", file, line, detail);
}

/**
 * @brief Deferred check for noexcept plugin callbacks; never aborts and never throws.
 *
 * Plugin callbacks are noexcept by contract, so a failure there must not unwind or exit:
 * it is recorded and the surrounding test observes the global failure count.
 */
#define RECORD(condition)                                                       \
    do {                                                                        \
        if (!(condition)) record_check(#condition, __FILE__, __LINE__);          \
    } while (false)

/** @brief Deferred check that reports an observed status alongside the expression. */
#define RECORD_STATUS(condition, actual)                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            record_check(#condition, __FILE__, __LINE__,                        \
                         "status=" + std::to_string(static_cast<unsigned>(actual))); \
        }                                                                       \
    } while (false)

/** @brief Report a broken test-side invariant from inside a noexcept callback. */
#define RECORD_NOTE(message) record_note("invariant", __FILE__, __LINE__, (message))

#define U42_STRINGIFY(...) #__VA_ARGS__
#define U42_EXPAND_STRING(...) U42_STRINGIFY(__VA_ARGS__)

/**
 * @brief Compile-time guard that CHECK calls fail_check and never assert().
 *
 * With NDEBUG defined, assert() collapses to ((void)0) and every failure would vanish.
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

/* ------------------------------------------------------------------ *
 * Global lifecycle trace.
 * ------------------------------------------------------------------ */

/** @brief Ordered lifecycle entries as "<kind>:<plug_id>". */
std::vector<std::string>& trace_log()
{
    static std::vector<std::string> entries;
    return entries;
}

/** @brief Append one lifecycle observation. */
void trace(const std::string& kind, const std::string& plug_id)
{
    trace_log().push_back(kind + ":" + plug_id);
}

/** @brief Ordered plugin identities recorded under one kind. */
std::vector<std::string> trace_ids(const char* kind)
{
    const std::string prefix = std::string(kind) + ":";
    std::vector<std::string> ids;
    for (const std::string& entry : trace_log()) {
        if (entry.compare(0, prefix.size(), prefix) == 0) ids.push_back(entry.substr(prefix.size()));
    }
    return ids;
}

/** @brief Number of entries recorded for one kind and identity. */
std::size_t trace_count(const char* kind, const std::string& plug_id)
{
    std::size_t count = 0;
    for (const std::string& id : trace_ids(kind)) {
        if (id == plug_id) ++count;
    }
    return count;
}

/** @brief Index of the first entry of one kind, or npos when the kind never appeared. */
std::size_t first_of_kind(const char* kind)
{
    const std::string prefix = std::string(kind) + ":";
    const std::vector<std::string>& entries = trace_log();
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (entries[index].compare(0, prefix.size(), prefix) == 0) return index;
    }
    return std::string::npos;
}

/** @brief Index of the last entry of one kind, or npos when the kind never appeared. */
std::size_t last_of_kind(const char* kind)
{
    const std::string prefix = std::string(kind) + ":";
    const std::vector<std::string>& entries = trace_log();
    for (std::size_t index = entries.size(); index > 0; --index) {
        if (entries[index - 1].compare(0, prefix.size(), prefix) == 0) return index - 1;
    }
    return std::string::npos;
}

/** @brief Index of one exact "<kind>:<id>" entry, or npos. */
std::size_t trace_position(const char* kind, const std::string& plug_id)
{
    const std::string wanted = std::string(kind) + ":" + plug_id;
    const std::vector<std::string>& entries = trace_log();
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (entries[index] == wanted) return index;
    }
    return std::string::npos;
}

/* ------------------------------------------------------------------ *
 * Small assertion helpers with readable diagnostics.
 * ------------------------------------------------------------------ */

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

/** @brief Compare record sets without depending on an unordered container. */
std::vector<std::string> sorted(std::vector<std::string> values)
{
    std::sort(values.begin(), values.end());
    return values;
}

/** @brief Human-readable name of an ABI status. */
const char* status_name(abi::status value)
{
    switch (value) {
    case abi::ok: return "ok";
    case abi::invalid_argument: return "invalid_argument";
    case abi::unsupported: return "unsupported";
    case abi::not_found: return "not_found";
    case abi::duplicate: return "duplicate";
    case abi::invalid_state: return "invalid_state";
    case abi::busy: return "busy";
    case abi::stale: return "stale";
    case abi::limit_exceeded: return "limit_exceeded";
    case abi::failed: return "failed";
    case abi::wrong_thread: return "wrong_thread";
    case abi::cycle: return "cycle";
    case abi::deferred: return "deferred";
    default: return "unknown";
    }
}

/** @brief Assert an exact status, printing both codes before aborting. */
void check_status(const char* label, abi::status actual, abi::status expected)
{
    if (actual == expected) return;
    std::fprintf(stderr, "%s: status %u (%s) != %u (%s)\n", label,
                 static_cast<unsigned>(actual), status_name(actual),
                 static_cast<unsigned>(expected), status_name(expected));
    std::fflush(stderr);
    CHECK(actual == expected);
}

/** @brief Assert that a call failed, printing the code before aborting. */
void check_not_ok(const char* label, abi::status actual)
{
    if (actual != abi::ok) return;
    std::fprintf(stderr, "%s: expected a failure but got ok\n", label);
    std::fflush(stderr);
    CHECK(actual != abi::ok);
}

/**
 * @brief Assert that a status is one of an explicitly allowed set.
 *
 * Used where the contract names a set rather than a single code, for example an
 * already-withdrawn but still-live lease.
 */
void check_one_of(const char* label, abi::status actual, std::initializer_list<abi::status> allowed)
{
    for (const abi::status candidate : allowed) {
        if (actual == candidate) return;
    }
    std::string text;
    for (const abi::status candidate : allowed) {
        if (!text.empty()) text += " or ";
        text += status_name(candidate);
    }
    std::fprintf(stderr, "%s: status %u (%s) is not %s\n", label,
                 static_cast<unsigned>(actual), status_name(actual), text.c_str());
    std::fflush(stderr);
    CHECK(false);
}

/** @brief Assert an ordered identity sequence, printing both sides on mismatch. */
void check_sequence(const char* label, const std::vector<std::string>& actual,
                    const std::vector<std::string>& expected)
{
    if (actual == expected) return;
    std::fprintf(stderr, "%s: expected [%s] but got [%s]\n", label, join(expected).c_str(),
                 join(actual).c_str());
    std::fflush(stderr);
    CHECK(actual == expected);
}

/* ------------------------------------------------------------------ *
 * Test-only data contract and writer.
 * Neither type is part of the ABI; both sides of the test are built together.
 * ------------------------------------------------------------------ */

/** @brief Business protocol family announced by every fake provider in this file. */
inline constexpr abi::iid test_protocol{0x7465737434325532ULL, 0x0001};

/** @brief Data-only contract every fake provider offers and every fake consumer requires. */
inline constexpr abi::contract test_contract{test_protocol, 1, 0};

/** @brief Method identity of the fake provider's dynamic methods. */
inline constexpr abi::method_id echo_method = 7;
inline constexpr abi::method_id ping_method = 9;

/** @brief Minimal caller-owned writer for direct icalls::call invocations. */
struct buffer_writer final : abi::iwriter {
    std::string value;
    abi::status result = abi::ok;

    abi::status U42_CALL write(abi::bytes data) noexcept override
    {
        if (result != abi::ok) return result;
        if (data.data == nullptr && data.size != 0) return result = abi::invalid_argument;
        if (data.size != 0) {
            value.append(static_cast<const char*>(data.data), static_cast<std::size_t>(data.size));
        }
        return abi::ok;
    }
};

/* ------------------------------------------------------------------ *
 * Administration lease helper.
 * ------------------------------------------------------------------ */

/**
 * @brief Control-thread administration lease whose revoker outlives the credential.
 *
 * Acquiring a lease needs a revocation receiver that stays alive and at a stable address until
 * the credential is returned. This helper owns both, so a test can hold the host bind/call
 * entries without repeating acquire/release, and the receiver is still valid when the provider
 * is revoked by unload() or shutdown(); it then returns the credential immediately instead of
 * fabricating a borrow.
 *
 * @note The revoker is a member of this object and the object lives for the whole lease
 *       lifetime; a temporary or stack-copied receiver would be a dangling pointer.
 */
class admin_lease {
public:
    /**
     * @brief Acquire one administration lease and keep its receiver alive.
     *
     * @param host Host under test, bound to the constructing control thread.
     * @param plug_id Provider identity.
     * @param required Explicitly accepted protocol.
     */
    admin_lease(u42::host& host, std::string plug_id, abi::contract required)
        : host_(&host), plug_id_(std::move(plug_id))
    {
        status_ = host_->acquire(plug_id_, required, &revoker_, &borrow_);
    }

    ~admin_lease() { (void)release(); }
    admin_lease(const admin_lease&) = delete;
    admin_lease& operator=(const admin_lease&) = delete;

    /** @brief Status of the acquire() performed by the constructor. */
    abi::status status() const noexcept { return status_; }

    /** @brief True while this object still owns a credential. */
    bool holds() const noexcept { return borrow_.credential.value != 0; }

    /** @brief Current credential; zero when the lease was refused or already returned. */
    abi::token credential() const noexcept { return borrow_.credential; }

    /** @brief Number of revocation callbacks observed on the owned receiver. */
    std::uint64_t revocations() const noexcept { return revoker_.calls; }

    /**
     * @brief Return the credential through the host and clear the local token on ok/stale.
     *
     * @return The host's release status; a busy/failed return keeps ownership here so the
     *         caller can retry instead of pretending the lease was returned.
     */
    abi::status release()
    {
        if (borrow_.credential.value == 0) return abi::stale;
        const abi::status released = host_->release(borrow_.credential);
        if (released == abi::ok || released == abi::stale) borrow_ = {};
        return released;
    }

    /** @brief Receiver that hands the credential back during revocation. */
    struct revoker_type final : abi::irevoker {
        admin_lease* owner = nullptr;
        std::uint64_t calls = 0;
        abi::status release_status = abi::failed;

        explicit revoker_type(admin_lease* value) noexcept : owner(value) {}
        revoker_type(const revoker_type&) = delete;
        revoker_type& operator=(const revoker_type&) = delete;

        void U42_CALL on_revoke(abi::token value) noexcept override
        {
            ++calls;
            release_status = owner->host_->release(value);
            if (release_status == abi::ok || release_status == abi::stale) owner->borrow_ = {};
        }
    };

private:
    u42::host* host_ = nullptr;
    std::string plug_id_;
    abi::borrow borrow_{};
    abi::status status_ = abi::failed;
    revoker_type revoker_{this};
};

/* ------------------------------------------------------------------ *
 * Configurable fake plugin.
 * ------------------------------------------------------------------ */

class fake_factory;

/** @brief Behavior switches of one fake plugin instance. */
struct fake_behavior {
    abi::status init_status = abi::ok;
    abi::status start_status = abi::ok;
    abi::status stop_status = abi::ok;
    /** Announce the protocol and both methods during start(). */
    bool announce = true;
    /** Register an icaps::watch during init(). */
    bool watch_in_init = false;
    /** Run the lease/bind/call probes during init(). */
    bool probe_in_init = false;
    /** Run the same probes from the capability callback, while still Initialized. */
    bool probe_on_capability = false;
    /** Resolve all four host services and log through idiag during init(). */
    bool query_services_in_init = false;
    /** Probe unknown and null host-context queries during init(). */
    bool query_unknown_in_init = false;
    /** Echo writes partial output and then fails. */
    bool partial_output = false;
    /** Echo with empty arguments keeps writing after the writer reported a failure. */
    bool limit_blow = false;
    /** Never return the lease credential from on_revoke(). */
    bool ignore_revocation = false;
    /** Protocol this instance announces; a data-only contract, never an interface. */
    abi::contract protocol = test_contract;
    /** Protocol this instance requires when it leases a peer. */
    abi::contract required = test_contract;
    /** Peer plugin identity used by the init probes and acquire_peer(). */
    std::string peer;
};

/** @brief One capability notification copied out of the borrowed callback data. */
struct cap_notice {
    std::string plug_id;
    bool available = false;
    bool saw_null_plug_id = false;
    bool counts_consistent = false;
    bool methods_described = true;
    abi::contract protocol{};
    std::vector<abi::method_id> methods;
    std::vector<std::string> method_names;
};

class fake_plug;

/** @brief Capability sink owned by the fake consumer for its whole instance lifetime. */
struct cap_recorder final : abi::icap_sink {
    fake_plug* owner = nullptr;
    std::vector<cap_notice> notices;
    std::uint64_t callback_count = 0;
    std::uint64_t null_events = 0;

    explicit cap_recorder(fake_plug* value) noexcept : owner(value) {}
    cap_recorder(const cap_recorder&) = delete;
    cap_recorder& operator=(const cap_recorder&) = delete;

    void U42_CALL on_capability(const abi::cap_event* value) noexcept override;

    /** @brief Most recent notification for one provider, or null. */
    const cap_notice* last_for(const std::string& plug_id) const;
    /** @brief Availability flags recorded for one provider, in delivery order. */
    std::vector<bool> availability_for(const std::string& plug_id) const;
};

/** @brief Revocation receiver owned by the fake consumer; never copied by the host. */
struct lease_revoker final : abi::irevoker {
    fake_plug* owner = nullptr;
    std::uint64_t calls = 0;
    std::uint64_t reentrant_calls = 0;
    bool notified = false;
    bool self_ok = true;
    bool release_attempted = false;
    bool in_callback = false;
    abi::token last{};
    abi::status release_status = abi::ok;

    explicit lease_revoker(fake_plug* value) noexcept : owner(value) {}
    lease_revoker(const lease_revoker&) = delete;
    lease_revoker& operator=(const lease_revoker&) = delete;

    void U42_CALL on_revoke(abi::token credential) noexcept override;
};

/**
 * @brief Test-only extra polymorphic base.
 *
 * It exists so the fake has a second non-primary base address: the invoke conversion check is
 * only meaningful when the iinvoke subobject is not at offset zero. It declares no function that
 * the host or another plugin could mistake for a business entry point.
 */
struct plug_extra {
    virtual ~plug_extra() = default;
    std::uint64_t marker = 0;
};

/** @brief Instance-side fake plugin: provider, consumer and host probe in one type. */
class fake_plug final : public abi::iplug, public abi::iinvoke, public plug_extra {
public:
    explicit fake_plug(fake_factory& owner) noexcept;

    // iplug
    abi::status U42_CALL init(abi::ictx* ctx) noexcept override;
    abi::status U42_CALL start() noexcept override;
    abi::status U42_CALL stop() noexcept override;
    void U42_CALL destroy() noexcept override;
    abi::status U42_CALL query(const abi::iid* type, void** out) noexcept override;
    // iinvoke: the host-private gateway, never handed to another plugin.
    abi::status U42_CALL invoke(abi::method_id method, abi::bytes args,
                               abi::iwriter* result) noexcept override;

    // Test-side driving of the consumer role.
    abi::status acquire_from(const std::string& provider);
    abi::status acquire_peer();
    /** @brief Bind "echo" under the credential of the currently held lease. */
    abi::status bind_bound_method();
    /** @brief Bind one named method under the currently held credential. */
    abi::status bind_named(const std::string& method, abi::binding* out);
    /** @brief Invoke the bound method with the given text through the host gateway. */
    abi::status call_bound_method(std::string text, std::string* out);
    abi::status release_lease();
    abi::status release_token(abi::token value);
    /** @brief Lease and bind the provider from inside a capability callback. */
    void run_capability_probe(const std::string& provider);
    bool holds_lease() const noexcept { return lease_.credential.value != 0; }
    bool has_binding() const noexcept { return binding_.value != 0; }
    /** @brief Note a credential returned by the host revocation protocol. */
    void on_lease_returned(abi::token value, abi::status status) noexcept;

    const std::string& id() const noexcept;
    const fake_behavior& behavior() const noexcept;

    // Lifecycle observations.
    std::uint64_t base_address = 0;
    std::uint64_t extra_address = 0;
    std::uint64_t invoke_address = 0;
    std::uint64_t init_calls = 0;
    std::uint64_t start_calls = 0;
    std::uint64_t stop_calls = 0;
    std::uint64_t destroy_calls = 0;
    bool destroyed = false;
    // Host-context probes.
    abi::status service_events = abi::failed;
    abi::status service_caps = abi::failed;
    abi::status service_calls = abi::failed;
    abi::status service_diag = abi::failed;
    bool services_resolved = false;
    bool diag_logged = false;
    abi::status unknown_iface_status = abi::failed;
    bool unknown_iface_cleared = false;
    abi::status null_type_status = abi::failed;
    bool null_type_untouched = false;
    abi::status null_out_status = abi::failed;
    // Capability and lease probes.
    abi::status announce_status = abi::failed;
    std::uint64_t announce_calls = 0;
    abi::status probe_acquire = abi::failed;
    bool probe_acquire_cleared = false;
    bool probe_business_blocked = false;
    bool probe_binding_cleared = false;
    bool probe_seen = false;
    /** True when the capability probe ran before this instance was started. */
    bool probe_while_initialized = false;
    abi::status probe_bind_name = abi::failed;
    abi::status probe_bind_id = abi::failed;
    abi::status probe_call = abi::failed;
    /** Binding value handed back while Initialized; a refused call must not clear it. */
    std::uint64_t probe_binding_value = 0;
    bool probe_binding_intact = false;
    abi::status watch_status = abi::failed;
    abi::token watch_token{};
    // Invocation observations.
    std::uint64_t invoke_calls = 0;
    abi::method_id last_invoke_method = 0;
    std::uint64_t writes_attempted = 0;
    std::int64_t first_failure_index = -1;
    abi::status observed_after_failure = abi::ok;
    // Consumer state: a credential plus a binding created from it, never a business pointer.
    abi::icaps* caps_ = nullptr;
    abi::icalls* calls_ = nullptr;
    cap_recorder cap_sink{this};
    lease_revoker revoker{this};
    abi::borrow lease_{};
    abi::binding binding_{};
    abi::status last_acquire = abi::failed;
    bool last_acquire_cleared = false;
    abi::status last_release = abi::failed;
    std::uint64_t acquire_calls = 0;
    std::uint64_t lease_returns = 0;
    std::uint64_t stale_release_calls = 0;

private:
    fake_factory& owner_;
};

/** @brief Library-owned fake factory with stable descriptor storage. */
class fake_factory final : public abi::iplug_fty {
public:
    /**
     * @brief Build one fake plugin type with immutable metadata.
     *
     * @param plug_id Stable plugin identity; descriptors borrow this storage.
     * @param behavior Per-instance behavior switches.
     * @param priority Initialization priority; lower runs first.
     * @param before Identities that must initialize after this plugin.
     * @param after Identities that must initialize before this plugin.
     */
    fake_factory(std::string plug_id, fake_behavior behavior, std::int32_t priority,
                 std::vector<std::string> before, std::vector<std::string> after);
    fake_factory(const fake_factory&) = delete;
    fake_factory& operator=(const fake_factory&) = delete;

    abi::status U42_CALL describe(const abi::plug_desc** out) noexcept override;
    abi::status U42_CALL create(abi::iplug** out) noexcept override;

    const std::string& id() const noexcept { return plug_id_; }
    const fake_behavior& behavior() const noexcept { return behavior_; }
    /** @brief Announced capability set, borrowed by the host during start(). */
    const abi::caps_desc* announced() const noexcept { return &caps_; }
    fake_plug* instance() const noexcept { return instance_.get(); }

    std::uint64_t create_calls = 0;

private:
    struct method_holder {
        std::string name;
        std::string description;
        std::string input_schema;
        std::string output_schema;
        abi::method_desc desc{};
    };

    /** @brief Point every descriptor field at storage that will not move again. */
    void finalize();

    std::string plug_id_;
    std::string version_ = "1.0";
    std::int32_t priority_ = 0;
    std::vector<std::string> before_;
    std::vector<std::string> after_;
    std::vector<const char*> before_ptrs_;
    std::vector<const char*> after_ptrs_;
    abi::plug_desc desc_{};
    std::vector<method_holder> methods_;
    std::vector<abi::method_desc> method_descs_;
    abi::caps_desc caps_{};
    fake_behavior behavior_;
    std::unique_ptr<fake_plug> instance_;
};

/* ------------------------------------------------------------------ *
 * Fake implementation.
 * ------------------------------------------------------------------ */

static_assert(noexcept(std::declval<fake_plug&>().init(nullptr)),
              "ABI lifecycle entry points must be noexcept");
static_assert(noexcept(std::declval<fake_plug&>().start()),
              "ABI lifecycle entry points must be noexcept");
static_assert(noexcept(std::declval<fake_plug&>().destroy()),
              "ABI lifecycle entry points must be noexcept");
static_assert(noexcept(std::declval<fake_plug&>().invoke(0, abi::bytes{}, nullptr)),
              "ABI call entry points must be noexcept");
static_assert(noexcept(std::declval<cap_recorder&>().on_capability(nullptr)),
              "sink callbacks must be noexcept");
static_assert(noexcept(std::declval<lease_revoker&>().on_revoke(abi::token{})),
              "revocation callbacks must be noexcept");

fake_factory::fake_factory(std::string plug_id, fake_behavior behavior, std::int32_t priority,
                           std::vector<std::string> before, std::vector<std::string> after)
    : plug_id_(std::move(plug_id)), priority_(priority), before_(std::move(before)),
      after_(std::move(after)), behavior_(std::move(behavior))
{
    before_ptrs_.reserve(before_.size());
    after_ptrs_.reserve(after_.size());
    methods_.reserve(2);
    methods_.push_back(method_holder{"echo", "Echo the request bytes back", "{}", "{}",
                                     abi::method_desc{}});
    methods_.push_back(method_holder{"ping", "Reply with a fixed pong", "{}", "{}",
                                     abi::method_desc{}});
    methods_[0].desc.id = echo_method;
    methods_[1].desc.id = ping_method;
    finalize();
}

void fake_factory::finalize()
{
    for (const std::string& value : before_) before_ptrs_.push_back(value.c_str());
    for (const std::string& value : after_) after_ptrs_.push_back(value.c_str());
    desc_.struct_size = sizeof(abi::plug_desc);
    desc_.reserved = 0;
    desc_.plug_id = plug_id_.c_str();
    desc_.version = version_.c_str();
    desc_.priority = priority_;
    desc_.before_count = static_cast<std::uint32_t>(before_ptrs_.size());
    desc_.before = before_ptrs_.empty() ? nullptr : before_ptrs_.data();
    desc_.after_count = static_cast<std::uint32_t>(after_ptrs_.size());
    desc_.after = after_ptrs_.empty() ? nullptr : after_ptrs_.data();
    for (method_holder& method : methods_) {
        method.desc.name = method.name.c_str();
        method.desc.description = method.description.c_str();
        method.desc.input_schema = method.input_schema.c_str();
        method.desc.output_schema = method.output_schema.c_str();
    }
    method_descs_.reserve(methods_.size());
    for (const method_holder& method : methods_) method_descs_.push_back(method.desc);
    // v2 announcement: one plugin-wide data contract plus its method set; no interface list.
    caps_.struct_size = sizeof(abi::caps_desc);
    caps_.method_count = static_cast<std::uint32_t>(method_descs_.size());
    caps_.methods = method_descs_.empty() ? nullptr : method_descs_.data();
    caps_.protocol = behavior_.protocol;
}

abi::status U42_CALL fake_factory::describe(const abi::plug_desc** out) noexcept
{
    if (out == nullptr) return abi::invalid_argument;
    *out = nullptr;
    *out = &desc_;
    return abi::ok;
}

abi::status U42_CALL fake_factory::create(abi::iplug** out) noexcept
{
    ++create_calls;
    if (out == nullptr) return abi::invalid_argument;
    *out = nullptr;
    if (instance_ != nullptr) {
        if (!instance_->destroyed) {
            RECORD_NOTE("create() while a live instance was not destroyed");
            return abi::invalid_state;
        }
        instance_.reset();
    }
    instance_ = std::make_unique<fake_plug>(*this);
    trace("create", plug_id_);
    *out = instance_.get();
    return abi::ok;
}

fake_plug::fake_plug(fake_factory& owner) noexcept : owner_(owner) {}

const std::string& fake_plug::id() const noexcept { return owner_.id(); }

const fake_behavior& fake_plug::behavior() const noexcept { return owner_.behavior(); }

void cap_recorder::on_capability(const abi::cap_event* value) noexcept
{
    ++callback_count;
    if (value == nullptr) {
        ++null_events;
        return;
    }
    cap_notice notice;
    notice.saw_null_plug_id = value->plug_id == nullptr;
    notice.plug_id = value->plug_id != nullptr ? std::string(value->plug_id) : std::string();
    notice.available = value->available != 0;
    const abi::caps_desc& caps = value->capabilities;
    notice.counts_consistent = caps.struct_size == sizeof(abi::caps_desc);
    notice.protocol = caps.protocol;
    if (caps.method_count != 0 && caps.methods != nullptr) {
        for (std::uint32_t index = 0; index < caps.method_count; ++index) {
            const abi::method_desc& method = caps.methods[index];
            notice.methods.push_back(method.id);
            notice.method_names.push_back(method.name != nullptr ? std::string(method.name)
                                                                : std::string());
            const bool described = method.name != nullptr && method.description != nullptr &&
                                   method.description[0] != '\0' && method.input_schema != nullptr &&
                                   method.output_schema != nullptr;
            notice.methods_described = notice.methods_described && described;
        }
    }
    notice.counts_consistent = notice.counts_consistent &&
                               caps.method_count == notice.methods.size();
    notices.push_back(std::move(notice));
    if (owner != nullptr) {
        trace("cap-notice", notices.back().plug_id);
        const fake_behavior& mode = owner->behavior();
        if (notices.back().available && mode.probe_on_capability && !owner->probe_seen &&
            notices.back().plug_id == mode.peer) {
            owner->run_capability_probe(notices.back().plug_id);
        }
    }
}

const cap_notice* cap_recorder::last_for(const std::string& plug_id) const
{
    for (std::size_t index = notices.size(); index > 0; --index) {
        if (notices[index - 1].plug_id == plug_id) return &notices[index - 1];
    }
    return nullptr;
}

std::vector<bool> cap_recorder::availability_for(const std::string& plug_id) const
{
    std::vector<bool> flags;
    for (const cap_notice& notice : notices) {
        if (notice.plug_id == plug_id) flags.push_back(notice.available);
    }
    return flags;
}

void lease_revoker::on_revoke(abi::token credential) noexcept
{
    ++calls;
    const bool nested = in_callback;
    if (nested) ++reentrant_calls;
    in_callback = true;
    notified = true;
    last = credential;
    // A copied receiver would call this on another address while owner still points here.
    self_ok = self_ok && (this == &owner->revoker);
    if (owner != nullptr && !owner->behavior().ignore_revocation) {
        release_attempted = true;
        release_status = owner->caps_ != nullptr ? owner->caps_->release(credential)
                                                 : abi::invalid_state;
        owner->on_lease_returned(credential, release_status);
    }
    if (!nested) in_callback = false;
}

void fake_plug::on_lease_returned(abi::token value, abi::status status) noexcept
{
    ++lease_returns;
    last_release = status;
    if (lease_.credential.value == value.value) {
        // The returned credential ends this generation's session: no binding survives it.
        lease_ = {};
        binding_ = {};
    }
}

abi::status U42_CALL fake_plug::init(abi::ictx* ctx) noexcept
{
    ++init_calls;
    trace("init", id());
    const fake_behavior& mode = behavior();
    base_address = reinterpret_cast<std::uint64_t>(static_cast<abi::iplug*>(this));
    extra_address = reinterpret_cast<std::uint64_t>(static_cast<plug_extra*>(this));
    invoke_address = reinterpret_cast<std::uint64_t>(static_cast<abi::iinvoke*>(this));
    if (ctx == nullptr) {
        RECORD_NOTE("init() received a null host context");
        return abi::invalid_argument;
    }

    void* raw = nullptr;
    const abi::status caps_status = ctx->query(&abi::caps_iid, &raw);
    caps_ = static_cast<abi::icaps*>(raw);
    raw = nullptr;
    const abi::status calls_status = ctx->query(&abi::calls_iid, &raw);
    calls_ = static_cast<abi::icalls*>(raw);
    if (caps_status != abi::ok || caps_ == nullptr || calls_status != abi::ok || calls_ == nullptr) {
        RECORD_NOTE("the host context did not expose the caps and calls services");
    }

    if (mode.query_services_in_init) {
        void* events = nullptr;
        void* caps = nullptr;
        void* calls = nullptr;
        void* diag = nullptr;
        service_events = ctx->query(&abi::events_iid, &events);
        service_caps = ctx->query(&abi::caps_iid, &caps);
        service_calls = ctx->query(&abi::calls_iid, &calls);
        service_diag = ctx->query(&abi::diag_iid, &diag);
        services_resolved = events != nullptr && caps != nullptr && calls != nullptr &&
                            diag != nullptr;
        if (diag != nullptr) {
            static_cast<abi::idiag*>(diag)->log("fake plugin init");
            diag_logged = true;
        }
    }

    if (mode.query_unknown_in_init) {
        const abi::iid unknown{0x1111222233334444ULL, 0x5555666677778888ULL};
        void* dirty = reinterpret_cast<void*>(&init_calls);
        unknown_iface_status = ctx->query(&unknown, &dirty);
        unknown_iface_cleared = dirty == nullptr;
        void* untouched = nullptr;
        null_type_status = ctx->query(nullptr, &untouched);
        null_type_untouched = untouched == nullptr;
        null_out_status = ctx->query(&abi::caps_iid, nullptr);
    }

    if (mode.watch_in_init && caps_ != nullptr) {
        watch_status = caps_->watch(&cap_sink, &watch_token);
    }

    if (mode.probe_in_init && caps_ != nullptr && calls_ != nullptr) {
        const abi::contract required = mode.required;
        abi::borrow probe{};
        probe.credential.value = 0x2; // dirty: a failed acquire must clear it
        probe_acquire = caps_->acquire(mode.peer.c_str(), &required, &revoker, &probe);
        probe_acquire_cleared = probe.credential.value == 0;
        if (probe_acquire == abi::ok) {
            lease_ = probe;
            binding_ = {};
        }
        abi::binding by_name{};
        by_name.value = 0x3;
        probe_bind_name = calls_->bind_name(probe.credential, "echo", &by_name);
        probe_binding_value = by_name.value;
        probe_binding_cleared = by_name.value == 0;
        abi::binding by_id{};
        by_id.value = 0x3;
        probe_bind_id = calls_->bind_id(probe.credential, echo_method, &by_id);
        probe_binding_cleared = probe_binding_cleared && by_id.value == 0;
        // No live credential exists while the provider is not active, so no business call can
        // be routed at all.
        buffer_writer writer;
        if (probe_bind_name == abi::ok) {
            binding_ = by_name;
            probe_call = calls_->call(by_name, abi::bytes{nullptr, 0}, &writer);
            probe_binding_intact = by_name.value == probe_binding_value && by_name.value != 0;
        } else {
            probe_call = probe_bind_name;
        }
        probe_business_blocked = !(probe_bind_name == abi::ok && probe_call == abi::ok) &&
                                 writer.value.empty();
    }

    if (mode.init_status != abi::ok) return mode.init_status;
    return abi::ok;
}

abi::status U42_CALL fake_plug::start() noexcept
{
    ++start_calls;
    trace("start", id());
    const fake_behavior& mode = behavior();
    if (mode.start_status != abi::ok) return mode.start_status;
    if (mode.announce && caps_ != nullptr) {
        ++announce_calls;
        announce_status = caps_->announce(owner_.announced());
        RECORD_STATUS(announce_status == abi::ok, announce_status);
    }
    return abi::ok;
}

abi::status U42_CALL fake_plug::stop() noexcept
{
    ++stop_calls;
    trace("stop", id());
    if (stop_calls > 1) trace("stop-again", id());
    return behavior().stop_status;
}

void U42_CALL fake_plug::destroy() noexcept
{
    ++destroy_calls;
    trace("destroy", id());
    if (destroy_calls > 1) trace("destroy-again", id());
    destroyed = true;
    // The allocating factory owns this storage; the host must never delete across the ABI.
}

abi::status U42_CALL fake_plug::query(const abi::iid* type, void** out) noexcept
{
    if (type == nullptr || out == nullptr) return abi::invalid_argument;
    *out = nullptr;
    if (*type == abi::invoke_iid) {
        abi::iinvoke* converted = static_cast<abi::iinvoke*>(this);
        // A wrong multiple-inheritance conversion would silently hand out a wrong address.
        RECORD(reinterpret_cast<std::uint64_t>(converted) == invoke_address);
        *out = converted;
        return abi::ok;
    }
    // No business interface is exposed to a consumer: the announced protocol is data, and only
    // the host ever asks for the private invoke gateway.
    return abi::unsupported;
}

abi::status U42_CALL fake_plug::invoke(abi::method_id method, abi::bytes args,
                                       abi::iwriter* result) noexcept
{
    ++invoke_calls;
    last_invoke_method = method;
    if (result == nullptr) return abi::invalid_argument;
    if (args.data == nullptr && args.size != 0) return abi::invalid_argument;
    if (method == ping_method) {
        static constexpr char pong[] = "pong";
        return result->write(abi::bytes{pong, sizeof(pong) - 1});
    }
    if (method != echo_method) return abi::not_found;

    const fake_behavior& mode = behavior();
    if (mode.partial_output) {
        static constexpr char half[] = "half";
        ++writes_attempted;
        const abi::status first = result->write(abi::bytes{half, sizeof(half) - 1});
        if (first != abi::ok) return first;
        // Partial output already handed to the writer must be dropped by the caller.
        return abi::failed;
    }
    if (mode.limit_blow && args.size == 0) {
        static constexpr char blob[] = "abcdefgh";
        bool saw_failure = false;
        for (std::size_t index = 0; index < sizeof(blob) - 1; ++index) {
            const abi::status written = result->write(abi::bytes{blob + index, 1});
            ++writes_attempted;
            if (saw_failure) {
                observed_after_failure = written;
            } else if (written != abi::ok) {
                saw_failure = true;
                first_failure_index = static_cast<std::int64_t>(index);
            }
        }
        // Deliberately ignore the writer failure: the host must still report the limit.
        return abi::ok;
    }
    return result->write(args);
}

abi::status fake_plug::acquire_from(const std::string& provider)
{
    ++acquire_calls;
    if (caps_ == nullptr) return abi::invalid_state;
    const abi::contract required = behavior().required;
    abi::borrow result{};
    result.credential.value = 0x1; // dirty: a failed acquire must clear it
    last_acquire = caps_->acquire(provider.c_str(), &required, &revoker, &result);
    last_acquire_cleared = result.credential.value == 0;
    if (last_acquire == abi::ok) {
        lease_ = result;
        binding_ = {};
    }
    return last_acquire;
}

abi::status fake_plug::acquire_peer()
{
    return acquire_from(behavior().peer);
}

abi::status fake_plug::bind_bound_method()
{
    if (calls_ == nullptr || lease_.credential.value == 0) return abi::invalid_state;
    abi::binding result{};
    result.value = 0x1; // dirty: a failed bind must clear it
    const abi::status bound = calls_->bind_name(lease_.credential, "echo", &result);
    if (bound != abi::ok) {
        binding_ = {};
        return bound;
    }
    binding_ = result;
    return bound;
}

abi::status fake_plug::bind_named(const std::string& method, abi::binding* out)
{
    if (calls_ == nullptr) return abi::invalid_state;
    return calls_->bind_name(lease_.credential, method.c_str(), out);
}

abi::status fake_plug::call_bound_method(std::string text, std::string* out)
{
    if (calls_ == nullptr || binding_.value == 0) return abi::invalid_state;
    buffer_writer writer;
    const abi::status called =
        calls_->call(binding_, abi::bytes{text.data(), text.size()}, &writer);
    if (out != nullptr) *out = writer.value;
    return called;
}

void fake_plug::run_capability_probe(const std::string& provider)
{
    // Runs inside a noexcept capability callback: results are recorded, never thrown.
    probe_seen = true;
    probe_while_initialized = start_calls == 0;
    if (caps_ == nullptr || calls_ == nullptr) {
        RECORD_NOTE("the capability probe ran without the caps or calls service");
        return;
    }
    const abi::contract required = behavior().required;
    abi::borrow probe{};
    probe.credential.value = 0x2; // dirty
    probe_acquire = caps_->acquire(provider.c_str(), &required, &revoker, &probe);
    probe_acquire_cleared = probe.credential.value == 0;
    if (probe_acquire == abi::ok) {
        lease_ = probe;
        binding_ = {};
    }
    abi::binding by_name{};
    by_name.value = 0x3;
    probe_bind_name = calls_->bind_name(probe.credential, "echo", &by_name);
    probe_binding_value = by_name.value;
    probe_binding_cleared = by_name.value == 0;
    abi::binding by_id{};
    by_id.value = 0x3;
    probe_bind_id = calls_->bind_id(probe.credential, echo_method, &by_id);
    probe_binding_cleared = probe_binding_cleared && by_id.value == 0;
    buffer_writer writer;
    if (probe_bind_name == abi::ok) {
        // Keep the credential-bound binding: the lease stays valid once this consumer becomes
        // Active, and the surrounding test reuses it to prove that.
        binding_ = by_name;
        probe_call = calls_->call(by_name, abi::bytes{nullptr, 0}, &writer);
        // call() takes the binding by value, so a refusal must leave the caller's handle alone.
        probe_binding_intact = by_name.value == probe_binding_value && by_name.value != 0;
    } else {
        probe_call = probe_bind_name;
    }
    probe_business_blocked = !(probe_bind_name == abi::ok && probe_call == abi::ok) &&
                             writer.value.empty();
}

abi::status fake_plug::release_lease()
{
    if (caps_ == nullptr || lease_.credential.value == 0) return abi::invalid_state;
    return release_token(lease_.credential);
}

abi::status fake_plug::release_token(abi::token value)
{
    if (caps_ == nullptr) return abi::invalid_state;
    const abi::status released = caps_->release(value);
    if (released != abi::ok) {
        if (released == abi::stale) ++stale_release_calls;
        return released;
    }
    // A successful return drops the session built on that credential, but only for the
    // credential this instance actually holds.
    if (lease_.credential.value == value.value) {
        lease_ = {};
        binding_ = {};
    }
    return released;
}

/* ------------------------------------------------------------------ *
 * Fixture: one host plus the fake factories that must outlive it.
 * ------------------------------------------------------------------ */

std::uint64_t g_fixture_serial = 0;

/**
 * @brief Test rack holding a host and the factories staged into it.
 *
 * Members are declared so that @c host_ is destroyed before @c factories_, which keeps every
 * borrowed factory alive for the whole host lifetime as the public contract requires.
 */
class rack {
public:
    explicit rack(u42::host_options options = {})
        : prefix_("f" + std::to_string(++g_fixture_serial) + "."), host_(options)
    {
    }
    rack(const rack&) = delete;
    rack& operator=(const rack&) = delete;

    u42::host& host() noexcept { return host_; }

    /** @brief Stable fixture-local plugin identity. */
    std::string id(const char* name) const { return prefix_ + name; }

    /**
     * @brief Create a factory with a fixture-local identity.
     *
     * @param name Fixture-local suffix; the full identity is unique inside this rack.
     * @param behavior Per-instance behavior switches.
     * @param priority Initialization priority.
     * @param before Identities that must initialize after this plugin.
     * @param after Identities that must initialize before this plugin.
     * @return Borrowed factory owned by this rack; valid until the rack is destroyed.
     */
    fake_factory& plug(const char* name, fake_behavior behavior = {}, std::int32_t priority = 0,
                       std::vector<std::string> before = {}, std::vector<std::string> after = {})
    {
        const std::string full = id(name);
        for (const std::unique_ptr<fake_factory>& existing : factories_) {
            CHECK(existing->id() != full);
        }
        return plug_with_id(full, std::move(behavior), priority, std::move(before),
                            std::move(after));
    }

    /** @brief Create a factory with an explicit identity, for duplicate-identity cases. */
    fake_factory& plug_with_id(std::string full_id, fake_behavior behavior = {},
                               std::int32_t priority = 0, std::vector<std::string> before = {},
                               std::vector<std::string> after = {})
    {
        factories_.push_back(std::make_unique<fake_factory>(std::move(full_id), std::move(behavior),
                                                            priority, std::move(before),
                                                            std::move(after)));
        return *factories_.back();
    }

    /** @brief Stage a factory, requiring the host to accept it. */
    fake_factory& stage(fake_factory& factory)
    {
        check_status("host.add", host_.add(&factory), abi::ok);
        return factory;
    }

private:
    std::string prefix_;
    std::vector<std::unique_ptr<fake_factory>> factories_;
    u42::host host_;
};

/* ------------------------------------------------------------------ *
 * Harness self-test.
 * ------------------------------------------------------------------ */

TEST_CASE(check_is_not_assert_and_record_defers)
{
    const std::size_t mark = g_failures;

    // CHECK must abort through fail_check; intercept it so the suite survives the probe.
    std::jmp_buf jump{};
    g_reported_expression = nullptr;
    g_jump_target = &jump;
    g_quiet = true;
    g_check_hook = [](const char* expr, const char*, int) {
        g_reported_expression = expr;
        std::longjmp(*g_jump_target, 1);
    };
    if (setjmp(jump) == 0) {
        CHECK(2 + 2 == 5);
        std::fprintf(stderr, "CHECK did not abort the test body\n");
        std::exit(EXIT_FAILURE);
    }
    g_check_hook = nullptr;
    g_jump_target = nullptr;
    CHECK(g_reported_expression != nullptr);
    CHECK(std::string(g_reported_expression) == "2 + 2 == 5");

    // RECORD must count the failure and keep running; only the outer scope decides.
    RECORD(1 + 1 == 3);

    const std::size_t raised = g_failures - mark;
    g_failures = mark;
    g_quiet = false;
    CHECK(raised == 2);
}

/* ------------------------------------------------------------------ *
 * Lifecycle contracts.
 * ------------------------------------------------------------------ */

TEST_CASE(add_null_factory_rejected)
{
    const std::size_t mark = g_failures;
    rack r;
    check_status("add(null)", r.host().add(nullptr), abi::invalid_argument);
    CHECK(!r.host().error().empty());
    CHECK(r.host().plugins().empty());
    check_status("start(empty batch)", r.host().start(), abi::ok);
    check_status("poll(empty)", r.host().poll(), abi::ok);
    check_status("shutdown(empty)", r.host().shutdown(), abi::ok);
    CHECK(g_failures == mark);
}

TEST_CASE(two_phase_init_completes_before_any_start)
{
    const std::size_t mark = g_failures;
    rack r;
    fake_factory& alpha = r.plug("alpha");
    fake_factory& mid = r.plug("mid");
    fake_factory& omega = r.plug("omega");
    r.stage(alpha);
    r.stage(mid);
    r.stage(omega);

    check_status("start", r.host().start(), abi::ok);
    CHECK(r.host().error().empty());

    const std::vector<std::string> expected = {r.id("alpha"), r.id("mid"), r.id("omega")};
    check_sequence("init order", trace_ids("init"), expected);
    check_sequence("start order", trace_ids("start"), expected);
    CHECK(last_of_kind("init") < first_of_kind("start"));

    for (fake_factory* factory : {&alpha, &mid, &omega}) {
        CHECK(factory->instance() != nullptr);
        CHECK(factory->instance()->init_calls == 1);
        CHECK(factory->instance()->start_calls == 1);
        CHECK(factory->instance()->destroy_calls == 0);
    }
    CHECK(sorted(r.host().plugins()) == sorted(expected));

    check_status("shutdown", r.host().shutdown(), abi::ok);
    for (fake_factory* factory : {&alpha, &mid, &omega}) {
        CHECK(factory->instance()->stop_calls == 1);
        CHECK(factory->instance()->destroy_calls == 1);
    }
    CHECK(g_failures == mark);
}

TEST_CASE(start_order_follows_priority_and_after_plan)
{
    const std::size_t mark = g_failures;
    rack r;
    // Pure priority would order alpha, mid, zebra; the after edge forces zebra before mid.
    fake_factory& alpha = r.plug("alpha", {}, -5);
    fake_factory& zebra = r.plug("zebra", {}, 5);
    fake_factory& mid = r.plug("mid", {}, 0, {}, {r.id("zebra")});
    r.stage(alpha);
    r.stage(zebra);
    r.stage(mid);

    check_status("start", r.host().start(), abi::ok);
    const std::vector<std::string> expected = {r.id("alpha"), r.id("zebra"), r.id("mid")};
    check_sequence("init order", trace_ids("init"), expected);
    check_sequence("start order", trace_ids("start"), expected);
    CHECK(trace_position("init", r.id("zebra")) < trace_position("init", r.id("mid")));

    check_status("shutdown", r.host().shutdown(), abi::ok);
    CHECK(g_failures == mark);
}

TEST_CASE(shutdown_stops_and_destroys_in_reverse_plan_order)
{
    const std::size_t mark = g_failures;
    rack r;
    fake_factory& first = r.plug("first", {}, 0);
    fake_factory& second = r.plug("second", {}, 1);
    fake_factory& third = r.plug("third", {}, 2);
    r.stage(first);
    r.stage(second);
    r.stage(third);
    check_status("start", r.host().start(), abi::ok);

    const std::vector<std::string> forward = {r.id("first"), r.id("second"), r.id("third")};
    const std::vector<std::string> reverse = {r.id("third"), r.id("second"), r.id("first")};
    check_sequence("init order", trace_ids("init"), forward);
    check_sequence("start order", trace_ids("start"), forward);

    check_status("shutdown", r.host().shutdown(), abi::ok);
    check_sequence("stop order", trace_ids("stop"), reverse);
    check_sequence("destroy order", trace_ids("destroy"), reverse);
    for (std::size_t index = 0; index < reverse.size(); ++index) {
        CHECK(trace_position("stop", reverse[index]) < trace_position("destroy", reverse[index]));
    }
    for (fake_factory* factory : {&first, &second, &third}) {
        CHECK(trace_count("stop", factory->id()) == 1);
        CHECK(trace_count("destroy", factory->id()) == 1);
    }
    CHECK(r.host().plugins().empty());
    check_status("poll after shutdown", r.host().poll(), abi::ok);
    CHECK(g_failures == mark);
}

TEST_CASE(init_failure_rolls_back_without_any_start)
{
    const std::size_t mark = g_failures;
    rack r;
    fake_behavior failing;
    failing.init_status = abi::failed;
    fake_factory& good = r.plug("good", {}, 0);
    fake_factory& bad = r.plug("bad", failing, 1);
    fake_factory& never = r.plug("never", {}, 2);
    r.stage(good);
    r.stage(bad);
    r.stage(never);

    check_not_ok("start", r.host().start());
    CHECK(!r.host().error().empty());
    CHECK(trace_ids("start").empty());
    CHECK(trace_count("start", r.id("bad")) == 0);

    // Every instance that entered init successfully must be stopped exactly once, in reverse;
    // the rolled-back instance is destroyed directly instead of being stopped.
    std::vector<std::string> entered = trace_ids("init");
    CHECK(entered.size() >= 2);
    CHECK(entered[0] == r.id("good"));
    CHECK(entered[1] == r.id("bad"));
    std::vector<std::string> expected_stops;
    for (std::size_t index = entered.size(); index > 0; --index) {
        if (entered[index - 1] != r.id("bad")) expected_stops.push_back(entered[index - 1]);
    }
    check_sequence("stop order", trace_ids("stop"), expected_stops);
    CHECK(trace_count("stop", r.id("bad")) == 0);

    CHECK(sorted(trace_ids("destroy")) ==
          sorted(std::vector<std::string>{r.id("good"), r.id("bad"), r.id("never")}));
    CHECK(good.instance()->start_calls == 0);
    CHECK(good.instance()->stop_calls == 1);
    CHECK(bad.instance()->init_calls == 1);
    CHECK(bad.instance()->start_calls == 0);
    CHECK(bad.instance()->destroy_calls == 1);
    CHECK(never.instance()->destroy_calls == 1);

    // The failed batch must not leave a half-initialized instance reachable: no lease and no
    // call can be issued against the rolled-back identity.
    admin_lease blocked_lease(r.host(), r.id("good"), test_contract);
    check_not_ok("acquire after rollback", blocked_lease.status());
    CHECK(!blocked_lease.holds());
    std::string output = "stale";
    check_not_ok("call after rollback",
                 r.host().call(r.id("good"), test_contract, "echo", abi::bytes{nullptr, 0},
                               &output));
    CHECK(output.empty());
    CHECK(g_failures == mark);
}

TEST_CASE(start_failure_revokes_capabilities_and_stops_everything)
{
    const std::size_t mark = g_failures;
    rack r;
    fake_behavior failing;
    failing.start_status = abi::failed;
    fake_behavior watching;
    watching.watch_in_init = true;
    fake_factory& provider = r.plug("provider", {}, 0);
    fake_factory& bad = r.plug("bad", failing, 1);
    fake_factory& consumer = r.plug("consumer", watching, 2);
    r.stage(provider);
    r.stage(bad);
    r.stage(consumer);

    check_not_ok("start", r.host().start());
    CHECK(!r.host().error().empty());
    CHECK(trace_count("start", r.id("bad")) == 1);
    CHECK(trace_count("start", r.id("consumer")) == 0);

    const std::vector<std::string> reverse = {r.id("consumer"), r.id("bad"), r.id("provider")};
    check_sequence("stop order", trace_ids("stop"), reverse);
    CHECK(trace_count("destroy", r.id("provider")) == 1);

    // The withdrawn capability must not be leasable, bindable or callable.
    admin_lease blocked_lease(r.host(), r.id("provider"), test_contract);
    check_not_ok("acquire after start failure", blocked_lease.status());
    CHECK(!blocked_lease.holds());
    abi::binding handle{};
    handle.value = 0x7;
    check_not_ok("bind after start failure", r.host().bind(blocked_lease.credential(), "echo",
                                                           &handle));
    CHECK(handle.value == 0);
    std::string output = "stale";
    check_not_ok("call after start failure",
                 r.host().call(r.id("provider"), test_contract, "echo", abi::bytes{nullptr, 0},
                               &output));
    CHECK(output.empty());

    // Note: the host withdraws the announced capability (proved by the failed lease/bind/call
    // above), but it is not required to publish a withdrawal notification to watchers when the
    // whole batch is aborted; the design only mandates the notification duty for a single unload.
    check_status("poll", r.host().poll(), abi::ok);
    CHECK(g_failures == mark);
}

TEST_CASE(duplicate_plug_identity_is_rejected)
{
    const std::size_t mark = g_failures;
    rack r;
    fake_factory& first = r.plug("dup", {}, 0);
    fake_factory& second = r.plug_with_id(r.id("dup"), fake_behavior{}, 1);
    check_status("add(first)", r.host().add(&first), abi::ok);
    const abi::status added_second = r.host().add(&second);

    const abi::status started = r.host().start();
    if (added_second != abi::ok) {
        // Refused while staging: the survivor may still run on its own.
        check_status("start(single survivor)", started, abi::ok);
        if (first.instance() != nullptr) CHECK(first.instance()->destroy_calls == 0);
    } else {
        // Accepted while staging: the batch itself must be refused before initialization.
        check_not_ok("start(duplicate batch)", started);
        CHECK(!r.host().error().empty());
    }
    // Whatever path refused it, the duplicate identity must never run twice.
    if (first.instance() != nullptr) CHECK(first.instance()->start_calls == 0 || started == abi::ok);
    if (second.instance() != nullptr) CHECK(second.instance()->start_calls == 0);
    CHECK(trace_ids("init").size() <= 1);
    CHECK(trace_ids("start").size() <= 1);
    CHECK(r.host().plugins().size() <= 1);
    CHECK(g_failures == mark);
}

/* ------------------------------------------------------------------ *
 * Context and interface queries.
 * ------------------------------------------------------------------ */

TEST_CASE(host_services_resolve_and_unknown_query_clears)
{
    const std::size_t mark = g_failures;
    rack r;
    fake_behavior behavior;
    behavior.query_services_in_init = true;
    behavior.query_unknown_in_init = true;
    fake_factory& factory = r.plug("probe", behavior);
    r.stage(factory);
    check_status("start", r.host().start(), abi::ok);

    fake_plug* instance = factory.instance();
    CHECK(instance != nullptr);
    CHECK(instance->services_resolved);
    check_status("events service", instance->service_events, abi::ok);
    check_status("caps service", instance->service_caps, abi::ok);
    check_status("calls service", instance->service_calls, abi::ok);
    check_status("diag service", instance->service_diag, abi::ok);
    CHECK(instance->diag_logged);
    check_status("unknown iid", instance->unknown_iface_status, abi::unsupported);
    CHECK(instance->unknown_iface_cleared);
    check_status("null type", instance->null_type_status, abi::invalid_argument);
    CHECK(instance->null_type_untouched);
    check_status("null output", instance->null_out_status, abi::invalid_argument);

    check_status("shutdown", r.host().shutdown(), abi::ok);
    CHECK(g_failures == mark);
}

TEST_CASE(plugin_query_exposes_only_the_host_private_invoke_subobject)
{
    const std::size_t mark = g_failures;
    rack r;
    fake_factory& factory = r.plug("multi");
    r.stage(factory);
    check_status("start", r.host().start(), abi::ok);
    fake_plug* instance = factory.instance();
    CHECK(instance != nullptr);
    // Distinct base addresses make the checks below meaningful rather than tautological.
    CHECK(instance->base_address != instance->invoke_address);
    CHECK(instance->base_address != instance->extra_address);

    const abi::iid unknown{0x1111222233334444ULL, 0x5555666677778888ULL};
    void* slot = reinterpret_cast<void*>(0x1);
    check_status("query(unknown)", instance->query(&unknown, &slot), abi::unsupported);
    CHECK(slot == nullptr);

    // A consumer never receives a business interface: the announced protocol identity is not a
    // queryable interface either, so no business pointer can be obtained this way.
    slot = reinterpret_cast<void*>(0x1);
    const abi::iid business_iid = test_protocol;
    check_status("query(business protocol)", instance->query(&business_iid, &slot),
                 abi::unsupported);
    CHECK(slot == nullptr);
    slot = reinterpret_cast<void*>(0x1);
    check_status("query(invoke)", instance->query(&abi::invoke_iid, &slot), abi::ok);
    CHECK(reinterpret_cast<std::uint64_t>(slot) == instance->invoke_address);

    slot = nullptr;
    check_status("query(null type)", instance->query(nullptr, &slot), abi::invalid_argument);
    CHECK(slot == nullptr);
    check_status("query(null output)", instance->query(&abi::invoke_iid, nullptr),
                 abi::invalid_argument);

    // The host reaches the method through that gateway and through nothing else.
    std::string out = "stale";
    check_status("call(host gateway)",
                 r.host().call(r.id("multi"), test_contract, "echo", abi::bytes{"x", 1}, &out),
                 abi::ok);
    CHECK(out == "x");
    CHECK(instance->invoke_calls == 1);
    CHECK(instance->last_invoke_method == echo_method);

    check_status("shutdown", r.host().shutdown(), abi::ok);
    CHECK(g_failures == mark);
}

/* ------------------------------------------------------------------ *
 * Capability disclosure and borrowing.
 * ------------------------------------------------------------------ */

TEST_CASE(capabilities_are_unavailable_until_the_provider_started)
{
    const std::size_t mark = g_failures;
    rack r;
    fake_behavior consumer_behavior;
    consumer_behavior.watch_in_init = true;
    consumer_behavior.probe_in_init = true;
    consumer_behavior.peer = r.id("provider");
    fake_factory& provider = r.plug("provider", {}, 0);
    fake_factory& consumer = r.plug("consumer", consumer_behavior, 1);
    r.stage(provider);
    r.stage(consumer);
    check_status("start", r.host().start(), abi::ok);

    fake_plug* instance = consumer.instance();
    CHECK(instance != nullptr);
    CHECK(instance->watch_status == abi::ok);
    CHECK(instance->watch_token.value != 0);
    // The provider was not Active during the consumer's init, so no lease could be granted.
    check_not_ok("acquire before provider start", instance->probe_acquire);
    CHECK(instance->probe_acquire_cleared);
    // Without a live credential every business entry point is refused and no output is produced.
    check_status("bind name with no lease", instance->probe_bind_name, abi::invalid_argument);
    check_status("bind id with no lease", instance->probe_bind_id, abi::invalid_argument);
    CHECK(instance->probe_binding_cleared);
    CHECK(instance->probe_business_blocked);
    CHECK(!instance->holds_lease());
    CHECK(!instance->has_binding());

    // A watch registered during init still receives the current snapshot afterwards.
    check_status("poll", r.host().poll(), abi::ok);
    const cap_notice* notice = instance->cap_sink.last_for(r.id("provider"));
    CHECK(notice != nullptr);
    if (notice != nullptr) {
        CHECK(notice->available);
        CHECK(notice->protocol.id == test_contract.id);
    }
    CHECK(instance->cap_sink.callback_count >= 1);

    check_status("shutdown", r.host().shutdown(), abi::ok);
    CHECK(g_failures == mark);
}

TEST_CASE(initialized_consumer_leases_but_cannot_call)
{
    const std::size_t mark = g_failures;
    rack r;
    fake_behavior consumer_behavior;
    consumer_behavior.watch_in_init = true;
    consumer_behavior.probe_on_capability = true;
    consumer_behavior.peer = r.id("provider");
    fake_factory& provider = r.plug("provider", {}, 0);
    fake_factory& consumer = r.plug("consumer", consumer_behavior, 1);
    r.stage(provider);
    r.stage(consumer);
    check_status("start", r.host().start(), abi::ok);

    fake_plug* instance = consumer.instance();
    CHECK(instance != nullptr);
    CHECK(instance->probe_seen);
    // The announcement is delivered after the provider started and before the consumer does.
    CHECK(trace_position("start", r.id("provider")) < trace_position("start", r.id("consumer")));
    CHECK(instance->probe_while_initialized);
    // An Initialized consumer may hold a lease credential and bind a published method ...
    check_status("acquire while initialized", instance->probe_acquire, abi::ok);
    CHECK(!instance->probe_acquire_cleared);
    CHECK(instance->holds_lease());
    CHECK(instance->lease_.credential.value != 0);
    check_status("bind name while initialized", instance->probe_bind_name, abi::ok);
    check_status("bind id while initialized", instance->probe_bind_id, abi::ok);
    CHECK(instance->probe_binding_value != 0);
    // ... but a business call must be refused while it is still Initialized: leasing and
    // binding are allowed for an Initialized consumer, calling is not.
    check_status("call while initialized", instance->probe_call, abi::invalid_state);
    // The refused call takes the binding by value and must not clear the caller's handle.
    CHECK(instance->probe_binding_intact);
    CHECK(instance->probe_business_blocked);
    CHECK(provider.instance()->invoke_calls == 0);

    // Once the consumer is Active the same credential-bound binding works, and the call really
    // reaches the provider gateway.
    std::string text;
    check_status("bound call", instance->call_bound_method("typed", &text), abi::ok);
    CHECK(text == "typed");
    CHECK(provider.instance()->invoke_calls == 1);
    CHECK(provider.instance()->last_invoke_method == echo_method);

    check_status("shutdown", r.host().shutdown(), abi::ok);
    if (instance->revoker.notified) {
        CHECK(instance->revoker.self_ok);
        check_status("release during shutdown", instance->revoker.release_status, abi::ok);
    }
    CHECK(g_failures == mark);
}

TEST_CASE(consumer_watch_receives_announced_capabilities)
{
    const std::size_t mark = g_failures;
    rack r;
    fake_factory& provider = r.plug("provider");
    r.stage(provider);
    check_status("start(provider)", r.host().start(), abi::ok);
    CHECK(provider.instance()->announce_calls == 1);
    CHECK(provider.instance()->announce_status == abi::ok);

    fake_behavior consumer_behavior;
    consumer_behavior.watch_in_init = true;
    fake_factory& consumer = r.plug("consumer", consumer_behavior);
    r.stage(consumer);
    check_status("start(consumer)", r.host().start(), abi::ok);
    check_status("poll", r.host().poll(), abi::ok);

    fake_plug* instance = consumer.instance();
    CHECK(instance != nullptr);
    const cap_notice* notice = instance->cap_sink.last_for(r.id("provider"));
    CHECK(notice != nullptr);
    if (notice != nullptr) {
        CHECK(notice->available);
        CHECK(!notice->saw_null_plug_id);
        CHECK(notice->counts_consistent);
        CHECK(notice->methods_described);
        CHECK(notice->protocol.id == test_contract.id);
        CHECK(notice->protocol.major == test_contract.major);
        CHECK(notice->protocol.minor == test_contract.minor);
        CHECK((notice->methods == std::vector<abi::method_id>{echo_method, ping_method}));
        CHECK((notice->method_names == std::vector<std::string>{"echo", "ping"}));
    }
    CHECK(instance->cap_sink.null_events == 0);

    // Explicit discovery names the protocol only; it never hands out a business interface and
    // is not a lifetime lock.
    abi::contract discovered{};
    check_status("protocol(provider)", r.host().protocol(r.id("provider"), &discovered),
                 abi::ok);
    CHECK(discovered.id == test_contract.id);
    CHECK(discovered.major == test_contract.major);
    CHECK(discovered.minor >= test_contract.minor);

    check_status("shutdown", r.host().shutdown(), abi::ok);
    CHECK(g_failures == mark);
}

/* ------------------------------------------------------------------ *
 * Dynamic invocation.
 * ------------------------------------------------------------------ */

TEST_CASE(name_and_id_invocation_reach_the_same_method)
{
    const std::size_t mark = g_failures;
    rack r;
    fake_factory& factory = r.plug("provider");
    r.stage(factory);
    check_status("start", r.host().start(), abi::ok);
    fake_plug* instance = factory.instance();

    // The administration context must hold a lease; there is no credential-free bind shortcut.
    admin_lease lease(r.host(), r.id("provider"), test_contract);
    check_status("acquire", lease.status(), abi::ok);
    CHECK(lease.holds());

    abi::binding by_name{};
    abi::binding by_id{};
    check_status("bind(name)", r.host().bind(lease.credential(), "echo", &by_name), abi::ok);
    CHECK(by_name.value != 0);
    check_status("bind(id)", r.host().bind(lease.credential(), echo_method, &by_id), abi::ok);
    CHECK(by_id.value != 0);

    const char* text = "hello";
    std::string from_name = "stale";
    std::string from_id = "stale";
    check_status("call(bound name)", r.host().call(by_name, abi::bytes{text, 5}, &from_name),
                 abi::ok);
    CHECK(from_name == "hello");
    check_status("call(bound id)", r.host().call(by_id, abi::bytes{text, 5}, &from_id), abi::ok);
    CHECK(from_id == from_name);
    CHECK(instance->last_invoke_method == echo_method);
    CHECK(instance->invoke_calls == 2);

    std::string convenience_name = "stale";
    std::string convenience_id = "stale";
    check_status("call(name)",
                 r.host().call(r.id("provider"), test_contract, "echo", abi::bytes{text, 5},
                               &convenience_name),
                 abi::ok);
    check_status("call(id)",
                 r.host().call(r.id("provider"), test_contract, echo_method, abi::bytes{text, 5},
                               &convenience_id),
                 abi::ok);
    CHECK(convenience_name == convenience_id);
    CHECK(convenience_name == "hello");
    CHECK(instance->invoke_calls == 4);

    std::string pong_name = "stale";
    std::string pong_id = "stale";
    check_status("call(name ping)",
                 r.host().call(r.id("provider"), test_contract, "ping", abi::bytes{nullptr, 0},
                               &pong_name),
                 abi::ok);
    check_status("call(id ping)",
                 r.host().call(r.id("provider"), test_contract, ping_method,
                               abi::bytes{nullptr, 0}, &pong_id),
                 abi::ok);
    CHECK(pong_name == "pong");
    CHECK(pong_id == "pong");
    CHECK(instance->last_invoke_method == ping_method);

    // Each binding owns its own record: releasing one must not disturb the other.
    check_status("unbind(name)", r.host().unbind(by_name), abi::ok);
    std::string still_bound = "stale";
    check_status("call(id) after unbind(name)", r.host().call(by_id, abi::bytes{text, 5},
                                                              &still_bound),
                 abi::ok);
    CHECK(still_bound == "hello");
    check_status("unbind(id)", r.host().unbind(by_id), abi::ok);
    std::string after_unbind = "stale";
    check_status("call after unbind", r.host().call(by_id, abi::bytes{text, 5}, &after_unbind),
                 abi::stale);
    CHECK(after_unbind.empty());
    CHECK(instance->invoke_calls == 7);

    // Unbinding never returns the lease, but releasing the lease invalidates its bindings.
    abi::binding released_binding{};
    check_status("bind before release", r.host().bind(lease.credential(), "echo", &released_binding),
                 abi::ok);
    check_status("release", lease.release(), abi::ok);
    CHECK(!lease.holds());
    std::string after_release = "stale";
    check_status("call after release", r.host().call(released_binding, abi::bytes{text, 5},
                                                     &after_release),
                 abi::stale);
    CHECK(after_release.empty());
    check_status("unbind after release", r.host().unbind(released_binding), abi::stale);

    check_status("shutdown", r.host().shutdown(), abi::ok);
    CHECK(g_failures == mark);
}

TEST_CASE(unknown_method_and_invalid_arguments_are_definite_errors)
{
    const std::size_t mark = g_failures;
    rack r;
    fake_factory& factory = r.plug("provider");
    r.stage(factory);
    check_status("start", r.host().start(), abi::ok);

    admin_lease lease(r.host(), r.id("provider"), test_contract);
    check_status("acquire", lease.status(), abi::ok);

    abi::binding handle{};
    handle.value = 0x5A;
    check_status("bind(unknown name)", r.host().bind(lease.credential(), "nosuch", &handle),
                 abi::not_found);
    CHECK(handle.value == 0);
    handle.value = 0x5A;
    check_status("bind(unknown id)",
                 r.host().bind(lease.credential(), static_cast<abi::method_id>(4242), &handle),
                 abi::not_found);
    CHECK(handle.value == 0);

    // A missing provider is discovered by acquire, so no credential can ever name it.
    admin_lease missing(r.host(), r.id("nosuch"), test_contract);
    check_status("acquire(unknown plugin)", missing.status(), abi::not_found);
    CHECK(!missing.holds());

    std::string output = "stale";
    check_status("call(unknown method)",
                 r.host().call(r.id("provider"), test_contract, "nosuch", abi::bytes{nullptr, 0},
                               &output),
                 abi::not_found);
    CHECK(output.empty());
    output = "stale";
    check_status("call(unknown plugin)",
                 r.host().call(r.id("nosuch"), test_contract, "echo", abi::bytes{nullptr, 0},
                               &output),
                 abi::not_found);
    CHECK(output.empty());

    // Required pointers: a null argument is a parameter error, never a partial result.
    check_status("bind(null output)", r.host().bind(lease.credential(), "echo", nullptr),
                 abi::invalid_argument);
    check_status("call(null output)",
                 r.host().call(r.id("provider"), test_contract, "echo", abi::bytes{nullptr, 0},
                               nullptr),
                 abi::invalid_argument);
    output = "stale";
    check_status("call(zero binding)", r.host().call(abi::binding{}, abi::bytes{nullptr, 0}, &output),
                 abi::invalid_argument);
    CHECK(output.empty());
    check_status("unbind(zero binding)", r.host().unbind(abi::binding{}), abi::invalid_argument);
    handle.value = 0x5A;
    check_status("bind(zero credential)", r.host().bind(abi::token{}, "echo", &handle),
                 abi::invalid_argument);
    CHECK(handle.value == 0);

    output = "stale";
    check_not_ok("call(fabricated binding)",
                 r.host().call(abi::binding{0xDEADBEEF}, abi::bytes{nullptr, 0}, &output));
    CHECK(output.empty());
    check_not_ok("unbind(fabricated binding)", r.host().unbind(abi::binding{0xDEADBEEF}));

    // Borrowed input bytes: null data is only legal for a zero length.
    output = "stale";
    check_status("call(null bytes with size)",
                 r.host().call(r.id("provider"), test_contract, "echo", abi::bytes{nullptr, 5},
                               &output),
                 abi::invalid_argument);
    CHECK(output.empty());

    // A successful bind replaces a dirty output; empty input is a legal call.
    abi::binding good{};
    good.value = 0x7;
    check_status("bind succeeds", r.host().bind(lease.credential(), "echo", &good), abi::ok);
    CHECK(good.value != 0);
    output = "stale";
    check_status("call(empty input)", r.host().call(good, abi::bytes{nullptr, 0}, &output),
                 abi::ok);
    CHECK(output.empty());
    check_status("unbind(good)", r.host().unbind(good), abi::ok);

    check_status("shutdown", r.host().shutdown(), abi::ok);
    CHECK(g_failures == mark);
}

TEST_CASE(partial_output_of_a_failed_call_is_discarded)
{
    const std::size_t mark = g_failures;
    rack r;
    fake_behavior behavior;
    behavior.partial_output = true;
    fake_factory& factory = r.plug("provider", behavior);
    r.stage(factory);
    check_status("start", r.host().start(), abi::ok);

    std::string output = "stale";
    check_status("call(partial)",
                 r.host().call(r.id("provider"), test_contract, "echo", abi::bytes{"payload", 7},
                               &output),
                 abi::failed);
    CHECK(output.empty());
    // The plugin really did hand partial bytes to the writer before failing.
    CHECK(factory.instance()->writes_attempted == 1);

    admin_lease lease(r.host(), r.id("provider"), test_contract);
    check_status("acquire", lease.status(), abi::ok);
    abi::binding handle{};
    check_status("bind", r.host().bind(lease.credential(), "echo", &handle), abi::ok);
    std::string via_binding = "stale";
    check_status("call(partial via binding)",
                 r.host().call(handle, abi::bytes{"payload", 7}, &via_binding), abi::failed);
    CHECK(via_binding.empty());
    check_status("unbind", r.host().unbind(handle), abi::ok);

    check_status("shutdown", r.host().shutdown(), abi::ok);
    CHECK(g_failures == mark);
}

TEST_CASE(output_limit_is_reported_and_sticks)
{
    const std::size_t mark = g_failures;
    u42::host_options options;
    options.output_limit = 4;
    rack r(options);
    fake_behavior behavior;
    behavior.limit_blow = true;
    fake_factory& factory = r.plug("provider", behavior);
    r.stage(factory);
    check_status("start", r.host().start(), abi::ok);

    std::string output = "stale";
    check_status("call(over limit)",
                 r.host().call(r.id("provider"), test_contract, "echo", abi::bytes{nullptr, 0},
                               &output),
                 abi::limit_exceeded);
    CHECK(output.empty());
    fake_plug* instance = factory.instance();
    // The plugin ignored the writer failure and kept writing; the limit must stay sticky.
    CHECK(instance->writes_attempted == 8);
    CHECK(instance->first_failure_index == 4);
    check_status("post-failure write", instance->observed_after_failure, abi::limit_exceeded);

    // Exactly at the limit succeeds, one byte over is refused atomically.
    std::string at_limit = "stale";
    check_status("call(at limit)",
                 r.host().call(r.id("provider"), test_contract, "echo", abi::bytes{"abcd", 4},
                               &at_limit),
                 abi::ok);
    CHECK(at_limit == "abcd");
    std::string over_limit = "stale";
    check_status("call(one over limit)",
                 r.host().call(r.id("provider"), test_contract, "echo", abi::bytes{"abcde", 5},
                               &over_limit),
                 abi::limit_exceeded);
    CHECK(over_limit.empty());

    check_status("shutdown", r.host().shutdown(), abi::ok);
    CHECK(g_failures == mark);
}

/* ------------------------------------------------------------------ *
 * Lease revocation, reload and unload ordering.
 * ------------------------------------------------------------------ */

TEST_CASE(unload_revokes_the_lease_and_waits_for_the_return)
{
    const std::size_t mark = g_failures;
    rack r;
    fake_factory& provider = r.plug("provider");
    r.stage(provider);
    check_status("start(provider)", r.host().start(), abi::ok);

    fake_behavior consumer_behavior;
    consumer_behavior.peer = r.id("provider");
    fake_factory& consumer = r.plug("consumer", consumer_behavior);
    r.stage(consumer);
    check_status("start(consumer)", r.host().start(), abi::ok);

    fake_plug* instance = consumer.instance();
    check_status("acquire", instance->acquire_peer(), abi::ok);
    CHECK(instance->holds_lease());
    const abi::token first = instance->lease_.credential;
    // The session is built from the credential only; the binding is bound under that credential.
    check_status("bind", instance->bind_bound_method(), abi::ok);
    CHECK(instance->has_binding());
    const abi::binding session_binding = instance->binding_;
    std::string before;
    check_status("call before unload", instance->call_bound_method("before", &before), abi::ok);
    CHECK(before == "before");
    CHECK(provider.instance()->invoke_calls == 1);

    check_status("unload(provider)", r.host().unload(r.id("provider")), abi::ok);
    CHECK(instance->revoker.notified);
    CHECK(instance->revoker.calls == 1);
    // The host stores the receiver pointer; a copied receiver would report a mismatch.
    CHECK(instance->revoker.self_ok);
    CHECK(instance->revoker.reentrant_calls == 0);
    check_status("release inside on_revoke", instance->revoker.release_status, abi::ok);
    CHECK(instance->lease_returns == 1);
    CHECK(!instance->holds_lease());
    CHECK(!instance->has_binding());
    CHECK(trace_count("stop", r.id("provider")) == 1);
    CHECK(trace_count("destroy", r.id("provider")) == 1);
    CHECK(sorted(r.host().plugins()) == std::vector<std::string>{r.id("consumer")});

    // The returned credential and the binding it authorized must not match anything afterwards.
    check_status("release(stale credential)", instance->release_token(first), abi::stale);
    CHECK(instance->stale_release_calls == 1);
    buffer_writer stale_writer;
    check_status("call(stale binding)",
                 instance->calls_->call(session_binding, abi::bytes{nullptr, 0}, &stale_writer),
                 abi::stale);
    CHECK(stale_writer.value.empty());

    check_status("shutdown", r.host().shutdown(), abi::ok);
    CHECK(g_failures == mark);
}

TEST_CASE(unreturned_lease_isolates_the_provider)
{
    const std::size_t mark = g_failures;
    rack r;
    fake_factory& provider = r.plug("provider");
    r.stage(provider);
    check_status("start(provider)", r.host().start(), abi::ok);

    fake_behavior consumer_behavior;
    consumer_behavior.peer = r.id("provider");
    consumer_behavior.ignore_revocation = true;
    fake_factory& consumer = r.plug("consumer", consumer_behavior);
    r.stage(consumer);
    check_status("start(consumer)", r.host().start(), abi::ok);

    fake_plug* instance = consumer.instance();
    check_status("acquire", instance->acquire_peer(), abi::ok);
    check_status("bind", instance->bind_bound_method(), abi::ok);
    const abi::binding held_binding = instance->binding_;
    const abi::token held_credential = instance->lease_.credential;

    check_not_ok("unload with an unreturned lease", r.host().unload(r.id("provider")));
    CHECK(!r.host().error().empty());
    CHECK(trace_count("stop", r.id("provider")) == 0);
    CHECK(trace_count("destroy", r.id("provider")) == 0);
    check_not_ok("repeated unload", r.host().unload(r.id("provider")));
    CHECK(instance->holds_lease());
    CHECK(r.host().plugins().size() == 2);

    // Capabilities were withdrawn first: the isolated provider can no longer be leased.
    admin_lease blocked_lease(r.host(), r.id("provider"), test_contract);
    check_not_ok("acquire(quarantined provider)", blocked_lease.status());
    CHECK(!blocked_lease.holds());
    // A still-live lease can neither bind a new method nor call a withdrawn provider.
    abi::binding handle{};
    handle.value = 0x1;
    check_one_of("bind(quarantined provider)", instance->bind_named("echo", &handle),
                 {abi::not_found, abi::invalid_state});
    CHECK(handle.value == 0);
    buffer_writer withdrawn;
    check_one_of("call(quarantined provider)",
                 instance->calls_->call(held_binding, abi::bytes{nullptr, 0}, &withdrawn),
                 {abi::not_found, abi::invalid_state});
    CHECK(withdrawn.value.empty());

    // Returning the credential unblocks the unload.
    check_status("release", instance->release_lease(), abi::ok);
    CHECK(!instance->holds_lease());
    check_status("unload after return", r.host().unload(r.id("provider")), abi::ok);
    CHECK(trace_count("stop", r.id("provider")) == 1);
    CHECK(trace_count("destroy", r.id("provider")) == 1);
    CHECK(sorted(r.host().plugins()) == std::vector<std::string>{r.id("consumer")});
    CHECK(held_credential.value != 0);

    check_status("shutdown", r.host().shutdown(), abi::ok);
    CHECK(g_failures == mark);
}

TEST_CASE(reload_stales_old_handles_and_serves_a_new_generation)
{
    const std::size_t mark = g_failures;
    rack r;
    fake_factory& provider = r.plug("provider");
    r.stage(provider);
    check_status("start(provider)", r.host().start(), abi::ok);

    fake_behavior consumer_behavior;
    consumer_behavior.peer = r.id("provider");
    consumer_behavior.watch_in_init = true;
    fake_factory& consumer = r.plug("consumer", consumer_behavior);
    r.stage(consumer);
    check_status("start(consumer)", r.host().start(), abi::ok);

    fake_plug* instance = consumer.instance();
    check_status("acquire(first generation)", instance->acquire_peer(), abi::ok);
    const abi::token first_token = instance->lease_.credential;
    check_status("bind(first generation)", instance->bind_bound_method(), abi::ok);
    const abi::binding first_binding = instance->binding_;

    admin_lease admin(r.host(), r.id("provider"), test_contract);
    check_status("admin acquire(first generation)", admin.status(), abi::ok);
    abi::binding old_binding{};
    check_status("admin bind(first generation)", r.host().bind(admin.credential(), "echo",
                                                               &old_binding),
                 abi::ok);
    std::string output = "stale";
    check_status("admin call(first generation)",
                 r.host().call(old_binding, abi::bytes{"old", 3}, &output), abi::ok);
    CHECK(output == "old");

    check_status("unload(provider)", r.host().unload(r.id("provider")), abi::ok);
    // Both leases went through the revocation path; the administration receiver stayed alive
    // and returned its credential synchronously.
    CHECK(admin.revocations() == 1);
    CHECK(!admin.holds());
    CHECK(!instance->holds_lease());
    CHECK(!instance->has_binding());
    output = "stale";
    check_status("call(stale admin binding)",
                 r.host().call(old_binding, abi::bytes{"old", 3}, &output), abi::stale);
    CHECK(output.empty());
    buffer_writer stale_session;
    check_status("call(stale session binding)",
                 instance->calls_->call(first_binding, abi::bytes{nullptr, 0}, &stale_session),
                 abi::stale);
    CHECK(stale_session.value.empty());

    // Reload the same plugin type: a new instance and a new internal generation.
    check_status("add(reload)", r.host().add(&provider), abi::ok);
    check_status("start(reload)", r.host().start(), abi::ok);
    fake_plug* reloaded = provider.instance();
    CHECK(reloaded != nullptr);
    CHECK(reloaded != instance);
    CHECK(reloaded->destroy_calls == 0);

    // The same protocol still needs a fresh lease and a fresh binding; the old session must not
    // be reused implicitly.
    check_status("acquire(new generation)", instance->acquire_peer(), abi::ok);
    CHECK(instance->holds_lease());
    const abi::token second_token = instance->lease_.credential;
    CHECK(second_token.value != first_token.value);
    // The stale return must not match the new record, and must not break it.
    check_status("release(old credential)", instance->release_token(first_token), abi::stale);
    CHECK(instance->stale_release_calls == 1);
    CHECK(instance->holds_lease());
    check_status("bind(new generation)", instance->bind_bound_method(), abi::ok);
    std::string typed;
    check_status("call(new generation)", instance->call_bound_method("typed", &typed), abi::ok);
    CHECK(typed == "typed");
    CHECK(reloaded->invoke_calls == 1);
    CHECK(reloaded->last_invoke_method == echo_method);
    check_status("release(new credential)", instance->release_token(second_token), abi::ok);
    CHECK(!instance->holds_lease());

    // Notifications stay ordered: the withdrawal of the old instance precedes the
    // announcement of the new one, and the reloaded generation ends up available.
    check_status("poll", r.host().poll(), abi::ok);
    const std::vector<bool> availability = instance->cap_sink.availability_for(r.id("provider"));
    CHECK(!availability.empty());
    CHECK(availability.back());
    CHECK(std::find(availability.begin(), availability.end(), false) != availability.end());
    CHECK(g_failures == mark);
}

TEST_CASE(consumer_unload_returns_its_outbound_leases)
{
    const std::size_t mark = g_failures;
    rack r;
    fake_factory& provider = r.plug("provider");
    r.stage(provider);
    check_status("start(provider)", r.host().start(), abi::ok);

    fake_behavior consumer_behavior;
    consumer_behavior.peer = r.id("provider");
    fake_factory& consumer = r.plug("consumer", consumer_behavior);
    r.stage(consumer);
    check_status("start(consumer)", r.host().start(), abi::ok);

    fake_plug* instance = consumer.instance();
    check_status("acquire", instance->acquire_peer(), abi::ok);
    CHECK(instance->holds_lease());
    check_status("bind", instance->bind_bound_method(), abi::ok);
    const abi::binding outbound = instance->binding_;
    CHECK(outbound.value != 0);

    check_status("unload(consumer)", r.host().unload(r.id("consumer")), abi::ok);
    CHECK(trace_count("stop", r.id("consumer")) == 1);
    CHECK(trace_count("destroy", r.id("consumer")) == 1);
    if (instance->revoker.notified) {
        CHECK(instance->revoker.self_ok);
        check_status("release during consumer unload", instance->revoker.release_status, abi::ok);
        CHECK(!instance->holds_lease());
    }

    // The provider is untouched and, crucially, still unloadable: no lease is attributed
    // to the destroyed consumer any more. A fresh administration lease proves it is intact.
    admin_lease survivor(r.host(), r.id("provider"), test_contract);
    check_status("acquire(surviving provider)", survivor.status(), abi::ok);
    abi::binding handle{};
    check_status("bind(surviving provider)", r.host().bind(survivor.credential(), "echo", &handle),
                 abi::ok);
    std::string text;
    check_status("call(surviving provider)", r.host().call(handle, abi::bytes{"alive", 5}, &text),
                 abi::ok);
    CHECK(text == "alive");
    check_status("unbind", r.host().unbind(handle), abi::ok);
    check_status("release", survivor.release(), abi::ok);
    check_status("unload(provider)", r.host().unload(r.id("provider")), abi::ok);
    CHECK(r.host().plugins().empty());

    check_status("shutdown", r.host().shutdown(), abi::ok);
    CHECK(g_failures == mark);
}

TEST_CASE(circular_leases_are_all_returned_by_shutdown)
{
    const std::size_t mark = g_failures;
    rack r;
    fake_factory& first = r.plug("a");
    fake_factory& second = r.plug("b");
    r.stage(first);
    r.stage(second);
    check_status("start", r.host().start(), abi::ok);

    fake_plug* a = first.instance();
    fake_plug* b = second.instance();
    check_status("a leases b", a->acquire_from(r.id("b")), abi::ok);
    check_status("b leases a", b->acquire_from(r.id("a")), abi::ok);
    CHECK(a->holds_lease());
    CHECK(b->holds_lease());
    check_status("a binds b.echo", a->bind_bound_method(), abi::ok);
    check_status("b binds a.echo", b->bind_bound_method(), abi::ok);

    check_status("shutdown", r.host().shutdown(), abi::ok);
    CHECK(a->revoker.self_ok);
    CHECK(b->revoker.self_ok);
    // A recursive unload would re-enter the receiver or keep notifying in a loop.
    CHECK(a->revoker.reentrant_calls == 0);
    CHECK(b->revoker.reentrant_calls == 0);
    CHECK(a->revoker.calls <= 2);
    CHECK(b->revoker.calls <= 2);
    if (a->revoker.notified) check_status("a release", a->revoker.release_status, abi::ok);
    if (b->revoker.notified) check_status("b release", b->revoker.release_status, abi::ok);

    const std::vector<std::string> reverse = {r.id("b"), r.id("a")};
    check_sequence("stop order", trace_ids("stop"), reverse);
    check_sequence("destroy order", trace_ids("destroy"), reverse);
    CHECK(a->stop_calls == 1 && b->stop_calls == 1);
    CHECK(a->destroy_calls == 1 && b->destroy_calls == 1);
    CHECK(r.host().plugins().empty());
    CHECK(g_failures == mark);
}

} // namespace

int main()
{
    std::size_t failed = 0;
    for (const test_case& test : tests()) {
        g_current_test = test.name;
        // The trace collector is global, but every case asserts only its own lifecycle run.
        trace_log().clear();
        const std::size_t before = g_failures;
        test.run();
        const std::size_t raised = g_failures - before;
        if (raised != 0) {
            ++failed;
            std::fprintf(stderr, "test '%s' recorded %zu deferred failure(s)\n", test.name, raised);
        }
    }
    if (failed != 0) {
        std::fprintf(stderr, "%zu of %zu test case(s) failed\n", failed, tests().size());
        std::fflush(stderr);
        return 1;
    }
    std::printf("%zu test cases passed\n", tests().size());
    return 0;
}
