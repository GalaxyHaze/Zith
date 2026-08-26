#include "cli/options.hpp"
#include "diagnostics/diagnostic-engine.hpp"
#include "diagnostics/error-codes.hpp"
#include "legacy-zith/ast/ast-builder.hpp"
#include "legacy-zith/sema/sema-pipeline.hpp"
#include "memory/arena.hpp"
#include "memory/string-interner.hpp"
#include "session/compilation-session.hpp"
#include "session/frontend-context.hpp"
#include "session/pipeline-plan.hpp"
#include "symbols/symbol-table.hpp"
#include "test-common.hpp"
#include "types/type-intern.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <memory>

using namespace zith;

struct SemaTest {
    memory::Arena arena;
    Options opts;

    SemaTest() : opts(arena) {}

    struct Result {
        bool ok             = false;
        size_t errorCount   = 0;
        size_t warningCount = 0;
        struct LightDiag {
            diagnostics::Severity severity;
            diagnostics::ErrCode code;
            std::string message;
        };
        bool hasMessage(std::string_view needle) const {
            for (const auto &d : diags) {
                if (d.message.find(needle) != std::string::npos) {
                    return true;
                }
            }
            return false;
        }
        bool hasErrorCode(diagnostics::ErrCode code) const {
            for (const auto &d : diags) {
                if (d.severity == diagnostics::Severity::Error && d.code == code) {
                    return true;
                }
            }
            return false;
        }
        bool hasWarningCode(diagnostics::ErrCode code) const {
            for (const auto &d : diags) {
                if (d.severity == diagnostics::Severity::Warning && d.code == code) {
                    return true;
                }
            }
            return false;
        }
        std::vector<LightDiag> diags;
    };

    Result run(std::string_view input, session::Stage target = session::Stage::TypeChecked) {
        session::CompilationSession session(opts, "test.zith");
        session.setBuffered(true);
        session.setContent(std::string(input));
        bool ok      = session.runTo(target);
        size_t errs  = 0;
        size_t warns = 0;
        std::vector<Result::LightDiag> copied_diags;
        for (const auto &d : session.diags().all()) {
            copied_diags.push_back(
                {d.severity, static_cast<diagnostics::ErrCode>(d.code), d.message});
            if (d.severity == diagnostics::Severity::Error) {
                errs++;
                std::printf("    [Diag] Code: %u, Message: %s\n", d.code, d.message.c_str());
            } else if (d.severity == diagnostics::Severity::Warning) {
                warns++;
                std::printf("    [Warn] Code: %u, Message: %s\n", d.code, d.message.c_str());
            }
        }
        return {ok && errs == 0, errs, warns, std::move(copied_diags)};
    }
};

struct ModernSemaTest {
    memory::Arena arena;
    Options opts;
    std::filesystem::path root;

    ModernSemaTest()
        : opts(arena), root(std::filesystem::temp_directory_path() / "zith-sema-modern-tests") {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        opts.targetStage = session::Stage::TypeChecked;
    }

    ~ModernSemaTest() {
        std::filesystem::remove_all(root);
    }

    void write(std::string_view name, std::string_view text) {
        auto path = root / name;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << text;
    }

    SemaTest::Result run(std::string_view main, session::Stage target = session::Stage::TypeChecked,
                         std::string_view main_name = "main.zith") {
        write(main_name, main);
        session::FrontendConfig config;
        config.workspaceRoot      = root.string();
        config.maxFrontendWorkers = 1;
        config.compilerVersion    = "test";
        auto context              = std::make_shared<session::FrontendContext>(config);
        session::CompilationSession session(opts, (root / main_name).string(), context);
        session.setBuffered(true);
        bool ok      = session.runTo(target);
        size_t errs  = 0;
        size_t warns = 0;
        std::vector<SemaTest::Result::LightDiag> copied_diags;
        for (const auto &d : session.diags().all()) {
            copied_diags.push_back(
                {d.severity, static_cast<diagnostics::ErrCode>(d.code), d.message});
            if (d.severity == diagnostics::Severity::Error) {
                errs++;
                std::printf("    [ModernDiag] Code: %u, Message: %s\n", d.code, d.message.c_str());
            } else if (d.severity == diagnostics::Severity::Warning) {
                warns++;
                std::printf("    [ModernWarn] Code: %u, Message: %s\n", d.code, d.message.c_str());
            }
        }
        return {ok && errs == 0, errs, warns, std::move(copied_diags)};
    }
};

// ── Tests ────────────────────────────────────────────────────

static void test_basic_unification() {
    SemaTest t;
    auto r = t.run("fn main(): i32 {\n"
                   "    var x: i32 = 42;\n"
                   "    return x;\n"
                   "}\n");
    CHECK(r.ok, "Basic variable unification with return");
}

static void test_type_mismatch() {
    SemaTest t;
    auto r = t.run("fn main(): i32 {\n"
                   "    var x: i32 = true;\n"
                   "    return x;\n"
                   "}\n");
    CHECK(!r.ok, "Type mismatch on initialization fails");
    CHECK(r.hasErrorCode(diagnostics::err::TypeMismatch), "Reports TypeMismatch (3001)");
}

static void test_return_type_mismatch() {
    SemaTest t;
    auto r = t.run("fn main(): bool {\n"
                   "    return 42;\n"
                   "}\n");
    CHECK(!r.ok, "Return type mismatch fails");
    CHECK(r.hasErrorCode(diagnostics::err::TypeMismatch), "Reports TypeMismatch (3001) for return");
}

static void test_control_flow_ok() {
    SemaTest t;
    auto r = t.run("fn main(): bool {\n"
                   "    var x: i32 = 5;\n"
                   "    if (x < 10) {\n"
                   "        true\n"
                   "    } else {\n"
                   "        false\n"
                   "    }\n"
                   "}\n");
    CHECK(r.ok, "Control flow with compatible branches compiles");
}

static void test_undefined_identifier() {
    SemaTest t;
    auto r = t.run("fn main() {\n"
                   "    var x: i32 = y;\n"
                   "}\n");
    CHECK(!r.ok, "Reference to undefined identifier fails");
    CHECK(r.hasErrorCode(diagnostics::err::UndefinedIdent), "Reports UndefinedIdent (2001)");
}

static void test_wrong_arity() {
    SemaTest t;
    auto r = t.run("fn add(a: i32, b: i32): i32 {\n"
                   "    return a + b;\n"
                   "}\n"
                   "fn main() {\n"
                   "    var x: i32 = add(1);\n"
                   "}\n");
    CHECK(!r.ok, "Function call with incorrect arity fails");
    CHECK(r.hasErrorCode(diagnostics::err::NoMatchingFn), "Reports NoMatchingFn (2007)");
}

static void test_state_machine_ok() {
    ModernSemaTest t;
    auto r = t.run("state Loop(n: i32): i32 {\n"
                   "    if (n < 10) {\n"
                   "        jump Loop(n + 1);\n"
                   "    }\n"
                   "    return n;\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    return dock Loop(0);\n"
                   "}\n",
                   session::Stage::HirLowered);
    CHECK(r.ok, "same-prototype state machine lowers successfully");
}

static void test_state_machine_allows_diverging_parameters() {
    ModernSemaTest t;
    auto r = t.run("state Start(n: i32): i32 {\n"
                   "    if (n == 0) {\n"
                   "        return 0;\n"
                   "    }\n"
                   "    jump Done(n, 0);\n"
                   "}\n"
                   "state Done(n: i32, acc: i32): i32 {\n"
                   "    if (n == 0) {\n"
                   "        return acc;\n"
                   "    }\n"
                   "    jump Start(n - 1);\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    return dock Start(3);\n"
                   "}\n",
                   session::Stage::HirLowered);
    CHECK(r.ok, "state machine with differing parameter lists lowers successfully");
}

static void test_state_machine_return_type_mismatch_is_rejected() {
    ModernSemaTest t;
    auto r = t.run("state Start(n: i32): i32 {\n"
                   "    jump Done(n);\n"
                   "}\n"
                   "state Done(n: i32): bool {\n"
                   "    return true;\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    return dock Start(0);\n"
                   "}\n",
                   session::Stage::HirLowered);
    CHECK(!r.ok, "state machine with mismatched return types is rejected");
    CHECK(r.hasMessage("jump target must be in the same state machine"),
          "mismatched state return types split the machine and reject the jump");
}

static void test_state_machine_rejects_mixed_return_types() {
    ModernSemaTest t;
    auto r = t.run("state Start(n: i32): i32 {\n"
                   "    jump Done(n);\n"
                   "}\n"
                   "state Other(x: f64): f64 {\n"
                   "    return x;\n"
                   "}\n"
                   "state Done(n: i32): bool {\n"
                   "    return true;\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    return dock Start(0);\n"
                   "}\n",
                   session::Stage::HirLowered);
    CHECK(!r.ok, "state declarations mixing return types are rejected");
    CHECK(r.hasMessage("jump target must be in the same state machine"),
          "mixed return types keep jump validation per target");
}

static void test_jump_requires_state_context() {
    SemaTest t;
    auto r = t.run("fn main(): i32 {\n"
                   "    jump Loop(0);\n"
                   "    return 0;\n"
                   "}\n",
                   session::Stage::HirLowered);
    CHECK(!r.ok, "jump outside a state function is rejected");
    CHECK(r.hasMessage("jump is only allowed inside a state function"),
          "jump restriction mentions state functions");
}

static void test_dock_requires_state_target() {
    SemaTest t;
    auto r = t.run("fn target(n: i32): i32 { return n; }\n"
                   "fn main(): i32 {\n"
                   "    return dock target(1);\n"
                   "}\n",
                   session::Stage::HirLowered);
    CHECK(!r.ok, "dock to a non-state function is rejected");
    CHECK(r.hasMessage("dock target must be a state function"),
          "dock restriction mentions state functions");
}

static void test_state_transition_arity_and_type_mismatch() {
    ModernSemaTest t;
    auto bad_arity = t.run("state Start(v: i32): i32 {\n"
                           "    jump Done();\n"
                           "}\n"
                           "state Done(v: i32): i32 {\n"
                           "    return v;\n"
                           "}\n"
                           "fn main(): i32 { return dock Start(1); }\n",
                           session::Stage::HirLowered);
    CHECK(!bad_arity.ok, "state transition arity mismatch is rejected");
    CHECK(bad_arity.hasErrorCode(diagnostics::err::NoMatchingFn),
          "state transition arity mismatch reports NoMatchingFn");

    auto bad_type = t.run("state Start(v: i32): i32 {\n"
                          "    jump Done(true);\n"
                          "}\n"
                          "state Done(v: i32): i32 {\n"
                          "    return v;\n"
                          "}\n"
                          "fn main(): i32 { return dock Start(1); }\n",
                          session::Stage::HirLowered);
    CHECK(!bad_type.ok, "state transition type mismatch is rejected");
    CHECK(bad_type.hasErrorCode(diagnostics::err::NoMatchingFn),
          "state transition type mismatch reports NoMatchingFn");
}

static void test_dock_argument_arity_and_type_mismatch() {
    ModernSemaTest t;
    auto bad_arity = t.run("state Start(v: i32): i32 {\n"
                           "    return v;\n"
                           "}\n"
                           "fn main(): i32 {\n"
                           "    return dock Start();\n"
                           "}\n",
                           session::Stage::HirLowered);
    CHECK(!bad_arity.ok, "dock arity mismatch is rejected");
    CHECK(bad_arity.hasErrorCode(diagnostics::err::NoMatchingFn),
          "dock arity mismatch reports NoMatchingFn");

    auto bad_type = t.run("state Start(v: i32): i32 {\n"
                          "    return v;\n"
                          "}\n"
                          "fn main(): i32 {\n"
                          "    return dock Start(true);\n"
                          "}\n",
                          session::Stage::HirLowered);
    CHECK(!bad_type.ok, "dock type mismatch is rejected");
    CHECK(bad_type.hasErrorCode(diagnostics::err::NoMatchingFn),
          "dock type mismatch reports NoMatchingFn");
}

static void test_state_return_type_checked_against_machine_result() {
    ModernSemaTest t;
    auto r = t.run("state Start(): i32 {\n"
                   "    return true;\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    return dock Start();\n"
                   "}\n",
                   session::Stage::HirLowered);
    CHECK(!r.ok, "state return value must match the declared machine result");
    CHECK(r.hasErrorCode(diagnostics::err::TypeMismatch),
          "state return mismatch reports TypeMismatch");
}

static void test_nested_state_machines() {
    ModernSemaTest t;
    auto ok = t.run("fn machine(): i32 {\n"
                    "    state Start(n: i32): i32 {\n"
                    "        if (n == 0) {\n"
                    "            return 42;\n"
                    "        }\n"
                    "        jump Done(n - 1);\n"
                    "    }\n"
                    "    state Done(n: i32): i32 {\n"
                    "        jump Start(n);\n"
                    "    }\n"
                    "    return dock Start(3);\n"
                    "}\n"
                    "fn main(): i32 {\n"
                    "    return machine();\n"
                    "}\n",
                    session::Stage::HirLowered);
    CHECK(ok.ok, "local state dock and jumps resolve within the owning function");

    auto outside = t.run("fn one(): i32 {\n"
                         "    state Start(): i32 { return 1; }\n"
                         "    return 1;\n"
                         "}\n"
                         "fn other(): i32 {\n"
                         "    return dock Start();\n"
                         "}\n");
    CHECK(!outside.ok, "a local state is not visible outside its owning function");

    auto same_name = t.run("fn first(): i32 {\n"
                           "    state Step(): i32 { return 1; }\n"
                           "    return dock Step();\n"
                           "}\n"
                           "fn second(): i32 {\n"
                           "    state Step(): i32 { return 2; }\n"
                           "    return dock Step();\n"
                           "}\n",
                           session::Stage::HirLowered);
    CHECK(same_name.ok, "identical local state names in separate functions do not conflict");
}

