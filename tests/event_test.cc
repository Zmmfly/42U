/**
 * @file event_test.cc
 * @brief Self-contained checks for the queued event service, its thread affinity and the
 *        callback-time subscription rules exposed by u42::host; no test framework.
 *
 * The file owns its main() and links only against the host sources, so it can be built as:
 *   g++ -std=c++17 -Wall -Wextra -Werror -Iinc src/events.cc src/context.cc src/order.cc
 *       src/plug.cc src/host.cc tests/event_test.cc -o /tmp/u42-event-test
 *
 * Every fixture is an in-process plugin factory handed to u42::host::add(), so no shared object,
 * no framework and no private header is involved: the checks only use the public ABI in
 * 42u/abi.hpp plus the header-only helpers of 42u/sdk.hpp.
 *
 * @note NDEBUG is defined deliberately: CHECK must keep reporting failures even when assert()
 *       has been compiled out, so a diagnostic can never vanish in release builds.
 *
 * @note The fixtures live in this file and observe the host through a hub that outlives both the
 *       factory and the host. A consumer that was unloaded is only ever inspected through that
 *       hub, never through the pointer the host destroyed, and no fixture keeps a pointer to a
 *       record, context or instance beyond the callback that borrowed it.
 */

#define NDEBUG 1

#include <42u/host.hpp>
#include <42u/sdk.hpp>

#include <atomic>
#include <cassert>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace abi = u42::abi::v1;
namespace sdk = u42::sdk;

const char* g_current_test = nullptr;
using check_hook = void (*)(const char* expr, const char* file, int line);
check_hook g_check_hook = nullptr;

/**
 * @brief Report one failed CHECK and stop the process with a non-zero status.
 *
 * @param expr Failed expression text.
 * @param file Source file of the failed CHECK.
 * @param line Source line of the failed CHECK.
 */
[[noreturn]] void fail_check(const char* expr, const char* file, int line)
{
    if (g_check_hook != nullptr) g_check_hook(expr, file, line);
    std::fprintf(stderr, "CHECK failed in %s: %s (%s:%d)\n",
                 g_current_test != nullptr ? g_current_test : "?", expr, file, line);
    std::fflush(stderr);
    std::exit(EXIT_FAILURE);
}

/** @brief Failure reporting that never compiles away, unlike assert(). */
#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) fail_check(#condition, __FILE__, __LINE__);            \
    } while (false)

#define U42_STRINGIFY(...) #__VA_ARGS__
#define U42_EXPAND_STRING(...) U42_STRINGIFY(__VA_ARGS__)

/**
 * @brief Compile-time guard: CHECK must expand to a call of fail_check, never to assert().
 *
 * With NDEBUG defined, assert() collapses to ((void)0), so every failure would silently
 * disappear. This is checked on the expansion text because a runtime probe cannot detect it: if
 * CHECK had become a no-op, the probe itself would be a no-op as well.
 */
inline constexpr std::string_view u42_check_expansion = U42_EXPAND_STRING(CHECK(0 == 1));
static_assert(u42_check_expansion.find("fail_check") != std::string_view::npos,
              "CHECK must call fail_check directly; defining it as assert() disables it "
              "under NDEBUG");

struct test_case {
    const char* name;
    void (*run)();
};

std::vector<test_case>& tests()
{
    static std::vector<test_case> all;
    return all;
}

struct registrar {
    registrar(const char* name, void (*run)()) { tests().push_back(test_case{name, run}); }
};

/** @brief Register one case without any test framework. */
#define TEST_CASE(name)                                                        \
    void name();                                                               \
    [[maybe_unused]] const registrar reg_##name(#name, &name);                 \
    void name()

/**
 * @brief Failure reporting shared with the checks that print the actual status.
 *
 * A bare CHECK would only name the expression, so a status mismatch is reported with both values.
 *
 * @param label Case-specific description of the call under test.
 * @param actual Status the host returned.
 * @param expected Status the documented contract requires.
 */
void expect_status(const char* label, abi::status actual, abi::status expected)
{
    if (actual == expected) return;
    std::fprintf(stderr, "%s: status %u != expected %u\n", label, actual, expected);
    std::fflush(stderr);
    fail_check(label, __FILE__, __LINE__);
}

/**
 * @brief Observations shared by every fixture of one case; owned by the test body.
 *
 * The hub outlives the host, the factory and the plugin instances, which is what makes it legal to
 * inspect the result of an unload: the counters are the test's, and the destroyed instance is
 * never touched again. Only the control thread writes here, so plain counters are enough.
 */
