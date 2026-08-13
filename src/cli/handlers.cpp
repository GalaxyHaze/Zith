#include "cli/cli.hpp"
#include "cli/terminal.hpp"
#include "session/dispatch.hpp"
#include "session/session.hpp"

#include <cstdio>
#include <string_view>

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

namespace {

std::string_view internedPath(const generated_cli::Options &opts, generated_cli::InternId id) {
    return opts.stringPool != nullptr ? opts.stringPool->lookup(id) : std::string_view{};
}

std::string_view firstNonEmpty(std::string_view first, std::string_view second) {
    return !first.empty() ? first : second;
}

std::string_view chooseInput(const generated_cli::Options &opts) {
    return firstNonEmpty(
        firstNonEmpty(firstNonEmpty(internedPath(opts, opts.check.input),
                                    internedPath(opts, opts.run.script)),
                      internedPath(opts, opts.build.input)),
        internedPath(opts, opts.input));
}

std::string_view chooseOutput(const generated_cli::Options &opts) {
    return firstNonEmpty(internedPath(opts, opts.build.output),
                         internedPath(opts, opts.output));
}

int apply(const generated_cli::Options &opts, std::string_view inputPath,
          std::string_view outputPath) {
    (void)outputPath;

    if (inputPath.empty())
        return 1;

    toolkit::session::ZithSessionContext context;
    context.filePath = inputPath;
    context.projectRoot = ".";
    context.options = const_cast<generated_cli::Options *>(&opts);

    toolkit::session::CompilationSession session(context);
    const auto result = session.runTo(toolkit::session::Stage::Lexed);
    if (!result) {
        std::fprintf(stderr, "erro no estágio %s\n", result.error().msg.c_str());
        return 1;
    }
    return 0;
}

} // namespace

namespace cmd {

int check(const generated_cli::Options &opts) {
    return apply(opts, chooseInput(opts), chooseOutput(opts));
}

int fmt(const generated_cli::Options &) {
    return 0;
}

int run(const generated_cli::Options &opts) {
    return apply(opts, chooseInput(opts), chooseOutput(opts));
}

int build(const generated_cli::Options &opts) {
    return apply(opts, chooseInput(opts), chooseOutput(opts));
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
