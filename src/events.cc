/**
 * @file events.cc
 * @brief Queued exact-name event service and capability-notification dispatch.
 *
 * This unit owns the service side of the event ABI: per-context subscription
 * bookkeeping, non-recursive event enqueueing, and the bounded drain that turns
 * queued work into plugin callbacks. Every entry point runs on the control
 * thread; dispatch never nests a plugin callback and never interrupts itself.
 * Queued work is copied on publish, so a notification outlives the caller's
 * borrowed views and stays valid until its own callback returns.
 */
#include "internal.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace u42::detail {
namespace {

/**
 * @brief Record a diagnostic without letting an allocation failure escape noexcept code.
 *
 * @param runtime Engine receiving the diagnostic.
 * @param value Status reported to the caller.
 * @param message Statically allocated explanation.
 * @return value, so callers can write `return fail_quietly(...)`.
 * @note The status alone is the contract; a lost diagnostic must not terminate the process.
 */
a::status fail_quietly(engine& runtime, a::status value, const char* message) noexcept
{
    try {
        runtime.error = message;
    } catch (...) {
        // Best effort only.
    }
    return value;
}

/**
 * @brief Test whether an event name uses the host-reserved lifecycle prefix.
 *
 * @param name Non-null, NUL-terminated candidate name.
 * @return True for names beginning with "u42.".
 */
bool reserved_name(const char* name) noexcept
{
    return std::strncmp(name, "u42.", 4) == 0;
}

/**
 * @brief Test whether a context owner may still be used.
 *
 * @param runtime Engine holding the registry.
 * @param owner Context owner; null means the host administration context.
 * @return True for null (the host context lives as long as the engine) or for a pointer
 *         that is still present in the record registry.
 * @note Only pointer identity is compared, so a removed and freed record can be tested
 *       without dereferencing it.
 */
bool owner_reachable(const engine& runtime, const record* owner) noexcept
{
    if (!owner) return true;
    for (const auto& entry : runtime.records)
        if (entry.second.get() == owner) return true;
    return false;
}

/**
 * @brief Test whether an instance state may still create event subscriptions.
 *
 * @param value Current instance phase.
 * @return True while the instance can grow host registrations.
 */
bool state_allows_subscribe(phase value) noexcept
{
    switch (value) {
    case phase::initializing:
    case phase::initialized:
    case phase::starting:
    case phase::active:
        return true;
    default:
        return false;
    }
}

/**
 * @brief Test whether an instance may receive a queued callback.
 *
 * @param value Current instance phase.
 * @return True for initialized and active instances; an initialized consumer may record
 *         facts from a notification but must not start business calls yet.
 */
bool state_receives_callback(phase value) noexcept
{
    return value == phase::initialized || value == phase::active;
}

/**
 * @brief Marks one dispatch pass, so a nested drain is rejected instead of recursing.
 */
struct drain_guard {
    engine& runtime;

    /**
     * @brief Enter a dispatch pass.
     * @param value Engine whose draining flag is raised.
     */
    explicit drain_guard(engine& value) noexcept : runtime(value) { runtime.draining = true; }

    /** @brief Leave the dispatch pass, including on an exception path. */
    ~drain_guard() { runtime.draining = false; }

