#include "legacy-zith/ast/ast-builder.hpp"
#include "formatter/fmt-visitor.hpp"
#include "memory/arena.hpp"
#include "memory/string-interner.hpp"
#include "test-common.hpp"

#include <string>

using namespace zith;

static void test_formatter_preserves_experimental_ast_nodes() {
    memory::Arena arena;
    memory::StringInterner interner(arena);
    ast::AstBuilder builder(arena, interner);

    memory::DynArray<std::string_view> word_params(arena);
    word_params.push("lhs");
    word_params.push("rhs");
    memory::DynArray<ast::StmtId> word_stmts(arena);
    auto word_body = builder.block(std::move(word_stmts), builder.litExpr(ast::LitKind::Int, "0"));
    auto word =
        builder.wordDecl("SELECT", ast::WordCategory::Infix, std::move(word_params), word_body);

    memory::DynArray<ast::DeclId> context_decls(arena);
    context_decls.push(word);
    auto context = builder.contextDecl("SQL", std::move(context_decls));

    memory::DynArray<ast::ExprId> word_call_args(arena);
    word_call_args.push(builder.ident("table"));
    word_call_args.push(builder.ident("column"));
    auto word_call = builder.wordCall("SELECT", std::move(word_call_args));
    memory::DynArray<ast::StmtId> use_block_stmts(arena);
    auto use_block = builder.block(std::move(use_block_stmts), word_call);
    auto use_context =
        builder.useStmt("SQL", memory::DynArray<std::string_view>{arena}, {}, {}, use_block);
    auto use_word =
        builder.useStmt({}, memory::DynArray<std::string_view>{arena}, "DOT", "math.vec.dot");

    memory::DynArray<ast::ExprId> operands(arena);
    operands.push(builder.ident("left"));
    operands.push(builder.ident("right"));
    memory::DynArray<ast::OpMarker> ops(arena);
    ops.push({{}, ast::BinaryOp::Add, "SELECT", 0, true});
    auto sequence = builder.seq(std::move(operands), std::move(ops));

    memory::DynArray<ast::StmtId> main_stmts(arena);
    main_stmts.push(use_context);
    main_stmts.push(use_word);
    auto main_body = builder.block(std::move(main_stmts), sequence);
    auto main      = builder.fnDecl("main", memory::DynArray<std::string_view>{arena}, main_body);

    ast::ProgramNode program(arena);
    program.decls.push(context);
    program.decls.push(main);

    formatter::FmtVisitor formatter(builder, program);
    formatter.format();
    const std::string &output = formatter.result();

    CHECK(output.find("context SQL {") != std::string::npos, "formats context declaration");
    CHECK(output.find("infix SELECT(lhs, rhs) {") != std::string::npos, "formats word declaration");
    CHECK(output.find("use SQL {") != std::string::npos, "formats scoped use statement");
    CHECK(output.find("SELECT(table, column)") != std::string::npos, "formats word call");
    CHECK(output.find("use math.vec.dot as DOT;") != std::string::npos, "formats aliased word use");
    CHECK(output.find("left SELECT right") != std::string::npos, "formats word sequence");
}

static void test_formatter() {
    test_formatter_preserves_experimental_ast_nodes();
}

TEST_MAIN(formatter)
