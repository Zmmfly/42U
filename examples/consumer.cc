/**
 * @file consumer.cc
 * @brief Example consumer plugin: watch, lease, invoke through the host, survive revocation.
 *
 * @details
 * The consumer runs entirely on the host control thread and never stores a provider address or
 * interface pointer. It watches capability changes, pre-filters an offer with the version carried
 * by that exact notification, and only then acquires a pointerless lease whose fixed accepted
 * range is example::echo_versions. Every business call travels
 * consumer -> host icalls -> provider iinvoke as a single
 * call_name(credential, "echo") under that lease.
 *
 * Leasing and invoking are deliberately separate phases. Acquiring a lease is not business work
 * and the host admits it from Initialized onwards, which matters because capability notifications
 * are dispatched between start() calls - often while this instance is still only Initialized.
 * Invoking is business work and stays gated on this instance being Active, because the host admits
 * no new work while it is still starting an owner. start() therefore only announces the consumer
 * capabilities and flips the Active flag: it performs no lease step, and a notification that
 * arrived before start() has already acquired the lease that invoking then uses.
 *
 * A revocation stops business and returns the credential; a refusal keeps the credential and the
 * granted version in the lease so a later notification or stop() can retry the return, and only
 * ok/stale clear it. A reloaded provider is always re-validated through a fresh notification and
 * re-leased from scratch: an old credential is never followed into a new generation, and a live
 * lease is never re-acquired.
 *
 * The plugin also announces one dynamic status method through iinvoke, which reports the counters
 * demonstrated here.
 *
 * @note The translation unit is built with -fvisibility=hidden. The consumer links no host
 *       implementation: it only needs the frozen ABI and the header-only SDK wrappers.
 */

#include "echo.hpp"

#include <42u/sdk.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace {

/**
 * @brief Count the elements of a fixed-size array at compile time.
 *
 * @tparam element Array element type.
 * @tparam size Number of elements deduced from the array extent.
 * @return The array extent narrowed to the 32-bit ABI count type.
 */
template <typename element, std::size_t size>
constexpr std::uint32_t count_of(const element (&)[size]) noexcept
{
    return static_cast<std::uint32_t>(size);
}

/// @brief Canonical plugin identity announced to the host.
constexpr char consumer_plug_id[] = "com.example.consumer";

/// @brief Identity of the only provider this consumer depends on.
constexpr char echo_plug_id[] = "com.example.echo";

/// @brief Published name of the periodic demonstration event this consumer subscribes to.
constexpr char tick_event_name[] = "demo.tick";

/// @brief Method name bound through icalls under the echo lease.
constexpr char echo_method_name[] = "echo";

/// @brief Published name of the consumer status method (number example::status_method_id).
constexpr char status_method_name[] = "status";

/// @brief Human-readable summary of the consumer status method.
constexpr char status_method_description[] = "Report connection and counters as JSON.";

/// @brief Input schema of the consumer status method.
constexpr char status_input_schema[] = "{\"type\":\"object\"}";

/// @brief Output schema of the consumer status method.
constexpr char status_output_schema[] = "{\"type\":\"object\"}";

/// @brief Hard initialization edge: the echo provider must precede this plugin.
const char* const consumer_after[]{echo_plug_id};

/// @brief Methods announced through icaps::announce; the version travels in plug_desc.
const a::method_desc consumer_methods[]{
    {example::status_method_id, status_method_name, status_method_description, status_input_schema,
     status_output_schema},
};

/// @brief Immutable factory metadata, including the typed plugin version, borrowed until unload.
const a::plug_desc consumer_plug_description{
    static_cast<std::uint32_t>(sizeof(a::plug_desc)), 0u, consumer_plug_id,
    example::consumer_version, 10, 0u, nullptr, count_of(consumer_after), consumer_after};

/// @brief Maximum bytes accepted from one echoed provider result.
constexpr std::size_t echo_result_limit = 4096;

