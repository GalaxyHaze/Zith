#pragma once

#include "cache/cache-types.hpp"
#include "hir/hir-module.hpp"
#include "memory/string-interner.hpp"
#include "session/frontend-context.hpp"
#include "symbols/symbol-id.hpp"
#include "symbols/symbol-table.hpp"
#include "types/type-intern.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace zith::cache {

// Builds a cache::Artifact from compiler session state.  Extracts the public
// ABI surface (exported/module-visible declarations), generic blueprints, and
// concrete HIR bodies into the self-contained compact representation used by
// the zirl format.
class ArtifactBuilder {
public:
    ArtifactBuilder(const symbols::SymbolTable &syms, const types::TypeIntern &types,
                    const hir::HirModule &hir, const memory::StringInterner &interner,
                    const session::ContentFingerprint &source_fp,
                    const session::CacheKey &cache_key);

    [[nodiscard]] Artifact build(std::string_view canonical_path, std::string_view module_name,
                                 const std::vector<DependencyRecord> &deps);

private:
    const symbols::SymbolTable &syms_;
    const types::TypeIntern &types_;
    const hir::HirModule &hir_;
    const memory::StringInterner &interner_;
    const session::ContentFingerprint &source_fp_;
    uint32_t cache_key_hash_ = 0;

    // String interning into the artifact's own string table.
    std::vector<std::string> strings_;
    std::unordered_map<std::string_view, uint32_t> string_index_;
    [[nodiscard]] uint32_t internString(std::string_view s);

    // Type interning into the artifact's compact type table.
    std::vector<CompactType> compact_types_;
    std::unordered_map<types::TypeId, uint32_t> type_index_;
    [[nodiscard]] uint32_t internType(types::TypeId id);
    [[nodiscard]] CompactType convertType(types::TypeId id);
    [[nodiscard]] CompactExpr convertExpr(hir::HirExprId id);

    // Compute the public ABI hash from exported declarations.
    [[nodiscard]] uint64_t computePublicAbiHash() const;
};

} // namespace zith::cache
