#include "cli/cli.hpp"
#include "cli/terminal.hpp"
#include "common/memory/arena.hpp"
#include "common/memory/result.hpp"
#include "common/sir/flat/flat.hpp"
#include "session/dispatch.hpp"
#include "session/session.hpp"
#include "support/resource-discovery.hpp"

#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

int show_help() {
    const generated_cli::term::Terminal terminal = generated_cli::term::init();
    generated_cli::term::UsagePrinter out{stdout, terminal.stdoutColor};

    out.bold("zith build tool");
    std::fputc('\n', stdout);
    std::fputc('\n', stdout);

    out.section("Usage");
    std::fprintf(stdout, "  zithc [options] [input]\n");
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
    out.flag("build", "Build project files to an output artifact");
    out.flag("check", "Type-check source files");
    out.flag("clean", "Remove build and cache artifacts");
    out.flag("compile", "Compile a serialized SIR file into a binary");
    out.flag("completion <shell>", "Generate shell completion scripts (bash|zsh|fish)");
    out.flag("create <name>", "Create a new project");
    out.flag("deps", "Manage dependencies");
    out.flag("docs [file]", "Generate documentation");
    out.flag("execute <file>", "Execute a compiled artifact");
    out.flag("fmt", "Format source files");
    out.flag("repl", "Start an interactive REPL");
    out.flag("run", "Build and run a script");
    out.flag("test", "Run project tests");
    std::fputc('\n', stdout);

    out.section("Subcommands");
    out.flag("deps add", "Add a dependency");
    out.flag("deps check", "Check a dependency");
    out.flag("deps list", "List project dependencies");
    out.flag("deps publish", "Publish a dependency");
    out.flag("deps remove", "Remove a dependency");
    out.flag("deps unpublish", "Unpublish a dependency");
    out.flag("deps update", "Update a dependency");
    std::fputc('\n', stdout);

    out.section("Command Flags");
    out.flag("fmt --color <mode>", "Formatter color mode: auto, off, on");
    out.flag("fmt --check", "Check formatting without rewriting");
    out.flag("deps add --force", "Force dependency addition");
    out.flag("<command> --help", "Show mini help for a command");
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

std::string absoluteRoot(std::string_view path) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    if (error)
        return std::string(path);
    return absolute.lexically_normal().string();
}

void populateResourceRoots(const generated_cli::Options &opts,
                           toolkit::session::ZithSessionContext &context) {
    context.stdlibRoots = toolkit::support::findStdlibRoots();

    if (opts.stringPool != nullptr) {
        for (const auto id : opts.includeDirs) {
            const std::string_view include = opts.stringPool->lookup(id);
            if (!include.empty())
                context.includeRoots.push_back(absoluteRoot(include));
        }
    }
    context.systemIncludeRoots = context.includeRoots;

    if (!context.projectRoot.empty()) {
        const std::string assets =
            absoluteRoot(std::string(context.projectRoot) + "/assets");
        context.assetRoots.push_back(assets);
    }
}

int checkHelp(const generated_cli::Options &opts);
int fmtHelp(const generated_cli::Options &opts);
int runHelp(const generated_cli::Options &opts);
int buildHelp(const generated_cli::Options &opts);
int depsHelp(const generated_cli::Options &opts);
int executeHelp(const generated_cli::Options &opts);
int testHelp(const generated_cli::Options &opts);
int docsHelp(const generated_cli::Options &opts);
int replHelp(const generated_cli::Options &opts);
int createHelp(const generated_cli::Options &opts);
int cleanHelp(const generated_cli::Options &opts);
int completionHelp(const generated_cli::Options &opts);
int compileHelp(const generated_cli::Options &opts);

std::string_view chooseCompileInput(const generated_cli::Options &opts) {
    return firstNonEmpty(internedPath(opts, opts.compile.sir),
                         internedPath(opts, opts.input));
}

std::string_view chooseCompileOutput(const generated_cli::Options &opts) {
    return firstNonEmpty(internedPath(opts, opts.compile.outputPath),
                         internedPath(opts, opts.compile.output));
}

void printCommandHelp(const char *usage, const char *description) {
    const generated_cli::term::Terminal terminal = generated_cli::term::init();
    generated_cli::term::UsagePrinter out{stdout, terminal.stdoutColor};
    out.bold(usage);
    std::fputc('\n', stdout);
    std::fputc('\n', stdout);
    std::fprintf(stdout, "  %s\n\n", description);
    out.section("Options");
    out.flag("--help", "Show this help message");
    std::fprintf(stdout, "\n");
}

