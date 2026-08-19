#include "frontend/lexer/lexer.hpp"
#include "common/memory/source-map.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

using generated_lexer::Lexer;
using generated_lexer::Token;
using generated_lexer::TokenStream;

namespace {

struct Workload {
    const char *name;
    std::string source;
};

struct PreparedWorkload {
    common::memory::SourceMap sourceMap;
    common::memory::FileId fileId = 0;
    std::string_view source;
};

constexpr uint64_t kOffsetBasis = 14695981039346656037ULL;
constexpr uint64_t kPrime = 1099511628211ULL;
constexpr uint64_t kTargetSampleNs = 100'000'000ULL;

std::string repeat(std::string_view line, size_t size) {
    std::string out;
    out.reserve(size + line.size());
    const size_t desired = size - size % line.size();
    while (out.size() < desired)
        out.append(line);
    return out;
}

std::string realistic_source(size_t size) {
    constexpr std::string_view line =
        "fn main = (alpha, beta) -> { let x = 1; if x >= 2 { return alpha + beta; } }\n";
    return repeat(line, size);
}

std::string mixed_valid_source(size_t size) {
    constexpr std::string_view block = R"(/// Documentation for benchmark_item.
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
    return repeat(block, size);
}

std::string operator_source(size_t size) {
    constexpr std::string_view line =
        "+= -> <<= >>= <= >= != == && || := ... & | ^ ~ $ { } [ ] ; , .\n";
    return repeat(line, size);
}

std::string numeric_identifier_source(size_t size) {
    constexpr std::string_view line =
        "1_000_000_123 0xFF_A0 0b1010_0011 0c7_1 alpha beta gamma delta\n";
    return repeat(line, size);
}

std::string read_sysfs(std::string_view path) {
    const std::string filename(path);
    std::ifstream input(filename);
    std::string value;
    if (input)
        std::getline(input, value);
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
        value.pop_back();
    return value;
}

size_t logical_cpu_count() {
#ifdef _SC_NPROCESSORS_ONLN
    const long count = sysconf(_SC_NPROCESSORS_ONLN);
    if (count > 0)
        return static_cast<size_t>(count);
#endif
    return 1;
}

size_t physical_memory_bytes() {
    const std::string value = read_sysfs("/proc/meminfo");
    // /proc/meminfo begins with MemTotal: <kb> kB.
    constexpr std::string_view needle = "MemTotal:";
    const size_t pos = value.find(needle);
    if (pos == std::string::npos)
        return 0;
    const char *cursor = value.c_str() + pos + needle.size();
    while (*cursor == ' ' || *cursor == '\t')
        ++cursor;
    const size_t kb = static_cast<size_t>(std::strtoull(cursor, nullptr, 10));
    return kb * 1024;
}

std::string host_model() {
    const std::string cpuinfo = read_sysfs("/proc/cpuinfo");
    constexpr std::string_view marker = "model name";
    const size_t pos = cpuinfo.find(marker);
    if (pos == std::string::npos)
        return {};
    const size_t colon = cpuinfo.find(':', pos);
    if (colon == std::string::npos)
        return {};
    size_t begin = colon + 1;
    while (begin < cpuinfo.size() && (cpuinfo[begin] == ' ' || cpuinfo[begin] == '\t'))
        ++begin;
    size_t end = begin;
    while (end < cpuinfo.size() && cpuinfo[end] != '\n' && cpuinfo[end] != '\r')
        ++end;
    return cpuinfo.substr(begin, end - begin);
}

#if defined(__clang__)
std::string cxx_compiler() {
    return "clang " __clang_version__;
}
#elif defined(__GNUC__)
std::string cxx_compiler() {
    return "gcc " __VERSION__;
}
#else
std::string cxx_compiler() {
    return "unknown";
}
#endif

void print_host_header() {
    std::printf(
        "host: OS=%s CPU=%s cores=%zu mem=%zu",
        "linux",
        host_model().c_str(),
        logical_cpu_count(),
        physical_memory_bytes()
    );
    std::printf(" compiler=%s\n", cxx_compiler().c_str());
}

uint64_t checksumTokenStream(const TokenStream &stream) {
    uint64_t checksum = kOffsetBasis;
    for (uint32_t index = 0; index < stream.len; ++index) {
        const Token &token = stream.src[index];
        checksum ^= static_cast<uint64_t>(token.kind);
        checksum *= kPrime;
        checksum ^= token.span.start;
        checksum *= kPrime;
        checksum ^= token.span.end;
        checksum *= kPrime;
        checksum ^= static_cast<uint8_t>(token.punc);
        checksum *= kPrime;
    }
    return checksum;
}

double percentile(std::vector<double> values, double fraction) {
    if (values.empty())
        return 0;
    std::sort(values.begin(), values.end());
    const double position = fraction * static_cast<double>(values.size() - 1);
    const size_t lower = static_cast<size_t>(position);
    const size_t upper = lower + 1 < values.size() ? lower + 1 : lower;
    const double blend = position - static_cast<double>(lower);
    return values[lower] * (1.0 - blend) + values[upper] * blend;
}

uint64_t measureTokenizations(std::string_view source, uint32_t iterations,
                              Lexer &lexer, TokenStream &tokens,
                              uint64_t &checksum, size_t &tokenCount) {
    const auto started = std::chrono::steady_clock::now();
    for (uint32_t index = 0; index < iterations; ++index) {
        tokens = lexer.run(source);
        tokenCount = tokens.size();
        const uint64_t runChecksum = checksumTokenStream(tokens);
        checksum ^= runChecksum + 0x9e3779b97f4a7c15ULL +
                    (checksum << 6) + (checksum >> 2);
    }
    const auto finished = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        finished - started).count());
}

