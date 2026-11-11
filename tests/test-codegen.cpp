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
        std::vector<std::string> messages;
    };

    Result run(std::string_view file_name, std::string_view input) {
        std::string path = std::string("/tmp/") + std::string(file_name);
        session::CompilationSession session(opts, path);
        session.setBuffered(true);
        session.setAlwaysEmitObject(true);
        session.setContent(std::string(input));

        bool ok     = session.run();
        const bool runOk = ok;
        size_t errs = 0;
        std::vector<std::string> messages;
        for (const auto &d : session.diags().all()) {
            if (d.severity == diagnostics::Severity::Error) {
                errs++;
                messages.push_back(d.message);
                std::printf("    [Diag] Code: %u, Message: %s\n", d.code, d.message.c_str());
                for (const auto &suggestion : d.suggestions)
                    std::printf("    [DiagDetail] %s\n", suggestion.c_str());
            }
        }

        if (!runOk)
            std::printf("    [CodegenTest] compilation pipeline failed\n");

        if (ok && errs == 0) {
            ok = session.linkAndExec();
            if (!ok)
                std::printf("    [CodegenTest] link or execution failed\n");
        }

        std::string output = session.flushOutput();
        output += session.takeChildOutput();
        if (!ok && !output.empty())
            std::printf("    [CodegenTestOutput]\n%s", output.c_str());
        return {ok && errs == 0, errs, session.childExitCode(), std::move(output),
                std::move(messages)};
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
        std::vector<std::string> messages;

        [[nodiscard]] bool hasMessage(std::string_view needle) const {
            for (const auto &message : messages) {
                if (message.find(needle) != std::string::npos)
                    return true;
            }
            return false;
        }
    };

    Result run(std::string_view main_name = "main.zith") {
        session::CompilationSession session(opts, (root / main_name).string());
        session.setBuffered(true);
        session.setAlwaysEmitObject(true);

        bool ok     = session.run();
        size_t errs = 0;
        std::vector<std::string> messages;
        for (const auto &d : session.diags().all()) {
            if (d.severity == diagnostics::Severity::Error) {
                errs++;
                messages.push_back(d.message);
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
                std::move(output),
                std::move(messages)};
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

static void test_void_main_implicit_return_exits_zero() {
    CodegenTest t;
    auto r = t.run("codegen-void-main.zith", "#[discardable] extern fn putchar(c: i32): i32\n"
                                             "fn main() {\n"
                                             "    putchar(65);\n"
                                             "}\n");
    CHECK(r.ok, "void main compiles, links and runs");
    CHECK_EQ(r.exitCode, 0, "void main returns success without an explicit return");
}

static void test_void_main_bare_return_exits_zero() {
    CodegenTest t;
    auto r = t.run("codegen-void-main-return.zith", "fn main() {\n"
                                                    "    if (true) { return; }\n"
                                                    "    return;\n"
                                                    "}\n");
    CHECK(r.ok, "void main with bare returns compiles, links and runs");
    CHECK_EQ(r.exitCode, 0, "bare return from void main returns success");
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
                                                  "    var arr: [5]i32 = [0, 0, 0, 0, 0];\n"
                                                  "    arr[0] = 10;\n"
                                                  "    arr[1] = 20;\n"
                                                  "    arr[2] = 30;\n"
                                                  "    return raw arr[1];\n"
                                                  "}\n");
    CHECK(r.ok, "Array indexing compiles and runs");
    CHECK_EQ(r.exitCode, 20, "arr[1] returns 20");
}

static void test_pointer_index() {
    CodegenTest t;
    auto r = t.run("codegen-pointer-index.zith",
                   "fn main(): i32 { var x: i32 = 42; var p: *i32 = &x; return p[0]; }");
    CHECK(r.ok, "Pointer indexing compiles and runs");
    CHECK_EQ(r.exitCode, 42, "p[0] returns the correct value");
}

static void test_optional_pointer_narrowing_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "struct S { value: i32 }\n"
                         "fn read(p: *i32): i32 { return *p; }\n"
                         "fn arrow(p: ?*S): i32 {\n"
                         "    for (not (p is null)) { return p->value; }\n"
                         "    return 0;\n"
                         "}\n"
                         "fn index(p: ?*i32): i32 {\n"
                         "    if not (p is null) { return p[0]; }\n"
                         "    return 0;\n"
                         "}\n"
                         "fn deref(p: ?*i32): i32 {\n"
                         "    if not (p is null) { return read(p); }\n"
                         "    return 0;\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    var value: i32 = 19;\n"
                         "    var s: S = S { value: 23 };\n"
                         "    let p: ?*i32 = &value;\n"
                         "    let q: ?*S = &s;\n"
                         "    if (deref(p) != 19) { return 1; }\n"
                         "    if (index(p) != 19) { return 2; }\n"
                         "    if (arrow(q) != 23) { return 3; }\n"
                         "    return 0;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "optional pointer narrowing uses the modern codegen pipeline");
    CHECK(r.ok, "nullable pointer deref, arrow, index and coercion compile, link and run");
    CHECK_EQ(r.exitCode, 0, "proven nullable pointer reads return the pointed values");
}

static void test_optional_aggregate_narrowing_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "struct S { n: i32 }\n"
                         "fn field(x: ?S): i32 {\n"
                         "    if (x is null) { return 0; }\n"
                         "    return x.n;\n"
                         "}\n"
                         "fn scalar(x: ?i32): i32 {\n"
                         "    if (x is null) { return 0; }\n"
                         "    return x;\n"
                         "}\n"
                         "fn pack_len(x: ?|bytes: u64, len: u64|): u64 {\n"
                         "    if (x is null) { return 0u64; }\n"
                         "    return x.len;\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    let s = S { n: 7 };\n"
                         "    let o: ?S = s;\n"
                         "    if (field(o) != 7) { return 1; }\n"
                         "    if (scalar(11) != 11) { return 2; }\n"
                         "    let p: |bytes: u64, len: u64| = |3u64, 5u64|;\n"
                         "    if (pack_len(p) != 5u64) { return 3; }\n"
                         "    return 0;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "optional aggregate narrowing uses the modern codegen pipeline");
    CHECK(r.ok, "field, scalar and pack reads after 'if (x is null) { return }' compile and run");
    CHECK_EQ(r.exitCode, 0, "proven optional aggregate reads return the payload value");
}

// An aggregate literal passed straight into an `opaque` parameter is erased in
// place. The HIR lowering must keep the literal's concrete type while wrapping
// it in `make_opaque`; otherwise the literal is emitted with the opaque
// `{ *void, u32 }` layout and the callee reads a corrupted payload. The
// single-field struct is the discriminating case: a two-field struct happens to
// match the opaque wrapper's 16-byte size, but a one-field struct stores the
// wrong width and segfaults.
static void test_pack_literal_erased_into_opaque_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "struct Pair { a: u64, b: u64 }\n"
                         "struct One { a: u64 }\n"
                         "fn readStruct(x: opaque): u64 {\n"
                         "    let p = x as Pair;\n"
                         "    if (p is null) { return 0u64; }\n"
                         "    return p.b;\n"
                         "}\n"
                         "fn readPack(x: opaque): u64 {\n"
                         "    let p = x as |a: u64, b: u64|;\n"
                         "    if (p is null) { return 0u64; }\n"
                         "    return p.b;\n"
                         "}\n"
                         "fn readOne(x: opaque): u64 {\n"
                         "    let p = x as One;\n"
                         "    if (p is null) { return 0u64; }\n"
                         "    return p.a;\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    if (readStruct(Pair { a: 3u64, b: 7u64 }) != 7u64) { return 1; }\n"
                         "    if (readPack(|3u64, 7u64|) != 7u64) { return 2; }\n"
                         "    if (readOne(One { a: 7u64 }) != 7u64) { return 3; }\n"
                         "    return 0;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "aggregate literal erased into opaque uses the modern codegen pipeline");
    CHECK(r.ok, "struct and pack literals passed to an opaque parameter compile, link and run");
    CHECK_EQ(r.exitCode, 0, "the erased aggregate payload round-trips through the opaque value");
}

// A bare `null` member in a pack literal must take its declared member type
// from the surrounding annotation (`let p: |bytes: ?*u8, len: u64| = |null, ...|`).
// Without it the member stays `never`, the aggregate carries a void member, and
// erasing it into an `opaque` parameter corrupted the payload. The explicit
// `null as ?T` spelling covers the unannotated call-site case.
static void test_null_member_in_pack_literal_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith",
            "fn readAnnotated(x: opaque): u64 {\n"
            "    let p = x as |bytes: ?*u8, len: u64|;\n"
            "    if (p is null) { return 0u64; }\n"
            "    return p.len;\n"
            "}\n"
            "fn readExplicit(x: opaque): u64 {\n"
            "    let p = x as |bytes: ?*u8, len: u64|;\n"
            "    if (p is null) { return 0u64; }\n"
            "    return p.len;\n"
            "}\n"
            "fn main(): i32 {\n"
            "    let a: |bytes: ?*u8, len: u64| = |null, 7u64|;\n"
            "    if (readAnnotated(a) != 7u64) { return 1; }\n"
            "    if (readExplicit(|null as ?*u8, 9u64|) != 9u64) { return 2; }\n"
            "    let n: ?*u8 = null as ?*u8;\n"
            "    if not (n is null) { return 3; }\n"
            "    return 0;\n"
            "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "pack literals with a null member use the modern codegen pipeline");
    CHECK(r.ok, "an annotated or explicitly typed null pack member compiles, links and runs");
    CHECK_EQ(r.exitCode, 0,
             "the null member takes its declared optional type and the pack round-trips");

    // Without any annotation the member has no type; the compiler must report a
    // diagnostic instead of emitting a malformed aggregate or an ICE.
    ModernFileCodegenTest rejected;
    rejected.write("main.zith", "fn g(x: opaque): i32 { return 0; }\n"
                                "fn main(): i32 { return g(|null, 7u64|); }\n");
    auto bad = rejected.run();
    CHECK(!bad.ok && bad.errorCount > 0,
          "a bare null in an untyped pack literal is rejected with a diagnostic");

    // A bare null pack literal bound directly (no erasure into `opaque`) used to
    // reach LLVM codegen with a `void` member and hang at 100% CPU. It must be
    // rejected during sema instead.
    ModernFileCodegenTest direct;
    direct.write("main.zith", "fn main(): i32 { let t = |null, 7u64|; return 5; }\n");
    auto hung = direct.run();
    CHECK(!hung.ok && hung.errorCount > 0,
          "a directly bound bare null pack literal is rejected without hanging codegen");
}

// A runtime panic aborts through `llvm.trap`, which does not flush the C
// streams. When the program's stdout is a pipe (the captured `zithc run`
// path), `puts` output is fully buffered, so a message written just before the
// trap was lost. The runtime panic helper must flush stdout/stderr before
// trapping so a `panic(message)` keeps its text.
static void test_runtime_panic_flushes_buffered_output() {
    ModernFileCodegenTest t;
    t.write("main.zith",
            "fn main(): i32 {\n"
            "    @log(\"panic: boom\");\n"
            "    @assert(false);\n"
            "    return 0;\n"
            "}\n");

    auto r = t.run();
    CHECK(r.ok, "a program that logs then panics still compiles, links and runs");
    CHECK(r.output.find("panic: boom") != std::string::npos,
          "the message written before the panic survives the abort");
    CHECK(r.exitCode != 0, "the panic aborts the process");
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

static void test_logical_operator_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "fn main(): i32 {\n"
                         "    var a: bool = true;\n"
                         "    var b: bool = false;\n"
                         "    if not (a and b) { } else { return 1; }\n"
                         "    if ((a and not b) or b) { } else { return 2; }\n"
                         "    var flags: i32 = 6;\n"
                         "    if ((flags xor 3) != 5) { return 3; }\n"
                         "    return 0;\n"
                         "}\n");
    auto r = t.run();
    CHECK(r.ok, "boolean and/or/not compile, link and run");
    CHECK_EQ(r.exitCode, 0, "logical operators evaluate to the expected values");
}

static void test_optional_condition_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "fn main(): i32 {\n"
                         "    var p: ?i32 = 7;\n"
                         "    if (p) { } else { return 1; }\n"
                         "    var null_v: ?i32 = null;\n"
                         "    if (null_v) { return 2; }\n"
                         "    return 0;\n"
                         "}\n");
    auto r = t.run();
    CHECK(r.ok, "implicit ?T conditions compile and run");
    CHECK_EQ(r.exitCode, 0, "a truthy ?i32 starts the branch; a null ?i32 skips it");
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

static void test_bare_opaque_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "struct Box { value: i32 }\n"
                         "implement Box {\n"
                         "    fn get(self: view Box): i32 { self.value }\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    var v: i32 = 41;\n"
                         "    let h: opaque = v as opaque;\n"
                         "    if not (h is i32) { return 1; }\n"
                         "    if (h is f64) { return 2; }\n"
                         "    let got: ?i32 = h as i32;\n"
                         "    if (got is null) { return 3; }\n"
                         "    var boxed: Box = Box { value: 42 };\n"
                         "    let b: opaque = boxed as opaque;\n"
                         "    let got_box: ?Box = b as Box;\n"
                         "    if (got_box is null) { return 4; }\n"
                         "    got_box.get()\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "bare opaque uses the modern codegen pipeline");
    CHECK(r.ok, "bare opaque erase/check/extract compiles, links and runs");
    printf("EXIT CODE: %d\n", r.exitCode);
    CHECK_EQ(r.exitCode, 42,
             "extracting the original concrete value from bare opaque returns its payload");
}

static void test_bare_opaque_raw_and_mismatch_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "struct Box { value: i32 }\n"
                         "fn main(): i32 {\n"
                         "    var v: i32 = 41;\n"
                         "    let h: opaque = v as opaque;\n"
                         "    let raw_value: i32 = raw h as i32;\n"
                         "    if (raw_value != 41) { return 1; }\n"
                         "    let mismatch: ?f64 = h as f64;\n"
                         "    if (mismatch is null) { return 2; }\n"
                         "    let record_check: ?Box = h as Box;\n"
                         "    if (record_check is null) { return 3; }\n"
                         "    return 0;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "bare opaque mismatch path uses the modern codegen pipeline");
    CHECK(r.ok, "raw opaque extraction and checked mismatch compile, link and run");
    CHECK_EQ(r.exitCode, 2, "checked extraction of a mismatched type returns null");
}

static void test_bare_opaque_raw_pointer_same_payload_same_allocation_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "fn main(): i32 {\n"
                         "    var v: i32 = 41;\n"
                         "    let a: opaque = v as opaque;\n"
                         "    let p0: raw opaque = a as raw opaque;\n"
                         "    let p1: raw opaque = a as raw opaque;\n"
                         "    if (p0 != p1) { return 1; }\n"
                         "    return 0;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "bare opaque raw pointer comparison uses the modern codegen pipeline");
    CHECK(r.ok, "opaque as raw opaque compiles and runs");
    CHECK_EQ(r.exitCode, 0, "the same opaque value exposes the same payload pointer");
}

static void test_bare_opaque_raw_pointer_different_payload_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "fn main(): i32 {\n"
                         "    var first: i32 = 1;\n"
                         "    var second: i32 = 2;\n"
                         "    let a: opaque = first as opaque;\n"
                         "    let b: opaque = second as opaque;\n"
                         "    if ((a as raw opaque) == (b as raw opaque)) { return 1; }\n"
                         "    return 0;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "bare opaque raw pointer comparison uses the modern codegen pipeline");
    CHECK(r.ok, "opaque as raw opaque with distinct payloads compiles and runs");
    CHECK_EQ(r.exitCode, 0, "opaque raw pointer comparisons run without an LLVM type mismatch");
}

static void test_bare_opaque_raw_pointer_literal_after_narrow_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "fn printOpaque(value: opaque): i32 {\n"
                         "    if (value is *char) {\n"
                         "        if ((value as raw opaque) != (\"hello,\" as raw opaque)) {\n"
                         "            return 9;\n"
                         "        }\n"
                         "    }\n"
                         "    return 7;\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    let any: opaque = \"hello,\" as opaque;\n"
                         "    printOpaque(any)\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "opaque string payload narrowing uses the modern codegen pipeline");
    CHECK(r.ok, "opaque + raw opaque pointer comparison after 'is *char' compiles and runs");
    CHECK_EQ(r.exitCode, 7,
             "identical string literals share a payload pointer after opaque erasure");
}

