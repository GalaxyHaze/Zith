#pragma once

#include <cstdint>

namespace zith::ast {

enum class ExprKind : uint8_t {
    Literal,
    Identifier,
    Binary,
    Unary,
    Call,
    Block,
    If,
    While,
    Field,
    Index,
    Range,
    When,
    Unbody,
    Intrinsic,
    MacroCall,
    Sequence,
    WordCall,
    WhenGuard,
    StructLiteral,
    ArrayLiteral,
    EnumValue,
    Error, ///< Produced during error recovery; sema treats its type as kErrorType.
};

enum class StmtKind : uint8_t {
    Let,
    Assign,
    Return,
    Goto,
    Marker,
    Expr,
    Use,
    Error, ///< Statement-level error recovery placeholder.
};

enum class DeclKind : uint8_t {
    Fn,
    Struct,
    Enum,
    Union,
    Component,
    Trait,
    Interface,
    Import,
    TypeAlias,
    Global,
    Word,
    Context,
    Error, ///< Declaration-level error recovery placeholder.
};

using NodeId     = uint32_t;
using ExprId     = uint32_t;
using StmtId     = uint32_t;
using DeclId     = uint32_t;
using TypeExprId = uint32_t;

inline constexpr NodeId kInvalidNode         = ~NodeId{0};
inline constexpr ExprId kInvalidExpr         = ~ExprId{0};
inline constexpr StmtId kInvalidStmt         = ~StmtId{0};
inline constexpr DeclId kInvalidDecl         = ~DeclId{0};
inline constexpr TypeExprId kInvalidTypeExpr = ~TypeExprId{0};

} // namespace zith::ast
