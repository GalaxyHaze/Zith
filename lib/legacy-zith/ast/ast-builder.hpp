#pragma once

#include "legacy-zith/ast/ast-node-utils.hpp"
#include "legacy-zith/ast/ast-nodes.hpp"
#include "legacy-zith/ast/type-expr.hpp"
#include "memory/arena.hpp"
#include "memory/dyn-array.hpp"
#include "memory/string-interner.hpp"

namespace zith::ast {

class AstBuilder {
    memory::Arena &arena_;
    memory::StringInterner &interner_;
    memory::DynArray<ExprNode> exprs_;
    memory::DynArray<StmtNode> stmts_;
    memory::DynArray<DeclNode> decls_;
    memory::DynArray<TypeExprNode> type_exprs_;

public:
    AstBuilder(memory::Arena &arena, memory::StringInterner &interner);

    ExprId addExpr(ExprNode node);
    StmtId addStmt(StmtNode node);
    StmtId addStmt(ExprId expr, memory::Span span = {});
    DeclId addDecl(DeclNode node);

    ExprNode &getExpr(ExprId id);
    StmtNode &getStmt(StmtId id);
    DeclNode &getDecl(DeclId id);

    const ExprNode &getExpr(ExprId id) const;
    const StmtNode &getStmt(StmtId id) const;
    const DeclNode &getDecl(DeclId id) const;

    LitValue makeLit(LitKind kind, std::string_view raw, memory::Span span = {});
    ExprId litExpr(LitKind kind, std::string_view raw, memory::Span span = {});
    ExprId ident(std::string_view name, memory::Span span = {}, bool scope_escape = false);
    ExprId binary(ExprId lhs, BinaryOp op, ExprId rhs, memory::Span span = {});
    ExprId unary(UnaryOp op, ExprId operand, memory::Span span = {});
    ExprId seq(memory::DynArray<ExprId> operands, memory::DynArray<ast::OpMarker> ops,
               memory::Span span = {});
    ExprId call(ExprId callee, memory::DynArray<ExprId> args, memory::Span span = {});
    ExprId field(ExprId object, std::string_view field_name, memory::Span span = {});
    ExprId index(ExprId object, ExprId index, memory::Span span = {});
    ExprId structLiteral(std::string_view type_name, memory::DynArray<StructFieldInit> fields,
                         memory::Span span = {});
    ExprId arrayLiteral(memory::DynArray<ExprId> elements, memory::Span span = {});
    ExprId enumValue(std::string_view enum_name, std::string_view variant_name,
                     memory::Span span = {});
    ExprId range(ExprId lhs, ExprId rhs, memory::Span span = {});
    ExprId range(ExprId lhs, ExprId rhs, RangeBounds bounds, memory::Span span = {});
    ExprId whenExpr(ExprId subject, memory::DynArray<WhenCase> cases, memory::Span span = {});
    ExprId block(memory::DynArray<StmtId> stmts, ExprId trailing = kInvalidExpr,
                 memory::Span span = {});
    ExprId ifExpr(ExprId cond, ExprId then_branch, ExprId else_branch = kInvalidExpr,
                  memory::Span span = {});
    ExprId whileExpr(ExprId cond, ExprId body, memory::Span span = {});

    StmtId letStmt(memory::DynArray<std::string_view> names, bool mut,
                   TypeExprId type_annot = kInvalidTypeExpr, ExprId init = kInvalidExpr,
                   memory::Span span = {});
    StmtId letStmt(std::string_view name, bool mut, ExprId init = kInvalidExpr,
                   memory::Span span = {});
    StmtId assign(ExprId target, ExprId value, memory::Span span = {});
    StmtId retStmt(ExprId value = kInvalidExpr, memory::Span span = {});
    StmtId gotoStmt(std::string_view target, memory::Span span = {});
    StmtId markerStmt(std::string_view name, memory::DynArray<StmtId> body, memory::Span span = {});

