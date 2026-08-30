#include "diagnostics/error-codes.hpp"
#include "frontend/frontend.hpp"
#include "test-common.hpp"

#include <string>
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
    for (const std::string_view word : {"fn", "from", "struct", "pub", "return", "opaque"}) {
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

static void test_bare_opaque_type_expression() {
    auto snapshot = frontend::parse("fn erased(value: i32): opaque {\n"
                                    "    return value as opaque;\n"
                                    "}\n");
    CHECK(snapshot.diagnostics().empty(), "bare 'opaque' type expressions parse cleanly");

    const frontend::TypeExpression *tagged_type = nullptr;
    for (const auto &type : snapshot.typeExpressions()) {
        if (type.kind == frontend::TypeExprKind::OpaqueTagged)
            tagged_type = &type;
    }
    CHECK(tagged_type != nullptr, "bare 'opaque' lowers to TypeExprKind::OpaqueTagged");
    if (tagged_type != nullptr)
        CHECK_EQ(frontend::canonicalTypeString(snapshot, tagged_type->id), std::string("opaque"),
                 "canonical rendering keeps bare 'opaque'");

    auto raw_snapshot = frontend::parse("fn thru(p: raw opaque): *i32 { return p as *i32; }\n");
    CHECK(raw_snapshot.diagnostics().empty(), "'raw opaque' still parses cleanly");
    const frontend::TypeExpression *raw_type = nullptr;
    for (const auto &type : raw_snapshot.typeExpressions()) {
        if (type.kind == frontend::TypeExprKind::Opaque)
            raw_type = &type;
    }
    CHECK(raw_type != nullptr, "'raw opaque' lowers to TypeExprKind::Opaque");
    if (raw_type != nullptr)
        CHECK_EQ(frontend::canonicalTypeString(raw_snapshot, raw_type->id),
                 std::string("raw opaque"), "canonical rendering keeps 'raw opaque'");
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
    auto snapshot = frontend::parse("fn add(left: i32, right: i32): i32 {\n"
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
    CHECK(snapshot.statements()[0].binding.initializer,
          "binding retains its initializer expression");
    CHECK_EQ(snapshot.statements()[1].kind, frontend::StmtKind::Return,
             "return is represented as a statement");
}

static void test_control_flow_and_scopes() {
    auto snapshot = frontend::parse("fn run(n: i32): i32 {\n"
                                    "    if (n < 0) {\n"
                                    "        return 0;\n"
                                    "    }\n"
                                    "    for (n > 0) {\n"
                                    "        var step: i32 = 1;\n"
                                    "    }\n"
                                    "    return n;\n"
                                    "}\n");

    CHECK(snapshot.diagnostics().empty(), "control flow lowers without diagnostics");
    CHECK(snapshot.scopes().size() >= 3u, "control flow introduces child scopes per block");
    bool found_while = false;
    bool found_if    = false;
    for (const auto &expression : snapshot.expressions()) {
        if (expression.kind == frontend::ExprKind::While)
            found_while = true;
        else if (expression.kind == frontend::ExprKind::If)
            found_if = true;
    }
    CHECK(found_if && found_while, "both if and conditional-loop expressions are lowered");
}

static void test_state_and_dock_jump_syntax() {
    auto snapshot = frontend::parse("state Loop(n: i32): i32 {\n"
                                    "    if (n < 10) {\n"
                                    "        jump Loop(n + 1);\n"
                                    "    }\n"
                                    "    return n;\n"
                                    "}\n"
                                    "fn main(): i32 {\n"
                                    "    let result: i32 = dock Loop(0);\n"
                                    "    return result;\n"
                                    "}\n");

    CHECK(snapshot.diagnostics().empty(), "state, dock, and jump parse without diagnostics");
    CHECK_EQ(snapshot.declarations().size(), 2u,
             "state source produces the state declaration and main");
    if (snapshot.declarations().size() == 2u) {
        CHECK_EQ(snapshot.declarations()[0].functionKind, frontend::FunctionKind::State,
                 "state declaration records FunctionKind::State");
        CHECK(snapshot.declarations()[0].declaredType,
              "state declaration retains its explicit return type");
        CHECK_EQ(snapshot.declarations()[1].functionKind, frontend::FunctionKind::Standard,
                 "ordinary functions remain standard");
    }

    bool saw_dock = false;
    bool saw_jump = false;
    for (const auto &expr : snapshot.expressions())
        saw_dock |= expr.kind == frontend::ExprKind::DockCall;
    for (const auto &statement : snapshot.statements())
        saw_jump |= statement.kind == frontend::StmtKind::Jump;
    CHECK(saw_dock, "dock lowers as a DockCall expression");
    CHECK(saw_jump, "jump lowers as a Jump statement");

    bool binding_uses_dock = false;
    for (const auto &statement : snapshot.statements()) {
        if (statement.kind != frontend::StmtKind::Binding || !statement.binding.initializer)
            continue;
        const auto initializer = statement.binding.initializer.value - 1U;
        if (initializer < snapshot.expressions().size() &&
            snapshot.expressions()[initializer].kind == frontend::ExprKind::DockCall) {
            binding_uses_dock = true;
        }
    }
    CHECK(binding_uses_dock, "dock is valid in expression position");
}

static void test_old_state_machine_syntax_is_rejected() {
    auto old_flow = frontend::parse("flow fn main(): i32 { return 0; }\n");
    CHECK(!old_flow.diagnostics().empty(), "flow fn syntax is rejected");

    auto old_marker = frontend::parse("marker Body() {}\n");
    CHECK(!old_marker.diagnostics().empty(), "marker syntax is rejected");

    auto old_stackful = frontend::parse("stackful marker Body() {}\n");
    CHECK(!old_stackful.diagnostics().empty(), "stackful marker syntax is rejected");

    auto old_dock = frontend::parse("fn main() {\n"
                                    "    dock {\n"
                                    "        state Body(): i32 { return 0; }\n"
                                    "    }\n"
                                    "}\n");
    CHECK(!old_dock.diagnostics().empty(), "dock block syntax is rejected");
}

static void test_while_is_deprecated() {
    auto snapshot = frontend::parse("fn run(n: i32): i32 {\n"
                                    "    while (n > 0) {\n"
                                    "        return 0;\n"
                                    "    }\n"
                                    "    return n;\n"
                                    "}\n");

    CHECK_EQ(snapshot.diagnostics().size(), 1u, "'while' produces exactly one diagnostic");
    CHECK(snapshot.diagnostics()[0].isWarning, "the 'while' diagnostic is a warning, not an error");
    bool found_while = false;
    for (const auto &expression : snapshot.expressions()) {
        if (expression.kind == frontend::ExprKind::While)
            found_while = true;
    }
    CHECK(found_while, "'while' still lowers to a loop expression");
}

static bool hasErrorCode(const frontend::FrontendSnapshot &snapshot, uint32_t code) {
    for (const auto &diagnostic : snapshot.diagnostics())
        if (!diagnostic.isWarning && diagnostic.code == code)
            return true;
    return false;
}

static void test_return_expression_requires_semicolon() {
    auto with_value = frontend::parse("fn f(): i32 {\n"
                                      "    return 5;\n"
                                      "}\n");
    CHECK(with_value.diagnostics().empty(), "'return expr;' parses without diagnostics");

    auto bare = frontend::parse("fn f() {\n"
                                "    return;\n"
                                "}\n");
    CHECK(bare.diagnostics().empty(), "'return;' parses without diagnostics");

    auto missing = frontend::parse("fn f(): i32 {\n"
                                   "    return 5\n"
                                   "}\n");
    CHECK(hasErrorCode(missing, diagnostics::err::ExpectedSemicolon),
          "'return expr' reports E1002 ExpectedSemicolon");
    CHECK_EQ(missing.diagnostics().size(), 1u,
             "'return expr' before '}' produces exactly one diagnostic");
}

static void test_else_condition_forms() {
    auto new_form = frontend::parse("fn run(n: i32): i32 {\n"
                                    "    if (n < 0) {\n"
                                    "        return 0;\n"
                                    "    } else (n < 10) {\n"
                                    "        return 1;\n"
                                    "    }\n"
                                    "    return 2;\n"
                                    "}\n");
    CHECK(new_form.diagnostics().empty(), "'else (cond) { }' parses without diagnostics");
    const frontend::Expression *if_node = nullptr;
    for (const auto &expression : new_form.expressions()) {
        if (expression.kind == frontend::ExprKind::If)
            if_node = &expression;
    }
    CHECK(if_node != nullptr, "if/else-expression graph is present");
    if (if_node != nullptr)
        CHECK_EQ(if_node->operands.size(), 4u,
                 "else condition uses the same 4-operand if layout as else if");

    auto old_form = frontend::parse("fn run(n: i32): i32 {\n"
                                    "    if (n < 0) {\n"
                                    "        return 0;\n"
                                    "    } else if (n < 10) {\n"
                                    "        return 1;\n"
                                    "    }\n"
                                    "    return 2;\n"
                                    "}\n");
    CHECK_EQ(old_form.diagnostics().size(), 1u, "'else if' produces exactly one diagnostic");
    CHECK(old_form.diagnostics()[0].isWarning, "the 'else if' diagnostic is a warning");
    CHECK_EQ(old_form.diagnostics()[0].code, diagnostics::err::DeprecatedSyntax,
             "the 'else if' warning uses W1008");
    CHECK(old_form.diagnostics()[0].message.find("'else (cond) { }'") != std::string::npos,
          "the 'else if' warning suggests the replacement spelling");

    bool old_if_found = false;
    for (const auto &expression : old_form.expressions()) {
        if (expression.kind == frontend::ExprKind::If &&
            (expression.operands.size() == 3u || expression.operands.size() == 4u))
            old_if_found = true;
    }
    CHECK(old_if_found, "deprecated 'else if' still lowers to an if graph");

    auto missing_paren = frontend::parse("fn run(n: i32): i32 {\n"
                                         "    if (n < 0) {\n"
                                         "        return 0;\n"
                                         "    } else n < 10 {\n"
                                         "    }\n"
                                         "    return 2;\n"
                                         "}\n");
    bool saw_recovery  = false;
    for (const auto &diagnostic : missing_paren.diagnostics()) {
        if (diagnostic.isWarning)
            continue;
        if (diagnostic.message.find("expected else body") != std::string::npos)
            saw_recovery = true;
    }
    CHECK(saw_recovery, "malformed 'else (cond)' still reports the existing body recovery");
}

static std::string_view tokenText(const frontend::FrontendSnapshot &snapshot,
                                  const frontend::Token &token) {
    return std::string_view(snapshot.source())
        .substr(token.span.start, token.span.end - token.span.start);
}

static bool hasOperatorToken(const frontend::FrontendSnapshot &snapshot, std::string_view text) {
    for (const auto &token : snapshot.tokens()) {
        if (token.kind == frontend::TokenKind::Operator && tokenText(snapshot, token) == text)
            return true;
    }
    return false;
}

static void test_multi_char_operators_are_single_tokens() {
    auto snapshot = frontend::parse("fn cmp(a: i32, b: i32): bool {\n"
                                    "    return a == b;\n"
                                    "}\n");
    CHECK(hasOperatorToken(snapshot, "=="), "'==' lexes as a single two-character operator token");

    for (const std::string_view op : {"==", "!=", "<=", ">="}) {
        auto pair = frontend::parse("fn cmp(a: i32, b: i32): bool { return a " + std::string(op) +
                                    " b; }\n");
        CHECK(hasOperatorToken(pair, op), "two-character comparison operator lexes as one token");
    }

    auto arrow = frontend::parse("fn deref(p: *i32): i32 { return p->x; }\n");
    CHECK(hasOperatorToken(arrow, "->"), "'->' lexes as a single two-character operator token");

    auto single = frontend::parse("fn f(a: i32, b: i32): bool { return not a > b; }\n");
    for (const auto &token : single.tokens()) {
        if (token.kind == frontend::TokenKind::Operator)
            CHECK_EQ(token.span.size(), 1u, "standalone comparison operator stays one character");
    }
}

static void test_binary_comparison_expression() {
    auto snapshot = frontend::parse("fn cmp(a: i32, b: i32): bool {\n"
                                    "    return a == b;\n"
                                    "}\n");

    CHECK(snapshot.diagnostics().empty(), "'==' parses without diagnostics");
    bool found = false;
    for (const auto &expression : snapshot.expressions()) {
        if (expression.kind == frontend::ExprKind::Binary && expression.text == "==")
            found = true;
    }
    CHECK(found, "'==' lowers to a binary expression carrying the full operator text");
}

static void test_cast_expression() {
    auto snapshot = frontend::parse("fn widen(n: i32): i64 {\n"
                                    "    return n as i64;\n"
                                    "}\n");

    CHECK(snapshot.diagnostics().empty(), "'as' parses without diagnostics");
    const frontend::Expression *cast = nullptr;
    for (const auto &expression : snapshot.expressions()) {
        if (expression.kind == frontend::ExprKind::Cast)
            cast = &expression;
    }
    CHECK(cast != nullptr, "'as' lowers to a cast expression");
    if (cast != nullptr) {
        CHECK_EQ(cast->operands.size(), 1u, "a cast has exactly one value operand");
        CHECK(static_cast<bool>(cast->cast_type), "a cast records its target type expression");
    }
}

static void test_is_null_expression() {
    auto snapshot = frontend::parse("fn empty(p: ?*i32): bool {\n"
                                    "    return p is null;\n"
                                    "}\n");

    CHECK(snapshot.diagnostics().empty(), "'is null' parses without diagnostics");
    const frontend::Expression *is_null = nullptr;
    for (const auto &expression : snapshot.expressions()) {
        if (expression.kind == frontend::ExprKind::IsNull)
            is_null = &expression;
    }
    CHECK(is_null != nullptr, "'is null' lowers to a dedicated expression");
    if (is_null != nullptr)
        CHECK_EQ(is_null->operands.size(), 1u, "'is null' has exactly one operand");
}

static void test_is_type_expression() {
    auto snapshot = frontend::parse("fn check(p: ?*i32): bool {\n"
                                    "    return p is i32;\n"
                                    "}\n");

    CHECK(snapshot.diagnostics().empty(), "'is Type' parses without diagnostics");
    const frontend::Expression *is_type = nullptr;
    for (const auto &expression : snapshot.expressions()) {
        if (expression.kind == frontend::ExprKind::IsType)
            is_type = &expression;
    }
    CHECK(is_type != nullptr, "'is Type' lowers to a dedicated expression");
    if (is_type != nullptr) {
        CHECK_EQ(is_type->operands.size(), 1u, "'is Type' has exactly one operand");
        CHECK(static_cast<bool>(is_type->cast_type), "'is Type' records the checked member type");
    }
}

static void test_for_loop_forms() {
    auto conditional = frontend::parse("fn run(n: i32): i32 {\n"
                                       "    for (n > 0) {\n"
                                       "        return 0;\n"
                                       "    }\n"
                                       "    return n;\n"
                                       "}\n");
    CHECK(conditional.diagnostics().empty(), "'for (cond)' parses without diagnostics");
    bool found_conditional = false;
    for (const auto &expression : conditional.expressions()) {
        if (expression.kind == frontend::ExprKind::While)
            found_conditional = true;
    }
    CHECK(found_conditional, "'for (cond)' lowers to a loop expression");

    auto infinite = frontend::parse("fn run(): i32 {\n"
                                    "    for {\n"
                                    "        return 0;\n"
                                    "    }\n"
                                    "    return 1;\n"
                                    "}\n");
    CHECK(infinite.diagnostics().empty(), "'for { }' parses without diagnostics");
    bool found_infinite = false;
    for (const auto &expression : infinite.expressions()) {
        if (expression.kind == frontend::ExprKind::While)
            found_infinite = true;
    }
    CHECK(found_infinite, "'for { }' lowers to a loop expression");

    auto iterator = frontend::parse("fn run(xs: i32): i32 {\n"
                                    "    for (x in xs) {\n"
                                    "        return 0;\n"
                                    "    }\n"
                                    "    return 1;\n"
                                    "}\n");
    CHECK(iterator.diagnostics().empty(), "the iterator form of 'for' parses without diagnostics");
    bool found_for_in = false;
    for (const auto &expression : iterator.expressions()) {
        if (expression.kind == frontend::ExprKind::ForIn)
            found_for_in = true;
    }
    CHECK(found_for_in, "the iterator form lowers to a ForIn expression");
}

static void test_loop_labels_and_not_negation() {
    auto conditional = frontend::parse("fn run(n: i32): i32 {\n"
                                       "    outer: for (n > 0) {\n"
                                       "        break outer;\n"
                                       "        continue outer;\n"
                                       "    }\n"
                                       "    return n;\n"
                                       "}\n");
    CHECK(conditional.diagnostics().empty(),
          "labeled conditional 'for' parses without diagnostics");
    bool labeled_while = false;
    bool labeled_break = false;
    bool labeled_cont  = false;
    for (const auto &expression : conditional.expressions()) {
        if (expression.kind == frontend::ExprKind::While && expression.label == "outer")
            labeled_while = true;
    }
    for (const auto &statement : conditional.statements()) {
        labeled_break |= statement.kind == frontend::StmtKind::Break && statement.label == "outer";
        labeled_cont |=
            statement.kind == frontend::StmtKind::Continue && statement.label == "outer";
    }
    CHECK(labeled_while, "label is stored on the loop expression");
    CHECK(labeled_break, "labeled break keeps its label");
    CHECK(labeled_cont, "labeled continue keeps its label");

    auto three_clause = frontend::parse("fn run(): i32 {\n"
                                        "    outer: for (var i: i32 = 0), (i < 3), (i = i + 1) {\n"
                                        "        continue outer;\n"
                                        "    }\n"
                                        "    return 0;\n"
                                        "}\n");
    CHECK(three_clause.diagnostics().empty(), "labeled 3-clause 'for' parses without diagnostics");
    bool labeled_for = false;
    for (const auto &expression : three_clause.expressions()) {
        if (expression.kind == frontend::ExprKind::For && expression.label == "outer")
            labeled_for = true;
    }
    CHECK(labeled_for, "3-clause loop label is applied to the For expression");

    auto not_bang = frontend::parse("fn negate(b: bool): bool {\n"
                                    "    return !b;\n"
                                    "}\n");
    CHECK(!not_bang.diagnostics().empty(), "prefix '!' is not accepted as a unary operator");
}

static void test_default_parameters_and_condition_syntax() {
    auto defaults = frontend::parse("fn add_with_default(left: i32, right: i32 = 5): i32 {\n"
                                    "    return left + right;\n"
                                    "}\n");
    CHECK(defaults.diagnostics().empty(), "function parameter defaults parse without diagnostics");
    if (defaults.declarations().size() == 1U) {
        const auto &fn = defaults.declarations()[0];
        CHECK(fn.parameters.size() == 2U, "defaulted function keeps both parameters");
        CHECK(fn.parameters[1].defaultValue,
              "the '=' expression is recorded as the parameter default");
    }

    auto conditions         = frontend::parse("fn run(ok: bool, done: bool, keep: bool): i32 {\n"
                                                      "    if (not ok) { return 0; }\n"
                                                      "    while (not done) { break; }\n"
                                                      "    for (var i: i32 = 0), (not done), (i = i + 1) {\n"
                                                      "        for (var j: i32 = 0), (not (done)), (j = j + 1) {\n"
                                                      "        }\n"
                                                      "    }\n"
                                                      "    return 1;\n"
                                                      "}\n");
    bool only_while_warning = true;
    for (const auto &diagnostic : conditions.diagnostics()) {
        if (diagnostic.isWarning)
            continue;
        only_while_warning = false;
    }
    CHECK(only_while_warning,
          "'not' conditions in if/while/flat and parenthesized for clauses parse cleanly");

    auto bare_conditions            = frontend::parse("fn run(x: ?i32, ok: bool): i32 {\n"
                                                                 "    if not ok { return 0; }\n"
                                                                 "    while not ok { break; }\n"
                                                                 "    for not ok { break; }\n"
                                                                 "    if optional x { return 1; }\n"
                                                                 "    while optional x { break; }\n"
                                                                 "    for optional x { return 2; }\n"
                                                                 "    return 3;\n"
                                                                 "}\n");
    size_t optional_condition_diags = 0;
    for (const auto &diagnostic : bare_conditions.diagnostics()) {
        if (!diagnostic.isWarning)
            ++optional_condition_diags;
    }
    CHECK_EQ(optional_condition_diags, 3u,
             "removed 'optional' condition forms report the implicit-condition diagnostic");
}

static void test_for_var_var_reports_specific_diagnostic() {
    auto snapshot     = frontend::parse("fn main() {\n"
                                            "    for (var one: i32, var two: i32) { }\n"
                                            "}\n");
    bool has_specific = false;
    int body_errors   = 0;
    for (const auto &diag : snapshot.diagnostics()) {
        if (diag.message.find("for expects (init), (cond), (step) or a single loop variable") !=
            std::string::npos)
            has_specific = true;
        if (diag.message == "expected for body")
            ++body_errors;
    }
    CHECK(has_specific, "the invalid two-var for header has a specific diagnostic");
    CHECK(body_errors == 0, "consuming the header prevents cascading 'expected for body' errors");
}

static void test_structured_imports() {
    auto snapshot = frontend::parse("from ../lib/utils(3) { render as draw, log } as utilities\n"
                                    "from assets/data.json as Data\n"
                                    "export core/math(..)\n");

    CHECK(snapshot.diagnostics().empty(), "all supported import forms lower without diagnostics");
    CHECK_EQ(snapshot.declarations().size(), 3u, "three imports are preserved");

    const auto &selective = snapshot.declarations()[0].import;
    CHECK_EQ(selective.path.size(), 3u, "relative import retains every path segment");
    CHECK_EQ(selective.path[0], std::string(".."), "relative parent segment is retained");
    CHECK_EQ(selective.path[1], std::string("lib"), "relative path retains directory");
    CHECK_EQ(selective.depth, 3, "finite directory depth is retained");
    CHECK_EQ(selective.alias, std::string("utilities"), "module alias is retained");
    CHECK_EQ(selective.selectors.size(), 2u, "selective import list is retained");
    CHECK_EQ(selective.selectors[0].name, std::string("render"), "selector name is retained");
    CHECK_EQ(selective.selectors[0].alias, std::string("draw"), "selector alias is retained");
    CHECK(selective.pathSpan.size() > 0 && selective.aliasSpan.size() > 0,
          "import path and alias have individual spans");

    const auto &asset = snapshot.declarations()[1].import;
    CHECK(asset.isAsset, "assets prefix classifies the import as an asset");
    CHECK_EQ(asset.alias, std::string("Data"), "asset alias is retained");
    CHECK_EQ(asset.rawPath, std::string("assets/data.json"),
             "asset path preserves its filename extension");

    const auto &unlimited = snapshot.declarations()[2].import;
    CHECK(unlimited.isExport, "export import is retained");
    CHECK_EQ(unlimited.depth, -1, "unlimited directory depth is retained");
}

static void test_c_header_import_syntax() {
    auto valid = frontend::parse("import \"fixture.h\";\n"
                                 "import \"fixture.h\" as c;\n");
    CHECK(valid.diagnostics().empty(), "C header import syntax parses without diagnostics");
    CHECK_EQ(valid.declarations().size(), 2u, "both C header imports are retained");
    CHECK(valid.declarations()[0].import.isHeader, "quoted .h import is classified as a C header");
    CHECK_EQ(valid.declarations()[0].import.headerPath, std::string("fixture.h"),
             "C header path is stored without quotes");
    CHECK_EQ(valid.declarations()[1].import.alias, std::string("c"),
             "C header import retains its optional alias");

    for (const std::string_view source : {
             "import \"fixture.hpp\";\n",
             "from \"fixture.h\";\n",
             "export \"fixture.h\";\n",
             "import \"fixture.h\" { c_add };\n",
             "import \"fixture.h\"(2);\n",
         }) {
        const auto invalid = frontend::parse(std::string(source));
        CHECK(!invalid.diagnostics().empty(), "unsupported C header import form is rejected");
    }
}

static void test_variable_declarations() {
    auto snapshot = frontend::parse("const z: f64 = 3.14;\n"
                                    "fn main() {\n"
                                    "    var x: i32 = 42;\n"
                                    "    let y = 100;\n"
                                    "}\n");

    CHECK(snapshot.diagnostics().empty(), "Zith-- variable declarations lower without diagnostics");
    CHECK_EQ(snapshot.declarations().size(), 2u, "const global and function are lowered");
    CHECK_EQ(snapshot.declarations()[0].kind, frontend::DeclKind::Variable,
             "const keyword produces a global variable declaration");
    CHECK_EQ(snapshot.declarations()[0].name, std::string("z"), "const name is preserved");
    CHECK_EQ(snapshot.declarations()[0].bindingKind, frontend::BindingKind::Const,
             "const records BindingKind::Const");

    bool saw_var = false;
    bool saw_let = false;
    for (const auto &statement : snapshot.statements()) {
        if (statement.kind != frontend::StmtKind::Binding)
            continue;
        saw_var |= statement.binding.name == "x" &&
                   statement.binding.bindingKind == frontend::BindingKind::Var;
        saw_let |= statement.binding.name == "y" &&
                   statement.binding.bindingKind == frontend::BindingKind::Let;
    }
    CHECK(saw_var, "var records BindingKind::Var for a local binding");
    CHECK(saw_let, "let records BindingKind::Let for a local binding");

    auto global = frontend::parse("global g: i32 = 1;\n");
    CHECK(!global.diagnostics().empty(), "'global' is rejected in Zith--");
    bool global_diagnostic = false;
    bool recovered_global  = false;
    for (const auto &diagnostic : global.diagnostics()) {
        global_diagnostic |=
            diagnostic.code == diagnostics::err::UnsupportedSyntax &&
            diagnostic.message.find("'global' is not supported") != std::string::npos;
    }
    for (const auto &decl : global.declarations()) {
        recovered_global |= decl.kind == frontend::DeclKind::Variable && decl.name == "g";
    }
    CHECK(global_diagnostic, "'global' reports a Zith-- UnsupportedSyntax diagnostic");
    CHECK(recovered_global, "the rejected global declaration is still recovered in the AST");
}

static void test_const_struct_fields() {
    auto snapshot = frontend::parse("struct P { const X: i32 = 1, y: i32 }\n");
    CHECK(snapshot.diagnostics().empty(), "const struct field parses cleanly");
    CHECK_EQ(snapshot.declarations().size(), 1u, "one struct declaration");
    const auto &fields = snapshot.declarations()[0].parameters;
    CHECK_EQ(fields.size(), 2u, "struct keeps const and regular fields");
    CHECK(fields[0].isConstField, "const field records isConstField");
    CHECK(static_cast<bool>(fields[0].defaultValue), "const field initializer is preserved");
    CHECK(!fields[1].isConstField, "regular field is not storage-const");

    auto missing = frontend::parse("struct P { const X: i32 }\n");
    CHECK(!missing.diagnostics().empty(), "const struct field requires an initializer");
    bool has_reason = false;
    for (const auto &diagnostic : missing.diagnostics()) {
        has_reason |= diagnostic.code == diagnostics::err::UnsupportedSyntax &&
                      diagnostic.message.find("const struct field requires an initializer") !=
                          std::string::npos;
    }
    CHECK(has_reason, "missing const field value reports the Zith-- reason");
}

static void test_zith_removed_bindings_and_qualifiers() {
    for (const std::string_view source :
         {"const x: i32;\n", "fn f(p: unique P): i32 { p.x }\n", "fn f(p: share P): i32 { p.x }\n",
          "fn f(p: belong P): i32 { p.x }\n", "fn f(p: mut i32): i32 { p }\n"}) {
        auto snapshot = frontend::parse(std::string(source));
        CHECK(!snapshot.diagnostics().empty(),
              (std::string("removed Zith-- syntax reports a diagnostic: ") + std::string(source))
                  .c_str());
        bool unsupported = false;
        for (const auto &diagnostic : snapshot.diagnostics())
            unsupported |= diagnostic.code == diagnostics::err::UnsupportedSyntax;
        CHECK(unsupported, (std::string("removed Zith-- syntax reports UnsupportedSyntax: ") +
                            std::string(source))
                               .c_str());
    }
}

static void test_type_alias_and_struct_enum_union() {
    auto snapshot = frontend::parse("type Age = i32;\n"
                                    "struct Point { x: i32, y: i32 }\n"
                                    "enum Color { Red, Green, Blue }\n"
                                    "union Value { i32, f64 }\n"
                                    "raw union Bits { u8, u32 }\n");

    CHECK(snapshot.diagnostics().empty(), "type declarations lower without diagnostics");
    CHECK_EQ(snapshot.declarations().size(), 5u, "five declarations are lowered");
    CHECK_EQ(snapshot.declarations()[0].kind, frontend::DeclKind::TypeAlias,
             "type is lowered as a type alias");
    CHECK_EQ(snapshot.declarations()[0].name, std::string("Age"), "type alias name is preserved");
    CHECK_EQ(snapshot.declarations()[1].kind, frontend::DeclKind::Struct,
             "struct is lowered as a struct declaration");
    CHECK_EQ(snapshot.declarations()[1].name, std::string("Point"), "struct name is preserved");
    CHECK_EQ(snapshot.declarations()[2].kind, frontend::DeclKind::Enum,
             "enum is lowered as an enum declaration");
    CHECK_EQ(snapshot.declarations()[2].name, std::string("Color"), "enum name is preserved");
    CHECK_EQ(snapshot.declarations()[3].kind, frontend::DeclKind::Union,
             "union is lowered as a union declaration");
    CHECK_EQ(snapshot.declarations()[3].name, std::string("Value"), "union name is preserved");
    CHECK(!snapshot.declarations()[3].isRawUnion, "plain union is not flagged raw");
    CHECK_EQ(snapshot.declarations()[3].parameters.size(), 2u,
             "union member types are retained positionally");
    CHECK(snapshot.declarations()[3].parameters[0].type,
          "union member type expression is retained");
    CHECK(snapshot.declarations()[3].parameters[0].name.empty() &&
              snapshot.declarations()[3].parameters[1].name.empty(),
          "positional union members carry no field names");
    CHECK_EQ(snapshot.declarations()[4].kind, frontend::DeclKind::Union,
             "raw union is lowered as a union declaration");
    CHECK_EQ(snapshot.declarations()[4].name, std::string("Bits"), "raw union name is preserved");
    CHECK(snapshot.declarations()[4].isRawUnion, "raw union is flagged raw");
    CHECK_EQ(snapshot.declarations()[4].parameters.size(), 2u,
             "raw union member types are retained");
}

static void test_function_type_expression() {
    auto snapshot = frontend::parse("alias Callback = fn(i32): i32;\n"
                                    "fn main() {\n"
                                    "    var f: fn(i32): i32 = 0;\n"
                                    "}\n");
    CHECK(snapshot.diagnostics().empty(), "fn(...): R type expressions parse cleanly");

    const frontend::TypeExpression *function_type = nullptr;
    for (const auto &type : snapshot.typeExpressions()) {
        if (type.kind == frontend::TypeExprKind::Function)
            function_type = &type;
    }
    CHECK(function_type != nullptr, "fn type is lowered to TypeExprKind::Function");
    if (function_type != nullptr) {
        CHECK_EQ(function_type->arguments.size(), 2u,
                 "function type stores params plus the result as its last argument");
        CHECK_EQ(function_type->arguments[1].value, function_type->arguments[0].value + 1u,
                 "i32 parameter and i32 result are distinct type expressions");
    }

    auto malformed = frontend::parse("alias Bad = fn(i32) i32;\n");
    CHECK(!malformed.diagnostics().empty(),
          "a function type missing ':' is rejected at parse time");
}

static void test_slice_range_expression() {
    auto snapshot = frontend::parse("fn main(): ?[]i32 {\n"
                                    "    var values: [3]i32 = [10, 20, 30];\n"
                                    "    return values[1..3];\n"
                                    "}\n");
    CHECK(snapshot.diagnostics().empty(), "array slice bounds parse cleanly");

    const frontend::Expression *slice = nullptr;
    for (const auto &expression : snapshot.expressions()) {
        if (expression.kind == frontend::ExprKind::SliceRange)
            slice = &expression;
    }
    CHECK(slice != nullptr, "arr[lo..hi] lowers to ExprKind::SliceRange");
    if (slice != nullptr) {
        CHECK_EQ(slice->operands.size(), 3u,
                 "slice expression stores object, lower bound, and upper bound");
        CHECK(slice->text == "..", "slice expression records the range spelling");
    }

    auto malformed = frontend::parse("fn main() {\n"
                                     "    var values: [3]i32 = [10, 20, 30];\n"
                                     "    values[1..3\n"
                                     "}\n");
    CHECK(!malformed.diagnostics().empty(), "an unterminated slice range is rejected");
}

static void test_raw_index_slice_expression() {
    auto snapshot = frontend::parse("fn main(): i32 {\n"
                                    "    var values: [3]i32 = [10, 20, 30];\n"
                                    "    let s: []i32 = raw values[1..3];\n"
                                    "    return raw s[0];\n"
                                    "}\n");
    CHECK(snapshot.diagnostics().empty(), "raw index/slice expressions parse cleanly");
    bool raw_slice = false;
    bool raw_index = false;
    for (const auto &expression : snapshot.expressions()) {
        if (expression.kind == frontend::ExprKind::SliceRange)
            raw_slice = expression.is_raw;
        if (expression.kind == frontend::ExprKind::Index)
            raw_index |= expression.is_raw;
    }
    CHECK(raw_slice, "raw slice range retains the raw marker");
    CHECK(raw_index, "raw index retains the raw marker");
}

static void test_unary_and_nested_expressions() {
    auto snapshot = frontend::parse("fn calc(x: i32): i32 {\n"
                                    "    return -x;\n"
                                    "}\n");

    CHECK(snapshot.diagnostics().empty(), "unary expression lowers without diagnostics");
    bool found_unary = false;
    for (const auto &expression : snapshot.expressions()) {
        if (expression.kind == frontend::ExprKind::Unary && expression.text == "-")
            found_unary = true;
    }
    CHECK(found_unary, "unary negation is lowered to an expression node");
}

static void test_multiple_top_level_decls_with_visibility() {
    auto snapshot = frontend::parse("pub fn main() {}\n"
                                    "mod fn helper(): i32 { 42 }\n"
                                    "fn internal() {}\n");

    CHECK(snapshot.diagnostics().empty(), "multiple visibility-hinted decls lower without errors");
    CHECK_EQ(snapshot.declarations().size(), 3u, "three top-level function declarations");
    CHECK_EQ(snapshot.declarations()[0].visibility, frontend::Visibility::Public,
             "pub fn has public visibility");
    CHECK_EQ(snapshot.declarations()[1].visibility, frontend::Visibility::Module,
             "mod fn has module visibility");
    CHECK_EQ(snapshot.declarations()[2].visibility, frontend::Visibility::Private,
             "unqualified fn has private visibility");
}

static void test_import_form_with_depth_and_export() {
    auto snapshot = frontend::parse("import core/math(2) as math\n"
                                    "export io/fs\n");

    CHECK(snapshot.diagnostics().empty(), "import with depth and export lower without errors");
    CHECK_EQ(snapshot.declarations().size(), 2u, "two import declarations");
    CHECK_EQ(snapshot.declarations()[0].import.depth, 2, "depth is retained for import");
    CHECK(snapshot.declarations()[1].import.isExport, "export import flag is retained");
}

static void test_error_diagnostic_span_preserved() {
    auto snapshot = frontend::parse("fn broken(}\n"
                                    "pub fn ok(): i32 { 0 }\n");

    CHECK(!snapshot.diagnostics().empty(), "bad syntax still produces a diagnostic");
    CHECK(snapshot.declarations().size() >= 1u, "error recovery produces at least one declaration");
    bool found_ok = false;
    for (const auto &d : snapshot.declarations()) {
        if (d.name == "ok" && d.kind == frontend::DeclKind::Function)
            found_ok = true;
    }
    CHECK(found_ok, "valid declaration after delimiter error is preserved");
}

static void test_unexpected_top_level_token_is_diagnosed() {
    // `@` is legitimate punctuation (for @offsetOf and macro calls), so @@@ lexes
    // cleanly; the top-level lowerer must report the garbage and still collect the
    // following declaration.
    auto snapshot = frontend::parse("@@@ fn good() { }\n");

    CHECK(!snapshot.diagnostics().empty(), "@@@ garbage reports a diagnostic");
    CHECK(hasErrorCode(snapshot, diagnostics::err::UnsupportedSyntax),
          "unexpected top-level token reports UnsupportedSyntax");
    bool found_good = false;
    for (const auto &d : snapshot.declarations()) {
        if (d.name == "good" && d.kind == frontend::DeclKind::Function)
            found_good = true;
    }
    CHECK(found_good, "valid declaration after garbage is still collected");
}

static void test_macro_invocation_is_tolerated() {
    // A top-level macro invocation `@name args;` stays tolerated (the spec shows
    // `@appendField Custom, x: i32;` before a declaration).
    auto snapshot = frontend::parse("@appendField Custom, x: i32;\n"
                                    "pub fn main() { }\n");

    CHECK(snapshot.diagnostics().empty(), "@appendField macro invocation reports no error");
    CHECK_EQ(snapshot.declarations().size(), 1u, "macro invocation adds no declaration");
    CHECK_EQ(snapshot.declarations()[0].name, std::string("main"),
             "declaration after macro invocation is preserved");
}

static void test_extern_before_declaration_is_tolerated() {
    auto snapshot = frontend::parse("extern fn puts(msg: *char)\n"
                                    "fn main() { }\n");

    CHECK(snapshot.diagnostics().empty(), "extern before a declaration reports no error");
    bool found_puts = false;
    bool found_main = false;
    for (const auto &d : snapshot.declarations()) {
        if (d.name == "puts" && d.kind == frontend::DeclKind::Function)
            found_puts = true;
        if (d.name == "main" && d.kind == frontend::DeclKind::Function)
            found_main = true;
    }
    CHECK(found_puts && found_main, "both extern and plain declarations are lowered");
}

static void test_function_kinds() {
    auto snapshot = frontend::parse("fn standard() {}\n"
                                    "raw fn unsafe_op() {}\n"
                                    "extern fn putchar(c: i32): i32\n"
                                    "state structured(): i32 { return 0; }\n");

    CHECK(snapshot.diagnostics().empty(),
          "supported Zith-- function kinds parse without diagnostics");
    CHECK_EQ(snapshot.declarations().size(), 4u,
             "supported function kinds produce one declaration each");

    if (snapshot.declarations().size() != 4u)
        return;

    CHECK_EQ(snapshot.declarations()[0].kind, frontend::DeclKind::Function,
             "plain fn is a function declaration");
    CHECK_EQ(snapshot.declarations()[0].functionKind, frontend::FunctionKind::Standard,
             "plain fn records Standard");
    CHECK_EQ(snapshot.declarations()[0].name, std::string("standard"),
             "plain fn keeps the function name");

    CHECK_EQ(snapshot.declarations()[1].kind, frontend::DeclKind::Function,
             "raw fn is a function declaration");
    CHECK_EQ(snapshot.declarations()[1].functionKind, frontend::FunctionKind::Raw,
             "raw fn records Raw");
    CHECK_EQ(snapshot.declarations()[1].name, std::string("unsafe_op"),
             "raw fn keeps the function name");

    CHECK_EQ(snapshot.declarations()[2].kind, frontend::DeclKind::Function,
             "extern fn is a function declaration");
    CHECK_EQ(snapshot.declarations()[2].functionKind, frontend::FunctionKind::Extern,
             "extern fn records Extern");
    CHECK(snapshot.declarations()[2].isExtern, "extern fn keeps the C-ABI flag");
    CHECK_EQ(snapshot.declarations()[2].name, std::string("putchar"),
             "extern fn keeps the function name");

    CHECK_EQ(snapshot.declarations()[3].kind, frontend::DeclKind::Function,
             "state is a function declaration");
    CHECK_EQ(snapshot.declarations()[3].functionKind, frontend::FunctionKind::State,
             "state records State");
    CHECK_EQ(snapshot.declarations()[3].name, std::string("structured"),
             "state keeps the function name");

    auto const_fn = frontend::parse("const fn compile_ready() {}\n");
    CHECK(!const_fn.diagnostics().empty(), "'const fn' is rejected in Zith--");
    bool const_fn_diagnostic = false;
    for (const auto &diagnostic : const_fn.diagnostics()) {
        const_fn_diagnostic |=
            diagnostic.code == diagnostics::err::UnsupportedSyntax &&
            diagnostic.message.find("'const fn' is not supported") != std::string::npos;
    }
    CHECK(const_fn_diagnostic, "'const fn' reports a Zith-- UnsupportedSyntax diagnostic");
}

static void test_function_kind_combinations_are_rejected() {
    for (const std::string_view source :
         {"raw const fn bad() {}\n", "const raw fn bad() {}\n", "extern raw fn bad()\n",
          "raw extern fn bad()\n", "state const fn bad(): i32 { return 0; }\n"}) {
        auto snapshot = frontend::parse(std::string(source));
        CHECK(!snapshot.diagnostics().empty(),
              (std::string("combined function-kind prefixes produce a diagnostic: ") +
               std::string(source))
                  .c_str());
    }
}

static void test_function_kind_methods_propagate() {
    auto snapshot = frontend::parse("struct Counter {\n"
                                    "    value: i32,\n"
                                    "    raw fn unsafeTick(self): i32 { 0 }\n"
                                    "}\n"
                                    "implement Counter {\n"
                                    "    raw fn implementUnsafe(self): i32 { 0 }\n"
                                    "}\n");

    CHECK(snapshot.diagnostics().empty(),
          "supported method function kinds parse without diagnostics");

    bool saw_raw  = false;
    bool saw_impl = false;
    for (const auto &decl : snapshot.declarations()) {
        if (decl.kind != frontend::DeclKind::Function)
            continue;
        if (decl.name == "unsafeTick" || decl.name == "implementUnsafe")
            saw_raw |= decl.functionKind == frontend::FunctionKind::Raw;
        if (decl.name == "implementUnsafe")
            saw_impl = decl.functionKind == frontend::FunctionKind::Raw;
    }
    CHECK(saw_raw, "raw method functions record Raw");
    CHECK(saw_impl, "implement-block methods propagate their function kind");

    auto const_method =
        frontend::parse("struct Counter { const fn constValue(self): i32 { 0 } }\n");
    CHECK(!const_method.diagnostics().empty(), "'const fn' methods are rejected in Zith--");
    bool const_method_diagnostic = false;
    for (const auto &diagnostic : const_method.diagnostics()) {
        const_method_diagnostic |=
            diagnostic.code == diagnostics::err::UnsupportedSyntax &&
            diagnostic.message.find("'const fn' is not supported") != std::string::npos;
    }
    CHECK(const_method_diagnostic, "rejected const methods report the Zith-- reason");
}

static void test_consecutive_garbage_coalesces_into_one_diagnostic() {
    auto snapshot = frontend::parse("$ $ $ fn ok() { }\n");

    size_t unsupported_count = 0;
    for (const auto &diagnostic : snapshot.diagnostics()) {
        if (!diagnostic.isWarning && diagnostic.code == diagnostics::err::UnsupportedSyntax)
            ++unsupported_count;
    }
    CHECK_EQ(unsupported_count, 1u,
             "a run of consecutive garbage tokens yields a single diagnostic");
    bool found_ok = false;
    for (const auto &d : snapshot.declarations()) {
        if (d.name == "ok" && d.kind == frontend::DeclKind::Function)
            found_ok = true;
    }
    CHECK(found_ok, "declaration after coalesced garbage is still collected");
}

static void test_struct_field_syntax_diagnostics() {
    auto eq = frontend::parse("struct S { x = 5 }\n");
    CHECK_EQ(eq.diagnostics().size(), 1u, "field = expr reports one unsupported-syntax error");
    CHECK_EQ(eq.diagnostics()[0].code, diagnostics::err::UnsupportedSyntax,
             "field = expr uses UnsupportedSyntax");
    CHECK(eq.diagnostics()[0].message.find("unsupported: field 'x = <expr>'") != std::string::npos,
          "field = expr diagnostic names the field");

    auto missing = frontend::parse("struct S { x }\n");
    CHECK_EQ(missing.diagnostics().size(), 1u,
             "a field without ':' or '=' reports one expected-colon error");
    CHECK(missing.diagnostics()[0].message.find("expected ':' after field name 'x'") !=
              std::string::npos,
          "field without type reports expected ':'");

    auto mixed = frontend::parse("struct S { x: i32 = 5, y = 6 }\n");
    CHECK_EQ(mixed.diagnostics().size(), 1u,
             "mixing a valid typed default with field = expr keeps only the real diagnostic");
    CHECK(mixed.diagnostics()[0].message.find("unsupported: field 'y = <expr>'") !=
              std::string::npos,
          "the unsupported-field diagnostic names the offending field");
}

static void test_nested_state_declarations() {
    auto snapshot = frontend::parse("fn run(): i32 {\n"
                                    "    state Loop(n: i32): i32 {\n"
                                    "        jump Done(n + 1);\n"
                                    "    }\n"
                                    "    state Done(n: i32): i32 {\n"
                                    "        return n;\n"
                                    "    }\n"
                                    "    return dock Loop(0);\n"
                                    "}\n");

    CHECK(snapshot.diagnostics().empty(), "local states parse without diagnostics");
    size_t local_states = 0;
    bool saw_marker     = false;
    for (const auto &decl : snapshot.declarations()) {
        if (decl.kind == frontend::DeclKind::Function &&
            decl.functionKind == frontend::FunctionKind::State && !decl.parentName.empty()) {
            ++local_states;
            CHECK_EQ(decl.parentName, std::string("run"),
                     "local state records the owning function name");
            CHECK(decl.parentScope, "local state records the owning body scope");
            CHECK_EQ(decl.visibility, frontend::Visibility::Private,
                     "local states are always private");
        }
    }
    CHECK_EQ(local_states, 2u, "both local states are flat frontend declarations");
    for (const auto &stmt : snapshot.statements()) {
        saw_marker |= stmt.kind == frontend::StmtKind::Declaration;
    }
    CHECK(saw_marker, "local states are represented by marker statements");

    auto nested = frontend::parse("state A(): i32 {\n"
                                  "    state B(): i32 { return 0; }\n"
                                  "    return 1;\n"
                                  "}\n");
    CHECK(hasErrorCode(nested, diagnostics::err::UnsupportedSyntax),
          "state inside state reports controlled recovery");

    for (const std::string_view unsupported :
         {"fn helper() {}\n", "struct S { x: i32 }\n", "enum E { X }\n"}) {
        auto bad =
            frontend::parse(std::string("fn outer() {\n") + std::string(unsupported) + "}\n");
        CHECK(hasErrorCode(bad, diagnostics::err::UnsupportedSyntax),
              "nested fn/struct/enum report controlled unsupported syntax");
    }
}

static void test_defer_statement_syntax() {
    auto snapshot = frontend::parse("fn main() {\n"
                                    "    defer release();\n"
                                    "    defer { first(); second(); }\n"
                                    "}\n");
    CHECK(snapshot.diagnostics().empty(), "defer expr and defer block parse without diagnostics");
    int defer_exprs  = 0;
    int defer_blocks = 0;
    bool saw_defer   = false;
    for (const auto &stmt : snapshot.statements()) {
        if (stmt.kind != frontend::StmtKind::Defer)
            continue;
        saw_defer = true;
        if (!stmt.expression)
            continue;
        const auto &body = snapshot.expressions()[stmt.expression.value - 1U];
        if (body.kind == frontend::ExprKind::Block) {
            ++defer_blocks;
        } else {
            ++defer_exprs;
        }
    }
    CHECK(saw_defer, "defer lowers to StmtKind::Defer");
    CHECK_EQ(defer_exprs, 1, "defer expression statement is retained");
    CHECK_EQ(defer_blocks, 1, "defer block statement is retained and stores a block expression");

    auto keyword = frontend::parse("defer");
    CHECK_EQ(keyword.tokens()[0].kind, frontend::TokenKind::Keyword,
             "defer is a recognized keyword");

    auto bad = frontend::parse("fn main() {\n"
                               "    defer return;\n"
                               "}\n");
    CHECK(hasErrorCode(bad, diagnostics::err::ExpectedExpr),
          "defer return reports a controlled parse diagnostic");

    auto bad_break = frontend::parse("fn main() {\n"
                                     "    for (true) { defer break; }\n"
                                     "}\n");
    CHECK(hasErrorCode(bad_break, diagnostics::err::ExpectedExpr),
          "defer break reports a controlled parse diagnostic");
}

static void test_state_without_return_type_parses() {
    auto snapshot = frontend::parse("state Done() { return; }\n"
                                    "state Start() { jump Done(); }\n"
                                    "fn main() { dock Start(); }\n");
    CHECK(snapshot.diagnostics().empty(), "state without a return type parses as void");
    for (const auto &decl : snapshot.declarations()) {
        if (decl.kind == frontend::DeclKind::Function &&
            decl.functionKind == frontend::FunctionKind::State) {
            CHECK(!decl.declaredType, "state without ': T' keeps declaredType empty");
        }
    }
}

static void test_struct_field_visibility_parses() {
    auto snapshot = frontend::parse("struct S {\n"
                                    "    data: i32,\n"
                                    "    pub open: i32,\n"
                                    "    mod sibling: i32,\n"
                                    "    mod(2) deep: i32,\n"
                                    "    mod(..) universe: i32,\n"
                                    "    [x, y]: i32,\n"
                                    "}\n");
    CHECK(snapshot.diagnostics().empty(), "field visibility prefixes parse cleanly");

    const frontend::Declaration *decl = nullptr;
    for (const auto &candidate : snapshot.declarations()) {
        if (candidate.kind == frontend::DeclKind::Struct && candidate.name == "S") {
            decl = &candidate;
            break;
        }
    }
    CHECK(decl != nullptr, "the struct declaration is present");
    if (decl == nullptr)
        return;
    CHECK_EQ(decl->parameters.size(), 7u, "all fields retain their expanded parameters");
    if (decl->parameters.size() != 7u)
        return;
    CHECK_EQ(decl->parameters[0].visibility, frontend::Visibility::Private,
             "a field without a prefix is private");
    CHECK_EQ(decl->parameters[1].visibility, frontend::Visibility::Public, "'pub' opens a field");
    CHECK_EQ(decl->parameters[2].visibility, frontend::Visibility::Module,
             "'mod' uses module visibility");
    CHECK_EQ(decl->parameters[2].modDepth, 0, "'mod' defaults to depth zero");
    CHECK_EQ(decl->parameters[3].visibility, frontend::Visibility::Module,
             "'mod(2)' uses module visibility");
    CHECK_EQ(decl->parameters[3].modDepth, 2, "'mod(2)' records its depth");
    CHECK_EQ(decl->parameters[4].visibility, frontend::Visibility::Module,
             "'mod(..)' uses module visibility");
    CHECK_EQ(decl->parameters[4].modDepth, -1, "'mod(..)' records unlimited depth");
    CHECK_EQ(decl->parameters[5].visibility, frontend::Visibility::Private,
             "a grouped field without a prefix is private");
    CHECK_EQ(decl->parameters[5].modDepth, 0, "grouped fields default to no module depth");
}

static void test_external_symbol_aliases() {
    auto snapshot =
        frontend::parse("struct Window {}\n"
                        "fn destroy(self: lend Window): void = extern SDL_DestroyWindow;\n"
                        "implement Window {\n"
                        "    fn createRenderer(self): ?*Renderer = extern SDL_CreateRenderer;\n"
                        "}\n");
    CHECK(snapshot.diagnostics().empty(),
          "external symbol aliases parse at top level and in implement blocks");
    if (snapshot.diagnostics().empty()) {
        const frontend::Declaration *destroy = nullptr;
        const frontend::Declaration *create  = nullptr;
        for (const auto &decl : snapshot.declarations()) {
            if (decl.kind != frontend::DeclKind::Function)
                continue;
            if (decl.name == "destroy")
                destroy = &decl;
            else if (decl.name == "createRenderer")
                create = &decl;
        }
        CHECK(destroy != nullptr, "top-level external alias declaration is present");
        if (destroy != nullptr) {
            CHECK_EQ(destroy->externalSymbol, std::string("SDL_DestroyWindow"),
                     "top-level alias records the C symbol");
            CHECK(!destroy->body, "external alias has no body");
            CHECK(!destroy->isExtern, "external alias keeps Zith function semantics");
        }
        CHECK(create != nullptr, "implement-block external alias is present");
        if (create != nullptr)
            CHECK_EQ(create->externalSymbol, std::string("SDL_CreateRenderer"),
                     "implement-block alias records the C symbol");
    }

    auto missing = frontend::parse("fn bad(): i32 = extern;\n");
    CHECK(!missing.diagnostics().empty(), "external alias without a C identifier is rejected");
    bool has_missing = false;
    for (const auto &diag : missing.diagnostics())
        has_missing |= diag.message.find("requires an identifier") != std::string::npos;
    CHECK(has_missing, "missing identifier diagnostic is specific");

    auto trait_alias = frontend::parse("trait T { fn run(self) = extern doRun; }\n"
                                       "interface I { fn run(self) = extern doRun; }\n"
                                       "const fn bad(): i32 = extern badC;\n"
                                       "state S(): i32 = extern badState;\n");
    CHECK(!trait_alias.diagnostics().empty(),
          "external aliases are rejected in trait/interface/const/state");
}

static void test_frontend() {
    test_lossless_trivia_and_spans();
    test_keywords_and_module_ast();
    test_bare_opaque_type_expression();
    test_recovery_creates_error_nodes();
    test_function_body_ast();
    test_control_flow_and_scopes();
    test_state_and_dock_jump_syntax();
    test_old_state_machine_syntax_is_rejected();
    test_while_is_deprecated();
    test_return_expression_requires_semicolon();
    test_multi_char_operators_are_single_tokens();
    test_binary_comparison_expression();
    test_cast_expression();
    test_is_null_expression();
    test_is_type_expression();
    test_for_loop_forms();
    test_loop_labels_and_not_negation();
    test_default_parameters_and_condition_syntax();
    test_else_condition_forms();
    test_for_var_var_reports_specific_diagnostic();
    test_structured_imports();
    test_c_header_import_syntax();
    test_variable_declarations();
    test_const_struct_fields();
    test_zith_removed_bindings_and_qualifiers();
    test_type_alias_and_struct_enum_union();
    test_unary_and_nested_expressions();
    test_multiple_top_level_decls_with_visibility();
    test_import_form_with_depth_and_export();
    test_error_diagnostic_span_preserved();
    test_unexpected_top_level_token_is_diagnosed();
    test_macro_invocation_is_tolerated();
    test_extern_before_declaration_is_tolerated();
    test_function_type_expression();
    test_slice_range_expression();
    test_raw_index_slice_expression();
    test_function_kinds();
    test_function_kind_combinations_are_rejected();
    test_function_kind_methods_propagate();
    test_consecutive_garbage_coalesces_into_one_diagnostic();
    test_struct_field_syntax_diagnostics();
    test_nested_state_declarations();
    test_defer_statement_syntax();
    test_state_without_return_type_parses();
    test_struct_field_visibility_parses();
    test_external_symbol_aliases();
}

TEST_MAIN(frontend)
