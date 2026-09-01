#include "zirl-decl-section.hpp"

namespace zith::zirl {

bool encodeDecls(const cache::Artifact &artifact, ByteWriter &w) {
    w.writeU32(static_cast<uint32_t>(artifact.decls.size()));
    for (const auto &d : artifact.decls) {
        w.writeU8(static_cast<uint8_t>(d.kind));
        w.writeU8(static_cast<uint8_t>(d.visibility));
        w.writeU8(d.is_extern ? 1 : 0);
        w.writeU8(0); // reserved
        w.writeI32(d.mod_depth);
        w.writeU32(d.name_id);
        w.writeU32(d.type_id);
        w.writeU32(d.template_index);
        w.writeU32(d.body_fn_index);
        w.writeU32(static_cast<uint32_t>(d.field_name_ids.size()));
        for (auto id : d.field_name_ids)
            w.writeU32(id);
        for (auto id : d.field_type_ids)
            w.writeU32(id);
        w.writeU32(static_cast<uint32_t>(d.method_decl_indices.size()));
        for (auto id : d.method_decl_indices)
            w.writeU32(id);
    }
    return true;
}

bool decodeDecls(ByteReader &r, cache::Artifact &out) {
    uint32_t n = 0;
    if (!r.readU32(n))
        return false;
    if (!r.canReadU32Count(n))
        return false;
    out.decls.resize(n);
    for (auto &d : out.decls) {
        uint8_t kind = 0, vis = 0, ext = 0, reserved = 0;
        if (!r.readU8(kind) || !r.readU8(vis) || !r.readU8(ext) || !r.readU8(reserved))
            return false;
        d.kind        = static_cast<cache::CompactSymKind>(kind);
        d.visibility  = static_cast<symbols::SymbolVisibility>(vis);
        d.is_extern   = ext != 0;
        int32_t depth = 0;
        if (!r.readI32(depth))
            return false;
        d.mod_depth = depth;
        if (!r.readU32(d.name_id) || !r.readU32(d.type_id) || !r.readU32(d.template_index) ||
            !r.readU32(d.body_fn_index))
            return false;
        uint32_t c = 0;
        if (!r.readU32(c))
            return false;
        if (!r.canReadU32Count(c))
            return false;
        d.field_name_ids.resize(c);
        for (auto &id : d.field_name_ids)
            if (!r.readU32(id))
                return false;
        d.field_type_ids.resize(c);
        for (auto &id : d.field_type_ids)
            if (!r.readU32(id))
                return false;
        if (!r.readU32(c))
            return false;
        if (!r.canReadU32Count(c))
            return false;
        d.method_decl_indices.resize(c);
        for (auto &id : d.method_decl_indices)
            if (!r.readU32(id))
                return false;
    }
    return true;
}

bool encodeTemplates(const cache::Artifact &artifact, ByteWriter &w) {
    w.writeU32(static_cast<uint32_t>(artifact.templates.size()));
    for (const auto &t : artifact.templates) {
        w.writeU8(static_cast<uint8_t>(t.kind));
        w.writeU8(t.is_extern ? 1 : 0);
        w.writeU8(0); // reserved
        w.writeU8(0);
        w.writeU32(t.name_id);
        w.writeU32(t.return_type_id);
        w.writeU32(static_cast<uint32_t>(t.params.size()));
        for (const auto &p : t.params) {
            w.writeU32(p.name_id);
            w.writeU32(static_cast<uint32_t>(p.bound_type_ids.size()));
            for (auto b : p.bound_type_ids)
                w.writeU32(b);
        }
        w.writeU32(static_cast<uint32_t>(t.param_type_ids.size()));
        for (auto id : t.param_type_ids)
            w.writeU32(id);
        w.writeU32(static_cast<uint32_t>(t.param_name_ids.size()));
        for (auto id : t.param_name_ids)
            w.writeU32(id);
        w.writeU32(static_cast<uint32_t>(t.field_name_ids.size()));
        for (auto id : t.field_name_ids)
            w.writeU32(id);
        w.writeU32(static_cast<uint32_t>(t.field_type_ids.size()));
        for (auto id : t.field_type_ids)
            w.writeU32(id);
        w.writeU32(static_cast<uint32_t>(t.canonical_field_order.size()));
        for (auto id : t.canonical_field_order)
            w.writeU32(id);
        w.writeU32(static_cast<uint32_t>(t.method_decl_indices.size()));
        for (auto id : t.method_decl_indices)
            w.writeU32(id);
        w.writeU32(static_cast<uint32_t>(t.trait_type_ids.size()));
        for (auto id : t.trait_type_ids)
            w.writeU32(id);
    }
    return true;
}

bool decodeTemplates(ByteReader &r, cache::Artifact &out) {
    uint32_t n = 0;
    if (!r.readU32(n))
        return false;
    if (!r.canReadU32Count(n))
        return false;
    out.templates.resize(n);
    for (auto &t : out.templates) {
        uint8_t kind = 0, ext = 0, a = 0, b = 0;
        if (!r.readU8(kind) || !r.readU8(ext) || !r.readU8(a) || !r.readU8(b))
            return false;
        t.kind      = static_cast<cache::CompactSymKind>(kind);
        t.is_extern = ext != 0;
        if (!r.readU32(t.name_id) || !r.readU32(t.return_type_id))
            return false;
        uint32_t c = 0;
        if (!r.readU32(c))
            return false;
        if (!r.canReadU32Count(c))
            return false;
        t.params.resize(c);
        for (auto &p : t.params) {
            if (!r.readU32(p.name_id))
                return false;
            uint32_t bc = 0;
            if (!r.readU32(bc))
                return false;
            if (!r.canReadU32Count(bc))
                return false;
            p.bound_type_ids.resize(bc);
            for (auto &bid : p.bound_type_ids)
                if (!r.readU32(bid))
                    return false;
        }
        auto readIds = [&](std::vector<uint32_t> &dst) {
            uint32_t k = 0;
            if (!r.readU32(k))
                return false;
            if (!r.canReadU32Count(k))
                return false;
            dst.resize(k);
            for (auto &id : dst)
                if (!r.readU32(id))
                    return false;
            return true;
        };
        if (!readIds(t.param_type_ids) || !readIds(t.param_name_ids) ||
            !readIds(t.field_name_ids) || !readIds(t.field_type_ids) ||
            !readIds(t.canonical_field_order) || !readIds(t.method_decl_indices) ||
            !readIds(t.trait_type_ids))
            return false;
    }
    return true;
}

} // namespace zith::zirl
