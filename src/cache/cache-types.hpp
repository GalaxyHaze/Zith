#pragma once

#include "memory/dyn-array.hpp"
#include "memory/string-interner.hpp"
#include "symbols/symbol-id.hpp"
#include "symbols/symbol-visibility.hpp"
#include "types/type-id.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace zith::cache {

using HirExprId = uint32_t;

// Compact type encoding used inside artifacts.  Primitive types are stored as
// a single tag; composite types reference other compact type ids stored in the
// same section.  This is self-contained and independent of the in-memory
// TypeIntern so a reader does not need to reconstruct the full type table to
// validate identity.
enum class CompactTypeKind : uint8_t {
    Error        = 0,
    Never        = 1,
    Void         = 2,
    Bool         = 3,
    Char         = 4,
    Int          = 5,
    Float        = 6,
    Ptr          = 7,
    Array        = 8,
    Struct       = 9,
    Fn           = 10,
    Optional     = 11,
    Failable     = 12,
    Slice        = 13,
    Enum         = 14,
    Union        = 15,
    TypeVar      = 16,
    GenericParam = 17,
    Incomplete   = 18,
    Opaque       = 19,
};

struct CompactType {
    CompactTypeKind kind = CompactTypeKind::Error;
    uint8_t int_width    = 0;        // IntWidth / FloatWidth
    uint8_t flags        = 0;        // is_mut | ownership | is_signed
    uint32_t ref0        = 0;        // pointee / elem / base / inner / ret
    uint32_t ref1        = 0;        // count / def_id
    std::vector<uint32_t> args;      // fn params / app args / pack members
    std::vector<uint32_t> arg_names; // pack member names (string ids)
};

// Definition payload for named composite types referenced by CompactType.
// The compact type table carries only def ids, so this table lets a cache reader
// rebuild private/internal composite definitions that are not part of the
// exported DeclRecord table.
struct CompactStructDef {
    std::string name;
    uint32_t name_id = 0;
    std::vector<uint32_t> field_name_ids;
    std::vector<uint32_t> field_type_ids;
};

struct CompactEnumVariant {
    std::string name;
    uint32_t name_id     = 0;
    int64_t discriminant = 0;
};

struct CompactEnumDef {
    std::string name;
    uint32_t name_id       = 0;
    uint32_t underlying_id = ~uint32_t{0};
    std::vector<CompactEnumVariant> variants;
};

struct CompactUnionDef {
    std::string name;
    uint32_t name_id = 0;
    std::vector<uint32_t> member_type_ids;
    bool is_raw = false;
};

// A dependency on another module.  Resolved at load time by matching the
// dependency's canonical path and public ABI hash against what the importer
// currently sees.
struct DependencyRecord {
    std::string canonical_path;
    std::string import_key;
    uint32_t public_abi_hi = 0;
    uint32_t public_abi_lo = 0;
};

enum class CompactSymKind : uint8_t {
    Fn,
    Struct,
    Trait,
    Interface,
    Enum,
    Alias,
    Variable,
    Module,
    Component,
    Union,
    Asset,
    Word,
    Context,
};

// One exported or module-visible declaration.  References metadata by compact
// string id and references types by compact type id.  For generic declarations
// `template_index` points into the templates section; otherwise it is invalid.
struct DeclRecord {
    std::string name;
    CompactSymKind kind                  = CompactSymKind::Variable;
    symbols::SymbolVisibility visibility = symbols::SymbolVisibility::Private;
    int32_t mod_depth                    = 0;
    uint32_t name_id                     = 0;
    uint32_t type_id                     = 0; // primary type (fn sig / struct type / alias target)
    uint32_t template_index              = ~uint32_t{0};
    uint32_t body_fn_index               = ~uint32_t{0}; // index into code section, if concrete
    std::vector<uint32_t> field_name_ids;                // struct/enum/component fields
    std::vector<uint32_t> field_type_ids;
    std::vector<uint32_t> method_decl_indices; // methods, as decl indices
    bool is_extern = false;
};

// Generic parameter with optional bounds (compact type ids).
struct GenericParamRecord {
    std::string name;
    uint32_t name_id = 0;
    std::vector<uint32_t> bound_type_ids;
};

// Blueprint for a generic struct/function/component/etc.  The body of a generic
// function is NOT lowered to HIR in sec5; instead the blueprint holds the
// declarative shape needed to re-instantiate it.
struct TemplateBlueprint {
    std::string name;
    uint32_t name_id    = 0;
    CompactSymKind kind = CompactSymKind::Fn;
    std::vector<GenericParamRecord> params;
    std::vector<GenericParamRecord> method_params; // for implement blocks
    uint32_t return_type_id = 0;                   // fn return type
    std::vector<uint32_t> param_type_ids;          // fn param types
    std::vector<uint32_t> param_name_ids;          // fn param names
    std::vector<uint32_t> field_name_ids;          // struct/component fields
    std::vector<uint32_t> field_type_ids;
    // Canonicalized field layout order (indices into field_* arrays).  Empty
    // when the declaration has no fields or the canonical order equals the
    // declared order.
    std::vector<uint32_t> canonical_field_order;
    std::vector<uint32_t> method_decl_indices; // methods, as decl indices
    std::vector<uint32_t> trait_type_ids;      // implemented traits / extends
    bool is_extern = false;
};