static void test_extern_fn_call_ok() {
    SemaTest t;
    auto r = t.run("extern fn putchar(c: i32): i32\n"
                   "fn main() {\n"
                   "    putchar(65);\n"
                   "}\n");
    CHECK(r.ok, "Calling extern function with correct argument type works");
}

static void test_extern_fn_call_bad_arg() {
    SemaTest t;
    auto r = t.run("extern fn putchar(c: i32): i32\n"
                   "fn main() {\n"
                   "    putchar(true);\n"
                   "}\n");
    CHECK(!r.ok, "Calling extern function with incorrect argument type fails");
    CHECK(r.hasErrorCode(diagnostics::err::NoMatchingFn), "Reports NoMatchingFn (2007)");
}

static void test_type_alias_unification() {
    SemaTest t;
    auto r = t.run("alias MyInt = i32;\n"
                   "fn main(): MyInt {\n"
                   "    var x: MyInt = 100;\n"
                   "    var y: i32 = x;\n"
                   "    return y;\n"
                   "}\n");
    CHECK(r.ok, "Type alias unifies successfully with underlying primitive type");
}

static void test_type_alias_invalid_assignment() {
    SemaTest t;
    auto r = t.run("alias MyInt = i32;\n"
                   "fn main() {\n"
                   "    var x: MyInt = true;\n"
                   "}\n");
    CHECK(!r.ok, "Invalid assignment to type aliased variable fails");
    CHECK(r.hasErrorCode(diagnostics::err::TypeMismatch), "Reports TypeMismatch (3001)");
}

static void test_binary_op_type_error() {
    SemaTest t;
    auto r = t.run("fn main() {\n"
                   "    var x: i32 = 1 + true;\n"
                   "}\n");
    CHECK(!r.ok, "Addition between incompatible types fails");
    CHECK(r.hasErrorCode(diagnostics::err::TypeMismatch), "Reports TypeMismatch (3001)");
}

static void test_while_loop_ok() {
    SemaTest t;
    auto r = t.run("fn main() {\n"
                   "    var x: i32 = 0;\n"
                   "    while (x < 5) {\n"
                   "        x = x + 1;\n"
                   "    }\n"
                   "}\n");
    CHECK(r.ok, "While loop with boolean condition compiles");
}

static void test_unary_op_ok() {
    SemaTest t;
    auto r = t.run("fn main(): bool {\n"
                   "    var x: bool = true;\n"
                   "    return not x;\n"
                   "}\n");
    CHECK(r.ok, "Unary negation compiles");
}

static void test_index_validation() {
    SemaTest t;
    auto r = t.run("fn main() {\n"
                   "    var x: i32 = 0;\n"
                   "    x[0];\n"
                   "}\n");
    CHECK(!r.ok, "Index access on non-indexable type fails semantic analysis");
    CHECK_EQ(r.errorCount, 1u, "Exactly one error for invalid index access");
    CHECK(r.hasErrorCode(diagnostics::err::TypeMismatch), "Reports TypeMismatch error");
}

static void test_field_not_implemented_warning() {
    SemaTest t;
    auto r = t.run("fn main() {\n"
                   "    var x: i32 = 0;\n"
                   "    x.value;\n"
                   "}\n");
    CHECK(!r.ok, "Field access on a non-struct fails semantic analysis");
    CHECK_EQ(r.errorCount, 1u, "Exactly one error for invalid field access");
    CHECK(r.hasErrorCode(diagnostics::err::TypeMismatch),
          "Reports TypeMismatch for invalid field access");
}

static void test_macro_unknown() {
    SemaTest t;
    auto r = t.run("fn main() {\n"
                   "    @foo();\n"
                   "}\n");
    CHECK(!r.ok, "Unknown macro fails semantic analysis");
    CHECK_EQ(r.errorCount, 1u, "Exactly one error for unknown macro");
    CHECK(r.hasErrorCode(diagnostics::err::MacroUnknown), "Reports MacroUnknown error");
}

static void test_macro_defined() {
    SemaTest t;
    auto r = t.run("macro m() { }\nfn main() { @m(); }\n");
    CHECK(r.ok, "Defined macro call passes semantic analysis");
}

static void check_unsupported_syntax(const SemaTest::Result &r) {
    CHECK(!r.ok, "Unsupported syntax fails semantic analysis");
    CHECK_EQ(r.errorCount, 1u, "Unsupported syntax emits one primary error");
    CHECK_EQ(r.warningCount, 0u, "Unsupported syntax does not emit a warning");
    CHECK(r.hasErrorCode(diagnostics::err::UnsupportedSyntax),
          "Reports UnsupportedSyntax semantic error");
    CHECK(!r.hasWarningCode(diagnostics::err::NotImplemented),
          "Does not reuse the NotImplemented warning");
}

static void test_tagged_union_is_syntax() {
    SemaTest tagged;
    auto tagged_result = tagged.run("union Value { i32, f64 }\n"
                                    "fn main(): bool {\n"
                                    "    var v: Value = Value { 42 };\n"
                                    "    return v is i32;\n"
                                    "}\n");
    CHECK(tagged_result.ok, "tagged union construction and 'is Type' pass sema");

    SemaTest is_test;
    auto is_result = is_test.run("fn main() { 1 is Missing; }\n");
    CHECK(!is_result.ok, "'is Type' on a non-union operand fails sema");
    CHECK(is_result.hasErrorCode(diagnostics::err::TypeMismatch),
          "non-union 'is Type' reports TypeMismatch");

    SemaTest raw_test;
    auto raw_result = raw_test.run("raw union Bits { u8, u32 }\n"
                                   "fn main() { var b: Bits = Bits { 1u8 }; _ = b is u8; }\n");
    CHECK(!raw_result.ok, "'is Type' on a raw union fails sema");
    CHECK(raw_result.hasErrorCode(diagnostics::err::TypeMismatch),
          "raw union 'is Type' reports TypeMismatch");

    SemaTest member_test;
    auto member_result =
        member_test.run("union Value { i32, f64 }\n"
                        "fn main() { var v: Value = Value { 42 }; _ = v is u8; }\n");
    CHECK(!member_result.ok, "'is Type' with a non-member type fails sema");
    CHECK(member_result.hasErrorCode(diagnostics::err::InvalidCast),
          "non-member 'is Type' reports InvalidCast");

    SemaTest as_test;
    auto as_result = as_test.run("fn main() { 1 as Missing; }\n");
    CHECK(!as_result.ok, "unknown cast target still fails sema");
    CHECK(as_result.hasErrorCode(diagnostics::err::UnsupportedSyntax),
          "unknown cast target reports UnsupportedSyntax");
}

static void test_modern_tagged_union_cast_policy() {
    ModernSemaTest narrowed;
    auto ok = narrowed.run("union Value { *char, i32 }\n"
                           "fn main() {\n"
                           "    let v = Value{\"ok\"};\n"
                           "    when (v) {\n"
                           "        (v is *char) ~> { let p: *char = v; },\n"
                           "        (_) ~> { }\n"
                           "    }\n"
                           "}\n",
                           session::Stage::TypeChecked);
    CHECK(ok.ok, "tagged union member is available in a narrowed when body without 'as'");

    ModernSemaTest manual;
    auto bad = manual.run("union Value { i32, f64 }\n"
                          "fn main() {\n"
                          "    let v = Value { 1 };\n"
                          "    v as i32;\n"
                          "}\n");
    CHECK(!bad.ok, "manual tagged union member extraction outside narrowing is rejected");
    CHECK(bad.hasMessage("requires a checked/narrowed context"),
          "the tagged union cast diagnostic asks for a narrowed/raw context");

    ModernSemaTest raw_cast;
    auto raw = raw_cast.run("union Value { i32, f64 }\n"
                            "fn main() {\n"
                            "    var v: Value = Value { 1 };\n"
                            "    var x: i32 = raw v as i32;\n"
                            "}\n");
    CHECK(raw.ok, "raw tagged union member extraction remains available");

    ModernSemaTest raw_union;
    auto raw_union_ok = raw_union.run("raw union Bits { u8, u32 }\n"
                                      "fn main() {\n"
                                      "    var b: Bits = Bits { 1u8 };\n"
                                      "    var word: u32 = b as u32;\n"
                                      "}\n");
    CHECK(raw_union_ok.ok, "raw union member casts keep free reinterpretation");
}

static void test_modern_qualified_method_receivers() {
    ModernSemaTest ok;
    auto good = ok.run("struct Sample {\n"
                       "    x: i32,\n"
                       "    fn bump(self: lend Sample) { self->x = self->x + 1; }\n"
                       "    fn read(self: view Sample): i32 { return self->x; }\n"
                       "    fn bump_ptr(self: *Sample) { self->x = self->x + 5; }\n"
                       "}\n"
                       "fn main(): i32 {\n"
                       "    var s: Sample = Sample { x: 1 };\n"
                       "    s.bump();\n"
                       "    s.bump_ptr();\n"
                       "    return s.read();\n"
                       "}\n");
    CHECK(good.ok, "qualified lend/view and explicit pointer receivers type-check");

    ModernSemaTest view_write;
    auto bad = view_write.run("struct Sample {\n"
                              "    x: i32,\n"
                              "    fn bad(self: view Sample) { self->x = 3; }\n"
                              "}\n"
                              "fn main(): i32 {\n"
                              "    let s: Sample = Sample { x: 1 };\n"
                              "    s.bad();\n"
                              "    return 0;\n"
                              "}\n");
    CHECK(!bad.ok, "writes through a view receiver are rejected");
    CHECK(bad.hasErrorCode(diagnostics::err::WriteThroughView),
          "view receiver field writes report WriteThroughView");
}

static void test_fallback_and_propagation_are_rejected() {
    SemaTest fallback_optional;
    check_unsupported_syntax(fallback_optional.run("fn main() { ?missing; }\n"));

    SemaTest fallback_failable;
    check_unsupported_syntax(fallback_failable.run("fn main() { !missing; }\n"));

    SemaTest propagate_failable;
    check_unsupported_syntax(propagate_failable.run("fn main() { missing!; }\n"));
}

static void test_optional_propagation_is_supported() {
    SemaTest t;
    auto r = t.run("fn main() {\n"
                   "    var x: ?i32 = null;\n"
                   "    x?;\n"
                   "}\n");
    CHECK(!r.ok, "Optional propagation on a non-optional-returning function fails");
    CHECK(r.hasErrorCode(diagnostics::err::TypeMismatch),
          "Reports the missing optional return as TypeMismatch (3001)");

    SemaTest unknown;
    auto r2 = unknown.run("fn main() { missing?; }\n");
    CHECK(!r2.ok, "Propagation on an unknown identifier fails");
    CHECK(r2.hasErrorCode(diagnostics::err::UndefinedIdent),
          "Reports the unknown identifier as UndefinedIdent (2001)");
}

static void test_word_sequences_are_rejected() {
    SemaTest t;
    check_unsupported_syntax(t.run("fn main() { 1 nop 2; }\n"));
}

static void test_use_statements_are_rejected() {
    SemaTest t;
    check_unsupported_syntax(t.run("fn main() { use SQL; }\n"));
}

static void test_word_and_context_declarations_are_rejected() {
    SemaTest word;
    check_unsupported_syntax(word.run("nop SELECT;\n"));

    SemaTest context;
    check_unsupported_syntax(context.run("context SQL {}\n"));
}

static void test_word_call_ast_is_rejected() {
    memory::Arena arena;
    memory::StringInterner interner(arena);
    diagnostics::DiagnosticEngine diags(arena);
    ast::AstBuilder builder(arena, interner);
    symbols::SymbolTable syms(arena, &interner);
    types::TypeIntern types(arena, interner);

    memory::DynArray<ast::ExprId> args(arena);
    args.push(builder.ident("missing"));
    auto call = builder.wordCall("SELECT", std::move(args));

    memory::DynArray<ast::StmtId> stmts(arena);
    auto body = builder.block(std::move(stmts), call);
    memory::DynArray<std::string_view> params(arena);
    auto fn = builder.fnDecl("main", std::move(params), body);
    syms.declare("main", symbols::SymbolVisibility::Private, 0, symbols::SymKind::Fn, fn);

    ast::ProgramNode program(arena);
    program.decls.push(fn);
    sema::SemaPipeline pipeline(syms, types, diags, builder, arena);
    bool ok = pipeline.run(program);

    size_t errors   = 0;
    size_t warnings = 0;
    std::vector<SemaTest::Result::LightDiag> copied_diags;
    for (const auto &diag : diags.all()) {
        copied_diags.push_back({diag.severity, static_cast<diagnostics::ErrCode>(diag.code)});
        if (diag.severity == diagnostics::Severity::Error)
            errors++;
        else if (diag.severity == diagnostics::Severity::Warning)
            warnings++;
    }

    check_unsupported_syntax({ok && errors == 0, errors, warnings, std::move(copied_diags)});
}

static void test_unsupported_syntax_does_not_reach_hir() {
    SemaTest t;
    check_unsupported_syntax(t.run("fn main() { ?missing; }\n", session::Stage::HirLowered));
}

static void test_offsetof_intrinsic_ok() {
    SemaTest t;
    auto r = t.run("struct Pair { left: i32, right: i32 }\n"
                   "fn main(): i32 {\n"
                   "    return @offsetOf(Pair, right);\n"
                   "}\n");
    CHECK(r.ok, "@offsetOf on a struct field type-checks");
}

