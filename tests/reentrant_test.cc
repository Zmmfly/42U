/**
 * @file reentrant_test.cc
 * @brief Regression checks for unload requests issued from inside plugin callbacks.
 *
 * The public host API lets an application - and therefore a plugin callback that captured the
 * host pointer, as the integration layer does - request `u42::host::unload()`. When such a
 * request is queued because a plugin stack is live, the later safe-point retirement must still
 * tear the instance down exactly once, and the host must not keep replaying a request that can
 * no longer be satisfied.
 *
 * Migrated to ABI v3 (typed version lease + direct calls): a provider publishes a numeric
 * plugin_version and a method set and answers query(invoke_iid); a business call is always an
 * explicit version range lease plus a direct call through the host call path, by method name or
 * numeric id, using the credential that `host.acquire()` returned. The binding layer is gone, so
 * the native trigger keeps a stable revoker alive until the credential is returned and never
 * holds a provider pointer.
 *
 * Four contracts are pinned here against the real host implementation (every src source):
 *
 *  1. A request queued from inside a method invocation, followed by a safe-point retry of the
 *     same identity, performs exactly one stop()/destroy() pair. Before the fix the retirement
 *     drained the deferred queue, re-entered itself and used the freed record (ASan
 *     heap-use-after-free plus a double destroy).
 *  2. A consumer requesting the unload of the very provider whose withdrawal notification it is
 *     handling cannot disturb that retirement: the nested attempt is refused, the provider is
 *     stopped and destroyed exactly once, and later poll() calls prune the unsatisfiable request
 *     instead of failing forever.
 *  3. A request queued from an availability callback stays pending until start() returns and
 *     is served exactly once at a later safe point.
 *  4. A provider whose version lease is not returned stays revoking (no stop, no destroy)
 *     and becomes unloadable as soon as the credential comes back.
 *
 * The remaining checks pin the reentrancy boundaries around the same hazard:
 *
 *  5. host::shutdown() from inside a plugin call is refused (busy) without marking the host as
 *     shutting down, so the caller can retry at a safe point and still get a clean teardown.
 *  6. A deferred unload request never outlives the identity it was queued for: a request queued
 *     from create(), abandoned by an initialization-plan rollback and followed by a new instance
 *     reusing that identity must not unload the replacement. An unknown identity is refused at
 *     once instead of being remembered for some future instance.
 *  7. The structural APIs (add/boot/load) are refused from inside a plugin call before any scan,
 *     open or stage, so a reentrant request cannot leave a staged record behind.
 *  8. The batch flag cannot stick: after a batch that failed during planning (cycle) or during
 *     initialization, a later batch still initializes, starts and receives callbacks.
 *
 * Build and run (real sources plus sanitizers), from the project root:
 *
 *   g++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -Iinc -Isrc tests/reentrant_test.cc src/context.cc src/events.cc src/host.cc src/order.cc src/plug.cc -o build/invoke-v3/safety/reentrant -ldl -pthread
 *   ./build/invoke-v3/safety/reentrant
 *
 * The test owns its main() and depends only on <42u/abi.hpp> and <42u/host.hpp>. Each fake owns
 * its instance, so a plugin-side destroy() does not free the object and the stop()/destroy()
 * call counts stay readable after teardown.
 */
#define NDEBUG 1

#include <42u/abi.hpp>
#include <42u/host.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace abi = u42::abi::v3;

/* ------------------------------------------------------------------ *
 * Test harness
 * ------------------------------------------------------------------ */

std::size_t g_checks = 0;
std::size_t g_failures = 0;

/**
 * @brief Report one failed CHECK and terminate with a non-zero exit status.
 *
 * A non-zero status is what the build's test runner observes, so a failure can never pass
 * silently the way a compiled-out assert() would.
 */
[[noreturn]] void fail_check(const char* expr, const char* file, int line)
{
    ++g_failures;
    std::fprintf(stderr, "CHECK failure: %s (%s:%d)\n", expr, file, line);
    std::fflush(stderr);
    std::exit(EXIT_FAILURE);
}

/** @brief Fatal check that survives NDEBUG, unlike assert(). */
#define CHECK(condition)                                                            \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(condition)) fail_check(#condition, __FILE__, __LINE__);                \
    } while (false)

/** @brief Plugin business version every provider fake publishes during describe(). */
inline constexpr abi::plugin_version k_provider_version{1, 0, 0};
/** @brief Exact version range a consumer must accept to lease those providers. */
inline constexpr abi::version_range k_required = abi::exact_version(k_provider_version);
/** @brief Numeric ids of the two methods a callable provider fake publishes. */
inline constexpr abi::method_id k_ping = 1;
inline constexpr abi::method_id k_echo = 2;

/* ------------------------------------------------------------------ *
 * Fakes
 * ------------------------------------------------------------------ */

class re_factory;

/** @brief Behavior switches of one fake plugin instance. */
struct re_behavior {
    /** Advertise capabilities during start(). */
    bool announce = false;
    /** Plugin business version published by describe() when @ref announce is set. */
    abi::plugin_version version = k_provider_version;
    /** Also advertise one callable method ("ping"), making this instance a callable provider. */
    bool announce_method = false;
    /** Register a capability watch during init(). */
    bool watch_in_init = false;
    /** Ask the captured host to unload @ref unload_target from inside invoke(). */
    bool request_on_invoke = false;
    /** Ask the captured host to unload @ref unload_target on an availability notice. */
    bool request_on_available = false;
    /** Ask the captured host to unload @ref unload_target on a withdrawal notice. */
    bool request_on_withdrawal = false;
    /** Ask the captured host to unload this plugin's own identity from inside create(). */
    bool request_on_create = false;
    /** Queue an unload during init() or start(), before the batch is complete. */
    bool request_on_init = false;
    bool request_on_start = false;
    abi::status start_status = abi::ok;
    /** Ask the captured host to shut the whole rack down from inside invoke(). */
    bool request_shutdown = false;
    /** Call the structural host APIs (add/boot/load) from inside invoke(). */
    bool request_structure_change = false;
    /** Factory handed to host::add() from inside invoke(); never staged otherwise. */
    re_factory* add_target = nullptr;
    /** Initialization outcome; a non-ok value makes start() roll the batch back. */
    abi::status init_status = abi::ok;
    /** Initialization priority; lower runs first. */
    std::int32_t priority = 0;
    /** Identity the reentrant unload requests and the lease refer to. */
    std::string unload_target;
    std::string acquire_target;
    /** Inclusive version range this consumer requires for @ref acquire_target. */
    abi::version_range required = k_required;
    /** Never return the lease credential from on_revoke(); the test clears this to release it. */
    bool hold_lease = false;
    /** Hard initialization edges, used to build a rejected (cyclic) plan. */
    std::vector<std::string> before;
    std::vector<std::string> after;
};

