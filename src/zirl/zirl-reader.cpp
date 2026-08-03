#include "zirl-reader.hpp"

namespace zith::zirl {

bool Reader::readCompactType(ByteReader &r, cache::CompactType &out) {
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
    out.args.resize(n);
    for (auto &a : out.args)
        if (!r.readU32(a))
            return false;
    if (!r.readU32(n))
        return false;
    out.arg_names.resize(n);
    for (auto &a : out.arg_names)
        if (!r.readU32(a))
            return false;
    return true;
}

bool Reader::readCompactExpr(ByteReader &r, cache::CompactExpr &out) {
    uint8_t kind = 0, op = 0, flags = 0, reserved = 0;
    if (!r.readU8(kind) || !r.readU8(op) || !r.readU8(flags) || !r.readU8(reserved))
        return false;
    out.kind  = static_cast<cache::CompactExprKind>(kind);
    out.op    = op;
    out.flags = flags;
    if (!r.readU32(out.type_id) || !r.readU32(out.ref_a) || !r.readU32(out.ref_b) ||
        !r.readU32(out.ref_c) || !r.readU32(out.ref_d) || !r.readU32(out.name_id))
        return false;
    if (!r.readI64(out.int_val) || !r.readF64(out.flt_val))
        return false;
    uint32_t n = 0;
    if (!r.readU32(n))
        return false;
    out.args.resize(n);
    for (auto &a : out.args)
        if (!r.readU32(a))
            return false;
    return true;
}

bool Reader::readMetadata(ByteReader &r, cache::Artifact &out) {
    uint32_t n = 0;
    if (!r.readU32(n))
        return false;
    out.strings.resize(n);
    for (auto &s : out.strings)
        if (!r.readBlob(s))
            return false;
    if (!r.readU32(n))
        return false;
    out.paths.resize(n);
    for (auto &s : out.paths)
        if (!r.readBlob(s))
            return false;
    if (!r.readU32(n))
        return false;
    out.types.resize(n);
    for (auto &t : out.types)
        if (!readCompactType(r, t))
            return false;
    return true;
}

bool Reader::readDecls(ByteReader &r, cache::Artifact &out) {
    uint32_t n = 0;
    if (!r.readU32(n))
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
        d.method_decl_indices.resize(c);
        for (auto &id : d.method_decl_indices)
            if (!r.readU32(id))
                return false;
    }
    return true;
}

