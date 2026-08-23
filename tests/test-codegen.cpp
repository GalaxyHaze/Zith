#include "cli/options.hpp"
#include "codegen/codegen-type.hpp"
#include "codegen/codegen.hpp"
#include "diagnostics/diagnostic-engine.hpp"
#include "hir/hir-expr.hpp"
#include "hir/hir-module.hpp"
#include "memory/arena.hpp"
#include "session/compilation-session.hpp"
#include "test-common.hpp"
#include "types/type-intern.hpp"

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

using namespace zith;

struct CodegenTest {
    memory::Arena arena;
    Options opts;

    CodegenTest() : opts(arena) {
#ifdef ZITH_STDLIB_DIR
        // Mirror `zithc --include stdlib`: give every codegen test access to the
        // language standard library (stdlib/std/io/console and friends).
        opts.includeDirs.push(ZITH_STDLIB_DIR);
#endif
    }

    struct Result {
        bool ok           = false;
        size_t errorCount = 0;
        int exitCode      = 0;
        std::string output;
    };

    Result run(std::string_view file_name, std::string_view input) {
        std::string path = std::string("/tmp/") + std::string(file_name);
        session::CompilationSession session(opts, path);
        session.setBuffered(true);
        session.setAlwaysEmitObject(true);
        session.setContent(std::string(input));

        bool ok     = session.run();
        size_t errs = 0;
        for (const auto &d : session.diags().all()) {
            if (d.severity == diagnostics::Severity::Error) {
                errs++;
                std::printf("    [Diag] Code: %u, Message: %s\n", d.code, d.message.c_str());
            }
        }

        if (ok && errs == 0)
            ok = session.linkAndExec();

        std::string output = session.flushOutput();
        output += session.takeChildOutput();
        return {ok && errs == 0, errs, session.childExitCode(), std::move(output)};
    }
};

struct ModernFileCodegenTest {
    memory::Arena arena;
    Options opts;
    std::filesystem::path root;

    ModernFileCodegenTest()
        : opts(arena), root(std::filesystem::temp_directory_path() / "zith-codegen-modern-tests") {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
#ifdef ZITH_STDLIB_DIR
        opts.includeDirs.push(ZITH_STDLIB_DIR);
#endif
    }

    ~ModernFileCodegenTest() {
        std::filesystem::remove_all(root);
    }

    void write(std::string_view name, std::string_view text) {
        auto path = root / name;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << text;
    }

    struct Result {
        bool ok           = false;
        bool usedModern   = false;
        size_t errorCount = 0;
        size_t cacheHits  = 0;
        int exitCode      = 0;
        std::string output;
    };

    Result run(std::string_view main_name = "main.zith") {
        session::CompilationSession session(opts, (root / main_name).string());
        session.setBuffered(true);
        session.setAlwaysEmitObject(true);

        bool ok     = session.run();
        size_t errs = 0;
        for (const auto &d : session.diags().all()) {
            if (d.severity == diagnostics::Severity::Error) {
                errs++;
                std::printf("    [ModernCodegenDiag] Code: %u, Message: %s\n", d.code,
                            d.message.c_str());
            }
        }

        if (ok && errs == 0)
            ok = session.linkAndExec();

        std::string output = session.flushOutput();
        output += session.takeChildOutput();
        return {ok && errs == 0,
                session.snapshot() != nullptr,
                errs,
                session.cacheMetrics().hits,
                session.childExitCode(),
                std::move(output)};
    }
};

static void test_return_literal() {
    CodegenTest t;
    auto r = t.run("codegen-return-literal.zith", "fn main(): i32 {\n"
                                                  "    return 7;\n"
                                                  "}\n");
    CHECK(r.ok, "Literal return reaches executable");
    CHECK_EQ(r.exitCode, 7, "Literal return value is preserved");
}

static void test_ref_deref_local() {
    CodegenTest t;
    auto r = t.run("codegen-ref-deref-local.zith", "fn main(): i32 {\n"
                                                   "    var x: i32 = 41;\n"
                                                   "    var p: *i32 = &x;\n"
                                                   "    return *p;\n"
                                                   "}\n");
    CHECK(r.ok, "Taking a reference to a local and dereferencing compiles");
    CHECK_EQ(r.exitCode, 41, "Dereferencing a local pointer returns the pointed value");
}

static void test_ref_deref_param() {
    CodegenTest t;
    auto r = t.run("codegen-ref-deref-param.zith", "fn reflect(x: i32): i32 {\n"
                                                   "    var p: *i32 = &x;\n"
                                                   "    return *p;\n"
                                                   "}\n"
                                                   "fn main(): i32 {\n"
                                                   "    return reflect(23);\n"
                                                   "}\n");
    CHECK(r.ok, "Taking a reference to a parameter compiles");
    CHECK_EQ(r.exitCode, 23, "Parameter references round-trip through memory");
}

static void test_pointer_parameter_call() {
    CodegenTest t;
    auto r = t.run("codegen-pointer-param-call.zith", "fn load(p: *i32): i32 {\n"
                                                      "    return *p;\n"
                                                      "}\n"
                                                      "fn main(): i32 {\n"
                                                      "    var x: i32 = 19;\n"
                                                      "    return load(&x);\n"
                                                      "}\n");
    CHECK(r.ok, "Passing a reference as a pointer argument compiles");
    CHECK_EQ(r.exitCode, 19, "Pointer arguments dereference correctly in callees");
}

static void test_double_pointer_roundtrip() {
    CodegenTest t;
    auto r = t.run("codegen-double-pointer-roundtrip.zith", "fn main(): i32 {\n"
                                                            "    var x: i32 = 9;\n"
                                                            "    var p: *i32 = &x;\n"
                                                            "    var pp: **i32 = &p;\n"
                                                            "    return **pp;\n"
                                                            "}\n");
    CHECK(r.ok, "Double indirection compiles");
    CHECK_EQ(r.exitCode, 9, "Double dereference returns the original value");
}

static void test_deref_ref_expression_chain() {
    CodegenTest t;
    auto r = t.run("codegen-deref-ref-expression-chain.zith", "fn main(): i32 {\n"
                                                              "    var x: i32 = 12;\n"
                                                              "    return *&x;\n"
                                                              "}\n");
    CHECK(r.ok, "Immediate deref-ref chain compiles");
    CHECK_EQ(r.exitCode, 12, "Immediate deref-ref chain preserves the value");
}

static void test_unsigned_comparison() {
    CodegenTest t;
    auto r = t.run("codegen-unsigned-cmp.zith", "fn main(): i32 {\n"
                                                "    var x: u32 = 4294967295;\n"
                                                "    var y: u32 = 1;\n"
                                                "    if (x > y) { return 1; }\n"
                                                "    return 0;\n"
                                                "}\n");
    CHECK(r.ok, "Unsigned comparison compiles and runs");
    CHECK_EQ(r.exitCode, 1, "4294967295u32 > 1u32 (unsigned comparison must use UGT not SGT)");
}

static void test_forward_reference() {
    CodegenTest t;
    auto r = t.run("codegen-forward-ref.zith", "fn main(): i32 {\n"
                                               "    return helper();\n"
                                               "}\n"
                                               "fn helper(): i32 {\n"
                                               "    return 42;\n"
                                               "}\n");
    CHECK(r.ok, "Forward reference compiles and runs");
    CHECK_EQ(r.exitCode, 42, "Function defined after main is resolved");
}

static void test_array_variable_indexing() {
    CodegenTest t;
    auto r = t.run("codegen-array-indexing.zith", "fn main(): i32 {\n"
                                                  "    var arr: [5]i32;\n"
                                                  "    arr[0] = 10;\n"
                                                  "    arr[1] = 20;\n"
                                                  "    arr[2] = 30;\n"
                                                  "    return arr[1];\n"
                                                  "}\n");
    CHECK(r.ok, "Array indexing compiles and runs");
    printf("EXIT CODE: %d\n", r.exitCode);
    CHECK_EQ(r.exitCode, 20, "arr[1] returns 20");
}