static void test_offsetof_non_struct_fails() {
    SemaTest t;
    auto r = t.run("fn main(): i32 {\n"
                   "    return @offsetOf(i32, value);\n"
                   "}\n");
    CHECK(!r.ok, "@offsetOf rejects non-struct types");
    CHECK(r.hasErrorCode(diagnostics::err::TypeMismatch), "Reports TypeMismatch for non-struct");
}

static void test_offsetof_unknown_field_fails() {
    SemaTest t;
    auto r = t.run("struct Pair { left: i32, right: i32 }\n"
                   "fn main(): i32 {\n"
                   "    return @offsetOf(Pair, middle);\n"
                   "}\n");
    CHECK(!r.ok, "@offsetOf rejects unknown fields");
    CHECK(r.hasErrorCode(diagnostics::err::NoMember), "Reports NoMember for unknown field");
}

static void test_sizeof_intrinsic_ok() {
    SemaTest t;
    auto r = t.run("struct Pair { left: i32, right: i32 }\n"
                   "fn main(): u64 {\n"
                   "    return @sizeOf(Pair);\n"
                   "}\n");
    CHECK(r.ok, "@sizeOf on a struct type-checks as u64");
}

static void test_sizeof_primitive_ok() {
    SemaTest t;
    auto r = t.run("fn main(): u64 {\n"
                   "    return @sizeOf(i32);\n"
                   "}\n");
    CHECK(r.ok, "@sizeOf on a primitive type-checks as u64");
}

static void test_sizeof_void_fails() {
    SemaTest t;
    auto r = t.run("fn main(): u64 {\n"
                   "    return @sizeOf(void);\n"
                   "}\n");
    CHECK(!r.ok, "@sizeOf rejects 'void'");
    CHECK(r.hasErrorCode(diagnostics::err::TypeMismatch), "Reports TypeMismatch for void");
}

static void test_when_literal_cases_ok() {
    SemaTest t;
    auto r = t.run("fn main(): i32 {\n"
                   "    var n: i32 = 2;\n"
                   "    return when (n) {\n"
                   "        (0) ~> 10,\n"
                   "        (1..3) ~> 20,\n"
                   "        (_) ~> 40\n"
                   "    };\n"
                   "}\n");
    CHECK(r.ok, "when with literal, range, and default cases type-checks");
}

static void test_when_boolean_conditions_ok() {
    SemaTest t;
    auto r = t.run("fn main(): i32 {\n"
                   "    var n: i32 = 2;\n"
                   "    return when (n) {\n"
                   "        (n == 0) ~> 10,\n"
                   "        (n > 3) ~> 20,\n"
                   "        (_) ~> 40\n"
                   "    };\n"
                   "}\n");
    CHECK(r.ok, "when with boolean conditions type-checks");
}

static void test_when_missing_default_fails() {
    SemaTest t;
    auto r = t.run("fn main(): i32 {\n"
                   "    var n: i32 = 2;\n"
                   "    return when (n) {\n"
                   "        (0) ~> 10,\n"
                   "        (1..3) ~> 20\n"
                   "    };\n"
                   "}\n");
    CHECK(!r.ok, "value-producing when without a default case fails");
    CHECK(r.hasErrorCode(diagnostics::err::TypeMismatch),
          "Reports TypeMismatch for non-exhaustive when");
}

static void test_when_mismatched_bodies_fail() {
    SemaTest t;
    auto r = t.run("fn main() {\n"
                   "    var n: i32 = 2;\n"
                   "    var x = when (n) {\n"
                   "        (0) ~> 10,\n"
                   "        (1..3) ~> \"a\",\n"
                   "        (_) ~> 40\n"
                   "    };\n"
                   "}\n");
    CHECK(!r.ok, "when with mismatched case body types fails");
    CHECK(r.hasErrorCode(diagnostics::err::TypeMismatch), "Reports TypeMismatch for bodies");
}

static void test_when_range_subject_mismatch_fails() {
    SemaTest t;
    auto r = t.run("fn main() {\n"
                   "    var s = \"abc\";\n"
                   "    var x = when (s) {\n"
                   "        (1..3) ~> 10,\n"
                   "        (_) ~> 40\n"
                   "    };\n"
                   "}\n");
    CHECK(!r.ok, "when range pattern must match the subject type");
    CHECK(r.hasErrorCode(diagnostics::err::TypeMismatch), "Reports TypeMismatch for range bounds");
}

static void test_named_struct_literal_ok() {
    SemaTest t;
    auto r = t.run("struct Pair { left: i32, right: i32 }\n"
                   "fn main() {\n"
                   "    var pair: Pair = Pair{right: 4, left: 3};\n"
                   "}\n");
    CHECK(r.ok, "Named struct literals type-check");
}

static void test_named_struct_literal_duplicate_field_fails() {
    SemaTest t;
    auto r = t.run("struct Pair { left: i32, right: i32 }\n"
                   "fn main() {\n"
                   "    var pair: Pair = Pair{left: 1, left: 2};\n"
                   "}\n");
    CHECK(!r.ok, "Duplicate named struct literal fields fail");
    CHECK(r.hasErrorCode(diagnostics::err::TypeMismatch),
          "Reports TypeMismatch for duplicate named field");
}

static void test_named_struct_literal_unknown_field_fails() {
    SemaTest t;
    auto r = t.run("struct Pair { left: i32, right: i32 }\n"
                   "fn main() {\n"
                   "    var pair: Pair = Pair{left: 1, middle: 2};\n"
                   "}\n");
    CHECK(!r.ok, "Unknown named struct literal fields fail");
    CHECK(r.hasErrorCode(diagnostics::err::NoMember), "Reports NoMember for unknown field");
}

static void test_named_struct_literal_mixed_form_fails() {
    SemaTest t;
    auto r = t.run("struct Pair { left: i32, right: i32 }\n"
                   "fn main() {\n"
                   "    var pair: Pair = Pair{1, right: 2};\n"
                   "}\n");
    CHECK(!r.ok, "Mixing positional and named struct literal fields fails");
    CHECK(r.hasErrorCode(diagnostics::err::TypeMismatch), "Reports TypeMismatch for mixed form");
}

static void test_named_struct_literal_placeholder_without_default_fails() {
    SemaTest t;
    auto r = t.run("struct Pair { left: i32, right: i32 }\n"
                   "fn main() {\n"
                   "    var pair: Pair = Pair{left: _, right: 2};\n"
                   "}\n");
    CHECK(!r.ok, "Using '_' without a field default fails");
    CHECK(r.hasErrorCode(diagnostics::err::TypeMismatch),
          "Reports TypeMismatch for missing default");
}

static void test_struct_field_default_type_mismatch_fails() {
    SemaTest t;
    auto r = t.run("struct Pair { left: i32 = true, right: i32 }\n"
                   "fn main() {}\n");
    CHECK(!r.ok, "Struct field defaults must match the field type");
    CHECK(r.hasErrorCode(diagnostics::err::TypeMismatch),
          "Reports TypeMismatch for invalid field default");
}

// ── Modern Sema tests (via FrontendContext) ─────────────────

static void test_modern_basic_valid() {
    ModernSemaTest t;
    auto r = t.run("fn main(): i32 {\n"
                   "    42\n"
                   "}\n");
    CHECK(r.ok, "Modern sema accepts a simple integer-returning function");
}

static void test_modern_return_type_mismatch() {
    ModernSemaTest t;
    auto r = t.run("fn main(): bool {\n"
                   "    42\n"
                   "}\n");
    CHECK(!r.ok, "Modern sema rejects an expression body with the wrong return type");
}

static void test_modern_call_arity() {
    ModernSemaTest t;
    auto r = t.run("fn add(a: i32): i32 { a }\n"
                   "fn main() {\n"
                   "    add(1, 2)\n"
                   "}\n");
    CHECK(!r.ok, "Modern sema rejects a call with too many arguments");
}

static void test_modern_call_arg_type() {
    ModernSemaTest t;
    auto r = t.run("fn add(a: i32): i32 { a }\n"
                   "fn main() {\n"
                   "    add(true)\n"
                   "}\n");
    CHECK(!r.ok, "Modern sema rejects a call with a mismatched argument type");
}

static void test_modern_if_condition() {
    ModernSemaTest t;
    auto r = t.run("fn main() {\n"
                   "    if (1) { }\n"
                   "}\n");
    CHECK(!r.ok, "Modern sema rejects an if condition that is not boolean");
}

static void test_modern_while_condition() {
    ModernSemaTest t;
    auto r = t.run("fn main() {\n"
                   "    while (1) { }\n"
                   "}\n");
    CHECK(!r.ok, "Modern sema rejects a while condition that is not boolean");
}

static void test_modern_struct_decl() {
    ModernSemaTest t;
    auto r = t.run("struct Point { x: i32, y: i32 }\n"
                   "fn main() { }\n");
    CHECK(r.ok, "Modern sema accepts a struct declaration without errors");
}

static void test_modern_struct_literal_missing_field_fails() {
    ModernSemaTest t;
    auto r = t.run("struct Pair { left: i32, right: i32 }\n"
                   "fn main() {\n"
                   "    var p: Pair = Pair{1};\n"
                   "}\n");
    CHECK(!r.ok, "Positional struct literals cannot omit fields without defaults");
    CHECK(r.hasErrorCode(diagnostics::err::TypeMismatch),
          "Missing struct literal fields report TypeMismatch");
    CHECK(r.hasMessage("missing field 'right'"),
          "The missing field diagnostic names the omitted field");
}

static void test_modern_struct_literal_omitted_default_allowed() {
    ModernSemaTest t;
    auto r = t.run("struct Pair { left: i32 = 3, right: i32 = 4 }\n"
                   "fn main() {\n"
                   "    var p: Pair = Pair{right: 9};\n"
                   "}\n");
    CHECK(r.ok, "Named struct literals may omit fields that have defaults");
}

static void test_modern_struct_literal_missing_named_field_without_default_fails() {
    ModernSemaTest t;
    auto r = t.run("struct Pair { left: i32, right: i32 }\n"
                   "fn main() {\n"
                   "    var p: Pair = Pair{right: 9};\n"
                   "}\n");
    CHECK(!r.ok, "Named struct literals cannot omit fields without defaults");
    CHECK(r.hasMessage("missing field 'left'"),
          "The named-literal diagnostic names the omitted field");
}

static void test_modern_struct_name_field_access_fails() {
    ModernSemaTest t;
    auto r = t.run("struct Pair { first: i32, second: i32 }\n"
                   "fn take(a: i32, b: i32) {}\n"
                   "fn main() {\n"
                   "    take(Pair.first, Pair.second);\n"
                   "}\n");
    CHECK(!r.ok, "Using a struct name for field access is rejected");
    CHECK(r.hasErrorCode(diagnostics::err::TypeMismatch),
          "Struct name field access reports TypeMismatch");
    CHECK(r.hasMessage("struct name 'Pair' cannot be used as a value in field access"),
          "The struct name error points at the type-as-value misuse");
}

static void test_modern_raw_union_member_cast() {
    ModernSemaTest t;
    auto r = t.run("raw union Bits { u8, u32 }\n"
                   "fn main() {\n"
                   "    var b: Bits = Bits { 255u8 };\n"
                   "    var word: u32 = b as u32;\n"
                   "    var byte: u8 = word as Bits as u8;\n"
                   "}\n");
    CHECK(r.ok, "Modern sema accepts raw union construction and member casts");
}

static void test_modern_raw_union_cast_rejects_non_member() {
    ModernSemaTest t;
    auto r = t.run("raw union Bits { u8, u32 }\n"
                   "fn main() {\n"
                   "    var b: Bits = Bits { 0u8 };\n"
                   "    var x: f64 = b as f64;\n"
                   "}\n");
    CHECK(!r.ok, "Modern sema rejects a cast from a union to a non-member type");
    CHECK(r.hasErrorCode(diagnostics::err::InvalidCast),
          "Non-member union cast reports InvalidCast");
    CHECK(r.hasMessage("'f64' is not a member of 'Bits'"),
          "The non-member diagnostic names the union member");
}

static void test_modern_union_parameter_and_return() {
    ModernSemaTest t;
    auto r = t.run("raw union Any { i32, f64 }\n"
                   "fn pick(v: Any): Any {\n"
                   "    return v;\n"
                   "}\n");
    CHECK(r.ok, "Modern sema accepts a raw union as parameter and return type");
}

static void test_modern_import_call() {
    ModernSemaTest t;
    t.write("dep.zith", "pub fn dep_fn(): i32 { 42 }\n");
    auto r = t.run("from dep\n"
                   "fn main() {\n"
                   "    dep_fn()\n"
                   "}\n");
    CHECK(r.ok, "Modern sema resolves an imported function call");
}

static void test_modern_void_fn() {
    ModernSemaTest t;
    auto r = t.run("fn log(msg: *char) { }\n"
                   "fn main() {\n"
                   "    log(\"ok\");\n"
                   "}\n");
    CHECK(r.ok, "Modern sema accepts a void function from snapshot without a return type");
}

static void test_modern_binary_arithmetic() {
    ModernSemaTest t;
    auto r = t.run("fn main(): i32 {\n"
                   "    var x: i32 = 1 + 2 * 3;\n"
                   "    x\n"
                   "}\n");
    CHECK(r.ok, "Modern sema accepts binary arithmetic expression body");
}

static void test_modern_unary_not_on_bool() {
    ModernSemaTest t;
    auto r = t.run("fn main() {\n"
                   "    var flag: bool = true;\n"
                   "    if (not flag) { }\n"
                   "}\n");
    CHECK(r.ok, "Modern sema accepts not on a boolean");
}

static void test_modern_unary_invalid_type() {
    ModernSemaTest t;
    auto r = t.run("fn main() {\n"
                   "    var b: bool = true;\n"
                   "    var x: i32 = -b;\n"
                   "}\n");
    CHECK(!r.ok, "Modern sema rejects unary minus on a boolean variable");
}