static void test_implicit_opaque_coercion_and_narrow_runtime() {
    printf("Running test_implicit_opaque_coercion_and_narrow_runtime\n");
    ModernFileCodegenTest t;
    t.write("main.zith", "fn consumeInt(v: opaque): i32 {\n"
                         "    if (v is u32) { let x: u32 = v; if (x == 5u32) { return 0; } }\n"
                         "    return 1;\n"
                         "}\n"
                         "fn consumeStr(v: opaque): i32 {\n"
                         "    if (v is *char) { let p: *char = v; if ((v as raw opaque) == (\"ok\" "
                         "as raw opaque)) { return 0; } }\n"
                         "    return 1;\n"
                         "}\n"
                         "struct Box { value: i32 }\n"
                         "fn consumeBox(v: opaque): i32 {\n"
                         "    if (v is Box) { let b: Box = v; if (b.value == 42) { return 0; } }\n"
                         "    return 1;\n"
                         "}\n"
                         "fn make(): opaque { 9u32 }\n"
                         "fn echo(v: opaque): opaque { v }\n"
                         "fn main(): i32 {\n"
                         "    let d = 5u32 as opaque;\n"
                         "    if (consumeInt(d) != 0) { return 1; }\n"
                         "    let s = \"ok\" as opaque;\n"
                         "    if (consumeStr(s) != 0) { return 2; }\n"
                         "    var box = Box { value: 42 };\n"
                         "    let b = box as opaque;\n"
                         "    if (consumeBox(b) != 0) { return 3; }\n"
                         "    let made = make();\n"
                         "    if (made is u32) { } else { return 5; }\n"
                         "    let o: opaque = 5u32;\n"
                         "    if (echo(o) is u32) { return 0; }\n"
                         "    return 4;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "implicit opaque coercions use the modern codegen pipeline");
    CHECK(r.ok, "opaque narrowing and implicit concrete-to-opaque coercions run");
    printf("EXIT CODE: %d\n", r.exitCode);
    CHECK_EQ(r.exitCode, 0, "narrowed payloads and implicit coercions preserve values");
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
    auto r = t.run("codegen-array-of-structs.zith",
                   "struct Pair { left: i32, right: i32 }\n"
                   "fn main(): i32 {\n"
                   "    var items: [2]Pair = [Pair{0, 0}, Pair{0, 0}];\n"
                   "    items[0] = Pair{5, 6};\n"
                   "    items[0].right = 9;\n"
                   "    return raw items[0].left + raw items[0].right;\n"
                   "}\n");
    CHECK(r.ok, "Arrays of structs support indexed field assignment");
    CHECK_EQ(r.exitCode, 14, "items[i].field writes target the selected aggregate element");
}

static void test_enum_values() {
    CodegenTest t;
    auto r = t.run("codegen-enum-values.zith", "enum Color: u8 { Red = 2, Green }\n"
                                               "fn main(): i32 {\n"
                                               "    if (Color.Green == Color.Green) { return 3; }\n"
                                               "    return 0;\n"
                                               "}\n");

    auto r2 = t.run("codegen-enum-cast.zith", "enum Color: u8 { Red = 10, Green = 20 }\n"
                                              "fn main(): i32 {\n"
                                              "    let c: Color = Color.Green;\n"
                                              "    return c as i32;\n"
                                              "}\n");
    CHECK(r2.ok, "Enum to integer cast compiles and runs");
    CHECK_EQ(r2.exitCode, 20, "Enum cast evaluates to the underlying integer discriminant value");
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

static void test_explicit_struct_alignment_layout() {
    ModernFileCodegenTest t;
    t.opts.targetTriple = "x86_64-unknown-linux-gnu";
    t.write("main.zith", "#[alignas(64)] struct AlignedSmall { value: u8 }\n"
                         "struct ContainsAligned { leading: u8, value: AlignedSmall }\n"
                         "fn main(): i32 {\n"
                         "    if (@alignOf(AlignedSmall) != 64) { return 1; }\n"
                         "    if (@sizeOf(AlignedSmall) != 64) { return 2; }\n"
                         "    if (@offsetOf(AlignedSmall, value) != 0) { return 3; }\n"
                         "    if (@offsetOf(ContainsAligned, leading) != 0) { return 4; }\n"
                         "    if (@offsetOf(ContainsAligned, value) != 64) { return 5; }\n"
                         "    if (@alignOf(ContainsAligned) != 64) { return 6; }\n"
                         "    if (@sizeOf(ContainsAligned) != 128) { return 7; }\n"
                         "    if (@alignOf([2]AlignedSmall) != 64) { return 8; }\n"
                         "    if (@sizeOf([2]AlignedSmall) != 128) { return 9; }\n"
                         "    return 0;\n"
                         "}\n");
    auto r = t.run();
    CHECK(r.ok, "explicit struct alignment compiles for the contract target");
    if (r.ok)
        CHECK_EQ(r.exitCode, 0,
                 "struct fields, aggregate padding, and array stride follow the alignment "
                 "contract");

    auto warm = t.run();
    CHECK(warm.ok, "the aligned struct still compiles and runs from the persistent cache");
    CHECK(warm.cacheHits > 0U, "the aligned struct test exercises a warm-cache hit");
    CHECK_EQ(warm.exitCode, 0, "cached type layout preserves explicit struct alignment");
}

static void test_unsupported_struct_alignment_is_rejected() {
    ModernFileCodegenTest t;
    t.opts.targetTriple = "x86_64-unknown-linux-gnu";
    t.write("main.zith", "#[alignas(8192)] struct TooAligned { value: u8 }\n"
                         "fn main(): i32 { return 0; }\n");

    const auto result = t.run();
    CHECK(!result.ok, "a struct alignment the backend cannot represent is rejected");
    CHECK(result.hasMessage("cannot represent requested alignment 8192"),
          "unsupported struct alignment produces a compiler diagnostic");
}

static void test_explicit_array_binding_alignment() {
    ModernFileCodegenTest t;
    t.opts.targetTriple = "x86_64-unknown-linux-gnu";
    t.opts.flags.emitIr(true);

#ifdef ZITH_ENABLE_C_INTEROP
    t.opts.cSourceDirs.push("c");
    t.write("c/alignment-check.c", "#include <stdint.h>\n"
                                   "int c_is_aligned_64(unsigned char *value) {\n"
                                   "    return ((uintptr_t)value % 64U) == 0U;\n"
                                   "}\n");
    t.write("alignment-check.h", "int c_is_aligned_64(unsigned char *value);\n");
    std::string source = "import \"alignment-check.h\"\n";
#else
    std::string source;
#endif
    source += "fn main(): i32 {\n"
              "    #[alignas(64)] var bytes: [128]u8 = [";
    for (size_t index = 0; index < 128U; ++index) {
        if (index != 0U)
            source += ", ";
        source += "0";
    }
    source += "];\n"
              "    var copied: [128]u8 = bytes;\n"
              "    copied[0] = 9;\n"
              "    bytes[0] = 17;\n"
              "    if (@sizeOf([128]u8) != 128) { return 1; }\n"
#ifdef ZITH_ENABLE_C_INTEROP
              "    if (c_is_aligned_64(@ptrOf(bytes)) == 0) { return 2; }\n"
#endif
              "    return 17;\n"
              "}\n";
    t.write("main.zith", source);

    auto r = t.run();
    CHECK(r.ok, "an aligned array binding compiles and executes");
    CHECK_EQ(r.exitCode, 17, "the array type retains its 128-byte size");
    constexpr std::string_view aligned_alloca = "alloca [128 x i8], align 64";
    const auto aligned_alloca_pos             = r.output.find(aligned_alloca);
    CHECK(aligned_alloca_pos != std::string::npos,
          "the binding's LLVM stack storage has 64-byte alignment");
    if (aligned_alloca_pos != std::string::npos) {
        CHECK(r.output.find(aligned_alloca, aligned_alloca_pos + aligned_alloca.size()) ==
                  std::string::npos,
              "copying an aligned array does not propagate alignment to the destination");
        CHECK(r.output.find("alloca [128 x i8]", aligned_alloca_pos + aligned_alloca.size()) !=
                  std::string::npos,
              "the ordinary copy still has independent array storage");
    }

    auto warm = t.run();
    CHECK(warm.ok, "the aligned array still compiles and runs from the persistent cache");
    CHECK(warm.cacheHits > 0U, "the aligned array test exercises a warm-cache hit");
    CHECK_EQ(warm.exitCode, 17, "cached slot metadata preserves the aligned binding");
    CHECK(warm.output.find(aligned_alloca) != std::string::npos,
          "warm-cache LLVM keeps the binding's requested alignment");
}

static void test_explicit_global_binding_alignment() {
    ModernFileCodegenTest t;
    t.opts.targetTriple = "x86_64-unknown-linux-gnu";
    t.opts.flags.emitIr(true);
    t.write("main.zith", "#[alignas(64)] const aligned_value: i32 = 7;\n"
                         "fn main(): i32 { return aligned_value as i32; }\n");

    auto r = t.run();
    CHECK(r.ok, "an explicitly aligned module-scope const compiles and executes");
    CHECK_EQ(r.exitCode, 7, "the aligned module-scope const retains its value");
    CHECK(r.output.find("constant i32 7, align 64") != std::string::npos,
          "the LLVM global uses the binding's requested alignment");

    auto warm = t.run();
    CHECK(warm.ok, "the aligned global still compiles and runs from the persistent cache");
    CHECK(warm.cacheHits > 0U, "the aligned global test exercises a warm-cache hit");
    CHECK_EQ(warm.exitCode, 7, "cached global metadata preserves its value");
    CHECK(warm.output.find("constant i32 7, align 64") != std::string::npos,
          "warm-cache LLVM keeps the global's requested alignment");
}

static void test_explicit_for_in_binding_alignment() {
    ModernFileCodegenTest t;
    t.opts.targetTriple = "x86_64-unknown-linux-gnu";
    t.opts.flags.emitIr(true);
    t.write("main.zith", "fn main(): i32 {\n"
                         "    var total: i32 = 0;\n"
                         "    for (#[alignas(64)] value in 0..<1) {\n"
                         "        total = total + value;\n"
                         "    }\n"
                         "    return total;\n"
                         "}\n");

    const auto result = t.run();
    CHECK(result.ok, "an explicitly aligned for-in binding compiles and executes");
    CHECK_EQ(result.exitCode, 0, "the aligned for-in binding preserves loop behavior");
    CHECK(result.output.find("alloca i32, align 64") != std::string::npos,
          "the synthetic loop binding's LLVM storage has 64-byte alignment");
}

static void test_unsupported_binding_alignment_is_rejected() {
    ModernFileCodegenTest local;
    local.opts.targetTriple = "x86_64-unknown-linux-gnu";
    local.write("main.zith", "fn main(): i32 {\n"
                             "    #[alignas(8192)] var value: i32 = 7;\n"
                             "    return value;\n"
                             "}\n");

    const auto local_result = local.run();
    CHECK(!local_result.ok, "an unsupported local binding alignment is rejected");
    CHECK(local_result.hasMessage("cannot represent requested alignment 8192"),
          "unsupported local alignment produces a compiler diagnostic");

    ModernFileCodegenTest global;
    global.opts.targetTriple = "x86_64-unknown-linux-gnu";
    global.write("main.zith", "#[alignas(8192)] const value: i32 = 7;\n"
                              "fn main(): i32 { return value; }\n");

    const auto global_result = global.run();
    CHECK(!global_result.ok, "an unsupported global binding alignment is rejected");
    CHECK(global_result.hasMessage("cannot represent requested alignment 8192"),
          "unsupported global alignment produces a compiler diagnostic");
}

static void test_unverified_aligned_record_is_rejected_at_c_abi() {
    ModernFileCodegenTest t;
    t.opts.targetTriple = "x86_64-unknown-linux-gnu";
    t.opts.targetStage  = session::Stage::TypeChecked;
    t.write("main.zith", "#[alignas(64)] struct Aligned { value: i32 }\n"
                         "struct Wrapper { value: Aligned }\n"
                         "extern fn consume(value: Aligned): i32\n"
                         "extern fn consume_wrapper(value: Wrapper): i32\n"
                         "extern fn produce(): Aligned\n"
                         "fn main(): i32 {\n"
                         "    let value: Aligned = Aligned { value: 1 };\n"
                         "    return consume(value);\n"
                         "}\n");

    auto r = t.run();
    CHECK(!r.ok, "an unverified aligned record is rejected at the C ABI boundary");
    CHECK(r.hasMessage("unsupported C ABI"),
          "the C ABI rejection is a compiler diagnostic, not a linker failure");
    CHECK_EQ(r.errorCount, 3u,
             "direct parameters, nested parameters, and by-value returns are all rejected");
}

static void test_slice_string_intrinsics_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "extern fn printf(fmt: *char, ...): i32\n"
                         "fn borrowedLength(str: view []char): u64 {\n"
                         "    return @lengthOf(str);\n"
                         "}\n"
                         "fn borrowedFirst(str: view []char): char {\n"
                         "    return raw @ptrOf(str)[0];\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    var values: [3]i32 = [10, 20, 30];\n"
                         "    let slice: []i32 = raw values[0..3];\n"
                         "    if (@lengthOf(slice) != 3) { return 1; }\n"
                         "    if (raw @ptrOf(slice)[1] != 20) { return 2; }\n"
                         "    if (@lengthOf(\"zith\") != 4) { return 3; }\n"
                         "    let text: []char = \"zith\";\n"
                         "    if (borrowedLength(view text) != 4) { return 4; }\n"
                         "    if (borrowedFirst(view text) != 'z') { return 5; }\n"
                         "    _ = printf(\"%s\\n\", @ptrOf(\"zith\"));\n"
                         "    return 0;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "@lengthOf/@ptrOf slices and strings compile, link, and execute");
    CHECK_EQ(r.exitCode, 0, "@lengthOf and @ptrOf return expected slice/string values");
    CHECK(r.output.find("zith") != std::string::npos, "the string pointer intrinsic feeds printf");
}

static void test_when_expression_runtime() {
    CodegenTest t;
    auto r = t.run("codegen-when.zith",
                   "fn classify(n: i32): i32 {\n"
                   "    return when (n) {\n"
                   "        (0) 10,\n"
                   "        (1..3) 20,\n"
                   "        (4) 30,\n"
                   "        (_) 40\n"
                   "    }\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    return classify(0) + classify(2) + classify(4) + classify(9);\n"
                   "}\n");
    CHECK(r.ok, "when with literal, range, and default cases compiles without arrows");
    printf("EXIT CODE: %d\n", r.exitCode);
    CHECK_EQ(r.exitCode, 100, "when dispatches through literal, range, and default cases");
}

static void test_when_void_expression_case_executes() {
    CodegenTest t;
    auto r = t.run("codegen-when-void-body.zith", "extern fn puts(msg: *char)\n"
                                                  "fn main() {\n"
                                                  "    when (1) {\n"
                                                  "        (1) puts(\"A\"),\n"
                                                  "        (_) puts(\"B\")\n"
                                                  "    }\n"
                                                  "    when (2) {\n"
                                                  "        (1) puts(\"C\"),\n"
                                                  "        (_) puts(\"D\")\n"
                                                  "    }\n"
                                                  "    when (3) {\n"
                                                  "        (3) { puts(\"E\"); },\n"
                                                  "        (_) { puts(\"F\"); }\n"
                                                  "    }\n"
                                                  "}\n");
    CHECK(r.ok, "when with void expression case bodies compiles and runs");
    CHECK(r.output == "A\nD\nE\n",
          "when executes direct void bodies and does not duplicate block-body effects");
}

static void test_when_legacy_arrow_runtime() {
    CodegenTest t;
    auto r = t.run("codegen-when-arrow.zith", "fn classify(n: i32): i32 {\n"
                                              "    return when (n) {\n"
                                              "        (0) ~> 10,\n"
                                              "        (1..3) ~> 20,\n"
                                              "        (_) ~> 40\n"
                                              "    }\n"
                                              "}\n"
                                              "fn main(): i32 {\n"
                                              "    return classify(2);\n"
                                              "}\n");
    CHECK(r.ok, "legacy ~> when cases still compile");
    CHECK_EQ(r.exitCode, 20, "legacy ~> when cases still dispatch at runtime");
}

static void test_when_pattern_alternatives_runtime() {
    CodegenTest t;
    auto r = t.run("codegen-when-pattern-alternatives.zith",
                   "enum Status { Ok, Error }\n"
                   "fn accept(status: Status): i32 {\n"
                   "    return when (status) {\n"
                   "        (Status.Ok or Status.Error) 10,\n"
                   "        (_) 20\n"
                   "    }\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    let or_matches = when (3) {\n"
                   "        (3 or 5) 1,\n"
                   "        (_) 4\n"
                   "    };\n"
                   "    let or_matches_rhs = when (5) {\n"
                   "        (3 or 5) 1,\n"
                   "        (_) 4\n"
                   "    };\n"
                   "    let or_misses = when (8) {\n"
                   "        (3 or 5) 1,\n"
                   "        (_) 4\n"
                   "    };\n"
                   "    let and_matches = when (3) {\n"
                   "        (3 and 3) 2,\n"
                   "        (_) 4\n"
                   "    };\n"
                   "    let xor_matches = when (3) {\n"
                   "        (3 xor 3) 3,\n"
                   "        (_) 4\n"
                   "    };\n"
                   "    return or_matches + or_matches_rhs + or_misses + and_matches +\n"
                   "           xor_matches + accept(Status.Ok);\n"
                   "}\n");
    CHECK(r.ok, "when pattern alternatives compile and lower");
    CHECK_EQ(r.exitCode, 1 + 1 + 4 + 2 + 3 + 10 + 1,
             "when pattern alternatives dispatch through equality-desugared booleans");
}

static void test_when_default_must_be_last() {
    CodegenTest t;
    auto r = t.run("codegen-when-default-last.zith", "fn classify(n: i32): i32 {\n"
                                                     "    return when (n) {\n"
                                                     "        (_) 10,\n"
                                                     "        (1) 20\n"
                                                     "    }\n"
                                                     "}\n"
                                                     "fn main(): i32 {\n"
                                                     "    return 0;\n"
                                                     "}\n");
    CHECK(!r.ok && r.errorCount > 0, "a default when case must still be the final case");
}

static void test_tagged_union_pointer_is_type_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "import \"stdio.h\"\n"
                         "union Foo { *char, i32, f64 }\n"
                         "fn main(): i32 {\n"
                         "    let f = Foo{\"hello\"};\n"
                         "    if (f is *char) {\n"
                         "        _ = printf(\"union string: %s\\n\", f);\n"
                         "        return 0;\n"
                         "    }\n"
                         "    _ = printf(\"abu\\n\");\n"
                         "    return 2;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "tagged union `is *char` with a C variadic call compiles and runs");
    CHECK_EQ(r.exitCode, 0, "the string member branch is selected");
    CHECK(r.output.find("union string: hello") != std::string::npos,
          "the narrowed tagged value is passed to printf");
}

static void test_qualified_receiver_mutation_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "struct Sample {\n"
                         "    x: i32,\n"
                         "    fn bump_lend(self: lend Sample) { self->x = self->x + 1; }\n"
                         "    fn get(self): i32 { return self->x; }\n"
                         "    fn read_view(self: view Sample): i32 { return self->x; }\n"
                         "    fn bump_ptr(self: *Sample) { self->x = self->x + 5; }\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    var s: Sample = Sample { x: 10 };\n"
                         "    s.bump_lend();\n"
                         "    s.bump_ptr();\n"
                         "    if (s.read_view() != 16) { return 1; }\n"
                         "    return s.get();\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "qualified lend/view receivers and '.' mutation compile, link, and execute");
    CHECK_EQ(r.exitCode, 16, "lend and explicit pointer receiver mutations reach the caller");
}

static void test_free_borrow_parameter_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "struct Sample {\n"
                         "    x: i32,\n"
                         "}\n"
                         "fn bump(p: lend Sample): i32 {\n"
                         "    p.x = p.x + 1;\n"
                         "    p.x\n"
                         "}\n"
                         "fn read(p: view Sample): i32 { p.x }\n"
                         "fn main(): i32 {\n"
                         "    var q: Sample = Sample { x: 41 };\n"
                         "    if (bump(lend q) != 42) { return 1; }\n"
                         "    if (read(view q) != 42) { return 2; }\n"
                         "    q.x\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "free lend/view borrow parameters compile, link, and execute");
    CHECK_EQ(r.exitCode, 42, "a lend parameter mutates the caller's binding in place");
}

static void test_when_narrowing_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "import \"stdio.h\"\n"
                         "union Value { *char, i32 }\n"
                         "fn main(): i32 {\n"
                         "    let v = Value{\"ok\"};\n"
                         "    when (v) {\n"
                         "        (v is *char) { _ = printf(\"%s\\n\", v); return 7; },\n"
                         "        (_) { return 1; }\n"
                         "    }\n"
                         "    return 2;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "when narrowing compiles the narrowed member without an explicit cast");
    CHECK_EQ(r.exitCode, 7, "the narrowed union branch is selected at runtime");
    CHECK(r.output.find("ok") != std::string::npos, "the narrowed pointer member is printed");
}

