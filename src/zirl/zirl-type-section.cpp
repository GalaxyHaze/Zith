#include "zirl-type-section.hpp"

namespace zith::zirl {

namespace {

void writeCompactType(const cache::CompactType &t, ByteWriter &w) {
    w.writeU8(static_cast<uint8_t>(t.kind));
    w.writeU8(t.int_width);
    w.writeU8(t.flags);
    w.writeU8(0); // reserved
    w.writeU32(t.ref0);
    w.writeU32(t.ref1);
    w.writeU32(static_cast<uint32_t>(t.args.size()));
    for (auto a : t.args)
        w.writeU32(a);
    w.writeU32(static_cast<uint32_t>(t.arg_names.size()));
    for (auto a : t.arg_names)
        w.writeU32(a);
}

bool readCompactType(ByteReader &r, cache::CompactType &out) {
    uint8_t kind = 0, w = 0, flags = 0, reserved = 0;
    if (!r.readU8(kind) || !r.readU8(w) || !r.readU8(flags) || !r.readU8(reserved))
        return false;
    out.kind      = static_cast<cache::CompactTypeKind>(kind);
    out.int_width = w;
    out.flags     = flags;
    if (!r.readU32(out.ref0) || !r.readU32(out.ref1))
        return false;
    uint32_t n = 0;
    if (!r.readU32(n))
        return false;
    if (!r.canReadU32Count(n))
        return false;
    out.args.resize(n);
    for (auto &a : out.args)
        if (!r.readU32(a))
            return false;
    if (!r.readU32(n))
        return false;
    if (!r.canReadU32Count(n))
        return false;
    out.arg_names.resize(n);
    for (auto &a : out.arg_names)
        if (!r.readU32(a))
            return false;
    return true;
}

} // namespace

bool encodeTypes(const cache::Artifact &artifact, ByteWriter &w) {
    // Module name: the header only carries canonical_path, so the human-facing
    // module name must round-trip through the metadata section.
    w.writeBlob(artifact.module_name);
    // String table.
    w.writeU32(static_cast<uint32_t>(artifact.strings.size()));
    for (const auto &s : artifact.strings)
        w.writeBlob(s);
    // Path table.
    w.writeU32(static_cast<uint32_t>(artifact.paths.size()));
    for (const auto &s : artifact.paths)
        w.writeBlob(s);
    // Type table.
    w.writeU32(static_cast<uint32_t>(artifact.types.size()));
    for (const auto &t : artifact.types)
        writeCompactType(t, w);

    w.writeU32(static_cast<uint32_t>(artifact.struct_defs.size()));
    for (const auto &s : artifact.struct_defs) {
        w.writeBlob(s.name);
        w.writeU32(s.name_id);
        w.writeU32(static_cast<uint32_t>(s.field_name_ids.size()));
        for (auto id : s.field_name_ids)
            w.writeU32(id);
        w.writeU32(static_cast<uint32_t>(s.field_type_ids.size()));
        for (auto id : s.field_type_ids)
            w.writeU32(id);
    }

    w.writeU32(static_cast<uint32_t>(artifact.enum_defs.size()));
    for (const auto &e : artifact.enum_defs) {
        w.writeBlob(e.name);
        w.writeU32(e.name_id);
        w.writeU32(e.underlying_id);
        w.writeU32(static_cast<uint32_t>(e.variants.size()));
        for (const auto &v : e.variants) {
            w.writeBlob(v.name);
            w.writeU32(v.name_id);
            w.writeI64(v.discriminant);
        }
    }

    w.writeU32(static_cast<uint32_t>(artifact.union_defs.size()));
    for (const auto &u : artifact.union_defs) {
        w.writeBlob(u.name);
        w.writeU32(u.name_id);
        w.writeU8(u.is_raw ? 1 : 0);
        w.writeU32(static_cast<uint32_t>(u.member_type_ids.size()));
        for (auto id : u.member_type_ids)
            w.writeU32(id);
    }
    return true;
}

bool decodeTypes(ByteReader &r, cache::Artifact &out) {
    if (!r.readBlob(out.module_name))
        return false;
    uint32_t n = 0;
    if (!r.readU32(n))
        return false;
    if (!r.canReadU32Count(n))
        return false;
    out.strings.resize(n);
    for (auto &s : out.strings)
        if (!r.readBlob(s))
            return false;
    if (!r.readU32(n))
        return false;
    if (!r.canReadU32Count(n))
        return false;
    out.paths.resize(n);
    for (auto &s : out.paths)
        if (!r.readBlob(s))
            return false;
    if (!r.readU32(n))
        return false;
    if (!r.canReadU32Count(n))
        return false;
    out.types.resize(n);
    for (auto &t : out.types)
        if (!readCompactType(r, t))
            return false;
    if (!r.readU32(n))
        return false;
    if (!r.canReadU32Count(n))
        return false;
    out.struct_defs.resize(n);
    for (auto &s : out.struct_defs) {
        if (!r.readBlob(s.name))
            return false;
        uint32_t k = 0;
        if (!r.readU32(s.name_id) || !r.readU32(k))
            return false;
        if (!r.canReadU32Count(k))
            return false;
        s.field_name_ids.resize(k);
        for (auto &id : s.field_name_ids)
            if (!r.readU32(id))
                return false;
        if (!r.readU32(k))
            return false;
        if (!r.canReadU32Count(k))
            return false;
        s.field_type_ids.resize(k);
        for (auto &id : s.field_type_ids)
            if (!r.readU32(id))
                return false;
    }
    if (!r.readU32(n))
        return false;
    if (!r.canReadU32Count(n))
        return false;
    out.enum_defs.resize(n);
    for (auto &e : out.enum_defs)
        if (!r.readBlob(e.name))
            return false;
    for (auto &e : out.enum_defs) {
        uint32_t k = 0;
        if (!r.readU32(e.name_id) || !r.readU32(e.underlying_id))
            return false;
        if (!r.readU32(k))
            return false;
        if (!r.canReadU32Count(k))
            return false;
        e.variants.resize(k);
        for (auto &v : e.variants) {
            if (!r.readBlob(v.name) || !r.readU32(v.name_id) || !r.readI64(v.discriminant))
                return false;
        }
    }
    if (!r.readU32(n))
        return false;
    if (!r.canReadU32Count(n))
        return false;
    out.union_defs.resize(n);
    for (auto &u : out.union_defs)
        if (!r.readBlob(u.name))
            return false;
    for (auto &u : out.union_defs) {
        uint8_t flags = 0;
        uint32_t k    = 0;
        if (!r.readU32(u.name_id) || !r.readU8(flags) || !r.readU32(k))
            return false;
        if (!r.canReadU32Count(k))
            return false;
        u.is_raw = flags != 0;
        u.member_type_ids.resize(k);
        for (auto &id : u.member_type_ids)
            if (!r.readU32(id))
                return false;
    }
    return true;
}

} // namespace zith::zirl