struct hub {
    int starts = 0;          //!< Number of start() calls seen by the fixtures.
    int stops = 0;           //!< Number of stop() calls; a destroyed instance proves quiescence.
    int destroys = 0;        //!< Number of destroy() calls; nothing may be inspected afterwards.
    int events = 0;          //!< Number of delivered on_event() callbacks.
    int nesting = 0;         //!< Current callback depth, so recursion is observable.
    int max_nesting = 0;     //!< Deepest callback nesting reached in this case.
    int inline_callbacks = 0; //!< Callbacks delivered while init()/start() was on the stack.
    int failures = 0;        //!< Problems recorded from noexcept callbacks instead of throwing.
    int cap_events = 0;      //!< Number of delivered on_capability() callbacks.
    int cap_available = 0;   //!< Capability notices that reported an available provider.
    int cap_withdrawn = 0;   //!< Capability notices that reported a withdrawal.
    int cancel_calls = 0;    //!< Callback-driven unsubscriptions of other subscriptions.
    abi::status cancel_status = abi::ok; //!< Status of the last callback-driven unsubscription.
    bool in_lifecycle = false;           //!< True while init()/start()/stop() is on the stack.
    std::vector<std::string> names;      //!< Event names in delivery order.
    std::vector<std::string> payloads;   //!< Copies of the delivered payloads, in delivery order.
};

/**
 * @brief Declarative description of one fixture instance.
 *
 * The specification is copied into the factory and outlives every instance, so the borrowed
 * strings of plug_desc and the fixture's own behaviour stay valid for the whole host lifetime.
 */
struct fixture_spec {
    std::string plug_id;                 //!< Identity the host registers.
    std::string version = "1.0";         //!< Version reported by describe().
    std::int32_t priority = 0;           //!< Ordering priority of the fixture.
    std::vector<std::string> before;     //!< Identities this fixture must precede.
    std::vector<std::string> after;      //!< Identities this fixture must follow.
    std::vector<std::string> subscribe_names; //!< Event names subscribed during init().
    bool watch_capabilities = false;     //!< Register a capability watch during init().
    std::string cancel_peer_on;          //!< Event whose callback removes the other subscriptions.
    std::string self_publish_name;       //!< Event name republished from its own callback.
    int self_publish_limit = 0;          //!< How often a callback may republish that event.
};

/**
 * @brief Build a specification with an identity and the names subscribed during init().
 *
 * @param plug_id Registered identity of the fixture.
 * @param subscribe_names Names subscribed in init(); empty for a pure publisher or watcher.
 * @return The completed specification, ready to be moved into a fixture_factory.
 */
fixture_spec make_spec(std::string plug_id, std::vector<std::string> subscribe_names = {})
{
    fixture_spec spec;
    spec.plug_id = std::move(plug_id);
    spec.subscribe_names = std::move(subscribe_names);
    return spec;
}

/**
 * @brief Test-only plugin that exercises the event, capability and lifetime contracts.
 *
 * The instance is a plugin, an event sink and a capability sink at once, so the host reaches both
 * sink subobjects through the ABI's plain void pointer conversion. Everything it observes is
 * written into the hub, never into a CHECK: the callbacks are noexcept and must not throw, unwind
 * into the host, or end the process; they only record what happened.
 */
