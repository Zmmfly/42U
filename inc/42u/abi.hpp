#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#if defined(_WIN32)
#  define U42_EXPORT __declspec(dllexport)
#  define U42_CALL __cdecl
#else
#  define U42_EXPORT __attribute__((visibility("default")))
#  define U42_CALL
#endif

/**
 * @brief Invoke-only plugin ABI: version-qualified leases authorize calls without bindings.
 * @note ABI v3 is incompatible with v1/v2. Use the platform's default packing.
 */
namespace u42::abi::v3 {
using status = std::uint32_t;
using method_id = std::uint32_t;
inline constexpr std::uint32_t abi_major = 3;
inline constexpr status ok = 0, invalid_argument = 1, unsupported = 2,
    not_found = 3, duplicate = 4, invalid_state = 5, busy = 6,
    stale = 7, limit_exceeded = 8, failed = 9, wrong_thread = 10,
    cycle = 11, deferred = 12;

/**
 * @brief A 128-bit identity for a fixed framework service interface, never a plugin instance.
 * @note Plugin business compatibility is expressed by plug_id and plugin_version, not an IID.
 */
struct iid { std::uint64_t high; std::uint64_t low; };
constexpr bool operator==(iid a, iid b) noexcept { return a.high == b.high && a.low == b.low; }
constexpr bool operator!=(iid a, iid b) noexcept { return !(a == b); }
inline constexpr iid events_iid{0x3432555f41424933ULL, 1};
inline constexpr iid caps_iid{0x3432555f41424933ULL, 2};
inline constexpr iid calls_iid{0x3432555f41424933ULL, 3};
inline constexpr iid diag_iid{0x3432555f41424933ULL, 4};
inline constexpr iid invoke_iid{0x3432555f41424933ULL, 5};

/**
 * @brief Borrowed bytes, valid only during the current call; null requires size zero.
 */
struct bytes { const void* data; std::uint64_t size; };

/**
 * @brief Opaque host credential; zero is invalid. Never fabricate or decode value.
 */
struct token { std::uint64_t value = 0; };

/**
 * @brief Numeric plugin business version, distinct from the framework ABI and instance generation.
 * @note All unsigned triples, including 0.0.0, are valid. No prerelease or build-tag syntax is
 *       supported. Providers must version their public data and behavior honestly; the host does
 *       not infer semantic compatibility from these numbers or preserve state across reloads.
 */
struct plugin_version {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;
};

/** @brief Compare plugin versions for numeric equality. */
constexpr bool operator==(plugin_version left, plugin_version right) noexcept
{
    return left.major == right.major && left.minor == right.minor && left.patch == right.patch;
}
/** @brief Compare plugin versions for numeric inequality. */
constexpr bool operator!=(plugin_version left, plugin_version right) noexcept { return !(left == right); }

/**
 * @brief Order numeric version triples lexicographically without overflow.
 * @param left First version.
 * @param right Second version.
 * @return true when left precedes right by major, then minor, then patch.
 */
constexpr bool version_less(plugin_version left, plugin_version right) noexcept
{
    return left.major < right.major ||
           (left.major == right.major && (left.minor < right.minor ||
            (left.minor == right.minor && left.patch < right.patch)));
}

/**
 * @brief Explicit inclusive range [minimum, maximum] of accepted plugin versions.
 * @note Equal endpoints request an exact version; reversed endpoints are invalid. Crossing a
 *       major boundary is allowed only when the caller explicitly supplies such a range.
 */
struct version_range { plugin_version minimum{}; plugin_version maximum{}; };

/**
 * @brief Validate inclusive version range ordering.
 * @param allowed Range supplied by the borrower.
 * @return true when minimum is not greater than maximum.
 */
constexpr bool valid_version_range(version_range allowed) noexcept
{
    return !version_less(allowed.maximum, allowed.minimum);
}

/**
 * @brief Check the caller's explicit version bounds, not an inferred compatibility policy.
 * @param allowed Inclusive accepted range.
 * @param actual Version copied from the selected instance descriptor.
 * @return true when the range is valid and contains actual, including both endpoints.
 */
constexpr bool accepts_version(version_range allowed, plugin_version actual) noexcept
{
    return valid_version_range(allowed) && !version_less(actual, allowed.minimum) &&
           !version_less(allowed.maximum, actual);
}

/**
 * @brief Build an exact-version requirement.
 * @param value The only version to accept.
 * @return A closed range with both endpoints equal to value.
 */
constexpr version_range exact_version(plugin_version value) noexcept { return {value, value}; }

/**
 * @brief Revocable instance credential and the actual version atomically selected by acquire().
 * @note Success is status==ok with a nonzero credential; version alone cannot indicate success.
 *       The value contains no provider pointer. Even identical versions never revive old tokens
 *       after reload. Every business call uses this credential plus a method name or ID.
 */
struct borrow { token credential{}; plugin_version version{}; };

/**
 * @brief Immutable factory metadata, borrowed until the library is unloaded.
 * @note Strings are NUL-terminated UTF-8. Array pointers may be null only for zero count.
 *       struct_size must equal sizeof(plug_desc); reserved must be zero.
 */
struct plug_desc {
    std::uint32_t struct_size = sizeof(plug_desc);
    std::uint32_t reserved = 0;
    const char* plug_id = nullptr;
    plugin_version version{};
    std::int32_t priority = 0;
    std::uint32_t before_count = 0;
    const char* const* before = nullptr;
    std::uint32_t after_count = 0;
    const char* const* after = nullptr;
};

/**
 * @brief Synchronous JSON method description. Names and numbers are unique per plugin.
 */
struct method_desc {
    method_id id;
    const char* name;
    const char* description;
    const char* input_schema;
    const char* output_schema;
};

/**
 * @brief Complete method announcement, copied by the host during start().
 * @note struct_size must match this ABI; null methods require zero method_count. Methods stay
 *       fixed for this instance. Version comes from plug_desc, not a second business contract.
 *       An Active published instance with no methods may still be leased for lifetime.
 */
struct caps_desc {
    std::uint32_t struct_size = sizeof(caps_desc);
    std::uint32_t method_count = 0;
    const method_desc* methods = nullptr;
};

/**
 * @brief Read-only event view, valid only during delivery.
 */
struct event { const char* name; bytes payload; };

/**
 * @brief Capability snapshot/withdrawal; all pointers expire when the callback returns.
 */
struct cap_event {
    const char* plug_id;
    std::uint32_t available;
    plugin_version version;
    caps_desc capabilities;
};

/**
 * @brief Caller-owned output writer; plugins must not retain it after invoke returns.
 */
struct iwriter {
    /**
     * @brief Append bytes, returning a failure if capacity is exceeded.
     */
    virtual status U42_CALL write(bytes data) noexcept = 0;
protected:
    ~iwriter() = default;
};

/**
 * @brief Event receiver, owned by the subscribing plugin until subscription removal.
 */
struct ievent_sink {
    /**
     * @brief Observe one event on the control thread; do not retain borrowed data.
     */
    virtual void U42_CALL on_event(const event* value) noexcept = 0;
protected:
    ~ievent_sink() = default;
};

/**
 * @brief Capability receiver, including current snapshots and subsequent changes.
 */
struct icap_sink {
    /**
     * @brief Record availability; an initialized consumer must not call business methods yet.
     */
    virtual void U42_CALL on_capability(const cap_event* value) noexcept = 0;
protected:
    ~icap_sink() = default;
};

/**
 * @brief Receives synchronous revocation; the credential must be returned before returning.
 */
struct irevoker {
    /**
     * @brief Stop dependent business, invalidate per-instance state, and return the credential.
     * @note Use icaps::release(); retain ownership and retry if release reports a failure.
     *       No new business calls are allowed against the revoking provider.
     */
    virtual void U42_CALL on_revoke(token credential) noexcept = 0;
protected:
    ~irevoker() = default;
};

/**
 * @brief Queue-based, exact-name event service. Host lifecycle names beginning u42. are reserved.
 */
struct ievents {
    /**
     * @brief Subscribe an owned sink; output is cleared on failure.
     */
    virtual status U42_CALL subscribe(const char* name, ievent_sink* sink, token* out) noexcept = 0;
    /**
     * @brief Remove only a subscription owned by the calling context.
     */
    virtual status U42_CALL unsubscribe(token value) noexcept = 0;
    /**
     * @brief Copy and enqueue data without recursive dispatch; a full queue returns an error.
     */
    virtual status U42_CALL publish(const char* name, bytes data) noexcept = 0;
protected:
    ~ievents() = default;
};

/**
 * @brief Method registration, versioned discovery, and revocable instance leasing.
 */
struct icaps {
    /**
     * @brief Submit the complete capability set once during start(); host copies all data.
     */
    virtual status U42_CALL announce(const caps_desc* value) noexcept = 0;
    /**
     * @brief Subscribe plus enqueue the current snapshot atomically; no inline callback.
     */
    virtual status U42_CALL watch(icap_sink* sink, token* out) noexcept = 0;
    /**
     * @brief Remove the caller's watch and discard its pending notifications.
     */
    virtual status U42_CALL unwatch(token value) noexcept = 0;
    /**
     * @brief Lease an Active instance whose version lies within the caller's explicit bounds.
     *
     * @param plug_id Required current provider identity.
     * @param allowed Required inclusive range; minimum must not exceed maximum.
     * @param receiver Required stable revoker, alive until the credential is returned.
     * @param[out] out Required output, cleared first; receives credential and actual version together.
     * @return ok on success; unsupported if the provider version is outside allowed; otherwise an
     *         argument/state/allocation/thread error. Initialized consumers may lease but not call.
     */
    virtual status U42_CALL acquire(const char* plug_id, const version_range* allowed,
                                    irevoker* receiver, borrow* out) noexcept = 0;
    /**
     * @brief Return a caller-owned lease; subsequent calls with it are stale, never a new instance.
     * @note Returns busy while this lease has in-flight calls, including result delivery. Other
     *       failures retain ownership; ok/stale allow the caller to clear the token.
     */
    virtual status U42_CALL release(token credential) noexcept = 0;
protected:
    ~icaps() = default;
};

/**
 * @brief String and numeric lookup share the same generation-checked invocation path.
 */
struct icalls {
    /**
     * @brief Resolve and invoke a named method directly under a live caller-owned lease.
     *
     * @param credential Lease returned by icaps::acquire() for this context.
     * @param name Required exact method name; empty names are rejected.
     * @param args Borrowed JSON input for this call; null data requires zero size.
     * @param result Required caller-owned writer; receives at most one complete bounded result.
     * @return ok on success; stale for a returned lease; not_found for an unknown method; otherwise
     *         an argument/ownership/state/reentry/limit error. Initialized consumers cannot call.
     * @note The host buffers output and discards it if invoke fails. The lease remains owned on
     *       any invocation failure and cannot be released until output delivery has finished.
     */
    virtual status U42_CALL call_name(token credential, const char* name, bytes args,
                                      iwriter* result) noexcept = 0;
    /**
     * @brief Invoke a published numeric method directly under a live caller-owned lease.
     *
     * @param credential Lease returned by icaps::acquire() for this context.
     * @param method Provider-local published method ID.
     * @param args Borrowed JSON input; null data requires zero size.
     * @param result Required output writer, never retained by the provider.
     * @return The same lease/state/invocation statuses as call_name().
     */
    virtual status U42_CALL call_id(token credential, method_id method, bytes args,
                                    iwriter* result) noexcept = 0;
protected:
    ~icalls() = default;
};

/**
 * @brief Always-available, minimal host diagnostic sink.
 */
struct idiag {
    /**
     * @brief Emit a diagnostic string synchronously; input is never retained.
     */
    virtual void U42_CALL log(const char* message) noexcept = 0;
protected:
    ~idiag() = default;
};

/**
 * @brief Host-private plugin invocation gateway; never exposed to another plugin.
 */
struct iinvoke {
    /**
     * @brief Invoke a published method; arguments and output writer are borrowed for this call.
     */
    virtual status U42_CALL invoke(method_id method, bytes args, iwriter* result) noexcept = 0;
protected:
    ~iinvoke() = default;
};

/**
 * @brief Host context borrowed from init entry until destroy returns, control-thread only.
 */
struct ictx {
    /**
     * @brief Query an exact host service interface. Required arguments cannot be null.
     * @param type Frozen service interface identifier.
     * @param out Cleared on failure; returned pointer remains valid with this context.
     * @return ok on success, unsupported for unknown identifiers.
     */
    virtual status U42_CALL query(const iid* type, void** out) noexcept = 0;
protected:
    ~ictx() = default;
};

/**
 * @brief Host-owned plugin instance. Destroy only after rollback or successful stop.
 */
struct iplug {
    /**
     * @brief Initialize exactly once; failure must roll back to a destroyable, silent state.
     */
    virtual status U42_CALL init(ictx* ctx) noexcept = 0;
    /**
     * @brief Start exactly once and announce capabilities; failure rolls back startup work.
     */
    virtual status U42_CALL start() noexcept = 0;
    /**
     * @brief Stop initialized/active work; success proves all private threads and callbacks ended.
     */
    virtual status U42_CALL stop() noexcept = 0;
    /**
     * @brief Destroy on the allocating side; callers must never use delete across the ABI.
     */
    virtual void U42_CALL destroy() noexcept = 0;
    /**
     * @brief Return the host-private invoke_iid interface; never a cross-plugin business query.
     * @note Only the host uses this hook to obtain iinvoke. Consumers use leased icalls instead.
     */
    virtual status U42_CALL query(const iid* type, void** out) noexcept = 0;
protected:
    ~iplug() = default;
};

/**
 * @brief Library-owned static factory; the host borrows it while the library remains mapped.
 */
struct iplug_fty {
    /**
     * @brief Return immutable metadata without starting work; clear output on failure.
     */
    virtual status U42_CALL describe(const plug_desc** out) noexcept = 0;
    /**
     * @brief Create an uninitialized instance; failure leaves no resources and clears output.
     */
    virtual status U42_CALL create(iplug** out) noexcept = 0;
protected:
    ~iplug_fty() = default;
};

using entry_fn = status (U42_CALL *)(std::uint32_t, iplug_fty**) noexcept;
inline constexpr const char* entry_name = "u42_get_factory";
static_assert(sizeof(iid) == 16 && sizeof(token) == 8);
static_assert(sizeof(plugin_version) == 12 && sizeof(version_range) == 24);
static_assert(std::is_standard_layout_v<plugin_version> && std::is_trivially_copyable_v<borrow>);
static_assert(std::is_standard_layout_v<plug_desc> && std::is_standard_layout_v<caps_desc>);
// Enforce the 64-bit native-packing profile at every SDK consumer, not only in host tests.
#if UINTPTR_MAX == UINT64_MAX
static_assert(alignof(iid) == 8 && sizeof(bytes) == 16 && alignof(bytes) == 8);
static_assert(sizeof(borrow) == 24 && alignof(borrow) == 8 && offsetof(borrow, version) == 8);
static_assert(alignof(plugin_version) == 4 && alignof(version_range) == 4);
static_assert(sizeof(plug_desc) == 64 && alignof(plug_desc) == 8);
static_assert(offsetof(plug_desc, plug_id) == 8 && offsetof(plug_desc, version) == 16);
static_assert(offsetof(plug_desc, priority) == 28 && offsetof(plug_desc, before) == 40);
static_assert(offsetof(plug_desc, after_count) == 48 && offsetof(plug_desc, after) == 56);
static_assert(sizeof(method_desc) == 40 && offsetof(method_desc, name) == 8);
static_assert(sizeof(caps_desc) == 16 && alignof(caps_desc) == 8);
static_assert(offsetof(caps_desc, methods) == 8);
static_assert(sizeof(event) == 24 && sizeof(cap_event) == 40);
static_assert(offsetof(cap_event, version) == 12 && offsetof(cap_event, capabilities) == 24);
static_assert(sizeof(iplug) == sizeof(void*) && sizeof(ictx) == sizeof(void*));
#endif
} // namespace u42::abi::v3

/**
 * @brief Entry declaration; plugin definitions add U42_EXPORT, host consumers do not export it.
 * @param major Requested profile-compatible ABI major, currently 3; v1/v2 layouts are incompatible.
 * @param out Library-owned factory; must be cleared on failure.
 * @return ok if supported, otherwise unsupported or invalid_argument.
 */
extern "C" u42::abi::v3::status U42_CALL u42_get_factory(
    std::uint32_t major, u42::abi::v3::iplug_fty** out) noexcept;