static void test_pointer_index() {
    CodegenTest t;
    auto r = t.run("codegen-pointer-index.zith",
                   "fn main(): i32 { var x: i32 = 42; var p: *i32 = &x; return p[0]; }");
    CHECK(r.ok, "Pointer indexing compiles and runs");
    CHECK_EQ(r.exitCode, 42, "p[0] returns the correct value");
}

static void test_shifts() {
    CodegenTest t;
    auto r = t.run("codegen-shifts.zith", "fn arithmetic(): i32 {\n"
                                          "    var signed: i32 = -8;\n"
                                          "    return signed >> 1;\n"
                                          "}\n"
                                          "fn logical(): u32 {\n"
                                          "    var unsigned: u32 = 1;\n"
                                          "    return unsigned << 3;\n"
                                          "}\n"
                                          "fn main(): i32 {\n"
                                          "    if (logical() == 8) { return arithmetic() + 8; }\n"
                                          "    return 0;\n"
                                          "}\n");
    CHECK(r.ok, "Signed and unsigned shifts compile and run");
    printf("EXIT CODE: %d\n", r.exitCode);
    CHECK_EQ(r.exitCode, 4,
             "Signed right shift is arithmetic and unsigned left shift is preserved");
}

static void test_compound_assign_runtime() {
    CodegenTest t;
    // 1 -> 3 (+=2) -> 6 (<<=1) -> 2 (&=3); then 2 |. 4 == 6, so 2 + 6 == 8.
    auto r = t.run("codegen-compound-assign.zith", "fn main(): i32 {\n"
                                                   "    var x: i32 = 1;\n"
                                                   "    x += 2;\n"
                                                   "    x <<= 1;\n"
                                                   "    x &= 3;\n"
                                                   "    var z: i32 = x |. 4;\n"
                                                   "    return x + z;\n"
                                                   "}\n");
    CHECK(r.ok, "Compound assignment and bitwise operators compile and run");
    CHECK_EQ(r.exitCode, 8, "Compound assignment and '|.' produce the expected exit status");
}

static void test_raw_opaque_round_trip_runtime() {
    CodegenTest t;
    auto r = t.run("codegen-raw-opaque.zith", "fn thru(p: raw opaque): *i32 {\n"
                                              "    return p as *i32;\n"
                                              "}\n"
                                              "fn main(): i32 {\n"
                                              "    var v: i32 = 41;\n"
                                              "    var addr: *i32 = &v;\n"
                                              "    var q: raw opaque = addr as raw opaque;\n"
                                              "    var r: *i32 = thru(q);\n"
                                              "    return *r + 1;\n"
                                              "}\n");
    CHECK(r.ok, "A 'raw opaque' pointer round-trip compiles and runs");
    CHECK_EQ(r.exitCode, 42, "'raw opaque' round-trip preserves the pointed-to value");
}

static void test_struct_fields_and_parameter() {
    CodegenTest t;
    auto r = t.run("codegen-struct-fields.zith",
                   "struct Pair { left: i32, right: i32 }\n"
                   "fn sum(pair: Pair): i32 { return pair.left + pair.right; }\n"
                   "fn main(): i32 {\n"
                   "    var pair: Pair = Pair{3, 4};\n"
                   "    pair.left = 8;\n"
                   "    return sum(pair);\n"
                   "}\n");
    CHECK(r.ok, "Struct literals, field assignment, and by-value parameters compile and run");
    printf("EXIT CODE: %d\n", r.exitCode);
    CHECK_EQ(r.exitCode, 12, "Struct field values preserve declaration order");
}

static void test_array_of_structs() {
    CodegenTest t;
    auto r = t.run("codegen-array-of-structs.zith", "struct Pair { left: i32, right: i32 }\n"
                                                    "fn main(): i32 {\n"
                                                    "    var items: [2]Pair;\n"
                                                    "    items[0] = Pair{5, 6};\n"
                                                    "    items[0].right = 9;\n"
                                                    "    return items[0].left + items[0].right;\n"
                                                    "}\n");
    CHECK(r.ok, "Arrays of structs support indexed field assignment");
    printf("EXIT CODE: %d\n", r.exitCode);
    CHECK_EQ(r.exitCode, 14, "items[i].field writes target the selected aggregate element");
}

static void test_enum_values() {
    CodegenTest t;
    auto r = t.run("codegen-enum-values.zith", "enum Color: u8 { Red = 2, Green }\n"
                                               "fn main(): i32 {\n"
                                               "    if (Color.Green == Color.Green) { return 3; }\n"
                                               "    return 0;\n"
                                               "}\n");
    CHECK(r.ok, "Typed enum variant constants compile and run");
    CHECK_EQ(r.exitCode, 3, "Enum variant comparisons use the underlying integer representation");
}

static void test_offsetof_and_alignof_runtime() {
    CodegenTest t;
    auto r = t.run("codegen-offsetof-alignof.zith", "struct Padded { left: u8, right: u32 }\n"
                                                    "fn main(): i32 {\n"
                                                    "    if (@offsetOf(Padded, right) == 4) {\n"
                                                    "        return @alignOf(Padded);\n"
                                                    "    }\n"
                                                    "    return 0;\n"
                                                    "}\n");
    CHECK(r.ok, "@offsetOf and @alignOf compile and run");
    printf("EXIT CODE: %d\n", r.exitCode);
    CHECK_EQ(r.exitCode, 4, "Layout intrinsics follow the target ABI for padded structs");
}

static void test_sizeof_intrinsic_runtime() {
    CodegenTest t;
    auto r = t.run("codegen-sizeof.zith", "struct Padded { left: u8, right: u32 }\n"
                                          "fn main(): i32 {\n"
                                          "    if (@sizeOf(u8) == 1) {\n"
                                          "        if (@sizeOf(i32) == 4) {\n"
                                          "            if (@sizeOf(Padded) == 8) { return 1; }\n"
                                          "        }\n"
                                          "    }\n"
                                          "    return 0;\n"
                                          "}\n");
    CHECK(r.ok, "@sizeOf on primitives and structs compiles and runs");
    printf("EXIT CODE: %d\n", r.exitCode);
    CHECK_EQ(r.exitCode, 1, "@sizeOf folds to the target-ABI size in bytes");
}

static void test_when_expression_runtime() {
    CodegenTest t;
    auto r = t.run("codegen-when.zith",
                   "fn classify(n: i32): i32 {\n"
                   "    return when (n) {\n"
                   "        (0) ~> 10,\n"
                   "        (1..3) ~> 20,\n"
                   "        (4) ~> 30,\n"
                   "        (_) ~> 40\n"
                   "    }\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    return classify(0) + classify(2) + classify(4) + classify(9);\n"
                   "}\n");
    CHECK(r.ok, "when with literal, range, and default cases compiles and runs");
    printf("EXIT CODE: %d\n", r.exitCode);
    CHECK_EQ(r.exitCode, 100, "when dispatches through literal, range, and default cases");
}

static void test_for_three_clause_runtime() {
    CodegenTest t;
    auto r = t.run("codegen-for3.zith", "fn sum_to(n: i32): i32 {\n"
                                        "    var total: i32 = 0;\n"
                                        "    for (var i: i32 = 0; i < n; i = i + 1) {\n"
                                        "        total = total + i;\n"
                                        "    }\n"
                                        "    return total;\n"
                                        "}\n"
                                        "fn count_down(n: i32): i32 {\n"
                                        "    var hits: i32 = 0;\n"
                                        "    for (var i: i32 = n; i > 0; i = i - 1) {\n"
                                        "        if (i == 3) {\n"
                                        "            continue;\n"
                                        "        }\n"
                                        "        hits = hits + 1;\n"
                                        "        if (hits > 5) {\n"
                                        "            break;\n"
                                        "        }\n"
                                        "    }\n"
                                        "    return hits;\n"
                                        "}\n"
                                        "fn main(): i32 {\n"
                                        "    return sum_to(5) * 10 + count_down(20);\n"
                                        "}\n");
    CHECK(r.ok, "for 3-clause form with init/cond/step compiles and runs");
    printf("EXIT CODE: %d\n", r.exitCode);
    CHECK_EQ(r.exitCode, 106, "init runs before the loop and step after each iteration");
}