class fixture_plugin final : public abi::iplug,
                             public abi::ievent_sink,
                             public abi::icap_sink {
public:
    /**
     * @brief Bind the instance to its immutable specification and its observation hub.
     *
     * @param spec Stable specification owned by the factory; borrowed for the instance lifetime.
     * @param shared Hub owned by the test body, which outlives this instance.
     */
    fixture_plugin(const fixture_spec& spec, hub& shared) : spec_(spec), shared_(shared) {}

    /**
     * @brief Query the host services and register the configured subscriptions and watch.
     *
     * @param ctx Borrowed host context, valid from this call until destroy() returns.
     * @return ok, or the failing status of the query/subscribe/watch call; a failed init() leaves
     *         an instance that can still be destroyed without side effects.
     */
    abi::status U42_CALL init(abi::ictx* ctx) noexcept override
    {
        shared_.in_lifecycle = true;
        abi::status outcome = abi::ok;
        try {
            outcome = sdk::query<abi::ievents>(ctx, abi::events_iid, &events_);
            if (outcome == abi::ok) outcome = sdk::query<abi::icaps>(ctx, abi::caps_iid, &caps_);
            if (outcome == abi::ok) {
                for (const std::string& name : spec_.subscribe_names) {
                    abi::token value{};
                    const abi::status added = events_->subscribe(name.c_str(), this, &value);
                    if (added != abi::ok) {
                        outcome = added;
                        break;
                    }
                    subs_.push_back(subscription{name, value});
                }
            }
            if (outcome == abi::ok && spec_.watch_capabilities) {
                outcome = caps_->watch(this, &watch_);
            }
        } catch (...) {
            outcome = abi::failed; // Allocation failure: report it instead of unwinding.
        }
        if (outcome != abi::ok) release_registrations();
        shared_.in_lifecycle = false;
        return outcome;
    }

    /**
     * @brief Announce an empty capability set, as a plugin without published interfaces does.
     *
     * @return The announce status; a non-ok status is recorded in the hub before it is returned.
     */
    abi::status U42_CALL start() noexcept override
    {
        shared_.in_lifecycle = true;
        ++shared_.starts;
        abi::status outcome = abi::invalid_state;
        try {
            abi::caps_desc announcement{}; // No interfaces and no methods by design.
            if (caps_ != nullptr) outcome = caps_->announce(&announcement);
        } catch (...) {
            outcome = abi::failed;
        }
        if (outcome != abi::ok) ++shared_.failures;
        shared_.in_lifecycle = false;
        return outcome;
    }

    /**
     * @brief Drop every registration; the fixture owns no thread, so success proves silence.
     *
     * @return Always ok: an already removed registration is expected during teardown and the
     *         fixture has no work of its own that could still be running.
     */
    abi::status U42_CALL stop() noexcept override
    {
        shared_.in_lifecycle = true;
        ++shared_.stops;
        release_registrations();
        shared_.in_lifecycle = false;
        return abi::ok;
    }

    /** @brief Record the destruction and release the instance on its allocating side. */
    void U42_CALL destroy() noexcept override
    {
        ++shared_.destroys;
        // iplug has a protected non-virtual destructor, so the concrete type is deleted here.
        delete this;
    }

    /**
     * @brief Report that this fixture publishes no optional interface.
     *
     * @param out Cleared output slot; a null value is tolerated.
     * @return Always unsupported, with the output cleared.
     */
    abi::status U42_CALL query(const abi::iid*, void** out) noexcept override
    {
        if (out != nullptr) *out = nullptr;
        return abi::unsupported;
    }

    /**
     * @brief Record one delivered event and run the callback-driven scenario of the specification.
     *
     * @param value Borrowed event view, valid only for this call; the payload is copied here,
     *              because nothing of it may be retained after the callback returns.
     *
     * @note noexcept by contract: every problem is recorded in the hub and nothing is thrown.
     */
    void U42_CALL on_event(const abi::event* value) noexcept override
    {
        if (shared_.in_lifecycle) ++shared_.inline_callbacks;
        ++shared_.nesting;
        if (shared_.nesting > shared_.max_nesting) shared_.max_nesting = shared_.nesting;
        try {
            if (value == nullptr || value->name == nullptr) {
                ++shared_.failures;
            } else {
                const std::string name = value->name;
                std::string payload;
                if (value->payload.size != 0 && value->payload.data != nullptr) {
                    payload.assign(static_cast<const char*>(value->payload.data),
                                   static_cast<std::size_t>(value->payload.size));
                }
                shared_.names.push_back(name);
                shared_.payloads.push_back(payload);
                ++shared_.events;

                if (!spec_.cancel_peer_on.empty() && name == spec_.cancel_peer_on) {
                    ++shared_.cancel_calls;
                    shared_.cancel_status = cancel_other_subscriptions(name);
                    if (shared_.cancel_status != abi::ok) ++shared_.failures;
                }
                if (!spec_.self_publish_name.empty() && name == spec_.self_publish_name &&
                    self_published_ < spec_.self_publish_limit) {
                    ++self_published_;
                    if (publish(name.c_str(), payload) != abi::ok) ++shared_.failures;
                }
            }
        } catch (...) {
            ++shared_.failures; // A callback must never let an exception reach the host.
        }
        --shared_.nesting;
    }

    /**
     * @brief Record one capability notice without calling any business method.
     *
     * @param value Borrowed notice, valid only for this call.
     *
     * @note noexcept by contract: only counters are updated, and allocation failures are recorded.
     */
    void U42_CALL on_capability(const abi::cap_event* value) noexcept override
    {
        if (shared_.in_lifecycle) ++shared_.inline_callbacks;
        try {
            ++shared_.cap_events;
            if (value != nullptr && value->available != 0) {
                ++shared_.cap_available;
            } else {
                ++shared_.cap_withdrawn;
            }
        } catch (...) {
            ++shared_.failures;
        }
    }

    /**
     * @brief Publish through the borrowed service exactly as a plugin would.
     *
     * @param name Event name; the host owns the reserved u42.* prefix.
     * @param data Borrowed payload bytes, copied by the host before publish() returns.
     * @return The publish status, or invalid_state before init() resolved the service.
     */
    abi::status publish_bytes(const char* name, abi::bytes data) noexcept
    {
        if (events_ == nullptr) return abi::invalid_state;
        return events_->publish(name, data);
    }

    /**
     * @brief Publish a borrowed string view, which the caller may reuse immediately afterwards.
     *
     * @param name Event name handed to publish_bytes().
     * @param payload Text whose characters are borrowed only for this call.
     * @return The publish status.
     */
    abi::status publish(const char* name, std::string_view payload) noexcept
    {
        return publish_bytes(name, sdk::view(payload));
    }

    /**
     * @brief Remove one of the fixture's own subscriptions by event name.
     *
     * @param name Subscribed event name to remove.
     * @return abi::v1::ok when removed, abi::v1::not_found when this fixture has no such
     *         subscription, otherwise the status from ievents::unsubscribe().
     */
    abi::status unsubscribe_named(const char* name) noexcept
    {
        if (events_ == nullptr) return abi::invalid_state;
        for (auto entry = subs_.begin(); entry != subs_.end(); ++entry) {
            if (entry->name != name) continue;
            const abi::status removed = events_->unsubscribe(entry->token);
            subs_.erase(entry);
            return removed;
        }
        return abi::not_found;
    }

    /** @brief Borrowed event service saved during init(), or null before init(). */
    abi::ievents* events() const noexcept { return events_; }

    /** @brief Borrowed capability service saved during init(), or null before init(). */
    abi::icaps* caps() const noexcept { return caps_; }

    /** @brief Stable address of the event sink handed to subscribe(). */
    abi::ievent_sink* sink() noexcept { return this; }

    /** @brief Credential of the capability watch, or zero when no watch was registered. */
    abi::token watch_token() const noexcept { return watch_; }

    /** @brief Number of subscriptions this fixture still owns. */
    std::size_t subscription_count() const noexcept { return subs_.size(); }

private:
    struct subscription {
        std::string name;  //!< Event name the token belongs to.
        abi::token token{}; //!< Credential returned by subscribe().
    };

    /**
     * @brief Remove the fixture's registrations, ignoring statuses.
     *
     * Used by init() rollback and by stop(), where a registration may already have been dropped by
     * the host. The fixture owns no callback target that could outlive it, so dropping the local
     * bookkeeping is enough.
     */
    void release_registrations() noexcept
    {
        try {
            if (events_ != nullptr) {
                for (const subscription& entry : subs_) (void)events_->unsubscribe(entry.token);
            }
            if (caps_ != nullptr && watch_.value != 0) (void)caps_->unwatch(watch_);
        } catch (...) {
            ++shared_.failures;
        }
        subs_.clear();
        watch_ = abi::token{};
    }

    /**
     * @brief Unsubscribe every other subscription from inside a callback.
     *
     * @param current Event name being delivered; its own subscription is kept.
     * @return ok, or the first status returned by ievents::unsubscribe().
     *
     * @note The credentials are snapshotted before anything is removed, so the dispatch pass that
     *       is currently running must tolerate registrations disappearing under it.
     */
    abi::status cancel_other_subscriptions(const std::string& current) noexcept
    {
        std::vector<abi::token> doomed;
        for (const subscription& entry : subs_) {
            if (entry.name != current) doomed.push_back(entry.token);
        }
        if (events_ == nullptr) return abi::invalid_state;
        abi::status outcome = abi::ok;
        for (const abi::token value : doomed) {
            const abi::status removed = events_->unsubscribe(value);
            if (removed != abi::ok) outcome = removed;
        }
        for (auto entry = subs_.begin(); entry != subs_.end();) {
            if (entry->name != current) {
                entry = subs_.erase(entry);
            } else {
                ++entry;
            }
        }
        return outcome;
    }

    fixture_spec spec_;        //!< Immutable behaviour of this instance.
    hub& shared_;              //!< Test-owned observations; outlives this instance.
    abi::ievents* events_ = nullptr;  //!< Borrowed service, valid until destroy() returns.
    abi::icaps* caps_ = nullptr;      //!< Borrowed service, valid until destroy() returns.
    std::vector<subscription> subs_;  //!< Subscriptions owned by this instance.
    abi::token watch_;                //!< Capability watch owned by this instance.
    int self_published_ = 0;          //!< Callback-driven republishes performed so far.
};