static void test_modern_assign_retype() {
    ModernSemaTest t;
    auto r = t.run("fn main() {\n"
                   "    var x: i32 = 0;\n"
                   "    x = true;\n"
                   "}\n");
    CHECK(!r.ok, "Modern sema rejects assignment with incompatible type");
    CHECK(r.hasMessage("expected 'i32'"), "the assignment diagnostic shows the target type");
    CHECK(r.hasMessage("has type 'bool'"), "the assignment diagnostic shows the source type");
}

static void test_modern_two_modules_at_once() {
    ModernSemaTest t;
    t.write("dep.zith", "pub fn dep_fn(): i32 { 42 }\n");
    t.write("sub/dep2.zith", "pub fn dep2_fn(): i32 { 7 }\n");
    auto r = t.run("from dep\n"
                   "from sub/dep2\n"
                   "fn main(): i32 {\n"
                   "    dep_fn() + dep2_fn()\n"
                   "}\n");
    CHECK(r.ok, "Modern sema resolves imports from two distinct sub-modules");
}

static void test_modern_let_binding_without_annotation() {
    ModernSemaTest initialized;
    auto r = initialized.run("fn main(): i32 {\n"
                             "    let x = 42;\n"
                             "    x\n"
                             "}\n");
    CHECK(r.ok, "Modern sema handles let binding without type annotation");

    ModernSemaTest uninitialized;
    auto u = uninitialized.run("fn main(): i32 {\n"
                               "    let x;\n"
                               "    x\n"
                               "}\n");
    CHECK(!u.ok, "reading a binding before an assignment cannot infer its type");
    CHECK(u.hasErrorCode(diagnostics::err::CannotInfer), "an untyped read reports CannotInfer");
    CHECK(u.hasMessage("'x'"), "the CannotInfer diagnostic names the binding");

    ModernSemaTest assigned;
    auto a = assigned.run("fn main(): i32 {\n"
                          "    let x;\n"
                          "    x = 5;\n"
                          "    x\n"
                          "}\n");
    CHECK(a.ok, "the first assignment infers an untyped binding");

    ModernSemaTest bool_assigned;
    auto b = bool_assigned.run("fn main(): bool {\n"
                               "    let x;\n"
                               "    x = true;\n"
                               "    x\n"
                               "}\n");
    CHECK(b.ok, "the first assignment infers the exact type from the right side");

    ModernSemaTest annotated;
    auto annotated_result = annotated.run("fn main(): i32 {\n"
                                          "    let x: i32;\n"
                                          "    x = 5;\n"
                                          "    x\n"
                                          "}\n");
    CHECK(annotated_result.ok, "an annotated binding keeps its explicit type");
}

static void test_modern_pointer_type_param() {
    ModernSemaTest t;
    auto r = t.run("extern fn puts(msg: *char)\n"
                   "fn main() {\n"
                   "    puts(\"hello\");\n"
                   "}\n");
    CHECK(r.ok, "Modern sema type-checks a pointer-type call through extern");
}

static void test_modern_type_alias_use() {
    ModernSemaTest t;
    auto r = t.run("alias MyInt = i32\n"
                   "fn main(): i32 {\n"
                   "    var x: MyInt = 3;\n"
                   "    x\n"
                   "}\n");
    CHECK(r.ok, "Modern sema resolves a transparent type alias in variables and returns");

    auto nominal = t.run("type MyInt = i32\n"
                         "fn main(): i32 {\n"
                         "    var x: MyInt = 3;\n"
                         "    x as i32\n"
                         "}\n");
    CHECK(!nominal.ok, "nominal 'type' does not accept the underlying type implicitly");
    CHECK(nominal.hasMessage("expected 'MyInt', has type 'i32'"),
          "nominal binding reports the wrapper type");

    auto wrapped = t.run("type MyInt = i32\n"
                         "fn main(): i32 {\n"
                         "    var x: MyInt = 3 as MyInt;\n"
                         "    x as i32\n"
                         "}\n");
    CHECK(wrapped.ok, "explicit nominal wrap/unwrap type-checks");
}

static void test_modern_grouped_struct_fields() {
    ModernSemaTest t;
    auto r = t.run("struct Point { [x, y, z]: i32, name: char = 'p' }\n"
                   "fn main(): i32 {\n"
                   "    var p: Point = Point { x: 1, y: 2, z: 3, name: 'p' };\n"
                   "    p.x + p.y + p.z\n"
                   "}\n");
    CHECK(r.ok, "grouped struct fields type-check as individual fields");
}

static void test_modern_static_method() {
    ModernSemaTest t;
    auto r = t.run("struct Point { x: i32 }\n"
                   "trait Sample {}\n"
                   "implement Point as Sample {\n"
                   "    fn foo(): i32 { 7 }\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    Point.foo()\n"
                   "}\n");
    CHECK(r.ok, "static method called on the owner type type-checks");
}

static void test_modern_imported_static_method() {
    ModernSemaTest t;
    t.write("lib.zith", "pub struct Box { value: i32 }\n"
                        "implement Box {\n"
                        "    fn make(v: i32): Box { Box { value: v } }\n"
                        "}\n");
    auto r = t.run("from lib\n"
                   "fn main(): i32 {\n"
                   "    let b = Box.make(42);\n"
                   "    b.value\n"
                   "}\n");
    CHECK(r.ok, "static method on an imported type type-checks");
}

static void test_modern_imported_receiver_method() {
    ModernSemaTest t;
    t.write("lib.zith", "pub struct Counter { value: i32 }\n"
                        "implement Counter {\n"
                        "    fn get(self): i32 { self->value }\n"
                        "}\n");
    auto r = t.run("from lib\n"
                   "fn main(): i32 {\n"
                   "    let c = Counter { value: 7 };\n"
                   "    c.get()\n"
                   "}\n");
    CHECK(r.ok, "receiver method on an imported type type-checks");
}

static void test_modern_imported_method_struct_literal_body() {
    ModernSemaTest t;
    t.write("lib.zith", "pub struct Box { value: i32 }\n"
                        "implement Box {\n"
                        "    fn make(v: i32): Box { Box { value: v } }\n"
                        "}\n");
    auto r = t.run("from lib\n"
                   "fn main(): i32 {\n"
                   "    let b = Box.make(42);\n"
                   "    b.value\n"
                   "}\n");
    CHECK(r.ok, "imported method body with a struct literal type-checks");
}

static void test_modern_self_resolves_to_owner() {
    ModernSemaTest t;
    auto r = t.run("struct Point { x: i32 }\n"
                   "trait Sample {}\n"
                   "implement Point as Sample {\n"
                   "    fn make(): Self { Point { x: 3 } }\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    Point.make().x\n"
                   "}\n");
    CHECK(r.ok, "'Self' in an implementation resolves to the implemented owner type");
}

static void test_modern_unary_minus_on_literal() {
    ModernSemaTest t;
    auto r = t.run("fn main(): i32 {\n"
                   "    -42\n"
                   "}\n");
    CHECK(r.ok, "Modern sema accepts unary minus on an integer literal");
}

static void test_modern_break_in_while() {
    ModernSemaTest t;
    auto r = t.run("fn main() {\n"
                   "    var i: i32 = 0;\n"
                   "    while (true) {\n"
                   "        if (i > 5) { break; }\n"
                   "        i = i + 1;\n"
                   "    }\n"
                   "}\n");
    CHECK(r.ok, "Modern sema accepts break statement inside a while loop");
}

static void test_modern_continue_in_while() {
    ModernSemaTest t;
    auto r = t.run("fn main() {\n"
                   "    var i: i32 = 0;\n"
                   "    while (i < 10) {\n"
                   "        i = i + 1;\n"
                   "        continue;\n"
                   "    }\n"
                   "}\n");
    CHECK(r.ok, "Modern sema accepts continue statement inside a while loop");
}

static void test_modern_empty_block_is_void() {
    ModernSemaTest t;
    auto r = t.run("fn main() {\n"
                   "    { }\n"
                   "}\n");
    CHECK(r.ok, "Modern sema accepts empty block in void context");
}

static void test_modern_if_else_unified_type() {
    ModernSemaTest t;
    auto r = t.run("fn main(): i32 {\n"
                   "    var flag: bool = true;\n"
                   "    if (flag) {\n"
                   "        1\n"
                   "    } else {\n"
                   "        2\n"
                   "    }\n"
                   "}\n");
    CHECK(r.ok, "Modern sema accepts if/else with same-type branches as function body");
}

static void test_modern_nested_block_type() {
    ModernSemaTest t;
    auto r = t.run("fn main(): i32 {\n"
                   "    {\n"
                   "        var x: i32 = 99;\n"
                   "        x\n"
                   "    }\n"
                   "}\n");
    CHECK(r.ok, "Modern sema infers the block type from the last expression");
}

static void test_modern_multi_param_call() {
    ModernSemaTest t;
    auto r = t.run("fn sum(a: i32, b: i32, c: i32): i32 { a + b + c }\n"
                   "fn main(): i32 {\n"
                   "    sum(1, 2, 3)\n"
                   "}\n");
    CHECK(r.ok, "Modern sema type-checks a three-argument call correctly");
}

static void test_modern_reassign_same_type() {
    ModernSemaTest t;
    auto r = t.run("fn main() {\n"
                   "    var x: i32 = 0;\n"
                   "    x = 42;\n"
                   "}\n");
    CHECK(r.ok, "Modern sema accepts assignment with matching types");
}

static void test_modern_extern_call_return() {
    ModernSemaTest t;
    auto r = t.run("extern fn strlen(msg: *char): i32\n"
                   "fn main(): i32 {\n"
                   "    strlen(\"hello\")\n"
                   "}\n");
    CHECK(r.ok, "Modern sema resolves an extern fn call returning a concrete type");
}

static void test_modern_numeric_cast_ok() {
    ModernSemaTest t;
    auto r = t.run("fn main(): f64 {\n"
                   "    var n: i32 = 3;\n"
                   "    let f: f64 = n as f64;\n"
                   "    return f;\n"
                   "}\n");
    CHECK(r.ok, "'as' between numeric types is accepted");
}

static void test_modern_implicit_numeric_conversion_rejected() {
    ModernSemaTest t;
    auto r = t.run("fn main(): f64 {\n"
                   "    var n: i32 = 3;\n"
                   "    let g: f64 = n;\n"
                   "    return g;\n"
                   "}\n");
    CHECK(!r.ok, "implicit numeric conversion between variables is rejected");
    CHECK(r.hasMessage("use 'as'"), "the diagnostic suggests an explicit 'as' cast");
}

static void test_modern_raw_opaque_cast_accepted() {
    ModernSemaTest t;
    auto r = t.run("fn thru(p: raw opaque): *i32 { return p as *i32; }\n"
                   "fn erase(q: *i32): raw opaque { return q as raw opaque; }\n"
                   "fn main(): i32 { return 0; }\n");
    CHECK(r.ok, "'as' converts between 'raw opaque' and a concrete pointer both ways");
}

static void test_modern_raw_opaque_to_integer_rejected() {
    ModernSemaTest t;
    auto r = t.run("fn f(p: raw opaque): i64 { return p as i64; }\n"
                   "fn main(): i32 { return 0; }\n");
    CHECK(!r.ok, "'raw opaque as i64' is rejected");
    CHECK(r.hasErrorCode(diagnostics::err::InvalidCast), "'raw opaque as i64' reports E3003");
}

static void test_modern_pointer_cast_rejected() {
    ModernSemaTest t;
    auto r = t.run("fn main(p: *i32): i32 {\n"
                   "    let s: *i32 = p as *i32;\n"
                   "    return 0;\n"
                   "}\n");
    CHECK(!r.ok, "'as' on pointer types is rejected");
    CHECK(r.hasMessage("numeric conversions and 'raw opaque' pointer conversions"),
          "pointer-to-pointer casts between concrete pointees report the cast restriction");
}

#ifdef ZITH_ENABLE_C_INTEROP
// A C pointer is `?*T`, so reinterpreting one must keep the nullability: `as ?*i32` is the
// accepted form and `as *i32` is a diagnostic.
static void test_modern_c_pointer_cast_to_nullable_accepted() {
    ModernSemaTest t;
    t.write("fixture.h", "void *fx_alloc(unsigned long size);\n");
    auto r = t.run("import \"fixture.h\"\n"
                   "fn main(): i32 {\n"
                   "    let x: ?*i32 = fx_alloc(64) as ?*i32;\n"
                   "    return 0;\n"
                   "}\n");
    CHECK(r.ok, "'?*void as ?*i32' is accepted and yields '?*i32'");
}

static void test_modern_c_pointer_cast_to_non_nullable_rejected() {
    ModernSemaTest t;
    t.write("fixture.h", "void *fx_alloc(unsigned long size);\n");
    auto r = t.run("import \"fixture.h\"\n"
                   "fn main(): i32 {\n"
                   "    let x: *i32 = fx_alloc(64) as *i32;\n"
                   "    return 0;\n"
                   "}\n");
    CHECK(!r.ok, "casting a nullable C pointer to '*i32' is rejected");
    CHECK(r.hasErrorCode(diagnostics::err::InvalidCast),
          "dropping nullability in a cast reports E3003");
    CHECK(r.hasMessage("use 'as ?*T'"), "the diagnostic points at the '?*T' form");
}

