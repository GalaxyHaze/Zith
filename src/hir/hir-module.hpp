#pragma once

#include "hir/hir-attrs.hpp"
#include "hir/hir-expr.hpp"
#include "hir/hir-types.hpp"
#include "memory/arena.hpp"
#include "memory/dyn-array.hpp"
#include "memory/span.hpp"
#include "memory/string-interner.hpp"

#include <cstdint>

namespace zith::hir {

using HirDeclarationId = uint32_t;
using HirTemplateId = uint32_t;

inline constexpr HirDeclarationId kInvalidHirDeclaration = ~HirDeclarationId{0};
inline constexpr HirTemplateId kInvalidHirTemplate = ~HirTemplateId{0};

enum class HirDeclarationKind : uint8_t {
    Function,
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

enum class HirVisibility : uint8_t { Private, Public, Module };

/// Frontend-neutral declaration metadata needed by cache and tooling consumers.
struct HirDeclaration {
    bool is_extern = false;
    memory::InternedId name = memory::kInvalidInternedId;
    HirDeclarationKind kind = HirDeclarationKind::Variable;
    HirVisibility visibility = HirVisibility::Private;
    int32_t module_depth = 0;
    HirTypeId type = types::kInvalidType;
    bool discardable = false;
    HirFunctionId body_function = kInvalidHirFunction;
    HirTemplateId template_id = kInvalidHirTemplate;
    memory::DynArray<HirDeclarationId> members;
    memory::DynArray<HirDeclarationId> method_declarations;

    explicit HirDeclaration(memory::Arena &arena)
        : members(arena), method_declarations(arena) {}
};

struct HirGenericParam {
    memory::InternedId name = memory::kInvalidInternedId;
    memory::DynArray<HirTypeId> bounds;

    explicit HirGenericParam(memory::Arena &arena) : bounds(arena) {}
};

struct HirTemplate {
    HirDeclarationId declaration_id = kInvalidHirDeclaration;
    HirFunctionId function_id = kInvalidHirFunction;
    HirDeclarationKind kind = HirDeclarationKind::Function;
    bool is_extern = false;
    memory::InternedId name = memory::kInvalidInternedId;
    HirTypeId return_type = types::kInvalidType;
    bool variadic = false;
    bool is_state = false;
    bool is_foreign_c = false;
    bool uses_tail_cc = false;
    memory::DynArray<HirGenericParam> params;
    memory::DynArray<HirTypeId> parameter_types;
    memory::DynArray<memory::InternedId> parameter_names;
    memory::DynArray<memory::InternedId> field_names;
    memory::DynArray<HirTypeId> field_types;
    memory::DynArray<uint32_t> canonical_field_order;
    memory::DynArray<HirDeclarationId> method_declarations;
    memory::DynArray<HirTypeId> trait_types;

    explicit HirTemplate(memory::Arena &arena)
        : params(arena), parameter_types(arena), parameter_names(arena),
          field_names(arena), field_types(arena), canonical_field_order(arena),
          method_declarations(arena), trait_types(arena) {}
};

struct HirBasicBlock {
    memory::DynArray<HirExprId> insts;
    HirExprId terminator = kInvalidHirExpr;

    explicit HirBasicBlock(memory::Arena &arena) : insts(arena) {}
};

struct HirFunction {
    memory::InternedId name = memory::kInvalidInternedId;
    memory::DynArray<HirTypeId> params;
    memory::DynArray<memory::InternedId> param_names;
    /// HIR slot allocated for each function parameter/body local. Codegen uses
    /// this to read residual ownership facts without assuming slot order.
    memory::DynArray<HirSlotId> param_slots;
    HirTypeId return_type = types::kInvalidType;
    /// True for `state` functions; their jumps are LLVM musttail transitions.
    bool isState = false;
    /// True for `state` functions and calls targeting them: LLVM `tailcc`.
    bool usesTailCC = false;
    /// Canonical return type shared by every state in this machine.
    HirTypeId machineReturnType = types::kInvalidType;
    uint32_t machineId          = 0;
    bool isVariadic             = false;
    /// Index of the final `[...]T` parameter when the function declares one.
    size_t variadicSliceParam = ~static_cast<size_t>(0);
    /// True for C-header functions declared by the cinterop layer.
    bool isForeignC       = false;
    /// Explicit body classification; declarations remain distinct after hydration.
    bool hasBody = false;
    /// C adapter called by the generated native-signature thunk, or invalid
    /// for foreign functions that do not pass records by value.
    memory::InternedId foreignAdapterName = memory::kInvalidInternedId;
    /// Source span of the `fn` declaration; empty for foreign/synthesized functions.
    memory::Span fnSpan{};
    memory::DynArray<HirBasicBlock> blocks;

