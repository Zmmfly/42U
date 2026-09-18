/**
 * @file context.cc
 * @brief Context services and capability bookkeeping for the single-threaded host.
 *
 * This translation unit owns every host service reachable through one plugin context
 * except the event queue itself: interface lookup, capability announcement, capability
 * watching, version-qualified leasing, direct credential-based invocation, plus the engine
 * transitions that publish, withdraw, revoke and clean up what those services recorded.
 * Event subscription, publishing and engine::drain belong to src/events.cc; lifecycle
 * orchestration belongs to src/host.cc.
 *
 * Contract implemented here:
 * - Every ABI entry point is noexcept and maps any escaping exception to abi::v3::failed.
 * - Required outputs are cleared before any other validation; mutating entry points
 *   validate the control thread before reading or writing any other shared state. The
 *   control-thread identity is immutable after engine construction, so reading it off
 *   thread cannot race.
 * - A context whose owner is null is the host administration context: it holds business
 *   authority (watch, acquire, call) but may never announce capabilities.
 * - A plugin instance may only acquire a lease while initialized or active, and may only
 *   call business methods while active; recovery actions (release, unwatch) stay available
 *   in every state, including revocation and shutdown.
 * - No plugin callback is ever invoked inline from a mutating host operation. Capability
 *   changes are queued in engine::notices and delivered by engine::drain, which validates
 *   the recorded watch id before dispatch; pending notices therefore never retain a
 *   pointer to an owner that may have been removed. Each notice copies the version of the
 *   instance it reports when it is queued, so a later same-identity instance can never
 *   rewrite the version of an older notification.
 * - Tokens are drawn from one monotonic counter, are never reused and never wrap into a
 *   valid value, so a stale credential can never match a later record.
 * - One plugin instance discloses one method set and one numeric plugin_version. An
 *   instance with no disclosed method can still be leased for its lifetime; calling any
 *   method it did not disclose returns not_found.
 * - No business pointer crosses a plugin boundary. acquire() records a logical lock only:
 *   it compares the caller's explicit inclusive version range against the version copied
 *   from the provider descriptor, never calls iplug::query and returns no provider address.
 *   Every call names a live lease credential and reaches the provider only through the
 *   host-owned iinvoke that query(invoke_iid) returned when the instance published.
 * - A map reference is never kept across a plugin callback: leases and notices are
 *   re-resolved by token after any call that could have erased them, and the announced
 *   method id is copied before the callback. A lease's in-flight counter covers both the
 *   invoke and the delivery of its result.
 */
