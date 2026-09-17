/**
 * @file context.cc
 * @brief Context services and capability bookkeeping for the single-threaded host.
 *
 * This translation unit owns every host service reachable through one plugin context
 * except the event queue itself: interface lookup, capability announcement, capability
 * watching, tracked interface borrowing, dynamic method binding and invocation, plus the
 * engine transitions that publish, withdraw, revoke and clean up what those services
 * recorded. Event subscription, publishing and engine::drain belong to src/events.cc;
 * lifecycle orchestration belongs to src/host.cc.
 *
 * Contract implemented here:
 * - Every ABI entry point is noexcept and maps any escaping exception to abi::v1::failed.
 * - Required outputs are cleared before any other validation; mutating entry points
 *   validate the control thread before reading or writing any other shared state. The
 *   control-thread identity is immutable after engine construction, so reading it off
 *   thread cannot race.
 * - A context whose owner is null is the host administration context: it holds business
 *   authority (watch, acquire, bind, call) but may never announce capabilities.
 * - A plugin instance may only acquire or bind while initialized or active, and may only
 *   call business methods while active; recovery actions (release, unwatch, unbind) stay
 *   available in every state, including revocation and shutdown.
 * - No plugin callback is ever invoked inline from a mutating host operation. Capability
 *   changes are queued in engine::notices and delivered by engine::drain, which validates
 *   the recorded watch id before dispatch; pending notices therefore never retain a
 *   pointer to an owner that may have been removed.
 * - Tokens are drawn from one monotonic counter, are never reused and never wrap into a
 *   valid value, so a stale credential can never match a later record.
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
 * @return abi::v1::ok, invalid_argument, or limit_exceeded for oversized text.
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
 * @return abi::v1::ok, invalid_argument for null/empty text, or limit_exceeded.
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
 * @brief Validate that the calling context may take on new work.
 *
 * @param self Calling context (plugin instance or host administration).
 * @return abi::v1::ok when a borrow or binding may start, otherwise invalid_state.
 * @note Called after the control-thread check and never off thread.
 */
a::status check_new_work(context& self)
{
    engine& runtime = self.runtime;
    if (runtime.shutting_down)
        return runtime.fail(a::invalid_state, "new work is not allowed while the host is shutting down");
    if (self.owner && self.owner->state != phase::initialized && self.owner->state != phase::active)
        return runtime.fail(a::invalid_state,
                           "only an initialized or active instance may acquire interfaces or bind methods");
    return a::ok;
}

/**
 * @brief Resolve and validate the provider of a new borrow or binding.
 *
 * @param runtime Engine owning the records.
 * @param plug_id Required provider identity.
 * @param[out] provider Receives the live, active, published provider record.
 * @return abi::v1::ok, invalid_argument/limit_exceeded for the identity, not_found when the
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
 * @brief Store one generation-checked binding for the calling context.
 *
 * @param self Calling context that will own the binding.
 * @param provider Resolved active provider.
 * @param method Announced method being bound.
 * @param[out] out Receives the opaque binding value on success.
 * @return abi::v1::ok, limit_exceeded when no token remains, or failed on allocation failure.
 * @note The binding keeps the provider identity and generation, never a provider pointer, so
 *       it cannot keep an instance alive and cannot silently follow a reload.
 */
a::status store_binding(context& self, record& provider, const owned_method& method, a::binding* out)
{
    engine& runtime = self.runtime;
    const std::uint64_t slot = take_token(runtime);
    if (slot == 0) return runtime.fail(a::limit_exceeded, "no method binding tokens remain");
    try {
        bound_method entry;
        entry.consumer = self.owner;
        entry.provider = provider.order.plug_id;
        entry.generation = provider.generation;
        entry.method = method.id;
        runtime.bindings.emplace(slot, std::move(entry));
    } catch (...) {
        return runtime.fail(a::failed, "the method binding could not be stored");
    }
    *out = a::binding{slot};
    return a::ok;
}

} // namespace

