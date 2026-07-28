#include "bench/lexer-benchmark.hpp"
#include "test-common.hpp"

using namespace zith;

static bool isValidUtf8(std::string_view text) {
    for (char byte : text) {
        if (static_cast<unsigned char>(byte) > 0x7F)
            return false;
    }
    return true;
}

static void test_mixed_valid_scenario() {
    const bench::LexerScenario scenario = bench::makeLexerScenario("mixed-valid");
    CHECK(!scenario.source.empty(), "scenario source is non-empty");
    CHECK_EQ(scenario.name, "mixed-valid", "scenario name is mixed-valid");
    CHECK(isValidUtf8(scenario.source), "scenario source is valid UTF-8");

    bench::PreparedLexerScenario prepared(scenario);
    std::string error;
    CHECK(bench::prepareLexerScenario(prepared, error), "scenario materializes in SourceMap");

    bench::LexerRunResult run;
    zith::memory::Arena arena(prepared.scenario.source.size() * 4);
    CHECK(bench::tokenizeOnce(prepared, arena, run, error),
          "scenario tokenizes without diagnostics");
    CHECK(run.tokenCount > 1, "token stream contains content and End");
    CHECK(run.checksum != 0, "token stream checksum is non-zero");
}

static void test_host_info() {
    const bench::HostInfo host = bench::queryHostInfo();
    CHECK(!host.operatingSystem.empty(), "operating system is reported");
    CHECK(!host.cpuModel.empty(), "CPU model is reported or marked unknown");
    CHECK(!host.compiler.empty(), "compiler is reported");
    CHECK(host.logicalCores > 0, "logical CPU count is reported");
}

static void test_lexer_benchmark() {
    test_mixed_valid_scenario();
    test_host_info();
}

TEST_MAIN(lexer_benchmark)