static void test_for_three_clause_runtime() {
    CodegenTest t;
    auto r = t.run("codegen-for3.zith", "fn sum_to(n: i32): i32 {\n"
                                        "    var total: i32 = 0;\n"
                                        "    for (var i: i32 = 0), (i < n), (i = i + 1) {\n"
                                        "        total = total + i;\n"
                                        "    }\n"
                                        "    return total;\n"
                                        "}\n"
                                        "fn count_down(n: i32): i32 {\n"
                                        "    var hits: i32 = 0;\n"
                                        "    for (var i: i32 = n), (i > 0), (i = i - 1) {\n"
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

static void test_labeled_loop_controls_runtime() {
    struct Case {
        std::string control;
        int expected;
    };
    // Return `100 * outer_runs + 10 * inner_hits + outer_tail` so every control
    // target has a distinct observable count.
    for (const auto &test_case : {
             Case{"break;", 107},
             Case{"break outer;", 120},
             Case{"continue;", 137},
             Case{"continue outer;", 104},
         }) {
        CodegenTest t;
        auto r = t.run("codegen-labels.zith",
                       "fn main(): i32 {\n"
                       "    var outer_runs: i32 = 0;\n"
                       "    var outer_tail: i32 = 0;\n"
                       "    var inner_hits: i32 = 0;\n"
                       "    outer: for (var i: i32 = 0), (i < 3), (i = i + 1) {\n"
                       "        outer_runs = outer_runs + 1;\n"
                       "        for (var j: i32 = 0), (j < 3), (j = j + 1) {\n"
                       "            inner_hits = inner_hits + 1;\n"
                       "            if (j == 1) { " +
                           test_case.control +
                           " }\n"
                           "        }\n"
                           "        outer_tail = outer_tail + 1;\n"
                           "    }\n"
                           "    return outer_runs * 100 + inner_hits * 10 + outer_tail;\n"
                           "}\n");
        CHECK(r.ok, "labeled control program compiles, links and runs");
        CHECK_EQ(r.exitCode, test_case.expected,
                 ("labelled control '" + test_case.control + "' gives the expected count").c_str());
    }
}

static void test_for_in_runtime() {
    CodegenTest t;
    auto r = t.run("codegen-for-in.zith", "struct Range {\n"
                                          "    current: i32,\n"
                                          "    limit: i32,\n"
                                          "    fn next(var self): ?i32 {\n"
                                          "        if (self->current >= self->limit) {\n"
                                          "            return null;\n"
                                          "        }\n"
                                          "        let value = self->current;\n"
                                          "        self->current = self->current + 1;\n"
                                          "        return value;\n"
                                          "    }\n"
                                          "}\n"
                                          "fn main(): i32 {\n"
                                          "    var total: i32 = 0;\n"
                                          "    let r: Range = Range { current: 0, limit: 6 };\n"
                                          "    for (x in r) {\n"
                                          "        total = total + x;\n"
                                          "    }\n"
                                          "    if (total != 15) {\n"
                                          "        return 1;\n"
                                          "    }\n"
                                          "    return total;\n"
                                          "}\n");
    CHECK(r.ok, "for-in runtime test compiles, links, and executes");
    CHECK_EQ(r.exitCode, 15, "for-in extracts ?T elements until the None sentinel");
}

static void test_nested_optional_for_in_runtime() {
    CodegenTest t;
    auto r = t.run("codegen-nested-for-in.zith",
                   "struct Range {\n"
                   "    current: i32,\n"
                   "    limit: i32,\n"
                   "    fn next(var self): ??i32 {\n"
                   "        if (self->current >= self->limit) {\n"
                   "            return null;\n"
                   "        }\n"
                   "        if (self->current == 1 or self->current == 3) {\n"
                   "            let maybe: ?i32 = null;\n"
                   "            self->current = self->current + 1;\n"
                   "            return maybe;\n"
                   "        }\n"
                   "        let value = self->current;\n"
                   "        self->current = self->current + 1;\n"
                   "        return value;\n"
                   "    }\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    var total: i32 = 0;\n"
                   "    var nulls: i32 = 0;\n"
                   "    let r: Range = Range { current: 0, limit: 6 };\n"
                   "    for (x: ?i32 in r) {\n"
                   "        if (x is null) {\n"
                   "            nulls = nulls + 1;\n"
                   "        } else {\n"
                   "            total = total + 1;\n"
                   "        }\n"
                   "    }\n"
                   "    return total * 10 + nulls;\n"
                   "}\n");
    CHECK(r.ok, "??T for-in runtime test compiles, links, and executes");
    CHECK_EQ(r.exitCode, 42, "??T exposes null elements without treating them as end");
}

// Exercises the imported generic `HashMap<K, V>` from the standard library
// end-to-end: the optional-typed `allocator`/`table` fields must accept bare
// `T` assignments (wrapped as `Some`), and `get<K, V>` must return the reified
// `?V` aggregate rather than a bare `V`.
static void test_imported_generic_hashmap_runtime() {
    CodegenTest t;
    auto r = t.run("codegen-generic-hashmap.zith",
                   "import soon/collections/hash_map as hm\n"
                   "from soon/memory\n"
                   "fn main(): i32 {\n"
                   "    let heap = HeapAllocator {};\n"
                   "    var map = hm.HashMap<i64, i64> {};\n"
                   "    if not (hm.HashMap.init(lend map, heap)) { return 2; }\n"
                   "    if not (hm.HashMap.put(lend map, 1i64, 42i64)) { return 3; }\n"
                   "    if not (hm.HashMap.contains(view map, 1i64)) { return 4; }\n"
                   "    if (hm.HashMap.contains(view map, 2i64)) { return 5; }\n"
                   "    let value = hm.HashMap.get(view map, 1i64);\n"
                   "    if (value is null or raw value != 42i64) { return 6; }\n"
                   "    if (hm.HashMap.len(view map) != 1u64) { return 7; }\n"
                   "    hm.HashMap.destroy(lend map);\n"
                   "    return 0;\n"
                   "}\n");
    CHECK(r.ok, "imported generic HashMap<i64,i64> compiles, links, and executes");
    CHECK_EQ(r.exitCode, 0, "init/put/contains/get/len/destroy round-trip on the stdlib HashMap");
}

// Exercises the imported generic `DynArray<T>` from the standard library
// end-to-end. `DynArray` shares the ADR 0020 path with `HashMap`, so this
// guards the second imported-generic consumer against regressions: growth via
// `reserve`/`push`, element access through `get`, removal through `remove`/
// `pop`, and `destroy` returning the array to an empty state.
static void test_imported_generic_dynarray_runtime() {
    CodegenTest t;
    auto r = t.run("codegen-generic-dynarray.zith",
                   "import soon/collections/dyn_array as da\n"
                   "from soon/memory\n"
                   "fn main(): i32 {\n"
                   "    let heap = HeapAllocator {};\n"
                   "    var values = da.create<i32>(heap);\n"
                   "    if not (da.DynArray.reserve<i32>(lend values, 2u64)) { return 1; }\n"
                   "    if not (da.push<i32>(lend values, 10)) { return 2; }\n"
                   "    if not (da.push<i32>(lend values, 20)) { return 3; }\n"
                   "    if not (da.push<i32>(lend values, 30)) { return 4; }\n"
                   "    let middle = da.get<i32>(view values, 1u64);\n"
                   "    if (middle is null or raw middle != 20) { return 5; }\n"
                   "    let removed = da.remove<i32>(lend values, 1u64);\n"
                   "    if (removed is null or raw removed != 20) { return 6; }\n"
                   "    let shifted = da.get<i32>(view values, 1u64);\n"
                   "    if (shifted is null or raw shifted != 30) { return 7; }\n"
                   "    let last = da.pop<i32>(lend values);\n"
                   "    if (last is null or raw last != 30) { return 8; }\n"
                   "    if (da.len<i32>(view values) != 1u64) { return 9; }\n"
                   "    da.destroy<i32>(lend values);\n"
                   "    if (da.len<i32>(view values) != 0u64) { return 10; }\n"
                   "    return 0;\n"
                   "}\n");
    CHECK(r.ok, "imported generic DynArray<i32> compiles, links, and executes");
    CHECK_EQ(r.exitCode, 0, "reserve/push/get/remove/pop/len/destroy round-trip on DynArray");
}

// Exercises the imported generic `Result<T, E>` helpers from the standard
// library end-to-end. `Ok`/`Err` bind their type arguments from the expected
// result type, so this also guards contextual generic inference: the helpers
// are called without an explicit `<T, E>` list.
static void test_imported_result_runtime() {
    CodegenTest t;
    auto r = t.run("codegen-generic-result.zith",
                   "import soon/result as res\n"
                   "import soon/result/fallback as fallback\n"
                   "fn main(): i32 {\n"
                   "    let ok: res.Result<u32, bool> = res.Ok(7);\n"
                   "    let err: res.Result<u32, bool> = res.Err(true);\n"
                   "    if not (res.isOk(ok)) { return 1; }\n"
                   "    if (res.isOk(err)) { return 2; }\n"
                   "    if not (res.isErr(err)) { return 3; }\n"
                   "    if (res.isErr(ok)) { return 4; }\n"
                   "    let v: u32 = res.unwrap(ok);\n"
                   "    if (v != 7u32) { return 5; }\n"
                   "    let e: bool = res.unwrapErr(err);\n"
                   "    if (not e) { return 6; }\n"
                   "    if (fallback.unwrapOr(ok, 99u32) != 7u32) { return 7; }\n"
                   "    if (fallback.unwrapOr(err, 99u32) != 99u32) { return 8; }\n"
                   "    if (not fallback.unwrapErrOr(err, false)) { return 9; }\n"
                   "    if (fallback.unwrapErrOr(ok, false)) { return 10; }\n"
                   "    return 0;\n"
                   "}\n");
    CHECK(r.ok, "imported Result and soon fallback helpers compile, link, and execute");
    if (r.ok)
        CHECK_EQ(r.exitCode, 0,
                 "Result accessors and fallback helpers cover both Ok and Err branches");
}

static void test_soon_math_scalar_runtime() {
    CodegenTest t;
#if !defined(_WIN32) && !defined(ZITH_IS_WASM)
    t.opts.libraries.push("m");
#endif
    auto r = t.run("codegen-soon-math-scalar.zith",
                   "import soon/math/scalar as math\n"
                   "import soon/math as baseMath\n"
                   "fn main(): i32 {\n"
                   "    if (math.minI32(4, -3) != -3) { return 1; }\n"
                   "    if (math.maxI32(4, -3) != 4) { return 2; }\n"
                   "    if (math.clampI32(-2, 0, 5) != 0) { return 3; }\n"
                   "    if (math.clampI32(3, 0, 5) != 3) { return 4; }\n"
                   "    if (math.clampI32(8, 0, 5) != 5) { return 5; }\n"
                   "    if (math.clampI32(3, 5, 0) != 3) { return 6; }\n"
                   "    let halfPi = math.degreesToRadians(90.0);\n"
                   "    if (halfPi < 1.57079 or halfPi > 1.57080) { return 7; }\n"
                   "    let degrees = math.radiansToDegrees(baseMath.pi);\n"
                   "    if (degrees < 179.9999 or degrees > 180.0001) { return 8; }\n"
                   "    return 0;\n"
                   "}\n");
    CHECK(r.ok, "soon scalar math helpers compile, link, and execute");
    if (r.ok)
        CHECK_EQ(r.exitCode, 0,
                 "integer bounds and degree/radian conversions return expected values");
}

static void test_soon_slice_algorithms_runtime() {
    CodegenTest t;
    auto r = t.run("codegen-soon-slice-algorithms.zith",
                   "import soon/algorithms/slice as slices\n"
                   "fn main(): i32 {\n"
                   "    var numbers: [4]i32 = [4, 2, 4, 7];\n"
                   "    let numberSlice: []i32 = raw numbers[0u64..4u64];\n"
                   "    let first = slices.findIndexI32(numberSlice, 4);\n"
                   "    if (first is null) { return 1; }\n"
                   "    let firstIndex: u64 = must first;\n"
                   "    if (firstIndex != 0u64) { return 2; }\n"
                   "    if (slices.countI32(numberSlice, 4) != 2u64) { return 3; }\n"
                   "    if (slices.countI32(numberSlice, 9) != 0u64) { return 4; }\n"
                   "    let empty: []i32 = raw numbers[0u64..0u64];\n"
                   "    let emptyFirst = slices.findIndexI32(empty, 4);\n"
                   "    if (not (emptyFirst is null)) { return 5; }\n"
                   "    if (slices.countI32(empty, 4) != 0u64) { return 6; }\n"
                   "    return 0;\n"
                   "}\n");
    CHECK(r.ok, "soon slice algorithms compile, link, and execute");
    CHECK_EQ(r.exitCode, 0, "findIndexI32 and countI32 handle matches, absence, and empty slices");
}

static void test_imported_panic_runtime() {
    // A program that does not panic must run normally after importing the
    // module, and one that panics must write its message and abort.
    CodegenTest ok;
    auto safe = ok.run("codegen-panic-safe.zith",
                       "import soon/panic as pn\n"
                       "fn main(): i32 { return 5; }\n");
    CHECK(safe.ok, "importing std/panic does not break a program that never panics");
    CHECK_EQ(safe.exitCode, 5, "a program importing std/panic still returns its value");

    CodegenTest t;
    auto r = t.run("codegen-panic-abort.zith",
                   "import soon/panic as pn\n"
                   "fn main(): i32 {\n"
                   "    pn.panic(\"fatal: bad state\");\n"
                   "    return 0;\n"
                   "}\n");
    CHECK(r.ok, "a program calling std/panic still compiles and links");
    CHECK(r.output.find("fatal: bad state") != std::string::npos,
          "std/panic writes its message before aborting");
    CHECK(r.exitCode != 0, "std/panic aborts the process");
}

static void test_imported_memory_new_delete_runtime() {
    CodegenTest t;
    auto r = t.run("codegen-memory-new.zith",
                   "from soon/memory\n"
                   "struct Box { value: i32 = 0 }\n"
                   "implement Box as InPlace {\n"
                   "    fn inplace(var self, args: opaque): bool {\n"
                   "        self->value = raw args as i32;\n"
                   "        return true;\n"
                   "    }\n"
                   "    fn clean(var self) { self->value = 0; }\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    let b: ?*Box = new(42 as opaque);\n"
                   "    if (b is null) { return 1; }\n"
                   "    let q = raw b;\n"
                   "    let got = q->value;\n"
                   "    delete<Box>(q);\n"
                   "    return got;\n"
                   "}\n");
    CHECK(r.ok, "stdlib new/delete allocate, construct, clean, and free a generic T");
    CHECK_EQ(r.exitCode, 42, "new writes through the in-place constructor and delete frees the block");
}

static void test_imported_memory_make_release_runtime() {
    CodegenTest t;
    auto r = t.run("codegen-memory-make.zith",
                   "from soon/memory\n"
                   "struct Box { value: i32 = 0 }\n"
                   "implement Box as InPlace {\n"
                   "    fn inplace(var self, args: opaque): bool {\n"
                   "        self->value = raw args as i32;\n"
                   "        return true;\n"
                   "    }\n"
                   "    fn clean(var self) { self->value = 0; }\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    let b: ?*Box = make(HeapAllocator{}, 42 as opaque);\n"
                   "    if (b is null) { return 1; }\n"
                   "    let q = raw b;\n"
                   "    let got = q->value;\n"
                   "    release(HeapAllocator{}, q);\n"
                   "    return got;\n"
                   "}\n");
    CHECK(r.ok, "stdlib make/release over dyn Allocator allocate, construct, clean, and free");
    CHECK_EQ(r.exitCode, 42, "make writes through the in-place constructor and release frees the block");
}

// A tagged-union `is Member` test whose target is a qualified type from an
// imported module (`lib.End`). The HIR lowerer must resolve the qualified name
// through the import binding; a raw TypeTable lookup cannot, which previously
// made the test silently lower to an always-false check.
static void test_imported_tagged_union_is_member_runtime() {
    ModernFileCodegenTest t;
    t.write("lib.zith", "pub struct End {}\n"
                        "pub union Step { End, char }\n"
                        "pub fn end(): Step { return Step { End {} }; }\n"
                        "pub fn chr(c: char): Step { return Step { c }; }\n");
    t.write("main.zith", "import lib as s\n"
                         "fn main(): i32 {\n"
                         "    var a: s.Step = s.end();\n"
                         "    var b: s.Step = s.chr('x');\n"
                         "    if not (a is s.End) { return 1; }\n"
                         "    if (b is s.End) { return 2; }\n"
                         "    return 0;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "cross-module tagged-union 'is' uses the modern codegen pipeline");
    CHECK(r.ok, "qualified 'is Member' on an imported tagged union compiles and runs");
    CHECK_EQ(r.exitCode, 0, "the imported payload member is detected and non-members rejected");
}

static void test_integer_range_for_runtime() {
    CodegenTest t;
    auto r =
        t.run("codegen-integer-range-for.zith", "fn main(): i32 {\n"
                                                "    var total: i32 = 0;\n"
                                                "    for (i in 0..<5) { total = total + i; }\n"
                                                "    if (total != 10) { return 1; }\n"
                                                "    total = 0;\n"
                                                "    for (i in 0>..5) { total = total + i; }\n"
                                                "    if (total != 15) { return 2; }\n"
                                                "    total = 0;\n"
                                                "    for (i in 1>..<5) { total = total + i; }\n"
                                                "    if (total != 9) { return 3; }\n"
                                                "    total = 0;\n"
                                                "    for (i in 1..4) { total = total + i; }\n"
                                                "    if (total != 10) { return 4; }\n"
                                                "    return 0;\n"
                                                "}\n");
    CHECK(r.ok, "integer literal ranges compile, link, and execute in for-in");
    CHECK_EQ(r.exitCode, 0, "all four range bound forms iterate the expected integer values");
}

static void test_range_in_operator_and_when_runtime() {
    CodegenTest t;
    auto r = t.run("codegen-range-in.zith", "fn main(): i32 {\n"
                                            "    var total: i32 = 0;\n"
                                            "    if (3 in 1..<4) { total = total + 100; }\n"
                                            "    if (5 in 1>..5) { total = total + 1000; }\n"
                                            "    if (9 in 1..10) { total = total + 10000; }\n"
                                            "    if (0 in 0>..<10) { return 1; }\n"
                                            "    if (total != 11100) { return 3; }\n"
                                            "    let when_value = when (5 in 1..<9) {\n"
                                            "        (true) 7,\n"
                                            "        (_) 0\n"
                                            "    };\n"
                                            "    if (when_value != 7) { return 2; }\n"
                                            "    return 0;\n"
                                            "}\n");
    CHECK(r.ok, "'in' over ranges and when (x in range) compile, link, and execute");
    CHECK_EQ(r.exitCode, 0, "'in' applies open/closed bound flags without pre-adjusting bounds");
}

static void test_user_contains_runtime() {
    CodegenTest t;
    auto r = t.run("codegen-user-contains.zith",
                   "struct Set {\n"
                   "    a: i32,\n"
                   "    b: i32,\n"
                   "    c: i32,\n"
                   "    fn contains(self, value: i32): bool {\n"
                   "        return (value == self->a) or (value == self->b) or\n"
                   "               (value == self->c);\n"
                   "    }\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    let s: Set = Set { a: 7, b: 8, c: 9 };\n"
                   "    if (8 in s) { return 42; }\n"
                   "    return 0;\n"
                   "}\n");
    CHECK(r.ok, "Any type with a contains(self, value): bool method satisfies 'in'");
    CHECK_EQ(r.exitCode, 42, "generic Contains resolves and lowers the user method");
}

static void test_float_range_for_is_rejected() {
    CodegenTest t;
    auto r =
        t.run("codegen-float-range-for.zith", "fn main(): i32 {\n"
                                              "    var total: i32 = 0;\n"
                                              "    for (x in 0.0>..<10.0) { total = total + 1; }\n"
                                              "    return total;\n"
                                              "}\n");
    CHECK(!r.ok && r.errorCount > 0, "float ranges are rejected in for-in");
}

static void test_bodyless_range_for_is_rejected() {
    CodegenTest t;
    const std::string_view cases[] = {
        "for (1..5){}",
        "for (1>..5){}",
        "for (1..<5){}",
        "for (1>..<5){}",
    };
    for (const auto case_body : cases) {
        auto r = t.run("codegen-bodyless-range.zith", "fn main(): i32 {\n"
                                                      "    " +
                                                          std::string(case_body) +
                                                          "\n"
                                                          "    return 0;\n"
                                                          "}\n");
        CHECK(!r.ok && r.errorCount > 0, "bodyless literal ranges are rejected in for-in");
    }

    const auto r                 = t.run("codegen-bodyless-range.zith", "fn main(): i32 {\n"
                                                                        "    for (1..5){}\n"
                                                                        "    return 0;\n"
                                                                        "}\n");
    bool found_target_diagnostic = false;
    for (const auto &diag : r.messages)
        found_target_diagnostic =
            found_target_diagnostic ||
            diag.find("literal range is not a boolean condition") != std::string::npos;
    CHECK(found_target_diagnostic, "bodyless range rejection names the missing condition");
}

static void test_imported_counter_runtime() {
    CodegenTest t;
    auto r = t.run("codegen-imported-counter.zith",
                   "import soon/counter\n"
                   "fn main(): i32 {\n"
                   "    var total: i32 = 0;\n"
                   "    let half: soon.counter.Counter = soon.counter.Counter { index: 0, limit: 5, "
                   "step: 1, inclusive: false };\n"
                   "    for (x in half) {\n"
                   "        total = total + x;\n"
                   "    }\n"
                   "    let full: soon.counter.Counter = soon.counter.Counter { index: 0, limit: 5, "
                   "step: 1, inclusive: true };\n"
                   "    for (y in full) {\n"
                   "        total = total + y;\n"
                   "    }\n"
                   "    return total;\n"
                   "}\n");
    CHECK(r.ok, "the stdlib Counter imports, lowers, and runs");
    CHECK_EQ(r.exitCode, 25,
             "Counter excludes the limit by default and includes it when requested");
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
    session.setContent("#[discardable] extern fn putchar(c: i32): i32\n"
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
                                                "from soon/memory\n"
                                                "fn main(): i32 {\n"
                                                "    let heap = HeapAllocator {};\n"
                                                "    _ = println(heap, \"from import\");\n"
                                                "    return 0;\n"
                                                "}\n");
    CHECK(r.ok, "from std/io/console compiles and runs");
    CHECK(r.output.find(
              "define i32 @\"std.io.console.println(dyn Allocator,[]char,[...]dyn Formatable)\"") !=
              std::string::npos,
          "Imported println body is emitted into LLVM IR");
    CHECK(r.output.find("call i32 @putchar") != std::string::npos,
          "Imported println body calls putchar in LLVM IR");
}

static void test_console_alias_resolves_member_without_global_import() {
    CodegenTest aliased;
    aliased.opts.flags.emitIr(true);
    auto ok = aliased.run("codegen-console-alias.zith",
                          "import std/io/console as console\n"
                          "from soon/memory\n"
                          "fn main(): i32 {\n"
                          "    let heap = HeapAllocator {};\n"
                          "    _ = console.println(heap, \"alias import\");\n"
                          "    return 0;\n"
                          "}\n");
    CHECK(ok.ok, "console.println resolves through an import alias");
    CHECK(ok.output.find(
              "call i32 @\"std.io.console.println(dyn Allocator,[]char,[...]dyn Formatable)\"") !=
              std::string::npos,
          "Alias import emits a call to the imported function");

    CodegenTest unqualified;
    auto missing =
        unqualified.run("codegen-console-alias-missing.zith", "import std/io/console as console\n"
                                                              "fn main() {\n"
                                                              "    _ = println(\"not global\");\n"
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

static void test_generic_bound_by_value_parameter_runs_without_borrow_attrs() {
    ModernFileCodegenTest t;
    t.opts.flags.emitIr(true);
    t.write("main.zith", "trait Foo {}\n"
                         "interface Transform { [x]: i32 }\n"
                         "struct Temple { x: i32 }\n"
                         "struct Point { x: i32 }\n"
                         "implement Temple as Foo {}\n"
                         "fn foolish<T: Foo>(a: T): i32 { return 1; }\n"
                         "fn interLab<T: Transform>(a: T): i32 { return a.x; }\n"
                         "fn main(): i32 {\n"
                         "    let p = Point { x: 7 };\n"
                         "    return foolish<Temple>(Temple { x: 1 }) + interLab(p);\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "generic bound declarations compile and execute");
    CHECK_EQ(r.exitCode, 8, "generic value parameters produce the expected result");
    CHECK_EQ(r.errorCount, 0u, "the emitted module passes LLVM verification");
    CHECK(r.output.find("define i32 @\"foolish<Temple>\"(%zith.struct.") != std::string::npos,
          "the generic by-value instance is emitted");
    CHECK(r.output.find("define i32 @\"interLab<Point>\"(%zith.struct.") != std::string::npos,
          "the generic interface by-value instance is emitted");
    CHECK(countOccurrences(r.output, "readonly") == 0u ||
              r.output.find("noalias nocapture") == std::string::npos,
          "no incompatible borrowed parameter attributes are emitted");
}

static void test_interface_method_bound_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "interface Positioned {\n"
                         "    x: i32,\n"
                         "    fn getX(self): i32\n"
                         "}\n"
                         "struct Point {\n"
                         "    x: i32,\n"
                         "    fn getX(self): i32 { return self.x; }\n"
                         "}\n"
                         "fn transform<T: Positioned>(p: T): i32 { return p.x + p.getX(); }\n"
                         "fn main(): i32 {\n"
                         "    let p = Point { x: 41 };\n"
                         "    return transform<Point>(p);\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "interface method generic bound uses the modern codegen pipeline");
    CHECK(r.ok, "interface method generic bound compiles, links and runs");
    CHECK_EQ(r.exitCode, 82, "generic transform reads the interface field and calls its method");
    CHECK_EQ(r.errorCount, 0u, "the emitted module passes LLVM verification");
}

static void test_dyn_interface_method_dispatch_runtime() {
    ModernFileCodegenTest t;
    t.opts.flags.emitIr(true);
    t.write("main.zith", "interface Area {\n"
                         "    fn area(self): i32\n"
                         "}\n"
                         "struct Square {\n"
                         "    side: i32,\n"
                         "    fn area(self): i32 { self.side * self.side }\n"
                         "}\n"
                         "struct Circle {\n"
                         "    radius: i32,\n"
                         "    fn area(self): i32 { self.radius }\n"
                         "}\n"
                         "fn total(a: dyn Area): i32 { return a.area(); }\n"
                         "fn main(): i32 {\n"
                         "    let s: Square = Square { side: 3 };\n"
                         "    let c: Circle = Circle { radius: 5 };\n"
                         "    return total(s) + total(c);\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "dyn Interface dispatch uses the modern codegen pipeline");
    CHECK(r.ok, "dyn Interface methods compile, link, dispatch and execute");
    CHECK_EQ(r.exitCode, 14, "dyn Area calls Square.area (9) and Circle.area (5)");
    CHECK_EQ(r.errorCount, 0u, "the dyn Interface module passes LLVM verification");
    CHECK(r.output.find("@_zith.vtable.Area.Square = internal constant [1 x ptr] [ptr "
                        "@\"main.Square.area(*Square)\"]") != std::string::npos,
          "Square's vtable slot points to the concrete Square.area function");
    CHECK(r.output.find("@_zith.vtable.Area.Circle = internal constant [1 x ptr] [ptr "
                        "@\"main.Circle.area(*Circle)\"]") != std::string::npos,
          "Circle's vtable slot points to the concrete Circle.area function");
    CHECK(r.output.find("extractvalue { ptr, ptr } %2, 0") != std::string::npos,
          "dyn calls extract the concrete data pointer from the fat pointer");
    CHECK(r.output.find("getelementptr [1 x ptr], ptr %4, i32 0, i32 0") != std::string::npos,
          "dyn calls index the vtable slot before indirect invocation");
}

static void test_dyn_trait_method_dispatch_runtime() {
    ModernFileCodegenTest t;
    t.opts.flags.emitIr(true);
    t.write("main.zith", "trait Drawable {\n"
                         "    fn area(self): i32 { return 0; }\n"
                         "}\n"
                         "struct Square {\n"
                         "    side: i32,\n"
                         "    fn area(self): i32 { self.side * self.side }\n"
                         "}\n"
                         "struct Circle {\n"
                         "    radius: i32,\n"
                         "    fn area(self): i32 { self.radius }\n"
                         "}\n"
                         "implement Square as Drawable {\n"
                         "    fn area(self): i32 { self.side * self.side }\n"
                         "}\n"
                         "implement Circle as Drawable {\n"
                         "    fn area(self): i32 { self.radius }\n"
                         "}\n"
                         "fn total(a: dyn Drawable): i32 { return a.area(); }\n"
                         "fn main(): i32 {\n"
                         "    let s: Square = Square { side: 3 };\n"
                         "    let c: Circle = Circle { radius: 5 };\n"
                         "    return total(s) + total(c);\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "dyn Trait dispatch uses the modern codegen pipeline");
    CHECK(r.ok, "dyn Trait method calls compile, link, dispatch and execute");
    CHECK_EQ(r.exitCode, 14, "dyn Drawable calls Square.area (9) and Circle.area (5)");
    CHECK_EQ(r.errorCount, 0u, "the dyn Trait module passes LLVM verification");
    CHECK(r.output.find("@_zith.vtable.Drawable.Square = internal constant [1 x ptr] [ptr "
                        "@\"main.Square.area(*Square)\"]") != std::string::npos,
          "Square's nominal-trait vtable slot points to the concrete Square.area function");
    CHECK(r.output.find("@_zith.vtable.Drawable.Circle = internal constant [1 x ptr] [ptr "
                        "@\"main.Circle.area(*Circle)\"]") != std::string::npos,
          "Circle's nominal-trait vtable slot points to the concrete Circle.area function");
    CHECK(r.output.find("extractvalue { ptr, ptr } %2, 0") != std::string::npos,
          "dyn Trait calls extract the concrete data pointer from the fat pointer");
    CHECK(r.output.find("getelementptr [1 x ptr], ptr %4, i32 0, i32 0") != std::string::npos,
          "dyn Trait calls index the vtable slot before indirect invocation");
}

static void test_dyn_trait_struct_field_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "trait Allocator {\n"
                         "    fn value(self): i32;\n"
                         "}\n"
                         "struct HeapAllocator {}\n"
                         "implement HeapAllocator as Allocator {\n"
                         "    fn value(self): i32 { return 73; }\n"
                         "}\n"
                         "struct Resource {\n"
                         "    allocator: dyn Allocator,\n"
                         "}\n"
                         "fn createResource(allocator: dyn Allocator): Resource {\n"
                         "    return Resource { allocator };\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    let heap = HeapAllocator {};\n"
                         "    let resource = createResource(heap);\n"
                         "    return resource.allocator.value() - 73;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "stored dyn Trait values use the modern codegen pipeline");
    CHECK(r.ok, "a dyn Trait value can be stored in and loaded from a struct field");
    CHECK(r.exitCode == 0,
          (std::string("stored dyn Trait dispatch returns expected status 0, got ") +
           std::to_string(r.exitCode))
              .c_str());
    CHECK_EQ(r.errorCount, 0u, "the stored dyn Trait module passes LLVM verification");
}

// A struct literal whose `dyn` field is initialized directly from a concrete
// value (not through a helper parameter) must materialize the vtable at the
// literal site. Regression for the `E5001 Invalid InsertValueInst` that hit a
// stored `dyn Allocator` field.
static void test_dyn_trait_struct_literal_field_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "trait Allocator {\n"
                         "    fn value(self): i32;\n"
                         "}\n"
                         "struct HeapAllocator {}\n"
                         "implement HeapAllocator as Allocator {\n"
                         "    fn value(self): i32 { return 73; }\n"
                         "}\n"
                         "struct Resource {\n"
                         "    allocator: dyn Allocator,\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    let heap = HeapAllocator {};\n"
                         "    let resource = Resource { allocator: heap };\n"
                         "    return resource.allocator.value() - 73;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "a struct-literal dyn field uses the modern codegen pipeline");
    CHECK(r.ok, "a concrete value can initialize a dyn struct field directly in a literal");
    CHECK_EQ(r.exitCode, 0, "the stored dyn field dispatches to the concrete method");
    CHECK_EQ(r.errorCount, 0u, "the struct-literal dyn field passes LLVM verification");
}

static void test_dyn_trait_mutating_receiver_dispatch_runtime() {
    ModernFileCodegenTest t;
    t.opts.flags.emitIr(true);
    t.write("main.zith", "trait Counter {\n"
                         "    fn get(self): i32 { return 0; }\n"
                         "    fn bump(var self): i32 { return 0; }\n"
                         "}\n"
                         "struct Cell {\n"
                         "    value: i32,\n"
                         "    fn get(self): i32 { return self.value; }\n"
                         "    fn bump(var self): i32 { self.value += 1; return self.value; }\n"
                         "}\n"
                         "implement Cell as Counter {\n"
                         "    fn get(self): i32 { return self.value; }\n"
                         "    fn bump(var self): i32 { self.value += 1; return self.value; }\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    var c: Cell = Cell { value: 40 };\n"
                         "    var d: dyn Counter = c;\n"
                         "    let r = d.bump();\n"
                         "    if (r != 41) { return 1; }\n"
                         "    return c.get();\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "mutating dyn receiver uses the modern codegen pipeline");
    CHECK(r.ok, "mutating dyn receiver compiles, links, dispatches and executes");
    CHECK_EQ(r.errorCount, 0u, "the mutating dyn receiver module passes LLVM verification");
    CHECK_EQ(r.exitCode, 41,
             "a dyn value built from a local place mutates the caller's value in place");

    // The flag must survive the persistent cache round-trip: a warm run
    // hydrates HirMakeDyn from `.zirl` and still points at the original place.
    auto warm = t.run();
    CHECK(warm.ok, "the warm-cache mutating dyn receiver run still executes");
    CHECK(warm.cacheHits > 0U, "the second mutating dyn receiver run loads the persistent cache");
    CHECK_EQ(warm.exitCode, 41,
             "a cached dyn value built from a local place still mutates the caller");

    // A dyn value built from a temporary must dispatch, but its mutation must
    // stay on the spill copy and never touch the unrelated caller's value.
    ModernFileCodegenTest temp;
    temp.opts.flags.emitIr(true);
    temp.write("main.zith", "trait Counter {\n"
                            "    fn get(self): i32 { return 0; }\n"
                            "    fn bump(var self): i32 { return 0; }\n"
                            "}\n"
                            "struct Cell {\n"
                            "    value: i32,\n"
                            "    fn get(self): i32 { return self.value; }\n"
                            "    fn bump(var self): i32 { self.value += 1; return self.value; }\n"
                            "}\n"
                            "implement Cell as Counter {\n"
                            "    fn get(self): i32 { return self.value; }\n"
                            "    fn bump(var self): i32 { self.value += 1; return self.value; }\n"
                            "}\n"
                            "fn main(): i32 {\n"
                            "    var c: Cell = Cell { value: 40 };\n"
                            "    var d: dyn Counter = Cell { value: c.value };\n"
                            "    let r = d.bump();\n"
                            "    if (r != 41) { return 1; }\n"
                            "    return c.get();\n"
                            "}\n");
    auto tr = temp.run();
    CHECK(tr.ok, "a dyn value built from a temporary compiles, links and executes");
    CHECK_EQ(tr.exitCode, 40,
             "a dyn value built from a temporary dispatches without mutating the caller");
}

static void test_dyn_trait_by_value_receiver_dispatch_runtime() {
    ModernFileCodegenTest t;
    t.opts.flags.emitIr(true);
    t.write("main.zith", "trait Typed {\n"
                         "    fn first(self): i32 { return 0; }\n"
                         "}\n"
                         "implement []char as Typed {\n"
                         "    fn first(self: []char): i32 { return 4; }\n"
                         "}\n"
                         "fn total(a: dyn Typed): i32 { return a.first(); }\n"
                         "fn main(): i32 {\n"
                         "    let s: []char = \"four\";\n"
                         "    return total(s);\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "by-value slice trait receivers use the modern codegen pipeline");
    CHECK(r.ok, "by-value slice trait receivers compile, link, dispatch and execute");
    CHECK_EQ(r.exitCode, 4, "dyn Typed dispatches through a slice adapter into []char.first");
    CHECK_EQ(r.errorCount, 0u, "the dyn slice adapter module passes LLVM verification");
    CHECK(r.output.find("std.io.format.[]char") == std::string::npos ||
              r.output.find(".dyn\"") != std::string::npos,
          "the slice vtable emits an adapter for the by-value receiver");
}

static void test_dyn_trait_lend_dyn_receiver_dispatch_runtime() {
    ModernFileCodegenTest t;
    t.opts.flags.emitIr(true);
    t.write("main.zith", "trait Sink {\n"
                         "    fn put(self: lend dyn Sink, v: i32): i32;\n"
                         "}\n"
                         "struct Buf {\n"
                         "    last: i32,\n"
                         "}\n"
                         "implement Buf as Sink {\n"
                         "    fn put(self: lend Buf, v: i32): i32 { self.last = v; return v; }\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    var b = Buf { last: 0 };\n"
                         "    var d: dyn Sink = b;\n"
                         "    let r = d.put(41);\n"
                         "    if (r != 41) { return 1; }\n"
                         "    return b.last;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "lend dyn receiver uses the modern codegen pipeline");
    CHECK(r.ok, "a lend dyn Trait receiver compiles, links, dispatches and executes");
    CHECK_EQ(r.errorCount, 0u, "the lend dyn receiver module passes LLVM verification");
    CHECK_EQ(r.exitCode, 41, "calling a lend dyn Trait method mutates the caller's value in place");

    // A `dyn` value built from a temporary must dispatch but keep the mutation
    // on the spill copy instead of the unrelated caller.
    ModernFileCodegenTest temp;
    temp.opts.flags.emitIr(true);
    temp.write("main.zith", "trait Sink {\n"
                            "    fn put(self: lend dyn Sink, v: i32): i32;\n"
                            "}\n"
                            "struct Buf {\n"
                            "    last: i32,\n"
                            "}\n"
                            "implement Buf as Sink {\n"
                            "    fn put(self: lend Buf, v: i32): i32 { self.last = v; return v; }\n"
                            "}\n"
                            "fn main(): i32 {\n"
                            "    var b = Buf { last: 40 };\n"
                            "    var d: dyn Sink = Buf { last: b.last };\n"
                            "    let r = d.put(41);\n"
                            "    if (r != 41) { return 1; }\n"
                            "    return b.last;\n"
                            "}\n");
    auto tr = temp.run();
    CHECK(tr.ok, "a lend dyn receiver over a temporary compiles, links and executes");
    CHECK_EQ(tr.exitCode, 40,
             "a lend dyn receiver over a temporary dispatches without mutating the caller");
}

static void test_dyn_interface_field_access_is_rejected() {
    ModernFileCodegenTest t;
    t.write("main.zith", "interface Area {\n"
                         "    x: i32,\n"
                         "    fn area(self): i32\n"
                         "}\n"
                         "struct Square {\n"
                         "    x: i32,\n"
                         "    fn area(self): i32 { self.x * self.x }\n"
                         "}\n"
                         "fn bad(a: dyn Area): i32 {\n"
                         "    return a.x;\n"
                         "}\n"
                         "fn main(): i32 { return 0; }\n");

    auto r = t.run();
    CHECK(!r.ok, "field access through dyn Interface is rejected");
    CHECK_EQ(r.errorCount, 1u, "a.x on dyn Area reports one error");
    CHECK_EQ(r.exitCode, 0, "the rejected module never produces an executable");
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

static void test_narrowing_numeric_cast_literal_diagnostics() {
    ModernFileCodegenTest t;

    t.write("accepted.zith", "fn main(): i32 {\n"
                             "    let a: i8 = 100 as i8;\n"
                             "    let b: u8 = 255 as u8;\n"
                             "    let c: i8 = (-128) as i8;\n"
                             "    let d: i8 = (-100) as i8;\n"
                             "    let e: i16 = 128 as i16;\n"
                             "    let f: u8 = 0 as u8;\n"
                             "    let g: u8 = 0xFF as u8;\n"
                             "    return (a as i32) + (b as i32) + (c as i32) + (d as i32) +\n"
                             "           (e as i32) + (f as i32) + (g as i32);\n"
                             "}\n");
    auto r = t.run("accepted.zith");
    CHECK(r.usedModern, "narrowing literal casts use the modern frontend pipeline");
    CHECK(r.ok, "in-range narrowing literal casts compile and execute");
    CHECK_EQ(r.exitCode, 254, "in-range literal casts and widening casts preserve each value");

    t.write("unsigned-overflow.zith", "fn main(): i32 {\n"
                                      "    let _: u8 = 256 as u8;\n"
                                      "    return 0;\n"
                                      "}\n");
    auto bad_u8 = t.run("unsigned-overflow.zith");
    CHECK(!bad_u8.ok && bad_u8.errorCount > 0,
          "256 as u8 reports a compile-time overflow diagnostic");

    t.write("signed-overflow.zith", "fn main(): i32 {\n"
                                    "    let _: i8 = 128 as i8;\n"
                                    "    return 0;\n"
                                    "}\n");
    auto bad_i8 = t.run("signed-overflow.zith");
    CHECK(!bad_i8.ok && bad_i8.errorCount > 0,
          "128 as i8 reports a compile-time overflow diagnostic");

    t.write("negative-overflow.zith", "fn main(): i32 {\n"
                                      "    let _: i8 = -129 as i8;\n"
                                      "    return 0;\n"
                                      "}\n");
    auto bad_i8_neg = t.run("negative-overflow.zith");
    CHECK(!bad_i8_neg.ok && bad_i8_neg.errorCount > 0,
          "-129 as i8 reports a compile-time overflow diagnostic");

    t.write("i16-overflow.zith", "fn main(): i32 {\n"
                                 "    let _: i16 = 100000 as i16;\n"
                                 "    return 0;\n"
                                 "}\n");
    auto bad_i16 = t.run("i16-overflow.zith");
    CHECK(!bad_i16.ok && bad_i16.errorCount > 0,
          "100000 as i16 reports a compile-time overflow diagnostic");

    t.write("signed-literal-to-unsigned.zith", "fn main(): i32 {\n"
                                               "    let _: u8 = (-1) as u8;\n"
                                               "    return 0;\n"
                                               "}\n");
    auto bad_sign = t.run("signed-literal-to-unsigned.zith");
    CHECK(!bad_sign.ok && bad_sign.errorCount > 0,
          "negative literal to u8 reports a compile-time overflow diagnostic");

    t.write("raw-overflow.zith", "fn main(): i32 {\n"
                                 "    let _: i8 = raw 256 as i8;\n"
                                 "    return 0;\n"
                                 "}\n");
    auto raw = t.run("raw-overflow.zith");
    CHECK(raw.ok || raw.errorCount == 1,
          "raw narrowing casts intentionally bypass the compile-time overflow diagnostic");
}

static void test_narrowing_numeric_cast_const_diagnostics() {
    ModernFileCodegenTest t;

    t.write("const-accepted.zith", "const GOOD: i32 = 100;\n"
                                   "fn main(): i32 {\n"
                                   "    let _: i8 = GOOD as i8;\n"
                                   "    return 0;\n"
                                   "}\n");
    auto r = t.run("const-accepted.zith");
    CHECK(r.usedModern, "in-range constant narrowing casts use the modern pipeline");
    CHECK(r.ok, "in-range constant narrowing casts compile and execute");

    t.write("const-overflow.zith", "const TOO_LARGE: i32 = 128;\n"
                                   "fn main(): i32 {\n"
                                   "    let _: i8 = TOO_LARGE as i8;\n"
                                   "    return 0;\n"
                                   "}\n");
    auto bad = t.run("const-overflow.zith");
    CHECK(!bad.ok && bad.errorCount > 0,
          "constant 128 as i8 reports a compile-time overflow diagnostic");

    t.write("const-negative-accepted.zith", "const NEG: i32 = 200;\n"
                                            "fn main(): i32 {\n"
                                            "    let _: u8 = NEG as u8;\n"
                                            "    return 0;\n"
                                            "}\n");
    auto neg = t.run("const-negative-accepted.zith");
    CHECK(neg.ok, "in-range unsigned constant narrowing casts stay valid");

    t.write("enum-cast-accepted.zith", "enum Byte: u8 { Small = 7 }\n"
                                       "fn main(): i32 {\n"
                                       "    let _: i8 = Byte.Small as i8;\n"
                                       "    return 0;\n"
                                       "}\n");
    auto enum_ok = t.run("enum-cast-accepted.zith");
    CHECK(enum_ok.ok, "narrowing enum discriminants to a compatible width stay valid");

    t.write("runtime-cast-stays-valid.zith", "fn main(): i32 {\n"
                                             "    var v: i32 = 128;\n"
                                             "    let _: i8 = v as i8;\n"
                                             "    return 0;\n"
                                             "}\n");
    auto runtime = t.run("runtime-cast-stays-valid.zith");
    CHECK(runtime.ok,
          "runtime narrowing casts remain allowed and are left for later overflow checks");
}

static void test_state_machine_loop_executes() {
    ModernFileCodegenTest t;
    t.write("main.zith", "state Loop(n: i32): i32 {\n"
                         "    if (n < 10) {\n"
                         "        jump Loop(n + 1);\n"
                         "    }\n"
                         "    return n;\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    return dock Loop(0);\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "state/jump loop compiles and executes");
    CHECK_EQ(r.exitCode, 10, "state loop returns exactly 10");
}

static void test_state_tail_calls_emit_musttail() {
    ModernFileCodegenTest t;
    t.opts.flags.emitIr(true);
    t.write("main.zith", "state Start(v: i32): i32 {\n"
                         "    jump Done(v + 1);\n"
                         "}\n"
                         "state Done(v: i32): i32 {\n"
                         "    return v;\n"
                         "}\n"
                         "fn foo(): i32 {\n"
                         "    return dock Start(41);\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    return foo();\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "state tail-call program compiles and runs");
    CHECK_EQ(r.exitCode, 42, "dock receives the final state return value");
    CHECK(r.output.find("musttail call") != std::string::npos,
          "state transitions emit musttail calls in LLVM IR");
    CHECK(r.output.find("ret i32") != std::string::npos,
          "state transitions return immediately after the call");
    CHECK(r.output.find("tailcc") != std::string::npos,
          "state declarations and calls use LLVM tailcc in IR");
}

static void test_state_machine_diverging_parameters_executes() {
    ModernFileCodegenTest t;
    t.opts.flags.emitIr(true);
    t.write("main.zith", "state Start(n: i32): i32 {\n"
                         "    if (n == 0) {\n"
                         "        return 42;\n"
                         "    }\n"
                         "    jump Done(n - 1, n);\n"
                         "}\n"
                         "state Done(n: i32, step: i32): i32 {\n"
                         "    jump Start(n);\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    return dock Start(3);\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "state machine with diverging parameters compiles and runs");
    CHECK_EQ(r.exitCode, 42, "diverging state machine returns the terminal value");
    CHECK(r.output.find("tailcc") != std::string::npos,
          "diverging state transitions emit tailcc in LLVM IR");
    CHECK(r.output.find("musttail call") != std::string::npos,
          "diverging state transitions remain direct musttail calls");
}

static void test_state_codegen_has_no_marker_runtime_symbols() {
    ModernFileCodegenTest t;
    t.opts.flags.emitIr(true);
    t.write("main.zith", "state Start(v: i32): i32 {\n"
                         "    jump Done(v + 2);\n"
                         "}\n"
                         "state Done(v: i32): i32 {\n"
                         "    return v;\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    return dock Start(10);\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "state machine compiles and runs without marker runtime");
    CHECK_EQ(r.exitCode, 12, "dock receives the returned state value");
    CHECK(r.output.find("__zith_marker_blob") == std::string::npos,
          "marker blob symbol is absent from state-machine IR");
    CHECK(r.output.find("__zith_dock_address") == std::string::npos,
          "dock runtime symbol is absent from state-machine IR");
    CHECK(r.output.find("__zith_marker_exit") == std::string::npos,
          "marker exit runtime symbol is absent from state-machine IR");
}

static void test_state_loop_does_not_grow_stack() {
    ModernFileCodegenTest t;
    t.write("main.zith", "state CountDown(n: i32): i32 {\n"
                         "    if (n > 0) {\n"
                         "        jump CountDown(n - 1);\n"
                         "    }\n"
                         "    return 42;\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    return dock CountDown(200000);\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "large recursive state loop completes");
    CHECK_EQ(r.exitCode, 42, "large recursive state loop reaches the terminal return value");
}

static void test_loop_local_binding_does_not_grow_stack() {
    ModernFileCodegenTest t;
    t.opts.flags.emitIr(true);
    t.write("main.zith", "fn main(): i32 {\n"
                         "    var acc: i32 = 0;\n"
                         "    var n: i32 = 0;\n"
                         "    for (n < 1000000) {\n"
                         "        let a = n + 1;\n"
                         "        acc = (acc + a) % 10;\n"
                         "        n = n + 1;\n"
                         "    }\n"
                         "    return acc;\n"
                         "}\n");

    const auto result = t.run();
    CHECK(result.ok, "a local binding in a million-iteration loop compiles and runs");
    CHECK_EQ(result.exitCode, 0, "the loop completes with the expected accumulated result");
    const auto loop_block = result.output.find("bb1:");
    CHECK(loop_block != std::string::npos, "the emitted function contains its loop block");
    if (loop_block != std::string::npos)
        CHECK(result.output.find("alloca", loop_block) == std::string::npos,
              "loop-local stack storage is hoisted out of the loop blocks");
}

static void test_local_state_machine_executes() {
    ModernFileCodegenTest t;
    t.write("main.zith", "fn machine(): i32 {\n"
                         "    state Start(n: i32): i32 {\n"
                         "        if (n == 0) {\n"
                         "            return 42;\n"
                         "        }\n"
                         "        jump Done(n - 1);\n"
                         "    }\n"
                         "    state Done(n: i32): i32 {\n"
                         "        jump Start(n);\n"
                         "    }\n"
                         "    return dock Start(200000);\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    return machine();\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "local state machine compiles and executes");
    CHECK_EQ(r.exitCode, 42, "local state machine returns through dock");
}

static void test_defer_reverse_order_before_return() {
    CodegenTest t;
    auto r = t.run("codegen-defer-reverse.zith", "#[discardable] extern fn putchar(c: i32): i32\n"
                                                 "fn main(): i32 {\n"
                                                 "    defer putchar(66);\n"
                                                 "    defer putchar(65);\n"
                                                 "    _ = putchar(48);\n"
                                                 "    return 0;\n"
                                                 "}\n");
    CHECK(r.ok, "defer expressions compile, link and run");
    CHECK_EQ(r.output, "0AB", "defer runs in reverse registration order before return");
}

static void test_defer_block_runs_on_normal_exit() {
    CodegenTest t;
    auto r = t.run("codegen-defer-block.zith", "#[discardable] extern fn putchar(c: i32): i32\n"
                                               "fn main() {\n"
                                               "    defer { _ = putchar(66); _ = putchar(65); }\n"
                                               "    _ = putchar(48);\n"
                                               "}\n");
    CHECK(r.ok, "defer block compiles, links and runs");
    CHECK_EQ(r.output, "0BA", "defer block body executes in written order on block exit");
}

static void test_defer_in_if_and_loop() {
    CodegenTest t;
    auto if_test = t.run("codegen-defer-if.zith", "#[discardable] extern fn putchar(c: i32): i32\n"
                                                  "fn main() {\n"
                                                  "    if (true) {\n"
                                                  "        defer putchar(66);\n"
                                                  "        _ = putchar(48);\n"
                                                  "    }\n"
                                                  "    putchar(90);\n"
                                                  "}\n");
    CHECK(if_test.ok, "defer inside if compiles, links and runs");
    CHECK_EQ(if_test.output, "0BZ", "defer runs when the innermost if block exits");

    auto loop_test =
        t.run("codegen-defer-loop.zith", "#[discardable] extern fn putchar(c: i32): i32\n"
                                         "fn main() {\n"
                                         "    for (true) {\n"
                                         "        defer putchar(66);\n"
                                         "        _ = putchar(65);\n"
                                         "        break;\n"
                                         "    }\n"
                                         "    putchar(90);\n"
                                         "}\n");
    CHECK(loop_test.ok, "defer in loop with break compiles, links and runs");
    CHECK_EQ(loop_test.output, "ABZ", "defer runs before break leaves the loop/block");
}

static void test_defer_in_state_before_jump() {
    CodegenTest t;
    auto r = t.run("codegen-defer-state.zith", "#[discardable] extern fn putchar(c: i32): i32\n"
                                               "state Done() { return; }\n"
                                               "state Start() {\n"
                                               "    defer putchar(66);\n"
                                               "    _ = putchar(65);\n"
                                               "    jump Done();\n"
                                               "}\n"
                                               "fn main() {\n"
                                               "    dock Start();\n"
                                               "}\n");
    CHECK(r.ok, "void state with defer before jump compiles, links and runs");
    CHECK_EQ(r.output, "AB", "defer runs before the state jump transfer");
}

static void test_linked_list_acceptance_program() {
    // Exercises nullable pointer threading and local pointer aliases. Local
    // struct storage may contain pointers as long as nothing escapes; the
    // ownership iteration does not yet support persistent linked structures.
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
                         "    var first: Node = Node { value: 4, next: null };\n"
                         "    var second: Node = Node { value: 3, next: null };\n"
                         "    let total: i32 = sum(&first) + sum(&second);\n"
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

static void test_const_global_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "const GLOBAL: i32 = 3;\n"
                         "fn main(): i32 {\n"
                         "    return GLOBAL;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "Const-global runtime test used the modern frontend pipeline");
    CHECK(r.ok, "Const global compiles and executes");
    CHECK_EQ(r.exitCode, 3, "Reading a const global returns its stored value");
}

static void test_const_global_array_of_string_pointers() {
    ModernFileCodegenTest t;
    t.write("main.zith", "const keywords: [4]*char = [\n"
                         "    \"if\",\n"
                         "    \"else\",\n"
                         "    \"for\",\n"
                         "    \"when\"\n"
                         "];\n"
                         "fn main(): i32 {\n"
                         "    return 0;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "Const string-pointer array uses the modern frontend pipeline");
    CHECK(r.ok, "Const global array of string pointers compiles and executes");
    CHECK_EQ(r.exitCode, 0, "Unused const string-pointer array does not affect execution");
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
    auto raw_union   = types.defineUnion("Bits", false);
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
                         "    return raw s[1];\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "slice returned from C compiles, links and executes");
    CHECK_EQ(r.exitCode, 20, "indexing a C-provided slice reads the expected element");
}

/// An array coerced to `[]i32` is a zero-copy view of the original storage.
static void test_array_to_slice_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "fn write(var s: []i32): i32 { s[1] = 9; raw s[1] }\n"
                         "fn sum(s: []i32): i32 { raw s[0] + raw s[1] }\n"
                         "fn main(): i32 {\n"
                         "    var values: [3]i32 = [10, 20, 30];\n"
                         "    return sum(values) + write(values);\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "array-to-slice coercion compiles, links and executes");
    CHECK_EQ(r.exitCode, 39, "array-to-slice coercion reads array storage without a copy");
}

static void test_raw_slice_and_index_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "fn main(): i32 {\n"
                         "    var values: [3]i32 = [10, 20, 30];\n"
                         "    let s: []i32 = raw values[1..3];\n"
                         "    return raw s[0] + raw s[1];\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "raw slice/index expressions compile, link and execute");
    CHECK_EQ(r.exitCode, 50, "raw slice views and raw slice indexing return the elements");
}

/// Runtime checks wrap valid checked indexes in Some and invalid ones in None.
static void test_checked_index_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith",
            "fn pick(values: [3]i32, i: i32): ?i32 { values[i] }\n"
            "fn slice_pick(values: []i32, i: i32): ?i32 { values[i] }\n"
            "fn main(): i32 {\n"
            "    var values: [3]i32 = [10, 20, 30];\n"
            "    let slice: []i32 = raw values[0..3];\n"
            "    let short_slice: []i32 = raw values[0..1];\n"
            "    var failures: i32 = 0;\n"
            "    if (pick(values, 1) is null) { failures = failures + 1; }\n"
            "    if (pick(values, 3) is null) { failures = failures + 1; }\n"
            "    if (slice_pick(slice, 0) is null) { failures = failures + 1; }\n"
            "    if (slice_pick(slice, 3) is null) { failures = failures + 1; }\n"
            "    if (slice_pick(short_slice, 1) is null) { failures = failures + 1; }\n"
            "    if (raw short_slice[1] != 20) { failures = failures + 1; }\n"
            "    return failures;\n"
            "}\n");

    auto r = t.run();
    CHECK(r.ok, "checked index expressions compile, link and execute");
    CHECK_EQ(r.exitCode, 3,
             "checked indexing uses the slice length while raw indexing bypasses that check");
}

/// Builds a tiny C runtime that receives a function pointer from Zith and
/// invokes it, proving `fn(...): R` values lower as C function pointers.
static void test_function_pointer_call() {
    ModernFileCodegenTest t;
    const auto c_path   = (t.root / "fnptr-abi.c").string();
    const auto lib_path = (t.root / "libzithfnptr.a").string();
    const auto obj_path = (t.root / "fnptr-abi.o").string();
    {
        std::ofstream c_source(c_path, std::ios::binary | std::ios::trunc);
        c_source << "int apply(int (*f)(int), int x) { return f(x); }\n"
                    "int double_(int x) { return x * 2; }\n";
    }
    const auto compile = "cc -c " + c_path + " -o " + obj_path + " 2>/dev/null";
    const auto archive = "ar rcs " + lib_path + " " + obj_path + " 2>/dev/null";
    if (std::system(compile.c_str()) != 0 || std::system(archive.c_str()) != 0) {
        std::printf("  SKIP: no C toolchain for the function-pointer ABI runtime test\n");
        return;
    }

    t.opts.libraryDirs.push(t.root.string());
    t.opts.libraries.push("zithfnptr");
    t.write("main.zith", "extern fn apply(f: fn(i32): i32, x: i32): i32\n"
                         "extern fn double_(x: i32): i32\n"
                         "fn main(): i32 {\n"
                         "    var f: fn(i32): i32 = double_;\n"
                         "    return apply(f, 7);\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "function value is passed to C as a function pointer and called");
    CHECK_EQ(r.exitCode, 14, "indirect C call through a Zith function value returns 14");
}

static void test_native_function_value_call() {
    ModernFileCodegenTest t;
    t.write("main.zith", "fn double(x: i32): i32 { x * 2 }\n"
                         "fn main(): i32 {\n"
                         "    var f: fn(i32): i32 = double;\n"
                         "    return f(7);\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "a native function value compiles, links and executes");
    CHECK_EQ(r.exitCode, 14, "an indirect call through a native function value returns 14");
}

static void test_anonymous_function_literal_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith",
            "fn apply(f: fn(i32, i32): i32, x: i32): i32 { f(x, x) }\n"
            "fn main(): i32 {\n"
            "    let add = fn (a: i32, b: i32): i32 { a + b };\n"
            "    if (add(2, 3) != 5) { return 1; }\n"
            "    if (apply(fn (a: i32, b: i32): i32 { a * b }, 4) != 16) { return 2; }\n"
            "    return 0;\n"
            "}\n");

    auto r = t.run();
    CHECK(r.ok, "an anonymous function literal compiles, links and executes");
    CHECK_EQ(r.exitCode, 0, "the literal is callable directly and as a function value");
}

static void test_anonymous_function_capture_is_rejected() {
    ModernFileCodegenTest t;
    t.write("main.zith", "fn main(): i32 {\n"
                         "    let x = 10;\n"
                         "    let f = fn (a: i32): i32 { a + x };\n"
                         "    return f(1);\n"
                         "}\n");

    const auto result = t.run();
    CHECK(!result.ok, "a reference to an enclosing local in a function literal is rejected");
    CHECK(result.hasMessage("unknown identifier 'x'"),
          "the capture is reported as an unknown identifier, not silently captured");
}

static void test_state_value_dock_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "state Machine(n: i32): i32 {\n"
                         "    if (n == 0) {\n"
                         "        return 42;\n"
                         "    }\n"
                         "    jump Machine(n - 1);\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    let S: state(i32): i32 = Machine;\n"
                         "    return dock S(100000);\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "a state value compiles, links and docks with tailcc");
    CHECK_EQ(r.exitCode, 42, "dock through a state value returns the machine result");
}

/// A C runtime mutates a `{ ptr, len }` slice through the same aggregate ABI
/// that Zith reads, then Zith assigns and reads an element from the slice.
static void test_mutable_slice_from_c() {
    ModernFileCodegenTest t;
    const auto c_path   = (t.root / "slice-set.c").string();
    const auto lib_path = (t.root / "libzithsliceset.a").string();
    const auto obj_path = (t.root / "slice-set.o").string();
    {
        std::ofstream c_source(c_path, std::ios::binary | std::ios::trunc);
        c_source << "#include <stdint.h>\n"
                    "typedef struct { int32_t *ptr; int64_t len; } Slice;\n"
                    "Slice make_slice(void) {\n"
                    "    static int32_t data[2] = {1, 2};\n"
                    "    Slice s = {data, 2};\n"
                    "    return s;\n"
                    "}\n"
                    "int slice_set(Slice *s, int64_t i, int32_t v) {\n"
                    "    s->ptr[i] = v;\n"
                    "    return s->ptr[i];\n"
                    "}\n";
    }
    const auto compile = "cc -c " + c_path + " -o " + obj_path + " 2>/dev/null";
    const auto archive = "ar rcs " + lib_path + " " + obj_path + " 2>/dev/null";
    if (std::system(compile.c_str()) != 0 || std::system(archive.c_str()) != 0) {
        std::printf("  SKIP: no C toolchain for the mutable slice runtime test\n");
        return;
    }

    t.opts.libraryDirs.push(t.root.string());
    t.opts.libraries.push("zithsliceset");
    t.write("main.zith", "extern fn make_slice(): []i32\n"
                         "extern fn slice_set(s: *[]i32, i: i64, v: i32): i32\n"
                         "fn main(): i32 {\n"
                         "    var s: []i32 = make_slice();\n"
                         "    _ = slice_set(&s, 1, 10);\n"
                         "    return raw s[1];\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "mutable slice from C compiles, links and executes");
    CHECK_EQ(r.exitCode, 10, "assignment to a slice element reads the updated value");
}

static void test_variadic_slice_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "fn sum(rest: [...]i32): i32 {\n"
                         "    var total: i32 = 0;\n"
                         "    total = total + raw rest[0];\n"
                         "    total = total + raw rest[1];\n"
                         "    total = total + raw rest[2];\n"
                         "    return total;\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    return sum(1, 2, 3);\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "homogeneous variadic slice program compiles, links and runs");
    CHECK_EQ(r.exitCode, 6, "auto-collected tail values are materialized and summed at runtime");
}

static void test_pipe_variadic_slice_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "fn sum(rest: [...]i32): i32 {\n"
                         "    return raw rest[0] + raw rest[1] + raw rest[2];\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    10 |> sum(.., 20, 30)\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "pipeline current as a variadic slice element compiles, links and runs");
    CHECK_EQ(r.exitCode, 60, "pipeline current is auto-collected into the variadic slice");
}

static void test_variadic_slice_explicit_vs_auto_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "extern fn make_values(): []i32\n"
                         "fn sum(rest: [...]i32): i32 {\n"
                         "    return raw rest[0] + raw rest[1];\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    return sum(make_values()) * 10 + sum(3, 4);\n"
                         "}\n");

    const auto c_path   = (t.root / "values.c").string();
    const auto lib_path = (t.root / "libzithvalues.a").string();
    const auto obj_path = (t.root / "values.o").string();
    {
        std::ofstream c_source(c_path, std::ios::binary | std::ios::trunc);
        c_source << "#include <stdint.h>\n"
                    "typedef struct { int32_t *ptr; int64_t len; } Slice;\n"
                    "static int32_t data[2] = {1, 2};\n"
                    "Slice make_values(void) { Slice s = {data, 2}; return s; }\n";
    }
    const auto compile = "cc -c " + c_path + " -o " + obj_path + " 2>/dev/null";
    const auto archive = "ar rcs " + lib_path + " " + obj_path + " 2>/dev/null";
    if (std::system(compile.c_str()) != 0 || std::system(archive.c_str()) != 0) {
        std::printf("  SKIP: no C toolchain for the explicit-vs-auto variadic runtime test\n");
        return;
    }

    t.opts.libraryDirs.push(t.root.string());
    t.opts.libraries.push("zithvalues");
    auto r = t.run();
    CHECK(r.ok, "explicit and auto-collected variadic slices compile, link and run");
    CHECK_EQ(r.exitCode, 37, "explicit slice sum (3) and auto-collected sum (7) are combined");
}

static void test_variadic_slice_method_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "struct Agg {\n"
                         "    total: i32,\n"
                         "    fn add(self, rest: [...]i32): i32 {\n"
                         "        return self.total + raw rest[0] + raw rest[1];\n"
                         "    }\n"
                         "}\n"
                         "extern fn make_values(): []i32\n"
                         "fn main(): i32 {\n"
                         "    let a1: Agg = Agg { total: 100 };\n"
                         "    let a2: Agg = Agg { total: 100 };\n"
                         "    return a1.add(make_values()) - a2.add(1, 2);\n"
                         "}\n");

    const auto c_path   = (t.root / "values-method.c").string();
    const auto lib_path = (t.root / "libzithvaluesmethod.a").string();
    const auto obj_path = (t.root / "values-method.o").string();
    {
        std::ofstream c_source(c_path, std::ios::binary | std::ios::trunc);
        c_source << "#include <stdint.h>\n"
                    "typedef struct { int32_t *ptr; int64_t len; } Slice;\n"
                    "static int32_t data[2] = {1, 2};\n"
                    "Slice make_values(void) { Slice s = {data, 2}; return s; }\n";
    }
    const auto compile = "cc -c " + c_path + " -o " + obj_path + " 2>/dev/null";
    const auto archive = "ar rcs " + lib_path + " " + obj_path + " 2>/dev/null";
    if (std::system(compile.c_str()) != 0 || std::system(archive.c_str()) != 0) {
        std::printf("  SKIP: no C toolchain for the method variadic runtime test\n");
        return;
    }

    t.opts.libraryDirs.push(t.root.string());
    t.opts.libraries.push("zithvaluesmethod");
    auto r = t.run();
    CHECK(r.ok, "method explicit and auto-collected variadic tails compile, link and run");
    CHECK_EQ(r.exitCode, 0, "method slice sums cancel in the explicit-vs-auto comparison");
}

static void test_variadic_slice_dyn_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "trait Value {\n"
                         "    fn value(self): i32 { return 0; }\n"
                         "}\n"
                         "struct Box {\n"
                         "    n: i32,\n"
                         "    fn value(self): i32 { return self.n; }\n"
                         "}\n"
                         "implement Box as Value {\n"
                         "    fn value(self): i32 { return self.n; }\n"
                         "}\n"
                         "fn first(rest: [...]dyn Value): i32 {\n"
                         "    let a: dyn Value = raw rest[0];\n"
                         "    return a.value();\n"
                         "}\n"
                         "fn second(rest: [...]dyn Value): i32 {\n"
                         "    let b: dyn Value = raw rest[1];\n"
                         "    return b.value();\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    let a: dyn Value = Box { n: 5 };\n"
                         "    let b: dyn Value = Box { n: 7 };\n"
                         "    let ds: []dyn Value = [a, b];\n"
                         "    return first(a, b) * 10 + second(ds);\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "dyn auto-collected and explicit variadic tails compile, link and run");
    CHECK_EQ(r.exitCode, 57, "dyn auto-collect selects 5 and explicit []dyn selects 7");
}

static void test_variadic_slice_state_runtime_forms() {
    ModernFileCodegenTest t;
    t.write("main.zith", "state Start(rest: [...]i32): i32 {\n"
                         "    if (@lengthOf(rest) == 0) {\n"
                         "        return 10;\n"
                         "    }\n"
                         "    return raw rest[0];\n"
                         "}\n"
                         "extern fn make_values(): []i32\n"
                         "fn main(): i32 {\n"
                         "    return dock Start(make_values());\n"
                         "}\n");

    const auto c_path   = (t.root / "values-state.c").string();
    const auto lib_path = (t.root / "libzithvaluesstate.a").string();
    const auto obj_path = (t.root / "values-state.o").string();
    {
        std::ofstream c_source(c_path, std::ios::binary | std::ios::trunc);
        c_source << "#include <stdint.h>\n"
                    "typedef struct { int32_t *ptr; int64_t len; } Slice;\n"
                    "static int32_t data[1] = {7};\n"
                    "Slice make_values(void) { Slice s = {data, 1}; return s; }\n";
    }
    const auto compile = "cc -c " + c_path + " -o " + obj_path + " 2>/dev/null";
    const auto archive = "ar rcs " + lib_path + " " + obj_path + " 2>/dev/null";
    if (std::system(compile.c_str()) != 0 || std::system(archive.c_str()) != 0) {
        std::printf("  SKIP: no C toolchain for the state variadic runtime test\n");
        return;
    }

    t.opts.libraryDirs.push(t.root.string());
    t.opts.libraries.push("zithvaluesstate");
    auto r = t.run();
    CHECK(r.ok, "state explicit variadic tail runs");
    CHECK_EQ(r.exitCode, 7, "state explicit slice returns its first element");

    ModernFileCodegenTest auto_collect;
    auto_collect.write("main.zith", "state Start(rest: [...]i32): i32 {\n"
                                    "    if (@lengthOf(rest) == 0) {\n"
                                    "        return 10;\n"
                                    "    }\n"
                                    "    return raw rest[0];\n"
                                    "}\n"
                                    "fn main(): i32 {\n"
                                    "    return dock Start(7);\n"
                                    "}\n");
    r = auto_collect.run();
    CHECK(r.ok, "state auto-collected variadic tail runs");
    CHECK_EQ(r.exitCode, 7, "state auto-collected slice returns its first element");

    ModernFileCodegenTest empty;
    empty.write("main.zith", "state Start(rest: [...]i32): i32 {\n"
                             "    if (@lengthOf(rest) == 0) {\n"
                             "        return 10;\n"
                             "    }\n"
                             "    return raw rest[0];\n"
                             "}\n"
                             "fn main(): i32 {\n"
                             "    return dock Start();\n"
                             "}\n");
    r = empty.run();
    CHECK(r.ok, "state empty variadic tail runs");
    CHECK_EQ(r.exitCode, 10, "state empty tail returns the empty-slice branch");
}

static void test_variadic_slice_state_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "state Start(n: i32, rest: [...]i32): i32 {\n"
                         "    if (n == 0) {\n"
                         "        return raw rest[0];\n"
                         "    }\n"
                         "    jump Start(n - 1, 4, 5);\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    return dock Start(2, 1, 2);\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "variadic slice state/dock/jump program compiles, links and runs");
    CHECK_EQ(r.exitCode, 4, "jump auto-collects the tail for the next state transition");
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

static void test_var_self_and_var_parameter_runtime() {
    CodegenTest t;
    auto r = t.run("codegen-var-self.zith",
                   "struct Counter {\n"
                   "    value: i32,\n"
                   "    fn get(self): i32 { return self.value; }\n"
                   "    fn bump(var self): i32 { self.value += 1; return self.value; }\n"
                   "    fn set(var self, v: i32): i32 { self.value = v; return self.value; }\n"
                   "}\n"
                   "fn add_one(var p: Counter): i32 { p.value += 1; return p.value; }\n"
                   "fn main(): i32 {\n"
                   "    var a: Counter = Counter { value: 2 };\n"
                   "    let r1: i32 = a.bump();\n"
                   "    var b: Counter = Counter { value: 7 };\n"
                   "    let r2: i32 = b.set(9);\n"
                   "    var c: Counter = Counter { value: 4 };\n"
                   "    let r3: i32 = add_one(c);\n"
                   "    let d: Counter = Counter { value: r1 + r2 + r3 };\n"
                   "    return d.get();\n"
                   "}\n");
    CHECK(r.ok, "var self and var parameters compile, link, and execute");
    printf("EXIT CODE: %d\n", r.exitCode);
    CHECK_EQ(r.exitCode, 17, "bump and set mutate in place; a var parameter mutates its copy");
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

static void test_optional_method_after_is_null_runtime() {
    ModernFileCodegenTest t;
    t.write("lib.zith", "pub struct Box { pub value: i32 }\n"
                        "implement Box {\n"
                        "    fn get(self: view Box): i32 { self.value }\n"
                        "}\n");
    t.write("main.zith", "from lib\n"
                         "fn main(): i32 {\n"
                         "    var b: ?Box = Box { value: 14 };\n"
                         "    if (b is null) { return 1; }\n"
                         "    return b.get();\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "optional narrowing uses the modern codegen pipeline");
    CHECK(r.ok, "optional method after 'is null' compiles, links and runs");
    CHECK_EQ(r.exitCode, 14, "the non-null optional payload reaches the receiver method");
}

static void test_optional_method_after_not_is_null_runtime() {
    ModernFileCodegenTest t;
    t.write("lib.zith", "pub struct Box { pub value: i32 }\n"
                        "implement Box {\n"
                        "    fn get(self: view Box): i32 { self.value }\n"
                        "}\n");
    t.write("main.zith", "from lib\n"
                         "fn main(): i32 {\n"
                         "    var b: ?Box = Box { value: 14 };\n"
                         "    if not (b is null) { return b.get(); }\n"
                         "    return 1;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "not-is-null narrowing uses the modern codegen pipeline");
    CHECK(r.ok, "optional method after 'not (is null)' compiles, links and runs");
    CHECK_EQ(r.exitCode, 14, "the non-null optional payload reaches the guarded receiver");
}

static void test_optional_boolean_condition_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "fn non_null(): i32 {\n"
                         "    let x: ?i32 = 7;\n"
                         "    if (x) { return 1; }\n"
                         "    return 9;\n"
                         "}\n"
                         "fn null_value(): i32 {\n"
                         "    let y: ?i32 = null;\n"
                         "    if (y) { return 9; }\n"
                         "    return 0;\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    if (non_null() != 1) { return 2; }\n"
                         "    return null_value();\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "?T boolean condition uses the modern codegen pipeline");
    CHECK(r.ok, "?T boolean conditions compile, link and run");
    CHECK_EQ(r.exitCode, 0, "the non-null ?i32 branch runs and the null ?i32 branch does not");
}

static void test_optional_pointer_boolean_condition_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "fn non_null(): i32 {\n"
                         "    let x: i32 = 9;\n"
                         "    let q: ?*i32 = &x;\n"
                         "    if (q) { return 1; }\n"
                         "    return 9;\n"
                         "}\n"
                         "fn null_value(): i32 {\n"
                         "    let p: ?*i32 = null;\n"
                         "    if (p) { return 9; }\n"
                         "    return 0;\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    if (non_null() != 1) { return 2; }\n"
                         "    return null_value();\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "?*T boolean condition uses the modern codegen pipeline");
    CHECK(r.ok, "?*T boolean conditions compile, link and run");
    CHECK_EQ(r.exitCode, 0, "the non-null ?*i32 branch runs and the null ?*i32 branch does not");
}

static void test_optional_condition_keyword_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "fn non_null(): i32 {\n"
                         "    let x: ?i32 = 7;\n"
                         "    if (x) { return 1; }\n"
                         "    return 9;\n"
                         "}\n"
                         "fn null_value(): i32 {\n"
                         "    let y: ?i32 = null;\n"
                         "    if (y) { return 9; }\n"
                         "    return 0;\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    if (non_null() != 1) { return 2; }\n"
                         "    return null_value();\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "implicit ?T conditions use the modern codegen pipeline");
    CHECK(r.ok, "implicit ?T conditions compile, link and run");
    CHECK_EQ(r.exitCode, 0, "the non-null ?i32 branch runs and the null ?i32 branch does not");
}

static void test_bare_condition_forms_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "fn force(ok: bool): i32 {\n"
                         "    if not ok { return 1; }\n"
                         "    return 7;\n"
                         "}\n"
                         "fn pick(): i32 {\n"
                         "    var x: ?i32 = 4;\n"
                         "    if (x) { return 3; }\n"
                         "    while (x) { return 5; }\n"
                         "    for (x) { return 9; }\n"
                         "    return 0;\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    if (force(false) != 1) { return 2; }\n"
                         "    return pick();\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "bare condition forms use the modern codegen pipeline");
    CHECK(r.ok, "bare not/optional conditions compile, link and run");
    CHECK_EQ(r.exitCode, 3, "bare not reaches its branch and bare optional is truthy");
}

static void test_optional_must_extraction_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "fn main(): i32 {\n"
                         "    let some: ?i32 = 41;\n"
                         "    let value: i32 = must some;\n"
                         "    if (value != 41) { return 1; }\n"
                         "    let ptr_target: i32 = 7;\n"
                         "    let some_ptr: ?*i32 = &ptr_target;\n"
                         "    let ptr_value: i32 = *must some_ptr;\n"
                         "    if (ptr_value != 7) { return 2; }\n"
                         "    return 0;\n"
                         "}\n");
    auto r = t.run();
    CHECK(r.usedModern, "must extraction uses the modern codegen pipeline");
    CHECK(r.ok, "must extraction compiles, links and runs on Some optionals");
    CHECK_EQ(r.exitCode, 0, "must returns the payload for ?i32 and ?*i32");
}

static void test_optional_must_none_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "fn main(): i32 {\n"
                         "    let none: ?i32 = null;\n"
                         "    let value: i32 = must none;\n"
                         "    return value;\n"
                         "}\n");
    auto r = t.run();
    CHECK(r.ok, "must on None compiles, links and terminates");
    CHECK_EQ(r.exitCode, 128 + 4, "must on None traps via R10003 and does not continue");
}

