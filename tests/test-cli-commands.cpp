#include "cli/commands.hpp"
#include "cli/options.hpp"
#include "test-common.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace zith;
using namespace zith::cli::commands;

namespace {

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

// ── All test aggregation ──────────────────────────────────────────

static void test_cli_commands() {
    test_options_defaults();
    test_options_command_enum();
    test_build_derives_codegen_stage();
    test_command_signatures_exist();
    test_count_passed();
    test_command_names_match_contract();
    test_llvm_version_information();
    test_system_includes_flag();
}

} // namespace

TEST_MAIN(cli_commands)
