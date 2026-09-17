/**
 * @file abi_test.cc
 * @brief Independent, dependency-free verification of the frozen contract in @c 42u/abi.hpp.
 *
 * The file owns its main() and links against nothing beyond the standard library, so it can be
 * built with either compiler without the host, spdlog, asio or fmt:
 *
 * @code
 *   g++     -std=c++17 -Wall -Wextra -Werror -Iinc tests/abi_test.cc -o /tmp/u42-abi-test
 *   clang++ -std=c++17 -Wall -Wextra -Werror -Iinc tests/abi_test.cc -o /tmp/u42-abi-test
 * @endcode
 *
 * What it verifies: the frozen v1 contract is restated here as compile-time assertions (integer
 * widths, status values, opaque identifier defaults, descriptor layout, interface shape, every
 * virtual signature with its calling convention and noexcept, the entry signature and symbol name)
 * and as mock-based runtime checks (entry negotiation, output clearing, multi-interface query
 * adjustment, lifecycle destruction). An accidental edit to abi.hpp therefore fails this build
 * instead of silently changing the binary interface.
 *
 * What it deliberately does not claim:
 * - It does not prove binary compatibility between a separately compiled plugin and host. Two
 *   translation units can satisfy every assertion here and still disagree in a real binary if they
 *   use different compilers, compiler versions, packing flags, standard-library versions or C++
 *   ABI families. Only loading a pre-built plugin exercises that.
 * - It cannot observe @c extern @c "C" language linkage: no standard type trait exposes it and
 *   neither GCC nor Clang encodes it in the function type. Only a real dlopen/GetProcAddress of an
 *   exported library proves the symbol name and its C linkage.
 * - It cannot pin a concrete calling convention on this platform, because @c U42_CALL expands to
 *   nothing off _WIN32. The signature assertions therefore pin return type, parameters and noexcept
 *   on the GNU/Clang builds, and additionally the convention in the MSVC build.
 * - It cannot observe virtual slot order or slot count through standard traits. Each slot's type is
 *   pinned below; the declaration order itself stays a review obligation.
 * - The exact descriptor sizes below are frozen literals only for 64-bit-pointer profiles, because
 *   they change with the data model. Everywhere else the same layout is still checked against a
 *   computed natural-alignment model, which is independent of the pointer width.
 *
 * @note NDEBUG is defined deliberately, as in tests/sdk_test.cc: CHECK must keep reporting failures
 *       even when assert() has been compiled out.
 */
#define NDEBUG 1

#include <42u/abi.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

namespace a = u42::abi::v1;

const char* g_current_test = nullptr;

/**
 * @brief Report one failed CHECK and stop the process with a non-zero status.
 *
 * @param expr Failed expression text.
 * @param file Source file of the failed CHECK.
 * @param line Source line of the failed CHECK.
 */
