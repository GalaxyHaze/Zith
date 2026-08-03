#include "cli/options.hpp"
#include "codegen/codegen-type.hpp"
#include "hir/hir-expr.hpp"
#include "memory/arena.hpp"
#include "session/compilation-session.hpp"
#include "test-common.hpp"
#include "types/type-intern.hpp"

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

        return {ok && errs == 0, errs, session.childExitCode(), session.flushOutput()};
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

        return {ok && errs == 0, session.snapshot() != nullptr, errs, session.childExitCode(),
                session.flushOutput()};
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
        if (session.interner().lookup(fn.name) != "signal")
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
    CHECK(r.output.find("define void @println") != std::string::npos,
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
    CHECK(ok.output.find("call void @println") != std::string::npos,
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
    CHECK(r.output.find("@get_y") != std::string::npos, "the accessor function is emitted");
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
    t.write("main.zith", "fn main(): i32 {\n"
                         "    var x: i32 = 0;\n"
                         "    marker my_loop {\n"
                         "        x = x + 1;\n"
                         "        if (x < 10) {\n"
                         "            jump my_loop;\n"
                         "        }\n"
                         "    }\n"
                         "    return x;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "marker/jump loop compiles and executes");
    CHECK_EQ(r.exitCode, 10, "the loop runs until x reaches 10");
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
                         "fn main(): MyInt {\n"
                         "    var x: MyInt = 44;\n"
                         "    x\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "Type-alias real-file program uses the modern frontend pipeline");
    CHECK(r.ok, "Type alias program compiles and executes through modern codegen");
    CHECK_EQ(r.exitCode, 44, "Type alias lowers to the aliased runtime representation");
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
    printf("Running test_struct_field_read_through_parameter\n");
    test_struct_field_read_through_parameter();
    printf("Running test_numeric_cast_codegen\n");
    test_numeric_cast_codegen();
    printf("Running test_marker_jump_loop_executes\n");
    test_marker_jump_loop_executes();
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
    printf("Running test_layout_api_matches_llvm\n");
    test_layout_api_matches_llvm();
    printf("Running test_optional_and_slice_layouts\n");
    test_optional_and_slice_layouts();
    printf("Running test_slice_abi_matches_c_runtime\n");
    test_slice_abi_matches_c_runtime();
}

TEST_MAIN(codegen)
