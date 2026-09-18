/**
 * @file runtime_test.cc
 * @brief Self-contained contract checks for the 42U host runtime; no external test framework.
 *
 * ABI v3 is invoke-only and binding-free: plugins never share a business class, a business
 * interface pointer or a provider iinvoke pointer. The only business path is
 * consumer -> host icalls -> provider iinvoke, authorized per call by an opaque lease
 * credential that icaps::acquire() returns together with the provider's actual plugin_version.
 * The fake plugins below therefore announce only a numeric metadata version plus their method
 * table, and every borrow is verified through its credential and a controlled business call,
 * never through a stored pointer or a binding handle.
 *
 * The file owns its main() and only depends on the public headers <42u/abi.hpp> and
 * <42u/host.hpp>, so it stays independent of the host implementation that is still being
 * written. Validate it with a syntax-only compile before linking:
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
 *    credential, used to reach the credential-addressed host call entries without repeating
 *    acquire/release.
 *
 * Covered contracts (design baseline docs/42U插件框架设计.md, migration contract
 * docs/thinks/invoke-v3-implementation-contract.md):
 *  - all plugins init before the first start; start order equals the initialization plan;
 *  - priority and before/after drive that single shared order;
 *  - stop and destroy run in reverse plan order;
 *  - init failure rolls back, nothing starts, the failed instance is never stopped;
 *  - start failure revokes announced capabilities and stops every initialized instance;
 *  - duplicate plug_id is refused;
 *  - unknown interface queries clear their output; known host services resolve;
 *  - a plugin query exposes only the host-private invoke_iid subobject and no business
 *    interface, and the host reaches the method through that gateway;
 *  - capabilities exist only after a successful start: an initialized consumer may acquire a
 *    lease but must not issue a business call;
 *  - a lease is a credential plus the provider's actual version, not a pointer: calls name the
 *    method directly and are routed through the host gateway to the provider's iinvoke;
 *  - name and numeric method lookup share one implementation;
 *  - unknown method/plugin, null required pointers, zero credentials and stale credentials
 *    return a definite error and clear the caller-owned output;
 *  - returning a lease makes its credential stale for every method, in both call forms;
 *  - partial output of a failed call is discarded, and the output limit stays sticky even if the
 *    plugin ignores the writer failure;
 *  - the plugin version travels through cap_event and through the borrow, and a version outside
 *    the caller's explicit range is refused without creating a lock;
 *  - revoking a lease lets the provider unload; an unreturned lease isolates the provider;
 *  - after a reload the old lease credential is stale for name and id while the new generation is
 *    usable after a fresh acquire;
 *  - unloading a consumer first returns its outbound leases;
 *  - a borrow cycle is fully returned by shutdown without recursive unloading.
 *
 * Deliberately not covered here:
 *  - boot()/load()/scan_plugins(): they need real .u42.so artefacts;
 *  - event publish/subscribe and cross-library ABI compatibility, which are outside the
 *    contract set requested for this file;
 *  - the exhaustive version_range matrix (inclusive edges, ordering, uint32 bounds), owned by
 *    tests/lease_test.cc.
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
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace abi = u42::abi::v3;

/**
 * @brief The exact {credential, version} shape a v3 borrow must keep.
 *
 * A borrow equal in size to this plain aggregate cannot hide a provider pointer, and the pinned
 * version offset proves the second member is the numeric version, not a second credential.
 */
struct borrow_shape {
    abi::token credential;
    abi::plugin_version version;
};
static_assert(sizeof(abi::borrow) == sizeof(borrow_shape),
              "a v3 lease must carry only a credential and the actual version, never a pointer");
static_assert(offsetof(abi::borrow, version) == 8,
              "the actual version must follow the credential at the pinned offset");
static_assert(std::is_same_v<decltype(abi::borrow{}.version), abi::plugin_version>,
              "borrow.version must be the numeric plugin_version, not a string or contract");
// caps_desc is a method table only: the removed protocol contract no longer widens it.
static_assert(sizeof(abi::caps_desc) == 16,
              "a v3 capability set is a method table only; bind/protocol types were removed");
static_assert(std::is_same_v<decltype(abi::caps_desc{}.methods), const abi::method_desc*>,
              "caps_desc exposes only the method table to the host");
static_assert(std::is_same_v<decltype(abi::plug_desc{}.version), abi::plugin_version>,
              "plug_desc metadata carries a numeric plugin_version triple, not a version string");
static_assert(std::is_same_v<decltype(abi::cap_event{}.version), abi::plugin_version>,
              "cap_event reports the notifying instance's numeric plugin_version");

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
 * Used where the contract names a set rather than a single code, for example a withdrawn but
 * still-live lease.
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
 * Test-only data values and caller-owned writer.
 * None of these types is part of the ABI; both sides of the test are built together.
 * ------------------------------------------------------------------ */