[[noreturn]] void fail_check(const char* expr, const char* file, int line)
{
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
 * @brief Compile-time guard: CHECK must expand to fail_check, never to assert().
 *
 * @note Without this, a CHECK redefined as assert() would silently disappear under NDEBUG.
 */
inline constexpr std::string_view u42_check_expansion = U42_EXPAND_STRING(CHECK(0 == 1));
static_assert(u42_check_expansion.find("fail_check") != std::string_view::npos,
              "CHECK must call fail_check directly; defining it as assert() disables it "
              "under NDEBUG");

/**
 * @brief Pointer width of the tested profile.
 *
 * The literal descriptor sizes pinned in this file depend on 8-byte pointers; the layout model
 * checks stay valid on every width.
 */
#if defined(UINTPTR_MAX) && UINTPTR_MAX == UINT64_MAX
#  define U42_ABI_TEST_POINTER64 1
#else
#  define U42_ABI_TEST_POINTER64 0
#endif

struct test_case {
    const char* name;
    void (*run)();
};

std::vector<test_case>& tests();

struct registrar {
    registrar(const char* name, void (*run)()) { tests().push_back(test_case{name, run}); }
};

/** @brief Register one case without any test framework. */
#define TEST_CASE(name)                                                        \
    void name();                                                               \
    [[maybe_unused]] const registrar reg_##name(#name, &name);                 \
    void name()

/**
 * @brief Independent helpers used by the assertions below; none of them is part of the ABI.
 */

/**
 * @brief Round up to the next multiple of @p alignment.
 *
 * @param offset Byte offset to round up.
 * @param alignment Natural alignment of the field that is placed at the returned offset.
 * @return Smallest multiple of @p alignment that is not less than @p offset.
 */
constexpr std::size_t align_up(std::size_t offset, std::size_t alignment) noexcept
{
    return (offset + alignment - 1) / alignment * alignment;
}

/**
 * @brief Test whether a 64-bit identifier tag spells @p text byte by byte, most significant first.
 *
 * @param tag Numeric identifier half, decoded from the most significant byte downwards.
 * @param text Expected 8 ASCII characters, NUL-terminated.
 * @return True when all 8 bytes match; no host endianness is involved because the value is shifted.
 */
constexpr bool tag_spells(std::uint64_t tag, const char* text) noexcept
{
    for (unsigned index = 0; index < 8; ++index) {
        const std::uint64_t expected =
            static_cast<std::uint64_t>(static_cast<unsigned char>(text[index]));
        if (((tag >> (56u - 8u * index)) & 0xFFu) != expected) return false;
    }
    return true;
}

/**
 * @brief Compare two NUL-terminated strings in a constant expression.
 *
 * @param left First string; null is never equal.
 * @param right Second string; null is never equal.
 * @return True only when both are non-null and identical.
 */
constexpr bool cstr_equal(const char* left, const char* right) noexcept
{
    if (left == nullptr || right == nullptr) return false;
    while (*left != '\0' && *right != '\0') {
        if (*left != *right) return false;
        ++left;
        ++right;
    }
    return *left == *right;
}

/**
 * @brief Restate the documented bytes rule: a null data pointer is only legal with size zero.
 *
 * @param value Borrowed view to inspect.
 * @return True when the view obeys the rule stated in abi.hpp.
 * @note The header states this obligation for callers; it does not enforce it anywhere, so this
 *       helper is the test's own executable restatement, not a promise made by the ABI.
 */
constexpr bool bytes_rule_holds(a::bytes value) noexcept
{
    return value.data != nullptr || value.size == 0;
}

/**
 * @brief Non-null sentinel used to prove that a failure path really overwrites an output slot.
 *
 * @tparam T Output pointee type.
 * @return A never-dereferenced non-null pointer value.
 */
template <class T>
T* stale_sentinel() noexcept
{
    return reinterpret_cast<T*>(static_cast<std::uintptr_t>(1));
}

/**
 * @brief State shared by the mock factory and every mock instance it creates.
 */
struct factory_state {
    bool fail_describe = false;
    bool fail_create = false;
    int describe_calls = 0;
    int create_calls = 0;
    int live_instances = 0;
    int init_calls = 0;
    int start_calls = 0;
    int stop_calls = 0;
    int destroy_calls = 0;
    int query_calls = 0;
    int invoke_calls = 0;
};

/**
 * @brief Mock plugin instance implementing the frozen lifecycle plus the optional invoke interface.
 *
 * It is allocated with @c new and released only by its own iplug::destroy(), which is what the ABI
 * requires of the allocating side. The interface types keep a protected, non-virtual destructor,
 * so no caller can delete it through iplug by accident.
 */
struct mock_plugin final : a::iplug, a::iinvoke {
    /** @brief Lifecycle phase the frozen contract distinguishes. */
    enum class phase { created, initialized, started, stopped };

    /**
     * @brief Build one instance and account for the live allocation.
     *
     * @param state Shared record that survives the instance.
     */
    explicit mock_plugin(factory_state& state) noexcept : state_(state) { ++state_.live_instances; }

    /** @brief Account for the released allocation. */
    ~mock_plugin() { --state_.live_instances; }

    a::status U42_CALL init(a::ictx* ctx) noexcept override
    {
        if (phase_ != phase::created) return a::invalid_state;
        if (ctx == nullptr) return a::invalid_argument;
        ++state_.init_calls;
        phase_ = phase::initialized;
        return a::ok;
    }

    a::status U42_CALL start() noexcept override
    {
        if (phase_ != phase::initialized) return a::invalid_state;
        ++state_.start_calls;
        phase_ = phase::started;
        return a::ok;
    }

    a::status U42_CALL stop() noexcept override
    {
        if (phase_ != phase::started) return a::invalid_state;
        ++state_.stop_calls;
        phase_ = phase::stopped;
        return a::ok;
    }

    void U42_CALL destroy() noexcept override
    {
        ++state_.destroy_calls;
        delete this; // Only the allocating side destroys an ABI object.
    }

    a::status U42_CALL query(const a::iid* type, void** out) noexcept override
    {
        if (out == nullptr) return a::invalid_argument;
        *out = nullptr;
        ++state_.query_calls;
        if (type == nullptr) return a::invalid_argument;
        if (*type == a::invoke_iid) {
            *out = static_cast<a::iinvoke*>(this);
            return a::ok;
        }
        return a::unsupported;
    }

    a::status U42_CALL invoke(a::method_id method, a::bytes args, a::iwriter* result) noexcept override
    {
        (void)method;
        (void)args;
        if (result == nullptr) return a::invalid_argument;
        ++state_.invoke_calls;
        return a::ok;
    }

    /** @brief Current phase, used only by the test that drives the lifecycle. */
    phase current() const noexcept { return phase_; }

    /**
     * @brief Address of this object's iinvoke subobject.
     *
     * @return Pointer used to cross-check what iplug::query() publishes; a host can only obtain
     *         that address through query(), never by casting the root interface pointer.
     */
    void* iinvoke_address() noexcept
    {
        return static_cast<void*>(static_cast<a::iinvoke*>(this));
    }

private:
    factory_state& state_;
    phase phase_ = phase::created;
};

/**
 * @brief Mock factory that advertises immutable metadata and creates mock instances.
 *
 * Every failure path clears the caller's output before returning and every required null argument
 * is rejected, mirroring the rules in the design (required pointers are parameter errors, a legal
 * output address is cleared before work starts).
 */
struct mock_fty final : a::iplug_fty {
    /**
     * @brief Borrow the shared record for this factory.
     *
     * @param state Record shared with the instances this factory creates.
     */
    explicit mock_fty(factory_state& state) noexcept : state_(state) {}

    a::status U42_CALL describe(const a::plug_desc** out) noexcept override
    {
        if (out == nullptr) return a::invalid_argument;
        *out = nullptr;
        ++state_.describe_calls;
        if (state_.fail_describe) return a::failed;
        desc_.plug_id = "u42.test.mock";
        desc_.version = "1.0.0";
        desc_.priority = 10;
        *out = &desc_;
        return a::ok;
    }

    a::status U42_CALL create(a::iplug** out) noexcept override
    {
        if (out == nullptr) return a::invalid_argument;
        *out = nullptr;
        ++state_.create_calls;
        if (state_.fail_create) return a::failed;
        *out = new mock_plugin(state_);
        return a::ok;
    }

private:
    factory_state& state_;
    a::plug_desc desc_{};
};

/** @brief Shared record of the mock library stand-in, alive for the whole program. */
factory_state& mock_state()
{
    static factory_state state;
    return state;
}

/**
 * @brief Borrowed factory handed out by the mock entry, mirroring a library-owned static factory.
 *
 * @return Factory pointer valid until the (never performed) unload of the mock library.
 */
a::iplug_fty* mock_factory()
{
    static mock_fty factory{mock_state()};
    return &factory;
}

/**
 * @brief Mock plugin entry with the frozen signature.
 *
 * @param major Requested ABI major.
 * @param out Caller-owned output slot; a required argument, so null is a parameter error.
 * @return invalid_argument for a null @p out or @p major zero, unsupported for a different major,
 *         ok when @p major matches abi::v1::abi_major. A rejected negotiation leaves the caller's
 *         output cleared rather than stale.
 */
extern "C" a::status U42_CALL u42_abi_test_mock_entry(std::uint32_t major,
                                                     a::iplug_fty** out) noexcept
{
    if (out == nullptr) return a::invalid_argument;
    *out = nullptr;
    if (major == 0) return a::invalid_argument;
    if (major != a::abi_major) return a::unsupported;
    *out = mock_factory();
    return a::ok;
}

/** @brief Counters proving which subobject of the mock host context was actually called. */
struct context_state {
    int query_calls = 0;
    int events_calls = 0;
    int caps_calls = 0;
};

/**
 * @brief Mock host context implementing several unrelated frozen interfaces at once.
 *
 * It exists to demonstrate the multi-interface query rule: the pointer published for an interface
 * must be that interface's own base subobject obtained by static_cast, never the root object
 * address, because the ABI transports only void* and the caller casts it back verbatim.
 */
struct mock_context final : a::ictx, a::ievents, a::icaps {
    a::status U42_CALL query(const a::iid* type, void** out) noexcept override
    {
        if (out == nullptr) return a::invalid_argument;
        *out = nullptr; // A legal output address is cleared before anything else happens.
        ++state.query_calls;
        if (type == nullptr) return a::invalid_argument;
        if (*type == a::events_iid) {
            *out = static_cast<a::ievents*>(this);
            return a::ok;
        }
        if (*type == a::caps_iid) {
            *out = static_cast<a::icaps*>(this);
            return a::ok;
        }
        return a::unsupported;
    }

    a::status U42_CALL subscribe(const char* name, a::ievent_sink* sink, a::token* out) noexcept override
    {
        (void)name;
        (void)sink;
        if (out == nullptr) return a::invalid_argument;
        *out = a::token{};
        ++state.events_calls;
        return a::ok;
    }

    a::status U42_CALL unsubscribe(a::token) noexcept override
    {
        ++state.events_calls;
        return a::ok;
    }

    a::status U42_CALL publish(const char*, a::bytes) noexcept override
    {
        ++state.events_calls;
        return a::ok;
    }

    a::status U42_CALL announce(const a::caps_desc*) noexcept override
    {
        ++state.caps_calls;
        return a::ok;
    }

    a::status U42_CALL watch(a::icap_sink*, a::token*) noexcept override
    {
        ++state.caps_calls;
        return a::ok;
    }

    a::status U42_CALL unwatch(a::token) noexcept override
    {
        ++state.caps_calls;
        return a::unsupported;
    }

    a::status U42_CALL acquire(const char*, const a::iid*, a::irevoker*, a::borrow*) noexcept override
    {
        ++state.caps_calls;
        return a::unsupported;
    }

    a::status U42_CALL release(a::token) noexcept override
    {
        ++state.caps_calls;
        return a::ok;
    }

    context_state state;
};

/**
 * @brief Mock output writer applying the documented bytes rule.
 *
 * @note The writer is borrowed by the caller and must never be retained, which the test encodes by
 *       keeping no pointer to the incoming data.
 */
struct bytes_writer final : a::iwriter {
    std::uint64_t accepted = 0;

    a::status U42_CALL write(a::bytes data) noexcept override
    {
        if (!bytes_rule_holds(data)) return a::invalid_argument;
        accepted += data.size;
        return a::ok;
    }
};

/**
 * @brief Expected field offsets under natural alignment, derived field by field.
 *
 * Each offset follows from the previous field's size and the next field's alignment, which is what
 * a compiler must reproduce with default (unpacked) settings. Comparing them with offsetof() fails
 * if any ABI struct is packed differently, and it does so on every data-model width.
 */
namespace natural {
constexpr std::size_t plug_id = align_up(2 * sizeof(std::uint32_t), alignof(const char*));
constexpr std::size_t version = plug_id + sizeof(const char*);
constexpr std::size_t priority = version + sizeof(const char*);
constexpr std::size_t before_count = priority + sizeof(std::int32_t);
constexpr std::size_t before =
    align_up(before_count + sizeof(std::uint32_t), alignof(const char* const*));
constexpr std::size_t after_count = before + sizeof(const char* const*);
constexpr std::size_t after =
    align_up(after_count + sizeof(std::uint32_t), alignof(const char* const*));
constexpr std::size_t plug_desc_size =
    align_up(after + sizeof(const char* const*), alignof(a::plug_desc));

constexpr std::size_t method_name = align_up(sizeof(a::method_id), alignof(const char*));
constexpr std::size_t method_description = method_name + sizeof(const char*);
constexpr std::size_t method_input_schema = method_description + sizeof(const char*);
constexpr std::size_t method_output_schema = method_input_schema + sizeof(const char*);
constexpr std::size_t method_desc_size =
    align_up(method_output_schema + sizeof(const char*), alignof(a::method_desc));

constexpr std::size_t caps_interfaces = align_up(2 * sizeof(std::uint32_t), alignof(const a::iid*));
constexpr std::size_t caps_method_count = caps_interfaces + sizeof(const a::iid*);
constexpr std::size_t caps_methods =
    align_up(caps_method_count + sizeof(std::uint32_t), alignof(const a::method_desc*));
constexpr std::size_t caps_desc_size =
    align_up(caps_methods + sizeof(const a::method_desc*), alignof(a::caps_desc));

constexpr std::size_t bytes_size_field = align_up(sizeof(const void*), alignof(std::uint64_t));
constexpr std::size_t bytes_size =
    align_up(bytes_size_field + sizeof(std::uint64_t), alignof(a::bytes));

constexpr std::size_t borrow_credential = align_up(sizeof(void*), alignof(a::token));
constexpr std::size_t borrow_size = align_up(borrow_credential + sizeof(a::token), alignof(a::borrow));

constexpr std::size_t event_payload = align_up(sizeof(const char*), alignof(a::bytes));
constexpr std::size_t event_size = align_up(event_payload + sizeof(a::bytes), alignof(a::event));

constexpr std::size_t cap_event_available = sizeof(const char*);
constexpr std::size_t cap_event_capabilities =
    align_up(cap_event_available + sizeof(std::uint32_t), alignof(a::caps_desc));
constexpr std::size_t cap_event_size =
    align_up(cap_event_capabilities + sizeof(a::caps_desc), alignof(a::cap_event));
} // namespace natural

/** @brief Frozen signature of one interface member: return type, convention, parameters, noexcept. */
#define U42_FROZEN_SIG(INTERFACE, NAME, RETURN, ...)                                           \
    static_assert(std::is_same_v<decltype(&INTERFACE::NAME),                                    \
                                 RETURN(U42_CALL INTERFACE::*)(__VA_ARGS__) noexcept>,          \
                  "frozen signature changed: " #INTERFACE "::" #NAME)

/** @brief Frozen shape shared by every interface: abstract, one vptr, destruction not public. */
#define U42_FROZEN_INTERFACE(TYPE)                                                             \
    static_assert(std::is_class_v<TYPE>, "interface must be a class");                          \
    static_assert(std::is_abstract_v<TYPE>, "interface must stay abstract");                    \
    static_assert(std::is_polymorphic_v<TYPE>, "interface must stay polymorphic");              \
    static_assert(!std::is_standard_layout_v<TYPE>, "virtual functions forbid standard layout"); \
    static_assert(sizeof(TYPE) == sizeof(void*), "interface must be one vptr and no data members"); \
    static_assert(!std::is_default_constructible_v<TYPE>, "an abstract interface has no instances"); \
    static_assert(!std::is_destructible_v<TYPE>, "destruction must not be reachable publicly"); \
    static_assert(!std::has_virtual_destructor_v<TYPE>,                                         \
                  "a virtual destructor would occupy frozen vtable slots");                     \
    static_assert(!std::is_trivially_copyable_v<TYPE>, "interfaces are never copied as values")

/** @brief Frozen shape shared by every pure-data ABI struct. */
#define U42_FROZEN_POD(TYPE)                                                                   \
    static_assert(std::is_standard_layout_v<TYPE>, "ABI struct must be standard layout");       \
    static_assert(std::is_trivially_copyable_v<TYPE>, "ABI struct must be trivially copyable");  \
    static_assert(std::is_trivially_copy_assignable_v<TYPE>, "ABI struct must copy trivially");  \
    static_assert(std::is_trivially_destructible_v<TYPE>, "ABI struct must have a trivial dtor"); \
    static_assert(!std::is_polymorphic_v<TYPE>, "ABI struct must not carry a vtable");          \
    static_assert(std::is_aggregate_v<TYPE>, "ABI struct must stay an aggregate")

TEST_CASE(status_and_method_id_are_fixed_width)
{
    static_assert(std::is_same_v<a::status, std::uint32_t>);
    static_assert(std::is_same_v<a::method_id, std::uint32_t>);
    static_assert(sizeof(a::status) == 4 && sizeof(a::method_id) == 4);
    static_assert(std::is_unsigned_v<a::status> && std::is_unsigned_v<a::method_id>);
    static_assert(a::abi_major == 1);

    // Every named status keeps the numeric value the frozen wire protocol publishes.
    static_assert(a::ok == 0);
    static_assert(a::invalid_argument == 1);
    static_assert(a::unsupported == 2);
    static_assert(a::not_found == 3);
    static_assert(a::duplicate == 4);
    static_assert(a::invalid_state == 5);
    static_assert(a::busy == 6);
    static_assert(a::stale == 7);
    static_assert(a::limit_exceeded == 8);
    static_assert(a::failed == 9);
    static_assert(a::wrong_thread == 10);
    static_assert(a::cycle == 11);
    static_assert(a::deferred == 12);

    static_assert(a::entry_name != nullptr);
    CHECK(a::abi_major == 1);

    constexpr a::status codes[] = {
        a::ok,          a::invalid_argument, a::unsupported, a::not_found, a::duplicate,
        a::invalid_state, a::busy,           a::stale,       a::limit_exceeded, a::failed,
        a::wrong_thread, a::cycle,           a::deferred};
    constexpr std::size_t count = sizeof(codes) / sizeof(codes[0]);
    static_assert(count == 13, "one assertion per published status is expected");
    CHECK(count == 13);
    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t j = i + 1; j < count; ++j) CHECK(codes[i] != codes[j]);
    }
}

