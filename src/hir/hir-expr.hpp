#pragma once

#include "hir/hir-types.hpp"
#include "memory/dyn-array.hpp"
#include "memory/string-interner.hpp"
#include "symbols/symbol-id.hpp"

#include <cstdint>
#include <variant>

namespace zith::hir {

using HirExprId = uint32_t;
using HirDeclId = uint32_t;

inline constexpr HirExprId kInvalidHirExpr = ~HirExprId{0};

enum class HirExprKind : uint8_t {
    Literal,
    Binary,
    Unary,
    Let,
    Var,
    Call,
    Ret,
    Branch,
    Jump,
    Phi,
    Assign,
    Index,
    Field,
    StructLiteral,
    ArrayLiteral,
    EnumValue,
    SlotAlloca,
    SlotStore,
    SlotLoad,
};

enum class HirBinaryOp : uint8_t {
    Add,
    Sub,
    Mul,
    Div,
    Rem,
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
    Shr
};

enum class HirUnaryOp : uint8_t {
    Neg,
    Not,
    BitNot,
    Ref,
    Deref,
};

struct HirLiteral {
    HirTypeId type;
    union {
        int64_t i;
        double f;
        bool b;
        memory::InternedId str_val;
    };
    HirExprKind tag = HirExprKind::Literal;
};

struct HirBinary {
    HirExprId lhs;
    HirExprId rhs;
    HirBinaryOp op;
    HirTypeId type  = types::kInvalidType;
    HirExprKind tag = HirExprKind::Binary;
};
struct HirUnary {
    HirUnaryOp op;
    HirExprId operand;
    HirTypeId type;
    HirExprKind tag = HirExprKind::Unary;
};
struct HirLet {
    memory::InternedId name;
    HirTypeId type;
    HirExprId init;
    HirExprKind tag = HirExprKind::Let;
};
struct HirVar {
    memory::InternedId name;
    uint32_t version;
    HirExprKind tag = HirExprKind::Var;
};
struct HirCall {
    HirExprId callee;
    memory::DynArray<HirExprId> args;
    symbols::SymId resolved_fn = symbols::kInvalidSym;
    HirExprKind tag            = HirExprKind::Call;
};
struct HirRet {
    HirExprId value;
    HirExprKind tag = HirExprKind::Ret;
};
struct HirBranch {
    HirExprId cond;
    HirDeclId then_block;
    HirDeclId else_block;
    HirExprKind tag = HirExprKind::Branch;
};
struct HirJump {
    HirDeclId target;
    HirExprKind tag = HirExprKind::Jump;
};
struct HirPhi {
    memory::DynArray<HirExprId> incoming;
    HirExprKind tag = HirExprKind::Phi;
};

struct HirAssign {
    HirExprId target;
    HirExprId value;
    HirExprKind tag = HirExprKind::Assign;
};

struct HirIndex {
    HirExprId object;
    HirExprId index;
    HirTypeId type;
    HirTypeId obj_type;
    bool is_array   = false;
    HirExprKind tag = HirExprKind::Index;
};

struct HirField {
    HirExprId object;
    uint32_t index;
    HirTypeId type;
    HirTypeId object_type;
    HirExprKind tag = HirExprKind::Field;
};

struct HirStructLiteral {
    memory::DynArray<HirExprId> values;
    HirTypeId type;
    HirExprKind tag = HirExprKind::StructLiteral;
    explicit HirStructLiteral(memory::Arena &arena) : values(arena) {}
};

struct HirArrayLiteral {
    memory::DynArray<HirExprId> elements;
    HirTypeId type;
    HirExprKind tag = HirExprKind::ArrayLiteral;
    explicit HirArrayLiteral(memory::Arena &arena) : elements(arena) {}
};

struct HirEnumValue {
    int64_t value;
    HirTypeId type;
    HirExprKind tag = HirExprKind::EnumValue;
};

/// Internal compiler temporary slot — allocated once, loaded/stored as needed.
/// These identifiers are synthetic and never collide with user names.
using HirSlotId = uint32_t;
inline constexpr HirSlotId kInvalidHirSlot = ~HirSlotId{0};

struct HirSlotAlloca {
    HirSlotId slot;
    HirTypeId type;
    HirExprKind tag = HirExprKind::SlotAlloca;
};

struct HirSlotStore {
    HirSlotId slot;
    HirExprId value;
    HirExprKind tag = HirExprKind::SlotStore;
};

struct HirSlotLoad {
    HirSlotId slot;
    HirTypeId type;
    HirExprKind tag = HirExprKind::SlotLoad;
};

using HirExpr = std::variant<HirLiteral, HirBinary, HirUnary, HirLet, HirVar, HirCall, HirRet,
                             HirBranch, HirJump, HirPhi, HirAssign, HirIndex, HirField,
                             HirStructLiteral, HirArrayLiteral, HirEnumValue,
                             HirSlotAlloca, HirSlotStore, HirSlotLoad>;

inline HirExprKind exprKind(const HirExpr &expr) {
    return std::visit([](const auto &entry) { return entry.tag; }, expr);
}

template <typename Visitor> decltype(auto) visitExpr(const HirExpr &expr, Visitor &&vis) {
    return std::visit(std::forward<Visitor>(vis), expr);
}

template <typename Visitor> decltype(auto) visitExpr(HirExpr &expr, Visitor &&vis) {
    return std::visit(std::forward<Visitor>(vis), expr);
}

} // namespace zith::hir