/**
 * @brief In-process factory that creates fixture instances and describes their constraints.
 *
 * The factory must outlive the host it was added to, and it must not be moved after construction:
 * plug_desc borrows the strings of the owned specification.
 */
class fixture_factory final : public abi::iplug_fty {
public:
    /**
     * @brief Take ownership of the specification and build the borrowed descriptor once.
     *
     * @param spec Behaviour of every instance this factory creates.
     * @param shared Test-owned hub passed on to each instance; must outlive the factory.
     */
    fixture_factory(fixture_spec spec, hub* shared) : spec_(std::move(spec)), shared_(shared)
    {
        desc_.plug_id = spec_.plug_id.c_str();
        desc_.version = spec_.version.c_str();
        desc_.priority = spec_.priority;
        before_ = to_pointers(spec_.before);
        after_ = to_pointers(spec_.after);
        desc_.before_count = static_cast<std::uint32_t>(before_.size());
        desc_.before = before_.empty() ? nullptr : before_.data();
        desc_.after_count = static_cast<std::uint32_t>(after_.size());
        desc_.after = after_.empty() ? nullptr : after_.data();
    }

    fixture_factory(const fixture_factory&) = delete;
    fixture_factory& operator=(const fixture_factory&) = delete;

    /**
     * @brief Hand out the immutable descriptor whose strings are owned by this factory.
     *
     * @param out Receives the borrowed descriptor; required.
     * @return ok, or invalid_argument for a null output.
     */
    abi::status U42_CALL describe(const abi::plug_desc** out) noexcept override
    {
        if (out == nullptr) return abi::invalid_argument;
        *out = &desc_;
        return abi::ok;
    }