TEST_CASE(abi_structs_are_standard_layout_and_trivially_copyable)
{
    U42_FROZEN_POD(a::iid);
    U42_FROZEN_POD(a::token);
    U42_FROZEN_POD(a::binding);
    U42_FROZEN_POD(a::bytes);
    U42_FROZEN_POD(a::borrow);
    U42_FROZEN_POD(a::plug_desc);
    U42_FROZEN_POD(a::method_desc);
    U42_FROZEN_POD(a::caps_desc);
    U42_FROZEN_POD(a::event);
    U42_FROZEN_POD(a::cap_event);

    // The traits above must be able to discriminate: an interface is none of those things.
    static_assert(!std::is_trivially_copyable_v<a::ictx>);
    static_assert(!std::is_standard_layout_v<a::ictx>);

    // Fixed-width members keep their width and natural alignment on any data model.
    static_assert(sizeof(a::iid) == 16 && sizeof(a::token) == 8 && sizeof(a::binding) == 8);
    static_assert(std::is_same_v<decltype(a::iid::high), std::uint64_t>);
    static_assert(std::is_same_v<decltype(a::iid::low), std::uint64_t>);
    static_assert(std::is_same_v<decltype(a::bytes::size), std::uint64_t>);
    static_assert(alignof(a::iid) == alignof(std::uint64_t));
    static_assert(alignof(a::token) == alignof(std::uint64_t));
}

