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

static void test_flow_marker_and_dock_statements() {
    auto snapshot = frontend::parse("flow fn main(): i32 {\n"
                                    "    dock {\n"
                                    "        jump check;\n"
                                    "    }\n"
                                    "    marker check {\n"
                                    "        dock {\n"
                                    "            jump body;\n"
                                    "        }\n"
                                    "    }\n"
                                    "    stackful marker body {\n"
                                    "        return 1;\n"
                                    "    }\n"
                                    "}\n");

    CHECK(snapshot.diagnostics().empty(), "dock and marker statements parse without diagnostics");

    const auto &statements = snapshot.statements();
    CHECK(statements.size() >= 5u, "flow statements are retained in the frontend AST");

    bool saw_dock  = false;
    bool saw_jump  = false;
    bool saw_check = false;
    bool saw_body  = false;
    for (const auto &statement : statements) {
        if (statement.kind == frontend::StmtKind::Dock)
            saw_dock = true;
        else if (statement.kind == frontend::StmtKind::Jump)
            saw_jump = true;
        else if (statement.kind == frontend::StmtKind::Marker && statement.label == "check")
            saw_check = true;
        else if (statement.kind == frontend::StmtKind::Marker && statement.label == "body")
            saw_body = true;
    }
    CHECK(saw_dock && saw_jump && saw_check && saw_body,
          "dock, jump, and marker statements are lowered");
    CHECK(statements.back().kind == frontend::StmtKind::Marker && statements.back().isStackful,
          "stackful modifier is retained on marker statements");
}

static void test_dock_requires_block_form() {
    auto snapshot = frontend::parse("flow fn main() {\n"
                                    "    dock target;\n"
                                    "}\n");

    CHECK(!snapshot.diagnostics().empty(), "dock target; does not parse as a shortcut dock form");
    bool parsed_as_dock = false;
    for (const auto &statement : snapshot.statements()) {
        if (statement.kind == frontend::StmtKind::Dock)
            parsed_as_dock = true;
    }
    CHECK(!parsed_as_dock, "dock target; is not lowered as a valid dock statement");
}

static void test_plain_marker_is_stackless_by_default() {
    auto snapshot = frontend::parse("flow fn main(): i32 {\n"
                                    "    var x: i32 = 0;\n"
                                    "    dock {\n"
                                    "        jump body;\n"
                                    "    }\n"
                                    "    marker body {\n"
                                    "        return x;\n"
                                    "    }\n"
                                    "}\n");

    CHECK(snapshot.diagnostics().empty(), "plain marker parses without diagnostics");
    bool saw_plain_marker = false;
    for (const auto &statement : snapshot.statements()) {
        if (statement.kind == frontend::StmtKind::Marker && statement.label == "body")
            saw_plain_marker = true;
        if (statement.kind == frontend::StmtKind::Marker)
            CHECK(!statement.isStackful, "plain markers keep isStackful == false");
    }
    CHECK(saw_plain_marker, "plain marker is present in the frontend AST");
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

static void test_is_other_type_is_rejected() {
    auto snapshot = frontend::parse("fn check(p: ?*i32): bool {\n"
                                    "    return p is i32;\n"
                                    "}\n");

    CHECK(!snapshot.diagnostics().empty(), "'is' with a type operand produces a diagnostic");
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
    CHECK(!iterator.diagnostics().empty(),
          "the iterator form of 'for' reports it is unimplemented");
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
    auto snapshot = frontend::parse("var x: i32 = 42;\n"
                                    "let y = 100;\n"
                                    "const z: f64 = 3.14;\n"
                                    "global g: i32 = 1;\n");

    CHECK(snapshot.diagnostics().empty(), "variable declarations lower without diagnostics");
    CHECK_EQ(snapshot.declarations().size(), 4u, "four variable declarations are lowered");
    CHECK_EQ(snapshot.declarations()[0].kind, frontend::DeclKind::Variable,
             "var keyword produces a variable declaration");
    CHECK_EQ(snapshot.declarations()[1].kind, frontend::DeclKind::Variable,
             "let keyword produces a variable declaration");
    CHECK_EQ(snapshot.declarations()[2].kind, frontend::DeclKind::Variable,
             "const keyword produces a variable declaration");
    CHECK_EQ(snapshot.declarations()[3].kind, frontend::DeclKind::Variable,
             "global keyword produces a variable declaration");
    CHECK_EQ(snapshot.declarations()[0].name, std::string("x"), "var name is preserved");
    CHECK_EQ(snapshot.declarations()[1].name, std::string("y"), "let name is preserved");
    CHECK_EQ(snapshot.declarations()[2].name, std::string("z"), "const name is preserved");
    CHECK_EQ(snapshot.declarations()[3].name, std::string("g"), "global name is preserved");
}

static void test_type_alias_and_struct_enum_union() {
    auto snapshot = frontend::parse("type Age = i32;\n"
                                    "struct Point { x: i32, y: i32 }\n"
                                    "enum Color { Red, Green, Blue }\n"
                                    "union Value { Int: i32, Float: f64 }\n");

    CHECK(snapshot.diagnostics().empty(), "type declarations lower without diagnostics");
    CHECK_EQ(snapshot.declarations().size(), 4u, "four declarations are lowered");
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

static bool hasErrorCode(const frontend::FrontendSnapshot &snapshot, uint32_t code) {
    for (const auto &diagnostic : snapshot.diagnostics())
        if (!diagnostic.isWarning && diagnostic.code == code)
            return true;
    return false;
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
                                    "const fn compile_ready() {}\n"
                                    "extern fn putchar(c: i32): i32\n"
                                    "flow fn structured() {}\n");

    CHECK(snapshot.diagnostics().empty(), "all five function kinds parse as functions");
    CHECK_EQ(snapshot.declarations().size(), 5u, "all five kinds produce one declaration each");

    if (snapshot.declarations().size() != 5u)
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
             "const fn is a function declaration");
    CHECK_EQ(snapshot.declarations()[2].functionKind, frontend::FunctionKind::Const,
             "const fn records Const");
    CHECK_EQ(snapshot.declarations()[2].name, std::string("compile_ready"),
             "const fn no longer names a binding named fn");

    CHECK_EQ(snapshot.declarations()[3].kind, frontend::DeclKind::Function,
             "extern fn is a function declaration");
    CHECK_EQ(snapshot.declarations()[3].functionKind, frontend::FunctionKind::Extern,
             "extern fn records Extern");
    CHECK(snapshot.declarations()[3].isExtern, "extern fn keeps the C-ABI flag");
    CHECK_EQ(snapshot.declarations()[3].name, std::string("putchar"),
             "extern fn keeps the function name");

    CHECK_EQ(snapshot.declarations()[4].kind, frontend::DeclKind::Function,
             "flow fn is a function declaration");
    CHECK_EQ(snapshot.declarations()[4].functionKind, frontend::FunctionKind::Flow,
             "flow fn records Flow");
    CHECK_EQ(snapshot.declarations()[4].name, std::string("structured"),
             "flow fn keeps the function name");
}