/** @brief Instance-side fake plugin: provider, capability consumer and contract consumer. */
class re_plug final : public abi::iplug, public abi::iinvoke, public abi::icap_sink {
public:
    explicit re_plug(re_factory& owner) noexcept;

    // iplug
    abi::status U42_CALL init(abi::ictx* ctx) noexcept override;
    abi::status U42_CALL start() noexcept override;
    abi::status U42_CALL stop() noexcept override;
    void U42_CALL destroy() noexcept override;
    abi::status U42_CALL query(const abi::iid* type, void** out) noexcept override;
    // iinvoke
    abi::status U42_CALL invoke(abi::method_id method, abi::bytes args,
                                abi::iwriter* result) noexcept override;
    // icap_sink
    void U42_CALL on_capability(const abi::cap_event* value) noexcept override;

    /**
     * @brief Lease the configured provider contract from test code.
     *
     * @return The acquire() status; only valid once this instance is initialized or active.
     * @note Test-side driving, mirroring the consumer role an integration would perform. The
     *       returned borrow carries a credential only; binding happens through the same context.
     */
    abi::status acquire_now();

    const re_behavior& behavior() const noexcept;

    /** @brief Statuses returned by the reentrant `u42::host::unload()` calls, in order. */
    std::vector<abi::status> unload_requests;
    /** @brief Statuses returned by the reentrant `u42::host::shutdown()` calls, in order. */
    std::vector<abi::status> shutdown_requests;
    /** @brief Statuses returned by the reentrant structural calls from invoke(). */
    abi::status add_status = abi::failed;
    abi::status load_status = abi::failed;
    abi::status boot_status = abi::failed;
    std::size_t request_exceptions = 0;
    std::size_t availability_notices = 0;
    std::size_t withdrawal_notices = 0;
    std::size_t revoke_calls = 0;
    std::uint64_t init_calls = 0;
    std::uint64_t start_calls = 0;
    std::uint64_t stop_calls = 0;
    std::uint64_t destroy_calls = 0;
    std::uint64_t invoke_calls = 0;
    bool saw_available = false;
    bool saw_withdrawal = false;
    bool held_lease = false;

private:
    /** @brief Revocation receiver owned by this instance; forwards to icaps::release(). */
    struct revoker final : abi::irevoker {
        re_plug* owner = nullptr;
        void U42_CALL on_revoke(abi::token credential) noexcept override;
    };

    /** @brief Ask the captured host to unload one identity; never lets an exception escape. */
    void request_unload(const char* provider_id) noexcept;

    re_factory& owner_;
    abi::ictx* ctx_ = nullptr;
    abi::icaps* caps_ = nullptr;
    revoker revoker_{};
    abi::borrow lease_{};
    abi::token watch_{};
};

/**
 * @brief Stable administration-side revocation receiver for the native lease triggers.
 *
 * @note The registered address must stay valid until the credential is returned, so each case
 *       declares a named local rather than passing a temporary. It returns whatever credential it
 *       is handed so a provider unload can complete; a refused release would leave the lease in
 *       place and is deliberately not retried here.
 */
struct host_revoker final : abi::irevoker {
    u42::host* host = nullptr;
    [[maybe_unused]] std::uint64_t calls = 0;
    explicit host_revoker(u42::host* value) noexcept : host(value) {}
    void U42_CALL on_revoke(abi::token credential) noexcept override
    {
        ++calls;
        if (host == nullptr) return;
        try {
            (void)host->release(credential);
        } catch (...) {
        }
    }
};

/** @brief Library-owned fake factory with stable descriptor storage and instance ownership. */
class re_factory final : public abi::iplug_fty {
public:
    /**
     * @brief Build one fake plugin type with immutable metadata.
     *
     * @param plug_id Stable plugin identity; the descriptor borrows this storage.
     * @param behavior Per-instance behavior switches.
     */
    re_factory(std::string plug_id, re_behavior behavior);

    re_factory(const re_factory&) = delete;
    re_factory& operator=(const re_factory&) = delete;

    abi::status U42_CALL describe(const abi::plug_desc** out) noexcept override;
    abi::status U42_CALL create(abi::iplug** out) noexcept override;

    const std::string& id() const noexcept { return plug_id_; }
    const re_behavior& behavior() const noexcept { return behavior_; }
    /** @brief Stable capability description borrowed by the host during start(). */
    const abi::caps_desc& caps() const noexcept { return caps_; }
    /** @brief Instance owned by this factory; still intact after plugin-side destroy(). */
    re_plug* instance() const noexcept { return instance_.get(); }
    void set_behavior(const re_behavior& value) { behavior_ = value; }
    /** @brief Statuses returned by `u42::host::unload()` called from inside create(). */
    std::vector<abi::status> create_unload_requests;
    std::size_t create_request_exceptions = 0;
    /** @brief Host pointer handed to the fake so callbacks can call the public API again. */
    u42::host* host = nullptr;

private:
    std::string plug_id_;
    re_behavior behavior_;
    abi::plug_desc desc_{};
    std::vector<std::string> before_;
    std::vector<std::string> after_;
    std::vector<const char*> before_ptrs_;
    std::vector<const char*> after_ptrs_;
    std::string method_name_ = "ping";
    std::string echo_name_ = "echo";
    abi::method_desc method_desc_{};
    abi::method_desc echo_desc_{};
    std::vector<abi::method_desc> methods_;
    abi::caps_desc caps_{};
    std::unique_ptr<re_plug> instance_;
};

/* ------------------------------------------------------------------ *
 * Fake implementation
 * ------------------------------------------------------------------ */