/**
 * @brief Consumer instance implementing the plugin, sink, revoker and invoker roles.
 *
 * @note Callbacks and methods all run on the control thread; the instance never publishes on its
 *       own and never creates threads.
 */
class consumer_plugin final : public a::iplug,
                              public a::ievent_sink,
                              public a::icap_sink,
                              public a::irevoker,
                              public a::iinvoke {
public:
    consumer_plugin() = default;
    consumer_plugin(const consumer_plugin&) = delete;
    consumer_plugin& operator=(const consumer_plugin&) = delete;

    a::status U42_CALL init(a::ictx* ctx) noexcept override;
    a::status U42_CALL start() noexcept override;
    a::status U42_CALL stop() noexcept override;
    void U42_CALL destroy() noexcept override;
    a::status U42_CALL query(const a::iid* type, void** out) noexcept override;

    void U42_CALL on_event(const a::event* value) noexcept override;
    void U42_CALL on_capability(const a::cap_event* value) noexcept override;
    void U42_CALL on_revoke(a::token credential) noexcept override;

    a::status U42_CALL invoke(a::method_id method, a::bytes args,
                              a::iwriter* result) noexcept override;

private:
    /**
     * @brief Attempt to return the current lease, remembering a retryable refusal.
     *
     * @note Only ok/stale empty the lease; every other status keeps the credential and the granted
     *       version inside it and sets revoked_, so a later notification or stop() knows the return
     *       is still owed instead of silently forgetting it.
     */
    void return_lease() noexcept;

    /**
     * @brief Acquire a fresh lease unless one is already held.
     *
     * @note Called only for an offer whose notification version passed accepts_version(); the
     *       acquire itself always uses the fixed example::echo_versions, and the actual version
     *       from the borrow is kept in the lease rather than the notification value.
     */
    void acquire_echo() noexcept;

    /// @brief Capability service borrowed during init; valid until destroy returns.
    a::icaps* caps_ = nullptr;

    /// @brief Call registry borrowed during init; valid until destroy returns.
    a::icalls* calls_ = nullptr;

    /// @brief Event service borrowed during init; valid until destroy returns.
    a::ievents* events_ = nullptr;

    /// @brief Capability watch token owned by this instance.
    a::token watch_token_{};

    /// @brief Event subscription token owned by this instance.
    a::token subscription_token_{};

    /// @brief Pointerless lease on one echo generation; empty while no credential is held.
    u42::sdk::lease echo_lease_{};

    /// @brief Whether the last accepted notification says the provider publishes an accepted version.
    bool echo_available_ = false;

    /// @brief Whether a return owed for an already revoked lease still has to be retried.
    bool revoked_ = false;

    /// @brief Whether init completed successfully.
    bool initialized_ = false;

    /// @brief Whether start completed and the plugin is Active.
    bool active_ = false;

    /// @brief Number of matching revocations observed.
    std::uint64_t revocation_count_ = 0;

    /// @brief Number of @c demo.tick events delivered.
    std::uint64_t event_count_ = 0;
};

/**
 * @brief Acquire the three host services, then watch and subscribe atomically.
 *
 * @param ctx Host context borrowed from the init entry until destroy returns.
 * @retval ok Services were acquired and the watch and subscription registered.
 * @retval invalid_argument The context was null.
 * @retval invalid_state The instance was already initialized.
 * @retval others The propagated host status; the watch is rolled back if the subscription fails.
 * @note No lease is acquired here: only an accepted capability notification may do that, and the
 *       notification may arrive while this instance is still Initialized.
 */
a::status U42_CALL consumer_plugin::init(a::ictx* ctx) noexcept
{
    if (ctx == nullptr) return a::invalid_argument;
    if (initialized_) return a::invalid_state;
    try {
        a::status status = u42::sdk::query(ctx, a::caps_iid, &caps_);
        if (status != a::ok) return status;
        status = u42::sdk::query(ctx, a::calls_iid, &calls_);
        if (status != a::ok) return status;
        status = u42::sdk::query(ctx, a::events_iid, &events_);
        if (status != a::ok) return status;
        status = caps_->watch(this, &watch_token_);
        if (status != a::ok) return status;
        status = events_->subscribe(tick_event_name, this, &subscription_token_);
        if (status != a::ok) {
            (void)caps_->unwatch(watch_token_);
            watch_token_ = {};
            return status;
        }
        initialized_ = true;
        return a::ok;
    } catch (...) {
        return a::failed;
    }
}