// Any pointer reaches a C `void*` parameter without a cast.
static void test_modern_pointer_coerces_to_c_void_pointer() {
    ModernSemaTest t;
    t.write("fixture.h", "void *fx_alloc(unsigned long size);\nvoid fx_free(void *ptr);\n");
    auto r = t.run("import \"fixture.h\"\n"
                   "fn main(): i32 {\n"
                   "    var local: i32 = 7;\n"
                   "    let borrowed: *i32 = &local;\n"
                   "    fx_free(borrowed);\n"
                   "    let owned: ?*i32 = fx_alloc(64) as ?*i32;\n"
                   "    fx_free(owned);\n"
                   "    fx_free(&local);\n"
                   "    return 0;\n"
                   "}\n");
    CHECK(r.ok, "'*T' and '?*T' both pass to a C 'void*' parameter without a cast");
}

// TEMPORARY companion of `allowsUncheckedNullablePointer`: until narrowing exists, a
// `?*T` still initializes a `*T`.
static void test_modern_nullable_c_pointer_initializes_non_optional() {
    ModernSemaTest t;
    t.write("fixture.h", "void *fx_alloc(unsigned long size);\n");
    auto r = t.run("import \"fixture.h\"\n"
                   "fn main(): i32 {\n"
                   "    let x: *i32 = fx_alloc(64) as ?*i32;\n"
                   "    return 0;\n"
                   "}\n");
    CHECK(r.ok, "'?*i32' still initializes a '*i32' binding");
}
#endif

// `raw opaque` is only a source of coercion, never a target: reading it back needs `as`.
static void test_modern_pointer_coercion_is_one_way() {
    ModernSemaTest t;
    auto r = t.run("fn take(p: *i32): i32 { return 0; }\n"
                   "fn main(q: raw opaque): i32 { return take(q); }\n");
    CHECK(!r.ok, "'raw opaque' does not coerce back to a concrete pointer");
}

// The wide `void*` coercion makes two overloads viable at once; the exact signature wins.
static void test_modern_overload_prefers_exact_pointer() {
    ModernSemaTest t;
    auto r = t.run("fn f(p: *i32): i32 { return 1; }\n"
                   "fn f(p: raw opaque): i32 { return 2; }\n"
                   "fn main(q: *i32): i32 { return f(q); }\n");
    CHECK(r.ok, "an exact pointer overload is preferred over the 'raw opaque' one");
    CHECK(!r.hasErrorCode(diagnostics::err::AmbiguousCall),
          "the wide 'void*' coercion does not make the call ambiguous");
}

static void test_modern_pointer_to_void_rejected() {
    ModernSemaTest t;
    auto r = t.run("fn main(p: *void): i32 {\n"
                   "    return 0;\n"
                   "}\n");
    CHECK(!r.ok, "'*void' is rejected");
    CHECK(r.hasMessage("pointer to 'void' is not allowed"),
          "'*void' reports the dedicated diagnostic");
}

static void test_modern_null_needs_optional_pointer() {
    ModernSemaTest init;
    auto r = init.run("fn main(): i32 {\n"
                      "    var q: *i32 = null;\n"
                      "    return 0;\n"
                      "}\n");
    CHECK(!r.ok, "'null' cannot initialize a non-optional pointer");
    CHECK(r.hasMessage("non-optional pointer"), "the initializer diagnostic mentions '?*T'");

    ModernSemaTest assign;
    auto a = assign.run("fn main(p: *i32): i32 {\n"
                        "    var q: *i32 = p;\n"
                        "    q = null;\n"
                        "    return 0;\n"
                        "}\n");
    CHECK(!a.ok, "'null' cannot be assigned to a non-optional pointer");
    CHECK(a.hasMessage("non-optional pointer"), "the assignment diagnostic mentions '?*T'");
}

static void test_modern_is_null_on_optional_pointer() {
    ModernSemaTest t;
    auto r = t.run("fn main(): bool {\n"
                   "    var r: ?*i32 = null;\n"
                   "    let empty: bool = r is null;\n"
                   "    return empty;\n"
                   "}\n");
    CHECK(r.ok, "'is null' on an optional pointer type-checks as bool");
}

static void test_modern_is_null_requires_optional() {
    ModernSemaTest t;
    auto r = t.run("fn main(): bool {\n"
                   "    var n: i32 = 1;\n"
                   "    return n is null;\n"
                   "}\n");
    CHECK(!r.ok, "'is null' on a non-optional operand is rejected");
    CHECK(r.hasMessage("requires an optional operand"),
          "'is null' reports the optional-operand requirement");
}

static void test_modern_radix_integer_literals() {
    ModernSemaTest t;
    auto r = t.run("fn main(): i32 {\n"
                   "    let h: i32 = 0xFF;\n"
                   "    let b: i32 = 0b101;\n"
                   "    let o: i32 = 0c17;\n"
                   "    return h + b + o;\n"
                   "}\n");
    CHECK(r.ok, "hex, binary and octal literals infer as integers");
    CHECK(r.errorCount == 0, "radix literals produce no diagnostics");
}

static void test_modern_integer_literal_overflow_reported() {
    ModernSemaTest t;
    auto r = t.run("fn main(): i32 {\n"
                   "    return 0xFFFFFFFFFFFFFFFFF;\n"
                   "}\n");
    CHECK(!r.ok, "an integer literal wider than 64 bits is rejected");
    CHECK(r.hasMessage("does not fit in 64 bits"),
          "the oversized literal diagnostic names the 64-bit limit");
}

static void test_modern_pointer_compared_to_integer_literal_fails() {
    // Both spellings must be rejected: `0x0` used to slip through because the radix form
    // was not recognised as an integer and inferred as `error`, which unifies with anything.
    for (const char *literal : {"0", "0x0"}) {
        ModernSemaTest t;
        auto r = t.run(std::string("fn main(): bool {\n"
                                   "    var p: ?*i32 = null;\n"
                                   "    return p != ") +
                       literal +
                       ";\n"
                       "}\n");
        CHECK(!r.ok, "comparing a pointer against an integer literal is rejected");
        CHECK(r.hasMessage("comparison between incompatible types"),
              "the pointer/integer comparison reports incompatible types");
    }
}

static void test_modern_loop_body_infers_locals() {
    ModernSemaTest while_loop;
    auto w = while_loop.run("fn main(): i32 {\n"
                            "    var b: i32 = 0;\n"
                            "    while (b < 3) {\n"
                            "        var t: i32 = b + 1;\n"
                            "        b = t;\n"
                            "    }\n"
                            "    return b;\n"
                            "}\n");
    CHECK(w.errorCount == 0, "the body of a 'while' loop infers its own locals");

    ModernSemaTest for_loop;
    auto f = for_loop.run("fn main(): i32 {\n"
                          "    var b: i32 = 0;\n"
                          "    for (b < 3) {\n"
                          "        var t: i32 = b + 1;\n"
                          "        b = t;\n"
                          "    }\n"
                          "    return b;\n"
                          "}\n");
    CHECK(f.ok, "the body of a 'for' loop infers its own locals");
}

static void test_modern_for_flat_and_parenthesized_forms() {
    ModernSemaTest flat;
    auto a = flat.run("fn main(): i32 {\n"
                      "    var total: i32 = 0;\n"
                      "    for (var i: i32 = 0, i < 5, i = i + 1) {\n"
                      "        total = total + i;\n"
                      "    }\n"
                      "    return total;\n"
                      "}\n");
    CHECK(a.ok, "flat 3-clause for with var init type-checks");

    ModernSemaTest flat_omissions;
    auto b = flat_omissions.run("fn main(): i32 {\n"
                                "    var n: i32 = 3;\n"
                                "    for (, n > 0, n = n - 1) {\n"
                                "    }\n"
                                "    return n;\n"
                                "}\n");
    CHECK(b.ok, "flat 3-clause for with omitted init comma type-checks");

    ModernSemaTest grouped;
    auto c = grouped.run("fn main(): i32 {\n"
                         "    var total: i32 = 0;\n"
                         "    var i: i32 = 0;\n"
                         "    for (i = 0), (i < 5), (i = i + 1) {\n"
                         "        total = total + i;\n"
                         "    }\n"
                         "    return total;\n"
                         "}\n");
    CHECK(c.ok, "parenthesized 3-clause for type-checks");

    ModernSemaTest missing_step;
    auto d = missing_step.run("fn main(): i32 {\n"
                              "    var i: i32 = 0;\n"
                              "    var total: i32 = 0;\n"
                              "    for (i = 0, i < 5,) {\n"
                              "        total = total + i;\n"
                              "        i = i + 1;\n"
                              "    }\n"
                              "    return total;\n"
                              "}\n");
    CHECK(d.ok, "flat 3-clause for with trailing comma omitted step type-checks");

    ModernSemaTest bad_sep;
    auto e = bad_sep.run("fn main(): i32 {\n"
                         "    var i: i32 = 0;\n"
                         "    for (i = 0, i < 5, i = i + 1) {\n"
                         "    }\n"
                         "    return i;\n"
                         "}\n");
    CHECK(e.ok, "flat for separator is still comma-based");
}

static void test_modern_for_three_clause() {
    ModernSemaTest ok;
    auto r = ok.run("fn sum_to(n: i32): i32 {\n"
                    "    var total: i32 = 0;\n"
                    "    for (var i: i32 = 0, i < n, i = i + 1) {\n"
                    "        total = total + i;\n"
                    "    }\n"
                    "    return total;\n"
                    "}\n"
                    "fn main(): i32 {\n"
                    "    return sum_to(5);\n"
                    "}\n");
    CHECK(r.ok, "3-clause for with a var init type-checks");

    ModernSemaTest expr_init;
    auto e = expr_init.run("fn main(): i32 {\n"
                           "    var i: i32 = 0;\n"
                           "    var total: i32 = 0;\n"
                           "    for (i = 0, i < 5, i = i + 1) {\n"
                           "        total = total + i;\n"
                           "    }\n"
                           "    return total;\n"
                           "}\n");
    CHECK(e.ok, "3-clause for with an expression init type-checks");

    ModernSemaTest empty_clauses;
    auto c = empty_clauses.run("fn main(): i32 {\n"
                               "    var n: i32 = 3;\n"
                               "    for (, n > 0, n = n - 1) {\n"
                               "    }\n"
                               "    return n;\n"
                               "}\n");
    CHECK(c.ok, "3-clause for with an omitted init type-checks");

    ModernSemaTest bad_cond;
    auto b = bad_cond.run("fn main(): i32 {\n"
                          "    var i: i32 = 0;\n"
                          "    for (var j: i32 = 0, 42, j = j + 1) {\n"
                          "        i = i + 1;\n"
                          "    }\n"
                          "    return i;\n"
                          "}\n");
    CHECK(!b.ok, "a non-boolean for condition is rejected");
    CHECK(b.hasMessage("loop condition must be boolean"),
          "the non-boolean 3-clause condition reports a type mismatch");
}

static void test_modern_for_in_iterators() {
    ModernSemaTest ok;
    auto a = ok.run("struct End {}\n"
                    "union RangeStep { i32, End }\n"
                    "struct Range {\n"
                    "    current: i32,\n"
                    "    limit: i32,\n"
                    "    fn next(var self): RangeStep {\n"
                    "        if (self->current >= self->limit) {\n"
                    "            return RangeStep { End {} };\n"
                    "        }\n"
                    "        let value = RangeStep { self->current };\n"
                    "        self->current = self->current + 1;\n"
                    "        return value;\n"
                    "    }\n"
                    "}\n"
                    "fn main(): i32 {\n"
                    "    var total: i32 = 0;\n"
                    "    let r: Range = Range { current: 0, limit: 5 };\n"
                    "    for (x in r) {\n"
                    "        total = total + x;\n"
                    "    }\n"
                    "    return total;\n"
                    "}\n");
    CHECK(a.ok, "for-in with next returning a tagged union with End type-checks");

    ModernSemaTest typed_binding;
    auto b = typed_binding.run("struct End {}\n"
                               "union RangeStep { i32, End }\n"
                               "struct Range {\n"
                               "    current: i32,\n"
                               "    limit: i32,\n"
                               "    fn next(var self): RangeStep {\n"
                               "        if (self->current >= self->limit) {\n"
                               "            return RangeStep { End {} };\n"
                               "        }\n"
                               "        let value = RangeStep { self->current };\n"
                               "        self->current = self->current + 1;\n"
                               "        return value;\n"
                               "    }\n"
                               "}\n"
                               "fn main(): i32 {\n"
                               "    var total: i32 = 0;\n"
                               "    let r: Range = Range { current: 0, limit: 2 };\n"
                               "    for (var x: i32 in r) {\n"
                               "        total = total + x;\n"
                               "    }\n"
                               "    return total;\n"
                               "}\n");
    CHECK(b.ok, "for-in with an annotated loop variable type-checks");

    ModernSemaTest missing_method;
    auto c = missing_method.run("struct Empty {}\n"
                                "fn main(): i32 {\n"
                                "    var total: i32 = 0;\n"
                                "    let e: Empty = Empty {};\n"
                                "    for (x in e) {\n"
                                "        total = total + 1;\n"
                                "    }\n"
                                "    return total;\n"
                                "}\n");
    CHECK(!c.ok, "for-in rejects an iterable without next");
    CHECK(c.hasMessage("is missing a 'next' method"), "reports the missing next method");

    ModernSemaTest bad_value;
    auto d = bad_value.run("struct End {}\n"
                           "union RangeStep { i32, End }\n"
                           "struct Range {\n"
                           "    current: i32,\n"
                           "    limit: i32,\n"
                           "    fn next(self): RangeStep { return RangeStep { 1 }; }\n"
                           "}\n"
                           "fn main(): i32 {\n"
                           "    var total: i32 = 0;\n"
                           "    let r: Range = Range { current: 0, limit: 2 };\n"
                           "    for (var x: bool in r) {\n"
                           "        total = total + 1;\n"
                           "    }\n"
                           "    return total;\n"
                           "}\n");
    CHECK(!d.ok, "for-in rejects a loop variable annotation that does not match the element");
    CHECK(d.hasMessage("iterator element type does not match loop variable annotation"),
          "reports the annotation mismatch");

    ModernSemaTest non_union;
    auto e = non_union.run("struct End {}\n"
                           "struct Range {\n"
                           "    fn next(self): i32 { return 1; }\n"
                           "}\n"
                           "fn main(): i32 {\n"
                           "    let r: Range = Range {};\n"
                           "    for (x in r) { }\n"
                           "    return 0;\n"
                           "}\n");
    CHECK(!e.ok, "for-in rejects next that does not return a tagged union");
    CHECK(e.hasMessage("must return a tagged union with one value member and 'End'"),
          "reports the non-union next result");

    ModernSemaTest no_end;
    auto f = no_end.run("union Step { i32, bool }\n"
                        "struct Range {\n"
                        "    fn next(self): Step { return Step { 1 }; }\n"
                        "}\n"
                        "fn main(): i32 {\n"
                        "    let r: Range = Range {};\n"
                        "    for (x in r) { }\n"
                        "    return 0;\n"
                        "}\n");
    CHECK(!f.ok, "for-in rejects a union without an End member");
    CHECK(f.hasMessage("must contain a value member and 'End'"), "reports the missing End member");

    ModernSemaTest three_members;
    auto g = three_members.run("struct End {}\n"
                               "union Step { i32, bool, End }\n"
                               "struct Range {\n"
                               "    fn next(self): Step { return Step { 1 }; }\n"
                               "}\n"
                               "fn main(): i32 {\n"
                               "    let r: Range = Range {};\n"
                               "    for (x in r) { }\n"
                               "    return 0;\n"
                               "}\n");
    CHECK(!g.ok, "for-in rejects an iterator union with more than two members");
    CHECK(g.hasMessage("must have exactly two members: a value and 'End'"),
          "reports the three-member union");
}

