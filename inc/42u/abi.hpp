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
 * @brief Frozen-shape, same-profile C++ plugin ABI for the first SDK.
 * @note Interface declarations must be compiled with the platform's default packing.
 */
namespace u42::abi::v1 {
using status = std::uint32_t;
using method_id = std::uint32_t;
inline constexpr std::uint32_t abi_major = 1;
inline constexpr status ok = 0, invalid_argument = 1, unsupported = 2,
    not_found = 3, duplicate = 4, invalid_state = 5, busy = 6,
    stale = 7, limit_exceeded = 8, failed = 9, wrong_thread = 10,
    cycle = 11, deferred = 12;

/**
 * @brief Identifies an exact immutable interface contract, not an object.
 */
struct iid { std::uint64_t high; std::uint64_t low; };
constexpr bool operator==(iid a, iid b) noexcept { return a.high == b.high && a.low == b.low; }
constexpr bool operator!=(iid a, iid b) noexcept { return !(a == b); }
inline constexpr iid events_iid{0x3432555f41424931ULL, 1};
inline constexpr iid caps_iid{0x3432555f41424931ULL, 2};
inline constexpr iid calls_iid{0x3432555f41424931ULL, 3};
inline constexpr iid diag_iid{0x3432555f41424931ULL, 4};
inline constexpr iid invoke_iid{0x3432555f41424931ULL, 5};

/**
 * @brief Borrowed bytes, valid only during the current call; null requires size zero.
 */
struct bytes { const void* data; std::uint64_t size; };

/**
 * @brief Opaque host credential; zero is invalid. Never fabricate or decode value.
 */
struct token { std::uint64_t value = 0; };

/**
 * @brief Opaque method binding; zero is invalid. It never silently follows reloads.
 */
struct binding { std::uint64_t value = 0; };

/**
 * @brief Interface pointer and separately revocable host borrowing credential.
 */
struct borrow { void* ptr = nullptr; token credential{}; };

/**
 * @brief Immutable factory metadata, borrowed until the library is unloaded.
 * @note Strings are NUL-terminated UTF-8. Array pointers may be null only for zero count.
 *       struct_size must equal sizeof(plug_desc); reserved must be zero.
 */
struct plug_desc {
    std::uint32_t struct_size = sizeof(plug_desc);
    std::uint32_t reserved = 0;
    const char* plug_id = nullptr;
    const char* version = nullptr;
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
 * @brief Full capability announcement, copied by the host during start().
 * @note struct_size must match this version; null arrays require a zero corresponding count.
 */
struct caps_desc {
    std::uint32_t struct_size = sizeof(caps_desc);
    std::uint32_t interface_count = 0;
    const iid* interfaces = nullptr;
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
struct cap_event { const char* plug_id; std::uint32_t available; caps_desc capabilities; };

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
     * @brief Clear cached pointers and return this credential through icaps::release().
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
 * @brief Capability registration, atomic snapshot watching, and tracked interface borrowing.
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
     * @brief Borrow an advertised interface from the current active instance.
     */
    virtual status U42_CALL acquire(const char* plug_id, const iid* type,
                                    irevoker* receiver, borrow* out) noexcept = 0;
    /**
     * @brief Return a caller-owned credential; old returns must not affect new records.
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
     * @brief Bind the current plugin method by its name.
     */
    virtual status U42_CALL bind_name(const char* plug_id, const char* name, binding* out) noexcept = 0;
    /**
     * @brief Bind the current plugin method by its published numeric ID.
     */
    virtual status U42_CALL bind_id(const char* plug_id, method_id id, binding* out) noexcept = 0;
    /**
     * @brief Invoke synchronously; partial output is discarded when invocation fails.
     */
    virtual status U42_CALL call(binding target, bytes args, iwriter* result) noexcept = 0;
    /**
     * @brief Release a binding owned by this context without keeping the provider alive.
     */
    virtual status U42_CALL unbind(binding target) noexcept = 0;
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
 * @brief Plugin-side optional synchronous dynamic-method interface.
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
     * @brief Return a stable borrowed optional interface pointer; unknown types clear output.
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
static_assert(sizeof(iid) == 16 && sizeof(token) == 8 && sizeof(binding) == 8);
static_assert(std::is_standard_layout_v<plug_desc> && std::is_standard_layout_v<caps_desc>);
// Enforce the 64-bit native-packing profile at every SDK consumer, not only in host tests.
#if UINTPTR_MAX == UINT64_MAX
static_assert(alignof(iid) == 8 && sizeof(bytes) == 16 && alignof(bytes) == 8);
static_assert(sizeof(borrow) == 16 && alignof(borrow) == 8);
static_assert(sizeof(plug_desc) == 56 && alignof(plug_desc) == 8);
static_assert(offsetof(plug_desc, plug_id) == 8 && offsetof(plug_desc, version) == 16);
static_assert(offsetof(plug_desc, priority) == 24 && offsetof(plug_desc, before) == 32);
static_assert(offsetof(plug_desc, after_count) == 40 && offsetof(plug_desc, after) == 48);
static_assert(sizeof(method_desc) == 40 && offsetof(method_desc, name) == 8);
static_assert(sizeof(caps_desc) == 32 && alignof(caps_desc) == 8);
static_assert(offsetof(caps_desc, interfaces) == 8 && offsetof(caps_desc, methods) == 24);
static_assert(sizeof(event) == 24 && sizeof(cap_event) == 48);
static_assert(sizeof(iplug) == sizeof(void*) && sizeof(ictx) == sizeof(void*));
#endif
} // namespace u42::abi::v1

/**
 * @brief Entry declaration; plugin definitions add U42_EXPORT, host consumers do not export it.
 * @param major Requested profile-compatible ABI major, currently 1.
 * @param out Library-owned factory; must be cleared on failure.
 * @return ok if supported, otherwise unsupported or invalid_argument.
 */
extern "C" u42::abi::v1::status U42_CALL u42_get_factory(
    std::uint32_t major, u42::abi::v1::iplug_fty** out) noexcept;
