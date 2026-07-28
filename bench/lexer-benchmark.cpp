#include "lexer-benchmark.hpp"

#include "diagnostics/diagnostic-engine.hpp"
#include "legacy-zith/lexer/lexer.hpp"
#include "memory/arena.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <thread>
#include <utility>

#if defined(__linux__)
#include <sys/utsname.h>
#endif

namespace zith::bench {
namespace {

constexpr size_t kTargetScenarioBytes = static_cast<size_t>(1024) * 1024;
constexpr uint64_t kTargetSampleNs    = 100'000'000;

uint64_t checksumTokenStream(const lexer::TokenStream &stream) {
    uint64_t checksum = 14695981039346656037ULL;
    for (uint32_t index = 0; index < stream.len; ++index) {
        const lexer::Token &token = stream.src[index];
        checksum ^= static_cast<uint64_t>(token.kind);
        checksum *= 1099511628211ULL;
        checksum ^= token.span.start;
        checksum *= 1099511628211ULL;
        checksum ^= token.span.end;
        checksum *= 1099511628211ULL;
        checksum ^= static_cast<uint8_t>(token.punc);
        checksum *= 1099511628211ULL;
    }
    return checksum;
}

uint64_t measureTokenizations(PreparedLexerScenario &scenario, uint32_t iterations,
                              uint64_t &checksum, LexerRunResult &lastRun, std::string &error) {
    memory::Arena arena(scenario.scenario.source.size() * 4);
    const auto begin = std::chrono::steady_clock::now();
    for (uint32_t index = 0; index < iterations; ++index) {
        arena.reset();
        if (!tokenizeOnce(scenario, arena, lastRun, error))
            return 0;
        checksum ^= lastRun.checksum + 0x9e3779b97f4a7c15ULL + (checksum << 6) + (checksum >> 2);
    }
    const auto end = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
}

uint32_t calibratedIterations(uint64_t durationNs) {
    if (durationNs == 0)
        return 1;
    const uint64_t iterations = (kTargetSampleNs + durationNs - 1) / durationNs;
    return static_cast<uint32_t>(
        std::min<uint64_t>(iterations, std::numeric_limits<uint32_t>::max()));
}

double percentile(std::vector<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    const size_t index =
        static_cast<size_t>(std::ceil(fraction * static_cast<double>(values.size() - 1)));
    return values[index];
}

std::string trim(std::string_view value) {
    constexpr std::string_view whitespace = " \t\r\n";
    const size_t begin                    = value.find_first_not_of(whitespace);
    if (begin == std::string_view::npos)
        return {};
    const size_t end = value.find_last_not_of(whitespace);
    return std::string(value.substr(begin, end - begin + 1));
}

std::string compilerDescription() {
#if defined(__clang__)
    return "Clang " + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__) +
           "." + std::to_string(__clang_patchlevel__);
#elif defined(__GNUC__)
    return "GCC " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__) + "." +
           std::to_string(__GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_VER);
#else
    return "unknown";
#endif
}

#if defined(__linux__)
std::string linuxCpuModel() {
    std::ifstream cpuInfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuInfo, line)) {
        constexpr std::string_view kModelName = "model name";
        const size_t separator                = line.find(':');
        if (separator != std::string::npos &&
            trim(std::string_view(line).substr(0, separator)) == kModelName)
            return trim(std::string_view(line).substr(separator + 1));
    }
    return {};
}

uint64_t linuxMemoryBytes() {
    std::ifstream memInfo("/proc/meminfo");
    std::string line;
    while (std::getline(memInfo, line)) {
        constexpr std::string_view kMemTotal = "MemTotal:";
        if (!std::string_view(line).starts_with(kMemTotal))
            continue;

        const std::string_view value = std::string_view(line).substr(kMemTotal.size());
        uint64_t kib                 = 0;
        const char *begin            = value.data();
        const char *end              = begin + value.size();
        while (begin != end && (*begin == ' ' || *begin == '\t'))
            ++begin;
        const auto parsed = std::from_chars(begin, end, kib);
        if (parsed.ec == std::errc{})
            return kib * 1024;
        return 0;
    }
    return 0;
}
#endif

void printJsonString(std::string_view value) {
    std::putchar('"');
    for (char byte : value) {
        const unsigned char character = static_cast<unsigned char>(byte);
        switch (character) {
        case '"':
            std::fputs("\\\"", stdout);
            break;
        case '\\':
            std::fputs("\\\\", stdout);
            break;
        case '\n':
            std::fputs("\\n", stdout);
            break;
        case '\r':
            std::fputs("\\r", stdout);
            break;
        case '\t':
            std::fputs("\\t", stdout);
            break;
        default:
            if (character < 0x20) {
                std::printf("\\u%04x", static_cast<unsigned int>(character));
            } else {
                std::putchar(character);
            }
            break;
        }
    }
    std::putchar('"');
}

} // namespace