#include "internal.hpp"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace u42::detail {
namespace {

/** @brief Largest accepted entry count in one capability announcement. */
constexpr std::size_t announce_entry_limit = 4096;
/** @brief Largest accepted single string; bounds every host-side copy of plugin text. */
constexpr std::size_t text_limit = 64u * 1024u;

/**
 * @brief Measure a NUL-terminated string without reading past a hard bound.
 *
 * @param text Non-null candidate string.
 * @param limit Largest accepted byte length; the terminator must appear within it.
 * @param[out] size Receives the measured length; untouched when the call fails.
 * @return true when a terminator was found, false when the text is longer than limit.
 */
bool bounded_length(const char* text, std::size_t limit, std::size_t& size) noexcept
{
    if (!text) return false;
    for (std::size_t index = 0; index <= limit; ++index) {
        if (text[index] == '\0') {
            size = index;
            return true;
        }
    }
    return false;
}

/**
 * @brief Copy one optional plugin string into host-owned storage.
 *
 * @param text Optional NUL-terminated UTF-8 string; null is copied as empty.
 * @param[out] out Host-owned copy, replaced in place.
 * @return abi::v3::ok, or limit_exceeded for oversized text.
 * @note A failure may leave out partially replaced; callers discard it on failure.
 */
a::status copy_text(const char* text, std::string& out)
{
    std::size_t size = 0;
    if (!text) {
        out.clear();
        return a::ok;
    }
    if (!bounded_length(text, text_limit, size)) return a::limit_exceeded;
    out.assign(text, size);
    return a::ok;
}

/**
 * @brief Validate a required, non-empty, bounded key string.
 *
 * @param text Required NUL-terminated string such as a plug_id or method name.
 * @param[out] size Receives the byte length on success.
 * @return abi::v3::ok, invalid_argument for null/empty text, or limit_exceeded.
 */
a::status check_key(const char* text, std::size_t& size) noexcept
{
    if (!text) return a::invalid_argument;
    if (!bounded_length(text, text_limit, size)) return a::limit_exceeded;
    return size == 0 ? a::invalid_argument : a::ok;
}

/**
 * @brief Allocate the next process-wide host token.
 *
 * @param runtime Engine owning the counter.
 * @return A non-zero token, or zero once every value has been handed out.
 * @note The counter is only ever advanced. A wrapped counter reports exhaustion instead
 *       of reusing an old value, so stale credentials can never match a new record.
 */
std::uint64_t take_token(engine& runtime) noexcept
{
    const std::uint64_t value = runtime.next_token;
    if (value == 0) return 0;
    ++runtime.next_token;
    return value;
}

/**
 * @brief Find a live record by exact plugin identity.
 *
 * @param runtime Engine owning the records.
 * @param id Canonical plugin identity.
 * @return The record, or null when no instance carries that identity.
 */
record* find_record(engine& runtime, const std::string& id) noexcept
{
    const auto found = runtime.records.find(id);
    return found == runtime.records.end() ? nullptr : found->second.get();
}

/**
 * @brief Report whether a record pointer still belongs to a live instance.
 *
 * @param runtime Engine owning the records.
 * @param value Candidate pointer, possibly dangling.
 * @return true when the pointer is one of the currently registered records.
 * @note The pointer is compared, never dereferenced, so a lease left behind by a removed
 *       consumer can be recognised without touching freed memory. A reload reuses the
 *       plugin identity but not the record address, so generations stay distinguishable.
 */
bool record_is_live(const engine& runtime, const record* value) noexcept
{
    if (!value) return false;
    for (const auto& entry : runtime.records) {
        if (entry.second.get() == value) return true;
    }
    return false;
}

/**
 * @brief Validate that the calling context may take on a new lease.
 *
 * @param self Calling context (plugin instance or host administration).
 * @return abi::v3::ok when a lease may start, otherwise invalid_state.
 * @note Called after the control-thread check and never off thread.
 */
a::status check_new_work(context& self)
{
    engine& runtime = self.runtime;
    if (runtime.shutting_down)
        return runtime.fail(a::invalid_state, "new work is not allowed while the host is shutting down");
    if (self.owner && self.owner->state != phase::initialized && self.owner->state != phase::active)
        return runtime.fail(a::invalid_state, "only an initialized or active instance may acquire leases");
    return a::ok;
}

/**
 * @brief Resolve and validate the provider of a new lease.
 *
 * @param runtime Engine owning the records.
 * @param plug_id Required provider identity.
 * @param[out] provider Receives the live, active, published provider record.
 * @return abi::v3::ok, invalid_argument/limit_exceeded for the identity, not_found when the
 *         identity is unknown or its capabilities are not published, or invalid_state when
 *         the instance is not active.
 */
a::status resolve_active_provider(engine& runtime, const char* plug_id, record*& provider)
{
    if (!plug_id) return runtime.fail(a::invalid_argument, "a provider plug_id is required");
    std::size_t size = 0;
    const a::status checked = check_key(plug_id, size);
    if (checked != a::ok)
        return runtime.fail(checked, "the provider plug_id must be a bounded non-empty string");
    const std::string id(plug_id, size);
    provider = find_record(runtime, id);
    if (!provider) return runtime.fail(a::not_found, "no such plugin instance: " + id);
    if (!provider->published)
        return runtime.fail(a::not_found, "plugin '" + id + "' has not published capabilities");
    if (provider->state != phase::active)
        return runtime.fail(a::invalid_state, "plugin '" + id + "' is not active");
    if (!provider->instance)
        return runtime.fail(a::invalid_state, "plugin '" + id + "' has no live instance");
    return a::ok;
}

/**
 * @brief Validate that a leased provider still denotes the generation the lease pinned.
 *
 * @param runtime Engine owning the records.
 * @param provider Provider record recorded by the lease; compared before it is dereferenced.
 * @param generation Provider generation recorded by the lease.
 * @param[out] checked Receives the live provider record on success; untouched on failure.
 * @return abi::v3::ok, stale when the instance is gone or its generation moved on, not_found
 *         when it withdrew its capabilities, or invalid_state when it cannot serve calls.
 * @note A lease keeps a record pointer for identity comparison only: the pointer is checked
 *       against the live registry before any field is read, so a lease that outlived its
 *       provider is refused without touching freed memory.
 */
a::status check_leased_provider(engine& runtime, record* provider, std::uint64_t generation,
                                record*& checked)
{
    if (!record_is_live(runtime, provider))
        return runtime.fail(a::stale, "the provider instance is gone; the lease must be re-acquired");
    if (provider->generation != generation)
        return runtime.fail(a::stale, "the provider was reloaded; the lease must be re-acquired");
    if (!provider->published)
        return runtime.fail(a::not_found, "the provider has withdrawn its capabilities");
    if (provider->state != phase::active)
        return runtime.fail(a::invalid_state, "the provider is not active");
    if (!provider->instance)
        return runtime.fail(a::invalid_state, "the provider has no live instance");
    checked = provider;
    return a::ok;
}

/**
 * @brief Reserve one in-flight call slot on a lease, resolved freshly by its credential.
 *
 * @param runtime Engine owning the lease table.
 * @param credential Lease credential copied from the ABI argument before any plugin code ran.
 * @return abi::v3::ok after the counter was raised, or stale when the lease is gone.
 */
a::status begin_active_call(engine& runtime, std::uint64_t credential)
{
    const auto found = runtime.leases.find(credential);
    if (found == runtime.leases.end())
        return runtime.fail(a::stale, "the lease behind this credential is gone; re-acquire it");
    ++found->second.active_calls;
    return a::ok;
}

/**
 * @brief Balance a lease's in-flight counter after the call it covers has finished.
 *
 * @note The counter is released through a fresh credential lookup rather than a retained
 *       iterator, so nothing here can reference an entry that a nested operation erased. The
 *       destructor neither allocates nor throws.
 */
struct active_call_guard {
    engine& runtime;
    std::uint64_t credential;
    bool armed = false;
    active_call_guard(engine& e, std::uint64_t value) noexcept : runtime(e), credential(value) {}
    ~active_call_guard()
    {
        if (!armed) return;
        const auto found = runtime.leases.find(credential);
        if (found != runtime.leases.end() && found->second.active_calls != 0) --found->second.active_calls;
    }
    /** @brief Start balancing on destruction; called once the counter was successfully taken. */
    void arm() noexcept { armed = true; }
    active_call_guard(const active_call_guard&) = delete;
    active_call_guard& operator=(const active_call_guard&) = delete;
};

/**
 * @brief Raise the engine call depth around a host-owned callback that has no plugin record.
 *
 * @note The administration context holds no record to nest under, so it cannot use
 *       call_scope. Raising engine::depth directly still marks a callback as on the stack,
 *       which makes the structural host operations (start, load, unload, shutdown) refuse to
 *       run while a revoker is observing the lease table.
 */
struct runtime_depth_scope {
    engine& runtime;
    explicit runtime_depth_scope(engine& value) noexcept : runtime(value) { ++runtime.depth; }
    ~runtime_depth_scope() { --runtime.depth; }
    runtime_depth_scope(const runtime_depth_scope&) = delete;
    runtime_depth_scope& operator=(const runtime_depth_scope&) = delete;
};

/**
 * @brief Resolve and invoke one published method directly under a live lease credential.
 *
 * @param self Calling context that owns the lease.
 * @param credential Lease credential copied from the ABI argument, zero rejected.
 * @param by_name True for name lookup, false for numeric method-id lookup.
 * @param name Required exact published method name when by_name; ignored otherwise.
 * @param numeric Published provider-local method id when !by_name; ignored otherwise.
 * @param args Borrowed JSON arguments, valid for this call only.
 * @param result Caller-owned writer; required and written at most once on success.
 * @return abi::v3::ok, invalid_argument for a zero credential, a null/empty/oversized name,
 *         another context's lease, or an inconsistent argument view, stale when the lease or
 *         its provider generation is gone, not_found when the provider withdrew its
 *         capabilities or does not publish the requested method, invalid_state when the
 *         caller may not call or the provider cannot serve calls, busy on provider reentry,
 *         limit_exceeded for oversized arguments, wrong_thread, or failed.
 * @note Named and numeric lookup share this implementation, so both apply the same lease,
 *       owner, generation, provider state, consumer state, disclosure, reentry and limit
 *       checks. The provider record is reached only through the lease and only after it was
 *       verified to be live; the announced method id is copied out of the method table
 *       before the callback, and no reference into engine::leases or the capability set
 *       survives the invoke. The lease's in-flight counter is taken before entering plugin
 *       code and covers both the invoke and the delivery of its result, so release reports
 *       busy throughout that window.
 */
a::status direct_call(context& self, std::uint64_t credential, bool by_name, const char* name,
                      a::method_id numeric, a::bytes args, a::iwriter* result)
{
    engine& runtime = self.runtime;
    if (!result) return a::invalid_argument;
    if (!runtime.on_thread()) return a::wrong_thread;
    if (credential == 0)
        return runtime.fail(a::invalid_argument, "call requires a non-zero lease credential");
    std::string wanted;
    if (by_name) {
        std::size_t name_size = 0;
        const a::status checked = check_key(name, name_size);
        if (checked != a::ok)
            return runtime.fail(checked, "the method name must be a bounded non-empty string");
        wanted.assign(name, name_size);
    }
    const auto found = runtime.leases.find(credential);
    if (found == runtime.leases.end())
        return runtime.fail(a::stale, "the lease credential is no longer valid");
    if (found->second.consumer != self.owner)
        return runtime.fail(a::invalid_argument, "the lease credential belongs to another context");
    // Copy the pinned identity before plugin code can run: this frame must never keep a
    // reference into engine::leases across the invocation or the result delivery.
    record* const pinned = found->second.provider;
    const std::uint64_t generation = found->second.generation;
    record* provider = nullptr;
    const a::status resolved = check_leased_provider(runtime, pinned, generation, provider);
    if (resolved != a::ok) return resolved;
    if (self.owner && self.owner->state != phase::active)
        return runtime.fail(a::invalid_state, "only an active instance may call business methods");
    if (runtime.shutting_down)
        return runtime.fail(a::invalid_state, "method calls are not allowed while the host is shutting down");
    if (!args.data && args.size)
        return runtime.fail(a::invalid_argument, "call arguments need a data pointer for a non-zero size");
    if (args.size > runtime.options.payload_limit)
        return runtime.fail(a::limit_exceeded, "call arguments exceed the configured payload limit");
    // Resolution runs before any plugin code and copies the method id out of the disclosure.
    a::method_id method = numeric;
    bool disclosed = false;
    if (by_name) {
        for (const owned_method& candidate : provider->capabilities.methods) {
            if (candidate.name == wanted) {
                method = candidate.id;
                disclosed = true;
                break;
            }
        }
        if (!disclosed)
            return runtime.fail(a::not_found, "the provider does not publish method '" + wanted + "'");
    } else {
        for (const owned_method& candidate : provider->capabilities.methods) {
            if (candidate.id == numeric) {
                disclosed = true;
                break;
            }
        }
        if (!disclosed) return runtime.fail(a::not_found, "the provider does not publish that method id");
    }
    if (!provider->invoker) return runtime.fail(a::failed, "the provider has no invoke interface");
    if (provider->depth != 0) return runtime.fail(a::busy, "the provider is already executing a call");
    const a::status begun = begin_active_call(runtime, credential);
    if (begun != a::ok) return begun;
    active_call_guard in_flight(runtime, credential);
    in_flight.arm();

    a::iinvoke* const invoker = provider->invoker;
    string_writer writer(runtime.options.output_limit);
    a::status invoked = a::failed;
    {
        call_scope scope(runtime, *provider);
        try {
            invoked = invoker->invoke(method, args, &writer);
        } catch (...) {
            return runtime.fail(a::failed, "the provider invocation threw an exception");
        }
    }
    if (invoked != a::ok)
        return runtime.fail(invoked, "the provider invocation failed; the partial output was discarded");
    if (writer.result != a::ok)
        return runtime.fail(writer.result, "the provider output exceeded the configured bounds");
    const a::bytes produced{writer.value.data(), writer.value.size()};
    a::status wrote = a::failed;
    try {
        wrote = result->write(produced);
    } catch (...) {
        return runtime.fail(a::failed, "the result writer threw an exception");
    }
    if (wrote != a::ok) return runtime.fail(wrote, "the result writer rejected the output");
    return a::ok;
}

} // namespace

