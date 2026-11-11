#include "cli/options.hpp"
#include "interp/hir-interpreter.hpp"
#include "session/compilation-session.hpp"
#include "test-common.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>

using namespace zith;

namespace {

namespace fs = std::filesystem;

void test_abi_exec_09_hello_world() {
    const fs::path root = fs::temp_directory_path() / "zith-abi-execution-tests";
    fs::remove_all(root);
    fs::create_directories(root);

    const fs::path source = root / "main.zith";
    std::ofstream output(source, std::ios::binary | std::ios::trunc);
    output << "extern fn puts(msg: *char)\n"
              "\n"
              "fn add(a: i32, b: i32): i32 {\n"
              "    a + b\n"
              "}\n"
              "\n"
              "fn main(): i32 {\n"
              "    var sum: i32 = add(2, 3);\n"
              "    _ = puts(\"hello\");\n"
              "    sum\n"
              "}\n";
    output.close();

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, source.string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "hello-world source lowers through the modern pipeline");

    interp::HirInterpreter interpreter(session.hirModule(), session.interner(), session.types());
    const auto result = interpreter.runMain();

    CHECK(result.status == interp::HirInterpStatus::Ok,
          "HIR interpreter runs the hello-world program");
    CHECK_EQ(result.output, std::string("hello\n"), "interpreter output matches the puts call");
    CHECK_EQ(result.exitCode, 5, "main returns the computed sum");
}

void test_abi_exec_10_control_flow() {
    // Regression: the interpreter must evaluate a `HirBranch` terminator and
    // the comparison binary ops, otherwise any `if` loops on the entry block.
    const fs::path root = fs::temp_directory_path() / "zith-abi-execution-flow";
    fs::remove_all(root);
    fs::create_directories(root);

    const fs::path source = root / "main.zith";
    std::ofstream output(source, std::ios::binary | std::ios::trunc);
    output << "fn pick(v: i32): i32 {\n"
              "    if (v > 3) { return 7; }\n"
              "    return 1;\n"
              "}\n"
              "\n"
              "fn main(): i32 {\n"
              "    pick(5)\n"
              "}\n";
    output.close();

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, source.string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "control-flow source lowers through the modern pipeline");

    interp::HirInterpreter interpreter(session.hirModule(), session.interner(), session.types());
    const auto result = interpreter.runMain();

    CHECK(result.status == interp::HirInterpStatus::Ok,
          "interpreter runs a program with an if and a comparison");
    CHECK_EQ(result.exitCode, 7, "the taken if branch drives the returned value");
}

void test_abi_exec_11_broadcast_initialization() {
    const fs::path root = fs::temp_directory_path() / "zith-abi-execution-broadcast";
    fs::remove_all(root);
    fs::create_directories(root);

    const fs::path source = root / "main.zith";
    std::ofstream output(source, std::ios::binary | std::ios::trunc);
    output << "alias Pair = |first: i32, second: i32|;\n"
              "alias Count = i32;\n"
              "fn main(): i32 {\n"
              "    var [a, b, c]: i32 = 4;\n"
              "    let [x, y]: i32 = 7;\n"
              "    let packed = |3, 4|;\n"
              "    let [p, q] = packed;\n"
              "    let [r, s]: i32 = |8, 9|;\n"
              "    const [u, v]: i32 = 2;\n"
              "    let pair: Pair = |10, 11|;\n"
              "    let [m, n]: Pair = pair;\n"
              "    let [ca, cb]: Count = 5;\n"
              "    var [fa, fb, fc]: f64 = 0.0;\n"
              "    a += 1;\n"
              "    b += 2;\n"
              "    c += 3;\n"
              "    _ = fa;\n"
              "    _ = fb;\n"
              "    _ = fc;\n"
              "    return a * 100 + b * 10 + c + x + y + p * 10 + q + r * 10 + s + u + v + "
              "m * 10 + n + ca + cb;\n"
              "}\n";
    output.close();

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, source.string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "broadcast and pack destructuring lower through the modern pipeline");

    interp::HirInterpreter interpreter(session.hirModule(), session.interner(), session.types());
    const auto result = interpreter.runMain();

    CHECK(result.status == interp::HirInterpStatus::Ok,
          "HIR interpreter runs a program with broadcast bindings");
    CHECK_EQ(result.exitCode, 829,
             "broadcasting preserves binding kinds and pack aliases destructure positionally");
}

void test_abi_exec_02_cli_interpreted() {
#ifdef ZITHC_BINARY
    const fs::path root = fs::temp_directory_path() / "zith-abi-execution-cli";
    fs::remove_all(root);
    fs::create_directories(root);

    const fs::path source = root / "main.zith";
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        output << "extern fn puts(msg: *char)\n"
                  "fn main(): i32 {\n"
                  "    _ = puts(\"cli-interpreted\");\n"
                  "    9\n"
                  "}\n";
    }

    const fs::path stdoutPath = root / "stdout.txt";
    const fs::path stderrPath = root / "stderr.txt";
    const std::string command = std::string("\"") + ZITHC_BINARY + "\" --include \"" +
                                ZITH_STDLIB_DIR + "\" run --interpreted \"" + source.string() +
                                "\" > \"" + stdoutPath.string() + "\" 2> \"" + stderrPath.string() +
                                "\"";
    const int status = std::system(command.c_str());
    const int code   = (status & 0x7F) == 0 ? ((status >> 8) & 0xFF) : -1;

    CHECK_EQ(code, 9, "zithc run --interpreted exits with the main return value");
    std::ifstream stdoutFile(stdoutPath, std::ios::binary);
    std::string output((std::istreambuf_iterator<char>(stdoutFile)),
                       std::istreambuf_iterator<char>());
    CHECK_EQ(output, std::string("cli-interpreted\n"),
             "zithc --interpreted writes program output to stdout");
#else
    CHECK(true, "CLI interpreted test skipped without a zithc target");
#endif
}

void test_abi_execution() {
    test_abi_exec_09_hello_world();
    test_abi_exec_10_control_flow();
    test_abi_exec_11_broadcast_initialization();
    test_abi_exec_02_cli_interpreted();
}

} // namespace

TEST_MAIN(abi_execution)
