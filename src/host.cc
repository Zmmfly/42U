/**
 * @file host.cc
 * @brief Lifecycle owner for the plugin rack: staging, two-phase start, unload and shutdown.
 *
 * The engine is deliberately single-threaded. Each mutating entry point proves it runs on the
 * control thread before it may touch the shared diagnostic string, so a rejected call from
 * another thread returns wrong_thread without racing the error text.
 *
 * Teardown never destroys a plugin object whose silence has not been proven: capabilities are
 * withdrawn, inbound credentials are revoked and stop() has to succeed before destroy() runs,
 * and the library mapping only disappears after that. A record that cannot reach that point is
 * quarantined in the engine with all of its resources instead of being forced apart.
 */
#include "internal.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <set>
#include <utility>

namespace u42::detail {
namespace {

/** @brief Descriptor string limit; longer values are rejected instead of being scanned further. */
constexpr std::size_t max_string_bytes = 64u * 1024u;
/** @brief Upper bound accepted for one before/after constraint array. */
constexpr std::uint32_t max_constraint_entries = 4096u;
/** @brief Bounded dispatch attempts used when the host needs queued lifecycle work flushed. */
constexpr std::size_t drain_rounds = 4;

/**
 * @brief Render an ABI status as a stable diagnostic word.
 *
 * @param value ABI status code.
 * @return Lowercase status name, or the decimal value for unknown codes.
 */
std::string status_text(a::status value)
{
    switch (value) {
        case a::ok: return "ok";
        case a::invalid_argument: return "invalid_argument";
        case a::unsupported: return "unsupported";
        case a::not_found: return "not_found";
        case a::duplicate: return "duplicate";
        case a::invalid_state: return "invalid_state";
        case a::busy: return "busy";
        case a::stale: return "stale";
        case a::limit_exceeded: return "limit_exceeded";
        case a::failed: return "failed";
        case a::wrong_thread: return "wrong_thread";
        case a::cycle: return "cycle";
        case a::deferred: return "deferred";
        default: return std::to_string(value);
    }
}

/**
 * @brief Strictly validate a UTF-8 byte range.
 *
 * Rejects truncated sequences, stray continuation bytes, overlong encodings, surrogate halves
 * and code points above U+10FFFF, so a malformed identity cannot reach the ordering planner or
 * the diagnostic stream.
 *
 * @param data First byte of the candidate text.
 * @param size Number of bytes to validate.
 * @return true when the range is well-formed UTF-8.
 */
bool valid_utf8(const char* data, std::size_t size)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    std::size_t index = 0;
    while (index < size) {
        const unsigned char lead = bytes[index];
        if (lead < 0x80u) {
            ++index;
            continue;
        }

        std::size_t extra = 0;
        std::uint32_t code_point = 0;
        if ((lead & 0xE0u) == 0xC0u) {
            extra = 1;
            code_point = lead & 0x1Fu;
            if (code_point < 2u) return false; // Overlong two-byte form.
        } else if ((lead & 0xF0u) == 0xE0u) {
            extra = 2;
            code_point = lead & 0x0Fu;
        } else if ((lead & 0xF8u) == 0xF0u) {
            extra = 3;
            code_point = lead & 0x07u;
            if (code_point > 4u) return false; // Beyond U+10FFFF.
        } else {
            return false; // Continuation byte or an illegal 5/6-byte lead.
        }

        if (index + extra >= size) return false; // Truncated sequence.
        for (std::size_t offset = 1; offset <= extra; ++offset) {
            const unsigned char trail = bytes[index + offset];
            if ((trail & 0xC0u) != 0x80u) return false;
            code_point = (code_point << 6) | (trail & 0x3Fu);
        }
        if (extra == 2 && (code_point < 0x800u || (code_point >= 0xD800u && code_point <= 0xDFFFu)))
            return false;
        if (extra == 3 && (code_point < 0x10000u || code_point > 0x10FFFFu)) return false;
        index += extra + 1;
    }
    return true;
}

/**
 * @brief Copy one NUL-terminated descriptor string after bounded length and UTF-8 checks.
 *
 * @param text Borrowed descriptor string; may be null only for optional fields.
 * @param required Whether the field must be present and non-empty.
 * @param field Field name used in diagnostics.
 * @param out Receives the copied value on success.
 * @param message Receives a human-readable diagnostic on failure.
 * @return ok on success, otherwise invalid_argument.
 */
a::status read_descriptor_string(const char* text, bool required, const std::string& field,
                                std::string& out, std::string& message)
{
    if (text == nullptr) {
        if (!required) {
            out.clear();
            return a::ok;
        }
        message = field + " must not be null";
        return a::invalid_argument;
    }

    const void* terminator = std::memchr(text, '\0', max_string_bytes + 1);
    if (terminator == nullptr) {
        message = field + " is not NUL-terminated within " + std::to_string(max_string_bytes) + " bytes";
        return a::invalid_argument;
    }
    const std::size_t size = static_cast<std::size_t>(static_cast<const char*>(terminator) - text);
    if (size == 0) {
        if (!required) {
            out.clear();
            return a::ok;
        }
        message = field + " must not be empty";
        return a::invalid_argument;
    }
    if (!valid_utf8(text, size)) {
        message = field + " is not valid UTF-8";
        return a::invalid_argument;
    }
    out.assign(text, size);
    return a::ok;
}

/**
 * @brief Validate and copy one before/after constraint array.
 *
 * A null array pointer is only accepted with a zero count, and every element has to be a
 * non-empty bounded UTF-8 string. Duplicate edges are left untouched: plan_order() owns edge
 * deduplication and self-reference/cycle reporting, so the host rejects them at the same
 * well-defined point regardless of how the plugin was staged.
 *
 * @param items Borrowed element array; may be null for zero count.
 * @param count Declared element count.
 * @param field Field name used in diagnostics.
 * @param out Receives the copied constraints; cleared on failure.
 * @param message Receives a human-readable diagnostic on failure.
 * @return ok on success, otherwise invalid_argument or limit_exceeded.
 */
a::status read_constraints(const char* const* items, std::uint32_t count, const std::string& field,
                           std::vector<std::string>& out, std::string& message)
{
    out.clear();
    if (count == 0) return a::ok; // A non-null pointer with a zero count carries no data and is tolerated.
    if (items == nullptr) {
        message = field + " has a null array pointer with count " + std::to_string(count);
        return a::invalid_argument;
    }
    if (count > max_constraint_entries) {
        message = field + " declares " + std::to_string(count) + " entries, exceeding the limit of " +
                  std::to_string(max_constraint_entries);
        return a::limit_exceeded;
    }

    out.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        std::string value;
        const a::status status =
            read_descriptor_string(items[index], true, field + "[" + std::to_string(index) + "]", value, message);
        if (status != a::ok) return status;
        out.push_back(std::move(value));
    }
    return a::ok;
}

/**
 * @brief Raise the host call depth around a plugin entry point that has no record yet.
 *
 * stage() has to call describe()/create() before the owning record exists, so call_scope cannot
 * be used; the depth counter still has to report that a plugin stack is in flight.
 */
struct depth_guard {
    engine& runtime;
    explicit depth_guard(engine& value) : runtime(value) { ++runtime.depth; }
    ~depth_guard() { --runtime.depth; }
    depth_guard(const depth_guard&) = delete;
    depth_guard& operator=(const depth_guard&) = delete;
};

/** @brief Drop one identity from the recorded successful initialization order. */
void forget_order(engine& e, const std::string& id)
{
    e.init_order.erase(std::remove(e.init_order.begin(), e.init_order.end(), id), e.init_order.end());
}

/**
 * @brief Report whether any borrowing record still points at this instance.
 *
 * A credential and a revocation receiver may still reach the plugin code, so the instance and
 * its library must stay mapped while one is live.
 */
