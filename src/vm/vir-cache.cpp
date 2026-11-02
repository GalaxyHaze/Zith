#include "vm/vir-cache.hpp"

#include "cache/cache-types.hpp"
#include "support/int-limits.hpp"
#include "vm/hir-to-vm.hpp"
#include "zirl/zirl-buffer.hpp"
#include "zirl/zirl-header.hpp"

#include <limits>
#include <string_view>

namespace zith::vm {
namespace {

constexpr uint32_t kCachedVirMagic = 0x32524956U; // "VIR2"

bool fitsU32(size_t size) {
    return support::fitsInU32(size);
}

uint16_t sectionId() {
    return static_cast<uint16_t>(zirl::SectionId::Vir);
}

} // namespace

bool encodeCachedModule(const Module &module, std::vector<uint8_t> &payload) {
    if (!fitsU32(module.strings.size()) || !fitsU32(module.i64Constants.size()) ||
        !fitsU32(module.f64Constants.size()) || !fitsU32(module.externs.size()) ||
        !fitsU32(module.functions.size()))
        return false;

    zirl::ByteWriter writer;
    writer.writeU32(kCachedVirMagic);
    writer.writeU32(kVmSemanticVersion);

    writer.writeU32(static_cast<uint32_t>(module.strings.size()));
    for (const auto value : module.strings) {
        if (!fitsU32(value.size()))
            return false;
        writer.writeBlob(value);
    }

    writer.writeU32(static_cast<uint32_t>(module.i64Constants.size()));
    for (const auto value : module.i64Constants)
        writer.writeI64(value);

    writer.writeU32(static_cast<uint32_t>(module.f64Constants.size()));
    for (const auto value : module.f64Constants)
        writer.writeF64(value);

    writer.writeU32(static_cast<uint32_t>(module.externs.size()));
    for (const auto value : module.externs) {
        if (!fitsU32(value.size()))
            return false;
        writer.writeBlob(value);
    }

    writer.writeU32(static_cast<uint32_t>(module.functions.size()));
    for (const auto &function : module.functions) {
        if (!fitsU32(function.name.size()) || !fitsU32(function.regTypes.size()) ||
            !fitsU32(function.body.size()))
            return false;
        writer.writeBlob(function.name);
        writer.writeU8(static_cast<uint8_t>(function.returnType));
        writer.writeU16(function.paramCount);
        writer.writeU16(function.regCount);
        writer.writeU8(function.returnsAggregate ? 1U : 0U);
        writer.writeU8(function.returnsSlice ? 1U : 0U);
        writer.writeU32(static_cast<uint32_t>(function.regTypes.size()));
        for (const auto type : function.regTypes)
            writer.writeU8(static_cast<uint8_t>(type));
        writer.writeU32(static_cast<uint32_t>(function.body.size()));
        for (const auto &instruction : function.body) {
            writer.writeU8(static_cast<uint8_t>(instruction.op));
            writer.writeU16(instruction.a);
            writer.writeU16(instruction.b);
            writer.writeU16(instruction.c);
            writer.writeU16(instruction.imm);
            writer.writeU16(instruction.d);
            writer.writeU16(instruction.e);
        }
    }

    payload = writer.data();
    return true;
}

bool decodeCachedModule(std::span<const uint8_t> payload, uint16_t section_schema_version,
                        memory::StringInterner &interner, memory::Arena &arena, Module &out) {
    if (section_schema_version != kVirSchemaVersion)
        return false;

    zirl::ByteReader reader(payload.data(), payload.size());
    uint32_t magic            = 0;
    uint32_t semantic_version = 0;
    if (!reader.readU32(magic) || !reader.readU32(semantic_version) || magic != kCachedVirMagic ||
        semantic_version != kVmSemanticVersion)
        return false;

    Module decoded(arena);
    uint32_t count = 0;

    if (!reader.readU32(count) || !reader.canReadU32Count(count))
        return false;
    for (uint32_t index = 0; index < count; ++index) {
        std::string_view value;
        if (!reader.readBlob(value))
            return false;
        decoded.strings.push(interner.lookup(interner.intern(value)));
    }

    if (!reader.readU32(count) || !reader.canReadU32Count(count, sizeof(uint64_t)))
        return false;
    for (uint32_t index = 0; index < count; ++index) {
        int64_t value = 0;
        if (!reader.readI64(value))
            return false;
        decoded.i64Constants.push(value);
    }

    if (!reader.readU32(count) || !reader.canReadU32Count(count, sizeof(uint64_t)))
        return false;
    for (uint32_t index = 0; index < count; ++index) {
        double value = 0;
        if (!reader.readF64(value))
            return false;
        decoded.f64Constants.push(value);
    }

    if (!reader.readU32(count) || !reader.canReadU32Count(count))
        return false;
    for (uint32_t index = 0; index < count; ++index) {
        std::string_view value;
        if (!reader.readBlob(value))
            return false;
        decoded.externs.push(interner.lookup(interner.intern(value)));
    }

    if (!reader.readU32(count) || !reader.canReadU32Count(count, 19U))
        return false;
    for (uint32_t index = 0; index < count; ++index) {
        auto &function = decoded.functions.emplace(arena);
        std::string_view name;
        uint8_t return_type       = 0;
        uint8_t returns_aggregate = 0;
        uint8_t returns_slice     = 0;
        if (!reader.readBlob(name) || !reader.readU8(return_type) ||
            return_type > static_cast<uint8_t>(ValueType::Void) ||
            !reader.readU16(function.paramCount) || !reader.readU16(function.regCount) ||
            !reader.readU8(returns_aggregate) || returns_aggregate > 1U ||
            !reader.readU8(returns_slice) || returns_slice > 1U)
            return false;
        function.name             = interner.lookup(interner.intern(name));
        function.returnType       = static_cast<ValueType>(return_type);
        function.returnsAggregate = returns_aggregate != 0;
        function.returnsSlice     = returns_slice != 0;

        uint32_t item_count = 0;
        if (!reader.readU32(item_count) || !reader.canReadU32Count(item_count, 1U))
            return false;
        for (uint32_t item = 0; item < item_count; ++item) {
            uint8_t type = 0;
            if (!reader.readU8(type) || type > static_cast<uint8_t>(ValueType::Void))
                return false;
            function.regTypes.push(static_cast<ValueType>(type));
        }

        if (!reader.readU32(item_count) || !reader.canReadU32Count(item_count, 13U))
            return false;
        for (uint32_t item = 0; item < item_count; ++item) {
            Instr instruction;
            uint8_t op = 0;
            if (!reader.readU8(op) || op > static_cast<uint8_t>(Op::Panic) ||
                !reader.readU16(instruction.a) || !reader.readU16(instruction.b) ||
                !reader.readU16(instruction.c) || !reader.readU16(instruction.imm) ||
                !reader.readU16(instruction.d) || !reader.readU16(instruction.e))
                return false;
            instruction.op = static_cast<Op>(op);
            function.body.push(instruction);
        }
    }

    if (!reader.empty())
        return false;
    out = std::move(decoded);
    return true;
}

CachedVirResult loadOrLowerCachedModule(const cache::Artifact *artifact, const hir::HirModule &hir,
                                        memory::StringInterner &interner,
                                        const types::TypeIntern &types, memory::Arena &arena,
                                        Module &out) {
    if (artifact != nullptr) {
        const auto vir_id = sectionId();
        for (const auto &section : artifact->optional_sections) {
            if (section.id != vir_id)
                continue;
            if (decodeCachedModule(section.payload, section.schema_version, interner, arena, out))
                return {true, true, {}};
            break;
        }
    }

    const auto lowered = lowerModule(hir, interner, types, arena, out);
    return {lowered.ok, false, lowered.message};
}

} // namespace zith::vm