TEST_CASE(official_iids_encode_the_v1_tag)
{
    // The high half spells "42U_ABI1" byte by byte, so an accidentally invented iid is obvious.
    static_assert(tag_spells(a::events_iid.high, "42U_ABI1"));
    static_assert(tag_spells(a::caps_iid.high, "42U_ABI1"));
    static_assert(tag_spells(a::calls_iid.high, "42U_ABI1"));
    static_assert(tag_spells(a::diag_iid.high, "42U_ABI1"));
    static_assert(tag_spells(a::invoke_iid.high, "42U_ABI1"));

    // The low half is the published per-contract number, and every official id is distinct.
    static_assert(a::events_iid.low == 1);
    static_assert(a::caps_iid.low == 2);
    static_assert(a::calls_iid.low == 3);
    static_assert(a::diag_iid.low == 4);
    static_assert(a::invoke_iid.low == 5);
    static_assert(a::events_iid != a::caps_iid);
    static_assert(a::events_iid != a::calls_iid);
    static_assert(a::events_iid != a::diag_iid);
    static_assert(a::events_iid != a::invoke_iid);
    static_assert(a::caps_iid != a::calls_iid);
    static_assert(a::caps_iid != a::diag_iid);
    static_assert(a::caps_iid != a::invoke_iid);
    static_assert(a::calls_iid != a::diag_iid);
    static_assert(a::calls_iid != a::invoke_iid);
    static_assert(a::diag_iid != a::invoke_iid);

    // A zero identifier is not a published contract; comparison stays constexpr and noexcept.
    static_assert(a::events_iid != a::iid{});
    static_assert(a::events_iid == a::iid{0x3432555f41424931ULL, 1});
    static_assert(std::is_same_v<decltype(&a::operator==), bool (*)(a::iid, a::iid) noexcept>);
    static_assert(std::is_same_v<decltype(&a::operator!=), bool (*)(a::iid, a::iid) noexcept>);
    static_assert(noexcept(a::events_iid == a::caps_iid));

    constexpr a::iid ids[] = {a::events_iid, a::caps_iid, a::calls_iid, a::diag_iid, a::invoke_iid};
    constexpr std::size_t count = sizeof(ids) / sizeof(ids[0]);
    CHECK(count == 5);
    for (std::size_t i = 0; i < count; ++i) {
        CHECK(ids[i].high == 0x3432555f41424931ULL);
        CHECK(ids[i].low == static_cast<std::uint64_t>(i + 1));
        for (std::size_t j = i + 1; j < count; ++j) CHECK(ids[i] != ids[j]);
    }
}

