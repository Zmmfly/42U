#pragma once

#include <42u/abi.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

/**
 * @file sdk.hpp
 * @brief Header-only, source-level RAII helpers for the frozen u42 plugin ABI.
 *
 * Everything declared here is compiled into its user: the helpers introduce no new interface
 * identifier, no vtable and no host-visible type, so 42u/abi.hpp stays exactly as frozen. They
 * only remove mechanical mistakes (uncleared outputs, leaked credentials, unchecked writers)
 * while keeping the ABI's ownership and lifetime rules visible at the call site.
 */
namespace u42 {
namespace sdk {

/**
 * @brief Query one host service through ictx and hand it back as a typed borrowed pointer.
 *
 * @tparam T Interface type that belongs to the queried identifier. The ABI transports only
 *           void*, so the caller must name the same type the host published for this iid.
 *
 * @param ctx Borrowed host context. A null context is rejected without being dereferenced.
 * @param type Frozen service interface identifier, e.g. abi::v3::caps_iid or abi::v3::calls_iid.
 * @param out Receives the borrowed interface. It is cleared before the context is touched and
 *            remains null on every failure; a null out is rejected, because otherwise a failed
 *            query could not be reported anywhere.
 *
 * @return ok on success, invalid_argument when ctx or out is null, otherwise the status returned
 *         by ictx::query().
 *
 * @note The result is the host's own pointer converted verbatim: this helper neither validates
 *       nor owns it, and it stays usable only as long as the context guarantees. In particular
 *       a host that reports ok with a null pointer yields an empty output rather than a
 *       fabricated one.
 */
template <class T>
abi::v3::status query(abi::v3::ictx* ctx, abi::v3::iid type, T** out) noexcept
{
    if (out == nullptr) return abi::v3::invalid_argument;
    *out = nullptr;
    if (ctx == nullptr) return abi::v3::invalid_argument;
    void* raw = nullptr;
    const abi::v3::status status = ctx->query(&type, &raw);
    if (status != abi::v3::ok) return status;
    *out = static_cast<T*>(raw);
    return status;
}

/**
 * @brief Borrow a string view as ABI bytes without copying it.
 *
 * @param text Caller-owned view; the returned bytes alias the same characters.
 * @return Borrowed bytes with the view's address and size. An empty view yields {nullptr, 0},
 *         because the ABI requires a null data pointer to carry size zero.
 *
 * @note The ABI neither copies nor owns the text: the result is valid only while text's
 *       underlying characters stay alive and unmodified, so never return it from a function
 *       that owns the string and never retain it after the call.
 */
inline abi::v3::bytes view(std::string_view text) noexcept
{
    if (text.empty()) return abi::v3::bytes{nullptr, 0};
    return abi::v3::bytes{text.data(), static_cast<std::uint64_t>(text.size())};
}

/**
 * @brief Move-only RAII holder for one icaps::acquire() borrow.
 *
 * A lease owns exactly one revocation credential together with the plugin version the host
 * actually granted for it. The ABI value it holds (abi::v3::borrow) is already the
 * {credential, version} pair, and the class deliberately exposes neither get() nor operator->
 * nor any interface pointer: the only way to reach business functionality is to pass
 * credential() to icalls::call_name()/call_id(). That keeps the plugin-to-plugin path
 * consumer -> host icalls -> provider iinvoke and makes it impossible for a lease to smuggle a
 * provider address across the boundary.
 *
 * version() reports the version copied out of the provider descriptor by acquire(), not the
 * range that was requested, so a caller that asked for [1.0.0, 1.9.9] can learn which instance
 * it actually holds. Because 0.0.0 is a legal plugin version, ownership is decided by the
 * credential alone: operator bool() and credential() inspect the token, and a zero-token borrow
 * is empty even when its version reads 0.0.0.
 *
 * The credential is returned through icaps::release(). Ownership is dropped only when the host
 * actually accepted the return (ok, or stale for a credential that a later acquire already
 * superseded); a retryable refusal such as busy, wrong_thread or failed keeps the credential and
 * the granted version in place so the control thread can retry, and a moved-from lease holds
 * nothing, so ownership transfers without any possibility of a second release. reset() is
 * therefore idempotent and never throws.
 *
 * @note Source-level only: this wrapper is never passed across the ABI - not as a parameter of a
 *       virtual function and not inside an ABI-visible struct. Only icaps::acquire()/release()
 *       and the raw borrow cross the boundary.
 *
 * @warning Never make the lease itself, or any class deriving from it, the irevoker handed to
 *          icaps::acquire(). Moving a lease transfers the credential but not the callback address,
 *          so the host would keep revoking into the address it saw at acquire() time - the
 *          moved-from or destroyed object - and a later irevoker::on_revoke() would be a
 *          use-after-free. Register a stable owner object as the irevoker instead and, from its
 *          on_revoke(), call lease.reset() only when lease.matches(credential) is true;
 *          tests/sdk_test.cc pins exactly that flow.
 */
class lease {
public:
    /** @brief Construct an empty lease that owns no credential. */
    lease() noexcept = default;