    /**
     * @brief Create one uninitialized fixture instance.
     *
     * @param out Cleared first; receives the new instance on success.
     * @return ok, invalid_argument for a null output, or failed when the instance cannot be
     *         allocated; a failure leaves no resources behind.
     */
    abi::status U42_CALL create(abi::iplug** out) noexcept override
    {
        if (out == nullptr) return abi::invalid_argument;
        *out = nullptr;
        try {
            fixture_plugin* instance = new fixture_plugin(spec_, *shared_);
            last_ = instance;
            *out = instance;
            return abi::ok;
        } catch (...) {
            return abi::failed;
        }
    }

    /** @brief Most recently created instance, or null when create() never ran. */
    fixture_plugin* instance() const noexcept { return last_; }

private:
    /**
     * @brief Build the borrowed pointer array plug_desc expects for a constraint list.
     *
     * @param values Owned strings of the specification; the pointers stay valid as long as the
     *               factory's copy of the specification is unchanged.
     * @return One pointer per string, in input order.
     */
    static std::vector<const char*> to_pointers(const std::vector<std::string>& values)
    {
        std::vector<const char*> pointers;
        pointers.reserve(values.size());
        for (const std::string& value : values) pointers.push_back(value.c_str());
        return pointers;
    }

    fixture_spec spec_;                  //!< Owns every string desc_ borrows.
    hub* shared_;                        //!< Observation hub shared with the instances.
    abi::plug_desc desc_{};              //!< Immutable metadata returned by describe().
    std::vector<const char*> before_;    //!< Backing array for desc_.before.
    std::vector<const char*> after_;     //!< Backing array for desc_.after.
    fixture_plugin* last_ = nullptr;     //!< Latest instance, for the test's own calls.
};

/**
 * @brief One host with the single fixture factory it was staged from.
 *
 * Members are declared so that destruction runs host, then factory, then hub: the host may still
 * call into the instances and the factory must outlive it, while the hub must outlive both.
 */
class fixture_env {
public:
    /**
     * @brief Build the hub, the factory built from spec, and the host in that order.
     *
     * @param options Bounded resources handed to the host.
     * @param spec Behaviour of the fixture; ownership is transferred to the factory.
     */
    fixture_env(u42::host_options options, fixture_spec spec)
        : shared_(), factory_(std::move(spec), &shared_), host_(options)
    {
    }

    fixture_env(const fixture_env&) = delete;
    fixture_env& operator=(const fixture_env&) = delete;

    /**
     * @brief Stage the fixture and start the batch.
     *
     * @return ok when the fixture was created, initialized, started and had its (empty)
     *         capabilities committed.
     */
    abi::status stage_and_start()
    {
        const abi::status added = host_.add(&factory_);
        if (added != abi::ok) return added;
        return host_.start();
    }

    /** @brief Borrowed host under test. */
    u42::host& host() noexcept { return host_; }

    /** @brief Test-owned observations of every fixture built from this environment. */
    hub& observed() noexcept { return shared_; }

    /** @brief Latest fixture instance, or null before the batch was staged. */
    fixture_plugin* plugin() const noexcept { return factory_.instance(); }

private:
    hub shared_;            //!< Declared first so it is destroyed last.
    fixture_factory factory_; //!< Must outlive host_.
    u42::host host_;        //!< Destroyed first, which stops and destroys the instances.
};