bool has_lease_references(const engine& e, const record& rec)
{
    for (const auto& entry : e.leases) {
        if (entry.second.provider == &rec || entry.second.consumer == &rec) return true;
    }
    return false;
}

/**
 * @brief Find a live record by exact plugin identity.
 *
 * @param e Engine owning the records.
 * @param id Canonical plugin identity.
 * @return The record, or null when no instance carries that identity.
 * @note Every teardown step re-resolves its target through this lookup instead of holding a
 *       record reference across a plugin call: a drain inside teardown can run the deferred
 *       unload queue and release an instance that the caller still believed to be live.
 */
record* lookup_record(engine& e, const std::string& id) noexcept
{
    auto found = e.records.find(id);
    return found == e.records.end() ? nullptr : found->second.get();
}

/**
 * @brief Identities currently inside retire() on this thread, keyed by engine.
 *
 * The engine is single-threaded, so a thread-local stack is enough and no extra engine field is
 * needed. Tracking the in-flight retirement is what keeps drain-triggered unload requests from
 * re-entering the same teardown: the nested attempt is refused instead of stopping, unregistering
 * and destroying an instance the outer frame is still using.
 */
thread_local std::vector<std::pair<const engine*, std::string>> t_retiring;

/**
 * @brief Report whether this thread is already retiring one identity.
 *
 * @param e Engine being inspected.
 * @param id Candidate identity.
 * @return true when an enclosing retire() frame owns the same (engine, identity) pair.
 */
bool retire_in_flight(const engine& e, const std::string& id) noexcept
{
    for (const auto& entry : t_retiring) {
        if (entry.first == &e && entry.second == id) return true;
    }
    return false;
}

/**
 * @brief Scope marker that publishes one in-flight retirement for the duration of retire().
 */
struct retire_marker {
    engine& runtime;
    std::string id;
    retire_marker(engine& value, const std::string& plug_id) : runtime(value), id(plug_id)
    {
        t_retiring.emplace_back(&runtime, id);
    }
    ~retire_marker()
    {
        // Remove exactly this entry: a nested retirement for another engine may sit above it.
        for (auto it = t_retiring.end(); it != t_retiring.begin();) {
            --it;
            if (it->first == &runtime && it->second == id) {
                t_retiring.erase(it);
                return;
            }
        }
    }
    retire_marker(const retire_marker&) = delete;
    retire_marker& operator=(const retire_marker&) = delete;
};

/**
 * @brief Drop queued unload requests whose identity no longer exists.
 *
 * The deferred queue is consumed by the dispatcher on every drain, and the dispatcher restores a
 * failing request to the front. A request for an already released record would therefore keep
 * every later drain reporting not_found, so the host prunes those entries before it dispatches.
 */
void prune_deferred(engine& e) noexcept
{
    for (auto it = e.deferred_unloads.begin(); it != e.deferred_unloads.end();) {
        if (lookup_record(e, *it) == nullptr) it = e.deferred_unloads.erase(it);
        else ++it;
    }
}

/**
 * @brief Reject a request with a fixed diagnostic without ever throwing.
 *
 * The rejection paths run while a plugin stack is live, so they must not turn a defined refusal
 * into an escaping bad_alloc. When the message cannot be stored the status is still returned and
 * host::error() merely keeps its previous text.
 *
 * @param e Engine owning the diagnostic.
 * @param value Status to return.
 * @param message Static message; never allocated.
 * @return value unchanged.
 */
a::status refuse(engine& e, a::status value, const char* message) noexcept
{
    try {
        e.error = message;
    } catch (...) {
        // Reporting is best effort; the refusal itself is what the caller must observe.
    }
    return value;
}

/**
 * @brief Reject an unload request for an unknown identity without ever throwing.
 *
 * @param e Engine owning the diagnostic.
 * @param id Requested identity; used only inside a guarded allocation.
 * @return a::not_found.
 */
a::status refuse_unknown(engine& e, const std::string& id) noexcept
{
    try {
        e.error = "unknown plugin '" + id + "'";
    } catch (...) {
    }
    return a::not_found;
}

/**
 * @brief Call the plugin-side destructor exactly once, inside a call scope.
 *
 * @note The instance pointer is cleared first so a reentrant failure cannot destroy it twice.
 */
void destroy_instance(engine& e, record& rec)
{
    if (rec.instance == nullptr) return;
    a::iplug* instance = rec.instance;
    rec.instance = nullptr;
    call_scope scope(e, rec);
    instance->destroy();
}

/**
 * @brief Drop a record from the registry.
 *
 * @warning The caller's record reference dangles afterwards; the library mapping is released
 *          here, which is only legal once the instance has been destroyed.
 */
void erase_record(engine& e, record& rec)
{
    const std::string id = rec.order.plug_id;
    forget_order(e, id);
    // A queued unload request names an identity, not an instance, so it must not outlive the
    // record it was queued for: once the identity is gone the dispatcher would restore the entry
    // forever, and a later instance reusing that identity would be unloaded by a stale request.
    // Dropping every entry for this id here (the single place records disappear) covers unload,
    // rollback and the rejected-creation path alike.
    e.deferred_unloads.erase(std::remove(e.deferred_unloads.begin(), e.deferred_unloads.end(), id),
                             e.deferred_unloads.end());
    e.records.erase(id);
}

/**
 * @brief Destroy a record that was never initialized or has already rolled back its init().
 *
 * Plugin-side destroy() is the only required step: such an instance has no started work, and the
 * ABI requires a failed init() to leave a directly destroyable, silent object. Registrations are
 * dropped first because a partially initialized instance may already have subscribed.
 *
 * @note The target is addressed by identity and re-resolved after destroy(), so a plugin whose
 *       destroy() re-enters the host cannot leave this helper erasing a freed record.
 */
void discard_record(engine& e, const std::string& id)
{
    record* rec = lookup_record(e, id);
    if (rec == nullptr) return;
    e.remove_owner(*rec);
    destroy_instance(e, *rec);
    rec = lookup_record(e, id);
    if (rec == nullptr) return;
    rec->state = phase::stopped;
    erase_record(e, *rec);
}

/**
 * @brief Report whether a dispatcher status means "work remains queued" instead of "delivery failed".
 *
 * The event owner returns busy (a plugin call is on the stack or a drain is already running),
 * deferred, limit_exceeded (the bounded dispatch budget ran out) and not_found (the dispatcher
 * restored a queued unload whose identity is already gone) while every undelivered notice and
 * event stays in its queue. Those statuses are retryable, so the host must not abort a start
 * batch or refuse to destroy a stopped instance because of them; a hard failure, by contrast,
 * proves that the notifications could not be handed over at all.
 *
 * @param value Status returned by engine::drain().
 * @return true when dispatching may simply be retried later.
 */
bool soft_dispatch_status(a::status value) noexcept
{
    return value == a::ok || value == a::busy || value == a::deferred || value == a::limit_exceeded ||
           value == a::not_found;
}

/**
 * @brief Give the dispatcher a few bounded chances to flush queued lifecycle work.
 *
 * A soft status (busy/deferred/limit_exceeded/not_found) means the dispatcher keeps work for a
 * later poll() call, so it is not a delivery failure. Only a hard status proves notifications
 * could not be handed to their consumers, which is why callers abort teardown on it.
 *
 * @return ok when the queue converged, the last soft status when work remains queued, or the
 *         hard failure that stopped dispatching.
 */
a::status drain_bounded(engine& e, std::size_t rounds = drain_rounds)
{
    a::status last = a::ok;
    for (std::size_t round = 0; round < rounds; ++round) {
        const a::status current = e.drain();
        if (current == a::ok) return a::ok;
        last = current;
        if (!soft_dispatch_status(current)) return current;
    }
    return last;
}

/**
 * @brief Raise engine::initializing_batch for exactly one scope.
 *
 * The flag gates the dispatcher, so a path that forgot to lower it would silently stop every
 * later batch from receiving callbacks. Owning it in a destructor makes a return, a failed
 * init() and an escaping allocation all equally safe.
 */
