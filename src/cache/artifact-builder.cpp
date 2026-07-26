#include "artifact-builder.hpp"

#include "common/overloaded.hpp"
#include "zirl/zirl-header.hpp"

#include <algorithm>
#include <sstream>

namespace zith::cache {

namespace {

CompactSymKind mapSymKind(symbols::SymKind k) {
    switch (k) {
    case symbols::SymKind::Fn:        return CompactSymKind::Fn;
    case symbols::SymKind::Struct:    return CompactSymKind::Struct;
    case symbols::SymKind::Trait:     return CompactSymKind::Trait;
    case symbols::SymKind::Interface: return CompactSymKind::Interface;
    case symbols::SymKind::Enum:      return CompactSymKind::Enum;
    case symbols::SymKind::Alias:     return CompactSymKind::Alias;
    case symbols::SymKind::Variable:  return CompactSymKind::Variable;
    case symbols::SymKind::Module:    return CompactSymKind::Module;
    case symbols::SymKind::Component: return CompactSymKind::Component;
    case symbols::SymKind::Union:     return CompactSymKind::Union;
    case symbols::SymKind::Asset:     return CompactSymKind::Asset;
    case symbols::SymKind::Word:      return CompactSymKind::Word;
    case symbols::SymKind::Context:   return CompactSymKind::Context;
    }
    return CompactSymKind::Variable;
}

} // namespace

ArtifactBuilder::ArtifactBuilder(const symbols::SymbolTable &syms,
                                 const types::TypeIntern &types, const hir::HirModule &hir,
                                 const memory::StringInterner &interner,
                                 const session::ContentFingerprint &source_fp,
                                 const session::CacheKey &cache_key)
    : syms_(syms), types_(types), hir_(hir), interner_(interner), source_fp_(source_fp),
      cache_key_hash_(zith::zirl::fnv1a32(cache_key.identity())) {}

uint32_t ArtifactBuilder::internString(std::string_view s) {
    if (s.empty())
        return 0;
    const auto it = string_index_.find(s);
    if (it != string_index_.end())
        return it->second;
    const uint32_t id = static_cast<uint32_t>(strings_.size());
    strings_.emplace_back(s);
    string_index_[s] = id;
    return id;
}

CompactType ArtifactBuilder::convertType(types::TypeId id) {
    CompactType out;
    if (id >= types_.count())
        return out;
    const auto &data = types_.lookup(id);
    out.kind = std::visit(
        common::overloaded{
            [&](const types::TypeError &) { return CompactTypeKind::Error; },
            [&](const types::TypeNever &) { return CompactTypeKind::Never; },
            [&](const types::TypeVoid &) { return CompactTypeKind::Void; },
            [&](const types::TypeBool &) { return CompactTypeKind::Bool; },
            [&](const types::TypeChar &) { return CompactTypeKind::Char; },
            [&](const types::TypeInt &t) { out.int_width = static_cast<uint8_t>(t.width); return CompactTypeKind::Int; },
            [&](const types::TypeFloat &t) { out.int_width = static_cast<uint8_t>(t.width); return CompactTypeKind::Float; },
            [&](const types::TypePtr &t) { out.ref0 = internType(t.pointee); out.flags = t.is_mut ? 1 : 0; return CompactTypeKind::Ptr; },
            [&](const types::TypeArray &t) { out.ref0 = internType(t.elem); out.ref1 = t.count; return CompactTypeKind::Array; },
            [&](const types::TypeStruct &t) { out.ref0 = t.def_id; return CompactTypeKind::Struct; },
            [&](const types::TypeFn &t) {
                out.ref0 = internType(t.ret);
                for (size_t i = 0; i < t.param_count; ++i)
                    out.args.push_back(internType(t.params[i]));
                return CompactTypeKind::Fn;
            },
            [&](const types::TypeTypeVar &t) { out.ref0 = t.id; return CompactTypeKind::TypeVar; },
            [&](const types::TypeOptional &t) { out.ref0 = internType(t.inner); return CompactTypeKind::Optional; },
            [&](const types::TypeFailable &t) { out.ref0 = internType(t.inner); return CompactTypeKind::Failable; },
            [&](const types::TypeOpaque &) { return CompactTypeKind::Opaque; },
            [&](const types::TypeUnknown &) { return CompactTypeKind::Error; },
            [&](const types::TypeSlice &t) { out.ref0 = internType(t.elem); return CompactTypeKind::Slice; },
            [&](const types::TypeEnum &t) { out.ref0 = t.def_id; return CompactTypeKind::Enum; },
            [&](const types::TypeUnion &t) { out.ref0 = t.def_id; return CompactTypeKind::Union; },
            [&](const types::TypePack &t) {
                for (size_t i = 0; i < t.count; ++i)
                    out.args.push_back(internType(t.members[i]));
                for (size_t i = 0; i < t.count; ++i)
                    out.arg_names.push_back(internString(interner_.lookup(t.names[i])));
                return CompactTypeKind::Struct; // packs encoded as struct
            },
            [&](const types::TypeSum &t) {
                for (size_t i = 0; i < t.count; ++i)
                    out.args.push_back(internType(t.members[i]));
                return CompactTypeKind::Union;
            },
            [&](const types::TypeGenericParam &t) { out.ref0 = t.decl_id; out.ref1 = t.param_index; return CompactTypeKind::GenericParam; },
            [&](const types::TypeIncomplete &t) {
                out.ref0 = internType(t.base);
                for (size_t i = 0; i < t.arg_count; ++i)
                    out.args.push_back(internType(t.args[i]));
                return CompactTypeKind::Incomplete;
            },
        },
        data);
    return out;
}

uint32_t ArtifactBuilder::internType(types::TypeId id) {
    if (id == types::kInvalidType)
        return 0;
    const auto it = type_index_.find(id);
    if (it != type_index_.end())
        return it->second;
    const uint32_t compact_id = static_cast<uint32_t>(compact_types_.size());
    compact_types_.push_back(convertType(id));
    type_index_[id] = compact_id;
    return compact_id;
}

uint64_t ArtifactBuilder::computePublicAbiHash() const {
    std::ostringstream oss;
    for (symbols::SymId id = 0; id < static_cast<symbols::SymId>(syms_.symbolCount()); ++id) {
        const auto &sym = syms_.get(id);
        if (sym.visibility != symbols::SymbolVisibility::Public &&
            sym.visibility != symbols::SymbolVisibility::Module)
            continue;
        oss << interner_.lookup(sym.name) << '\x1f'
            << static_cast<int>(sym.kind) << '\x1f'
            << static_cast<int>(sym.visibility) << '\x1f'
            << sym.mod_depth << '\n';
    }
    const auto str = oss.str();
    const uint32_t hi = zith::zirl::fnv1a32(str);
    // Combine with the module name hash for a 64-bit value.
    return (static_cast<uint64_t>(hi) << 32u) | zith::zirl::fnv1a32(str);
}

Artifact ArtifactBuilder::build(std::string_view canonical_path, std::string_view module_name,
                                const std::vector<DependencyRecord> &deps) {
    Artifact art;
    art.canonical_path = std::string(canonical_path);
    art.module_name    = std::string(module_name);
    art.cache_key_hash = cache_key_hash_;
    art.source_fp_hi   = static_cast<uint32_t>(source_fp_.primary >> 32u);
    art.source_fp_lo   = static_cast<uint32_t>(source_fp_.primary & 0xFFFFFFFFu);

    const uint64_t abi = computePublicAbiHash();
    art.public_abi_hi  = static_cast<uint32_t>(abi >> 32u);
    art.public_abi_lo  = static_cast<uint32_t>(abi & 0xFFFFFFFFu);

    const uint32_t name_hash = zith::zirl::fnv1a32(module_name);
    art.module_id_hi  = name_hash ^ art.public_abi_hi;
    art.module_id_lo  = zith::zirl::fnv1a32(canonical_path) ^ art.public_abi_lo;

    art.deps = deps;

    // Collect exported/module-visible declarations.
    for (symbols::SymId id = 0; id < static_cast<symbols::SymId>(syms_.symbolCount()); ++id) {
        const auto &sym = syms_.get(id);
        if (sym.visibility != symbols::SymbolVisibility::Public &&
            sym.visibility != symbols::SymbolVisibility::Module)
            continue;
        DeclRecord decl;
        decl.name        = std::string(interner_.lookup(sym.name));
        decl.name_id     = internString(interner_.lookup(sym.name));
        decl.kind        = mapSymKind(sym.kind);
        decl.visibility  = sym.visibility;
        decl.mod_depth   = sym.mod_depth;
        art.decls.push_back(std::move(decl));
    }

    // Extract concrete HIR functions into the code section.
    for (size_t fi = 0; fi < hir_.getFnCount(); ++fi) {
        const auto &fn = hir_.getFn(fi);
        CompactFunction cfn;
        cfn.name           = std::string(interner_.lookup(fn.name));
        cfn.name_id        = internString(interner_.lookup(fn.name));
        cfn.is_extern      = fn.blocks.empty();
        cfn.return_type_id = internType(fn.return_type);
        for (size_t pi = 0; pi < fn.params.size(); ++pi) {
            cfn.param_type_ids.push_back(internType(fn.params[pi]));
            cfn.param_name_ids.push_back(internString(interner_.lookup(fn.param_names[pi])));
        }
        for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
            const auto &blk = fn.blocks[bi];
            CompactBasicBlock cblk;
            for (auto eid : blk.insts)
                cblk.insts.push_back(eid);
            cblk.terminator = blk.terminator;
            cfn.blocks.push_back(std::move(cblk));
        }
        // Flatten HIR expressions into compact form.
        // Exprs are indexed by HirExprId; we copy them in order.
        for (size_t ei = 0; ei < fi; ++ei) {
            // The HirModule does not expose expr count directly; we skip expr
            // detail in v1 and rely on block instruction ids for structure.
        }
        art.functions.push_back(std::move(cfn));
    }

    // Commit string and type tables.
    art.strings = std::move(strings_);
    art.types   = std::move(compact_types_);
    return art;
}

} // namespace zith::cache