TEST_CASE(publish_copies_the_payload_before_returning)
{
    fixture_env env({}, make_spec("fixture.copy", {"demo.copy"}));
    expect_status("stage and start", env.stage_and_start(), abi::ok);
    fixture_plugin* plugin = env.plugin();
    CHECK(plugin != nullptr);

    // The payload is borrowed only for the publish() call: reusing the source buffer immediately
    // afterwards must not change what the subscriber receives.
    std::string source = "first";
    expect_status("publish", plugin->publish("demo.copy", source), abi::ok);
    source.assign("changed");
    source[0] = 'X';

    CHECK(env.observed().events == 0); // publish() queues, it never dispatches
    expect_status("poll", env.host().poll(), abi::ok);
    CHECK(env.observed().events == 1);
    CHECK(env.observed().names.size() == 1);
    CHECK(env.observed().names[0] == "demo.copy");
    CHECK(env.observed().payloads.size() == 1);
    CHECK(env.observed().payloads[0] == "first");
    CHECK(env.observed().failures == 0);
}

TEST_CASE(publish_is_non_recursive_and_only_poll_delivers)
{
    fixture_env env({}, make_spec("fixture.queue", {"demo.queue"}));
    expect_status("stage and start", env.stage_and_start(), abi::ok);
    fixture_plugin* plugin = env.plugin();
    CHECK(plugin != nullptr);

    expect_status("first publish", plugin->publish("demo.queue", "one"), abi::ok);
    CHECK(env.observed().events == 0); // no callback ran inside publish()
    expect_status("second publish", plugin->publish("demo.queue", "two"), abi::ok);
    CHECK(env.observed().events == 0);

    expect_status("poll", env.host().poll(), abi::ok);
    CHECK(env.observed().events == 2);
    CHECK((env.observed().payloads == std::vector<std::string>{"one", "two"}));
    CHECK(env.observed().max_nesting == 1);
    CHECK(env.observed().failures == 0);
}

TEST_CASE(callback_publish_is_queued_not_recursive)
{
    fixture_spec spec = make_spec("fixture.tick", {"demo.tick"});
    spec.self_publish_name = "demo.tick";
    spec.self_publish_limit = 1;
    fixture_env env({}, spec);
    expect_status("stage and start", env.stage_and_start(), abi::ok);
    fixture_plugin* plugin = env.plugin();
    CHECK(plugin != nullptr);

    expect_status("seed publish", plugin->publish("demo.tick", "seed"), abi::ok);
    CHECK(env.observed().events == 0);

    expect_status("poll", env.host().poll(), abi::ok);
    if (env.observed().events < 2) expect_status("second poll", env.host().poll(), abi::ok);

    // The republished event was queued and only delivered after its producing callback returned:
    // a recursive delivery would have nested the callback and raised max_nesting above one.
    CHECK(env.observed().events == 2);
    CHECK(env.observed().max_nesting == 1);
    CHECK(env.observed().payloads.size() == 2);
    CHECK(env.observed().failures == 0);
}

TEST_CASE(reserved_and_malformed_publishes_are_rejected)
{
    fixture_env env({}, make_spec("fixture.reject", {"demo.reject"}));
    expect_status("stage and start", env.stage_and_start(), abi::ok);
    fixture_plugin* plugin = env.plugin();
    CHECK(plugin != nullptr);

    // u42.* belongs to the host lifecycle; an empty name and a null payload with a non-zero size
    // are malformed. None of them may be queued.
    expect_status("reserved name", plugin->publish("u42.lifecycle", "x"), abi::invalid_argument);
    expect_status("empty name", plugin->publish("", "x"), abi::invalid_argument);
    expect_status("null payload with size", plugin->publish_bytes("demo.reject", abi::bytes{nullptr, 4}),
                  abi::invalid_argument);

    CHECK(env.observed().events == 0);
    expect_status("poll", env.host().poll(), abi::ok);
    CHECK(env.observed().events == 0);
    CHECK(env.observed().failures == 0);
}

TEST_CASE(event_capacity_and_payload_limit_are_enforced)
{
    u42::host_options options;
    options.event_capacity = 3;
    options.payload_limit = 4;
    fixture_env env(options, make_spec("fixture.limits", {"demo.fill"}));
    expect_status("stage and start", env.stage_and_start(), abi::ok);
    fixture_plugin* plugin = env.plugin();
    CHECK(plugin != nullptr);

    expect_status("payload at the limit", plugin->publish("demo.fill", "abcd"), abi::ok);
    expect_status("second event", plugin->publish("demo.fill", "ab"), abi::ok);
    expect_status("third event", plugin->publish("demo.fill", "ab"), abi::ok);
    // The queue is full, and an oversized payload is refused even when there is room for it.
    expect_status("full queue", plugin->publish("demo.fill", "ab"), abi::limit_exceeded);
    expect_status("payload over the limit", plugin->publish("demo.fill", "abcde"), abi::limit_exceeded);

    expect_status("poll", env.host().poll(), abi::ok);
    CHECK(env.observed().events == 3);
    CHECK(env.observed().payloads.size() == 3);
    CHECK(env.observed().payloads[0] == "abcd"); // the byte-exact boundary payload survived

    // "Full" described the queue, not the service: it accepts work again once drained.
    expect_status("publish after drain", plugin->publish("demo.fill", "ab"), abi::ok);
    CHECK(env.observed().failures == 0);
}

