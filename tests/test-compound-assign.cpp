// test-compound-assign.cpp - Compound assignment, bitwise base operators and `raw opaque`.
#include "cli/options.hpp"
#include "diagnostics/diagnostic-engine.hpp"
#include "diagnostics/error-codes.hpp"
#include "frontend/frontend.hpp"
#include "session/compilation-session.hpp"
#include "session/frontend-context.hpp"
#include "session/pipeline-plan.hpp"
#include "test-common.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace zith;

namespace {

namespace fs = std::filesystem;

struct Result {
    bool ok           = false;
    size_t errorCount = 0;
    std::vector<uint32_t> codes;
    std::vector<std::string> messages;

    [[nodiscard]] bool hasErrorCode(diagnostics::ErrCode code) const {
        for (const auto c : codes) {
            if (c == static_cast<uint32_t>(code))
                return true;
        }
        return false;
    }

    [[nodiscard]] bool hasMessage(std::string_view needle) const {
        for (const auto &message : messages) {
            if (message.find(needle) != std::string::npos)
                return true;
        }
        return false;
    }
};

struct Workspace {
    fs::path root = fs::temp_directory_path() / "zith-compound-assign-tests";

    Workspace() {
        fs::remove_all(root);
        fs::create_directories(root);
    }

    ~Workspace() {
        fs::remove_all(root);
    }

