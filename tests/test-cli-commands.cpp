#include "cli/commands.hpp"
#include "cli/options.hpp"
#include "session/project-options-merge.hpp"
#include "support/project-root.hpp"
#include "test-common.hpp"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <toml++/toml.hpp>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

using namespace zith;
using namespace zith::cli::commands;

namespace {

struct CurrentDirectory {
    std::filesystem::path previous = std::filesystem::current_path();

    explicit CurrentDirectory(const std::filesystem::path &path) {
        std::filesystem::current_path(path);
    }

    ~CurrentDirectory() {
        std::error_code error;
        std::filesystem::current_path(previous, error);
    }
};

struct CliCapture {
    struct Result {
        int exitCode = -1;
        std::string stdoutText;
        std::string stderrText;
    };

    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("zith-docs-cli-" +
         std::to_string(std::filesystem::file_time_type::clock::now().time_since_epoch().count()));

    ~CliCapture() {
        std::filesystem::remove_all(root);
    }

    Result run(const std::string &args) const {
        std::filesystem::create_directories(root);
        const auto stdoutPath     = root / "stdout.txt";
        const auto stderrPath     = root / "stderr.txt";
        const std::string command = std::string("\"") + ZITHC_BINARY + "\" " + args + " > \"" +
                                    stdoutPath.string() + "\" 2> \"" + stderrPath.string() + "\"";
        const int status          = std::system(command.c_str());
        int exitCode              = status;
#ifndef _WIN32
        exitCode = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
#endif

        const auto read = [](const std::filesystem::path &path) {
            std::ifstream input(path, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
        };
        return {exitCode, read(stdoutPath), read(stderrPath)};
    }

    std::string run(const std::string &args, const std::string &stream) const {
        const auto result = run(args);
        return std::to_string(result.exitCode) + "\n" +
               (stream == "stdout" ? result.stdoutText : result.stderrText);
    }
};

// ── Options parsing validation ────────────────────────────────────

static void test_options_defaults() {
    memory::Arena arena;
    Options opts(arena);

    // command defaults to None
    CHECK(opts.command == Options::Command::None, "default command is None");

    // input files start empty
    CHECK(opts.inputFiles.empty(), "default inputFiles is empty");

    // include dirs start empty
    CHECK(opts.includeDirs.empty(), "default includeDirs is empty");

    CHECK(!opts.flags.debugSema(), "debug-sema defaults to disabled");
}

static void test_options_command_enum() {
    memory::Arena arena;
    Options opts(arena);

    opts.command = Options::Command::Build;
    CHECK(opts.command == Options::Command::Build, "Command::Build is set");

    opts.command = Options::Command::Test;
    CHECK(opts.command == Options::Command::Test, "Command::Test is set");

    opts.command = Options::Command::Deps;
    CHECK(opts.command == Options::Command::Deps, "Command::Deps is set");

    opts.command = Options::Command::Docs;
    CHECK(opts.command == Options::Command::Docs, "Command::Docs is set");
}

static void test_build_derives_codegen_stage() {
    memory::Arena arena;

    Options plain(arena);
    plain.command = Options::Command::Build;
    plain.deriveTargetStage();
    CHECK(plain.targetStage == session::Stage::Cached,
          "build with no --emit reaches codegen (Stage::Cached)");

    Options emitIr(arena);
    emitIr.command    = Options::Command::Build;
    emitIr.emitTarget = Options::EmitTarget::Ir;
    emitIr.deriveTargetStage();
    CHECK(emitIr.targetStage == session::Stage::CodegenReady,
          "build --emit ir still stops at Stage::CodegenReady");

    Options emitVir(arena);
    emitVir.command = Options::Command::Build;
    emitVir.flags.emitVir(true);
    emitVir.deriveTargetStage();
    CHECK(emitVir.targetStage == session::Stage::HirLowered,
          "build --emit-vir stops at Stage::HirLowered");

    Options emitCst(arena);
    emitCst.command = Options::Command::Build;
    emitCst.flags.emitCst(true);
    emitCst.deriveTargetStage();
    CHECK(emitCst.targetStage == session::Stage::Imported,
          "build --emit-cst stops after frontend import");
}

static void test_new_emit_flags_parse_and_compose() {
    char program[] = "zithc";
    char command[] = "check";
    char cst[]     = "--emit-cst";
    char vir[]     = "--emit-vir";
    char all[]     = "--emit-all";
    char *args[]   = {program, command, cst, vir};
    Cli cli;
    cli.parseArgs(4, args);
    CHECK(cli.opts.flags.emitCst(), "--emit-cst enables CST emission");
    CHECK(cli.opts.flags.emitVir(), "--emit-vir enables VIR emission");

    char *all_args[] = {program, command, all};
    Cli all_cli;
    all_cli.parseArgs(3, all_args);
    CHECK(all_cli.opts.flags.emitCst() && all_cli.opts.flags.emitVir(),
          "--emit-all includes CST and VIR");
}

static void test_virtual_machine_flag_parse_and_stage() {
    {
        char program[] = "zithc";
        char command[] = "run";
        char flag[]    = "--virtual-machine";
        char *args[]   = {program, command, flag};
        Cli cli;
        cli.parseArgs(3, args);
        CHECK(cli.opts.flags.virtualMachine(), "--virtual-machine enables the VM flag");
        CHECK(!cli.opts.flags.interpreted(), "--virtual-machine is distinct from --interpreted");
    }

    {
        // On a Run command the flag must stop the pipeline at HIR so the VM can
        // execute it, regardless of whether LLVM codegen is compiled in.
        memory::Arena arena;
        Options opts(arena);
        opts.command = Options::Command::Run;
        opts.flags.virtualMachine(true);
        opts.deriveTargetStage();
        CHECK(opts.targetStage == session::Stage::HirLowered,
              "run --virtual-machine stops at Stage::HirLowered");
    }
}

static void test_run_emit_vir_still_executes_program() {
#ifdef ZITHC_BINARY
    CliCapture capture;
    std::filesystem::create_directories(capture.root);
    const auto source = capture.root / "main.zith";
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        output << "fn main(): i32 { return 7; }\n";
    }

#ifdef ZITH_HAS_VM
    const auto result = capture.run("run --emit-vir --no-cache \"" + source.string() + "\"");
    CHECK_EQ(result.exitCode, 7, "run --emit-vir executes main and returns its exit code");
    CHECK(result.stdoutText.empty(), "compiler VIR dump does not contaminate program stdout");
    CHECK(result.stderrText.find("--- VIR ---") != std::string::npos,
          "run --emit-vir writes the VIR dump to compiler stderr");
    CHECK(result.stderrText.find("LoadConstI32") != std::string::npos,
          "run --emit-vir includes lowered VM v2 instructions");
#else
    // The VM v2 slice is optional on native builds; without it --emit-vir is a
    // hard error, so the execution check is skipped.
    CHECK(true, "run --emit-vir execution is skipped when the VM slice is not built");
#endif
#else
    CHECK(true, "CLI subprocess tests are skipped when zithc is not built");
#endif
}

static void test_run_virtual_machine_executes_program() {
#ifdef ZITHC_BINARY
    CliCapture capture;
    std::filesystem::create_directories(capture.root);
    const auto source = capture.root / "main.zith";
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        output << "fn main(): i32 { return 9; }\n";
    }

#ifdef ZITH_HAS_VM
    // --no-cache keeps the run on the cold lowering path, matching the other
    // subprocess checks; the VM must still produce the program exit code.
    const auto result = capture.run("run --virtual-machine --no-cache \"" + source.string() + "\"");
    CHECK_EQ(result.exitCode, 9, "run --virtual-machine executes main and returns its exit code");
    CHECK(result.stdoutText.empty(), "run --virtual-machine leaves program stdout empty");

