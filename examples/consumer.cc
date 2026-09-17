/**
 * @file consumer.cc
 * @brief Example consumer plugin: observe, borrow, use and revoke a provider lease.
 *
 * @details
 * The consumer runs entirely on the host control thread. During init it acquires
 * the capability, call and event services, then atomically watches capability
 * changes and subscribes to @c demo.tick. Availability may arrive while the plugin
 * is still only Initialized, so a lease is acquired there but business methods are
 * called only while Active. Withdrawals never drop the outstanding credential; the
 * credential is returned in on_revoke, and a fresh lease is acquired afterwards.
 * The plugin also publishes a dynamic @c status method through iinvoke and
 * demonstrates the generic icalls binding path.
 *
 * @note The translation unit is built with -fvisibility=hidden and requires no host
 *       implementation to compile or link.
 */

#include "echo.hpp"

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

/// @brief Human-readable plugin version.
constexpr char consumer_version[] = "1.0.0";

/// @brief Identity of the provider this consumer depends on.
constexpr char echo_plug_id[] = "com.example.echo";

/// @brief Published name of the periodic demonstration event.
constexpr char tick_event_name[] = "demo.tick";

/// @brief Published name of the provider method invoked generically.
constexpr char echo_method_name[] = "echo";

/// @brief Numeric identifier of the consumer status method.
constexpr a::method_id status_method_id = 1;

/// @brief Published name of the consumer status method.
constexpr char status_method_name[] = "status";

/// @brief Human-readable summary of the consumer status method.
constexpr char status_method_description[] = "Report connection and counters as JSON.";

/// @brief Input schema of the consumer status method.
constexpr char status_input_schema[] = "{\"type\":\"object\"}";

/// @brief Output schema of the consumer status method.
constexpr char status_output_schema[] = "{\"type\":\"object\"}";

/// @brief Hard initialization edge: the echo provider must precede this plugin.
const char* const consumer_after[]{echo_plug_id};

/// @brief Interfaces published through icaps::announce during start().
const a::iid consumer_interfaces[]{a::invoke_iid};

/// @brief Methods published through icaps::announce during start().
const a::method_desc consumer_methods[]{
    {status_method_id, status_method_name, status_method_description, status_input_schema,
     status_output_schema},
};

/// @brief Immutable factory metadata borrowed by the host until the library unloads.
const a::plug_desc consumer_plug_description{
    static_cast<std::uint32_t>(sizeof(a::plug_desc)), 0u, consumer_plug_id, consumer_version, 10,
    0u, nullptr, count_of(consumer_after), consumer_after};

/**
 * @brief Bounded, discarding output sink used to demonstrate synchronous calls.
 *
 * @note The consumer never inspects echoed bytes; it only proves that the typed and
 *       generic invocation paths complete within a bounded buffer.
 */
class scratch_writer final : public a::iwriter {
public:
    /**
     * @brief Construct a sink with a fixed accepted-byte budget.
     *
     * @param limit Maximum number of bytes this sink will accept.
     */
    explicit scratch_writer(std::size_t limit) noexcept : limit_(limit) {}

    /**
     * @brief Account for one written slice without retaining it.
     *
     * @param data Borrowed bytes to count.
     * @retval ok The bytes fit within the budget.
     * @retval invalid_argument The slice was null with a non-zero size.
     * @retval limit_exceeded The slice would overflow the budget.
     */
    a::status U42_CALL write(a::bytes data) noexcept override
    {
        if (data.data == nullptr && data.size != 0u) return a::invalid_argument;
        if (data.size > limit_ - accepted_) return a::limit_exceeded;
        accepted_ += static_cast<std::size_t>(data.size);
        return a::ok;
    }

private:
    /// @brief Maximum number of bytes accepted over the writer lifetime.
    std::size_t limit_;

    /// @brief Number of bytes accepted so far; never exceeds limit_.
    std::size_t accepted_ = 0;
};

/**
 * @brief Query one host service interface and reject a null result.
 *
 * @tparam interface_type Exact ABI service type expected for the identifier.
 * @param ctx Host context borrowed from the init entry.
 * @param type Frozen service interface identifier.
 * @param out Cleared first; receives the borrowed service pointer on success.
 * @retval ok The service was found and is non-null.
 * @retval failed The host reported success but returned a null pointer.
 * @retval others The propagated host query status.
 */