TEST_CASE(descriptor_layout_is_pinned_to_natural_alignment)
{
    // The layout model itself must be correct before it is used to judge the ABI structs.
    CHECK(align_up(0, 8) == 0);
    CHECK(align_up(8, 8) == 8);
    CHECK(align_up(5, 8) == 8);
    CHECK(align_up(9, 8) == 16);

    // Offsets follow from the declared field list, field by field (see namespace natural), so a
    // packed or reordered struct fails here on every data-model width.
    static_assert(offsetof(a::plug_desc, struct_size) == 0);
    static_assert(offsetof(a::plug_desc, reserved) == sizeof(std::uint32_t));
    static_assert(offsetof(a::plug_desc, plug_id) == natural::plug_id);
    static_assert(offsetof(a::plug_desc, version) == natural::version);
    static_assert(offsetof(a::plug_desc, priority) == natural::priority);
    static_assert(offsetof(a::plug_desc, before_count) == natural::before_count);
    static_assert(offsetof(a::plug_desc, before) == natural::before);
    static_assert(offsetof(a::plug_desc, after_count) == natural::after_count);
    static_assert(offsetof(a::plug_desc, after) == natural::after);
    static_assert(sizeof(a::plug_desc) == natural::plug_desc_size);

    static_assert(offsetof(a::method_desc, id) == 0);
    static_assert(offsetof(a::method_desc, name) == natural::method_name);
    static_assert(offsetof(a::method_desc, description) == natural::method_description);
    static_assert(offsetof(a::method_desc, input_schema) == natural::method_input_schema);
    static_assert(offsetof(a::method_desc, output_schema) == natural::method_output_schema);
    static_assert(sizeof(a::method_desc) == natural::method_desc_size);

    static_assert(offsetof(a::caps_desc, struct_size) == 0);
    static_assert(offsetof(a::caps_desc, interface_count) == sizeof(std::uint32_t));
    static_assert(offsetof(a::caps_desc, interfaces) == natural::caps_interfaces);
    static_assert(offsetof(a::caps_desc, method_count) == natural::caps_method_count);
    static_assert(offsetof(a::caps_desc, methods) == natural::caps_methods);
    static_assert(sizeof(a::caps_desc) == natural::caps_desc_size);

    static_assert(offsetof(a::bytes, data) == 0);
    static_assert(offsetof(a::bytes, size) == natural::bytes_size_field);
    static_assert(sizeof(a::bytes) == natural::bytes_size);

    static_assert(offsetof(a::borrow, ptr) == 0);
    static_assert(offsetof(a::borrow, credential) == natural::borrow_credential);
    static_assert(sizeof(a::borrow) == natural::borrow_size);

    static_assert(offsetof(a::event, name) == 0);
    static_assert(offsetof(a::event, payload) == natural::event_payload);
    static_assert(sizeof(a::event) == natural::event_size);

    static_assert(offsetof(a::cap_event, plug_id) == 0);
    static_assert(offsetof(a::cap_event, available) == natural::cap_event_available);
    static_assert(offsetof(a::cap_event, capabilities) == natural::cap_event_capabilities);
    static_assert(sizeof(a::cap_event) == natural::cap_event_size);

    // The version tag a producer publishes must describe the layout this build actually compiled.
    static_assert(a::plug_desc{}.struct_size == sizeof(a::plug_desc));
    static_assert(a::caps_desc{}.struct_size == sizeof(a::caps_desc));
    static_assert(std::is_same_v<decltype(a::plug_desc::struct_size), std::uint32_t>);
    static_assert(std::is_same_v<decltype(a::caps_desc::interface_count), std::uint32_t>);
    static_assert(a::plug_desc{}.reserved == 0);
    static_assert(a::plug_desc{}.before == nullptr && a::plug_desc{}.after == nullptr);
    static_assert(a::plug_desc{}.before_count == 0 && a::plug_desc{}.after_count == 0);
    static_assert(a::caps_desc{}.interfaces == nullptr && a::caps_desc{}.methods == nullptr);

    // Width-independent structural facts, true on every data model.
    static_assert(alignof(a::plug_desc) >= alignof(void*));
    static_assert(sizeof(a::plug_desc) % alignof(a::plug_desc) == 0);
    static_assert(sizeof(a::plug_desc) >= offsetof(a::plug_desc, after) + sizeof(const char* const*));
    static_assert(sizeof(a::caps_desc) >=
                  offsetof(a::caps_desc, methods) + sizeof(const a::method_desc*));
    CHECK(a::plug_desc{}.struct_size == sizeof(a::plug_desc));
    CHECK(a::caps_desc{}.struct_size == sizeof(a::caps_desc));

#if U42_ABI_TEST_POINTER64
    // Exact numbers for the 64-bit-pointer profiles this test is built for. They are the frozen
    // wire layout: 2 x uint32 tag, then pointers, with a 4-byte hole before each pointer that
    // follows a 32-bit field.
    static_assert(sizeof(void*) == 8);
    static_assert(sizeof(a::iid) == 16 && alignof(a::iid) == 8);
    static_assert(offsetof(a::iid, high) == 0 && offsetof(a::iid, low) == 8);
    static_assert(sizeof(a::token) == 8 && alignof(a::token) == 8);
    static_assert(sizeof(a::binding) == 8 && alignof(a::binding) == 8);
    static_assert(sizeof(a::bytes) == 16 && alignof(a::bytes) == 8);
    static_assert(sizeof(a::borrow) == 16 && alignof(a::borrow) == 8);
    static_assert(sizeof(a::plug_desc) == 56 && alignof(a::plug_desc) == 8);
    static_assert(offsetof(a::plug_desc, plug_id) == 8);
    static_assert(offsetof(a::plug_desc, version) == 16);
    static_assert(offsetof(a::plug_desc, priority) == 24);
    static_assert(offsetof(a::plug_desc, before_count) == 28);
    static_assert(offsetof(a::plug_desc, before) == 32);
    static_assert(offsetof(a::plug_desc, after_count) == 40);
    static_assert(offsetof(a::plug_desc, after) == 48);
    static_assert(sizeof(a::method_desc) == 40 && alignof(a::method_desc) == 8);
    static_assert(offsetof(a::method_desc, name) == 8);
    static_assert(offsetof(a::method_desc, output_schema) == 32);
    static_assert(sizeof(a::caps_desc) == 32 && alignof(a::caps_desc) == 8);
    static_assert(offsetof(a::caps_desc, interfaces) == 8);
    static_assert(offsetof(a::caps_desc, method_count) == 16);
    static_assert(offsetof(a::caps_desc, methods) == 24);
    static_assert(sizeof(a::event) == 24 && alignof(a::event) == 8);
    static_assert(offsetof(a::event, payload) == 8);
    static_assert(offsetof(a::event, payload.data) == 8);
    static_assert(offsetof(a::event, payload.size) == 16);
    static_assert(sizeof(a::cap_event) == 48 && alignof(a::cap_event) == 8);
    static_assert(offsetof(a::cap_event, plug_id) == 0);
    static_assert(offsetof(a::cap_event, available) == 8);
    static_assert(offsetof(a::cap_event, capabilities) == 16);
#else
    // Profiles whose pointers are not 64 bit wide: no literal size is pinned here. Changing the
    // data model changes the offsets and sizes (a 4-byte pointer moves every pointer field and
    // shrinks the structs), so a 64-bit literal would be wrong rather than stricter. What still
    // has to hold on every width is the natural-alignment layout asserted above, which is why the
    // offsetof() checks against namespace natural are unconditional and only these freeze literals
    // are 64-bit-only. This branch was not compiled in the environment that produced this file
    // (no 32-bit standard library is installed), so it is asserted, not tested.
    static_assert(sizeof(void*) != 8, "this branch covers a non-64-bit pointer width");
#endif
}