static_assert(noexcept(std::declval<re_plug&>().stop()), "lifecycle entry points must be noexcept");
static_assert(noexcept(std::declval<re_plug&>().destroy()), "destroy must be noexcept");
static_assert(noexcept(std::declval<re_plug&>().invoke(0, abi::bytes{}, nullptr)),
              "invoke must be noexcept");
static_assert(noexcept(std::declval<re_plug&>().on_capability(nullptr)),
              "sink callbacks must be noexcept");

re_factory::re_factory(std::string plug_id, re_behavior behavior)
    : plug_id_(std::move(plug_id)), behavior_(std::move(behavior))
{
    before_ = behavior_.before;
    after_ = behavior_.after;
    before_ptrs_.clear();
    for (const std::string& target : before_) before_ptrs_.push_back(target.c_str());
    after_ptrs_.clear();
    for (const std::string& target : after_) after_ptrs_.push_back(target.c_str());

    desc_.struct_size = sizeof(abi::plug_desc);
    desc_.reserved = 0;
    desc_.plug_id = plug_id_.c_str();
    desc_.version = behavior_.version;
    desc_.priority = behavior_.priority;
    desc_.before_count = static_cast<std::uint32_t>(before_ptrs_.size());
    desc_.before = before_ptrs_.empty() ? nullptr : before_ptrs_.data();
    desc_.after_count = static_cast<std::uint32_t>(after_ptrs_.size());
    desc_.after = after_ptrs_.empty() ? nullptr : after_ptrs_.data();

    method_desc_.id = k_ping;
    method_desc_.name = method_name_.c_str();
    method_desc_.description = "queues a reentrant unload request";
    method_desc_.input_schema = "{}";
    method_desc_.output_schema = "{}";
    echo_desc_.id = k_echo;
    echo_desc_.name = echo_name_.c_str();
    echo_desc_.description = "second published probe method";
    echo_desc_.input_schema = "{}";
    echo_desc_.output_schema = "{}";
    methods_ = {method_desc_, echo_desc_};

    caps_.struct_size = sizeof(abi::caps_desc);
    caps_.method_count = behavior_.announce_method ? static_cast<std::uint32_t>(methods_.size()) : 0u;
    caps_.methods = behavior_.announce_method ? methods_.data() : nullptr;
}

abi::status U42_CALL re_factory::describe(const abi::plug_desc** out) noexcept
{
    if (out == nullptr) return abi::invalid_argument;
    *out = &desc_;
    return abi::ok;
}

abi::status U42_CALL re_factory::create(abi::iplug** out) noexcept
{
    if (out == nullptr) return abi::invalid_argument;
    *out = nullptr;
    if (instance_ == nullptr) instance_ = std::make_unique<re_plug>(*this);
    // create() runs with a plugin stack on the host, so this request is queued for the record that
    // was inserted just before the call; if that record is later abandoned, the request must go
    // with it instead of unloading whatever instance reuses the identity next.
    if (behavior_.request_on_create && host != nullptr) {
        try {
            create_unload_requests.push_back(host->unload(plug_id_.c_str()));
        } catch (...) {
            ++create_request_exceptions;
        }
    }
    *out = instance_.get();
    return abi::ok;
}

re_plug::re_plug(re_factory& owner) noexcept : owner_(owner) { revoker_.owner = this; }

const re_behavior& re_plug::behavior() const noexcept { return owner_.behavior(); }

abi::status U42_CALL re_plug::init(abi::ictx* ctx) noexcept
{
    ++init_calls;
    if (ctx == nullptr) return abi::invalid_argument;
    ctx_ = ctx;
    if (behavior().request_on_init) request_unload(behavior().unload_target.c_str());
    if (owner_.behavior().init_status != abi::ok) return owner_.behavior().init_status;

    void* service = nullptr;
    if (ctx->query(&abi::caps_iid, &service) == abi::ok && service != nullptr) {
        caps_ = static_cast<abi::icaps*>(service);
    }

    const re_behavior& behavior = owner_.behavior();
    if (behavior.watch_in_init) {
        if (caps_ == nullptr) return abi::invalid_state;
        const abi::status status = caps_->watch(this, &watch_);
        if (status != abi::ok) return status;
    }
    return abi::ok;
}

abi::status U42_CALL re_plug::start() noexcept
{
    ++start_calls;
    if (behavior().request_on_start) request_unload(behavior().unload_target.c_str());
    if (behavior().start_status != abi::ok) return behavior().start_status;
    if (!behavior().announce) return abi::ok;
    if (caps_ == nullptr) return abi::invalid_state;
    return caps_->announce(&owner_.caps());
}

abi::status U42_CALL re_plug::stop() noexcept
{
    ++stop_calls;
    return abi::ok;
}

void U42_CALL re_plug::destroy() noexcept
{
    ++destroy_calls;
    caps_ = nullptr; // Prove the instance returned to a silent, destroyable state.
    ctx_ = nullptr;
}

abi::status U42_CALL re_plug::query(const abi::iid* type, void** out) noexcept
{
    if (out == nullptr || type == nullptr) return abi::invalid_argument;
    *out = nullptr;
    // The host-private invoke gateway is the only interface this plugin ever provides; consumers
    // reach business methods through a leased icalls binding instead of a cross-plugin query.
    if (*type == abi::invoke_iid) {
        *out = static_cast<abi::iinvoke*>(this);
        return abi::ok;
    }
    return abi::unsupported;
}

abi::status U42_CALL re_plug::invoke(abi::method_id method, abi::bytes args,
                                     abi::iwriter* result) noexcept
{
    (void)method;
    (void)args;
    (void)result;
    ++invoke_calls;
    const re_behavior& behavior = owner_.behavior();
    // Depth is non-zero here (the host wrapped this invocation), so every request below exercises
    // a reentrant entry into the host from inside a plugin call.
    if (behavior.request_on_invoke) request_unload(behavior.unload_target.c_str());

    if (behavior.request_shutdown && owner_.host != nullptr) {
        try {
            shutdown_requests.push_back(owner_.host->shutdown());
        } catch (...) {
            ++request_exceptions;
        }
    }
    if (behavior.request_structure_change && owner_.host != nullptr) {
        try {
            if (behavior.add_target != nullptr) add_status = owner_.host->add(behavior.add_target);
            // Deliberately nonexistent locations: only the depth check runs before scan/open, so a
            // reentrant call must report busy instead of not_found.
            load_status = owner_.host->load("/nonexistent/42u/reentrant.u42.so");
            boot_status = owner_.host->boot("/nonexistent/42u/rack");
        } catch (...) {
            ++request_exceptions;
        }
    }
    return abi::ok;
}

