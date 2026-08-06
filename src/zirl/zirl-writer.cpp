#include "zirl/zirl-writer.hpp"
#include "zirl/zirl-attrs-section.hpp"
#include "zirl/zirl-code-section.hpp"
#include "zirl/zirl-decl-section.hpp"
#include "zirl/zirl-header.hpp"
#include "zirl/zirl-type-section.hpp"

namespace zith::zirl {

uint32_t Writer::write(const cache::Artifact &artifact, ByteWriter &out) {
    ByteWriter meta, decls, templates, code, attrs;
    // Encoders only fail when a buffer cannot grow; never emit a partial artifact.
    if (!encodeTypes(artifact, meta) || !encodeDecls(artifact, decls) ||
        !encodeTemplates(artifact, templates) || !encodeCode(artifact, code) ||
        !encodeAttrs(artifact, attrs))
        return 0;

    static_assert(sizeof(FileHeader) == 68,
                  "sizeof(FileHeader) must match the field-by-field header write");

    const uint32_t section_count = 5;
    const uint32_t path_len      = static_cast<uint32_t>(artifact.canonical_path.size());

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

    uint32_t checksum = 0;
    checksum          = fnv1a32(meta.ptr(), meta.size());
    checksum          = fnv1a32(decls.ptr(), decls.size()) ^ checksum;
    checksum          = fnv1a32(templates.ptr(), templates.size()) ^ checksum;
    checksum          = fnv1a32(code.ptr(), code.size()) ^ checksum;
    checksum          = fnv1a32(attrs.ptr(), attrs.size()) ^ checksum;
    for (const auto &dep : artifact.deps) {
        checksum ^= fnv1a32(dep.canonical_path);
        checksum ^= fnv1a32(dep.import_key);
        uint32_t buf[2] = {dep.public_abi_hi, dep.public_abi_lo};
        checksum ^= fnv1a32(reinterpret_cast<const uint8_t *>(buf), sizeof(buf));
    }
    hdr.checksum = checksum;

    uint64_t base = hdr.header_size;
    SectionEntry entries[5];
    entries[0] = {base, meta.size()};
    entries[1] = {base + meta.size(), decls.size()};
    entries[2] = {base + meta.size() + decls.size(), templates.size()};
    entries[3] = {base + meta.size() + decls.size() + templates.size(), code.size()};
    entries[4] = {base + meta.size() + decls.size() + templates.size() + code.size(), attrs.size()};

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
    out.writeBytes(attrs.ptr(), attrs.size());

    return checksum;
}

} // namespace zith::zirl
