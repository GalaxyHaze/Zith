#include "lexer-benchmark.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace {

enum class OutputFormat : uint8_t {
    Text,
    Json,
};

struct CommandLine {
    zith::bench::LexerBenchmarkOptions options;
    OutputFormat format       = OutputFormat::Text;
    std::string_view scenario = "mixed-valid";
};

void printUsage(const char *program) {
    std::printf("Usage: %s [--format text|json] [--warmup N] [--samples N] "
                "[--scenario mixed-valid]\n",
                program);
}

bool parseUnsigned(std::string_view text, uint32_t &value) {
    const char *begin = text.data();
    const char *end   = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool parseCommandLine(int argc, char **argv, CommandLine &commandLine, std::string &error) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h") {
            printUsage(argv[0]);
            return false;
        }
        if (index + 1 == argc) {
            error = "missing value for " + std::string(argument);
            return false;
        }

        const std::string_view value(argv[++index]);
        if (argument == "--format") {
            if (value == "text") {
                commandLine.format = OutputFormat::Text;
            } else if (value == "json") {
                commandLine.format = OutputFormat::Json;
            } else {
                error = "--format must be text or json";
                return false;
            }
        } else if (argument == "--warmup") {
            if (!parseUnsigned(value, commandLine.options.warmup)) {
                error = "--warmup must be an unsigned integer";
                return false;
            }
        } else if (argument == "--samples") {
            if (!parseUnsigned(value, commandLine.options.samples) ||
                commandLine.options.samples == 0) {
                error = "--samples must be greater than zero";
                return false;
            }
        } else if (argument == "--scenario") {
            if (value != "mixed-valid") {
                error = "--scenario must be mixed-valid";
                return false;
            }
            commandLine.scenario = value;
        } else {
            error = "unknown option " + std::string(argument);
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char **argv) {
    CommandLine commandLine;
    std::string error;
    if (!parseCommandLine(argc, argv, commandLine, error)) {
        if (!error.empty()) {
            std::fprintf(stderr, "bench-lexer: %s\n", error.c_str());
            printUsage(argv[0]);
            return 2;
        }
        return 0;
    }

    zith::bench::LexerScenario scenario = zith::bench::makeLexerScenario(commandLine.scenario);
    zith::bench::PreparedLexerScenario prepared(std::move(scenario));
    if (!zith::bench::prepareLexerScenario(prepared, error)) {
        std::fprintf(stderr, "bench-lexer: %s\n", error.c_str());
        return 1;
    }

    zith::bench::LexerBenchmarkResult result;
    if (!zith::bench::runLexerBenchmark(prepared, commandLine.options, result, error)) {
        std::fprintf(stderr, "bench-lexer: %s\n", error.c_str());
        return 1;
    }
    const auto statistics = zith::bench::calculateLexerBenchmarkStatistics(result);
    const auto host       = zith::bench::queryHostInfo();
    if (commandLine.format == OutputFormat::Json) {
        zith::bench::printLexerBenchmarkJson(prepared, commandLine.options, result, statistics,
                                             host);
    } else {
        zith::bench::printLexerBenchmarkText(prepared, commandLine.options, result, statistics,
                                             host);
    }
    return 0;
}
