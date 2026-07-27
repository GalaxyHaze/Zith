#include "legacy-zith/ast/ast-builder.hpp"
#include "cli/options.hpp"
#include "diagnostics/diagnostic-engine.hpp"
#include "diagnostics/error-codes.hpp"
#include "memory/arena.hpp"
#include "memory/string-interner.hpp"
#include "legacy-zith/sema/sema-pipeline.hpp"
#include "session/compilation-session.hpp"
#include "session/frontend-context.hpp"
#include "session/pipeline-plan.hpp"
#include "symbols/symbol-table.hpp"
#include "test-common.hpp"
#include "types/type-intern.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
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
        };
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
            copied_diags.push_back({d.severity, static_cast<diagnostics::ErrCode>(d.code)});
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

    SemaTest::Result run(std::string_view main, std::string_view main_name = "main.zith") {
        write(main_name, main);
        session::FrontendConfig config;
        config.workspaceRoot      = root.string();
        config.maxFrontendWorkers = 1;
        config.compilerVersion    = "test";
        auto context              = std::make_shared<session::FrontendContext>(config);
        session::CompilationSession session(opts, (root / main_name).string(), context);
        session.setBuffered(true);
        bool ok      = session.runTo(session::Stage::TypeChecked);
        size_t errs  = 0;
        size_t warns = 0;
        std::vector<SemaTest::Result::LightDiag> copied_diags;
        for (const auto &d : session.diags().all()) {
            copied_diags.push_back({d.severity, static_cast<diagnostics::ErrCode>(d.code)});
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

static void test_marker_jump_ok() {
    SemaTest t;
    auto r = t.run("fn main() {\n"
                   "    var x: i32 = 0;\n"
                   "    marker my_loop {\n"
                   "        x = x + 1;\n"
                   "        if (x < 10) {\n"
                   "            jump my_loop;\n"
                   "        }\n"
                   "    }\n"
                   "}\n",
                   session::Stage::HirLowered);
    CHECK(r.ok, "Valid marker and jump setup lowers successfully");
}

static void test_marker_jump_undefined() {
    SemaTest t;
    auto r = t.run("fn main() {\n"
                   "    jump nonexistent_label;\n"
                   "}\n");
    CHECK(!r.ok, "Jump to undefined marker fails");
    CHECK(r.hasErrorCode(diagnostics::err::UndefinedIdent), "Reports UndefinedIdent (2001)");
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

static void test_macro_not_implemented_warning() {
    SemaTest t;
    auto r = t.run("fn main() {\n"
                   "    @foo();\n"
                   "}\n");
    CHECK(r.ok, "Macro calls currently warn instead of failing");
    CHECK_EQ(r.warningCount, 1u, "Exactly one warning for macro call");
    CHECK(r.hasWarningCode(diagnostics::err::NotImplemented), "Reports NotImplemented warning");
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

static void test_is_and_as_are_rejected() {
    SemaTest is_test;
    check_unsupported_syntax(is_test.run("fn main() { 1 is Missing; }\n"));

    SemaTest as_test;
    check_unsupported_syntax(as_test.run("fn main() { 1 as Missing; }\n"));
}

static void test_fallback_and_propagation_are_rejected() {
    SemaTest fallback_optional;
    check_unsupported_syntax(fallback_optional.run("fn main() { ?missing; }\n"));

    SemaTest fallback_failable;
    check_unsupported_syntax(fallback_failable.run("fn main() { !missing; }\n"));

    SemaTest propagate_optional;
    check_unsupported_syntax(propagate_optional.run("fn main() { missing?; }\n"));

    SemaTest propagate_failable;
    check_unsupported_syntax(propagate_failable.run("fn main() { missing!; }\n"));
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
    check_unsupported_syntax(t.run("fn main() { 1 is Missing; }\n", session::Stage::HirLowered));
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

static void test_modern_import_call() {
    ModernSemaTest t;
    t.write("dep.zith", "pub fn dep_fn(): i32 { 42 }\n");
    auto r = t.run("from dep\n"
                   "fn main() {\n"
                   "    dep_fn()\n"
                   "}\n");
    CHECK(r.ok, "Modern sema resolves an imported function call");
}

static void test_sema() {
    test_basic_unification();
    test_type_mismatch();
    test_return_type_mismatch();
    test_control_flow_ok();
    test_undefined_identifier();
    test_wrong_arity();
    test_marker_jump_ok();
    test_marker_jump_undefined();
    test_extern_fn_call_ok();
    test_extern_fn_call_bad_arg();
    test_type_alias_unification();
    test_type_alias_invalid_assignment();
    test_binary_op_type_error();
    test_while_loop_ok();
    test_unary_op_ok();
    test_index_validation();
    test_field_not_implemented_warning();
    test_macro_not_implemented_warning();
    test_is_and_as_are_rejected();
    test_fallback_and_propagation_are_rejected();
    test_word_sequences_are_rejected();
    test_use_statements_are_rejected();
    test_word_and_context_declarations_are_rejected();
    test_word_call_ast_is_rejected();
    test_unsupported_syntax_does_not_reach_hir();
    test_offsetof_intrinsic_ok();
    test_offsetof_non_struct_fails();
    test_offsetof_unknown_field_fails();
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
    test_modern_import_call();
}

TEST_MAIN(sema)