/**
 * @brief Announce the status method and enter the Active phase.
 *
 * @retval ok The capability set was announced and the plugin is Active.
 * @retval invalid_state The instance was not initialized or was already started.
 * @retval others The propagated announce status.
 * @note This performs no lease step on purpose. An accepted notification has normally acquired the
 *       lease already, while this instance was Initialized; that existing lease simply becomes
 *       usable for business because invoking is gated on active_.
 */
a::status U42_CALL consumer_plugin::start() noexcept
{
    if (!initialized_ || caps_ == nullptr) return a::invalid_state;
    if (active_) return a::invalid_state;
    try {
        const a::caps_desc capabilities{static_cast<std::uint32_t>(sizeof(a::caps_desc)),
                                        count_of(consumer_methods), consumer_methods};
        const a::status status = caps_->announce(&capabilities);
        if (status != a::ok) return status;
    } catch (...) {
        return a::failed;
    }
    active_ = true;
    return a::ok;
}

/**
 * @brief Stop business, remove both registrations and return the lease.
 *
 * @return ok only when every owned registration and the lease were released.
 * @note A failing removal or a refused icaps::release() is reported instead of being hidden, and
 *       the affected token and its granted version stay owned rather than silently abandoning a
 *       tracked dependency. A failing stop() makes the host quarantine this instance; the host
 *       does not retry stop().
 */
a::status U42_CALL consumer_plugin::stop() noexcept
{
    active_ = false;
    echo_available_ = false;
    a::status failure = a::ok;
    if (events_ != nullptr && subscription_token_.value != 0u) {
        a::status status = a::failed;
        try {
            status = events_->unsubscribe(subscription_token_);
        } catch (...) {
        }
        if (status == a::ok || status == a::stale || status == a::not_found) {
            subscription_token_ = {};
        } else if (failure == a::ok) {
            failure = status;
        }
    }
    if (caps_ != nullptr && watch_token_.value != 0u) {
        a::status status = a::failed;
        try {
            status = caps_->unwatch(watch_token_);
        } catch (...) {
        }
        if (status == a::ok || status == a::stale || status == a::not_found) {
            watch_token_ = {};
        } else if (failure == a::ok) {
            failure = status;
        }
    }
    const a::status returned = echo_lease_.reset();
    if (returned == a::ok || returned == a::stale) {
        revoked_ = false;
    } else {
        revoked_ = true;
        if (failure == a::ok) failure = returned;
    }
    return failure;
}

/**
 * @brief Destroy the instance on the allocating side.
 */
void U42_CALL consumer_plugin::destroy() noexcept
{
    delete this;
}

/**
 * @brief Return the host-private dynamic invoker for the status method.
 *
 * @param type Requested interface identifier.
 * @param out Cleared on entry; receives the borrowed pointer on success.
 * @retval ok The identifier is invoke_iid.
 * @retval invalid_argument The output pointer or identifier was null.
 * @retval unsupported The identifier is unknown to this instance.
 */
a::status U42_CALL consumer_plugin::query(const a::iid* type, void** out) noexcept
{
    if (out == nullptr) return a::invalid_argument;
    *out = nullptr;
    if (type == nullptr) return a::invalid_argument;
    if (*type == a::invoke_iid) {
        *out = static_cast<a::iinvoke*>(this);
        return a::ok;
    }
    return a::unsupported;
}