template <typename interface_type>
a::status resolve_service(a::ictx* ctx, const a::iid& type, interface_type*& out) noexcept
{
    out = nullptr;
    void* raw = nullptr;
    const a::status status = ctx->query(&type, &raw);
    if (status != a::ok) return status;
    if (raw == nullptr) return a::failed;
    out = static_cast<interface_type*>(raw);
    return a::ok;
}

/**
 * @brief Consumer instance implementing the plugin, sink, revoker and invoker roles.
 *
 * @note Callbacks and methods all run on the control thread; the instance never
 *       publishes on its own and never creates threads.
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
     * @brief Acquire the echo lease unless one is already outstanding.
     *
     * @note The withdrawal path keeps the current credential, so a new lease is
     *       requested only after on_revoke returned the previous one.
     */
    void acquire_echo() noexcept;

    /**
     * @brief Invoke the provider through the generic icalls binding path.
     *
     * @param payload Borrowed request bytes echoed verbatim by the provider.
     */
    void invoke_echo_by_name(a::bytes payload) noexcept;

    /// @brief Capability service borrowed during init; valid until destroy.
    a::icaps* caps_ = nullptr;

    /// @brief Call registry borrowed during init; valid until destroy.
    a::icalls* calls_ = nullptr;

    /// @brief Event service borrowed during init; valid until destroy.
    a::ievents* events_ = nullptr;

    /// @brief Capability watch token owned by this instance.
    a::token watch_token_{};

    /// @brief Event subscription token owned by this instance.
    a::token subscription_token_{};

    /// @brief Outstanding lease credential; zero means no lease is held.
    a::token lease_token_{};

    /// @brief Cached typed lease target; cleared in on_revoke and stop.
    example::iecho* echo_ = nullptr;

    /// @brief Whether business calls on the current lease are permitted.
    bool echo_available_ = false;

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
 * @retval others The propagated host status; partial registrations are rolled back.
 */
