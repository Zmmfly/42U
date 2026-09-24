/**
 * @file cli_frontend_test.cc
 * @brief Dependency-free unit tests for the owned CLI catalog and restricted parser.
 */

#define NDEBUG 1

#include <42u/cli_host.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace abi = u42::abi::v3;
namespace cli = u42::cli;
namespace wire = u42::cli::v1;

constexpr abi::method_id serve_method = 42u;
constexpr abi::method_id inspect_method = 43u;
const char* phase = "startup";

/** @brief Convert one string literal to a length-delimited ABI view. */
template <std::size_t Size>
constexpr wire::text_view text(const char (&value)[Size]) noexcept
{
    return {value, Size - 1};
}

/** @brief Report one failed check with the active test phase. */
[[noreturn]] void fail_check(const char* expression, const char* file, int line)
{
    std::fprintf(stderr, "CHECK failed [%s]: %s (%s:%d)\n", phase, expression, file, line);
    std::exit(EXIT_FAILURE);
}

/** @brief Runtime assertion that remains active under NDEBUG. */
#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) fail_check(#condition, __FILE__, __LINE__);           \
    } while (false)

/** @brief Temporarily label checks with one test case name. */
class phase_guard {
public:
    /** @brief Install one phase label until this guard is destroyed. */
    explicit phase_guard(const char* value) noexcept : previous_(phase) { phase = value; }
    /** @brief Restore the enclosing phase label. */
    ~phase_guard() { phase = previous_; }

private:
    const char* previous_;
};

constexpr wire::text_view log_defaults[]{text("info")};

const wire::parameter_desc logging_roots[]{
    {sizeof(wire::parameter_desc),
     0u,
     0u,
     wire::text,
     wire::no_position,
     text("log-level"),
     text("log-level"),
     text("Set the logging persistence level."),
     text("LEVEL"),
     {},
     {log_defaults, 1u},
     {}},
};

const wire::manifest logging_manifest{sizeof(wire::manifest),
                                      0u,
                                      0u,
                                      {logging_roots, 1u},
                                      {}};

const wire::parameter_desc serve_parameters[]{
    {sizeof(wire::parameter_desc),
     0u,
     wire::parameter_required,
     wire::text,
     wire::no_position,
     text("port"),
     text("port"),
     text("Required service port."),
     text("PORT"),
     {},
     {},
     {}},
    {sizeof(wire::parameter_desc),
     0u,
     wire::parameter_repeatable,
     wire::text,
     wire::no_position,
     text("label"),
     text("label"),
     text("Ordered label."),
     text("LABEL"),
     {},
     {},
     {}},
    {sizeof(wire::parameter_desc),
     0u,
     0u,
     wire::flag,
     wire::no_position,
     text("verbose"),
     text("verbose"),
     text("Enable verbose mode."),
     {},
     {},
     {},
     {}},
    {sizeof(wire::parameter_desc),
     0u,
     0u,
     wire::text,
     0u,
     text("target"),
     {},
     text("Service target."),
     text("TARGET"),
     {},
     {},
     {}},
    {sizeof(wire::parameter_desc),
     0u,
     0u,
     wire::text,
     wire::no_position,
     text("mode-option"),
     text("mode"),
     text("Plugin-local execution mode."),
     text("MODE"),
     text("mode"),
     {},
     {}},
};

const wire::parameter_desc inspect_parameters[]{
    {sizeof(wire::parameter_desc),
     0u,
     wire::parameter_required,
     wire::text,
     wire::no_position,
     text("format"),
     text("format"),
     text("Required output format."),
     text("FORMAT"),
     {},
     {},
     {}},
};

const wire::command_desc service_commands[]{
    {sizeof(wire::command_desc),
     0u,
     0u,
     serve_method,
     0u,
     text("serve"),
     text("serve"),
     text("Execute one finite service request."),
     {serve_parameters, 5u}},
    {sizeof(wire::command_desc),
     0u,
     0u,
     inspect_method,
     0u,
     text("inspect"),
     text("inspect"),
     text("Inspect fixture state."),
     {inspect_parameters, 1u}},
};

const wire::manifest service_manifest{sizeof(wire::manifest),
                                      0u,
                                      0u,
                                      {},
                                      {service_commands, 2u}};

const wire::parameter_desc invalid_roots[]{
    {sizeof(wire::parameter_desc),
     0u,
     0u,
     wire::flag,
     wire::no_position,
     text("bad-help"),
     text("help"),
     {},
     {},
     {},
     {},
     {}},
};

