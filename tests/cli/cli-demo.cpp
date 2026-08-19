#include "cli/cli.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

bool expect(bool condition, std::string_view message) {
    if (!condition)
        std::cerr << "cli-demo: " << message << '\n';
    return condition;
}

} // namespace

int main() {
    generated_cli::Cli cli;

    const char *helpArgs[] = {"zithc", "--help"};
    const int helpStatus = cli.parseArgs(2, const_cast<char **>(helpArgs));
    if (!expect(helpStatus == 0, "--help parses"))
        return EXIT_FAILURE;
    if (!expect(cli.options.help, "global --help is recorded"))
        return EXIT_FAILURE;
    const int helpDispatch = cli.dispatch();
    if (!expect(helpDispatch == 0, "--help dispatches"))
        return EXIT_FAILURE;

    generated_cli::Cli versionCli;
    const char *versionArgs[] = {"zithc", "--version"};
    const int versionStatus = versionCli.parseArgs(2, const_cast<char **>(versionArgs));
    if (!expect(versionStatus == 0, "--version parses"))
        return EXIT_FAILURE;
    if (!expect(versionCli.pendingAction == generated_cli::PendingAction::ShowVersion,
                "--version selects the version action"))
        return EXIT_FAILURE;
    const int versionDispatch = versionCli.dispatch();
    if (!expect(versionDispatch == 0, "--version dispatches"))
        return EXIT_FAILURE;

    generated_cli::Cli commandCli;
    const char *commandArgs[] = {"zithc", "deps", "add", "pkg"};
    const int commandStatus = commandCli.parseArgs(4, const_cast<char **>(commandArgs));
    if (!expect(commandStatus == 0, "deps add parses"))
        return EXIT_FAILURE;
    if (!expect(commandCli.command == generated_cli::Command::Deps,
                "deps selects the deps command"))
        return EXIT_FAILURE;
    if (!expect(commandCli.depsSubcommand == generated_cli::DepsSubcommand::Add,
                "deps add selects the add subcommand"))
        return EXIT_FAILURE;
    if (!expect(commandCli.dispatch() != 0, "deps add fails until it is wired"))
        return EXIT_FAILURE;

    std::cout << "cli-demo: parse and dispatch checks passed\n";
    return EXIT_SUCCESS;
}
