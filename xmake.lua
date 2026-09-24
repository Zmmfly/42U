set_project("42u")
set_version("0.3.0")
set_xmakever("2.9.0")
set_languages("c++17")
set_warnings("all", "error")
add_rules("mode.debug", "mode.release")

add_requires("cli11 2.7.2")

option("sanitizer")
    set_default("none")
    set_values("none", "address", "undefined")
    set_showmenu(true)
    set_description("Runtime sanitizer for the host, plugins and tests")
option_end()

if has_config("sanitizer") and get_config("sanitizer") ~= "none" then
    set_policy("build.sanitizer." .. get_config("sanitizer"), true)
end

local output = "$(builddir)/$(plat)/$(arch)/$(mode)"
local plugin_suffix = ".u42.so"
if is_plat("windows") then
    plugin_suffix = ".u42.dll"
elseif is_plat("macosx") then
    plugin_suffix = ".u42.dylib"
end

target("42u")
    set_kind("static")
    add_files("src/*.cc")
    add_includedirs("inc", {public = true})
    add_headerfiles("inc/(42u/*.hpp)")
    if is_plat("linux") then
        add_syslinks("dl", "pthread", {public = true})
    end

target("42uhost_cli")
    set_kind("static")
    set_default(false)
    add_files("host/src/cli_catalog.cc", "host/src/cli_frontend.cc")
    add_includedirs("host/include", {public = true})
    add_deps("42u")
    add_packages("cli11")

target("42uhost")
    set_kind("binary")
    add_files("host/src/main.cc")
    add_deps("42uhost_cli")

for _, name in ipairs({"echo", "consumer"}) do
    target(name)
        set_kind("shared")
        set_filename(name .. plugin_suffix)
        set_targetdir(output .. "/plugins")
        add_files("examples/" .. name .. ".cc")
        add_includedirs("inc", "examples")
        if not is_plat("windows") then
            add_cxflags("-fvisibility=hidden")
        end
end

for _, name in ipairs({"bad_abi", "missing_entry", "legacy_v1", "legacy_v2"}) do
    target("fixture_" .. name)
        set_kind("shared")
        set_filename(name .. plugin_suffix)
        set_targetdir(output .. "/fixtures")
        add_files("tests/fixtures/" .. name .. ".cc")
        add_includedirs("inc")
        if not is_plat("windows") then
            add_cxflags("-fvisibility=hidden")
        end
end

for _, name in ipairs({"cli_logging", "cli_service", "cli_bad_manifest"}) do
    target("fixture_" .. name)
        set_kind("shared")
        set_filename(name .. plugin_suffix)
        set_targetdir(output .. "/fixtures/cli")
        add_files("tests/fixtures/" .. name .. ".cc")
        add_includedirs("inc")
        if not is_plat("windows") then
            add_cxflags("-fvisibility=hidden")
        end
end

if not is_plat("windows") then
    for _, fixture in ipairs({
        {name = "primary", define = "U42_CLI_RUNTIME_PRIMARY_FIXTURE"},
        {name = "secondary", define = "U42_CLI_RUNTIME_SECONDARY_FIXTURE"},
        {name = "conflict", define = "U42_CLI_RUNTIME_CONFLICT_FIXTURE"}
    }) do
        target("fixture_cli_runtime_" .. fixture.name)
            set_kind("shared")
            set_filename("cli_runtime_" .. fixture.name .. plugin_suffix)
            set_targetdir(output .. "/fixtures/cli_runtime")
            add_files("tests/cli_runtime_test.cc")
            add_defines(fixture.define)
            add_includedirs("inc")
            add_cxflags("-fvisibility=hidden")
    end
end

-- These real DSOs share one fixture implementation, but never link the host library.
for _, name in ipairs({"provider", "created_a", "created_b", "batch_good", "batch_fail", "batch_tail"}) do
    target("fixture_boot_" .. name)
        set_kind("shared")
        set_filename(name .. plugin_suffix)
        set_targetdir(output .. "/fixtures/boot/" .. (name:sub(1, 6) == "batch_" and "batch" or "base"))
        add_files("tests/boot_test.cc")
        add_defines("U42_BOOT_" .. name:upper() .. "_FIXTURE")
        add_includedirs("inc")
        if not is_plat("windows") then
            add_cxflags("-fvisibility=hidden")
        end