const wire::manifest invalid_manifest{sizeof(wire::manifest),
                                      0u,
                                      0u,
                                      {invalid_roots, 1u},
                                      {}};

const wire::parameter_desc incompatible_text_config[]{
    {sizeof(wire::parameter_desc), 0u, 0u, wire::text, wire::no_position,
     text("text-value"), text("text-value"), {}, text("VALUE"), text("shared"), {}, {}},
};
const wire::parameter_desc incompatible_flag_config[]{
    {sizeof(wire::parameter_desc), 0u, 0u, wire::flag, wire::no_position,
     text("flag-value"), text("flag-value"), {}, {}, text("shared"), {}, {}},
};
const wire::command_desc incompatible_commands[]{
    {sizeof(wire::command_desc), 0u, 0u, 1u, 0u, text("first"), text("first"), {},
     {incompatible_text_config, 1u}},
    {sizeof(wire::command_desc), 0u, 0u, 2u, 0u, text("second"), text("second"), {},
     {incompatible_flag_config, 1u}},
};
const wire::manifest incompatible_config_manifest{sizeof(wire::manifest), 0u, 0u, {},
                                                  {incompatible_commands, 2u}};

/** @brief Create the two-plugin catalog shared by parser cases. */
cli::catalog make_catalog()
{
    cli::catalog surface;
    std::string error;
    CHECK(surface.add("com.example.cli.logging", &logging_manifest, &error) == abi::ok);
    CHECK(error.empty());
    CHECK(surface.add("com.example.cli.service", &service_manifest, &error) == abi::ok);
    CHECK(error.empty());
    return surface;
}

/** @brief Parse an owning argv vector through the public frontend entry. */
abi::status parse(const cli::catalog& surface, const std::vector<std::string>& arguments,
                  cli::parse_result& out, std::string& error)
{
    std::vector<const char*> pointers;
    pointers.reserve(arguments.size());
    for (const std::string& argument : arguments) pointers.push_back(argument.c_str());
    return cli::parse_invocation(surface, static_cast<int>(pointers.size()), pointers.data(),
                                 &out, &error);
}

/** @brief Require one invocation to parse as an executable command. */
cli::parse_result parse_execute(const cli::catalog& surface,
                                const std::vector<std::string>& arguments)
{
    cli::parse_result result;
    std::string error;
    CHECK(parse(surface, arguments, result, error) == abi::ok);
    CHECK(error.empty());
    CHECK(result.kind == cli::parse_kind::execute);
    CHECK(result.value.command.has_value());
    return result;
}

/** @brief Require one invocation to be rejected as user input. */
void expect_usage_error(const cli::catalog& surface,
                        const std::vector<std::string>& arguments)
{
    cli::parse_result result;
    result.rendered_help = "dirty";
    std::string error;
    CHECK(parse(surface, arguments, result, error) == abi::invalid_argument);
    CHECK(!error.empty());
    CHECK(result.rendered_help.empty());
    CHECK(!result.value.command.has_value());
}

/** @brief Find one owning plugin configuration in a parsed invocation. */
const cli::plugin_configuration& configuration(const cli::invocation& value,
                                               std::string_view plugin_id)
{
    for (const cli::plugin_configuration& candidate : value.configurations) {
        if (candidate.plugin_id == plugin_id) return candidate;
    }
    fail_check("configuration exists", __FILE__, __LINE__);
}

/** @brief Find one exact key in a plugin-local parsed configuration. */
const cli::configuration_value& config_value(const cli::plugin_configuration& owner,
                                             std::string_view key)
{
    for (const cli::configuration_value& candidate : owner.entries) {
        if (candidate.key == key) return candidate;
    }
    fail_check("configuration value exists", __FILE__, __LINE__);
}

/** @brief Verify catalog copying and deterministic rejection of a reserved host name. */
void catalog_checks()
{
    phase_guard guard("catalog");
    cli::catalog surface = make_catalog();
    CHECK(surface.root_parameters().size() == 1u);
    CHECK(surface.commands().size() == 2u);
    CHECK(surface.root_parameters()[0].plugin_id == "com.example.cli.logging");
    CHECK(surface.root_parameters()[0].default_values == std::vector<std::string>{"info"});
    CHECK(surface.commands()[0].handler == serve_method);

    std::string error;
    CHECK(surface.add("com.example.cli.bad-manifest", &invalid_manifest, &error) ==
          abi::duplicate);
    CHECK(error.find("<host>") != std::string::npos);
    CHECK(surface.root_parameters().size() == 1u);
    CHECK(surface.commands().size() == 2u);

    cli::catalog incompatible;
    error.clear();
    CHECK(incompatible.add("com.example.cli.incompatible", &incompatible_config_manifest,
                           &error) == abi::invalid_argument);
    CHECK(error.find("incompatible kind or sensitivity") != std::string::npos);
    CHECK(incompatible.commands().empty());
}