static void test_function_kind_combinations_are_rejected() {
    for (const std::string_view source :
         {"raw const fn bad() {}\n", "const raw fn bad() {}\n", "extern raw fn bad()\n",
          "raw extern fn bad()\n", "flow const fn bad() {}\n"}) {
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
                                    "    const fn constValue(self): i32 { 0 }\n"
                                    "    flow fn flowTick(self) {}\n"
                                    "}\n"
                                    "implement Counter {\n"
                                    "    raw fn implementUnsafe(self): i32 { 0 }\n"
                                    "    const fn implementConst(self): i32 { 0 }\n"
                                    "    flow fn implementFlow(self) {}\n"
                                    "}\n");

    CHECK(snapshot.diagnostics().empty(), "method function kinds parse without diagnostics");

    bool saw_raw    = false;
    bool saw_const  = false;
    bool saw_flow   = false;
    bool saw_impl   = false;
    bool saw_impl_c = false;
    bool saw_impl_f = false;
    for (const auto &decl : snapshot.declarations()) {
        if (decl.kind != frontend::DeclKind::Function)
            continue;
        if (decl.name == "unsafeTick" || decl.name == "implementUnsafe")
            saw_raw |= decl.functionKind == frontend::FunctionKind::Raw;
        if (decl.name == "constValue" || decl.name == "implementConst")
            saw_const |= decl.functionKind == frontend::FunctionKind::Const;
        if (decl.name == "flowTick" || decl.name == "implementFlow")
            saw_flow |= decl.functionKind == frontend::FunctionKind::Flow;
        if (decl.name == "implementUnsafe")
            saw_impl = decl.functionKind == frontend::FunctionKind::Raw;
        if (decl.name == "implementConst")
            saw_impl_c = decl.functionKind == frontend::FunctionKind::Const;
        if (decl.name == "implementFlow")
            saw_impl_f = decl.functionKind == frontend::FunctionKind::Flow;
    }
    CHECK(saw_raw, "raw method functions record Raw");
    CHECK(saw_const, "const method functions record Const");
    CHECK(saw_flow, "flow method functions record Flow");
    CHECK(saw_impl && saw_impl_c && saw_impl_f,
          "implement-block methods propagate their function kind");
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

static void test_frontend() {
    test_lossless_trivia_and_spans();
    test_keywords_and_module_ast();
    test_recovery_creates_error_nodes();
    test_function_body_ast();
    test_control_flow_and_scopes();
    test_flow_marker_and_dock_statements();
    test_dock_requires_block_form();
    test_plain_marker_is_stackless_by_default();
    test_while_is_deprecated();
    test_multi_char_operators_are_single_tokens();
    test_binary_comparison_expression();
    test_cast_expression();
    test_is_null_expression();
    test_is_other_type_is_rejected();
    test_for_loop_forms();
    test_structured_imports();
    test_c_header_import_syntax();
    test_variable_declarations();
    test_type_alias_and_struct_enum_union();
    test_unary_and_nested_expressions();
    test_multiple_top_level_decls_with_visibility();
    test_import_form_with_depth_and_export();
    test_error_diagnostic_span_preserved();
    test_unexpected_top_level_token_is_diagnosed();
    test_macro_invocation_is_tolerated();
    test_extern_before_declaration_is_tolerated();
    test_function_kinds();
    test_function_kind_combinations_are_rejected();
    test_function_kind_methods_propagate();
    test_consecutive_garbage_coalesces_into_one_diagnostic();
    test_struct_field_syntax_diagnostics();
}

TEST_MAIN(frontend)