void U42_CALL re_plug::on_capability(const abi::cap_event* value) noexcept
{
    if (value == nullptr) return;
    const bool available = value->available != 0;
    if (available) {
        ++availability_notices;
        saw_available = true;
    } else {
        ++withdrawal_notices;
        saw_withdrawal = true;
    }

    const char* provider = value->plug_id != nullptr ? value->plug_id : "";
    const re_behavior& behavior = owner_.behavior();
    if (behavior.unload_target.empty()) return;
    if (std::string_view(provider) != behavior.unload_target) return;
    if (available && !behavior.request_on_available) return;
    if (!available && !behavior.request_on_withdrawal) return;
    request_unload(provider);
}

abi::status re_plug::acquire_now()
{
    if (ctx_ == nullptr) return abi::invalid_state;
    void* service = nullptr;
    if (ctx_->query(&abi::caps_iid, &service) != abi::ok || service == nullptr) return abi::invalid_state;
    caps_ = static_cast<abi::icaps*>(service);
    const re_behavior& behavior = owner_.behavior();
    const abi::version_range required = behavior.required;
    const abi::status status =
        caps_->acquire(behavior.acquire_target.c_str(), &required, &revoker_, &lease_);
    held_lease = status == abi::ok && lease_.credential.value != 0;
    return status;
}

void re_plug::request_unload(const char* provider_id) noexcept
{
    if (owner_.host == nullptr) return;
    try {
        unload_requests.push_back(owner_.host->unload(provider_id));
    } catch (...) {
        ++request_exceptions; // A plugin callback must never let an exception cross the ABI.
    }
}

void U42_CALL re_plug::revoker::on_revoke(abi::token credential) noexcept
{
    if (owner == nullptr) return;
    ++owner->revoke_calls;
    if (owner->behavior().hold_lease) return; // Simulate a consumer ignoring the protocol.
    if (owner->caps_ == nullptr) return;
    if (owner->caps_->release(credential) == abi::ok) owner->held_lease = false;
}

/* ------------------------------------------------------------------ *
 * Test cases
 * ------------------------------------------------------------------ */

/** @brief Behavior of a provider that advertises one contract and accepts unload. */
re_behavior provider_behavior()
{
    re_behavior behavior;
    behavior.announce = true;
    behavior.version = k_provider_version;
    return behavior;
}

/**
 * @brief A request queued by a method invocation must be consumed by the safe-point retry.
 *
 * The invocation runs with a plugin stack on the host, so its request is queued. Nothing drains
 * afterwards (host::call does not dispatch), which leaves exactly the pending entry that the
 * retirement's own drain used to feed back into unload_one() - the double-teardown that is
 * asserted against here. The native trigger leases the contract, binds "ping" with the credential
 * and invokes through the host gateway.
 */
void invocation_request_then_safe_point()
{
    re_behavior provider_behavior_value = provider_behavior();
    provider_behavior_value.announce_method = true;
    provider_behavior_value.request_on_invoke = true;
    provider_behavior_value.unload_target = "invoke.provider";
    re_factory provider_factory("invoke.provider", provider_behavior_value);

    u42::host host;
    provider_factory.host = &host;

    CHECK(host.add(&provider_factory) == abi::ok);
    CHECK(host.start() == abi::ok);
    re_plug* provider = provider_factory.instance();
    CHECK(provider != nullptr);
    CHECK(provider->start_calls == 1);

    host_revoker revoker(&host);
    abi::borrow lease{};
    CHECK(host.acquire("invoke.provider", k_required, &revoker, &lease) == abi::ok);
    CHECK(lease.credential.value != 0);
    CHECK(lease.version == k_provider_version);
    std::string output;
    CHECK(host.call(lease.credential, "ping", abi::bytes{nullptr, 0}, &output) == abi::ok);
    CHECK(provider->invoke_calls == 1);
    CHECK(provider->unload_requests.size() == 1);
    CHECK(provider->unload_requests.front() == abi::deferred);
    CHECK(provider->request_exceptions == 0);
    CHECK(provider->stop_calls == 0);
    CHECK(provider->destroy_calls == 0);
    // The direct call is the whole business path; a released credential stays stale by name and id.
    CHECK(host.release(lease.credential) == abi::ok);
    CHECK(host.call(lease.credential, "ping", abi::bytes{nullptr, 0}, &output) == abi::stale);
    CHECK(host.call(lease.credential, k_ping, abi::bytes{nullptr, 0}, &output) == abi::stale);

    // Safe point: retry the identity that is still queued. One teardown, no re-entry.
    CHECK(host.unload("invoke.provider") == abi::ok);
    CHECK(provider->stop_calls == 1);
    CHECK(provider->destroy_calls == 1);
    CHECK(host.unload("invoke.provider") == abi::not_found);

    // The stale queued entry is pruned: polling stays clean instead of failing forever.
    for (int round = 0; round < 3; ++round) CHECK(host.poll() == abi::ok);
    CHECK(host.error().empty());
    CHECK(host.plugins().empty());
    CHECK(host.shutdown() == abi::ok);
}

/**
 * @brief A withdrawal callback asking for the unload of that same provider must not disturb it.
 *
 * The request arrives while the provider is already revoking, so the host refuses the nested
 * attempt, finishes the single teardown, and drops the unsatisfiable request on the next poll.
 */
