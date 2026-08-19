#include "frontend/parser/parse.hpp"

#include "common/memory/arena.hpp"
#include "common/memory/string-interner.hpp"
#include "frontend/lexer/lexer.hpp"
#include "frontend/parser/parser.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

using common::memory::Arena;
using common::memory::StringInterner;
using generated_lexer::Lexer;
using generated_lexer::TokenStream;
using Parser = generated_parser::Parser<sample::ParseOutput>;

namespace {

bool printDiagnostics(std::string_view source,
                      const std::vector<sample::ParserDiagnostic> &diagnostics) {
    for (const auto &diagnostic : diagnostics) {
        std::cerr << "diag for '" << source << "' [" << diagnostic.span.start
                  << ',' << diagnostic.span.end << "] "
                  << diagnostic.message << '\n';
    }
    return !diagnostics.empty();
}

sample::ParseOutput parse(Arena &arena, StringInterner &strings,
                          std::string_view source) {
    Lexer lexer;
    TokenStream tokens = lexer.run(source);
    Parser parser(arena);
    return hooks::parser::parseSource(parser, tokens, source);
}

bool parseAndCheck(std::string_view source, bool expectError,
                   bool expectImports = false) {
    Arena arena;
    StringInterner strings(arena);
    sample::ParseOutput output = parse(arena, strings, source);
    const bool hasError = !output.diagnostics.empty();
    if (hasError != expectError) {
        std::cerr << "parse result mismatch for '" << source << "'\n";
        printDiagnostics(source, output.diagnostics);
        return false;
    }
    if (expectImports && output.imports.empty()) {
        std::cerr << "expected import metadata for '" << source << "'\n";
        return false;
    }
    if (output.ast.root == nullptr) {
        std::cerr << "program root missing for '" << source << "'\n";
        return false;
    }
    return true;
}

bool expectFunction() {
    Arena arena;
    StringInterner strings(arena);
    sample::ParseOutput output =
        parse(arena, strings, "pub fn add(a: i32, b: i32) -> i32 { return a + b; }");
    if (!output.diagnostics.empty())
        return printDiagnostics("pub fn add", output.diagnostics);
    if (output.ast.root == nullptr || output.ast.root->body.size() != 1 ||
        output.ast.root->body[0]->kind != generated_ast::NodeKind::Declaration)
        return false;
    auto *declaration =
        static_cast<generated_ast::Declaration *>(output.ast.root->body[0]);
    return declaration->kind == static_cast<int>(sample::DeclKind::Function) &&
           declaration->name == "add" &&
           declaration->visibility ==
               static_cast<int>(sample::VisibilityKind::Public) &&
           declaration->parameters.size() == 2 && declaration->body != nullptr;
}

bool expectImports() {
    struct Case {
        std::string_view source;
        bool expectError;
        bool expectFrom;
        bool expectExport;
        bool expectAsset;
        bool expectHeader;
        int depth;
        std::size_t selectors;
        std::string_view rawPath;
        std::string_view alias;
    };
    const Case cases[] = {
        {"import parent/sample;", false, false, false, false, false, 1, 0,
         "parent/sample", ""},
        {"import parent/sample as Alias;", false, false, false, false, false, 1,
         0, "parent/sample", "Alias"},
        {"from parent/sample { printf as show };", false, true, false, false,
         false, 1, 1, "parent/sample", ""},
        {"export parent/sample(..);", false, false, true, false, false, -1, 0,
         "parent/sample", ""},
        {"import \"stdio.h\" { printf };", false, false, false, false, true, 1,
         1, "\"stdio.h\"", ""},
        {"import assets/dat.json as Json;", false, false, false, true, false, 1,
         0, "assets/dat.json", "Json"},
        {"import asset assets/dat.json as Json;", false, false, false, true,
         false, 1, 0, "assets/dat.json", "Json"},
        {"import;", true, false, false, false, false, 1, 0, "", ""},
        {"import assets/dat.json;", false, false, false, true, false, 1, 0,
         "assets/dat.json", ""},
    };

    bool ok = true;
    for (const auto &test : cases) {
        Arena arena;
        StringInterner strings(arena);
        sample::ParseOutput output = parse(arena, strings, test.source);
        if (!output.diagnostics.empty() != test.expectError) {
            std::cerr << "import diagnostic mismatch for '" << test.source
                      << "'\n";
            printDiagnostics(test.source, output.diagnostics);
            ok = false;
            continue;
        }
        if (test.expectError)
            continue;
        if (output.imports.size() != 1) {
            std::cerr << "import count mismatch for '" << test.source << "'\n";
            ok = false;
            continue;
        }
        const auto &decl = output.imports.front();
        if (decl.rawPath != test.rawPath || decl.alias != test.alias ||
            decl.isFrom != test.expectFrom || decl.isExport != test.expectExport ||
            decl.isAsset != test.expectAsset || decl.isHeader != test.expectHeader ||
            decl.depth != test.depth || decl.selectors.size() != test.selectors) {
            std::cerr << "import fields mismatch for '" << test.source << "'\n";
            ok = false;
        }
    }
    return ok;
}

bool expectExpressions() {
    const std::string_view sources[] = {
        "let a = 1 + 2 * 3;",
        "let b = -x + call(1, 2)[0].field->other;",
        "let c = [1, 2, 3];",
        "let d = Point { x: 1, y: 2 };",
        "let e = a as i32;",
        "let f = if (cond) { a } else { b };",
        "pub fn h() { for { continue; } }",
    };
    bool ok = true;
    for (std::string_view source : sources) {
        Arena arena;
        StringInterner strings(arena);
        sample::ParseOutput output = parse(arena, strings, source);
        if (!output.diagnostics.empty()) {
            printDiagnostics(source, output.diagnostics);
            ok = false;
        }
    }
    return ok;
}

bool expectWhileRejected() {
    Arena arena;
    StringInterner strings(arena);
    sample::ParseOutput output =
        parse(arena, strings, "fn g() { while (true) { break; } }");
    if (output.diagnostics.empty())
        return false;
    for (const auto &diag : output.diagnostics) {
        if (diag.message.find("while is no longer supported") !=
            std::string::npos)
            return true;
    }
    return false;
}

bool expectRecovery() {
    Arena arena;
    StringInterner strings(arena);
    sample::ParseOutput output =
        parse(arena, strings, "garbage ??? ; fn valid() {}");
    if (output.diagnostics.empty()) {
        std::cerr << "expected garbage recovery diagnostics\n";
        return false;
    }
    if (output.ast.root == nullptr || output.ast.root->body.size() != 1 ||
        output.ast.root->body[0]->kind != generated_ast::NodeKind::Declaration)
        return false;
    auto *declaration =
        static_cast<generated_ast::Declaration *>(output.ast.root->body[0]);
    return declaration->name == "valid";
}

bool expectSpans() {
    Arena arena;
    StringInterner strings(arena);
    const std::string_view source = "let value = 10;";
    sample::ParseOutput output = parse(arena, strings, source);
    if (!output.diagnostics.empty())
        return false;
    if (output.ast.root == nullptr || output.ast.root->body.size() != 1)
        return false;
    auto *declaration =
        static_cast<generated_ast::Declaration *>(output.ast.root->body[0]);
    if (declaration->span.start != 0 || declaration->span.end != source.size())
        return false;
    if (declaration->initializer == nullptr)
        return false;
    auto *initializer =
        static_cast<generated_ast::Expr *>(declaration->initializer);
    return initializer->span.end > initializer->span.start;
}

bool expectTypeKinds() {
    const std::string_view sources[] = {
        "type Ptr = *i32;",
        "type Opt = ?i32;",
        "type Slice = []i32;",
        "type Arr = [4]i32;",
        "type Opaque = raw opaque;",
        "type Mut = mut i32;",
    };
    bool ok = true;
    for (std::string_view source : sources) {
        Arena arena;
        StringInterner strings(arena);
        sample::ParseOutput output = parse(arena, strings, source);
        if (!output.diagnostics.empty()) {
            printDiagnostics(source, output.diagnostics);
            ok = false;
        }
    }
    return ok;
}

bool expectVisibilityKinds() {
    struct Case {
        std::string_view source;
        sample::VisibilityKind kind;
        int ancestors;
        int descendants;
    };

    const Case cases[] = {
        {"pub(..) fn a() {}", sample::VisibilityKind::Public, -1, -1},
        {"pub(0..) fn b() {}", sample::VisibilityKind::Module, 0, -1},
        {"pub(0..=) fn c() {}", sample::VisibilityKind::Module, 0, 0},
        {"pub(=..0) fn d() {}", sample::VisibilityKind::Module, 1, 0},
        {"pub(siblings) fn e() {}", sample::VisibilityKind::Module, 0, 0},
        {"pub fn f() {}", sample::VisibilityKind::Public, -1, -1},
    };

    bool ok = true;
    for (const auto &test : cases) {
        Arena arena;
        StringInterner strings(arena);
        sample::ParseOutput output = parse(arena, strings, test.source);
        if (!output.diagnostics.empty()) {
            printDiagnostics(test.source, output.diagnostics);
            ok = false;
            continue;
        }
        if (output.ast.root == nullptr || output.ast.root->body.empty()) {
            ok = false;
            continue;
        }
        const auto *decl = static_cast<generated_ast::Declaration *>(
            output.ast.root->body[0]);
        if (decl->visibility != static_cast<int>(test.kind) ||
            decl->visibilityAncestors != test.ancestors ||
            decl->visibilityDescendants != test.descendants) {
            std::cerr << "visibility mismatch for '" << test.source << "'\n";
            ok = false;
        }
    }
    return ok;
}

bool expectFlowSyntax() {
    Arena arena;
    StringInterner strings(arena);
    sample::ParseOutput output = parse(
        arena, strings,
        "state Ready(x: i32) { enter Next(1); }\n"
        "fn main() { enter Ready(1); jump Next(2); leave 3; }\n");
    if (!output.diagnostics.empty())
        return printDiagnostics("flow syntax", output.diagnostics);
    if (output.ast.root == nullptr || output.ast.root->body.size() != 2)
        return false;
    auto *state = static_cast<generated_ast::Declaration *>(
        output.ast.root->body[0]);
    if (state->kind != static_cast<int>(sample::DeclKind::State) ||
        state->name != "Ready" || state->parameters.size() != 1 ||
        state->body == nullptr)
        return false;
    auto *stateBody =
        static_cast<generated_ast::Expr *>(state->body);
    if (stateBody->statements.empty())
        return false;
    auto *enterInState = static_cast<generated_ast::Stmt *>(
        stateBody->statements[0]);
    if (enterInState->kind != static_cast<int>(sample::StmtKind::Enter) ||
        enterInState->label != "Next")
        return false;

    auto *main = static_cast<generated_ast::Declaration *>(
        output.ast.root->body[1]);
    if (main->kind != static_cast<int>(sample::DeclKind::Function) ||
        main->body == nullptr)
        return false;
    auto *mainBody = static_cast<generated_ast::Expr *>(main->body);
    if (mainBody->statements.size() != 3)
        return false;
    const auto *enter = static_cast<generated_ast::Stmt *>(
        mainBody->statements[0]);
    const auto *jump = static_cast<generated_ast::Stmt *>(
        mainBody->statements[1]);
    const auto *leave = static_cast<generated_ast::Stmt *>(
        mainBody->statements[2]);
    return enter->kind == static_cast<int>(sample::StmtKind::Enter) &&
           enter->label == "Ready" &&
           jump->kind == static_cast<int>(sample::StmtKind::Jump) &&
           jump->label == "Next" &&
           leave->kind == static_cast<int>(sample::StmtKind::Leave) &&
           leave->expression != nullptr;
}

bool expectNoLegacyFlowNodes() {
    Arena arena;
    StringInterner strings(arena);
    sample::ParseOutput output =
        parse(arena, strings,
              "fn main() { marker Old() {} dock Target(); }\n");
    if (output.diagnostics.empty())
        return false;
    if (output.ast.root == nullptr || output.ast.root->body.empty())
        return false;
    auto *main = static_cast<generated_ast::Declaration *>(
        output.ast.root->body[0]);
    if (main->body == nullptr)
        return false;
    auto *body = static_cast<generated_ast::Expr *>(main->body);
    for (generated_ast::AstNode *node : body->statements) {
        auto *stmt = static_cast<generated_ast::Stmt *>(node);
        if (stmt->kind == static_cast<int>(sample::StmtKind::Enter) ||
            stmt->kind == static_cast<int>(sample::StmtKind::Leave) ||
            stmt->kind == static_cast<int>(sample::StmtKind::Jump))
            return false;
    }
    return true;
}

bool expectControlFlow() {
    bool ok = true;
    const auto verify = [&ok](bool condition, std::string_view what) {
        if (!condition) {
            std::cerr << "control-flow: " << what << '\n';
            ok = false;
        }
    };
    {
        Arena arena;
        StringInterner strings(arena);
        sample::ParseOutput output = parse(
            arena, strings,
            "fn f(a: i32, b: i32) -> i32 {\n"
            "  return a ?;\n"
            "  if (a) { b } else if (b) { a } else { 0 }\n"
            "  when (a) { 1 ~> b, (_) ~> 0 }\n"
            "  for (var i = 0; i < a; i + 1) { b }\n"
            "  for (b) { a }\n"
            "  for { b }\n"
            "}\n");
        if (!output.diagnostics.empty()) {
            printDiagnostics("control flow", output.diagnostics);
            return false;
        }
        if (output.ast.root == nullptr || output.ast.root->body.size() != 1)
            return false;
        auto *decl = static_cast<generated_ast::Declaration *>(
            output.ast.root->body[0]);
        if (decl->body == nullptr)
            return false;
        auto *body = static_cast<generated_ast::Expr *>(decl->body);
        if (body->statements.empty())
            return false;

        const auto *returnStmt = static_cast<generated_ast::Stmt *>(
            body->statements[0]);
        verify(returnStmt->kind == static_cast<int>(sample::StmtKind::Return),
               "return statement kind");
        verify(returnStmt->returnValue == returnStmt->expression,
               "returnValue mirrors expression");
        verify(returnStmt->expression != nullptr &&
                   static_cast<generated_ast::Expr *>(returnStmt->expression)
                           ->text == "a?",
               "trailing return marker text");

        const auto *ifExpr = static_cast<generated_ast::Expr *>(
            static_cast<generated_ast::Stmt *>(body->statements[1])->expression);
        verify(ifExpr->kind == static_cast<int>(sample::ExprKind::If),
               "if expression kind");
        verify(ifExpr->conditions.size() == 1 &&
                   ifExpr->statements.size() == 1,
               "if condition and body");
        verify(ifExpr->alternate != nullptr &&
                   static_cast<generated_ast::Expr *>(ifExpr->alternate)->kind ==
                       static_cast<int>(sample::ExprKind::If),
               "else-if alternate");
        const auto *nestedIf = static_cast<generated_ast::Expr *>(ifExpr->alternate);
        verify(nestedIf->alternate != nullptr, "else alternate");

        const auto *whenStmt = static_cast<generated_ast::Stmt *>(
            body->statements[2]);
        const auto *whenExpr = static_cast<generated_ast::Expr *>(
            whenStmt->expression);
        verify(whenExpr->kind == static_cast<int>(sample::ExprKind::When),
               "when expression kind");
        verify(whenExpr->conditions.size() == 1 &&
                   whenExpr->cases.size() == 2,
               "when subject and paired cases");
        verify(whenExpr->alternate != nullptr, "when default alternate");

        const auto *forClauses = static_cast<generated_ast::Stmt *>(
            body->statements[3]);
        const auto *forClauseExpr = static_cast<generated_ast::Expr *>(
            forClauses->expression);
        verify(forClauseExpr->kind == static_cast<int>(sample::ExprKind::For),
               "three-clause for kind");
        verify(forClauseExpr->conditions.size() == 1 &&
                   forClauseExpr->statements.size() == 1,
               "three-clause for condition/body");
        verify(forClauseExpr->operands.size() >= 1,
               "three-clause for init/step operands");

        const auto *forCond = static_cast<generated_ast::Stmt *>(
            body->statements[4]);
        const auto *forCondExpr = static_cast<generated_ast::Expr *>(
            forCond->expression);
        verify(forCondExpr->kind == static_cast<int>(sample::ExprKind::For),
               "parenthesized for kind");
        verify(forCondExpr->conditions.size() == 1 &&
                   forCondExpr->statements.size() == 1,
               "parenthesized for condition/body");

        const auto *forInfiniteStmt = static_cast<generated_ast::Stmt *>(
            body->statements[5]);
        const auto *forInfinite = static_cast<generated_ast::Expr *>(
            forInfiniteStmt->expression);
        verify(forInfinite->kind == static_cast<int>(sample::ExprKind::For),
               "infinite for kind");
        verify(forInfinite->statements.size() == 1,
               "infinite for body");
    }
    {
        Arena arena;
        StringInterner strings(arena);
        sample::ParseOutput output = parse(
            arena, strings,
            "fn g() -> i32 { return 1 !; }\n"
            "fn h() { return; }\n"
            "fn malformed() { if { } for (; ) { } when (x) { 1 2 } }\n");
        if (output.diagnostics.empty()) {
            std::cerr << "expected malformed control-flow diagnostics\n";
            return false;
        }
        bool malformed = false;
        for (const auto &diag : output.diagnostics) {
            if (diag.message.find("expected") != std::string::npos ||
                diag.message.find("~>") != std::string::npos)
                malformed = true;
        }
        verify(malformed, "malformed control-flow diagnostics");
    }
    {
        Arena arena;
        StringInterner strings(arena);
        sample::ParseOutput output = parse(
            arena, strings,
            "fn marks() -> i32 { return 1 !; }\n"
            "fn plain() -> i32 { return 1; }\n"
            "fn none() { return; }\n");
        if (!output.diagnostics.empty()) {
            printDiagnostics("plain returns", output.diagnostics);
            return false;
        }
        if (output.ast.root == nullptr ||
            output.ast.root->body.size() != 3)
            return false;

        auto *marks = static_cast<generated_ast::Declaration *>(
            output.ast.root->body[0]);
        auto *marksBody = static_cast<generated_ast::Expr *>(marks->body);
        const auto *marksStmt = static_cast<generated_ast::Stmt *>(
            marksBody->statements[0]);
        verify(marksStmt->expression != nullptr &&
                   marksStmt->returnValue == marksStmt->expression &&
                   static_cast<generated_ast::Expr *>(marksStmt->expression)
                           ->text == "1!",
               "return expr! shape");

        auto *plain = static_cast<generated_ast::Declaration *>(
            output.ast.root->body[1]);
        auto *plainBody = static_cast<generated_ast::Expr *>(plain->body);
        const auto *plainStmt = static_cast<generated_ast::Stmt *>(
            plainBody->statements[0]);
        verify(plainStmt->expression != nullptr &&
                   plainStmt->returnValue == plainStmt->expression &&
                   static_cast<generated_ast::Expr *>(plainStmt->expression)
                           ->text == "1",
               "return expr shape");

        auto *none = static_cast<generated_ast::Declaration *>(
            output.ast.root->body[2]);
        auto *noneBody = static_cast<generated_ast::Expr *>(none->body);
        const auto *noneStmt = static_cast<generated_ast::Stmt *>(
            noneBody->statements[0]);
        verify(noneStmt->expression == nullptr &&
                   noneStmt->returnValue == nullptr,
               "return without value shape");
    }
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= parseAndCheck("alpha", true);
    ok &= parseAndCheck("", false);  // empty source with no parse error
    ok &= parseAndCheck("garbage", true);
    ok &= parseAndCheck("let x = 1 + 2 * 3;", false);
    ok &= parseAndCheck("import parent/sample;", false, true);
    ok &= expectFunction();
    ok &= expectImports();
    ok &= expectExpressions();
    ok &= expectRecovery();
    ok &= expectSpans();
    ok &= expectTypeKinds();
    ok &= expectWhileRejected();
    ok &= expectVisibilityKinds();
    ok &= expectFlowSyntax();
    ok &= expectNoLegacyFlowNodes();
    ok &= expectControlFlow();
    if (!ok)
        return EXIT_FAILURE;
    std::cout << "parser-test: real parseSource parity checks passed\n";
    return EXIT_SUCCESS;
}