/**
 * @brief Return the host service interface matching an exact frozen identifier.
 *
 * @param type Requested service identifier; required.
 * @param out Receives the borrowed interface pointer, cleared first; required.
 * @return abi::v3::ok for a known service, unsupported for an unknown identifier,
 *         invalid_argument for a null argument, or failed on an escaping exception.
 * @note The returned pointer is the correct base subobject of this context and stays valid
 *       with the context; this operation reads no mutable host state and touches no plugin.
 */
a::status U42_CALL context::query(const a::iid* type, void** out) noexcept
{
    try {
        if (!out) return a::invalid_argument;
        *out = nullptr;
        if (!type) return a::invalid_argument;
        if (*type == a::events_iid) {
            *out = static_cast<a::ievents*>(this);
            return a::ok;
        }
        if (*type == a::caps_iid) {
            *out = static_cast<a::icaps*>(this);
            return a::ok;
        }
        if (*type == a::calls_iid) {
            *out = static_cast<a::icalls*>(this);
            return a::ok;
        }
        if (*type == a::diag_iid) {
            *out = static_cast<a::idiag*>(this);
            return a::ok;
        }
        return a::unsupported;
    } catch (...) {
        if (out) *out = nullptr;
        return a::failed;
    }
}

/**
 * @brief Submit the complete method set of a starting instance exactly once.
 *
 * @param value Capability description to copy; required.
 * @return abi::v3::ok, invalid_argument for a malformed description, an empty method name, or
 *         a null method table with a non-zero count, limit_exceeded for oversized counts or
 *         text, duplicate for a repeated method id or name, invalid_state outside a first
 *         start() or for the host administration context, wrong_thread, or failed.
 * @note Every string is copied before anything is committed, so a rejected announcement
 *       leaves the instance exactly as it was. Announcing does not publish: only a
 *       successful engine::commit makes the set discoverable. An empty method set is legal
 *       and describes an instance that offers no business method but can still be leased for
 *       its lifetime, where every call returns not_found.
 * @note The announcement carries only method descriptions: the plugin identity and its numeric
 *       plugin_version, taken from the factory descriptor, are the compatibility statement, and
 *       version filtering belongs to the borrower's explicit range.
 */
