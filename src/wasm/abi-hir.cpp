#include "wasm/abi-hir.hpp"

#include "common/ast-ids.hpp"
#include "common/overloaded.hpp"
#include "hir/hir-expr.hpp"
#include "memory/flat-map.hpp"
#include "types/type-kind.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zith::wasm {
namespace {

constexpr uint32_t kBlobMagic   = 0x5A4D4849U; // "ZMHI"
// v2: HirMakeDyn carries value_is_place; v3: HirCall carries variadic slice plans;
// v4: function references are HIR-module indices, not symbol-table ids.
// v5: safety modes and expression-bound proof facts are part of the HIR blob.
// v6: generic parameter owners are canonical names, not frontend-local IDs.
// v7: module-local vtables and concrete struct definitions are serialized.
constexpr uint32_t kBlobVersion = 7U;

constexpr size_t kMaxArrayCount = 1U << 20;

class Writer {
    std::vector<uint8_t> data_;

public:
    void putU8(uint8_t value) {
        data_.push_back(value);
    }
    void putU32(uint32_t value) {
        for (int i = 0; i < 4; ++i)
            data_.push_back(static_cast<uint8_t>(value >> (i * 8)));
    }
    void putI64(int64_t value) {
        for (int i = 0; i < 8; ++i)
            data_.push_back(static_cast<uint8_t>(static_cast<uint64_t>(value) >> (i * 8)));
    }
    void putString(std::string_view text) {
        putU32(static_cast<uint32_t>(text.size()));
        data_.insert(data_.end(), text.begin(), text.end());
    }

    [[nodiscard]] const std::vector<uint8_t> &bytes() const noexcept {
        return data_;
    }
};

class Reader {
    std::span<const uint8_t> data_;
    size_t offset_ = 0;

public:
    explicit Reader(std::span<const uint8_t> data) : data_(data) {}