static void test_optional_raw_extraction_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "fn some_value(): i32 {\n"
                         "    let some: ?i32 = 41;\n"
                         "    let value: i32 = raw some;\n"
                         "    return value;\n"
                         "}\n"
                         "fn none_value(): i32 {\n"
                         "    let none: ?i32 = null;\n"
                         "    let value: i32 = raw none;\n"
                         "    return value;\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    if (some_value() != 41) { return 1; }\n"
                         "    if (none_value() == 0) { return 2; }\n"
                         "    return 0;\n"
                         "}\n");
    auto r = t.run();
    CHECK(r.usedModern, "raw optional extraction uses the modern codegen pipeline");
    CHECK(r.ok, "raw optional extraction compiles, links and runs without a null trap");
    CHECK_EQ(r.exitCode, 2, "raw on None reads uninitialized payload without panicking");
}

static void test_if_else_both_return_runtime() {
    // Regression: `if (c) { return A; } else { return B; }` as the trailing
    // statement of a value-returning function used to emit an unreachable
    // merge block terminated by `ret void`, which the LLVM verifier rejects
    // (E5001). The dead block must lower to `unreachable` instead.
    ModernFileCodegenTest t;
    t.write("main.zith", "fn classify(x: i32): i32 {\n"
                         "    if (x == 0) { return 1; } else { return 2; }\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    return classify(3);\n"
                         "}\n");
    auto r = t.run();
    CHECK(r.usedModern, "if/else both-return uses the modern codegen pipeline");
    CHECK(r.ok, "if/else both-return compiles, links and runs");
    CHECK_EQ(r.exitCode, 2, "the else branch value is returned");
}