a::status U42_CALL context::announce(const a::caps_desc* value) noexcept
{
    try {
        if (!runtime.on_thread()) return a::wrong_thread;
        if (!value) return runtime.fail(a::invalid_argument, "announce requires a capability description");
        if (value->struct_size != sizeof(a::caps_desc))
            return runtime.fail(a::invalid_argument,
                                "caps_desc.struct_size does not match this ABI version");
        if (value->method_count > announce_entry_limit)
            return runtime.fail(a::limit_exceeded, "the capability announcement exceeds the entry limit");
        if (value->method_count != 0 && !value->methods)
            return runtime.fail(a::invalid_argument, "caps_desc.methods is null with a non-zero count");
        if (!owner)
            return runtime.fail(a::invalid_state, "the host administration context cannot announce capabilities");
        record& self = *owner;
        if (self.state != phase::starting || self.announced)
            return runtime.fail(a::invalid_state, "capabilities may be announced once during start()");

        std::vector<owned_method> methods;
        methods.reserve(value->method_count);
        for (std::uint32_t index = 0; index < value->method_count; ++index) {
            const a::method_desc& source = value->methods[index];
            owned_method method;
            method.id = source.id;
            a::status copied = copy_text(source.name, method.name);
            if (copied != a::ok)
                return runtime.fail(copied, "an announced method name exceeds the text limit");
            if (method.name.empty())
                return runtime.fail(a::invalid_argument, "an announced method name is empty");
            copied = copy_text(source.description, method.description);
            if (copied != a::ok)
                return runtime.fail(copied, "an announced method description exceeds the text limit");
            copied = copy_text(source.input_schema, method.input_schema);
            if (copied != a::ok)
                return runtime.fail(copied, "an announced input schema exceeds the text limit");
            copied = copy_text(source.output_schema, method.output_schema);
            if (copied != a::ok)
                return runtime.fail(copied, "an announced output schema exceeds the text limit");
            for (const owned_method& known : methods) {
                if (known.id == method.id)
                    return runtime.fail(a::duplicate, "the announcement repeats a method id");
                if (known.name == method.name)
                    return runtime.fail(a::duplicate, "the announcement repeats a method name");
            }
            methods.push_back(std::move(method));
        }
        self.capabilities.methods = std::move(methods);
        self.announced = true;
        return a::ok;
    } catch (...) {
        return a::failed;
    }
}