TEST_CASE(unsubscribe_drops_already_queued_messages)
{
    fixture_env env({}, make_spec("fixture.unsub", {"demo.unsub"}));
    expect_status("stage and start", env.stage_and_start(), abi::ok);
    fixture_plugin* plugin = env.plugin();
    CHECK(plugin != nullptr);

    expect_status("publish", plugin->publish("demo.unsub", "pending"), abi::ok);
    expect_status("unsubscribe", plugin->unsubscribe_named("demo.unsub"), abi::ok);
    CHECK(plugin->subscription_count() == 0);

    // The message was queued while the subscription existed; removal must apply to the queue too.
    expect_status("poll", env.host().poll(), abi::ok);
    CHECK(env.observed().events == 0);

    expect_status("publish after unsubscribe", plugin->publish("demo.unsub", "after"), abi::ok);
    expect_status("second poll", env.host().poll(), abi::ok);
    CHECK(env.observed().events == 0);
    CHECK(env.observed().failures == 0);
}

TEST_CASE(callback_may_cancel_another_subscription)
{
    fixture_spec spec = make_spec("fixture.cancel", {"demo.a", "demo.b"});
    spec.cancel_peer_on = "demo.a";
    fixture_env env({}, spec);
    expect_status("stage and start", env.stage_and_start(), abi::ok);
    fixture_plugin* plugin = env.plugin();
    CHECK(plugin != nullptr);
    CHECK(plugin->subscription_count() == 2);

    expect_status("publish a", plugin->publish("demo.a", "a"), abi::ok);
    expect_status("publish b", plugin->publish("demo.b", "b"), abi::ok);

    // The callback for demo.a unsubscribes demo.b mid-dispatch: the registration disappearing
    // under the running pass must not crash, and the queued demo.b event must find no subscriber.
    expect_status("poll", env.host().poll(), abi::ok);
    CHECK(env.observed().cancel_calls == 1);
    CHECK(env.observed().cancel_status == abi::ok);
    CHECK(env.observed().events == 1);
    CHECK(env.observed().payloads.size() == 1);
    CHECK(env.observed().payloads[0] == "a");
    CHECK(plugin->subscription_count() == 1); // only the subscription being delivered survived

    expect_status("publish b again", plugin->publish("demo.b", "b2"), abi::ok);
    expect_status("second poll", env.host().poll(), abi::ok);
    CHECK(env.observed().events == 1);
    CHECK(env.observed().failures == 0);
}

TEST_CASE(unloading_a_consumer_drops_its_pending_callbacks)
{
    hub shared;
    fixture_spec consumer = make_spec("fixture.consumer", {"demo.unload"});
    consumer.after = {"fixture.publisher"};
    fixture_factory consumer_factory(consumer, &shared);
    fixture_factory publisher_factory(make_spec("fixture.publisher"), &shared);
    u42::host rack; // destroyed first: the factories and the hub outlive every host call

    expect_status("add consumer", rack.add(&consumer_factory), abi::ok);
    expect_status("add publisher", rack.add(&publisher_factory), abi::ok);
    expect_status("start", rack.start(), abi::ok);

    fixture_plugin* publisher = publisher_factory.instance();
    CHECK(publisher != nullptr);
    expect_status("publish", publisher->publish("demo.unload", "pending"), abi::ok);
    CHECK(shared.events == 0); // queued for the consumer, not delivered yet

    expect_status("unload consumer", rack.unload("fixture.consumer"), abi::ok);
    // The consumer instance no longer exists: from here on only the hub is inspected.
    CHECK(shared.stops == 1);
    CHECK(shared.destroys == 1);
    CHECK((rack.plugins() == std::vector<std::string>{"fixture.publisher"}));

    expect_status("poll", rack.poll(), abi::ok);
    CHECK(shared.events == 0); // the pending callback never reached the destroyed consumer
    CHECK(shared.failures == 0);
}

