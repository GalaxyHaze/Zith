#include "test-common.hpp"

#include <cstddef>
#include <string>
#include <string_view>

extern "C" int zith_run_source(const char *ptr, int len);
extern "C" int zith_compile_hir(const char *ptr, int len);
extern "C" int zith_execute_cached(const char *ptr, int len);
extern "C" int zith_restore_cached(const char *ptr, int len, const char *blob_ptr, int blob_len);
extern "C" long long zith_hir_cache_hits();
extern "C" long long zith_hir_cache_misses();
extern "C" long long zith_hir_cache_stale();
extern "C" long long zith_hir_cache_size();
extern "C" long long zith_exit_code();
extern "C" const char *zith_last_output_ptr();
extern "C" int zith_last_output_len();
extern "C" const char *zith_last_diagnostics_json_ptr();
extern "C" int zith_last_diagnostics_json_len();

namespace {

std::string host_output;
std::string host_errors;
std::string host_input;
std::size_t host_input_offset = 0;
bool host_input_cancelled = false;

auto diagnosticsJson() -> std::string {
    const char *ptr = zith_last_diagnostics_json_ptr();
    const int len   = zith_last_diagnostics_json_len();
    if (ptr == nullptr || len < 0)
        return {};
    return std::string(ptr, static_cast<std::size_t>(len));
}

auto countOccurrences(std::string_view text, std::string_view needle) -> std::size_t {
    std::size_t count = 0;
    std::size_t start = 0;
    while ((start = text.find(needle, start)) != std::string_view::npos) {
        ++count;
        start += needle.size();
    }
    return count;
}

auto runSource(std::string_view source) -> int {
    return zith_run_source(source.data(), static_cast<int>(source.size()));
}

constexpr std::string_view kCacheProgram = "extern fn puts(msg: *char)\n"
                                           "\n"
                                           "fn main(): i32 {\n"
                                           "    _ = puts(\"cached-hir\");\n"
                                           "    11\n"
                                           "}\n";

} // namespace

extern "C" void host_write(int stream, const char *ptr, int len) {
    if (ptr == nullptr || len <= 0)
        return;
    auto &output = stream == 1 ? host_output : host_errors;
    output.append(ptr, static_cast<std::size_t>(len));
}

extern "C" int host_read_byte() {
    if (host_input_cancelled)
        return -2;
    if (host_input_offset >= host_input.size())
        return -1;
    return static_cast<unsigned char>(host_input[host_input_offset++]);
}