void withdrawal_callback_request()
{
    re_factory provider_factory("re.provider", provider_behavior());
    re_behavior consumer_behavior;
    consumer_behavior.watch_in_init = true;
    consumer_behavior.request_on_withdrawal = true;
    consumer_behavior.unload_target = "re.provider";
    re_factory consumer_factory("re.consumer", consumer_behavior);

    u42::host host;
    provider_factory.host = &host;
    consumer_factory.host = &host;

    CHECK(host.add(&provider_factory) == abi::ok);
    CHECK(host.start() == abi::ok);
    CHECK(host.add(&consumer_factory) == abi::ok);
    CHECK(host.start() == abi::ok);
    re_plug* consumer = consumer_factory.instance();
    CHECK(consumer != nullptr);

    CHECK(host.poll() == abi::ok); // Deliver the availability snapshot: no request configured.
    CHECK(consumer->saw_available);
    CHECK(consumer->unload_requests.empty());

    CHECK(host.unload("re.provider") == abi::ok);
    re_plug* provider = provider_factory.instance();
    CHECK(provider != nullptr);
    CHECK(provider->stop_calls == 1);
    CHECK(provider->destroy_calls == 1);
    CHECK(consumer->saw_withdrawal);
    CHECK(consumer->unload_requests.size() == 1);
    CHECK(consumer->unload_requests.front() == abi::deferred);

    // Nothing can satisfy the queued request now; poll must prune it, not report not_found.
    CHECK(host.poll() == abi::ok);
    CHECK(host.error().empty());
    CHECK(host.poll() == abi::ok);
    CHECK(host.unload("re.provider") == abi::not_found); // The identity is gone for good.
    CHECK(host.poll() == abi::ok);                        // A later poll is still clean.
    CHECK(host.error().empty());
    CHECK(host.shutdown() == abi::ok);
    CHECK(consumer->destroy_calls == 1);
}

/**
 * @brief A request queued from an availability callback is served exactly once.
 *
 * Capability delivery still occurs during startup, but queued unloads wait until the batch
 * returns. An explicit unload then satisfies the queued request without leaving residue.
 */
void availability_callback_request_is_served_once()
{
    re_factory provider_factory("avail.provider", provider_behavior());
    re_behavior consumer_behavior;
    consumer_behavior.watch_in_init = true;
    consumer_behavior.request_on_available = true;
    consumer_behavior.unload_target = "avail.provider";
    re_factory consumer_factory("avail.consumer", consumer_behavior);

    u42::host host;
    provider_factory.host = &host;
    consumer_factory.host = &host;

    CHECK(host.add(&provider_factory) == abi::ok);
    CHECK(host.start() == abi::ok);
    CHECK(host.add(&consumer_factory) == abi::ok);
    CHECK(host.start() == abi::ok);

    re_plug* consumer = consumer_factory.instance();
    re_plug* provider = provider_factory.instance();
    CHECK(consumer != nullptr && provider != nullptr);
    CHECK(consumer->saw_available);
    CHECK(!consumer->unload_requests.empty());
    CHECK(consumer->unload_requests.front() == abi::deferred);

    CHECK(provider->stop_calls == 0);
    CHECK(provider->destroy_calls == 0);
    CHECK(host.plugins().size() == 2);
    const abi::status retry = host.unload("avail.provider");
    CHECK(retry == abi::ok);
    CHECK(provider->stop_calls == 1);
    CHECK(provider->destroy_calls == 1);

    CHECK(host.poll() == abi::ok);
    CHECK(host.error().empty());
    CHECK(host.unload("avail.provider") == abi::not_found);
    CHECK(host.poll() == abi::ok);
    CHECK(host.error().empty());
    const std::vector<std::string> ids = host.plugins();
    CHECK(ids.size() == 1);
    CHECK(!ids.empty() && ids[0] == "avail.consumer");

    CHECK(host.shutdown() == abi::ok);
    CHECK(consumer->stop_calls == 1);
    CHECK(consumer->destroy_calls == 1);
}

/**
 * @brief An unreturned contract lease keeps the provider revoking and unloadable after the return.
 *
 * This pins the retirement retry path: the first attempt must not stop or destroy anything, the
 * second attempt (after the consumer returns its credential) must complete exactly once. The
 * provider discloses a valid contract with no methods, so only the lifetime lock is borrowed.
 */
void unreturned_lease_blocks_then_releases()
{
    re_factory provider_factory("lease.provider", provider_behavior());
    re_behavior consumer_behavior;
    consumer_behavior.acquire_target = "lease.provider";
    consumer_behavior.hold_lease = true;
    re_factory consumer_factory("lease.consumer", consumer_behavior);

    u42::host host;
    provider_factory.host = &host;
    consumer_factory.host = &host;

    CHECK(host.add(&provider_factory) == abi::ok);
    CHECK(host.start() == abi::ok);
    CHECK(host.add(&consumer_factory) == abi::ok);
    CHECK(host.start() == abi::ok);
    re_plug* consumer = consumer_factory.instance();
    re_plug* provider = provider_factory.instance();
    CHECK(consumer != nullptr && provider != nullptr);

    CHECK(consumer->acquire_now() == abi::ok);
    CHECK(consumer->held_lease);

    CHECK(host.unload("lease.provider") == abi::busy);
    CHECK(provider->stop_calls == 0);
    CHECK(provider->destroy_calls == 0);
    CHECK(host.plugins().size() == 2);

    re_behavior released = consumer_factory.behavior();
    released.hold_lease = false; // The consumer now answers the revocation protocol.
    consumer_factory.set_behavior(released);

    CHECK(host.unload("lease.provider") == abi::ok);
    CHECK(provider->stop_calls == 1);
    CHECK(provider->destroy_calls == 1);
    CHECK(consumer->revoke_calls >= 2); // Notified by both attempts.

    CHECK(host.shutdown() == abi::ok);
    CHECK(consumer->stop_calls == 1);
    CHECK(consumer->destroy_calls == 1);
}

/**
 * @brief host::shutdown() from inside a plugin call is refused, then works at a safe point.
 *
 * The caller's own instance is part of the graph a shutdown would stop and destroy, so serving
 * the request in-callback would pull the stack out from under the plugin. The host must refuse
 * without marking itself as shutting down: a later, normal shutdown still tears everything down.
 */
