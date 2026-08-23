#include "cli/commands.hpp"
#include "cli/options.hpp"
#include "session/compilation-session.hpp"
#include "test-common.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace zith;

namespace {

struct TempProject {
    std::filesystem::path root;

    TempProject() : root(std::filesystem::temp_directory_path() / "zith-c-compile-tests") {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
    }

    ~TempProject() {
        std::filesystem::remove_all(root);
    }

    void write(std::string_view relativePath, std::string_view content) const {
        const auto path = root / relativePath;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << content;
    }
};

std::string readFile(FILE *file) {
    std::string text;
    std::rewind(file);
    char buffer[512];
    while (const size_t bytes = std::fread(buffer, 1, sizeof(buffer), file))
        text.append(buffer, bytes);
    return text;
}

std::string captureStdout(const std::function<void()> &fn) {
#ifdef _WIN32
    fn();
    return {};
#else
    std::fflush(stdout);
    FILE *tmp = std::tmpfile();
    if (tmp == nullptr)
        return {};

    const int saved = dup(STDOUT_FILENO);
    dup2(fileno(tmp), STDOUT_FILENO);
    fn();
    std::fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);

    const std::string captured = readFile(tmp);
    std::fclose(tmp);
    return captured;
#endif
}

size_t countOccurrences(const std::string &haystack, const std::string &needle) {
    size_t count = 0;
    size_t pos   = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

void test_cli_flag_parses_repeated_c_source_dirs() {
    char program[] = "zithc";
    char command[] = "build";
    char flag[]    = "--c-source-dir";
    char dirA[]    = "vendor/c";
    char dirB[]    = "src/native";
    char input[]   = "main.zith";
    char *argv[]   = {program, command, flag, dirA, flag, dirB, input};

    Cli cli;
    cli.parseArgs(7, argv);

    CHECK_EQ(cli.opts.cSourceDirs.size(), 2u, "CLI stores both --c-source-dir values");
    CHECK(cli.opts.cSourceDirs[0] == "vendor/c", "first --c-source-dir is preserved");
    CHECK(cli.opts.cSourceDirs[1] == "src/native", "second --c-source-dir is preserved");
}

void test_help_mentions_c_source_dir() {
    FILE *tmp = std::tmpfile();
    CHECK(tmp != nullptr, "tmpfile exists for help capture");
    if (tmp == nullptr)
        return;

    cli::commands::help(tmp);
    const std::string text = readFile(tmp);
    std::fclose(tmp);
    CHECK(text.find("--c-source-dir <DIR>") != std::string::npos,
          "help text documents --c-source-dir");
}

void test_completion_mentions_c_source_dir() {
    memory::Arena arena;
    Options opts(arena);
    opts.subcommandStr     = "bash";
    const std::string text = captureStdout([&] { cli::commands::completion(opts); });
    CHECK(text.find("--c-source-dir") != std::string::npos, "bash completion lists --c-source-dir");
}

void test_project_config_reads_c_source_dirs() {
    TempProject project;
    project.write("main.zith", "fn main(): i32 { return 0; }\n");
    project.write("ZithProject.toml", "[ffi]\n"
                                      "c_source_dirs = [\"vendor/c\", \"src/native\"]\n");

    memory::Arena arena;
    Options opts(arena);
    session::CompilationSession session(opts, (project.root / "main.zith").string());

    CHECK_EQ(session.projectConfig().cSourceDirs.size(), 2u,
             "session project config loads ffi.c_source_dirs");
    CHECK(session.projectConfig().cSourceDirs[0] == "vendor/c",
          "project keeps first C source root");
    CHECK(session.projectConfig().cSourceDirs[1] == "src/native",
          "project keeps second C source root");
}

void test_c_sources_compile_and_link() {
    TempProject project;
    project.write("main.zith", "extern fn ffi_add(a: i32, b: i32): i32\n"
                               "fn main(): i32 {\n"
                               "    return ffi_add(2, 5);\n"
                               "}\n");
    project.write("native/math.c", "int ffi_add(int a, int b) { return a + b; }\n");
    project.write("ZithProject.toml", "[ffi]\n"
                                      "c_source_dirs = [\"native\"]\n");

    memory::Arena arena;
    Options opts(arena);
    session::CompilationSession session(opts, (project.root / "main.zith").string());
    session.setBuffered(true);
    session.setAlwaysEmitObject(true);

    CHECK(session.run(), "main program with extern fn reaches object emission");
    CHECK(session.linkAndExec(), "discovered C sources compile and link into the final executable");
    CHECK_EQ(session.childExitCode(), 7, "runtime can call the discovered C implementation");
}

void test_c_source_roots_deduplicate_overlaps() {
    TempProject project;
    project.write("main.zith", "extern fn only_once(): i32\n"
                               "fn main(): i32 {\n"
                               "    return only_once();\n"
                               "}\n");
    project.write("native/once.c", "int only_once(void) { return 11; }\n");
    project.write("ZithProject.toml", "[ffi]\n"
                                      "c_source_dirs = [\"native\", \"native/.\"]\n");

    memory::Arena arena;
    Options opts(arena);
    opts.flags.verbose(true);
    session::CompilationSession session(opts, (project.root / "main.zith").string());
    session.setBuffered(true);
    session.setAlwaysEmitObject(true);

    CHECK(session.run(), "overlapping C roots still allow codegen");
    CHECK(session.linkAndExec(), "overlapping C roots still link and execute");
    const std::string output = session.flushOutput() + session.takeChildOutput();
    CHECK_EQ(countOccurrences(output, "  [c-compile] "), 1u,
             "overlapping roots do not compile the same C source twice");
}

void test_invalid_c_source_aborts_link() {
    TempProject project;
    project.write("main.zith", "fn main(): i32 { return 0; }\n");
    project.write("native/bad.c", "int broken( { return 0; }\n");
    project.write("ZithProject.toml", "[ffi]\n"
                                      "c_source_dirs = [\"native\"]\n");

    memory::Arena arena;
    Options opts(arena);
    opts.flags.verbose(true);
    session::CompilationSession session(opts, (project.root / "main.zith").string());
    session.setBuffered(true);
    session.setAlwaysEmitObject(true);

    CHECK(session.run(), "invalid companion C source does not block Zith object emission");
    CHECK(!session.link(), "invalid C source aborts before native link succeeds");
    const std::string output = session.flushOutput();
    CHECK(output.find("C compilation failed") != std::string::npos,
          "link path reports a C compilation failure");
    CHECK(session.executablePath().empty(), "failed C compilation leaves no executable path");
}

#ifdef ZITH_ENABLE_C_INTEROP
void test_libclang_header_import_can_link_discovered_c_source() {
    TempProject project;
    project.write("main.zith", "import \"native/add.h\";\n"
                               "fn main(): i32 {\n"
                               "    return imported_sum(3, 9);\n"
                               "}\n");
    project.write("native/add.h", "int imported_sum(int a, int b);\n");
    project.write("native/add.c", "int imported_sum(int a, int b) { return a + b; }\n");
    project.write("ZithProject.toml", "[ffi]\n"
                                      "include_dirs = [\".\"]\n"
                                      "c_source_dirs = [\"native\"]\n");

    memory::Arena arena;
    Options opts(arena);
    session::CompilationSession session(opts, (project.root / "main.zith").string());
    session.setBuffered(true);
    session.setAlwaysEmitObject(true);

    CHECK(session.run(), "libclang import of a project header succeeds");
    CHECK(session.linkAndExec(), "imported header links against discovered C implementation");
    CHECK_EQ(session.childExitCode(), 12, "imported header and compiled C source agree at runtime");
}
#endif

void test_c_compile() {
    test_cli_flag_parses_repeated_c_source_dirs();
    test_help_mentions_c_source_dir();
    test_completion_mentions_c_source_dir();
    test_project_config_reads_c_source_dirs();
    test_c_sources_compile_and_link();
    test_c_source_roots_deduplicate_overlaps();
    test_invalid_c_source_aborts_link();
#ifdef ZITH_ENABLE_C_INTEROP
    test_libclang_header_import_can_link_discovered_c_source();
#endif
}

} // namespace

TEST_MAIN(c_compile)
