/**
 * @file echo.cc
 * @brief Example echo provider plugin: verbatim JSON echo plus a failing method.
 *
 * @details
 * The library exports the frozen @c u42_get_factory entry point and owns a single
 * static factory. Created instances announce the example::echo_iid contract and the
 * optional dynamic-invoker interface, need no host implementation, and start no
 * threads, so init/start/stop stay trivial and synchronous.
 *
 * @note The translation unit is built with -fvisibility=hidden; only the frozen
 *       entry symbol carries default visibility.
 */

#include "echo.hpp"

#include <cstddef>
#include <cstdint>

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
constexpr char echo_plug_id[] = "com.example.echo";

/// @brief Human-readable plugin version.
constexpr char echo_version[] = "1.0.0";

/// @brief Numeric identifier of the successful echo method.
constexpr a::method_id echo_method_id = 1;

/// @brief Numeric identifier of the deliberate partial-then-fail method.
constexpr a::method_id fail_method_id = 2;

/// @brief Published name of the successful echo method.
constexpr char echo_method_name[] = "echo";

/// @brief Human-readable summary of the successful echo method.
constexpr char echo_method_description[] = "Return the request JSON bytes unchanged.";

/// @brief Input schema of the successful echo method.
constexpr char echo_input_schema[] = "{\"type\":\"object\"}";

/// @brief Output schema of the successful echo method.
constexpr char echo_output_schema[] = "{\"type\":\"object\"}";

/// @brief Published name of the deliberate failure method.
constexpr char fail_method_name[] = "fail";

/// @brief Human-readable summary of the deliberate failure method.
constexpr char fail_method_description[] = "Write a partial JSON prefix and then fail.";

/// @brief Input schema of the deliberate failure method.
constexpr char fail_input_schema[] = "{\"type\":\"object\"}";

/// @brief Output schema of the deliberate failure method.
constexpr char fail_output_schema[] = "{\"type\":\"object\"}";

/// @brief Interfaces published through icaps::announce during start().
const a::iid announced_interfaces[]{example::echo_iid, a::invoke_iid};

/// @brief Methods published through icaps::announce during start().
const a::method_desc announced_methods[]{
    {echo_method_id, echo_method_name, echo_method_description, echo_input_schema,
     echo_output_schema},
    {fail_method_id, fail_method_name, fail_method_description, fail_input_schema,
     fail_output_schema},
};

/// @brief Immutable factory metadata borrowed by the host until the library unloads.
const a::plug_desc echo_plug_description{static_cast<std::uint32_t>(sizeof(a::plug_desc)), 0u,
                                         echo_plug_id, echo_version, 0, 0u, nullptr, 0u, nullptr};

/**
 * @brief Forward plugin output through a caller-owned writer with strict validation.
 *
 * @param out Caller-owned writer; null is rejected.
 * @param data Borrowed bytes to append; null data is allowed only with a zero size.
 * @retval ok The writer accepted the whole slice.
 * @retval invalid_argument A required argument was null or inconsistent.
 * @retval failed The writer threw a foreign exception across the ABI.
 */
a::status append_bytes(a::iwriter* out, a::bytes data) noexcept
{
    if (out == nullptr) return a::invalid_argument;
    if (data.data == nullptr && data.size != 0u) return a::invalid_argument;
    try {
        return out->write(data);
    } catch (...) {
        return a::failed;
    }
}

/**
 * @brief Provider instance implementing the echo contract and the optional invoker.
 *
 * @note All calls arrive on the single host control thread; the instance is created
 *       by the factory, initialized once, started once and finally destroyed through
 *       destroy(), which performs the deletion on the allocating side.
 */
class echo_plugin final : public a::iplug, public a::iinvoke, public example::iecho {
public:
    echo_plugin() = default;
    echo_plugin(const echo_plugin&) = delete;
    echo_plugin& operator=(const echo_plugin&) = delete;

    a::status U42_CALL init(a::ictx* ctx) noexcept override;
    a::status U42_CALL start() noexcept override;
    a::status U42_CALL stop() noexcept override;
    void U42_CALL destroy() noexcept override;
    a::status U42_CALL query(const a::iid* type, void** out) noexcept override;

    a::status U42_CALL invoke(a::method_id method, a::bytes args,
                              a::iwriter* result) noexcept override;
    a::status U42_CALL echo(a::bytes input, a::iwriter* out) noexcept override;

private:
    /**
     * @brief Emit a partial JSON prefix and then fail on purpose.
     *
     * @param out Caller-owned writer receiving the partial bytes.
     * @retval invalid_argument The writer was null.
     * @retval failed Always, after the partial prefix was accepted, so a host can
     *         verify that partial output is discarded atomically.
     */
    a::status write_partial_then_fail(a::iwriter* out) noexcept;

    /// @brief Capability service borrowed during init; valid until destroy.
    a::icaps* caps_ = nullptr;

    /// @brief Guards icaps::announce against a second start().
    bool announced_ = false;
};

/**
 * @brief Borrow the capability service and cache it for start().
 *
 * @param ctx Host context borrowed from the init entry until destroy returns.
 * @retval ok The capability service was acquired.
 * @retval invalid_argument The context was null.
 * @retval invalid_state The instance was already initialized.
 * @retval others The propagated host query status, or failed for a null service.
 */