struct batch_guard {
    engine& runtime;
    explicit batch_guard(engine& value) noexcept : runtime(value) { runtime.initializing_batch = true; }
    ~batch_guard() { runtime.initializing_batch = false; }
    batch_guard(const batch_guard&) = delete;
    batch_guard& operator=(const batch_guard&) = delete;
};

/**
 * @brief Hold deferred unloads across planning, initialization, startup and failure rollback.
 *
 * Capability notices still dispatch between starts. Queued structural requests must not remove
 * a planned peer or an established provider until this operation has returned to its caller.
 */
struct startup_guard {
    engine& runtime;
    explicit startup_guard(engine& value) noexcept : runtime(value) { runtime.starting_batch = true; }
    ~startup_guard() { runtime.starting_batch = false; }
    startup_guard(const startup_guard&) = delete;
    startup_guard& operator=(const startup_guard&) = delete;
};

/**
 * @brief Shared teardown tail: withdraw capabilities, prove inbound silence, stop, unregister, destroy.
 *
 * @param id Identity of the instance being retired; never a record reference, because the drain
 *           performed here can run the deferred unload queue and release this very record.
 * @param active true when the instance may hold published capabilities and inbound borrowings.
 * @param reason Receives a diagnostic whenever the instance has to be quarantined.
 * @return ok once the record was fully destroyed, or when a nested path already released it; any
 *         other status means the record was pinned with its resources (revoking for an unfinished
 *         revoke, faulted for a failed stop, busy while another teardown of it is in flight).
 *
 * @note stop() is attempted at most once per record: a failed stop marks the record faulted and
 *       later unload attempts report it instead of retrying, so a plugin that refuses to quiesce
 *       is never called again behind the operator's back.
 * @note Re-entry for the same identity is refused before any plugin call, so a queued unload
 *       request cannot double-stop or destroy an instance an outer frame is still using.
 */
a::status retire(engine& e, const std::string& id, bool active, std::string& reason)
{
    if (retire_in_flight(e, id)) {
        reason = "plugin '" + id + "' is already being retired on this thread";
        return a::busy;
    }
    retire_marker marker(e, id);

    // A queued request for this identity is satisfied by this very retirement; dropping it also
    // keeps the drains below from re-entering unload_one for the record they are tearing down.
    e.deferred_unloads.erase(
        std::remove(e.deferred_unloads.begin(), e.deferred_unloads.end(), id),
        e.deferred_unloads.end());

    record* rec = lookup_record(e, id);
    if (rec == nullptr) {
        reason.clear();
        return a::ok;
    }
    rec->state = phase::revoking;

    if (active) {
        const a::status withdrawn = e.withdraw(*rec);
        if (withdrawn != a::ok) {
            reason = "capability withdrawal for '" + id + "' could not be queued";
            return withdrawn;
        }
        prune_deferred(e);
        const a::status drained = drain_bounded(e);
        if (!soft_dispatch_status(drained)) {
            reason = "capability withdrawal notices for '" + id + "' could not be delivered (" +
                     status_text(drained) + ")";
            return drained;
        }
        rec = lookup_record(e, id);
        if (rec == nullptr) {
            reason.clear(); // Released by a consumer callback during the drain: nothing left to do.
            return a::ok;
        }
        const a::status revoked = e.revoke(*rec);
        if (revoked != a::ok) {
            reason = "borrowings of '" + id + "' are not returned (" + status_text(revoked) + ")";
            return revoked;
        }
        rec = lookup_record(e, id);
        if (rec == nullptr) {
            reason.clear();
            return a::ok;
        }
    }

    a::iplug* instance = rec->instance;
    a::status stopped = a::ok;
    if (instance != nullptr) {
        call_scope scope(e, *rec);
        stopped = instance->stop();
    }
    if (stopped != a::ok) {
        record* still = lookup_record(e, id);
        if (still == nullptr) {
            reason.clear(); // Already released; pinning it again would be meaningless.
            return a::ok;
        }
        still->state = phase::faulted;
        reason = "stop() of '" + id + "' failed (" + status_text(stopped) + "); resources are quarantined";
        return stopped;
    }

    rec = lookup_record(e, id);
    if (rec == nullptr) {
        reason.clear();
        return a::ok;
    }
    e.remove_owner(*rec);
    rec = lookup_record(e, id);
    if (rec == nullptr) {
        reason.clear();
        return a::ok;
    }
    if (has_lease_references(e, *rec)) {
        rec->state = phase::revoking;
        reason = "borrowings of '" + id + "' are still outstanding";
        return a::busy;
    }

    destroy_instance(e, *rec);
    rec = lookup_record(e, id);
    if (rec == nullptr) {
        reason.clear();
        return a::ok;
    }
    erase_record(e, *rec); // Erases the record this frame resolved, never a freed reference.
    return a::ok;
}

/**
 * @brief Discard every staged-but-never-initialized record.
 *
 * Used when planning fails: the whole pending batch is abandoned and re-staged by the operator,
 * while already initialized or active instances are left untouched.
 *
 * @note Best effort and never throwing: this runs on failure paths (including under memory
 *       pressure), so an interrupted pass may leave some records staged, which a later shutdown
 *       still retires or quarantines normally.
 */
void rollback_created(engine& e) noexcept
{
    try {
        std::vector<std::string> doomed;
        for (const auto& entry : e.records) {
            if (entry.second->state == phase::created) doomed.push_back(entry.first);
        }
        for (auto it = doomed.rbegin(); it != doomed.rend(); ++it) discard_record(e, *it);
    } catch (...) {
        // Records left behind stay in the registry with a real state, never half-erased.
    }
}

/**
 * @brief Roll back only the records of the current start batch, in reverse acting order.
 *
 * The action depends on the state the attempt left behind: a failed init() is destroyed
 * silently, an initialized or start-failed instance is stopped before destruction, and an
 * instance that already published capabilities is withdrawn and revoked first.
 *
 * @param pending Batch identities in planning-input order.
 * @param plan Planner output, used in reverse as the rollback order.
 * @param diagnostics Receives a quarantine report when a record could not be released.
 * @return ok when the batch was fully released, otherwise busy with a populated diagnostic.
 */
a::status rollback_pending(engine& e, const std::vector<std::string>& pending,
                           const std::vector<std::size_t>& plan, std::string& diagnostics)
{
    std::vector<std::string> pinned;
    for (auto step = plan.rbegin(); step != plan.rend(); ++step) {
        if (*step >= pending.size()) continue;
        const std::string& id = pending[*step];
        auto found = e.records.find(id);
        if (found == e.records.end()) continue;
        // The state is copied rather than referenced: retire() may drain, and a drain can release
        // this record through the deferred queue before the switch below has finished.
        const phase state = found->second->state;

        std::string reason;
        a::status outcome = a::ok;
        switch (state) {
            case phase::created:
            case phase::initializing: // init() already failed: silent direct destroy, stop() is forbidden.
                discard_record(e, id);
                break;
            case phase::initialized:
            case phase::starting: // start() failed and the contract returns the instance to Initialized.
                outcome = retire(e, id, false, reason);
                break;
            case phase::active:
                outcome = retire(e, id, true, reason);
                break;
            default: // revoking/faulted/stopped were already pinned by an earlier attempt.
                break;
        }
        if (outcome != a::ok) {
            pinned.push_back("'" + id + "' (" + (reason.empty() ? status_text(outcome) : reason) + ")");
        }
    }

    if (pinned.empty()) return a::ok;
    std::string message = "rollback left quarantined resources: ";
    for (std::size_t index = 0; index < pinned.size(); ++index) {
        message += pinned[index];
        if (index + 1 != pinned.size()) message += ", ";
    }
    diagnostics = message;
    return a::busy;
}