static void test_modern_generic_params() {
    ModernSemaTest fn_decl;
    auto f = fn_decl.run("fn identity<T>(x: T): T {\n"
                         "    return x;\n"
                         "}\n"
                         "fn main(): i32 {\n"
                         "    return 0;\n"
                         "}\n");
    CHECK(f.ok, "a generic fn declaration with a generic body type-checks");

    ModernSemaTest struct_decl;
    auto s = struct_decl.run("struct Pair<T> {\n"
                             "    left: T,\n"
                             "    right: T\n"
                             "}\n"
                             "fn main(): i32 {\n"
                             "    return 0;\n"
                             "}\n");
    CHECK(s.ok, "a generic struct declaration type-checks");

    ModernSemaTest alias_decl;
    auto a = alias_decl.run("alias Box<T> = i32;\n"
                            "fn main(): i32 {\n"
                            "    return 0;\n"
                            "}\n");
    CHECK(a.ok, "a generic alias declaration type-checks");

    ModernSemaTest instantiation;
    auto i = instantiation.run("fn identity<T>(x: T): T {\n"
                               "    return x;\n"
                               "}\n"
                               "fn main(): i32 {\n"
                               "    return identity<i32>(42);\n"
                               "}\n");
    CHECK(i.ok, "an explicitly typed generic fn call type-checks");

    ModernSemaTest inferred_call;
    auto c = inferred_call.run("fn identity<T>(x: T): T {\n"
                               "    return x;\n"
                               "}\n"
                               "fn main(): i32 {\n"
                               "    return identity(1);\n"
                               "}\n");
    CHECK(c.ok, "a generic fn call with an inferable argument type-checks");

    ModernSemaTest arity;
    auto ar = arity.run("fn identity<T>(x: T): T {\n"
                        "    return x;\n"
                        "}\n"
                        "fn main(): i32 {\n"
                        "    identity<i32, i64>(42);\n"
                        "    return 0;\n"
                        "}\n");
    CHECK(!ar.ok, "a generic fn call with too many type arguments is rejected");
    CHECK(ar.hasErrorCode(diagnostics::err::GenericArity), "the wrong generic arity reports E3010");
    CHECK(ar.hasMessage("wrong generic argument count"),
          "the generic arity diagnostic is actionable");

    ModernSemaTest cannot_infer;
    auto n = cannot_infer.run("fn needs<T>(x: i32): T {\n"
                              "    return 0 as T;\n"
                              "}\n"
                              "fn main(): i32 {\n"
                              "    needs(1);\n"
                              "    return 0;\n"
                              "}\n");
    CHECK(!n.ok, "a generic fn call with no inferable parameter is rejected");
    CHECK(n.hasErrorCode(diagnostics::err::GenericCannotInfer),
          "the un-inferable generic call reports E3011");
    CHECK(n.hasMessage("cannot infer generic argument"),
          "the inference diagnostic asks for explicit type arguments");

    ModernSemaTest struct_literal;
    auto sl = struct_literal.run("struct Pair<T, U> { left: T, right: U }\n"
                                 "fn main(): i32 {\n"
                                 "    let p: Pair<i32, f64> = "
                                 "Pair<i32, f64>{ left: 1, right: 2.5 };\n"
                                 "    return p.left;\n"
                                 "}\n");
    CHECK(sl.ok, "a generic struct literal type-checks against its concrete instance");
}

static void test_modern_generic_struct_literal_inference() {
    ModernSemaTest named_inferred;
    auto named = named_inferred.run("struct Pair<T, U> { left: T, right: U }\n"
                                    "fn main(): i32 {\n"
                                    "    let p = Pair{ left: 1, right: \"hello\" };\n"
                                    "    return p.left;\n"
                                    "}\n");
    CHECK(named.ok, "named generic struct literal fields deduce type arguments");

    ModernSemaTest positional_inferred;
    auto positional = positional_inferred.run("struct Pair<T, U> { left: T, right: U }\n"
                                              "fn main(): i32 {\n"
                                              "    let p = Pair{ 1, \"hello\" };\n"
                                              "    return p.left;\n"
                                              "}\n");
    CHECK(positional.ok, "positional generic struct literal fields deduce type arguments");

    ModernSemaTest explicit_still_works;
    auto explicit_test = explicit_still_works.run("struct Pair<T, U> { left: T, right: U }\n"
                                                  "fn main(): i32 {\n"
                                                  "    let p: Pair<i32, f64> = "
                                                  "Pair<i32, f64>{ left: 1, right: 2.5 };\n"
                                                  "    return p.left;\n"
                                                  "}\n");
    CHECK(explicit_test.ok,
          "explicit generic struct literal arguments continue to override inference");

    ModernSemaTest cannot_infer;
    auto cannot = cannot_infer.run("struct Only<T, U> { left: T }\n"
                                   "fn main(): i32 {\n"
                                   "    let p = Only{ left: 1 };\n"
                                   "    return p.left;\n"
                                   "}\n");
    CHECK(!cannot.ok, "a generic struct literal cannot leave a fieldless parameter unresolved");
    CHECK(cannot.hasErrorCode(diagnostics::err::GenericStructInfer),
          "the unresolved generic struct literal reports E3013");

    ModernSemaTest coercion_mismatch;
    auto coercion = coercion_mismatch.run("struct Same<T> { left: T, right: T }\n"
                                          "fn main(): i32 {\n"
                                          "    let p = Same{ left: 1, right: true };\n"
                                          "    return p.left;\n"
                                          "}\n");
    CHECK(!coercion.ok, "conflicting generic struct literal fields are rejected");
    CHECK(coercion.hasErrorCode(diagnostics::err::TypeMismatch),
          "conflicting provided fields report TypeMismatch");
    CHECK(!coercion.hasErrorCode(diagnostics::err::GenericStructInfer),
          "conflicting provided fields do not report E3013");

    ModernSemaTest explicit_arity;
    auto arity = explicit_arity.run("struct Pair<T, U> { left: T, right: U }\n"
                                    "fn main(): i32 {\n"
                                    "    let p = Pair<i32>{ left: 1, right: 2 };\n"
                                    "    return p.left;\n"
                                    "}\n");
    CHECK(!arity.ok, "wrong explicit generic struct literal arity is rejected");
    CHECK(arity.hasErrorCode(diagnostics::err::GenericArity),
          "wrong explicit generic struct literal arity reports E3010");
}

static void test_modern_array_literal() {
    ModernSemaTest t;
    auto r = t.run("fn sum(arr: [4]i32): i32 {\n"
                   "    return raw arr[0] + raw arr[1] + raw arr[2] + raw arr[3];\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    var xs = [1, 2, 3, 4];\n"
                   "    return sum(xs);\n"
                   "}\n");
    CHECK(r.ok, "array literal with matching elements type-checks and passes to an array param");
}

static void test_modern_array_literal_mismatch() {
    ModernSemaTest t;
    auto r = t.run("fn main() {\n"
                   "    var xs = [1, \"a\"];\n"
                   "}\n");
    CHECK(!r.ok, "array literal with mismatched element types fails");
    CHECK(r.hasErrorCode(diagnostics::err::TypeMismatch), "Reports TypeMismatch (3001)");
}

static void test_modern_array_literal_empty() {
    ModernSemaTest t;
    auto r = t.run("fn main() {\n"
                   "    var xs = [];\n"
                   "}\n");
    CHECK(r.ok, "empty array literal defaults to i32[0]");
}

static void test_modern_char_literal_and_escapes() {
    ModernSemaTest t;
    auto r = t.run("fn main(): i32 {\n"
                   "    let c: char = 'B';\n"
                   "    let nl: char = '\\n';\n"
                   "    let cast: char = 65 as char;\n"
                   "    return 0;\n"
                   "}\n");
    CHECK(r.ok, "char literals, escapes and char casts type-check and lower");
    CHECK(!r.hasErrorCode(diagnostics::err::CannotInfer),
          "char literal is typed as char rather than error");

    ModernSemaTest bad;
    auto b = bad.run("fn main() {\n"
                     "    let c: char = '\\q';\n"
                     "}\n",
                     session::Stage::HirLowered);
    CHECK(!b.ok, "an unknown char escape is rejected");
    CHECK(b.hasErrorCode(diagnostics::err::InvalidEscape),
          "unknown char escape reports E0001 InvalidEscape");

    ModernSemaTest bad_string;
    auto s = bad_string.run("fn main() {\n"
                            "    let p: *char = \"\\q\";\n"
                            "}\n",
                            session::Stage::HirLowered);
    CHECK(!s.ok, "an unknown string escape is rejected");
    CHECK(s.hasErrorCode(diagnostics::err::InvalidEscape),
          "unknown string escape reports E0001 InvalidEscape");
}

static void test_modern_struct_method_decl() {
    ModernSemaTest t;
    auto r = t.run("struct Counter {\n"
                   "    value: i32,\n"
                   "    fn bump(self, by: i32): i32 {\n"
                   "        return self->value + by;\n"
                   "    }\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    let c: Counter = Counter { value: 5 };\n"
                   "    return c.bump(3);\n"
                   "}\n");
    CHECK(r.ok, "a method declared in a struct body type-checks and is callable");
}

static void test_modern_parameter_mutability() {
    ModernSemaTest t;
    auto immutable_param = t.run("struct P { x: i32 }\n"
                                 "fn set(p: P) { p.x = 1; }\n"
                                 "fn main() {}\n");
    CHECK(!immutable_param.ok, "a plain struct parameter cannot be written through");
    CHECK(immutable_param.hasMessage("cannot write through immutable binding 'p'"),
          "plain parameters are immutable by default");

    auto var_param = t.run("struct P { x: i32 }\n"
                           "fn set(var p: P) { p.x = 1; }\n"
                           "fn main() {}\n");
    CHECK(var_param.ok, "`var p` permits direct field assignment through the parameter");

    auto let_param = t.run("struct P { x: i32 }\n"
                           "fn set(let p: P) { p.x = 1; }\n"
                           "fn main() {}\n");
    CHECK(!let_param.ok, "`let p` keeps the parameter immutable");
    CHECK(let_param.hasMessage("cannot write through immutable binding 'p'"),
          "explicit `let` is the same as the default");
}

static void test_modern_self_mutability_and_auto_deref() {
    ModernSemaTest ok;
    auto r = ok.run("struct Counter {\n"
                    "    value: i32,\n"
                    "    fn get(self): i32 { return self.value; }\n"
                    "    fn bump(var self): i32 { self.value += 1; return self.value; }\n"
                    "}\n"
                    "fn main(): i32 {\n"
                    "    let c: Counter = Counter { value: 2 };\n"
                    "    var total: i32 = c.get();\n"
                    "    var d: Counter = Counter { value: 3 };\n"
                    "    total += d.bump();\n"
                    "    return total;\n"
                    "}\n");
    CHECK(r.ok, "`self.field` auto-derefs and `var self` permits in-place mutation");

    ModernSemaTest read_only_self;
    auto b = read_only_self.run("struct Counter { value: i32 }\n"
                                "fn bad(self) { self.value = 3; }\n"
                                "fn main() {}\n");
    CHECK(!b.ok, "a plain `self` receiver cannot write through its fields");
    CHECK(b.hasMessage("cannot write through immutable binding 'self'"),
          "bare self is read-only even though it lowers to *Owner");
}

