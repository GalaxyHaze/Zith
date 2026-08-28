#pragma once

#include "common/ast-ids.hpp"
#include "hir/hir-attrs.hpp"
#include "hir/hir-expr.hpp"
#include "hir/hir-types.hpp"
#include "memory/arena.hpp"
#include "memory/dyn-array.hpp"
#include "memory/span.hpp"
#include "memory/string-interner.hpp"

namespace zith::hir {

struct HirBasicBlock {
    memory::DynArray<HirExprId> insts;
    HirExprId terminator = kInvalidHirExpr;

    explicit HirBasicBlock(memory::Arena &arena) : insts(arena) {}
};

struct HirFunction {
    memory::InternedId name;
    memory::DynArray<HirTypeId> params;
    memory::DynArray<memory::InternedId> param_names;
    /// HIR slot allocated for each function parameter/body local. Codegen uses
    /// this to read residual ownership facts without assuming slot order.
    memory::DynArray<HirSlotId> param_slots;
    HirTypeId return_type;
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
    ast::DeclId decl_id       = ast::kInvalidDecl;
    symbols::SymId sym_id     = symbols::kInvalidSym;
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

    explicit HirGlobalConst(memory::Arena &arena) {
        (void)arena;
    }
};

/// A vtable for one `(trait/interface, concrete type)` pairing. Slot order is
/// exactly the trait/interface method requirement order used by `HirDynCall`.
/// `slots` holds the HIR function symbol for the implementation/default method
/// selected at lowering time.
struct HirVTable {
    memory::InternedId name{};
    memory::DynArray<symbols::SymId> slots;

    explicit HirVTable(memory::Arena &arena) : slots(arena) {}
};

class HirModule {
    memory::DynArray<HirExpr> exprs_;
    memory::DynArray<HirFunction> fns_;
    memory::DynArray<HirGlobalConst> globals_;
    memory::DynArray<HirVTable> vtables_;
    HirAttrs attrs_;

public:
    explicit HirModule(memory::Arena &arena);
    HirModule(HirModule &&)            = default;
    HirModule &operator=(HirModule &&) = default;

    HirExprId addExpr(HirExpr expr);
    HirFunction &addFn(memory::InternedId name);
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
