#include "cache/cache-codec.hpp"

#include "cache/cache-buffer.hpp"
#include "cache/cache-codec.gen.hpp"
#include "cache/cache-section.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <vector>

namespace toolkit::cache {

namespace {

using ResultArtifact = CacheResult<Artifact>;

CacheError corrupt(std::string message = "corrupt cache file") {
    return CacheError{CacheErrorKind::Corrupt, std::move(message)};
}

CacheError unsupported() {
    return CacheError{CacheErrorKind::UnsupportedVersion, "unsupported cache version"};
}

void alignWriter(ByteWriter &writer, std::size_t alignment) {
    const std::size_t rem = writer.size() % alignment;
    if (rem == 0)
        return;
    static constexpr std::array<std::uint8_t, 15> kZeros{};
    writer.writeBytes(std::string_view(
        reinterpret_cast<const char *>(kZeros.data()), alignment - rem));
}

struct SectionBuffers {
    std::vector<std::vector<std::uint8_t>> bytes;
};

SectionBuffers encodeSections(const Artifact &artifact) {
    constexpr std::size_t kSectionCount = toolkit::cache::kSectionCount;
    SectionBuffers out;
    out.bytes.resize(kSectionCount);

    for (std::size_t index = 0; index < kSectionCount; ++index) {
        ByteWriter writer;
        const auto id = static_cast<SectionId>(index);
        switch (id) {
        case SectionId::Metadata:
            writeMetadata(writer, artifact);
            break;
        case SectionId::Deps:
            writeDeps(writer, artifact);
            break;
        case SectionId::Strings:
            writeStrings(writer, artifact);
            break;
        case SectionId::Paths:
            writePaths(writer, artifact);
            break;
        case SectionId::Decls:
            writeDecls(writer, artifact);
            break;
        case SectionId::Templates:
            writeTemplates(writer, artifact);
            break;
        case SectionId::Functions:
            writeFunctions(writer, artifact);
            break;
        case SectionId::Markers:
            writeMarkers(writer, artifact);
            break;
        case SectionId::Attrs:
            writeAttrs(writer, artifact);
            break;
        case SectionId::Debug:
            writeDebug(writer, artifact);
            break;
        }
        out.bytes[index] = writer.takeBytes();
    }
    return out;
}

std::uint64_t checksumOf(std::string_view bytes) {
    std::string copy(bytes);
    for (std::size_t i = 24; i < 32 && i < copy.size(); ++i)
        copy[i] = 0;
    return fnv1a64(copy);
}

} // namespace

std::uint64_t fnv1a64(std::string_view bytes) noexcept {
    std::uint64_t hash = 0xCBF29CE484222325ULL;
    for (const unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

CacheResult<std::string> encodeArtifact(const Artifact &artifact) {
    constexpr std::size_t kSectionCount = toolkit::cache::kSectionCount;
    const auto sections = encodeSections(artifact);
    const std::string_view magic = CacheHeader::kMagic;

    std::vector<SectionEntry> entries(kSectionCount);
    std::uint64_t cursor = kCacheHeaderSize;
    for (std::size_t index = 0; index < kSectionCount; ++index) {
        entries[index].offset = cursor;
        entries[index].size = sections.bytes[index].size();
        cursor += sections.bytes[index].size();
        const std::size_t remainder = cursor % 8;
        if (remainder != 0)
            cursor += 8 - remainder;
    }

    ByteWriter bytes;
    bytes.writeBytes(magic);
    while (bytes.size() < 8)
        bytes.writeU8(0);
    bytes.writeU32(CacheHeader::kVersion);
    bytes.writeU8(CacheHeader::kEndian);
    bytes.writeU8(0);
    bytes.writeU8(0);
    bytes.writeU8(0);
    bytes.writeU32(static_cast<std::uint32_t>(kSectionCount));
    bytes.writeU32(static_cast<std::uint32_t>(kCacheHeaderSize));
    bytes.writeU64(0); // checksum placeholder
    bytes.writeU32(artifact.cache_key_hash);
    bytes.writeU32(artifact.module_id_hi);
    bytes.writeU32(artifact.module_id_lo);
    bytes.writeU32(artifact.source_fp_hi);
    bytes.writeU32(artifact.source_fp_lo);
    bytes.writeU32(artifact.public_abi_hi);
    bytes.writeU32(artifact.public_abi_lo);
    alignWriter(bytes, 8);
    for (const auto &entry : entries) {
        bytes.writeU64(entry.offset);
        bytes.writeU64(entry.size);
    }
    if (bytes.size() != kCacheHeaderSize)
        return CacheError{CacheErrorKind::WriteFailed, "header size mismatch"};

    for (std::size_t index = 0; index < kSectionCount; ++index) {
        const auto &section = sections.bytes[index];
        bytes.writeBytes(std::string_view(reinterpret_cast<const char *>(section.data()),
                                          section.size()));
        alignWriter(bytes, 8);
    }
    auto output = bytes.takeBytes();
    const std::uint64_t checksum = checksumOf(std::string_view(
        reinterpret_cast<const char *>(output.data()), output.size()));
    for (std::size_t i = 0; i < 8; ++i)
        output[24 + i] = static_cast<std::uint8_t>((checksum >> (i * 8)) & 0xFFu);
    return std::string(reinterpret_cast<const char *>(output.data()), output.size());
}

ResultArtifact decodeArtifact(std::string_view bytes) {
    if (bytes.size() < kCacheHeaderSize)
        return corrupt("cache file is truncated");

    ByteReader reader(bytes);
    std::string_view magic;
    std::uint32_t version = 0;
    std::uint8_t endian = 0;
    std::uint8_t reserved[3] = {};
    std::uint32_t section_count = 0;
    std::uint32_t header_size = 0;
    std::uint64_t stored_checksum = 0;
    if (!reader.readBytes(8, magic) || !reader.readU32(version) || !reader.readU8(endian) ||
        !reader.readU8(reserved[0]) || !reader.readU8(reserved[1]) ||
        !reader.readU8(reserved[2]) || !reader.readU32(section_count) ||
        !reader.readU32(header_size) || !reader.readU64(stored_checksum))
        return corrupt("cache header is truncated");

    if (magic != CacheHeader::kMagic)
        return corrupt("invalid cache magic");
    if (version != CacheHeader::kVersion)
        return unsupported();
    if (endian != CacheHeader::kEndian)
        return corrupt("unsupported endianness");
    if (section_count != kSectionCount)
        return corrupt("section count mismatch");
    if (header_size != kCacheHeaderSize)
        return corrupt("header size mismatch");

    Artifact artifact;
    if (!reader.readU32(artifact.cache_key_hash) || !reader.readU32(artifact.module_id_hi) ||
        !reader.readU32(artifact.module_id_lo) || !reader.readU32(artifact.source_fp_hi) ||
        !reader.readU32(artifact.source_fp_lo) || !reader.readU32(artifact.public_abi_hi) ||
        !reader.readU32(artifact.public_abi_lo))
        return corrupt("metadata is truncated");

    std::uint8_t padding[4] = {};
    if (!reader.readU8(padding[0]) || !reader.readU8(padding[1]) ||
        !reader.readU8(padding[2]) || !reader.readU8(padding[3]))
        return corrupt("header padding is truncated");

    std::vector<SectionEntry> entries;
    entries.reserve(kSectionCount);
    for (std::size_t i = 0; i < kSectionCount; ++i) {
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
        if (!reader.readU64(offset) || !reader.readU64(size))
            return corrupt("section table is truncated");
        if (offset < kCacheHeaderSize || size > bytes.size() - offset)
            return corrupt("section table is out of bounds");
        entries.push_back(SectionEntry{offset, size});
    }
    for (std::size_t i = 1; i < entries.size(); ++i) {
        if (entries[i - 1].offset > entries[i].offset)
            return corrupt("section offsets are not ordered");
        if (entries[i - 1].offset + entries[i - 1].size > entries[i].offset)
            return corrupt("sections overlap");
    }

    if (checksumOf(bytes) != stored_checksum)
        return corrupt("checksum mismatch");

    CacheFile file;
    file.header = CacheHeader{};
    std::memcpy(file.header.magic, magic.data(), magic.size());
    file.header.version = version;
    file.header.endian = endian;
    file.header.section_count = section_count;
    file.header.header_size = header_size;
    file.header.checksum = stored_checksum;
    file.header.metadata.cacheKeyHash = artifact.cache_key_hash;
    file.header.metadata.moduleIdHi = artifact.module_id_hi;
    file.header.metadata.moduleIdLo = artifact.module_id_lo;
    file.header.metadata.sourceFpHi = artifact.source_fp_hi;
    file.header.metadata.sourceFpLo = artifact.source_fp_lo;
    file.header.metadata.publicAbiHi = artifact.public_abi_hi;
    file.header.metadata.publicAbiLo = artifact.public_abi_lo;
    for (std::size_t i = 0; i < entries.size(); ++i)
        file.header.entries[i] = entries[i];
    file.payload = bytes;

    for (std::size_t index = 0; index < kSectionCount; ++index) {
        ByteReader section(file.section(static_cast<SectionId>(index)));
        switch (static_cast<SectionId>(index)) {
        case SectionId::Metadata:
            if (!readMetadata(section, artifact))
                return corrupt("metadata section is corrupt");
            break;
        case SectionId::Deps:
            if (!readDeps(section, artifact))
                return corrupt("deps section is corrupt");
            break;
        case SectionId::Strings:
            if (!readStrings(section, artifact))
                return corrupt("strings section is corrupt");
            break;
        case SectionId::Paths:
            if (!readPaths(section, artifact))
                return corrupt("paths section is corrupt");
            break;
        case SectionId::Decls:
            if (!readDecls(section, artifact))
                return corrupt("decls section is corrupt");
            break;
        case SectionId::Templates:
            if (!readTemplates(section, artifact))
                return corrupt("templates section is corrupt");
            break;
        case SectionId::Functions:
            if (!readFunctions(section, artifact))
                return corrupt("functions section is corrupt");
            break;
        case SectionId::Markers:
            if (!readMarkers(section, artifact))
                return corrupt("markers section is corrupt");
            break;
        case SectionId::Attrs:
            if (!readAttrs(section, artifact))
                return corrupt("attrs section is corrupt");
            break;
        case SectionId::Debug:
            if (!readDebug(section, artifact))
                return corrupt("debug section is corrupt");
            break;
        }
    }
    return artifact;
}

} // namespace toolkit::cache