    void write(std::string_view name, std::string_view text) const {
        const auto path = root / name;
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << text;
    }
};

Result check(std::string_view source) {
    Workspace workspace;
    workspace.write("main.zith", source);

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::TypeChecked;

    session::FrontendConfig config;
    config.workspaceRoot      = workspace.root.string();
    config.maxFrontendWorkers = 1;
    config.compilerVersion    = "test";
    auto context              = std::make_shared<session::FrontendContext>(config);

    session::CompilationSession session(options, (workspace.root / "main.zith").string(), context);
    session.setBuffered(true);

    Result result;
    result.ok = session.runTo(session::Stage::TypeChecked);
    for (const auto &diagnostic : session.diags().all()) {
        if (diagnostic.severity != diagnostics::Severity::Error)
            continue;
        ++result.errorCount;
        result.codes.push_back(diagnostic.code);
        result.messages.emplace_back(diagnostic.message);
        std::printf("    [Diag] Code: %u, Message: %s\n", diagnostic.code,
                    diagnostic.message.c_str());
    }
    result.ok = result.ok && result.errorCount == 0;
    return result;
}

/// Finds the Assign expression spelled `op` and returns it, or nullptr.
const frontend::Expression *findAssign(const frontend::FrontendSnapshot &snapshot,
                                       const std::string_view op) {
    for (const auto &expr : snapshot.expressions()) {
        if (expr.kind == frontend::ExprKind::Assign && expr.text == op)
            return &expr;
    }
    return nullptr;
}

// ── Desugaring shape ─────────────────────────────────────────

void compoundDesugarsToAssignOverBinary() {
    struct Case {
        const char *compound;
        const char *base;
    };
    static constexpr Case cases[] = {
        {"+=", "+"},   {"-=", "-"},   {"*=", "*"},  {"/=", "/"},  {"%=", "%"},
        {"<<=", "<<"}, {">>=", ">>"}, {"&=", "&."}, {"|=", "|."}, {"^=", "^."},
    };
    for (const auto &c : cases) {
        const std::string source = std::string("fn main(): i32 {\n    var x: i32 = 8;\n    x ") +
                                   c.compound + " 1;\n    return x;\n}\n";
        const auto snapshot   = frontend::parse(source);
        const auto *assign    = findAssign(snapshot, c.compound);
        const std::string tag = std::string("'") + c.compound + "'";
        CHECK(assign != nullptr,
              (tag + " produces an Assign keeping the compound spelling").c_str());
        if (assign == nullptr || assign->operands.size() != 2U)
            continue;
        const auto &folded = snapshot.expressions()[assign->operands[1].value - 1U];
        CHECK(folded.kind == frontend::ExprKind::Binary && folded.text == c.base,
              (tag + " desugars to a Binary over its base operator").c_str());
        CHECK(folded.operands.size() == 2U && folded.operands[0] == assign->operands[0],
              (tag + " reuses the assignment target as the left operand").c_str());
    }
}

void assignmentIsRightAssociative() {
    const auto snapshot = frontend::parse("fn main(): i32 {\n"
                                          "    var a: i32 = 1;\n"
                                          "    var b: i32 = 2;\n"
                                          "    a = b += 1;\n"
                                          "    return a;\n"
                                          "}\n");
    const auto *outer   = findAssign(snapshot, "=");
    CHECK(outer != nullptr, "'a = b += 1' parses as a plain assignment at the top");
    if (outer == nullptr || outer->operands.size() != 2U)
        return;
    const auto &rhs = snapshot.expressions()[outer->operands[1].value - 1U];
    CHECK(rhs.kind == frontend::ExprKind::Assign && rhs.text == "+=",
          "'a = b += 1' nests the compound assignment on the right");
}

// ── Type checking ────────────────────────────────────────────

void everyCompoundOperatorTypeChecks() {
    auto r = check("fn main(): i32 {\n"
                   "    var x: i32 = 8;\n"
                   "    x += 2;\n"
                   "    x -= 1;\n"
                   "    x *= 3;\n"
                   "    x /= 2;\n"
                   "    x %= 5;\n"
                   "    x <<= 1;\n"
                   "    x >>= 1;\n"
                   "    x &= 3;\n"
                   "    x |= 4;\n"
                   "    x ^= 1;\n"
                   "    return x;\n"
                   "}\n");
    CHECK(r.ok, "all ten compound assignment operators type-check on an i32");
}

void compoundAssignmentYieldsAValue() {
    auto r = check("fn main(): i32 {\n"
                   "    var x: i32 = 1;\n"
                   "    var y: i32 = (x += 2);\n"
                   "    return y;\n"
                   "}\n");
    CHECK(r.ok, "compound assignment yields a value, like '='");
}

void compoundOnViewBindingIsRejected() {
    auto r = check("fn main(): i32 {\n"
                   "    var x: view i32 = 1;\n"
                   "    x += 1;\n"
                   "    return 0;\n"
                   "}\n");
    CHECK(!r.ok, "'+=' through a 'view' binding is rejected");
    CHECK(r.hasErrorCode(diagnostics::err::WriteThroughView),
          "'+=' through a 'view' binding reports E4004");
}

void compoundBetweenMismatchedTypesIsRejected() {
    auto r = check("fn main(): i32 {\n"
                   "    var x: i32 = 1;\n"
                   "    var f: f64 = 2.0;\n"
                   "    x += f;\n"
                   "    return x;\n"
                   "}\n");
    CHECK(!r.ok, "'+=' between mismatched numeric types is rejected");
}

void bitwiseCompoundOnBoolIsRejected() {
    auto r = check("fn main(): i32 {\n"
                   "    var b: bool = true;\n"
                   "    b &= true;\n"
                   "    return 0;\n"
                   "}\n");
    CHECK(!r.ok, "'&=' on a bool is rejected");
    CHECK(r.hasMessage("bitwise operator expects integer operands"),
          "'&=' on a bool reports the bitwise-integer error");
}

// ── Bitwise base operators ───────────────────────────────────

void bitwiseBaseOperatorsTypeCheck() {
    auto r = check("fn main(): i32 {\n"
                   "    var a: i32 = 6;\n"
                   "    var b: i32 = 3;\n"
                   "    var c: i32 = a &. b;\n"
                   "    var d: i32 = a |. b;\n"
                   "    var e: i32 = a ^. b;\n"
                   "    var f: i32 = ~a;\n"
                   "    return c + d + e + f;\n"
                   "}\n");
    CHECK(r.ok, "'&.', '|.', '^.' and '~' type-check on integers");
}

void bitwiseOnBoolIsRejected() {
    auto r = check("fn main(): i32 {\n"
                   "    var a: i32 = 1;\n"
                   "    var c: i32 = a &. true;\n"
                   "    return c;\n"
                   "}\n");
    CHECK(!r.ok, "'&.' with a bool operand is rejected");
    CHECK(r.hasMessage("bitwise operator expects integer operands"),
          "'&.' with a bool operand reports the bitwise-integer error");
}

void bitwiseNotOnBoolIsRejected() {
    auto r = check("fn main(): i32 {\n"
                   "    var c: i32 = ~true;\n"
                   "    return c;\n"
                   "}\n");
    CHECK(!r.ok, "'~true' is rejected");
    CHECK(r.hasMessage("unary '~' expects an integer operand"),
          "'~true' reports the unary bitwise-not error");
}

void bitwiseAndBindsTighterThanOr() {
    // `a |. b &. c` must parse as `a |. (b &. c)`.
    const auto snapshot              = frontend::parse("fn main(): i32 {\n"
                                                                    "    var a: i32 = 1;\n"
                                                                    "    var r: i32 = a |. a &. a;\n"
                                                                    "    return r;\n"
                                                                    "}\n");
    const frontend::Expression *root = nullptr;
    for (const auto &expr : snapshot.expressions()) {
        if (expr.kind == frontend::ExprKind::Binary && expr.text == "|.")
            root = &expr;
    }
    CHECK(root != nullptr, "'a |. a &. a' has '|.' at the root");
    if (root == nullptr || root->operands.size() != 2U)
        return;
    const auto &rhs = snapshot.expressions()[root->operands[1].value - 1U];
    CHECK(rhs.kind == frontend::ExprKind::Binary && rhs.text == "&.",
          "'&.' binds tighter than '|.'");
}

// ── `&&` / `||` rejection ────────────────────────────────────

void logicalAndIsRejected() {
    auto r = check("fn main(): i32 {\n"
                   "    var a: bool = true;\n"
                   "    var b: bool = false;\n"
                   "    var c: bool = a && b;\n"
                   "    if (c) { return 1; }\n"
                   "    return 0;\n"
                   "}\n");
    CHECK(!r.ok, "'&&' is rejected");
    CHECK(r.hasMessage("'&&' is not a Zith operator; use 'and'"), "'&&' points the user at 'and'");
    CHECK(r.errorCount == 1U, "'&&' produces exactly one diagnostic (no cascade)");
}

void logicalOrIsRejected() {
    auto r = check("fn main(): i32 {\n"
                   "    var a: bool = true;\n"
                   "    var b: bool = false;\n"
                   "    var c: bool = a || b;\n"
                   "    if (c) { return 1; }\n"
                   "    return 0;\n"
                   "}\n");
    CHECK(!r.ok, "'||' is rejected");
    CHECK(r.hasMessage("'||' is not a Zith operator; use 'or'"), "'||' points the user at 'or'");
    CHECK(r.errorCount == 1U, "'||' produces exactly one diagnostic (no cascade)");
}

void logicalAndInIntContextNoLongerMiscompiles() {
    // Regression: `&` used to have precedence -1, so the parser abandoned the binary
    // loop and silently discarded `&& b`, compiling this to `var c: i32 = a;`.
    auto r = check("fn main(): i32 {\n"
                   "    var a: i32 = 6;\n"
                   "    var b: i32 = 3;\n"
                   "    var c: i32 = a && b;\n"
                   "    return c;\n"
                   "}\n");
    CHECK(!r.ok, "'a && b' in value position no longer reaches codegen");
    CHECK(r.hasErrorCode(diagnostics::err::UnsupportedSyntax),
          "'a && b' reports UnsupportedSyntax rather than silently dropping the rhs");
}

// ── `raw opaque` ─────────────────────────────────────────────

void rawOpaqueParsesAsItsOwnTypeKind() {
    const auto snapshot = frontend::parse("fn f(p: raw opaque): i32 { return 0; }\n");
    bool found          = false;
    for (const auto &type : snapshot.typeExpressions()) {
        if (type.kind == frontend::TypeExprKind::Opaque)
            found = true;
    }
    CHECK(found, "'raw opaque' parses to TypeExprKind::Opaque");
}

void rawOpaqueCastsBothWays() {
    auto r = check("fn f(p: raw opaque): *i32 { return p as *i32; }\n"
                   "fn g(q: *i32): raw opaque { return q as raw opaque; }\n"
                   "fn main(): i32 { return 0; }\n");
    CHECK(r.ok, "'raw opaque as *T' and '*T as raw opaque' both type-check");
}

void concretePointerCastIsStillRejected() {
    auto r = check("fn f(p: *u8): *i32 { return p as *i32; }\n"
                   "fn main(): i32 { return 0; }\n");
    CHECK(!r.ok, "'*u8 as *i32' is still rejected");
    CHECK(r.hasErrorCode(diagnostics::err::InvalidCast), "'*u8 as *i32' reports E3003");
}

void opaqueToIntegerCastIsRejected() {
    auto r = check("fn f(p: raw opaque): i64 { return p as i64; }\n"
                   "fn main(): i32 { return 0; }\n");
    CHECK(!r.ok, "'raw opaque as i64' is rejected");
    CHECK(r.hasErrorCode(diagnostics::err::InvalidCast), "'raw opaque as i64' reports E3003");
}

void literalPointerToVoidIsStillRejected() {
    auto r = check("fn f(p: *void): i32 { return 0; }\n"
                   "fn main(): i32 { return 0; }\n");
    CHECK(!r.ok, "a literally written '*void' is still rejected");
    CHECK(r.hasMessage("use 'raw opaque' for C interop"),
          "'*void' still points the user at 'raw opaque'");
}

} // namespace

void test_compound_assign() {
    compoundDesugarsToAssignOverBinary();
    assignmentIsRightAssociative();
    everyCompoundOperatorTypeChecks();
    compoundAssignmentYieldsAValue();
    compoundOnViewBindingIsRejected();
    compoundBetweenMismatchedTypesIsRejected();
    bitwiseCompoundOnBoolIsRejected();
    bitwiseBaseOperatorsTypeCheck();
    bitwiseOnBoolIsRejected();
    bitwiseNotOnBoolIsRejected();
    bitwiseAndBindsTighterThanOr();
    logicalAndIsRejected();
    logicalOrIsRejected();
    logicalAndInIntContextNoLongerMiscompiles();
    rawOpaqueParsesAsItsOwnTypeKind();
    rawOpaqueCastsBothWays();
    concretePointerCastIsStillRejected();
    opaqueToIntegerCastIsRejected();
    literalPointerToVoidIsStillRejected();
}

TEST_MAIN(compound_assign)