void in_callback_shutdown_is_refused()
{
    re_behavior behavior = provider_behavior();
    behavior.announce_method = true;
    behavior.request_shutdown = true;
    re_factory factory("shutdown.provider", behavior);

    u42::host host;
    factory.host = &host;

    CHECK(host.add(&factory) == abi::ok);
    CHECK(host.start() == abi::ok);
    re_plug* provider = factory.instance();
    CHECK(provider != nullptr);

    host_revoker revoker(&host);
    abi::borrow lease{};
    CHECK(host.acquire("shutdown.provider", k_required, &revoker, &lease) == abi::ok);
    std::string output;
    CHECK(host.call(lease.credential, "ping", abi::bytes{nullptr, 0}, &output) == abi::ok);
    CHECK(host.release(lease.credential) == abi::ok);
    CHECK(host.call(lease.credential, "ping", abi::bytes{nullptr, 0}, &output) == abi::stale);
    CHECK(host.call(lease.credential, k_ping, abi::bytes{nullptr, 0}, &output) == abi::stale);

    CHECK(provider->shutdown_requests.size() == 1);
    CHECK(provider->shutdown_requests.front() == abi::busy);
    CHECK(provider->request_exceptions == 0);
    // A refused in-callback shutdown must not have stopped or destroyed anything.
    CHECK(provider->stop_calls == 0);
    CHECK(provider->destroy_calls == 0);
    CHECK(host.plugins().size() == 1);

    // Safe point: the same call now performs the normal teardown, and stays idempotent.
    CHECK(host.shutdown() == abi::ok);
    CHECK(provider->stop_calls == 1);
    CHECK(provider->destroy_calls == 1);
    CHECK(host.plugins().empty());
    CHECK(host.shutdown() == abi::ok);
}

/**
 * @brief A deferred request dies with its identity and never hits a later instance reusing it.
 *
 * The request is queued from create(), so the record exists while it is queued. The plan is then
 * rejected as cyclic and the whole batch is rolled back; a new instance that reuses the identity
 * must stay untouched by the abandoned request.
 */
void stale_deferred_request_dies_with_its_identity()
{
    re_behavior first_behavior = provider_behavior();
    first_behavior.request_on_create = true;
    first_behavior.before = {"gen.b"};
    re_factory first_factory("gen.a", first_behavior);

    re_behavior second_behavior = provider_behavior();
    second_behavior.before = {"gen.a"};
    re_factory second_factory("gen.b", second_behavior);

    u42::host host;
    first_factory.host = &host;
    second_factory.host = &host;

    CHECK(host.add(&first_factory) == abi::ok);
    CHECK(first_factory.create_unload_requests.size() == 1);
    CHECK(first_factory.create_unload_requests.front() == abi::deferred);
    CHECK(first_factory.create_request_exceptions == 0);
    CHECK(host.add(&second_factory) == abi::ok);

    CHECK(host.start() == abi::cycle);
    re_plug* first_instance = first_factory.instance();
    re_plug* second_instance = second_factory.instance();
    CHECK(first_instance != nullptr && second_instance != nullptr);
    CHECK(first_instance->destroy_calls == 1);
    CHECK(second_instance->destroy_calls == 1);
    CHECK(host.plugins().empty());

    re_behavior replacement_behavior = provider_behavior();
    replacement_behavior.announce_method = true;
    re_factory replacement_factory("gen.a", replacement_behavior);
    replacement_factory.host = &host;
    CHECK(host.add(&replacement_factory) == abi::ok);
    CHECK(host.start() == abi::ok);

    CHECK(host.poll() == abi::ok); // No stale entry may survive the rollback.
    re_plug* replacement = replacement_factory.instance();
    CHECK(replacement != nullptr);
    CHECK(replacement->stop_calls == 0);
    CHECK(replacement->destroy_calls == 0);

    host_revoker revoker(&host);
    abi::borrow lease{};
    CHECK(host.acquire("gen.a", k_required, &revoker, &lease) == abi::ok);
    std::string output;
    CHECK(host.call(lease.credential, "ping", abi::bytes{nullptr, 0}, &output) == abi::ok);
    CHECK(replacement->invoke_calls == 1);
    CHECK(host.release(lease.credential) == abi::ok);
    CHECK(host.call(lease.credential, "ping", abi::bytes{nullptr, 0}, &output) == abi::stale);
    CHECK(host.call(lease.credential, k_ping, abi::bytes{nullptr, 0}, &output) == abi::stale);
    CHECK(host.shutdown() == abi::ok);
    CHECK(replacement->destroy_calls == 1);
}

/**
 * @brief An unload request for an unknown identity is refused, never remembered.
 */
void unknown_request_is_not_queued()
{
    re_behavior behavior = provider_behavior();
    behavior.announce_method = true;
    behavior.request_on_invoke = true;
    behavior.unload_target = "ghost.plugin";
    re_factory factory("ghost.provider", behavior);

    u42::host host;
    factory.host = &host;

    CHECK(host.add(&factory) == abi::ok);
    CHECK(host.start() == abi::ok);
    re_plug* provider = factory.instance();
    CHECK(provider != nullptr);

    host_revoker revoker(&host);
    abi::borrow lease{};
    CHECK(host.acquire("ghost.provider", k_required, &revoker, &lease) == abi::ok);
    std::string output;
    CHECK(host.call(lease.credential, "ping", abi::bytes{nullptr, 0}, &output) == abi::ok);
    CHECK(host.release(lease.credential) == abi::ok);
    CHECK(host.call(lease.credential, "ping", abi::bytes{nullptr, 0}, &output) == abi::stale);
    CHECK(host.call(lease.credential, k_ping, abi::bytes{nullptr, 0}, &output) == abi::stale);

    CHECK(provider->unload_requests.size() == 1);
    CHECK(provider->unload_requests.front() == abi::not_found);
    CHECK(provider->request_exceptions == 0);

    // Nothing was queued, so the request cannot be applied to a later instance either.
    CHECK(host.poll() == abi::ok);
    CHECK(provider->stop_calls == 0);
    CHECK(provider->destroy_calls == 0);
    CHECK(host.unload("ghost.plugin") == abi::not_found);
    CHECK(host.shutdown() == abi::ok);
}

/**
 * @brief The structural host APIs are refused inside a plugin call, before any side effect.
 *
 * The load/boot targets do not exist on disk, so a busy status proves the depth check runs before
 * any scan or open rather than the call failing for a missing file.
 */