    // Regression: a prior native run writes the persistent cache. A later
    // --virtual-machine run hydrates HIR from it, and hydration does not
    // restore the session-local `decl_id`. The VM must still find and run
    // `main` from the hydrated body rather than dropping it as extern.
    const auto warmNative = capture.run("run \"" + source.string() + "\"");
    CHECK_EQ(warmNative.exitCode, 9, "native run warms the cache with the program result");
    const auto warmVm = capture.run("run --virtual-machine \"" + source.string() + "\"");
    CHECK_EQ(warmVm.exitCode, 9, "run --virtual-machine executes main from a hydrated cache entry");
#else
    // Without the VM slice the flag is a hard error instead of executing.
    const auto result = capture.run("run --virtual-machine \"" + source.string() + "\"");
    CHECK_EQ(result.exitCode, 1, "run --virtual-machine errors when the VM slice is absent");
    CHECK(result.stderrText.find("excludes the VM v2 slice") != std::string::npos,
          "the missing VM slice is reported on stderr");
#endif
#else
    CHECK(true, "CLI subprocess tests are skipped when zithc is not built");
#endif
}

static void test_docs_options_parse_and_validate() {
    {
        char program[] = "zithc";
        char command[] = "docs";
        char *args[]   = {program, command};
        Cli cli;
        cli.parseArgs(2, args);
        CHECK(cli.opts.docsMode == Options::DocsMode::Interface, "docs defaults to interface mode");
        CHECK(cli.opts.docsOptionsError().empty(), "default docs options are valid");
    }

    {
        char program[] = "zithc";
        char command[] = "docs";
        char spec[]    = "--spec";
        char *args[]   = {program, command, spec};
        Cli cli;
        cli.parseArgs(3, args);
        CHECK(cli.opts.docsMode == Options::DocsMode::Spec, "--spec selects spec mode");
        CHECK(cli.opts.docsModeExplicit, "--spec records an explicit mode");
    }

    {
        char program[] = "zithc";
        char command[] = "docs";
        char mode[]    = "--mode=interface";
        char *args[]   = {program, command, mode};
        Cli cli;
        cli.parseArgs(3, args);
        CHECK(cli.opts.docsMode == Options::DocsMode::Interface,
              "--mode=interface selects interface mode");
        CHECK(cli.opts.docsModeExplicit, "--mode=interface records an explicit mode");
    }

    {
        char program[] = "zithc";
        char command[] = "docs";
        char out[]     = "--out";
        char index[]   = "--index";
        char *args[]   = {program, command, out, index};
        Cli cli;
        cli.parseArgs(4, args);
        CHECK(cli.opts.docsOut, "--out enables file output");
        CHECK(cli.opts.docsIndex, "--index enables multipage output");
        CHECK(cli.opts.docsOptionsError().empty(), "--index is valid with --out");
    }

    {
        char program[] = "zithc";
        char command[] = "docs";
        char out[]     = "--out=generated";
        char *args[]   = {program, command, out};
        Cli cli;
        cli.parseArgs(3, args);
        CHECK(cli.opts.docsOut, "--out=PATH enables file output");
        CHECK_EQ(cli.opts.docsOutPath, std::string("generated"),
                 "--out=PATH preserves the requested directory");
    }

    {
        char program[] = "zithc";
        char command[] = "docs";
        char spec[]    = "--spec";
        char iface[]   = "--interface";
        char *args[]   = {program, command, spec, iface};
        Cli cli;
        cli.parseArgs(4, args);
        CHECK(!cli.opts.docsOptionsError().empty(),
              "contradictory explicit docs modes are rejected");
    }

    {
        char program[] = "zithc";
        char command[] = "docs";
        char index[]   = "--index";
        char *args[]   = {program, command, index};
        Cli cli;
        cli.parseArgs(3, args);
        CHECK(!cli.opts.docsOptionsError().empty(), "--index without --out is rejected");
    }

    {
        char program[] = "zithc";
        char command[] = "docs";
        char mode[]    = "--mode=unknown";
        char *args[]   = {program, command, mode};
        Cli cli;
        cli.parseArgs(3, args);
        CHECK(!cli.opts.docsOptionsError().empty(), "unknown docs modes are rejected");
    }

    {
        char program[] = "zithc";
        char command[] = "build";
        char mode[]    = "--mode";
        char debug[]   = "debug";
        char *args[]   = {program, command, mode, debug};
        Cli cli;
        cli.parseArgs(4, args);
        CHECK(cli.opts.flags.mode() == Options::Mode::Debug,
              "the existing --mode debug build option is unchanged");
    }
}

static void test_docs_cli_rejects_invalid_options_before_compilation() {
#ifdef ZITHC_BINARY
    {
        CliCapture capture;
        const std::string result = capture.run("docs --mode=unknown", "stderr");
        CHECK(result.starts_with("1\n"), "unknown docs mode fails with exit code 1");
        CHECK(result.find("invalid docs mode") != std::string::npos,
              "unknown docs mode reports its argument error");
    }

    {
        CliCapture capture;
        const std::string result = capture.run("docs --out=first --out=second", "stderr");
        CHECK(result.starts_with("1\n"), "conflicting docs output paths fail with exit code 1");
        CHECK(result.find("conflicting --out paths") != std::string::npos,
              "conflicting docs output paths are reported");
    }

    {
        CliCapture capture;
        const std::string result = capture.run("build --mode=release", "stderr");
        CHECK(result.starts_with("1\n"), "unsupported global --mode= syntax remains an error");
        CHECK(result.find("unknown flag '--mode=release'") != std::string::npos,
              "global --mode=release is not consumed as a docs mode");
    }
#else
    CHECK(true, "CLI subprocess tests are skipped when zithc is not built");
#endif
}