TEST_CASE(wrong_thread_publish_and_subscribe_are_rejected)
{
    fixture_env env({}, make_spec("fixture.thread"));
    expect_status("stage and start", env.stage_and_start(), abi::ok);
    fixture_plugin* plugin = env.plugin();
    CHECK(plugin != nullptr);
    abi::ievents* service = plugin->events();
    abi::ievent_sink* sink = plugin->sink();
    CHECK(service != nullptr);
    CHECK(sink != nullptr);

    constexpr std::uint32_t pending = 0xffffffffu;
    std::atomic<std::uint32_t> publish_status{pending};
    std::atomic<std::uint32_t> subscribe_status{pending};
    std::atomic<std::uint32_t> null_output_status{pending};
    std::atomic<std::uint64_t> token_value{pending};
    const std::string diagnostic_before = env.host().error();
    std::thread worker([&] {
        publish_status.store(service->publish("demo.thread", abi::bytes{nullptr, 0}));
        abi::token value{99};
        subscribe_status.store(service->subscribe("demo.thread", sink, &value));
        null_output_status.store(service->subscribe("demo.thread", sink, nullptr));
        token_value.store(value.value);
    });
    worker.join();

    // The service is control-thread only: both calls are refused before they touch the queue or
    // the subscription table, and a refused subscribe clears its credential slot.
    CHECK(publish_status.load() == abi::wrong_thread);
    CHECK(subscribe_status.load() == abi::wrong_thread);
    CHECK(null_output_status.load() == abi::wrong_thread);
    CHECK(token_value.load() == 0);
    CHECK(env.host().error() == diagnostic_before);

    // Nothing from the foreign thread was registered or queued.
    expect_status("publish after join", plugin->publish("demo.thread", "ok"), abi::ok);
    expect_status("poll", env.host().poll(), abi::ok);
    CHECK(env.observed().events == 0);
    CHECK(env.observed().failures == 0);
}

TEST_CASE(dispatch_budget_bounds_one_poll)
{
    u42::host_options options;
    options.dispatch_budget = 2;
    fixture_spec spec = make_spec("fixture.budget", {"demo.tick"});
    spec.self_publish_name = "demo.tick";
    spec.self_publish_limit = 6;
    fixture_env env(options, spec);
    expect_status("stage and start", env.stage_and_start(), abi::ok);
    fixture_plugin* plugin = env.plugin();
    CHECK(plugin != nullptr);

    expect_status("seed publish", plugin->publish("demo.tick", "seed"), abi::ok);
    CHECK(env.observed().events == 0);

    // Every callback republishes one event, so the queue refills itself: the budget is the only
    // reason a poll returns, and it must return instead of spinning until the queue is empty.
    expect_status("first poll", env.host().poll(), abi::limit_exceeded);
    CHECK(env.observed().events == 2);
    CHECK(env.observed().max_nesting == 1);

    expect_status("second poll", env.host().poll(), abi::limit_exceeded);
    CHECK(env.observed().events == 4);

    // The sixth delivery performs the last allowed republish, so one more bounded poll is needed
    // to hand that event out and leave the queue empty.
    expect_status("third poll", env.host().poll(), abi::limit_exceeded);
    CHECK(env.observed().events == 6);

    expect_status("fourth poll", env.host().poll(), abi::ok);
    CHECK(env.observed().events == 7); // one seed plus six republishes, then the queue is empty
    CHECK(env.observed().failures == 0);
}

TEST_CASE(capability_watch_queues_the_snapshot_instead_of_calling_inline)
{
    hub shared;
    fixture_spec watcher = make_spec("fixture.watcher");
    watcher.after = {"fixture.provider"}; // the provider is already active when init() watches
    watcher.watch_capabilities = true;
    fixture_factory watcher_factory(watcher, &shared);
    fixture_factory provider_factory(make_spec("fixture.provider"), &shared);
    u42::host rack;

    expect_status("add watcher", rack.add(&watcher_factory), abi::ok);
    expect_status("add provider", rack.add(&provider_factory), abi::ok);
    expect_status("start", rack.start(), abi::ok);

    fixture_plugin* watcher_plugin = watcher_factory.instance();
    CHECK(watcher_plugin != nullptr);
    CHECK(watcher_plugin->watch_token().value != 0);

    // The provider was available and published while init() called watch(): the snapshot was
    // queued for the drain instead of interrupting the initializing consumer.
    CHECK(shared.inline_callbacks == 0);
    CHECK(shared.cap_events >= 1);
    CHECK(shared.cap_available >= 1);
    CHECK(shared.failures == 0);

    const int after_start = shared.cap_events;
    expect_status("poll", rack.poll(), abi::ok);
    CHECK(shared.cap_events == after_start); // nothing else was pending
}

} // namespace

int main()
{
    for (const test_case& test : tests()) {
        g_current_test = test.name;
        test.run();
    }
    std::printf("%zu test cases passed\n", tests().size());
    return 0;
}