/** @brief Arbitrary interface identifier; the host must never publish a business interface. */
inline constexpr abi::iid test_business_iid{0x7465737434325533ULL, 0x0001};

/**
 * @brief Plugin business version every fake announces by default.
 *
 * A numeric triple, not a string and not a contract: the host copies it as metadata and returns
 * it again through cap_event and through borrow.
 */
inline constexpr abi::plugin_version base_version{1, 0, 0};
/** @brief A different version used only to prove an explicit range is really enforced. */
inline constexpr abi::plugin_version next_version{1, 1, 0};

/** @brief Inclusive range accepting the whole 1.x line; every fake consumer requests it. */
inline constexpr abi::version_range accepted_versions{base_version,
                                                      abi::plugin_version{1, UINT32_MAX, UINT32_MAX}};
/** @brief Exact range for base_version. */
inline constexpr abi::version_range exact_base = abi::exact_version(base_version);
/** @brief Exact range for next_version, deliberately not satisfied by a base_version provider. */
inline constexpr abi::version_range exact_next = abi::exact_version(next_version);

/** @brief Method identity of the fake provider's dynamic methods. */
inline constexpr abi::method_id echo_method = 7;
inline constexpr abi::method_id ping_method = 9;

/** @brief Minimal caller-owned writer for direct icalls invocations. */
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
 * the credential is returned. This helper owns both, so a test can hold the credential-addressed
 * host call entries without repeating acquire/release, and the receiver is still valid when the
 * provider is revoked by unload() or shutdown(); it then returns the credential immediately
 * instead of fabricating a lease.
 *
 * @note This is a plain owner of the ABI's own {credential, version} borrow. It adds no binding
 *       step and no business pointer; every call it enables still passes the credential to
 *       u42::host::call(), exactly like production code.
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
     * @param allowed Explicitly accepted inclusive version range.
     */
    admin_lease(u42::host& host, std::string plug_id, abi::version_range allowed)
        : host_(&host), plug_id_(std::move(plug_id))
    {
        status_ = host_->acquire(plug_id_, allowed, &revoker_, &borrow_);
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

    /**
     * @brief Actual provider version returned with the credential.
     * @return The version copied into the borrow by acquire(), or 0.0.0 when nothing is held;
     *         ownership must be tested with holds(), because 0.0.0 is a legal version.
     */
    abi::plugin_version version() const noexcept { return borrow_.version; }

    /** @brief Number of revocation callbacks observed on the owned receiver. */
    std::uint64_t revocations() const noexcept { return revoker_.calls; }

    /**
     * @brief Return the credential through the host and clear the local borrow on ok/stale.
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
    /** Announce the method table during start(). */
    bool announce = true;
    /** Register an icaps::watch during init(). */
    bool watch_in_init = false;
    /** Run the lease/call probes during init(). */
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
    /** Numeric metadata version this factory advertises in its plug_desc. */
    abi::plugin_version version = base_version;
    /** Inclusive range this instance requires when it acquires a peer lease. */
    abi::version_range required = accepted_versions;
    /** Version this instance expects the acquired peer to actually report. */
    abi::plugin_version peer_version = base_version;
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
    abi::plugin_version version{};
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

    // Test-side driving of the consumer role: a credential, never a binding.
    abi::status acquire_from(const std::string& provider);
    abi::status acquire_peer();
    /**
     * @brief Invoke a named method with an explicit credential through this instance's icalls.
     *
     * The credential is copied by value because the lease is just a token; the helper is what the
     * tests use to prove that a returned credential stays stale for name and id alike.
     */
    abi::status call_name_with(abi::token credential, const char* name, abi::bytes args,
                               std::string* out);
    /** @brief Numeric form of @ref call_name_with. */
    abi::status call_id_with(abi::token credential, abi::method_id method, abi::bytes args,
                             std::string* out);
    /** @brief Named "echo" call under the currently held lease credential. */
    abi::status call_echo(const std::string& text, std::string* out);
    /** @brief Numeric "echo" call under the currently held lease credential. */
    abi::status call_echo_id(const std::string& text, std::string* out);
    abi::status release_lease();
    abi::status release_token(abi::token value);
    /** @brief Lease and call-probe the provider from inside a capability callback. */
    void run_capability_probe(const std::string& provider);
    bool holds_lease() const noexcept { return lease_.credential.value != 0; }
    /** @brief Version the host returned together with the held credential. */
    abi::plugin_version lease_version() const noexcept { return lease_.version; }
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
    bool probe_version_matches = false;
    abi::token probe_credential{};
    abi::status probe_call_name = abi::failed;
    abi::status probe_call_id = abi::failed;
    bool probe_credential_intact = false;
    bool probe_business_blocked = false;
    bool probe_seen = false;
    /** True when the capability probe ran before this instance was started. */
    bool probe_while_initialized = false;
    abi::status watch_status = abi::failed;
    abi::token watch_token{};
    // Invocation observations.
    std::uint64_t invoke_calls = 0;
    abi::method_id last_invoke_method = 0;
    std::uint64_t writes_attempted = 0;
    std::int64_t first_failure_index = -1;
    abi::status observed_after_failure = abi::ok;
    // Consumer state: one credential plus the actual provider version, never a binding.
    abi::icaps* caps_ = nullptr;
    abi::icalls* calls_ = nullptr;
    cap_recorder cap_sink{this};
    lease_revoker revoker{this};
    abi::borrow lease_{};
    abi::status last_acquire = abi::failed;
    bool last_acquire_cleared = false;
    abi::status last_release = abi::failed;
    std::uint64_t acquire_calls = 0;
    std::uint64_t lease_returns = 0;
    std::uint64_t stale_release_calls = 0;

private:
    /** @brief Shared acquire-and-probe body used from init() and from the capability callback. */
    void run_lease_probe(const std::string& provider);

    fake_factory& owner_;
};

