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
//   sec1 ... sec6 payloads
//
// Sections are written in a fixed order so the same semantic module produces
// byte-stable output.  The file header carries enough identity information to
// reject an artifact before reading any section.

inline constexpr uint32_t kMagic = 0x5A49524Cu; // "ZIRL"
// Bumped to 5 when module marker metadata and marker IR nodes were added to
// the cache. Version 9 removes that marker format in favour of `state`
// functions whose transitions serialize as musttail direct calls.
// Bumped to 4 when HIR expressions and residual HirAttrs were added to the
// cache.
// Bumped to 3 when module_name was added to the metadata section.
// Bumped to 2 when dependency records were folded into header_size: artifacts
// written before that are unreadable for any module with imports, so the version
// check turns stale caches into a clean miss rather than a checksum failure.
// Bumped to 6 when the persistent artifact gained the monomorphized-instance
// summary section.
/// Version 7: plain unions carry a runtime tag and store member payload first.
/// Version 8: HIR module-level `const` globals and their load nodes are
/// serialized in the Code section.
/// Version 9: state machine metadata and musttail calls replace the marker
/// blob runtime in the cache format.
/// Version 10: state machine callers use tailcc and caches persist the
/// machine return type alongside the machine id.
/// Version 11: HIR functions persist the parameter-to-HIR-slot mapping so
/// codegen can attach borrow attributes to the exact ABI argument.
/// Version 14: code expressions persist the wide `CompactExpr::ints` payload
/// used by `canonicalType`, so old caches become misses and are regenerated.
/// Version 15: artifacts persist per-module `canonicalMappings` so cached bare
/// opaque values can restore a stable project-local runtime tag.
/// Version 16: canonical type ids use the defining module, not the consumer
/// module, so `at-canonicalType(T)` agrees across module boundaries.
inline constexpr uint32_t kFormatVersion = 16;
inline constexpr uint8_t kEndianLittle   = 1;

enum class SectionId : uint8_t {
    Header    = 0,
    Metadata  = 1, // sec2: strings, paths, identifiers, constants
    Decls     = 2, // sec3: exported/module-visible declarations
    Templates = 3, // sec4: generic blueprints
    Code      = 4, // sec5: concrete lowered HIR bodies
    Attrs     = 5, // sec6: HIR residual ownership/call/fn attributes
    Debug     = 6, // reserved
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