void test_wasm_playground_structured_diagnostics() {
    const auto validStatus = runSource("fn main(): i32 {\n    42\n}\n");
    CHECK_EQ(validStatus, 0, "valid program keeps the existing success status");
    CHECK_EQ(diagnosticsJson(), std::string("{\"diagnostics\":[]}"),
             "successful calls expose an empty diagnostics array");

    constexpr std::string_view invalidSource = "fn main() {\n    let x: MissingType = 1;\n}\n";
    host_errors.clear();
    const auto compileStatus = runSource(invalidSource);
    CHECK_EQ(compileStatus, 1, "compile failure keeps the existing status code");
    const auto compileJson = diagnosticsJson();
    CHECK(compileJson.find("\"diagnostics\":[{") != std::string::npos,
          "compile diagnostics are represented in the JSON array");
    CHECK_EQ(countOccurrences(compileJson, "\"severity\":\"error\""), 1u,
             "one compile failure is emitted once in structured diagnostics");
    CHECK_EQ(countOccurrences(host_errors, "error: "), 1u,
             "one compile failure is emitted once through host stderr");
    CHECK(compileJson.find("\"severity\":\"error\"") != std::string::npos,
          "compile diagnostic includes severity");
    CHECK(compileJson.find("\"message\":") != std::string::npos,
          "compile diagnostic includes message");
    CHECK(compileJson.find("\"code\":") != std::string::npos,
          "compile diagnostic includes its numeric code");
    CHECK(compileJson.find("\"span\":{\"start\":") != std::string::npos &&
              compileJson.find("\"end\":") != std::string::npos,
          "compile diagnostic includes its source byte span");

    host_errors.clear();
    CHECK_EQ(runSource(invalidSource), 1, "repeated compile failure keeps its status");
    CHECK_EQ(diagnosticsJson(), compileJson,
             "repeated compilation returns the same diagnostics without accumulating entries");
    CHECK_EQ(countOccurrences(host_errors, "error: "), 1u,
             "repeated compilation writes the diagnostic once to host stderr");

    host_errors.clear();
    const auto missingImportStatus = runSource("from missing/module\nfn main() { }\n");
    CHECK_EQ(missingImportStatus, 1, "missing from-import reports a compile failure");
    const auto missingImportJson = diagnosticsJson();
    CHECK_EQ(countOccurrences(missingImportJson, "\"severity\":\"error\""), 1u,
             "a missing import is reported once in the playground");
    CHECK_EQ(countOccurrences(host_errors, "error: "), 1u,
             "a missing import is written once through host stderr");
    CHECK(missingImportJson.find("could not find module 'missing/module'") != std::string::npos,
          "playground exposes the actionable missing-module diagnostic");

    // Both malformed import prefixes must reach the dedicated E1013 message
    // through the playground, without the generic 'expected an import path'
    // cascade.
    for (const std::string_view malformedImport : {
             std::string_view("import from graphics { draw as paint }\nfn main() { }\n"),
             std::string_view("import / from graphics { draw as paint }\nfn main() { }\n"),
         }) {
        host_errors.clear();
        const auto malformedStatus = runSource(malformedImport);
        CHECK_EQ(malformedStatus, 1, "a malformed import prefix reports a compile failure");
        const auto malformedJson = diagnosticsJson();
        CHECK_EQ(countOccurrences(malformedJson, "\"code\":1013"), 1u,
                 "a malformed import prefix has one structured E1013 diagnostic");
        CHECK(malformedJson.find("expected an import path") == std::string::npos,
              "a malformed import prefix does not cascade the generic E0001 message");
        CHECK(malformedJson.find("use 'from path'") != std::string::npos,
              "the malformed import diagnostic suggests 'from path'");
        CHECK_EQ(countOccurrences(host_errors, "error: "), 1u,
                 "a malformed import prefix writes the actionable diagnostic once");
    }

    constexpr std::string_view missingControlBody = "fn main() {\n"
                                                    "    if (true) 42;\n"
                                                    "}\n";
    host_errors.clear();
    const auto controlBodyStatus = runSource(missingControlBody);
    CHECK_EQ(controlBodyStatus, 1, "a control statement without a block fails compilation");
    const auto controlBodyJson = diagnosticsJson();
    CHECK_EQ(countOccurrences(controlBodyJson, "\"code\":1012"), 1u,
             "a missing control body has one structured E1012 diagnostic");
    CHECK(controlBodyJson.find("if (condition) { ... }") != std::string::npos,
          "E1012 suggests the required if block syntax in the playground");
    CHECK_EQ(countOccurrences(host_errors, "error: "), 1u,
             "a missing control body is written once through host stderr");

    host_errors.clear();
    CHECK_EQ(runSource(missingControlBody), 1,
             "repeated missing-control-body compilation keeps its status");
    CHECK_EQ(diagnosticsJson(), controlBodyJson,
             "repeated missing-control-body compilation does not accumulate diagnostics");
    CHECK_EQ(countOccurrences(host_errors, "error: "), 1u,
             "repeated missing-control-body compilation writes the diagnostic once");

    CHECK_EQ(runSource("fn main(): i32 {\n    42\n}\n"), 0,
             "a valid call after a failed call succeeds");
    CHECK_EQ(diagnosticsJson(), std::string("{\"diagnostics\":[]}"),
             "a valid call after a failed call clears prior diagnostics");

    const auto trapStatus = runSource("fn main(): i32 {\n"
                                      "    let values: [1]i32 = [7];\n"
                                      "    let index: i32 = 2;\n"
                                      "    raw values[index]\n"
                                      "}\n");
    CHECK_EQ(trapStatus, 3, "runtime trap keeps the existing status code");
    const auto trapJson = diagnosticsJson();
    CHECK(trapJson.find("\"severity\":\"error\"") != std::string::npos &&
              trapJson.find("\"message\":") != std::string::npos,
          "runtime trap includes an error severity and message");

    const auto unsupportedStatus = runSource("extern var external_value: i32;\n"
                                             "fn main(): i32 {\n"
                                             "    return external_value;\n"
                                             "}\n");
    CHECK_EQ(unsupportedStatus, 5, "unsupported construct keeps the existing status code");
    const auto unsupportedJson = diagnosticsJson();
    CHECK(unsupportedJson.find("\"severity\":\"error\"") != std::string::npos &&
              unsupportedJson.find("\"message\":") != std::string::npos,
          "unsupported construct includes an error severity and message");
    CHECK(unsupportedJson.find("unsupported external global 'external_value' in v2 VM") !=
              std::string::npos,
          "unsupported external global diagnostic is exposed through the WASM ABI");
}