/** @brief Library-owned fake factory with stable descriptor storage. */
class fake_factory final : public abi::iplug_fty {
public:
    /**
     * @brief Build one fake plugin type with immutable metadata.
     *
     * @param plug_id Stable plugin identity; descriptors borrow this storage.
     * @param behavior Per-instance behavior switches, including the numeric version metadata.
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
    /** @brief Numeric metadata version advertised by this factory. */
    abi::plugin_version version() const noexcept { return behavior_.version; }
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
// The direct-call surface is binding-free: a credential plus a name or id, nothing else.
static_assert(noexcept(std::declval<abi::icaps&>().acquire(nullptr, nullptr, nullptr, nullptr)),
              "icaps::acquire must stay noexcept");
static_assert(noexcept(std::declval<abi::icaps&>().release(abi::token{})),
              "icaps::release must stay noexcept");
static_assert(noexcept(std::declval<abi::icalls&>().call_name(abi::token{}, nullptr, abi::bytes{},
                                                              nullptr)),
              "icalls::call_name must stay noexcept");
static_assert(noexcept(std::declval<abi::icalls&>().call_id(abi::token{}, 0, abi::bytes{},
                                                             nullptr)),
              "icalls::call_id must stay noexcept");
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
    // v3 metadata: the compatibility promise is a numeric triple, not a string or a contract.
    desc_.version = behavior_.version;
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
    // v3 announcement: the method table only; there is no protocol member any more.
    caps_.struct_size = sizeof(abi::caps_desc);
    caps_.method_count = static_cast<std::uint32_t>(method_descs_.size());
    caps_.methods = method_descs_.empty() ? nullptr : method_descs_.data();
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
    // The notification carries the numeric version of the instance it describes.
    notice.version = value->version;
    const abi::caps_desc& caps = value->capabilities;
    notice.counts_consistent = caps.struct_size == sizeof(abi::caps_desc);
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
        // The returned credential ends this generation's session; nothing else has to be dropped.
        lease_ = {};
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