static void test_completion_command_emits_updated_shell_options() {
    {
        char program[] = "zithc";
        char command[] = "completion";
        char shell[]   = "fish";
        char *args[]   = {program, command, shell};
        Cli cli;
        cli.parseArgs(3, args);
        CHECK(cli.opts.command == Options::Command::Completion,
              "completion is parsed as a CLI command");
        CHECK_EQ(cli.opts.subcommandStr, std::string("fish"),
                 "completion captures the requested shell");
    }

#ifdef ZITHC_BINARY
    CliCapture capture;
    for (const std::string shell : {"bash", "zsh", "fish"}) {
        const auto result = capture.run("completion " + shell);
        CHECK_EQ(result.exitCode, 0, "shell completion command succeeds");
        CHECK(result.stdoutText.starts_with("# " + shell + " completion for zithc"),
              "shell completion writes its script instead of the general help");

        const std::string optionPrefix = shell == "fish" ? "-l " : "--";
        CHECK(result.stdoutText.find(optionPrefix + "interface") != std::string::npos,
              "shell completion includes interface mode");
        CHECK(result.stdoutText.find(optionPrefix + "spec") != std::string::npos,
              "shell completion includes spec mode");
        CHECK(result.stdoutText.find(optionPrefix + "index") != std::string::npos,
              "shell completion includes the docs index option");
        CHECK(result.stdoutText.find(optionPrefix + "force") != std::string::npos,
              "shell completion includes the overwrite option");
    }
#else
    CHECK(true, "CLI completion subprocess test is skipped when zithc is not built");
#endif
}

static void test_docs_error_policy() {
    CliCapture capture;
    std::filesystem::create_directories(capture.root);
    const auto source = capture.root / "main.zith";
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        output << "pub fn documented(): i32 { return 7; }\n"
                  "pub fn broken(): i32 { return missing(); }\n";
    }

    const auto defaultResult = capture.run("docs \"" + source.string() + "\"");
    CHECK_EQ(defaultResult.exitCode, 1,
             "docs reports a non-zero exit status when source compilation fails");
    CHECK(defaultResult.stdoutText.empty(),
          "docs without --error does not publish partial Markdown");

    const auto partialResult = capture.run("docs --error \"" + source.string() + "\"");
    CHECK_EQ(partialResult.exitCode, 1, "docs --error retains the failure exit status");
    CHECK(partialResult.stdoutText.find("documented") != std::string::npos,
          "docs --error includes successfully documented declarations");
    CHECK(partialResult.stdoutText.find("## Errors") != std::string::npos,
          "docs --error appends an Errors section");
    CHECK(partialResult.stdoutText.find("Functions") != std::string::npos,
          "docs --error identifies the section containing the diagnostic");

    const auto partialDirectory  = capture.root / "partial-output";
    const auto partialFileResult = capture.run("docs --error --out=\"" + partialDirectory.string() +
                                               "\" \"" + source.string() + "\"");
    CHECK_EQ(partialFileResult.exitCode, 1, "docs --error --out retains the failure exit status");
    CHECK(partialFileResult.stdoutText.empty(),
          "partial file output does not also publish Markdown on stdout");
    std::ifstream partialFile(partialDirectory / "API.md", std::ios::binary);
    const std::string partialText((std::istreambuf_iterator<char>(partialFile)),
                                  std::istreambuf_iterator<char>());
    CHECK(partialText.find("documented") != std::string::npos,
          "partial file output includes valid declarations");
    CHECK(partialText.find("## Errors") != std::string::npos,
          "partial file output includes the Errors section");
}

static void test_docs_parse_and_import_error_policy() {
#ifdef ZITHC_BINARY
    CliCapture capture;
    std::filesystem::create_directories(capture.root);

    struct ErrorCase {
        std::string name;
        std::string sourceText;
        std::string expectedSection;
    };
    const std::vector<ErrorCase> cases = {
        {"parse",
         "pub fn documented(): i32 { return 7; }\n"
         "pub fn broken(: i32 { return 0; }\n",
         "Functions"},
        {"import",
         "from missing_docs_dependency\n"
         "pub fn documented(): i32 { return 7; }\n",
         "Imports"},
    };

    for (const auto &errorCase : cases) {
        const auto source = capture.root / (errorCase.name + ".zith");
        {
            std::ofstream output(source, std::ios::binary | std::ios::trunc);
            output << errorCase.sourceText;
        }

        const auto outputDirectory = capture.root / (errorCase.name + "-output");
        const auto defaultResult   = capture.run("docs --out=\"" + outputDirectory.string() +
                                                 "\" \"" + source.string() + "\"");
        CHECK_EQ(defaultResult.exitCode, 1, "docs returns failure when a frontend error occurs");
        CHECK(defaultResult.stdoutText.empty(),
              "docs without --error suppresses partial stdout after a frontend error");
        CHECK(!std::filesystem::exists(outputDirectory / "API.md"),
              "docs without --error does not write API.md after a frontend error");

        const auto partialResult = capture.run("docs --error \"" + source.string() + "\"");
        CHECK_EQ(partialResult.exitCode, 1, "docs --error retains frontend failure status");
        CHECK(partialResult.stdoutText.find("documented") != std::string::npos,
              "docs --error retains valid declarations after a frontend error");
        CHECK(partialResult.stdoutText.find("## Errors") != std::string::npos,
              "docs --error renders frontend diagnostics");
        CHECK(partialResult.stdoutText.find(errorCase.expectedSection) != std::string::npos,
              "docs --error identifies the section containing the frontend error");
    }
#else
    CHECK(true, "CLI docs parse/import subprocess tests are skipped when zithc is not built");
#endif
}

static void test_docs_stdout_file_and_index_outputs() {
#ifdef ZITHC_BINARY
    CliCapture capture;
    std::filesystem::create_directories(capture.root);
    const auto source = capture.root / "main.zith";
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        output << "pub fn documented(): i32 { return 7; }\n";
    }

    const auto terminalResult = capture.run("docs \"" + source.string() + "\"");
    CHECK_EQ(terminalResult.exitCode, 0, "docs succeeds for a valid standalone source file");
    CHECK(terminalResult.stdoutText.starts_with("# API Reference\n"),
          "docs writes the rendered Markdown to stdout by default");
    CHECK(terminalResult.stdoutText.find("documented") != std::string::npos,
          "stdout Markdown contains the public function");

    const auto outputDirectory = capture.root / "generated";
    const auto fileResult =
        capture.run("docs --out=\"" + outputDirectory.string() + "\" \"" + source.string() + "\"");
    CHECK_EQ(fileResult.exitCode, 0, "docs --out writes the aggregate document");
    CHECK(fileResult.stdoutText.empty(), "file output does not duplicate Markdown on stdout");
    std::ifstream apiFile(outputDirectory / "API.md", std::ios::binary);
    const std::string apiText((std::istreambuf_iterator<char>(apiFile)),
                              std::istreambuf_iterator<char>());
    CHECK(apiText.starts_with("# API Reference\n"), "--out creates API.md");

    const auto indexDirectory = capture.root / "indexed";
    const auto indexResult    = capture.run("docs --out=\"" + indexDirectory.string() +
                                            "\" --index \"" + source.string() + "\"");
    CHECK_EQ(indexResult.exitCode, 0, "--index writes the multipage documentation set");
    CHECK(std::filesystem::exists(indexDirectory / "README.md"), "--index creates a README index");
    const auto modulesDirectory = indexDirectory / "modules";
    CHECK(std::filesystem::is_directory(modulesDirectory),
          "--index creates the module pages directory");
    bool hasModulePage = false;
    for (const auto &entry : std::filesystem::directory_iterator(modulesDirectory))
        hasModulePage = hasModulePage || entry.path().extension() == ".md";
    CHECK(hasModulePage, "--index creates at least one Markdown module page");