/**
 * @brief Register a capability sink and queue the current snapshot without a callback.
 *
 * @param sink Receiver owned by the caller until the watch is removed; required.
 * @param out Receives the watch token, cleared first; required.
 * @return abi::v3::ok, invalid_argument for a null sink, invalid_state while shutting down
 *         or after revocation, limit_exceeded when no token remains, wrong_thread, or failed.
 * @note Snapshots for every published active instance are queued, never delivered here, so
 *       engine::drain can respect the batch gate while an initializing consumer is still
 *       being initialized. Each notice copies the version of the instance it reports at the
 *       time it is queued, never a version read from a later instance with the same identity.
 */
a::status U42_CALL context::watch(a::icap_sink* sink, a::token* out) noexcept
{
    try {
        if (!out) return a::invalid_argument;
        *out = a::token{};
        if (!runtime.on_thread()) return a::wrong_thread;
        if (!sink) return runtime.fail(a::invalid_argument, "watch requires a capability sink");
        if (runtime.shutting_down)
            return runtime.fail(a::invalid_state, "watch is not allowed while the host is shutting down");
        if (owner) {
            const phase state = owner->state;
            if (state != phase::initializing && state != phase::initialized &&
                state != phase::starting && state != phase::active)
                return runtime.fail(a::invalid_state, "watch is only allowed before the instance is revoked");
        }
        const std::uint64_t watch_token = take_token(runtime);
        if (watch_token == 0) return runtime.fail(a::limit_exceeded, "no capability watch tokens remain");
        const std::size_t queued = runtime.notices.size();
        try {
            runtime.watches.emplace(watch_token, cap_subscription{owner, sink});
            for (const auto& entry : runtime.records) {
                record& provider = *entry.second;
                if (!provider.published || provider.state != phase::active) continue;
                cap_notice note{};
                note.watch = watch_token;
                note.provider = provider.order.plug_id;
                note.generation = provider.generation;
                note.available = true;
                note.version = provider.version;
                note.capabilities = provider.capabilities;
                runtime.notices.push_back(std::move(note));
            }
        } catch (...) {
            while (runtime.notices.size() > queued) runtime.notices.pop_back();
            runtime.watches.erase(watch_token);
            return runtime.fail(a::failed, "the capability watch could not be registered");
        }
        *out = a::token{watch_token};
        return a::ok;
    } catch (...) {
        if (out) *out = a::token{};
        return a::failed;
    }
}

/**
 * @brief Remove one capability watch owned by the calling context.
 *
 * @param value Watch token returned by watch(); zero is rejected.
 * @return abi::v3::ok, invalid_argument for a zero token or another context's watch,
 *         stale for an unknown token, wrong_thread, or failed.
 * @note Queued notices for the removed token are dropped later by the watch-id check in
 *       engine::drain; this call only unregisters the sink.
 */
a::status U42_CALL context::unwatch(a::token value) noexcept
{
    try {
        if (!runtime.on_thread()) return a::wrong_thread;
        if (value.value == 0)
            return runtime.fail(a::invalid_argument, "unwatch requires a non-zero watch token");
        const auto found = runtime.watches.find(value.value);
        if (found == runtime.watches.end())
            return runtime.fail(a::stale, "the capability watch token is no longer valid");
        if (found->second.owner != owner)
            return runtime.fail(a::invalid_argument, "the capability watch belongs to another context");
        runtime.watches.erase(found);
        return a::ok;
    } catch (...) {
        return a::failed;
    }
}