// HIR expression in compact form.  Mirrors hir::HirExpr but uses compact ids
// and std::vector so it is self-contained for serialization.
enum class CompactExprKind : uint8_t {
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
    SlotAddr,
    MakeNone,
    MakeSome,
    MakeSlice,
    Cast,
    LayoutIntrinsic,
    MarkerStore,
    MarkerLoad,
    MarkerDock,
    MarkerJump,
    MarkerRet,
};

enum class CompactBinaryOp : uint8_t {
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
};
enum class CompactUnaryOp : uint8_t { Neg, Not, BitNot, Ref, Deref };

struct CompactExpr {
    CompactExprKind kind = CompactExprKind::Literal;
    uint32_t type_id     = 0;
    uint32_t ref_a       = 0; // lhs / operand / object / callee / target
    uint32_t ref_b       = 0; // rhs / index / value
    uint32_t ref_c       = 0; // then_block / field index / version
    uint32_t ref_d       = 0; // else_block
    uint32_t ref_e       = 0; // Cast::from / LayoutIntrinsic::which / operand type / call fn_type
    uint32_t ref_f       = 0; // LayoutIntrinsic::field_index
    uint8_t op           = 0; // binary/unary op
    uint8_t flags        = 0; // is_array / literal sub-tag
    int64_t int_val      = 0;
    double flt_val       = 0.0;
    uint32_t name_id     = 0;        // let/var name
    std::vector<uint32_t> args;      // call args / phi incoming / literal values
    std::vector<uint32_t> arg_types; // call argument types
};

struct CompactBasicBlock {
    std::vector<uint32_t> insts;
    uint32_t terminator = ~uint32_t{0};
};

struct CompactFunction {
    std::string name;
    uint32_t name_id = 0;
    std::vector<uint32_t> param_type_ids;
    std::vector<uint32_t> param_name_ids;
    uint32_t return_type_id = 0;
    std::vector<CompactBasicBlock> blocks;
    std::vector<CompactExpr> exprs;
    uint32_t instance_index = ~uint32_t{0};
    bool is_extern          = false;
    bool is_variadic        = false;
};

struct CompactMarkerParam {
    uint32_t name_id = 0;
    uint32_t type_id = 0;
    uint32_t offset  = 0;
};

struct CompactMarker {
    uint32_t name_id        = 0;
    uint32_t marker_id      = ~uint32_t{0};
    bool stackful           = false;
    uint32_t blob_offset    = 0;
    uint32_t body_expr      = 0;
    uint32_t module_name_id = 0;
    std::vector<CompactMarkerParam> params;
};

struct HirSlotAttrsRecord {
    uint32_t slot     = 0;
    uint8_t ownership = 0;
    uint8_t consumed  = 0;
    bool nonNull      = false;
};

struct HirCallAttrsRecord {
    uint32_t expr_id = 0;
    std::vector<uint32_t> arg_escapes;
    uint32_t returns_arg = ~uint32_t{0};
};

struct HirFnAttrsRecord {
    uint32_t fn_index       = 0;
    uint8_t return_consumed = 0;
    bool nonNull            = false;
    bool noAlias            = false;
    bool readOnly           = false;
    bool noCapture          = false;
};

// Monomorphized instances produced by the comptime pass. The HIR bodies already
// carry the concrete names and call targets, so this record is a stable summary
// for cache inspection and future re-instantiation without rerunning sema.
struct InstantiationRecord {
    std::string module;
    std::string mangled;
    std::string template_name;
    uint32_t decl_id = 0;
    std::vector<std::string> arg_types;
};

// The full decoded artifact.  Built by the reader and consumed by the hydrator.
struct Artifact {
    std::string canonical_path;
    std::string module_name;
    uint32_t cache_key_hash = 0;
    uint32_t module_id_hi   = 0;
    uint32_t module_id_lo   = 0;
    uint32_t source_fp_hi   = 0;
    uint32_t source_fp_lo   = 0;
    uint32_t public_abi_hi  = 0;
    uint32_t public_abi_lo  = 0;
    std::vector<std::string> strings;         // sec2 string table
    std::vector<std::string> paths;           // sec2 path table
    std::vector<CompactType> types;           // sec2 type table
    std::vector<DependencyRecord> deps;       // sec1 dependency list
    std::vector<DeclRecord> decls;            // sec3
    std::vector<TemplateBlueprint> templates; // sec4
    std::vector<CompactFunction> functions;   // sec5
    std::vector<CompactMarker> markers;       // sec5 marker metadata
    // Definition tables used to restore composite types regardless of whether
    // they appear among exported DeclRecord entries.
    std::vector<CompactStructDef> struct_defs;
    std::vector<CompactEnumDef> enum_defs;
    std::vector<CompactUnionDef> union_defs;
    // sec6 ownership/call/fn residual facts (HirAttrs)
    std::vector<HirSlotAttrsRecord> attrs_slots;
    std::vector<HirCallAttrsRecord> attrs_calls;
    std::vector<HirFnAttrsRecord> attrs_fns;
    std::vector<InstantiationRecord> instantiations;
};

} // namespace zith::cache