#else
    CHECK(true, "CLI docs subprocess tests are skipped when zithc is not built");
#endif
}

static void test_deps_resolve_writes_deterministic_lockfile() {
#ifdef ZITHC_BINARY
    namespace fs = std::filesystem;

    CliCapture capture;
    const auto projectRoot = capture.root / "project";
    fs::create_directories(projectRoot / "vendor" / "logging" / "src");
    {
        std::ofstream manifest(projectRoot / "ZithProject.toml", std::ios::binary);
        manifest << "[project]\n"
                    "name = \"application\"\n"
                    "version = \"0.1.0\"\n"
                    "\n[dependencies]\n"
                    "logging = \"vendor/logging\"\n";
        CHECK(manifest.good(), "project manifest fixture is written");
    }
    {
        std::ofstream manifest(projectRoot / "vendor" / "logging" / "ZithProject.toml",
                               std::ios::binary);
        manifest << "[project]\n"
                    "name = \"logging\"\n"
                    "version = \"1\"\n";
        CHECK(manifest.good(), "dependency manifest fixture is written");
    }
    {
        std::ofstream source(projectRoot / "vendor" / "logging" / "src" / "logging.zith",
                             std::ios::binary);
        source << "pub fn log() {}\n";
        CHECK(source.good(), "dependency module fixture is written");
    }

    CurrentDirectory currentDirectory(projectRoot);

    const auto first = capture.run("deps resolve");
    CHECK_EQ(first.exitCode, 0, "deps resolve succeeds for a valid local package graph");

    const auto lockfilePath = projectRoot / "Zith.lock";
    std::ifstream firstLockfile(lockfilePath, std::ios::binary);
    const std::string firstText((std::istreambuf_iterator<char>(firstLockfile)),
                                std::istreambuf_iterator<char>());
    CHECK(firstLockfile.good() || firstLockfile.eof(), "deps resolve creates Zith.lock");
    const auto parsedLockfile = toml::parse(firstText);
    const auto *packages      = parsedLockfile["package"].as_array();
    bool hasLoggingPackage    = false;
    if (packages != nullptr) {
        for (const auto &record : *packages) {
            const auto *package  = record.as_table();
            const auto *nameNode = package == nullptr ? nullptr : package->get("name");
            const auto name   = nameNode == nullptr ? std::nullopt : nameNode->value<std::string>();
            hasLoggingPackage = hasLoggingPackage || (name && *name == "logging");
        }
    }
    CHECK(hasLoggingPackage, "Zith.lock records the resolved local package");

    const auto second = capture.run("deps resolve");
    CHECK_EQ(second.exitCode, 0, "deps resolve succeeds when run repeatedly");
    std::ifstream secondLockfile(lockfilePath, std::ios::binary);
    const std::string secondText((std::istreambuf_iterator<char>(secondLockfile)),
                                 std::istreambuf_iterator<char>());
    CHECK_EQ(secondText, firstText, "repeated deps resolve preserves deterministic lockfile text");

    {
        std::ofstream manifest(projectRoot / "ZithProject.toml",
                               std::ios::binary | std::ios::trunc);
        manifest << "[project]\n"
                    "name = \"application\"\n"
                    "version = \"0.1.0\"\n"
                    "\n[dependencies]\n"
                    "missing = \"vendor/missing\"\n";
    }
    const auto failed = capture.run("deps resolve");
    CHECK_EQ(failed.exitCode, 1, "deps resolve reports a missing local dependency");

    std::ifstream preservedLockfile(lockfilePath, std::ios::binary);
    const std::string preservedText((std::istreambuf_iterator<char>(preservedLockfile)),
                                    std::istreambuf_iterator<char>());
    CHECK_EQ(preservedText, firstText, "failed resolution leaves the previous lockfile untouched");
#else
    CHECK(true, "deps resolve subprocess test is skipped when zithc is not built");
#endif
}