/**
 * @brief Drop the staged records a failed boot batch added, leaving prior identities alone.
 *
 * @param known Identities that existed before the batch; anything else that still has a loaded
 *              library and was never initialized belongs to this batch.
 * @note Best effort and never throwing, for the same reason as rollback_created().
 */
void rollback_boot_batch(engine& e, const std::set<std::string>& known) noexcept
{
    try {
        std::vector<std::string> doomed;
        for (const auto& entry : e.records) {
            if (entry.second->state != phase::created) continue; // start_pending owns later rollback stages.
            if (entry.second->library == nullptr) continue;      // Static factories are not part of a scan batch.
            if (known.count(entry.first) != 0) continue;
            doomed.push_back(entry.first);
        }
        for (auto it = doomed.rbegin(); it != doomed.rend(); ++it) discard_record(e, *it);
    } catch (...) {
    }
}

/**
 * @brief Roll one batch back on an escaping-exception path, swallowing secondary failures.
 *
 * The caller is already handling an unexpected failure; a second exception from the cleanup must
 * not replace it, and the batch has to stay in a state that shutdown_all() can still retire.
 */
void rollback_pending_quiet(engine& e, const std::vector<std::string>& pending,
                            const std::vector<std::size_t>& plan) noexcept
{
    try {
        std::string ignored;
        (void)rollback_pending(e, pending, plan, ignored);
    } catch (...) {
    }
}

/**
 * @brief Deterministic teardown order: reverse initialization order, then any remaining records.
 *
 * Nothing is destroyed here; the snapshot is only an iteration order that survives the erasures
 * later passes perform.
 */
std::vector<std::string> teardown_order(const engine& e)
{
    std::vector<std::string> ids;
    ids.reserve(e.records.size());
    for (auto it = e.init_order.rbegin(); it != e.init_order.rend(); ++it) {
        if (e.records.count(*it) != 0) ids.push_back(*it);
    }
    for (const auto& entry : e.records) {
        if (std::find(ids.begin(), ids.end(), entry.first) == ids.end()) ids.push_back(entry.first);
    }
    return ids;
}

/**
 * @brief Receiver registered for the temporary lease of one convenient administration call.
 *
 * The framework records this address inside the lease, so the receiver has to stay alive until
 * the credential is provably returned. It therefore keeps only the issuing administration
 * context and returns whatever credential the framework hands back, which makes it stateless and
 * still usable after the operation that created it has returned.
 */
struct one_shot_receiver final : a::irevoker {
    /** @brief Borrowed administration context that issued the credential; never null in use. */
    context* admin = nullptr;

    /**
     * @brief Return the credential named by the framework on the control thread.
     *
     * @param credential Lease issued through this receiver; zero is ignored.
     */
    void U42_CALL on_revoke(a::token credential) noexcept override
    {
        if (admin != nullptr && credential.value != 0) (void)admin->release(credential);
    }
};

/**
 * @brief Lifetime owner for the temporary receiver of one convenient administration call.
 *
 * @note The receiver may only be destroyed once no credential referring to it exists. The flag
 *       starts raised because the receiver is about to be handed to acquire(), and it is lowered
 *       only when acquire() reported no credential or release() reported the credential gone
 *       (ok/stale). Any other exit, including an escaping exception, deliberately retains the
 *       receiver so a still-live credential never points at freed memory; this mirrors the
 *       host's policy of retaining resources whose silence was not proven.
 */
struct one_shot_receiver_owner {
    std::unique_ptr<one_shot_receiver> receiver;
    bool holds_credential = true;

    one_shot_receiver_owner() : receiver(std::make_unique<one_shot_receiver>()) {}
    ~one_shot_receiver_owner()
    {
        if (holds_credential && receiver) (void)receiver.release(); // Deliberate retention.
    }
    one_shot_receiver_owner(const one_shot_receiver_owner&) = delete;
    one_shot_receiver_owner& operator=(const one_shot_receiver_owner&) = delete;
};

/**
 * @brief Run one explicit-version administration call with a temporary, always-returned lease.
 *
 * The synchronous frame performs acquire, the direct invocation and release in that order; there
 * is no binding to create or retire. The caller's inclusive range is passed through unchanged, so
 * a version the host could discover never substitutes for the expectation the caller stated, and
 * no provider pointer is used anywhere. *out is cleared by the nested invocation and never keeps
 * partial output after a failure.
 *
 * @tparam Caller Callable performing the named or numeric invocation for the acquired credential.
 * @param owner Host facade performing the nested lease, invocation and release operations.
 * @param e Engine owning the administration context and its diagnostics.
 * @param plug_id Current provider identity.
 * @param allowed Explicitly accepted version range for this call only.
 * @param caller Invocation of the requested method under the acquired credential.
 * @param out Required output string, cleared before any failure is reported.
 * @return ok with complete output; otherwise the first acquire, invocation or release error.
 */
template <typename Caller>
a::status one_shot_call(u42::host& owner, engine& e, const std::string& plug_id,
                        const a::version_range& allowed, Caller caller, std::string* out)
{
    one_shot_receiver_owner receiver;
    receiver.receiver->admin = e.admin.get();

    a::borrow lease{};
    a::status outcome = owner.acquire(plug_id, allowed, receiver.receiver.get(), &lease);
    if (outcome != a::ok) {
        // acquire() clears its output on failure, so no credential was issued and the receiver can
        // be destroyed together with this frame.
        receiver.holds_credential = false;
        out->clear();
        return outcome;
    }

    // One call per acquired credential; no binding is created or cached in between.
    const a::status invoked = caller(lease.credential); // Clears *out when the invocation fails.

    // The credential is returned on every path, including a failed invocation.
    const a::status returned = owner.release(lease.credential);
    const bool returned_cleanly = returned == a::ok || returned == a::stale;
    if (returned_cleanly) receiver.holds_credential = false;

    if (invoked != a::ok) return invoked;
    if (!returned_cleanly) {
        // The lease stays alive; the receiver was retained above so revoking it stays callable.
        out->clear();
        return e.fail(a::failed, "the temporary lease of plugin '" + plug_id +
                                     "' could not be returned (" + status_text(returned) + ")");
    }
    e.error.clear();
    return a::ok;
}

} // namespace