/**
 * @brief Lease one Active instance whose numeric version lies inside the caller's range.
 *
 * @param plug_id Provider identity; required and non-empty.
 * @param allowed Required inclusive version range; minimum must not exceed maximum.
 * @param receiver Revocation receiver that must be able to return the credential; required.
 * @param out Receives the credential and the actual provider version together, cleared first;
 *            required.
 * @return abi::v3::ok, invalid_argument for a null argument or a reversed range,
 *         invalid_state when the caller is not initialized/active or the provider is not
 *         active, not_found when the identity is unknown or unpublished, unsupported when the
 *         provider version is outside the allowed range, limit_exceeded when no credential
 *         remains, wrong_thread, or failed.
 * @note This records a logical lock only: it reads the version already copied into the
 *       provider record, never calls iplug::query or any other plugin entry point, and
 *       returns no provider address. A failed acquire clears the whole borrow, so success is
 *       always status==ok together with a non-zero credential; the version is the provider's
 *       actual version, not an endpoint of the caller's range.
 */
a::status U42_CALL context::acquire(const char* plug_id, const a::version_range* allowed,
                                    a::irevoker* receiver, a::borrow* out) noexcept
{
    try {
        if (!out) return a::invalid_argument;
        *out = a::borrow{};
        if (!runtime.on_thread()) return a::wrong_thread;
        if (!plug_id || !allowed || !receiver)
            return runtime.fail(a::invalid_argument,
                                "acquire requires a plug_id, an allowed version range and a revocation receiver");
        if (!a::valid_version_range(*allowed))
            return runtime.fail(a::invalid_argument,
                                "the allowed version range must not have its minimum above its maximum");
        const a::status allowed_state = check_new_work(*this);
        if (allowed_state != a::ok) return allowed_state;
        record* provider = nullptr;
        const a::status resolved = resolve_active_provider(runtime, plug_id, provider);
        if (resolved != a::ok) return resolved;
        // Copy the instance version and generation before anything else can run: neither the
        // lease nor this frame may keep a reference into the provider's metadata.
        const a::plugin_version actual = provider->version;
        const std::uint64_t generation = provider->generation;
        if (!a::accepts_version(*allowed, actual))
            return runtime.fail(a::unsupported,
                                "the provider version is outside the range allowed by the consumer");
        const std::uint64_t credential = take_token(runtime);
        if (credential == 0) return runtime.fail(a::limit_exceeded, "no lease credentials remain");
        try {
            runtime.leases.emplace(credential,
                                   lease_record{owner, provider, generation, actual, receiver, 0});
        } catch (...) {
            return runtime.fail(a::failed, "the lease record could not be stored");
        }
        out->credential = a::token{credential};
        out->version = actual;
        return a::ok;
    } catch (...) {
        if (out) *out = a::borrow{};
        return a::failed;
    }
}

/**
 * @brief Return one lease owned by the calling context.
 *
 * @param credential Credential obtained from acquire(); zero is rejected.
 * @return abi::v3::ok, invalid_argument for a zero credential or another context's credential,
 *         stale for a credential that no longer identifies a lease, busy while the lease still
 *         covers a call in flight, wrong_thread, or failed.
 * @note Release stays available in every state, including revocation and shutdown, because
 *       returning a lease is a recovery action. A returned credential is erased, so an old
 *       credential can never be matched against a later record. Nothing is removed when the
 *       call fails: a lease with calls in flight keeps existing until it has delivered their
 *       results. Release removes nothing else: the credential alone authorizes calls, so there is
 *       no separate lookup entry to clean up.
 */
a::status U42_CALL context::release(a::token credential) noexcept
{
    try {
        if (!runtime.on_thread()) return a::wrong_thread;
        if (credential.value == 0)
            return runtime.fail(a::invalid_argument, "release requires a non-zero credential");
        const auto found = runtime.leases.find(credential.value);
        if (found == runtime.leases.end())
            return runtime.fail(a::stale, "the lease credential is no longer valid");
        if (found->second.consumer != owner)
            return runtime.fail(a::invalid_argument, "the lease credential belongs to another context");
        if (found->second.active_calls != 0)
            return runtime.fail(a::busy,
                                "the lease still covers a call in flight; retry release once it returns");
        runtime.leases.erase(found);
        return a::ok;
    } catch (...) {
        return a::failed;
    }
}

/**
 * @brief Invoke one published method by name under a live lease owned by the caller.
 *
 * @param credential Lease credential returned by acquire(); zero is rejected.
 * @param name Published method name; required and non-empty.
 * @param args Borrowed JSON arguments, valid for this call only.
 * @param result Caller-owned writer; required and written at most once on success.
 * @return The shared direct-call statuses: ok, invalid_argument for a zero credential, a
 *         null/empty/oversized name or another context's lease, stale when the lease or its
 *         provider generation is gone, not_found when the method is not published,
 *         invalid_state when the caller may not call, busy on provider reentry,
 *         limit_exceeded for oversized arguments, wrong_thread, or failed.
 * @note Name and numeric lookup share one implementation, so both reach the provider through
 *       the same validated generation-checked path.
 */
a::status U42_CALL context::call_name(a::token credential, const char* name, a::bytes args,
                                      a::iwriter* result) noexcept
{
    try {
        return direct_call(*this, credential.value, true, name, 0, args, result);
    } catch (...) {
        return a::failed;
    }
}

