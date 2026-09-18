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
    abi::v3::status add(abi::v3::iplug_fty* factory);
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
    abi::v3::status boot(const std::filesystem::path& directory);
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
    abi::v3::status start();
    /**
     * @brief Hot-load one library; existing after constraints may be satisfied, before may not.
     *
     * @param path Plugin library to open.
     * @return ok on success; busy inside a plugin callback; otherwise a loading/startup error.
     * @note Starts all Created instances, using start() semantics. Failed cleanup can leave staged
     *       or quarantined records for later shutdown(); failure is not an unconditional rollback.
     */
    abi::v3::status load(const std::filesystem::path& path);
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
    abi::v3::status unload(const std::string& plug_id);
    /**
     * @brief Withdraw capabilities, request lease returns, then stop in reverse initialization order.
     *
     * @return ok after complete cleanup; busy inside a callback without changing shutdown state;
     *         otherwise a cleanup error with unsafe resources retained.
     * @note Callback rejection does not queue shutdown. Outside callbacks, shutdown closes business
     *       entry points even if cleanup fails. Failed consumers retain their outgoing borrowings,
     *       potentially retaining their providers too; failed stop() is not automatically retried.
     */
    abi::v3::status shutdown();
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
    abi::v3::status poll();
    /**
     * @brief Discover an Active published instance's current version without leasing it.
     *
     * @param plug_id Current provider identity.
     * @param[out] out Required output, cleared first on the control thread.
     * @return ok on discovery, otherwise a lookup/state/argument error.
     * @note This is not a lifetime lock or compatibility acceptance. Stateful consumers should
     *       acquire with their own allowed range and use the version returned with that lease.
     */
    abi::v3::status version(const std::string& plug_id, abi::v3::plugin_version* out);
    /**
     * @brief Acquire an administration-owned lease and atomically return the actual plugin version.
     *
     * @param plug_id Current provider identity.
     * @param allowed Inclusive numeric version range; equal endpoints mean an exact version.
     * @param receiver Required stable revoker, alive until the lease is successfully returned.
     * @param[out] out Required output, cleared on the control thread; contains credential and version.
     * @return ok, unsupported for an out-of-range version, or an argument/state/allocation error.
     * @note The credential pins one generation. Reacquire and rebuild state after every reload,
     *       including a reload of exactly the same version. No provider pointer is returned.
     */
    abi::v3::status acquire(const std::string& plug_id, const abi::v3::version_range& allowed,
                            abi::v3::irevoker* receiver, abi::v3::borrow* out);
    /**
     * @brief Return an administration-owned lease, proactively or in response to revocation.
     *
     * @param credential Nonzero credential issued by acquire().
     * @return ok after return; stale if already gone; busy while this lease has an in-flight call;
     *         otherwise an argument/ownership/thread error. Refusal does not drop the lease.
     */
    abi::v3::status release(abi::v3::token credential);
    /**
     * @brief Resolve a method name and invoke it directly under an administration-owned lease.
     *
     * @param credential Live lease issued by acquire().
     * @param method Exact published method name; must not alias *out.
     * @param args Borrowed input; null data requires zero size; must not refer into *out.
     * @param[out] out Required string, cleared on the control thread; partial output is discarded.
     * @return ok, stale for an expired lease, not_found for an unknown method, or a call error.
     */
    abi::v3::status call(abi::v3::token credential, const std::string& method,
                         abi::v3::bytes args, std::string* out);
    /**
     * @brief Invoke a numeric method directly under an administration-owned lease.
     *
     * @param credential Live lease issued by acquire().
     * @param method Provider-local published method ID.
     * @param args Borrowed input; null data requires zero size; must not refer into *out.
     * @param[out] out Required string, cleared on the control thread; partial output is discarded.
     * @return The same lease/state/invocation statuses as the named form.
     */
    abi::v3::status call(abi::v3::token credential, abi::v3::method_id method,
                         abi::v3::bytes args, std::string* out);
    /**
     * @brief One-shot administration call using explicit version bounds and a temporary lease.
     *
     * @param plug_id Current provider identity; must not alias *out.
     * @param allowed Explicit inclusive accepted version range, never silently broadened.
     * @param method Exact published method name; must not alias *out.
     * @param args Borrowed input; null data requires zero size; must not refer into *out.
     * @param[out] out Required output, cleared before validation on the control thread.
     * @return ok with complete output; otherwise a version/lookup/invocation/output error.
     * @note Acquires, calls and returns the lease synchronously; there is no binding. This does
     *       not preserve sessions across calls. Use acquire() explicitly when state or selection
     *       of input format based on the returned version matters.
     */
    abi::v3::status call(const std::string& plug_id, const abi::v3::version_range& allowed,
                         const std::string& method, abi::v3::bytes args, std::string* out);
    /**
     * @brief Numeric form of the explicit-version one-shot administration call.
     *
     * @param plug_id Current provider identity; must not alias *out.
     * @param allowed Explicit inclusive version range.
     * @param method Provider-local published method ID.
     * @param args Borrowed input; null data requires zero size; must not refer into *out.
     * @param[out] out Required output, cleared before validation on the control thread.
     * @return ok with complete output; otherwise a version/lookup/invocation/output error.
     */
    abi::v3::status call(const std::string& plug_id, const abi::v3::version_range& allowed,
                         abi::v3::method_id method, abi::v3::bytes args, std::string* out);
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