TEST_CASE(interfaces_are_abstract_single_vptr_without_public_destruction)
{
    U42_FROZEN_INTERFACE(a::iwriter);
    U42_FROZEN_INTERFACE(a::ievent_sink);
    U42_FROZEN_INTERFACE(a::icap_sink);
    U42_FROZEN_INTERFACE(a::irevoker);
    U42_FROZEN_INTERFACE(a::ievents);
    U42_FROZEN_INTERFACE(a::icaps);
    U42_FROZEN_INTERFACE(a::icalls);
    U42_FROZEN_INTERFACE(a::idiag);
    U42_FROZEN_INTERFACE(a::iinvoke);
    U42_FROZEN_INTERFACE(a::ictx);
    U42_FROZEN_INTERFACE(a::iplug);
    U42_FROZEN_INTERFACE(a::iplug_fty);

    // The very check that keeps `delete` and `sizeof`-based slot arithmetic away from an interface.
    static_assert(!std::is_destructible_v<a::iwriter>);
    static_assert(!std::is_destructible_v<a::iplug_fty>);
    CHECK(sizeof(a::ictx) == sizeof(void*));
}

TEST_CASE(interface_signatures_are_frozen)
{
    U42_FROZEN_SIG(a::iwriter, write, a::status, a::bytes);
    U42_FROZEN_SIG(a::ievent_sink, on_event, void, const a::event*);
    U42_FROZEN_SIG(a::icap_sink, on_capability, void, const a::cap_event*);
    U42_FROZEN_SIG(a::irevoker, on_revoke, void, a::token);

    U42_FROZEN_SIG(a::ievents, subscribe, a::status, const char*, a::ievent_sink*, a::token*);
    U42_FROZEN_SIG(a::ievents, unsubscribe, a::status, a::token);
    U42_FROZEN_SIG(a::ievents, publish, a::status, const char*, a::bytes);

    U42_FROZEN_SIG(a::icaps, announce, a::status, const a::caps_desc*);
    U42_FROZEN_SIG(a::icaps, watch, a::status, a::icap_sink*, a::token*);
    U42_FROZEN_SIG(a::icaps, unwatch, a::status, a::token);
    U42_FROZEN_SIG(a::icaps, acquire, a::status, const char*, const a::iid*, a::irevoker*, a::borrow*);
    U42_FROZEN_SIG(a::icaps, release, a::status, a::token);

    U42_FROZEN_SIG(a::icalls, bind_name, a::status, const char*, const char*, a::binding*);
    U42_FROZEN_SIG(a::icalls, bind_id, a::status, const char*, a::method_id, a::binding*);
    U42_FROZEN_SIG(a::icalls, call, a::status, a::binding, a::bytes, a::iwriter*);
    U42_FROZEN_SIG(a::icalls, unbind, a::status, a::binding);

    U42_FROZEN_SIG(a::idiag, log, void, const char*);
    U42_FROZEN_SIG(a::iinvoke, invoke, a::status, a::method_id, a::bytes, a::iwriter*);
    U42_FROZEN_SIG(a::ictx, query, a::status, const a::iid*, void**);

    U42_FROZEN_SIG(a::iplug, init, a::status, a::ictx*);
    U42_FROZEN_SIG(a::iplug, start, a::status, void);
    U42_FROZEN_SIG(a::iplug, stop, a::status, void);
    U42_FROZEN_SIG(a::iplug, destroy, void, void);
    U42_FROZEN_SIG(a::iplug, query, a::status, const a::iid*, void**);

    U42_FROZEN_SIG(a::iplug_fty, describe, a::status, const a::plug_desc**);
    U42_FROZEN_SIG(a::iplug_fty, create, a::status, a::iplug**);

    // The frozen entry type: C-callable shape, requested major, factory output, noexcept.
    static_assert(std::is_same_v<decltype(&u42_get_factory),
                                 a::status(U42_CALL*)(std::uint32_t, a::iplug_fty**) noexcept>);
    static_assert(std::is_same_v<decltype(&u42_get_factory), a::entry_fn>);
    static_assert(std::is_same_v<decltype(u42_get_factory), std::remove_pointer_t<a::entry_fn>>);
    static_assert(cstr_equal(a::entry_name, "u42_get_factory"));
    static_assert(std::is_same_v<decltype(a::entry_name), const char* const>);
    static_assert(a::entry_name[0] == 'u' && a::entry_name[1] == '4' && a::entry_name[2] == '2');

    // The assertions above only matter if a mismatch is actually detectable, which requires
    // noexcept to be part of the function type: it is, from C++17 onwards. These two negative
    // checks would silently pass under -std=c++14 and are the reason this file demands C++17.
    static_assert(!std::is_same_v<decltype(&a::ievents::subscribe),
                                  a::status(U42_CALL a::ievents::*)(const char*, a::ievent_sink*,
                                                                    a::token*)>);
    static_assert(!std::is_same_v<decltype(&a::ievents::subscribe),
                                  a::status(U42_CALL a::ievents::*)(const char*, a::ievent_sink*,
                                                                    std::uint64_t*) noexcept>);
}