bool Reader::readTemplates(ByteReader &r, cache::Artifact &out) {
    uint32_t n = 0;
    if (!r.readU32(n))
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
        t.params.resize(c);
        for (auto &p : t.params) {
            if (!r.readU32(p.name_id))
                return false;
            uint32_t bc = 0;
            if (!r.readU32(bc))
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

bool Reader::readCode(ByteReader &r, cache::Artifact &out) {
    uint32_t n = 0;
    if (!r.readU32(n))
        return false;
    out.functions.resize(n);
    for (auto &fn : out.functions) {
        uint8_t ext = 0, a = 0, b = 0, c = 0;
        if (!r.readU32(fn.name_id) || !r.readU8(ext) || !r.readU8(a) || !r.readU8(b) ||
            !r.readU8(c))
            return false;
        fn.is_extern = ext != 0;
        if (!r.readU32(fn.return_type_id))
            return false;
        uint32_t k = 0;
        if (!r.readU32(k))
            return false;
        fn.param_type_ids.resize(k);
        for (auto &id : fn.param_type_ids)
            if (!r.readU32(id))
                return false;
        if (!r.readU32(k))
            return false;
        fn.param_name_ids.resize(k);
        for (auto &id : fn.param_name_ids)
            if (!r.readU32(id))
                return false;
        if (!r.readU32(k))
            return false;
        fn.blocks.resize(k);
        for (auto &blk : fn.blocks) {
            uint32_t m = 0;
            if (!r.readU32(m))
                return false;
            blk.insts.resize(m);
            for (auto &id : blk.insts)
                if (!r.readU32(id))
                    return false;
            if (!r.readU32(blk.terminator))
                return false;
        }
        if (!r.readU32(k))
            return false;
        fn.exprs.resize(k);
        for (auto &e : fn.exprs)
            if (!readCompactExpr(r, e))
                return false;
    }
    return true;
}

std::optional<cache::Artifact> Reader::read(std::string_view bytes) {
    ByteReader r(bytes);
    // FileHeader fields.
    uint32_t magic = 0, version = 0;
    uint8_t endianness = 0, ra = 0, rb = 0, section_count = 0;
    if (!r.readU32(magic) || !r.readU32(version) || !r.readU8(endianness) || !r.readU8(ra) ||
        !r.readU8(rb) || !r.readU8(section_count))
        return std::nullopt;
    if (magic != kMagic || version != kFormatVersion || endianness != kEndianLittle)
        return std::nullopt;

    uint32_t header_size = 0, checksum = 0, cache_key_hash = 0;
    uint32_t mod_hi = 0, mod_lo = 0, src_hi = 0, src_lo = 0, abi_hi = 0, abi_lo = 0;
    uint32_t dep_count = 0, decl_count = 0, template_count = 0, fn_count = 0, path_len = 0;
    if (!r.readU32(header_size) || !r.readU32(checksum) || !r.readU32(cache_key_hash) ||
        !r.readU32(mod_hi) || !r.readU32(mod_lo) || !r.readU32(src_hi) || !r.readU32(src_lo) ||
        !r.readU32(abi_hi) || !r.readU32(abi_lo) || !r.readU32(dep_count) ||
        !r.readU32(decl_count) || !r.readU32(template_count) || !r.readU32(fn_count) ||
        !r.readU32(path_len))
        return std::nullopt;

    if (header_size > bytes.size() || path_len > bytes.size())
        return std::nullopt;

    // Canonical path is written raw (no length prefix); the length is in the
    // header field canonical_path_len.
    std::string canonical_path;
    {
        if (r.position() + path_len > bytes.size())
            return std::nullopt;
        canonical_path.assign(reinterpret_cast<const char *>(bytes.data() + r.position()),
                              path_len);
        // Advance the reader past the path bytes.
        for (uint32_t i = 0; i < path_len; ++i) {
            uint8_t dummy = 0;
            if (!r.readU8(dummy))
                return std::nullopt;
        }
    }

    cache::Artifact out;
    out.canonical_path = std::move(canonical_path);
    out.cache_key_hash = cache_key_hash;
    out.module_id_hi   = mod_hi;
    out.module_id_lo   = mod_lo;
    out.source_fp_hi   = src_hi;
    out.source_fp_lo   = src_lo;
    out.public_abi_hi  = abi_hi;
    out.public_abi_lo  = abi_lo;

    out.deps.resize(dep_count);
    for (auto &dep : out.deps) {
        if (!r.readBlob(dep.canonical_path) || !r.readBlob(dep.import_key) ||
            !r.readU32(dep.public_abi_hi) || !r.readU32(dep.public_abi_lo))
            return std::nullopt;
    }

    // The reader is now positioned at the section table.  Recompute the header
    // size the way the writer does (FileHeader fields + canonical path + dep
    // records + section table) and require the reader position to land exactly at
    // the section table start.  Any writer/reader divergence becomes a clean
    // cache miss instead of a misparse.
    uint64_t deps_size = 0;
    for (const auto &dep : out.deps)
        deps_size += 2 * sizeof(uint32_t) + dep.canonical_path.size() + dep.import_key.size() +
                     2 * sizeof(uint32_t);
    const uint64_t recomputed_header_end =
        static_cast<uint64_t>(sizeof(FileHeader)) + path_len + deps_size +
        static_cast<uint64_t>(section_count) * sizeof(SectionEntry);
    const uint64_t table_start =
        recomputed_header_end - static_cast<uint64_t>(section_count) * sizeof(SectionEntry);
    if (recomputed_header_end != header_size || r.position() != table_start)
        return std::nullopt;

    // Section table.
    if (section_count < 4)
        return std::nullopt;
    SectionEntry entries[4];
    for (int i = 0; i < 4; ++i) {
        uint64_t off = 0, sz = 0;
        if (!r.readU64(off) || !r.readU64(sz))
            return std::nullopt;
        entries[i] = {off, sz};
    }

    // Verify checksum over payloads.
    const uint8_t *base = reinterpret_cast<const uint8_t *>(bytes.data());
    const size_t total  = bytes.size();
    for (int i = 0; i < 4; ++i) {
        if (entries[i].offset + entries[i].size > total)
            return std::nullopt;
    }
    uint32_t calc = 0;
    calc          = fnv1a32(base + entries[0].offset, static_cast<size_t>(entries[0].size));
    calc ^= fnv1a32(base + entries[1].offset, static_cast<size_t>(entries[1].size));
    calc ^= fnv1a32(base + entries[2].offset, static_cast<size_t>(entries[2].size));
    calc ^= fnv1a32(base + entries[3].offset, static_cast<size_t>(entries[3].size));
    for (const auto &dep : out.deps) {
        calc ^= fnv1a32(dep.canonical_path);
        calc ^= fnv1a32(dep.import_key);
        uint32_t buf[2] = {dep.public_abi_hi, dep.public_abi_lo};
        calc ^= fnv1a32(reinterpret_cast<const uint8_t *>(buf), sizeof(buf));
    }
    if (calc != checksum)
        return std::nullopt;

    // Read sections by offset.
    auto sectionView = [&](int i) {
        return std::string_view(reinterpret_cast<const char *>(base + entries[i].offset),
                                static_cast<size_t>(entries[i].size));
    };
    ByteReader mr(sectionView(0));
    if (!readMetadata(mr, out))
        return std::nullopt;
    ByteReader dr(sectionView(1));
    if (!readDecls(dr, out))
        return std::nullopt;
    ByteReader tr(sectionView(2));
    if (!readTemplates(tr, out))
        return std::nullopt;
    ByteReader cr(sectionView(3));
    if (!readCode(cr, out))
        return std::nullopt;

    return out;
}

} // namespace zith::zirl
