#pragma once
#include <42u/abi.hpp>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace u42 {
namespace detail { struct engine; }
/**
 * @brief Bounded host resources; all operations run on the constructing thread.
 */
struct host_options {
    /** @brief Maximum queued ordinary events; lifecycle notices use a separate queue. */
    std::size_t event_capacity = 1024;
    /** @brief Maximum bytes per ordinary event payload or dynamic-call input. */
    std::size_t payload_limit = 1024 * 1024;
    /** @brief Maximum bytes of output accepted from one native dynamic call. */
    std::size_t output_limit = 1024 * 1024;
    /** @brief Maximum events/notices delivered per drain; startup may perform several drains. */
    std::size_t dispatch_budget = 10000;
};
/**
 * @brief Single-control-thread plugin rack. It pins unsafe instances instead of forcing destruction.
 *
 * @note All operations, including destruction and diagnostic reads, belong to the constructing
 *       thread. Status-returning operations reject other threads with wrong_thread; they leave
 *       native output arguments untouched on that rejection.
 * @note Status-returning operations translate internal exceptions to failed (parameter rejection
 *       retains its status even if diagnostic allocation fails). Construction and plugins() can
 *       throw; allocation while preparing caller-side arguments is outside this boundary.
 * @warning The caller must keep this object alive throughout every host operation and plugin
 *          callback. Deleting it in a callback is invalid even though shutdown() rejects reentry.
 */
