set_project("42u")
set_languages("c++latest")

add_rules("mode.debug", "mode.release")
add_requires("spdlog", "asio", "fmt")

target("42u")
    set_kind("static")
    add_files("src/**.cc")
    add_includedirs("inc", {public = true})
    add_packages("spdlog", "asio", "fmt", {public = true})

target("42uhost")
    set_kind("binary")
    add_files("host/src/**.cc")
    add_includedirs("host/inc")
    add_deps("42u")