void test_wasm_playground_hir_cache() {
    const auto source = kCacheProgram;
    const auto *ptr   = source.data();
    const auto len    = static_cast<int>(source.size());

    const auto hitsBefore   = zith_hir_cache_hits();
    const auto missesBefore = zith_hir_cache_misses();

    CHECK_EQ(zith_compile_hir(ptr, len), 0, "compile_hir stores an artifact");
    CHECK(zith_hir_cache_size() >= 1, "compile_hir populates the artifact cache");

    // A fresh compile+execute is the reference behavior.
    host_output.clear();
    const auto referenceStatus = zith_run_source(ptr, len);
    const auto referenceOutput = host_output;
    const auto referenceExit   = zith_exit_code();
    CHECK_EQ(referenceStatus, 0, "reference run succeeds");
    CHECK_EQ(referenceOutput, std::string("cached-hir\n"),
             "reference run writes the program output");

    // Two replays from the cache must match the reference and skip compilation.
    for (int replay = 0; replay < 2; ++replay) {
        host_output.clear();
        const auto replayStatus = zith_execute_cached(ptr, len);
        CHECK_EQ(replayStatus, 0, "cached replay succeeds");
        CHECK_EQ(host_output, referenceOutput, "cached replay reproduces stdout");
        CHECK_EQ(zith_exit_code(), referenceExit, "cached replay reproduces the exit code");
    }
    CHECK_EQ(zith_hir_cache_hits() - hitsBefore, 2, "both replays hit the cache");
    CHECK_EQ(zith_hir_cache_misses() - missesBefore, 0, "no cache miss during replay");

    // A miss is reported through the structured diagnostics channel.
    const std::string_view missing = "fn main(): i32 {\n    1\n}\n";
    const auto missStatus = zith_execute_cached(missing.data(), static_cast<int>(missing.size()));
    CHECK_EQ(missStatus, 1, "cache miss reports a compile failure status");
    CHECK(diagnosticsJson().find("HIR artifact cache miss") != std::string::npos,
          "cache miss is reported through the structured diagnostics channel");

    // A restored blob that no longer decodes is reported as stale, not as a
    // silent recompile.
    const std::string_view garbage     = "not-a-flat-hir-blob";
    const std::string_view staleSource = "fn main(): i32 {\n    2\n}\n";
    const auto staleBefore             = zith_hir_cache_stale();
    CHECK_EQ(zith_restore_cached(staleSource.data(), static_cast<int>(staleSource.size()),
                                 garbage.data(), static_cast<int>(garbage.size())),
             0, "restore_cached accepts a persisted blob");
    const auto staleStatus =
        zith_execute_cached(staleSource.data(), static_cast<int>(staleSource.size()));
    CHECK_EQ(staleStatus, 1, "stale artifact reports a compile failure status");
    CHECK_EQ(zith_hir_cache_stale() - staleBefore, 1, "stale replay increments the stale counter");
    CHECK(diagnosticsJson().find("stale HIR artifact") != std::string::npos,
          "stale artifact is reported through the structured diagnostics channel");
}

void test_wasm_playground_interactive_input() {
    host_output.clear();
    host_errors.clear();
    host_input = "A\n";
    host_input_offset = 0;
    host_input_cancelled = false;
    constexpr std::string_view source =
        "extern fn puts(msg: *char)\n"
        "extern fn getchar(): i32\n"
        "\n"
        "fn main(): i32 {\n"
        "    _ = puts(\"prompt\");\n"
        "    getchar()\n"
        "}\n";
    CHECK_EQ(runSource(source), 0, "playground forwards bytes to the VM input callback");
    CHECK_EQ(host_output, std::string("prompt\n"),
             "playground streams output before the VM reads input");
    CHECK_EQ(zith_exit_code(), 65, "playground guest receives the submitted byte");
    CHECK_EQ(std::string(zith_last_output_ptr(), static_cast<std::size_t>(zith_last_output_len())),
             std::string("prompt\n"), "playground retains output accumulated by a guest run");

    host_output.clear();
    host_input_cancelled = true;
    CHECK_EQ(runSource(source), 6, "playground cancellation has a distinct status");
    CHECK_EQ(host_output, std::string("prompt\n"),
             "playground streams output before returning cancellation");
    CHECK_EQ(std::string(zith_last_output_ptr(), static_cast<std::size_t>(zith_last_output_len())),
             std::string("prompt\n"), "playground retains output accumulated before cancellation");
    CHECK_EQ(diagnosticsJson(), std::string("{\"diagnostics\":[]}"),
             "cancellation is not reported as a compiler/runtime diagnostic");
}

void test_wasm_playground() {
    test_wasm_playground_structured_diagnostics();
    test_wasm_playground_hir_cache();
    test_wasm_playground_interactive_input();
}

TEST_MAIN(wasm_playground)