TEST_CASE(opaque_values_default_to_the_documented_invalid_zero)
{
    static_assert(a::token{}.value == 0);
    static_assert(a::binding{}.value == 0);
    static_assert(std::is_same_v<decltype(a::token::value), std::uint64_t>);
    static_assert(std::is_same_v<decltype(a::binding::value), std::uint64_t>);
    static_assert(a::borrow{}.ptr == nullptr && a::borrow{}.credential.value == 0);
    static_assert(a::bytes{}.data == nullptr && a::bytes{}.size == 0);

    // Zero is the documented invalid credential, and it must not be reachable by accident.
    const a::token empty_token{};
    const a::binding empty_binding{};
    const a::borrow empty_borrow{};
    const a::bytes empty_bytes{};
    CHECK(empty_token.value == 0);
    CHECK(empty_binding.value == 0);
    CHECK(empty_borrow.ptr == nullptr);
    CHECK(empty_borrow.credential.value == 0);
    CHECK(empty_bytes.data == nullptr && empty_bytes.size == 0);
}

TEST_CASE(bytes_rule_has_a_concrete_reference_shape)
{
    static_assert(bytes_rule_holds(a::bytes{nullptr, 0}));
    static_assert(!bytes_rule_holds(a::bytes{nullptr, 1}));

    const char text[] = "{}";
    bytes_writer writer;
    a::iwriter* const base = &writer; // Used through the interface, exactly as icalls would.

    CHECK(base->write(a::bytes{nullptr, 0}) == a::ok); // legal empty append
    CHECK(writer.accepted == 0);
    CHECK(!bytes_rule_holds(a::bytes{nullptr, 1}));
    CHECK(base->write(a::bytes{nullptr, 1}) == a::invalid_argument); // null requires size zero
    CHECK(writer.accepted == 0);
    CHECK(base->write(a::bytes{text, 2}) == a::ok);
    CHECK(writer.accepted == 2);
}

TEST_CASE(mock_entry_rejects_major_mismatch_and_clears_output)
{
    // The mock entry is the plugin-side stand-in for the exported symbol: its type must be exactly
    // the frozen entry type, including the calling convention and noexcept.
    static_assert(std::is_same_v<decltype(u42_abi_test_mock_entry),
                                 std::remove_pointer_t<a::entry_fn>>);
    static_assert(std::is_same_v<decltype(&u42_abi_test_mock_entry), a::entry_fn>);

    a::iplug_fty* out = stale_sentinel<a::iplug_fty>();
    CHECK(u42_abi_test_mock_entry(a::abi_major + 1, &out) == a::unsupported);
    CHECK(out == nullptr); // A rejected negotiation never leaves a stale factory behind.

    out = stale_sentinel<a::iplug_fty>();
    CHECK(u42_abi_test_mock_entry(0, &out) == a::invalid_argument);
    CHECK(out == nullptr);

    out = nullptr;
    CHECK(u42_abi_test_mock_entry(a::abi_major + 1, &out) == a::unsupported);
    CHECK(out == nullptr);

    // Required output pointer: null is a parameter error, not a crash and not a success.
    CHECK(u42_abi_test_mock_entry(a::abi_major, nullptr) == a::invalid_argument);

    out = nullptr;
    CHECK(u42_abi_test_mock_entry(a::abi_major, &out) == a::ok);
    CHECK(out == mock_factory());
}

TEST_CASE(mock_factory_clears_outputs_on_failure)
{
    factory_state& state = mock_state();
    state = factory_state{};
    a::iplug_fty* const factory = mock_factory();

    const a::plug_desc* desc = nullptr;
    CHECK(factory->describe(&desc) == a::ok);
    CHECK(desc != nullptr);
    CHECK(desc->struct_size == sizeof(a::plug_desc));
    CHECK(cstr_equal(desc->plug_id, "u42.test.mock"));
    CHECK(cstr_equal(desc->version, "1.0.0"));
    CHECK(desc->priority == 10);
    // The documented array rule: a null array pointer is only legal with a zero count.
    CHECK((desc->before_count != 0) == (desc->before != nullptr));
    CHECK((desc->after_count != 0) == (desc->after != nullptr));
    CHECK(state.describe_calls == 1);

    desc = stale_sentinel<const a::plug_desc>();
    state.fail_describe = true;
    CHECK(factory->describe(&desc) == a::failed);
    CHECK(desc == nullptr);
    state.fail_describe = false;

    CHECK(factory->describe(nullptr) == a::invalid_argument);
    CHECK(factory->create(nullptr) == a::invalid_argument);

    a::iplug* plugin = stale_sentinel<a::iplug>();
    CHECK(factory->create(&plugin) == a::ok);
    CHECK(plugin != nullptr);
    CHECK(state.live_instances == 1);

    a::iplug* refused = stale_sentinel<a::iplug>();
    state.fail_create = true;
    CHECK(factory->create(&refused) == a::failed);
    CHECK(refused == nullptr);
    CHECK(state.live_instances == 1); // A failed create leaves no half-built instance behind.
    state.fail_create = false;

    // Rollback of a created-but-never-initialized instance still goes through destroy().
    plugin->destroy();
    CHECK(state.destroy_calls == 1);
    CHECK(state.live_instances == 0);
}