/**
 * @brief Invoke one published numeric method id under a live lease owned by the caller.
 *
 * @param credential Lease credential returned by acquire(); zero is rejected.
 * @param method Published provider-local method id.
 * @param args Borrowed JSON arguments, valid for this call only.
 * @param result Caller-owned writer; required and written at most once on success.
 * @return The same lease, ownership, generation, disclosure, reentry and limit statuses as
 *         the named form.
 */
a::status U42_CALL context::call_id(a::token credential, a::method_id method, a::bytes args,
                                    a::iwriter* result) noexcept
{
    try {
        return direct_call(*this, credential.value, false, nullptr, method, args, result);
    } catch (...) {
        return a::failed;
    }
}

/**
 * @brief Emit one diagnostic line on the control thread.
 *
 * @param message Optional NUL-terminated diagnostic; null is ignored.
 * @note Never throws, never retains the input and never allocates. Off-thread or oversized
 *       diagnostics are dropped rather than serialised, so a plugin worker thread cannot
 *       interleave output with the control thread.
 */
void U42_CALL context::log(const char* message) noexcept
{
    try {
        if (!message) return;
        if (!runtime.on_thread()) return;
        std::size_t size = 0;
        if (!bounded_length(message, text_limit, size)) size = text_limit;
        if (size) (void)std::fwrite(message, 1, size, stderr);
        (void)std::fputc('\n', stderr);
    } catch (...) {
        // Diagnostics must never propagate failures into plugin code.
    }
}

/**
 * @brief Publish the announced method set and queue one notice per capability watch.
 *
 * @param target Instance whose start() just succeeded; the lifecycle owner has already moved
 *               it to active.
 * @return abi::v3::ok, not_found/invalid_state when the instance may not publish, busy when the
 *         instance is re-entered, wrong_thread, or failed.
 * @note An instance that announces methods must answer query(invoke_iid); an instance with an
 *       empty set publishes without any plugin call and still notifies watchers. The method
 *       descriptions are copied host-side, and query(invoke_iid) is the only plugin hook this
 *       step ever calls. Notices are queued only - no sink is called here - and every notice
 *       copies the target's version at queue time; queuing is rolled back on failure, so the
 *       instance is never published without its watchers being able to learn about it.
 */
a::status engine::commit(record& target)
{
    try {
        if (!on_thread()) return a::wrong_thread;
        if (shutting_down) return fail(a::invalid_state, "capabilities cannot be committed during shutdown");
        if (target.state != phase::active && target.state != phase::starting)
            return fail(a::invalid_state, "only a starting or active instance may publish capabilities");
        if (target.published) return fail(a::invalid_state, "the instance already published capabilities");

        a::iinvoke* invoker = nullptr;
        if (!target.capabilities.methods.empty()) {
            if (target.instance == nullptr)
                return fail(a::invalid_state, "the instance has no live instance to query");
            if (target.depth != 0)
                return fail(a::busy, "the instance is already executing a call");
            void* raw = nullptr;
            a::status queried = a::failed;
            {
                call_scope scope(*this, target);
                try {
                    queried = target.instance->query(&a::invoke_iid, &raw);
                } catch (...) {
                    return fail(a::failed, "the invoke interface query threw an exception");
                }
            }
            if (queried != a::ok)
                return fail(queried, "an instance that announces methods must provide the invoke interface");
            if (!raw)
                return fail(a::failed, "the instance returned a null invoke interface");
            invoker = static_cast<a::iinvoke*>(raw);
        }

        const std::size_t queued = notices.size();
        try {
            for (const auto& entry : watches) {
                cap_notice note{};
                note.watch = entry.first;
                note.provider = target.order.plug_id;
                note.generation = target.generation;
                note.available = true;
                note.version = target.version;
                note.capabilities = target.capabilities;
                notices.push_back(std::move(note));
            }
        } catch (...) {
            while (notices.size() > queued) notices.pop_back();
            return fail(a::failed, "the capability notices could not be queued");
        }
        target.invoker = invoker;
        target.published = true;
        return a::ok;
    } catch (...) {
        return a::failed;
    }
}

/**
 * @brief Withdraw an instance from the available capability table and notify watchers.
 *
 * @param target Instance leaving the active state.
 * @return abi::v3::ok when the instance is withdrawn or was already withdrawn, wrong_thread off
 *         the control thread, or failed when the withdrawal notices could not all be queued.
 * @note The operation is transactional: it either queues exactly one withdrawal notice per live
 *       watch and then leaves the instance unpublished, or it leaves both the instance and the
 *       notice queue untouched and reports failed. Callers must therefore treat failed as "still
 *       published": destroying or unmapping the instance would drop a withdrawal that watchers
 *       never observed. No sink is called here and a repeated withdrawal queues nothing, so
 *       watchers see exactly one withdrawal per commit. Each notice copies the version of the
 *       instance being withdrawn at queue time. A wrong-thread or failed call records no
 *       diagnostic, because reporting would itself have to allocate; the failure path only
 *       shrinks the queue and must not throw a second time.
 */