    explicit HirFunction(memory::Arena &arena)
        : params(arena), param_names(arena), param_slots(arena), blocks(arena) {}
};

/// A module-scoped `const` global declaration. The initializer is a HIR
/// expression of constant form; codegen lowers it into an immutable LLVM global.
struct HirGlobalConst {
    memory::InternedId name{};
    HirTypeId type = types::kInvalidType;
    HirExprId init = kInvalidHirExpr;
    uint32_t alignmentBytes = 0;

    explicit HirGlobalConst(memory::Arena &arena) {
        (void)arena;
    }
};

/// A vtable for one `(trait/interface, concrete type)` pairing. Slot order is
/// exactly the trait/interface method requirement order used by `HirDynCall`.
/// `slots` holds the module-local HIR function id selected at lowering time.
struct HirVTable {
    memory::InternedId name{};
    memory::DynArray<HirFunctionId> slots;

    explicit HirVTable(memory::Arena &arena) : slots(arena) {}
};

/// A descriptive name of a named block (`setup: { ... }`) preserved for tooling
/// and diagnostics. Named blocks lower into ordinary control flow, so there is
/// no dedicated HIR expression node; this side table records the name, the
/// function it belongs to, and the source span. It is never serialized into
/// ZIRL and has no ABI effect, so a block can be renamed without changing any
/// compiled artifact.
struct HirNamedScope {
    HirFunctionId function = kInvalidHirFunction;
    memory::InternedId name = memory::kInvalidInternedId;
    memory::Span span{};

    explicit HirNamedScope(memory::Arena &arena) {
        (void)arena;
    }
};

class HirModule {
    memory::DynArray<HirExpr> exprs_;
    memory::DynArray<HirFunction> fns_;
    memory::DynArray<HirDeclaration> declarations_;
    memory::DynArray<HirTemplate> templates_;
    memory::DynArray<HirGlobalConst> globals_;
    memory::DynArray<HirVTable> vtables_;
    memory::DynArray<HirNamedScope> named_scopes_;
    HirAttrs attrs_;

public:
    explicit HirModule(memory::Arena &arena);
    HirModule(HirModule &&)            = default;
    HirModule &operator=(HirModule &&) = default;

    HirExprId addExpr(HirExpr expr);
    HirFunction &addFn(memory::InternedId name);
    HirDeclaration &addDeclaration();
    HirDeclaration &getDeclaration(size_t idx);
    const HirDeclaration &getDeclaration(size_t idx) const;
    HirTemplate &addTemplate();
    HirTemplate &getTemplate(size_t idx);
    const HirTemplate &getTemplate(size_t idx) const;
    [[nodiscard]] size_t templateCount() const noexcept {
        return templates_.size();
    }
    [[nodiscard]] size_t declarationCount() const noexcept {
        return declarations_.size();
    }
    HirFunction &getFn(size_t idx);
    HirGlobalConst &addGlobalConst();
    size_t getGlobalConstCount() const noexcept {
        return globals_.size();
    }
    const HirGlobalConst &getGlobalConst(size_t idx) const;
    HirVTable &addVTable(memory::InternedId name);
    size_t getVTableCount() const noexcept {
        return vtables_.size();
    }
    const HirVTable &getVTable(size_t idx) const;
    /// Records the descriptive name of a named block. Purely informational:
    /// `named_scopes_` is not serialized and carries no ABI meaning.
    void addNamedScope(HirFunctionId function, memory::InternedId name, memory::Span span);
    [[nodiscard]] size_t namedScopeCount() const noexcept {
        return named_scopes_.size();
    }
    const HirNamedScope &getNamedScope(size_t idx) const;
    const HirExpr &getExpr(HirExprId id) const;
    HirExpr &getExprMut(HirExprId id);
    [[nodiscard]] size_t exprCount() const noexcept {
        return exprs_.size();
    }
    const HirFunction &getFn(size_t idx) const;
    size_t getFnCount() const;
    HirAttrs &attrs() noexcept {
        return attrs_;
    }
    [[nodiscard]] const HirAttrs &attrs() const noexcept {
        return attrs_;
    }

    void dump(FILE *out, const memory::StringInterner &interner) const;
    std::string toString(const memory::StringInterner &interner) const;
};

} // namespace zith::hir