TEST_CASE(mock_plugin_lifecycle_destroys_through_the_allocator)
{
    factory_state& state = mock_state();
    state = factory_state{};
    mock_context ctx;

    // The instance is allocated on its own side and is only ever handled through the root
    // interface, exactly as a loaded library's instance would be.
    mock_plugin* const instance = new mock_plugin(state);
    a::iplug* const plugin = instance;
    CHECK(state.live_instances == 1);

    // The documented order is init, start, stop, destroy; out-of-order calls are state errors and
    // leave the instance destroyable.
    CHECK(plugin->start() == a::invalid_state);
    CHECK(plugin->stop() == a::invalid_state);
    CHECK(plugin->init(nullptr) == a::invalid_argument);
    CHECK(plugin->init(&ctx) == a::ok);
    CHECK(plugin->init(&ctx) == a::invalid_state); // init is exactly-once
    CHECK(state.init_calls == 1);
    CHECK(plugin->start() == a::ok);
    CHECK(plugin->start() == a::invalid_state);
    CHECK(plugin->stop() == a::ok);
    CHECK(plugin->stop() == a::invalid_state);

    // Optional interface query: the published pointer is the iinvoke base subobject, which is a
    // different address than the iplug root because both are unrelated polymorphic bases. The
    // design forbids deriving one interface address from another, and this is why.
    // For the same reason a static_cast from iplug* to iinvoke* does not even compile here: the
    // query result is the only way to an optional interface, and it must carry the adjustment.
    void* raw = stale_sentinel<void>();
    CHECK(plugin->query(&a::invoke_iid, &raw) == a::ok);
    CHECK(raw != nullptr);
    CHECK(raw == instance->iinvoke_address());
    CHECK(raw != static_cast<void*>(plugin));
    CHECK(static_cast<a::iinvoke*>(raw)->invoke(7, a::bytes{nullptr, 0}, nullptr) ==
          a::invalid_argument);
    CHECK(state.invoke_calls == 0);

    void* again = nullptr;
    CHECK(plugin->query(&a::invoke_iid, &again) == a::ok);
    CHECK(again == raw); // borrowed interface pointers are stable while the instance lives

    void* unknown = stale_sentinel<void>();
    CHECK(plugin->query(&a::events_iid, &unknown) == a::unsupported);
    CHECK(unknown == nullptr); // unknown identifiers clear the output

    void* missing = stale_sentinel<void>();
    CHECK(plugin->query(nullptr, &missing) == a::invalid_argument);
    CHECK(missing == nullptr);
    CHECK(plugin->query(&a::invoke_iid, nullptr) == a::invalid_argument);

    // The allocating side destroys: destroy() must release the allocation itself, and the caller
    // must not touch the instance afterwards.
    plugin->destroy();
    CHECK(state.destroy_calls == 1);
    CHECK(state.live_instances == 0);
}

TEST_CASE(mock_context_query_publishes_adjusted_interface_pointers)
{
    mock_context ctx;
    a::ictx* const host = &ctx;

    void* events_raw = stale_sentinel<void>();
    CHECK(host->query(&a::events_iid, &events_raw) == a::ok);
    CHECK(events_raw == static_cast<void*>(static_cast<a::ievents*>(&ctx)));
    CHECK(events_raw != static_cast<void*>(&ctx)); // the secondary base needs an adjustment

    void* caps_raw = stale_sentinel<void>();
    CHECK(host->query(&a::caps_iid, &caps_raw) == a::ok);
    CHECK(caps_raw == static_cast<void*>(static_cast<a::icaps*>(&ctx)));
    CHECK(caps_raw != events_raw);
    CHECK(caps_raw != static_cast<void*>(&ctx));

    // A caller that casts the published void* back to the queried type must reach this object's
    // own override; that only works because each pointer is the right base subobject.
    CHECK(static_cast<a::ievents*>(events_raw)->publish("u42.test.event", a::bytes{nullptr, 0}) ==
          a::ok);
    CHECK(ctx.state.events_calls == 1);
    CHECK(static_cast<a::icaps*>(caps_raw)->unwatch(a::token{}) == a::unsupported);
    CHECK(ctx.state.caps_calls == 1);

    void* again = nullptr;
    CHECK(host->query(&a::events_iid, &again) == a::ok);
    CHECK(again == events_raw);

    void* unknown = stale_sentinel<void>();
    CHECK(host->query(&a::invoke_iid, &unknown) == a::unsupported);
    CHECK(unknown == nullptr);

    void* cleared = stale_sentinel<void>();
    CHECK(host->query(nullptr, &cleared) == a::invalid_argument);
    CHECK(cleared == nullptr);
    CHECK(host->query(&a::events_iid, nullptr) == a::invalid_argument);
    CHECK(ctx.state.query_calls == 5);
}

std::vector<test_case>& tests()
{
    static std::vector<test_case> all;
    return all;
}

} // namespace

int main()
{
    for (const test_case& test : tests()) {
        g_current_test = test.name;
        test.run();
    }
    std::printf("%zu test cases passed\n", tests().size());
    std::printf("note: this pins the declared C++ contract of u42/abi.hpp only. It does not prove "
                "binary compatibility between differently compiled binaries, and off _WIN32 it "
                "cannot pin a concrete calling convention (U42_CALL is empty there).\n");
    return 0;
}