class host {
public:
    /**
     * @brief Create an empty rack bound to the current control thread.
     *
     * @param options Queue, payload, output and dispatch bounds.
     * @throws std::bad_alloc If runtime allocation fails.
     */
    explicit host(host_options options = {});
    /**
     * @brief Attempt shutdown without throwing; retain the entire runtime on cleanup failure.
     *
     * @note Prefer explicit shutdown() so failure can be reported. Retained instances, contexts,
     *       factories and library mappings may remain necessary until process exit.
     * @warning Destruction during any host operation or plugin callback is forbidden.
     */
    ~host();
    host(const host&) = delete;
    host& operator=(const host&) = delete;
    /**
     * @brief Stage an in-process factory for later start().
     *
     * @param factory Non-null borrowed factory; must remain valid until its instance is destroyed,
     *                including process-lifetime retention if safe teardown fails.
     * @return ok when staged; busy inside a plugin callback; otherwise the staging error.
     */
    abi::v2::status add(abi::v2::iplug_fty* factory);
    /**
     * @brief Scan and eagerly stage every candidate before initialization, then start the batch.
     *
     * @param directory Non-recursive directory of platform-specific .u42 libraries.
     * @return ok on success (also for an empty directory); busy inside a plugin callback;
     *         otherwise a scan, loading or startup error.
     * @note A nonempty scan starts all Created instances, including previously staged ones.
     *       Startup has the same deferred-unload boundary as start(). Scan/staging failure only
     *       rolls back newly scanned records; cleanup is best effort under allocation failure.
     */
    abi::v2::status boot(const std::filesystem::path& directory);
    /**
     * @brief Initialize all staged plugins before starting any, using a deterministic plan.
     *
     * @return ok when every instance in the pending batch is Active (or the batch is empty);
     *         invalid_state on callback reentry; otherwise the planning/init/start error.
     * @note Capability notices dispatch between starts, allowing Initialized consumers to save
     *       borrowings but not invoke business methods. Deferred unloads stay queued throughout
     *       planning, init, start and rollback; a later poll() or safe teardown can consume them.
     *       Lifecycle failure rolls back this pending batch, retaining unsafe resources. Cleanup
     *       is best effort: allocation failure before planning or during rollback may leave Created
     *       or quarantined records for retry/shutdown; failed is not a strong rollback guarantee.
     */
    abi::v2::status start();
    /**
     * @brief Hot-load one library; existing after constraints may be satisfied, before may not.
     *
     * @param path Plugin library to open.
     * @return ok on success; busy inside a plugin callback; otherwise a loading/startup error.
     * @note Starts all Created instances, using start() semantics. Failed cleanup can leave staged
     *       or quarantined records for later shutdown(); failure is not an unconditional rollback.
     */
    abi::v2::status load(const std::filesystem::path& path);
    /**
     * @brief Revoke, stop and unload an instance; unreturned leases make this fail safely.
     *
     * @param plug_id Identity of the current instance to retire.
     * @return ok after teardown; deferred when a known identity is queued inside a callback;
     *         not_found for an unknown identity (never queued); otherwise a teardown/state error.
     * @note Deferred means accepted, not completed. Startup does not consume queued unloads;
     *       poll() after it returns provides a safe point. Explicit unload can satisfy a request.
     *       Requests disappear with their instance and never target a later same-ID replacement.
     */
    abi::v2::status unload(const std::string& plug_id);
    /**
     * @brief Withdraw capabilities, request lease returns, then stop in reverse initialization order.
     *
     * @return ok after complete cleanup; busy inside a callback without changing shutdown state;
     *         otherwise a cleanup error with unsafe resources retained.
     * @note Callback rejection does not queue shutdown. Outside callbacks, shutdown closes business
     *       entry points even if cleanup fails. Failed consumers retain their outgoing borrowings,
     *       potentially retaining their providers too; failed stop() is not automatically retried.
     */
    abi::v2::status shutdown();
    /**
     * @brief Dispatch queued events/notifications and deferred unloads at a safe point.
     *
     * @return ok on completed dispatch; busy on callback reentry; limit_exceeded if the event/notice
     *         budget is exhausted; otherwise a dispatch or deferred teardown error.
     * @note Does not recursively dispatch callbacks. Budget exhaustion leaves remaining work
     *       queued, including unloads not yet attempted. A deferred unload returning a non-ok
     *       status is requeued if storage permits; unresolved leases or failed stop() can block it.
     *       Internal allocation exceptions provide only best-effort queue retention. After failed,
     *       explicitly retry unload(plug_id) rather than assuming the request is still queued.
     */
    abi::v2::status poll();
    /**
     * @brief Discover the currently published business protocol without acquiring a lease.
     *
     * @param plug_id Current provider identity.
     * @param[out] out Required output, cleared first on the control thread.
     * @return ok for an Active provider with a valid protocol; otherwise a lookup/state error.
     * @note Discovery is not compatibility acceptance or a lifetime lock. Stateful consumers
     *       must supply their own expected protocol to acquire(), not blindly copy this result.
     */
    abi::v2::status protocol(const std::string& plug_id, abi::v2::contract* out);
    /**
     * @brief Acquire an administration-owned, revocable lease on one compatible provider instance.
     *
     * @param plug_id Current provider identity.
     * @param required Expected protocol family/major and minimum compatible minor.
     * @param receiver Required stable revoker, kept alive until successful lease return.
     * @param[out] out Required output, cleared on the control thread; contains no interface pointer.
     * @return ok, unsupported for incompatible contracts, or an argument/state/allocation error.
     * @note The lease pins one generation. Reacquire after reload and reconstruct dependent state.
     */
    abi::v2::status acquire(const std::string& plug_id, const abi::v2::contract& required,
                            abi::v2::irevoker* receiver, abi::v2::borrow* out);
    /**
     * @brief Return an administration-owned lease and invalidate all of its method bindings.
     *
     * @param credential Nonzero lease credential issued by acquire().
     * @return ok after return; stale if already gone; busy during a call on this lease;
     *         otherwise an argument/ownership/thread error, retaining the lease on refusal.
     */
    abi::v2::status release(abi::v2::token credential);
    /**
     * @brief Bind a named method under a live administration-owned lease.
     *
     * @param credential Lease issued by acquire(), fixing the provider instance and protocol.
     * @param method Exact published method name.
     * @param[out] out Required output, cleared on the control thread.
     * @return ok, stale for an expired lease, or an argument/ownership/state/lookup error.
     */
    abi::v2::status bind(abi::v2::token credential, const std::string& method,
                         abi::v2::binding* out);
    /**
     * @brief Bind a numeric method under a live administration-owned lease.
     *
     * @param credential Lease issued by acquire().
     * @param method Provider-local method ID.
     * @param[out] out Required output, cleared on the control thread.
     * @return ok, stale for an expired lease, or an argument/ownership/state/lookup error.
     */
    abi::v2::status bind(abi::v2::token credential, abi::v2::method_id method,
                         abi::v2::binding* out);
    /**
     * @brief Release a binding without releasing its lease or unlocking the provider.
     *
     * @param value Nonzero administration-owned binding.
     * @return ok if released; stale if already invalidated; otherwise an argument/ownership error.
     */
    abi::v2::status unbind(abi::v2::binding value);
    /**
     * @brief Invoke through the host gateway with lease, owner, generation and state checks.
     *
     * @param target Administration-owned binding backed by a still-live lease.
     * @param args Borrowed input; null data requires zero size; must not refer into *out.
     * @param[out] out Required string, cleared on the control thread. Partial output is discarded.
     * @return ok with complete output; stale after lease return/reload; otherwise a call error.
     */
    abi::v2::status call(abi::v2::binding target, abi::v2::bytes args, std::string* out);
    /**
     * @brief One-shot administration call using an explicit protocol and a temporary lease.
     *
     * @param plug_id Current provider identity; must not alias *out.
     * @param required Explicitly accepted protocol; never inferred from plugin release version.
     * @param method Exact method name; must not alias *out.
     * @param args Borrowed input; null data requires zero size; must not refer into *out.
     * @param[out] out Required output, cleared before validation on the control thread.
     * @return ok with complete output; otherwise a compatibility/lookup/invocation/output error.
     * @note Acquires, binds, calls, unbinds and returns the lease synchronously. This does not
     *       preserve provider sessions across calls; use an explicit lease for stateful work.
     */
    abi::v2::status call(const std::string& plug_id, const abi::v2::contract& required,
                         const std::string& method, abi::v2::bytes args, std::string* out);
    /**
     * @brief Numeric form of the explicit-protocol one-shot administration call.
     *
     * @param plug_id Current provider identity; must not alias *out.
     * @param required Explicitly accepted protocol family/major and minimum compatible minor.
     * @param method Provider-local method ID.
     * @param args Borrowed input; null data requires zero size; must not refer into *out.
     * @param[out] out Required output, cleared before validation on the control thread.
     * @return ok with complete output; otherwise a compatibility/lookup/invocation/output error.
     */
    abi::v2::status call(const std::string& plug_id, const abi::v2::contract& required,
                         abi::v2::method_id method, abi::v2::bytes args, std::string* out);
    /**
     * @brief Current plugin identities, including staged and quarantined instances.
     *
     * @return A snapshot of current identities; empty when called off the control thread.
     * @throws std::bad_alloc If snapshot allocation fails.
     */
    std::vector<std::string> plugins() const;
    /**
     * @brief Last host diagnostic, used only on the control thread.
     *
     * @return Borrowed diagnostic string, potentially changed by the next operation. Allocation
     *         failure may leave an earlier diagnostic; the operation's status is authoritative.
     */
    const std::string& error() const noexcept;
private:
    std::unique_ptr<detail::engine> engine_;
};
} // namespace u42