    DeclId fnDecl(std::string_view name, memory::DynArray<GenericParam> generic_params,
                  memory::DynArray<FnParam> params, TypeExprId return_type = kInvalidTypeExpr,
                  ExprId body = kInvalidExpr, bool is_extern = false, memory::Span span = {});
    DeclId fnDecl(std::string_view name, memory::DynArray<std::string_view> param_names,
                  ExprId body = kInvalidExpr, memory::Span span = {});
    DeclId structDecl(std::string_view name, memory::DynArray<GenericParam> generic_params,
                      memory::DynArray<StructField> fields, memory::DynArray<DeclId> methods,
                      TypeExprId extends_parent = kInvalidTypeExpr, memory::Span span = {});
    DeclId enumDecl(std::string_view name, memory::DynArray<GenericParam> generic_params,
                    memory::DynArray<EnumVariant> variants,
                    TypeExprId underlying_type = kInvalidTypeExpr, memory::Span span = {});
    DeclId unionDecl(std::string_view name, memory::DynArray<GenericParam> generic_params,
                     memory::DynArray<UnionVariant> variants, bool is_raw = false,
                     memory::Span span = {});
    DeclId componentDecl(std::string_view name, memory::DynArray<GenericParam> generic_params,
                         memory::DynArray<StructField> fields, memory::Span span = {});
    DeclId traitDecl(std::string_view name, memory::DynArray<GenericParam> generic_params,
                     memory::DynArray<TraitMethod> methods, memory::Span span = {});
    DeclId interfaceDecl(std::string_view name, memory::DynArray<GenericParam> generic_params,
                         memory::DynArray<TraitMethod> methods, memory::Span span = {});
    DeclId importDecl(memory::DynArray<std::string_view> path,
                      memory::DynArray<ImportSymbol> symbols, std::string_view alias = {},
                      bool is_from = false, bool is_export = false, bool is_asset = false,
                      int32_t import_depth = 1, memory::Span span = {});
    DeclId typeAliasDecl(std::string_view name, memory::DynArray<GenericParam> generic_params,
                         TypeExprId target_type, memory::Span span = {});
    DeclId globalDecl(std::string_view name, bool is_const,
                      TypeExprId type_annot = kInvalidTypeExpr, ExprId init = kInvalidExpr,
                      memory::Span span = {});

    ExprId wordCall(std::string_view word_name, memory::DynArray<ExprId> args,
                    memory::Span span = {});
    StmtId useStmt(std::string_view context_name, memory::DynArray<std::string_view> words,
                   std::string_view alias_name, std::string_view target_path,
                   ExprId block = kInvalidExpr, memory::Span span = {});
    DeclId wordDecl(std::string_view name, WordCategory category,
                    memory::DynArray<std::string_view> params, ExprId body = kInvalidExpr,
                    memory::Span span = {});
    DeclId contextDecl(std::string_view name, memory::DynArray<DeclId> decls,
                       ExprId body = kInvalidExpr, memory::Span span = {});

    ExprId unbody(memory::Span body_span, uint32_t token_start, uint32_t token_end);
    ExprId errorExpr(memory::Span span = {});
    StmtId errorStmt(memory::Span span = {});
    DeclId errorDecl(memory::Span span = {});
    ExprId intrinsic(IntrinsicKind kind, memory::DynArray<ExprId> args, memory::Span span = {});
    ExprId macroCall(std::string_view name, memory::DynArray<ExprId> args, memory::Span span = {});

    // ── Type expression helpers ──────────────────────────────────────
    TypeExprId addTypeExpr(TypeExprNode node);
    TypeExprNode &getTypeExpr(TypeExprId id);
    const TypeExprNode &getTypeExpr(TypeExprId id) const;
    TypeExprId builtinExpr(BuiltinType kind);
    TypeExprId pathExpr(memory::DynArray<std::string_view> segments, memory::Span span = {});
    TypeExprId inferExpr();
    TypeExprId dynExpr(TypeExprId inner);
    TypeExprId unionExpr();
    TypeExprId typeSpecialization(TypeExprId base, memory::DynArray<TypeExprId> args);

    memory::Span exprSpan(ExprId id) const;
    memory::Span stmtSpan(StmtId id) const;
    memory::Span declSpan(DeclId id) const;

    memory::Arena &arena();
    memory::StringInterner &interner();
};

} // namespace zith::ast