a::status engine::stage(a::iplug_fty* factory, std::unique_ptr<plug> library)
{
    if (!on_thread()) return a::wrong_thread;
    if (depth != 0) {
        // Structural modification from inside a plugin call is refused before anything is opened,
        // described or registered, so a reentrant request cannot leave a half-staged record.
        return refuse(*this, a::busy, "plugins cannot be staged from inside a plugin call");
    }
    if (factory == nullptr) return fail(a::invalid_argument, "staging requires a non-null factory");
    if (shutting_down) return fail(a::invalid_state, "the host is shutting down and rejects new plugins");

    const a::plug_desc* desc = nullptr;
    a::status described;
    {
        depth_guard guard(*this);
        described = factory->describe(&desc);
    }
    if (described != a::ok) {
        return fail(described, "factory describe() failed (" + status_text(described) + ")");
    }
    if (desc == nullptr) return fail(a::failed, "factory describe() returned a null plug_desc");
    if (desc->struct_size != sizeof(a::plug_desc)) {
        return fail(a::invalid_argument, "plug_desc struct_size is " + std::to_string(desc->struct_size) +
                                             ", expected " + std::to_string(sizeof(a::plug_desc)));
    }
    if (desc->reserved != 0) return fail(a::invalid_argument, "plug_desc reserved field must be zero");

    std::string id;
    std::string message;
    a::status status = read_descriptor_string(desc->plug_id, true, "plug_desc plug_id", id, message);
    if (status != a::ok) return fail(status, message);
    // The declared business version is an ABI value now, so it is copied verbatim: no string
    // termination, UTF-8 or version-syntax parsing applies to it any more.

    order_node node;
    node.plug_id = id;
    node.priority = desc->priority;
    status = read_constraints(desc->before, desc->before_count, "plug_desc before", node.before, message);
    if (status != a::ok) return fail(status, message);
    status = read_constraints(desc->after, desc->after_count, "plug_desc after", node.after, message);
    if (status != a::ok) return fail(status, message);

    if (records.count(id) != 0) {
        return fail(a::duplicate, "plugin identity '" + id + "' is already known to this host");
    }
    if (library && !library->path().empty()) {
        for (const auto& entry : records) {
            if (entry.second->library && !entry.second->library->path().empty() &&
                entry.second->library->path().native() == library->path().native()) {
                return fail(a::duplicate, "plugin library '" + library->path().string() + "' is already loaded");
            }
        }
    }

    auto fresh = std::make_unique<record>();
    fresh->factory = factory;
    fresh->order = std::move(node);
    fresh->version = desc->version; // Exact published instance version; 0.0.0 is a legal value.
    fresh->generation = next_generation++;
    fresh->state = phase::created;
    fresh->ctx = std::make_unique<context>(*this, fresh.get());
    fresh->library = std::move(library);

    auto inserted = records.emplace(fresh->order.plug_id, std::move(fresh));
    if (!inserted.second) {
        return fail(a::duplicate, "plugin identity '" + id + "' was registered concurrently");
    }
    record& rec = *inserted.first->second;

    a::iplug* instance = nullptr;
    a::status created;
    {
        depth_guard guard(*this);
        created = factory->create(&instance);
    }
    if (created != a::ok || instance == nullptr) {
        // The ABI requires the factory to clear its output and leave no resources on failure; a
        // non-null instance is therefore a contract violation and is released defensively here so
        // the host does not lose the only pointer to it. The record is then dropped through the
        // shared erase path, which also discards any unload request the factory queued for its own
        // identity from inside create() - a request that could otherwise unload a later instance
        // reusing that identity.
        if (instance != nullptr) {
            call_scope scope(*this, rec);
            instance->destroy();
        }
        if (record* rejected = lookup_record(*this, id)) erase_record(*this, *rejected);
        if (created != a::ok) {
            return fail(created, "factory create() for plugin '" + id + "' failed (" + status_text(created) + ")");
        }
        return fail(a::failed, "factory create() for plugin '" + id + "' returned a null instance");
    }

    rec.instance = instance;
    error.clear();
    return a::ok;
}

a::status engine::start_pending()
{
    if (!on_thread()) return a::wrong_thread;
    if (shutting_down) return fail(a::invalid_state, "the host is shutting down and rejects new plugins");
    if (starting_batch) return fail(a::invalid_state, "a start batch is already in progress");
    if (depth != 0) return fail(a::invalid_state, "plugins cannot be started from inside a plugin call");
    startup_guard startup(*this);

    std::vector<std::string> pending;
    std::vector<order_node> nodes;
    for (const auto& entry : records) {
        if (entry.second->state != phase::created) continue;
        pending.push_back(entry.first);
        nodes.push_back(entry.second->order);
    }
    if (pending.empty()) {
        error.clear();
        return a::ok;
    }

    std::vector<std::string> established;
    for (const auto& entry : records) {
        const phase value = entry.second->state;
        if (value == phase::initialized || value == phase::starting || value == phase::active) {
            established.push_back(entry.first);
        }
    }

    std::vector<std::size_t> plan;
    std::string plan_error;
    const a::status planned = plan_order(nodes, established, plan, plan_error);
    if (planned != a::ok) {
        // Planning failure happens before any init() call: abandon the whole pending batch so a
        // rejected plan cannot leave half-defined constraints behind.
        rollback_created(*this);
        return fail(planned, plan_error.empty() ? "initialization planning failed" : plan_error);
    }
    if (plan.size() != pending.size()) {
        rollback_created(*this);
        return fail(a::failed, "initialization planner did not return every pending plugin");
    }

    // Phase one: initialize every pending instance before any of them starts. The batch flag is
    // owned by a scope guard, so no exit path - a return, a failed init() or an escaping
    // allocation - can leave it raised and silently block callbacks for every later batch.
    {
        batch_guard batch(*this);
        std::size_t cursor = 0;
        try {
            for (; cursor < plan.size(); ++cursor) {
                // Defensive: a malformed plan cannot index a batch entry.
                if (plan[cursor] >= pending.size()) break;
                const std::string id = pending[plan[cursor]];
                auto found = records.find(id);
                if (found == records.end() || found->second->state != phase::created ||
                    found->second->instance == nullptr) {
                    std::string cleanup;
                    rollback_pending(*this, pending, plan, cleanup);
                    std::string diagnostic = "plugin records changed while initializing '" + id + "'";
                    if (!cleanup.empty()) diagnostic += "; " + cleanup;
                    return fail(a::invalid_state, diagnostic);
                }

                record& rec = *found->second;
                rec.state = phase::initializing;
                a::status initialized;
                {
                    call_scope scope(*this, rec);
                    initialized = rec.instance->init(static_cast<a::ictx*>(rec.ctx.get()));
                }
                if (initialized != a::ok) {
                    std::string cleanup;
                    rollback_pending(*this, pending, plan, cleanup);
                    std::string diagnostic = "init() of plugin '" + id + "' failed (" +
                                             status_text(initialized) + ")";
                    if (!cleanup.empty()) diagnostic += "; " + cleanup;
                    return fail(initialized, diagnostic);
                }
                rec.state = phase::initialized;
                // Successful initialization order drives reverse-order teardown.
                init_order.push_back(id);
            }
        } catch (...) {
            // A batch that failed with an escaping exception must still leave records in a state
            // shutdown_all() can retire, and the cleanup itself must not replace the first failure.
            rollback_pending_quiet(*this, pending, plan);
            return refuse(*this, a::failed, "the initialization batch failed unexpectedly");
        }
        // A plan index outside the batch never ran an init(); the batch is rolled back the same way.
        if (cursor != plan.size()) {
            std::string cleanup;
            rollback_pending(*this, pending, plan, cleanup);
            std::string diagnostic = "initialization plan was malformed";
            if (!cleanup.empty()) diagnostic += "; " + cleanup;
            return fail(a::failed, diagnostic);
        }
    }

    // Phase two: start in the same planned order; capabilities become available per instance.
    try {
        for (std::size_t step = 0; step < plan.size(); ++step) {
            const std::string id = pending[plan[step]];
            auto found = records.find(id);
            if (found == records.end() || found->second->state != phase::initialized) {
                std::string cleanup;
                rollback_pending(*this, pending, plan, cleanup);
                std::string diagnostic = "plugin '" + id + "' left the initialized state before its start()";
                if (!cleanup.empty()) diagnostic += "; " + cleanup;
                return fail(a::invalid_state, diagnostic);
            }

            record& rec = *found->second;
            rec.state = phase::starting; // announce() is only legal during this call.
            a::status started;
            {
                call_scope scope(*this, rec);
                started = rec.instance->start();
            }
            if (started != a::ok) {
                std::string cleanup;
                rollback_pending(*this, pending, plan, cleanup);
                std::string diagnostic = "start() of plugin '" + id + "' failed (" + status_text(started) + ")";
                if (!cleanup.empty()) diagnostic += "; " + cleanup;
                return fail(started, diagnostic);
            }

            // Lifecycle contract with the context owner: the instance is Active before its
            // capability table is committed, so notifications queued by commit() find a legal
            // provider state.
            rec.state = phase::active;
            const a::status committed = commit(rec);
            if (committed != a::ok) {
                std::string cleanup;
                rollback_pending(*this, pending, plan, cleanup);
                std::string diagnostic = "capability commit for plugin '" + id + "' failed (" +
                                         status_text(committed) + ")";
                if (!cleanup.empty()) diagnostic += "; " + cleanup;
                return fail(committed, diagnostic);
            }

            const a::status drained = drain_bounded(*this);
            if (!soft_dispatch_status(drained)) {
                // The batch never claims success while lifecycle notifications cannot be
                // delivered; consumers that are still Initialized must observe the new provider.
                std::string cleanup;
                rollback_pending(*this, pending, plan, cleanup);
                std::string diagnostic = "lifecycle notifications after starting '" + id +
                                         "' could not be delivered (" + status_text(drained) + ")";
                if (!cleanup.empty()) diagnostic += "; " + cleanup;
                return fail(drained, diagnostic);
            }
        }
    } catch (...) {
        rollback_pending_quiet(*this, pending, plan);
        return refuse(*this, a::failed, "the start batch failed unexpectedly");
    }

    error.clear();
    return a::ok;
}