/** @brief Verify plugin defaults, explicit empty strings, and their source metadata. */
void default_and_empty_checks()
{
    phase_guard guard("defaults and empty values");
    const cli::catalog surface = make_catalog();

    const cli::parse_result defaults =
        parse_execute(surface, {"42u", "serve", "--port", "8080", "node-a"});
    const cli::plugin_configuration& logging =
        configuration(defaults.value, "com.example.cli.logging");
    const cli::configuration_value& default_level = config_value(logging, "log-level");
    CHECK(default_level.source == wire::plugin_default);
    CHECK(default_level.values == std::vector<std::string>{"info"});
    CHECK(defaults.value.command->payload == "{\"port\":\"8080\",\"target\":\"node-a\"}");

    const cli::parse_result equals_empty = parse_execute(
        surface, {"42u", "--log-level=", "serve", "--port", "8080", "node-a"});
    const cli::configuration_value& equals_level = config_value(
        configuration(equals_empty.value, "com.example.cli.logging"), "log-level");
    CHECK(equals_level.source == wire::command_line);
    CHECK(equals_level.values == std::vector<std::string>{""});

    const cli::parse_result separate_empty = parse_execute(
        surface, {"42u", "--log-level", "", "serve", "--port", "8080", "node-a"});
    const cli::configuration_value& separate_level = config_value(
        configuration(separate_empty.value, "com.example.cli.logging"), "log-level");
    CHECK(separate_level.source == wire::command_line);
    CHECK(separate_level.values == std::vector<std::string>{""});
}

/** @brief Verify payload shape, repeat order, flags, and config_key redirection. */
void payload_checks()
{
    phase_guard guard("payload and config routing");
    const cli::catalog surface = make_catalog();
    const cli::parse_result parsed = parse_execute(
        surface,
        {"42u", "--log-level", "debug", "serve", "--port", "8080", "--label", "a",
         "--label", "b", "--verbose", "node-a", "--mode", "fast"});

    CHECK(parsed.value.command->plugin_id == "com.example.cli.service");
    CHECK(parsed.value.command->command_id == "serve");
    CHECK(parsed.value.command->handler == serve_method);
    CHECK(parsed.value.command->payload ==
          "{\"port\":\"8080\",\"label\":[\"a\",\"b\"],\"verbose\":true,"
          "\"target\":\"node-a\"}");
    CHECK(parsed.value.command->arguments.size() == 4u);

    const cli::configuration_value& level = config_value(
        configuration(parsed.value, "com.example.cli.logging"), "log-level");
    CHECK(level.source == wire::command_line);
    CHECK(level.values == std::vector<std::string>{"debug"});

    const cli::plugin_configuration& service =
        configuration(parsed.value, "com.example.cli.service");
    CHECK(service.entries.size() == 1u);
    const cli::configuration_value& mode = config_value(service, "mode");
    CHECK(mode.source == wire::command_line);
    CHECK(mode.values == std::vector<std::string>{"fast"});
    CHECK(parsed.value.command->payload.find("mode") == std::string::npos);
}

/** @brief Verify explicit text values accept strict UTF-8 and reject malformed byte sequences. */
void utf8_checks()
{
    phase_guard guard("UTF-8 values");
    const cli::catalog surface = make_catalog();
    const std::string unicode = "\xE6\x97\xA5\xE5\xBF\x97";
    const cli::parse_result parsed = parse_execute(
        surface, {"42u", "--log-level", unicode, "serve", "--port", "8080", "--label",
                  unicode, "node-a", "--mode", unicode});
    CHECK(config_value(configuration(parsed.value, "com.example.cli.logging"), "log-level")
              .values == std::vector<std::string>{unicode});
    CHECK(config_value(configuration(parsed.value, "com.example.cli.service"), "mode").values ==
          std::vector<std::string>{unicode});
    CHECK(parsed.value.command->payload.find(unicode) != std::string::npos);

    const std::string invalid("\xC3\x28", 2u);
    expect_usage_error(
        surface, {"42u", "--log-level", invalid, "serve", "--port", "8080", "node-a"});
    expect_usage_error(surface,
                       {"42u", std::string("--log-level=") + invalid, "serve", "--port",
                        "8080", "node-a"});
    expect_usage_error(surface,
                       {"42u", "serve", "--port", "8080", "--label", invalid, "node-a"});
    expect_usage_error(surface,
                       {"42u", "serve", "--port", "8080", "node-a", "--mode", invalid});
    expect_usage_error(surface, {"42u", "serve", "--port", "8080", "--", invalid});
}