static void test_deps_add_remove_edit_only_the_manifest() {
#ifdef ZITHC_BINARY
    namespace fs = std::filesystem;

    CliCapture capture;
    const auto projectRoot = capture.root / "deps-edit-project";
    fs::create_directories(projectRoot);
    const auto manifestPath = projectRoot / "ZithProject.toml";
    const auto lockfilePath = projectRoot / "Zith.lock";
    {
        std::ofstream manifest(manifestPath, std::ios::binary);
        manifest << "[project]\n"
                    "name = \"application\"\n"
                    "version = \"0.1.0\"\n"
                    "\n[paths]\n"
                    "src_dir = \"source\"\n";
        CHECK(manifest.good(), "dependency edit project manifest is written");
    }
    {
        std::ofstream lockfile(lockfilePath, std::ios::binary);
        lockfile << "keep this lockfile unchanged\n";
        CHECK(lockfile.good(), "dependency edit lockfile sentinel is written");
    }

    CurrentDirectory currentDirectory(projectRoot);
    const auto added = capture.run("deps add logging ../vendor/logging");
    CHECK_EQ(added.exitCode, 0, "deps add accepts a package name and relative local path");
    CHECK(added.stdoutText.find("added dependency 'logging'") != std::string::npos,
          "deps add reports the declared dependency");

    auto parsedManifest      = toml::parse_file(manifestPath.string());
    const auto *dependencies = parsedManifest["dependencies"].as_table();
    CHECK(dependencies != nullptr, "deps add creates the dependencies table");
    if (dependencies != nullptr) {
        CHECK(dependencies->get("logging") != nullptr,
              "deps add inserts the requested package name");
        CHECK_EQ(dependencies->get("logging")->value<std::string>().value_or(""),
                 std::string("../vendor/logging"), "deps add stores the relative path");
    }
    CHECK_EQ(parsedManifest["paths"]["src_dir"].value<std::string>().value_or(""),
             std::string("source"), "deps add preserves unrelated manifest settings");

    const auto readManifest = [&] {
        std::ifstream input(manifestPath, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    };
    const auto manifestAfterAdd = readManifest();
    const auto duplicate        = capture.run("deps add logging ../another/logging");
    CHECK_EQ(duplicate.exitCode, 1, "deps add rejects an already declared package");
    const auto absolutePath = capture.run("deps add absolute \"/tmp/external-package\"");
    CHECK_EQ(absolutePath.exitCode, 1, "deps add rejects an absolute package path");
    const auto standardLibrary = capture.run("deps add std ../stdlib");
    CHECK_EQ(standardLibrary.exitCode, 1, "deps add rejects std as a package dependency");
    const auto emptyName = capture.run("deps add \"\" ../vendor/empty");
    CHECK_EQ(emptyName.exitCode, 1, "deps add rejects an empty package name");
    const auto wrongAddArguments = capture.run("deps add only-name");
    CHECK_EQ(wrongAddArguments.exitCode, 1, "deps add rejects the wrong argument count");
    const auto missingRemove = capture.run("deps remove absent");
    CHECK_EQ(missingRemove.exitCode, 1, "deps remove rejects an undeclared package");
    const auto wrongRemoveArguments = capture.run("deps remove logging extra");
    CHECK_EQ(wrongRemoveArguments.exitCode, 1, "deps remove rejects extra arguments");
    CHECK_EQ(readManifest(), manifestAfterAdd,
             "invalid dependency commands leave ZithProject.toml unchanged");

    const auto removed = capture.run("deps remove logging");
    CHECK_EQ(removed.exitCode, 0, "deps remove removes an existing package declaration");
    parsedManifest = toml::parse_file(manifestPath.string());
    dependencies   = parsedManifest["dependencies"].as_table();
    CHECK(dependencies == nullptr || dependencies->get("logging") == nullptr,
          "deps remove deletes the requested package declaration");
    CHECK_EQ(parsedManifest["paths"]["src_dir"].value<std::string>().value_or(""),
             std::string("source"), "deps remove preserves unrelated manifest settings");

    {
        std::ofstream manifest(manifestPath, std::ios::binary | std::ios::trunc);
        manifest << "[project]\n"
                    "name = \"application\"\n"
                    "version = \"0.1.0\"\n"
                    "\n[dependencies]\n"
                    "broken = 42\n";
    }
    const auto removedInvalid = capture.run("deps remove broken");
    CHECK_EQ(removedInvalid.exitCode, 0,
             "deps remove can delete a malformed dependency value for recovery");

    std::ifstream lockfile(lockfilePath, std::ios::binary);
    const std::string lockfileContents((std::istreambuf_iterator<char>(lockfile)),
                                       std::istreambuf_iterator<char>());
    CHECK_EQ(lockfileContents, std::string("keep this lockfile unchanged\n"),
             "deps add and remove leave Zith.lock untouched");
#else
    CHECK(true, "dependency edit subprocess test is skipped when zithc is not built");
#endif
}

static void test_check_build_require_explicit_lockfile() {
#ifdef ZITHC_BINARY
    namespace fs = std::filesystem;

    CliCapture capture;
    const auto projectRoot = capture.root / "lock-gate";
    fs::create_directories(projectRoot);
    const auto writeManifest = [&](const std::string &version) {
        std::ofstream manifest(projectRoot / "ZithProject.toml",
                               std::ios::binary | std::ios::trunc);
        manifest << "[project]\n"
                    "name = \"application\"\n"
                    "version = \""
                 << version << "\"\n";
        CHECK(manifest.good(), "project manifest fixture is written");
    };
    writeManifest("0.1.0");

    CurrentDirectory currentDirectory(projectRoot);
    for (const auto *command : {"check", "build"}) {
        const auto result = capture.run(command);
        CHECK_EQ(result.exitCode, 1, "check/build reject a package project without Zith.lock");
        CHECK(result.stderrText.find("zithc deps resolve") != std::string::npos,
              "missing lockfile diagnostic recommends explicit resolution");
        CHECK(!fs::exists(projectRoot / "Zith.lock"),
              "check/build do not create a missing lockfile implicitly");
    }

    const auto resolved = capture.run("deps resolve");
    CHECK_EQ(resolved.exitCode, 0, "deps resolve creates a lockfile for the gate fixture");
    const auto lockfilePath = projectRoot / "Zith.lock";
    std::ifstream lockfileInput(lockfilePath, std::ios::binary);
    const std::string originalLockfile((std::istreambuf_iterator<char>(lockfileInput)),
                                       std::istreambuf_iterator<char>());
    CHECK(!originalLockfile.empty(), "resolved lockfile fixture is available");

    writeManifest("0.2.0");
    for (const auto *command : {"check", "build"}) {
        const auto result = capture.run(command);
        CHECK_EQ(result.exitCode, 1, "check/build reject a stale project lockfile");
        CHECK(result.stderrText.find("zithc deps resolve") != std::string::npos,
              "stale lockfile diagnostic recommends explicit resolution");

        std::ifstream currentLockfile(lockfilePath, std::ios::binary);
        const std::string currentText((std::istreambuf_iterator<char>(currentLockfile)),
                                      std::istreambuf_iterator<char>());
        CHECK_EQ(currentText, originalLockfile, "check/build leave a stale lockfile unchanged");
    }
#else
    CHECK(true, "lockfile gate subprocess tests are skipped when zithc is not built");
#endif
}

static void test_create_writes_lockfile_for_generated_project() {
#ifdef ZITHC_BINARY
    namespace fs = std::filesystem;

    CliCapture capture;
    fs::create_directories(capture.root);

    CurrentDirectory currentDirectory(capture.root);
    const auto created = capture.run("create demo");
    CHECK_EQ(created.exitCode, 0, "create succeeds for a new project");

    const auto projectRoot = capture.root / "demo";
    CHECK(fs::exists(projectRoot / "ZithProject.toml"),
          "create writes the generated project manifest");
    CHECK(fs::exists(projectRoot / "Zith.lock"),
          "create resolves and writes Zith.lock for the empty dependency graph");

    CurrentDirectory projectDirectory(projectRoot);
    for (const auto *command : {"check", "build"}) {
        const auto result = capture.run(command);
        CHECK_EQ(result.exitCode, 0,
                 "a freshly created project passes the lockfile gate for check/build");
    }
#else
    CHECK(true, "create lockfile tests are skipped when zithc is not built");
#endif
}

static void test_legacy_and_malformed_project_manifest_gate_behavior() {
#ifdef ZITHC_BINARY
    namespace fs = std::filesystem;

    {
        CliCapture capture;
        const auto projectRoot = capture.root / "legacy-project";
        fs::create_directories(projectRoot / "src");
        {
            std::ofstream manifest(projectRoot / "ZithProject.toml", std::ios::binary);
            manifest << "[build]\nentry = \"src/main.zith\"\n";
        }
        {
            std::ofstream source(projectRoot / "src" / "main.zith", std::ios::binary);
            source << "fn main(): i32 { return 0; }\n";
        }

        CurrentDirectory currentDirectory(projectRoot);
        const auto result = capture.run("check src/main.zith");
        CHECK_EQ(result.exitCode, 0,
                 "legacy project without package metadata remains usable without Zith.lock");
        CHECK(!fs::exists(projectRoot / "Zith.lock"),
              "legacy check does not create a lockfile implicitly");
    }

    {
        CliCapture capture;
        const auto projectRoot = capture.root / "malformed-project";
        fs::create_directories(projectRoot);
        {
            std::ofstream manifest(projectRoot / "ZithProject.toml", std::ios::binary);
            manifest << "[project\nname = \"application\"\nversion = \"0.1.0\"\n";
        }

        CurrentDirectory currentDirectory(projectRoot);
        const auto result = capture.run("check");
        CHECK_EQ(result.exitCode, 1, "check rejects malformed project TOML");
        CHECK(result.stderrText.find("invalid package manifest: invalid TOML") != std::string::npos,
              "malformed manifest is reported instead of bypassing the lockfile gate");
        CHECK(!fs::exists(projectRoot / "Zith.lock"),
              "malformed project check does not create a lockfile");
    }
#else
    CHECK(true, "project manifest gate subprocess tests are skipped when zithc is not built");
#endif
}

static void test_project_root_stops_at_vcs_boundary() {
    namespace fs = std::filesystem;
    CliCapture capture;

    // Layout: <root>/ZithProject.toml is a *foreign* project above a checkout
    // marked with a `.git` entry. A lookup from inside the checkout must not
    // adopt the foreign manifest.
    const auto root = capture.root / "boundary";
    const auto repo = root / "checkout";
    fs::create_directories(repo / "src");
    fs::create_directories(repo / ".git");
    {
        std::ofstream foreign(root / "ZithProject.toml", std::ios::binary);
        foreign << "[project]\nname = \"foreign\"\n";
    }

    const auto located = support::findProjectRoot(repo / "src");
    CHECK(!located.has_value(),
          "project lookup stops at the checkout boundary and ignores the foreign manifest");

    // Once the checkout itself owns a manifest, it is found from any depth.
    {
        std::ofstream own(repo / "ZithProject.toml", std::ios::binary);
        own << "[project]\nname = \"checkout\"\nversion = \"0.1.0\"\n";
    }
    const auto ownRoot = support::findProjectRoot(repo / "src");
    CHECK(ownRoot.has_value() && *ownRoot == fs::weakly_canonical(repo),
          "project lookup finds the nearest manifest inside the checkout");

    // Without a `.git` boundary, a plain ancestor manifest is still discovered.
    const auto plain = capture.root / "plain";
    fs::create_directories(plain / "nested" / "deep");
    {
        std::ofstream manifest(plain / "ZithProject.toml", std::ios::binary);
        manifest << "[project]\nname = \"plain\"\n";
    }
    const auto plainRoot = support::findProjectRoot(plain / "nested" / "deep");
    CHECK(plainRoot.has_value() && *plainRoot == fs::weakly_canonical(plain),
          "project lookup walks up to a manifest when no VCS boundary intervenes");
}

static void test_clean_preserves_artifacts_without_confirmation() {
#ifdef ZITHC_BINARY
    namespace fs = std::filesystem;
    CliCapture capture;

    const auto projectRoot = capture.root / "clean-project";
    fs::create_directories(projectRoot / "src");
    fs::create_directories(projectRoot / "target");
    fs::create_directories(projectRoot / ".zith-cache");
    {
        std::ofstream manifest(projectRoot / "ZithProject.toml", std::ios::binary);
        manifest << "[project]\nname = \"clean\"\nversion = \"0.1.0\"\n";
    }
    {
        std::ofstream binary(projectRoot / "target" / "clean", std::ios::binary);
        binary << "artifact\n";
    }
    {
        std::ofstream cacheEntry(projectRoot / ".zith-cache" / "entry", std::ios::binary);
        cacheEntry << "cache\n";
    }

    CurrentDirectory currentDirectory(projectRoot);
    const auto result = capture.run("clean");
    CHECK_EQ(result.exitCode, 0, "clean succeeds after clearing the cache");
    CHECK(!fs::exists(projectRoot / ".zith-cache"), "clean removes the persistent cache");
    CHECK(fs::exists(projectRoot / "target"),
          "clean keeps build artifacts when stdin is not an interactive terminal");
    CHECK(result.stderrText.find("--yes") != std::string::npos,
          "clean explains how to remove artifacts non-interactively");

    const auto forced = capture.run("clean -y");
    CHECK_EQ(forced.exitCode, 0, "clean -y succeeds");
    CHECK(!fs::exists(projectRoot / "target"), "clean -y removes the build artifacts");
#else
    CHECK(true, "CLI clean subprocess tests are skipped when zithc is not built");
#endif
}

// ── Command function signatures exist ─────────────────────────────

static void test_command_signatures_exist() {
    // Verify each contract signature compiles (pointer-to-function type check)
    using TestFn = int (*)(const Options &);
    TestFn t     = test;
    TestFn d     = deps;
    TestFn dc    = docs;
    // repl stays stub but must exist
    TestFn r = repl;
    (void)t;
    (void)d;
    (void)dc;
    (void)r;
    CHECK(true, "command function pointers resolve");
}

// ── Shared helpers ────────────────────────────────────────────────

static void test_count_passed() {
    std::vector<bool> all_pass = {true, true, true, true};
    CHECK(countPassed(all_pass) == 4, "countPassed returns 4 for all true");

    std::vector<bool> mixed = {true, false, true, false, false};
    CHECK(countPassed(mixed) == 2, "countPassed returns 2 for mixed");

    std::vector<bool> none_pass = {false, false};
    CHECK(countPassed(none_pass) == 0, "countPassed returns 0 for all false");

    std::vector<bool> empty;
    CHECK(countPassed(empty) == 0, "countPassed returns 0 for empty");
}

// ── Command names required by the contract ────────────────────────

static void test_command_names_match_contract() {
    memory::Arena arena;
    Options opts(arena);
    memory::StringInterner pool(arena);

    // All commands declared in commands.hpp must be callable
    int (*cmds[])(const Options &) = {
        check, build, execute, run, test, fmt, docs, repl, create, clean, deps, completion,
    };
    (void)cmds;

    // version and help have distinct signatures
    int v = version();
    (void)v;
    CHECK(true, "version() compiles");
}

static void test_info_command_aliases_version() {
    char program[] = "zithc";
    char info[]    = "info";
    char *argv[]   = {program, info};

    Cli cli;
    cli.parseArgs(2, argv);
    CHECK(cli.opts.command == Options::Command::Version,
          "`info` selects the same command as `--version`");
#ifdef ZITHC_BINARY
    CliCapture capture;
    const auto result = capture.run("info");
    CHECK_EQ(result.exitCode, 0, "`zithc info` exits successfully");
    CHECK(result.stdoutText.find("Version:") != std::string::npos,
          "`zithc info` prints compiler version information");
#endif
}

static void test_llvm_version_information() {
#ifdef ZITH_LLVM_VERSION
    CHECK(std::strlen(ZITH_LLVM_VERSION) > 0,
          "LLVM version macro is non-empty when LLVM is present");
#endif
}

static void test_system_includes_flag() {
    memory::Arena arena;
    Options opts(arena);
    CHECK(opts.systemIncludes, "system includes default to enabled");

    char program[]    = "zithc";
    char command[]    = "check";
    char input[]      = "main.zith";
    char no_sysinc[]  = "--no-system-includes";
    char *without[]   = {program, command, input};
    char *with_flag[] = {program, command, no_sysinc, input};

    Cli plain;
    plain.parseArgs(3, without);
    CHECK(plain.opts.systemIncludes, "absent flag leaves system includes enabled");

    Cli disabled;
    disabled.parseArgs(4, with_flag);
    CHECK(!disabled.opts.systemIncludes, "--no-system-includes clears system includes");
}

static void test_debug_sema_flag() {
    memory::Arena arena;
    Options opts(arena);
    CHECK(!opts.flags.debugSema(), "debug-sema is off by default");

    char program[] = "zithc";
    char command[] = "check";
    char input[]   = "main.zith";
    char flag[]    = "--debug-sema";
    char *argv[]   = {program, command, flag, input};

    Cli cli;
    cli.parseArgs(4, argv);
    CHECK(cli.opts.flags.debugSema(), "--debug-sema enables the sema probe flag");
}

static void test_verbose_flag_overrides_config() {
    char program[] = "zithc";
    char command[] = "test";
    char flag[]    = "--verbose";
    char *argv[]   = {program, command, flag};

    Cli cli;
    cli.parseArgs(3, argv);
    cli.loadFlags();
    CHECK(cli.opts.flags.verbose(), "--verbose takes precedence over ZithFlags.toml");
}

static void test_test_command_filters_and_executes_tests() {
#ifdef ZITHC_BINARY
    CliCapture capture;
    std::filesystem::create_directories(capture.root / "tests");
    {
        std::ofstream project(capture.root / "ZithProject.toml", std::ios::binary);
        project << "[paths]\ntest_dir = \"tests\"\n";
    }
    {
        std::ofstream tests(capture.root / "tests" / "runner.zith", std::ios::binary);
        tests << "macro assertViaMacro(condition: expr) {\n"
                 "    @assert(condition);\n"
                 "}\n"
                 "#[test]\n"
                 "fn first() {\n"
                 "    @log(\"selected-log\");\n"
                 "    @log(\"zith-test-results-begin\\nzith-test fake "
                 "0\\nzith-test-results-end\\n\");\n"
                 "    @assertViaMacro(true);\n"
                 "}\n"
                 "#[test]\n"
                 "fn second() {\n"
                 "    @log(\"excluded-log\");\n"
                 "    @assert(false);\n"
                 "}\n";
    }

    const auto result =
        capture.run("test --filter runner::first \"" + capture.root.string() + "\"");
    const std::string failure_detail =
        "filtered test command succeeds when the selected test passes; stdout: " +
        result.stdoutText + "; stderr: " + result.stderrText;
    CHECK(result.exitCode == 0, failure_detail.c_str());
    CHECK(result.stdoutText.find("first") != std::string::npos,
          "filtered test command reports the selected test");
    CHECK(result.stdoutText.find("second") == std::string::npos &&
              result.stderrText.find("second") == std::string::npos,
          "filtered test command does not report the excluded test");
    CHECK(result.stdoutText.find("selected-log") == std::string::npos,
          "passing-test logs are hidden by default");
    CHECK(result.stdoutText.find("zith-test fake 0") == std::string::npos,
          "passing-test logs that resemble result lines are hidden by default");
    CHECK(result.stdoutText.find("excluded-log") == std::string::npos &&
              result.stderrText.find("excluded-log") == std::string::npos,
          "filtered test command does not execute the excluded test");

    const auto verbose =
        capture.run("test --verbose --filter=runner::first \"" + capture.root.string() + "\"");
    CHECK(verbose.exitCode == 0, "verbose filtered test command succeeds");
    CHECK(verbose.stderrText.find("[verbose] running 1 test(s)") != std::string::npos,
          "--verbose reports selected-test progress");
    CHECK(verbose.stdoutText.find("selected-log") != std::string::npos &&
              verbose.stdoutText.find("zith-test fake 0") != std::string::npos,
          "verbose output preserves logs that resemble result lines");

    const auto interpreted =
        capture.run("--interpreted test --filter=runner::first \"" + capture.root.string() + "\"");
    CHECK(interpreted.exitCode == 0, "test command executes the selected test in the interpreter");

#ifdef ZITH_HAS_VM
    const auto virtual_machine = capture.run("--virtual-machine test --filter=runner::first \"" +
                                             capture.root.string() + "\"");
    CHECK(virtual_machine.exitCode == 0, "test command executes the selected test in VM v2");
#endif

    const auto unfiltered = capture.run("test \"" + capture.root.string() + "\"");
    CHECK(unfiltered.exitCode == 1, "a failed @assert makes the test command fail");
    CHECK(unfiltered.stdoutText.find("1 passed; 1 failed; 2 total") != std::string::npos,
          "unfiltered run reports both per-test results");
    CHECK(unfiltered.stdoutText.find("excluded-log") != std::string::npos,
          "logs from the failing test are relayed");
    CHECK(unfiltered.stderrText.find((capture.root / "tests" / "runner.zith").string() + ":13:") !=
              std::string::npos,
          "failed assertion reports its source line");

    const auto no_match = capture.run("test --filter=missing \"" + capture.root.string() + "\"");
    CHECK(no_match.exitCode == 1, "a filter without matches is an error");
    CHECK(no_match.stderrText.find("no tests matched filter 'missing'") != std::string::npos,
          "a filter without matches gets a specific diagnostic");

    const auto help = capture.run("--help");
    CHECK(help.stdoutText.find("--filter <SUBSTRING>") != std::string::npos,
          "help documents the test filter");
    const auto completion = capture.run("completion bash");
    CHECK(completion.stdoutText.find("--filter") != std::string::npos,
          "bash completion includes the test filter");

    const auto no_test_root = capture.root / "no-test-project";
    std::filesystem::create_directories(no_test_root / "tests");
    {
        std::ofstream project(no_test_root / "ZithProject.toml", std::ios::binary);
        project << "[paths]\ntest_dir = \"tests\"\n";
        std::ofstream source(no_test_root / "tests" / "utility.zith", std::ios::binary);
        source << "fn helper() {}\n";
    }
    const auto no_tests = capture.run("test \"" + no_test_root.string() + "\"");
    CHECK(no_tests.exitCode == 0, "a project with no #[test] functions is skipped successfully");
    CHECK(no_tests.stderrText.find("no #[test] functions") != std::string::npos,
          "a file without tests gets an explicit skip note");

    const auto empty_root = capture.root / "empty-project";
    std::filesystem::create_directories(empty_root / "tests");
    {
        std::ofstream project(empty_root / "ZithProject.toml", std::ios::binary);
        project << "[paths]\ntest_dir = \"tests\"\n";
    }
    const auto empty_directory = capture.run("test \"" + empty_root.string() + "\"");
    CHECK(empty_directory.exitCode == 1, "an empty test directory is reported as an error");
    CHECK(empty_directory.stderrText.find("no .zith test files found") != std::string::npos,
          "an empty test directory gets a specific diagnostic");

    const auto trap_root = capture.root / "trap-project";
    std::filesystem::create_directories(trap_root / "tests");
    {
        std::ofstream project(trap_root / "ZithProject.toml", std::ios::binary);
        project << "[paths]\ntest_dir = \"tests\"\n";
        std::ofstream source(trap_root / "tests" / "helper.zith", std::ios::binary);
        source << "fn ordinary_helper() {\n"
                  "    @assert(false);\n"
                  "}\n"
                  "#[test]\n"
                  "fn calls_helper() {\n"
                  "    ordinary_helper();\n"
                  "}\n";
    }
    {
        std::ofstream source(trap_root / "tests" / "check-helper.zith", std::ios::binary);
        source << "fn ordinary_helper() {\n"
                  "    @check(false);\n"
                  "}\n"
                  "#[test]\n"
                  "fn calls_helper() {\n"
                  "    ordinary_helper();\n"
                  "}\n";
    }
    const auto trap = capture.run("test \"" + trap_root.string() + "\"");
    CHECK(trap.exitCode == 1, "a trap in a helper fails the test process");
    CHECK(trap.stderrText.find("test process failed") != std::string::npos,
          "a trapped test process gets an execution-failure diagnostic");
    CHECK(trap.stderrText.find("check-helper.zith") != std::string::npos,
          "@check in an ordinary helper traps outside lexical test context");

    const auto error_root = capture.root / "compile-error-project";
    std::filesystem::create_directories(error_root / "tests");
    {
        std::ofstream project(error_root / "ZithProject.toml", std::ios::binary);
        project << "[paths]\ntest_dir = \"tests\"\n";
        std::ofstream source(error_root / "tests" / "broken.zith", std::ios::binary);
        source << "#[test]\nfn broken() {\n    @assert(1);\n}\n"
                  "#[test]\nfn broken_check() {\n    @check(1);\n}\n";
    }
    const auto compile_error = capture.run("test \"" + error_root.string() + "\"");
    CHECK(compile_error.exitCode == 1, "a test compilation error fails the command");
    CHECK(compile_error.stderrText.find("failed to compile test file") != std::string::npos,
          "a test compilation error gets a file diagnostic");
    CHECK(compile_error.stderrText.find("@check") != std::string::npos,
          "a non-boolean @check argument is rejected during semantic analysis");
#endif
}

// ── ProjectConfig + Options merge helper ──────────────────────────

static void test_merge_strings_order_and_append() {
    memory::Arena arena;
    ProjectConfig config(arena);
    Options opts(arena);

    config.includeDirs.push("config/include");
    config.cSourceDirs.push("config/csrc");
    config.defines.push("CONFIG_DEFINE");
    config.libraryDirs.push("config/lib");
    config.libraries.push("config-lib");

    opts.includeDirs.push("cli/include");
    opts.cSourceDirs.push("cli/csrc");
    opts.defines.push("CLI_DEFINE");
    opts.libraryDirs.push("cli/lib");
    opts.libraries.push("cli-lib");

    std::vector<std::string> includeDirs;
    session::mergeStrings(config, opts, "includeDirs", includeDirs);
    CHECK(includeDirs.size() == 2, "includeDirs merge concatenates both sources");
    CHECK(includeDirs[0] == "config/include", "includeDirs keeps config order first");
    CHECK(includeDirs[1] == "cli/include", "includeDirs keeps CLI order second");

    std::vector<std::string> cSourceDirs;
    session::mergeStrings(config, opts, "cSourceDirs", cSourceDirs);
    CHECK(cSourceDirs.size() == 2 && cSourceDirs[0] == "config/csrc" &&
              cSourceDirs[1] == "cli/csrc",
          "cSourceDirs merge keeps config then CLI order");

    std::vector<std::string> defines;
    session::mergeStrings(config, opts, "defines", defines);
    CHECK(defines.size() == 2 && defines[0] == "CONFIG_DEFINE" && defines[1] == "CLI_DEFINE",
          "defines merge keeps config then CLI order");

    std::vector<std::string> libraryDirs;
    session::mergeStrings(config, opts, "libraryDirs", libraryDirs);
    CHECK(libraryDirs.size() == 2 && libraryDirs[0] == "config/lib" && libraryDirs[1] == "cli/lib",
          "libraryDirs merge keeps config then CLI order");

    std::vector<std::string> libraries;
    session::mergeStrings(config, opts, "libraries", libraries);
    CHECK(libraries.size() == 2 && libraries[0] == "config-lib" && libraries[1] == "cli-lib",
          "libraries merge keeps config then CLI order");

    std::vector<std::string> second;
    second.push_back("existing");
    session::mergeStrings(config, opts, "defines", second, /*append=*/false);
    CHECK(second.size() == 2 && second[0] == "CONFIG_DEFINE" && second[1] == "CLI_DEFINE",
          "mergeStrings append=false clears the existing output first");
}

static void test_merge_strings_source_filter() {
    memory::Arena arena;
    ProjectConfig config(arena);
    Options opts(arena);

    config.includeDirs.push("config/include");
    opts.cSourceDirs.push("cli/csrc");

    std::vector<std::pair<bool, std::string>> merged;
    session::mergeStrings(
        config, opts, "includeDirs",
        [&](const std::string &value, const bool fromCli) { merged.emplace_back(fromCli, value); });
    CHECK(merged.size() == 1 && !merged[0].first && merged[0].second == "config/include",
          "source-filter appender sees only the requested includeDirs");

    merged.clear();
    session::mergeStrings(
        config, opts, "cSourceDirs",
        [&](const std::string &value, const bool fromCli) { merged.emplace_back(fromCli, value); });
    CHECK(merged.size() == 1 && merged[0].first && merged[0].second == "cli/csrc",
          "source-filter appender sees only the requested cSourceDirs");
}

// ── All test aggregation ──────────────────────────────────────────

static void test_cli_commands() {
    test_options_defaults();
    test_options_command_enum();
    test_build_derives_codegen_stage();
    test_new_emit_flags_parse_and_compose();
    test_virtual_machine_flag_parse_and_stage();
    test_run_emit_vir_still_executes_program();
    test_run_virtual_machine_executes_program();
    test_docs_options_parse_and_validate();
    test_docs_cli_rejects_invalid_options_before_compilation();
    test_completion_command_emits_updated_shell_options();
    test_docs_error_policy();
    test_docs_parse_and_import_error_policy();
    test_docs_stdout_file_and_index_outputs();
    test_deps_resolve_writes_deterministic_lockfile();
    test_deps_add_remove_edit_only_the_manifest();
    test_check_build_require_explicit_lockfile();
    test_create_writes_lockfile_for_generated_project();
    test_legacy_and_malformed_project_manifest_gate_behavior();
    test_project_root_stops_at_vcs_boundary();
    test_clean_preserves_artifacts_without_confirmation();
    test_command_signatures_exist();
    test_count_passed();
    test_command_names_match_contract();
    test_info_command_aliases_version();
    test_llvm_version_information();
    test_system_includes_flag();
    test_debug_sema_flag();
    test_verbose_flag_overrides_config();
    test_test_command_filters_and_executes_tests();
    test_merge_strings_order_and_append();
    test_merge_strings_source_filter();
}

} // namespace

TEST_MAIN(cli_commands)