int genericCommandHelp(bool helpRequested, const char *usage, const char *description) {
    if (helpRequested) {
        printCommandHelp(usage, description);
        return 0;
    }
    return 1;
}

int runPipeline(const generated_cli::Options &opts, std::string_view inputPath,
                toolkit::session::Stage target) {
    if (inputPath.empty()) {
        std::fprintf(stderr, "error: no input file given\n");
        return 1;
    }

    toolkit::session::ZithSessionContext context;
    context.filePath = inputPath;
    context.projectRoot = ".";
    context.options = const_cast<generated_cli::Options *>(&opts);
    populateResourceRoots(opts, context);

    toolkit::session::CompilationSession session(context);
    const auto result = session.runTo(target);
    if (!result) {
        const std::string_view label = toolkit::session::stageLabel(result.error().stage);
        const std::string_view message = result.error().msg;
        std::fprintf(stderr, "error in stage %.*s: %.256s\n",
                     static_cast<int>(label.size()), label.data(),
                     message.empty() ? "(no message)" : message.data());
        return 1;
    }

    if (opts.verbose) {
        std::fprintf(stderr, "reached stage %s for %.*s\n",
                     toolkit::session::stageLabel(target),
                     static_cast<int>(inputPath.size()), inputPath.data());
    }
    return 0;
}

int notWired(std::string_view command) {
    std::fprintf(stderr, "%s: this command is not wired to a compiler backend yet\n",
                 std::string(command).c_str());
    return 1;
}

} // namespace

namespace cmd {

int checkHelp(const generated_cli::Options &opts) {
    return genericCommandHelp(opts.check.help, "zithc check [file]",
                              "Parse source files, stopping after the parsed stage.");
}

int fmtHelp(const generated_cli::Options &opts) {
    return genericCommandHelp(opts.fmt.help, "zithc fmt [file]", "Format source files, optionally in place or with --check.");
}

int runHelp(const generated_cli::Options &opts) {
    return genericCommandHelp(opts.run.help, "zithc run [script]", "Build and run a source file.");
}

int buildHelp(const generated_cli::Options &opts) {
    return genericCommandHelp(opts.build.help, "zithc build [file]", "Build project files to an output artifact.");
}

int depsHelp(const generated_cli::Options &opts) {
    return genericCommandHelp(opts.deps.help, "zithc deps <subcommand>",
                              "Manage dependencies: add, check, remove, list, publish, unpublish, update.");
}

int executeHelp(const generated_cli::Options &opts) {
    return genericCommandHelp(opts.execute.help, "zithc execute <file>", "Execute a compiled artifact.");
}

int testHelp(const generated_cli::Options &opts) {
    return genericCommandHelp(opts.test.help, "zithc test", "Run project tests.");
}

int docsHelp(const generated_cli::Options &opts) {
    return genericCommandHelp(opts.docs.help, "zithc docs [file]", "Generate documentation from public symbols.");
}

int replHelp(const generated_cli::Options &opts) {
    return genericCommandHelp(opts.repl.help, "zithc repl", "Start an interactive REPL.");
}

int createHelp(const generated_cli::Options &opts) {
    return genericCommandHelp(opts.create.help, "zithc create <name>", "Create a new project.");
}

int cleanHelp(const generated_cli::Options &opts) {
    return genericCommandHelp(opts.clean.help, "zithc clean [path]", "Remove build and cache artifacts.");
}

int completionHelp(const generated_cli::Options &opts) {
    return genericCommandHelp(opts.completion.help, "zithc completion <shell>",
                              "Generate shell completion scripts for bash, zsh, or fish.");
}

int compileHelp(const generated_cli::Options &opts) {
    return genericCommandHelp(opts.compile.help, "zithc compile <serialized.sir> [output]",
                              "Compile a serialized SIR file into a binary.");
}

int check(const generated_cli::Options &opts) {
    if (opts.check.help)
        return checkHelp(opts);
    return runPipeline(opts, chooseInput(opts), toolkit::session::Stage::Parsed);
}

int fmt(const generated_cli::Options &opts) {
    if (opts.fmt.help)
        return fmtHelp(opts);
    return notWired("fmt");
}

int run(const generated_cli::Options &opts) {
    if (opts.run.help)
        return runHelp(opts);
    return runPipeline(opts, chooseInput(opts), toolkit::session::Stage::Cached);
}

int build(const generated_cli::Options &opts) {
    if (opts.build.help)
        return buildHelp(opts);
    return runPipeline(opts, chooseInput(opts), toolkit::session::Stage::Cached);
}

int deps(const generated_cli::Options &opts) {
    if (opts.deps.help)
        return depsHelp(opts);
    return notWired("deps");
}

int execute(const generated_cli::Options &opts) {
    if (opts.execute.help)
        return executeHelp(opts);
    return notWired("execute");
}

int test(const generated_cli::Options &opts) {
    if (opts.test.help)
        return testHelp(opts);
    return notWired("test");
}

int docs(const generated_cli::Options &opts) {
    if (opts.docs.help)
        return docsHelp(opts);
    return notWired("docs");
}

int repl(const generated_cli::Options &opts) {
    if (opts.repl.help)
        return replHelp(opts);
    return notWired("repl");
}

int create(const generated_cli::Options &opts) {
    if (opts.create.help)
        return createHelp(opts);
    return notWired("create");
}

int clean(const generated_cli::Options &opts) {
    if (opts.clean.help)
        return cleanHelp(opts);
    return notWired("clean");
}

int completion(const generated_cli::Options &opts) {
    if (opts.completion.help)
        return completionHelp(opts);
    return notWired("completion");
}

int compile(const generated_cli::Options &opts) {
    if (opts.compile.help)
        return compileHelp(opts);
    const std::string_view sirPath = chooseCompileInput(opts);
    const std::string_view outputPath = chooseCompileOutput(opts);
    if (sirPath.empty()) {
        std::fprintf(stderr, "compile: no serialized SIR file given\n");
        return 1;
    }

    const std::string fileName{sirPath};
    FILE *file = std::fopen(fileName.c_str(), "rb");
    if (file == nullptr) {
        std::fprintf(stderr, "compile: cannot open %s\n", fileName.c_str());
        return 1;
    }

    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size < 0) {
        std::fprintf(stderr, "compile: cannot determine size of %s\n", fileName.c_str());
        std::fclose(file);
        return 1;
    }