void structural_changes_are_refused_inside_callbacks()
{
    re_behavior side_behavior = provider_behavior();
    re_factory side_factory("side.plugin", side_behavior);

    re_behavior behavior = provider_behavior();
    behavior.announce_method = true;
    behavior.request_structure_change = true;
    behavior.add_target = &side_factory;
    re_factory factory("struct.provider", behavior);

    u42::host host;
    factory.host = &host;

    CHECK(host.add(&factory) == abi::ok);
    CHECK(host.start() == abi::ok);
    re_plug* provider = factory.instance();
    CHECK(provider != nullptr);

    host_revoker revoker(&host);
    abi::borrow lease{};
    CHECK(host.acquire("struct.provider", k_required, &revoker, &lease) == abi::ok);
    std::string output;
    // One credential reaches every published method: by name and by numeric id.
    CHECK(host.call(lease.credential, "ping", abi::bytes{nullptr, 0}, &output) == abi::ok);
    CHECK(host.call(lease.credential, k_echo, abi::bytes{nullptr, 0}, &output) == abi::ok);
    CHECK(host.call(lease.credential, "missing", abi::bytes{nullptr, 0}, &output) == abi::not_found);
    CHECK(host.release(lease.credential) == abi::ok);
    CHECK(host.call(lease.credential, "ping", abi::bytes{nullptr, 0}, &output) == abi::stale);
    CHECK(host.call(lease.credential, k_ping, abi::bytes{nullptr, 0}, &output) == abi::stale);

    CHECK(provider->add_status == abi::busy);
    CHECK(provider->load_status == abi::busy);
    CHECK(provider->boot_status == abi::busy);
    CHECK(provider->request_exceptions == 0);
    // The refused calls left no staged record and never reached the side factory.
    CHECK(side_factory.instance() == nullptr);
    CHECK(host.plugins().size() == 1);
    CHECK(host.poll() == abi::ok);
    CHECK(host.shutdown() == abi::ok);
}

/**
 * @brief A failed batch must leave the batch gate clear for later batches and callbacks.
 */
void batch_flag_clears_after_failed_batches()
{
    // Planning failure: cyclic constraints are rejected before any init() call.
    re_behavior cyclic_first = provider_behavior();
    cyclic_first.before = {"flag.b"};
    re_factory cyclic_first_factory("flag.a", cyclic_first);
    re_behavior cyclic_second = provider_behavior();
    cyclic_second.before = {"flag.a"};
    re_factory cyclic_second_factory("flag.b", cyclic_second);

    u42::host host;
    CHECK(host.add(&cyclic_first_factory) == abi::ok);
    CHECK(host.add(&cyclic_second_factory) == abi::ok);
    CHECK(host.start() == abi::cycle);
    CHECK(host.plugins().empty());

    // Initialization failure: the good instance initializes first and is stopped again, the
    // failing one is destroyed directly, and the batch gate must be released either way.
    re_behavior good = provider_behavior();
    good.priority = -1;
    re_factory good_factory("flag.good", good);
    re_behavior bad = provider_behavior();
    bad.init_status = abi::invalid_state;
    re_factory bad_factory("flag.bad", bad);
    CHECK(host.add(&good_factory) == abi::ok);
    CHECK(host.add(&bad_factory) == abi::ok);
    CHECK(host.start() == abi::invalid_state);
    CHECK(good_factory.instance() != nullptr && bad_factory.instance() != nullptr);
    CHECK(good_factory.instance()->init_calls == 1);
    CHECK(good_factory.instance()->stop_calls == 1);
    CHECK(good_factory.instance()->destroy_calls == 1);
    CHECK(bad_factory.instance()->init_calls == 1);
    CHECK(bad_factory.instance()->stop_calls == 0);
    CHECK(bad_factory.instance()->destroy_calls == 1);
    CHECK(host.plugins().empty());

    // A later batch still initializes, starts, and has its lifecycle notices dispatched: a stuck
    // batch gate would either reject this start or silently suppress the notification below.
    re_behavior consumer_behavior;
    consumer_behavior.watch_in_init = true;
    re_factory consumer_factory("flag.consumer", consumer_behavior);
    CHECK(host.add(&consumer_factory) == abi::ok);
    CHECK(host.start() == abi::ok);
    re_plug* consumer = consumer_factory.instance();
    CHECK(consumer != nullptr);

    re_factory late_factory("flag.late", provider_behavior());
    CHECK(host.add(&late_factory) == abi::ok);
    CHECK(host.start() == abi::ok);
    CHECK(host.poll() == abi::ok);
    CHECK(consumer->availability_notices >= 1);
    CHECK(consumer->saw_available);

    CHECK(host.shutdown() == abi::ok);
    CHECK(consumer->destroy_calls == 1);
}

/**
 * @brief Self-unload from create/init/start cannot turn successful startup into an empty rack.
 */
void startup_self_unload_waits_for_poll()
{
    for (int phase = 0; phase != 3; ++phase) {
        re_behavior behavior = provider_behavior();
        behavior.request_on_create = phase == 0;
        behavior.request_on_init = phase == 1;
        behavior.request_on_start = phase == 2;
        behavior.unload_target = "batch.self";
        re_factory factory("batch.self", behavior);
        u42::host host;
        factory.host = &host;
        CHECK(host.add(&factory) == abi::ok);
        CHECK(host.start() == abi::ok);
        re_plug* instance = factory.instance();
        CHECK(instance->init_calls == 1);
        CHECK(instance->start_calls == 1);
        CHECK(instance->stop_calls == 0);
        CHECK(instance->destroy_calls == 0);
        const auto& requests = phase == 0 ? factory.create_unload_requests : instance->unload_requests;
        CHECK(requests.size() == 1 && requests[0] == abi::deferred);
        CHECK(host.plugins() == std::vector<std::string>{"batch.self"});
        CHECK(host.poll() == abi::ok);
        CHECK(instance->stop_calls == 1);
        CHECK(instance->destroy_calls == 1);
        CHECK(host.plugins().empty());
        CHECK(host.poll() == abi::ok);
        CHECK(host.shutdown() == abi::ok);
    }
}

/**
 * @brief A queued peer unload cannot skip a later planned start or remove an earlier provider.
 */
