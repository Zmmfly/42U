#pragma once
// Shared implementation contract; owned by the coordinator, not an ABI header.
#include <42u/host.hpp>
#include <42u/order.hpp>
#include <42u/plug.hpp>
#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace u42::detail {
namespace a = abi::v3;
struct engine;
struct record;
enum class phase { created, initializing, initialized, starting, active, revoking, stopped, faulted };
struct owned_method {
    a::method_id id = 0;
    std::string name, description, input_schema, output_schema;
};
struct capability_set {
    std::vector<owned_method> methods;
};

/**
 * @brief One stable per-instance context; a null owner is the host administration context.
 */
struct context final : a::ictx, a::ievents, a::icaps, a::icalls, a::idiag {
    engine& runtime;
    record* owner;
    explicit context(engine& e, record* p) : runtime(e), owner(p) {}
    a::status U42_CALL query(const a::iid*, void**) noexcept override;
    a::status U42_CALL subscribe(const char*, a::ievent_sink*, a::token*) noexcept override;
    a::status U42_CALL unsubscribe(a::token) noexcept override;
    a::status U42_CALL publish(const char*, a::bytes) noexcept override;
    a::status U42_CALL announce(const a::caps_desc*) noexcept override;
    a::status U42_CALL watch(a::icap_sink*, a::token*) noexcept override;
    a::status U42_CALL unwatch(a::token) noexcept override;
    a::status U42_CALL acquire(const char*, const a::version_range*, a::irevoker*, a::borrow*) noexcept override;
    a::status U42_CALL release(a::token) noexcept override;
    a::status U42_CALL call_name(a::token, const char*, a::bytes, a::iwriter*) noexcept override;
    a::status U42_CALL call_id(a::token, a::method_id, a::bytes, a::iwriter*) noexcept override;
    void U42_CALL log(const char*) noexcept override;
};

struct record {
    std::unique_ptr<plug> library;
    a::iplug_fty* factory = nullptr;
    a::iplug* instance = nullptr;
    std::unique_ptr<context> ctx;
    order_node order;
    a::plugin_version version{};
    std::uint64_t generation = 0;
    phase state = phase::created;
    std::size_t depth = 0;
    bool announced = false;
    bool published = false;
    capability_set capabilities;
    a::iinvoke* invoker = nullptr;
};
struct event_subscription { record* owner; a::ievent_sink* sink; std::string name; };
struct cap_subscription { record* owner; a::icap_sink* sink; };
struct lease_record {
    record* consumer;
    record* provider;
    std::uint64_t generation;
    a::plugin_version version;
    a::irevoker* receiver;
    std::size_t active_calls = 0;
};
struct queued_event { std::string name; std::string payload; };
struct cap_notice {
    std::uint64_t watch;
    std::string provider;
    std::uint64_t generation;
    bool available;
    a::plugin_version version{};
    capability_set capabilities;
};

struct engine {
    host_options options;
    std::thread::id thread = std::this_thread::get_id();
    std::map<std::string, std::unique_ptr<record>> records;
    std::vector<std::string> init_order;
    std::map<std::uint64_t, event_subscription> subscriptions;
    std::map<std::uint64_t, cap_subscription> watches;
    std::map<std::uint64_t, lease_record> leases;
    std::deque<queued_event> events;
    std::deque<cap_notice> notices;
    std::deque<std::string> deferred_unloads;
    std::unique_ptr<context> admin;
    std::uint64_t next_token = 1;
    std::uint64_t next_generation = 1;
    std::size_t depth = 0;
    bool initializing_batch = false;
    bool starting_batch = false; // Dispatch notices, but defer queued unloads until start returns.
    bool shutting_down = false;
    bool draining = false;
    std::string error;

    explicit engine(host_options value) : options(value), admin(std::make_unique<context>(*this, nullptr)) {}
    bool on_thread() const noexcept { return thread == std::this_thread::get_id(); }
    a::status fail(a::status value, const std::string& message) { error = message; return value; }
    // Lifecycle owner: src/host.cc.
    a::status stage(a::iplug_fty*, std::unique_ptr<plug> library = {});
    a::status start_pending();
    a::status unload_one(const std::string&);
    a::status shutdown_all();
    // Services owner: src/context.cc. Called outside a plugin stack unless documented.
    a::status commit(record&);
    a::status withdraw(record&);
    a::status revoke(record&);
    void remove_owner(record&);
    a::status drain();
};

/**
 * @brief Guard every host-to-plugin call. Callers must reject reentry before constructing it.
 */
struct call_scope {
    engine& runtime;
    record& target;
    call_scope(engine& e, record& p) : runtime(e), target(p) { ++runtime.depth; ++target.depth; }
    ~call_scope() { --target.depth; --runtime.depth; }
    call_scope(const call_scope&) = delete;
    call_scope& operator=(const call_scope&) = delete;
};
/**
 * @brief Host-owned bounded temporary output; errors stay sticky even if plugin ignores them.
 */
struct string_writer final : a::iwriter {
    std::string value;
    std::size_t limit;
    a::status result = a::ok;
    explicit string_writer(std::size_t maximum) : limit(maximum) {}
    a::status U42_CALL write(a::bytes data) noexcept override {
        if (result != a::ok) return result;
        if (!data.data && data.size) return result = a::invalid_argument;
        if (data.size > limit || value.size() > limit - static_cast<std::size_t>(data.size))
            return result = a::limit_exceeded;
        try {
            if (data.size) value.append(static_cast<const char*>(data.data), static_cast<std::size_t>(data.size));
            return a::ok;
        } catch (...) { return result = a::failed; }
    }
};
} // namespace u42::detail