/**
 * @brief Serve one queued event through the host call gateway.
 *
 * @param value Borrowed event view; valid only during this call.
 *
 * @note @c demo.tick increments the event counter; the echoed call happens only while the plugin
 *       is Active, the last accepted notification still reports availability and the lease holds a
 *       credential. An event delivered while Initialized only counts, because a lease is not
 *       permission to invoke. The call is one direct call_name() under the lease credential; the
 *       provider result is bounded and discarded, so this example proves the call completed, not
 *       what it returned.
 */
void U42_CALL consumer_plugin::on_event(const a::event* value) noexcept
{
    if (value == nullptr || value->name == nullptr) return;
    if (std::strcmp(value->name, tick_event_name) != 0) return;
    ++event_count_;
    if (!active_ || !echo_available_ || !echo_lease_ || calls_ == nullptr) return;
    try {
        std::string discarded;
        u42::sdk::string_writer sink(&discarded, echo_result_limit);
        (void)calls_->call_name(echo_lease_.credential(), echo_method_name, value->payload, &sink);
    } catch (...) {
    }
}

/**
 * @brief Accept an offer only when the notification version passes the consumer's range.
 *
 * @param value Borrowed capability view; valid only during this call.
 *
 * @note The version carried by this exact notification is pre-filtered with
 *       accepts_version(example::echo_versions, value->version); an offer outside the range never
 *       opens business and returns any credential granted earlier, because the provider no longer
 *       publishes what this consumer accepts. A withdrawal stops new business while keeping the
 *       credential for irevoker::on_revoke(). An accepted offer is leased immediately, which is
 *       legal while this instance is only Initialized because leasing is not business work;
 *       invoking still waits for start(). Acquire itself always passes the fixed
 *       example::echo_versions, and a still-live credential is never re-borrowed - only a return
 *       that was refused earlier is retried here.
 */
void U42_CALL consumer_plugin::on_capability(const a::cap_event* value) noexcept
{
    if (value == nullptr || value->plug_id == nullptr) return;
    if (std::strcmp(value->plug_id, echo_plug_id) != 0) return;
    try {
        if (value->available == 0u) {
            // Withdrawal forbids new business but keeps the credential for irevoker::on_revoke().
            echo_available_ = false;
            return;
        }
        if (!a::accepts_version(example::echo_versions, value->version)) {
            // An offer this consumer does not accept must not open business. A credential granted
            // for an earlier, accepted version is returned rather than reused against a provider
            // that now publishes a version outside the consumer's explicit range.
            echo_available_ = false;
            return_lease();
            return;
        }
        if (echo_lease_) {
            if (revoked_) {
                // A return owed for an already revoked credential is retried here; if it still
                // cannot be returned, availability stays false so no new business is attempted.
                return_lease();
                if (echo_lease_) {
                    echo_available_ = false;
                    return;
                }
            } else {
                // A still-live credential is never re-borrowed.
                echo_available_ = true;
                return;
            }
        }
        acquire_echo();
        if (echo_lease_) echo_available_ = true;
    } catch (...) {
    }
}

/**
 * @brief Stop business and return the revoked credential.
 *
 * @param credential Host credential being revoked.
 *
 * @note An unrelated or already returned credential is ignored. Availability is cleared before the
 *       return is attempted, and a refused return keeps the credential and the granted version in
 *       the lease, with revoked_ remembering that the return is still owed; a later notification
 *       or stop() retries it, and a reloaded provider is then leased anew instead of reusing any
 *       old token.
 */
void U42_CALL consumer_plugin::on_revoke(a::token credential) noexcept
{
    if (!echo_lease_.matches(credential)) return;
    echo_available_ = false;
    ++revocation_count_;
    return_lease();
}

/**
 * @brief Serve the published @c status method with the current counters.
 *
 * @param method Published numeric method identifier.
 * @param args Borrowed request bytes; ignored by this method.
 * @param result Caller-owned output writer.
 * @retval ok The status JSON was written.
 * @retval invalid_argument The writer was null.
 * @retval not_found The identifier is not the published status method.
 * @retval failed Building or writing the response failed.
 */
