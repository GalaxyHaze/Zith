#pragma once

#include "common/ast-ids.hpp"
#include "hir/hir-expr.hpp"
#include "hir/hir-types.hpp"
#include "memory/arena.hpp"
#include "memory/dyn-array.hpp"
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
    HirTypeId return_type;
    ast::DeclId decl_id   = ast::kInvalidDecl;
    symbols::SymId sym_id = symbols::kInvalidSym;
    memory::DynArray<HirBasicBlock> blocks;

    explicit HirFunction(memory::Arena &arena) : params(arena), param_names(arena), blocks(arena) {}
};

class HirModule {
    memory::DynArray<HirExpr> exprs_;
    memory::DynArray<HirFunction> fns_;

public:
    explicit HirModule(memory::Arena &arena);
    HirModule(HirModule &&)            = default;
    HirModule &operator=(HirModule &&) = default;

    HirExprId addExpr(HirExpr expr);
    HirFunction &addFn(memory::InternedId name);
    HirFunction &getFn(size_t idx);

    const HirExpr &getExpr(HirExprId id) const;
    HirExpr &getExprMut(HirExprId id);
    const HirFunction &getFn(size_t idx) const;
    size_t getFnCount() const;

    void dump(FILE *out, const memory::StringInterner &interner) const;
};

} // namespace zith::hir