    /**
     * @brief Adopt an already acquired borrow.
     *
     * @param caps Owning capability service used to return the credential; borrowed, not owned.
     *             A null value makes the credential unreturnable: reset() then reports
     *             invalid_state and keeps both the token and the granted version, so only
     *             construct a lease with the icaps that actually issued the borrow.
     * @param value Borrow returned by icaps::acquire(); the credential and the actual granted
     *              version are stored together. A zero credential means "owns nothing", even
     *              when its version reads 0.0.0.
     */
    lease(abi::v3::icaps* caps, abi::v3::borrow value) noexcept : caps_(caps), value_(value) {}

    /** @brief A borrow is owned by exactly one lease, so copying is disabled. */
    lease(const lease&) = delete;
    /** @brief Copy assignment would duplicate the credential; use move assignment instead. */
    lease& operator=(const lease&) = delete;

    /**
     * @brief Take over other's borrow; other is left empty and will not release anything.
     *
     * @param other Lease to transfer from, which must outlive this call only.
     */
    lease(lease&& other) noexcept : caps_(other.caps_), value_(other.value_)
    {
        other.caps_ = nullptr;
        other.value_ = abi::v3::borrow{};
    }

    /**
     * @brief Return this lease's credential, then take over other's borrow.
     *
     * @param other Lease to transfer from; self-assignment is a no-op and touching nothing.
     * @return *this.
     * @note Both objects are left completely unchanged when returning this lease's own credential
     *       fails with a retryable status: no borrow is dropped and nothing is transferred, so the
     *       caller still owns both borrows - credential and granted version alike - and can retry
     *       the assignment once the host can accept the release. Only after a successful
     *       (ok/stale) return is other drained.
     */
    lease& operator=(lease&& other) noexcept
    {
        if (this != &other) {
            // reset() empties this lease with ok when it holds nothing at all.
            const abi::v3::status status = reset();
            if (status == abi::v3::ok || status == abi::v3::stale) {
                caps_ = other.caps_;
                value_ = other.value_;
                other.caps_ = nullptr;
                other.value_ = abi::v3::borrow{};
            }
        }
        return *this;
    }

    /**
     * @brief Last-chance attempt to return the held credential before the storage disappears.
     *
     * @warning Destruction cannot retry. If reset() fails here with a retryable status the
     *          credential goes away with the object and the host keeps tracking a borrow nobody
     *          holds any more. Destroy a lease on the same valid control thread - and only while
     *          the icaps/ictx that issued it is still usable - where icaps::release() can
     *          succeed; never destroy one after the owning context is gone.
     */
    ~lease() { (void)reset(); }

    /**
     * @brief Try to return the credential through icaps::release(), keeping ownership if refused.
     *
     * @return ok when a credential was returned or nothing was held; stale when the host reported
     *         the credential as already superseded; invalid_state when a credential is held but no
     *         icaps was known; otherwise the status from icaps::release(), e.g. wrong_thread,
     *         busy or failed.
     *
     * @note The whole borrow - credential and granted version together - is cleared only for ok
     *       and stale. Every other status is treated as retryable and leaves the token and the
     *       version exactly as they were, because dropping the credential would silently lose a
     *       borrow the host still tracks and would also forget which provider version had been
     *       selected. Callers must retry on the control thread until the status is no longer
     *       retryable; the function never throws, is idempotent once empty, and is the function to
     *       call from irevoker::on_revoke() when lease.matches(credential) is true.
     */
    abi::v3::status reset() noexcept
    {
        if (value_.credential.value == 0) {
            value_ = abi::v3::borrow{};
            return abi::v3::ok;
        }
        if (caps_ == nullptr) return abi::v3::invalid_state;
        const abi::v3::status status = caps_->release(value_.credential);
        if (status == abi::v3::ok || status == abi::v3::stale) value_ = abi::v3::borrow{};
        return status;
    }

    /**
     * @brief True while a non-zero credential is held, i.e. this lease still pins its provider.
     *
     * @note This is the whole ownership state and it deliberately ignores the version: 0.0.0 is a
     *       valid plugin version, so only the token decides whether anything is owned, and bool
     *       and credential() always agree.
     */
    explicit operator bool() const noexcept { return value_.credential.value != 0; }