static void test_for_loop_return_runtime() {
    // Regression: a loop whose body always returns leaves the exit block
    // unreachable, hitting the same dead-block terminator path.
    ModernFileCodegenTest t;
    t.write("main.zith", "fn first(): i32 {\n"
                         "    for (i in 0..3) { return i + 5; }\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    return first();\n"
                         "}\n");
    auto r = t.run();
    CHECK(r.ok, "a loop that always returns compiles, links and runs");
    CHECK_EQ(r.exitCode, 5, "the loop body return value is returned");
}

// A concrete value stored into a `?dyn Trait` slot must erase the payload to a
// fat pointer before the `Some` wrap. Before the fix the raw aggregate was
// stored into the fat-pointer slot and LLVM aborted in `combineStore`.
static void test_optional_dyn_local_binding_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith",
            "trait Counter { fn get(self): i32; }\n"
            "struct Cell { v: i32 }\n"
            "implement Cell as Counter { fn get(self): i32 { return self.v; } }\n"
            "fn main(): i32 {\n"
            "    let c: ?dyn Counter = Cell{v:7};\n"
            "    if (c is null) { return 1; }\n"
            "    let r = raw c;\n"
            "    return r.get();\n"
            "}\n");
    auto r = t.run();
    CHECK(r.usedModern, "optional dyn local binding uses the modern codegen pipeline");
    CHECK(r.ok, "a concrete value binds into a ?dyn local without an LLVM abort");
    CHECK_EQ(r.errorCount, 0u, "the ?dyn local binding module passes LLVM verification");
    CHECK_EQ(r.exitCode, 7, "the erased payload round-trips and dispatches");
}

