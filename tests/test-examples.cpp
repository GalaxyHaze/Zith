// End-to-end acceptance suite: every program under `examples/` is compiled and
// executed through the `zithc` CLI, and its exit code is compared against the
// value documented in that example's header comment.
#include "test-common.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

namespace {

struct Example {
    const char *file;
    int exitCode;
    bool needsCInterop;
};

constexpr Example kExamples[] = {
    {"linked-list.zith", 7, false},  {"for-loops.zith", 10, false},
    {"bitwise.zith", 0, false},      {"when-dispatch.zith", 20, false},
    {"arrays.zith", 5, false},       {"nested-structs.zith", 4, false},
    {"optionals.zith", 0, false},    {"generics-decl.zith", 3, false},
    {"tagged-union.zith", 0, false}, {"dyn-interface.zith", 18, false},
    {"c-interop.zith", 0, true},
};

#ifdef ZITH_ENABLE_C_INTEROP
constexpr bool kCInteropAvailable = true;
#else
constexpr bool kCInteropAvailable = false;
#endif

/// `zithc` writes `target/` and `cache/` next to the *source* file, so each
/// example is copied into the build tree before it runs. Running in place would
/// leave build artifacts inside the checked-in `examples/` directory.
std::string stageExample(const fs::path &workdir, const char *name) {
    std::error_code ec;
    fs::create_directories(workdir, ec);
    const fs::path staged = workdir / name;
    fs::copy_file(fs::path(ZITH_EXAMPLES_DIR) / name, staged, fs::copy_options::overwrite_existing,
                  ec);
    if (ec) {
        return {};
    }
    return staged.string();
}

int runExample(const fs::path &workdir, const char *name) {
    const std::string staged = stageExample(workdir, name);
    if (staged.empty()) {
        return -1;
    }
    const std::string command = std::string("cd \"") + workdir.string() + "\" && \"" +
                                ZITHC_BINARY + "\" --include \"" + ZITH_STDLIB_DIR + "\" run \"" +
                                staged + "\"";
    const int status = std::system(command.c_str());
    if (status < 0) {
        return -1;
    }
    // std::system reports a wait(2) status; the exit code is the high byte.
    return (status & 0x7F) == 0 ? ((status >> 8) & 0xFF) : -1;
}

std::string readFile(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// `zithc run` must keep the two output bands apart: the program owns stdout,
/// while compiler logs and `--emit-*` dumps go to stderr.
void test_run_separates_program_stdout_from_compiler_logs() {
    const fs::path workdir = fs::path(ZITH_EXAMPLES_WORKDIR);
    std::error_code ec;
    fs::create_directories(workdir, ec);

    const fs::path source = workdir / "run-output-split.zith";
    {
        std::ofstream out(source, std::ios::binary);
        out << "extern fn printf(fmt: *char, ...): i32\n"
               "fn main(): i32 {\n"
               "    printf(\"program-stdout=%d\\n\", 5);\n"
               "    return 21;\n"
               "}\n";
    }

    const fs::path outPath    = workdir / "run-split.stdout";
    const fs::path errPath    = workdir / "run-split.stderr";
    const std::string command = std::string("cd \"") + workdir.string() + "\" && \"" +
                                ZITHC_BINARY + "\" --include \"" + ZITH_STDLIB_DIR +
                                "\" run --emit-hir \"" + source.string() + "\" > \"" +
                                outPath.string() + "\" 2> \"" + errPath.string() + "\"";
    const int status   = std::system(command.c_str());
    const int exitCode = (status & 0x7F) == 0 ? ((status >> 8) & 0xFF) : -1;
    CHECK_EQ(exitCode, 21, "run --emit-hir exits with the program's status");

    const std::string out = readFile(outPath);
    const std::string err = readFile(errPath);

    CHECK(out == "program-stdout=5\n", "stdout carries only the program's bytes");
    CHECK(err.find("--- HIR ---") != std::string::npos, "the HIR dump lands on stderr");
    CHECK(err.find("program-stdout=5") == std::string::npos,
          "stderr does not duplicate the program's output");

    fs::remove(outPath, ec);
    fs::remove(errPath, ec);
    fs::remove(source, ec);
}

void test_examples() {
    const fs::path workdir = fs::path(ZITH_EXAMPLES_WORKDIR);

    for (const Example &example : kExamples) {
        if (example.needsCInterop && !kCInteropAvailable) {
            std::printf("  SKIP: %s requires C interop support\n", example.file);
            continue;
        }
        const int actual = runExample(workdir, example.file);
        const std::string label =
            std::string(example.file) + " exits with " + std::to_string(example.exitCode);
        CHECK_EQ(actual, example.exitCode, label.c_str());
    }

    test_run_separates_program_stdout_from_compiler_logs();
}

} // namespace

TEST_MAIN(examples)