a::status engine::unload_one(const std::string& plug_id)
{
    if (!on_thread()) return a::wrong_thread;
    if (shutting_down) {
        return fail(a::invalid_state, "the host is shutting down; '" + plug_id + "' is handled by shutdown()");
    }
    if (depth != 0) {
        // A plugin stack is live, so unloading now could destroy a provider that is being called.
        // Only a currently known identity may be queued: a request naming an unknown plugin is a
        // plain error, and it must never be remembered and later applied to a different instance
        // that happens to reuse the identity.
        prune_deferred(*this);
        if (lookup_record(*this, plug_id) == nullptr) return refuse_unknown(*this, plug_id);
        if (std::find(deferred_unloads.begin(), deferred_unloads.end(), plug_id) == deferred_unloads.end()) {
            deferred_unloads.push_back(plug_id);
        }
        return a::deferred;
    }
    prune_deferred(*this); // A queued request for a record released meanwhile must not poison drains.

    auto found = records.find(plug_id);
    if (found == records.end()) return fail(a::not_found, "unknown plugin '" + plug_id + "'");
    // Only the state is read here: every branch below tears the record down by identity, because
    // retire() drains and a drain can run the deferred unload queue.
    const phase state = found->second->state;

    switch (state) {
        case phase::created:
            // Never initialized: no stop() is required and no registration exists.
            discard_record(*this, plug_id);
            error.clear();
            return a::ok;

        case phase::initialized: {
            std::string reason;
            const a::status outcome = retire(*this, plug_id, false, reason);
            if (outcome != a::ok) return fail(outcome, reason.empty() ? status_text(outcome) : reason);
            error.clear();
            return a::ok;
        }

        case phase::active:
        case phase::revoking: {
            // A record that is already revoking is retried here: a consumer may have returned its
            // last credential since the blocking attempt, which is the documented recovery path.
            std::string reason;
            const a::status outcome = retire(*this, plug_id, true, reason);
            if (outcome != a::ok) return fail(outcome, reason.empty() ? status_text(outcome) : reason);
            error.clear();
            return a::ok;
        }

        case phase::faulted:
            return fail(a::invalid_state, "plugin '" + plug_id +
                                             "' is quarantined after a failed stop(); stop() is never retried automatically");
        case phase::stopped:
            return fail(a::invalid_state, "plugin '" + plug_id + "' is already stopped");
        case phase::initializing:
        case phase::starting:
            return fail(a::invalid_state, "plugin '" + plug_id + "' cannot be unloaded during its own lifecycle call");
    }

    return fail(a::invalid_state, "plugin '" + plug_id + "' is in an unknown lifecycle state");
}

a::status engine::shutdown_all()
{
    if (!on_thread()) return a::wrong_thread;
    if (depth != 0) {
        // The caller's own instance is part of the graph this pass would stop and destroy, so an
        // in-callback shutdown must be refused rather than served: nothing is marked shutting
        // down and no request is queued, and the caller retries at a safe point.
        return refuse(*this, a::busy, "shutdown cannot run while a plugin call is on the stack");
    }

    if (records.empty() && leases.empty() && deferred_unloads.empty()) {
        shutting_down = true;
        // Host-side bookkeeping can only be stale at this point (no instance owns it), and a
        // repeated shutdown must not leave a receiver or notice behind.
        subscriptions.clear();
        watches.clear();
        events.clear();
        notices.clear();
        error.clear();
        return a::ok;
    }

    // Business entry points close here; retrieve-credential operations stay allowed so consumers
    // can still return borrowings during the revocation pass.
    shutting_down = true;
    deferred_unloads.clear(); // The global pass covers every identity, queued requests included.

    const std::vector<std::string> teardown = teardown_order(*this);

    // Pass one: withdraw every published capability globally, before any instance is stopped.
    for (const std::string& id : teardown) {
        auto found = records.find(id);
        if (found == records.end()) continue;
        record& rec = *found->second;
        if (rec.state == phase::active || rec.state == phase::revoking) {
            const a::status withdrawn = withdraw(rec);
            if (withdrawn != a::ok)
                return fail(withdrawn, "shutdown aborted: capability withdrawal could not be queued");
        }
    }
    {
        const a::status drained = drain_bounded(*this);
        if (!soft_dispatch_status(drained)) {
            // Nothing was destroyed yet: withdrawal notices could not be delivered, so claiming a
            // safe exit would be wrong.
            return fail(drained, "shutdown aborted while withdrawing capabilities (" + status_text(drained) + ")");
        }
    }

    // Pass two: revoke every live instance's inbound borrowings so consumers return credentials
    // before any provider is stopped. Failures here are re-checked by retire() below.
    for (const std::string& id : teardown) {
        auto found = records.find(id);
        if (found == records.end()) continue;
        record& rec = *found->second;
        if (rec.state == phase::active || rec.state == phase::revoking) (void)revoke(rec);
    }

    // Pass three: stop and destroy in teardown order; never-initialized records are dropped directly.
    std::vector<std::pair<std::string, std::string>> pinned;
    for (const std::string& id : teardown) {
        auto found = records.find(id);
        if (found == records.end()) continue;
        // The state is copied: the retire() below drains, and that drain may release this record
        // through the deferred queue before this iteration ends.
        const phase state = found->second->state;

        if (state == phase::created) {
            discard_record(*this, id);
            continue;
        }
        if (state == phase::faulted || state == phase::stopped) {
            pinned.emplace_back(id, "already quarantined; stop() is never retried automatically");
            continue;
        }

        std::string reason;
        const bool active = state == phase::active || state == phase::revoking;
        const a::status outcome = retire(*this, id, active, reason);
        if (outcome != a::ok) pinned.emplace_back(id, reason.empty() ? status_text(outcome) : reason);
    }

    // Pass four: consumers destroyed above may have released the last credential of a provider
    // that was only blocked by borrowings, so retry those records to avoid leaking on a healthy exit.
    std::vector<std::pair<std::string, std::string>> remaining;
    for (const auto& entry : pinned) {
        auto found = records.find(entry.first);
        if (found == records.end()) continue;
        if (found->second->state != phase::revoking) {
            remaining.push_back(entry); // faulted or otherwise pinned: no automatic retry.
            continue;
        }
        std::string reason;
        const a::status outcome = retire(*this, entry.first, true, reason);
        if (outcome != a::ok) {
            remaining.emplace_back(entry.first, reason.empty() ? status_text(outcome) : reason);
        }
    }

    if (!remaining.empty() || !leases.empty() || !records.empty()) {
        std::string message = "shutdown incomplete; quarantined resources retained: ";
        if (remaining.empty()) {
            message += "(none)";
        } else {
            for (std::size_t index = 0; index < remaining.size(); ++index) {
                message += "'" + remaining[index].first + "' (" + remaining[index].second + ")";
                if (index + 1 != remaining.size()) message += ", ";
            }
        }
        if (!leases.empty()) message += "; outstanding borrowings: " + std::to_string(leases.size());
        if (!records.empty()) message += "; live records: " + std::to_string(records.size());
        return fail(a::busy, message);
    }

    // Everything was released on the plugin side; defensive cleanup of host-side bookkeeping so no
    // stale receiver or queued notice can outlive the instances it referred to.
    subscriptions.clear();
    watches.clear();
    events.clear();
    notices.clear();
    error.clear();
    return a::ok;
}

} // namespace u42::detail