void startup_peer_unload_waits_for_poll()
{
    for (bool target_starts_first : {false, true}) {
        re_behavior requester = provider_behavior();
        requester.priority = target_starts_first ? 1 : -1;
        requester.request_on_start = true;
        requester.unload_target = "batch.target";
        re_factory source("batch.source", requester);
        re_factory target("batch.target", provider_behavior());
        u42::host host;
        source.host = &host;
        CHECK(host.add(&source) == abi::ok);
        CHECK(host.add(&target) == abi::ok);
        CHECK(host.start() == abi::ok);
        CHECK(source.instance()->unload_requests == std::vector<abi::status>{abi::deferred});
        CHECK(target.instance()->init_calls == 1);
        CHECK(target.instance()->start_calls == 1);
        CHECK(target.instance()->stop_calls == 0);
        CHECK(target.instance()->destroy_calls == 0);
        CHECK(host.plugins().size() == 2);
        CHECK(host.poll() == abi::ok);
        CHECK(target.instance()->stop_calls == 1);
        CHECK(target.instance()->destroy_calls == 1);
        CHECK(host.plugins() == std::vector<std::string>{"batch.source"});
        CHECK(host.shutdown() == abi::ok);
    }
}

/**
 * @brief Failure rollback does not execute queued unloads of established instances.
 *
 * Requests for destroyed batch identities disappear; requests for existing instances remain
 * real queued operations, and the batch gate must release even when start() fails.
 */
void startup_failure_preserves_deferred_boundaries()
{
    re_factory established("batch.established", provider_behavior());
    re_behavior first = provider_behavior();
    first.priority = -1;
    first.request_on_start = true;
    first.unload_target = "batch.established";
    re_factory requester("batch.requester", first);
    re_behavior bad = provider_behavior();
    bad.request_on_start = true;
    bad.unload_target = "batch.bad";
    bad.start_status = abi::failed;
    re_factory failing("batch.bad", bad);
    re_factory replacement("batch.bad", provider_behavior());
    u42::host host;
    requester.host = &host;
    failing.host = &host;
    CHECK(host.add(&established) == abi::ok);
    CHECK(host.start() == abi::ok);
    CHECK(host.add(&requester) == abi::ok);
    CHECK(host.add(&failing) == abi::ok);
    CHECK(host.start() == abi::failed);
    CHECK(requester.instance()->unload_requests == std::vector<abi::status>{abi::deferred});
    CHECK(failing.instance()->unload_requests == std::vector<abi::status>{abi::deferred});
    CHECK(requester.instance()->destroy_calls == 1);
    CHECK(failing.instance()->destroy_calls == 1);
    CHECK(established.instance()->stop_calls == 0);
    CHECK(host.plugins() == std::vector<std::string>{"batch.established"});
    CHECK(host.add(&replacement) == abi::ok);
    CHECK(host.start() == abi::ok);
    CHECK(host.plugins().size() == 2);
    CHECK(host.poll() == abi::ok);
    CHECK(established.instance()->destroy_calls == 1);
    CHECK(replacement.instance()->destroy_calls == 0);
    CHECK(host.plugins() == std::vector<std::string>{"batch.bad"});
    CHECK(host.shutdown() == abi::ok);
}

/** @brief A deferred unload blocked by a live contract lease remains queued for the next poll. */
void deferred_unload_retries_after_busy()
{
    re_behavior provider = provider_behavior();
    provider.request_on_start = true;
    provider.unload_target = "retry.provider";
    re_factory provider_factory("retry.provider", provider);
    re_behavior consumer;
    consumer.acquire_target = "retry.provider";
    consumer.hold_lease = true;
    re_factory consumer_factory("retry.consumer", consumer);
    u42::host host;
    provider_factory.host = &host;
    CHECK(host.add(&provider_factory) == abi::ok);
    CHECK(host.add(&consumer_factory) == abi::ok);
    CHECK(host.start() == abi::ok);
    CHECK(consumer_factory.instance()->acquire_now() == abi::ok);
    CHECK(host.poll() == abi::busy);
    CHECK(provider_factory.instance()->stop_calls == 0);
    CHECK(provider_factory.instance()->destroy_calls == 0);
    CHECK(consumer_factory.instance()->revoke_calls == 1);
    consumer.hold_lease = false;
    consumer_factory.set_behavior(consumer);
    CHECK(host.poll() == abi::ok);
    CHECK(consumer_factory.instance()->revoke_calls == 2);
    CHECK(provider_factory.instance()->stop_calls == 1);
    CHECK(provider_factory.instance()->destroy_calls == 1);
    CHECK(host.plugins() == std::vector<std::string>{"retry.consumer"});
    CHECK(host.poll() == abi::ok);
    CHECK(host.shutdown() == abi::ok);
}

/** @brief An exhausted notification budget leaves deferred unloads for a later safe point. */
void dispatch_budget_does_not_bypass_deferred_queue()
{
    re_behavior provider = provider_behavior();
    provider.request_on_start = true;
    provider.unload_target = "budget.provider";
    re_factory provider_factory("budget.provider", provider);
    re_behavior consumer;
    consumer.watch_in_init = true;
    re_factory consumer_factory("budget.consumer", consumer);
    u42::host_options options;
    options.dispatch_budget = 0;
    u42::host host(options);
    provider_factory.host = &host;
    CHECK(host.add(&provider_factory) == abi::ok);
    CHECK(host.add(&consumer_factory) == abi::ok);
    CHECK(host.start() == abi::ok);
    CHECK(host.poll() == abi::limit_exceeded);
    CHECK(provider_factory.instance()->stop_calls == 0);
    CHECK(provider_factory.instance()->destroy_calls == 0);
    CHECK(host.plugins().size() == 2);
    // Explicit teardown is still legal; a zero automatic budget does not forbid control actions.
    CHECK(host.unload("budget.provider") == abi::ok);
    CHECK(host.shutdown() == abi::ok);
}

} // namespace

int main()
{
    deferred_unload_retries_after_busy();
    dispatch_budget_does_not_bypass_deferred_queue();
    startup_self_unload_waits_for_poll();
    startup_peer_unload_waits_for_poll();
    startup_failure_preserves_deferred_boundaries();
    invocation_request_then_safe_point();
    withdrawal_callback_request();
    availability_callback_request_is_served_once();
    unreturned_lease_blocks_then_releases();
    in_callback_shutdown_is_refused();
    stale_deferred_request_dies_with_its_identity();
    unknown_request_is_not_queued();
    structural_changes_are_refused_inside_callbacks();
    batch_flag_clears_after_failed_batches();

    std::printf("%zu reentrant checks passed\n", g_checks - g_failures);
    return g_failures == 0 ? 0 : 1;
}
