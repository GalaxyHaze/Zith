#include "zirl-writer.hpp"

namespace zith::zirl {

void Writer::writeCompactType(const cache::CompactType &t, ByteWriter &w) {
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

void Writer::writeCompactExpr(const cache::CompactExpr &e, ByteWriter &w) {
    w.writeU8(static_cast<uint8_t>(e.kind));
    w.writeU8(e.op);
    w.writeU8(e.flags);
    w.writeU8(0); // reserved
    w.writeU32(e.type_id);
    w.writeU32(e.ref_a);
    w.writeU32(e.ref_b);
    w.writeU32(e.ref_c);
    w.writeU32(e.ref_d);
    w.writeU32(e.name_id);
    w.writeI64(e.int_val);
    w.writeF64(e.flt_val);
    w.writeU32(static_cast<uint32_t>(e.args.size()));
    for (auto a : e.args)
        w.writeU32(a);
}

void Writer::writeMetadata(const cache::Artifact &artifact, ByteWriter &w) {
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
}

void Writer::writeDecls(const cache::Artifact &artifact, ByteWriter &w) {
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
}

void Writer::writeTemplates(const cache::Artifact &artifact, ByteWriter &w) {
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
}

void Writer::writeCode(const cache::Artifact &artifact, ByteWriter &w) {
    w.writeU32(static_cast<uint32_t>(artifact.functions.size()));
    for (const auto &fn : artifact.functions) {
        w.writeU32(fn.name_id);
        w.writeU8(fn.is_extern ? 1 : 0);
        w.writeU8(0); // reserved
        w.writeU8(0);
        w.writeU8(0);
        w.writeU32(fn.return_type_id);
        w.writeU32(static_cast<uint32_t>(fn.param_type_ids.size()));
        for (auto id : fn.param_type_ids)
            w.writeU32(id);
        w.writeU32(static_cast<uint32_t>(fn.param_name_ids.size()));
        for (auto id : fn.param_name_ids)
            w.writeU32(id);
        w.writeU32(static_cast<uint32_t>(fn.blocks.size()));
        for (const auto &blk : fn.blocks) {
            w.writeU32(static_cast<uint32_t>(blk.insts.size()));
            for (auto id : blk.insts)
                w.writeU32(id);
            w.writeU32(blk.terminator);
        }
        w.writeU32(static_cast<uint32_t>(fn.exprs.size()));
        for (const auto &e : fn.exprs)
            writeCompactExpr(e, w);
    }
}

uint32_t Writer::write(const cache::Artifact &artifact, ByteWriter &out) {
    // Encode sections into temporary buffers first so we can compute offsets,
    // the section table, and the checksum over payloads.
    ByteWriter meta;
    writeMetadata(artifact, meta);
    ByteWriter decls;
    writeDecls(artifact, decls);
    ByteWriter templates;
    writeTemplates(artifact, templates);
    ByteWriter code;
    writeCode(artifact, code);

    // The header is written field-wise below.  sizeof(FileHeader) must equal the
    // byte count of that field-by-field writeU32/writeU8 sequence (68 today:
    // eleven uint32 fields at 4 bytes each plus four uint8 fields, no padding).
    // If a field is ever added or the struct grows padding, this trips instead of
    // silently shifting every section offset.
    static_assert(sizeof(FileHeader) == 68,
                  "sizeof(FileHeader) must match the field-by-field header write");

    const uint32_t section_count = 4;
    const uint32_t path_len      = static_cast<uint32_t>(artifact.canonical_path.size());

    // Dependency records are written between the canonical path and the section
    // table (part of the header region).  Each record is two length-prefixed
    // blobs (a u32 length prefix plus the bytes) followed by two u32 ABI fields.
    // Compute the byte count in one pass and reuse it for header_size so writer
    // and reader cannot drift.
    uint64_t deps_size = 0;
    for (const auto &dep : artifact.deps)
        deps_size += 2 * sizeof(uint32_t) + dep.canonical_path.size() + dep.import_key.size() +
                     2 * sizeof(uint32_t);

    FileHeader hdr;
    hdr.magic          = kMagic;
    hdr.format_version = kFormatVersion;
    hdr.endianness     = kEndianLittle;
    hdr.section_count  = static_cast<uint8_t>(section_count);
    // header = FileHeader fields + canonical path + dep records + section table
    hdr.header_size        = static_cast<uint32_t>(sizeof(FileHeader) + path_len + deps_size +
                                                   section_count * sizeof(SectionEntry));
    hdr.cache_key_hash     = artifact.cache_key_hash;
    hdr.module_id_hi       = artifact.module_id_hi;
    hdr.module_id_lo       = artifact.module_id_lo;
    hdr.source_fp_hi       = artifact.source_fp_hi;
    hdr.source_fp_lo       = artifact.source_fp_lo;
    hdr.public_abi_hi      = artifact.public_abi_hi;
    hdr.public_abi_lo      = artifact.public_abi_lo;
    hdr.dep_count          = static_cast<uint32_t>(artifact.deps.size());
    hdr.decl_count         = static_cast<uint32_t>(artifact.decls.size());
    hdr.template_count     = static_cast<uint32_t>(artifact.templates.size());
    hdr.fn_count           = static_cast<uint32_t>(artifact.functions.size());
    hdr.canonical_path_len = path_len;

    // Checksum over all section payloads (meta, decls, templates, code) plus
    // the dependency records, which live in the header region.
    uint32_t checksum = 0;
    checksum          = fnv1a32(meta.ptr(), meta.size());
    checksum          = fnv1a32(decls.ptr(), decls.size()) ^ checksum;
    checksum          = fnv1a32(templates.ptr(), templates.size()) ^ checksum;
    checksum          = fnv1a32(code.ptr(), code.size()) ^ checksum;
    // Fold dependencies into the checksum so a changed dependency list is caught.
    for (const auto &dep : artifact.deps) {
        checksum ^= fnv1a32(dep.canonical_path);
        checksum ^= fnv1a32(dep.import_key);
        uint32_t buf[2] = {dep.public_abi_hi, dep.public_abi_lo};
        checksum ^= fnv1a32(reinterpret_cast<const uint8_t *>(buf), sizeof(buf));
    }
    hdr.checksum = checksum;

    // Section offsets start right after the section table.
    uint64_t base = hdr.header_size;
    SectionEntry entries[4];
    entries[0] = {base, meta.size()};
    entries[1] = {base + meta.size(), decls.size()};
    entries[2] = {base + meta.size() + decls.size(), templates.size()};
    entries[3] = {base + meta.size() + decls.size() + templates.size(), code.size()};

    // Write the file: header struct, canonical path, section table, payloads.
    out.writeU32(hdr.magic);
    out.writeU32(hdr.format_version);
    out.writeU8(hdr.endianness);
    out.writeU8(hdr.reserved_a);
    out.writeU8(hdr.reserved_b);
    out.writeU8(hdr.section_count);
    out.writeU32(hdr.header_size);
    out.writeU32(hdr.checksum);
    out.writeU32(hdr.cache_key_hash);
    out.writeU32(hdr.module_id_hi);
    out.writeU32(hdr.module_id_lo);
    out.writeU32(hdr.source_fp_hi);
    out.writeU32(hdr.source_fp_lo);
    out.writeU32(hdr.public_abi_hi);
    out.writeU32(hdr.public_abi_lo);
    out.writeU32(hdr.dep_count);
    out.writeU32(hdr.decl_count);
    out.writeU32(hdr.template_count);
    out.writeU32(hdr.fn_count);
    out.writeU32(hdr.canonical_path_len);
    out.writeRaw(artifact.canonical_path);

    // Dependency records live in the header region for fast identity checks.
    for (const auto &dep : artifact.deps) {
        out.writeBlob(dep.canonical_path);
        out.writeBlob(dep.import_key);
        out.writeU32(dep.public_abi_hi);
        out.writeU32(dep.public_abi_lo);
    }

    for (const auto &e : entries) {
        out.writeU64(e.offset);
        out.writeU64(e.size);
    }

    out.writeBytes(meta.ptr(), meta.size());
    out.writeBytes(decls.ptr(), decls.size());
    out.writeBytes(templates.ptr(), templates.size());
    out.writeBytes(code.ptr(), code.size());

    return checksum;
}

} // namespace zith::zirl
