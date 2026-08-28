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
    MakeSlice,
    SlotAddr,
    Cast,
    UnionCheck,
    LayoutIntrinsic,
    StateTailCall,
    Cleanup,
    GlobalConstLoad,
    MakeDyn,
    DynCall,
    MakeOpaque,
    OpaqueCast,
    OpaqueCheck,
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
    /// Lowered function type for indirect calls (`callee` is an expression).
    /// Direct calls resolved by symbol keep this invalid.
    types::TypeId fn_type      = types::kInvalidType;
    symbols::SymId resolved_fn = symbols::kInvalidSym;
    /// True when this call targets a `state` function and must use LLVM `tailcc`.
    bool usesTailCC = false;
    /// True when this call is a direct LLVM `musttail` state transition.
    bool musttail   = false;
    HirExprKind tag = HirExprKind::Call;

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
    HirExprKind tag = HirExprKind::Jump;
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

/// A zero-copy view over an array or an existing slice. When `checked` is true,
/// runtime bounds validation returns None on failure; otherwise the bounds are
/// statically known to describe the whole array.
struct HirMakeSlice {
    HirExprId object      = kInvalidHirExpr;
    HirExprId lo          = kInvalidHirExpr;
    HirExprId hi          = kInvalidHirExpr;
    HirTypeId type        = types::kInvalidType; // slice type
    HirTypeId object_type = types::kInvalidType;
    HirTypeId bound_type  = types::kInvalidType;
    bool is_array         = false;
    bool is_pointer       = false;
    bool checked          = false;
    HirExprKind tag       = HirExprKind::MakeSlice;
};

/// Reinterpret the bytes of a value through the given union type. Unlike a
/// numeric `as`, this stores into union storage and never converts the value.
struct HirUnionCast {
    HirExprId value;
    HirTypeId from;
    HirTypeId to;
    /// Member index when `to` is a tagged union member, otherwise ~0U.
    uint32_t member_index = ~0U;
    /// When extracting from a tagged union, verify the stored tag before reading.
    bool checked    = false;
    HirExprKind tag = HirExprKind::Cast;
};
/// Tagged-union member test (`x is Member`). Materialised as a separate HIR
/// node so the checked member type and the tested value stay explicit.
struct HirUnionCheck {
    HirExprId value;
    HirTypeId union_type  = types::kInvalidType;
    uint32_t member_index = ~0U;
    HirExprKind tag       = HirExprKind::UnionCheck;
};

/// The offsetOf / alignOf / sizeOf layout intrinsics; resolved to a constant at codegen.
struct HirLayoutIntrinsic {
    enum class Which : uint8_t { OffsetOf, AlignOf, SizeOf };
    Which which;
    HirTypeId type       = types::kInvalidType;
    uint32_t field_index = ~0U; // OffsetOf only
    HirExprKind tag      = HirExprKind::LayoutIntrinsic;
};

/// A terminating transfer to another state in the same machine. The call is
/// lowered as a direct LLVM `musttail` call followed immediately by `ret`.
struct HirStateTailCall {
    hir::HirCall call;
    HirExprKind tag = HirExprKind::StateTailCall;

    explicit HirStateTailCall(memory::Arena &arena) : call(arena) {}
};

/// A block-level cleanup sequence. The expressions run in reverse order when
/// the enclosing block exits; codegen emits them before the terminator.
struct HirCleanup {
    memory::DynArray<HirExprId> exprs;
    HirExprKind tag = HirExprKind::Cleanup;

    explicit HirCleanup(memory::Arena &arena) : exprs(arena) {}
};

/// Load of a module-scoped `const` global.
struct HirGlobalConstLoad {
    memory::InternedId name;
    HirTypeId type  = types::kInvalidType;
    HirExprKind tag = HirExprKind::GlobalConstLoad;
};

/// Materialises a `dyn Trait` fat pointer from a concrete aggregate value.
/// The vtable stores only the trait/interface method requirements; interface
/// fields are accessed through the data pointer before the value is erased.
struct HirMakeDyn {
    HirExprId value       = hir::kInvalidHirExpr;
    HirTypeId source_type = types::kInvalidType;
    HirTypeId dyn_type    = types::kInvalidType;
    memory::InternedId vtable_name{};
    HirExprKind tag = HirExprKind::MakeDyn;
};

/// Indirect call through a `dyn` vtable slot. The receiver is the full fat
/// pointer emitted by `HirMakeDyn`; codegen extracts the data pointer,
/// indexes the vtable global and calls the loaded function pointer.
struct HirDynCall {
    HirExprId receiver = hir::kInvalidHirExpr;
    memory::InternedId vtable_name{};
    uint32_t slot_index   = 0;
    bool has_receiver     = true;
    HirTypeId result_type = types::kInvalidType;
    types::TypeId fn_type = types::kInvalidType;
    memory::DynArray<HirExprId> args;
    memory::DynArray<types::TypeId> arg_types;
    HirExprKind tag = HirExprKind::DynCall;

    explicit HirDynCall(memory::Arena &arena) : args(arena), arg_types(arena) {}
};

/// Materialises a bare `opaque` value `{ *void, typeId }` from a concrete
/// value. The concrete value is spilled to a temporary so the data pointer is
/// stable for the lifetime of the erased view.
struct HirMakeOpaque {
    HirExprId value       = hir::kInvalidHirExpr;
    HirTypeId source_type = types::kInvalidType;
    HirTypeId opaque_type = types::kInvalidType;
    uint32_t type_id      = 0;
    HirExprKind tag       = HirExprKind::MakeOpaque;
};

/// Checked (or raw reinterpreting) extraction of a concrete value from a bare
/// `opaque`. When `checked`, codegen compares the stored type id and emits
/// `?T` (`Some(T)` / `None`); raw extraction ignores the tag and emits `T`.
struct HirOpaqueCast {
    HirExprId value       = hir::kInvalidHirExpr;
    HirTypeId from        = types::kInvalidType;
    HirTypeId to          = types::kInvalidType;
    HirTypeId opaque_type = types::kInvalidType;
    HirTypeId result_type = types::kInvalidType; // checked: `?to`, raw: `to`
    uint32_t type_id      = 0;
    bool checked          = false;
    /// `as raw opaque` reinterprets the tagged payload pointer itself (field 0)
    /// as `void*`, so codegen does not load through the payload.
    bool returns_ptr      = false;
    HirExprKind tag       = HirExprKind::OpaqueCast;
};

/// Tag check for `opaque is T`: compares the stored type id with the requested
/// concrete type's module-local id.
struct HirOpaqueCheck {
    HirExprId value       = hir::kInvalidHirExpr;
    HirTypeId opaque_type = types::kInvalidType;
    uint32_t type_id      = 0;
    HirExprKind tag       = HirExprKind::OpaqueCheck;
};

using HirExpr =
    std::variant<HirLiteral, HirBinary, HirUnary, HirLet, HirVar, HirCall, HirRet, HirBranch,
                 HirJump, HirPhi, HirAssign, HirIndex, HirField, HirStructLiteral, HirArrayLiteral,
                 HirEnumValue, HirSlotAlloca, HirSlotStore, HirSlotLoad, HirSlotAddr, HirMakeNone,
                 HirMakeSome, HirMakeSlice, HirCast, HirUnionCast, HirUnionCheck,
                 HirLayoutIntrinsic, HirStateTailCall, HirCleanup, HirGlobalConstLoad, HirMakeDyn,
                 HirDynCall, HirMakeOpaque, HirOpaqueCast, HirOpaqueCheck>;

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
