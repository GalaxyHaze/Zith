#pragma once

#include "common/ast-ids.hpp"
#include "memory/dyn-array.hpp"
#include "memory/span.hpp"

#include <cstdint>
#include <string_view>
#include <variant>

namespace zith::ast {

enum class BinaryOp : uint8_t {
    Add,
    Sub,
    Mul,
    Div,
    Rest,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    And,
    Or,
    Xor,
    Shl,
    Shr,
    Is,
    As
};

enum class UnaryOp : uint8_t {
    Neg,
    Not,
    Ref,
    Deref,
    FallbackOpt,
    FallbackRes,
    PropagateOpt,
    PropagateRes
};

enum class LitKind : uint8_t { Int, Float, Bool, String, Char, Nil };

struct LitValue {
    std::string_view raw;
    memory::Span span{};
    LitKind kind;
    int : 24;
    ExprKind tag = ExprKind::Literal;
};

struct BinaryNode {
    memory::Span span{};
    ExprId lhs;
    ExprId rhs;
    BinaryOp op;
    int : 24;
    ExprKind tag = ExprKind::Binary;
};

struct UnaryNode {
    memory::Span span{};
    ExprId operand;
    UnaryOp op;
    int : 24;
    ExprKind tag = ExprKind::Unary;
};

struct CallNode {
    memory::DynArray<ExprId> args;
    memory::Span span{};
    ExprId callee;
    ExprKind tag = ExprKind::Call;
};

struct IdentNode {
    std::string_view name;
    memory::Span span{};
    bool scope_escape = false;
    int : 24;
    ExprKind tag = ExprKind::Identifier;
};

struct FieldNode {
    std::string_view field;
    memory::Span span{};
    ExprId object;
    ExprKind tag = ExprKind::Field;
};

struct IndexNode {
    ExprId object;
    ExprId index;
    memory::Span span{};
    ExprKind tag = ExprKind::Index;
};

struct StructFieldInit {
    std::string_view name;
    ExprId value; // kInvalidExpr for `_`
};

/// Aggregate construction: `Point{1, 2}` or `Point{x: 1, y: _}`.
struct StructLiteralNode {
    std::string_view type_name;
    memory::DynArray<StructFieldInit> fields;
    memory::Span span{};
    ExprKind tag = ExprKind::StructLiteral;
};

/// Array literal: `[1, 2, 3]`
struct ArrayLiteralNode {
    memory::DynArray<ExprId> elements;
    memory::Span span{};
    ExprKind tag = ExprKind::ArrayLiteral;
};

/// An enum variant constant, produced by `Color.Red`.
struct EnumValueNode {
    std::string_view enum_name;
    std::string_view variant_name;
    memory::Span span{};
    ExprKind tag = ExprKind::EnumValue;
};

enum class RangeBounds : uint8_t { Closed, OpenLeft, OpenRight, Open };

struct RangeNode {
    ExprId lhs;
    ExprId rhs;
    RangeBounds bounds = RangeBounds::Closed;
    memory::Span span{};
    ExprKind tag = ExprKind::Range;
};

struct WhenCase {
    ExprId condition;
    ExprId body;
    bool is_default = false;
    memory::Span span{};
};

struct WhenNode {
    ExprId subject;
    memory::DynArray<WhenCase> cases;
    memory::Span span{};
    ExprKind tag = ExprKind::When;
};

struct BlockNode {
    memory::DynArray<StmtId> stmts;
    ExprId trailing = kInvalidExpr;
    memory::Span span{};
    ExprKind tag = ExprKind::Block;
};

struct IfNode {
    ExprId cond;
    ExprId then_branch;
    ExprId else_branch = kInvalidExpr;
    memory::Span span{};
    ExprKind tag = ExprKind::If;
};

struct WhileNode {
    ExprId cond;
    ExprId body;
    memory::Span span{};
    ExprKind tag = ExprKind::While;
};

struct FnParam {
    std::string_view name;
    TypeExprId type = kInvalidTypeExpr;
    int : 32;
};

struct GenericParam {
    std::string_view name;
    memory::DynArray<TypeExprId> bounds;
};

struct LetNode {
    memory::DynArray<std::string_view> names;
    memory::Span span{};
    TypeExprId type_annot = kInvalidTypeExpr;
    ExprId init           = kInvalidExpr;
    bool mut              = false;
    int : 24;
    StmtKind tag = StmtKind::Let;
};

struct AssignNode {
    ExprId target;
    ExprId value;
    memory::Span span{};
    StmtKind tag = StmtKind::Assign;
};

struct RetNode {
    ExprId value = kInvalidExpr;
    memory::Span span{};
    StmtKind tag = StmtKind::Return;
};

struct FnDeclNode {
    std::string_view name;
    memory::DynArray<GenericParam> generic_params;
    memory::DynArray<FnParam> params;
    TypeExprId return_type = kInvalidTypeExpr;
    ExprId body            = kInvalidExpr;
    bool is_extern         = false;
    memory::Span span{};
    int : 24;
    DeclKind tag = DeclKind::Fn;
};

enum class FieldBind : uint8_t { Auto, Const, Let, Var, Global, Comptime };
enum class FieldKind : uint8_t { Struct, Enum, Union, Component };

struct StructField {
    std::string_view name;
    TypeExprId type_expr = kInvalidTypeExpr;
    ExprId default_value = kInvalidExpr;
    FieldBind bind       = FieldBind::Auto;
    FieldKind kind       = FieldKind::Struct;
};

struct StructDeclNode {
    std::string_view name;
    memory::DynArray<GenericParam> generic_params;
    memory::DynArray<StructField> fields;
    memory::DynArray<DeclId> methods;
    TypeExprId extends_parent = kInvalidTypeExpr;
    memory::DynArray<TypeExprId> traits;
    memory::Span span{};
    DeclKind tag = DeclKind::Struct;
};

struct EnumVariant {
    std::string_view name;
    memory::DynArray<StructField> fields;
    ExprId discriminant = kInvalidExpr;
};

struct EnumDeclNode {
    memory::DynArray<GenericParam> generic_params;
    memory::DynArray<EnumVariant> variants;
    std::string_view name;
    TypeExprId underlying_type = kInvalidTypeExpr;
    memory::Span span{};
    int : 32;
    DeclKind tag = DeclKind::Enum;
};

struct UnionVariant {
    std::string_view name;
    TypeExprId type_expr = kInvalidTypeExpr;
};

struct UnionDeclNode {
    std::string_view name;
    memory::DynArray<GenericParam> generic_params;
    memory::DynArray<UnionVariant> variants;
    memory::Span span{};
    bool is_raw = false;
    int : 32;
    DeclKind tag = DeclKind::Union;
};

struct ComponentDeclNode {
    std::string_view name;
    memory::DynArray<GenericParam> generic_params;
    memory::DynArray<StructField> fields;
    memory::DynArray<ast::DeclId> methods;
    memory::Span span{};
    int : 32;
    DeclKind tag = DeclKind::Component;
};

struct TraitMethod {
    std::string_view name;
    memory::DynArray<std::string_view> params;
};

struct TraitDeclNode {
    std::string_view name;
    memory::DynArray<GenericParam> generic_params;
    memory::DynArray<TraitMethod> methods;
    memory::Span span{};
    int : 32;
    DeclKind tag = DeclKind::Trait;
};

struct InterfaceDeclNode {
    std::string_view name;
    memory::DynArray<GenericParam> generic_params;
    memory::DynArray<TraitMethod> methods;
    memory::Span span{};
    int : 32;
    DeclKind tag = DeclKind::Interface;
};

struct GlobalDeclNode {
    std::string_view name;
    memory::Span span{};
    bool is_const;
    TypeExprId type_annot = kInvalidTypeExpr;
    ExprId init           = kInvalidExpr;
    int : 24;
    DeclKind tag = DeclKind::Global;
};

struct TypeAliasDeclNode {
    std::string_view name;
    memory::DynArray<GenericParam> generic_params;
    TypeExprId target_type = kInvalidTypeExpr;
    memory::Span span{};
    DeclKind tag = DeclKind::TypeAlias;
};

struct ImportSymbol {
    std::string_view name;
    std::string_view alias; // empty = no renaming
};

struct ImportNode {
    memory::DynArray<std::string_view> path;
    memory::DynArray<ImportSymbol> symbols; // empty = import all
    std::string_view alias;
    memory::Span span{};
    int32_t import_depth = 1;
    bool is_from         = false;
    bool is_export       = false;
    bool is_asset        = false;
    int : 8;
    int : 32;
    DeclKind tag = DeclKind::Import;
};

struct UnbodyNode {
    memory::Span body_span;
    uint32_t token_start;
    uint32_t token_end;
    memory::Span span{};
    ExprKind tag = ExprKind::Unbody;
};

struct ProgramNode {
    memory::DynArray<DeclId> decls;