/** @brief Verify -- escapes all remaining tokens into the command's unfilled positionals. */
void positional_escape_checks()
{
    phase_guard guard("positional escape");
    const cli::catalog surface = make_catalog();

    const cli::parse_result negative =
        parse_execute(surface, {"42u", "serve", "--port", "8080", "--", "-1"});
    CHECK(negative.value.command->payload == "{\"port\":\"8080\",\"target\":\"-1\"}");

    const cli::parse_result long_option =
        parse_execute(surface, {"42u", "serve", "--port", "8080", "--", "--node"});
    CHECK(long_option.value.command->payload ==
          "{\"port\":\"8080\",\"target\":\"--node\"}");

    expect_usage_error(surface, {"42u", "--", "serve", "--port", "8080"});
    expect_usage_error(surface, {"42u", "serve", "--", "--port"});
    expect_usage_error(surface,
                       {"42u", "serve", "--port", "8080", "--", "--node", "extra"});
    expect_usage_error(surface,
                       {"42u", "serve", "--port", "8080", "node-a", "--"});
}

/** @brief Verify help bypasses command required checks without producing an invocation. */
void help_checks()
{
    phase_guard guard("help");
    const cli::catalog surface = make_catalog();
    cli::parse_result result;
    std::string error;

    CHECK(parse(surface, {"42u", "--help"}, result, error) == abi::ok);
    CHECK(result.kind == cli::parse_kind::root_help);
    CHECK(result.rendered_help.find("--log-level") != std::string::npos);
    CHECK(result.rendered_help.find("serve") != std::string::npos);
    CHECK(!result.value.command.has_value());

    CHECK(parse(surface, {"42u", "serve", "--help"}, result, error) == abi::ok);
    CHECK(result.kind == cli::parse_kind::command_help);
    CHECK(result.rendered_help.find("--port") != std::string::npos);
    CHECK(!result.value.command.has_value());
}

/** @brief Verify 42U's root/local ordering and restricted syntax boundary. */
void restricted_syntax_checks()
{
    phase_guard guard("restricted syntax");
    const cli::catalog surface = make_catalog();

    expect_usage_error(surface, {"42u"});
    expect_usage_error(surface, {"42u", "serve", "--port", "8080", "node-a", "--log-level", "debug"});
    expect_usage_error(surface, {"42u", "--port", "8080", "serve", "node-a"});
    expect_usage_error(surface, {"42u", "--serve.port=8080"});
    expect_usage_error(surface, {"42u", "serve", "--port", "8080", "node-a", "++"});
    expect_usage_error(surface, {"42u", "serve", "--port", "8080", "node-a", "--"});
    expect_usage_error(surface, {"42u", "-h"});
    expect_usage_error(surface, {"42u", "serv", "--port", "8080", "node-a"});
    expect_usage_error(surface, {"42u", "serve", "--port", "8080", "node-a", "extra"});
    expect_usage_error(surface,
                       {"42u", "serve", "--port", "8080", "node-a", "inspect"});
    expect_usage_error(surface,
                       {"42u", "serve", "--port", "8080", "--port", "9090", "node-a"});
    expect_usage_error(surface, {"42u", "serve", "node-a"});
    expect_usage_error(surface, {"42u", "inspect"});

    const cli::parse_result inspect =
        parse_execute(surface, {"42u", "inspect", "--format", "json"});
    CHECK(inspect.value.command->handler == inspect_method);
    CHECK(inspect.value.command->payload == "{\"format\":\"json\"}");
}

} // namespace

/** @brief Run all catalog and restricted-frontend checks. */
int main()
{
    catalog_checks();
    default_and_empty_checks();
    payload_checks();
    utf8_checks();
    positional_escape_checks();
    help_checks();
    restricted_syntax_checks();
    std::fprintf(stdout, "cli_frontend_test: all checks passed\n");
    return EXIT_SUCCESS;
}