a::status engine::withdraw(record& target)
{
    if (!on_thread()) return a::wrong_thread;
    if (!target.published) return a::ok;
    const std::size_t queued = notices.size();
    try {
        for (const auto& entry : watches) {
            cap_notice note{};
            note.watch = entry.first;
            note.provider = target.order.plug_id;
            note.generation = target.generation;
            note.available = false;
            note.version = target.version;
            note.capabilities = target.capabilities;
            notices.push_back(std::move(note));
        }
    } catch (...) {
        // Roll back to the pre-call length and leave published untouched. pop_back() only
        // destroys, so this recovery path cannot allocate or throw a second time.
        while (notices.size() > queued) notices.pop_back();
        return a::failed;
    }
    target.published = false;
    return a::ok;
}

/**
 * @brief Notify and collect every incoming lease held from one provider instance.
 *
 * @param target Provider instance being revoked.
 * @return abi::v3::ok when every lease is gone, busy while a consumer keeps a credential,
 *         cannot be called back or still has a call in flight, wrong_thread, or failed on an
 *         escaping exception.
 * @note Credentials are snapshotted first because one callback may return several leases and
 *       therefore invalidate every iterator into engine::leases; each credential is
 *       re-resolved before and after its callback. A consumer that is currently executing is
 *       left for a later attempt, and a lease whose consumer record no longer exists is
 *       dropped instead of dereferenced. No lease a live consumer still owns is ever erased
 *       here: only that consumer can return it, so an uncooperative consumer keeps the
 *       provider pinned instead of having its lease fabricated away.
 * @note A callback for the administration context has no record to nest under, so it runs with
 *       engine::depth raised directly. That still keeps a revoker from running a structural
 *       host operation (start, load, unload, shutdown) against the lease table it observes.
 */
a::status engine::revoke(record& target)
{
    try {
        if (!on_thread()) return a::wrong_thread;
        std::vector<std::uint64_t> pending;
        for (const auto& entry : leases) {
            if (entry.second.provider == &target) pending.push_back(entry.first);
        }
        bool any_busy = false;
        bool any_failed = false;
        for (const std::uint64_t credential : pending) {
            const auto found = leases.find(credential);
            if (found == leases.end()) continue; // already returned by an earlier callback
            if (found->second.provider != &target) continue; // defensive: no longer ours
            record* consumer = found->second.consumer;
            a::irevoker* receiver = found->second.receiver;
            const bool live = record_is_live(*this, consumer);
            if (consumer && !live) {
                // The owning instance no longer exists, so nothing can use or return this
                // credential. A removed consumer drops its outgoing leases with it, which makes
                // this branch a defensive case; it never applies to a live consumer.
                leases.erase(found);
                continue;
            }
            if (!receiver) {
                // No one can be asked to return the credential. Pin the provider instead of
                // pretending the lease was returned; the owning context may still release it.
                any_busy = true;
                continue;
            }
            if (found->second.active_calls != 0) {
                // The consumer is inside a call that is still delivering its result; calling back
                // into it now would be a callback inside a callback. Retry once it returns.
                any_busy = true;
                continue;
            }
            if (live && consumer->depth != 0) {
                any_busy = true;
                continue;
            }
            bool threw = false;
            if (live) {
                call_scope scope(*this, *consumer);
                try {
                    receiver->on_revoke(a::token{credential});
                } catch (...) {
                    threw = true;
                }
            } else {
                runtime_depth_scope scope(*this);
                try {
                    receiver->on_revoke(a::token{credential});
                } catch (...) {
                    threw = true;
                }
            }
            if (threw) {
                any_failed = true;
                continue;
            }
            if (leases.find(credential) != leases.end()) any_busy = true;
        }
        if (any_failed) return a::failed;
        return any_busy ? a::busy : a::ok;
    } catch (...) {
        return a::failed;
    }
}

/**
 * @brief Remove every registration owned by one instance after it stopped or failed to start.
 *
 * @param target Instance whose registrations must disappear.
 * @note Only outgoing registrations are removed: event subscriptions, watches and the lease
 *       credentials this instance holds as a consumer. Incoming leases are deliberately kept so
 *       the consumers that hold them can still return their credentials during revocation.
 *       Pending notices carry only the watch id, the provider identity and the version copied
 *       at queue time, never a record pointer, so an already queued capability notice cannot
 *       keep this instance alive and is dropped by engine::drain once its watch id is gone.
 * @note The lifecycle owner calls this only after a successful stop(); a consumer whose stop()
 *       failed keeps its outgoing leases, which is what pins the providers it borrowed from
 *       until the operator resolves the quarantine.
 */
void engine::remove_owner(record& target)
{
    try {
        if (!on_thread()) {
            error = "owner cleanup must run on the host control thread";
            return;
        }
        for (auto entry = subscriptions.begin(); entry != subscriptions.end();) {
            if (entry->second.owner == &target)
                entry = subscriptions.erase(entry);
            else
                ++entry;
        }
        for (auto entry = watches.begin(); entry != watches.end();) {
            if (entry->second.owner == &target)
                entry = watches.erase(entry);
            else
                ++entry;
        }
        for (auto entry = leases.begin(); entry != leases.end();) {
            if (entry->second.consumer == &target)
                entry = leases.erase(entry);
            else
                ++entry;
        }
    } catch (...) {
        error = "the owner registrations could not be removed";
    }
}

} // namespace u42::detail
