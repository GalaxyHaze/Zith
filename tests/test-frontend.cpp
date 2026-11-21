#include "diagnostics/error-codes.hpp"
#include "frontend/frontend-printer.hpp"
#include "frontend/frontend.hpp"
#include "test-common.hpp"

#include <algorithm>
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

static void test_cst_dump_is_deterministic_and_keeps_nested_tokens() {
    const std::string source = "fn main() { foo([1]); }\n";
    const auto first         = frontend::parse(source);
    const auto second        = frontend::parse(source);
    const auto dump          = frontend::dumpCst(first);

    CHECK(dump.starts_with("--- CST ---\nRoot [0..24]"),
          "CST dump includes a root node with its source span");
    CHECK(dump.find("Token Identifier [12..15] \"foo\"") != std::string::npos,
          "CST dump includes token kinds, spans and lexemes");
    CHECK(dump.find("Token Punctuation [16..17] \"[\"") != std::string::npos &&
              dump.find("Token Punctuation [18..19] \"]\"") != std::string::npos,
          "CST dump preserves nested delimiter tokens");
    CHECK_EQ(dump, frontend::dumpCst(second), "equivalent CSTs have identical textual dumps");
    CHECK(dump.ends_with("---\n"), "CST dump has a deterministic closing delimiter");
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

static void test_kebab_case_qualified_type_path() {
    auto snapshot = frontend::parse("fn consume(value: std.memory.in-place.InPlace) {}\n");
    CHECK(snapshot.diagnostics().empty(),
          "kebab-case module segments parse in qualified type paths");

    const frontend::TypeExpression *qualified = nullptr;
    for (const auto &type : snapshot.typeExpressions()) {
        if (type.kind == frontend::TypeExprKind::Name && type.name == "std.memory.in-place.InPlace")
            qualified = &type;
    }
    CHECK(qualified != nullptr, "qualified type retains its complete kebab-case name");
    if (qualified != nullptr) {
        CHECK_EQ(qualified->segments.size(), 4u, "qualified type records each module/type segment");
        if (qualified->segments.size() == 4u)
            CHECK_EQ(qualified->segments[2], std::string("in-place"),
                     "contiguous hyphen joins tokens within one path segment");
    }
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
                                    "        jump Loop(...n);\n"
                                    "    }\n"
                                    "    return n;\n"
                                    "}\n"
                                    "fn main(): i32 {\n"
                                    "    let result: i32 = dock Loop(...0);\n"
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

    bool saw_dock         = false;
    bool dock_spread      = false;
    bool saw_jump         = false;
    bool jump_spread      = false;
    for (const auto &expr : snapshot.expressions())
        if (expr.kind == frontend::ExprKind::DockCall) {
            saw_dock = true;
            dock_spread = !expr.argIsSpread.empty() && expr.argIsSpread.front();
        }
    for (const auto &statement : snapshot.statements())
        if (statement.kind == frontend::StmtKind::Jump) {
            saw_jump = true;
            jump_spread = !statement.argumentIsSpread.empty() &&
                          statement.argumentIsSpread.front();
        }
    CHECK(saw_dock, "dock lowers as a DockCall expression");
    CHECK(saw_jump, "jump lowers as a Jump statement");
    CHECK(dock_spread, "dock retains the explicit spread marker");
    CHECK(jump_spread, "jump retains the explicit spread marker");

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

static void test_explicit_discard_statement() {
    auto snapshot = frontend::parse("fn main(): i32 {\n"
                                    "    make();\n"
                                    "    _ = make();\n"
                                    "    return 0;\n"
                                    "}\n");
    CHECK(snapshot.diagnostics().empty(),
          "call statement and explicit discard both parse without diagnostics");
    bool saw_call_statement = false;
    bool saw_discard        = false;
    for (const auto &statement : snapshot.statements()) {
        if (statement.kind == frontend::StmtKind::Expression)
            saw_call_statement = true;
        else if (statement.kind == frontend::StmtKind::Discard)
            saw_discard = true;
    }
    CHECK(saw_call_statement, "plain call statement remains StmtKind::Expression");
    CHECK(saw_discard, "`_ = make();` lowers to StmtKind::Discard");
}

static void test_attributes_parse_and_classify() {
    auto snapshot      = frontend::parse("#[discardable] #[volatile] fn make(): i32 {\n"
                                         "    #[volatile] let reg: i32 = 0;\n"
                                         "    return reg;\n"
                                         "}\n");
    bool saw_misplaced = false;
    for (const auto &diagnostic : snapshot.diagnostics()) {
        if (diagnostic.code == diagnostics::err::AttributeNotApplicable)
            saw_misplaced = true;
    }
    CHECK(saw_misplaced, "attribute source reports the misplaced volatile fn warning");
    const auto decl_it =
        std::find_if(snapshot.declarations().begin(), snapshot.declarations().end(),
                     [](const auto &decl) { return decl.kind == frontend::DeclKind::Function; });
    CHECK(decl_it != snapshot.declarations().end(), "attribute fn exists");
    if (decl_it != snapshot.declarations().end()) {
        CHECK(decl_it->attributes.size() == 2u, "fn carries both attributes");
        CHECK(decl_it->discardable(), "fn is discardable after classification");
    }
}

static void test_alignment_and_branch_attributes_parse() {
    auto snapshot = frontend::parse("#[alignas(64)] struct Aligned { value: u8 }\n"
                                    "#[noalias] fn make(): *i32 { return null; }\n"
                                    "#[likely] #[unlikely] fn branch() {}\n"
                                    "fn main() {\n"
                                    "    #[alignas(64)] let bytes: [2]u8 = [0, 0];\n"
                                    "}\n");
    CHECK(snapshot.diagnostics().empty(),
          "alignment and companion attributes parse without diagnostics");

    const auto struct_it =
        std::find_if(snapshot.declarations().begin(), snapshot.declarations().end(),
                     [](const auto &decl) { return decl.kind == frontend::DeclKind::Struct; });
    CHECK(struct_it != snapshot.declarations().end(), "aligned struct declaration is retained");
    if (struct_it != snapshot.declarations().end()) {
        CHECK_EQ(struct_it->attributes.size(), 1u, "struct retains its alignment attribute");
        if (!struct_it->attributes.empty()) {
            CHECK_EQ(struct_it->attributes.front().text, std::string("alignas"),
                     "struct attribute name is alignas");
            CHECK(struct_it->attributes.front().span.size() >
                      struct_it->attributes.front().text.size(),
                  "alignment attribute span includes its argument syntax");
        }
    }

    bool found_aligned_binding = false;
    for (const auto &statement : snapshot.statements()) {
        if (statement.kind != frontend::StmtKind::Binding || statement.binding.name != "bytes")
            continue;
        found_aligned_binding = true;
        CHECK_EQ(statement.binding.attributes.size(), 1u,
                 "array binding retains its alignment attribute");
        if (!statement.binding.attributes.empty()) {
            CHECK_EQ(statement.binding.attributes.front().text, std::string("alignas"),
                     "binding attribute name is alignas");
            CHECK(statement.binding.attributes.front().span.size() >
                      statement.binding.attributes.front().text.size(),
                  "binding alignment span includes its argument syntax");
            CHECK(static_cast<bool>(statement.binding.attributes.front().argument),
                  "binding alignment retains its argument expression");
        }
    }
    CHECK(found_aligned_binding, "aligned array binding is retained");

    const auto has_attribute = [&](const std::string_view name) {
        for (const auto &decl : snapshot.declarations())
            for (const auto &attribute : decl.attributes)
                if (attribute.text == name)
                    return true;
        return false;
    };
    CHECK(has_attribute("noalias"), "noalias is recognized on a function");
    CHECK(has_attribute("likely"), "likely is recognized on a declaration");
    CHECK(has_attribute("unlikely"), "unlikely is recognized on a declaration");
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

static std::size_t countErrorCode(const frontend::FrontendSnapshot &snapshot, uint32_t code) {
    std::size_t count = 0;
    for (const auto &diagnostic : snapshot.diagnostics())
        if (!diagnostic.isWarning && diagnostic.code == code)
            ++count;
    return count;
}

static bool hasErrors(const frontend::FrontendSnapshot &snapshot) {
    return std::any_of(
        snapshot.diagnostics().begin(), snapshot.diagnostics().end(),
        [](const frontend::Diagnostic &diagnostic) { return !diagnostic.isWarning; });
}

static bool hasErrorExpression(const frontend::FrontendSnapshot &snapshot) {
    return std::any_of(snapshot.expressions().begin(), snapshot.expressions().end(),
                       [](const frontend::Expression &expression) {
                           return expression.kind == frontend::ExprKind::Error;
                       });
}

static void test_parser_depth_limit() {
    constexpr std::size_t maxDepth = 128U;

    const auto parse_parenthesized = [](const std::size_t nesting) {
        std::string source = "fn nested(): i32 { return ";
        source.append(nesting, '(');
        source += "1";
        source.append(nesting, ')');
        source += "; }\nfn after_depth_error() {}\n";
        return frontend::parse(source);
    };
    const auto expressions_at_limit = parse_parenthesized(maxDepth - 2U);
    CHECK(!hasErrors(expressions_at_limit),
          "128 active parser entries accept nested parenthesized expressions");
    const auto expressions_over_limit = parse_parenthesized(maxDepth - 1U);
    CHECK_EQ(countErrorCode(expressions_over_limit, diagnostics::err::ParseDepthExceeded), 1U,
             "the first over-limit expression reports exactly one E1010");
    CHECK(hasErrorExpression(expressions_over_limit),
          "over-limit expression recovery creates an error expression");
    CHECK_EQ(expressions_over_limit.declarations().size(), 2U,
             "expression recovery preserves the following declaration");

    const auto parse_nested_type = [](const std::size_t nesting) {
        std::string source = "type Nested = ";
        for (std::size_t depth = 0; depth < nesting; ++depth)
            source += '?';
        source += "i32;\nfn after_depth_error() {}\n";
        return frontend::parse(source);
    };
    const auto types_at_limit = parse_nested_type(maxDepth - 1U);
    CHECK(types_at_limit.diagnostics().empty(), "128 active parser entries accept nested types");
    const auto types_over_limit = parse_nested_type(maxDepth);
    CHECK_EQ(countErrorCode(types_over_limit, diagnostics::err::ParseDepthExceeded), 1U,
             "the first over-limit type reports exactly one E1010");
    CHECK(std::any_of(types_over_limit.typeExpressions().begin(),
                      types_over_limit.typeExpressions().end(),
                      [](const frontend::TypeExpression &type) {
                          return type.kind == frontend::TypeExprKind::Error;
                      }),
          "over-limit type recovery creates an error type");
    CHECK_EQ(types_over_limit.declarations().size(), 2U,
             "type recovery preserves the following declaration");

    const auto parse_nested_blocks = [](const std::size_t nesting) {
        std::string source = "fn nested() { defer {";
        source.append(nesting, '{');
        source.append(nesting, '}');
        source += "} }\nfn after_depth_error() {}\n";
        return frontend::parse(source);
    };
    const auto blocks_at_limit = parse_nested_blocks((maxDepth - 2U) / 2U);
    CHECK(!hasErrors(blocks_at_limit), "128 active parser entries accept nested block statements");
    const auto blocks_over_limit = parse_nested_blocks((maxDepth - 2U) / 2U + 1U);
    CHECK_EQ(countErrorCode(blocks_over_limit, diagnostics::err::ParseDepthExceeded), 1U,
             "over-limit nested blocks report exactly one E1010");
    CHECK(hasErrorExpression(blocks_over_limit),
          "over-limit block recovery creates an error expression");
    CHECK_EQ(blocks_over_limit.declarations().size(), 2U,
             "block recovery preserves the following declaration");

    const auto parse_nested_conditionals = [](const std::size_t nesting) {
        std::string source = "fn nested() { ";
        for (std::size_t depth = 0; depth < nesting; ++depth) {
            if (depth > 0U)
                source += " else ";
            source += "if (true) {}";
        }
        source += " }\nfn after_depth_error() {}\n";
        return frontend::parse(source);
    };
    const auto conditionals_at_limit = parse_nested_conditionals(maxDepth - 3U);
    CHECK(!hasErrors(conditionals_at_limit),
          "128 active parser entries accept nested conditionals");
    const auto conditionals_over_limit = parse_nested_conditionals(maxDepth - 2U);
    CHECK_EQ(countErrorCode(conditionals_over_limit, diagnostics::err::ParseDepthExceeded), 1U,
             "the first over-limit conditional reports exactly one E1010");
    CHECK(hasErrorExpression(conditionals_over_limit),
          "over-limit conditional recovery creates an error expression");
    CHECK_EQ(conditionals_over_limit.declarations().size(), 2U,
             "conditional recovery preserves the following declaration");
}

static void test_pipe_operators_parse_as_chain() {
    auto snapshot = frontend::parse("fn addOne(x: i32): i32 { x + 1 }\n"
                                    "fn main(): i32 {\n"
                                    "    let out = 10 |> addOne(..);\n"
                                    "    out |> addOne(..) do addOne(..) |> addOne(..)\n"
                                    "}\n");

    CHECK(snapshot.diagnostics().empty(), "pipe syntax lowers without recovery diagnostics");
    std::size_t pipes    = 0;
    std::size_t effects  = 0;
    std::size_t currents = 0;
    bool pipe_current_is_not_spread = false;
    for (const auto &expression : snapshot.expressions()) {
        if (expression.kind == frontend::ExprKind::Pipe)
            ++pipes;
        else if (expression.kind == frontend::ExprKind::PipeDo)
            ++effects;
        else if (expression.kind == frontend::ExprKind::PipeCurrent)
            ++currents;
        else if (expression.kind == frontend::ExprKind::Call &&
                 expression.operands.size() == 2U && expression.argIsSpread.size() == 1U)
            pipe_current_is_not_spread |= !expression.argIsSpread.front();
    }
    CHECK_EQ(pipes, 3u, "three '|>' nodes are lowered");
    CHECK_EQ(effects, 1u, "one 'do' node is lowered");
    CHECK_EQ(currents, 4u, "each stage contributes its explicit '..' placeholder");
    CHECK(pipe_current_is_not_spread, "'..' pipeline arguments remain non-spread arguments");
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

    auto missing_paren              = frontend::parse("fn run(n: i32): i32 {\n"
                                                      "    if (n < 0) {\n"
                                                      "        return 0;\n"
                                                      "    } else n < 10 {\n"
                                                      "    }\n"
                                                      "    return 2;\n"
                                                      "}\n");
    bool saw_parentheses_diagnostic = false;
    for (const auto &diagnostic : missing_paren.diagnostics()) {
        if (diagnostic.isWarning)
            continue;
        saw_parentheses_diagnostic |= diagnostic.code == diagnostics::err::ExpectedConditionParens;
    }
    CHECK(saw_parentheses_diagnostic,
          "an unparenthesized else condition reports the required correction");
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

static void test_dots_lex_as_one_token() {
    auto range        = frontend::parse("fn main(): i32 {\n"
                                        "    for (i in 0..<3) { }\n"
                                        "    return 0;\n"
                                        "}\n");
    bool saw_two_dots = false;
    for (const auto &token : range.tokens()) {
        if (token.kind == frontend::TokenKind::Dots && tokenText(range, token) == "..")
            saw_two_dots = true;
    }
    CHECK(saw_two_dots, "'..' lexes as a single Dots token");

    auto variadic       = frontend::parse("extern fn printf(fmt: *char, ...): i32;\n");
    bool saw_three_dots = false;
    for (const auto &token : variadic.tokens()) {
        if (token.kind == frontend::TokenKind::Dots && tokenText(variadic, token) == "...")
            saw_three_dots = true;
    }
    CHECK(saw_three_dots, "'...' lexes as a single Dots token covering both lexemes");
}

static void test_variadic_slice_spread_argument_parses() {
    auto snapshot = frontend::parse("fn main(): i32 {\n"
                                    "    send(...values);\n"
                                    "    return send<i32>(...values);\n"
                                    "}\n");
    CHECK(snapshot.diagnostics().empty(), "call-site variadic slice spreads parse cleanly");
    std::size_t spread_calls = 0;
    bool generic_spread      = false;
    for (const auto &expression : snapshot.expressions()) {
        if (expression.kind != frontend::ExprKind::Call || expression.operands.size() != 2U ||
            expression.argIsSpread.size() != 1U || !expression.argIsSpread.front())
            continue;
        ++spread_calls;
        generic_spread |= !expression.genericArgs.empty();
    }
    CHECK_EQ(spread_calls, 2u, "ordinary and generic calls retain their spread marker");
    CHECK(generic_spread, "generic call AST retains the explicit spread marker");
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

    auto conditions = frontend::parse("fn run(ok: bool, done: bool, keep: bool): i32 {\n"
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

    auto bare_conditions = frontend::parse("fn run(ok: bool, done: bool, keep: bool): i32 {\n"
                                           "    if ok { return 0; }\n"
                                           "    while done { break; }\n"
                                           "    for keep { break; }\n"
                                           "    return 1;\n"
                                           "}\n");
    CHECK_EQ(countErrorCode(bare_conditions, diagnostics::err::ExpectedConditionParens), 3u,
             "if, while, and conditional for require parenthesized conditions");
    bool has_if_correction        = false;
    bool has_while_correction     = false;
    bool has_for_correction       = false;
    bool has_cascading_body_error = false;
    for (const auto &diagnostic : bare_conditions.diagnostics()) {
        if (diagnostic.code == diagnostics::err::ExpectedConditionParens) {
            has_if_correction |= diagnostic.message.find("if (condition) {") != std::string::npos;
            has_while_correction |=
                diagnostic.message.find("while (condition) {") != std::string::npos;
            has_for_correction |= diagnostic.message.find("for (condition) {") != std::string::npos;
        }
        has_cascading_body_error |= diagnostic.message == "expected if body" ||
                                    diagnostic.message == "expected while body" ||
                                    diagnostic.message == "expected for body";
    }
    CHECK(has_if_correction && has_while_correction && has_for_correction,
          "each missing-parentheses diagnostic shows the correction for that control form");
    CHECK(!has_cascading_body_error,
          "missing condition parentheses do not hide the loop or branch body");

    auto optional_conditions = frontend::parse("fn run(x: ?i32): i32 {\n"
                                               "    if optional x { return 0; }\n"
                                               "    while optional x { break; }\n"
                                               "    for optional x { return 1; }\n"
                                               "    return 2;\n"
                                               "}\n");
    CHECK_EQ(countErrorCode(optional_conditions, diagnostics::err::UnsupportedSyntax), 3u,
             "removed 'optional' condition forms retain their focused diagnostic");
    CHECK_EQ(countErrorCode(optional_conditions, diagnostics::err::ExpectedConditionParens), 0u,
             "the obsolete optional form does not produce a redundant parentheses error");

    auto bare_else_and_when = frontend::parse("fn run(ok: bool): i32 {\n"
                                              "    if (ok) { } else ok { }\n"
                                              "    let result = when ok { (true) 1, (_) 0 };\n"
                                              "    return result;\n"
                                              "}\n");
    CHECK_EQ(countErrorCode(bare_else_and_when, diagnostics::err::ExpectedConditionParens), 2u,
             "conditional else and when subjects require parentheses");
    CHECK_EQ(bare_else_and_when.diagnostics().size(), 2u,
             "missing else/when parentheses do not produce cascading diagnostics");

    auto call_conditions = frontend::parse("fn predicate<T>(): bool { return true; }\n"
                                           "fn subject(): i32 { return 0; }\n"
                                           "fn run(ok: bool, done: bool): i32 {\n"
                                           "    if predicate() and (ok) { }\n"
                                           "    if predicate<i32>() { }\n"
                                           "    while predicate() { break; }\n"
                                           "    for predicate() { break; }\n"
                                           "    if (ok) { } else predicate() { }\n"
                                           "    if (ok) { } else not done { }\n"
                                           "    let value = when subject() { (0) 1, (_) 0 };\n"
                                           "    return value;\n"
                                           "}\n");
    CHECK_EQ(countErrorCode(call_conditions, diagnostics::err::ExpectedConditionParens), 0u,
             "function-call conditions and legacy 'else not' remain accepted");
    CHECK(!hasErrors(call_conditions),
          "function-call conditions and legacy negation parse without errors");
}

static void test_control_bodies_require_blocks() {
    const auto missing_bodies =
        frontend::parse("fn run(flag: bool, index: i32) {\n"
                        "    if (flag) work();\n"
                        "    while (flag) work();\n"
                        "    for (flag) work();\n"
                        "    if (flag) { } else (flag) work();\n"
                        "    for (index in items) work();\n"
                        "    for (var i: i32 = 0, i < 2, i += 1) work();\n"
                        "    for (var j: i32 = 0), (j < 2), (j += 1) work();\n"
                        "    let retained: i32 = 0;\n"
                        "}\n"
                        "fn missing_else(flag: bool) {\n"
                        "    if (flag) { } else\n"
                        "}\n"
                        "fn after() { }\n");

    CHECK_EQ(countErrorCode(missing_bodies, diagnostics::err::ExpectedControlBody), 8u,
             "all control-flow forms require block bodies");
    bool has_if_example           = false;
    bool has_while_example        = false;
    bool has_for_example          = false;
    bool has_for_in_example       = false;
    bool has_else_example         = false;
    bool has_plain_else_example   = false;
    bool has_flat_for_example     = false;
    bool has_comma_for_example    = false;
    std::size_t unexpected_errors = 0;
    for (const auto &diagnostic : missing_bodies.diagnostics()) {
        if (diagnostic.isWarning)
            continue;
        if (diagnostic.code != diagnostics::err::ExpectedControlBody) {
            ++unexpected_errors;
            continue;
        }
        has_if_example |= diagnostic.message.find("if (condition) { ... }") != std::string::npos;
        has_while_example |=
            diagnostic.message.find("while (condition) { ... }") != std::string::npos;
        has_for_example |= diagnostic.message.find("for (condition) { ... }") != std::string::npos;
        has_for_in_example |=
            diagnostic.message.find("for (variable in iterable) { ... }") != std::string::npos;
        has_else_example |=
            diagnostic.message.find("else (condition) { ... }") != std::string::npos;
        has_plain_else_example |= diagnostic.message.find("else { ... }") != std::string::npos;
        has_flat_for_example |=
            diagnostic.message.find("for (init), (condition), (step) { ... }") != std::string::npos;
        has_comma_for_example |=
            diagnostic.message.find("for (init, condition, step) { ... }") != std::string::npos;
    }
    CHECK_EQ(unexpected_errors, 0u, "missing control bodies do not emit generic duplicate errors");
    CHECK(has_if_example && has_while_example && has_for_example && has_for_in_example &&
              has_else_example && has_plain_else_example && has_flat_for_example &&
              has_comma_for_example,
          "missing control bodies include a correction matching their syntax");

    bool retained_in_function_body = false;
    for (const auto &declaration : missing_bodies.declarations()) {
        if (declaration.kind != frontend::DeclKind::Function || declaration.name != "run" ||
            !declaration.body)
            continue;
        const auto &body = missing_bodies.expressions()[declaration.body.value - 1U];
        for (const auto statement_id : body.statements) {
            if (!statement_id)
                continue;
            const auto &statement = missing_bodies.statements()[statement_id.value - 1U];
            retained_in_function_body |= statement.kind == frontend::StmtKind::Binding &&
                                         statement.binding.name == "retained";
        }
    }
    CHECK(retained_in_function_body,
          "a missing flat-for body does not absorb following statements in its function");

    bool preserved_following_function = false;
    for (const auto &declaration : missing_bodies.declarations())
        preserved_following_function |= declaration.name == "after";
    CHECK(preserved_following_function,
          "a missing flat-for body does not consume the following function declaration");
}

static void test_import_from_prefix_has_actionable_diagnostic() {
    const auto snapshot = frontend::parse("import from graphics { draw as paint }\n"
                                          "fn after_import() { }\n");

    CHECK_EQ(countErrorCode(snapshot, 1013u), 1u,
             "'import from path' receives its dedicated parse diagnostic");
    CHECK_EQ(snapshot.diagnostics().size(), 1u,
             "the malformed import does not produce a generic or cascading diagnostic");
    if (!snapshot.diagnostics().empty()) {
        const auto &message = snapshot.diagnostics().front().message;
        CHECK(message.find("use 'from path'") != std::string::npos,
              "the diagnostic suggests 'from path' for importing symbols");
        CHECK(message.find("'import path'") != std::string::npos,
              "the diagnostic explains the namespace-import alternative");
    }

    CHECK_EQ(snapshot.declarations().size(), 2u,
             "the malformed import is recovered as one declaration");
    if (!snapshot.declarations().empty()) {
        const auto &import = snapshot.declarations().front().import;
        CHECK(import.isFrom, "recovery keeps the apparent direct-import form");
        CHECK(!import.path.empty(), "recovery keeps the module path");
        if (!import.path.empty())
            CHECK_EQ(import.path.front(), std::string("graphics"),
                     "recovery keeps the module path after the unexpected prefix");
        CHECK_EQ(import.selectors.size(), 1u, "recovery keeps selectors following the module path");
        if (!import.selectors.empty())
            CHECK_EQ(import.selectors.front().alias, std::string("paint"),
                     "recovery keeps selector aliases");
    }
    if (!snapshot.declarations().empty())
        CHECK_EQ(snapshot.declarations().back().name, std::string("after_import"),
                 "the declaration after the malformed import remains available");

    const auto valid_from = frontend::parse("from graphics { draw as paint }\n");
    CHECK(valid_from.diagnostics().empty(),
          "'from path { selectors }' remains a valid direct-import form");
    CHECK_EQ(valid_from.declarations().size(), 1u,
             "a valid selective direct import remains one declaration");
    if (!valid_from.declarations().empty()) {
        const auto &import = valid_from.declarations().front().import;
        CHECK(import.isFrom, "the valid 'from path' declaration is a direct import");
        CHECK_EQ(import.path.size(), 1u, "the valid direct import keeps its path");
        CHECK_EQ(import.selectors.size(), 1u, "the valid direct import keeps its selector");
    }

    const auto separate_lines = frontend::parse("import\nfrom graphics\n");
    CHECK_EQ(countErrorCode(separate_lines, diagnostics::err::UnexpectedFromAfterImport), 0u,
             "the dedicated diagnostic only matches 'import from' on one line");
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

static void test_when_no_arrow_syntax() {
    auto snapshot = frontend::parse("fn main(n: i32): i32 {\n"
                                    "    return when (n) {\n"
                                    "        (0) 10,\n"
                                    "        (1..3) 20,\n"
                                    "        (n > 100) 30,\n"
                                    "        (_) 40\n"
                                    "    };\n"
                                    "}\n");
    CHECK(snapshot.diagnostics().empty(), "no-arrow when cases parse without diagnostics");

    const frontend::Expression *when_node = nullptr;
    for (const auto &expression : snapshot.expressions()) {
        if (expression.kind == frontend::ExprKind::When)
            when_node = &expression;
    }
    CHECK(when_node != nullptr, "when expression is present");
    if (when_node != nullptr) {
        CHECK(when_node->conditions.size() == 4u, "no-arrow when keeps one condition per case");
        for (const auto &condition : when_node->conditions) {
            if (condition && condition.value <= snapshot.expressions().size()) {
                const auto &node = snapshot.expressions()[condition.value - 1u];
                CHECK(node.kind == frontend::ExprKind::WhenGuard ||
                          node.kind == frontend::ExprKind::Range ||
                          node.kind == frontend::ExprKind::Binary,
                      "when conditions lower into guard/range/binary nodes");
            }
        }
    }

    auto arrow = frontend::parse("fn main(n: i32): i32 {\n"
                                 "    return when (n) {\n"
                                 "        (0) ~> 10,\n"
                                 "        (_) ~> 40\n"
                                 "    };\n"
                                 "}\n");
    CHECK_EQ(arrow.diagnostics().size(), 2u,
             "each legacy ~> marker emits exactly one W1008 warning");
    std::size_t deprecated_count = 0;
    for (const auto &diagnostic : arrow.diagnostics())
        deprecated_count +=
            diagnostic.isWarning && diagnostic.code == diagnostics::err::DeprecatedSyntax;
    CHECK_EQ(deprecated_count, 2u, "legacy ~> markers use W1008 DeprecatedSyntax");

    auto unary_tilde_body = frontend::parse("fn main(n: i32, bits: i32): i32 {\n"
                                            "    return when (n) {\n"
                                            "        (0) ~bits,\n"
                                            "        (_) 40\n"
                                            "    };\n"
                                            "}\n");
    CHECK(unary_tilde_body.diagnostics().empty(),
          "a no-arrow body beginning with unary '~' is not treated as a legacy arrow");
}

static void test_when_guard_islands() {
    auto snapshot = frontend::parse("fn main(status: i32, retries: i32): i32 {\n"
                                    "    return when (status) {\n"
                                    "        (0) and (retries < 5) 10,\n"
                                    "        (1) or (retries > 10) 20,\n"
                                    "        (_) 30\n"
                                    "    };\n"
                                    "}\n");
    CHECK(snapshot.diagnostics().empty(),
          "no-arrow when accepts boolean guard islands after the pattern island");

    auto missing_comma = frontend::parse("fn main(n: i32): i32 {\n"
                                         "    return when (n) {\n"
                                         "        (0) 10\n"
                                         "        (1) 20,\n"
                                         "        (_) 30\n"
                                         "    };\n"
                                         "}\n");
    CHECK(!missing_comma.diagnostics().empty(),
          "no-arrow when cases require a comma between cases");

    auto invalid = frontend::parse("fn main(status: i32, retries: i32): i32 {\n"
                                   "    return when (status) {\n"
                                   "        (_) and (retries < 5) 10,\n"
                                   "        (_) 20\n"
                                   "    };\n"
                                   "}\n");
    CHECK(!invalid.diagnostics().empty(),
          "default island cannot be combined with a later guard island");
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

static void test_range_literal_and_in_operator() {
    auto snapshot = frontend::parse("fn main(x: i32): bool {\n"
                                    "    if (x in 1..<4) { return true; }\n"
                                    "    if (x in 1>..5) { return true; }\n"
                                    "    if (x in 1>..<5) { return true; }\n"
                                    "    return x in 1..4;\n"
                                    "}\n");
    CHECK(snapshot.diagnostics().empty(),
          "all four range literals and 'in' parse without diagnostics");

    const frontend::Expression *in_node     = nullptr;
    const frontend::Expression *first_range = nullptr;
    unsigned range_count                    = 0;
    unsigned in_count                       = 0;
    bool saw_open_only_hi                   = false;
    bool saw_open_only_lo                   = false;
    bool saw_open_both                      = false;
    bool saw_closed                         = false;
    for (const auto &expression : snapshot.expressions()) {
        if (expression.kind == frontend::ExprKind::Binary && expression.text == "in") {
            in_count++;
            in_node = &expression;
        } else if (expression.kind == frontend::ExprKind::Range) {
            range_count++;
            if (first_range == nullptr)
                first_range = &expression;
        }
    }
    CHECK_EQ(range_count, 4u, "all four range spellings lower to ExprKind::Range");
    for (const auto &expression : snapshot.expressions()) {
        if (expression.kind != frontend::ExprKind::Range)
            continue;
        if (expression.openAtLo && expression.openAtHi)
            saw_open_both = true;
        else if (expression.openAtLo)
            saw_open_only_lo = true;
        else if (expression.openAtHi)
            saw_open_only_hi = true;
        else
            saw_closed = true;
    }
    CHECK(in_node != nullptr, "'in' lowers to a binary expression");
    CHECK(saw_closed, "a range with plain '..' keeps both bounds closed");
    CHECK(saw_open_only_lo, "a range with '>..' records openAtLo");
    CHECK(saw_open_only_hi, "a range with '..<' records openAtHi");
    CHECK(saw_open_both, "a range with '>..<' records both open flags");
    if (in_node != nullptr)
        CHECK_EQ(in_node->operands.size(), 2u, "'in' keeps value and RHS operands");
    if (first_range != nullptr)
        CHECK_EQ(first_range->operands.size(), 2u, "Range literal keeps lo and hi operands");

    auto closed =
        frontend::parse("fn in_range(x: i32): bool { return x in 1..<4 or x in 4>..<9; }\n");
    CHECK(closed.diagnostics().empty(),
          "compound boolean expressions containing 'in' parse cleanly");
    in_count = 0;
    for (const auto &expression : closed.expressions())
        in_count += expression.kind == frontend::ExprKind::Binary && expression.text == "in";
    CHECK_EQ(in_count, 2u, "each 'in' is a normal binary operator");

    auto when = frontend::parse("fn main(x: i32): i32 {\n"
                                "    return when (x in 1..<9) {\n"
                                "        (true) 7,\n"
                                "        (_) 0\n"
                                "    };\n"
                                "}\n");
    CHECK(when.diagnostics().empty(), "when subject may directly be an 'in' condition");
    bool when_has_bool_subject = false;
    for (const auto &expression : when.expressions()) {
        if (expression.kind == frontend::ExprKind::When && !expression.operands.empty() &&
            expression.operands[0].value <= when.expressions().size()) {
            const auto &subject = when.expressions()[expression.operands[0].value - 1u];
            when_has_bool_subject =
                subject.kind == frontend::ExprKind::Binary && subject.text == "in";
        }
    }
    CHECK(when_has_bool_subject, "when subject 'x in range' remains a binary expression");

    auto slice = frontend::parse("fn main(): []i32 {\n"
                                 "    var values: [4]i32 = [0, 1, 2, 3];\n"
                                 "    return values[1..<3];\n"
                                 "}\n");
    CHECK(!slice.diagnostics().empty(), "open bounds are not valid slice syntax");
}

static void test_for_in_range_parsing() {
    auto snapshot = frontend::parse("fn main(): i32 {\n"
                                    "    var total: i32 = 0;\n"
                                    "    for (i in 0>..<5) { total = total + i; }\n"
                                    "    return total;\n"
                                    "}\n");
    CHECK(snapshot.diagnostics().empty(), "for-in over a range literal parses without diagnostics");

    const frontend::Expression *for_in = nullptr;
    bool saw_range                     = false;
    for (const auto &expression : snapshot.expressions()) {
        if (expression.kind == frontend::ExprKind::ForIn)
            for_in = &expression;
        if (expression.kind == frontend::ExprKind::Range)
            saw_range = expression.openAtLo && expression.openAtHi;
    }
    CHECK(for_in != nullptr, "range for-in lowers to ExprKind::ForIn");
    CHECK(saw_range, "the iterable operand of for-in is the parsed open Range");
    if (for_in != nullptr) {
        CHECK(for_in->forInBinding, "for-in keeps a synthetic local binding");
        CHECK_EQ(for_in->operands.size(), 2u, "for-in keeps iterable and body operands");
    }
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

static void test_external_global_declarations() {
    const auto snapshot = frontend::parse("extern var counter: i32;\n"
                                          "extern const rate: f64 = extern c_rate;\n"
                                          "extern var stream: *FILE = extern c_stream;\n");
    CHECK(snapshot.diagnostics().empty(),
          "external global declarations accept optional C-symbol aliases");
    CHECK_EQ(snapshot.declarations().size(), 3u, "all external globals are lowered");
    if (snapshot.declarations().size() == 3U) {
        const auto &counter = snapshot.declarations()[0];
        CHECK_EQ(counter.kind, frontend::DeclKind::Variable, "extern var is a variable");
        CHECK(counter.isExtern, "extern var is marked as external storage");
        CHECK_EQ(counter.bindingKind, frontend::BindingKind::Var,
                 "extern var preserves mutability");
        CHECK(counter.declaredType, "extern var preserves its declared type");
        if (counter.declaredType &&
            counter.declaredType.value <= snapshot.typeExpressions().size()) {
            const auto &type = snapshot.typeExpressions()[counter.declaredType.value - 1U];
            CHECK_EQ(type.kind, frontend::TypeExprKind::Name, "scalar type remains a name");
            CHECK_EQ(type.name, std::string("i32"), "scalar type name is preserved");
        }

        const auto &rate = snapshot.declarations()[1];
        CHECK_EQ(rate.kind, frontend::DeclKind::Variable, "extern const is a variable");
        CHECK(rate.isExtern, "extern const is marked as external storage");
        CHECK_EQ(rate.bindingKind, frontend::BindingKind::Const,
                 "extern const preserves immutability");
        CHECK_EQ(rate.externalSymbol, std::string("c_rate"),
                 "extern const preserves its C symbol alias");
        CHECK(!rate.initializer, "C-symbol alias is not parsed as an initializer");
        CHECK(rate.declaredType, "extern const preserves its declared type");

        const auto &stream = snapshot.declarations()[2];
        CHECK_EQ(stream.bindingKind, frontend::BindingKind::Var,
                 "aliased extern var remains mutable");
        CHECK_EQ(stream.externalSymbol, std::string("c_stream"),
                 "extern var preserves its C symbol alias");
        CHECK(stream.declaredType, "pointer external global preserves its declared type");
        if (stream.declaredType && stream.declaredType.value <= snapshot.typeExpressions().size()) {
            const auto &type = snapshot.typeExpressions()[stream.declaredType.value - 1U];
            CHECK_EQ(type.kind, frontend::TypeExprKind::Pointer,
                     "pointer external global keeps its pointer type");
        }
    }

    const auto invalid = frontend::parse("var ordinary: i32;\n"
                                         "const missing: i32;\n"
                                         "extern var initialized: i32 = 7;\n"
                                         "extern const initialized_const: i32 = 8;\n"
                                         "extern let unsupported: i32;\n"
                                         "extern var missing_type;\n"
                                         "extern var missing_alias: i32 = extern;\n"
                                         "fn recovered() {}\n");
    CHECK(!invalid.diagnostics().empty(), "invalid external-global forms are diagnosed");
    bool rejected_ordinary_var              = false;
    bool rejected_const_without_initializer = false;
    bool rejected_external_initializer      = false;
    bool rejected_external_let              = false;
    bool rejected_missing_type              = false;
    bool rejected_missing_alias             = false;
    for (const auto &diagnostic : invalid.diagnostics()) {
        rejected_ordinary_var |=
            diagnostic.message.find("top-level variables must be declared") != std::string::npos;
        rejected_const_without_initializer |=
            diagnostic.message.find("const declaration requires an initializer") !=
            std::string::npos;
        rejected_external_initializer |=
            diagnostic.message.find("external global") != std::string::npos &&
            diagnostic.message.find("initializer") != std::string::npos;
        rejected_external_let |= diagnostic.message.find("extern") != std::string::npos &&
                                 diagnostic.message.find("'var' or 'const'") != std::string::npos;
        rejected_missing_type |=
            diagnostic.message.find("requires an explicit type") != std::string::npos;
        rejected_missing_alias |=
            diagnostic.message.find("requires an identifier after '= extern'") != std::string::npos;
    }
    CHECK(rejected_ordinary_var, "ordinary top-level var remains rejected");
    CHECK(rejected_const_without_initializer, "ordinary const still requires an initializer");
    CHECK(rejected_external_initializer, "external globals reject value initializers");
    CHECK(rejected_external_let, "external globals reject let bindings");
    CHECK(rejected_missing_type, "external globals require an explicit type");
    CHECK(rejected_missing_alias, "incomplete C-symbol alias has a specific diagnostic");
    bool recovered_function = false;
    for (const auto &decl : invalid.declarations())
        recovered_function |= decl.kind == frontend::DeclKind::Function && decl.name == "recovered";
    CHECK(recovered_function, "parser recovers and retains a following declaration");

    const auto local = frontend::parse("fn holder() {\n"
                                       "    extern var misplaced: i32;\n"
                                       "    var recovered: i32 = 1;\n"
                                       "}\n");
    CHECK(!local.diagnostics().empty(), "external globals are rejected inside function bodies");
    bool recovered_local = false;
    for (const auto &statement : local.statements()) {
        if (statement.kind == frontend::StmtKind::Binding)
            recovered_local |= statement.binding.name == "recovered";
    }
    CHECK(recovered_local, "local parser recovers after a misplaced external global");
}

static void test_frontend() {
    test_lossless_trivia_and_spans();
    test_cst_dump_is_deterministic_and_keeps_nested_tokens();
    test_keywords_and_module_ast();
    test_dots_lex_as_one_token();
    test_variadic_slice_spread_argument_parses();
    test_bare_opaque_type_expression();
    test_kebab_case_qualified_type_path();
    test_recovery_creates_error_nodes();
    test_function_body_ast();
    test_control_flow_and_scopes();
    test_pipe_operators_parse_as_chain();
    test_state_and_dock_jump_syntax();
    test_explicit_discard_statement();
    test_old_state_machine_syntax_is_rejected();
    test_while_is_deprecated();
    test_return_expression_requires_semicolon();
    test_parser_depth_limit();
    test_multi_char_operators_are_single_tokens();
    test_binary_comparison_expression();
    test_cast_expression();
    test_is_null_expression();
    test_is_type_expression();
    test_for_loop_forms();
    test_loop_labels_and_not_negation();
    test_default_parameters_and_condition_syntax();
    test_control_bodies_require_blocks();
    test_import_from_prefix_has_actionable_diagnostic();
    test_else_condition_forms();
    test_for_var_var_reports_specific_diagnostic();
    test_structured_imports();
    test_c_header_import_syntax();
    test_variable_declarations();
    test_const_struct_fields();
    test_zith_removed_bindings_and_qualifiers();
    test_type_alias_and_struct_enum_union();
    test_unary_and_nested_expressions();
    test_attributes_parse_and_classify();
    test_alignment_and_branch_attributes_parse();
    test_multiple_top_level_decls_with_visibility();
    test_import_form_with_depth_and_export();
    test_error_diagnostic_span_preserved();
    test_unexpected_top_level_token_is_diagnosed();
    test_macro_invocation_is_tolerated();
    test_extern_before_declaration_is_tolerated();
    test_function_type_expression();
    test_slice_range_expression();
    test_when_no_arrow_syntax();
    test_when_guard_islands();
    test_raw_index_slice_expression();
    test_range_literal_and_in_operator();
    test_for_in_range_parsing();
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
    test_external_global_declarations();
}

TEST_MAIN(frontend)
