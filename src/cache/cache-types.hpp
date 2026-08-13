#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace toolkit::cache {

struct CacheKey {
    std::string compilerVersion;

    [[nodiscard]] std::string identity() const noexcept {
        return compilerVersion;
    }
};

struct ContentFingerprint {
    std::uint64_t primary = 0;
};

struct DependencyRecord {
    std::string canonical_path;
    std::string import_key;
    std::uint32_t public_abi_hi = 0;
    std::uint32_t public_abi_lo = 0;
};

enum class DeclKind : std::uint8_t {
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

enum class Visibility : std::uint8_t {
    Private,
    Module,
    Public,
};

struct DeclRecord {
    std::string name;
    DeclKind kind = DeclKind::Variable;
    Visibility visibility = Visibility::Private;
    std::int32_t mod_depth = 0;
    std::uint32_t name_id = 0;
    std::uint32_t type_id = 0;
    std::uint32_t template_index = ~std::uint32_t{0};
    std::uint32_t body_fn_index = ~std::uint32_t{0};
    std::vector<std::uint32_t> field_name_ids;
    std::vector<std::uint32_t> field_type_ids;
    std::vector<std::uint32_t> method_decl_indices;
    bool is_extern = false;
};

struct GenericParamRecord {
    std::string name;
    std::uint32_t name_id = 0;
    std::vector<std::uint32_t> bound_type_ids;
};

struct TemplateBlueprint {
    std::string name;
    std::uint32_t name_id = 0;
    DeclKind kind = DeclKind::Fn;
    std::vector<GenericParamRecord> params;
    std::vector<GenericParamRecord> method_params;
    std::uint32_t return_type_id = 0;
    std::vector<std::uint32_t> param_type_ids;
    std::vector<std::uint32_t> param_name_ids;
    std::vector<std::uint32_t> field_name_ids;
    std::vector<std::uint32_t> field_type_ids;
    std::vector<std::uint32_t> canonical_field_order;
    std::vector<std::uint32_t> method_decl_indices;
    std::vector<std::uint32_t> trait_type_ids;
    bool is_extern = false;
};

enum class ExprKind : std::uint8_t {
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
    Cast,
    LayoutIntrinsic,
    MarkerStore,
    MarkerLoad,
    MarkerDock,
    MarkerJump,
    MarkerRet,
};

struct CompactExpr {
    ExprKind kind = ExprKind::Literal;
    std::uint32_t type_id = 0;
    std::uint32_t ref_a = 0;
    std::uint32_t ref_b = 0;
    std::uint32_t ref_c = 0;
    std::uint32_t ref_d = 0;
    std::uint32_t ref_e = 0;
    std::uint32_t ref_f = 0;
    std::uint8_t op = 0;
    std::uint8_t flags = 0;
    std::int64_t int_val = 0;
    double flt_val = 0.0;
    std::uint32_t name_id = 0;
    std::vector<std::uint32_t> args;
    std::vector<std::uint32_t> arg_types;
};

struct CompactBasicBlock {
    std::vector<std::uint32_t> insts;
    std::uint32_t terminator = ~std::uint32_t{0};
};

struct CompactFunction {
    std::string name;
    std::uint32_t name_id = 0;
    std::vector<std::uint32_t> param_type_ids;
    std::vector<std::uint32_t> param_name_ids;
    std::uint32_t return_type_id = 0;
    std::vector<CompactBasicBlock> blocks;
    std::vector<CompactExpr> exprs;
    bool is_extern = false;
    bool is_variadic = false;
};

struct CompactMarkerParam {
    std::uint32_t name_id = 0;
    std::uint32_t type_id = 0;
    std::uint32_t offset = 0;
};

struct CompactMarker {
    std::uint32_t name_id = 0;
    std::uint32_t marker_id = ~std::uint32_t{0};
    bool stackful = false;
    std::uint32_t blob_offset = 0;
    std::uint32_t body_expr = 0;
    std::uint32_t module_name_id = 0;
    std::vector<CompactMarkerParam> params;
};

struct HirSlotAttrsRecord {
    std::uint32_t slot = 0;
    std::uint8_t ownership = 0;
    std::uint8_t consumed = 0;
    bool non_null = false;
};

struct HirCallAttrsRecord {
    std::uint32_t expr_id = 0;
    std::vector<std::uint32_t> arg_escapes;
    std::uint32_t returns_arg = ~std::uint32_t{0};
};

struct HirFnAttrsRecord {
    std::uint32_t fn_index = 0;
    std::uint8_t return_consumed = 0;
    bool non_null = false;
    bool no_alias = false;
    bool read_only = false;
    bool no_capture = false;
};

struct Artifact {
    std::string canonical_path;
    std::string module_name;
    std::uint32_t cache_key_hash = 0;
    std::uint32_t module_id_hi = 0;
    std::uint32_t module_id_lo = 0;
    std::uint32_t source_fp_hi = 0;
    std::uint32_t source_fp_lo = 0;
    std::uint32_t public_abi_hi = 0;
    std::uint32_t public_abi_lo = 0;
    std::vector<std::string> strings;
    std::vector<std::string> paths;
    std::vector<DependencyRecord> deps;
    std::vector<DeclRecord> decls;
    std::vector<TemplateBlueprint> templates;
    std::vector<CompactFunction> functions;
    std::vector<CompactMarker> markers;
    std::vector<HirSlotAttrsRecord> attrs_slots;
    std::vector<HirCallAttrsRecord> attrs_calls;
    std::vector<HirFnAttrsRecord> attrs_fns;
};

} // namespace toolkit::cache