    explicit ProgramNode(memory::Arena &arena) : decls(arena) {}
};

enum class IntrinsicKind : uint8_t {
    Fields,
    SizeOf,
    AlignOf,
    HasTrait,
    Struct,
    Component,
    Union,
    Enum,
    Nullable,
    Primitive,
    Allocate,
    Pack,
    ToStruct,
    ToPack,
    AppendField,
    RemoveField,
    AppendMethod,
    File,
    Line,
    FnName,
    Location,
    Ok,
    Err,
    OffsetOf,
};

struct IntrinsicNode {
    IntrinsicKind kind;
    memory::DynArray<ExprId> args;
    memory::Span span{};
    int : 24;
    ExprKind tag = ExprKind::Intrinsic;
};

struct MacroCallNode {
    std::string_view name;
    memory::DynArray<ExprId> args;
    memory::Span span{};
    int : 32;
    ExprKind tag = ExprKind::MacroCall;
};

struct OpMarker {
    memory::Span span{};
    ast::BinaryOp builtin_op = BinaryOp::Add;
    std::string_view word_name;
    uint8_t builtin_prec = 0;
    bool is_word         = false;
};

enum class WordCategory : uint8_t { Nop, Prefix, Suffix, Infix };

struct WordDeclNode {
    std::string_view name;
    WordCategory category = WordCategory::Nop;
    memory::DynArray<std::string_view> params;
    ExprId body = kInvalidExpr;
    memory::Span span{};
    DeclKind tag = DeclKind::Word;
};

struct ContextDeclNode {
    std::string_view name;
    memory::DynArray<DeclId> decls;
    ExprId body = kInvalidExpr;
    memory::Span span{};
    DeclKind tag = DeclKind::Context;
};

struct WordCallNode {
    std::string_view word_name;
    memory::DynArray<ExprId> args;
    memory::Span span{};
    ExprKind tag = ExprKind::WordCall;
};

struct UseNode {
    std::string_view context_name;
    memory::DynArray<std::string_view> words;
    std::string_view alias_name;
    std::string_view target_path;
    ExprId block = kInvalidExpr;
    memory::Span span{};
    StmtKind tag = StmtKind::Use;
};

struct SeqNode {
    memory::DynArray<ExprId> operands;
    memory::DynArray<OpMarker> ops;
    memory::Span span{};
    ExprKind tag = ExprKind::Sequence;
};

/// Created by the parser's error-recovery path; sema suppresses cascades from this node.
struct ErrorExprNode {
    memory::Span span{};
    ExprKind tag = ExprKind::Error;
};

using ExprNode = std::variant<LitValue, IdentNode, BinaryNode, UnaryNode, CallNode, BlockNode,
                              IfNode, WhileNode, FieldNode, IndexNode, StructLiteralNode,
                              ArrayLiteralNode, EnumValueNode, RangeNode, UnbodyNode, IntrinsicNode,
                              MacroCallNode, SeqNode, WordCallNode, WhenNode, ErrorExprNode>;

struct GotoNode {
    std::string_view target;
    memory::Span span{};
    int : 32;
    StmtKind tag = StmtKind::Goto;
};

struct MarkerNode {
    std::string_view name;
    memory::DynArray<ast::StmtId> body;
    memory::Span span{};
    int : 32;
    StmtKind tag = StmtKind::Marker;
};

struct ExprStmtNode {
    ExprId expr = kInvalidExpr;
    memory::Span span{};
    StmtKind tag = StmtKind::Expr;
};

/// Created by the parser's error-recovery path at statement level.
struct ErrorStmtNode {
    memory::Span span{};
    StmtKind tag = StmtKind::Error;
};

using StmtNode = std::variant<LetNode, AssignNode, RetNode, GotoNode, MarkerNode, ExprStmtNode,
                              UseNode, ErrorStmtNode>;

/// Created by the parser's error-recovery path at declaration level.
struct ErrorDeclNode {
    memory::Span span{};
    DeclKind tag = DeclKind::Error;
};

using DeclNode =
    std::variant<FnDeclNode, StructDeclNode, EnumDeclNode, UnionDeclNode, ComponentDeclNode,
                 TraitDeclNode, InterfaceDeclNode, ImportNode, TypeAliasDeclNode, GlobalDeclNode,
                 WordDeclNode, ContextDeclNode, ErrorDeclNode>;

} // namespace zith::ast