PreparedLexerScenario::PreparedLexerScenario(LexerScenario scenario_)
    : scenario(std::move(scenario_)) {}

LexerScenario makeLexerScenario(std::string_view name) {
    if (name != "mixed-valid")
        return {};

    static constexpr std::string_view kBlock = R"(/// Documentation for benchmark_item.
// A normal single-line comment.
/** Documentation block. */
/* A normal block comment. */
fn benchmark_item_123(value: i64) -> i64 {
    let decimal_value = 123456789;
    let hexadecimal_value = 0xDEADBEEF;
    let octal_value = 0c76543210;
    let binary_value = 0b10101010;
    let message = "escaped\n\t\"quote\"\\slash";
    if decimal_value >= 42 and true {
        return value + decimal_value - hexadecimal_value / octal_value * binary_value % 7;
    } else {
        return value;
    }
}
@annotation #tag `quoted` [item], :; . ! = < > & | ^ ~ ?
)";

    LexerScenario scenario;
    scenario.name = "mixed-valid";
    scenario.source.reserve(kTargetScenarioBytes + kBlock.size());
    while (scenario.source.size() + kBlock.size() <= kTargetScenarioBytes)
        scenario.source.append(kBlock);
    return scenario;
}

HostInfo queryHostInfo() {
    HostInfo host;
    host.compiler     = compilerDescription();
    host.logicalCores = std::thread::hardware_concurrency();

#if defined(__linux__)
    utsname system{};
    if (uname(&system) == 0) {
        host.operatingSystem =
            std::string(system.sysname) + " " + system.release + " " + system.machine;
    }
    host.cpuModel         = linuxCpuModel();
    host.totalMemoryBytes = linuxMemoryBytes();
#elif defined(_WIN32)
    host.operatingSystem = "Windows";
#elif defined(__APPLE__)
    host.operatingSystem = "macOS";
#else
    host.operatingSystem = "unknown";
#endif

    if (host.cpuModel.empty())
        host.cpuModel = "unknown";
    if (host.operatingSystem.empty())
        host.operatingSystem = "unknown";
    return host;
}

bool prepareLexerScenario(PreparedLexerScenario &scenario, std::string &error) {
    if (scenario.scenario.name.empty() || scenario.scenario.source.empty()) {
        error = "benchmark scenario is empty";
        return false;
    }

    const auto addResult = scenario.sourceMap.addFile("bench-lexer.zith", scenario.scenario.source);
    if (!addResult) {
        error = addResult.error().msg;
        return false;
    }
    scenario.fileId = addResult.value();
    return true;
}

bool tokenizeOnce(PreparedLexerScenario &scenario, memory::Arena &arena, LexerRunResult &result,
                  std::string &error) {
    diagnostics::DiagnosticEngine diagnostics(arena);
    const auto tokenResult =
        lexer::tokenize(scenario.sourceMap, arena, scenario.fileId, diagnostics);
    if (!tokenResult) {
        error = tokenResult.error().msg;
        return false;
    }
    if (diagnostics.hasErrors()) {
        error = "tokenization emitted diagnostics";
        return false;
    }

    const lexer::TokenStream stream = tokenResult.value();
    if (stream.len == 0 || stream.src == nullptr ||
        stream.src[stream.len - 1].kind != lexer::TokenKind::End) {
        error = "token stream does not end with TokenKind::End";
        return false;
    }

    result.tokenCount = stream.len;
    result.checksum   = checksumTokenStream(stream);
    if (result.checksum == 0) {
        error = "token stream checksum is zero";
        return false;
    }
    return true;
}

bool runLexerBenchmark(PreparedLexerScenario &scenario, const LexerBenchmarkOptions &options,
                       LexerBenchmarkResult &result, std::string &error) {
    if (options.samples == 0) {
        error = "samples must be greater than zero";
        return false;
    }

    LexerRunResult lastRun;
    uint64_t checksum = 0;
    for (uint32_t index = 0; index < options.warmup; ++index) {
        if (measureTokenizations(scenario, 1, checksum, lastRun, error) == 0)
            return false;
    }

    const uint64_t calibrationNs = measureTokenizations(scenario, 1, checksum, lastRun, error);
    if (calibrationNs == 0)
        return false;

    result.inputBytes             = scenario.scenario.source.size();
    result.tokenCount             = lastRun.tokenCount;
    result.tokenizationsPerSample = calibratedIterations(calibrationNs);
    result.sampleNs.clear();
    result.sampleNs.reserve(options.samples);

    for (uint32_t index = 0; index < options.samples; ++index) {
        const uint64_t duration =
            measureTokenizations(scenario, result.tokenizationsPerSample, checksum, lastRun, error);
        if (duration == 0)
            return false;
        result.sampleNs.push_back(duration);
    }
    result.checksum = checksum;
    return true;
}

