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
    abi::v1::status add(abi::v1::iplug_fty* factory);
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
    abi::v1::status boot(const std::filesystem::path& directory);
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
    abi::v1::status start();
    /**
     * @brief Hot-load one library; existing after constraints may be satisfied, before may not.
     *
     * @param path Plugin library to open.
     * @return ok on success; busy inside a plugin callback; otherwise a loading/startup error.
     * @note Starts all Created instances, using start() semantics. Failed cleanup can leave staged
     *       or quarantined records for later shutdown(); failure is not an unconditional rollback.
     */
    abi::v1::status load(const std::filesystem::path& path);
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
    abi::v1::status unload(const std::string& plug_id);
    /**
     * @brief Withdraw capabilities, request lease returns, then stop in reverse initialization order.
     *
     * @return ok after complete cleanup; busy inside a callback without changing shutdown state;
     *         otherwise a cleanup error with unsafe resources retained.
     * @note Callback rejection does not queue shutdown. Outside callbacks, shutdown closes business
     *       entry points even if cleanup fails. Failed consumers retain their outgoing borrowings,
     *       potentially retaining their providers too; failed stop() is not automatically retried.
     */
    abi::v1::status shutdown();
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
    abi::v1::status poll();
    /**
     * @brief Bind an active plugin method by name; binding does not retain the provider.
     *
     * @param plug_id Current provider identity.
     * @param method Exact method name.
     * @param[out] out Required output, cleared before lookup on the control thread.
     * @return ok with a host-owned binding; otherwise a lookup/state/argument error.
     */
    abi::v1::status bind(const std::string& plug_id, const std::string& method,
                         abi::v1::binding* out);
    /**
     * @brief Bind an active plugin method by its numeric ID.
     *
     * @param plug_id Current provider identity.
     * @param method Provider-local method ID.
     * @param[out] out Required output, cleared before lookup on the control thread.
     * @return ok with a host-owned binding; otherwise a lookup/state/argument error.
     */
    abi::v1::status bind(const std::string& plug_id, abi::v1::method_id method,
                         abi::v1::binding* out);
    /**
     * @brief Release a host-owned binding; its value must not subsequently be used.
     *
     * @param value Nonzero binding issued to this host's administration context.
     * @return ok if released; stale if unknown or not owned; invalid_argument if zero.
     */
    abi::v1::status unbind(abi::v1::binding value);
    /**
     * @brief Synchronously invoke a checked binding and atomically return bounded JSON output.
     *
     * @param target Nonzero host-owned binding; does not keep its provider alive.
     * @param args Borrowed input for this call; null data requires zero size.
     * @param[out] out Required string, cleared before validation on the control thread; must not
     *                 own storage referenced by args. Failure discards partial output.
     * @return ok with complete output; otherwise a binding/state/invocation or output-limit error.
     */
    abi::v1::status call(abi::v1::binding target, abi::v1::bytes args, std::string* out);
    /**
     * @brief Convenience name lookup plus invocation, without leaking a temporary binding.
     *
     * @param plug_id Current provider identity; must not alias *out.
     * @param method Exact method name; must not alias *out.
     * @param args Borrowed input; null data requires zero size; must not refer into *out.
     * @param[out] out Required string, cleared on the control thread before lookup.
     * @return ok with complete output; otherwise a lookup/invocation/output error.
     */
    abi::v1::status call(const std::string& plug_id, const std::string& method,
                         abi::v1::bytes args, std::string* out);
    /**
     * @brief Convenience numeric lookup plus invocation.
     *
     * @param plug_id Current provider identity; must not alias *out.
     * @param method Provider-local method ID.
     * @param args Borrowed input; null data requires zero size; must not refer into *out.
     * @param[out] out Required string, cleared on the control thread before lookup.
     * @return ok with complete output; otherwise a lookup/invocation/output error.
     */
    abi::v1::status call(const std::string& plug_id, abi::v1::method_id method,
                         abi::v1::bytes args, std::string* out);
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