static void test_named_struct_literal_and_defaults_runtime() {
    CodegenTest t;
    auto r = t.run("codegen-named-struct-literal.zith",
                   "struct Pair { left: i32 = 3, right: i32 = 4 }\n"
                   "fn main(): i32 {\n"
                   "    var named: Pair = Pair{right: 9, left: _};\n"
                   "    return named.left + named.right;\n"
                   "}\n");
    CHECK(r.ok, "Named struct literals with defaults compile and run");
    CHECK_EQ(r.exitCode, 12, "Named literals reorder fields and materialize defaults");
}

static size_t countOccurrences(const std::string &text, std::string_view needle) {
    size_t count = 0;
    size_t start = 0;
    while ((start = text.find(needle, start)) != std::string::npos) {
        ++count;
        start += needle.size();
    }
    return count;
}

static void test_trailing_void_call_is_emitted_once() {
    memory::Arena arena;
    Options opts(arena);
    opts.flags.emitIr(true);

    session::CompilationSession session(opts, "/tmp/codegen-trailing-void-call.zith");
    session.setBuffered(true);
    session.setAlwaysEmitObject(true);
    session.setContent("extern fn putchar(c: i32): i32\n"
                       "fn signal() {\n"
                       "    putchar(65)\n"
                       "}\n"
                       "fn main(): i32 {\n"
                       "    signal();\n"
                       "    return 0;\n"
                       "}\n");

    CHECK(session.run(), "Trailing void call reaches code generation");

    size_t hir_calls = 0;
    const auto &hir  = session.hirModule();
    for (size_t i = 0; i < hir.getFnCount(); ++i) {
        const auto &fn = hir.getFn(i);
        if (session.interner().lookup(fn.name).find("signal") == std::string_view::npos)
            continue;
        for (const auto &block : fn.blocks) {
            for (auto inst : block.insts) {
                if (std::get_if<hir::HirCall>(&hir.getExpr(inst)))
                    ++hir_calls;
            }
        }
    }
    CHECK_EQ(hir_calls, 1u, "Trailing call appears once in HIR instructions");

    auto output = session.flushOutput();
    CHECK_EQ(countOccurrences(output, "call i32 @putchar"), 1u,
             "Trailing call appears once in LLVM IR");
    CHECK(session.linkAndExec(), "Trailing void call links and executes");
    CHECK_EQ(session.childExitCode(), 0, "Trailing void call returns normally");
}

static void test_from_console_lowers_println_body() {
    CodegenTest t;
    t.opts.flags.emitIr(true);
    auto r = t.run("codegen-from-console.zith", "from std/io/console\n"
                                                "fn main(): i32 {\n"
                                                "    println(\"from import\");\n"
                                                "    return 0;\n"
                                                "}\n");
    CHECK(r.ok, "from std/io/console compiles and runs");
    CHECK(r.output.find("define void @\"std.io.console.println(*char)\"") != std::string::npos,
          "Imported println body is emitted into LLVM IR");
    CHECK(r.output.find("call i32 @puts") != std::string::npos,
          "Imported println body calls puts in LLVM IR");
}

static void test_console_alias_resolves_member_without_global_import() {
    CodegenTest aliased;
    aliased.opts.flags.emitIr(true);
    auto ok = aliased.run("codegen-console-alias.zith", "import std/io/console as console\n"
                                                        "fn main(): i32 {\n"
                                                        "    console.println(\"alias import\");\n"
                                                        "    return 0;\n"
                                                        "}\n");
    CHECK(ok.ok, "console.println resolves through an import alias");
    CHECK(ok.output.find("call void @\"std.io.console.println(*char)\"") != std::string::npos,
          "Alias import emits a call to the imported function");

    CodegenTest unqualified;
    auto missing =
        unqualified.run("codegen-console-alias-missing.zith", "import std/io/console as console\n"
                                                              "fn main() {\n"
                                                              "    println(\"not global\");\n"
                                                              "}\n");
    CHECK(!missing.ok, "Alias import does not expose println globally");
    CHECK(missing.errorCount > 0, "Unqualified println reports a diagnostic");
}

