#pragma once

#include <cstdint>
#include <string_view>

namespace zith::zirl {

// Zith Intermediate Representation on disk (zirl).
//
// One artifact per canonical module path.  Layout:
//
//   [file header]
//   [section table]
//   sec1 ... sec5 payloads
//
// Sections are written in a fixed order so the same semantic module produces
// byte-stable output.  The file header carries enough identity information to
// reject an artifact before reading any section.

inline constexpr uint32_t kMagic         = 0x5A49524Cu; // "ZIRL"
inline constexpr uint32_t kFormatVersion = 1;
inline constexpr uint8_t kEndianLittle   = 1;

enum class SectionId : uint8_t {
    Header    = 0,
    Metadata  = 1, // sec2: strings, paths, identifiers, constants
    Decls     = 2, // sec3: exported/module-visible declarations
    Templates = 3, // sec4: generic blueprints
    Code      = 4, // sec5: concrete lowered HIR bodies
    Debug     = 5, // reserved
};

struct SectionEntry {
    uint64_t offset = 0;
    uint64_t size   = 0;
};

struct FileHeader {
    uint32_t magic              = kMagic;
    uint32_t format_version     = kFormatVersion;
    uint8_t endianness          = kEndianLittle;
    uint8_t reserved_a          = 0;
    uint8_t reserved_b          = 0;
    uint8_t section_count       = 0;
    uint32_t header_size        = 0; // bytes from file start to end of section table
    uint32_t checksum           = 0; // FNV-1a over all section payloads
    uint32_t cache_key_hash     = 0; // hash of CacheKey identity
    uint32_t module_id_hi       = 0; // hash(name) ^ hash(public ABI) high 32 bits
    uint32_t module_id_lo       = 0; // ... low 32 bits
    uint32_t source_fp_hi       = 0; // ContentFingerprint.primary
    uint32_t source_fp_lo       = 0; // ContentFingerprint.primary (low 32 bits)
    uint32_t public_abi_hi      = 0; // public ABI hash high 32 bits
    uint32_t public_abi_lo      = 0; // ... low 32 bits
    uint32_t dep_count          = 0;
    uint32_t decl_count         = 0;
    uint32_t template_count     = 0;
    uint32_t fn_count           = 0;
    uint32_t canonical_path_len = 0; // followed by canonical_path bytes
};

[[nodiscard]] uint32_t fnv1a32(std::string_view data) noexcept;
[[nodiscard]] uint32_t fnv1a32(const uint8_t *data, size_t len) noexcept;

} // namespace zith::zirl