uint32_t calibratedIterations(uint64_t durationNs) {
    if (durationNs == 0)
        return 1;
    const uint64_t iterations =
        (kTargetSampleNs + durationNs - 1) / durationNs;
    return static_cast<uint32_t>(std::min<uint64_t>(
        iterations, static_cast<uint64_t>(UINT32_MAX)));
}

bool prepareWorkload(const Workload &workload, PreparedWorkload &prepared) {
    const auto added = prepared.sourceMap.addFile("bench.zith", workload.source);
    if (!added) {
        std::fprintf(stderr, "lexer-perf-smoke: %s\n", added.error().msg.c_str());
        return false;
    }
    prepared.fileId = added.value();
    const auto loc = prepared.sourceMap.get(prepared.fileId);
    if (!loc) {
        std::fprintf(stderr, "lexer-perf-smoke: source disappeared\n");
        return false;
    }
    prepared.source = loc->get().slice();
    return !prepared.source.empty();
}

bool print_workload(const Workload &workload, size_t samples) {
    PreparedWorkload prepared;
    if (!prepareWorkload(workload, prepared))
        return false;

    Lexer lexer;
    TokenStream tokens;
    uint64_t checksum = 0;
    size_t tokenCount = 0;

    constexpr uint32_t kWarmup = 3;
    for (uint32_t index = 0; index < kWarmup; ++index)
        (void)measureTokenizations(prepared.source, 1, lexer, tokens,
                                   checksum, tokenCount);

    const uint64_t calibrationNs =
        measureTokenizations(prepared.source, 1, lexer, tokens,
                             checksum, tokenCount);
    if (calibrationNs == 0 || tokenCount == 0) {
        std::fprintf(stderr, "lexer-perf-smoke: calibration failed\n");
        return false;
    }

    const uint32_t iterationsPerSample = calibratedIterations(calibrationNs);
    std::vector<double> nsPerRun;
    nsPerRun.reserve(samples);
    for (size_t index = 0; index < samples; ++index) {
        const uint64_t duration = measureTokenizations(
            prepared.source, iterationsPerSample, lexer, tokens,
            checksum, tokenCount);
        nsPerRun.push_back(static_cast<double>(duration) /
                           static_cast<double>(iterationsPerSample));
    }

    const double median = percentile(nsPerRun, 0.50);
    const double p95 = percentile(nsPerRun, 0.95);
    const double min_run = percentile(nsPerRun, 0.00);
    const double max_run = percentile(nsPerRun, 1.00);
    const double mib = static_cast<double>(workload.source.size()) /
                       (1024.0 * 1024.0);
    const double milliseconds = median / 1'000'000.0;
    std::printf(
        "%-9s bytes=%zu tokens=%zu samples=%zu x%u median=%8.3f ms "
        "min=%8.3f ms p95=%8.3f ms max=%8.3f ms  "
        "%8.2f MiB/s %10.2f Mtokens/s %9.2f ns/token checksum=%llu\n",
        workload.name,
        workload.source.size(),
        tokenCount,
        samples,
        iterationsPerSample,
        milliseconds,
        min_run / 1'000'000.0,
        p95 / 1'000'000.0,
        max_run / 1'000'000.0,
        mib / milliseconds * 1000.0,
        static_cast<double>(tokenCount) / milliseconds * 1e-3,
        static_cast<double>(tokenCount) == 0.0
            ? 0.0
            : median / static_cast<double>(tokenCount),
        static_cast<unsigned long long>(checksum)
    );
    return tokenCount != 0;
}

bool run_benchmark(size_t size, size_t samples) {
    const std::vector<Workload> workloads = {
        {"realistic", realistic_source(size)},
        {"mixed-valid", mixed_valid_source(size)},
        {"operators", operator_source(size)},
        {"numbers_id", numeric_identifier_source(size)},
    };
    print_host_header();
    bool ok = true;
    for (const Workload &workload : workloads)
        ok &= print_workload(workload, samples);
    return ok;
}

} // namespace

int main(int argc, char **argv) {
    bool quick = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--quick") == 0)
            quick = true;
        else if (std::strcmp(argv[i], "--stress") == 0)
            quick = false;
        else
            return EXIT_FAILURE;
    }

    const bool ok = quick
                        ? run_benchmark(1024 * 1024, 20)
                        : run_benchmark(4 * 1024 * 1024, 50);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