a::status U42_CALL echo_plugin::init(a::ictx* ctx) noexcept
{
    if (ctx == nullptr) return a::invalid_argument;
    if (caps_ != nullptr) return a::invalid_state;
    void* raw = nullptr;
    try {
        const a::status status = ctx->query(&a::caps_iid, &raw);
        if (status != a::ok) return status;
    } catch (...) {
        return a::failed;
    }
    if (raw == nullptr) return a::failed;
    caps_ = static_cast<a::icaps*>(raw);
    return a::ok;
}

/**
 * @brief Publish the interface set and the two synchronous methods.
 *
 * @retval ok The complete capability set was announced.
 * @retval invalid_state The instance was not initialized or was already started.
 * @retval others The propagated announce status.
 */
a::status U42_CALL echo_plugin::start() noexcept
{
    if (caps_ == nullptr) return a::invalid_state;
    if (announced_) return a::invalid_state;
    try {
        const a::caps_desc capabilities{static_cast<std::uint32_t>(sizeof(a::caps_desc)),
                                        count_of(announced_interfaces), announced_interfaces,
                                        count_of(announced_methods), announced_methods};
        const a::status status = caps_->announce(&capabilities);
        if (status != a::ok) return status;
    } catch (...) {
        return a::failed;
    }
    announced_ = true;
    return a::ok;
}

/**
 * @brief Retire the announced capability set.
 *
 * @return ok; the provider owns no threads, callbacks or leases to unwind.
 */
a::status U42_CALL echo_plugin::stop() noexcept
{
    announced_ = false;
    return a::ok;
}

/**
 * @brief Destroy the instance on the allocating side.
 */
void U42_CALL echo_plugin::destroy() noexcept
{
    delete this;
}

/**
 * @brief Return a borrowed interface pointer for a known identifier.
 *
 * @param type Requested interface identifier.
 * @param out Cleared on entry; receives the borrowed pointer on success.
 * @retval ok The identifier is served by this instance.
 * @retval invalid_argument The output pointer or identifier was null.
 * @retval unsupported The identifier is unknown to this instance.
 */
a::status U42_CALL echo_plugin::query(const a::iid* type, void** out) noexcept
{
    if (out == nullptr) return a::invalid_argument;
    *out = nullptr;
    if (type == nullptr) return a::invalid_argument;
    if (*type == example::echo_iid) {
        *out = static_cast<example::iecho*>(this);
        return a::ok;
    }
    if (*type == a::invoke_iid) {
        *out = static_cast<a::iinvoke*>(this);
        return a::ok;
    }
    return a::unsupported;
}

/**
 * @brief Dispatch a published method through the dynamic-invoker interface.
 *
 * @param method Published numeric method identifier.
 * @param args Borrowed request bytes forwarded to the echo method.
 * @param result Caller-owned output writer.
 * @retval ok The echo method succeeded.
 * @retval invalid_argument A required argument was null.
 * @retval not_found The identifier is not a published method.
 * @retval failed The deliberate failure method ran, or a foreign exception escaped.
 */
a::status U42_CALL echo_plugin::invoke(a::method_id method, a::bytes args,
                                       a::iwriter* result) noexcept
{
    switch (method) {
    case echo_method_id:
        return echo(args, result);
    case fail_method_id:
        return write_partial_then_fail(result);
    default:
        return a::not_found;
    }
}

/**
 * @brief Copy the request bytes to the writer without parsing them.
 *
 * @param input Borrowed request bytes.
 * @param out Caller-owned output writer.
 * @retval ok The whole payload was accepted.
 * @retval invalid_argument A required argument was null or inconsistent.
 * @retval failed The writer threw a foreign exception across the ABI.
 */
a::status U42_CALL echo_plugin::echo(a::bytes input, a::iwriter* out) noexcept
{
    return append_bytes(out, input);
}

/**
 * @brief Emit a partial JSON prefix and then fail on purpose.
 *
 * @param out Caller-owned writer receiving the partial bytes.
 * @retval invalid_argument The writer was null.
 * @retval failed Always, after the partial prefix was accepted.
 */
a::status echo_plugin::write_partial_then_fail(a::iwriter* out) noexcept
{
    if (out == nullptr) return a::invalid_argument;
    static constexpr char partial_prefix[] = "{\"echo\":";
    const a::status status =
        append_bytes(out, a::bytes{partial_prefix, sizeof(partial_prefix) - 1u});
    if (status != a::ok) return status;
    return a::failed;
}

/**
 * @brief Library-owned factory returning metadata and fresh instances.
 *
 * @note describe() performs no work beyond handing out immutable metadata, and
 *       create() returns an uninitialized, silent instance.
 */
class echo_factory final : public a::iplug_fty {
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
        *out = &echo_plug_description;
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
            *out = new echo_plugin();
        } catch (...) {
            *out = nullptr;
            return a::failed;
        }
        return a::ok;
    }
};

/// @brief Single library-owned factory borrowed by the host while mapped.
echo_factory factory_singleton;

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