    if (mode.probe_in_init) run_lease_probe(mode.peer);

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
    // No business interface is exposed to a consumer: a plugin has no protocol contract to query
    // and only the host ever asks for the private invoke gateway.
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

void fake_plug::run_lease_probe(const std::string& provider)
{
    if (caps_ == nullptr || calls_ == nullptr) {
        RECORD_NOTE("the lease probe ran without the caps or calls service");
        return;
    }
    const fake_behavior& mode = behavior();
    abi::borrow probe{};
    probe.credential.value = 0x2;                             // dirty: a failed acquire must clear it
    probe.version = abi::plugin_version{9, 9, 9};             // dirty: the actual version must win
    probe_acquire = caps_->acquire(provider.c_str(), &mode.required, &revoker, &probe);
    probe_acquire_cleared = probe.credential.value == 0 && probe.version == abi::plugin_version{};
    if (probe_acquire == abi::ok) {
        lease_ = probe;
        probe_version_matches = probe.version == mode.peer_version;
    }
    const abi::token credential = probe.credential;
    // No binding step exists: a refused call is proved by the credential-addressed call itself.
    buffer_writer by_name;
    buffer_writer by_id;
    probe_call_name = calls_->call_name(probe.credential, "echo", abi::bytes{nullptr, 0}, &by_name);
    probe_call_id = calls_->call_id(probe.credential, echo_method, abi::bytes{nullptr, 0}, &by_id);
    // A refused call takes the credential by value and must not disturb the caller's token.
    probe_credential_intact = probe.credential.value == credential.value;
    probe_business_blocked = probe_call_name != abi::ok && probe_call_id != abi::ok &&
                             by_name.value.empty() && by_id.value.empty();
}

void fake_plug::run_capability_probe(const std::string& provider)
{
    // Runs inside a noexcept capability callback: results are recorded, never thrown.
    probe_seen = true;
    probe_while_initialized = start_calls == 0;
    run_lease_probe(provider);
}

abi::status fake_plug::acquire_from(const std::string& provider)
{
    ++acquire_calls;
    if (caps_ == nullptr) return abi::invalid_state;
    const abi::version_range required = behavior().required;
    abi::borrow result{};
    result.credential.value = 0x1; // dirty: a failed acquire must clear it
    result.version = abi::plugin_version{9, 9, 9};
    last_acquire = caps_->acquire(provider.c_str(), &required, &revoker, &result);
    last_acquire_cleared = result.credential.value == 0 && result.version == abi::plugin_version{};
    if (last_acquire == abi::ok) lease_ = result;
    return last_acquire;
}

abi::status fake_plug::acquire_peer()
{
    return acquire_from(behavior().peer);
}

abi::status fake_plug::call_name_with(abi::token credential, const char* name, abi::bytes args,
                                      std::string* out)
{
    if (calls_ == nullptr) return abi::invalid_state;
    buffer_writer writer;
    const abi::status called = calls_->call_name(credential, name, args, &writer);
    if (out != nullptr) *out = writer.value;
    return called;
}

abi::status fake_plug::call_id_with(abi::token credential, abi::method_id method, abi::bytes args,
                                    std::string* out)
{
    if (calls_ == nullptr) return abi::invalid_state;
    buffer_writer writer;
    const abi::status called = calls_->call_id(credential, method, args, &writer);
    if (out != nullptr) *out = writer.value;
    return called;
}

abi::status fake_plug::call_echo(const std::string& text, std::string* out)
{
    return call_name_with(lease_.credential, "echo", abi::bytes{text.data(), text.size()}, out);
}

abi::status fake_plug::call_echo_id(const std::string& text, std::string* out)
{
    return call_id_with(lease_.credential, echo_method, abi::bytes{text.data(), text.size()}, out);
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
    // credential this instance actually holds. There is no binding to clean up.
    if (lease_.credential.value == value.value) lease_ = {};
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
    admin_lease blocked_lease(r.host(), r.id("good"), accepted_versions);
    check_not_ok("acquire after rollback", blocked_lease.status());
    CHECK(!blocked_lease.holds());
    CHECK(blocked_lease.version() == abi::plugin_version{});
    std::string output = "stale";
    check_not_ok("call after rollback",
                 r.host().call(r.id("good"), accepted_versions, "echo", abi::bytes{nullptr, 0},
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

    // The withdrawn capability must not be leasable or callable.
    admin_lease blocked_lease(r.host(), r.id("provider"), accepted_versions);
    check_not_ok("acquire after start failure", blocked_lease.status());
    CHECK(!blocked_lease.holds());
    CHECK(blocked_lease.version() == abi::plugin_version{});
    std::string output = "stale";
    check_not_ok("call after start failure",
                 r.host().call(r.id("provider"), accepted_versions, "echo", abi::bytes{nullptr, 0},
                               &output));
    CHECK(output.empty());

    // Note: the host withdraws the announced capability (proved by the failed acquire/call
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

    // A consumer never receives a business interface: a plugin has no queryable business iid,
    // so no business pointer can be obtained this way.
    slot = reinterpret_cast<void*>(0x1);
    check_status("query(business iid)", instance->query(&test_business_iid, &slot),
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
                 r.host().call(r.id("multi"), accepted_versions, "echo", abi::bytes{"x", 1}, &out),
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
    CHECK(!instance->holds_lease());
    CHECK(instance->probe_credential.value == 0);
    // Without a lease every direct call is refused and produces no output.
    check_status("call name with no lease", instance->probe_call_name, abi::invalid_argument);
    check_status("call id with no lease", instance->probe_call_id, abi::invalid_argument);
    CHECK(instance->probe_credential_intact);
    CHECK(instance->probe_business_blocked);

    // A watch registered during init still receives the current snapshot afterwards, carrying
    // the version of the instance that announced it.
    check_status("poll", r.host().poll(), abi::ok);
    const cap_notice* notice = instance->cap_sink.last_for(r.id("provider"));
    CHECK(notice != nullptr);
    if (notice != nullptr) {
        CHECK(notice->available);
        CHECK(notice->version == base_version);
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
    consumer_behavior.peer_version = base_version;
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
    // An Initialized consumer may hold a lease credential and learn the actual provider version ...
    check_status("acquire while initialized", instance->probe_acquire, abi::ok);
    CHECK(!instance->probe_acquire_cleared);
    CHECK(instance->holds_lease());
    CHECK(instance->probe_version_matches);
    CHECK(instance->lease_.credential.value != 0);
    CHECK(instance->lease_version() == base_version);
    instance->probe_credential = instance->lease_.credential;
    // ... but a business call must be refused while it is still Initialized: leasing is allowed
    // for an Initialized consumer, calling is not.
    check_status("call name while initialized", instance->probe_call_name, abi::invalid_state);
    check_status("call id while initialized", instance->probe_call_id, abi::invalid_state);
    CHECK(instance->probe_credential_intact);
    CHECK(instance->probe_business_blocked);
    CHECK(provider.instance()->invoke_calls == 0);

    // Once the consumer is Active the same credential works, and the call really reaches the
    // provider gateway through the direct call path.
    CHECK(instance->lease_.credential.value == instance->probe_credential.value);
    std::string text;
    check_status("direct call", instance->call_echo("named", &text), abi::ok);
    CHECK(text == "named");
    CHECK(provider.instance()->invoke_calls == 1);
    check_status("direct numeric call", instance->call_echo_id("numeric", &text), abi::ok);
    CHECK(text == "numeric");
    CHECK(provider.instance()->invoke_calls == 2);
    CHECK(instance->lease_.credential.value == instance->probe_credential.value);
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
        // The notification carries the notifying instance's numeric version, not a contract.
        CHECK(notice->version == provider.version());
        CHECK((notice->methods == std::vector<abi::method_id>{echo_method, ping_method}));
        CHECK((notice->method_names == std::vector<std::string>{"echo", "ping"}));
    }
    CHECK(instance->cap_sink.null_events == 0);

    // Explicit discovery returns the current version without locking anything or accepting it
    // on the caller's behalf.
    abi::plugin_version discovered{9, 9, 9};
    check_status("version(provider)", r.host().version(r.id("provider"), &discovered), abi::ok);
    CHECK(discovered == provider.version());
    abi::plugin_version unknown_version{9, 9, 9};
    check_status("version(unknown)", r.host().version(r.id("nosuch"), &unknown_version),
                 abi::not_found);
    CHECK(unknown_version == abi::plugin_version{});
    check_status("version(null output)", r.host().version(r.id("provider"), nullptr),
                 abi::invalid_argument);

    // An explicitly mismatched range is refused and leaves no lock behind; the borrow is cleared
    // in full, version included.
    admin_lease mismatched(r.host(), r.id("provider"), exact_next);
    check_status("acquire(out of range)", mismatched.status(), abi::unsupported);
    CHECK(!mismatched.holds());
    CHECK(mismatched.version() == abi::plugin_version{});
    // A matching range succeeds and returns the actual version together with the credential.
    admin_lease matched(r.host(), r.id("provider"), exact_base);
    check_status("acquire(exact version)", matched.status(), abi::ok);
    CHECK(matched.holds());
    CHECK(matched.version() == base_version);
    std::string text;
    check_status("call under matched lease",
                 r.host().call(matched.credential(), "echo", abi::bytes{"ok", 2}, &text), abi::ok);
    CHECK(text == "ok");
    check_status("release", matched.release(), abi::ok);

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

    // The administration context must hold a lease; there is no credential-free call shortcut.
    admin_lease lease(r.host(), r.id("provider"), accepted_versions);
    check_status("acquire", lease.status(), abi::ok);
    CHECK(lease.holds());
    CHECK(lease.version() == base_version);
    const abi::token live = lease.credential();

    const char* text = "hello";
    std::string from_name = "stale";
    std::string from_id = "stale";
    check_status("call(name)",
                 r.host().call(live, "echo", abi::bytes{text, 5}, &from_name), abi::ok);
    CHECK(from_name == "hello");
    check_status("call(id)", r.host().call(live, echo_method, abi::bytes{text, 5}, &from_id),
                 abi::ok);
    CHECK(from_id == from_name);
    CHECK(instance->last_invoke_method == echo_method);
    CHECK(instance->invoke_calls == 2);

    // The convenient one-shot form acquires, calls and returns without leaving a lease behind.
    std::string one_name = "stale";
    std::string one_id = "stale";
    check_status("one-shot call(name)",
                 r.host().call(r.id("provider"), accepted_versions, "echo", abi::bytes{text, 5},
                               &one_name),
                 abi::ok);
    check_status("one-shot call(id)",
                 r.host().call(r.id("provider"), accepted_versions, echo_method,
                               abi::bytes{text, 5}, &one_id),
                 abi::ok);
    CHECK(one_name == one_id);
    CHECK(one_name == "hello");
    CHECK(instance->invoke_calls == 4);

    std::string pong_name = "stale";
    std::string pong_id = "stale";
    check_status("call(name ping)",
                 r.host().call(live, "ping", abi::bytes{nullptr, 0}, &pong_name), abi::ok);
    check_status("call(id ping)",
                 r.host().call(live, ping_method, abi::bytes{nullptr, 0}, &pong_id), abi::ok);
    CHECK(pong_name == "pong");
    CHECK(pong_id == "pong");
    CHECK(instance->last_invoke_method == ping_method);

    // Returning the lease makes its credential stale for every method and both call forms.
    check_status("release", lease.release(), abi::ok);
    CHECK(!lease.holds());
    std::string after_name = "stale";
    std::string after_id = "stale";
    check_status("call(name) after release",
                 r.host().call(live, "echo", abi::bytes{text, 5}, &after_name), abi::stale);
    check_status("call(id) after release",
                 r.host().call(live, echo_method, abi::bytes{text, 5}, &after_id), abi::stale);
    CHECK(after_name.empty());
    CHECK(after_id.empty());
    std::string after_ping = "stale";
    check_status("call(ping) after release",
                 r.host().call(live, "ping", abi::bytes{nullptr, 0}, &after_ping), abi::stale);
    CHECK(after_ping.empty());
    check_status("release again", r.host().release(live), abi::stale);
    // The stale calls never reached the provider.
    CHECK(instance->invoke_calls == 6);

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

    admin_lease lease(r.host(), r.id("provider"), accepted_versions);
    check_status("acquire", lease.status(), abi::ok);
    CHECK(lease.version() == base_version);

    // An unknown method is discovered through a valid lease and reported as not_found.
    std::string output = "stale";
    check_status("call(unknown name)",
                 r.host().call(lease.credential(), "nosuch", abi::bytes{nullptr, 0}, &output),
                 abi::not_found);
    CHECK(output.empty());
    output = "stale";
    check_status("call(unknown id)",
                 r.host().call(lease.credential(), static_cast<abi::method_id>(4242),
                               abi::bytes{nullptr, 0}, &output),
                 abi::not_found);
    CHECK(output.empty());

    // A missing provider is discovered by acquire, so no credential can ever name it.
    admin_lease missing(r.host(), r.id("nosuch"), accepted_versions);
    check_status("acquire(unknown plugin)", missing.status(), abi::not_found);
    CHECK(!missing.holds());
    CHECK(missing.version() == abi::plugin_version{});
    output = "stale";
    check_status("call(unknown plugin)",
                 r.host().call(r.id("nosuch"), accepted_versions, "echo", abi::bytes{nullptr, 0},
                               &output),
                 abi::not_found);
    CHECK(output.empty());

    // Required pointers: a null argument is a parameter error, never a partial result.
    check_status("call(null output)",
                 r.host().call(lease.credential(), "echo", abi::bytes{nullptr, 0}, nullptr),
                 abi::invalid_argument);
    check_status("call id(null output)",
                 r.host().call(lease.credential(), echo_method, abi::bytes{nullptr, 0}, nullptr),
                 abi::invalid_argument);
    check_status("one-shot(null output)",
                 r.host().call(r.id("provider"), accepted_versions, "echo", abi::bytes{nullptr, 0},
                               nullptr),
                 abi::invalid_argument);
    output = "stale";
    check_status("call(zero credential)",
                 r.host().call(abi::token{}, "echo", abi::bytes{nullptr, 0}, &output),
                 abi::invalid_argument);
    CHECK(output.empty());
    output = "stale";
    check_status("call id(zero credential)",
                 r.host().call(abi::token{}, echo_method, abi::bytes{nullptr, 0}, &output),
                 abi::invalid_argument);
    CHECK(output.empty());
    check_status("release(zero credential)", r.host().release(abi::token{}), abi::invalid_argument);

    // A fabricated credential names no lease: it is stale, not a new instance.
    output = "stale";
    check_status("call(fabricated credential)",
                 r.host().call(abi::token{0xDEADBEEF}, "echo", abi::bytes{nullptr, 0}, &output),
                 abi::stale);
    CHECK(output.empty());

    // Borrowed input bytes: null data is only legal for a zero length, and the name is required.
    output = "stale";
    check_status("call(null bytes with size)",
                 r.host().call(lease.credential(), "echo", abi::bytes{nullptr, 5}, &output),
                 abi::invalid_argument);
    CHECK(output.empty());
    output = "stale";
    check_status("call(empty name)",
                 r.host().call(lease.credential(), "", abi::bytes{nullptr, 0}, &output),
                 abi::invalid_argument);
    CHECK(output.empty());

    // Empty input under a live lease is a legal call.
    output = "stale";
    check_status("call(empty input)",
                 r.host().call(lease.credential(), "echo", abi::bytes{nullptr, 0}, &output),
                 abi::ok);
    CHECK(output.empty());

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
    check_status("one-shot call(partial)",
                 r.host().call(r.id("provider"), accepted_versions, "echo", abi::bytes{"payload", 7},
                               &output),
                 abi::failed);
    CHECK(output.empty());
    // The plugin really did hand partial bytes to the writer before failing.
    CHECK(factory.instance()->writes_attempted == 1);

    admin_lease lease(r.host(), r.id("provider"), accepted_versions);
    check_status("acquire", lease.status(), abi::ok);
    std::string via_lease = "stale";
    check_status("leased call(partial)",
                 r.host().call(lease.credential(), "echo", abi::bytes{"payload", 7}, &via_lease),
                 abi::failed);
    CHECK(via_lease.empty());
    CHECK(factory.instance()->writes_attempted == 2);
    // The failed call left the lease usable: "ping" does not take the failing echo path.
    std::string after = "stale";
    check_status("call after failure",
                 r.host().call(lease.credential(), "ping", abi::bytes{nullptr, 0}, &after),
                 abi::ok);
    CHECK(after == "pong");

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
                 r.host().call(r.id("provider"), accepted_versions, "echo", abi::bytes{nullptr, 0},
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
                 r.host().call(r.id("provider"), accepted_versions, "echo", abi::bytes{"abcd", 4},
                               &at_limit),
                 abi::ok);
    CHECK(at_limit == "abcd");
    std::string over_limit = "stale";
    check_status("call(one over limit)",
                 r.host().call(r.id("provider"), accepted_versions, "echo", abi::bytes{"abcde", 5},
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
    CHECK(instance->lease_version() == base_version);
    const abi::token first = instance->lease_.credential;
    std::string before;
    check_status("call before unload", instance->call_echo("before", &before), abi::ok);
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
    CHECK(trace_count("stop", r.id("provider")) == 1);
    CHECK(trace_count("destroy", r.id("provider")) == 1);
    CHECK(sorted(r.host().plugins()) == std::vector<std::string>{r.id("consumer")});

    // The returned credential must not match anything afterwards, in either call form.
    check_status("release(stale credential)", instance->release_token(first), abi::stale);
    CHECK(instance->stale_release_calls == 1);
    std::string stale_name = "stale";
    std::string stale_id = "stale";
    check_status("call name(stale credential)",
                 instance->call_name_with(first, "echo", abi::bytes{nullptr, 0}, &stale_name),
                 abi::stale);
    check_status("call id(stale credential)",
                 instance->call_id_with(first, echo_method, abi::bytes{nullptr, 0}, &stale_id),
                 abi::stale);
    CHECK(stale_name.empty());
    CHECK(stale_id.empty());

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
    const abi::token held_credential = instance->lease_.credential;
    CHECK(held_credential.value != 0);
    CHECK(instance->lease_version() == base_version);

    check_not_ok("unload with an unreturned lease", r.host().unload(r.id("provider")));
    CHECK(!r.host().error().empty());
    CHECK(trace_count("stop", r.id("provider")) == 0);
    CHECK(trace_count("destroy", r.id("provider")) == 0);
    check_not_ok("repeated unload", r.host().unload(r.id("provider")));
    CHECK(instance->holds_lease());
    CHECK(r.host().plugins().size() == 2);

    // Capabilities were withdrawn first: the isolated provider can no longer be leased.
    admin_lease blocked_lease(r.host(), r.id("provider"), accepted_versions);
    check_not_ok("acquire(quarantined provider)", blocked_lease.status());
    CHECK(!blocked_lease.holds());
    // The still-live lease can no longer call the withdrawn provider, in either form.
    std::string withdrawn_name = "stale";
    std::string withdrawn_id = "stale";
    check_one_of("call name(quarantined provider)",
                 instance->call_name_with(held_credential, "echo", abi::bytes{nullptr, 0},
                                          &withdrawn_name),
                 {abi::not_found, abi::invalid_state});
    check_one_of("call id(quarantined provider)",
                 instance->call_id_with(held_credential, echo_method, abi::bytes{nullptr, 0},
                                        &withdrawn_id),
                 {abi::not_found, abi::invalid_state});
    CHECK(withdrawn_name.empty());
    CHECK(withdrawn_id.empty());

    // Returning the credential unblocks the unload.
    check_status("release", instance->release_lease(), abi::ok);
    CHECK(!instance->holds_lease());
    check_status("unload after return", r.host().unload(r.id("provider")), abi::ok);
    CHECK(trace_count("stop", r.id("provider")) == 1);
    CHECK(trace_count("destroy", r.id("provider")) == 1);
    CHECK(sorted(r.host().plugins()) == std::vector<std::string>{r.id("consumer")});

    check_status("shutdown", r.host().shutdown(), abi::ok);
    CHECK(g_failures == mark);
}

TEST_CASE(reload_stales_old_leases_and_serves_a_new_generation)
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
    CHECK(instance->lease_version() == base_version);
    std::string first_call;
    check_status("call(first generation)", instance->call_echo("first", &first_call), abi::ok);
    CHECK(first_call == "first");

    admin_lease admin(r.host(), r.id("provider"), accepted_versions);
    check_status("admin acquire(first generation)", admin.status(), abi::ok);
    CHECK(admin.version() == base_version);
    const abi::token admin_token = admin.credential();
    std::string output = "stale";
    check_status("admin call(first generation)",
                 r.host().call(admin.credential(), "echo", abi::bytes{"old", 3}, &output), abi::ok);
    CHECK(output == "old");

    check_status("unload(provider)", r.host().unload(r.id("provider")), abi::ok);
    // Both leases went through the revocation path; the administration receiver stayed alive
    // and returned its credential synchronously.
    CHECK(admin.revocations() == 1);
    CHECK(!admin.holds());
    CHECK(!instance->holds_lease());
    output = "stale";
    check_status("call(stale admin credential)",
                 r.host().call(admin_token, "echo", abi::bytes{"old", 3}, &output), abi::stale);
    CHECK(output.empty());
    std::string stale_session = "stale";
    check_status("call(stale session credential)",
                 instance->call_name_with(first_token, "echo", abi::bytes{nullptr, 0},
                                          &stale_session),
                 abi::stale);
    CHECK(stale_session.empty());
    std::string stale_session_id = "stale";
    check_status("call id(stale session credential)",
                 instance->call_id_with(first_token, echo_method, abi::bytes{nullptr, 0},
                                        &stale_session_id),
                 abi::stale);
    CHECK(stale_session_id.empty());

    // Reload the same plugin type: a new instance and a new internal generation, even for the
    // identical version.
    check_status("add(reload)", r.host().add(&provider), abi::ok);
    check_status("start(reload)", r.host().start(), abi::ok);
    fake_plug* reloaded = provider.instance();
    CHECK(reloaded != nullptr);
    CHECK(reloaded != instance);
    CHECK(reloaded->destroy_calls == 0);

    // The same version still needs a fresh lease; the old credential must not follow the reload.
    check_status("acquire(new generation)", instance->acquire_peer(), abi::ok);
    CHECK(instance->holds_lease());
    const abi::token second_token = instance->lease_.credential;
    CHECK(second_token.value != first_token.value);
    CHECK(instance->lease_version() == base_version);
    // The stale return must not match the new record, and must not break it.
    check_status("release(old credential)", instance->release_token(first_token), abi::stale);
    CHECK(instance->stale_release_calls == 1);
    CHECK(instance->holds_lease());
    std::string typed;
    check_status("call(new generation)", instance->call_echo("typed", &typed), abi::ok);
    CHECK(typed == "typed");
    CHECK(reloaded->invoke_calls == 1);
    CHECK(reloaded->last_invoke_method == echo_method);
    check_status("release(new credential)", instance->release_token(second_token), abi::ok);
    CHECK(!instance->holds_lease());

    // Notifications stay ordered: the withdrawal of the old instance precedes the announcement
    // of the new one, the reloaded generation ends up available, and each notice carries the
    // version of the instance it describes.
    check_status("poll", r.host().poll(), abi::ok);
    const std::vector<bool> availability = instance->cap_sink.availability_for(r.id("provider"));
    CHECK(!availability.empty());
    CHECK(availability.back());
    CHECK(std::find(availability.begin(), availability.end(), false) != availability.end());
    const cap_notice* latest = instance->cap_sink.last_for(r.id("provider"));
    CHECK(latest != nullptr);
    if (latest != nullptr) CHECK(latest->version == base_version);
    abi::plugin_version reloaded_version{};
    check_status("version after reload", r.host().version(r.id("provider"), &reloaded_version),
                 abi::ok);
    CHECK(reloaded_version == base_version);
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
    CHECK(instance->lease_version() == base_version);

    check_status("unload(consumer)", r.host().unload(r.id("consumer")), abi::ok);
    CHECK(trace_count("stop", r.id("consumer")) == 1);
    CHECK(trace_count("destroy", r.id("consumer")) == 1);
    if (instance->revoker.notified) {
        CHECK(instance->revoker.self_ok);
        check_status("release during consumer unload", instance->revoker.release_status, abi::ok);
        CHECK(!instance->holds_lease());
    }

    // The provider is untouched and, crucially, still unloadable: no lease is attributed to the
    // destroyed consumer any more. A fresh administration lease proves it is intact.
    admin_lease survivor(r.host(), r.id("provider"), accepted_versions);
    check_status("acquire(surviving provider)", survivor.status(), abi::ok);
    CHECK(survivor.version() == base_version);
    std::string text;
    check_status("call(surviving provider)",
                 r.host().call(survivor.credential(), "echo", abi::bytes{"alive", 5}, &text),
                 abi::ok);
    CHECK(text == "alive");
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
    CHECK(a->lease_version() == base_version);
    CHECK(b->lease_version() == base_version);
    std::string from_a;
    std::string from_b;
    check_status("a calls b.echo", a->call_echo("a->b", &from_a), abi::ok);
    CHECK(from_a == "a->b");
    check_status("b calls a.echo", b->call_echo("b->a", &from_b), abi::ok);
    CHECK(from_b == "b->a");
    CHECK(second.instance()->invoke_calls == 1);
    CHECK(first.instance()->invoke_calls == 1);

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
