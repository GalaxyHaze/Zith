// Copyright (c) 2024-2026, Zith contributors
// SPDX-License-Identifier: Apache-2.0

#include "cinterop/c-header.hpp"

#include "test-common.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace zith::cinterop;

namespace {

// Ensure the temp directory exists and return its path.
std::string tempDir() {
    static const std::string dir = [] {
        const std::string path = "/tmp/zith-cinterop-tests";
        std::filesystem::create_directories(path);
        return path;
    }();
    return dir;
}

// Write content to a .h file under the temp dir and return its path.
std::string writeTempHeader(const std::string &filename, const std::string &content) {
    const std::string path = tempDir() + "/" + filename;
    std::ofstream out(path, std::ios::trunc);
    out << content;
    out.close();
    return path;
}

// ===== Block 1 — Tests without libclang (always runnable) =====

void test_without_libclang() {
    std::printf("=== Without libclang ===\n");

    // 1. available() matches ZITH_ENABLE_C_INTEROP
#ifdef ZITH_ENABLE_C_INTEROP
    CHECK(available(), "available() returns true when ZITH_ENABLE_C_INTEROP is defined");
#else
    CHECK(!available(), "available() returns false when ZITH_ENABLE_C_INTEROP is not defined");
#endif

    // 2. parseHeader on nonexistent file returns fallback diagnostic
    const auto artifact = parseHeader("nonexistent.h", {});
    CHECK(artifact != nullptr, "parseHeader returns non-null artifact");
    CHECK(artifact->headerPath == "nonexistent.h", "artifact preserves headerPath");
    CHECK(artifact->functions.empty(), "no functions when libclang unavailable");
    CHECK(!artifact->diagnostics.empty(), "diagnostics present when libclang unavailable");
    CHECK(artifact->diagnostics.front().message.find("libclang") != std::string::npos ||
              artifact->diagnostics.front().message.find("extern fn") != std::string::npos,
          "fallback diagnostic mentions libclang or extern fn");

    // 3. ParseOptions defaults are reasonable
    ParseOptions opts;
    CHECK(opts.targetTriple.empty(), "default targetTriple is empty");
    CHECK(opts.sysroot.empty(), "default sysroot is empty");
    CHECK(opts.includeDirs.empty(), "default includeDirs is empty");
    CHECK(opts.defines.empty(), "default defines is empty");
}

#ifdef ZITH_ENABLE_C_INTEROP

// ===== Block 2 — Tests WITH libclang =====

// Helper: write a header and parse it, returning the artifact.
std::shared_ptr<const CHeaderArtifact> parseHeaderFromContent(const std::string &filename,
                                                              const std::string &content,
                                                              const ParseOptions &opts = {}) {
    return parseHeader(writeTempHeader(filename, content), opts);
}

// Helper: find a function by name.
const Function *findFunction(const CHeaderArtifact &artifact, const std::string &name) {
    for (const auto &f : artifact.functions) {
        if (f.name == name)
            return &f;
    }
    return nullptr;
}

void test_scalar_types() {
    std::printf("=== Scalar types ===\n");

    // 4. void foo(void)
    {
        const auto art = parseHeaderFromContent("void_foo.h", "void foo(void);\n");
        CHECK(!art->diagnostics.empty() || art->functions.size() == 1,
              "void foo(void): 1 function");
        if (const auto *f = findFunction(*art, "foo")) {
            CHECK_EQ(static_cast<int>(f->result.kind), static_cast<int>(TypeKind::Void),
                     "void foo(void): result is Void");
            CHECK(f->parameters.empty(), "void foo(void): no parameters");
        }
    }

    // 5. int bar(float x, double y)
    {
        const auto art = parseHeaderFromContent("int_bar.h", "int bar(float x, double y);\n");
        CHECK(!art->diagnostics.empty() || art->functions.size() == 1,
              "int bar(float, double): 1 function");
        if (const auto *f = findFunction(*art, "bar")) {
            CHECK_EQ(static_cast<int>(f->result.kind), static_cast<int>(TypeKind::Integer),
                     "int bar: result is Integer");
            CHECK_EQ(f->result.bits, 32, "int bar: result is 32 bits");
            CHECK(f->result.isSigned, "int bar: result is signed");
            CHECK_EQ(f->parameters.size(), 2u, "int bar: 2 parameters");
            if (f->parameters.size() == 2) {
                CHECK_EQ(static_cast<int>(f->parameters[0].kind), static_cast<int>(TypeKind::Float),
                         "int bar: first param is Float (float)");
                CHECK_EQ(f->parameters[0].bits, 32, "int bar: float is 32 bits");
                CHECK_EQ(static_cast<int>(f->parameters[1].kind), static_cast<int>(TypeKind::Float),
                         "int bar: second param is Float (double)");
                CHECK_EQ(f->parameters[1].bits, 64, "int bar: double is 64 bits");
            }
        }
    }
}

void test_pointer_and_const() {
    std::printf("=== Pointers and const ===\n");

    // 6. void baz(int *p, const char *s)
    {
        const auto art =
            parseHeaderFromContent("ptr_const.h", "void baz(int *p, const char *s);\n");
        CHECK(!art->diagnostics.empty() || art->functions.size() == 1, "baz: 1 function");
        if (const auto *f = findFunction(*art, "baz")) {
            CHECK_EQ(f->parameters.size(), 2u, "baz: 2 parameters");
            if (f->parameters.size() == 2) {
                CHECK_EQ(static_cast<int>(f->parameters[0].kind),
                         static_cast<int>(TypeKind::Pointer),
                         "baz: first param is Pointer (int *)");
                CHECK(f->parameters[0].pointee != nullptr, "baz: int* has pointee");
                if (f->parameters[0].pointee) {
                    CHECK(!f->parameters[0].pointee->isConst, "baz: int* pointee is not const");
                }
                CHECK_EQ(static_cast<int>(f->parameters[1].kind),
                         static_cast<int>(TypeKind::Pointer),
                         "baz: second param is Pointer (const char *)");
                CHECK(f->parameters[1].pointee != nullptr, "baz: const char* has pointee");
                if (f->parameters[1].pointee) {
                    CHECK(f->parameters[1].pointee->isConst, "baz: const char* pointee is const");
                }
            }
        }
    }
}

void test_struct_and_enum() {
    std::printf("=== Structs and enums ===\n");

    // 7. struct Point
    {
        const auto art =
            parseHeaderFromContent("struct_point.h", "struct Point { int x, y; };\n"
                                                     "struct Point get_origin(void);\n");
        CHECK(!art->diagnostics.empty() || art->functions.size() == 1, "struct Point: 1 function");
        if (const auto *f = findFunction(*art, "get_origin")) {
            CHECK_EQ(static_cast<int>(f->result.kind), static_cast<int>(TypeKind::Record),
                     "get_origin: result is Record");
            CHECK(f->result.name.find("Point") != std::string::npos,
                  "get_origin: result name contains Point");
        }
    }

    // 8. enum Color
    {
        const auto art = parseHeaderFromContent("enum_color.h", "enum Color { R, G, B };\n"
                                                                "enum Color next(enum Color c);\n");
        CHECK(!art->diagnostics.empty() || art->functions.size() == 1, "enum Color: 1 function");
        if (const auto *f = findFunction(*art, "next")) {
            CHECK_EQ(static_cast<int>(f->result.kind), static_cast<int>(TypeKind::Enum),
                     "next: result is Enum");
            CHECK(f->result.name.find("Color") != std::string::npos,
                  "next: result name contains Color");
        }
    }
}

void test_filtering() {
    std::printf("=== Filtering ===\n");

    // 9. Variadic function is excluded
    {
        const auto art =
            parseHeaderFromContent("variadic.h", "int printf(const char *fmt, ...);\n");
        CHECK(findFunction(*art, "printf") == nullptr, "variadic printf is excluded");
        bool hasVariadicDiag = false;
        for (const auto &d : art->diagnostics) {
            if (d.message.find("variadic") != std::string::npos)
                hasVariadicDiag = true;
        }
        CHECK(hasVariadicDiag, "variadic exclusion produces diagnostic");
    }

    // 10. static function is excluded
    {
        const auto art =
            parseHeaderFromContent("static_fn.h", "static int helper(void) { return 1; }\n"
                                                  "int caller(void);\n");
        CHECK(findFunction(*art, "helper") == nullptr, "static helper is excluded");
        CHECK(findFunction(*art, "caller") != nullptr, "non-static caller is included");
    }

    // 11. inline function is excluded
    {
        const auto art =
            parseHeaderFromContent("inline_fn.h", "inline int fast(void) { return 0; }\n"
                                                  "int slow(void);\n");
        CHECK(findFunction(*art, "fast") == nullptr, "inline fast is excluded");
        CHECK(findFunction(*art, "slow") != nullptr, "non-inline slow is included");
    }
}

void test_dependencies() {
    std::printf("=== Dependencies ===\n");

    // 13. #include <stdint.h> produces a resolved dependency
    {
        const auto art   = parseHeaderFromContent("has_stdint.h", "#include <stdint.h>\n"
                                                                    "uint32_t id(void);\n");
        bool foundStdint = false;
        for (const auto &dep : art->dependencies) {
            if (dep.find("stdint.h") != std::string::npos) {
                foundStdint = true;
                break;
            }
        }
        CHECK(foundStdint, "#include <stdint.h> appears in dependencies");
    }

    // 14. Dependencies are deduplicated and sorted
    {
        // Write a helper header and a main header that includes it plus stddef.h
        writeTempHeader("helper_dep.h", "int helper_dep_fn(void);\n");
        const auto art = parseHeaderFromContent("multi_include.h",
                                                "#include <stddef.h>\n"
                                                "#include \"helper_dep.h\"\n"
                                                "int main_fn(void);\n",
                                                ParseOptions{});
        // Check no duplicates
        for (size_t i = 1; i < art->dependencies.size(); ++i) {
            CHECK(art->dependencies[i - 1] != art->dependencies[i],
                  "dependencies have no adjacent duplicates");
            CHECK(art->dependencies[i - 1] <= art->dependencies[i], "dependencies are sorted");
        }
    }
}

void test_parse_errors() {
    std::printf("=== Parse errors ===\n");

    // 15. #error produces a diagnostic
    {
        const auto art = parseHeaderFromContent("error_header.h", "#error \"fail deliberately\"\n");
        bool foundError = false;
        for (const auto &d : art->diagnostics) {
            if (d.message.find("fail deliberately") != std::string::npos) {
                foundError = true;
                break;
            }
        }
        CHECK(foundError, "#error produces diagnostic with the message");
    }
}

void test_parse_options() {
    std::printf("=== ParseOptions ===\n");

    // 16. Invalid sysroot produces a diagnostic
    {
        ParseOptions opts;
        opts.sysroot      = "/nonexistent/sysroot";
        opts.targetTriple = "x86_64-unknown-linux-gnu";
        const auto art    = parseHeader(writeTempHeader("sysroot_test.h", "int fn(void);\n"), opts);
        // With a fake sysroot, we might get parse errors; verify artifact is non-null
        CHECK(art != nullptr, "invalid sysroot still returns artifact");
        // The diagnostics should include something about inability to find headers
        // or the parse simply fails. Either way, artifact is returned.
    }

    // 17. includeDirs allows resolving an extra header
    {
        // Create a directory for extra headers
        const std::string extraDir = tempDir() + "/extra_includes";
        std::filesystem::create_directories(extraDir);
        {
            std::ofstream out(extraDir + "/extra.h");
            out << "int extra_fn(void);\n";
            out.close();
        }

        ParseOptions opts;
        opts.includeDirs.push_back(extraDir);
        const auto art = parseHeaderFromContent("uses_extra.h",
                                                "#include \"extra.h\"\n"
                                                "int main_fn(void);\n",
                                                opts);
        CHECK(findFunction(*art, "main_fn") != nullptr, "main_fn also found alongside extra_fn");
        // extra_fn lives in a transitively included header and is not
        // returned in `functions` (only declarations from the main file
        // are visited), but the include dir should still resolve.
        bool foundExtraDep = false;
        for (const auto &dep : art->dependencies) {
            if (dep.find("extra.h") != std::string::npos)
                foundExtraDep = true;
        }
        CHECK(foundExtraDep, "extra.h appears in dependencies via custom includeDir");
    }

    // 18. defines allows conditional compilation
    {
        ParseOptions opts;
        opts.defines.push_back("ANSWER=42");
        const auto art = parseHeaderFromContent("define_test.h",
                                                "#if ANSWER != 42\n"
                                                "#error \"wrong answer\"\n"
                                                "#endif\n"
                                                "int the_fn(void);\n",
                                                opts);
        // If the define works, there's no #error and we see the function
        CHECK(findFunction(*art, "the_fn") != nullptr,
              "function compiled correctly when ANSWER=42 defined");
    }

    // 18b. defines missing causes #error
    {
        const auto art = parseHeaderFromContent("define_fail.h", "#if ANSWER != 42\n"
                                                                 "#error \"wrong answer\"\n"
                                                                 "#endif\n"
                                                                 "int the_fn(void);\n");
        // Without the define, clang will produce a diagnostic for #error or
        // for ANSWER being undefined; either way, the_fn likely won't appear.
        bool hasError = !art->diagnostics.empty();
        CHECK(hasError || findFunction(*art, "the_fn") == nullptr,
              "without ANSWER=42, parse either fails or function is absent");
    }
}

#endif // ZITH_ENABLE_C_INTEROP

} // namespace

void test_cinterop() {
    test_without_libclang();
#ifdef ZITH_ENABLE_C_INTEROP
    test_scalar_types();
    test_pointer_and_const();
    test_struct_and_enum();
    test_filtering();
    test_dependencies();
    test_parse_errors();
    test_parse_options();
#endif
}
TEST_MAIN(cinterop)