LexerBenchmarkStatistics calculateLexerBenchmarkStatistics(const LexerBenchmarkResult &result) {
    LexerBenchmarkStatistics statistics;
    if (result.sampleNs.empty() || result.tokenizationsPerSample == 0 || result.tokenCount == 0)
        return statistics;

    std::vector<double> nsPerRun;
    nsPerRun.reserve(result.sampleNs.size());
    for (uint64_t duration : result.sampleNs)
        nsPerRun.push_back(static_cast<double>(duration) / result.tokenizationsPerSample);

    std::vector<double> sorted = nsPerRun;
    std::sort(sorted.begin(), sorted.end());
    statistics.minimumNsPerRun = sorted.front();
    statistics.medianNsPerRun  = percentile(nsPerRun, 0.50);
    statistics.p95NsPerRun     = percentile(nsPerRun, 0.95);
    statistics.maximumNsPerRun = sorted.back();

    const double bytesPerMiB      = 1024.0 * 1024.0;
    statistics.medianMiBPerSecond = (static_cast<double>(result.inputBytes) / bytesPerMiB) *
                                    1'000'000'000.0 / statistics.medianNsPerRun;
    statistics.medianMTokensPerSecond =
        static_cast<double>(result.tokenCount) * 1'000.0 / statistics.medianNsPerRun;
    statistics.medianNsPerToken =
        statistics.medianNsPerRun / static_cast<double>(result.tokenCount);
    return statistics;
}

void printLexerBenchmarkText(const PreparedLexerScenario &scenario,
                             const LexerBenchmarkOptions &options,
                             const LexerBenchmarkResult &result,
                             const LexerBenchmarkStatistics &statistics, const HostInfo &host) {
    std::printf("bench-lexer %s\n", scenario.scenario.name.c_str());
    std::printf("host: %s\n", host.operatingSystem.c_str());
    std::printf("cpu: %s (%u logical cores)\n", host.cpuModel.c_str(), host.logicalCores);
    if (host.totalMemoryBytes != 0) {
        const double gib = static_cast<double>(host.totalMemoryBytes) / (1024.0 * 1024.0 * 1024.0);
        std::printf("memory: %.2f GiB\n", gib);
    }
    std::printf("compiler: %s\n", host.compiler.c_str());
    std::printf("input: %zu bytes, %u tokens\n", result.inputBytes, result.tokenCount);
    std::printf("configuration: warmup=%u samples=%u tokenizations/sample=%u\n", options.warmup,
                options.samples, result.tokenizationsPerSample);
    std::printf("median: %.2f MiB/s, %.2f Mtokens/s, %.2f ns/token\n",
                statistics.medianMiBPerSecond, statistics.medianMTokensPerSecond,
                statistics.medianNsPerToken);
    std::printf("time per tokenization: median %.0f ns, p95 %.0f ns, min %.0f ns, max %.0f ns\n",
                statistics.medianNsPerRun, statistics.p95NsPerRun, statistics.minimumNsPerRun,
                statistics.maximumNsPerRun);
    std::printf("checksum: %llu\n", static_cast<unsigned long long>(result.checksum));
}

void printLexerBenchmarkJson(const PreparedLexerScenario &scenario,
                             const LexerBenchmarkOptions &options,
                             const LexerBenchmarkResult &result,
                             const LexerBenchmarkStatistics &statistics, const HostInfo &host) {
    std::printf("{\"scenario\":");
    printJsonString(scenario.scenario.name);
    std::printf(",\"host\":{\"operating_system\":");
    printJsonString(host.operatingSystem);
    std::printf(",\"cpu_model\":");
    printJsonString(host.cpuModel);
    std::printf(",\"logical_cores\":%u,\"total_memory_bytes\":%llu,\"compiler\":",
                host.logicalCores, static_cast<unsigned long long>(host.totalMemoryBytes));
    printJsonString(host.compiler);
    std::printf("},\"configuration\":{\"warmup\":%u,\"samples\":%u,"
                "\"tokenizations_per_sample\":%u},\"input\":{\"bytes\":%zu,\"tokens\":%u},"
                "\"statistics\":{\"median_ns\":%.6f,\"p95_ns\":%.6f,\"min_ns\":%.6f,"
                "\"max_ns\":%.6f,\"median_mib_per_s\":%.6f,\"median_mtokens_per_s\":%.6f,"
                "\"median_ns_per_token\":%.6f},\"samples_ns\":[",
                options.warmup, options.samples, result.tokenizationsPerSample, result.inputBytes,
                result.tokenCount, statistics.medianNsPerRun, statistics.p95NsPerRun,
                statistics.minimumNsPerRun, statistics.maximumNsPerRun,
                statistics.medianMiBPerSecond, statistics.medianMTokensPerSecond,
                statistics.medianNsPerToken);
    for (size_t index = 0; index < result.sampleNs.size(); ++index) {
        if (index != 0)
            std::putchar(',');
        std::printf("%llu", static_cast<unsigned long long>(result.sampleNs[index]));
    }
    std::printf("],\"checksum\":%llu}\n", static_cast<unsigned long long>(result.checksum));
}

} // namespace zith::bench
