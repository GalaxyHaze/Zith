#pragma once

#include "memory/source-map.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace zith::bench {

struct LexerScenario {
    std::string name;
    std::string source;
};

struct PreparedLexerScenario {
    LexerScenario scenario;
    memory::SourceMap sourceMap;
    memory::FileId fileId = 0;

    explicit PreparedLexerScenario(LexerScenario scenario_);
};

struct LexerRunResult {
    uint32_t tokenCount = 0;
    uint64_t checksum   = 0;
};

struct LexerBenchmarkOptions {
    uint32_t warmup  = 5;
    uint32_t samples = 30;
};

struct LexerBenchmarkResult {
    size_t inputBytes                 = 0;
    uint32_t tokenCount               = 0;
    uint32_t tokenizationsPerSample   = 0;
    uint64_t checksum                 = 0;
    std::vector<uint64_t> sampleNs;
};

struct LexerBenchmarkStatistics {
    double minimumNsPerRun        = 0.0;
    double medianNsPerRun         = 0.0;
    double p95NsPerRun            = 0.0;
    double maximumNsPerRun        = 0.0;
    double medianMiBPerSecond     = 0.0;
    double medianMTokensPerSecond = 0.0;
    double medianNsPerToken       = 0.0;
};

struct HostInfo {
    std::string operatingSystem;
    std::string cpuModel;
    std::string compiler;
    uint32_t logicalCores   = 0;
    uint64_t totalMemoryBytes = 0;
};

[[nodiscard]] LexerScenario makeLexerScenario(std::string_view name);
[[nodiscard]] HostInfo queryHostInfo();
[[nodiscard]] bool prepareLexerScenario(PreparedLexerScenario &scenario, std::string &error);
[[nodiscard]] bool tokenizeOnce(PreparedLexerScenario &scenario, memory::Arena &arena, LexerRunResult &result,
                                std::string &error);
[[nodiscard]] bool runLexerBenchmark(PreparedLexerScenario &scenario,
                                     const LexerBenchmarkOptions &options,
                                     LexerBenchmarkResult &result, std::string &error);
[[nodiscard]] LexerBenchmarkStatistics
calculateLexerBenchmarkStatistics(const LexerBenchmarkResult &result);

void printLexerBenchmarkText(const PreparedLexerScenario &scenario,
                             const LexerBenchmarkOptions &options,
                             const LexerBenchmarkResult &result,
                             const LexerBenchmarkStatistics &statistics, const HostInfo &host);
void printLexerBenchmarkJson(const PreparedLexerScenario &scenario,
                             const LexerBenchmarkOptions &options,
                             const LexerBenchmarkResult &result,
                             const LexerBenchmarkStatistics &statistics, const HostInfo &host);

} // namespace zith::bench