static void test_modern_logical_move_for_method_receivers() {
    ModernSemaTest mutating;
    auto r = mutating.run("struct Counter {\n"
                          "    value: i32,\n"
                          "    fn bump(var self): i32 { self.value += 1; return self.value; }\n"
                          "}\n"
                          "fn main(): i32 {\n"
                          "    var c: Counter = Counter { value: 1 };\n"
                          "    let r1: i32 = c.bump();\n"
                          "    return c.value;\n"
                          "}\n");
    CHECK(!r.ok, "using a receiver after a `var self` call is rejected");
    CHECK(r.hasErrorCode(diagnostics::err::UseAfterMove),
          "the read after mutation reports E4001 UseAfterMove");
    CHECK(r.hasMessage("cannot use 'c' after it was moved"), "the move reports on the dead name");

    ModernSemaTest original_by_value;
    auto s = original_by_value.run("struct Counter {\n"
                                   "    value: i32,\n"
                                   "    fn get(self): i32 { return self.value; }\n"
                                   "}\n"
                                   "fn main(): i32 {\n"
                                   "    let c: Counter = Counter { value: 1 };\n"
                                   "    return c.get() + c.value;\n"
                                   "}\n");
    CHECK(!s.ok, "an implicit-by-value self call also invalidates the caller binding");
    CHECK(s.hasErrorCode(diagnostics::err::UseAfterMove),
          "getting `c.value` after `c.get()` is a logical use after move");
}