a::status U42_CALL consumer_plugin::invoke(a::method_id method, a::bytes args,
                                           a::iwriter* result) noexcept
{
    (void)args;
    if (method != example::status_method_id) return a::not_found;
    if (result == nullptr) return a::invalid_argument;
    try {
        std::string payload = "{\"connected\":";
        payload += (echo_available_ && static_cast<bool>(echo_lease_)) ? "true" : "false";
        payload += ",\"revocations\":";
        payload += std::to_string(revocation_count_);
        payload += ",\"events\":";
        payload += std::to_string(event_count_);
        payload += "}";
        return result->write(u42::sdk::view(payload));
    } catch (...) {
        return a::failed;
    }
}

/**
 * @brief Attempt to return the current lease, remembering a retryable refusal.
 */
void consumer_plugin::return_lease() noexcept
{
    if (!echo_lease_) {
        revoked_ = false;
        return;
    }
    const a::status status = echo_lease_.reset();
    // ok and stale both mean the host no longer tracks the credential; anything else keeps it.
    revoked_ = status != a::ok && status != a::stale;
}

/**
 * @brief Acquire a fresh lease unless one is already held.
 *
 * @note Called only for an offer whose notification version passed accepts_version(); the acquire
 *       always uses the fixed example::echo_versions and stores the actual version the host
 *       returned, so the notification value never substitutes for the borrow result.
 */
void consumer_plugin::acquire_echo() noexcept
{
    if (caps_ == nullptr || echo_lease_) return;
    a::borrow borrowed{};
    try {
        if (caps_->acquire(echo_plug_id, &example::echo_versions, this, &borrowed) != a::ok) return;
    } catch (...) {
        return;
    }
    if (borrowed.credential.value == 0u) return;
    echo_lease_ = u42::sdk::lease(caps_, borrowed);
    revoked_ = false;
}

/**
 * @brief Library-owned factory returning metadata and fresh instances.
 *
 * @note describe() performs no work beyond handing out immutable metadata, and create() returns an
 *       uninitialized, silent instance.
 */
class consumer_factory final : public a::iplug_fty {
public:
    /**
     * @brief Return immutable metadata without starting any work.
     *
     * @param out Cleared on entry; receives the library-owned metadata on success.
     * @retval ok The metadata pointer was written.
     * @retval invalid_argument The output pointer was null.
     */
    a::status U42_CALL describe(const a::plug_desc** out) noexcept override
    {
        if (out == nullptr) return a::invalid_argument;
        *out = nullptr;
        *out = &consumer_plug_description;
        return a::ok;
    }

    /**
     * @brief Create an uninitialized instance and clear output on failure.
     *
     * @param out Cleared on entry; receives the new instance on success.
     * @retval ok A fresh instance was created without starting work.
     * @retval invalid_argument The output pointer was null.
     * @retval failed Allocation failed; no resources were leaked.
     */
    a::status U42_CALL create(a::iplug** out) noexcept override
    {
        if (out == nullptr) return a::invalid_argument;
        *out = nullptr;
        try {
            *out = new consumer_plugin();
        } catch (...) {
            *out = nullptr;
            return a::failed;
        }
        return a::ok;
    }
};

/// @brief Single library-owned factory borrowed by the host while mapped.
consumer_factory factory_singleton;

} // namespace

/**
 * @brief Frozen entry point; negotiate the ABI major before exposing the factory.
 *
 * @param major Requested ABI major version.
 * @param out Cleared on entry; receives the library-owned factory on success.
 * @retval ok The requested major is supported and the factory was written.
 * @retval invalid_argument The output pointer was null.
 * @retval unsupported The requested major is not supported.
 */
extern "C" U42_EXPORT a::status U42_CALL u42_get_factory(std::uint32_t major,
                                                         a::iplug_fty** out) noexcept
{
    if (out == nullptr) return a::invalid_argument;
    *out = nullptr;
    if (major != a::abi_major) return a::unsupported;
    *out = &factory_singleton;
    return a::ok;
}