    drain_guard(const drain_guard&) = delete;
    drain_guard& operator=(const drain_guard&) = delete;
};

/**
 * @brief Deliver one capability notice, carrying the provider version and its method table.
 *
 * @param runtime Engine owning the watch and record registries.
 * @param notice Dequeued notice, moved out of the queue by the caller so that the
 *               callback cannot invalidate the data it borrows.
 * @note A missing watch, a removed or ineligible owner, and a stale "available" notice
 *       are dropped silently: the queue is a best-effort channel, not a promise that the
 *       observation still matters. Withdrawals are delivered even when the provider is gone.
 */
void deliver_notice(engine& runtime, cap_notice& notice) noexcept
{
    record* consumer = nullptr;
    a::icap_sink* sink = nullptr;
    {
        const auto found = runtime.watches.find(notice.watch);
        if (found == runtime.watches.end()) return;
        consumer = found->second.owner;
        sink = found->second.sink;
    }
    if (!sink || !owner_reachable(runtime, consumer)) return;
    if (consumer && !state_receives_callback(consumer->state)) return;

    if (notice.available) {
        // Report availability only for the exact instance that is currently serving.
        const auto provider = runtime.records.find(notice.provider);
        if (provider == runtime.records.end()) return;
        const record& target = *provider->second;
        if (target.generation != notice.generation || !target.published ||
            target.state != phase::active)
            return;
    }

    std::vector<a::method_desc> methods;
    try {
        methods.reserve(notice.capabilities.methods.size());
        for (const owned_method& method : notice.capabilities.methods) {
            a::method_desc row{};
            row.id = method.id;
            row.name = method.name.c_str();
            row.description = method.description.c_str();
            row.input_schema = method.input_schema.c_str();
            row.output_schema = method.output_schema.c_str();
            methods.push_back(row);
        }
    } catch (...) {
        return;
    }

    // Both views borrow the notice and this array; they expire when the callback returns. The
    // version is a plain value copied out of the notice, which captured the version of the
    // generation that queued it, so only the method strings stay borrowed from the notice while
    // the callback runs.
    a::caps_desc caps{};
    caps.struct_size = sizeof(a::caps_desc);
    caps.method_count = static_cast<std::uint32_t>(methods.size());
    caps.methods = methods.empty() ? nullptr : methods.data();

    a::cap_event value{};
    value.plug_id = notice.provider.c_str();
    value.available = notice.available ? 1u : 0u;
    value.version = notice.version;
    value.capabilities = caps;

    if (runtime.depth != 0) return;
    if (!consumer) {
        sink->on_capability(&value);
        return;
    }
    // Re-check after the views are built: no iterator or record reference is kept across
    // a callback, and a running callback must not be re-entered.
    if (consumer->depth != 0) return;
    if (!state_receives_callback(consumer->state)) return;
    call_scope scope(runtime, *consumer);
    sink->on_capability(&value);
}

/**
 * @brief Deliver one queued event to every matching subscription.
 *
 * @param runtime Engine owning the subscription registry.
 * @param entry Dequeued event, moved out of the queue so that the payload stays valid for
 *              every callback in this pass and cannot be invalidated by new publishes.
 * @note The subscription list is snapshotted by token, then re-looked-up per delivery, so a
 *       callback may freely subscribe or unsubscribe without invalidating this pass.
 */
void deliver_event(engine& runtime, queued_event& entry) noexcept
{
    std::vector<std::uint64_t> tokens;
    try {
        tokens.reserve(runtime.subscriptions.size());
        for (const auto& item : runtime.subscriptions) tokens.push_back(item.first);
    } catch (...) {
        return;
    }

    a::event value{};
    value.name = entry.name.c_str();
    value.payload.data =
        entry.payload.empty() ? nullptr : static_cast<const void*>(entry.payload.data());
    value.payload.size = entry.payload.size();

    for (const std::uint64_t id : tokens) {
        const auto found = runtime.subscriptions.find(id);
        if (found == runtime.subscriptions.end()) continue;
        if (found->second.name != entry.name) continue;
        record* const consumer = found->second.owner;
        a::ievent_sink* const sink = found->second.sink;
        if (!sink || !owner_reachable(runtime, consumer)) continue;
        if (consumer && !state_receives_callback(consumer->state)) continue;
        if (runtime.depth != 0) continue;
        if (!consumer) {
            sink->on_event(&value);
            continue;
        }
        // Re-check immediately before the call; a previous callback may have removed the
        // instance, which makes this registration stale rather than a dangling call.
        if (consumer->depth != 0 || !state_receives_callback(consumer->state)) continue;
        call_scope scope(runtime, *consumer);
        sink->on_event(&value);
    }
}

/**
 * @brief Run the unload requests that were deferred while dispatch was in progress.
 *
 * @param runtime Engine whose deferred queue is consumed.
 * @return ok, or the first unload failure; a non-ok result is restored to the queue if storage
 *         permits. Allocation exceptions during unloading or requeueing may lose the request;
 *         drain() translates them to failed and the caller must explicitly retry unload().
 * @note Callers must have left the drain scope first, because unload_one() may itself
 *       drain to deliver withdrawal notifications.
 * @note Only the requests present on entry are attempted, so a request that re-defers
 *       itself cannot make this loop unbounded.
 */
a::status drain_deferred(engine& runtime)
{
    const std::size_t bound = runtime.deferred_unloads.size();
    for (std::size_t i = 0; i < bound; ++i) {
        if (runtime.deferred_unloads.empty()) break;
        std::string plug_id = std::move(runtime.deferred_unloads.front());
        runtime.deferred_unloads.pop_front();
        const a::status result = runtime.unload_one(plug_id);
        if (result != a::ok) {
            runtime.deferred_unloads.push_front(std::move(plug_id));
            return result;
        }
    }
    return a::ok;
}

} // namespace