    std::string bytes(static_cast<std::size_t>(size), '\0');
    const std::size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    if (read != bytes.size()) {
        std::fprintf(stderr, "compile: failed while reading %s\n", fileName.c_str());
        return 1;
    }

    common::memory::Arena arena;
    auto decoded = toolkit::sir::flat::deserializeFlatModule(
        arena, std::string_view{bytes.data(), bytes.size()});
    if (!decoded) {
        std::fprintf(stderr, "compile: invalid serialized SIR: %s\n",
                     decoded.error().msg.c_str());
        return 1;
    }

    std::fprintf(stderr,
                 "compile: validated %.*s (functions=%zu); binary emission is not wired yet\n",
                 static_cast<int>(outputPath.size()), outputPath.data(),
                 decoded.value().functions.size());
    return 1;
}

int depsAdd(const generated_cli::Options &opts) {
    if (opts.deps.add.help)
        return genericCommandHelp(true, "zithc deps add <package>",
                                  "Add a dependency to the project.");
    return notWired("deps add");
}

int depsCheck(const generated_cli::Options &opts) {
    if (opts.deps.check.help)
        return genericCommandHelp(true, "zithc deps check [package]",
                                  "Check project dependencies.");
    return notWired("deps check");
}

int depsRemove(const generated_cli::Options &opts) {
    if (opts.deps.remove.help)
        return genericCommandHelp(true, "zithc deps remove <package>",
                                  "Remove a dependency from the project.");
    return notWired("deps remove");
}

int depsList(const generated_cli::Options &opts) {
    if (opts.deps.list.help)
        return genericCommandHelp(true, "zithc deps list [package]",
                                  "List project dependencies.");
    return notWired("deps list");
}

int depsPublish(const generated_cli::Options &opts) {
    if (opts.deps.publish.help)
        return genericCommandHelp(true, "zithc deps publish <package>",
                                  "Publish a dependency.");
    return notWired("deps publish");
}

int depsUnpublish(const generated_cli::Options &opts) {
    if (opts.deps.unpublish.help)
        return genericCommandHelp(true, "zithc deps unpublish <package>",
                                  "Unpublish a dependency.");
    return notWired("deps unpublish");
}

int depsUpdate(const generated_cli::Options &opts) {
    if (opts.deps.update.help)
        return genericCommandHelp(true, "zithc deps update <package>",
                                  "Update a dependency.");
    return notWired("deps update");
}

} // namespace cmd