    [[nodiscard]] bool ok() const noexcept {
        return offset_ <= data_.size();
    }
    uint8_t u8() {
        if (!ok() || data_.size() - offset_ < 1) {
            offset_ = data_.size() + 1;
            return 0;
        }
        return data_[offset_++];
    }
    uint32_t u32() {
        if (!ok() || data_.size() - offset_ < 4) {
            offset_ = data_.size() + 1;
            return 0;
        }
        uint32_t value = 0;
        for (size_t i = 0; i < 4; ++i)
            value |= static_cast<uint32_t>(data_[offset_ + i]) << (i * 8);
        offset_ += 4;
        return value;
    }
    int64_t i64() {
        if (!ok() || data_.size() - offset_ < 8) {
            offset_ = data_.size() + 1;
            return 0;
        }
        uint64_t value = 0;
        for (size_t i = 0; i < 8; ++i)
            value |= static_cast<uint64_t>(data_[offset_ + i]) << (i * 8);
        offset_ += 8;
        return static_cast<int64_t>(value);
    }
    std::string_view string() {
        const uint32_t length = u32();
        if (!ok() || data_.size() - offset_ < length) {
            offset_ = data_.size() + 1;
            return {};
        }
        const std::string_view result(
            reinterpret_cast<const char *>(data_.data() + offset_), length);
        offset_ += length;
        return result;
    }
};

template <typename E> uint8_t enumByte(E value) {
    return static_cast<uint8_t>(value);
}

template <typename E> E readEnum(Reader &reader) {
    return static_cast<E>(reader.u8());
}

void writeTypeData(Writer &writer, const types::TypeData &data,
                   const memory::StringInterner &interner) {
    std::visit(
        common::overloaded{
            [&](const types::TypeError &) { writer.putU8(enumByte(types::TypeKind::Error)); },
            [&](const types::TypeNever &) { writer.putU8(enumByte(types::TypeKind::Never)); },
            [&](const types::TypeVoid &) { writer.putU8(enumByte(types::TypeKind::Void)); },
            [&](const types::TypeBool &) { writer.putU8(enumByte(types::TypeKind::Bool)); },
            [&](const types::TypeChar &) { writer.putU8(enumByte(types::TypeKind::Char)); },
            [&](const types::TypeInt &value) {
                writer.putU8(enumByte(types::TypeKind::Int));
                writer.putU8(enumByte(value.width));
            },
            [&](const types::TypeFloat &value) {
                writer.putU8(enumByte(types::TypeKind::Float));
                writer.putU8(enumByte(value.width));
            },
            [&](const types::TypePtr &value) {
                writer.putU8(enumByte(types::TypeKind::Ptr));
                writer.putU32(value.pointee);
                writer.putU8(value.is_mut ? 1U : 0U);
                writer.putU8(enumByte(value.ownership));
            },
            [&](const types::TypeArray &value) {
                writer.putU8(enumByte(types::TypeKind::Array));
                writer.putU32(value.elem);
                writer.putU32(value.count);
            },
            [&](const types::TypeStruct &value) {
                writer.putU8(enumByte(types::TypeKind::Struct));
                writer.putU32(value.def_id);
            },
            [&](const types::TypeFn &value) {
                writer.putU8(enumByte(types::TypeKind::Fn));
                writer.putU32(static_cast<uint32_t>(value.param_count));
                for (size_t i = 0; i < value.param_count; ++i)
                    writer.putU32(value.params[i]);
                writer.putU32(value.ret);
            },
            [&](const types::TypeTypeVar &value) {
                writer.putU8(enumByte(types::TypeKind::TypeVar));
                writer.putU32(value.id);
            },
            [&](const types::TypeOptional &value) {
                writer.putU8(enumByte(types::TypeKind::Optional));
                writer.putU32(value.inner);
            },
            [&](const types::TypeFailable &value) {
                writer.putU8(enumByte(types::TypeKind::Failable));
                writer.putU32(value.inner);
            },
            [&](const types::TypeAlias &value) {
                writer.putU8(enumByte(types::TypeKind::Alias));
                writer.putU32(value.target);
            },
            [&](const types::TypeNominal &value) {
                writer.putU8(enumByte(types::TypeKind::Nominal));
                writer.putU32(value.name);
                writer.putU32(value.target);
            },
            [&](const types::TypeTrait &value) {
                writer.putU8(enumByte(types::TypeKind::Trait));
                writer.putU32(value.name);
            },
            [&](const types::TypeDyn &value) {
                writer.putU8(enumByte(types::TypeKind::Dyn));
                writer.putU32(value.target);
                writer.putU32(static_cast<uint32_t>(value.method_count));
            },
            [&](const types::TypeOpaque &) { writer.putU8(enumByte(types::TypeKind::Opaque)); },
            [&](const types::TypeOpaqueTagged &) {
                writer.putU8(enumByte(types::TypeKind::OpaqueTagged));
            },
            [&](const types::TypeUnknown &) { writer.putU8(enumByte(types::TypeKind::Unknown)); },
            [&](const types::TypeQualified &value) {
                writer.putU8(enumByte(types::TypeKind::Qualified));
                writer.putU32(value.inner);
                writer.putU8(enumByte(value.ownership));
                writer.putU8(value.isMut ? 1U : 0U);
            },
            [&](const types::TypeSlice &value) {
                writer.putU8(enumByte(types::TypeKind::Slice));
                writer.putU32(value.elem);
            },
            [&](const types::TypeEnum &value) {
                writer.putU8(enumByte(types::TypeKind::Enum));
                writer.putU32(value.def_id);
            },
            [&](const types::TypeUnion &value) {
                writer.putU8(enumByte(types::TypeKind::Union));
                writer.putU32(value.def_id);
            },
            [&](const types::TypePack &value) {
                writer.putU8(enumByte(types::TypeKind::Pack));
                writer.putU32(static_cast<uint32_t>(value.count));
                for (size_t i = 0; i < value.count; ++i)
                    writer.putU32(value.members[i]);
                for (size_t i = 0; i < value.count; ++i)
                    writer.putU32(value.names[i]);
            },
            [&](const types::TypeSum &value) {
                writer.putU8(enumByte(types::TypeKind::Sum));
                writer.putU32(static_cast<uint32_t>(value.count));
                for (size_t i = 0; i < value.count; ++i)
                    writer.putU32(value.members[i]);
            },
            [&](const types::TypeGenericParam &value) {
                writer.putU8(enumByte(types::TypeKind::GenericParam));
                writer.putString(interner.lookup(value.owner_name));
                writer.putU32(value.param_index);
            },
            [&](const types::TypeIncomplete &value) {
                writer.putU8(enumByte(types::TypeKind::Incomplete));
                writer.putU32(value.base);
                writer.putU32(static_cast<uint32_t>(value.arg_count));
                for (size_t i = 0; i < value.arg_count; ++i)
                    writer.putU32(value.args[i]);
            },
        },
        data);
}

/// Rebuild one type row without deduplication. `TypeIntern::intern` collapses
/// structurally equal rows, which would shift canonical type ids and break the
/// expression/function tables that reference them. The decoded table must keep
/// the same positional identity as the encoded table.
[[nodiscard]] types::TypeData readTypeData(Reader &reader, memory::Arena &arena,
                                           memory::StringInterner &interner) {
    const auto kind = readEnum<types::TypeKind>(reader);
    switch (kind) {
    case types::TypeKind::Error:
        return types::TypeError{};
    case types::TypeKind::Never:
        return types::TypeNever{};
    case types::TypeKind::Void:
        return types::TypeVoid{};
    case types::TypeKind::Bool:
        return types::TypeBool{};
    case types::TypeKind::Char:
        return types::TypeChar{};
    case types::TypeKind::Int:
        return types::TypeInt{readEnum<types::IntWidth>(reader)};
    case types::TypeKind::Float:
        return types::TypeFloat{readEnum<types::FloatWidth>(reader)};
    case types::TypeKind::Ptr:
        return types::TypePtr{reader.u32(), reader.u8() != 0,
                              readEnum<types::OwnershipKind>(reader)};
    case types::TypeKind::Array:
        return types::TypeArray{reader.u32(), reader.u32()};
    case types::TypeKind::Struct:
        return types::TypeStruct{reader.u32()};
    case types::TypeKind::Fn: {
        const uint32_t count = reader.u32();
        if (count > kMaxArrayCount)
            return {};
        auto *params = static_cast<types::TypeId *>(
            arena.alloc(count * sizeof(types::TypeId), alignof(types::TypeId)));
        for (uint32_t i = 0; i < count; ++i)
            params[i] = reader.u32();
        return types::TypeFn{params, count, reader.u32()};
    }
    case types::TypeKind::TypeVar:
        return types::TypeTypeVar{reader.u32()};
    case types::TypeKind::Optional:
        return types::TypeOptional{reader.u32()};
    case types::TypeKind::Failable:
        return types::TypeFailable{reader.u32()};
    case types::TypeKind::Alias:
        return types::TypeAlias{reader.u32()};
    case types::TypeKind::Nominal:
        return types::TypeNominal{reader.u32(), reader.u32()};
    case types::TypeKind::Trait:
        return types::TypeTrait{reader.u32()};
    case types::TypeKind::Dyn:
        return types::TypeDyn{reader.u32(), reader.u32()};
    case types::TypeKind::Opaque:
        return types::TypeOpaque{};
    case types::TypeKind::OpaqueTagged:
        return types::TypeOpaqueTagged{};
    case types::TypeKind::Unknown:
        return types::TypeUnknown{};
    case types::TypeKind::Qualified:
        return types::TypeQualified{reader.u32(), readEnum<types::OwnershipKind>(reader),
                                    reader.u8() != 0};
    case types::TypeKind::Slice:
        return types::TypeSlice{reader.u32()};
    case types::TypeKind::Enum:
        return types::TypeEnum{reader.u32()};
    case types::TypeKind::Union:
        return types::TypeUnion{reader.u32()};
    case types::TypeKind::Pack: {
        const uint32_t count = reader.u32();
        if (count > kMaxArrayCount)
            return {};
        auto *members = static_cast<types::TypeId *>(
            arena.alloc(count * sizeof(types::TypeId), alignof(types::TypeId)));
        auto *names = static_cast<memory::InternedId *>(
            arena.alloc(count * sizeof(memory::InternedId), alignof(memory::InternedId)));
        for (uint32_t i = 0; i < count; ++i)
            members[i] = reader.u32();
        for (uint32_t i = 0; i < count; ++i)
            names[i] = reader.u32();
        return types::TypePack{members, names, count};
    }
    case types::TypeKind::Sum: {
        const uint32_t count = reader.u32();
        if (count > kMaxArrayCount)
            return {};
        auto *members = static_cast<types::TypeId *>(
            arena.alloc(count * sizeof(types::TypeId), alignof(types::TypeId)));
        for (uint32_t i = 0; i < count; ++i)
            members[i] = reader.u32();
        return types::TypeSum{members, count};
    }
    case types::TypeKind::GenericParam: {
        const auto owner_name = reader.string();
        const auto owner_id = reader.ok() ? interner.intern(owner_name)
                                          : memory::kInvalidInternedId;
        return types::TypeGenericParam{owner_id, reader.u32()};
    }
    case types::TypeKind::Incomplete: {
        const uint32_t count = reader.u32();
        if (count > kMaxArrayCount)
            return {};
        auto *args = static_cast<types::TypeId *>(
            arena.alloc(count * sizeof(types::TypeId), alignof(types::TypeId)));
        for (uint32_t i = 0; i < count; ++i)
            args[i] = reader.u32();
        return types::TypeIncomplete{reader.u32(), args, count};
    }
    case types::TypeKind::String:
    case types::TypeKind::Invalid:
        return {};
    }
    return {};
}

void writeExpr(Writer &writer, const hir::HirModule &module, hir::HirExprId id) {
    const auto &expr = module.getExpr(id);
    std::visit(
        common::overloaded{
            [&](const hir::HirLiteral &value) {
                writer.putU8(enumByte(hir::HirExprKind::Literal));
                writer.putU32(value.type);
                writer.putI64(value.i);
                writer.putU32(value.str_val);
            },
            [&](const hir::HirBinary &value) {
                writer.putU8(enumByte(hir::HirExprKind::Binary));
                writer.putU32(value.lhs);
                writer.putU32(value.rhs);
                writer.putU8(enumByte(value.op));
                writer.putU32(value.type);
                writer.putU32(value.operand_type);
            },
            [&](const hir::HirUnary &value) {
                writer.putU8(enumByte(hir::HirExprKind::Unary));
                writer.putU8(enumByte(value.op));
                writer.putU32(value.operand);
                writer.putU32(value.type);
            },
            [&](const hir::HirLet &value) {
                writer.putU8(enumByte(hir::HirExprKind::Let));
                writer.putU32(value.name);
                writer.putU32(value.type);
                writer.putU32(value.init);
            },
            [&](const hir::HirVar &value) {
                writer.putU8(enumByte(hir::HirExprKind::Var));
                writer.putU32(value.name);
                writer.putU32(value.version);
            },
            [&](const hir::HirCall &value) {
                writer.putU8(enumByte(hir::HirExprKind::Call));
                writer.putU32(value.callee);
                writer.putU32(static_cast<uint32_t>(value.args.size()));
                for (const auto arg : value.args)
                    writer.putU32(arg);
                writer.putU32(static_cast<uint32_t>(value.argument_types.size()));
                for (const auto type : value.argument_types)
                    writer.putU32(type);
                writer.putU32(value.fn_type);
                writer.putU32(value.resolved_fn);
                writer.putU32(value.variadicSliceParam);
                writer.putU8(value.isVariadicSlice ? 1U : 0U);
                writer.putU8(value.autoCollectTail ? 1U : 0U);
                writer.putU8(value.usesTailCC ? 1U : 0U);
                writer.putU8(value.musttail ? 1U : 0U);
            },
            [&](const hir::HirRet &value) {
                writer.putU8(enumByte(hir::HirExprKind::Ret));
                writer.putU32(value.value);
            },
            [&](const hir::HirBranch &value) {
                writer.putU8(enumByte(hir::HirExprKind::Branch));
                writer.putU32(value.cond);
                writer.putU32(value.then_block);
                writer.putU32(value.else_block);
            },
            [&](const hir::HirJump &value) {
                writer.putU8(enumByte(hir::HirExprKind::Jump));
                writer.putU32(value.target);
            },
            [&](const hir::HirPhi &value) {
                writer.putU8(enumByte(hir::HirExprKind::Phi));
                writer.putU32(static_cast<uint32_t>(value.incoming.size()));
                for (const auto incoming : value.incoming)
                    writer.putU32(incoming);
            },
            [&](const hir::HirAssign &value) {
                writer.putU8(enumByte(hir::HirExprKind::Assign));
                writer.putU32(value.target);
                writer.putU32(value.value);
            },
            [&](const hir::HirIndex &value) {
                writer.putU8(enumByte(hir::HirExprKind::Index));
                writer.putU32(value.object);
                writer.putU32(value.index);
                writer.putU32(value.type);
                writer.putU32(value.obj_type);
                writer.putU8(value.is_array ? 1U : 0U);
                writer.putU8(enumByte(value.safety));
            },
            [&](const hir::HirField &value) {
                writer.putU8(enumByte(hir::HirExprKind::Field));
                writer.putU32(value.object);
                writer.putU32(value.index);
                writer.putU32(value.type);
                writer.putU32(value.object_type);
                writer.putU8(enumByte(value.safety));
            },
            [&](const hir::HirStructLiteral &value) {
                writer.putU8(enumByte(hir::HirExprKind::StructLiteral));
                writer.putU32(static_cast<uint32_t>(value.values.size()));
                for (const auto entry : value.values)
                    writer.putU32(entry);
                writer.putU32(value.type);
            },
            [&](const hir::HirArrayLiteral &value) {
                writer.putU8(enumByte(hir::HirExprKind::ArrayLiteral));
                writer.putU32(static_cast<uint32_t>(value.elements.size()));
                for (const auto entry : value.elements)
                    writer.putU32(entry);
                writer.putU32(value.type);
            },
            [&](const hir::HirEnumValue &value) {
                writer.putU8(enumByte(hir::HirExprKind::EnumValue));
                writer.putI64(value.value);
                writer.putU32(value.type);
            },
            [&](const hir::HirSlotAlloca &value) {
                writer.putU8(enumByte(hir::HirExprKind::SlotAlloca));
                writer.putU32(value.slot);
                writer.putU32(value.type);
            },
            [&](const hir::HirSlotStore &value) {
                writer.putU8(enumByte(hir::HirExprKind::SlotStore));
                writer.putU32(value.slot);
                writer.putU32(value.value);
            },
            [&](const hir::HirSlotLoad &value) {
                writer.putU8(enumByte(hir::HirExprKind::SlotLoad));
                writer.putU32(value.slot);
                writer.putU32(value.type);
            },
            [&](const hir::HirSlotAddr &value) {
                writer.putU8(enumByte(hir::HirExprKind::SlotAddr));
                writer.putU32(value.slot);
                writer.putU32(value.type);
            },
            [&](const hir::HirMakeNone &value) {
                writer.putU8(enumByte(hir::HirExprKind::MakeNone));
                writer.putU32(value.type);
            },
            [&](const hir::HirMakeSome &value) {
                writer.putU8(enumByte(hir::HirExprKind::MakeSome));
                writer.putU32(value.value);
                writer.putU32(value.type);
            },
            [&](const hir::HirMakeSlice &value) {
                writer.putU8(enumByte(hir::HirExprKind::MakeSlice));
                writer.putU32(value.object);
                writer.putU32(value.lo);
                writer.putU32(value.hi);
                writer.putU32(value.type);
                writer.putU32(value.object_type);
                writer.putU32(value.bound_type);
                writer.putU8(value.is_array ? 1U : 0U);
                writer.putU8(value.is_pointer ? 1U : 0U);
                writer.putU8(enumByte(value.safety));
            },
            [&](const hir::HirCast &value) {
                writer.putU8(enumByte(hir::HirExprKind::Cast));
                writer.putU8(0U); // plain numeric cast marker
                writer.putU32(value.value);
                writer.putU32(value.from);
                writer.putU32(value.to);
            },
            [&](const hir::HirUnionCast &value) {
                writer.putU8(enumByte(hir::HirExprKind::Cast));
                writer.putU8(1U); // union reinterpret cast marker
                writer.putU32(value.value);
                writer.putU32(value.from);
                writer.putU32(value.to);
                writer.putU32(value.member_index);
                writer.putU8(enumByte(value.safety));
            },
            [&](const hir::HirUnionCheck &value) {
                writer.putU8(enumByte(hir::HirExprKind::UnionCheck));
                writer.putU32(value.value);
                writer.putU32(value.union_type);
                writer.putU32(value.member_index);
            },
            [&](const hir::HirLayoutIntrinsic &value) {
                writer.putU8(enumByte(hir::HirExprKind::LayoutIntrinsic));
                writer.putU8(enumByte(value.which));
                writer.putU32(value.type);
                writer.putU32(value.field_index);
                writer.putU32(value.operand);
                writer.putU32(value.operand_type);
                writer.putI64(static_cast<int64_t>(value.string_length));
            },
            [&](const hir::HirStateTailCall &value) {
                writer.putU8(enumByte(hir::HirExprKind::StateTailCall));
                const auto &call = value.call;
                writer.putU32(call.callee);
                writer.putU32(static_cast<uint32_t>(call.args.size()));
                for (const auto arg : call.args)
                    writer.putU32(arg);
                writer.putU32(static_cast<uint32_t>(call.argument_types.size()));
                for (const auto type : call.argument_types)
                    writer.putU32(type);
                writer.putU32(call.fn_type);
                writer.putU32(call.resolved_fn);
                writer.putU8(call.usesTailCC ? 1U : 0U);
                writer.putU8(call.musttail ? 1U : 0U);
            },
            [&](const hir::HirCleanup &value) {
                writer.putU8(enumByte(hir::HirExprKind::Cleanup));
                writer.putU32(static_cast<uint32_t>(value.exprs.size()));
                for (const auto element : value.exprs)
                    writer.putU32(element);
            },
            [&](const hir::HirGlobalConstLoad &value) {
                writer.putU8(enumByte(hir::HirExprKind::GlobalConstLoad));
                writer.putU32(value.name);
                writer.putU32(value.type);
            },
            [&](const hir::HirExternalGlobalAddr &value) {
                writer.putU8(enumByte(hir::HirExprKind::ExternalGlobalAddr));
                writer.putU32(value.name);
                writer.putU32(value.type);
            },
            [&](const hir::HirMakeDyn &value) {
                writer.putU8(enumByte(hir::HirExprKind::MakeDyn));
                writer.putU32(value.value);
                writer.putU32(value.source_type);
                writer.putU32(value.dyn_type);
                writer.putU32(value.vtable_name);
                writer.putU8(value.value_is_place ? 1U : 0U);
            },
            [&](const hir::HirDynCall &value) {
                writer.putU8(enumByte(hir::HirExprKind::DynCall));
                writer.putU32(value.receiver);
                writer.putU32(value.vtable_name);
                writer.putU32(value.slot_index);
                writer.putU8(value.has_receiver ? 1U : 0U);
                writer.putU32(value.result_type);
                writer.putU32(value.fn_type);
                writer.putU32(static_cast<uint32_t>(value.args.size()));
                for (const auto arg : value.args)
                    writer.putU32(arg);
                writer.putU32(static_cast<uint32_t>(value.arg_types.size()));
                for (const auto type : value.arg_types)
                    writer.putU32(type);
            },
            [&](const hir::HirMakeOpaque &value) {
                writer.putU8(enumByte(hir::HirExprKind::MakeOpaque));
                writer.putU32(value.value);
                writer.putU32(value.source_type);
                writer.putU32(value.opaque_type);
                writer.putU32(value.type_id);
                writer.putI64(static_cast<int64_t>(value.canonical_id.hi));
                writer.putI64(static_cast<int64_t>(value.canonical_id.lo));
            },
            [&](const hir::HirOpaqueCast &value) {
                writer.putU8(enumByte(hir::HirExprKind::OpaqueCast));
                writer.putU32(value.value);
                writer.putU32(value.from);
                writer.putU32(value.to);
                writer.putU32(value.opaque_type);
                writer.putU32(value.result_type);
                writer.putU32(value.type_id);
                writer.putI64(static_cast<int64_t>(value.canonical_id.hi));
                writer.putI64(static_cast<int64_t>(value.canonical_id.lo));
                writer.putU8(enumByte(value.safety));
                writer.putU8(value.returns_ptr ? 1U : 0U);
            },
            [&](const hir::HirOpaqueCheck &value) {
                writer.putU8(enumByte(hir::HirExprKind::OpaqueCheck));
                writer.putU32(value.value);
                writer.putU32(value.opaque_type);
                writer.putU32(value.type_id);
                writer.putI64(static_cast<int64_t>(value.canonical_id.hi));
                writer.putI64(static_cast<int64_t>(value.canonical_id.lo));
            },
            [&](const hir::HirRuntimePanic &value) {
                writer.putU8(enumByte(hir::HirExprKind::RuntimePanic));
                writer.putU32(value.code);
            },
            [&](const hir::HirCanonicalType &value) {
                writer.putU8(enumByte(hir::HirExprKind::CanonicalType));
                writer.putI64(static_cast<int64_t>(value.canonical_id.hi));
                writer.putI64(static_cast<int64_t>(value.canonical_id.lo));
                writer.putU32(value.type);
            },
            [&](const hir::HirPipe &value) {
                writer.putU8(enumByte(hir::HirExprKind::Pipe));
                writer.putU32(value.source_slot);
                writer.putU32(value.source_type);
                writer.putU32(value.stage);
                writer.putU32(value.type);
                writer.putU8(value.is_effect ? 1U : 0U);
            },
            [&](const hir::HirPipeCurrent &value) {
                writer.putU8(enumByte(hir::HirExprKind::PipeCurrent));
                writer.putU32(value.slot);
                writer.putU32(value.type);
            },
        },
        expr);
}

void writeExprs(Writer &writer, const hir::HirModule &module) {
    writer.putU32(static_cast<uint32_t>(module.exprCount()));
    for (size_t i = 0; i < module.exprCount(); ++i)
        writeExpr(writer, module, static_cast<hir::HirExprId>(i));
}

void writeFunction(Writer &writer, const hir::HirFunction &function) {
    writer.putU32(function.name);
    writer.putU32(static_cast<uint32_t>(function.params.size()));
    for (const auto param : function.params)
        writer.putU32(param);
    writer.putU32(static_cast<uint32_t>(function.param_names.size()));
    for (const auto name : function.param_names)
        writer.putU32(name);
    writer.putU32(static_cast<uint32_t>(function.param_slots.size()));
    for (const auto slot : function.param_slots)
        writer.putU32(slot);
    writer.putU32(function.return_type);
    writer.putU8(function.isState ? 1U : 0U);
    writer.putU8(function.usesTailCC ? 1U : 0U);
    writer.putU32(function.machineReturnType);
    writer.putU32(function.machineId);
    writer.putU8(function.isVariadic ? 1U : 0U);
    writer.putU32(static_cast<uint32_t>(function.variadicSliceParam));
    writer.putU8(function.isForeignC ? 1U : 0U);
    writer.putU8(function.hasBody ? 1U : 0U);
    writer.putU32(function.fnSpan.file);
    writer.putU32(function.fnSpan.start);
    writer.putU32(function.fnSpan.end);
    writer.putU32(static_cast<uint32_t>(function.blocks.size()));
    for (const auto &block : function.blocks) {
        writer.putU32(static_cast<uint32_t>(block.insts.size()));
        for (const auto inst : block.insts)
            writer.putU32(inst);
        writer.putU32(block.terminator);
    }
}

void writeFunctions(Writer &writer, const hir::HirModule &module) {
    writer.putU32(static_cast<uint32_t>(module.getFnCount()));
    for (size_t i = 0; i < module.getFnCount(); ++i)
        writeFunction(writer, module.getFn(i));
}

auto readDynU32(Reader &reader, memory::DynArray<uint32_t> &out) -> bool {
    const uint32_t count = reader.u32();
    if (count > kMaxArrayCount)
        return false;
    for (uint32_t i = 0; i < count; ++i)
        out.push(reader.u32());
    return reader.ok();
}

auto readDynType(Reader &reader, memory::DynArray<types::TypeId> &out) -> bool {
    const uint32_t count = reader.u32();
    if (count > kMaxArrayCount)
        return false;
    for (uint32_t i = 0; i < count; ++i)
        out.push(reader.u32());
    return reader.ok();
}

auto readExpr(Reader &reader, hir::HirModule &module, memory::Arena &arena) -> bool {
    const auto kind = readEnum<hir::HirExprKind>(reader);
    switch (kind) {
    case hir::HirExprKind::Literal: {
        hir::HirLiteral value;
        value.type = reader.u32();
        value.i    = reader.i64();
        value.str_val = reader.u32();
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::Binary: {
        hir::HirBinary value;
        value.lhs          = reader.u32();
        value.rhs          = reader.u32();
        value.op           = readEnum<hir::HirBinaryOp>(reader);
        value.type         = reader.u32();
        value.operand_type = reader.u32();
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::Unary: {
        hir::HirUnary value;
        value.op     = readEnum<hir::HirUnaryOp>(reader);
        value.operand = reader.u32();
        value.type   = reader.u32();
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::Let: {
        hir::HirLet value;
        value.name = reader.u32();
        value.type = reader.u32();
        value.init = reader.u32();
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::Var: {
        hir::HirVar value;
        value.name    = reader.u32();
        value.version = reader.u32();
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::Call: {
        hir::HirCall value(reader.u32(), memory::DynArray<hir::HirExprId>(arena),
                           memory::DynArray<types::TypeId>(arena));
        if (!readDynU32(reader, value.args) || !readDynType(reader, value.argument_types))
            return false;
        value.fn_type      = reader.u32();
        value.resolved_fn  = reader.u32();
        value.variadicSliceParam = reader.u32();
        value.isVariadicSlice    = reader.u8() != 0;
        value.autoCollectTail    = reader.u8() != 0;
        value.usesTailCC   = reader.u8() != 0;
        value.musttail     = reader.u8() != 0;
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::Ret: {
        hir::HirRet value;
        value.value = reader.u32();
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::Branch: {
        hir::HirBranch value;
        value.cond       = reader.u32();
        value.then_block = reader.u32();
        value.else_block = reader.u32();
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::Jump: {
        hir::HirJump value;
        value.target = reader.u32();
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::Phi: {
        hir::HirPhi value(arena);
        if (!readDynU32(reader, value.incoming))
            return false;
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::Assign: {
        hir::HirAssign value;
        value.target = reader.u32();
        value.value  = reader.u32();
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::Index: {
        hir::HirIndex value;
        value.object   = reader.u32();
        value.index    = reader.u32();
        value.type     = reader.u32();
        value.obj_type = reader.u32();
        value.is_array = reader.u8() != 0;
        const auto safety = reader.u8();
        if (safety > enumByte(hir::HirSafetyMode::Raw))
            return false;
        value.safety = static_cast<hir::HirSafetyMode>(safety);
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::Field: {
        hir::HirField value;
        value.object      = reader.u32();
        value.index       = reader.u32();
        value.type        = reader.u32();
        value.object_type = reader.u32();
        const auto safety = reader.u8();
        if (safety > enumByte(hir::HirSafetyMode::Raw))
            return false;
        value.safety = static_cast<hir::HirSafetyMode>(safety);
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::StructLiteral: {
        hir::HirStructLiteral value(arena);
        if (!readDynU32(reader, value.values))
            return false;
        value.type = reader.u32();
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::ArrayLiteral: {
        hir::HirArrayLiteral value(arena);
        if (!readDynU32(reader, value.elements))
            return false;
        value.type = reader.u32();
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::EnumValue: {
        hir::HirEnumValue value;
        value.value = reader.i64();
        value.type  = reader.u32();
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::SlotAlloca: {
        hir::HirSlotAlloca value;
        value.slot = reader.u32();
        value.type = reader.u32();
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::SlotStore: {
        hir::HirSlotStore value;
        value.slot  = reader.u32();
        value.value = reader.u32();
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::SlotLoad: {
        hir::HirSlotLoad value;
        value.slot = reader.u32();
        value.type = reader.u32();
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::SlotAddr: {
        hir::HirSlotAddr value;
        value.slot = reader.u32();
        value.type = reader.u32();
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::MakeNone: {
        hir::HirMakeNone value;
        value.type = reader.u32();
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::MakeSome: {
        hir::HirMakeSome value;
        value.value = reader.u32();
        value.type  = reader.u32();
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::MakeSlice: {
        hir::HirMakeSlice value;
        value.object      = reader.u32();
        value.lo          = reader.u32();
        value.hi          = reader.u32();
        value.type        = reader.u32();
        value.object_type = reader.u32();
        value.bound_type  = reader.u32();
        value.is_array    = reader.u8() != 0;
        value.is_pointer  = reader.u8() != 0;
        const auto safety = reader.u8();
        if (safety > enumByte(hir::HirSafetyMode::Raw))
            return false;
        value.safety = static_cast<hir::HirSafetyMode>(safety);
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::Cast: {
        const uint8_t cast_kind = reader.u8();
        if (cast_kind == 0U) {
            hir::HirCast value;
            value.value = reader.u32();
            value.from  = reader.u32();
            value.to    = reader.u32();
            module.addExpr(std::move(value));
        } else if (cast_kind == 1U) {
            hir::HirUnionCast value;
            value.value       = reader.u32();
            value.from        = reader.u32();
            value.to          = reader.u32();
            value.member_index = reader.u32();
            const auto safety = reader.u8();
            if (safety > enumByte(hir::HirSafetyMode::Raw))
                return false;
            value.safety = static_cast<hir::HirSafetyMode>(safety);
            module.addExpr(std::move(value));
        } else {
            return false;
        }
        return reader.ok();
    }
    case hir::HirExprKind::UnionCheck: {
        hir::HirUnionCheck value;
        value.value       = reader.u32();
        value.union_type  = reader.u32();
        value.member_index = reader.u32();
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::LayoutIntrinsic: {
        hir::HirLayoutIntrinsic value;
        value.which        = readEnum<hir::HirLayoutIntrinsic::Which>(reader);
        value.type         = reader.u32();
        value.field_index  = reader.u32();
        value.operand      = reader.u32();
        value.operand_type = reader.u32();
        value.string_length = static_cast<uint64_t>(reader.i64());
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::StateTailCall: {
        hir::HirStateTailCall value(arena);
        value.call = hir::HirCall(reader.u32(), std::move(value.call.args),
                                  std::move(value.call.argument_types));
        if (!readDynU32(reader, value.call.args) || !readDynType(reader, value.call.argument_types))
            return false;
        value.call.fn_type     = reader.u32();
        value.call.resolved_fn = reader.u32();
        value.call.usesTailCC  = reader.u8() != 0;
        value.call.musttail    = reader.u8() != 0;
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::Cleanup: {
        hir::HirCleanup value(arena);
        if (!readDynU32(reader, value.exprs))
            return false;
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::GlobalConstLoad: {
        hir::HirGlobalConstLoad value;
        value.name = reader.u32();
        value.type = reader.u32();
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::ExternalGlobalAddr: {
        hir::HirExternalGlobalAddr value;
        value.name = reader.u32();
        value.type = reader.u32();
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::MakeDyn: {
        hir::HirMakeDyn value;
        value.value          = reader.u32();
        value.source_type    = reader.u32();
        value.dyn_type       = reader.u32();
        value.vtable_name    = reader.u32();
        value.value_is_place = reader.u8() != 0;
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::DynCall: {
        hir::HirDynCall value(arena);
        value.receiver     = reader.u32();
        value.vtable_name  = reader.u32();
        value.slot_index   = reader.u32();
        value.has_receiver = reader.u8() != 0;
        value.result_type  = reader.u32();
        value.fn_type      = reader.u32();
        if (!readDynU32(reader, value.args) || !readDynType(reader, value.arg_types))
            return false;
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::MakeOpaque: {
        hir::HirMakeOpaque value;
        value.value        = reader.u32();
        value.source_type  = reader.u32();
        value.opaque_type  = reader.u32();
        value.type_id      = reader.u32();
        value.canonical_id = {static_cast<uint64_t>(reader.i64()),
                              static_cast<uint64_t>(reader.i64())};
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::OpaqueCast: {
        hir::HirOpaqueCast value;
        value.value        = reader.u32();
        value.from         = reader.u32();
        value.to           = reader.u32();
        value.opaque_type  = reader.u32();
        value.result_type  = reader.u32();
        value.type_id      = reader.u32();
        value.canonical_id = {static_cast<uint64_t>(reader.i64()),
                              static_cast<uint64_t>(reader.i64())};
        const auto safety = reader.u8();
        if (safety > enumByte(hir::HirSafetyMode::Raw))
            return false;
        value.safety = static_cast<hir::HirSafetyMode>(safety);
        value.returns_ptr  = reader.u8() != 0;
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::OpaqueCheck: {
        hir::HirOpaqueCheck value;
        value.value        = reader.u32();
        value.opaque_type  = reader.u32();
        value.type_id      = reader.u32();
        value.canonical_id = {static_cast<uint64_t>(reader.i64()),
                              static_cast<uint64_t>(reader.i64())};
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::RuntimePanic: {
        hir::HirRuntimePanic value;
        value.code = reader.u32();
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::CanonicalType: {
        hir::HirCanonicalType value;
        value.canonical_id = {static_cast<uint64_t>(reader.i64()),
                              static_cast<uint64_t>(reader.i64())};
        value.type         = reader.u32();
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::Pipe: {
        hir::HirPipe value;
        value.source_slot = reader.u32();
        value.source_type = reader.u32();
        value.stage       = reader.u32();
        value.type        = reader.u32();
        value.is_effect   = reader.u8() != 0;
        module.addExpr(std::move(value));
        return reader.ok();
    }
    case hir::HirExprKind::PipeCurrent: {
        hir::HirPipeCurrent value;
        value.slot = reader.u32();
        value.type = reader.u32();
        module.addExpr(std::move(value));
        return reader.ok();
    }
    }
    return false;
}

auto readFunction(Reader &reader, hir::HirModule &module, memory::Arena &arena) -> bool {
    const auto name = reader.u32();
    auto &function  = module.addFn(name);

    const uint32_t paramCount = reader.u32();
    if (paramCount > kMaxArrayCount)
        return false;
    for (uint32_t i = 0; i < paramCount; ++i)
        function.params.push(reader.u32());

    const uint32_t nameCount = reader.u32();
    if (nameCount > kMaxArrayCount)
        return false;
    for (uint32_t i = 0; i < nameCount; ++i)
        function.param_names.push(reader.u32());

    const uint32_t slotCount = reader.u32();
    if (slotCount > kMaxArrayCount)
        return false;
    for (uint32_t i = 0; i < slotCount; ++i)
        function.param_slots.push(reader.u32());

    function.return_type     = reader.u32();
    function.isState         = reader.u8() != 0;
    function.usesTailCC      = reader.u8() != 0;
    function.machineReturnType = reader.u32();
    function.machineId       = reader.u32();
    function.isVariadic      = reader.u8() != 0;
    function.variadicSliceParam = reader.u32();
    function.isForeignC      = reader.u8() != 0;
    function.hasBody         = reader.u8() != 0;
    function.fnSpan.file     = reader.u32();
    function.fnSpan.start    = reader.u32();
    function.fnSpan.end      = reader.u32();

    const uint32_t blockCount = reader.u32();
    if (blockCount > kMaxArrayCount)
        return false;
    for (uint32_t b = 0; b < blockCount; ++b) {
        auto &block   = function.blocks.emplace(arena);
        block.insts   = memory::DynArray<hir::HirExprId>(arena);
        const uint32_t instCount = reader.u32();
        if (instCount > kMaxArrayCount)
            return false;
        for (uint32_t i = 0; i < instCount; ++i)
            block.insts.push(reader.u32());
        block.terminator = reader.u32();
    }
    return reader.ok();
}

} // namespace

auto encodeHir(const session::CompilationSession &session) -> EncodeHirResult {
    return encodeHir(session.hirModule(), session.interner(), session.types());
}

auto encodeHir(const hir::HirModule &module, const memory::StringInterner &interner,
               const types::TypeIntern &types) -> EncodeHirResult {
    EncodeHirResult result;

    Writer writer;
    writer.putU32(kBlobMagic);
    writer.putU32(kBlobVersion);

    writer.putU8(1); // explicit string-pool section marker

    writer.putU32(static_cast<uint32_t>(interner.poolSize()));
    for (size_t i = 0; i < interner.poolSize(); ++i)
        writer.putString(interner.lookup(static_cast<memory::InternedId>(i)));

    writer.putU8(2); // explicit type-pool section marker

    // TypeIntern always starts with the five builtin rows (Error..Char), so
    // only the custom rows need to cross the wire. Decoded positional ids
    // therefore stay aligned with the compiler-side type table.
    const size_t customStart = types::kFirstCustom;
    const size_t customCount = types.count() > customStart ? types.count() - customStart : 0;
    writer.putU32(static_cast<uint32_t>(customCount));
    for (size_t i = customStart; i < types.count(); ++i) {
        writeTypeData(writer, types.lookup(static_cast<types::TypeId>(i)), interner);
    }

    writer.putU8(3); // expression-pool section marker
    writeExprs(writer, module);
    writer.putU8(4); // function-pool section marker
    writeFunctions(writer, module);

    writer.putU8(5); // safety-proof attribute section marker
    uint32_t proofCount = 0;
    for (hir::HirExprId expr_id = 0; expr_id < module.exprCount(); ++expr_id) {
        const auto *proof = module.attrs().trySafetyProof(expr_id);
        if (proof != nullptr && proof->present)
            ++proofCount;
    }
    writer.putU32(proofCount);
    for (hir::HirExprId expr_id = 0; expr_id < module.exprCount(); ++expr_id) {
        const auto *proof = module.attrs().trySafetyProof(expr_id);
        if (proof == nullptr || !proof->present)
            continue;
        writer.putU32(expr_id);
        writer.putU8(enumByte(proof->kind));
        writer.putU32(proof->operand_a);
        writer.putU32(proof->operand_b);
        writer.putU32(proof->operand_c);
    }

    if (module.getVTableCount() > kMaxArrayCount) {
        result.message = "flat HIR vtable count overflow";
        return result;
    }
    writer.putU8(6); // module-local vtable section marker
    writer.putU32(static_cast<uint32_t>(module.getVTableCount()));
    for (size_t i = 0; i < module.getVTableCount(); ++i) {
        const auto &vtable = module.getVTable(i);
        if (vtable.slots.size() > kMaxArrayCount) {
            result.message = "flat HIR vtable slot count overflow";
            return result;
        }
        writer.putU32(vtable.name);
        writer.putU32(static_cast<uint32_t>(vtable.slots.size()));
        for (const auto slot : vtable.slots)
            writer.putU32(slot);
    }

    if (types.structDefCount() > kMaxArrayCount) {
        result.message = "flat HIR struct-definition count overflow";
        return result;
    }
    writer.putU8(7); // concrete struct-definition section marker
    writer.putU32(static_cast<uint32_t>(types.structDefCount()));
    for (uint32_t i = 0; i < types.structDefCount(); ++i) {
        const auto *definition = types.lookupStructDef(i);
        if (definition == nullptr || definition->fields.size() > kMaxArrayCount ||
            definition->args.size() > kMaxArrayCount) {
            result.message = "flat HIR struct-definition row invalid";
            return result;
        }
        writer.putU32(definition->name);
        writer.putU32(static_cast<uint32_t>(definition->fields.size()));
        for (const auto &field : definition->fields) {
            writer.putU32(field.name);
            writer.putU32(field.type);
            writer.putI64(static_cast<int64_t>(field.foreignOffsetBytes));
        }
        writer.putU32(static_cast<uint32_t>(definition->args.size()));
        for (const auto arg : definition->args)
            writer.putU32(arg);
        writer.putString(definition->defining_module);
        writer.putU8(definition->hasForeignLayout ? 1U : 0U);
        writer.putU8(definition->foreignAbiIsSingleI64 ? 1U : 0U);
        writer.putI64(static_cast<int64_t>(definition->foreignSizeBytes));
        writer.putI64(static_cast<int64_t>(definition->foreignAlignBytes));
        writer.putU32(definition->explicitAlignmentBytes);
    }

    result.blob.bytes = writer.bytes();
    result.ok         = true;
    return result;
}

auto decodeHir(std::span<const uint8_t> data, DecodedHir &result) -> bool {
    Reader reader(data);

    if (reader.u32() != kBlobMagic || reader.u32() != kBlobVersion) {
        result.message = "invalid flat HIR header";
        return false;
    }

    if (reader.u8() != 1)
        return false;
    const uint32_t stringCount = reader.u32();
    if (stringCount > kMaxArrayCount) {
        result.message = "flat HIR string count overflow";
        return false;
    }
    for (uint32_t i = 0; i < stringCount; ++i) {
        const auto text = reader.string();
        if (!reader.ok()) {
            result.message = "flat HIR string pool truncated";
            return false;
        }
        (void)result.interner.copyString(text);
    }

    if (reader.u8() != 2)
        return false;
    const uint32_t typeCount = reader.u32();
    if (typeCount > kMaxArrayCount) {
        result.message = "flat HIR type count overflow";
        return false;
    }
    for (uint32_t i = 0; i < typeCount; ++i)
        result.types.appendPositional(readTypeData(reader, result.arena, result.interner));

    if (reader.u8() != 3)
        return false;
    const uint32_t exprCount = reader.u32();
    if (exprCount > kMaxArrayCount) {
        result.message = "flat HIR expression count overflow";
        return false;
    }
    for (uint32_t i = 0; i < exprCount; ++i) {
        if (!readExpr(reader, result.module, result.arena)) {
            result.message = "flat HIR expression row invalid";
            return false;
        }
    }

    if (reader.u8() != 4)
        return false;
    const uint32_t functionCount = reader.u32();
    if (functionCount > kMaxArrayCount) {
        result.message = "flat HIR function count overflow";
        return false;
    }
    for (uint32_t i = 0; i < functionCount; ++i) {
        if (!readFunction(reader, result.module, result.arena)) {
            result.message = "flat HIR function row invalid";
            return false;
        }
    }

    if (reader.u8() != 5) {
        result.message = "flat HIR safety-proof section marker invalid";
        return false;
    }
    const uint32_t proofCount = reader.u32();
    if (proofCount > result.module.exprCount()) {
        result.message = "flat HIR safety-proof count exceeds expression count";
        return false;
    }
    std::vector<bool> proofSeen(result.module.exprCount(), false);
    for (uint32_t i = 0; i < proofCount; ++i) {
        const auto expr_id = reader.u32();
        const auto kind = reader.u8();
        if (expr_id >= result.module.exprCount() ||
            kind > enumByte(hir::HirSafetyProofKind::OpaqueTypeMatches) ||
            proofSeen[expr_id]) {
            result.message = "flat HIR safety-proof row invalid";
            return false;
        }
        proofSeen[expr_id] = true;
        auto &proof = result.module.attrs().safetyProof(expr_id);
        proof.kind = static_cast<hir::HirSafetyProofKind>(kind);
        proof.operand_a = reader.u32();
        proof.operand_b = reader.u32();
        proof.operand_c = reader.u32();
        proof.present = true;
    }

    if (reader.u8() != 6) {
        result.message = "flat HIR vtable section marker invalid";
        return false;
    }
    const uint32_t vtableCount = reader.u32();
    if (vtableCount > kMaxArrayCount) {
        result.message = "flat HIR vtable count overflow";
        return false;
    }
    for (uint32_t i = 0; i < vtableCount; ++i) {
        const auto name          = reader.u32();
        const uint32_t slotCount = reader.u32();
        if (slotCount > kMaxArrayCount) {
            result.message = "flat HIR vtable slot count overflow";
            return false;
        }
        auto &vtable = result.module.addVTable(name);
        for (uint32_t slot = 0; slot < slotCount; ++slot)
            vtable.slots.push(reader.u32());
        if (!reader.ok()) {
            result.message = "flat HIR vtable row invalid";
            return false;
        }
    }

    if (reader.u8() != 7) {
        result.message = "flat HIR struct-definition section marker invalid";
        return false;
    }
    const uint32_t structDefCount = reader.u32();
    if (structDefCount > kMaxArrayCount) {
        result.message = "flat HIR struct-definition count overflow";
        return false;
    }
    for (uint32_t i = 0; i < structDefCount; ++i) {
        const auto name = reader.u32();
        if (!reader.ok() || name >= result.interner.poolSize()) {
            result.message = "flat HIR struct-definition row invalid";
            return false;
        }
        const auto structType = result.types.defineStruct(result.interner.lookup(name));
        const auto *type      = std::get_if<types::TypeStruct>(&result.types.lookup(structType));
        if (type == nullptr || type->def_id != i) {
            result.message = "flat HIR struct-definition identity invalid";
            return false;
        }

        const uint32_t fieldCount = reader.u32();
        if (fieldCount > kMaxArrayCount) {
            result.message = "flat HIR struct field count overflow";
            return false;
        }
        for (uint32_t fieldIndex = 0; fieldIndex < fieldCount; ++fieldIndex) {
            const auto fieldName     = reader.u32();
            const auto fieldType     = reader.u32();
            const auto foreignOffset = static_cast<uint64_t>(reader.i64());
            if (!reader.ok() || fieldName >= result.interner.poolSize() ||
                fieldType >= result.types.count()) {
                result.message = "flat HIR struct field row invalid";
                return false;
            }
            result.types.addField(structType, result.interner.lookup(fieldName), fieldType,
                                  foreignOffset);
        }

        const uint32_t argCount = reader.u32();
        if (argCount > kMaxArrayCount) {
            result.message = "flat HIR struct type-argument count overflow";
            return false;
        }
        std::vector<types::TypeId> args;
        args.reserve(argCount);
        for (uint32_t argIndex = 0; argIndex < argCount; ++argIndex) {
            const auto arg = reader.u32();
            if (arg >= result.types.count()) {
                result.message = "flat HIR struct type-argument row invalid";
                return false;
            }
            args.push_back(arg);
        }

        const auto definingModule = reader.string();
        auto &definition          = result.types.getStructDef(structType);
        definition.args.clear();
        for (const auto arg : args)
            definition.args.push(arg);
        definition.defining_module =
            result.interner.lookup(result.interner.copyString(definingModule));
        definition.hasForeignLayout       = reader.u8() != 0;
        definition.foreignAbiIsSingleI64  = reader.u8() != 0;
        definition.foreignSizeBytes       = static_cast<uint64_t>(reader.i64());
        definition.foreignAlignBytes      = static_cast<uint64_t>(reader.i64());
        definition.explicitAlignmentBytes = reader.u32();
        if (!reader.ok()) {
            result.message = "flat HIR struct-definition row invalid";
            return false;
        }
    }

    if (!reader.ok()) {
        result.message = "flat HIR blob truncated";
        return false;
    }

    result.ok = true;
    return true;
}

} // namespace zith::wasm