/**
 * @brief Subscribe an owned sink to one exact event name.
 *
 * @param name Non-null, non-empty exact event name. Names are compared literally; wildcards
 *             and waterfall semantics are not provided.
 * @param sink Non-null receiver owned by the subscribing context until removal.
 * @param out Non-null credential slot; cleared before any validation, and on failure.
 * @return ok on success, invalid_argument for null or empty arguments, invalid_state when the
 *         engine is shutting down or the owner is gone or not in a state that may subscribe,
 *         limit_exceeded when the token space is exhausted, duplicate on an impossible token
 *         collision, failed when the registration cannot be stored, or wrong_thread.
 */
a::status U42_CALL context::subscribe(const char* name, a::ievent_sink* sink, a::token* out) noexcept
{
    if (out) *out = a::token{};
    if (!runtime.on_thread()) return a::wrong_thread;
    if (!out) return fail_quietly(runtime, a::invalid_argument, "events.subscribe: out is null");
    if (!name || !*name)
        return fail_quietly(runtime, a::invalid_argument, "events.subscribe: name is empty");
    if (!sink) return fail_quietly(runtime, a::invalid_argument, "events.subscribe: sink is null");
    if (runtime.shutting_down)
        return fail_quietly(runtime, a::invalid_state,
                            "events.subscribe: the host is shutting down");
    if (owner) {
        if (!owner_reachable(runtime, owner))
            return fail_quietly(runtime, a::invalid_state,
                                "events.subscribe: the calling context is no longer valid");
        if (!state_allows_subscribe(owner->state))
            return fail_quietly(runtime, a::invalid_state,
                                "events.subscribe: the instance state forbids subscriptions");
    }

    const std::uint64_t id = runtime.next_token;
    if (id == 0)
        return fail_quietly(runtime, a::limit_exceeded,
                            "events.subscribe: the token space is exhausted");
    try {
        event_subscription entry{};
        entry.owner = owner;
        entry.sink = sink;
        entry.name = name;
        if (!runtime.subscriptions.emplace(id, std::move(entry)).second)
            return fail_quietly(runtime, a::duplicate,
                                "events.subscribe: the token is already in use");
    } catch (...) {
        return fail_quietly(runtime, a::failed,
                            "events.subscribe: the subscription cannot be stored");
    }
    // Tokens are never reused; a wrap to zero marks permanent exhaustion for later calls.
    runtime.next_token = id + 1;
    *out = a::token{id};
    return a::ok;
}

/**
 * @brief Remove one subscription owned by the calling context.
 *
 * @param value Credential returned by subscribe(); zero is invalid.
 * @return ok on success, invalid_argument for a zero token, stale for an unknown token or one
 *         owned by another context, or wrong_thread.
 */
a::status U42_CALL context::unsubscribe(a::token value) noexcept
{
    if (!runtime.on_thread()) return a::wrong_thread;
    if (value.value == 0)
        return fail_quietly(runtime, a::invalid_argument, "events.unsubscribe: zero token");
    const auto found = runtime.subscriptions.find(value.value);
    if (found == runtime.subscriptions.end())
        return fail_quietly(runtime, a::stale, "events.unsubscribe: unknown subscription");
    if (found->second.owner != owner)
        return fail_quietly(runtime, a::stale,
                            "events.unsubscribe: the subscription belongs to another context");
    runtime.subscriptions.erase(found);
    return a::ok;
}