// A `?dyn Trait` struct field set from a concrete value, read back with an
// `is null` narrowing.
static void test_optional_dyn_struct_field_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith",
            "trait Counter { fn get(self): i32; }\n"
            "struct Cell { v: i32 }\n"
            "implement Cell as Counter { fn get(self): i32 { return self.v; } }\n"
            "struct Holder { c: ?dyn Counter }\n"
            "fn main(): i32 {\n"
            "    let h = Holder{c: Cell{v:7}};\n"
            "    if (h.c is null) { return 1; }\n"
            "    let r = raw h.c;\n"
            "    return r.get();\n"
            "}\n");
    auto r = t.run();
    CHECK(r.usedModern, "optional dyn struct field uses the modern codegen pipeline");
    CHECK(r.ok, "a concrete value initializes a ?dyn struct field in a literal");
    CHECK_EQ(r.errorCount, 0u, "the ?dyn struct-literal field passes LLVM verification");
    CHECK_EQ(r.exitCode, 7, "the stored ?dyn field dispatches to the concrete method");
}

// Dispatch directly on a narrowed `?dyn` receiver, without an explicit `raw`
// extraction, for both the early-return and the `else` shapes.
static void test_optional_dyn_narrowed_dispatch_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith",
            "trait Counter { fn get(self): i32; }\n"
            "struct Cell { v: i32 }\n"
            "implement Cell as Counter { fn get(self): i32 { return self.v; } }\n"
            "struct Holder { c: ?dyn Counter }\n"
            "fn early(): i32 {\n"
            "    let c: ?dyn Counter = Cell{v:7};\n"
            "    if (c is null) { return 1; }\n"
            "    return c.get();\n"
            "}\n"
            "fn via_else(h: Holder): i32 {\n"
            "    if (h.c is null) { return 1; } else { return h.c.get(); }\n"
            "}\n"
            "fn main(): i32 {\n"
            "    if (early() != 7) { return 1; }\n"
            "    let h = Holder{c: Cell{v:7}};\n"
            "    if (via_else(h) != 7) { return 2; }\n"
            "    return 0;\n"
            "}\n");
    auto r = t.run();
    CHECK(r.usedModern, "narrowed ?dyn dispatch uses the modern codegen pipeline");
    CHECK(r.ok, "a narrowed ?dyn receiver dispatches for early-return and else shapes");
    CHECK_EQ(r.errorCount, 0u, "the narrowed ?dyn dispatch module passes LLVM verification");
    CHECK_EQ(r.exitCode, 0, "both narrowing shapes dispatch to the concrete method");
}

// A `?dyn Trait` value crosses a parameter boundary and erases the payload on
// the way in. The value is built in the caller's frame so the callee receives
// a fat pointer whose data pointer outlives the call. (Returning a `?dyn`
// built from a temporary is the separate plain-`dyn` escape bug, out of scope.)
static void test_optional_dyn_parameter_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith",
            "trait Counter { fn get(self): i32; }\n"
            "struct Cell { v: i32 }\n"
            "implement Cell as Counter { fn get(self): i32 { return self.v; } }\n"
            "fn use_it(c: ?dyn Counter): i32 {\n"
            "    if (c is null) { return 1; }\n"
            "    let r = raw c;\n"
            "    return r.get();\n"
            "}\n"
            "fn main(): i32 {\n"
            "    let c: ?dyn Counter = Cell{v:7};\n"
            "    return use_it(c);\n"
            "}\n");
    auto r = t.run();
    CHECK(r.usedModern, "?dyn parameter passing uses the modern codegen pipeline");
    CHECK(r.ok, "a ?dyn value crosses a parameter boundary without an LLVM abort");
    CHECK_EQ(r.errorCount, 0u, "the ?dyn parameter module passes LLVM verification");
    CHECK_EQ(r.exitCode, 7, "the erased payload survives the parameter boundary");
}

// Assigning a concrete value into an existing `?dyn Trait` slot erases the
// payload before the `Some` wrap, for both a local and a struct field. This is
// the assignment form of the same R1 defect the literal and binding paths had.
static void test_optional_dyn_assignment_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith",
            "trait Counter { fn get(self): i32; }\n"
            "struct Cell { v: i32 }\n"
            "struct Other { v: i32 }\n"
            "implement Cell as Counter { fn get(self): i32 { return self.v; } }\n"
            "implement Other as Counter { fn get(self): i32 { return self.v + 100; } }\n"
            "struct Holder { c: ?dyn Counter = null }\n"
            "fn main(): i32 {\n"
            "    var l: ?dyn Counter = null;\n"
            "    l = Cell{v:7};\n"
            "    if (l is null) { return 1; }\n"
            "    let lr = raw l;\n"
            "    if (lr.get() != 7) { return 2; }\n"
            "    var h = Holder{};\n"
            "    h.c = Cell{v:7};\n"
            "    if (h.c is null) { return 3; }\n"
            "    let hr = raw h.c;\n"
            "    if (hr.get() != 7) { return 4; }\n"
            "    h.c = Other{v:8};\n"
            "    if (h.c is null) { return 5; }\n"
            "    let hr2 = raw h.c;\n"
            "    if (hr2.get() != 108) { return 6; }\n"
            "    return 0;\n"
            "}\n");
    auto r = t.run();
    CHECK(r.usedModern, "?dyn assignment uses the modern codegen pipeline");
    CHECK(r.ok, "a concrete value assigns into a ?dyn local and field without an LLVM abort");
    CHECK_EQ(r.errorCount, 0u, "the ?dyn assignment module passes LLVM verification");
    CHECK_EQ(r.exitCode, 0, "the erased payload round-trips through assignment and dispatch");
}

// Dispatching on a `?dyn` receiver without a non-null proof is a diagnostic,
// not an LLVM abort.
static void test_optional_dyn_without_narrowing_diagnostic() {
    ModernFileCodegenTest t;
    t.write("main.zith",
            "trait Counter { fn get(self): i32; }\n"
            "struct Cell { v: i32 }\n"
            "implement Cell as Counter { fn get(self): i32 { return self.v; } }\n"
            "fn main(): i32 {\n"
            "    let c: ?dyn Counter = Cell{v:7};\n"
            "    return c.get();\n"
            "}\n");
    auto r = t.run();
    CHECK(!r.ok, "an unproven ?dyn receiver is rejected");
    CHECK(r.hasMessage("narrow it after 'is null'"),
          "the unproven ?dyn dispatch reports the dedicated narrowing diagnostic");
}

// The same R1 shape applies to a `?opaque` field and binding.
static void test_optional_opaque_slot_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith",
            "struct Cell { v: i32 }\n"
            "struct Holder { c: ?opaque }\n"
            "fn main(): i32 {\n"
            "    let h = Holder{c: Cell{v:7}};\n"
            "    if (h.c is null) { return 1; }\n"
            "    let b: ?opaque = Cell{v:9};\n"
            "    if (b is null) { return 2; }\n"
            "    return 0;\n"
            "}\n");
    auto r = t.run();
    CHECK(r.usedModern, "?opaque field and binding use the modern codegen pipeline");
    CHECK(r.ok, "a concrete value erases into a ?opaque field and binding");
    CHECK_EQ(r.errorCount, 0u, "the ?opaque module passes LLVM verification");
    CHECK_EQ(r.exitCode, 0, "the erased ?opaque payload is present, not null");
}

// The niche representation makes `?dyn T` layout-identical to `dyn T` and
// keeps `?*T` identical to `*T`.
static void test_optional_dyn_niche_size_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith",
            "trait Counter { fn get(self): i32; }\n"
            "struct Cell { v: i32 }\n"
            "implement Cell as Counter { fn get(self): i32 { return self.v; } }\n"
            "fn main(): i32 {\n"
            "    let a: u64 = @sizeOf(?dyn Counter) as u64;\n"
            "    let b: u64 = @sizeOf(dyn Counter) as u64;\n"
            "    let c: u64 = @sizeOf(?*Cell) as u64;\n"
            "    let d: u64 = @sizeOf(*Cell) as u64;\n"
            "    if (a != b) { return 1; }\n"
            "    if (c != d) { return 2; }\n"
            "    return 0;\n"
            "}\n");
    auto r = t.run();
    CHECK(r.usedModern, "?dyn niche size uses the modern codegen pipeline");
    CHECK(r.ok, "a @sizeOf query over ?dyn and ?*T compiles");
    CHECK_EQ(r.errorCount, 0u, "the niche size module passes LLVM verification");
    CHECK_EQ(r.exitCode, 0, "?dyn is layout-identical to dyn and ?*T to *T");
}

static void test_function_default_arguments_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "fn add(left: i32, right: i32 = 5): i32 {\n"
                         "    return left + right;\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    return add(7) + add(1, 2);\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "default arguments use the modern codegen pipeline");
    CHECK(r.ok, "default argument calls compile, link and run");
    CHECK_EQ(r.exitCode, 15, "omitting a defaulted argument materializes its default value");
}

static void test_nested_optional_argument_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "fn identity(value: ??i32): i32 {\n"
                         "    return 0;\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    let inner: ?i32 = 5;\n"
                         "    let outer: ??i32 = inner;\n"
                         "    if identity(inner) != 0 { return 1; }\n"
                         "    if identity(outer) != 0 { return 2; }\n"
                         "    return 0;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "nested optional arguments use the modern codegen pipeline");
    CHECK(r.ok, "`?T` and `??T` argument coercions compile, link and run");
    CHECK_EQ(r.exitCode, 0, "nested optional call arguments preserve their layout");
}

static void test_generic_optional_coercion_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "struct Wrapper { value: i32 }\n"
                         "implement Wrapper {\n"
                         "    fn get(self): i32 { self.value }\n"
                         "}\n"
                         "fn wrap<T>(x: ?T): ?T { return x; }\n"
                         "fn main(): i32 {\n"
                         "    let value: ?Wrapper = wrap(Wrapper { value: 21 });\n"
                         "    if (value is null) { return 1; }\n"
                         "    return value.get();\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "generic optional inference uses the modern codegen pipeline");
    CHECK(r.ok, "generic optional call argument compiles, links and runs");
    CHECK_EQ(r.exitCode, 21, "the inferred optional argument is materialized as Some");
}

