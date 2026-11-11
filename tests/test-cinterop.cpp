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

void test_system_include_dirs() {
    std::printf("=== systemIncludeDirs ===\n");

    const auto dirs = systemIncludeDirs("", "");
    CHECK(!dirs.empty(), "systemIncludeDirs returns at least one directory");
    bool all_exist = true;
    for (const auto &dir : dirs) {
        std::error_code error;
        if (!std::filesystem::is_directory(dir, error))
            all_exist = false;
    }
    CHECK(all_exist, "every reported system include directory exists");

    const auto again = systemIncludeDirs("", "");
    CHECK(again == dirs, "repeated calls return the memoised result");

#ifdef ZITH_ENABLE_C_INTEROP
    bool has_stdint = false;
    for (const auto &dir : dirs) {
        std::error_code error;
        if (std::filesystem::exists(std::filesystem::path(dir) / "stdint.h", error))
            has_stdint = true;
    }
    CHECK(has_stdint, "a discovered directory contains stdint.h");
#endif
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

const Constant *findConstant(const CHeaderArtifact &artifact, const std::string &name) {
    for (const auto &c : artifact.constants)
        if (c.name == name)
            return &c;
    return nullptr;
}

const Global *findGlobal(const CHeaderArtifact &artifact, const std::string &name) {
    for (const auto &global : artifact.globals)
        if (global.name == name)
            return &global;
    return nullptr;
}

const Typedef *findTypedef(const CHeaderArtifact &artifact, const std::string &name) {
    for (const auto &alias : artifact.typedefs)
        if (alias.name == name)
            return &alias;
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

void test_decayed_and_special_parameter_types() {
    std::printf("=== Decayed and special parameter types ===\n");

    // 6b. Array parameters import as the pointer types carried by the function type.
    {
        const auto art = parseHeaderFromContent("decay_arrays.h", "void f(char buf[20]);\n"
                                                                  "void g(int m[3][4]);\n");
        CHECK(art->diagnostics.empty(), "decayed array parameters import without diagnostics");
        CHECK_EQ(art->functions.size(), 2u, "both array-parameter functions are imported");
        if (const auto *f = findFunction(*art, "f")) {
            CHECK_EQ(f->parameters.size(), 1u, "f keeps its single parameter");
            if (!f->parameters.empty()) {
                CHECK_EQ(static_cast<int>(f->parameters[0].kind),
                         static_cast<int>(TypeKind::Pointer), "char buf[20] decays to Pointer");
            }
        }
        if (const auto *g = findFunction(*art, "g")) {
            CHECK_EQ(g->parameters.size(), 1u, "g keeps its single parameter");
            if (!g->parameters.empty()) {
                CHECK_EQ(static_cast<int>(g->parameters[0].kind),
                         static_cast<int>(TypeKind::Pointer), "int m[3][4] decays to Pointer");
            }
        }
    }

    // 6c. va_list parameters import once libclang carries the decayed type.
    {
        const auto art = parseHeaderFromContent("va_list_param.h",
                                                "#include <stdarg.h>\n"
                                                "int vprintf(const char *fmt, va_list ap);\n");
        CHECK(art->diagnostics.empty(), "va_list parameter imports without diagnostics");
        if (const auto *f = findFunction(*art, "vprintf")) {
            CHECK_EQ(f->parameters.size(), 2u, "vprintf keeps both parameters");
        }
    }

    // 6d. Reject function-pointer parameters until the imported type preserves callability.
    {
        const auto art = parseHeaderFromContent(
            "callback_param.h", "#include <stddef.h>\n"
                                "void qsort(void *base, size_t nmemb, size_t size,\n"
                                "           int (*compar)(const void *, const void *));\n"
                                "int (*get_callback(void))(int);\n");
        CHECK(art->diagnostics.empty(), "an unsupported callback does not fail sibling imports");
        CHECK(findFunction(*art, "qsort") == nullptr,
              "a declaration with a callback parameter is not imported");
        CHECK(findFunction(*art, "get_callback") == nullptr,
              "a declaration with a callback result is not imported");
        bool found_parameter_skip = false;
        bool found_result_skip    = false;
        for (const auto &skipped : art->skippedFunctions)
            if (skipped.find("function pointer") != std::string::npos) {
                found_parameter_skip =
                    found_parameter_skip || skipped.find("qsort") != std::string::npos;
                found_result_skip =
                    found_result_skip || skipped.find("get_callback") != std::string::npos;
            }
        CHECK(found_parameter_skip,
              "the skipped callback parameter declaration records its unsupported type");
        CHECK(found_result_skip,
              "the skipped callback result declaration records its unsupported type");
    }
}

void test_skips_and_dedupe() {
    std::printf("=== Skips and dedupe ===\n");

    // 6e. One unsupported decl is skipped silently; siblings still import.
    {
        const auto art = parseHeaderFromContent("mixed_support.h", "int take(_Atomic(int) box);\n"
                                                                   "int keep(int x);\n");
        CHECK(art->diagnostics.empty(), "unsupported decl does not fail the import");
        CHECK_EQ(art->functions.size(), 1u, "only the supported declaration is imported");
        CHECK(findFunction(*art, "keep") != nullptr, "the supported sibling is present");
        CHECK(findFunction(*art, "take") == nullptr, "the unsupported decl is absent");
        bool found_skip = false;
        for (const auto &skipped : art->skippedFunctions)
            found_skip = found_skip || skipped.find("take") != std::string::npos;
        CHECK(found_skip, "skippedFunctions records take and its reason");
    }

    // 6f. Same-name glibc-style duplicates collapse to one binding.
    {
        const auto art = parseHeaderFromContent("duplicate_names.h",
                                                "int dup_fn(int) __asm__(\"first_linkage\");\n"
                                                "int dup_fn(int) __asm__(\"second_linkage\");\n");
        bool counted   = false;
        for (const auto &f : art->functions) {
            if (f.name == "dup_fn")
                counted = counted || f.linkageName == "first_linkage";
        }
        CHECK_EQ(art->functions.size(), 1u, "duplicate C names collapse to one function");
        CHECK(counted, "the first declaration's linkage name wins");
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

    // 7a. Simple records passed/returned by value are validated for both ABI targets.
    for (const auto *triple : {"x86_64-unknown-linux-gnu", "aarch64-unknown-linux-gnu"}) {
        ParseOptions opts;
        opts.targetTriple = triple;
        const auto art    = parseHeaderFromContent(
            "validated_records.h",
            "struct Point { int x, y; };\n"
            "int classify(struct Point p);\n"
            "struct Point get_point(void);\n"
            "struct Inner { int v; };\n"
            "struct Outer { struct Inner inner; int extra; };\n"
            "int classify_outer(struct Outer o);\n"
            "struct Outer get_outer(void);\n"
            "struct FloatPair { float left, right; };\n"
            "float sum_float_pair(struct FloatPair value);\n"
            "struct FloatPair make_float_pair(float left, float right);\n"
            "struct Pair64 { long long lo, hi; };\n"
            "int sum_pair64(struct Pair64 p);\n"
            "struct Pair64 make_pair64(long long lo, long long hi);\n"
            "struct Triple64 { long long first, second, third; };\n"
            "long long sum_triple64(struct Triple64 value);\n"
            "struct Triple64 make_triple64(long long first, long long second, long long third);\n"
            "struct Mixed32_64 { int small; long long large; };\n"
            "long long sum_mixed32_64(struct Mixed32_64 value);\n"
            "struct Mixed32_64 make_mixed32_64(int small, long long large);\n"
            "struct Mixed64_32 { long long large; int small; };\n"
            "long long sum_mixed64_32(struct Mixed64_32 value);\n"
            "struct Mixed64_32 make_mixed64_32(long long large, int small);\n",
            opts);
        const std::string simple_no_diag =
            std::string("simple record parse has no diagnostics: ") + triple;
        CHECK_EQ(art->diagnostics.size(), 0u, simple_no_diag.c_str());
        const auto check_adapter = [&](const char *function_name) {
            const auto *function = findFunction(*art, function_name);
            const std::string found =
                std::string(function_name) + " imports with a generated adapter on " + triple;
            CHECK(function != nullptr, found.c_str());
            if (function == nullptr)
                return;
            const std::string adapter =
                std::string(function_name) + " has a nonempty generated adapter on " + triple;
            const std::string call =
                std::string(function_name) + " adapter calls the C declaration on " + triple;
            CHECK(!function->adapterName.empty(), adapter.c_str());
            CHECK(function->adapterSource.find(function->adapterName) != std::string::npos &&
                      function->adapterSource.find(std::string(function_name) + "(") !=
                          std::string::npos,
                  call.c_str());
        };
        check_adapter("classify_outer");
        check_adapter("get_outer");
        check_adapter("sum_float_pair");
        check_adapter("make_float_pair");
        check_adapter("sum_triple64");
        check_adapter("make_triple64");
        if (const auto *f = findFunction(*art, "classify")) {
            const std::string classify_param =
                std::string("classify keeps its by-value parameter: ") + triple;
            CHECK_EQ(f->parameters.size(), 1u, classify_param.c_str());
            if (!f->parameters.empty()) {
                const auto &point = f->parameters[0];
                const std::string point_verified =
                    std::string("classify's Point layout is verified: ") + triple;
                const std::string point_size  = std::string("Point is 8 bytes: ") + triple;
                const std::string point_align = std::string("Point aligns to 4: ") + triple;
                const std::string point_fields =
                    std::string("Point has two validated fields: ") + triple;
                CHECK(point.hasVerifiedLayout, point_verified.c_str());
                CHECK_EQ(point.sizeBytes, 8u, point_size.c_str());
                CHECK_EQ(point.alignBytes, 4u, point_align.c_str());
                CHECK_EQ(point.recordFields.size(), 2u, point_fields.c_str());
            }
        } else {
            const std::string classify_imports = std::string("classify imports on ") + triple;
            CHECK(false, classify_imports.c_str());
        }
        if (const auto *f = findFunction(*art, "get_point")) {
            const std::string result_verified =
                std::string("get_point's Point result layout is verified: ") + triple;
            const std::string result_fields =
                std::string("returned Point has two validated fields: ") + triple;
            CHECK(f->result.hasVerifiedLayout, result_verified.c_str());
            CHECK_EQ(f->result.recordFields.size(), 2u, result_fields.c_str());
        } else {
            const std::string get_point_imports = std::string("get_point imports on ") + triple;
            CHECK(false, get_point_imports.c_str());
        }
        if (const auto *f = findFunction(*art, "classify_outer")) {
            const std::string outer_verified =
                std::string("nested Outer layout is verified: ") + triple;
            CHECK(f->parameters.size() == 1u && f->parameters[0].hasVerifiedLayout,
                  outer_verified.c_str());
        } else {
            const std::string outer_imports = std::string("classify_outer imports on ") + triple;
            CHECK(false, outer_imports.c_str());
        }
        if (const auto *f = findFunction(*art, "sum_float_pair")) {
            const std::string float_pair_layout =
                std::string("FloatPair HFA layout is verified: ") + triple;
            CHECK(f->parameters.size() == 1U && f->parameters[0].hasVerifiedLayout &&
                      f->parameters[0].sizeBytes == 8U && f->parameters[0].alignBytes == 4U &&
                      f->parameters[0].recordFields.size() == 2U,
                  float_pair_layout.c_str());
        } else {
            const std::string float_pair_imports =
                std::string("sum_float_pair imports on ") + triple;
            CHECK(false, float_pair_imports.c_str());
        }
        if (const auto *f = findFunction(*art, "make_float_pair")) {
            const std::string float_pair_result =
                std::string("FloatPair HFA return layout is verified: ") + triple;
            CHECK(f->result.hasVerifiedLayout && f->result.sizeBytes == 8U &&
                      f->result.alignBytes == 4U && f->result.recordFields.size() == 2U,
                  float_pair_result.c_str());
            const std::string float_pair_result_copy =
                std::string("FloatPair return adapter copies the result: ") + triple;
            CHECK(f->adapterSource.find("memcpy(result, &result_value") != std::string::npos,
                  float_pair_result_copy.c_str());
        } else {
            const std::string float_pair_result_imports =
                std::string("make_float_pair imports on ") + triple;
            CHECK(false, float_pair_result_imports.c_str());
        }
        if (const auto *f = findFunction(*art, "sum_pair64")) {
            const std::string pair64_param =
                std::string("sum_pair64 keeps its by-value Pair64 parameter: ") + triple;
            CHECK_EQ(f->parameters.size(), 1u, pair64_param.c_str());
            const std::string adapter_name =
                std::string("sum_pair64 receives a generated C ABI adapter: ") + triple;
            const std::string adapter_body =
                std::string("adapter marshals the Pair64 parameter and calls C: ") + triple;
            CHECK(!f->adapterName.empty(), adapter_name.c_str());
            CHECK(f->adapterSource.find(f->adapterName) != std::string::npos &&
                      f->adapterSource.find("memcpy") != std::string::npos &&
                      f->adapterSource.find("sum_pair64(arg0)") != std::string::npos,
                  adapter_body.c_str());
            if (!f->parameters.empty()) {
                const auto &pair = f->parameters[0];
                const std::string pair64_verified =
                    std::string("Pair64 layout is verified: ") + triple;
                const std::string pair64_size  = std::string("Pair64 is 16 bytes: ") + triple;
                const std::string pair64_align = std::string("Pair64 aligns to 8: ") + triple;
                const std::string pair64_fields =
                    std::string("Pair64 has two validated fields: ") + triple;
                const std::string pair64_offsets =
                    std::string("Pair64 fields are adjacent at 0 and 64 bits: ") + triple;
                CHECK(pair.hasVerifiedLayout, pair64_verified.c_str());
                CHECK_EQ(pair.sizeBytes, 16u, pair64_size.c_str());
                CHECK_EQ(pair.alignBytes, 8u, pair64_align.c_str());
                CHECK_EQ(pair.recordFields.size(), 2u, pair64_fields.c_str());
                if (pair.recordFields.size() == 2U) {
                    CHECK_EQ(pair.recordFields[0].offsetBits, 0u, pair64_offsets.c_str());
                    CHECK_EQ(pair.recordFields[1].offsetBits, 64u, pair64_offsets.c_str());
                }
            }
        } else {
            const std::string pair64_imports = std::string("sum_pair64 imports on ") + triple;
            CHECK(false, pair64_imports.c_str());
        }
        if (const auto *f = findFunction(*art, "make_pair64")) {
            const std::string pair64_result =
                std::string("make_pair64 result has the validated Pair64 layout: ") + triple;
            CHECK(f->result.hasVerifiedLayout && f->result.sizeBytes == 16u &&
                      f->result.alignBytes == 8u && f->result.recordFields.size() == 2U,
                  pair64_result.c_str());
        } else {
            const std::string pair64_result_imports =
                std::string("make_pair64 imports on ") + triple;
            CHECK(false, pair64_result_imports.c_str());
        }
        if (const auto *f = findFunction(*art, "sum_triple64")) {
            const std::string triple_layout =
                std::string("Triple64 has a verified three-field layout: ") + triple;
            CHECK(f->parameters.size() == 1U && f->parameters[0].hasVerifiedLayout &&
                      f->parameters[0].sizeBytes == 24U && f->parameters[0].alignBytes == 8U &&
                      f->parameters[0].recordFields.size() == 3U,
                  triple_layout.c_str());
        } else {
            const std::string triple_imports = std::string("sum_triple64 imports on ") + triple;
            CHECK(false, triple_imports.c_str());
        }
        if (const auto *f = findFunction(*art, "make_triple64")) {
            const std::string triple_result =
                std::string("Triple64 return layout is verified: ") + triple;
            CHECK(f->result.hasVerifiedLayout && f->result.sizeBytes == 24U &&
                      f->result.alignBytes == 8U && f->result.recordFields.size() == 3U,
                  triple_result.c_str());
            const std::string triple_result_copy =
                std::string("Triple64 return adapter copies the result: ") + triple;
            CHECK(f->adapterSource.find("memcpy(result, &result_value") != std::string::npos,
                  triple_result_copy.c_str());
        } else {
            const std::string triple_result_imports =
                std::string("make_triple64 imports on ") + triple;
            CHECK(false, triple_result_imports.c_str());
        }
        if (const auto *f = findFunction(*art, "sum_mixed32_64")) {
            const std::string mixed_param =
                std::string("sum_mixed32_64 keeps its by-value parameter: ") + triple;
            CHECK_EQ(f->parameters.size(), 1u, mixed_param.c_str());
            if (!f->parameters.empty()) {
                const auto &value = f->parameters[0];
                const std::string mixed_layout =
                    std::string("Mixed32_64 has a verified 16-byte, 8-aligned layout: ") + triple;
                const std::string mixed_fields =
                    std::string("Mixed32_64 has two validated fields: ") + triple;
                CHECK(value.hasVerifiedLayout && value.sizeBytes == 16U && value.alignBytes == 8U,
                      mixed_layout.c_str());
                CHECK_EQ(value.recordFields.size(), 2u, mixed_fields.c_str());
                if (value.recordFields.size() == 2U) {
                    const std::string mixed_offsets =
                        std::string("Mixed32_64 fields are at 0 and 64 bits: ") + triple;
                    CHECK_EQ(value.recordFields[0].offsetBits, 0u, mixed_offsets.c_str());
                    CHECK_EQ(value.recordFields[1].offsetBits, 64u, mixed_offsets.c_str());
                }
            }
        } else {
            const std::string mixed_param_imports =
                std::string("sum_mixed32_64 imports on ") + triple;
            CHECK(false, mixed_param_imports.c_str());
        }
        if (const auto *f = findFunction(*art, "make_mixed32_64")) {
            const std::string mixed_result =
                std::string("make_mixed32_64 returns a verified Mixed32_64: ") + triple;
            CHECK(f->result.hasVerifiedLayout && f->result.sizeBytes == 16U &&
                      f->result.alignBytes == 8U && f->result.recordFields.size() == 2U,
                  mixed_result.c_str());
        } else {
            const std::string mixed_result_imports =
                std::string("make_mixed32_64 imports on ") + triple;
            CHECK(false, mixed_result_imports.c_str());
        }
        if (const auto *f = findFunction(*art, "sum_mixed64_32")) {
            const std::string mixed_reverse_param =
                std::string("sum_mixed64_32 keeps its by-value parameter: ") + triple;
            CHECK_EQ(f->parameters.size(), 1u, mixed_reverse_param.c_str());
            if (!f->parameters.empty()) {
                const auto &value = f->parameters[0];
                const std::string mixed_reverse_layout =
                    std::string("Mixed64_32 has a verified 16-byte, 8-aligned layout: ") + triple;
                CHECK(value.hasVerifiedLayout && value.sizeBytes == 16U && value.alignBytes == 8U &&
                          value.recordFields.size() == 2U,
                      mixed_reverse_layout.c_str());
                if (value.recordFields.size() == 2U) {
                    const std::string mixed_reverse_offsets =
                        std::string("Mixed64_32 fields are at 0 and 64 bits: ") + triple;
                    CHECK_EQ(value.recordFields[0].offsetBits, 0u, mixed_reverse_offsets.c_str());
                    CHECK_EQ(value.recordFields[1].offsetBits, 64u, mixed_reverse_offsets.c_str());
                }
            }
        } else {
            const std::string mixed_reverse_imports =
                std::string("sum_mixed64_32 imports on ") + triple;
            CHECK(false, mixed_reverse_imports.c_str());
        }
        if (const auto *f = findFunction(*art, "make_mixed64_32")) {
            const std::string mixed_reverse_result =
                std::string("make_mixed64_32 returns a verified Mixed64_32: ") + triple;
            CHECK(f->result.hasVerifiedLayout && f->result.sizeBytes == 16U &&
                      f->result.alignBytes == 8U && f->result.recordFields.size() == 2U,
                  mixed_reverse_result.c_str());
        } else {
            const std::string mixed_reverse_result_imports =
                std::string("make_mixed64_32 imports on ") + triple;
            CHECK(false, mixed_reverse_result_imports.c_str());
        }
    }

    // 7a. Matching layout does not imply support for an unvalidated target ABI.
    {
        ParseOptions opts;
        opts.targetTriple = "x86_64-pc-windows-msvc";
        const auto art    = parseHeaderFromContent("unvalidated_target_record.h",
                                                   "struct Pair64 { long long lo, hi; };\n"
                                                   "long long sum_pair64(struct Pair64 value);\n",
                                                   opts);
        CHECK(findFunction(*art, "sum_pair64") == nullptr,
              "Pair64 is not imported by value for the unvalidated Windows x64 ABI");
        bool found_skip = false;
        for (const auto &skipped : art->skippedFunctions)
            found_skip = found_skip || skipped.find("sum_pair64") != std::string::npos;
        CHECK(found_skip, "the unsupported target ABI has an explicit skipped-function reason");
    }

    // 7b. Unsupported record layouts produce explicit skips instead of imports.
    for (const auto *triple : {"x86_64-unknown-linux-gnu", "aarch64-unknown-linux-gnu"}) {
        ParseOptions opts;
        opts.targetTriple = triple;
        const auto art =
            parseHeaderFromContent("unsupported_records.h",
                                   "struct Bits { unsigned int a : 4; unsigned int b : 4; };\n"
                                   "int with_bits(struct Bits b);\n"
                                   "struct Packed { int x; } __attribute__((packed));\n"
                                   "int with_packed(struct Packed p);\n"
                                   "struct Anonymous { struct { int x; }; };\n"
                                   "int with_anonymous(struct Anonymous a);\n"
                                   "struct Flexible { int count; int data[]; };\n"
                                   "int with_flexible(struct Flexible f);\n"
                                   "struct Wide { long double value; };\n"
                                   "int with_wide(struct Wide w);\n"
                                   "struct Int128 { __int128 value; };\n"
                                   "int with_int128(struct Int128 i);\n"
                                   "struct Mixed { long long lo; int mid; int hi; };\n"
                                   "int with_mixed(struct Mixed m);\n"
                                   "struct Variadic { long long lo, hi; };\n"
                                   "int with_variadic_record(struct Variadic value, ...);\n",
                                   opts);
        const std::string bits_skip = std::string("bitfield record function is skipped: ") + triple;
        const std::string packed_skip = std::string("packed record function is skipped: ") + triple;
        const std::string anonymous_skip =
            std::string("anonymous record function is skipped: ") + triple;
        const std::string flexible_skip =
            std::string("flexible array record function is skipped: ") + triple;
        const std::string wide_skip =
            std::string("long double record function is skipped: ") + triple;
        const std::string int128_skip =
            std::string("__int128 record function is skipped: ") + triple;
        CHECK(findFunction(*art, "with_bits") == nullptr, bits_skip.c_str());
        CHECK(findFunction(*art, "with_packed") == nullptr, packed_skip.c_str());
        CHECK(findFunction(*art, "with_anonymous") == nullptr, anonymous_skip.c_str());
        CHECK(findFunction(*art, "with_flexible") == nullptr, flexible_skip.c_str());
        CHECK(findFunction(*art, "with_wide") == nullptr, wide_skip.c_str());
        CHECK(findFunction(*art, "with_int128") == nullptr, int128_skip.c_str());
        CHECK(findFunction(*art, "with_mixed") != nullptr,
              "mixed-layout record is passed through the generated adapter");
        CHECK(findFunction(*art, "with_variadic_record") == nullptr,
              "variadic functions with by-value records are rejected");

        int explicit_skips = 0;
        for (const auto &skipped : art->skippedFunctions) {
            if (skipped.find("with_bits") != std::string::npos ||
                skipped.find("with_packed") != std::string::npos ||
                skipped.find("with_anonymous") != std::string::npos ||
                skipped.find("with_flexible") != std::string::npos ||
                skipped.find("with_wide") != std::string::npos ||
                skipped.find("with_int128") != std::string::npos ||
                skipped.find("with_variadic_record") != std::string::npos)
                explicit_skips++;
        }
        const std::string skips_all = std::string("every unsupported record has a skip: ") + triple;
        CHECK_EQ(explicit_skips, 7, skips_all.c_str());
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
            CHECK(f->result.enumUnderlying != nullptr,
                  "next: result records its clang-reported underlying type");
            if (f->result.enumUnderlying != nullptr) {
                CHECK_EQ(static_cast<int>(f->result.enumUnderlying->kind),
                         static_cast<int>(TypeKind::Integer),
                         "next: enum underlying type is integer");
                CHECK_EQ(f->result.enumUnderlying->bits, 32U,
                         "next: ordinary C enum has a 32-bit underlying type on this target");
            }
        }
    }
}

void test_filtering() {
    std::printf("=== Filtering ===\n");

    // 9. Variadic function is included and flagged for codegen
    {
        const auto art =
            parseHeaderFromContent("variadic.h", "int printf(const char *fmt, ...);\n");
        if (const auto *f = findFunction(*art, "printf")) {
            CHECK(f->isVariadic, "clang reports printf as variadic");
            CHECK_EQ(f->parameters.size(), 1u, "printf keeps its fixed parameter");
        } else {
            CHECK(false, "variadic printf is included");
        }
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

void test_object_like_macro_constants() {
    std::printf("=== Object-like macro constants ===\n");

    {
        const auto art =
            parseHeaderFromContent("scalar_constants.h", "#define ANSWER 42\n"
                                                         "#define RATIO 1.5f\n"
                                                         "#define YES true\n"
                                                         "#define LETTER 'A'\n"
                                                         "#define BIG 12345678901234567890u64\n"
                                                         "#define NEG -7i64\n"
                                                         "#define NAME(x) x\n");
        CHECK(art->diagnostics.empty(), "scalar macro constants do not fail the import");
        if (const auto *c = findConstant(*art, "ANSWER")) {
            CHECK_EQ(static_cast<int>(c->kind), static_cast<int>(ConstantKind::Integer),
                     "ANSWER is Integer");
            CHECK_EQ(c->bits, 32, "ANSWER is i32");
            CHECK(c->isSigned, "ANSWER is signed");
            CHECK_EQ(c->integerValue, 42, "ANSWER has value 42");
        } else {
            CHECK(false, "ANSWER imported as a constant");
        }
        if (const auto *c = findConstant(*art, "RATIO")) {
            CHECK_EQ(static_cast<int>(c->kind), static_cast<int>(ConstantKind::Float),
                     "RATIO is Float");
            CHECK_EQ(c->bits, 32, "RATIO is f32");
            CHECK_EQ(c->floatValue, 1.5, "RATIO has value 1.5");
        } else {
            CHECK(false, "RATIO imported as a constant");
        }
        if (const auto *c = findConstant(*art, "YES")) {
            CHECK_EQ(static_cast<int>(c->kind), static_cast<int>(ConstantKind::Bool),
                     "YES is Bool");
            CHECK(c->boolValue, "YES is true");
        } else {
            CHECK(false, "YES imported as a constant");
        }
        if (const auto *c = findConstant(*art, "LETTER")) {
            CHECK_EQ(static_cast<int>(c->kind), static_cast<int>(ConstantKind::Char),
                     "LETTER is Char");
            CHECK_EQ(static_cast<char>(c->charValue), 'A', "LETTER is 'A'");
        } else {
            CHECK(false, "LETTER imported as a constant");
        }
        if (const auto *c = findConstant(*art, "BIG")) {
            CHECK_EQ(static_cast<int>(c->kind), static_cast<int>(ConstantKind::Integer),
                     "BIG is Integer");
            CHECK_EQ(c->bits, 64, "BIG is u64");
            CHECK(!c->isSigned, "BIG is unsigned");
            CHECK_EQ(static_cast<unsigned long long>(c->integerValue), 12345678901234567890ULL,
                     "BIG preserves u64 magnitude");
        } else {
            std::printf("skips: ");
            for (const auto &skipped : art->skippedFunctions)
                std::printf("%s | ", skipped.c_str());
            std::printf("\n");
            CHECK(false, "BIG imported as a constant");
        }
        if (const auto *c = findConstant(*art, "NEG")) {
            CHECK_EQ(static_cast<int>(c->kind), static_cast<int>(ConstantKind::Integer),
                     "NEG is Integer");
            CHECK_EQ(c->bits, 64, "NEG is i64");
            CHECK(c->isSigned, "NEG is signed");
            CHECK_EQ(c->integerValue, -7, "NEG is -7");
        } else {
            CHECK(false, "NEG imported as a constant");
        }
        CHECK(findConstant(*art, "NAME") == nullptr, "function-like macro is not a constant");
        bool found_skip = false;
        for (const auto &skipped : art->skippedFunctions)
            found_skip = found_skip || skipped.find("NAME") != std::string::npos;
        CHECK(found_skip, "function-like macro appears in skips");
    }

    {
        const auto art = parseHeaderFromContent("unsupported_constants.h",
                                                "#define TOKEN 1 + 2\n"
                                                "#define MSG \"hello\"\n"
                                                "#define TOOBIG 9999999999999999999999999\n");
        CHECK(art->diagnostics.empty(), "unsupported constants do not fail the import");
        CHECK(findConstant(*art, "TOKEN") == nullptr, "TOKEN is not a scalar constant");
        CHECK(findConstant(*art, "MSG") == nullptr, "MSG is not a constant");
        CHECK(findConstant(*art, "TOOBIG") == nullptr, "out-of-range integer is skipped");
        bool found_token  = false;
        bool found_msg    = false;
        bool found_toobig = false;
        for (const auto &skipped : art->skippedFunctions) {
            found_token  = found_token || skipped.find("TOKEN") != std::string::npos;
            found_msg    = found_msg || skipped.find("MSG") != std::string::npos;
            found_toobig = found_toobig || skipped.find("TOOBIG") != std::string::npos;
        }
        CHECK(found_token, "TOKEN appears in skips");
        CHECK(found_msg, "MSG appears in skips");
        CHECK(found_toobig, "TOOBIG appears in skips");
    }

    {
        // Compiler predefines are not part of the main-file macro visit; the
        // artifact must not expose them even when a name collides with a test.
        const auto art = parseHeaderFromContent("predefined.h", "#define __LINE__ 123\n");
        CHECK(findConstant(*art, "__LINE__") == nullptr,
              "builtin/predefined macros are not imported");
    }
}

void test_external_globals_and_reachable_typedefs() {
    std::printf("=== External globals ===\n");

    {
        const auto art = parseHeaderFromContent(
            "external_globals.h", "typedef struct _zith_file FILE;\n"
                                  "typedef FILE FileAlias;\n"
                                  "typedef unsigned long UnusedType;\n"
                                  "extern int counter;\n"
                                  "extern int *pointer;\n"
                                  "extern FileAlias *stream;\n");
        CHECK(art->diagnostics.empty(), "supported external globals do not fail the import");
        CHECK_EQ(art->globals.size(), 3u, "scalar and pointer globals are imported");
        CHECK(findGlobal(*art, "counter") != nullptr, "scalar global is present");
        if (const auto *counter = findGlobal(*art, "counter")) {
            CHECK_EQ(counter->linkageName, std::string("counter"),
                     "C global keeps its external linker name");
            CHECK_EQ(counter->type.kind, TypeKind::Integer, "scalar global keeps its C type");
            CHECK_EQ(counter->type.bits, 32, "int global keeps its target width");
            CHECK(!counter->isConst, "mutable global is not marked const");
        }
        if (const auto *pointer = findGlobal(*art, "pointer")) {
            CHECK_EQ(pointer->type.kind, TypeKind::Pointer, "pointer global keeps pointer type");
            CHECK(pointer->type.pointee != nullptr, "pointer global has pointee metadata");
            if (pointer->type.pointee != nullptr)
                CHECK_EQ(pointer->type.pointee->kind, TypeKind::Integer,
                         "pointer global retains scalar pointee type");
        }
        if (const auto *stream = findGlobal(*art, "stream")) {
            CHECK_EQ(stream->type.kind, TypeKind::Pointer, "typedef pointer global is a pointer");
            CHECK(stream->type.pointee != nullptr, "typedef pointer retains opaque pointee");
            if (stream->type.pointee != nullptr)
                CHECK_EQ(stream->type.pointee->kind, TypeKind::Record,
                         "opaque typedef pointee remains a record");
        }
        CHECK(findTypedef(*art, "FILE") != nullptr, "reachable FILE typedef is imported");
        CHECK(findTypedef(*art, "FileAlias") != nullptr,
              "reachable typedef alias chain is imported");
        CHECK(findTypedef(*art, "UnusedType") == nullptr,
              "unreferenced typedef is not imported");
    }

    {
        (void)writeTempHeader("globals_dependency.h", "extern int transitive_global;\n");
        const auto art = parseHeaderFromContent(
            "direct_globals.h", "#include \"globals_dependency.h\"\n"
                                "extern int direct_global;\n"
                                "static int internal_global;\n");
        CHECK_EQ(art->globals.size(), 1u, "only the direct external global is imported");
        CHECK(findGlobal(*art, "direct_global") != nullptr, "direct global is present");
        CHECK(findGlobal(*art, "transitive_global") == nullptr,
              "global declared by an included header is not imported");
        CHECK(findGlobal(*art, "internal_global") == nullptr,
              "file-local static global is not imported");
    }

    {
        const auto art = parseHeaderFromContent(
            "qualified_globals.h", "extern const int immutable_scalar;\n"
                                   "extern const int *pointee_const;\n"
                                   "extern int *const object_const;\n"
                                   "extern const int *const both_const;\n");
        CHECK_EQ(art->globals.size(), 4u, "all qualified globals are imported");
        const auto *immutable_scalar = findGlobal(*art, "immutable_scalar");
        CHECK(immutable_scalar != nullptr && immutable_scalar->isConst,
              "top-level scalar constness is retained");
        const auto *pointee_const = findGlobal(*art, "pointee_const");
        const auto *object_const  = findGlobal(*art, "object_const");
        const auto *both_const    = findGlobal(*art, "both_const");
        CHECK(pointee_const != nullptr && !pointee_const->isConst,
              "pointee constness does not make the pointer object immutable");
        CHECK(object_const != nullptr && object_const->isConst,
              "top-level pointer constness makes the object immutable");
        CHECK(both_const != nullptr && both_const->isConst,
              "both pointer and pointee constness are retained");
        if (pointee_const != nullptr && pointee_const->type.pointee != nullptr)
            CHECK(pointee_const->type.pointee->isConst,
                  "const pointee qualification remains on the pointee type");
        if (object_const != nullptr && object_const->type.pointee != nullptr)
            CHECK(!object_const->type.pointee->isConst,
                  "mutable pointee stays mutable for a const pointer object");
        if (both_const != nullptr && both_const->type.pointee != nullptr)
            CHECK(both_const->type.pointee->isConst,
                  "const pointee qualification is independent of object constness");
    }

    {
        const auto art = parseHeaderFromContent(
            "unsupported_globals.h", "struct Incomplete;\n"
                                    "extern struct Incomplete unsupported_object;\n"
                                    "extern int supported_object;\n");
        CHECK(findGlobal(*art, "unsupported_object") == nullptr,
              "unsupported by-value aggregate is not imported");
        CHECK(findGlobal(*art, "supported_object") != nullptr,
              "supported sibling is still imported");
        CHECK(!art->skippedDeclarations.empty(),
              "unsupported global is retained as a non-fatal diagnostic");
        bool names_unsupported = false;
        for (const auto &diagnostic : art->skippedDeclarations)
            names_unsupported =
                names_unsupported || diagnostic.message.find("unsupported_object") !=
                                         std::string::npos;
        CHECK(names_unsupported, "unsupported global diagnostic identifies the declaration");
    }
}

#endif // ZITH_ENABLE_C_INTEROP

} // namespace

void test_cinterop() {
    test_without_libclang();
    test_system_include_dirs();
#ifdef ZITH_ENABLE_C_INTEROP
    test_scalar_types();
    test_pointer_and_const();
    test_decayed_and_special_parameter_types();
    test_skips_and_dedupe();
    test_struct_and_enum();
    test_filtering();
    test_dependencies();
    test_parse_errors();
    test_parse_options();
    test_object_like_macro_constants();
    test_external_globals_and_reachable_typedefs();
#endif
}
TEST_MAIN(cinterop)