static void test_modern_implement_block_method() {
    ModernSemaTest t;
    auto r = t.run("struct Point {\n"
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
    CHECK(r.ok, "a method declared in an implement block type-checks and is callable");
}

static void test_modern_method_call_arity_mismatch() {
    ModernSemaTest t;
    auto r = t.run("struct Counter {\n"
                   "    value: i32,\n"
                   "    fn bump(self, by: i32): i32 {\n"
                   "        return self->value + by;\n"
                   "    }\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    let c: Counter = Counter { value: 5 };\n"
                   "    return c.bump();\n"
                   "}\n");
    CHECK(!r.ok, "a method call with too few arguments is rejected");
    CHECK(r.hasMessage("method call arity mismatch"), "reports a method arity mismatch");
}

static void test_modern_method_call_arg_type_mismatch() {
    ModernSemaTest t;
    auto r = t.run("struct Counter {\n"
                   "    value: i32,\n"
                   "    fn bump(self, by: i32): i32 {\n"
                   "        return self->value + by;\n"
                   "    }\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    let c: Counter = Counter { value: 5 };\n"
                   "    return c.bump(\"x\");\n"
                   "}\n");
    CHECK(!r.ok, "a method call with a wrongly typed argument is rejected");
    CHECK(r.hasMessage("method call argument type mismatch"),
          "reports a method argument type mismatch");
}

static void test_modern_unknown_method_still_reports_field() {
    ModernSemaTest t;
    auto r = t.run("struct Counter {\n"
                   "    value: i32\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    let c: Counter = Counter { value: 5 };\n"
                   "    return c.missing();\n"
                   "}\n");
    CHECK(!r.ok, "calling a non-existent method is rejected");
    CHECK(r.hasErrorCode(diagnostics::err::NoMember),
          "an unresolved method falls back to a field diagnostic (E2006)");
}

static void test_modern_function_value_and_call() {
    ModernSemaTest t;
    t.write("runtime.zith", "pub extern fn apply(f: fn(i32): i32, x: i32): i32\n"
                            "pub extern fn double(x: i32): i32\n");
    auto r = t.run("from runtime\n"
                   "fn main(): i32 {\n"
                   "    var f: fn(i32): i32 = double;\n"
                   "    return apply(f, 7);\n"
                   "}\n");
    CHECK(r.ok, "a function value binds to a function-typed local and is passed by value");
}

static void test_modern_function_value_type_mismatch() {
    ModernSemaTest t;
    auto r = t.run("extern fn double(x: i32): i32\n"
                   "fn main() {\n"
                   "    var f: fn(i32): i32 = 5;\n"
                   "}\n");
    CHECK(!r.ok, "assigning a non-function to a function type is rejected");
    CHECK(r.hasErrorCode(diagnostics::err::TypeMismatch),
          "function value mismatch reports TypeMismatch (3001)");
}

static void test_modern_array_to_slice_coercion() {
    ModernSemaTest t;
    auto r = t.run("fn read_first(s: []i32): i32 { raw s[0] }\n"
                   "fn full_view(a: [4]i32): []i32 { a }\n"
                   "fn main(): i32 {\n"
                   "    let values: [4]i32 = [1, 2, 3, 4];\n"
                   "    return read_first(values) + raw full_view(values)[3];\n"
                   "}\n",
                   session::Stage::TypeChecked);
    CHECK(r.ok, "arrays coerce to slices for bindings, parameters, and returns");
}

static void test_modern_array_to_slice_element_mismatch() {
    ModernSemaTest t;
    auto r = t.run("fn read_first(s: []u32): i32 { 0 }\n"
                   "fn main(): i32 {\n"
                   "    let values: [3]i32 = [1, 2, 3];\n"
                   "    read_first(values);\n"
                   "    0\n"
                   "}\n");
    CHECK(!r.ok, "an array does not coerce to a slice with a different element type");
    CHECK(r.hasErrorCode(diagnostics::err::NoMatchingFn),
          "array-to-slice element mismatch rejects the call");
}

static void test_modern_slice_range_sema() {
    ModernSemaTest t;
    auto ok = t.run("fn main(): i32 {\n"
                    "    var values: [3]i32 = [10, 20, 30];\n"
                    "    let s: ?[]i32 = values[1..3];\n"
                    "    0\n"
                    "}\n");
    CHECK(ok.ok, "a statically valid array slice is accepted by sema");

    auto reversed = t.run("fn main(): ?[]i32 {\n"
                          "    var values: [3]i32 = [10, 20, 30];\n"
                          "    return values[1..0];\n"
                          "}\n");
    CHECK(!reversed.ok, "reversed static slice bounds are rejected");
    CHECK(reversed.hasErrorCode(diagnostics::err::TypeMismatch),
          "reversed static bounds report TypeMismatch (3001)");

    auto negative = t.run("fn main(): ?[]i32 {\n"
                          "    var values: [3]i32 = [10, 20, 30];\n"
                          "    return values[-1..2];\n"
                          "}\n");
    CHECK(!negative.ok, "negative static slice bounds are rejected");
    CHECK(negative.hasErrorCode(diagnostics::err::TypeMismatch),
          "negative static bounds report TypeMismatch (3001)");

    auto oversized = t.run("fn main(): ?[]i32 {\n"
                           "    var values: [3]i32 = [10, 20, 30];\n"
                           "    return values[0..N];\n"
                           "}\n");
    CHECK(!oversized.ok, "an oversized static slice bound is rejected");
}

static void test_modern_raw_slice_and_index_sema() {
    ModernSemaTest t;
    auto ok = t.run("fn main(): i32 {\n"
                    "    var values: [3]i32 = [10, 20, 30];\n"
                    "    let s: []i32 = raw values[1..3];\n"
                    "    return raw s[0];\n"
                    "}\n");
    CHECK(ok.ok, "raw index and slice expressions type-check without an optional result");

    auto checked = t.run("fn main(): ?i32 {\n"
                         "    var values: [3]i32 = [10, 20, 30];\n"
                         "    return values[1];\n"
                         "}\n");
    CHECK(checked.ok, "registry slice/index expressions type-check with an optional result");

    auto dynamic_slice = t.run("fn read(s: []i32, i: i32): ?i32 { s[i] }\n"
                               "fn main(): i32 { 0 }\n");
    CHECK(dynamic_slice.ok, "dynamic slice indexes type-check as optional");

    auto static_oob = t.run("fn main(): ?i32 {\n"
                            "    var values: [3]i32 = [10, 20, 30];\n"
                            "    return values[3];\n"
                            "}\n");
    CHECK(!static_oob.ok, "static out-of-bounds array index is rejected");

    auto static_negative = t.run("fn main(): ?i32 {\n"
                                 "    var values: [3]i32 = [10, 20, 30];\n"
                                 "    return values[-1];\n"
                                 "}\n");
    CHECK(!static_negative.ok, "static negative array index is rejected");

    auto unchecked = t.run("fn main(): []i32 {\n"
                           "    var values: [3]i32 = [10, 20, 30];\n"
                           "    return raw values[-1..2];\n"
                           "}\n");
    CHECK(unchecked.ok,
          "raw array slicing skips static bounds rejection and returns the slice type");

    auto malformed = t.run("fn main(): i32 {\n"
                           "    var values: [3]i32 = [10, 20, 30];\n"
                           "    return raw values + 1;\n"
                           "}\n");
    CHECK(!malformed.ok, "raw prefix is rejected on a non-index/slice expression");
}

static void test_modern_zith_bindings() {
    ModernSemaTest t;

    auto valid = t.run("const GLOBAL: i32 = 3;\n"
                       "struct P { const X: i32 = 1, y: i32 }\n"
                       "fn main(): i32 {\n"
                       "    let a: i32 = 1;\n"
                       "    var b: i32 = 2;\n"
                       "    const LOCAL: i32 = 4;\n"
                       "    var n: i32;\n"
                       "    var p: P = P { y: 2 };\n"
                       "    n = GLOBAL + a + b + LOCAL + p.X + n;\n"
                       "    return n;\n"
                       "}\n");
    CHECK(valid.ok, "Zith-- let/var/const, scalar default and const fields type-check");

    auto const_without_init = t.run("const C: i32;\n"
                                    "fn main(): i32 { return 0; }\n");
    CHECK(!const_without_init.ok, "top-level const without initializer is rejected");
    CHECK(const_without_init.hasErrorCode(diagnostics::err::UnsupportedSyntax),
          "top-level const without initializer reports UnsupportedSyntax");
    CHECK(const_without_init.hasMessage("const declaration requires an initializer"),
          "top-level const diagnostic names the missing initializer");

    auto local_const_without_init = t.run("fn main(): i32 {\n"
                                          "    const C: i32;\n"
                                          "    return 0;\n"
                                          "}\n");
    CHECK(!local_const_without_init.ok, "local const without initializer is rejected");
    CHECK(local_const_without_init.hasMessage("const binding requires an initializer"),
          "local const diagnostic names the missing initializer");

    auto non_constant = t.run("fn foo(): i32 { 1 }\n"
                              "const C: i32 = foo();\n"
                              "fn main(): i32 { return C; }\n");
    CHECK(!non_constant.ok, "const initializer must be a constant expression");
    CHECK(non_constant.hasMessage("const initializer must be a constant expression"),
          "non-constant const initializer reports the Zith-- reason");

    auto non_trivial_let = t.run("fn main(): i32 {\n"
                                 "    let p: *i32;\n"
                                 "    return 0;\n"
                                 "}\n");
    CHECK(!non_trivial_let.ok, "non-trivial let without initializer is rejected");
    CHECK(non_trivial_let.hasMessage("non-trivial let/var binding requires an initializer"),
          "non-trivial let diagnostic names the missing initializer");

    auto non_trivial_var = t.run("fn main(): []i32 {\n"
                                 "    var s: []i32;\n"
                                 "    return s;\n"
                                 "}\n");
    CHECK(!non_trivial_var.ok, "non-trivial var without initializer is rejected");
    CHECK(non_trivial_var.hasErrorCode(diagnostics::err::UnsupportedSyntax),
          "non-trivial var reports UnsupportedSyntax");

    auto scalar_without_init = t.run("fn main(): i32 {\n"
                                     "    var n: i32;\n"
                                     "    n = 1;\n"
                                     "    return n;\n"
                                     "}\n");
    CHECK(scalar_without_init.ok, "scalar var without initializer remains accepted");

    auto assign_let = t.run("fn main(): i32 {\n"
                            "    let n: i32 = 1;\n"
                            "    n = 2;\n"
                            "    return n;\n"
                            "}\n");
    CHECK(!assign_let.ok, "assigning to let is rejected");
    CHECK(assign_let.hasErrorCode(diagnostics::err::UnsupportedSyntax),
          "assigning to let reports UnsupportedSyntax");
    CHECK(assign_let.hasMessage("cannot assign to an immutable let/const binding"),
          "immutable assignment diagnostic names the binding");

    auto assign_const = t.run("const C: i32 = 1;\n"
                              "fn main(): i32 {\n"
                              "    C = 2;\n"
                              "    return C;\n"
                              "}\n");
    CHECK(!assign_const.ok, "assigning to a const global is rejected");
    CHECK(assign_const.hasMessage("cannot assign to a const global"),
          "const global assignment diagnostic names the global");

    auto assign_const_field = t.run("struct P { const X: i32 = 1, y: i32 }\n"
                                    "fn main(): i32 {\n"
                                    "    var p: P = P { y: 2 };\n"
                                    "    p.X = 3;\n"
                                    "    return p.X;\n"
                                    "}\n");
    CHECK(!assign_const_field.ok, "assigning to a const struct field is rejected");
    CHECK(assign_const_field.hasMessage("cannot assign to a const struct field"),
          "const field assignment diagnostic names the field");
}

static void test_modern_mutability_propagates_to_struct_fields() {
    ModernSemaTest t;

    auto var_root = t.run("struct Inner { x: i32 }\n"
                          "struct Outer { inner: Inner }\n"
                          "fn main(): i32 {\n"
                          "    var p: Outer = Outer { inner: Inner { x: 1 } };\n"
                          "    p.inner.x = 2;\n"
                          "    return p.inner.x;\n"
                          "}\n");
    CHECK(var_root.ok, "writing a nested field remains allowed when rooted at 'var'");

    auto dot_let = t.run("struct Point { x: i32 }\n"
                         "fn main() {\n"
                         "    let p: Point = Point { x: 1 };\n"
                         "    p.x = 2;\n"
                         "}\n");
    CHECK(!dot_let.ok, "writing through a 'let' struct root is rejected");
    CHECK(dot_let.hasMessage("cannot write through immutable binding 'p'"),
          "immutable field write names the root binding");

    auto arrow_let = t.run("struct Point { x: i32 }\n"
                           "fn main() {\n"
                           "    let q: Point = Point { x: 1 };\n"
                           "    let p: *Point = &q;\n"
                           "    p->x = 2;\n"
                           "}\n");
    CHECK(!arrow_let.ok, "writing through an immutable struct root and arrow is rejected");
    CHECK(arrow_let.hasMessage("cannot write through immutable binding 'p'"),
          "arrow field write names the pointer root binding");

    auto nested_let = t.run("struct Inner { x: i32 }\n"
                            "struct Outer { inner: Inner }\n"
                            "fn main() {\n"
                            "    let p: Outer = Outer { inner: Inner { x: 1 } };\n"
                            "    p.inner.x = 2;\n"
                            "}\n");
    CHECK(!nested_let.ok, "deep field writes inherit the immutable root");
    CHECK(nested_let.hasMessage("cannot write through immutable binding 'p'"),
          "nested immutable field write names the root binding");

    auto const_global = t.run("struct Point { x: i32 }\n"
                              "const P: Point = Point { x: 1 };\n"
                              "fn main() {\n"
                              "    P.x = 2;\n"
                              "}\n");
    CHECK(!const_global.ok, "writing through a const struct global is rejected");
    CHECK(const_global.hasMessage("cannot write through immutable binding 'P'"),
          "const global field write names the root binding");

    auto const_local = t.run("struct Point { x: i32 }\n"
                             "fn main() {\n"
                             "    const P: Point = Point { x: 1 };\n"
                             "    P.x = 2;\n"
                             "}\n");
    CHECK(!const_local.ok, "writing through a local const struct is rejected");
    CHECK(const_local.hasMessage("cannot write through immutable binding 'P'"),
          "local const field write names the root binding");

    auto current_field = t.run("struct P { const X: i32 = 1, y: i32 }\n"
                               "fn main(): i32 {\n"
                               "    var p: P = P { y: 2 };\n"
                               "    p.X = 3;\n"
                               "    return p.X;\n"
                               "}\n");
    CHECK(!current_field.ok, "const struct fields remain immutable for any root");
    CHECK(current_field.hasMessage("cannot assign to a const struct field"),
          "dedicated const-field diagnostic still fires");

    auto const_union_cast = t.run("raw union Any { i32, f64 }\n"
                                  "fn main() {\n"
                                  "    const u: Any = Any { 1 };\n"
                                  "    var x: i32 = u as i32;\n"
                                  "    x = 2;\n"
                                  "}\n");
    CHECK(const_union_cast.ok, "reading a const union through a member cast stays valid");

    auto let_union_cast = t.run("raw union Any { i32, f64 }\n"
                                "fn main() {\n"
                                "    let u: Any = Any { 1 };\n"
                                "    u as i32;\n"
                                "}\n");
    CHECK(let_union_cast.ok, "const/let union values continue to type-check");
}

static void test_modern_enum_constant_discriminants() {
    ModernSemaTest t;

    auto ok =
        t.run("enum Flag { ONE = 1, SHIFT = 1 << 4, OR = 1 |. 4, NEG = -1, PREV = SHIFT + 1 }\n"
              "fn main(): Flag { Flag.PREV }\n",
              session::Stage::HirLowered);
    CHECK(ok.ok, "constant enum discriminant expressions lower successfully");

    auto prior = t.run("enum Flag { ONE = 1, TWO = ONE + 1 }\n"
                       "fn main(): Flag { Flag.TWO }\n");
    CHECK(prior.ok, "enum variants can reference earlier variants");

    auto qualified_prior = t.run("enum Flag { ONE = 1, TWO = Flag.ONE + 1 }\n"
                                 "fn main(): Flag { Flag.TWO }\n");
    CHECK(qualified_prior.ok, "enum variants can reference earlier variants by qualified name");

    auto global = t.run("const BASE: i32 = 10;\n"
                        "enum Flag { SHIFT = BASE << 2 }\n"
                        "fn main(): Flag { Flag.SHIFT }\n");
    CHECK(global.ok, "enum variants can reference integer const globals");

    auto local = t.run("const BASE: i32 = 3;\n"
                       "enum Flag { V = BASE + 1 }\n"
                       "fn main(): Flag { Flag.V }\n");
    CHECK(local.ok, "enum variants can reference integer local consts");

    auto float_disc = t.run("enum Flag { BAD = 1.5 }\n"
                            "fn main(): Flag { Flag.BAD }\n");
    CHECK(!float_disc.ok, "float enum discriminants are rejected");
    CHECK(float_disc.hasMessage("constant integer expression"),
          "float enum discriminant reports the constant-expr diagnostic");

    auto call_disc = t.run("fn foo(): i32 { 1 }\n"
                           "enum Flag { BAD = foo() }\n"
                           "fn main(): Flag { Flag.BAD }\n");
    CHECK(!call_disc.ok, "calls in enum discriminants are rejected");
    CHECK(call_disc.hasMessage("constant integer expression"),
          "call enum discriminant reports the constant-expr diagnostic");

    auto divide_by_zero = t.run("enum Bad { V = 1 / 0 }\n"
                                "fn main(): Bad { Bad.V }\n");
    CHECK(!divide_by_zero.ok, "division by zero in enum discriminants is rejected");
    CHECK(divide_by_zero.hasMessage("constant integer expression"),
          "division by zero reports the constant-expr diagnostic");

    auto shift_out_of_range = t.run("enum Bad { V = 1 << 64 }\n"
                                    "fn main(): Bad { Bad.V }\n");
    CHECK(!shift_out_of_range.ok, "invalid shift amounts in enum discriminants are rejected");
    CHECK(shift_out_of_range.hasMessage("constant integer expression"),
          "invalid shift reports the constant-expr diagnostic");

    auto overflow_disc = t.run("enum Small: u8 { BIG = 256 }\n"
                               "fn main(): Small { Small.BIG }\n");
    CHECK(!overflow_disc.ok, "enum discriminants outside the underlying type are rejected");
    CHECK(overflow_disc.hasMessage("does not fit its underlying type"),
          "underlying-type overflow names the enum type");

    auto negative_u8 = t.run("enum Small: u8 { NEG = -1 }\n"
                             "fn main(): Small { Small.NEG }\n");
    CHECK(!negative_u8.ok, "negative values are rejected for unsigned enum underlying types");
    CHECK(negative_u8.hasMessage("does not fit its underlying type"),
          "negative unsigned discriminant reports the underlying-type diagnostic");

    auto int64_overflow = t.run("const BIG: i64 = 9223372036854775807;\n"
                                "enum Bad { V = BIG + 1 }\n"
                                "fn main(): Bad { Bad.V }\n");
    CHECK(!int64_overflow.ok, "int64 discriminant arithmetic overflow is rejected");
    CHECK(int64_overflow.hasMessage("constant integer expression"),
          "int64 overflow reports the constant-expr diagnostic");

    auto wide_mul = t.run("const BIG: i64 = 4611686018427387904;\n"
                          "enum Bad { V = BIG * 2 }\n"
                          "fn main(): Bad { Bad.V }\n");
    CHECK(!wide_mul.ok, "int64 discriminant multiplication overflow is rejected");
    CHECK(wide_mul.hasMessage("constant integer expression"),
          "int64 multiplication overflow reports the constant-expr diagnostic");

    auto enum_to_int = t.run("enum Color { Red = 1, Green = 2 }\n"
                             "fn main(): i32 {\n"
                             "    let c: Color = Color.Green;\n"
                             "    return c as i32;\n"
                             "}\n");
    CHECK(enum_to_int.ok, "enum to integer cast is allowed");

    auto int_to_enum = t.run("enum Color { Red = 1, Green = 2 }\n"
                             "fn main(): Color {\n"
                             "    let x: i32 = 2;\n"
                             "    return x as Color;\n"
                             "}\n");
    CHECK(!int_to_enum.ok, "integer to enum cast is disallowed");
}

static void test_sema() {
    test_basic_unification();
    test_type_mismatch();
    test_return_type_mismatch();
    test_control_flow_ok();
    test_undefined_identifier();
    test_wrong_arity();
    test_state_machine_ok();
    test_state_machine_allows_diverging_parameters();
    test_state_machine_return_type_mismatch_is_rejected();
    test_state_machine_rejects_mixed_return_types();
    test_jump_requires_state_context();
    test_dock_requires_state_target();
    test_state_transition_arity_and_type_mismatch();
    test_dock_argument_arity_and_type_mismatch();
    test_state_return_type_checked_against_machine_result();
    test_nested_state_machines();
    test_extern_fn_call_ok();
    test_extern_fn_call_bad_arg();
    test_type_alias_unification();
    test_type_alias_invalid_assignment();
    test_binary_op_type_error();
    test_while_loop_ok();
    test_unary_op_ok();
    test_index_validation();
    test_field_not_implemented_warning();
    test_macro_unknown();
    test_macro_defined();
    test_tagged_union_is_syntax();
    test_fallback_and_propagation_are_rejected();
    test_optional_propagation_is_supported();
    test_word_sequences_are_rejected();
    test_use_statements_are_rejected();
    test_word_and_context_declarations_are_rejected();
    test_word_call_ast_is_rejected();
    test_unsupported_syntax_does_not_reach_hir();
    test_offsetof_intrinsic_ok();
    test_offsetof_non_struct_fails();
    test_offsetof_unknown_field_fails();
    test_sizeof_intrinsic_ok();
    test_sizeof_primitive_ok();
    test_sizeof_void_fails();
    test_when_literal_cases_ok();
    test_when_boolean_conditions_ok();
    test_when_missing_default_fails();
    test_when_mismatched_bodies_fail();
    test_when_range_subject_mismatch_fails();
    test_named_struct_literal_ok();
    test_named_struct_literal_duplicate_field_fails();
    test_named_struct_literal_unknown_field_fails();
    test_named_struct_literal_mixed_form_fails();
    test_named_struct_literal_placeholder_without_default_fails();
    test_struct_field_default_type_mismatch_fails();
    test_modern_basic_valid();
    test_modern_return_type_mismatch();
    test_modern_call_arity();
    test_modern_call_arg_type();
    test_modern_if_condition();
    test_modern_while_condition();
    test_modern_struct_decl();
    test_modern_struct_literal_missing_field_fails();
    test_modern_struct_literal_omitted_default_allowed();
    test_modern_struct_literal_missing_named_field_without_default_fails();
    test_modern_struct_name_field_access_fails();
    test_modern_raw_union_member_cast();
    test_modern_raw_union_cast_rejects_non_member();
    test_modern_union_parameter_and_return();
    test_modern_import_call();
    test_modern_void_fn();
    test_modern_binary_arithmetic();
    test_modern_unary_not_on_bool();
    test_modern_unary_invalid_type();
    test_modern_assign_retype();
    test_modern_two_modules_at_once();
    test_modern_let_binding_without_annotation();
    test_modern_pointer_type_param();
    test_modern_type_alias_use();
    test_modern_grouped_struct_fields();
    test_modern_static_method();
    test_modern_imported_static_method();
    test_modern_imported_receiver_method();
    test_modern_imported_method_struct_literal_body();
    test_modern_self_resolves_to_owner();
    test_modern_unary_minus_on_literal();
    test_modern_break_in_while();
    test_modern_continue_in_while();
    test_modern_empty_block_is_void();
    test_modern_if_else_unified_type();
    test_modern_nested_block_type();
    test_modern_multi_param_call();
    test_modern_reassign_same_type();
    test_modern_extern_call_return();
    test_modern_numeric_cast_ok();
    test_modern_implicit_numeric_conversion_rejected();
    test_modern_raw_opaque_cast_accepted();
    test_modern_raw_opaque_to_integer_rejected();
    test_modern_pointer_cast_rejected();
    test_modern_pointer_to_void_rejected();
#ifdef ZITH_ENABLE_C_INTEROP
    test_modern_c_pointer_cast_to_nullable_accepted();
    test_modern_c_pointer_cast_to_non_nullable_rejected();
    test_modern_pointer_coerces_to_c_void_pointer();
    test_modern_nullable_c_pointer_initializes_non_optional();
#endif
    test_modern_pointer_coercion_is_one_way();
    test_modern_overload_prefers_exact_pointer();
    test_modern_null_needs_optional_pointer();
    test_modern_is_null_on_optional_pointer();
    test_modern_is_null_requires_optional();
    test_modern_radix_integer_literals();
    test_modern_integer_literal_overflow_reported();
    test_modern_pointer_compared_to_integer_literal_fails();
    test_modern_loop_body_infers_locals();
    test_modern_for_flat_and_parenthesized_forms();
    test_modern_for_three_clause();
    test_modern_for_in_iterators();
    test_modern_generic_params();
    test_modern_generic_struct_literal_inference();
    test_modern_array_literal();
    test_modern_array_literal_mismatch();
    test_modern_array_literal_empty();
    test_modern_char_literal_and_escapes();
    test_modern_struct_method_decl();
    test_modern_parameter_mutability();
    test_modern_self_mutability_and_auto_deref();
    test_modern_logical_move_for_method_receivers();
    test_modern_implement_block_method();
    test_modern_method_call_arity_mismatch();
    test_modern_method_call_arg_type_mismatch();
    test_modern_tagged_union_cast_policy();
    test_modern_qualified_method_receivers();
    test_modern_unknown_method_still_reports_field();
    test_modern_function_value_and_call();
    test_modern_function_value_type_mismatch();
    test_modern_array_to_slice_coercion();
    test_modern_array_to_slice_element_mismatch();
    test_modern_slice_range_sema();
    test_modern_raw_slice_and_index_sema();
    test_modern_zith_bindings();
    test_modern_mutability_propagates_to_struct_fields();
    test_modern_enum_constant_discriminants();
}

TEST_MAIN(sema)
