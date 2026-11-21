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
    const char *file           = nullptr;
    int exitCode               = 0;
    bool needsCInterop         = false;
    const char *expectedStdout = nullptr;
};

constexpr Example kExamples[] = {
    {"allocator-simple.zithm", 42, false},
    {"bindings-simple.zithm", 6, false},
    {"bindings-advanced.zithm", 42, false},
    {"bitwise-simple.zithm", 7, false},
    {"bitwise-advanced.zithm", 3, false},
    {"booleans-simple.zithm", 7, false},
    {"booleans-advanced.zithm", 10, false},
    {"const-enums-simple.zithm", 7, false},
    {"const-enums-advanced.zithm", 12, false},
    {"dyn-array.zithm", 0, false},
    {"dyn-interfaces-simple.zithm", 10, false},
    {"dyn-interfaces-advanced.zithm", 14, false},
    {"enum-union-generics.zithm", 42, false},
    {"function-values-simple.zithm", 14, false},
    {"function-values-advanced.zithm", 31, false},
    {"functions-defaults-simple.zithm", 15, false},
    {"functions-defaults-advanced.zithm", 17, false},
    {"generics-simple.zithm", 42, false},
    {"generics-advanced.zithm", 82, false},
    {"hash-map.zithm", 0, false},
    {"hash-map-u64.zithm", 0, false},
    {"inplace-simple.zithm", 42, false},
    {"loops-simple.zithm", 12, false},
    {"loops-advanced.zithm", 10, false},
    {"macros-simple.zithm", 42, false},
    {"macros-advanced.zithm", 9, false},
    {"opaque-simple.zithm", 0, false},
    {"opaque-advanced.zithm", 42, false},
    {"optionals-simple.zithm", 4, false},
    {"optionals-advanced.zithm", 14, false},
    {"ownership-simple.zithm", 42, false},
    {"ownership-advanced.zithm", 16, false},
    {"slices-variadic-simple.zithm", 6, false},
    {"slices-variadic-advanced.zithm", 24, false},
    {"state-defer-simple.zithm", 42, false},
    {"state-defer-advanced.zithm", 42, false},
    {"structs-arrays-simple.zithm", 9, false},
    {"structs-arrays-advanced.zithm", 7, false},
    {"styles-zith.zithm", 0, false, "bucket=1\n"},
    {"styles-c.zithm", 0, true, "bucket=1\n"},
    {"styles-functional.zithm", 0, false, "bucket=1\n"},
    {"tagged-unions-simple.zithm", 0, false},
    {"tagged-unions-advanced.zithm", 7, false},
    {"when-simple.zithm", 14, false},
    {"when-advanced.zithm", 2, false},
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

std::string readFile(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int runExample(const fs::path &workdir, const char *name, std::string *capturedStdout = nullptr) {
    std::error_code ec;
    fs::remove_all(workdir / "cache", ec);
    fs::remove_all(workdir / "target", ec);
    fs::remove_all(workdir / ".zith-cache", ec);
    const std::string staged = stageExample(workdir, name);
    if (staged.empty()) {
        return -1;
    }
    const fs::path stdoutPath = workdir / (std::string(name) + ".stdout");
    if (capturedStdout != nullptr) {
        fs::remove(stdoutPath, ec);
    }
    std::string command = std::string("cd \"") + workdir.string() + "\" && \"" + ZITHC_BINARY +
                          "\" --include \"" + ZITH_STDLIB_DIR + "\" run \"" + staged + "\"";
    if (capturedStdout != nullptr) {
        command += " > \"" + stdoutPath.string() + "\"";
    }
    const int status = std::system(command.c_str());
    if (capturedStdout != nullptr) {
        *capturedStdout = readFile(stdoutPath);
        fs::remove(stdoutPath, ec);
    }
    if (status < 0) {
        return -1;
    }
    // std::system reports a wait(2) status; the exit code is the high byte.
    return (status & 0x7F) == 0 ? ((status >> 8) & 0xFF) : -1;
}

/// `zithc run` must keep the two output bands apart: the program owns stdout,
/// while compiler logs and `--emit-*` dumps go to stderr.
void test_run_separates_program_stdout_from_compiler_logs() {
    const fs::path workdir = fs::path(ZITH_EXAMPLES_WORKDIR);
    std::error_code ec;
    fs::create_directories(workdir, ec);

    const fs::path source = workdir / "run-output-split.zithm";
    {
        std::ofstream out(source, std::ios::binary);
        out << "extern fn printf(fmt: *char, ...): i32\n"
               "fn main(): i32 {\n"
               "    _ = printf(\"program-stdout=%d\\n\", 5);\n"
               "    return 21;\n"
               "}\n";
    }

    const fs::path outPath    = workdir / "run-split.stdout";
    const fs::path errPath    = workdir / "run-split.stderr";
    const std::string command = std::string("cd \"") + workdir.string() + "\" && \"" +
                                ZITHC_BINARY + "\" --include \"" + ZITH_STDLIB_DIR +
                                "\" run --emit-hir \"" + source.string() + "\" > \"" +
                                outPath.string() + "\" 2> \"" + errPath.string() + "\"";
    const int status          = std::system(command.c_str());
    const int exitCode        = (status & 0x7F) == 0 ? ((status >> 8) & 0xFF) : -1;
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

void test_debug_sema_probes() {
    const fs::path workdir = fs::path(ZITH_EXAMPLES_WORKDIR);
    std::error_code ec;
    fs::create_directories(workdir, ec);

    const fs::path source = workdir / "debug-sema-probes.zithm";
    {
        std::ofstream out(source, std::ios::binary);
        out << "fn add(x: i32, y: i32): i32 {\n"
               "    return x + y;\n"
               "}\n"
               "fn main(): i32 {\n"
               "    return add(1, 2);\n"
               "}\n";
    }

    const fs::path outPath = workdir / "debug-sema-probes.stdout";
    const fs::path errPath = workdir / "debug-sema-probes.stderr";
    const auto runWithFlag = [&](bool debugSema) {
        fs::remove_all(workdir / "cache", ec);
        fs::remove_all(workdir / "target", ec);
        fs::remove_all(workdir / ".zith-cache", ec);
        const std::string flag    = debugSema ? " --debug-sema" : "";
        const std::string command = std::string("cd \"") + workdir.string() + "\" && \"" +
                                    ZITHC_BINARY + "\" --include \"" + ZITH_STDLIB_DIR + "\" run" +
                                    flag + " \"" + source.string() + "\" > \"" + outPath.string() +
                                    "\" 2> \"" + errPath.string() + "\"";
        const int status          = std::system(command.c_str());
        return (status & 0x7F) == 0 ? ((status >> 8) & 0xFF) : -1;
    };

    CHECK_EQ(runWithFlag(false), 3, "sema probes off does not change run exit code");
    CHECK(readFile(errPath).find("[probe]") == std::string::npos,
          "stderr has no sema probes by default");

    CHECK_EQ(runWithFlag(true), 3, "sema probes on does not change run exit code");
    const std::string err = readFile(errPath);
    CHECK(err.find("[probe] inferCall") != std::string::npos,
          "--debug-sema prints inferCall probes on stderr");

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
        std::string capturedStdout;
        const int actual = runExample(
            workdir, example.file, example.expectedStdout == nullptr ? nullptr : &capturedStdout);
        const std::string label =
            std::string(example.file) + " exits with " + std::to_string(example.exitCode);
        CHECK_EQ(actual, example.exitCode, label.c_str());
        if (example.expectedStdout != nullptr) {
            const std::string outputLabel = std::string(example.file) + " prints expected output";
            CHECK_EQ(capturedStdout, std::string(example.expectedStdout), outputLabel.c_str());
        }
    }

    test_run_separates_program_stdout_from_compiler_logs();
    test_debug_sema_probes();
}

} // namespace

TEST_MAIN(examples)