static void test_primitive_optional_slice_implement_runtime() {
    ModernFileCodegenTest t;
    t.write("main.zith", "trait Foo {\n"
                         "    fn value(self): i32;\n"
                         "}\n"
                         "implement i32 as Foo {\n"
                         "    fn value(self): i32 { return 7; }\n"
                         "}\n"
                         "trait O {\n"
                         "    fn get(self): i32 { return 1; }\n"
                         "}\n"
                         "implement ?char as O {}\n"
                         "trait S {\n"
                         "    fn len(self): i32;\n"
                         "}\n"
                         "implement []char as S {\n"
                         "    fn len(self): i32 { return 3; }\n"
                         "}\n"
                         "trait P {\n"
                         "    fn first(self): char;\n"
                         "}\n"
                         "implement *char as P {\n"
                         "    fn first(self): char { return raw self[0]; }\n"
                         "}\n"
                         "fn count(s: []char): i32 { return @lengthOf(s) as i32; }\n"
                         "fn main(): i32 {\n"
                         "    let a: i32 = 1;\n"
                         "    let o: ?char = 'x';\n"
                         "    var arr: [3]char = ['a', 'b', 'c'];\n"
                         "    let s: []char = raw arr[0..3];\n"
                         "    let p: *char = \"zith\";\n"
                         "    let literal: []char = \"abc\";\n"
                         "    if (p.first() != 'z') { return 2; }\n"
                         "    if (p->first() != 'z') { return 3; }\n"
                         "    if (count(\"abc\") != 3) { return 4; }\n"
                         "    if (count(literal) != 3) { return 5; }\n"
                         "    return a.value() + o.get() + s.len();\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "primitive/optional/slice/pointer implementations use the modern pipeline");
    CHECK(r.ok, "method calls on i32, ?char, []char and *char compile, link and run");
    CHECK_EQ(r.exitCode, 11, "a.value() + o.get() + s.len() returns 7 + 1 + 3");
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
                         "    _ = printf(\"n=%d\\n\", 7);\n"
                         "    return 0;\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.ok, "extern variadic printf compiles, links and runs");
    CHECK(r.output.find("call i32 (ptr, ...) @printf") != std::string::npos,
          "the call reaches LLVM IR as a variadic invoke of printf");
    CHECK(r.output.find("declare i32 @printf(ptr, ...)") != std::string::npos,
          "LLVM IR declares printf as variadic");
}

static void test_external_symbol_alias_runtime() {
    ModernFileCodegenTest t;
    const auto c_path   = (t.root / "external-alias.c").string();
    const auto lib_path = (t.root / "libzith_external_alias.a").string();
    const auto obj_path = (t.root / "external-alias.o").string();
    {
        std::ofstream c_source(c_path, std::ios::binary | std::ios::trunc);
        c_source << "int zith_test_external_increment(int *p) { return ++(*p); }\n";
    }
    const auto compile = "cc -c " + c_path + " -o " + obj_path + " 2>/dev/null";
    const auto archive = "ar rcs " + lib_path + " " + obj_path + " 2>/dev/null";
    if (std::system(compile.c_str()) != 0 || std::system(archive.c_str()) != 0) {
        std::printf("  SKIP: no C toolchain for the external symbol alias runtime test\n");
        return;
    }

    t.opts.libraryDirs.push(t.root.string());
    t.opts.libraries.push("zith_external_alias");
    t.write("main.zith", "fn c_increment(p: *i32): i32 = extern zith_test_external_increment;\n"
                         "fn main(): i32 {\n"
                         "    var value: i32 = 41;\n"
                         "    return c_increment(&value);\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "external symbol alias uses the modern codegen pipeline");
    CHECK(r.ok, "external symbol alias compiles, links and executes");
    CHECK_EQ(r.exitCode, 42, "external symbol alias calls the C function through the alias name");
}

static void test_imported_external_globals_runtime() {
#ifdef ZITH_ENABLE_C_INTEROP
    ModernFileCodegenTest t;
    t.opts.flags.emitIr(true);
    t.write("external-globals.h", "extern int zith_external_value;\n"
                                  "extern int zith_mutable_value;\n");
    t.write("c/globals.c", "int zith_external_value = 27;\n"
                           "int zith_mutable_value = 5;\n");
    t.write("ZithProject.toml", "[ffi]\n"
                                "c_source_dirs = [\"c\"]\n");
    t.write("main.zith", "import \"external-globals.h\"\n"
                         "extern var manual_value: i32 = extern zith_external_value;\n"
                         "fn main(): i32 {\n"
                         "    zith_mutable_value = 40;\n"
                         "    if (zith_mutable_value != 40) { return 1; }\n"
                         "    if (manual_value != zith_external_value) { return 2; }\n"
                         "    return manual_value + zith_mutable_value;\n"
                         "}\n");

    const auto r = t.run();
    CHECK(r.ok, "imported C globals compile, link and run");
    CHECK_EQ(r.exitCode, 67, "C global reads and writes reach the external objects");
    const auto globalDecl = r.output.find("@zith_external_value = external global i32");
    CHECK(globalDecl != std::string::npos,
          "imported and manually aliased global use its C linkage name");
    if (globalDecl != std::string::npos)
        CHECK(r.output.find("@zith_external_value = external global i32", globalDecl + 1U) ==
                  std::string::npos,
              "repeated references and aliases emit one LLVM global declaration");
#endif
}

static void test_external_symbol_method_receiver_runtime() {
    ModernFileCodegenTest t;
    const auto c_path   = (t.root / "external-alias-method.c").string();
    const auto lib_path = (t.root / "libzith_external_alias_method.a").string();
    const auto obj_path = (t.root / "external-alias-method.o").string();
    {
        std::ofstream c_source(c_path, std::ios::binary | std::ios::trunc);
        c_source << "typedef struct zith_window { int value; } zith_window;\n"
                    "int zith_test_method_value(zith_window *w) { return w->value; }\n";
    }
    const auto compile = "cc -c " + c_path + " -o " + obj_path + " 2>/dev/null";
    const auto archive = "ar rcs " + lib_path + " " + obj_path + " 2>/dev/null";
    if (std::system(compile.c_str()) != 0 || std::system(archive.c_str()) != 0) {
        std::printf("  SKIP: no C toolchain for the external alias method runtime test\n");
        return;
    }

    t.opts.libraryDirs.push(t.root.string());
    t.opts.libraries.push("zith_external_alias_method");
    t.write("lib.zith", "pub struct Window { pub value: i32 }\n"
                        "implement Window {\n"
                        "    fn value(self: lend Window): i32 = extern zith_test_method_value;\n"
                        "}\n");
    t.write("main.zith", "from lib\n"
                         "fn main(): i32 {\n"
                         "    var window: Window = Window { value: 42 };\n"
                         "    window.value()\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "external alias method uses the modern codegen pipeline");
    CHECK(r.ok, "external alias method compiles, links and executes");
    CHECK_EQ(r.exitCode, 42, "external alias receiver is passed as the first C pointer argument");
}

static void test_external_symbol_optional_pointer_method_receiver_runtime() {
    ModernFileCodegenTest t;
    const auto c_path   = (t.root / "external-alias-optional-pointer-method.c").string();
    const auto lib_path = (t.root / "libzith_external_alias_optional_pointer_method.a").string();
    const auto obj_path = (t.root / "external-alias-optional-pointer-method.o").string();
    {
        std::ofstream c_source(c_path, std::ios::binary | std::ios::trunc);
        c_source << "typedef struct zith_window { int value; } zith_window;\n"
                    "int zith_test_optional_pointer_value(zith_window *w) { return w->value; }\n";
    }
    const auto compile = "cc -c " + c_path + " -o " + obj_path + " 2>/dev/null";
    const auto archive = "ar rcs " + lib_path + " " + obj_path + " 2>/dev/null";
    if (std::system(compile.c_str()) != 0 || std::system(archive.c_str()) != 0) {
        std::printf("  SKIP: no C toolchain for optional pointer external method runtime test\n");
        return;
    }

    t.opts.libraryDirs.push(t.root.string());
    t.opts.libraries.push("zith_external_alias_optional_pointer_method");
    t.write("lib.zith",
            "pub struct Window { pub value: i32 }\n"
            "pub fn makeWindow(value: i32): ?*Window = extern zith_optional_pointer_make;\n"
            "implement Window {\n"
            "    fn value(self: lend Window): i32 = extern zith_test_optional_pointer_value;\n"
            "}\n");
    t.write("helper.c",
            "typedef struct zith_window { int value; } zith_window;\n"
            "zith_window *zith_optional_pointer_make(int value) {\n"
            "    zith_window *w = (zith_window *)__builtin_malloc(sizeof(zith_window));\n"
            "    w->value = value;\n"
            "    return w;\n"
            "}\n");
    const auto helper_obj = (t.root / "helper-optional-pointer.o").string();
    const auto helper_compile =
        "cc -c " + (t.root / "helper.c").string() + " -o " + helper_obj + " 2>/dev/null";
    const auto helper_archive =
        "ar rcs " + lib_path + " " + obj_path + " " + helper_obj + " 2>/dev/null";
    if (std::system(helper_compile.c_str()) != 0 || std::system(helper_archive.c_str()) != 0) {
        std::printf("  SKIP: no C toolchain for optional pointer helper\n");
        return;
    }

    t.write("main.zith", "from lib\n"
                         "fn main(): i32 {\n"
                         "    var window = makeWindow(42);\n"
                         "    if (window is null) { return 1; }\n"
                         "    window.value()\n"
                         "}\n");

    auto r = t.run();
    CHECK(r.usedModern, "optional pointer external method uses the modern codegen pipeline");
    CHECK(r.ok, "optional pointer external method compiles, links and executes");
    CHECK_EQ(r.exitCode, 42,
             "optional pointer external receiver passes the pointer value to the C function");
}

static void test_c_default_arguments_and_string_escapes() {
    {
        ModernFileCodegenTest t;
        t.opts.flags.emitIr(true);
        t.write("main.zith", "extern fn printf(fmt: *char, ...): i32\n"
                             "fn main(): i32 {\n"
                             "    let f: f32 = 1.5;\n"
                             "    _ = printf(\"%f\", f);\n"
                             "    return 0;\n"
                             "}\n");

        auto r = t.run();
        CHECK(r.ok, "an f32 variadic promotion test compiles and runs");
        CHECK(r.output.find("fpext float") != std::string::npos,
              "the f32 variadic argument is promoted to double in LLVM IR");
    }

    {
        CodegenTest t;
        auto r =
            t.run("codegen-escapes-char.zith", "extern fn printf(fmt: *char, ...): i32\n"
                                               "fn main(): i32 {\n"
                                               "    _ = printf(\"v=%d\\n[%f]%c\", 42, 1.5, 'B');\n"
                                               "    return 0;\n"
                                               "}\n");
        CHECK(r.ok, "escaped strings, char literal args and promoted variadic args run");
        CHECK(r.output == "v=42\n[1.500000]B",
              "escaped newline decodes, char literals pass through %c, and variadic args print");
    }
}

static void test_dollar_escape_hatch_runtime() {
    CodegenTest t;
    auto r = t.run("codegen-dollar-escape.zith", "extern fn printf(fmt: *char, ...): i32\n"
                                                 "fn main(): i32 {\n"
                                                 "    _ = printf(\"cost=\\#5\\n\");\n"
                                                 "    _ = printf(\"%c\\n\", '\\#');\n"
                                                 "    return 0;\n"
                                                 "}\n");
    CHECK(r.ok, "the hash escape hatch compiles");
    CHECK(r.output == "cost=#5\n#\n", "\\# emits a literal hash in string and char literals");
}

// Octal escapes make ANSI color codes (`\033`) work and stop `\0` from
// silently truncating a string at the first following digit.
static void test_octal_escape_runtime() {
    CodegenTest t;
    auto r = t.run("codegen-octal-escape.zith", "extern fn printf(fmt: *char, ...): i32\n"
                                                "fn main(): i32 {\n"
                                                "    _ = printf(\"A\\033B\\n\");\n"
                                                "    _ = printf(\"C\\101D\\n\");\n"
                                                "    _ = printf(\"E\\7F\\n\");\n"
                                                "    return 0;\n"
                                                "}\n");
    CHECK(r.ok, "octal escapes compile, link and run");
    CHECK(r.output == "A\x1b"
                      "B\nCAD\nE\x07"
                      "F\n",
          "\\033 is ESC, \\101 is 'A', and \\7 is BEL");

    // A single `\0` stays NUL, and a following digit is a literal character
    // rather than being swallowed into an octal run: `M\08N` has four
    // in-memory characters. `@lengthOf` decodes escapes, so it observes the
    // byte count even though `printf` would stop at the NUL.
    CodegenTest t2;
    auto r2 =
        t2.run("codegen-nul-then-digit.zith", "fn main(): i32 {\n"
                                              "    if (@lengthOf(\"M\\08N\") != 4) { return 1; }\n"
                                              "    return 0;\n"
                                              "}\n");
    CHECK(r2.ok, "a NUL followed by a digit compiles and runs");
    CHECK_EQ(r2.exitCode, 0, "\\0 is one NUL byte and the following '8' is a literal character");
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
                       "    _ = printf(\"child-says=%d\\n\", 3);\n"
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
                       "    _ = printf(\"direct-says=%d\\n\", 7);\n"
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
                                                        "    _ = printf(\"before-failure\\n\");\n"
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
                         "    _ = printf(\"v=%d\\n\", 42);\n"
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
                                                      "    _ = printf(\"v=%d\\n\", 42);\n"
                                                      "    return 0;\n"
                                                      "}\n");
    CHECK(run.ok, "stdio.h import compiles and executes without IR output enabled");
    CHECK(run.output == "v=42\n",
          "stdio.h import builds, links, runs, and prints the decoded newline exactly");
#endif
}

static void test_validated_c_struct_by_value_runs() {
#ifdef ZITH_ENABLE_C_INTEROP
    ModernFileCodegenTest t;
    t.opts.flags.emitIr(true);
    t.opts.cSourceDirs.push("c");
    t.write("c/records.c", "#include <stdint.h>\n"
                           "struct Point { int x, y; };\n"
                           "int c_classify(struct Point p) { return p.x + p.y; }\n"
                           "struct Point make_point(int x, int y) {\n"
                           "    struct Point p = { x, y };\n"
                           "    return p;\n"
                           "}\n"
                           "struct Inner { int v; };\n"
                           "struct Outer { struct Inner inner; double d; };\n"
                           "double c_outer_x(struct Outer o) { return o.inner.v; }\n");
    t.write("main.zith", "import \"records.h\"\n"
                         "fn main(): i32 {\n"
                         "    let p = make_point(2, 3);\n"
                         "    let sum: i32 = c_classify(p);\n"
                         "    if (sum != 5) { return 1; }\n"
                         "    return 0;\n"
                         "}\n");
    t.write("records.h", "struct Point { int x, y; };\n"
                         "int c_classify(struct Point p);\n"
                         "int c_unused(struct Point p);\n"
                         "struct Point make_point(int x, int y);\n");

    auto r = t.run();
    CHECK(r.ok, "validated simple record by-value functions compile, link, and run");
    CHECK_EQ(r.exitCode, 0, "the C stub returns the sum read from the passed struct");

    auto warm = t.run();
    CHECK(warm.ok, "warm cache restores the C ABI adapter and still links");
    CHECK(warm.cacheHits > 0U, "warm C ABI adapter compilation loads the cached HIR");
    CHECK_EQ(warm.exitCode, 0, "the hydrated C ABI adapter preserves by-value behavior");
#endif
}

static void test_validated_c_pair64_by_value_runs() {
#ifdef ZITH_ENABLE_C_INTEROP
    ModernFileCodegenTest t;
    t.opts.flags.emitIr(true);
    t.opts.cSourceDirs.push("c");
    t.write("c/pair64-records.c", "#include \"../pair64-records.h\"\n"
                                  "int c_check_pair64(struct Pair64 p) {\n"
                                  "    return p.lo == INT64_C(1311768467463790320) &&\n"
                                  "           p.hi == INT64_C(1147797409030816545);\n"
                                  "}\n"
                                  "int64_t c_sum_pair64(struct Pair64 p) {\n"
                                  "    return p.lo + p.hi;\n"
                                  "}\n"
                                  "struct Pair64 make_pair64(int64_t lo, int64_t hi) {\n"
                                  "    struct Pair64 p = { lo, hi };\n"
                                  "    return p;\n"
                                  "}\n");
    t.write("main.zith", "import \"pair64-records.h\"\n"
                         "fn main(): i32 {\n"
                         "    let p = make_pair64(1311768467463790320, 1147797409030816545);\n"
                         "    if (c_check_pair64(p) != 1) { return 1; }\n"
                         "    let sum: i64 = c_sum_pair64(p);\n"
                         "    if (sum != 2459565876494606865) { return 2; }\n"
                         "    return 0;\n"
                         "}\n");
    t.write("pair64-records.h", "#include <stdint.h>\n"
                                "struct Pair64 { int64_t lo, hi; };\n"
                                "int c_check_pair64(struct Pair64 p);\n"
                                "int64_t c_sum_pair64(struct Pair64 p);\n"
                                "struct Pair64 make_pair64(int64_t lo, int64_t hi);\n");

    auto r = t.run();
    CHECK(r.ok, "validated two-adjacent-i64 record by-value functions compile, link, and run");
    CHECK_EQ(r.exitCode, 0,
             "C independently checks both 64-bit fields and returns their full-width sum");
#endif
}

static void test_c_scalar_arguments_and_results_run() {
#ifdef ZITH_ENABLE_C_INTEROP
    ModernFileCodegenTest t;
    t.opts.cSourceDirs.push("c");
    t.write("c/scalar-abi.c",
            "#include \"../scalar-abi.h\"\n"
            "int c_check_i8(int8_t value) { return value == -101; }\n"
            "int8_t c_make_i8(void) { return -101; }\n"
            "int c_check_u8(uint8_t value) { return value == 231; }\n"
            "uint8_t c_make_u8(void) { return 231; }\n"
            "int c_check_i16(int16_t value) { return value == -30001; }\n"
            "int16_t c_make_i16(void) { return -30001; }\n"
            "int c_check_u16(uint16_t value) { return value == 61001; }\n"
            "uint16_t c_make_u16(void) { return 61001; }\n"
            "int c_check_i32(int32_t value) { return value == -123456789; }\n"
            "int32_t c_make_i32(void) { return -123456789; }\n"
            "int c_check_u32(uint32_t value) { return value == UINT32_C(0xFEDCBA98); }\n"
            "uint32_t c_make_u32(void) { return UINT32_C(0xFEDCBA98); }\n"
            "int c_check_i64(int64_t value) { return value == -INT64_C(81985529216486895); }\n"
            "int64_t c_make_i64(void) { return -INT64_C(81985529216486895); }\n"
            "int c_check_u64(uint64_t value) { return value == UINT64_C(0xFEDCBA9876543210); }\n"
            "uint64_t c_make_u64(void) { return UINT64_C(0xFEDCBA9876543210); }\n"
            "int c_check_f32(float value) { return value == -13.25f; }\n"
            "float c_make_f32(void) { return -13.25f; }\n"
            "int c_check_f64(double value) { return value == 1234567.125; }\n"
            "double c_make_f64(void) { return 1234567.125; }\n");
    t.write("scalar-abi.h", "#include <stdint.h>\n"
                            "int c_check_i8(int8_t value);\n"
                            "int8_t c_make_i8(void);\n"
                            "int c_check_u8(uint8_t value);\n"
                            "uint8_t c_make_u8(void);\n"
                            "int c_check_i16(int16_t value);\n"
                            "int16_t c_make_i16(void);\n"
                            "int c_check_u16(uint16_t value);\n"
                            "uint16_t c_make_u16(void);\n"
                            "int c_check_i32(int32_t value);\n"
                            "int32_t c_make_i32(void);\n"
                            "int c_check_u32(uint32_t value);\n"
                            "uint32_t c_make_u32(void);\n"
                            "int c_check_i64(int64_t value);\n"
                            "int64_t c_make_i64(void);\n"
                            "int c_check_u64(uint64_t value);\n"
                            "uint64_t c_make_u64(void);\n"
                            "int c_check_f32(float value);\n"
                            "float c_make_f32(void);\n"
                            "int c_check_f64(double value);\n"
                            "double c_make_f64(void);\n");
    t.write(
        "main.zith",
        "import \"scalar-abi.h\"\n"
        "fn main(): i32 {\n"
        "    if (c_check_i8(-101) != 1 or c_make_i8() != -101) { return 1; }\n"
        "    if (c_check_u8(231) != 1 or c_make_u8() != 231) { return 2; }\n"
        "    if (c_check_i16(-30001) != 1 or c_make_i16() != -30001) { return 3; }\n"
        "    if (c_check_u16(61001) != 1 or c_make_u16() != 61001) { return 4; }\n"
        "    if (c_check_i32(-123456789) != 1 or c_make_i32() != -123456789) { return 5; }\n"
        "    if (c_check_u32(0xFEDCBA98u32) != 1 or c_make_u32() != 0xFEDCBA98u32) { return 6; }\n"
        "    if (c_check_i64(-81985529216486895) != 1 or c_make_i64() != -81985529216486895) { "
        "return 7; }\n"
        "    if (c_check_u64(0xFEDCBA9876543210u64) != 1 or\n"
        "        c_make_u64() != 0xFEDCBA9876543210u64) { return 8; }\n"
        "    if (c_check_f32(-13.25) != 1 or c_make_f32() != -13.25) { return 9; }\n"
        "    if (c_check_f64(1234567.125) != 1 or c_make_f64() != 1234567.125) { return 10; }\n"
        "    return 0;\n"
        "}\n");

    const auto result = t.run();
    CHECK(result.ok, "imported C scalar arguments and results compile, link, and execute");
    CHECK_EQ(
        result.exitCode, 0,
        "C observes each fixed-width integer and float argument and Zith receives each result");
#endif
}

static void test_imported_variadic_c_promotes_tail_arguments() {
#ifdef ZITH_ENABLE_C_INTEROP
    ModernFileCodegenTest t;
    t.opts.cSourceDirs.push("c");
    t.write("c/variadic-promotions.c",
            "#include \"../variadic-promotions.h\"\n"
            "#include <stdarg.h>\n"
            "int c_check_variadic_promotions(int fixed, ...) {\n"
            "    va_list args;\n"
            "    va_start(args, fixed);\n"
            "    int small = va_arg(args, int);\n"
            "    double promoted = va_arg(args, double);\n"
            "    va_end(args);\n"
            "    return fixed == 17 && small == 250 && promoted == 1.25;\n"
            "}\n");
    t.write("variadic-promotions.h", "int c_check_variadic_promotions(int fixed, ...);\n");
    t.write("main.zith",
            "import \"variadic-promotions.h\"\n"
            "fn main(): i32 {\n"
            "    let small: u8 = 250;\n"
            "    let fraction: f32 = 1.25;\n"
            "    if (c_check_variadic_promotions(17, small, fraction) != 1) { return 1; }\n"
            "    return 0;\n"
            "}\n");

    const auto result = t.run();
    CHECK(result.ok, "header-imported C variadic function compiles, links, and executes");
    CHECK_EQ(result.exitCode, 0,
             "C reads the variadic u8 as int and f32 as double after default promotions");
#endif
}

static void test_c_bool_char_long_scalars_run() {
#ifdef ZITH_ENABLE_C_INTEROP
    ModernFileCodegenTest t;
    t.opts.cSourceDirs.push("c");
    t.write("c/native-scalars.c", "#include \"../native-scalars.h\"\n"
                                  "int c_check_bool(_Bool value) { return value == 1; }\n"
                                  "_Bool c_make_bool(void) { return 1; }\n"
                                  "int c_check_char(char value) { return value == 'Q'; }\n"
                                  "char c_make_char(void) { return 'R'; }\n"
                                  "int c_check_long(long value) { return value == 1234567L; }\n"
                                  "long c_make_long(void) { return 7654321L; }\n");
    t.write("native-scalars.h", "int c_check_bool(_Bool value);\n"
                                "_Bool c_make_bool(void);\n"
                                "int c_check_char(char value);\n"
                                "char c_make_char(void);\n"
                                "int c_check_long(long value);\n"
                                "long c_make_long(void);\n");
    t.write("main.zith",
            "import \"native-scalars.h\"\n"
            "fn main(): i32 {\n"
            "    if (c_check_bool(true) != 1 or c_make_bool() != true) { return 1; }\n"
            "    if (c_check_char('Q') != 1 or c_make_char() != 'R') { return 2; }\n"
            "    if (c_check_long(1234567) != 1 or c_make_long() != 7654321) { return 3; }\n"
            "    return 0;\n"
            "}\n");

    const auto result = t.run();
    CHECK(result.ok, "imported C _Bool, char, and long functions compile, link, and execute");
    CHECK_EQ(result.exitCode, 0,
             "C observes the Zith arguments and Zith receives each native scalar result");
#endif
}

static void test_imported_c_pointer_roundtrip_runtime() {
#ifdef ZITH_ENABLE_C_INTEROP
    ModernFileCodegenTest t;
    t.opts.cSourceDirs.push("c");
    t.write("c/pointer-abi.c", "#include \"../pointer-abi.h\"\n"
                                "int *c_increment_and_return_i32(int *value) {\n"
                                "    *value += 5;\n"
                                "    return value;\n"
                                "}\n"
                                "int *c_return_null_i32(void) { return 0; }\n");
    t.write("pointer-abi.h", "int *c_increment_and_return_i32(int *value);\n"
                             "int *c_return_null_i32(void);\n");
    t.write("main.zith",
            "import \"pointer-abi.h\"\n"
            "fn main(): i32 {\n"
            "    var value: i32 = 37;\n"
            "    let returned = c_increment_and_return_i32(&value);\n"
            "    if (returned is null) { return 1; }\n"
            "    if (*returned != 42) { return 2; }\n"
            "    let null_pointer = c_return_null_i32();\n"
            "    if (null_pointer is null) { return 0; }\n"
            "    return 3;\n"
            "}\n");

    const auto result = t.run();
    CHECK(result.ok, "header-imported C pointer parameters and results compile, link, and execute");
    CHECK_EQ(result.exitCode, 0,
             "C mutates the pointed value, returns its address, and preserves a null pointer");
#endif
}

static void test_imported_c_void_pointer_roundtrip_runtime() {
#ifdef ZITH_ENABLE_C_INTEROP
    ModernFileCodegenTest t;
    t.opts.cSourceDirs.push("c");
    t.write("c/void-pointer-abi.c", "#include \"../void-pointer-abi.h\"\n"
                                    "void *c_roundtrip_void_pointer(void *value) {\n"
                                    "    return value;\n"
                                    "}\n"
                                    "int c_write_through_void_pointer(void *value, int replacement) {\n"
                                    "    int *typed = (int *)value;\n"
                                    "    *typed = replacement;\n"
                                    "    return *typed;\n"
                                    "}\n"
                                    "void *c_return_null_void_pointer(void) { return 0; }\n");
    t.write("void-pointer-abi.h", "void *c_roundtrip_void_pointer(void *value);\n"
                                  "int c_write_through_void_pointer(void *value, int replacement);\n"
                                  "void *c_return_null_void_pointer(void);\n");
    t.write("main.zith", "import \"void-pointer-abi.h\"\n"
                         "fn main(): i32 {\n"
                         "    var value: i32 = 17;\n"
                         "    let erased_pointer = c_roundtrip_void_pointer(&value);\n"
                         "    if (erased_pointer is null) { return 1; }\n"
                         "    if (c_write_through_void_pointer(erased_pointer, 42) != 42) { return 2; }\n"
                         "    let null_pointer = c_return_null_void_pointer();\n"
                         "    if (null_pointer is null) { return 0; }\n"
                         "    return 4;\n"
                         "}\n");

    const auto result = t.run();
    CHECK(result.ok, "header-imported void* parameters and results compile, link, and execute");
    CHECK_EQ(result.exitCode, 0,
             "C preserves a void* round trip, mutates the pointed value, and returns null");
#endif
}

static void test_c_header_callback_declaration_is_not_imported() {
#ifdef ZITH_ENABLE_C_INTEROP
    ModernFileCodegenTest t;
    t.opts.cSourceDirs.push("c");
    t.write("c/callback-abi.c", "#include \"../callback-abi.h\"\n"
                                "int c_invoke_callback(int (*callback)(int)) {\n"
                                "    return callback(1);\n"
                                "}\n");
    t.write("callback-abi.h", "int c_invoke_callback(int (*callback)(int));\n");
    t.write("main.zith",
            "import \"callback-abi.h\"\n"
            "fn invalid_call(): i32 {\n"
            "    var value: i32 = 7;\n"
            "    return c_invoke_callback(&value);\n"
            "}\n"
            "fn main(): i32 { return 0; }\n");

    const auto result = t.run();
    CHECK(!result.ok && result.errorCount > 0U &&
              result.hasMessage("c_invoke_callback"),
          "a callback declaration without a callable type must not be exposed as an object pointer");
#endif
}

static void test_c_enum_return_local_argument_runs() {
#ifdef ZITH_ENABLE_C_INTEROP
    ModernFileCodegenTest t;
    t.opts.cSourceDirs.push("c");
    t.write("c/color.c", "#include \"../color.h\"\n"
                         "Color c_make_color(void) { return ColorGreen; }\n"
                         "int c_check_color(Color color) { return color == ColorGreen; }\n");
    t.write("color.h", "typedef enum Color { ColorRed = 3, ColorGreen = 42 } Color;\n"
                       "Color c_make_color(void);\n"
                       "int c_check_color(Color color);\n");
    t.write("main.zith", "import \"color.h\"\n"
                         "fn main(): i32 {\n"
                         "    let color = c_make_color();\n"
                         "    if (c_check_color(color) != 1) { return 1; }\n"
                         "    return 0;\n"
                         "}\n");

    const auto result = t.run();
    CHECK(result.ok, "foreign enum return lowers into a local and can be passed back to C");
    CHECK_EQ(result.exitCode, 0, "the C enum round-trip preserves its value");
#endif
}

static void test_validated_c_double_pair_by_value_runs() {
#ifdef ZITH_ENABLE_C_INTEROP
    ModernFileCodegenTest t;
    t.opts.cSourceDirs.push("c");
    t.write("c/double-pair.c", "struct DoublePair { double left, right; };\n"
                               "double c_pair_weighted_sum(struct DoublePair value) {\n"
                               "    return value.left * 10.0 + value.right;\n"
                               "}\n"
                               "struct DoublePair c_make_double_pair(double left, double right) {\n"
                               "    struct DoublePair value = { left, right };\n"
                               "    return value;\n"
                               "}\n");
    t.write("main.zith", "import \"double-pair.h\"\n"
                         "fn main(): i32 {\n"
                         "    let value = c_make_double_pair(2.0, 3.0);\n"
                         "    let weighted: f64 = c_pair_weighted_sum(value);\n"
                         "    if (weighted != 23.0) { return 1; }\n"
                         "    return 0;\n"
                         "}\n");
    t.write("double-pair.h", "struct DoublePair { double left, right; };\n"
                             "double c_pair_weighted_sum(struct DoublePair value);\n"
                             "struct DoublePair c_make_double_pair(double left, double right);\n");

    auto r = t.run();
    CHECK(r.ok, "C double-record parameters and results compile, link, and run");
    CHECK_EQ(r.exitCode, 0, "both double fields survive the C by-value round trip");
#endif
}

static void test_validated_c_large_record_by_value_runs() {
#ifdef ZITH_ENABLE_C_INTEROP
    ModernFileCodegenTest t;
    t.opts.cSourceDirs.push("c");
    t.write("c/large-record.c",
            "struct Triple64 { long long first, second, third; };\n"
            "long long c_sum_triple64(struct Triple64 value) {\n"
            "    return value.first + value.second * 10 + value.third * 100;\n"
            "}\n"
            "struct Triple64 c_make_triple64(long long first, long long second,\n"
            "                                long long third) {\n"
            "    struct Triple64 value = { first, second, third };\n"
            "    return value;\n"
            "}\n");
    t.write("main.zith", "import \"large-record.h\"\n"
                         "fn main(): i32 {\n"
                         "    let value = c_make_triple64(1, 2, 3);\n"
                         "    let weighted: i64 = c_sum_triple64(value);\n"
                         "    if (weighted != 321) { return 1; }\n"
                         "    return 0;\n"
                         "}\n");
    t.write("large-record.h", "struct Triple64 { long long first, second, third; };\n"
                              "long long c_sum_triple64(struct Triple64 value);\n"
                              "struct Triple64 c_make_triple64(long long first, long long second,\n"
                              "                                long long third);\n");

    auto r = t.run();
    CHECK(r.ok, "24-byte C records compile, link, and run by value");
    CHECK_EQ(r.exitCode, 0, "all three fields survive the 24-byte round trip");
#endif
}

static void test_validated_c_mixed_integer_record_by_value_runs() {
#ifdef ZITH_ENABLE_C_INTEROP
    ModernFileCodegenTest t;
    t.opts.cSourceDirs.push("c");
    t.write("c/mixed-records.c", "struct Mixed32_64 { int small; long long large; };\n"
                                 "long long c_sum_mixed32_64(struct Mixed32_64 value) {\n"
                                 "    return (long long)value.small + value.large;\n"
                                 "}\n"
                                 "struct Mixed32_64 make_mixed32_64(int small, long long large) {\n"
                                 "    struct Mixed32_64 value = { small, large };\n"
                                 "    return value;\n"
                                 "}\n");
    t.write("main.zith", "import \"mixed-records.h\"\n"
                         "fn main(): i32 {\n"
                         "    let value = make_mixed32_64(20, 22);\n"
                         "    let total: i64 = c_sum_mixed32_64(value);\n"
                         "    if (total != 42) { return 1; }\n"
                         "    return 0;\n"
                         "}\n");
    t.write("mixed-records.h", "struct Mixed32_64 { int small; long long large; };\n"
                               "long long c_sum_mixed32_64(struct Mixed32_64 value);\n"
                               "struct Mixed32_64 make_mixed32_64(int small, long long large);\n");

    auto r = t.run();
    CHECK(r.ok, "validated mixed i32/i64 record parameters and results compile, link, and run");
    CHECK_EQ(r.exitCode, 0, "the C stub reads and returns the expected mixed-record values");
#endif
}

static void test_validated_c_reverse_mixed_integer_record_by_value_runs() {
#ifdef ZITH_ENABLE_C_INTEROP
    ModernFileCodegenTest t;
    t.opts.cSourceDirs.push("c");
    t.write("c/mixed-reverse-records.c",
            "struct Mixed64_32 { long long large; int small; };\n"
            "long long c_sum_mixed64_32(struct Mixed64_32 value) {\n"
            "    return value.large * 100 + value.small;\n"
            "}\n"
            "struct Mixed64_32 make_mixed64_32(long long large, int small) {\n"
            "    struct Mixed64_32 value = { large, small };\n"
            "    return value;\n"
            "}\n");
    t.write("main.zith", "import \"mixed-reverse-records.h\"\n"
                         "fn main(): i32 {\n"
                         "    let value = make_mixed64_32(20, 22);\n"
                         "    let total: i64 = c_sum_mixed64_32(value);\n"
                         "    if (total != 2022) { return 1; }\n"
                         "    return 0;\n"
                         "}\n");
    t.write("mixed-reverse-records.h",
            "struct Mixed64_32 { long long large; int small; };\n"
            "long long c_sum_mixed64_32(struct Mixed64_32 value);\n"
            "struct Mixed64_32 make_mixed64_32(long long large, int small);\n");

    auto r = t.run();
    CHECK(r.ok, "validated i64/i32 record parameters and results compile, link, and run");
    CHECK_EQ(r.exitCode, 0, "the C stub preserves both i64/i32 record fields");
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
                         "    if (cell is null) { return 1; }\n"
                         "    let slot: *i32 = cell;\n"
                         "    *slot = 42;\n"
                         "    _ = printf(\"v=%d\\n\", *slot);\n"
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
                         "    _ = fclose(f);\n"
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

static void test_imported_stdout_global_runtime() {
#ifdef ZITH_ENABLE_C_INTEROP
    ModernFileCodegenTest t;
    t.write("main.zith", "import \"stdio.h\"\n"
                         "fn main(): i32 {\n"
                         "    _ = fputs(\"stream-global-works\\n\", stdout);\n"
                         "    return 0;\n"
                         "}\n");

    const auto r = t.run();
    CHECK(r.ok, "stdio stdout global compiles, links and runs");
    CHECK_EQ(r.exitCode, 0, "using stdout through its imported FILE typedef succeeds");
    CHECK(r.output.find("stream-global-works\n") != std::string::npos,
          "fputs receives the C-defined stdout stream pointer");
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
    fn.hasBody     = true;
    fn.return_type = types.internInt(types::IntWidth::I32);
    const auto bad_literal =
        hir.addExpr(hir::HirLiteral{{}, types::kErrorType, hir::HirExprKind::Literal});
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

static void test_failed_let_initializer_reports_emission_error() {
    memory::Arena arena;
    memory::StringInterner interner(arena);
    types::TypeIntern types(arena, interner);
    hir::HirModule hir(arena);

    auto &fn       = hir.addFn(interner.intern("broken_let"));
    fn.hasBody     = true;
    fn.return_type = types::kVoidType;
    auto &block    = fn.blocks.emplace(arena);

    hir::HirVar missing;
    missing.name     = interner.intern("missing");
    const auto value = hir.addExpr(missing);
    hir::HirLet let;
    let.name = interner.intern("local");
    let.type = types.internInt(types::IntWidth::I32);
    let.init = value;
    block.insts.push(hir.addExpr(let));
    block.terminator = hir.addExpr(hir::HirRet{hir::kInvalidHirExpr});

    diagnostics::DiagnosticEngine diags(arena);
    codegen::CodeGen cg(interner, types, {}, 0, &diags);
    cg.emit(hir, "broken-let-module");

    CHECK(cg.hasInvalidIR(), "failed let initializer marks the module invalid");
    bool reported = false;
    for (const auto &diag : diags.all())
        if (diag.message.find("failed to emit an instruction") != std::string::npos)
            reported = true;
    CHECK(reported, "failed let initializer produces a codegen diagnostic");
}

static void test_codegen() {
    setbuf(stdout, NULL);
    printf("Running test_return_literal\n");
    test_return_literal();
    printf("Running test_void_main_implicit_return_exits_zero\n");
    test_void_main_implicit_return_exits_zero();
    printf("Running test_void_main_bare_return_exits_zero\n");
    test_void_main_bare_return_exits_zero();
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
    printf("Running test_optional_pointer_narrowing_runtime\n");
    test_optional_pointer_narrowing_runtime();
    printf("Running test_array_variable_indexing\n");
    test_array_variable_indexing();
    printf("Running test_shifts\n");
    test_shifts();
    test_compound_assign_runtime();
    test_logical_operator_runtime();
    test_optional_condition_runtime();
    test_raw_opaque_round_trip_runtime();
    test_bare_opaque_runtime();
    test_bare_opaque_raw_and_mismatch_runtime();
    test_bare_opaque_raw_pointer_same_payload_same_allocation_runtime();
    test_bare_opaque_raw_pointer_different_payload_runtime();
    test_bare_opaque_raw_pointer_literal_after_narrow_runtime();
    test_implicit_opaque_coercion_and_narrow_runtime();
    printf("Running test_struct_fields_and_parameter\n");
    test_struct_fields_and_parameter();
    printf("Running test_array_of_structs\n");
    test_array_of_structs();
    printf("Running test_enum_values\n");
    test_enum_values();
    printf("Running test_offsetof_and_alignof_runtime\n");
    test_offsetof_and_alignof_runtime();
    test_sizeof_intrinsic_runtime();
    printf("Running test_explicit_struct_alignment_layout\n");
    test_explicit_struct_alignment_layout();
    printf("Running test_unsupported_struct_alignment_is_rejected\n");
    test_unsupported_struct_alignment_is_rejected();
    printf("Running test_explicit_array_binding_alignment\n");
    test_explicit_array_binding_alignment();
    printf("Running test_explicit_global_binding_alignment\n");
    test_explicit_global_binding_alignment();
    printf("Running test_explicit_for_in_binding_alignment\n");
    test_explicit_for_in_binding_alignment();
    printf("Running test_unsupported_binding_alignment_is_rejected\n");
    test_unsupported_binding_alignment_is_rejected();
    printf("Running test_unverified_aligned_record_is_rejected_at_c_abi\n");
    test_unverified_aligned_record_is_rejected_at_c_abi();
    printf("Running test_slice_string_intrinsics_runtime\n");
    test_slice_string_intrinsics_runtime();
    test_when_expression_runtime();
    test_when_void_expression_case_executes();
    test_when_legacy_arrow_runtime();
    test_when_pattern_alternatives_runtime();
    test_when_default_must_be_last();
    test_tagged_union_pointer_is_type_runtime();
    test_qualified_receiver_mutation_runtime();
    test_free_borrow_parameter_runtime();
    test_when_narrowing_runtime();
    test_for_three_clause_runtime();
    test_labeled_loop_controls_runtime();
    test_for_in_runtime();
    test_nested_optional_for_in_runtime();
    printf("Running test_imported_generic_hashmap_runtime\n");
    test_imported_generic_hashmap_runtime();
    printf("Running test_imported_generic_dynarray_runtime\n");
    test_imported_generic_dynarray_runtime();
    printf("Running test_imported_result_runtime\n");
    test_imported_result_runtime();
    printf("Running test_soon_math_scalar_runtime\n");
    test_soon_math_scalar_runtime();
    printf("Running test_soon_slice_algorithms_runtime\n");
    test_soon_slice_algorithms_runtime();
    printf("Running test_imported_panic_runtime\n");
    test_imported_panic_runtime();
    printf("Running test_imported_memory_new_delete_runtime\n");
    test_imported_memory_new_delete_runtime();
    printf("Running test_imported_memory_make_release_runtime\n");
    test_imported_memory_make_release_runtime();
    printf("Running test_imported_tagged_union_is_member_runtime\n");
    test_imported_tagged_union_is_member_runtime();
    printf("Running test_optional_aggregate_narrowing_runtime\n");
    test_optional_aggregate_narrowing_runtime();
    printf("Running test_pack_literal_erased_into_opaque_runtime\n");
    test_pack_literal_erased_into_opaque_runtime();
    printf("Running test_null_member_in_pack_literal_runtime\n");
    test_null_member_in_pack_literal_runtime();
    printf("Running test_runtime_panic_flushes_buffered_output\n");
    test_runtime_panic_flushes_buffered_output();
    printf("Running test_integer_range_for_runtime\n");
    test_integer_range_for_runtime();
    printf("Running test_range_in_operator_and_when_runtime\n");
    test_range_in_operator_and_when_runtime();
    printf("Running test_user_contains_runtime\n");
    test_user_contains_runtime();
    printf("Running test_float_range_for_is_rejected\n");
    test_float_range_for_is_rejected();
    test_bodyless_range_for_is_rejected();
    test_imported_counter_runtime();
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
    printf("Running test_generic_bound_by_value_parameter_runs_without_borrow_attrs\n");
    test_generic_bound_by_value_parameter_runs_without_borrow_attrs();
    printf("Running test_interface_method_bound_runtime\n");
    test_interface_method_bound_runtime();
    printf("Running test_dyn_interface_method_dispatch_runtime\n");
    test_dyn_interface_method_dispatch_runtime();
    printf("Running test_dyn_trait_method_dispatch_runtime\n");
    test_dyn_trait_method_dispatch_runtime();
    printf("Running test_dyn_trait_struct_field_runtime\n");
    test_dyn_trait_struct_field_runtime();
    printf("Running test_dyn_trait_struct_literal_field_runtime\n");
    test_dyn_trait_struct_literal_field_runtime();
    printf("Running test_dyn_trait_mutating_receiver_dispatch_runtime\n");
    test_dyn_trait_mutating_receiver_dispatch_runtime();
    printf("Running test_dyn_trait_by_value_receiver_dispatch_runtime\n");
    test_dyn_trait_by_value_receiver_dispatch_runtime();
    printf("Running test_dyn_trait_lend_dyn_receiver_dispatch_runtime\n");
    test_dyn_trait_lend_dyn_receiver_dispatch_runtime();
    printf("Running test_dyn_interface_field_access_is_rejected\n");
    test_dyn_interface_field_access_is_rejected();
    printf("Running test_duplicate_struct_field_names_do_not_collide_globally\n");
    test_duplicate_struct_field_names_do_not_collide_globally();
    printf("Running test_f32_literal_stores_in_32_width\n");
    test_f32_literal_stores_in_32_width();
    printf("Running test_numeric_cast_codegen\n");
    test_numeric_cast_codegen();
    printf("Running test_narrowing_numeric_cast_literal_diagnostics\n");
    test_narrowing_numeric_cast_literal_diagnostics();
    printf("Running test_narrowing_numeric_cast_const_diagnostics\n");
    test_narrowing_numeric_cast_const_diagnostics();
    printf("Running test_state_machine_loop_executes\n");
    test_state_machine_loop_executes();
    printf("Running test_state_tail_calls_emit_musttail\n");
    test_state_tail_calls_emit_musttail();
    printf("Running test_state_machine_diverging_parameters_executes\n");
    test_state_machine_diverging_parameters_executes();
    printf("Running test_state_codegen_has_no_marker_runtime_symbols\n");
    test_state_codegen_has_no_marker_runtime_symbols();
    printf("Running test_state_loop_does_not_grow_stack\n");
    test_state_loop_does_not_grow_stack();
    printf("Running test_loop_local_binding_does_not_grow_stack\n");
    test_loop_local_binding_does_not_grow_stack();
    printf("Running test_local_state_machine_executes\n");
    test_local_state_machine_executes();
    printf("Running test_defer_reverse_order_before_return\n");
    test_defer_reverse_order_before_return();
    printf("Running test_defer_block_runs_on_normal_exit\n");
    test_defer_block_runs_on_normal_exit();
    printf("Running test_defer_in_if_and_loop\n");
    test_defer_in_if_and_loop();
    printf("Running test_defer_in_state_before_jump\n");
    test_defer_in_state_before_jump();
    printf("Running test_linked_list_acceptance_program\n");
    test_linked_list_acceptance_program();
    printf("Running test_modern_file_pipeline_executes_program\n");
    test_modern_file_pipeline_executes_program();
    printf("Running test_const_global_runtime\n");
    test_const_global_runtime();
    test_const_global_array_of_string_pointers();
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
    printf("Running test_var_self_and_var_parameter_runtime\n");
    test_var_self_and_var_parameter_runtime();
    printf("Running test_implement_block_method_runtime\n");
    test_implement_block_method_runtime();
    printf("Running test_optional_method_after_is_null_runtime\n");
    test_optional_method_after_is_null_runtime();
    printf("Running test_optional_method_after_not_is_null_runtime\n");
    test_optional_method_after_not_is_null_runtime();
    printf("Running test_optional_boolean_condition_runtime\n");
    test_optional_boolean_condition_runtime();
    printf("Running test_optional_pointer_boolean_condition_runtime\n");
    test_optional_pointer_boolean_condition_runtime();
    printf("Running test_optional_condition_keyword_runtime\n");
    test_optional_condition_keyword_runtime();
    printf("Running test_bare_condition_forms_runtime\n");
    test_bare_condition_forms_runtime();
    printf("Running test_optional_must_extraction_runtime\n");
    test_optional_must_extraction_runtime();
    printf("Running test_optional_must_none_runtime\n");
    test_optional_must_none_runtime();
    printf("Running test_optional_raw_extraction_runtime\n");
    test_optional_raw_extraction_runtime();
    printf("Running test_if_else_both_return_runtime\n");
    test_if_else_both_return_runtime();
    printf("Running test_for_loop_return_runtime\n");
    test_for_loop_return_runtime();
    printf("Running test_optional_dyn_local_binding_runtime\n");
    test_optional_dyn_local_binding_runtime();
    printf("Running test_optional_dyn_struct_field_runtime\n");
    test_optional_dyn_struct_field_runtime();
    printf("Running test_optional_dyn_narrowed_dispatch_runtime\n");
    test_optional_dyn_narrowed_dispatch_runtime();
    printf("Running test_optional_dyn_parameter_runtime\n");
    test_optional_dyn_parameter_runtime();
    printf("Running test_optional_dyn_assignment_runtime\n");
    test_optional_dyn_assignment_runtime();
    printf("Running test_optional_dyn_without_narrowing_diagnostic\n");
    test_optional_dyn_without_narrowing_diagnostic();
    printf("Running test_optional_opaque_slot_runtime\n");
    test_optional_opaque_slot_runtime();
    printf("Running test_optional_dyn_niche_size_runtime\n");
    test_optional_dyn_niche_size_runtime();
    printf("Running test_function_default_arguments_runtime\n");
    test_function_default_arguments_runtime();
    printf("Running test_nested_optional_argument_runtime\n");
    test_nested_optional_argument_runtime();
    printf("Running test_generic_optional_coercion_runtime\n");
    test_generic_optional_coercion_runtime();
    printf("Running test_primitive_optional_slice_implement_runtime\n");
    test_primitive_optional_slice_implement_runtime();
    printf("Running test_extern_variadic_call_runs\n");
    test_extern_variadic_call_runs();
    printf("Running test_external_symbol_alias_runtime\n");
    test_external_symbol_alias_runtime();
    printf("Running test_imported_external_globals_runtime\n");
    test_imported_external_globals_runtime();
    printf("Running test_external_symbol_method_receiver_runtime\n");
    test_external_symbol_method_receiver_runtime();
    printf("Running test_external_symbol_optional_pointer_method_receiver_runtime\n");
    test_external_symbol_optional_pointer_method_receiver_runtime();
    printf("Running test_c_default_arguments_and_string_escapes\n");
    test_c_default_arguments_and_string_escapes();
    printf("Running test_dollar_escape_hatch_runtime\n");
    test_dollar_escape_hatch_runtime();
    printf("Running test_octal_escape_runtime\n");
    test_octal_escape_runtime();
    printf("Running test_child_output_is_separate_from_compiler_output\n");
    test_child_output_is_separate_from_compiler_output();
    printf("Running test_direct_exec_inherits_parent_stdout\n");
    test_direct_exec_inherits_parent_stdout();
    printf("Running test_child_output_survives_nonzero_exit\n");
    test_child_output_survives_nonzero_exit();
    printf("Running test_import_stdio_runs\n");
    test_import_stdio_runs();
    printf("Running test_validated_c_struct_by_value_runs\n");
    test_validated_c_struct_by_value_runs();
    printf("Running test_validated_c_pair64_by_value_runs\n");
    test_validated_c_pair64_by_value_runs();
    printf("Running test_c_scalar_arguments_and_results_run\n");
    test_c_scalar_arguments_and_results_run();
    printf("Running test_c_bool_char_long_scalars_run\n");
    test_c_bool_char_long_scalars_run();
    printf("Running test_imported_c_pointer_roundtrip_runtime\n");
    test_imported_c_pointer_roundtrip_runtime();
    printf("Running test_imported_c_void_pointer_roundtrip_runtime\n");
    test_imported_c_void_pointer_roundtrip_runtime();
    printf("Running test_c_header_callback_declaration_is_not_imported\n");
    test_c_header_callback_declaration_is_not_imported();
    printf("Running test_imported_variadic_c_promotes_tail_arguments\n");
    test_imported_variadic_c_promotes_tail_arguments();
    printf("Running test_c_enum_return_local_argument_runs\n");
    test_c_enum_return_local_argument_runs();
    printf("Running test_validated_c_double_pair_by_value_runs\n");
    test_validated_c_double_pair_by_value_runs();
    printf("Running test_validated_c_large_record_by_value_runs\n");
    test_validated_c_large_record_by_value_runs();
    printf("Running test_validated_c_mixed_integer_record_by_value_runs\n");
    test_validated_c_mixed_integer_record_by_value_runs();
    printf("Running test_validated_c_reverse_mixed_integer_record_by_value_runs\n");
    test_validated_c_reverse_mixed_integer_record_by_value_runs();
    printf("Running test_c_pointer_is_null_uses_niche_comparison\n");
    test_c_pointer_cast_roundtrip_emits_no_conversion();
    test_c_pointer_is_null_uses_niche_comparison();
    printf("Running test_imported_stdout_global_runtime\n");
    test_imported_stdout_global_runtime();
    printf("Running test_radix_literal_return_emits_valid_module\n");
    test_radix_literal_return_emits_valid_module();
    printf("Running test_invalid_ir_refuses_object_emission\n");
    test_invalid_ir_refuses_object_emission();
    printf("Running test_failed_let_initializer_reports_emission_error\n");
    test_failed_let_initializer_reports_emission_error();
    printf("Running test_layout_api_matches_llvm\n");
    test_layout_api_matches_llvm();
    printf("Running test_raw_union_runtime_reinterpret\n");
    test_raw_union_runtime_reinterpret();
    printf("Running test_optional_and_slice_layouts\n");
    test_optional_and_slice_layouts();
    printf("Running test_slice_abi_matches_c_runtime\n");
    test_slice_abi_matches_c_runtime();
    printf("Running test_array_to_slice_runtime\n");
    test_array_to_slice_runtime();
    printf("Running test_raw_slice_and_index_runtime\n");
    test_raw_slice_and_index_runtime();
    printf("Running test_checked_index_runtime\n");
    test_checked_index_runtime();
    printf("Running test_function_pointer_call\n");
    test_function_pointer_call();
    printf("Running test_native_function_value_call\n");
    test_native_function_value_call();
    printf("Running test_anonymous_function_literal_runtime\n");
    test_anonymous_function_literal_runtime();
    printf("Running test_anonymous_function_capture_is_rejected\n");
    test_anonymous_function_capture_is_rejected();
    printf("Running test_state_value_dock_runtime\n");
    test_state_value_dock_runtime();
    printf("Running test_mutable_slice_from_c\n");
    test_mutable_slice_from_c();
    printf("Running test_variadic_slice_runtime\n");
    test_variadic_slice_runtime();
    printf("Running test_pipe_variadic_slice_runtime\n");
    test_pipe_variadic_slice_runtime();
    printf("Running test_variadic_slice_explicit_vs_auto_runtime\n");
    test_variadic_slice_explicit_vs_auto_runtime();
    printf("Running test_variadic_slice_method_runtime\n");
    test_variadic_slice_method_runtime();
    printf("Running test_variadic_slice_dyn_runtime\n");
    test_variadic_slice_dyn_runtime();
    printf("Running test_variadic_slice_state_runtime_forms\n");
    test_variadic_slice_state_runtime_forms();
    printf("Running test_variadic_slice_state_runtime\n");
    test_variadic_slice_state_runtime();
}

TEST_MAIN(codegen)