a::status U42_CALL consumer_plugin::init(a::ictx* ctx) noexcept
{
    if (ctx == nullptr) return a::invalid_argument;
    if (initialized_) return a::invalid_state;
    try {
        a::status status = resolve_service(ctx, a::caps_iid, caps_);
        if (status != a::ok) return status;
        status = resolve_service(ctx, a::calls_iid, calls_);
        if (status != a::ok) return status;
        status = resolve_service(ctx, a::events_iid, events_);
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
 * @brief Publish the dynamic status method and enter the Active phase.
 *
 * @retval ok The capability set was announced and the plugin is Active.
 * @retval invalid_state The instance was not initialized or was already started.
 * @retval others The propagated announce status.
 */
a::status U42_CALL consumer_plugin::start() noexcept
{
    if (!initialized_ || caps_ == nullptr) return a::invalid_state;
    if (active_) return a::invalid_state;
    try {
        const a::caps_desc capabilities{static_cast<std::uint32_t>(sizeof(a::caps_desc)),
                                        count_of(consumer_interfaces), consumer_interfaces,
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
 * @brief Remove the subscription and watch, then return any outstanding lease.
 *
 * @return ok; all private registrations and leases owned by this instance ended.
 */
a::status U42_CALL consumer_plugin::stop() noexcept
{
    active_ = false;
    echo_available_ = false;
    if (events_ != nullptr && subscription_token_.value != 0u) {
        try {
            (void)events_->unsubscribe(subscription_token_);
        } catch (...) {
        }
        subscription_token_ = {};
    }
    if (caps_ != nullptr && watch_token_.value != 0u) {
        try {
            (void)caps_->unwatch(watch_token_);
        } catch (...) {
        }
        watch_token_ = {};
    }
    if (caps_ != nullptr && lease_token_.value != 0u) {
        const a::token returning = lease_token_;
        lease_token_ = {};
        echo_ = nullptr;
        try {
            (void)caps_->release(returning);
        } catch (...) {
        }
    }
    echo_ = nullptr;
    return a::ok;
}

/**
 * @brief Destroy the instance on the allocating side.
 */
void U42_CALL consumer_plugin::destroy() noexcept
{
    delete this;
}

/**
 * @brief Return the borrowed dynamic-invoker interface for the status method.
 *
 * @param type Requested interface identifier.
 * @param out Cleared on entry; receives the borrowed pointer on success.
 * @retval ok The identifier is served by this instance.
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
 * @brief Handle one queued event, using the lease only while Active.
 *
 * @param value Borrowed event view; valid only during this call.
 *
 * @note @c demo.tick increments the event counter; the echoed call happens only when
 *       the plugin is Active and a usable lease is held, never during Initialized.
 */
void U42_CALL consumer_plugin::on_event(const a::event* value) noexcept
{
    if (value == nullptr || value->name == nullptr) return;
    if (std::strcmp(value->name, tick_event_name) != 0) return;
    ++event_count_;
    if (!active_ || !echo_available_ || echo_ == nullptr) return;
    try {
        scratch_writer typed_sink(4096);
        (void)echo_->echo(value->payload, &typed_sink);
        invoke_echo_by_name(value->payload);
    } catch (...) {
    }
}

/**
 * @brief Track provider availability without losing an outstanding credential.
 *
 * @param value Borrowed capability view; valid only during this call.
 *
 * @note Availability delivered while Initialized acquires a lease but performs no
 *       business call. A withdrawal marks the lease unusable while keeping both the
 *       pointer and the credential, because on_revoke must still return them.
 */
void U42_CALL consumer_plugin::on_capability(const a::cap_event* value) noexcept
{
    if (value == nullptr || value->plug_id == nullptr) return;
    if (std::strcmp(value->plug_id, echo_plug_id) != 0) return;
    try {
        if (value->available != 0u) {
            acquire_echo();
        } else {
            echo_available_ = false;
        }
    } catch (...) {
    }
}

/**
 * @brief Return a matching lease and clear the cached pointer.
 *
 * @param credential Host credential being revoked.
 *
 * @note Only a credential matching the current lease is acted upon; the pointer is
 *       cleared first, the lease is returned through icaps::release, and the
 *       revocation counter is incremented exactly once per match.
 */
void U42_CALL consumer_plugin::on_revoke(a::token credential) noexcept
{
    if (credential.value == 0u || credential.value != lease_token_.value) return;
    echo_ = nullptr;
    echo_available_ = false;
    const a::token returning = lease_token_;
    lease_token_ = {};
    ++revocation_count_;
    if (caps_ != nullptr) {
        try {
            (void)caps_->release(returning);
        } catch (...) {
        }
    }
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
    if (method != status_method_id) return a::not_found;
    if (result == nullptr) return a::invalid_argument;
    try {
        const bool connected = echo_available_ && echo_ != nullptr;
        std::string payload = "{\"connected\":";
        payload += connected ? "true" : "false";
        payload += ",\"revocations\":";
        payload += std::to_string(revocation_count_);
        payload += ",\"events\":";
        payload += std::to_string(event_count_);
        payload += "}";
        return result->write(a::bytes{payload.data(), payload.size()});
    } catch (...) {
        return a::failed;
    }
}

/**
 * @brief Acquire the echo lease unless one is already outstanding.
 */
void consumer_plugin::acquire_echo() noexcept
{
    if (caps_ == nullptr) return;
    if (lease_token_.value != 0u) return;
    a::borrow borrowed{};
    const a::status status = caps_->acquire(echo_plug_id, &example::echo_iid, this, &borrowed);
    if (status != a::ok) return;
    if (borrowed.ptr == nullptr || borrowed.credential.value == 0u) {
        if (borrowed.credential.value != 0u) {
            try {
                (void)caps_->release(borrowed.credential);
            } catch (...) {
            }
        }
        return;
    }
    echo_ = static_cast<example::iecho*>(borrowed.ptr);
    lease_token_ = borrowed.credential;
    echo_available_ = true;
}

/**
 * @brief Invoke the provider through the generic icalls binding path.
 *
 * @param payload Borrowed request bytes echoed verbatim by the provider.
 */
void consumer_plugin::invoke_echo_by_name(a::bytes payload) noexcept
{
    if (calls_ == nullptr) return;
    a::binding target{};
    if (calls_->bind_name(echo_plug_id, echo_method_name, &target) != a::ok) return;
    scratch_writer generic_sink(4096);
    (void)calls_->call(target, payload, &generic_sink);
    (void)calls_->unbind(target);
}

/**
 * @brief Library-owned factory returning metadata and fresh instances.
 *
 * @note describe() performs no work beyond handing out immutable metadata, and
 *       create() returns an uninitialized, silent instance.
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