namespace u42 {

host::host(host_options options) : engine_(std::make_unique<detail::engine>(options)) {}

host::~host()
{
    if (!engine_) return;
    abi::v3::status outcome = abi::v3::failed;
    try {
        outcome = engine_->shutdown_all();
    } catch (...) {
        outcome = abi::v3::failed;
    }
    if (outcome != abi::v3::ok) {
        // Deliberate retention: destroying a quarantined graph here would free plugin objects
        // whose stop()/borrowings were never proven silent. The engine, its contexts, instances
        // and library mappings stay alive until process exit instead of being partially torn down.
        (void)engine_.release();
    }
}

abi::v3::status host::add(abi::v3::iplug_fty* factory)
{
    if (!engine_) return abi::v3::failed;
    try {
        return engine_->stage(factory);
    } catch (...) {
        // Native API boundary: an allocation failure must not escape as a C++ exception, and the
        // handler must not allocate a diagnostic either, so it reports the status alone.
        return abi::v3::failed;
    }
}

abi::v3::status host::boot(const std::filesystem::path& directory)
{
    if (!engine_) return abi::v3::failed;
    if (!engine_->on_thread()) return abi::v3::wrong_thread;

    std::set<std::string> known; // Default construction cannot allocate; filled inside the guard.
    bool known_complete = false;
    try {
        // Reentrancy is rejected before any scan, open or stage: a callback must not reshape the
        // graph, and the refusal leaves no staged record behind.
        if (engine_->depth != 0) {
            return detail::refuse(*engine_, abi::v3::busy,
                                  "boot cannot run while a plugin call is on the stack");
        }
        for (const auto& entry : engine_->records) known.insert(entry.first);
        known_complete = true;

        std::vector<std::filesystem::path> candidates;
        std::string diagnostic;
        abi::v3::status outcome = scan_plugins(directory, candidates, diagnostic);
        if (outcome != abi::v3::ok) return engine_->fail(outcome, diagnostic);
        if (candidates.empty()) {
            engine_->error.clear();
            return abi::v3::ok;
        }

        for (const std::filesystem::path& candidate : candidates) {
            auto library = std::make_unique<plug>();
            outcome = library->open(candidate, diagnostic);
            if (outcome != abi::v3::ok) {
                detail::rollback_boot_batch(*engine_, known);
                return engine_->fail(outcome, diagnostic);
            }
            // Read the borrowed factory before the mapping is handed over to the record: function
            // argument evaluation order is unspecified, so library->factory() must not share a
            // call with std::move(library).
            abi::v3::iplug_fty* factory = library->factory();
            outcome = engine_->stage(factory, std::move(library));
            if (outcome != abi::v3::ok) {
                // stage() already released the record it refused; report its precise diagnostic.
                detail::rollback_boot_batch(*engine_, known);
                return outcome;
            }
        }
        const abi::v3::status started = engine_->start_pending();
        if (started != abi::v3::ok) detail::rollback_boot_batch(*engine_, known);
        return started;
    } catch (...) {
        // A partial snapshot must never classify a pre-existing record as newly staged.
        if (known_complete) detail::rollback_boot_batch(*engine_, known);
        return abi::v3::failed;
    }
}

abi::v3::status host::start()
{
    if (!engine_) return abi::v3::failed;
    try {
        return engine_->start_pending();
    } catch (...) {
        return abi::v3::failed; // No diagnostic allocation on a native boundary failure path.
    }
}

abi::v3::status host::load(const std::filesystem::path& path)
{
    if (!engine_) return abi::v3::failed;
    if (!engine_->on_thread()) return abi::v3::wrong_thread;
    try {
        // Reentrancy is refused before the library is opened or staged.
        if (engine_->depth != 0) {
            return detail::refuse(*engine_, abi::v3::busy,
                                  "load cannot run while a plugin call is on the stack");
        }

        auto library = std::make_unique<plug>();
        std::string diagnostic;
        abi::v3::status outcome = library->open(path, diagnostic);
        if (outcome != abi::v3::ok) return engine_->fail(outcome, diagnostic);

        // stage() rejects a second mapping of the same canonical library and a duplicate plug_id,
        // and it releases the record it refuses, which also unmaps this library. The borrowed
        // factory is read before the mapping is handed over (argument order is unspecified).
        abi::v3::iplug_fty* factory = library->factory();
        outcome = engine_->stage(factory, std::move(library));
        if (outcome != abi::v3::ok) return outcome;
        return engine_->start_pending();
    } catch (...) {
        return abi::v3::failed;
    }
}

abi::v3::status host::unload(const std::string& plug_id)
{
    if (!engine_) return abi::v3::failed;
    try {
        return engine_->unload_one(plug_id);
    } catch (...) {
        return abi::v3::failed;
    }
}

abi::v3::status host::shutdown()
{
    if (!engine_) return abi::v3::failed;
    try {
        return engine_->shutdown_all();
    } catch (...) {
        return abi::v3::failed;
    }
}

abi::v3::status host::poll()
{
    if (!engine_) return abi::v3::failed;
    if (!engine_->on_thread()) return abi::v3::wrong_thread;
    try {
        // Queued requests for identities that no longer exist are dropped first: the dispatcher
        // restores a failing request, so a stale one would report not_found on every drain.
        detail::prune_deferred(*engine_);
        const abi::v3::status dispatched = engine_->drain();
        if (!detail::soft_dispatch_status(dispatched)) {
            // Keep a richer diagnostic from the dispatcher when it recorded one.
            if (!engine_->error.empty()) return dispatched;
            return engine_->fail(dispatched, "poll could not dispatch queued work (" +
                                                 detail::status_text(dispatched) + ")");
        }

        // drain() owns deferred processing and restores a failed request for a later retry.
        // Do not pop it a second time here: a busy provider must stay queued, and an exhausted
        // event budget must leave all remaining work for the next bounded poll.
        if (dispatched == abi::v3::ok) engine_->error.clear();
        return dispatched;
    } catch (const std::exception&) {
        return abi::v3::failed; // Allocation-free: the failure must not be reported by allocating.
    } catch (...) {
        return abi::v3::failed;
    }
}

abi::v3::status host::version(const std::string& plug_id, abi::v3::plugin_version* out)
{
    if (!engine_) return abi::v3::failed;
    if (!engine_->on_thread()) return abi::v3::wrong_thread; // Native rule: *out stays untouched.
    if (out == nullptr) return detail::refuse(*engine_, abi::v3::invalid_argument, "version requires a non-null output version");
    *out = abi::v3::plugin_version{};
    try {
        // Discovery reads the recorded instance metadata only: it calls no plugin, creates no lease
        // and never becomes the caller's accepted requirement.
        auto found = engine_->records.find(plug_id);
        if (found == engine_->records.end())
            return engine_->fail(abi::v3::not_found, "unknown plugin '" + plug_id + "'");
        detail::record& rec = *found->second;
        if (!rec.published)
            return engine_->fail(abi::v3::not_found, "plugin '" + plug_id + "' has not published capabilities");
        if (rec.state != detail::phase::active || rec.instance == nullptr)
            return engine_->fail(abi::v3::invalid_state, "plugin '" + plug_id + "' is not an active provider");
        *out = rec.version; // A zero version is a legal published version, never a failure marker.
        engine_->error.clear();
        return abi::v3::ok;
    } catch (const std::exception&) {
        *out = abi::v3::plugin_version{};
        return abi::v3::failed; // Allocation free; host::error() keeps its previous text.
    } catch (...) {
        *out = abi::v3::plugin_version{};
        return abi::v3::failed;
    }
}

abi::v3::status host::acquire(const std::string& plug_id, const abi::v3::version_range& allowed,
                              abi::v3::irevoker* receiver, abi::v3::borrow* out)
{
    if (!engine_) return abi::v3::failed;
    if (!engine_->on_thread()) return abi::v3::wrong_thread;
    if (out == nullptr) return detail::refuse(*engine_, abi::v3::invalid_argument, "acquire requires a non-null output borrow");
    *out = abi::v3::borrow{};
    if (receiver == nullptr) return detail::refuse(*engine_, abi::v3::invalid_argument, "acquire requires a non-null revocation receiver");
    try {
        // The administration context owns lease validation: provider identity and published state,
        // the caller's explicit inclusive range, a non-null stable receiver and the ABI rule that no
        // provider address is ever returned. The range is forwarded unchanged, so a version the host
        // could discover never silently replaces the requirement the caller stated.
        const abi::v3::status outcome = engine_->admin->acquire(plug_id.c_str(), &allowed, receiver, out);
        if (outcome != abi::v3::ok) {
            *out = abi::v3::borrow{}; // The ABI requires a cleared output on every failure.
            return outcome;
        }
        engine_->error.clear();
        return abi::v3::ok;
    } catch (const std::exception&) {
        *out = abi::v3::borrow{};
        return abi::v3::failed; // Allocation free; host::error() keeps its previous text.
    } catch (...) {
        *out = abi::v3::borrow{};
        return abi::v3::failed;
    }
}

abi::v3::status host::release(abi::v3::token credential)
{
    if (!engine_) return abi::v3::failed;
    if (!engine_->on_thread()) return abi::v3::wrong_thread;
    if (credential.value == 0) return detail::refuse(*engine_, abi::v3::invalid_argument, "release requires a non-zero lease credential");
    try {
        // Only a successful return is a return: busy, an ownership error or a failure retains the
        // credential, and the caller has to keep the receiver and retry.
        const abi::v3::status outcome = engine_->admin->release(credential);
        if (outcome == abi::v3::ok) engine_->error.clear();
        return outcome;
    } catch (const std::exception&) {
        return abi::v3::failed; // Allocation free; host::error() keeps its previous text.
    } catch (...) {
        return abi::v3::failed;
    }
}

abi::v3::status host::call(abi::v3::token credential, const std::string& method,
                           abi::v3::bytes args, std::string* out)
{
    if (!engine_) return abi::v3::failed;
    if (!engine_->on_thread()) return abi::v3::wrong_thread;
    if (out == nullptr) return detail::refuse(*engine_, abi::v3::invalid_argument, "call requires a non-null output string");
    out->clear();
    if (credential.value == 0) return detail::refuse(*engine_, abi::v3::invalid_argument, "call requires a non-zero lease credential");
    if (args.data == nullptr && args.size != 0) {
        return detail::refuse(*engine_, abi::v3::invalid_argument, "call received a null argument view with a non-zero size");
    }
    try {
        // The credential is used directly: no binding is created, cached or retired, and the
        // administration context revalidates lease ownership and generation on this call.
        detail::string_writer writer(engine_->options.output_limit);
        const abi::v3::status outcome = engine_->admin->call_name(credential, method.c_str(), args, &writer);
        if (outcome != abi::v3::ok) {
            out->clear(); // Partial output is discarded when the invocation fails.
            return outcome;
        }
        if (writer.result != abi::v3::ok) {
            out->clear();
            return engine_->fail(writer.result, "result writer rejected the output of the invocation (" +
                                                    detail::status_text(writer.result) + ")");
        }
        *out = std::move(writer.value);
        engine_->error.clear();
        return abi::v3::ok;
    } catch (const std::exception&) {
        out->clear();
        return abi::v3::failed;
    } catch (...) {
        out->clear();
        return abi::v3::failed;
    }
}

abi::v3::status host::call(abi::v3::token credential, abi::v3::method_id method,
                           abi::v3::bytes args, std::string* out)
{
    if (!engine_) return abi::v3::failed;
    if (!engine_->on_thread()) return abi::v3::wrong_thread;
    if (out == nullptr) return detail::refuse(*engine_, abi::v3::invalid_argument, "call requires a non-null output string");
    out->clear();
    if (credential.value == 0) return detail::refuse(*engine_, abi::v3::invalid_argument, "call requires a non-zero lease credential");
    if (args.data == nullptr && args.size != 0) {
        return detail::refuse(*engine_, abi::v3::invalid_argument, "call received a null argument view with a non-zero size");
    }
    try {
        // Numeric lookup shares the same generation-checked, binding-free invocation path as the
        // named form; only the resolved method entry differs.
        detail::string_writer writer(engine_->options.output_limit);
        const abi::v3::status outcome = engine_->admin->call_id(credential, method, args, &writer);
        if (outcome != abi::v3::ok) {
            out->clear(); // Partial output is discarded when the invocation fails.
            return outcome;
        }
        if (writer.result != abi::v3::ok) {
            out->clear();
            return engine_->fail(writer.result, "result writer rejected the output of the invocation (" +
                                                    detail::status_text(writer.result) + ")");
        }
        *out = std::move(writer.value);
        engine_->error.clear();
        return abi::v3::ok;
    } catch (const std::exception&) {
        out->clear();
        return abi::v3::failed;
    } catch (...) {
        out->clear();
        return abi::v3::failed;
    }
}

abi::v3::status host::call(const std::string& plug_id, const abi::v3::version_range& allowed,
                           const std::string& method, abi::v3::bytes args, std::string* out)
{
    if (!engine_) return abi::v3::failed;
    if (!engine_->on_thread()) return abi::v3::wrong_thread;
    if (out == nullptr) return detail::refuse(*engine_, abi::v3::invalid_argument, "call requires a non-null output string");
    out->clear();
    if (args.data == nullptr && args.size != 0) {
        return detail::refuse(*engine_, abi::v3::invalid_argument, "call received a null argument view with a non-zero size");
    }
    try {
        // The stated range is this frame's expectation for one administration call; it is never
        // replaced by a version the host could discover, and it grants no lasting lease.
        return detail::one_shot_call(*this, *engine_, plug_id, allowed,
                                     [this, &method, &args, out](abi::v3::token credential) {
                                         return call(credential, method, args, out);
                                     },
                                     out);
    } catch (const std::exception&) {
        out->clear();
        return abi::v3::failed; // Allocation free; host::error() keeps its previous text.
    } catch (...) {
        out->clear();
        return abi::v3::failed;
    }
}

abi::v3::status host::call(const std::string& plug_id, const abi::v3::version_range& allowed,
                           abi::v3::method_id method, abi::v3::bytes args, std::string* out)
{
    if (!engine_) return abi::v3::failed;
    if (!engine_->on_thread()) return abi::v3::wrong_thread;
    if (out == nullptr) return detail::refuse(*engine_, abi::v3::invalid_argument, "call requires a non-null output string");
    out->clear();
    if (args.data == nullptr && args.size != 0) {
        return detail::refuse(*engine_, abi::v3::invalid_argument, "call received a null argument view with a non-zero size");
    }
    try {
        // Numeric one-shot form; numeric lookup shares the same generation-checked, binding-free
        // path as the named one.
        return detail::one_shot_call(*this, *engine_, plug_id, allowed,
                                     [this, &method, &args, out](abi::v3::token credential) {
                                         return call(credential, method, args, out);
                                     },
                                     out);
    } catch (const std::exception&) {
        out->clear();
        return abi::v3::failed; // Allocation free; host::error() keeps its previous text.
    } catch (...) {
        out->clear();
        return abi::v3::failed;
    }
}

std::vector<std::string> host::plugins() const
{
    std::vector<std::string> ids;
    // Read-only and control-thread only: never writes the shared diagnostic, so a call from
    // another thread cannot race a host error update.
    if (!engine_ || !engine_->on_thread()) return ids;
    ids.reserve(engine_->records.size());
    for (const auto& entry : engine_->records) ids.push_back(entry.first);
    return ids;
}

const std::string& host::error() const noexcept
{
    static const std::string empty;
    if (!engine_) return empty;
    return engine_->error;
}

} // namespace u42
