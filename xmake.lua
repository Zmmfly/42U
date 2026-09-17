set_project("42u")
set_version("0.1.0")
set_xmakever("2.9.0")
set_languages("c++17")
set_warnings("all", "error")
add_rules("mode.debug", "mode.release")

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

target("42uhost")
    set_kind("binary")
    add_files("host/src/*.cc")
    add_deps("42u")

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

for _, name in ipairs({"bad_abi", "missing_entry"}) do
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

for _, name in ipairs({"order", "sdk", "abi", "runtime", "event", "safety", "reentrant", "withdraw"}) do
    target(name .. "_test")
        set_kind("binary")
        add_files("tests/" .. name .. "_test.cc")
        add_deps("42u")
        add_tests("default")
end

target("integration_test")
    set_kind("binary")
    add_files("tests/integration_test.cc")
    add_deps("42u")
    add_deps("echo", "consumer", "fixture_bad_abi", "fixture_missing_entry", {inherit = false})
    set_rundir("$(projectdir)")
    after_load(function (target)
        target:add("tests", "default", {runargs = {
            path.absolute(target:dep("echo"):targetdir()),
            path.absolute(target:dep("fixture_bad_abi"):targetfile()),
            path.absolute(target:dep("fixture_missing_entry"):targetfile())
        }})
    end)
