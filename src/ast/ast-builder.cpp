#include "ast-builder.hpp"

namespace zith::ast {

namespace {

template <typename T> struct ExprTagFor;
template <typename T> struct StmtTagFor;
template <typename T> struct DeclTagFor;
template <> struct ExprTagFor<LitValue> {
    static constexpr ExprKind value = ExprKind::Literal;
};
template <> struct ExprTagFor<IdentNode> {
    static constexpr ExprKind value = ExprKind::Identifier;
};
template <> struct ExprTagFor<BinaryNode> {
    static constexpr ExprKind value = ExprKind::Binary;
};
template <> struct ExprTagFor<UnaryNode> {
    static constexpr ExprKind value = ExprKind::Unary;
};
template <> struct ExprTagFor<CallNode> {
    static constexpr ExprKind value = ExprKind::Call;
};
template <> struct ExprTagFor<BlockNode> {
    static constexpr ExprKind value = ExprKind::Block;
};
template <> struct ExprTagFor<IfNode> {
    static constexpr ExprKind value = ExprKind::If;
};
template <> struct ExprTagFor<WhileNode> {
    static constexpr ExprKind value = ExprKind::While;
};
template <> struct ExprTagFor<FieldNode> {
    static constexpr ExprKind value = ExprKind::Field;
};
template <> struct ExprTagFor<IndexNode> {
    static constexpr ExprKind value = ExprKind::Index;
};
template <> struct ExprTagFor<StructLiteralNode> {
    static constexpr ExprKind value = ExprKind::StructLiteral;
};
template <> struct ExprTagFor<ArrayLiteralNode> {
    static constexpr ExprKind value = ExprKind::ArrayLiteral;
};
template <> struct ExprTagFor<EnumValueNode> {
    static constexpr ExprKind value = ExprKind::EnumValue;
};
template <> struct ExprTagFor<RangeNode> {
    static constexpr ExprKind value = ExprKind::Range;
};
template <> struct ExprTagFor<WhenNode> {
    static constexpr ExprKind value = ExprKind::When;
};
template <> struct ExprTagFor<UnbodyNode> {
    static constexpr ExprKind value = ExprKind::Unbody;
};
template <> struct ExprTagFor<IntrinsicNode> {
    static constexpr ExprKind value = ExprKind::Intrinsic;
};
template <> struct ExprTagFor<MacroCallNode> {
    static constexpr ExprKind value = ExprKind::MacroCall;
};
template <> struct ExprTagFor<SeqNode> {
    static constexpr ExprKind value = ExprKind::Sequence;
};

template <> struct ExprTagFor<WordCallNode> {
    static constexpr ExprKind value = ExprKind::WordCall;
};
template <> struct ExprTagFor<ErrorExprNode> {
    static constexpr ExprKind value = ExprKind::Error;
};
template <> struct StmtTagFor<UseNode> {
    static constexpr StmtKind value = StmtKind::Use;
};
template <> struct StmtTagFor<ErrorStmtNode> {
    static constexpr StmtKind value = StmtKind::Error;
};
template <> struct DeclTagFor<WordDeclNode> {
    static constexpr DeclKind value = DeclKind::Word;
};
template <> struct DeclTagFor<ContextDeclNode> {
    static constexpr DeclKind value = DeclKind::Context;
};
template <> struct DeclTagFor<ErrorDeclNode> {
    static constexpr DeclKind value = DeclKind::Error;
};
template <typename T> struct StmtTagFor;
template <> struct StmtTagFor<LetNode> {
    static constexpr StmtKind value = StmtKind::Let;
};
template <> struct StmtTagFor<AssignNode> {
    static constexpr StmtKind value = StmtKind::Assign;
};
template <> struct StmtTagFor<RetNode> {
    static constexpr StmtKind value = StmtKind::Return;
};
template <> struct StmtTagFor<GotoNode> {
    static constexpr StmtKind value = StmtKind::Goto;
};
template <> struct StmtTagFor<MarkerNode> {
    static constexpr StmtKind value = StmtKind::Marker;
};
template <> struct StmtTagFor<ExprStmtNode> {
    static constexpr StmtKind value = StmtKind::Expr;
};

template <typename T> struct DeclTagFor;
template <> struct DeclTagFor<FnDeclNode> {
    static constexpr DeclKind value = DeclKind::Fn;
};
template <> struct DeclTagFor<StructDeclNode> {
    static constexpr DeclKind value = DeclKind::Struct;
};
template <> struct DeclTagFor<EnumDeclNode> {
    static constexpr DeclKind value = DeclKind::Enum;
};
template <> struct DeclTagFor<UnionDeclNode> {
    static constexpr DeclKind value = DeclKind::Union;
};
template <> struct DeclTagFor<ComponentDeclNode> {
    static constexpr DeclKind value = DeclKind::Component;
};
template <> struct DeclTagFor<TraitDeclNode> {
    static constexpr DeclKind value = DeclKind::Trait;
};
template <> struct DeclTagFor<InterfaceDeclNode> {
    static constexpr DeclKind value = DeclKind::Interface;
};
template <> struct DeclTagFor<ImportNode> {
    static constexpr DeclKind value = DeclKind::Import;
};
template <> struct DeclTagFor<TypeAliasDeclNode> {
    static constexpr DeclKind value = DeclKind::TypeAlias;
};
template <> struct DeclTagFor<GlobalDeclNode> {
    static constexpr DeclKind value = DeclKind::Global;
};

void normalizeExprNode(ExprNode &node) {
    std::visit([](auto &entry) { entry.tag = ExprTagFor<std::decay_t<decltype(entry)>>::value; },
               node);
}

void normalizeStmtNode(StmtNode &node) {
    std::visit([](auto &entry) { entry.tag = StmtTagFor<std::decay_t<decltype(entry)>>::value; },
               node);
}

void normalizeDeclNode(DeclNode &node) {
    std::visit([](auto &entry) { entry.tag = DeclTagFor<std::decay_t<decltype(entry)>>::value; },
               node);
}

} // anonymous namespace

AstBuilder::AstBuilder(memory::Arena &arena, memory::StringInterner &interner)
    : arena_(arena), interner_(interner), exprs_(arena), stmts_(arena), decls_(arena),
      type_exprs_(arena) {}

ExprId AstBuilder::addExpr(ExprNode node) {
    normalizeExprNode(node);
    ExprId id = static_cast<ExprId>(exprs_.size());
    exprs_.push(std::move(node));
    return id;
}

StmtId AstBuilder::addStmt(StmtNode node) {
    normalizeStmtNode(node);
    StmtId id = static_cast<StmtId>(stmts_.size());
    stmts_.push(std::move(node));
    return id;
}

StmtId AstBuilder::addStmt(ExprId expr, memory::Span span) {
    return addStmt(ExprStmtNode{expr, span});
}

DeclId AstBuilder::addDecl(DeclNode node) {
    normalizeDeclNode(node);
    DeclId id = static_cast<DeclId>(decls_.size());
    decls_.push(std::move(node));
    return id;
}

ExprNode &AstBuilder::getExpr(ExprId id) {
    return exprs_[id];
}
StmtNode &AstBuilder::getStmt(StmtId id) {
    return stmts_[id];
}
DeclNode &AstBuilder::getDecl(DeclId id) {
    return decls_[id];
}
const ExprNode &AstBuilder::getExpr(ExprId id) const {
    return exprs_[id];
}
const StmtNode &AstBuilder::getStmt(StmtId id) const {
    return stmts_[id];
}
const DeclNode &AstBuilder::getDecl(DeclId id) const {
    return decls_[id];
}

LitValue AstBuilder::makeLit(LitKind kind, std::string_view raw, memory::Span span) {
    return LitValue{raw, span, kind};
}

ExprId AstBuilder::litExpr(LitKind kind, std::string_view raw, memory::Span span) {
    return addExpr(makeLit(kind, raw, span));
}

ExprId AstBuilder::ident(std::string_view name, memory::Span span, bool scope_escape) {
    return addExpr(IdentNode{name, span, scope_escape});
}

ExprId AstBuilder::binary(ExprId lhs, BinaryOp op, ExprId rhs, memory::Span span) {
    return addExpr(BinaryNode{span, lhs, rhs, op});
}

ExprId AstBuilder::unary(UnaryOp op, ExprId operand, memory::Span span) {
    return addExpr(UnaryNode{span, operand, op});
}

ExprId AstBuilder::call(ExprId callee, memory::DynArray<ExprId> args, memory::Span span) {
    return addExpr(CallNode{std::move(args), span, callee});
}

ExprId AstBuilder::field(ExprId object, std::string_view field_name, memory::Span span) {
    return addExpr(FieldNode{field_name, span, object});
}

ExprId AstBuilder::index(ExprId object, ExprId index, memory::Span span) {
    return addExpr(IndexNode{object, index, span});
}

ExprId AstBuilder::structLiteral(std::string_view type_name,
                                 memory::DynArray<StructFieldInit> fields, memory::Span span) {
    return addExpr(StructLiteralNode{type_name, std::move(fields), span});
}

ExprId AstBuilder::arrayLiteral(memory::DynArray<ExprId> elements, memory::Span span) {
    return addExpr(ArrayLiteralNode{std::move(elements), span});
}

ExprId AstBuilder::enumValue(std::string_view enum_name, std::string_view variant_name,
                             memory::Span span) {
    return addExpr(EnumValueNode{enum_name, variant_name, span});
}

ExprId AstBuilder::range(ExprId lhs, ExprId rhs, memory::Span span) {
    return addExpr(RangeNode{lhs, rhs, span});
}

ExprId AstBuilder::range(ExprId lhs, ExprId rhs, RangeBounds bounds, memory::Span span) {
    return addExpr(RangeNode{lhs, rhs, bounds, span});
}

ExprId AstBuilder::whenExpr(ExprId subject, memory::DynArray<WhenCase> cases, memory::Span span) {
    return addExpr(WhenNode{subject, std::move(cases), span});
}

ExprId AstBuilder::block(memory::DynArray<StmtId> stmts, ExprId trailing, memory::Span span) {
    return addExpr(BlockNode{std::move(stmts), trailing, span});
}

ExprId AstBuilder::ifExpr(ExprId cond, ExprId then_branch, ExprId else_branch, memory::Span span) {
    return addExpr(IfNode{cond, then_branch, else_branch, span});
}

ExprId AstBuilder::whileExpr(ExprId cond, ExprId body, memory::Span span) {
    return addExpr(WhileNode{cond, body, span});
}

StmtId AstBuilder::letStmt(memory::DynArray<std::string_view> names, bool mut,
                           TypeExprId type_annot, ExprId init, memory::Span span) {
    return addStmt(LetNode{std::move(names), span, type_annot, init, mut});
}

StmtId AstBuilder::letStmt(std::string_view name, bool mut, ExprId init, memory::Span span) {
    memory::DynArray<std::string_view> names{arena_};
    names.push(name);
    return addStmt(LetNode{std::move(names), span, kInvalidTypeExpr, init, mut});
}

StmtId AstBuilder::assign(ExprId target, ExprId value, memory::Span span) {
    return addStmt(AssignNode{target, value, span});
}

StmtId AstBuilder::retStmt(ExprId value, memory::Span span) {
    return addStmt(RetNode{value, span});
}

StmtId AstBuilder::gotoStmt(std::string_view target, memory::Span span) {
    return addStmt(GotoNode{target, span});
}

StmtId AstBuilder::markerStmt(std::string_view name, memory::DynArray<StmtId> body,
                              memory::Span span) {
    return addStmt(MarkerNode{name, std::move(body), span});
}

DeclId AstBuilder::fnDecl(std::string_view name, memory::DynArray<GenericParam> generic_params,
                          memory::DynArray<FnParam> params, TypeExprId return_type, ExprId body,
                          bool is_extern, memory::Span span) {
    return addDecl(FnDeclNode{name, std::move(generic_params), std::move(params), return_type, body,
                              is_extern, span});
}

DeclId AstBuilder::fnDecl(std::string_view name, memory::DynArray<std::string_view> param_names,
                          ExprId body, memory::Span span) {
    memory::DynArray<FnParam> params{arena_};
    for (auto &pn : param_names)
        params.push({pn, kInvalidTypeExpr});
    return addDecl(FnDeclNode{name, memory::DynArray<GenericParam>{arena_}, std::move(params),
                              kInvalidTypeExpr, body, false, span});
}

DeclId AstBuilder::structDecl(std::string_view name, memory::DynArray<GenericParam> generic_params,
                              memory::DynArray<StructField> fields,
                              memory::DynArray<DeclId> methods, TypeExprId extends_parent,
                              memory::Span span) {
    memory::DynArray<TypeExprId> empty_traits{arena_};
    return addDecl(StructDeclNode{name, std::move(generic_params), std::move(fields),
                                  std::move(methods), extends_parent, std::move(empty_traits),
                                  span});
}

DeclId AstBuilder::enumDecl(std::string_view name, memory::DynArray<GenericParam> generic_params,
                            memory::DynArray<EnumVariant> variants, TypeExprId underlying_type,
                            memory::Span span) {
    return addDecl(
        EnumDeclNode{std::move(generic_params), std::move(variants), name, underlying_type, span});
}

DeclId AstBuilder::unionDecl(std::string_view name, memory::DynArray<GenericParam> generic_params,
                             memory::DynArray<UnionVariant> variants, bool is_raw,
                             memory::Span span) {
    return addDecl(
        UnionDeclNode{name, std::move(generic_params), std::move(variants), span, is_raw});
}

DeclId AstBuilder::componentDecl(std::string_view name,
                                 memory::DynArray<GenericParam> generic_params,
                                 memory::DynArray<StructField> fields, memory::Span span) {
    return addDecl(ComponentDeclNode{name, std::move(generic_params), std::move(fields),
                                     memory::DynArray<ast::DeclId>(arena_), span});
}

DeclId AstBuilder::traitDecl(std::string_view name, memory::DynArray<GenericParam> generic_params,
                             memory::DynArray<TraitMethod> methods, memory::Span span) {
    return addDecl(TraitDeclNode{name, std::move(generic_params), std::move(methods), span});
}

DeclId AstBuilder::interfaceDecl(std::string_view name,
                                 memory::DynArray<GenericParam> generic_params,
                                 memory::DynArray<TraitMethod> methods, memory::Span span) {
    return addDecl(InterfaceDeclNode{name, std::move(generic_params), std::move(methods), span});
}

DeclId AstBuilder::importDecl(memory::DynArray<std::string_view> path,
                              memory::DynArray<ImportSymbol> symbols, std::string_view alias,
                              bool is_from, bool is_export, bool is_asset, int32_t import_depth,
                              memory::Span span) {
    return addDecl(ImportNode{std::move(path), std::move(symbols), alias, span, import_depth,
                              is_from, is_export, is_asset});
}

DeclId AstBuilder::typeAliasDecl(std::string_view name,
                                 memory::DynArray<GenericParam> generic_params,
                                 TypeExprId target_type, memory::Span span) {
    return addDecl(TypeAliasDeclNode{name, std::move(generic_params), target_type, span});
}

DeclId AstBuilder::globalDecl(std::string_view name, bool is_const, TypeExprId type_annot,
                              ExprId init, memory::Span span) {
    return addDecl(GlobalDeclNode{name, span, is_const, type_annot, init});
}

ExprId AstBuilder::unbody(memory::Span body_span, uint32_t token_start, uint32_t token_end) {
    return addExpr(UnbodyNode{body_span, token_start, token_end});
}

ExprId AstBuilder::errorExpr(memory::Span span) {
    return addExpr(ErrorExprNode{span});
}

StmtId AstBuilder::errorStmt(memory::Span span) {
    return addStmt(ErrorStmtNode{span});
}

DeclId AstBuilder::errorDecl(memory::Span span) {
    return addDecl(ErrorDeclNode{span});
}

ExprId AstBuilder::intrinsic(IntrinsicKind kind, memory::DynArray<ExprId> args, memory::Span span) {
    return addExpr(IntrinsicNode{kind, std::move(args), span});
}

ExprId AstBuilder::macroCall(std::string_view name, memory::DynArray<ExprId> args,
                             memory::Span span) {
    return addExpr(MacroCallNode{name, std::move(args), span});
}

ExprId AstBuilder::seq(memory::DynArray<ExprId> operands, memory::DynArray<ast::OpMarker> ops,
                       memory::Span span) {
    return addExpr(SeqNode{std::move(operands), std::move(ops), span});
}

// ── Type expression helpers ─────────────────────────────────────────

TypeExprId AstBuilder::addTypeExpr(TypeExprNode node) {
    TypeExprId id = static_cast<TypeExprId>(type_exprs_.size());
    type_exprs_.push(std::move(node));
    return id;
}

TypeExprNode &AstBuilder::getTypeExpr(TypeExprId id) {
    return type_exprs_[id];
}

const TypeExprNode &AstBuilder::getTypeExpr(TypeExprId id) const {
    return type_exprs_[id];
}

TypeExprId AstBuilder::builtinExpr(BuiltinType kind) {
    return addTypeExpr(TypeBuiltin{kind});
}

TypeExprId AstBuilder::pathExpr(memory::DynArray<std::string_view> segments, memory::Span span) {
    TypePath p{std::move(segments)};
    p.span = span;
    return addTypeExpr(std::move(p));
}

TypeExprId AstBuilder::inferExpr() {
    return addTypeExpr(TypeInfer{});
}

TypeExprId AstBuilder::dynExpr(TypeExprId inner) {
    return addTypeExpr(TypeDyn{inner});
}

TypeExprId AstBuilder::unionExpr() {
    return addTypeExpr(TypeUnion{});
}

TypeExprId AstBuilder::typeSpecialization(TypeExprId base, memory::DynArray<TypeExprId> args) {
    return addTypeExpr(TypeSpecialization{base, std::move(args)});
}

memory::Span AstBuilder::exprSpan(ExprId id) const {
    return std::visit([](auto &node) { return node.span; }, exprs_[id]);
}

memory::Span AstBuilder::stmtSpan(StmtId id) const {
    return std::visit([](auto &node) { return node.span; }, stmts_[id]);
}

memory::Span AstBuilder::declSpan(DeclId id) const {
    return std::visit([](auto &node) { return node.span; }, decls_[id]);
}

memory::Arena &AstBuilder::arena() {
    return arena_;
}
memory::StringInterner &AstBuilder::interner() {
    return interner_;
}

ExprId AstBuilder::wordCall(std::string_view word_name, memory::DynArray<ExprId> args,
                            memory::Span span) {
    return addExpr(WordCallNode{word_name, std::move(args), span});
}

StmtId AstBuilder::useStmt(std::string_view context_name, memory::DynArray<std::string_view> words,
                           std::string_view alias_name, std::string_view target_path, ExprId block,
                           memory::Span span) {
    return addStmt(UseNode{context_name, std::move(words), alias_name, target_path, block, span});
}

DeclId AstBuilder::wordDecl(std::string_view name, WordCategory category,
                            memory::DynArray<std::string_view> params, ExprId body,
                            memory::Span span) {
    return addDecl(WordDeclNode{name, category, std::move(params), body, span});
}

DeclId AstBuilder::contextDecl(std::string_view name, memory::DynArray<DeclId> decls, ExprId body,
                               memory::Span span) {
    return addDecl(ContextDeclNode{name, std::move(decls), body, span});
}

} // namespace zith::ast