static void test_struct_type_has_fields_in_ir() {
    ModernFileCodegenTest t;
    t.opts.flags.emitIr(true);
    t.write("main.zith", "struct P { x: i32, y: i32, }\n"
                         "fn main(): i32 {\n"
                         "    var p: P = P { x: 1, y: 2, };\n"
                         "    return p.y;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "a struct with fields compiles and executes");
    CHECK_EQ(r.exitCode, 2, "reading a struct field returns the stored value");
    // Regression: struct types used to reach LLVM empty (`%zith.struct.0 = type {}`).
    CHECK(r.output.find("type {}") == std::string::npos,
          "no lowered struct type reaches LLVM without fields");
    CHECK(r.output.find("type { i32, i32 }") != std::string::npos,
          "the two-field struct lowers to an LLVM type carrying both fields");
}

static void test_struct_field_read_through_parameter() {
    ModernFileCodegenTest t;
    t.opts.flags.emitIr(true);
    t.write("main.zith", "struct P { x: i32, y: i32, }\n"
                         "fn get_y(p: P): i32 { return p.y; }\n"
                         "fn main(): i32 {\n"
                         "    var p: P = P { x: 4, y: 9, };\n"
                         "    return get_y(p);\n"
                         "}\n");

    auto r = t.run();
    // Regression: this used to fail with E5001 "Basic Block does not have terminator".
    CHECK(r.ok, "returning a struct field of a parameter produces valid IR");
    CHECK_EQ(r.exitCode, 9, "the field read through a struct parameter returns the right value");
    CHECK(r.output.find("@\"main.get_y(P)\"") != std::string::npos,
          "the accessor function is emitted with a qualified name");
}

static void test_duplicate_struct_field_names_do_not_collide_globally() {
    ModernFileCodegenTest t;
    t.write("main.zith", "struct A { value: i32 }\n"
                         "struct B { value: i32 }\n"
                         "fn main(): i32 {\n"
                         "    let a: A = A { value: 1 };\n"
                         "    let b: B = B { value: 2 };\n"
                         "    return a.value + b.value;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "homonymous struct fields use the modern frontend pipeline");
    CHECK(r.ok, "two structs with a field named 'value' compile in one module");
    CHECK_EQ(r.exitCode, 3, "reading homonymous fields returns the sum of both stored values");
}

static void test_f32_literal_stores_in_32_width() {
    ModernFileCodegenTest t;
    t.opts.flags.emitIr(true);
    t.write("main.zith", "fn main(): i32 {\n"
                         "    let f: f32 = 1.5;\n"
                         "    return f as i32;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "f32 literal initialization uses the modern frontend pipeline");
    CHECK(r.ok, "an f32 literal compiling, links and runs");
    CHECK_EQ(r.exitCode, 1, "float-to-int truncation of 1.5f32 yields 1");
    CHECK(r.output.find("alloca float") != std::string::npos,
          "the f32 binding is allocated as a 32-bit float slot");
    CHECK(r.output.find("store double") == std::string::npos,
          "an f32 literal is not stored as a double in the emitting module");
}

static void test_numeric_cast_codegen() {
    ModernFileCodegenTest t;
    t.opts.flags.emitIr(true);
    t.write("main.zith", "fn main(): i32 {\n"
                         "    var n: i32 = 21;\n"
                         "    let f: f64 = n as f64;\n"
                         "    return f as i32;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "numeric 'as' conversions compile and execute");
    CHECK_EQ(r.exitCode, 21, "round-tripping through f64 preserves the value");
    CHECK(r.output.find("sitofp") != std::string::npos, "i32 -> f64 emits sitofp");
    CHECK(r.output.find("fptosi") != std::string::npos, "f64 -> i32 emits fptosi");
}

static void test_marker_jump_loop_executes() {
    ModernFileCodegenTest t;
    t.write("main.zith", "flow fn main(): i32 {\n"
                         "    dock loop(0);\n"
                         "    marker loop(n: i32) {\n"
                         "        if (n < 10) {\n"
                         "            jump loop(n + 1);\n"
                         "        } else {\n"
                         "            return n;\n"
                         "        }\n"
                         "    }\n"
                         "    return -1;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "marker/jump loop compiles and executes");
    CHECK_EQ(r.exitCode, 10, "marker loop returns exactly 10");
}

static void test_marker_jump_returns_to_origin_dock() {
    ModernFileCodegenTest t;
    t.opts.flags.emitIr(true);
    t.write("main.zith", "extern fn printf(fmt: *char, ...): i32\n"
                         "flow fn foo(): i32 {\n"
                         "    marker Test() {\n"
                         "        printf(\"Second\\n\");\n"
                         "        jump Done();\n"
                         "    }\n"
                         "    marker Done() {\n"
                         "    }\n"
                         "    printf(\"First\\n\");\n"
                         "    dock Test();\n"
                         "    printf(\"Third\\n\");\n"
                         "    return 0;\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    foo();\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "the flow marker example compiles and runs");
    CHECK(r.output.find("First\nSecond\nThird\n") != std::string::npos,
          "jump marker resumes immediately after the originating dock");
    CHECK_EQ(r.exitCode, 0, "the flow marker example exits normally");
}

static void test_marker_jump_chain_returns_to_origin_dock() {
    ModernFileCodegenTest t;
    t.write("main.zith", "extern fn printf(fmt: *char, ...): i32\n"
                         "flow fn foo(): i32 {\n"
                         "    marker A() {\n"
                         "        printf(\"A\\n\");\n"
                         "        jump B();\n"
                         "    }\n"
                         "    marker B() {\n"
                         "        printf(\"B\\n\");\n"
                         "    }\n"
                         "    printf(\"Start\\n\");\n"
                         "    dock A();\n"
                         "    printf(\"End\\n\");\n"
                         "    return 0;\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    foo();\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "chained marker jumps compile and run");
    CHECK(r.output.find("Start\nA\nB\nEnd\n") != std::string::npos,
          "B resumes the outer dock that started A instead of resuming A");
    CHECK_EQ(r.exitCode, 0, "chained marker jumps exit normally");
}

static void test_marker_arguments_cross_chain_frames() {
    ModernFileCodegenTest t;
    t.opts.flags.emitIr(true);
    t.write("main.zith", "flow fn main(): i32 {\n"
                         "    marker A(v: i32) {\n"
                         "        jump B(v + 2);\n"
                         "    }\n"
                         "    marker B(v: i32) {\n"
                         "        return v;\n"
                         "    }\n"
                         "    dock A(10);\n"
                         "    return -1;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "marker arguments cross a chain of jumps");
    CHECK_EQ(r.exitCode, 12, "the last marker sees arguments from the final jump");
    CHECK(r.output.find("__zith_marker_blob") != std::string::npos,
          "marker blob is present in LLVM IR");
}

static void test_stackless_marker_has_no_host_slot_access() {
    ModernFileCodegenTest t;
    t.write("main.zith", "flow fn main(): i32 {\n"
                         "    var host: i32 = 5;\n"
                         "    dock body();\n"
                         "    marker body() {\n"
                         "        return host;\n"
                         "    }\n"
                         "    return 0;\n"
                         "}\n");

    auto r = t.run();
    CHECK(!r.ok, "stackless marker cannot capture the host flow fn's local slots");
}

static void test_stackful_marker_uses_local_binding() {
    ModernFileCodegenTest t;
    t.opts.flags.emitIr(true);
    t.write("main.zith", "flow fn main(): i32 {\n"
                         "    marker body() {\n"
                         "        stackful marker twice(x: i32) {\n"
                         "            var result: i32 = x * 2;\n"
                         "        }\n"
                         "        jump twice(3);\n"
                         "        return 0;\n"
                         "    }\n"
                         "    dock body();\n"
                         "    return -1;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "stackful marker with a local binding lowers and emits");
    CHECK(r.output.find("alloca i32") != std::string::npos,
          "stackful marker local binding becomes a real slot allocation");
    CHECK(r.output.find("__zith_marker_exit") != std::string::npos,
          "stackful sample falls back to marker exit instead of reaching a missing terminator");
}

static void test_stackless_marker_cannot_allocate_local_binding() {
    ModernFileCodegenTest t;
    t.write("main.zith", "flow fn main(): i32 {\n"
                         "    dock body(3);\n"
                         "    marker body(v: i32) {\n"
                         "        var twice: i32 = v * 2;\n"
                         "        return twice;\n"
                         "    }\n"
                         "    return 0;\n"
                         "}\n");

    auto r = t.run();
    CHECK(!r.ok, "stackless marker cannot allocate a local binding");
}

static void test_linked_list_acceptance_program() {
    // Mirrors examples/linked-list.zith: nullable pointers, `is null`, conditional `for`,
    // two-character comparison operators, and explicit numeric `as` conversions.
    ModernFileCodegenTest t;
    t.write("main.zith", "struct Node {\n"
                         "    value: i32,\n"
                         "    next: ?*Node,\n"
                         "}\n"
                         "fn sum(start: ?*Node): i32 {\n"
                         "    var total: i32 = 0;\n"
                         "    var cur: ?*Node = start;\n"
                         "    for (not (cur is null)) {\n"
                         "        total = total + cur->value;\n"
                         "        cur = cur->next;\n"
                         "    }\n"
                         "    return total;\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    var tail: Node = Node { value: 3, next: null };\n"
                         "    var head: Node = Node { value: 4, next: &tail };\n"
                         "    let total: i32 = sum(&head);\n"
                         "    if (total != 7) {\n"
                         "        return 1;\n"
                         "    }\n"
                         "    let scaled: f64 = total as f64;\n"
                         "    let back: i32 = scaled as i32;\n"
                         "    if (back == total) {\n"
                         "        return back;\n"
                         "    }\n"
                         "    return 2;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "the linked-list acceptance program compiles, links, and executes");
    CHECK_EQ(r.exitCode, 7, "traversing the list through ?*Node sums both node values");
}

static void test_modern_file_pipeline_executes_program() {
    ModernFileCodegenTest t;
    t.write("main.zith", "fn main(): i32 {\n"
                         "    return 31;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "Real-file codegen test used the modern frontend pipeline");
    CHECK(r.ok, "Real-file program compiles and executes");
    CHECK_EQ(r.exitCode, 31, "Real-file program preserves exit status");
}

static void test_modern_file_import_codegen_executes() {
    ModernFileCodegenTest t;
    t.write("math.zith", "pub fn add(a: i32, b: i32): i32 { a + b }\n");
    t.write("main.zith", "from math\n"
                         "fn main(): i32 {\n"
                         "    return add(20, 22);\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "Imported real-file program uses the modern frontend pipeline");
    CHECK(r.ok, "Imported real-file program compiles and executes");
    CHECK_EQ(r.exitCode, 42, "Imported function call returns the expected result");
}

static void test_modern_file_type_alias_codegen_executes() {
    ModernFileCodegenTest t;
    t.write("main.zith", "type MyInt = i32\n"
                         "alias AInt = i32\n"
                         "fn main(): i32 {\n"
                         "    var x: MyInt = 44 as MyInt;\n"
                         "    var y: AInt = 4;\n"
                         "    (x as i32) + y\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "Nominal/alias real-file program uses the modern frontend pipeline");
    CHECK(r.ok, "Nominal wrapper and transparent alias compile through modern codegen");
    CHECK_EQ(r.exitCode, 48, "Nominal wrapper round-trips and alias stays transparent");
}

static void test_run_emit_hir_still_executes() {
    CodegenTest t;
    t.opts.command = Options::Command::Run;
    t.opts.flags.emitHir(true);
    t.opts.deriveTargetStage();

    CHECK_EQ(t.opts.targetStage, session::Stage::Cached,
             "run --emit-hir keeps the pipeline target at code generation");

    auto r = t.run("codegen-run-emit-hir.zith", "fn main(): i32 {\n"
                                                "    return 29;\n"
                                                "}\n");
    CHECK(r.ok, "run --emit-hir still produces and runs an executable");
    CHECK_EQ(r.exitCode, 29, "run --emit-hir preserves program exit status");
}

static void test_emit_hir_static_method_dump() {
    ModernFileCodegenTest t;
    t.opts.targetStage = session::Stage::HirLowered;
    t.opts.flags.emitHir(true);
    t.write("main.zith", "struct Point { x: i32 }\n"
                         "trait Sample {}\n"
                         "implement Point as Sample {\n"
                         "    fn foo(): i32 { 7 }\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    Point.foo()\n"
                         "}\n");

    session::CompilationSession session(t.opts, (t.root / "main.zith").string());
    session.setBuffered(true);
    auto ok = session.runTo(session::Stage::HirLowered);
    CHECK(ok, "--emit-hir dumps a static method call without touching the invalid callee");
    auto output = session.flushOutput();
    CHECK(output.find("--- HIR ---") != std::string::npos &&
              output.find("call <resolved>(") != std::string::npos,
          "dumper prints the resolved static method call");
}

static void test_layout_api_matches_llvm() {
    memory::Arena arena;
    memory::StringInterner interner(arena);
    types::TypeIntern types(arena, interner);

    auto padded = types.defineStruct("Padded");
    types.addField(padded, "left", types.internInt(types::IntWidth::U8));
    types.addField(padded, "right", types.internInt(types::IntWidth::U32));

    auto tuple = types.defineStruct("Tuple");
    types.addField(tuple, "first", types.internInt(types::IntWidth::I32));
    types.addField(tuple, "second", types.internInt(types::IntWidth::I32));

    auto tuple_array = types.internArray(tuple, 3);
    auto color       = types.defineEnum("Color", types.internInt(types::IntWidth::U8));
    auto raw_union   = types.defineUnion("Bits", true);
    types.addUnionMember(raw_union, types.internInt(types::IntWidth::U8));
    types.addUnionMember(raw_union, types.internInt(types::IntWidth::U32));

    auto layout = codegen::makeTargetDataLayout({});
    CHECK(layout.has_value(), "Target data layout is available for LLVM codegen tests");
    if (!layout)
        return;

    llvm::LLVMContext llvm_ctx;
    codegen::CodeGenType type_gen(llvm_ctx, types, &*layout);

    auto *padded_llvm = llvm::cast<llvm::StructType>(type_gen.lower(padded));
    auto *layout_ref  = layout->getStructLayout(padded_llvm);
    CHECK_EQ(type_gen.sizeOf(padded), layout->getTypeAllocSize(padded_llvm).getFixedValue(),
             "Struct size matches LLVM DataLayout");
    CHECK_EQ(type_gen.alignOf(padded), layout->getABITypeAlign(padded_llvm).value(),
             "Struct ABI alignment matches LLVM DataLayout");
    CHECK_EQ(type_gen.fieldOffset(padded, "left"), layout_ref->getElementOffset(0),
             "First field offset matches LLVM StructLayout");
    CHECK_EQ(type_gen.fieldOffset(padded, "right"), layout_ref->getElementOffset(1),
             "Second field offset matches LLVM StructLayout");

    CHECK_EQ(type_gen.sizeOf(tuple_array), type_gen.sizeOf(tuple) * 3,
             "Array stride uses the ABI size of the struct element");
    CHECK_EQ(type_gen.sizeOf(color), type_gen.sizeOf(types.internInt(types::IntWidth::U8)),
             "Enum size matches its underlying integer type");
    CHECK_EQ(type_gen.alignOf(color), type_gen.alignOf(types.internInt(types::IntWidth::U8)),
             "Enum alignment matches its underlying integer type");

    auto *union_llvm = llvm::cast<llvm::StructType>(type_gen.lower(raw_union));
    CHECK_EQ(type_gen.sizeOf(raw_union), layout->getTypeAllocSize(union_llvm).getFixedValue(),
             "Raw union size matches its lowered LLVM storage struct");
    CHECK_EQ(type_gen.alignOf(raw_union), layout->getABITypeAlign(union_llvm).value(),
             "Raw union alignment matches its lowered LLVM storage struct");
    CHECK_EQ(type_gen.alignOf(raw_union), type_gen.alignOf(types.internInt(types::IntWidth::U32)),
             "Raw union alignment equals its maximum member alignment");
    CHECK_EQ(type_gen.sizeOf(raw_union), 4u, "Raw union storage size covers its widest member");
}

static void test_raw_union_runtime_reinterpret() {
    ModernFileCodegenTest t;
    t.write("main.zith", "extern fn printf(fmt: *char, ...): i32\n"
                         "raw union Bits { u8, u32 }\n"
                         "fn main(): u32 {\n"
                         "    var b: Bits = Bits { 255u8 };\n"
                         "    return b as u32;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "Raw union construction and member read run successfully");
    CHECK_EQ(r.exitCode, 255, "Raw union member read reinterprets the stored byte as u32");
}

/// Builds a tiny C static library that returns a `{ptr, len}` aggregate and
/// checks that a Zith slice parameter indexes it correctly at runtime.
static void test_slice_abi_matches_c_runtime() {
    ModernFileCodegenTest t;
    const auto c_path   = (t.root / "slice-abi.c").string();
    const auto lib_path = (t.root / "libzithsliceabi.a").string();
    const auto obj_path = (t.root / "slice-abi.o").string();
    {
        std::ofstream c_source(c_path, std::ios::binary | std::ios::trunc);
        c_source << "#include <stdint.h>\n"
                    "typedef struct { int32_t *ptr; int64_t len; } Slice;\n"
                    "static int32_t data[3] = {10, 20, 30};\n"
                    "Slice zith_test_slice(void) { Slice s = {data, 3}; return s; }\n";
    }
    const auto compile = "cc -c " + c_path + " -o " + obj_path + " 2>/dev/null";
    const auto archive = "ar rcs " + lib_path + " " + obj_path + " 2>/dev/null";
    if (std::system(compile.c_str()) != 0 || std::system(archive.c_str()) != 0) {
        std::printf("  SKIP: no C toolchain for the slice ABI runtime test\n");
        return;
    }

    t.opts.libraryDirs.push(t.root.string());
    t.opts.libraries.push("zithsliceabi");
    t.write("main.zith", "extern fn zith_test_slice(): []i32\n"
                         "fn main(): i32 {\n"
                         "    var s: []i32 = zith_test_slice();\n"
                         "    return s[1];\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "slice returned from C compiles, links and executes");
    CHECK_EQ(r.exitCode, 20, "indexing a C-provided slice reads the expected element");
}

static void test_optional_and_slice_layouts() {
    memory::Arena arena;
    memory::StringInterner interner(arena);
    types::TypeIntern types(arena, interner);

    const auto i32_type     = types.internInt(types::IntWidth::I32);
    const auto optional_i32 = types.internOptional(i32_type);
    const auto optional_ptr = types.internOptional(types.internPtr(i32_type, false));
    const auto slice_i32    = types.internSlice(i32_type);

    auto layout = codegen::makeTargetDataLayout({});
    CHECK(layout.has_value(), "Target data layout is available for optional/slice layout tests");
    if (!layout)
        return;

    llvm::LLVMContext llvm_ctx;
    codegen::CodeGenType type_gen(llvm_ctx, types, &*layout);

    auto *optional_llvm = type_gen.lower(optional_i32);
    CHECK(optional_llvm->isStructTy(), "?i32 lowers to a struct");
    if (auto *as_struct = llvm::dyn_cast<llvm::StructType>(optional_llvm)) {
        CHECK_EQ(as_struct->getNumElements(), 2u, "?i32 has a payload and a discriminant");
        CHECK(as_struct->getElementType(0)->isIntegerTy(32), "?i32 payload is an i32");
        CHECK(as_struct->getElementType(1)->isIntegerTy(1), "?i32 discriminant is an i1");
    }

    CHECK(type_gen.lower(optional_ptr)->isPointerTy(),
          "?*i32 uses the nullptr niche and stays a pointer");

    auto *slice_llvm = type_gen.lower(slice_i32);
    CHECK(slice_llvm->isStructTy(), "[]i32 lowers to a struct");
    if (auto *as_struct = llvm::dyn_cast<llvm::StructType>(slice_llvm)) {
        CHECK_EQ(as_struct->getNumElements(), 2u, "[]i32 is a pointer and a length");
        CHECK(as_struct->getElementType(0)->isPointerTy(), "[]i32 field 0 is the data pointer");
        CHECK(as_struct->getElementType(1)->isIntegerTy(64), "[]i32 field 1 is an i64 length");
        const auto *slice_layout = layout->getStructLayout(as_struct);
        CHECK_EQ(slice_layout->getElementOffset(0), 0u, "[]i32 data pointer is at offset 0");
        CHECK_EQ(slice_layout->getElementOffset(1),
                 layout->getTypeAllocSize(as_struct->getElementType(0)).getFixedValue(),
                 "[]i32 length follows the data pointer");
    }
}

static void test_struct_method_call_runtime() {
    CodegenTest t;
    auto r = t.run("codegen-struct-method-call.zith", "struct Counter {\n"
                                                      "    value: i32,\n"
                                                      "    fn bump(self, by: i32): i32 {\n"
                                                      "        return self->value + by;\n"
                                                      "    }\n"
                                                      "}\n"
                                                      "fn main(): i32 {\n"
                                                      "    let c: Counter = Counter { value: 5 };\n"
                                                      "    return c.bump(3);\n"
                                                      "}\n");
    CHECK(r.ok, "A struct-body method call compiles and runs");
    CHECK_EQ(r.exitCode, 8, "The method receives self and returns value + by");
}

static void test_implement_block_method_runtime() {
    CodegenTest t;
    auto r =
        t.run("codegen-implement-method-call.zith", "struct Point {\n"
                                                    "    x: i32,\n"
                                                    "    y: i32\n"
                                                    "}\n"
                                                    "implement Point {\n"
                                                    "    fn sum(self): i32 {\n"
                                                    "        return self->x + self->y;\n"
                                                    "    }\n"
                                                    "}\n"
                                                    "fn main(): i32 {\n"
                                                    "    let p: Point = Point { x: 4, y: 9 };\n"
                                                    "    return p.sum();\n"
                                                    "}\n");
    CHECK(r.ok, "An implement-block method call compiles and runs");
    CHECK_EQ(r.exitCode, 13, "The implicit self argument reaches the method body");
}

static void test_overloaded_functions_link_and_run() {
    ModernFileCodegenTest t;
    t.opts.flags.emitIr(true);
    t.write("main.zith", "fn add(a: i32, b: i32): i32 { a + b }\n"
                         "fn add(a: f64, b: f64): f64 { a + b }\n"
                         "fn add(a: i32, b: i32, c: i32): i32 { a + b + c }\n"
                         "fn main(): i32 {\n"
                         "    let f: f64 = add(1.5, 2.5);\n"
                         "    return add(add(10, 20), 6, 6);\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "an overloaded program compiles, links and runs");
    CHECK_EQ(r.exitCode, 42, "each call site reaches the overload selected by sema");
    // Overloads must not collide in the object file: distinct qualified symbols.
    CHECK(r.output.find("@\"main.add(i32,i32)\"") != std::string::npos,
          "the i32 overload is emitted under its qualified linkage name");
    CHECK(r.output.find("@\"main.add(f64,f64)\"") != std::string::npos,
          "the f64 overload is emitted under a distinct qualified linkage name");
    CHECK(r.output.find("@\"main.add(i32,i32,i32)\"") != std::string::npos,
          "the three-parameter overload is emitted under its own linkage name");
}

static void test_extern_variadic_call_runs() {
    ModernFileCodegenTest t;
    t.opts.flags.emitIr(true);
    t.write("main.zith", "extern fn printf(fmt: *char, ...): i32\n"
                         "fn main(): i32 {\n"
                         "    printf(\"n=%d\\n\", 7);\n"
                         "    return 0;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "extern variadic printf compiles, links and runs");
    CHECK(r.output.find("call i32 (ptr, ...) @printf") != std::string::npos,
          "the call reaches LLVM IR as a variadic invoke of printf");
    CHECK(r.output.find("declare i32 @printf(ptr, ...)") != std::string::npos,
          "LLVM IR declares printf as variadic");
}

static void test_c_default_arguments_and_string_escapes() {
    {
        ModernFileCodegenTest t;
        t.opts.flags.emitIr(true);
        t.write("main.zith", "extern fn printf(fmt: *char, ...): i32\n"
                             "fn main(): i32 {\n"
                             "    let f: f32 = 1.5;\n"
                             "    printf(\"%f\", f);\n"
                             "    return 0;\n"
                             "}\n");

        auto r = t.run();
        CHECK(r.ok, "an f32 variadic promotion test compiles and runs");
        CHECK(r.output.find("fpext float") != std::string::npos,
              "the f32 variadic argument is promoted to double in LLVM IR");
    }

    {
        CodegenTest t;
        auto r = t.run("codegen-escapes-char.zith", "extern fn printf(fmt: *char, ...): i32\n"
                                                    "fn main(): i32 {\n"
                                                    "    printf(\"v=%d\\n[%f]%c\", 42, 1.5, 'B');\n"
                                                    "    return 0;\n"
                                                    "}\n");
        CHECK(r.ok, "escaped strings, char literal args and promoted variadic args run");
        CHECK(r.output == "v=42\n[1.500000]B",
              "escaped newline decodes, char literals pass through %c, and variadic args print");
    }
}

// Program output must live in takeChildOutput(), not in the compiler's
// diagnostic buffer: `zithc run` writes the former to stdout after execution.
static void test_child_output_is_separate_from_compiler_output() {
    memory::Arena arena;
    Options opts(arena);
    session::CompilationSession session(opts, "/tmp/codegen-child-output-split.zith");
    session.setBuffered(true);
    session.setAlwaysEmitObject(true);
    session.setContent("extern fn printf(fmt: *char, ...): i32\n"
                       "fn main(): i32 {\n"
                       "    printf(\"child-says=%d\\n\", 3);\n"
                       "    return 0;\n"
                       "}\n");

    CHECK(session.run(), "child-output split program compiles");
    CHECK(session.linkAndExec(), "child-output split program links and executes");

    auto compilerOutput = session.flushOutput();
    CHECK(compilerOutput.find("child-says=3") == std::string::npos,
          "flushOutput() does not contain the program's output");

    auto childOutput = session.takeChildOutput();
    CHECK(childOutput == "child-says=3\n", "takeChildOutput() returns the program's bytes exactly");
    CHECK(session.takeChildOutput().empty(), "takeChildOutput() clears the captured buffer");
}

// `zithc run` executes the program with inherited stdio: nothing is captured,
// the bytes land on the parent's real stdout, and only the exit code is recorded.
static void test_direct_exec_inherits_parent_stdout() {
    memory::Arena arena;
    Options opts(arena);
    session::CompilationSession session(opts, "/tmp/codegen-direct-exec.zith");
    session.setBuffered(true);
    session.setAlwaysEmitObject(true);
    session.setContent("extern fn printf(fmt: *char, ...): i32\n"
                       "fn main(): i32 {\n"
                       "    printf(\"direct-says=%d\\n\", 7);\n"
                       "    return 12;\n"
                       "}\n");

    CHECK(session.run(), "direct-exec program compiles");

    // Redirect this process's stdout to a temp file so the inherited-fd write
    // can be observed instead of polluting the test log.
    const std::string capturePath = "/tmp/codegen-direct-exec-stdout.txt";
    std::fflush(stdout);
    const int savedStdout = dup(STDOUT_FILENO);
    const int captureFd   = open(capturePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(savedStdout >= 0 && captureFd >= 0, "stdout redirection for direct exec is set up");
    dup2(captureFd, STDOUT_FILENO);
    close(captureFd);

    const bool executed = session.linkAndExecDirect();

    std::fflush(stdout);
    dup2(savedStdout, STDOUT_FILENO);
    close(savedStdout);

    CHECK(executed, "direct-exec program links and executes");
    CHECK_EQ(session.childExitCode(), 12, "direct exec preserves the program exit code");
    CHECK(session.takeChildOutput().empty(), "direct exec captures no child bytes");

    auto compilerOutput = session.flushOutput();
    CHECK(compilerOutput.find("direct-says=7") == std::string::npos,
          "direct exec keeps program bytes out of the compiler output band");

    std::ifstream captured(capturePath, std::ios::binary);
    std::string inherited((std::istreambuf_iterator<char>(captured)),
                          std::istreambuf_iterator<char>());
    CHECK(inherited == "direct-says=7\n", "program bytes reach the inherited stdout verbatim");
    std::filesystem::remove(capturePath);
}

// A program that prints and then exits non-zero must still surface its output.
static void test_child_output_survives_nonzero_exit() {
    CodegenTest t;
    auto r = t.run("codegen-child-output-nonzero.zith", "extern fn printf(fmt: *char, ...): i32\n"
                                                        "fn main(): i32 {\n"
                                                        "    printf(\"before-failure\\n\");\n"
                                                        "    return -1;\n"
                                                        "}\n");
    CHECK(r.ok, "a program returning -1 still compiles, links and runs");
    CHECK_EQ(r.exitCode, 255, "return -1 is reported as exit code 255");
    CHECK(r.output.find("before-failure\n") != std::string::npos,
          "output printed before a non-zero exit is still captured");
}

static void test_import_stdio_runs() {
#ifdef ZITH_ENABLE_C_INTEROP
    ModernFileCodegenTest t;
    t.opts.flags.emitIr(true);
    t.write("main.zith", "import \"stdio.h\"\n"
                         "fn main(): i32 {\n"
                         "    printf(\"v=%d\\n\", 42);\n"
                         "    return 0;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "import \"stdio.h\" compiles, links and runs");
    CHECK_EQ(r.cacheHits, 0U, "first stdio.h run misses and writes a cache entry");
    CHECK(r.output.find("declare i32 @printf(ptr, ...)") != std::string::npos,
          "stdio.h's variadic printf is declared in LLVM IR");
    CHECK(r.output.find("v=42\n") != std::string::npos,
          "cold stdio.h program prints the expected payload");

    // A second run reuses the persistent artifact. The cache must restore the
    // foreign C signature exactly: `printf` needs `?*char`, which codegen
    // lowers to a bare `ptr`, not `{ i8, i1 }`.
    auto warm = t.run();
    CHECK(warm.ok, "import \"stdio.h\" reuses the persistent cache without IR errors");
    CHECK(warm.cacheHits > 0U, "second stdio.h run loads the persistent cache entry");
    CHECK(warm.output.find("declare i32 @printf(ptr, ...)") != std::string::npos,
          "warm printf declaration still uses the C pointer ABI");
    CHECK(warm.output.find("v=42\n") != std::string::npos,
          "warm stdio.h program prints the expected payload");

    CodegenTest plain;
    auto run = plain.run("codegen-import-stdio.zith", "import \"stdio.h\"\n"
                                                      "fn main(): i32 {\n"
                                                      "    printf(\"v=%d\\n\", 42);\n"
                                                      "    return 0;\n"
                                                      "}\n");
    CHECK(run.ok, "stdio.h import compiles and executes without IR output enabled");
    CHECK(run.output == "v=42\n",
          "stdio.h import builds, links, runs, and prints the decoded newline exactly");
#endif
}

/// `malloc` -> `as ?*i32` -> store/load -> `free`: both pointer casts are representation
/// preserving (LLVM pointers are opaque), so no conversion instruction may appear, and the
/// pointer must reach `free` directly.
static void test_c_pointer_cast_roundtrip_emits_no_conversion() {
#ifdef ZITH_ENABLE_C_INTEROP
    ModernFileCodegenTest t;
    t.opts.flags.emitIr(true);
    t.write("main.zith", "import \"stdio.h\"\n"
                         "import \"stdlib.h\"\n"
                         "fn main(): i32 {\n"
                         "    let cell: ?*i32 = malloc(64) as ?*i32;\n"
                         "    let slot: *i32 = cell;\n"
                         "    *slot = 42;\n"
                         "    printf(\"v=%d\\n\", *slot);\n"
                         "    free(cell);\n"
                         "    return 0;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "malloc + 'as ?*i32' + free compiles, links and runs");
    CHECK_EQ(r.exitCode, 0, "the pointer roundtrip exits cleanly");
    CHECK(r.output.find("v=42\n") != std::string::npos,
          "the value stored through the cast pointer is read back");
    CHECK(r.output.find("inttoptr") == std::string::npos,
          "a pointer-to-pointer cast emits no inttoptr");
    CHECK(r.output.find("ptrtoint") == std::string::npos,
          "a pointer-to-pointer cast emits no ptrtoint");
    CHECK(r.output.find("bitcast") == std::string::npos,
          "a pointer-to-pointer cast emits no bitcast");
    CHECK(r.output.find("{ ptr, i1 }") == std::string::npos,
          "?*T stays a bare pointer through the cast, with no tagged struct");
    CHECK_EQ(r.errorCount, 0u, "the emitted module passes LLVM verification");
#endif
}

/// A C pointer is `?*T`, so `is null` is the canonical null check. The niche layout means the
/// comparison must be against a bare `null` pointer, with no optional tag struct involved.
static void test_c_pointer_is_null_uses_niche_comparison() {
#ifdef ZITH_ENABLE_C_INTEROP
    ModernFileCodegenTest t;
    t.opts.flags.emitIr(true);
    t.write("main.zith", "import \"stdio.h\"\n"
                         "fn main(): i32 {\n"
                         "    let f = fopen(\"/definitely/not/here\", \"r\");\n"
                         "    if (f is null) {\n"
                         "        return 0;\n"
                         "    }\n"
                         "    fclose(f);\n"
                         "    return 1;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "fopen + 'is null' compiles, links and runs");
    CHECK_EQ(r.exitCode, 0, "the failed fopen is detected as null at runtime");
    CHECK(r.output.find("icmp eq ptr") != std::string::npos,
          "'is null' on a C pointer compares the pointer itself");
    CHECK(r.output.find("null") != std::string::npos, "the comparison is against a null pointer");
    // An optional with a tag would lower to `{ ptr, i1 }` and be built with insertvalue.
    CHECK(r.output.find("{ ptr, i1 }") == std::string::npos,
          "?*T uses the pointer niche, not a tagged struct");
    // `emit` verifies the whole module, so a verification failure would have been reported.
    CHECK_EQ(r.errorCount, 0u, "the emitted module passes LLVM verification");
#endif
}

/// A radix literal used to infer as `error`, emit no value, and leave `entry` without a
/// terminator, which crashed inside LLVM's MachineBasicBlock construction. Codegen must now
/// produce a valid module for it.
static void test_radix_literal_return_emits_valid_module() {
    CodegenTest t;
    auto r = t.run("codegen-radix-return.zith", "fn main(): i32 {\n"
                                                "    return 0x2A;\n"
                                                "}\n");
    CHECK(r.ok, "a hex literal return compiles, links and runs");
    CHECK_EQ(r.exitCode, 42, "0x2A returns 42");
}

/// The invariant this guards: a module that fails IR verification is never handed to a
/// TargetMachine. `emitObject`/`emitAsm`/`printAsm` must refuse instead of running the
/// PassManager, which crashes rather than diagnosing invalid IR.
static void test_invalid_ir_refuses_object_emission() {
    memory::Arena arena;
    // The default-constructed interner has no arena; it must be built from one.
    memory::StringInterner interner(arena);
    types::TypeIntern types(arena, interner);
    hir::HirModule hir(arena);

    // A function whose body cannot be emitted: the return operand has the error type, so
    // `emitLiteral` yields nullptr and the block's terminator never materialises.
    auto &fn       = hir.addFn(interner.intern("broken"));
    fn.return_type = types.internInt(types::IntWidth::I32);
    const auto bad_literal =
        hir.addExpr(hir::HirLiteral{types::kErrorType, {}, hir::HirExprKind::Literal});
    auto &block      = fn.blocks.emplace(arena);
    block.terminator = hir.addExpr(hir::HirRet{bad_literal});

    diagnostics::DiagnosticEngine diags(arena);
    codegen::CodeGen cg(interner, types, {}, 0, &diags);
    cg.emit(hir, "broken-module");

    CHECK(cg.hasInvalidIR(), "a body that fails to emit marks the module as invalid IR");

    const std::string obj = (std::filesystem::temp_directory_path() / "zith-invalid-ir.o").string();
    std::filesystem::remove(obj);
    CHECK(!cg.emitObject(obj), "emitObject refuses an invalid module");
    CHECK(!std::filesystem::exists(obj), "no object file is produced for an invalid module");
    CHECK(!cg.emitAsm(obj), "emitAsm refuses an invalid module");
    CHECK(cg.printAsm().empty(), "printAsm refuses an invalid module");

    bool refused = false;
    for (const auto &d : diags.all()) {
        if (d.message.find("refusing to") != std::string::npos)
            refused = true;
    }
    CHECK(refused, "the refusal is reported as a diagnostic rather than crashing");

    // Even on the failure path the module itself stays well-formed: no unterminated block.
    CHECK(cg.printIR().find("unreachable") != std::string::npos,
          "the unemittable block is closed with 'unreachable' instead of left open");
}

static void test_codegen() {
    setbuf(stdout, NULL);
    printf("Running test_return_literal\n");
    test_return_literal();
    printf("Running test_ref_deref_local\n");
    test_ref_deref_local();
    printf("Running test_ref_deref_param\n");
    test_ref_deref_param();
    printf("Running test_pointer_parameter_call\n");
    test_pointer_parameter_call();
    printf("Running test_double_pointer_roundtrip\n");
    test_double_pointer_roundtrip();
    printf("Running test_deref_ref_expression_chain\n");
    test_deref_ref_expression_chain();
    printf("Running test_unsigned_comparison\n");
    test_unsigned_comparison();
    printf("Running test_forward_reference\n");
    test_forward_reference();
    printf("Running test_pointer_index\n");
    test_pointer_index();
    printf("Running test_array_variable_indexing\n");
    test_array_variable_indexing();
    printf("Running test_shifts\n");
    test_shifts();
    test_compound_assign_runtime();
    test_raw_opaque_round_trip_runtime();
    printf("Running test_struct_fields_and_parameter\n");
    test_struct_fields_and_parameter();
    printf("Running test_array_of_structs\n");
    test_array_of_structs();
    printf("Running test_enum_values\n");
    test_enum_values();
    printf("Running test_offsetof_and_alignof_runtime\n");
    test_offsetof_and_alignof_runtime();
    test_sizeof_intrinsic_runtime();
    test_when_expression_runtime();
    test_for_three_clause_runtime();
    printf("Running test_named_struct_literal_and_defaults_runtime\n");
    test_named_struct_literal_and_defaults_runtime();
    printf("Running test_trailing_void_call_is_emitted_once\n");
    test_trailing_void_call_is_emitted_once();
    printf("Running test_from_console_lowers_println_body\n");
    test_from_console_lowers_println_body();
    printf("Running test_console_alias_resolves_member_without_global_import\n");
    test_console_alias_resolves_member_without_global_import();
    printf("Running test_struct_type_has_fields_in_ir\n");
    test_struct_type_has_fields_in_ir();
    printf("Running test_overloaded_functions_link_and_run\n");
    test_overloaded_functions_link_and_run();
    printf("Running test_struct_field_read_through_parameter\n");
    test_struct_field_read_through_parameter();
    printf("Running test_duplicate_struct_field_names_do_not_collide_globally\n");
    test_duplicate_struct_field_names_do_not_collide_globally();
    printf("Running test_f32_literal_stores_in_32_width\n");
    test_f32_literal_stores_in_32_width();
    printf("Running test_numeric_cast_codegen\n");
    test_numeric_cast_codegen();
    printf("Running test_marker_jump_loop_executes\n");
    test_marker_jump_loop_executes();
    printf("Running test_marker_jump_returns_to_origin_dock\n");
    test_marker_jump_returns_to_origin_dock();
    printf("Running test_marker_jump_chain_returns_to_origin_dock\n");
    test_marker_jump_chain_returns_to_origin_dock();
    printf("Running test_marker_arguments_cross_chain_frames\n");
    test_marker_arguments_cross_chain_frames();
    printf("Running test_stackless_marker_has_no_host_slot_access\n");
    test_stackless_marker_has_no_host_slot_access();
    printf("Running test_stackful_marker_uses_local_binding\n");
    test_stackful_marker_uses_local_binding();
    printf("Running test_stackless_marker_cannot_allocate_local_binding\n");
    test_stackless_marker_cannot_allocate_local_binding();
    printf("Running test_linked_list_acceptance_program\n");
    test_linked_list_acceptance_program();
    printf("Running test_modern_file_pipeline_executes_program\n");
    test_modern_file_pipeline_executes_program();
    printf("Running test_modern_file_import_codegen_executes\n");
    test_modern_file_import_codegen_executes();
    printf("Running test_modern_file_type_alias_codegen_executes\n");
    test_modern_file_type_alias_codegen_executes();
    printf("Running test_run_emit_hir_still_executes\n");
    test_run_emit_hir_still_executes();
    printf("Running test_emit_hir_static_method_dump\n");
    test_emit_hir_static_method_dump();
    printf("Running test_struct_method_call_runtime\n");
    test_struct_method_call_runtime();
    printf("Running test_implement_block_method_runtime\n");
    test_implement_block_method_runtime();
    printf("Running test_extern_variadic_call_runs\n");
    test_extern_variadic_call_runs();
    printf("Running test_c_default_arguments_and_string_escapes\n");
    test_c_default_arguments_and_string_escapes();
    printf("Running test_child_output_is_separate_from_compiler_output\n");
    test_child_output_is_separate_from_compiler_output();
    printf("Running test_direct_exec_inherits_parent_stdout\n");
    test_direct_exec_inherits_parent_stdout();
    printf("Running test_child_output_survives_nonzero_exit\n");
    test_child_output_survives_nonzero_exit();
    printf("Running test_import_stdio_runs\n");
    test_import_stdio_runs();
    printf("Running test_c_pointer_is_null_uses_niche_comparison\n");
    test_c_pointer_cast_roundtrip_emits_no_conversion();
    test_c_pointer_is_null_uses_niche_comparison();
    printf("Running test_radix_literal_return_emits_valid_module\n");
    test_radix_literal_return_emits_valid_module();
    printf("Running test_invalid_ir_refuses_object_emission\n");
    test_invalid_ir_refuses_object_emission();
    printf("Running test_layout_api_matches_llvm\n");
    test_layout_api_matches_llvm();
    printf("Running test_raw_union_runtime_reinterpret\n");
    test_raw_union_runtime_reinterpret();
    printf("Running test_optional_and_slice_layouts\n");
    test_optional_and_slice_layouts();
    printf("Running test_slice_abi_matches_c_runtime\n");
    test_slice_abi_matches_c_runtime();
}

TEST_MAIN(codegen)