end

target("boot_test")
    set_kind("binary")
    add_files("tests/boot_test.cc")
    add_deps("42u")
    add_deps("fixture_boot_provider", "fixture_boot_created_a", "fixture_boot_created_b",
             "fixture_boot_batch_good", "fixture_boot_batch_fail", "fixture_boot_batch_tail",
             {inherit = false})
    set_rundir("$(projectdir)")
    after_load(function (target)
        target:add("tests", "default", {runargs = {
            path.absolute(target:dep("fixture_boot_provider"):targetfile()),
            path.absolute(target:dep("fixture_boot_created_a"):targetfile()),
            path.absolute(target:dep("fixture_boot_created_b"):targetfile()),
            path.absolute(target:dep("fixture_boot_batch_good"):targetdir())
        }})
    end)

for _, name in ipairs({"order", "sdk", "abi", "runtime", "event", "safety", "reentrant", "withdraw", "lease"}) do
    target(name .. "_test")
        set_kind("binary")
        add_files("tests/" .. name .. "_test.cc")
        add_deps("42u")
        add_tests("default")
end

target("cli_abi_test")
    set_kind("binary")
    add_files("tests/cli_abi_test.cc")
    add_includedirs("host/include")
    add_deps("42u")
    add_tests("default")

target("cli_frontend_test")
    set_kind("binary")
    add_files("tests/cli_frontend_test.cc")
    add_deps("42uhost_cli")
    add_tests("default")

if not is_plat("windows") then
    target("cli_runtime_test")
        set_kind("binary")
        add_files("tests/cli_runtime_test.cc")
        add_deps("42u")
        add_deps("fixture_cli_runtime_primary", "fixture_cli_runtime_secondary",
                 "fixture_cli_runtime_conflict", {inherit = false})
        set_rundir("$(projectdir)")
        after_load(function (target)
            target:add("tests", "default", {runargs = {
                path.absolute(target:dep("fixture_cli_runtime_primary"):targetfile()),
                path.absolute(target:dep("fixture_cli_runtime_secondary"):targetfile()),
                path.absolute(target:dep("fixture_cli_runtime_conflict"):targetfile())
            }})
        end)
end

target("cli_e2e_test")
    set_kind("binary")
    add_files("tests/cli_e2e_test.cc")
    add_includedirs("host/include", "inc")
    add_deps("42uhost", "fixture_cli_logging", "fixture_cli_service",
             "fixture_cli_bad_manifest", "echo", {inherit = false})
    set_rundir("$(projectdir)")
    after_load(function (target)
        target:add("tests", "default", {runargs = {
            path.absolute(target:dep("42uhost"):targetfile()),
            path.absolute(target:dep("fixture_cli_logging"):targetfile()),
            path.absolute(target:dep("fixture_cli_service"):targetfile()),
            path.absolute(target:dep("fixture_cli_bad_manifest"):targetfile()),
            path.absolute(target:dep("echo"):targetfile())
        }})
    end)

target("integration_test")
    set_kind("binary")
    add_files("tests/integration_test.cc")
    add_deps("42u")
    add_deps("echo", "consumer", "fixture_bad_abi", "fixture_missing_entry", "fixture_legacy_v1",
             "fixture_legacy_v2", {inherit = false})
    set_rundir("$(projectdir)")
    after_load(function (target)
        target:add("tests", "default", {runargs = {
            path.absolute(target:dep("echo"):targetdir()),
            path.absolute(target:dep("fixture_bad_abi"):targetfile()),
            path.absolute(target:dep("fixture_missing_entry"):targetfile()),
            path.absolute(target:dep("fixture_legacy_v1"):targetfile()),
            path.absolute(target:dep("fixture_legacy_v2"):targetfile())
        }})
    end)
