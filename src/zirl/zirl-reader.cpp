#include "zirl/zirl-reader.hpp"
#include "zirl/zirl-attrs-section.hpp"
#include "zirl/zirl-buffer.hpp"
#include "zirl/zirl-code-section.hpp"
#include "zirl/zirl-decl-section.hpp"
#include "zirl/zirl-header.hpp"
#include "zirl/zirl-instantiation-section.hpp"
#include "zirl/zirl-type-section.hpp"

#include <algorithm>

namespace zith::zirl {

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

    if (!r.canReadU32Count(dep_count))
        return std::nullopt;
    out.deps.resize(dep_count);
    for (auto &dep : out.deps) {
        if (!r.readBlob(dep.canonical_path) || !r.readBlob(dep.import_key) ||
            !r.readU32(dep.public_abi_hi) || !r.readU32(dep.public_abi_lo))
            return std::nullopt;
    }

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

    if (section_count != 4 && section_count != 5 && section_count != 6)
        return std::nullopt;
    SectionEntry entries[6];
    const int entry_count = std::min<int>(6, section_count);
    int i                 = 0;
    for (; i < entry_count; ++i) {
        uint64_t off = 0, sz = 0;
        if (!r.readU64(off) || !r.readU64(sz))
            return std::nullopt;
        entries[i] = {off, sz};
    }

    const uint8_t *base = reinterpret_cast<const uint8_t *>(bytes.data());
    const size_t total  = bytes.size();
    for (i = 0; i < entry_count; ++i) {
        if (entries[i].offset > total || entries[i].size > total - entries[i].offset)
            return std::nullopt;
    }
    uint32_t calc = 0;
    calc          = fnv1a32(base + entries[0].offset, static_cast<size_t>(entries[0].size));
    calc ^= fnv1a32(base + entries[1].offset, static_cast<size_t>(entries[1].size));
    calc ^= fnv1a32(base + entries[2].offset, static_cast<size_t>(entries[2].size));
    calc ^= fnv1a32(base + entries[3].offset, static_cast<size_t>(entries[3].size));
    if (entry_count > 4)
        calc ^= fnv1a32(base + entries[4].offset, static_cast<size_t>(entries[4].size));
    if (entry_count > 5)
        calc ^= fnv1a32(base + entries[5].offset, static_cast<size_t>(entries[5].size));
    for (const auto &dep : out.deps) {
        calc ^= fnv1a32(dep.canonical_path);
        calc ^= fnv1a32(dep.import_key);
        uint32_t buf[2] = {dep.public_abi_hi, dep.public_abi_lo};
        calc ^= fnv1a32(reinterpret_cast<const uint8_t *>(buf), sizeof(buf));
    }
    if (calc != checksum)
        return std::nullopt;

    auto sectionView = [&](int section_index) {
        return std::string_view(
            reinterpret_cast<const char *>(base + entries[section_index].offset),
            static_cast<size_t>(entries[section_index].size));
    };
    ByteReader mr(sectionView(0));
    if (!decodeTypes(mr, out))
        return std::nullopt;
    ByteReader dr(sectionView(1));
    if (!decodeDecls(dr, out))
        return std::nullopt;
    ByteReader tr(sectionView(2));
    if (!decodeTemplates(tr, out))
        return std::nullopt;
    ByteReader cr(sectionView(3));
    if (!decodeCode(cr, out))
        return std::nullopt;
    if (entry_count > 4) {
        ByteReader ar(sectionView(4));
        if (!decodeAttrs(ar, out))
            return std::nullopt;
    }
    if (entry_count > 5) {
        ByteReader ir(sectionView(5));
        if (!decodeInstantiations(ir, out))
            return std::nullopt;
    }

    return out;
}

} // namespace zith::zirl
