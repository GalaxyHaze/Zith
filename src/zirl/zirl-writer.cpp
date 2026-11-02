#include "zirl/zirl-writer.hpp"
#include "zirl/zirl-attrs-section.hpp"
#include "zirl/zirl-code-section.hpp"
#include "zirl/zirl-decl-section.hpp"
#include "zirl/zirl-debug-section.hpp"
#include "zirl/zirl-header.hpp"
#include "zirl/zirl-instantiation-section.hpp"
#include "zirl/zirl-type-section.hpp"
#include "support/int-limits.hpp"

#include <algorithm>
#include <limits>

namespace zith::zirl {

namespace {

struct EncodedSection {
    uint16_t id             = 0;
    uint16_t flags          = 0;
    uint16_t schema_version = kSectionSchemaVersion;
    ByteWriter payload;
    uint64_t offset = 0;
};

void writeDependency(ByteWriter &out, const cache::DependencyRecord &dep) {
    out.writeBlob(dep.canonical_path);
    out.writeBlob(dep.import_key);
    out.writeU32(dep.public_abi_hi);
    out.writeU32(dep.public_abi_lo);
}

uint64_t dependencySize(const cache::DependencyRecord &dep) {
    return 2U * sizeof(uint32_t) + dep.canonical_path.size() + dep.import_key.size() +
           2U * sizeof(uint32_t);
}

} // namespace

uint32_t Writer::write(const cache::Artifact &artifact, ByteWriter &out) {
    if (artifact.canonical_path.empty() ||
        artifact.module_identity.path != artifact.canonical_path ||
        artifact.semantic_profile.name.empty() ||
        artifact.hir_schema_version != cache::kHirSchemaVersion ||
        (artifact.required_features & cache::kRequiredHirFeatures) != cache::kRequiredHirFeatures ||
        (artifact.required_features & ~cache::kRequiredHirFeatures) != 0 ||
        support::exceedsU32(artifact.deps.size()) ||
        support::exceedsU32(artifact.canonical_path.size()) ||
        support::exceedsU32(artifact.module_name.size()) ||
        support::exceedsU32(artifact.semantic_profile.name.size()))
        return 0;

    std::vector<EncodedSection> sections;
    sections.reserve(7U + artifact.optional_sections.size());
    const auto addKnownSection = [&](SectionId id, uint16_t flags, uint16_t schema_version,
                                     auto encode) {
        EncodedSection section;
        section.id             = static_cast<uint16_t>(id);
        section.flags          = flags;
        section.schema_version = schema_version;
        if (!encode(section.payload))
            return false;
        sections.push_back(std::move(section));
        return true;
    };

    if (!addKnownSection(SectionId::Metadata, kSectionFlagRequired, kMetadataSectionSchemaVersion,
                         [&](ByteWriter &w) { return encodeTypes(artifact, w); }) ||
        !addKnownSection(SectionId::Decls, kSectionFlagRequired, kDeclsSectionSchemaVersion,
                         [&](ByteWriter &w) { return encodeDecls(artifact, w); }) ||
        !addKnownSection(SectionId::Templates, kSectionFlagRequired,
                         kTemplatesSectionSchemaVersion,
                         [&](ByteWriter &w) { return encodeTemplates(artifact, w); }) ||
        !addKnownSection(SectionId::Code, kSectionFlagRequired, kCodeSectionSchemaVersion,
                         [&](ByteWriter &w) { return encodeCode(artifact, w); }) ||
        !addKnownSection(SectionId::Attrs, kSectionFlagRequired, kAttrsSectionSchemaVersion,
                         [&](ByteWriter &w) { return encodeAttrs(artifact, w); }) ||
        !addKnownSection(SectionId::Instantiations, 0, kSectionSchemaVersion,
                         [&](ByteWriter &w) { return encodeInstantiations(artifact, w); }))
        return 0;

    uint64_t optional_features = (artifact.optional_features & ~cache::kOptionalFeatureDerivedVir) |
                                 cache::kOptionalFeatureInstantiationSummary;
    const bool has_frontend_identity = !artifact.frontend_identity.dialect.empty() ||
                                       !artifact.frontend_identity.implementation.empty() ||
                                       !artifact.frontend_identity.version.empty();
    if (has_frontend_identity) {
        if (support::exceedsU32(artifact.frontend_identity.dialect.size()) ||
            support::exceedsU32(artifact.frontend_identity.implementation.size()) ||
            support::exceedsU32(artifact.frontend_identity.version.size()))
            return 0;
        EncodedSection section;
        section.id = static_cast<uint16_t>(SectionId::FrontendProvenance);
        section.payload.writeBlob(artifact.frontend_identity.dialect);
        section.payload.writeBlob(artifact.frontend_identity.implementation);
        section.payload.writeBlob(artifact.frontend_identity.version);
        sections.push_back(std::move(section));
        optional_features |= cache::kOptionalFeatureFrontendProvenance;
    }

    // Debug section: assert/check locations for the native test runner. Emitted
    // only when there is something to resolve, so non-test artifacts stay
    // byte-identical to the pre-debug format.
    if (!artifact.test_failure_locations.empty()) {
        EncodedSection section;
        section.id             = static_cast<uint16_t>(SectionId::Debug);
        section.schema_version = kDebugSectionSchemaVersion;
        if (!encodeDebug(artifact, section.payload))
            return 0;
        sections.push_back(std::move(section));
        optional_features |= cache::kOptionalFeatureTestDebug;
    }

    for (const auto &optional : artifact.optional_sections) {
        if (optional.id == 0 ||
            optional.id <= static_cast<uint16_t>(SectionId::FrontendProvenance) ||
            support::exceedsU32(optional.payload.size()))
            return 0;
        EncodedSection section;
        section.id             = optional.id;
        section.schema_version = optional.schema_version;
        section.payload.writeBytes(optional.payload.data(), optional.payload.size());
        sections.push_back(std::move(section));
        if (optional.id == static_cast<uint16_t>(SectionId::Vir))
            optional_features |= cache::kOptionalFeatureDerivedVir;
    }

    std::sort(sections.begin(), sections.end(),
              [](const EncodedSection &a, const EncodedSection &b) { return a.id < b.id; });
    for (size_t i = 1; i < sections.size(); ++i)
        if (sections[i - 1U].id == sections[i].id)
            return 0;

    uint64_t deps_size = 0;
    for (const auto &dep : artifact.deps) {
        if (support::exceedsU32(dep.canonical_path.size()) ||
            support::exceedsU32(dep.import_key.size()))
            return 0;
        deps_size += dependencySize(dep);
    }
    const uint64_t header_size = static_cast<uint64_t>(kFixedHeaderSize) +
                                 artifact.canonical_path.size() + artifact.module_name.size() +
                                 artifact.semantic_profile.name.size() + deps_size +
                                 static_cast<uint64_t>(sections.size()) * kSectionEntrySize;
    if (header_size > std::numeric_limits<uint32_t>::max() ||
        sections.size() > std::numeric_limits<uint16_t>::max())
        return 0;

    uint64_t next_offset = header_size;
    std::vector<SectionEntry> entries;
    entries.reserve(sections.size());
    for (auto &section : sections) {
        section.offset = next_offset;
        next_offset += section.payload.size();
        entries.push_back({section.id, section.flags, section.schema_version, 0, section.offset,
                           section.payload.size()});
    }

    uint32_t checksum = fnv1a32(artifact.canonical_path) ^ fnv1a32(artifact.module_name) ^
                        fnv1a32(artifact.semantic_profile.name);
    for (const auto &dep : artifact.deps) {
        checksum ^= fnv1a32(dep.canonical_path);
        checksum ^= fnv1a32(dep.import_key);
        uint32_t abi[2] = {dep.public_abi_hi, dep.public_abi_lo};
        checksum ^= fnv1a32(reinterpret_cast<const uint8_t *>(abi), sizeof(abi));
    }
    for (const auto &section : sections)
        checksum ^= fnv1a32(section.payload.ptr(), section.payload.size());

    out.writeU32(kMagic);
    out.writeU32(kContainerVersion);
    out.writeU32(artifact.hir_schema_version);
    out.writeU8(kEndianLittle);
    out.writeU8(0);
    out.writeU16(static_cast<uint16_t>(sections.size()));
    out.writeU32(static_cast<uint32_t>(header_size));
    out.writeU32(checksum);
    out.writeU64(artifact.required_features);
    out.writeU64(optional_features);
    out.writeU64(artifact.source_fingerprint.primary);
    out.writeU64(artifact.source_fingerprint.secondary);
    out.writeU32(artifact.semantic_profile.version);
    out.writeU32(artifact.public_abi_hi);
    out.writeU32(artifact.public_abi_lo);
    out.writeU32(static_cast<uint32_t>(artifact.deps.size()));
    out.writeU32(static_cast<uint32_t>(artifact.decls.size()));
    out.writeU32(static_cast<uint32_t>(artifact.templates.size()));
    out.writeU32(static_cast<uint32_t>(artifact.functions.size()));
    out.writeU32(static_cast<uint32_t>(artifact.canonical_path.size()));
    out.writeU32(static_cast<uint32_t>(artifact.module_name.size()));
    out.writeU32(static_cast<uint32_t>(artifact.semantic_profile.name.size()));
    out.writeRaw(artifact.canonical_path);
    out.writeRaw(artifact.module_name);
    out.writeRaw(artifact.semantic_profile.name);
    for (const auto &dep : artifact.deps)
        writeDependency(out, dep);

    for (const auto &entry : entries) {
        out.writeU16(entry.id);
        out.writeU16(entry.flags);
        out.writeU16(entry.schema_version);
        out.writeU16(entry.reserved);
        out.writeU64(entry.offset);
        out.writeU64(entry.size);
    }
    for (const auto &section : sections)
        out.writeBytes(section.payload.ptr(), section.payload.size());

    return checksum;
}

} // namespace zith::zirl
