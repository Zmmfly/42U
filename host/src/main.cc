/**
 * @file main.cc
 * @brief Process entry for the pure-host 42U command-line application.
 */
#include <42u/cli_host.hpp>

#include <exception>
#include <iostream>

/**
 * @brief Construct and run the pure-host command-line application.
 *
 * @param argc Number of command-line arguments, including the program name.
 * @param argv Borrowed command-line argument vector.
 * @return 0 on success, 1 on host/plugin failure, or 2 on invalid user input.
 */
int main(int argc, char** argv)
{
    try {
        u42::cli::application app({"0.3.0", {}});
        return app.run(argc, argv);
    } catch (const std::exception& exception) {
        std::cerr << "error: application construction failed: " << exception.what() << '\n';
    } catch (...) {
        std::cerr << "error: application construction failed with an unknown exception\n";
    }
    return u42::cli::exit_failure;
}
