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
    HirTypeId return_type;
    bool isVariadic       = false;
    ast::DeclId decl_id   = ast::kInvalidDecl;
    symbols::SymId sym_id = symbols::kInvalidSym;
    /// Source span of the `fn` declaration; empty for foreign/synthesized functions.
    memory::Span fnSpan{};
    memory::DynArray<HirBasicBlock> blocks;

    explicit HirFunction(memory::Arena &arena) : params(arena), param_names(arena), blocks(arena) {}
};

struct HirMarkerParam {
    memory::InternedId name{};
    HirTypeId type  = types::kInvalidType;
    uint32_t offset = 0;
    uint32_t size   = 0;
    uint32_t align  = 0;
};

struct HirMarker {
    memory::InternedId name{};
    uint32_t marker_id   = ~0U;
    bool stackful        = false;
    uint32_t blob_offset = 0;
    memory::DynArray<HirMarkerParam> params;
    uint32_t body_expr = 0;

    explicit HirMarker(memory::Arena &arena) : params(arena) {}
};

struct HirModuleMarkerLayout {
    memory::InternedId module_name = {};
    memory::DynArray<HirMarker> markers;
    uint32_t blob_size  = 0;
    uint32_t blob_align = 1;

    explicit HirModuleMarkerLayout(memory::Arena &arena) : markers(arena) {}
};

class HirModule {
    memory::DynArray<HirExpr> exprs_;
    memory::DynArray<HirFunction> fns_;
    HirModuleMarkerLayout marker_layout_;
    HirAttrs attrs_;

public:
    explicit HirModule(memory::Arena &arena);
    HirModule(HirModule &&)            = default;
    HirModule &operator=(HirModule &&) = default;

    HirExprId addExpr(HirExpr expr);
    HirFunction &addFn(memory::InternedId name);
    HirFunction &getFn(size_t idx);
    HirMarker &addMarker();
    void setModuleMarkerLayout(uint32_t size, uint32_t align) noexcept {
        marker_layout_.blob_size  = size;
        marker_layout_.blob_align = align;
    }
    size_t getMarkerCount() const noexcept {
        return marker_layout_.markers.size();
    }
    const HirMarker &getMarker(size_t idx) const;
    HirMarker &getMarkerMut(size_t idx);
    const HirMarker *findMarker(memory::InternedId name) const;
    uint32_t markerLayoutBlobSize() const noexcept {
        return marker_layout_.blob_size;
    }
    uint32_t markerLayoutBlobAlign() const noexcept {
        return marker_layout_.blob_align;
    }
    HirModuleMarkerLayout &markers() noexcept {
        return marker_layout_;
    }
    [[nodiscard]] const HirModuleMarkerLayout &markers() const noexcept {
        return marker_layout_;
    }

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
