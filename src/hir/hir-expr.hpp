#pragma once

#include "hir/hir-types.hpp"
#include "memory/dyn-array.hpp"
#include "memory/string-interner.hpp"
#include "symbols/symbol-id.hpp"
#include "types/type-id.hpp"

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
    MakeNone,
    MakeSome,
    SlotAddr,
    Cast,
    LayoutIntrinsic,
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
    Shr,
    Invalid = 0xFF
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
    HirTypeId type         = types::kInvalidType;
    HirTypeId operand_type = types::kInvalidType;
    HirExprKind tag        = HirExprKind::Binary;
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
    /// Type ids for `args`, kept for ABI promotion rules such as C variadic calls.
    memory::DynArray<types::TypeId> argument_types;
    symbols::SymId resolved_fn = symbols::kInvalidSym;
    HirExprKind tag            = HirExprKind::Call;

    explicit HirCall(memory::Arena &arena) : args(arena), argument_types(arena) {}
    HirCall(HirExprId callee_, memory::DynArray<HirExprId> &&args_,
            memory::DynArray<types::TypeId> &&argument_types_)
        : args(std::move(args_)), argument_types(std::move(argument_types_)) {
        callee = callee_;
    }
};
struct HirRet {
    HirExprId value = kInvalidHirExpr;
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
    /// True for `jump marker` inside a flow function: the marker body returns
    /// to `return_block` when it falls through.
    bool flowReturn = false;
    // Destination used only when `flowReturn` is true. Direct `jump` entries
    // created by the lowerer leave it unread.
    HirDeclId return_block = ~HirDeclId{0};
    HirExprKind tag        = HirExprKind::Jump;
};
struct HirPhi {
    memory::DynArray<HirExprId> incoming;
    HirExprKind tag = HirExprKind::Phi;

    explicit HirPhi(memory::Arena &arena) : incoming(arena) {}
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
using HirSlotId                            = uint32_t;
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

struct HirSlotAddr {
    HirSlotId slot;
    HirTypeId type;
    HirExprKind tag = HirExprKind::SlotAddr;
};

struct HirMakeNone {
    HirTypeId type;
    HirExprKind tag = HirExprKind::MakeNone;
};

/// Numeric conversion produced by the `as` operator.
struct HirCast {
    HirExprId value;
    HirTypeId from;
    HirTypeId to;
    HirExprKind tag = HirExprKind::Cast;
};

struct HirMakeSome {
    HirExprId value;
    HirTypeId type;
    HirExprKind tag = HirExprKind::MakeSome;
};

/// The offsetOf / alignOf / sizeOf layout intrinsics; resolved to a constant at codegen.
struct HirLayoutIntrinsic {
    enum class Which : uint8_t { OffsetOf, AlignOf, SizeOf };
    Which which;
    HirTypeId type       = types::kInvalidType;
    uint32_t field_index = ~0U; // OffsetOf only
    HirExprKind tag      = HirExprKind::LayoutIntrinsic;
};

using HirExpr =
    std::variant<HirLiteral, HirBinary, HirUnary, HirLet, HirVar, HirCall, HirRet, HirBranch,
                 HirJump, HirPhi, HirAssign, HirIndex, HirField, HirStructLiteral, HirArrayLiteral,
                 HirEnumValue, HirSlotAlloca, HirSlotStore, HirSlotLoad, HirSlotAddr, HirMakeNone,
                 HirMakeSome, HirCast, HirLayoutIntrinsic>;

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