/**
 * @brief Return the host service interface matching an exact frozen identifier.
 *
 * @param type Requested service identifier; required.
 * @param out Receives the borrowed interface pointer, cleared first; required.
 * @return abi::v1::ok for a known service, unsupported for an unknown identifier,
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
 * @brief Submit the complete capability set of a starting instance exactly once.
 *
 * @param value Capability description to copy; required.
 * @return abi::v1::ok, invalid_argument for a malformed description or empty method name,
 *         limit_exceeded for oversized counts or text, duplicate for a repeated interface
 *         id or a repeated method id/name, invalid_state outside a first start() or for the
 *         host administration context, wrong_thread, or failed.
 * @note Every string is copied before anything is committed, so a rejected announcement
 *       leaves the instance exactly as it was. Announcing does not publish: only a
 *       successful engine::commit makes the set discoverable.
 */
a::status U42_CALL context::announce(const a::caps_desc* value) noexcept
{
    try {
        if (!runtime.on_thread()) return a::wrong_thread;
        if (!value) return runtime.fail(a::invalid_argument, "announce requires a capability description");
        if (value->struct_size != sizeof(a::caps_desc))
            return runtime.fail(a::invalid_argument,
                                "caps_desc.struct_size does not match this ABI version");
        if (value->interface_count > announce_entry_limit || value->method_count > announce_entry_limit)
            return runtime.fail(a::limit_exceeded, "the capability announcement exceeds the entry limit");
        if (value->interface_count != 0 && !value->interfaces)
            return runtime.fail(a::invalid_argument, "caps_desc.interfaces is null with a non-zero count");
        if (value->method_count != 0 && !value->methods)
            return runtime.fail(a::invalid_argument, "caps_desc.methods is null with a non-zero count");
        if (!owner)
            return runtime.fail(a::invalid_state, "the host administration context cannot announce capabilities");
        record& self = *owner;
        if (self.state != phase::starting || self.announced)
            return runtime.fail(a::invalid_state, "capabilities may be announced once during start()");

        capability_set next;
        next.interfaces.reserve(value->interface_count);
        for (std::uint32_t index = 0; index < value->interface_count; ++index) {
            const a::iid candidate = value->interfaces[index];
            for (const a::iid& known : next.interfaces) {
                if (known == candidate)
                    return runtime.fail(a::duplicate, "the announcement repeats an interface id");
            }
            next.interfaces.push_back(candidate);
        }
        next.methods.reserve(value->method_count);
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
            for (const owned_method& known : next.methods) {
                if (known.id == method.id)
                    return runtime.fail(a::duplicate, "the announcement repeats a method id");
                if (known.name == method.name)
                    return runtime.fail(a::duplicate, "the announcement repeats a method name");
            }
            next.methods.push_back(std::move(method));
        }
        self.capabilities = std::move(next);
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
 * @return abi::v1::ok, invalid_argument for a null sink, invalid_state while shutting down
 *         or after revocation, limit_exceeded when no token remains, wrong_thread, or failed.
 * @note Snapshots for every published active instance are queued, never delivered here, so
 *       engine::drain can respect the batch gate while an initializing consumer is still
 *       being initialized.
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
 * @return abi::v1::ok, invalid_argument for a zero token or another context's watch,
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
 * @brief Borrow an advertised interface from the current active provider instance.
 *
 * @param plug_id Provider identity; required and non-empty.
 * @param type Exact interface identifier to borrow; required.
 * @param receiver Revocation receiver that must be able to return the credential; required.
 * @param out Receives the interface pointer and credential, cleared first; required.
 * @return abi::v1::ok, invalid_argument for a null argument, invalid_state when the caller is
 *         not initialized/active or the provider is not active, not_found when the identity is
 *         unknown, unpublished or does not advertise type, busy when the provider is already
 *         executing, limit_exceeded when no credential remains, wrong_thread, or failed.
 * @note The interface pointer comes from the provider's own query, so it is stable for the
 *       lifetime of the borrow. A lease is recorded only for a successful non-null result.
 */
a::status U42_CALL context::acquire(const char* plug_id, const a::iid* type, a::irevoker* receiver,
                                   a::borrow* out) noexcept
{
    try {
        if (!out) return a::invalid_argument;
        *out = a::borrow{};
        if (!runtime.on_thread()) return a::wrong_thread;
        if (!plug_id || !type || !receiver)
            return runtime.fail(a::invalid_argument,
                                "acquire requires a plug_id, an iid and a revocation receiver");
        const a::status allowed = check_new_work(*this);
        if (allowed != a::ok) return allowed;
        record* provider = nullptr;
        const a::status resolved = resolve_active_provider(runtime, plug_id, provider);
        if (resolved != a::ok) return resolved;
        if (std::find(provider->capabilities.interfaces.begin(), provider->capabilities.interfaces.end(),
                      *type) == provider->capabilities.interfaces.end())
            return runtime.fail(a::not_found, "the provider does not advertise the requested interface");
        if (provider->depth != 0)
            return runtime.fail(a::busy, "the provider is already executing a call");

        void* raw = nullptr;
        a::status queried = a::failed;
        {
            call_scope scope(runtime, *provider);
            try {
                queried = provider->instance->query(type, &raw);
            } catch (...) {
                return runtime.fail(a::failed, "the provider interface query threw an exception");
            }
        }
        if (queried != a::ok)
            return runtime.fail(queried, "the provider does not offer the requested interface");
        if (!raw)
            return runtime.fail(a::failed, "the provider returned a null interface pointer");

        const std::uint64_t credential = take_token(runtime);
        if (credential == 0) return runtime.fail(a::limit_exceeded, "no borrowing credentials remain");
        try {
            runtime.leases.emplace(credential,
                                   lease_record{owner, provider, provider->generation, *type, raw, receiver});
        } catch (...) {
            return runtime.fail(a::failed, "the borrowing record could not be stored");
        }
        out->ptr = raw;
        out->credential = a::token{credential};
        return a::ok;
    } catch (...) {
        if (out) *out = a::borrow{};
        return a::failed;
    }
}

/**
 * @brief Return one credential owned by the calling context.
 *
 * @param credential Credential obtained from acquire(); zero is rejected.
 * @return abi::v1::ok, invalid_argument for a zero credential or another context's credential,
 *         stale for a credential that no longer identifies a lease, wrong_thread, or failed.
 * @note Release stays available in every state, including revocation and shutdown, because
 *       returning a borrow is a recovery action. A returned credential is erased, so an old
 *       value can never be matched against a later record.
 */
a::status U42_CALL context::release(a::token credential) noexcept
{
    try {
        if (!runtime.on_thread()) return a::wrong_thread;
        if (credential.value == 0)
            return runtime.fail(a::invalid_argument, "release requires a non-zero credential");
        const auto found = runtime.leases.find(credential.value);
        if (found == runtime.leases.end())
            return runtime.fail(a::stale, "the borrowing credential is no longer valid");
        if (found->second.consumer != owner)
            return runtime.fail(a::invalid_argument, "the borrowing credential belongs to another context");
        runtime.leases.erase(found);
        return a::ok;
    } catch (...) {
        return a::failed;
    }
}

/**
 * @brief Bind the current method of an active provider by its published name.
 *
 * @param plug_id Provider identity; required and non-empty.
 * @param name Announced method name; required and non-empty.
 * @param out Receives the opaque binding, cleared first; required.
 * @return abi::v1::ok, invalid_argument for a null or empty key, invalid_state when the caller
 *         is not initialized/active or the provider is not active, not_found when the identity
 *         is unknown, unpublished or does not announce name, limit_exceeded when no token
 *         remains, wrong_thread, or failed.
 */
a::status U42_CALL context::bind_name(const char* plug_id, const char* name, a::binding* out) noexcept
{
    try {
        if (!out) return a::invalid_argument;
        *out = a::binding{};
        if (!runtime.on_thread()) return a::wrong_thread;
        if (!plug_id || !name)
            return runtime.fail(a::invalid_argument, "bind_name requires a plug_id and a method name");
        const a::status allowed = check_new_work(*this);
        if (allowed != a::ok) return allowed;
        std::size_t name_size = 0;
        const a::status checked = check_key(name, name_size);
        if (checked != a::ok)
            return runtime.fail(checked, "the method name must be a bounded non-empty string");
        const std::string wanted(name, name_size);
        record* provider = nullptr;
        const a::status resolved = resolve_active_provider(runtime, plug_id, provider);
        if (resolved != a::ok) return resolved;
        const owned_method* method = nullptr;
        for (const owned_method& candidate : provider->capabilities.methods) {
            if (candidate.name == wanted) {
                method = &candidate;
                break;
            }
        }
        if (!method)
            return runtime.fail(a::not_found, "the provider does not announce method '" + wanted + "'");
        return store_binding(*this, *provider, *method, out);
    } catch (...) {
        if (out) *out = a::binding{};
        return a::failed;
    }
}

/**
 * @brief Bind the current method of an active provider by its published numeric id.
 *
 * @param plug_id Provider identity; required and non-empty.
 * @param id Announced method id.
 * @param out Receives the opaque binding, cleared first; required.
 * @return abi::v1::ok, invalid_argument for a null key, invalid_state when the caller is not
 *         initialized/active or the provider is not active, not_found when the identity is
 *         unknown, unpublished or does not announce id, limit_exceeded when no token remains,
 *         wrong_thread, or failed.
 */
a::status U42_CALL context::bind_id(const char* plug_id, a::method_id id, a::binding* out) noexcept
{
    try {
        if (!out) return a::invalid_argument;
        *out = a::binding{};
        if (!runtime.on_thread()) return a::wrong_thread;
        const a::status allowed = check_new_work(*this);
        if (allowed != a::ok) return allowed;
        record* provider = nullptr;
        const a::status resolved = resolve_active_provider(runtime, plug_id, provider);
        if (resolved != a::ok) return resolved;
        const owned_method* method = nullptr;
        for (const owned_method& candidate : provider->capabilities.methods) {
            if (candidate.id == id) {
                method = &candidate;
                break;
            }
        }
        if (!method)
            return runtime.fail(a::not_found, "the provider does not announce that method id");
        return store_binding(*this, *provider, *method, out);
    } catch (...) {
        if (out) *out = a::binding{};
        return a::failed;
    }
}

/**
 * @brief Invoke one bound method and hand the complete output to the caller's writer.
 *
 * @param target Binding obtained from bind_name()/bind_id(); zero is rejected.
 * @param args Borrowed JSON arguments, valid for this call only.
 * @param result Caller-owned writer; required and written at most once on success.
 * @return abi::v1::ok, invalid_argument for a zero binding, a null writer, an inconsistent
 *         argument view or another context's binding, stale for an unknown binding, a missing
 *         provider instance or a provider generation change, not_found when the provider
 *         withdrew its capabilities, invalid_state when the caller is not active or the host
 *         is shutting down, busy on provider reentry, limit_exceeded when the arguments or the
 *         output exceed the configured bounds, wrong_thread, or failed.
 * @note The binding is validated before the caller's own state, so a binding whose provider
 *       generation is gone reports stale even after the provider was unloaded or the whole
 *       host shut down. Output is buffered host-side: a failing invocation, or a writer that
 *       reports a sticky failure, discards the partial result instead of forwarding it, so the
 *       external writer receives at most one complete, bounded payload.
 */
a::status U42_CALL context::call(a::binding target, a::bytes args, a::iwriter* result) noexcept
{
    try {
        if (!result) return a::invalid_argument;
        if (!runtime.on_thread()) return a::wrong_thread;
        if (target.value == 0)
            return runtime.fail(a::invalid_argument, "call requires a binding obtained from bind_name/bind_id");
        const auto found = runtime.bindings.find(target.value);
        if (found == runtime.bindings.end())
            return runtime.fail(a::stale, "the method binding is no longer valid");
        if (found->second.consumer != owner)
            return runtime.fail(a::invalid_argument, "the method binding belongs to another context");
        const bound_method& binding = found->second;
        // The binding is resolved before the caller's own state: once the provider generation is
        // gone the binding can never be used again, and that stays stale even while the host is
        // winding down or the caller has already stopped.
        record* provider = find_record(runtime, binding.provider);
        if (!provider)
            return runtime.fail(a::stale, "the provider instance is gone; the binding must be recreated");
        if (provider->generation != binding.generation)
            return runtime.fail(a::stale, "the provider was reloaded; the binding must be recreated");
        if (owner && owner->state != phase::active)
            return runtime.fail(a::invalid_state, "only an active instance may call business methods");
        if (runtime.shutting_down)
            return runtime.fail(a::invalid_state, "method calls are not allowed while the host is shutting down");
        if (!args.data && args.size)
            return runtime.fail(a::invalid_argument, "call arguments need a data pointer for a non-zero size");
        if (args.size > runtime.options.payload_limit)
            return runtime.fail(a::limit_exceeded, "call arguments exceed the configured payload limit");
        if (!provider->published)
            return runtime.fail(a::not_found, "the provider has withdrawn its capabilities");
        if (provider->state != phase::active)
            return runtime.fail(a::invalid_state, "the provider is not active");
        if (!provider->instance || !provider->invoker)
            return runtime.fail(a::failed, "the provider has no invoke interface");
        if (provider->depth != 0)
            return runtime.fail(a::busy, "the provider is already executing a call");

        const a::method_id method = binding.method;
        string_writer writer(runtime.options.output_limit);
        a::status invoked = a::failed;
        {
            call_scope scope(runtime, *provider);
            try {
                invoked = provider->invoker->invoke(method, args, &writer);
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
    } catch (...) {
        return a::failed;
    }
}

/**
 * @brief Release one method binding owned by the calling context.
 *
 * @param target Binding to release; zero is rejected.
 * @return abi::v1::ok, invalid_argument for a zero value or another context's binding,
 *         stale for an unknown binding, wrong_thread, or failed.
 * @note Binding never kept the provider alive, so unbinding does not affect the provider.
 *       Unbind stays available in every state, including revocation and shutdown.
 */
a::status U42_CALL context::unbind(a::binding target) noexcept
{
    try {
        if (!runtime.on_thread()) return a::wrong_thread;
        if (target.value == 0)
            return runtime.fail(a::invalid_argument, "unbind requires a non-zero binding");
        const auto found = runtime.bindings.find(target.value);
        if (found == runtime.bindings.end())
            return runtime.fail(a::stale, "the method binding is no longer valid");
        if (found->second.consumer != owner)
            return runtime.fail(a::invalid_argument, "the method binding belongs to another context");
        runtime.bindings.erase(found);
        return a::ok;
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
 * @brief Publish the announced capability set and queue one notice per capability watch.
 *
 * @param target Instance whose start() just succeeded; the lifecycle owner has already moved
 *               it to active.
 * @return abi::v1::ok, not_found/invalid_state when the instance may not publish, busy when the
 *         instance is re-entered, wrong_thread, or failed.
 * @note An instance that announces methods must answer query(invoke_iid); an instance with an
 *       empty set publishes without any plugin call and still notifies watchers. Notices are
 *       queued only - no sink is called here - and are rolled back if queuing fails, so the
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
 * @return abi::v1::ok when the instance is withdrawn or was already withdrawn, wrong_thread off
 *         the control thread, or failed when the withdrawal notices could not all be queued.
 * @note The operation is transactional: it either queues exactly one withdrawal notice per live
 *       watch and then leaves the instance unpublished, or it leaves both the instance and the
 *       notice queue untouched and reports failed. Callers must therefore treat failed as "still
 *       published": destroying or unmapping the instance would drop a withdrawal that watchers
 *       never observed. No sink is called here and a repeated withdrawal queues nothing, so
 *       watchers see exactly one withdrawal per commit. A wrong-thread or failed call records no
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
 * @brief Notify and collect every incoming borrow held from one provider instance.
 *
 * @param target Provider instance being revoked.
 * @return abi::v1::ok when every lease is gone, busy while a consumer keeps a credential or
 *         cannot be called back, wrong_thread, or failed on an escaping exception.
 * @note Credentials are snapshotted first because one callback may return several leases and
 *       therefore invalidate every iterator into engine::leases; each credential is
 *       re-resolved before and after its callback. A consumer that is currently executing is
 *       left for a later attempt, and a lease whose consumer record no longer exists is
 *       dropped instead of dereferenced.
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
                // The owning instance is gone; nothing can use or return the pointer.
                leases.erase(found);
                continue;
            }
            if (!receiver) {
                leases.erase(found);
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
 * @note Only outgoing registrations are removed: subscriptions, watches, leases this instance
 *       holds as a consumer and its method bindings. Incoming leases are deliberately kept so
 *       the consumers that hold them can still return their credentials through the revocation
 *       protocol. Pending notices carry only the watch id and provider identity, never a record
 *       pointer, so an already queued capability notice cannot keep this instance alive and is
 *       dropped by engine::drain once its watch id is gone.
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
        for (auto entry = bindings.begin(); entry != bindings.end();) {
            if (entry->second.consumer == &target)
                entry = bindings.erase(entry);
            else
                ++entry;
        }
    } catch (...) {
        error = "the owner registrations could not be removed";
    }
}

} // namespace u42::detail
