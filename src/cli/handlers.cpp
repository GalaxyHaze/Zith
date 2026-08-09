#include "cli/actions.hpp"

#include "cli/cli.hpp"
#include "cli/terminal.hpp"

#include <cstdio>

int show_help() {
    const generated_cli::term::Terminal terminal = generated_cli::term::init();
    generated_cli::term::UsagePrinter out{stdout, terminal.stdoutColor};

    out.bold("zith build tool");
    std::fputc('\n', stdout);
    std::fputc('\n', stdout);

    out.section("Usage");
    std::fprintf(stdout, "  zithc [options] [input] [output]\n");
    std::fprintf(stdout, "  zithc <command> [command-options] [arguments]\n\n");

    out.section("Options");
    out.flag("-h, --help", "Show this help message");
    out.flag("--version", "Show version information");
    out.flag("-v, --verbose", "Enable verbose output");
    out.flag("-s, --strict", "Enable strict mode");
    out.flag("--opt-level <0..3>", "Set optimization level");
    out.flag("-I, --include <paths>", "Add include directories");
    out.flag("-j, --jobs <n>", "Set worker count");
    std::fputc('\n', stdout);

    out.section("Commands");
    out.flag("check", "Run the check command");
    out.flag("fmt", "Run the formatter");
    out.flag("run", "Run a script");
    out.flag("build", "Build an output artifact");
    out.flag("deps", "Manage dependencies");
    std::fputc('\n', stdout);

    out.section("Subcommands");
    out.flag("deps add", "Add a dependency");
    out.flag("deps check", "Check dependencies");
    out.flag("deps remove", "Remove a dependency");
    std::fputc('\n', stdout);

    out.section("Command Flags");
    out.flag("fmt --color <mode>", "Formatter color mode: auto, off, on");
    out.flag("fmt --check", "Check formatting without rewriting");
    out.flag("deps add --force", "Force dependency addition");
    return 0;
}

int show_version() {
    const generated_cli::term::Terminal terminal = generated_cli::term::init();
    generated_cli::term::UsagePrinter out{stdout, terminal.stdoutColor};
    out.bold("zith 0.1.0");
    std::fputc('\n', stdout);
    return 0;
}

namespace cmd {

int check(const generated_cli::Options &) {
    return 0;
}

int fmt(const generated_cli::Options &) {
    return 0;
}

int run(const generated_cli::Options &) {
    return 0;
}

int build(const generated_cli::Options &) {
    return 0;
}

int depsAdd() {
    return 0;
}

int depsCheck() {
    return 0;
}

int depsRemove() {
    return 0;
}

} // namespace cmd