    /**
     * @brief Credential getter used to call methods and to match an incoming revocation.
     * @return The owned credential, or a zero token when the lease is empty. This is the value to
     *         pass to icalls::call_name()/call_id(); it is never a provider address.
     */
    abi::v3::token credential() const noexcept { return value_.credential; }

    /**
     * @brief Plugin version the host granted together with the credential.
     *
     * @return The actual provider version copied into the borrow by icaps::acquire(), or 0.0.0
     *         when the lease holds nothing. A zero value is not a failure signal: 0.0.0 is a
     *         legal version, so test ownership with operator bool() instead.
     */
    abi::v3::plugin_version version() const noexcept { return value_.version; }

    /**
     * @brief Test whether a revocation names the credential this lease still holds.
     *
     * @param revoked Credential delivered to irevoker::on_revoke().
     * @return True only when this lease holds exactly that non-zero credential, so an unrelated
     *         or already returned revocation is ignored instead of triggering a second release.
     */
    bool matches(abi::v3::token revoked) const noexcept
    {
        return value_.credential.value != 0 && value_.credential.value == revoked.value;
    }

private:
    abi::v3::icaps* caps_ = nullptr;
    abi::v3::borrow value_{};
};

/**
 * @brief iwriter implementation that appends into a caller-owned std::string under a hard cap.
 *
 * The writer is the plugin-facing end of icalls::call_name()/call_id() and iinvoke::invoke():
 * the ABI only ever sees the borrowed iwriter*, while the std::string target and this class stay
 * on the producing side and never cross the boundary. A call that fails leaves partial output
 * discarded by the host, so a writer that also refuses to grow past its limit keeps the result
 * all-or-nothing.
 *
 * @note iwriter deliberately has no virtual destructor; a string_writer is stack-owned by its
 *       caller and must not be deleted through the ABI base pointer.
 */
class string_writer final : public abi::v3::iwriter {
public:
    /**
     * @brief Bind to a target string and its maximum size.
     *
     * @param out Caller-owned target that receives the appended bytes; it must outlive this
     *            writer. A null target is accepted, but every write then fails with
     *            invalid_argument instead of dereferencing it.
     * @param limit Maximum total size of *out in bytes. It is never exceeded: a write that would
     *              cross it fails and leaves *out unchanged, so no truncated prefix is published.
     */
    explicit string_writer(std::string* out, std::size_t limit) noexcept : out_(out), limit_(limit)
    {
    }

    /**
     * @brief Append one borrowed chunk, returning a status instead of growing without bound.
     *
     * @param data Borrowed bytes, valid only for this call; a null pointer is allowed only with
     *             size zero.
     * @return ok on success, limit_exceeded when the cap would be crossed, invalid_argument for a
     *         missing target or a malformed chunk, or failed when appending threw.
     *
     * @note The status is sticky: after the first failure every later write returns that same
     *       status without touching the target, so a caller cannot mistake a partial result for
     *       a complete one. Exceptions are caught here because iwriter::write is noexcept and
     *       must not unwind into the ABI.
     */
    abi::v3::status U42_CALL write(abi::v3::bytes data) noexcept override
    {
        if (status_ != abi::v3::ok) return status_;
        if (out_ == nullptr) return record(abi::v3::invalid_argument);
        if (data.size != 0 && data.data == nullptr) return record(abi::v3::invalid_argument);
        const std::size_t used = out_->size();
        const std::size_t room = used < limit_ ? limit_ - used : 0;
        if (data.size > static_cast<std::uint64_t>(room)) return record(abi::v3::limit_exceeded);
        if (data.size != 0) {
            try {
                out_->append(static_cast<const char*>(data.data),
                             static_cast<std::size_t>(data.size));
            } catch (...) {
                return record(abi::v3::failed);
            }
        }
        return status_;
    }

    /** @brief Sticky result of the writes so far; ok until the first failure. */
    abi::v3::status status() const noexcept { return status_; }

private:
    /**
     * @brief Remember the first failure, since a later failure must not overwrite it.
     *
     * @param status Failure to record when no failure was recorded yet.
     * @return The recorded sticky status.
     */
    abi::v3::status record(abi::v3::status status) noexcept
    {
        if (status_ == abi::v3::ok) status_ = status;
        return status_;
    }

    std::string* out_ = nullptr;
    std::size_t limit_ = 0;
    abi::v3::status status_ = abi::v3::ok;
};

} // namespace sdk
} // namespace u42
