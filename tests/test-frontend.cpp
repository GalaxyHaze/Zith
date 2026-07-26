#include "frontend/frontend.hpp"
#include "test-common.hpp"

#include <string_view>

using namespace zith;

static void test_lossless_trivia_and_spans() {
    constexpr std::string_view source = "/// docs\nfn main() { // comment\n  return 42;\n}\n";
    auto snapshot                     = frontend::parse(std::string(source));

    CHECK_EQ(snapshot.reconstruct(), source, "lossless reconstruction preserves the source");
    CHECK(snapshot.trivia().size() >= 4, "trivia includes whitespace and comments");
    CHECK_EQ(snapshot.root().span().start, 0u, "root starts at the source start");
    CHECK_EQ(snapshot.root().span().end, static_cast<uint32_t>(source.size()),
             "root covers the complete source");
    CHECK(snapshot.diagnostics().empty(), "valid source has no recovery diagnostics");
}

static void test_keywords_and_module_ast() {
    for (const std::string_view word : {"fn", "from", "struct", "pub", "return"}) {
        auto snapshot = frontend::parse(std::string(word));
        CHECK_EQ(snapshot.tokens().size(), 2u, "keyword produces one token plus EOF");
        CHECK_EQ(snapshot.tokens()[0].kind, frontend::TokenKind::Keyword,
                 "known word is classified as a keyword");
    }
    auto snapshot = frontend::parse("not_a_keyword");
    CHECK_EQ(snapshot.tokens()[0].kind, frontend::TokenKind::Identifier,
             "unknown words remain identifiers");

    auto module = frontend::parse("pub fn main() {}\nfrom dep\n");
    CHECK_EQ(module.declarations().size(), 2u, "module AST has declarations and imports");
    CHECK_EQ(module.declarations()[0].kind, frontend::DeclKind::Function,
             "function declaration is lowered from CST");
    CHECK_EQ(module.declarations()[0].visibility, frontend::Visibility::Public,
             "declaration visibility is preserved");
    CHECK_EQ(module.declarations()[1].kind, frontend::DeclKind::Import,
             "import is lowered from CST");
    CHECK_EQ(module.declarations()[1].import.path[0], std::string("dep"),
             "import path is preserved");
}

static void test_recovery_creates_error_nodes() {
    auto snapshot = frontend::parse("fn broken(}");
    CHECK(!snapshot.diagnostics().empty(), "mismatched delimiter produces a diagnostic");

    const auto root = snapshot.root();
    bool hasError   = false;
    for (uint32_t index = 0; index < root.childCount(); ++index) {
        if (root.child(index).isNode() &&
            root.child(index).node->kind == frontend::SyntaxKind::Error)
            hasError = true;
    }
    CHECK(hasError, "recovery retains an explicit error node");
    CHECK_EQ(snapshot.reconstruct(), "fn broken(}", "recovery remains lossless");
}

static void test_function_body_ast() {
    auto snapshot = frontend::parse(
        "fn add(left: i32, right: i32): i32 {\n"
        "    var total: i32 = left + right;\n"
        "    return total;\n"
        "}\n");

    CHECK(snapshot.diagnostics().empty(), "function body is lowered without recovery diagnostics");
    CHECK_EQ(snapshot.declarations().size(), 1u, "function is the only top-level declaration");
    const auto &function = snapshot.declarations()[0];
    CHECK_EQ(function.parameters.size(), 2u, "parameters are retained in the frontend AST");
    CHECK(function.parameters[0].id && function.parameters[1].id,
          "parameters receive stable local identities");
    CHECK(function.parameters[0].id != function.parameters[1].id,
          "parameter local identities do not alias");
    CHECK(function.declaredType, "function return type is represented by a type expression ID");
    CHECK(function.body, "function body is represented by an expression ID");
    CHECK_EQ(snapshot.typeExpressions().size(), 4u,
             "parameter, binding, and return annotations are lowered into type expressions");
    CHECK_EQ(snapshot.expressions().size(), 5u,
             "body has binary, names, return value, and block expressions");
    CHECK_EQ(snapshot.statements().size(), 2u, "body has binding and return statements");
    CHECK_EQ(snapshot.statements()[0].kind, frontend::StmtKind::Binding,
             "variable declaration is a binding statement");
    CHECK(snapshot.statements()[0].binding.initializer, "binding retains its initializer expression");
    CHECK_EQ(snapshot.statements()[1].kind, frontend::StmtKind::Return,
             "return is represented as a statement");
}

static void test_control_flow_and_scopes() {
    auto snapshot = frontend::parse(
        "fn run(n: i32): i32 {\n"
        "    if (n < 0) {\n"
        "        return 0;\n"
        "    }\n"
        "    while (n > 0) {\n"
        "        var step: i32 = 1;\n"
        "    }\n"
        "    return n;\n"
        "}\n");

    CHECK(snapshot.diagnostics().empty(), "control flow lowers without diagnostics");
    CHECK(snapshot.scopes().size() >= 3u,
          "control flow introduces child scopes per block");
    bool found_while = false;
    bool found_if    = false;
    for (const auto &expression : snapshot.expressions()) {
        if (expression.kind == frontend::ExprKind::While)
            found_while = true;
        else if (expression.kind == frontend::ExprKind::If)
            found_if = true;
    }
    CHECK(found_if && found_while, "both if and while expressions are lowered");
}

static void test_frontend() {
    test_lossless_trivia_and_spans();
    test_keywords_and_module_ast();
    test_recovery_creates_error_nodes();
    test_function_body_ast();
    test_control_flow_and_scopes();
}

TEST_MAIN(frontend)