/**
 * @brief Copy an event into the queue without dispatching any callback.
 *
 * @param name Non-null, non-empty event name outside the host-reserved "u42." prefix.
 * @param data Borrowed payload; the host copies it before returning. A null pointer requires
 *             a zero size.
 * @return ok when enqueued, invalid_argument for an empty or reserved name or a null payload
 *         with a non-zero size, invalid_state when the owner is not an active instance,
 *         limit_exceeded when the payload or the queue exceeds its limit, failed when the copy
 *         cannot be stored, or wrong_thread.
 */
a::status U42_CALL context::publish(const char* name, a::bytes data) noexcept
{
    if (!runtime.on_thread()) return a::wrong_thread;
    if (!name || !*name)
        return fail_quietly(runtime, a::invalid_argument, "events.publish: name is empty");
    if (reserved_name(name))
        return fail_quietly(runtime, a::invalid_argument,
                            "events.publish: u42.* names are reserved for the host");
    if (!data.data && data.size != 0)
        return fail_quietly(runtime, a::invalid_argument,
                            "events.publish: a null payload must have a zero size");
    if (owner) {
        if (!owner_reachable(runtime, owner))
            return fail_quietly(runtime, a::invalid_state,
                                "events.publish: the calling context is no longer valid");
        if (owner->state != phase::active)
            return fail_quietly(runtime, a::invalid_state,
                                "events.publish: only an active instance may publish");
    }
    if (data.size > runtime.options.payload_limit)
        return fail_quietly(runtime, a::limit_exceeded,
                            "events.publish: the payload exceeds the configured limit");
    if (runtime.events.size() >= runtime.options.event_capacity)
        return fail_quietly(runtime, a::limit_exceeded, "events.publish: the event queue is full");
    try {
        queued_event entry;
        entry.name = name;
        if (data.size != 0)
            entry.payload.assign(static_cast<const char*>(data.data),
                                 static_cast<std::size_t>(data.size));
        runtime.events.push_back(std::move(entry));
    } catch (...) {
        return fail_quietly(runtime, a::failed, "events.publish: the event cannot be queued");
    }
    return a::ok;
}

/**
 * @brief Deliver queued capability notices and events with a bounded budget.
 *
 * @return ok when there was nothing to deliver or nothing remains; wrong_thread off the
 *         control thread; busy when a plugin call is on the stack or a drain is already
 *         running; limit_exceeded when work remains after the dispatch budget; failed when
 *         queued unload handling fails or for an unexpected internal error.
 * @note Notices are always preferred over events, and each dequeued notice or event consumes
 *       one unit of host_options::dispatch_budget. Undelivered work stays queued.
 * @note An initialization batch never dispatches: callbacks only start after the batch
 *       completed. The encompassing start batch dispatches notices but holds deferred unloads
 *       until a later safe point after start returns. Otherwise deferred unloads run after the
 *       dispatch scope ends, so unload_one() may drain with the recursion guard released.
 */
a::status engine::drain()
{
    try {
        if (!on_thread()) return a::wrong_thread;
        if (depth != 0)
            return fail_quietly(*this, a::busy, "events.drain: a plugin call is on the stack");
        if (draining) return fail_quietly(*this, a::busy, "events.drain: drain is already running");
        if (initializing_batch) return a::ok;

        a::status result = a::ok;
        {
            drain_guard guard(*this);
            std::size_t budget = options.dispatch_budget;
            while (budget != 0) {
                if (!notices.empty()) {
                    cap_notice notice = std::move(notices.front());
                    notices.pop_front();
                    --budget;
                    deliver_notice(*this, notice);
                    continue;
                }
                if (!events.empty()) {
                    queued_event entry = std::move(events.front());
                    events.pop_front();
                    --budget;
                    deliver_event(*this, entry);
                    continue;
                }
                break;
            }
            if (!notices.empty() || !events.empty())
                result = fail_quietly(*this, a::limit_exceeded,
                                      "events.drain: the dispatch budget is exhausted");
        }
        if (result != a::ok) return result;
        if (starting_batch) return a::ok; // Unloads remain queued; notices are already delivered.
        return drain_deferred(*this);
    } catch (...) {
        return fail_quietly(*this, a::failed, "events.drain: unexpected exception");
    }
}

} // namespace u42::detail
