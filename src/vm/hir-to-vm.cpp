#include "vm/hir-to-vm.hpp"

#include "common/overloaded.hpp"
#include "types/type-canonical.hpp"
#include "types/type-kind.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace zith::vm {
namespace {

constexpr std::uint16_t kUnassignedReg = 0xFFFFU;

std::string_view sourceName(std::string_view linkage) {
    auto paren = linkage.find('(');
    if (paren != std::string_view::npos)
        linkage = linkage.substr(0, paren);
    auto dot = linkage.rfind('.');
    if (dot != std::string_view::npos)
        linkage = linkage.substr(dot + 1);
    return linkage;
}

bool startsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

auto isUserFunction(std::string_view linkage, const hir::HirFunction &hirFn) -> bool {
    // A hydrated function with a body is user code too, so classify it from
    // HIR and linkage data rather than frontend declaration metadata.
    if (!hirFn.hasBody)
        return false;
    const std::string_view name = sourceName(linkage);
    if (name == "main")
        return true;
    return !startsWith(linkage, "std.") && !startsWith(linkage, "zith.");
}

auto u16(std::size_t value, LowerResult *result) -> std::uint16_t {
    if (value > 0xFFFFU) {
        result->ok      = false;
        result->message = "v2 module table exceeds 65535 entries";
        return 0;
    }
    return static_cast<std::uint16_t>(value);
}

auto findOrAddString(Module &out, std::string_view text, LowerResult *result) -> std::uint16_t {
    for (std::size_t i = 0; i < out.strings.size(); ++i)
        if (out.strings[i] == text)
            return u16(i, result);
    out.strings.push(text);
    return u16(out.strings.size() - 1, result);
}

auto findOrAddExtern(Module &out, std::string_view name, LowerResult *result) -> std::uint16_t {
    for (std::size_t i = 0; i < out.externs.size(); ++i)
        if (out.externs[i] == name)
            return u16(i, result);
    out.externs.push(name);
    return u16(out.externs.size() - 1, result);
}

struct LowerState {
    memory::Arena &arena;
    const hir::HirModule &hir;
    const memory::StringInterner &interner;
    const types::TypeIntern &types;
    Module &out;
    const std::vector<std::uint16_t> &functionIndices;
    const std::vector<std::uint16_t> &adapterIndices;

    const hir::HirFunction *hirFn = nullptr;
    Function *fn                  = nullptr;
    memory::DynArray<std::uint16_t> regs;
    memory::DynArray<std::uint16_t> paramRegs;
    memory::DynArray<std::uint16_t> slotRegs;
    memory::DynArray<std::uint8_t> slotWidths;
    /// HIR type of every slot, needed to lay out aggregate slots in memory.
    memory::DynArray<types::TypeId> slotTypes;
    /// Alignment requested by each function-local slot allocation.
    memory::DynArray<std::uint32_t> slotAlignments;
    /// 1 when the slot owns linear-memory storage (scalars, aggregates, or an
    /// explicitly aligned register-backed value), otherwise 0.
    memory::DynArray<std::uint8_t> slotBacked;
    /// Hidden return-slot pointer for functions that return an aggregate by
    /// value. Register 0 of such a function; kUnassignedReg otherwise.
    std::uint16_t sretReg = kUnassignedReg;
    LowerResult *result   = nullptr;
};

void emit(LowerState &state, Instr instr) {
    state.fn->body.push(instr);
}

/// Aggregate values live by address in linear memory, so their register
/// representation is a guest pointer. This is the single place that decides
/// which HIR types are address-backed, and the width helpers below must agree
/// with it.
auto isAggregateKind(types::TypeKind kind) -> bool {
    switch (kind) {
    case types::TypeKind::Struct:
    case types::TypeKind::Pack:
    case types::TypeKind::Union:
    case types::TypeKind::Optional:
    case types::TypeKind::Dyn:
    case types::TypeKind::OpaqueTagged:
        return true;
    default:
        return false;
    }
}

auto isNicheOptional(const types::TypeIntern &types, types::TypeId type) -> bool {
    if (type == types::kInvalidType || types.kindOf(type) != types::TypeKind::Optional)
        return false;
    const auto *optional = std::get_if<types::TypeOptional>(&types.lookup(type));
    return optional != nullptr && types.kindOf(optional->inner) == types::TypeKind::Ptr;
}

auto isAggregateType(const types::TypeIntern &types, types::TypeId type) -> bool {
    if (type == types::kInvalidType)
        return false;
    if (isNicheOptional(types, type))
        return false;
    return isAggregateKind(types.kindOf(type));
}

/// Scalar values are stored in linear memory like aggregates so that taking
/// their address (`&x`) yields real storage and a write through the resulting
/// pointer is visible to later reads of `x`. Slices keep a two-register
/// {ptr, len} value and arrays keep their base pointer in the slot register, so
/// neither is memory-backed here.
auto isScalarSlotType(const types::TypeIntern &types, types::TypeId type) -> bool {
    if (type == types::kInvalidType)
        return false;
    switch (types.kindOf(type)) {
    case types::TypeKind::Int:
    case types::TypeKind::Bool:
    case types::TypeKind::Char:
    case types::TypeKind::Float:
    case types::TypeKind::Ptr:
    case types::TypeKind::String:
    case types::TypeKind::Enum:
        return true;
    default:
        return isNicheOptional(types, type);
    }
}

/// Slot types that always require memory-backed storage. Explicitly aligned
/// local arrays are backed separately from this type classification.
auto isMemorySlotType(const types::TypeIntern &types, types::TypeId type) -> bool {
    return isScalarSlotType(types, type) || isAggregateType(types, type);
}

/// Value kinds whose register representation is already a guest pointer to
/// their storage, so loading one through a pointer yields the pointer itself
/// rather than reading a scalar out of it. Aggregates qualify, and so does an
/// array, whose register holds its element base pointer.
auto isPointerValued(const types::TypeIntern &types, types::TypeId type) -> bool {
    return isAggregateType(types, type) ||
           (type != types::kInvalidType && types.kindOf(type) == types::TypeKind::Array);
}

auto valueTypeOf(const types::TypeIntern &types, types::TypeId type) -> ValueType {
    switch (types.kindOf(type)) {
    case types::TypeKind::Int:
    case types::TypeKind::Bool:
    case types::TypeKind::Char:
        return ValueType::I64;
    case types::TypeKind::Float:
        return ValueType::F64;
    case types::TypeKind::Ptr:
    case types::TypeKind::String:
        return ValueType::Ptr;
    case types::TypeKind::Array:
        return ValueType::Ptr;
    case types::TypeKind::Slice:
        return ValueType::Slice;
    case types::TypeKind::Struct:
    case types::TypeKind::Pack:
    case types::TypeKind::Union:
    case types::TypeKind::Optional:
    case types::TypeKind::Dyn:
    case types::TypeKind::OpaqueTagged:
        return ValueType::Ptr;
    case types::TypeKind::Void:
        return ValueType::Void;
    default:
        return ValueType::Void;
    }
}

auto alignUp(std::size_t value, std::size_t alignment) -> std::size_t {
    if (alignment <= 1U)
        return value;
    return (value + alignment - 1U) / alignment * alignment;
}

/// Byte size and alignment of a type as laid out in linear memory. Aggregates
/// are pointer-sized slots; structs and packs are a sequence of aligned field
/// slots; optionals are a payload slot followed by a 1-byte presence flag.
auto layoutSize(const types::TypeIntern &types, types::TypeId type, std::size_t &align)
    -> std::size_t {
    switch (types.kindOf(type)) {
    case types::TypeKind::Int:
    case types::TypeKind::Bool:
    case types::TypeKind::Char:
    case types::TypeKind::Float:
    case types::TypeKind::Ptr:
    case types::TypeKind::String:
    case types::TypeKind::Enum:
        align = 8U;
        return 8U;
    case types::TypeKind::Array: {
        const auto *array = std::get_if<types::TypeArray>(&types.lookup(type));
        if (array == nullptr)
            return 0U;
        std::size_t element_align = 1U;
        const auto element_size =
            types.kindOf(array->elem) == types::TypeKind::Char
                ? 1U
                : layoutSize(types, array->elem, element_align);
        if (types.kindOf(array->elem) == types::TypeKind::Char)
            element_align = 1U;
        if (element_size == 0U)
            return 0U;
        align = element_align > 1U ? element_align : 1U;
        return array->count * element_size;
    }
    case types::TypeKind::Slice:
        align = 8U;
        return 16U;
    case types::TypeKind::Struct:
    case types::TypeKind::Pack: {
        align            = 8U;
        std::size_t size = 0;
        const auto count = types.memberCount(type);
        for (std::size_t i = 0; i < count; ++i) {
            std::size_t field_align = 1U;
            const auto field_size   = layoutSize(types, types.memberAt(type, i).type, field_align);
            if (field_size == 0U)
                return 0U;
            size = alignUp(size, field_align) + field_size;
            if (field_align > align)
                align = field_align;
        }
        align = std::max(align, static_cast<std::size_t>(types::typeAlignBytes(types, type)));
        // Empty aggregates still need a distinct, non-null address when stored
        // in a VM slot even though they contain no fields.
        return size == 0U ? 1U : alignUp(size, align);
    }
    case types::TypeKind::Optional: {
        const auto *optional = std::get_if<types::TypeOptional>(&types.lookup(type));
        if (optional == nullptr)
            return 0U;
        // Optional pointers use the null sentinel and carry no flag slot.
        if (types.kindOf(optional->inner) == types::TypeKind::Ptr) {
            align = 8U;
            return 8U;
        }
        std::size_t inner_align = 1U;
        const auto inner_size   = layoutSize(types, optional->inner, inner_align);
        if (inner_size == 0U)
            return 0U;
        align = inner_align > 1U ? inner_align : 1U;
        return alignUp(inner_size, 1U) + 1U;
    }
    case types::TypeKind::Dyn:
    case types::TypeKind::OpaqueTagged:
        align = 8U;
        return 16U;
    case types::TypeKind::Union: {
        const auto *union_type = std::get_if<types::TypeUnion>(&types.lookup(type));
        const auto *def =
            union_type != nullptr ? types.lookupUnionDef(union_type->def_id) : nullptr;
        if (def == nullptr)
            return 0U;
        align            = 8U;
        std::size_t size = 0;
        for (const auto member : def->members) {
            std::size_t member_align = 1U;
            const auto member_size   = layoutSize(types, member, member_align);
            if (member_size == 0U)
                return 0U;
            if (member_size > size)
                size = member_size;
            if (member_align > align)
                align = member_align;
        }
        size = alignUp(size, align);
        if (def->is_tagged) {
            const auto tag_width =
                def->members.size() > 0xFFFFU ? 4U : (def->members.size() > 0xFFU ? 2U : 1U);
            size = alignUp(size, tag_width) + tag_width;
        }
        return size;
    }
    default:
        return 0U;
    }
}

auto typeSize(const types::TypeIntern &types, types::TypeId type) -> std::size_t {
    std::size_t align = 1U;
    return layoutSize(types, type, align);
}

/// Byte offset of field `index` inside a struct/pack laid out by `layoutSize`.
auto fieldByteOffset(const types::TypeIntern &types, types::TypeId struct_type, std::uint32_t index)
    -> std::size_t {
    if (types.kindOf(struct_type) == types::TypeKind::Optional) {
        const auto *optional = std::get_if<types::TypeOptional>(&types.lookup(struct_type));
        if (optional == nullptr)
            return 0U;
        if (index == 0U)
            return 0U;
        if (index == 1U) {
            std::size_t inner_align = 1U;
            return layoutSize(types, optional->inner, inner_align);
        }
        return 0U;
    }

    std::size_t offset = 0;
    const auto count   = types.memberCount(struct_type);
    for (std::size_t i = 0; i < count && i <= index; ++i) {
        std::size_t field_align = 1U;
        const auto field_size = layoutSize(types, types.memberAt(struct_type, i).type, field_align);
        if (field_size == 0U)
            return 0U;
        offset = alignUp(offset, field_align);
        if (i == index)
            return offset;
        offset += field_size;
    }
    return 0U;
}

auto newTypedReg(LowerState &state, ValueType vt) -> std::uint16_t {
    const auto reg = static_cast<std::uint16_t>(state.fn->regCount++);
    state.fn->regTypes.push(vt);
    return reg;
}

auto newReg(LowerState &state, types::TypeId type) -> std::uint16_t {
    return newTypedReg(state, valueTypeOf(state.types, type));
}

struct UnionStorageRef {
    std::uint16_t address = kUnassignedReg;
    types::TypeId type    = types::kInvalidType;
};

auto taggedUnionTagWidth(const types::TypeIntern &types, types::TypeId type) -> std::size_t {
    if (types.kindOf(type) != types::TypeKind::Union)
        return 0U;
    const auto *union_type = std::get_if<types::TypeUnion>(&types.lookup(type));
    const auto *def = union_type != nullptr ? types.lookupUnionDef(union_type->def_id) : nullptr;
    if (def == nullptr || !def->is_tagged || def->members.empty())
        return 0U;
    return def->members.size() > 0xFFFFU ? 4U : (def->members.size() > 0xFFU ? 2U : 1U);
}

/// VM byte offset of the member-index tag, without native ABI tail padding.
auto taggedUnionTagOffset(const types::TypeIntern &types, types::TypeId type) -> std::size_t {
    if (taggedUnionTagWidth(types, type) == 0U)
        return 0U;
    const auto *union_type = std::get_if<types::TypeUnion>(&types.lookup(type));
    const auto *def = union_type != nullptr ? types.lookupUnionDef(union_type->def_id) : nullptr;
    if (def == nullptr)
        return 0U;

    std::size_t payload_size  = 0U;
    std::size_t payload_align = 8U;
    for (const auto member : def->members) {
        std::size_t member_align = 1U;
        const auto member_size   = layoutSize(types, member, member_align);
        if (member_size == 0U)
            return 0U;
        if (member_size > payload_size)
            payload_size = member_size;
        if (member_align > payload_align)
            payload_align = member_align;
    }
    return alignUp(payload_size, payload_align);
}

auto emitI64Constant(LowerState &state, std::int64_t value) -> std::uint16_t {
    const auto reg = newTypedReg(state, ValueType::I64);
    if (value >= 0 && static_cast<std::uint64_t>(value) <= 0xFFFFU) {
        emit(state, Instr::withImm(Op::LoadConstI64, reg, static_cast<std::uint16_t>(value)));
        return reg;
    }
    const auto index = u16(state.out.i64Constants.size(), state.result);
    if (!state.result->ok)
        return kUnassignedReg;
    state.out.i64Constants.push(value);
    emit(state, Instr::withImm(Op::LoadConstI64FromPool, reg, index));
    return reg;
}

auto unionTagByteAddress(LowerState &state, UnionStorageRef storage, std::size_t byte_index)
    -> std::uint16_t {
    const auto offset = taggedUnionTagOffset(state.types, storage.type);
    const auto width  = taggedUnionTagWidth(state.types, storage.type);
    if (width == 0U || byte_index >= width || offset > 0xFFFFU - byte_index) {
        state.result->ok      = false;
        state.result->message = "invalid tagged-union layout in v2 lowering";
        return kUnassignedReg;
    }
    const auto address = newTypedReg(state, ValueType::Ptr);
    emit(state, Instr{Op::FieldPtr, address, storage.address, 0,
                      static_cast<std::uint16_t>(offset + byte_index)});
    return address;
}

auto loadUnionTag(LowerState &state, UnionStorageRef storage) -> std::uint16_t {
    const auto width = taggedUnionTagWidth(state.types, storage.type);
    if (width == 0U) {
        state.result->ok      = false;
        state.result->message = "invalid tagged-union tag in v2 lowering";
        return kUnassignedReg;
    }

    std::uint16_t value = kUnassignedReg;
    for (std::size_t i = 0; i < width; ++i) {
        const auto address = unionTagByteAddress(state, storage, i);
        if (address == kUnassignedReg)
            return kUnassignedReg;
        const auto byte = newTypedReg(state, ValueType::I64);
        emit(state, Instr::simple(Op::LoadI8, byte, address, 0));
        if (i == 0U) {
            value = byte;
            continue;
        }

        const auto shift = emitI64Constant(state, static_cast<std::int64_t>(i * 8U));
        if (shift == kUnassignedReg)
            return kUnassignedReg;
        const auto shifted = newTypedReg(state, ValueType::I64);
        emit(state, Instr::simple(Op::Shl, shifted, byte, shift));
        const auto combined = newTypedReg(state, ValueType::I64);
        emit(state, Instr::simple(Op::BitOr, combined, value, shifted));
        value = combined;
    }
    return value;
}

auto storeUnionTag(LowerState &state, UnionStorageRef storage, std::uint32_t member_index) -> bool {
    const auto width = taggedUnionTagWidth(state.types, storage.type);
    if (width == 0U) {
        state.result->ok      = false;
        state.result->message = "invalid tagged-union tag in v2 lowering";
        return false;
    }
    const auto value = emitI64Constant(state, static_cast<std::int64_t>(member_index));
    if (value == kUnassignedReg)
        return false;

    for (std::size_t i = 0; i < width; ++i) {
        const auto address = unionTagByteAddress(state, storage, i);
        if (address == kUnassignedReg)
            return false;
        auto byte = value;
        if (i != 0U) {
            const auto shift = emitI64Constant(state, static_cast<std::int64_t>(i * 8U));
            if (shift == kUnassignedReg)
                return false;
            byte = newTypedReg(state, ValueType::I64);
            emit(state, Instr::simple(Op::Shr, byte, value, shift));
        }
        emit(state, Instr::simple(Op::StoreI8, address, byte, 0));
    }
    return true;
}

auto emitCheckedUnionCast(LowerState &state, UnionStorageRef storage, const hir::HirUnionCast &cast)
    -> bool {
    const auto tag = loadUnionTag(state, storage);
    if (tag == kUnassignedReg)
        return false;
    const auto expected = emitI64Constant(state, static_cast<std::int64_t>(cast.member_index));
    if (expected == kUnassignedReg)
        return false;
    const auto matches = newTypedReg(state, ValueType::I64);
    emit(state, Instr::simple(Op::Eq, matches, tag, expected));

    const auto branch_index = state.fn->body.size();
    if (branch_index > std::numeric_limits<std::uint16_t>::max() - 2U) {
        state.result->ok      = false;
        state.result->message = "union cast branch exceeds the v2 instruction width";
        return false;
    }
    const auto trap_index     = static_cast<std::uint16_t>(branch_index + 1U);
    const auto continue_index = static_cast<std::uint16_t>(branch_index + 2U);
    emit(state, Instr{Op::Branch2, 0, matches, 0, continue_index, trap_index});
    emit(state, Instr::trap());
    return true;
}

auto lowerOperand(LowerState &state, hir::HirExprId id) -> std::uint16_t;

auto valueRegisterWidth(const types::TypeIntern &types, types::TypeId type) -> std::size_t {
    return types.kindOf(type) == types::TypeKind::Slice ? 2U : 1U;
}

auto slotRegister(const LowerState &state, hir::HirSlotId slot) -> std::uint16_t {
    if (slot >= state.slotRegs.size())
        return kUnassignedReg;
    return state.slotRegs[slot];
}

auto lowerSlotValue(LowerState &state, hir::HirSlotId slot, types::TypeId type)
    -> std::uint16_t;

/// Guest address of storage backing an aggregate- or array-valued operand.
auto aggregateAddress(LowerState &state, hir::HirExprId id) -> std::uint16_t;

auto scalarElementSize(const types::TypeIntern &types, types::TypeId type) -> std::size_t {
    switch (types.kindOf(type)) {
    case types::TypeKind::Char:
        return 1U;
    case types::TypeKind::Int:
    case types::TypeKind::Bool:
    case types::TypeKind::Ptr:
    case types::TypeKind::String:
    case types::TypeKind::Enum:
        return sizeof(std::int64_t);
    default:
        return isNicheOptional(types, type) ? sizeof(std::int64_t) : 0U;
    }
}

auto arrayElementSize(const types::TypeIntern &types, types::TypeId type) -> std::size_t {
    const auto *array = std::get_if<types::TypeArray>(&types.lookup(type));
    if (array == nullptr)
        return 0U;
    return isPointerValued(types, array->elem) ? typeSize(types, array->elem)
                                               : scalarElementSize(types, array->elem);
}

auto copyBytes(LowerState &state, std::uint16_t dst, std::uint16_t src, std::size_t size) -> void;

auto lowerArrayLiteral(LowerState &state, const hir::HirArrayLiteral &literal) -> std::uint16_t {
    const auto *array      = std::get_if<types::TypeArray>(&state.types.lookup(literal.type));
    const auto elementSize = arrayElementSize(state.types, literal.type);
    if (array == nullptr || elementSize == 0 || literal.elements.size() != array->count ||
        array->count > std::numeric_limits<std::size_t>::max() / elementSize ||
        array->count * elementSize > 0xFFFFU) {
        return kUnassignedReg;
    }

    const auto count = newTypedReg(state, ValueType::I64);
    const auto align = newTypedReg(state, ValueType::I64);
    emit(state, Instr::withImm(Op::LoadConstI64, count,
                               static_cast<std::uint16_t>(array->count * elementSize)));
    emit(state, Instr::withImm(Op::LoadConstI64, align, 8));
    const auto base = newTypedReg(state, ValueType::Ptr);
    emit(state, Instr::simple(Op::AllocBytes, base, count, align));

    for (std::size_t i = 0; i < literal.elements.size(); ++i) {
        const auto address = newTypedReg(state, ValueType::Ptr);
        emit(state,
             Instr{Op::FieldPtr, address, base, 0, static_cast<std::uint16_t>(i * elementSize)});
        if (isPointerValued(state.types, array->elem)) {
            const auto source = aggregateAddress(state, literal.elements[i]);
            if (source == kUnassignedReg)
                return kUnassignedReg;
            copyBytes(state, address, source, elementSize);
        } else {
            const auto value = lowerOperand(state, literal.elements[i]);
            if (value == kUnassignedReg)
                return kUnassignedReg;
            emit(state, Instr::simple(state.types.kindOf(array->elem) == types::TypeKind::Char
                                          ? Op::StoreI8
                                          : Op::StoreI64,
                                      address, value));
        }
    }
    return base;
}

auto lowerLiteral(LowerState &state, const hir::HirLiteral &lit) -> std::uint16_t {
    const auto reg = newReg(state, lit.type);
    switch (state.types.kindOf(lit.type)) {
    case types::TypeKind::Bool:
    case types::TypeKind::Char:
    case types::TypeKind::Int:
        emit(state, Instr::withImm(Op::LoadConstI32, reg, static_cast<std::uint16_t>(lit.i)));
        break;
    case types::TypeKind::Float: {
        const auto rawIndex = state.out.f64Constants.size();
        const auto index    = u16(rawIndex, state.result);
        if (index == 0 && rawIndex != 0)
            return reg;
        state.out.f64Constants.push(lit.f);
        emit(state, Instr::withImm(Op::LoadConstF64, reg, index));
        break;
    }
    case types::TypeKind::Ptr:
    case types::TypeKind::String: {
        const auto index =
            findOrAddString(state.out, state.interner.lookup(lit.str_val), state.result);
        emit(state, Instr::withImm(Op::LoadString, reg, index));
        break;
    }
    default:
        state.result->ok      = false;
        state.result->message = "unsupported literal type in v2 lowering";
        break;
    }
    return reg;
}

auto lowerStringSlicePointer(LowerState &state, const hir::HirMakeSlice &slice) -> std::uint16_t {
    const auto &objExpr = state.hir.getExpr(slice.object);
    const auto *lit     = std::get_if<hir::HirLiteral>(&objExpr);
    if (lit != nullptr) {
        const auto text = state.interner.lookup(lit->str_val);
        if (!text.empty()) {
            const auto kind = state.types.kindOf(lit->type);
            if (slice.is_pointer || kind == types::TypeKind::Ptr ||
                kind == types::TypeKind::String) {
                const auto index = findOrAddString(state.out, text, state.result);
                const auto reg   = newReg(state, lit->type);
                emit(state, Instr::withImm(Op::LoadString, reg, index));
                return reg;
            }
        }
    }

    if (slice.is_pointer)
        return lowerOperand(state, slice.object);

    return kUnassignedReg;
}

auto lowerStringSlicePointerAndLength(LowerState &state, const hir::HirMakeSlice &slice,
                                      std::uint16_t &outLength) -> std::uint16_t {
    const auto &objExpr = state.hir.getExpr(slice.object);
    const auto *lit     = std::get_if<hir::HirLiteral>(&objExpr);
    if (lit != nullptr) {
        const auto text = state.interner.lookup(lit->str_val);
        if (!text.empty()) {
            const auto kind = state.types.kindOf(lit->type);
            if (slice.is_pointer || kind == types::TypeKind::Ptr ||
                kind == types::TypeKind::String) {
                const auto index = findOrAddString(state.out, text, state.result);
                const auto reg   = newReg(state, lit->type);
                emit(state, Instr::withImm(Op::LoadString, reg, index));

                const auto lenReg = newTypedReg(state, ValueType::I64);
                emit(state, Instr::withImm(Op::LoadConstI64, lenReg,
                                           static_cast<std::uint16_t>(text.size())));
                outLength = lenReg;
                return reg;
            }
        }
    }
    outLength = kUnassignedReg;
    return kUnassignedReg;
}

auto makeSlicePair(LowerState &state, std::uint16_t pointer, std::uint16_t length)
    -> std::uint16_t {
    const auto base = newTypedReg(state, ValueType::Ptr);
    (void)newTypedReg(state, ValueType::I64);
    emit(state, Instr::simple(Op::MakeSlice, base, pointer, length));
    return base;
}

auto lowerMakeSlice(LowerState &state, const hir::HirMakeSlice &slice) -> std::uint16_t {
    std::uint16_t literalLength = kUnassignedReg;
    const auto literalPointer   = lowerStringSlicePointerAndLength(state, slice, literalLength);
    if (literalPointer != kUnassignedReg && literalLength != kUnassignedReg)
        return makeSlicePair(state, literalPointer, literalLength);

    const auto pointer = lowerOperand(state, slice.object);
    const auto lo      = lowerOperand(state, slice.lo);
    const auto hi      = lowerOperand(state, slice.hi);
    if (pointer == kUnassignedReg || lo == kUnassignedReg || hi == kUnassignedReg)
        return kUnassignedReg;

    const auto *sliceType = std::get_if<types::TypeSlice>(&state.types.lookup(slice.type));
    const auto elementSize =
        sliceType == nullptr
            ? 0U
            : (state.types.kindOf(sliceType->elem) == types::TypeKind::Char
                   ? scalarElementSize(state.types, sliceType->elem)
                   : typeSize(state.types, sliceType->elem));
    if (sliceType == nullptr || elementSize == 0U || elementSize > 0xFFFFU) {
        state.result->ok      = false;
        state.result->message = "unsupported slice element type in v2 lowering";
        return kUnassignedReg;
    }

    std::uint16_t dataPointer = pointer;
    const auto &loExpr        = state.hir.getExpr(slice.lo);
    const auto *loLiteral     = std::get_if<hir::HirLiteral>(&loExpr);
    if (loLiteral == nullptr || loLiteral->i != 0) {
        const auto scale = newTypedReg(state, ValueType::I64);
        emit(state,
             Instr::withImm(Op::LoadConstI64, scale, static_cast<std::uint16_t>(elementSize)));
        const auto byteOffset = newTypedReg(state, ValueType::I64);
        emit(state, Instr::simple(Op::Mul, byteOffset, lo, scale));
        dataPointer = newTypedReg(state, ValueType::Ptr);
        emit(state, Instr::simple(Op::Add, dataPointer, pointer, byteOffset));
    }

    const auto length = newTypedReg(state, ValueType::I64);
    emit(state, Instr::simple(Op::Sub, length, hi, lo));
    return makeSlicePair(state, dataPointer, length);
}

/// Counts the format placeholders in a literal message slice. A `#` followed
/// by the unit-separator byte is the escape for a literal `#`, so it does not
/// consume a value. This mirrors the escape handling in the stdlib
/// `std/io/console` `writeBuffer`.
auto slicePlaceholderCount(const LowerState &state, const hir::HirMakeSlice &slice) -> std::size_t {
    const auto &objExpr = state.hir.getExpr(slice.object);
    const auto *lit     = std::get_if<hir::HirLiteral>(&objExpr);
    if (lit == nullptr)
        return 0;
    const auto text   = state.interner.lookup(lit->str_val);
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '#')
            continue;
        if (i + 1 < text.size() && text[i + 1] == '\x1f') {
            ++i;
            continue;
        }
        ++count;
    }
    return count;
}

/// Index of the `[]char` message parameter in a console `print`/`println`
/// signature. The canonical stdlib declares `println(allocator, msg, values...)`,
/// so the message is not the first argument. Locating it by parameter type keeps
/// the fast path working for both the canonical signature and the older
/// `println(msg, values...)` shape. Returns `call.args.size()` when the
/// signature has no message parameter.
auto consoleMessageArg(const LowerState &state, const hir::HirFunction &fn,
                       const hir::HirCall &call) -> std::size_t {
    for (std::size_t i = 0; i < fn.params.size() && i < call.args.size(); ++i) {
        if (state.types.kindOf(fn.params[i]) != types::TypeKind::Slice)
            continue;
        const auto *paramSlice = std::get_if<types::TypeSlice>(&state.types.lookup(fn.params[i]));
        if (paramSlice != nullptr && state.types.kindOf(paramSlice->elem) == types::TypeKind::Char)
            return i;
    }
    return call.args.size();
}

auto materializeBorrowedSlice(LowerState &state, hir::HirExprId argument,
                              types::TypeId argumentType, bool &handled) -> std::uint16_t {
    handled = false;
    if (argumentType == types::kInvalidType)
        return kUnassignedReg;
    const auto *pointerType = std::get_if<types::TypePtr>(&state.types.lookup(argumentType));
    if (pointerType == nullptr ||
        state.types.kindOf(pointerType->pointee) != types::TypeKind::Slice ||
        argument == hir::kInvalidHirExpr || argument >= state.regs.size())
        return kUnassignedReg;

    const auto &argumentExpr = state.hir.getExpr(argument);
    hir::HirSlotId slot      = hir::kInvalidHirSlot;
    if (const auto *address = std::get_if<hir::HirSlotAddr>(&argumentExpr))
        slot = address->slot;
    else if (const auto *load = std::get_if<hir::HirSlotLoad>(&argumentExpr))
        slot = load->slot;
    if (slot == hir::kInvalidHirSlot || slot >= state.slotTypes.size() ||
        state.types.kindOf(state.slotTypes[slot]) != types::TypeKind::Slice)
        return kUnassignedReg;

    handled           = true;
    const auto source = lowerSlotValue(state, slot, state.slotTypes[slot]);
    if (source == kUnassignedReg || static_cast<std::size_t>(source) + 1U >= state.fn->regCount) {
        state.result->ok      = false;
        state.result->message = "invalid slice slot in borrowed v2 call argument";
        return kUnassignedReg;
    }

    const auto descriptor = newTypedReg(state, ValueType::Ptr);
    const auto size       = newTypedReg(state, ValueType::I64);
    const auto alignment  = newTypedReg(state, ValueType::I64);
    emit(state, Instr::withImm(Op::LoadConstI64, size, 16U));
    emit(state, Instr::withImm(Op::LoadConstI64, alignment, 8U));
    emit(state, Instr::simple(Op::AllocBytes, descriptor, size, alignment));
    emit(state, Instr::simple(Op::StoreI64, descriptor, source));

    const auto lengthAddress = newTypedReg(state, ValueType::Ptr);
    emit(state, Instr{Op::FieldPtr, lengthAddress, descriptor, 0, 8U});
    emit(state,
         Instr::simple(Op::StoreI64, lengthAddress, static_cast<std::uint16_t>(source + 1U)));
    return descriptor;
}

auto lowerCall(LowerState &state, const hir::HirCall &call, bool tailTransfer = false)
    -> std::uint16_t {
    if (call.resolved_fn == hir::kInvalidHirFunction) {
        state.result->ok      = false;
        state.result->message = "unsupported unresolved call target in v2 lowering";
        return kUnassignedReg;
    }
    if (tailTransfer && !call.musttail) {
        state.result->ok      = false;
        state.result->message = "state tail transfer is missing musttail metadata in v2 lowering";
        return kUnassignedReg;
    }

    std::string_view unsupported_linkage;
    for (std::size_t f = 0; f < state.hir.getFnCount(); ++f) {
        const auto &hirFn = state.hir.getFn(f);
        if (static_cast<hir::HirFunctionId>(f) != call.resolved_fn)
            continue;
        const auto linkage    = state.interner.lookup(hirFn.name);
        unsupported_linkage  = linkage;
        const auto name       = sourceName(linkage);
        const bool is_println = startsWith(linkage, "std.io.console.println");
        const bool is_print   = startsWith(linkage, "std.io.console.print");
        if (is_println || is_print) {
            if (tailTransfer) {
                state.result->ok      = false;
                state.result->message = "state tail transfer does not target a VM function";
                return kUnassignedReg;
            }
            if (call.args.empty())
                return newReg(state, types::kVoidType);
            const auto messageIndex = consoleMessageArg(state, hirFn, call);
            if (messageIndex >= call.args.size())
                return newReg(state, types::kVoidType);
            const auto &argExpr = state.hir.getExpr(call.args[messageIndex]);
            const auto *slice   = std::get_if<hir::HirMakeSlice>(&argExpr);
            // The fast path below forwards the literal message to `puts` or
            // `write_stdout`. It cannot render the variadic `Formatable`
            // values, so a call whose message has placeholders and carries
            // values must report unsupported instead of silently dropping the
            // formatted output. A message without placeholders ignores the
            // extra values, exactly like the native stdlib path.
            if (call.args.size() > messageIndex + 1 && slice != nullptr &&
                slicePlaceholderCount(state, *slice) > 0) {
                state.result->ok = false;
                state.result->message =
                    "variadic print formatting is not in the v2 lowering subset";
                return kUnassignedReg;
            }
            if (slice != nullptr) {
                const auto ptr = lowerStringSlicePointer(state, *slice);
                if (ptr != kUnassignedReg) {
                    while (state.fn->regTypes.size() < 5)
                        state.fn->regTypes.push(ValueType::I64);
                    if (is_println) {
                        const auto externIndex = findOrAddExtern(state.out, "puts", state.result);
                        const auto scratch     = newReg(state, types::kVoidType);
                        emit(state,
                             Instr::callExtern(Op::CallExtern, scratch, ptr, 0, externIndex));
                        return scratch;
                    }
                    std::uint16_t len = kUnassignedReg;
                    (void)lowerStringSlicePointerAndLength(state, *slice, len);
                    if (len != kUnassignedReg) {
                        const auto externIndex =
                            findOrAddExtern(state.out, "write_stdout", state.result);
                        const auto scratch = newReg(state, types::kVoidType);
                        emit(state,
                             Instr::callExtern(Op::CallExtern, scratch, ptr, len, externIndex));
                        return scratch;
                    }
                }
            }
            continue;
        }
        if (name == "main" || !hirFn.hasBody) {
            if (tailTransfer) {
                state.result->ok      = false;
                state.result->message = "state tail transfer does not target a VM function";
                return kUnassignedReg;
            }
            if (name == "puts" || name == "putchar" || name == "malloc" ||
                name == "aligned_alloc" || name == "free" || name == "realloc" ||
                name == "printf" || name == "snprintf" || name == "strlen" ||
                name == "memcpy" || name == "calloc" || name == "strncmp" || name == "strcpy" ||
                name == "strcat" || name == "strcmp" || name == "memcmp" || name == "getchar") {
                if ((name == "printf" || name == "snprintf") && hirFn.isVariadic) {
                    const std::size_t fixedCount = name == "printf" ? 1U : 3U;
                    if (call.args.size() < fixedCount) {
                        state.result->ok      = false;
                        state.result->message = "variadic call has too few fixed arguments";
                        return kUnassignedReg;
                    }
                    const auto argBase  = u16(state.fn->regTypes.size(), state.result);
                    const auto argCount = u16(call.args.size(), state.result);
                    if (!state.result->ok)
                        return kUnassignedReg;
                    for (std::size_t i = 0; i < call.args.size(); ++i)
                        state.fn->regTypes.push(ValueType::I64);
                    state.fn->regCount = static_cast<std::uint16_t>(state.fn->regTypes.size());
                    for (std::size_t i = 0; i < call.args.size(); ++i) {
                        const auto value = lowerOperand(state, call.args[i]);
                        if (value == kUnassignedReg)
                            return kUnassignedReg;
                        emit(state, Instr::simple(Op::Move, static_cast<std::uint16_t>(argBase + i),
                                                  value));
                    }
                    const auto externIndex = findOrAddExtern(state.out, name, state.result);
                    const auto resultReg   = newReg(state, call.fn_type);
                    emit(state, Instr::callExternRange(resultReg, argBase, argCount, externIndex));
                    return resultReg;
                }
                if (call.args.size() > 4)
                    return kUnassignedReg;
                const std::uint16_t arg0 =
                    call.args.size() > 0 ? lowerOperand(state, call.args[0]) : 0;
                const std::uint16_t arg1 =
                    call.args.size() > 1 ? lowerOperand(state, call.args[1]) : 0;
                const std::uint16_t arg2 =
                    call.args.size() > 2 ? lowerOperand(state, call.args[2]) : 0;
                const std::uint16_t arg3 =
                    call.args.size() > 3 ? lowerOperand(state, call.args[3]) : 0;
                while (state.fn->regTypes.size() < 5)
                    state.fn->regTypes.push(ValueType::I64);
                const auto externIndex = findOrAddExtern(state.out, name, state.result);
                const auto reg         = newReg(state, call.fn_type);
                emit(state,
                     Instr::callExtern(Op::CallExtern, reg, arg0, arg1, externIndex, arg2, arg3));
                return reg;
            }
            continue;
        }
        if ((startsWith(linkage, "std.") || startsWith(linkage, "zith.")) &&
            !hirFn.hasBody) {
            if (tailTransfer) {
                state.result->ok      = false;
                state.result->message = "state tail transfer does not target a VM function";
                return kUnassignedReg;
            }
            continue;
        }
        const auto functionIndex =
            call.resolved_fn < state.functionIndices.size()
                ? state.functionIndices[call.resolved_fn]
                : kUnassignedReg;
        if (functionIndex != kUnassignedReg) {
                if (call.isVariadicSlice &&
                    (call.variadicSliceParam >= call.args.size() ||
                     call.variadicSliceParam >= call.argument_types.size() ||
                     state.types.kindOf(call.argument_types[call.variadicSliceParam]) !=
                         types::TypeKind::Slice)) {
                    state.result->ok      = false;
                    state.result->message = "invalid variadic slice plan in v2 lowering";
                    return kUnassignedReg;
                }

                // Decide from the HIR signature, not the already-lowered row:
                // the callee may not have been lowered yet when its caller is.
                const bool calleeReturnsAggregate = isAggregateType(state.types, hirFn.return_type);
                if (tailTransfer && state.fn->returnsAggregate != calleeReturnsAggregate) {
                    state.result->ok = false;
                    state.result->message =
                        "state tail transfer has an incompatible aggregate return in v2 lowering";
                    return kUnassignedReg;
                }

                std::vector<std::size_t> argumentWidths;
                argumentWidths.reserve(call.args.size());
                std::size_t argCount = 0;
                for (std::size_t i = 0; i < call.args.size(); ++i) {
                    const bool isPlannedSlice =
                        call.isVariadicSlice && i == call.variadicSliceParam;
                    const bool isSlice =
                        isPlannedSlice ||
                        (i < call.argument_types.size() &&
                         state.types.kindOf(call.argument_types[i]) == types::TypeKind::Slice);
                    const std::size_t width = isSlice ? 2U : 1U;
                    if (argCount + width > 0xFFFFU) {
                        state.result->ok      = false;
                        state.result->message = "v2 call argument range exceeds 65535 registers";
                        return kUnassignedReg;
                    }
                    argumentWidths.push_back(width);
                    argCount += width;
                }

                const auto fnIndex = functionIndex;
                // An aggregate-returning callee takes its hidden result pointer
                // as register 0, ahead of the declared parameters.
                const std::size_t windowSize = argCount + (calleeReturnsAggregate ? 1U : 0U);
                const std::uint16_t argBase =
                    windowSize == 0 ? std::uint16_t{0} : u16(state.fn->regCount, state.result);
                if (!state.result->ok ||
                    static_cast<std::size_t>(state.fn->regCount) + windowSize > 0xFFFFU) {
                    if (state.result->ok) {
                        state.result->ok = false;
                        state.result->message =
                            "v2 caller registers exceed 65535 while preparing arguments";
                    }
                    return kUnassignedReg;
                }

                // Reserve the contiguous argument window first. An
                // aggregate-returning callee keeps register `argBase` for the
                // hidden result pointer, ahead of the declared parameters.
                std::uint16_t sretSlot = kUnassignedReg;
                if (calleeReturnsAggregate) {
                    state.fn->regTypes.push(ValueType::Ptr);
                    sretSlot = argBase;
                }

                for (std::size_t i = 0; i < call.args.size(); ++i) {
                    const auto type = i < call.argument_types.size() ? call.argument_types[i]
                                                                     : types::kInvalidType;
                    state.fn->regTypes.push(argumentWidths[i] == 2U
                                                ? ValueType::Ptr
                                                : (type != types::kInvalidType
                                                       ? valueTypeOf(state.types, type)
                                                       : ValueType::I64));
                    if (argumentWidths[i] == 2U)
                        state.fn->regTypes.push(ValueType::I64);
                }
                state.fn->regCount = u16(state.fn->regTypes.size(), state.result);
                if (!state.result->ok)
                    return kUnassignedReg;

                // Allocate the caller-owned result slot after the argument
                // window so the scratch registers do not overlap it.
                if (calleeReturnsAggregate && tailTransfer) {
                    if (state.sretReg == kUnassignedReg) {
                        state.result->ok = false;
                        state.result->message =
                            "state tail transfer has no return slot in v2 lowering";
                        return kUnassignedReg;
                    }
                    emit(state, Instr::simple(Op::Move, sretSlot, state.sretReg));
                } else if (calleeReturnsAggregate) {
                    const auto size = typeSize(state.types, hirFn.return_type);
                    if (size == 0U || size > 0xFFFFU) {
                        state.result->ok      = false;
                        state.result->message = "unsupported aggregate return size in v2 lowering";
                        return kUnassignedReg;
                    }
                    const auto count = newTypedReg(state, ValueType::I64);
                    const auto align = newTypedReg(state, ValueType::I64);
                    emit(state,
                         Instr::withImm(Op::LoadConstI64, count, static_cast<std::uint16_t>(size)));
                    emit(state, Instr::withImm(Op::LoadConstI64, align, 8));
                    emit(state, Instr::simple(Op::AllocBytes, sretSlot, count, align));
                }

                if (argCount > 0) {
                    std::size_t argumentOffset = calleeReturnsAggregate ? 1U : 0U;
                    for (std::size_t i = 0; i < call.args.size(); ++i) {
                        bool materializedSlice = false;
                        auto val = materializeBorrowedSlice(state, call.args[i],
                                                            i < call.argument_types.size()
                                                                ? call.argument_types[i]
                                                                : types::kInvalidType,
                                                            materializedSlice);
                        if (!materializedSlice)
                            val = lowerOperand(state, call.args[i]);
                        if (val == kUnassignedReg)
                            return kUnassignedReg;
                        const auto destination =
                            u16(static_cast<std::size_t>(argBase) + argumentOffset, state.result);
                        emit(state, Instr::simple(Op::Move, destination, val));
                        if (argumentWidths[i] == 2U) {
                            if (static_cast<std::size_t>(val) + 1U >= state.fn->regCount) {
                                state.result->ok = false;
                                state.result->message =
                                    "slice argument pair exceeds v2 caller registers";
                                return kUnassignedReg;
                            }
                            emit(state, Instr::simple(Op::Move,
                                                      static_cast<std::uint16_t>(destination + 1U),
                                                      static_cast<std::uint16_t>(val + 1U)));
                        }
                        argumentOffset += argumentWidths[i];
                    }
                }
                if (tailTransfer) {
                    emit(state,
                         Instr::tailCallRange(argBase, u16(windowSize, state.result), fnIndex));
                    return 0;
                }
                // A slice-returning callee hands back a `{ptr, len}` pair in
                // two consecutive registers. Reserve both and copy the length
                // half next to the pointer half so callers can read it.
                const bool calleeReturnsSlice =
                    state.types.kindOf(hirFn.return_type) == types::TypeKind::Slice;
                const auto reg = newTypedReg(state, calleeReturnsSlice ? ValueType::Ptr
                                                                       : valueTypeOf(state.types,
                                                                                     call.fn_type));
                if (calleeReturnsSlice)
                    (void)newTypedReg(state, ValueType::I64);
                emit(state, Instr::callRange(reg, argBase, u16(windowSize, state.result), fnIndex));
                if (calleeReturnsAggregate)
                    return sretSlot;
                return reg;
        }
    }

    state.result->ok      = false;
    state.result->message =
        unsupported_linkage.empty()
            ? "resolved call target " + std::to_string(call.resolved_fn) +
                  " is missing in v2 lowering"
            : "unsupported resolved call target '" + std::string(unsupported_linkage) +
                  "' in v2 lowering";
    return 0;
}

auto lowerOperand(LowerState &state, hir::HirExprId id) -> std::uint16_t;

/// Guest address of a struct/pack field. The base is an aggregate address, so
/// nested fields compose without copying.
auto fieldAddress(LowerState &state, const hir::HirField &field) -> std::uint16_t {
    const auto base = aggregateAddress(state, field.object);
    if (base == kUnassignedReg)
        return kUnassignedReg;
    const auto offset = fieldByteOffset(state.types, field.object_type, field.index);
    if (offset > 0xFFFFU) {
        state.result->ok      = false;
        state.result->message = "struct field offset exceeds the v2 instruction width";
        return kUnassignedReg;
    }
    const auto addr = newTypedReg(state, ValueType::Ptr);
    emit(state, Instr{Op::FieldPtr, addr, base, 0, static_cast<std::uint16_t>(offset)});
    return addr;
}

auto aggregateAddress(LowerState &state, hir::HirExprId id) -> std::uint16_t {
    if (id == hir::kInvalidHirExpr || id >= state.regs.size())
        return kUnassignedReg;
    const auto &expr = state.hir.getExpr(id);
    if (const auto *slot_addr = std::get_if<hir::HirSlotAddr>(&expr))
        return slotRegister(state, slot_addr->slot);
    if (const auto *slot_load = std::get_if<hir::HirSlotLoad>(&expr))
        return slotRegister(state, slot_load->slot);
    if (const auto *un = std::get_if<hir::HirUnary>(&expr);
        un != nullptr && un->op == hir::HirUnaryOp::Deref)
        return lowerOperand(state, un->operand);
    if (const auto *var = std::get_if<hir::HirVar>(&expr)) {
        for (std::size_t p = 0; p < state.hirFn->param_names.size(); ++p)
            if (state.hirFn->param_names[p] == var->name)
                return p < state.paramRegs.size() ? state.paramRegs[p] : kUnassignedReg;
        return kUnassignedReg;
    }
    if (const auto *field = std::get_if<hir::HirField>(&expr))
        return fieldAddress(state, *field);
    // A struct literal, aggregate call result, or `HirCast` over an aggregate
    // lowers to a pointer to freshly allocated storage.
    return lowerOperand(state, id);
}

/// Copy `size` bytes from `src` to `dst` (both guest addresses).
auto copyBytes(LowerState &state, std::uint16_t dst, std::uint16_t src, std::size_t size) -> void {
    const auto count = newTypedReg(state, ValueType::I64);
    emit(state, Instr::withImm(Op::LoadConstI64, count, u16(size, state.result)));
    emit(state, Instr::simple(Op::MemCopy, dst, src, count));
}

auto allocateGuestBytes(LowerState &state, std::size_t size) -> std::uint16_t {
    if (size == 0U || size > 0xFFFFU) {
        state.result->ok      = false;
        state.result->message = "unsupported aggregate size in v2 lowering";
        return kUnassignedReg;
    }
    const auto base  = newTypedReg(state, ValueType::Ptr);
    const auto count = newTypedReg(state, ValueType::I64);
    const auto align = newTypedReg(state, ValueType::I64);
    emit(state, Instr::withImm(Op::LoadConstI64, count, u16(size, state.result)));
    emit(state, Instr::withImm(Op::LoadConstI64, align, 8U));
    emit(state, Instr::simple(Op::AllocBytes, base, count, align));
    return base;
}

auto functionIndexForSymbol(LowerState &state, hir::HirFunctionId function_id) -> std::uint16_t {
    if (function_id < state.adapterIndices.size() &&
        state.adapterIndices[function_id] != kUnassignedReg)
        return state.adapterIndices[function_id];
    if (function_id < state.functionIndices.size() &&
        state.functionIndices[function_id] != kUnassignedReg)
        return state.functionIndices[function_id];
    state.result->ok      = false;
    state.result->message = "vtable slot references a function outside the reachable VM module";
    return kUnassignedReg;
}

auto lowerMakeDyn(LowerState &state, const hir::HirMakeDyn &make) -> std::uint16_t {
    if (state.types.kindOf(make.dyn_type) != types::TypeKind::Dyn) {
        state.result->ok      = false;
        state.result->message = "HirMakeDyn has a non-dyn result type in v2 lowering";
        return kUnassignedReg;
    }

    const hir::HirVTable *vtable = nullptr;
    for (std::size_t i = 0; i < state.hir.getVTableCount(); ++i) {
        const auto &candidate = state.hir.getVTable(i);
        if (candidate.name == make.vtable_name) {
            vtable = &candidate;
            break;
        }
    }
    if (vtable == nullptr) {
        state.result->ok      = false;
        state.result->message = "missing HIR vtable in v2 lowering";
        return kUnassignedReg;
    }
    if (vtable->slots.size() > 0xFFFFU / sizeof(std::uint64_t)) {
        state.result->ok      = false;
        state.result->message = "vtable exceeds the v2 linear-memory limit";
        return kUnassignedReg;
    }

    std::uint16_t data = kUnassignedReg;
    if (state.types.kindOf(make.source_type) == types::TypeKind::Slice) {
        const auto value = lowerOperand(state, make.value);
        if (value == kUnassignedReg ||
            static_cast<std::size_t>(value) + 1U >= state.fn->regCount)
            return kUnassignedReg;
        data = allocateGuestBytes(state, typeSize(state.types, make.source_type));
        if (data == kUnassignedReg)
            return kUnassignedReg;
        emit(state, Instr::simple(Op::StoreI64, data, value));
        const auto lengthAddress = newTypedReg(state, ValueType::Ptr);
        emit(state, Instr{Op::FieldPtr, lengthAddress, data, 0, 8U});
        emit(state, Instr::simple(Op::StoreI64, lengthAddress,
                                  static_cast<std::uint16_t>(value + 1U)));
    } else if (state.types.kindOf(make.source_type) == types::TypeKind::Ptr) {
        data = lowerOperand(state, make.value);
    } else if (make.value_is_place || isAggregateType(state.types, make.source_type)) {
        data = aggregateAddress(state, make.value);
    } else {
        const auto value = lowerOperand(state, make.value);
        if (value == kUnassignedReg)
            return kUnassignedReg;
        data = allocateGuestBytes(state, typeSize(state.types, make.source_type));
        if (data != kUnassignedReg)
            emit(state, Instr::simple(Op::StoreI64, data, value));
    }
    if (data == kUnassignedReg)
        return kUnassignedReg;

    const auto dyn        = allocateGuestBytes(state, typeSize(state.types, make.dyn_type));
    const auto vtableData = allocateGuestBytes(
        state, std::max<std::size_t>(vtable->slots.size(), 1U) * sizeof(std::uint64_t));
    if (dyn == kUnassignedReg || vtableData == kUnassignedReg)
        return kUnassignedReg;

    const auto dataField = newTypedReg(state, ValueType::Ptr);
    emit(state, Instr{Op::FieldPtr, dataField, dyn, 0, 0});
    emit(state, Instr::simple(Op::StoreI64, dataField, data));

    const auto vtableField = newTypedReg(state, ValueType::Ptr);
    emit(state, Instr{Op::FieldPtr, vtableField, dyn, 0, 8});
    emit(state, Instr::simple(Op::StoreI64, vtableField, vtableData));

    for (std::size_t slot = 0; slot < vtable->slots.size(); ++slot) {
        const auto functionIndex = functionIndexForSymbol(state, vtable->slots[slot]);
        if (functionIndex == kUnassignedReg)
            return kUnassignedReg;
        const auto functionRef = newTypedReg(state, ValueType::FnRef);
        emit(state, Instr::withImm(Op::LoadFnRef, functionRef, functionIndex));
        const auto slotAddress = newTypedReg(state, ValueType::Ptr);
        emit(state, Instr{Op::FieldPtr, slotAddress, vtableData, 0,
                          u16(slot * sizeof(std::uint64_t), state.result)});
        if (!state.result->ok)
            return kUnassignedReg;
        emit(state, Instr::simple(Op::StoreI64, slotAddress, functionRef));
    }
    return dyn;
}

auto lowerMakeOpaque(LowerState &state, const hir::HirMakeOpaque &make) -> std::uint16_t {
    if (state.types.kindOf(make.opaque_type) != types::TypeKind::OpaqueTagged) {
        state.result->ok      = false;
        state.result->message = "HirMakeOpaque has a non-opaque result type in v2 lowering";
        return kUnassignedReg;
    }

    const auto payload_value = lowerOperand(state, make.value);
    if (payload_value == kUnassignedReg)
        return kUnassignedReg;

    const auto payload_size = typeSize(state.types, make.source_type);
    const auto payload_data = allocateGuestBytes(state, payload_size);
    if (payload_data == kUnassignedReg)
        return kUnassignedReg;
    if (isPointerValued(state.types, make.source_type)) {
        const auto source = aggregateAddress(state, make.value);
        if (source == kUnassignedReg) {
            state.result->ok      = false;
            state.result->message = "unsupported opaque aggregate payload in v2 lowering";
            return kUnassignedReg;
        }
        copyBytes(state, payload_data, source, payload_size);
    } else {
        emit(state, Instr::simple(Op::StoreI64, payload_data, payload_value));
    }

    const auto opaque_data = allocateGuestBytes(state, typeSize(state.types, make.opaque_type));
    if (opaque_data == kUnassignedReg)
        return kUnassignedReg;

    const auto data_field = newTypedReg(state, ValueType::Ptr);
    emit(state, Instr{Op::FieldPtr, data_field, opaque_data, 0, 0});
    emit(state, Instr::simple(Op::StoreI64, data_field, payload_data));

    const auto tag_field = newTypedReg(state, ValueType::Ptr);
    emit(state, Instr{Op::FieldPtr, tag_field, opaque_data, 0, 8U});
    const auto tag = emitI64Constant(state, static_cast<std::int64_t>(make.type_id));
    if (tag == kUnassignedReg)
        return kUnassignedReg;
    emit(state, Instr::simple(Op::StoreI64, tag_field, tag));
    return opaque_data;
}

auto lowerOpaqueCheck(LowerState &state, const hir::HirOpaqueCheck &check) -> std::uint16_t {
    if (state.types.kindOf(check.opaque_type) != types::TypeKind::OpaqueTagged) {
        state.result->ok      = false;
        state.result->message = "HirOpaqueCheck has a non-opaque value in v2 lowering";
        return kUnassignedReg;
    }
    const auto opaque_data = aggregateAddress(state, check.value);
    if (opaque_data == kUnassignedReg)
        return kUnassignedReg;

    const auto tag_field = newTypedReg(state, ValueType::Ptr);
    emit(state, Instr{Op::FieldPtr, tag_field, opaque_data, 0, 8U});
    const auto actual_tag = newTypedReg(state, ValueType::I64);
    emit(state, Instr::simple(Op::LoadI64, actual_tag, tag_field));
    const auto expected_tag = emitI64Constant(state, static_cast<std::int64_t>(check.type_id));
    if (expected_tag == kUnassignedReg)
        return kUnassignedReg;
    const auto matches = newReg(state, types::kBoolType);
    emit(state, Instr::simple(Op::Eq, matches, actual_tag, expected_tag));
    return matches;
}

auto lowerOpaqueCast(LowerState &state, const hir::HirOpaqueCast &cast) -> std::uint16_t {
    if (state.types.kindOf(cast.opaque_type) != types::TypeKind::OpaqueTagged ||
        cast.safety == hir::HirSafetyMode::RequiredCheck || cast.returns_ptr) {
        state.result->ok      = false;
        state.result->message = "unsupported checked or raw opaque cast in v2 lowering";
        return kUnassignedReg;
    }
    const auto opaque_data = aggregateAddress(state, cast.value);
    if (opaque_data == kUnassignedReg)
        return kUnassignedReg;

    const auto payload_field = newTypedReg(state, ValueType::Ptr);
    emit(state, Instr{Op::FieldPtr, payload_field, opaque_data, 0, 0});
    const auto payload_data = newTypedReg(state, ValueType::Ptr);
    emit(state, Instr::simple(Op::LoadI64, payload_data, payload_field));
    if (isPointerValued(state.types, cast.to))
        return payload_data;

    const auto payload_value = newReg(state, cast.to);
    emit(state, Instr::simple(Op::LoadI64, payload_value, payload_data));
    return payload_value;
}

auto lowerDynCall(LowerState &state, const hir::HirDynCall &call) -> std::uint16_t {
    if (call.args.size() != call.arg_types.size()) {
        state.result->ok      = false;
        state.result->message = "dynamic call argument metadata is inconsistent in v2 lowering";
        return kUnassignedReg;
    }
    if (isAggregateType(state.types, call.result_type)) {
        state.result->ok      = false;
        state.result->message = "aggregate-returning dynamic calls are unsupported in v2";
        return kUnassignedReg;
    }

    const auto receiver = lowerOperand(state, call.receiver);
    if (receiver == kUnassignedReg)
        return kUnassignedReg;

    const auto dataField = newTypedReg(state, ValueType::Ptr);
    emit(state, Instr{Op::FieldPtr, dataField, receiver, 0, 0});
    const auto data = newTypedReg(state, ValueType::Ptr);
    emit(state, Instr::simple(Op::LoadI64, data, dataField));

    const auto vtableField = newTypedReg(state, ValueType::Ptr);
    emit(state, Instr{Op::FieldPtr, vtableField, receiver, 0, 8});
    const auto vtable = newTypedReg(state, ValueType::Ptr);
    emit(state, Instr::simple(Op::LoadI64, vtable, vtableField));

    const std::size_t slotOffset =
        static_cast<std::size_t>(call.slot_index) * sizeof(std::uint64_t);
    if (slotOffset > 0xFFFFU) {
        state.result->ok      = false;
        state.result->message = "dynamic call slot exceeds the v2 instruction width";
        return kUnassignedReg;
    }
    const auto slotAddress = newTypedReg(state, ValueType::Ptr);
    emit(state,
         Instr{Op::FieldPtr, slotAddress, vtable, 0, static_cast<std::uint16_t>(slotOffset)});
    const auto functionRef = newTypedReg(state, ValueType::FnRef);
    emit(state, Instr::simple(Op::LoadI64, functionRef, slotAddress));

    std::vector<std::uint16_t> arguments;
    std::vector<std::uint8_t> argumentWidths;
    std::vector<ValueType> flattenedTypes;
    arguments.reserve(call.args.size() + (call.has_receiver ? 1U : 0U));
    argumentWidths.reserve(arguments.capacity());
    if (call.has_receiver) {
        arguments.push_back(data);
        argumentWidths.push_back(1U);
        flattenedTypes.push_back(ValueType::Ptr);
    }
    for (std::size_t index = 0; index < call.args.size(); ++index) {
        const auto argument = call.args[index];
        const auto value = lowerOperand(state, argument);
        if (value == kUnassignedReg)
            return kUnassignedReg;
        arguments.push_back(value);
        const bool isSlice = state.types.kindOf(call.arg_types[index]) == types::TypeKind::Slice;
        const auto width  = static_cast<std::uint8_t>(isSlice ? 2U : 1U);
        if (isSlice && static_cast<std::size_t>(value) + 1U >= state.fn->regCount) {
            state.result->ok      = false;
            state.result->message = "slice argument pair exceeds v2 caller registers";
            return kUnassignedReg;
        }
        argumentWidths.push_back(width);
        if (isSlice) {
            flattenedTypes.push_back(ValueType::Ptr);
            flattenedTypes.push_back(ValueType::I64);
        } else {
            flattenedTypes.push_back(valueTypeOf(state.types, call.arg_types[index]));
        }
    }

    const auto maxRegisterCount = static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max());
    if (flattenedTypes.size() >= maxRegisterCount ||
        static_cast<std::size_t>(state.fn->regCount) >
            maxRegisterCount - flattenedTypes.size() - 1U) {
        state.result->ok      = false;
        state.result->message = "dynamic call argument range exceeds v2 registers";
        return kUnassignedReg;
    }
    const auto argBase = u16(state.fn->regCount, state.result);
    if (!state.result->ok)
        return kUnassignedReg;
    for (const auto type : flattenedTypes)
        (void)newTypedReg(state, type);

    std::size_t argumentOffset = 0U;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto destination =
            u16(static_cast<std::size_t>(argBase) + argumentOffset, state.result);
        emit(state, Instr::simple(Op::Move, destination, arguments[i]));
        if (argumentWidths[i] == 2U) {
            emit(state, Instr::simple(
                            Op::Move, static_cast<std::uint16_t>(destination + 1U),
                            static_cast<std::uint16_t>(arguments[i] + 1U)));
        }
        argumentOffset += argumentWidths[i];
    }

    // A slice-returning dyn call hands back a `{ptr, len}` pair; reserve both
    // registers so the length half is not clobbered by a later value.
    const bool returnsSlice =
        state.types.kindOf(call.result_type) == types::TypeKind::Slice;
    const auto result = newReg(state, call.result_type);
    if (!state.result->ok)
        return kUnassignedReg;
    if (returnsSlice)
        (void)newTypedReg(state, ValueType::I64);
    emit(state, Instr::callRangeRef(Op::CallRangeFnRef, result, functionRef, argBase,
                                    u16(flattenedTypes.size(), state.result)));
    return result;
}

auto lowerMakeNone(LowerState &state, const hir::HirMakeNone &none) -> std::uint16_t {
    const auto *optional = std::get_if<types::TypeOptional>(&state.types.lookup(none.type));
    if (optional == nullptr) {
        state.result->ok      = false;
        state.result->message = "HirMakeNone has a non-optional type in v2 lowering";
        return kUnassignedReg;
    }
    if (types::TypeKind::Ptr == state.types.kindOf(optional->inner)) {
        // Guest offset zero is valid, so reserve -1 as the optional-pointer
        // sentinel rather than conflating a live pointer with None.
        const auto index = u16(state.out.i64Constants.size(), state.result);
        if (!state.result->ok)
            return kUnassignedReg;
        state.out.i64Constants.push(-1);
        const auto result = newReg(state, none.type);
        emit(state, Instr::withImm(Op::LoadConstI64FromPool, result, index));
        return result;
    }
    const auto base = allocateGuestBytes(state, typeSize(state.types, none.type));
    if (base == kUnassignedReg)
        return base;

    std::size_t inner_align = 1U;
    const auto inner_size   = layoutSize(state.types, optional->inner, inner_align);
    const auto tag_offset   = u16(inner_size, state.result);
    if (!state.result->ok || inner_size == 0U)
        return kUnassignedReg;

    const auto tag = newTypedReg(state, ValueType::Ptr);
    emit(state, Instr{Op::FieldPtr, tag, base, 0, tag_offset});
    const auto absent = newTypedReg(state, ValueType::I64);
    emit(state, Instr::withImm(Op::LoadConstI64, absent, 0));
    emit(state, Instr::simple(Op::StoreI8, tag, absent));
    return base;
}

auto lowerMakeSome(LowerState &state, const hir::HirMakeSome &some) -> std::uint16_t {
    const auto *optional = std::get_if<types::TypeOptional>(&state.types.lookup(some.type));
    if (optional == nullptr) {
        state.result->ok      = false;
        state.result->message = "HirMakeSome has a non-optional type in v2 lowering";
        return kUnassignedReg;
    }
    const auto value = lowerOperand(state, some.value);
    if (value == kUnassignedReg)
        return kUnassignedReg;
    if (types::TypeKind::Ptr == state.types.kindOf(optional->inner))
        return value;

    std::size_t inner_align = 1U;
    const auto inner_size   = layoutSize(state.types, optional->inner, inner_align);
    const auto base         = allocateGuestBytes(state, typeSize(state.types, some.type));
    if (base == kUnassignedReg || inner_size == 0U)
        return kUnassignedReg;

    if (isAggregateType(state.types, optional->inner)) {
        const auto source = aggregateAddress(state, some.value);
        if (source == kUnassignedReg)
            return kUnassignedReg;
        copyBytes(state, base, source, inner_size);
    } else {
        emit(state, Instr::simple(Op::StoreI64, base, value));
    }

    const auto tag = newTypedReg(state, ValueType::Ptr);
    emit(state, Instr{Op::FieldPtr, tag, base, 0, u16(inner_size, state.result)});
    const auto present = newTypedReg(state, ValueType::I64);
    emit(state, Instr::withImm(Op::LoadConstI64, present, 1U));
    emit(state, Instr::simple(Op::StoreI8, tag, present));
    return base;
}

/// Lower a struct/pack literal into freshly allocated linear memory. Field
/// values are stored at their laid-out offsets; nested aggregates are copied.
auto lowerStructLiteral(LowerState &state, const hir::HirStructLiteral &literal) -> std::uint16_t {
    const auto size = typeSize(state.types, literal.type);
    if (size == 0U || size > 0xFFFFU) {
        state.result->ok      = false;
        state.result->message = "unsupported aggregate size in v2 lowering";
        return kUnassignedReg;
    }
    const auto base  = newTypedReg(state, ValueType::Ptr);
    const auto count = newTypedReg(state, ValueType::I64);
    const auto align = newTypedReg(state, ValueType::I64);
    emit(state, Instr::withImm(Op::LoadConstI64, count, static_cast<std::uint16_t>(size)));
    emit(state, Instr::withImm(Op::LoadConstI64, align, 8));
    emit(state, Instr::simple(Op::AllocBytes, base, count, align));

    const auto field_count = state.types.memberCount(literal.type);
    for (std::size_t i = 0; i < literal.values.size(); ++i) {
        if (literal.values[i] == hir::kInvalidHirExpr)
            continue;
        const auto field_type =
            i < field_count ? state.types.memberAt(literal.type, i).type : types::kInvalidType;
        if (field_type == types::kInvalidType)
            continue;
        const auto offset =
            fieldByteOffset(state.types, literal.type, static_cast<std::uint32_t>(i));
        if (offset > 0xFFFFU) {
            state.result->ok      = false;
            state.result->message = "struct field offset exceeds the v2 instruction width";
            return kUnassignedReg;
        }
        const auto addr = newTypedReg(state, ValueType::Ptr);
        emit(state, Instr{Op::FieldPtr, addr, base, 0, static_cast<std::uint16_t>(offset)});
        if (isPointerValued(state.types, field_type)) {
            const auto source = aggregateAddress(state, literal.values[i]);
            if (source == kUnassignedReg)
                return kUnassignedReg;
            copyBytes(state, addr, source, typeSize(state.types, field_type));
        } else {
            const auto value = lowerOperand(state, literal.values[i]);
            if (value == kUnassignedReg)
                return kUnassignedReg;
            emit(state, Instr::simple(Op::StoreI64, addr, value));
        }
    }
    return base;
}

/// Address of an lvalue target (`p.x`, `a[i]`, `*p`) so an assignment can
/// store into it. Scalar targets yield the slot/pointer address; aggregate
/// targets yield their storage base.
auto lvalueAddress(LowerState &state, hir::HirExprId target) -> std::uint16_t {
    if (target == hir::kInvalidHirExpr || target >= state.regs.size())
        return kUnassignedReg;
    const auto &expr = state.hir.getExpr(target);
    if (const auto *field = std::get_if<hir::HirField>(&expr))
        return fieldAddress(state, *field);
    if (const auto *index = std::get_if<hir::HirIndex>(&expr)) {
        // Address of `a[i]` without loading the element, so an assignment can
        // store through it. Array and slice bases are already addresses; a
        // pointer base is the data pointer. Pointer indexing uses 1-byte
        // elements, matching the load path below.
        const auto base = lowerOperand(state, index->object);
        if (base == kUnassignedReg)
            return kUnassignedReg;
        const auto kind = state.types.kindOf(index->obj_type);
        // Pointer indexing has no store opcode yet, so it stays unsupported.
        if (kind == types::TypeKind::Ptr) {
            state.result->ok      = false;
            state.result->message = "unsupported pointer index assignment in v2 lowering";
            return kUnassignedReg;
        }
        const auto elementSize = index->is_array ? arrayElementSize(state.types, index->obj_type)
                                                 : scalarElementSize(state.types, index->type);
        if (elementSize == 0U || elementSize > 0xFFFFU) {
            state.result->ok      = false;
            state.result->message = "unsupported index assignment in v2 lowering";
            return kUnassignedReg;
        }
        const auto indexReg = lowerOperand(state, index->index);
        if (indexReg == kUnassignedReg)
            return kUnassignedReg;
        const auto scale = newTypedReg(state, ValueType::I64);
        emit(state,
             Instr::withImm(Op::LoadConstI64, scale, static_cast<std::uint16_t>(elementSize)));
        const auto byteOffset = newTypedReg(state, ValueType::I64);
        emit(state, Instr::simple(Op::Mul, byteOffset, indexReg, scale));
        const auto addr = newTypedReg(state, ValueType::Ptr);
        emit(state, Instr::simple(Op::Add, addr, base, byteOffset));
        return addr;
    }
    if (const auto *slot_addr = std::get_if<hir::HirSlotAddr>(&expr))
        return slotRegister(state, slot_addr->slot);
    if (const auto *slot_load = std::get_if<hir::HirSlotLoad>(&expr))
        return slotRegister(state, slot_load->slot);
    if (const auto *un = std::get_if<hir::HirUnary>(&expr);
        un != nullptr && un->op == hir::HirUnaryOp::Deref)
        return lowerOperand(state, un->operand);
    return aggregateAddress(state, target);
}

auto lowerSlotValue(LowerState &state, hir::HirSlotId slot, types::TypeId type) -> std::uint16_t {
    const auto base = slotRegister(state, slot);
    if (base == kUnassignedReg) {
        state.result->ok      = false;
        state.result->message = "unknown HIR slot in v2 lowering";
        return kUnassignedReg;
    }

    if (slot < state.slotBacked.size() && state.slotBacked[slot] != 0U &&
        state.types.kindOf(type) == types::TypeKind::Slice) {
        const auto value = newTypedReg(state, ValueType::Ptr);
        (void)newTypedReg(state, ValueType::I64);
        emit(state, Instr::simple(Op::LoadI64, value, base));
        const auto lengthAddress = newTypedReg(state, ValueType::Ptr);
        emit(state, Instr{Op::FieldPtr, lengthAddress, base, 0, 8U});
        emit(state, Instr::simple(Op::LoadI64, static_cast<std::uint16_t>(value + 1U),
                                  lengthAddress));
        return value;
    }

    const bool memoryScalar = slot < state.slotBacked.size() && state.slotBacked[slot] != 0U &&
                              !isPointerValued(state.types, type);
    if (!memoryScalar)
        return base;

    const auto value = newReg(state, type);
    emit(state, Instr::simple(Op::LoadI64, value, base, 0));
    return value;
}

auto lowerSlotStore(LowerState &state, const hir::HirSlotStore &store) -> std::uint16_t {
    const auto destination = slotRegister(state, store.slot);
    if (destination == kUnassignedReg) {
        state.result->ok      = false;
        state.result->message = "invalid HIR slot store in v2 lowering";
        return kUnassignedReg;
    }

    const auto slotType =
        store.slot < state.slotTypes.size() ? state.slotTypes[store.slot] : types::kInvalidType;
    if (store.slot < state.slotBacked.size() && state.slotBacked[store.slot] != 0U &&
        state.types.kindOf(slotType) == types::TypeKind::Slice) {
        const auto value = lowerOperand(state, store.value);
        if (value == kUnassignedReg ||
            static_cast<std::size_t>(value) + 1U >= state.fn->regCount) {
            state.result->ok      = false;
            state.result->message = "invalid slice value in v2 slot store";
            return kUnassignedReg;
        }
        emit(state, Instr::simple(Op::StoreI64, destination, value));
        const auto lengthAddress = newTypedReg(state, ValueType::Ptr);
        emit(state, Instr{Op::FieldPtr, lengthAddress, destination, 0, 8U});
        emit(state, Instr::simple(Op::StoreI64, lengthAddress,
                                  static_cast<std::uint16_t>(value + 1U)));
        return value;
    }
    if (isAggregateType(state.types, slotType)) {
        const auto source = aggregateAddress(state, store.value);
        if (source == kUnassignedReg)
            return kUnassignedReg;
        if (source != destination)
            copyBytes(state, destination, source, typeSize(state.types, slotType));
        return source;
    }
    if (store.slot < state.slotBacked.size() && state.slotBacked[store.slot] != 0U &&
        state.types.kindOf(slotType) == types::TypeKind::Array) {
        const auto source = aggregateAddress(state, store.value);
        if (source == kUnassignedReg)
            return kUnassignedReg;
        if (source != destination)
            copyBytes(state, destination, source, typeSize(state.types, slotType));
        return source;
    }

    const auto value = lowerOperand(state, store.value);
    if (value == kUnassignedReg)
        return kUnassignedReg;
    const bool memoryScalar = store.slot < state.slotBacked.size() &&
                              state.slotBacked[store.slot] != 0U &&
                              !isAggregateType(state.types, slotType);
    if (memoryScalar) {
        emit(state, Instr::simple(Op::StoreI64, destination, value));
        return value;
    }

    const bool isSliceSlot = state.slotWidths[store.slot] == 2U;
    if (isSliceSlot && (static_cast<std::size_t>(destination) + 1U >= state.fn->regCount ||
                        static_cast<std::size_t>(value) + 1U >= state.fn->regCount)) {
        state.result->ok      = false;
        state.result->message = "slice HIR slot store exceeds v2 registers";
        return kUnassignedReg;
    }
    if (destination != value)
        emit(state, Instr::simple(Op::Move, destination, value));
    if (isSliceSlot) {
        emit(state, Instr::simple(Op::Move, static_cast<std::uint16_t>(destination + 1U),
                                  static_cast<std::uint16_t>(value + 1U)));
    }
    return value;
}

auto lowerOperand(LowerState &state, hir::HirExprId id) -> std::uint16_t {
    if (id >= state.regs.size())
        return kUnassignedReg;
    if (state.regs[id] != kUnassignedReg)
        return state.regs[id];

    const auto &expr = state.hir.getExpr(id);
    const auto reg   = hir::visitExpr(
        expr,
        common::overloaded{
            [&](const hir::HirLiteral &lit) { return lowerLiteral(state, lit); },
            [&](const hir::HirEnumValue &value) -> std::uint16_t {
                const auto *enum_type =
                    std::get_if<types::TypeEnum>(&state.types.lookup(value.type));
                if (enum_type == nullptr ||
                    state.types.lookupEnumDef(enum_type->def_id) == nullptr) {
                    state.result->ok      = false;
                    state.result->message = "invalid HIR enum value in v2 lowering";
                    return kUnassignedReg;
                }
                const auto index = u16(state.out.i64Constants.size(), state.result);
                if (!state.result->ok)
                    return kUnassignedReg;
                state.out.i64Constants.push(value.value);
                const auto result = newReg(state, value.type);
                emit(state, Instr::withImm(Op::LoadConstI64FromPool, result, index));
                return result;
            },
            [&](const hir::HirVar &var) -> std::uint16_t {
                for (std::size_t p = 0; p < state.hirFn->param_names.size(); ++p)
                    if (state.hirFn->param_names[p] == var.name) {
                        if (p >= state.paramRegs.size())
                            return kUnassignedReg;
                        state.regs[id] = state.paramRegs[p];
                        return state.paramRegs[p];
                    }
                state.result->ok      = false;
                state.result->message = "unsupported HIR variable in v2 lowering";
                return kUnassignedReg;
            },
            [&](const hir::HirExternalGlobalAddr &address) -> std::uint16_t {
                const auto name  = state.interner.lookup(address.name);
                state.result->ok = false;
                state.result->message =
                    "unsupported external global '" + std::string(name) + "' in v2 VM";
                return kUnassignedReg;
            },
            [&](const hir::HirBinary &bin) -> std::uint16_t {
                const auto lhs = lowerOperand(state, bin.lhs);
                const auto rhs = lowerOperand(state, bin.rhs);
                if (lhs == kUnassignedReg || rhs == kUnassignedReg)
                    return kUnassignedReg;
                Op op = Op::Trap;
                switch (bin.op) {
                case hir::HirBinaryOp::Add:
                    op = Op::Add;
                    break;
                case hir::HirBinaryOp::Sub:
                    op = Op::Sub;
                    break;
                case hir::HirBinaryOp::Mul:
                    op = Op::Mul;
                    break;
                case hir::HirBinaryOp::Div:
                    op = Op::Div;
                    break;
                case hir::HirBinaryOp::Rem:
                    op = Op::Rem;
                    break;
                case hir::HirBinaryOp::Eq:
                    op = Op::Eq;
                    break;
                case hir::HirBinaryOp::Ne:
                    op = Op::Ne;
                    break;
                case hir::HirBinaryOp::Lt:
                    op = Op::Lt;
                    break;
                case hir::HirBinaryOp::Le:
                    op = Op::Le;
                    break;
                case hir::HirBinaryOp::Gt:
                    op = Op::Gt;
                    break;
                case hir::HirBinaryOp::Ge:
                    op = Op::Ge;
                    break;
                case hir::HirBinaryOp::And:
                    op = Op::BitAnd;
                    break;
                case hir::HirBinaryOp::Or:
                    op = Op::BitOr;
                    break;
                case hir::HirBinaryOp::Xor:
                    op = Op::BitXor;
                    break;
                case hir::HirBinaryOp::Shl:
                    op = Op::Shl;
                    break;
                case hir::HirBinaryOp::Shr:
                    op = Op::Shr;
                    break;
                case hir::HirBinaryOp::Invalid:
                    op = Op::Trap;
                    break;
                }
                if (op == Op::Trap) {
                    state.result->ok      = false;
                    state.result->message = "unsupported HIR binary operator in v2 lowering";
                    return kUnassignedReg;
                }
                const auto dst = newReg(state, bin.type);
                emit(state, Instr::simple(op, dst, lhs, rhs));
                return dst;
            },
            [&](const hir::HirUnary &un) -> std::uint16_t {
                if (un.op == hir::HirUnaryOp::Ref)
                    return lvalueAddress(state, un.operand);
                if (un.op == hir::HirUnaryOp::Deref) {
                    const auto pointer = lowerOperand(state, un.operand);
                    if (pointer == kUnassignedReg)
                        return kUnassignedReg;
                    if (isPointerValued(state.types, un.type))
                        return pointer;
                    const auto dst = newReg(state, un.type);
                    emit(state, Instr::simple(Op::LoadI64, dst, pointer, 0));
                    return dst;
                }
                const auto operand = lowerOperand(state, un.operand);
                if (operand == kUnassignedReg)
                    return kUnassignedReg;
                Op op = Op::Trap;
                if (un.op == hir::HirUnaryOp::Neg)
                    op = Op::Neg;
                else if (un.op == hir::HirUnaryOp::Not)
                    op = Op::Not;
                else if (un.op == hir::HirUnaryOp::BitNot)
                    op = Op::BitNot;
                if (op == Op::Trap) {
                    state.result->ok      = false;
                    state.result->message = "unsupported HIR unary operator in v2 lowering";
                    return kUnassignedReg;
                }
                const auto dst = newReg(state, un.type);
                emit(state, Instr::simple(op, dst, operand, 0));
                return dst;
            },
            [&](const hir::HirSlotLoad &load) -> std::uint16_t {
                // A memory-backed scalar slot stores the address of its value,
                // so a load reads it. Aggregate slots return their address
                // (address-backed), and slice slots keep their {ptr, len} pair.
                return lowerSlotValue(state, load.slot, load.type);
            },
            [&](const hir::HirSlotAddr &addr) -> std::uint16_t {
                const auto base = slotRegister(state, addr.slot);
                if (base == kUnassignedReg) {
                    state.result->ok      = false;
                    state.result->message = "unknown HIR slot address in v2 lowering";
                }
                return base;
            },
            [&](const hir::HirSlotStore &store) { return lowerSlotStore(state, store); },
            [&](const hir::HirField &field) -> std::uint16_t {
                if (state.types.kindOf(field.object_type) == types::TypeKind::Union &&
                    field.index == 1U) {
                    const auto storage = aggregateAddress(state, field.object);
                    if (storage == kUnassignedReg)
                        return kUnassignedReg;
                    return loadUnionTag(state, UnionStorageRef{storage, field.object_type});
                }
                // A slice is a `{ptr, len}` register pair, not address-backed
                // memory, so its `data` (index 0) and `length` (index 1) fields
                // read the pair directly. The HIR bounds-check lowering reaches
                // a slice length through `HirField`, so this must not go through
                // `fieldAddress`.
                if (state.types.kindOf(field.object_type) == types::TypeKind::Slice) {
                    const auto base = lowerOperand(state, field.object);
                    if (base == kUnassignedReg)
                        return kUnassignedReg;
                    if (field.index == 0U)
                        return base;
                    if (field.index == 1U) {
                        if (static_cast<std::size_t>(base) + 1U >= state.fn->regCount) {
                            state.result->ok      = false;
                            state.result->message = "slice length field exceeds v2 registers";
                            return kUnassignedReg;
                        }
                        const auto length = newTypedReg(state, ValueType::I64);
                        emit(state, Instr::simple(Op::Move, length,
                                                  static_cast<std::uint16_t>(base + 1U)));
                        return length;
                    }
                    state.result->ok      = false;
                    state.result->message = "unsupported slice field index in v2 lowering";
                    return kUnassignedReg;
                }
                const auto addr = fieldAddress(state, field);
                if (addr == kUnassignedReg)
                    return kUnassignedReg;
                if (isPointerValued(state.types, field.type))
                    return addr;
                const auto dst = newReg(state, field.type);
                const auto op =
                    state.types.kindOf(field.object_type) == types::TypeKind::Optional &&
                            field.index == 1U
                        ? Op::LoadI8
                        : Op::LoadI64;
                emit(state, Instr::simple(op, dst, addr, 0));
                return dst;
            },
            [&](const hir::HirUnionCast &cast) -> std::uint16_t {
                const auto from_kind = state.types.kindOf(cast.from);
                const auto to_kind   = state.types.kindOf(cast.to);
                if (to_kind == types::TypeKind::Union && from_kind != types::TypeKind::Union) {
                    const auto *union_type =
                        std::get_if<types::TypeUnion>(&state.types.lookup(cast.to));
                    const auto *def = union_type != nullptr
                                          ? state.types.lookupUnionDef(union_type->def_id)
                                          : nullptr;
                    if (def == nullptr ||
                        (def->is_tagged && cast.member_index >= def->members.size())) {
                        state.result->ok      = false;
                        state.result->message = "invalid HIR union member in v2 lowering";
                        return kUnassignedReg;
                    }
                    const auto size = typeSize(state.types, cast.to);
                    if (size == 0U || size > 0xFFFFU) {
                        state.result->ok      = false;
                        state.result->message = "unsupported union size in v2 lowering";
                        return kUnassignedReg;
                    }
                    const auto storage = allocateGuestBytes(state, size);
                    const auto value   = lowerOperand(state, cast.value);
                    if (storage == kUnassignedReg || value == kUnassignedReg)
                        return kUnassignedReg;
                    if (isPointerValued(state.types, cast.from)) {
                        const auto source       = aggregateAddress(state, cast.value);
                        const auto payload_size = typeSize(state.types, cast.from);
                        if (source == kUnassignedReg || payload_size == 0U) {
                            state.result->ok      = false;
                            state.result->message = "unsupported union payload in v2 lowering";
                            return kUnassignedReg;
                        }
                        copyBytes(state, storage, source, payload_size);
                    } else {
                        emit(state, Instr::simple(Op::StoreI64, storage, value));
                    }
                    if (def->is_tagged &&
                        !storeUnionTag(state, UnionStorageRef{storage, cast.to}, cast.member_index))
                        return kUnassignedReg;
                    return storage;
                }
                if (from_kind == types::TypeKind::Union && to_kind != types::TypeKind::Union) {
                    const auto storage = aggregateAddress(state, cast.value);
                    if (storage == kUnassignedReg)
                        return kUnassignedReg;
                    const auto tag_width = taggedUnionTagWidth(state.types, cast.from);
                    if (cast.safety == hir::HirSafetyMode::RequiredCheck && tag_width != 0U &&
                        cast.member_index != ~0U &&
                        !emitCheckedUnionCast(state, UnionStorageRef{storage, cast.from}, cast))
                        return kUnassignedReg;
                    if (isPointerValued(state.types, cast.to))
                        return storage;
                    const auto value = newReg(state, cast.to);
                    emit(state, Instr::simple(Op::LoadI64, value, storage, 0));
                    return value;
                }
                state.result->ok      = false;
                state.result->message = "unsupported HIR union cast in v2 lowering";
                return kUnassignedReg;
            },
            [&](const hir::HirUnionCheck &check) -> std::uint16_t {
                if (check.member_index == ~0U) {
                    state.result->ok      = false;
                    state.result->message = "invalid HIR union check in v2 lowering";
                    return kUnassignedReg;
                }
                const auto tag = lowerOperand(state, check.value);
                if (tag == kUnassignedReg)
                    return kUnassignedReg;
                const auto expected =
                    emitI64Constant(state, static_cast<std::int64_t>(check.member_index));
                if (expected == kUnassignedReg)
                    return kUnassignedReg;
                const auto result = newReg(state, types::kBoolType);
                emit(state, Instr::simple(Op::Eq, result, tag, expected));
                return result;
            },
            [&](const hir::HirMakeDyn &make) { return lowerMakeDyn(state, make); },
            [&](const hir::HirDynCall &call) { return lowerDynCall(state, call); },
            [&](const hir::HirMakeOpaque &make) { return lowerMakeOpaque(state, make); },
            [&](const hir::HirOpaqueCast &cast) { return lowerOpaqueCast(state, cast); },
            [&](const hir::HirOpaqueCheck &check) { return lowerOpaqueCheck(state, check); },
            [&](const hir::HirMakeNone &none) { return lowerMakeNone(state, none); },
            [&](const hir::HirMakeSome &some) { return lowerMakeSome(state, some); },
            [&](const hir::HirStructLiteral &literal) {
                return lowerStructLiteral(state, literal);
            },
            [&](const hir::HirAssign &assign) -> std::uint16_t {
                const auto addr = lvalueAddress(state, assign.target);
                if (addr == kUnassignedReg)
                    return kUnassignedReg;
                const auto &targetExpr  = state.hir.getExpr(assign.target);
                const auto *targetField = std::get_if<hir::HirField>(&targetExpr);
                const auto *targetIndex = std::get_if<hir::HirIndex>(&targetExpr);
                const auto target_type = targetField != nullptr
                                             ? targetField->type
                                             : (targetIndex != nullptr ? targetIndex->type
                                                                       : types::kInvalidType);
                if (isPointerValued(state.types, target_type)) {
                    const auto source = aggregateAddress(state, assign.value);
                    if (source == kUnassignedReg)
                        return kUnassignedReg;
                    copyBytes(state, addr, source, typeSize(state.types, target_type));
                    return source;
                }
                const auto value = lowerOperand(state, assign.value);
                if (value == kUnassignedReg)
                    return kUnassignedReg;
                const bool byteElement =
                    targetIndex != nullptr &&
                    state.types.kindOf(targetIndex->type) == types::TypeKind::Char;
                emit(state, Instr::simple(byteElement ? Op::StoreI8 : Op::StoreI64, addr, value));
                return value;
            },
            [&](const hir::HirCast &cast) -> std::uint16_t {
                const auto value = lowerOperand(state, cast.value);
                if (value == kUnassignedReg)
                    return kUnassignedReg;
                return value;
            },
            [&](const hir::HirIndex &index) -> std::uint16_t {
                const auto object = lowerOperand(state, index.object);
                if (object == kUnassignedReg)
                    return kUnassignedReg;
                if (index.is_array) {
                    const auto *array =
                        std::get_if<types::TypeArray>(&state.types.lookup(index.obj_type));
                    const auto elementSize = arrayElementSize(state.types, index.obj_type);
                    if (array == nullptr || elementSize == 0 || array->count > 0xFFFFU ||
                        elementSize > 0xFFFFU) {
                        state.result->ok      = false;
                        state.result->message = "unsupported array index in v2 lowering";
                        return kUnassignedReg;
                    }
                    const auto indexReg = lowerOperand(state, index.index);
                    if (indexReg == kUnassignedReg)
                        return kUnassignedReg;
                    const bool byteElement =
                        state.types.kindOf(array->elem) == types::TypeKind::Char;
                    const bool aggregateResult = isPointerValued(state.types, index.type);
                    const bool addressResult   = byteElement || aggregateResult;
                    const auto address = addressResult ? newTypedReg(state, ValueType::Ptr)
                                                       : newReg(state, index.type);
                    emit(state, Instr{Op::IndexLoad, address, object, indexReg,
                                      static_cast<std::uint16_t>(elementSize),
                                      static_cast<std::uint16_t>(array->count),
                                      static_cast<std::uint16_t>(
                                          (index.safety == hir::HirSafetyMode::Raw ? 2U : 0U) |
                                          (addressResult ? 4U : 0U))});
                    if (!byteElement)
                        return address;
                    const auto value = newReg(state, index.type);
                    emit(state, Instr::simple(Op::LoadI8, value, address));
                    return value;
                }

                const auto *slice =
                    std::get_if<types::TypeSlice>(&state.types.lookup(index.obj_type));
                if (slice != nullptr) {
                    const bool byteElement =
                        state.types.kindOf(slice->elem) == types::TypeKind::Char;
                    const bool aggregateResult = isPointerValued(state.types, index.type);
                    const bool addressResult   = byteElement || aggregateResult;
                    const auto elementSize =
                        byteElement ? 1U
                                    : (aggregateResult ? typeSize(state.types, index.type)
                                                       : scalarElementSize(state.types, slice->elem));
                    const auto indexReg    = lowerOperand(state, index.index);
                    if (elementSize == 0U || elementSize > 0xFFFFU ||
                        static_cast<std::size_t>(object) + 1U >= state.fn->regCount ||
                        indexReg == kUnassignedReg || indexReg >= state.fn->regCount) {
                        state.result->ok      = false;
                        state.result->message = "unsupported slice index in v2 lowering";
                        return kUnassignedReg;
                    }
                    const auto address = addressResult ? newTypedReg(state, ValueType::Ptr)
                                                       : newReg(state, index.type);
                    emit(state, Instr{Op::IndexLoad, address, object, indexReg,
                                      static_cast<std::uint16_t>(elementSize),
                                      static_cast<std::uint16_t>(object + 1U),
                                      static_cast<std::uint16_t>(
                                          1U | (index.safety == hir::HirSafetyMode::Raw ? 2U : 0U) |
                                          (addressResult ? 4U : 0U))});
                    if (!byteElement)
                        return address;
                    const auto value = newReg(state, index.type);
                    emit(state, Instr::simple(Op::LoadI8, value, address));
                    return value;
                }

                // Pointer indexing: the base register already holds the data
                // pointer. Pointers carry no length, so the access is raw.
                if (const auto *pointer =
                        std::get_if<types::TypePtr>(&state.types.lookup(index.obj_type));
                    pointer != nullptr) {
                    const auto pointee     = pointer->pointee;
                    const bool byteElement = state.types.kindOf(pointee) == types::TypeKind::Char;
                    const bool aggregateResult = isPointerValued(state.types, index.type);
                    const bool addressResult   = byteElement || aggregateResult;
                    const auto elementSize =
                        byteElement ? 1U
                                    : (aggregateResult ? typeSize(state.types, index.type)
                                                       : scalarElementSize(state.types, pointee));
                    // Element sizes that do not resolve (for example a pointer
                    // to an aggregate) fall through to the literal-index path.
                    if (elementSize != 0U && elementSize <= 0xFFFFU) {
                        const auto indexReg = lowerOperand(state, index.index);
                        if (indexReg == kUnassignedReg)
                            return kUnassignedReg;
                        const auto address = addressResult ? newTypedReg(state, ValueType::Ptr)
                                                           : newReg(state, index.type);
                        emit(state,
                             Instr{Op::IndexLoad, address, object, indexReg,
                                   static_cast<std::uint16_t>(elementSize), 0U,
                                   static_cast<std::uint16_t>(2U | (addressResult ? 4U : 0U))});
                        if (!byteElement)
                            return address;
                        const auto value = newReg(state, index.type);
                        emit(state, Instr::simple(Op::LoadI8, value, address));
                        return value;
                    }
                }

                const auto &indexExpr = state.hir.getExpr(index.index);
                const auto *indexLit  = std::get_if<hir::HirLiteral>(&indexExpr);
                if (indexLit == nullptr) {
                    state.result->ok      = false;
                    state.result->message = "unsupported dynamic index in v2 lowering";
                    return kUnassignedReg;
                }
                const auto ptr = newReg(state, index.obj_type);
                emit(state,
                     Instr{Op::FieldPtr, ptr, object, 0, static_cast<std::uint16_t>(indexLit->i)});
                const auto count = newReg(state, types::kCharType);
                emit(state, Instr::withImm(Op::LoadConstI32, count, 1));
                const auto dst = newReg(state, index.type);
                emit(state, Instr::simple(Op::LoadBytes, dst, ptr, count));
                return dst;
            },
            [&](const hir::HirLayoutIntrinsic &intrinsic) -> std::uint16_t {
                if (intrinsic.operand == hir::kInvalidHirExpr)
                    return kUnassignedReg;
                const auto operand = lowerOperand(state, intrinsic.operand);
                if (operand == kUnassignedReg)
                    return kUnassignedReg;
                const auto *pointer_type =
                    std::get_if<types::TypePtr>(&state.types.lookup(intrinsic.operand_type));
                const bool points_to_slice =
                    pointer_type != nullptr &&
                    state.types.kindOf(pointer_type->pointee) == types::TypeKind::Slice;
                if (intrinsic.which == hir::HirLayoutIntrinsic::Which::PtrOf) {
                    if (points_to_slice) {
                        const auto data = newReg(state, intrinsic.type);
                        emit(state, Instr::simple(Op::LoadI64, data, operand, 0));
                        return data;
                    }
                    return operand;
                }
                if (intrinsic.which == hir::HirLayoutIntrinsic::Which::LengthOf) {
                    const auto kind = state.types.kindOf(intrinsic.operand_type);
                    if (points_to_slice) {
                        const auto length_address = newTypedReg(state, ValueType::Ptr);
                        emit(state, Instr{Op::FieldPtr, length_address, operand, 0, 8U});
                        const auto length = newTypedReg(state, ValueType::I64);
                        emit(state, Instr::simple(Op::LoadI64, length, length_address, 0));
                        return length;
                    }
                    if (kind == types::TypeKind::Slice) {
                        if (static_cast<std::size_t>(operand) + 1U >= state.fn->regCount)
                            return kUnassignedReg;
                        return static_cast<std::uint16_t>(operand + 1U);
                    }
                    if (const auto *array = std::get_if<types::TypeArray>(
                            &state.types.lookup(intrinsic.operand_type))) {
                        const auto length = newTypedReg(state, ValueType::I64);
                        emit(state, Instr::withImm(Op::LoadConstI64, length,
                                                   static_cast<std::uint16_t>(array->count)));
                        return length;
                    }
                }
                if (intrinsic.which == hir::HirLayoutIntrinsic::Which::LengthOf &&
                    intrinsic.string_length <= 0xFFFFU) {
                    const auto length = newTypedReg(state, ValueType::I64);
                    emit(state,
                         Instr::withImm(Op::LoadConstI64, length,
                                        static_cast<std::uint16_t>(intrinsic.string_length)));
                    return length;
                }
                return kUnassignedReg;
            },
            [&](const hir::HirMakeSlice &slice) { return lowerMakeSlice(state, slice); },
            [&](const hir::HirArrayLiteral &literal) { return lowerArrayLiteral(state, literal); },
            [&](const hir::HirCall &call) { return lowerCall(state, call); },
            [&](const hir::HirPipe &pipe) -> std::uint16_t {
                const auto stage = lowerOperand(state, pipe.stage);
                if (!pipe.is_effect) {
                    if (stage == kUnassignedReg)
                        return kUnassignedReg;
                    return stage;
                }
                if (!state.result->ok)
                    return kUnassignedReg;
                return lowerSlotValue(state, pipe.source_slot, pipe.source_type);
            },
            [&](const hir::HirPipeCurrent &current) {
                return lowerSlotValue(state, current.slot, current.type);
            },
            [&](const hir::HirCleanup &cleanup) -> std::uint16_t {
                for (const auto cleanup_expr : cleanup.exprs) {
                    (void)lowerOperand(state, cleanup_expr);
                    if (!state.result->ok)
                        return kUnassignedReg;
                }
                return kUnassignedReg;
            },
            [&](const auto &) -> std::uint16_t {
                // An unhandled HIR node must fail the lowering instead of
                // silently producing an unassigned register, which callers
                // could mistake for "no value" and skip a store or return.
                state.result->ok      = false;
                state.result->message = "unsupported HIR expression kind " +
                                        std::to_string(static_cast<unsigned>(hir::exprKind(expr))) +
                                        " in v2 lowering";
                return kUnassignedReg;
            },
        });
    if (reg != kUnassignedReg)
        state.regs[id] = reg;
    return reg;
}

auto prepareFunctionRegisters(LowerState &state) -> bool {
    std::size_t nextRegister = 0;
    if (state.hirFn->param_names.size() != state.hirFn->params.size()) {
        state.result->ok      = false;
        state.result->message = "HIR parameter names and types disagree in v2 lowering";
        return false;
    }

    // A function that returns an aggregate by value reserves register 0 for
    // the caller-provided hidden result pointer, ahead of its parameters.
    if (isAggregateType(state.types, state.hirFn->return_type)) {
        state.fn->returnsAggregate = true;
        state.sretReg              = 0;
        state.fn->regTypes.push(ValueType::Ptr);
        nextRegister = 1;
    } else if (state.types.kindOf(state.hirFn->return_type) == types::TypeKind::Slice) {
        // A slice-returning function hands back its `{ptr, len}` pair in the
        // return register and the one right after it.
        state.fn->returnsSlice = true;
    }

    state.paramRegs.resize(state.hirFn->params.size(), kUnassignedReg);
    for (std::size_t i = 0; i < state.hirFn->params.size(); ++i) {
        const auto type  = state.hirFn->params[i];
        const auto width = valueRegisterWidth(state.types, type);
        if (nextRegister + width > 0xFFFFU) {
            state.result->ok      = false;
            state.result->message = "v2 function parameter registers exceed 65535";
            return false;
        }
        state.paramRegs[i] = u16(nextRegister, state.result);
        state.fn->regTypes.push(valueTypeOf(state.types, type));
        if (width == 2U)
            state.fn->regTypes.push(ValueType::I64);
        nextRegister += width;
    }
    state.fn->paramCount = u16(nextRegister, state.result);
    if (!state.result->ok)
        return false;

    std::size_t slotCount = 0;
    auto includeSlot      = [&](hir::HirSlotId slot) {
        if (slot == hir::kInvalidHirSlot || slot >= 0xFFFFU) {
            state.result->ok      = false;
            state.result->message = "HIR slot index exceeds v2 register limit";
            return false;
        }
        slotCount = std::max(slotCount, static_cast<std::size_t>(slot) + 1U);
        return true;
    };
    for (const auto slot : state.hirFn->param_slots)
        if (!includeSlot(slot))
            return false;
    for (const auto &block : state.hirFn->blocks) {
        for (const auto instId : block.insts) {
            const auto &expr = state.hir.getExpr(instId);
            if (const auto *alloca = std::get_if<hir::HirSlotAlloca>(&expr)) {
                if (!includeSlot(alloca->slot))
                    return false;
            } else if (const auto *store = std::get_if<hir::HirSlotStore>(&expr)) {
                if (!includeSlot(store->slot))
                    return false;
            } else if (const auto *load = std::get_if<hir::HirSlotLoad>(&expr)) {
                if (!includeSlot(load->slot))
                    return false;
            } else if (const auto *addr = std::get_if<hir::HirSlotAddr>(&expr)) {
                if (!includeSlot(addr->slot))
                    return false;
            }
        }
    }
    state.slotRegs.resize(slotCount, kUnassignedReg);
    state.slotWidths.resize(slotCount, 0U);
    state.slotTypes.resize(slotCount, types::kInvalidType);
    state.slotAlignments.resize(slotCount, 0U);
    state.slotBacked.resize(slotCount, 0U);

    for (const auto &block : state.hirFn->blocks) {
        for (const auto instId : block.insts) {
            const auto *alloca = std::get_if<hir::HirSlotAlloca>(&state.hir.getExpr(instId));
            if (alloca == nullptr || alloca->slot >= state.slotWidths.size())
                continue;
            const auto width =
                static_cast<std::uint8_t>(valueRegisterWidth(state.types, alloca->type));
            if (state.slotWidths[alloca->slot] != 0U && state.slotWidths[alloca->slot] != width) {
                state.result->ok      = false;
                state.result->message = "inconsistent HIR slot widths in v2 lowering";
                return false;
            }
            state.slotWidths[alloca->slot] = width;
            state.slotTypes[alloca->slot]  = alloca->type;
            state.slotAlignments[alloca->slot] =
                std::max(state.slotAlignments[alloca->slot], alloca->alignmentBytes);
        }
    }
    for (std::size_t i = 0; i < state.hirFn->param_slots.size() && i < state.hirFn->params.size();
         ++i) {
        const auto slot = state.hirFn->param_slots[i];
        if (slot >= state.slotRegs.size())
            continue;
        const auto width = valueRegisterWidth(state.types, state.hirFn->params[i]);
        if (state.slotWidths[slot] == 0U)
            state.slotWidths[slot] = static_cast<std::uint8_t>(width);
        if (state.slotWidths[slot] != width) {
            state.result->ok      = false;
            state.result->message = "parameter slot width disagrees with its v2 ABI";
            return false;
        }
        if (state.slotTypes[slot] == types::kInvalidType)
            state.slotTypes[slot] = state.hirFn->params[i];
        // Slices and arrays keep their value in the incoming register pair (a
        // {ptr, len} pair or a base pointer), so they can share it. Scalars and
        // aggregates are memory-backed and need their own storage, so their
        // slot register is allocated below and the prologue fills it.
        if (!isMemorySlotType(state.types, state.hirFn->params[i]))
            state.slotRegs[slot] = state.paramRegs[i];
    }

    // Register-backed values need storage when their binding has an explicit
    // alignment request.
    for (std::size_t slot = 0; slot < state.slotTypes.size(); ++slot) {
        const bool alignedRegisterValue = state.slotAlignments[slot] != 0U;
        if (isMemorySlotType(state.types, state.slotTypes[slot]) || alignedRegisterValue)
            state.slotBacked[slot] = 1U;
    }

    for (std::size_t slot = 0; slot < state.slotRegs.size(); ++slot) {
        if (state.slotWidths[slot] == 0U)
            state.slotWidths[slot] = 1U;
        if (state.slotRegs[slot] != kUnassignedReg)
            continue;
        const auto width = state.slotBacked[slot] != 0U
                               ? std::size_t{1}
                               : static_cast<std::size_t>(state.slotWidths[slot]);
        if (nextRegister + width > 0xFFFFU) {
            state.result->ok      = false;
            state.result->message = "v2 function local registers exceed 65535";
            return false;
        }
        state.slotRegs[slot] = u16(nextRegister, state.result);
        nextRegister += width;
    }

    state.fn->regCount = u16(std::max<std::size_t>(nextRegister, 1U), state.result);
    if (!state.result->ok)
        return false;
    while (state.fn->regTypes.size() < state.fn->regCount)
        state.fn->regTypes.push(ValueType::I64);
    // A memory-backed slot register holds the storage address, not the value,
    // so record it as a pointer in the IR metadata.
    for (std::size_t slot = 0; slot < state.slotBacked.size(); ++slot) {
        if (state.slotBacked[slot] == 0U || state.slotRegs[slot] == kUnassignedReg)
            continue;
        const auto reg = static_cast<std::size_t>(state.slotRegs[slot]);
        if (reg < state.fn->regTypes.size())
            state.fn->regTypes[reg] = ValueType::Ptr;
    }
    return true;
}

auto reachableFunctions(const hir::HirModule &hir, const memory::StringInterner &interner)
    -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> reachable(hir.getFnCount(), 0U);
    std::vector<hir::HirFunctionId> pending;

    const auto enqueueFunction = [&](const hir::HirFunctionId function_id) {
        if (function_id >= hir.getFnCount() || reachable[function_id] != 0U ||
            !hir.getFn(function_id).hasBody)
            return;
        reachable[function_id] = 1U;
        pending.push_back(function_id);
    };
    const auto enqueueDirectCall = [&](const hir::HirFunctionId function_id) {
        if (function_id >= hir.getFnCount())
            return;
        const auto linkage = interner.lookup(hir.getFn(function_id).name);
        if (startsWith(linkage, "std.io.console.print"))
            return;
        enqueueFunction(function_id);
    };

    // Preserve the VM's existing behavior for project functions, then include
    // only stdlib bodies selected by their calls or dynamic-dispatch tables.
    for (std::size_t i = 0; i < hir.getFnCount(); ++i) {
        const auto &function = hir.getFn(i);
        if (isUserFunction(interner.lookup(function.name), function))
            enqueueFunction(static_cast<hir::HirFunctionId>(i));
    }

    const auto enqueueVTable = [&](const memory::InternedId vtable_name) {
        for (std::size_t i = 0; i < hir.getVTableCount(); ++i) {
            const auto &vtable = hir.getVTable(i);
            if (vtable.name != vtable_name)
                continue;
            for (const auto function_id : vtable.slots)
                enqueueFunction(function_id);
            return;
        }
    };

    std::vector<std::uint8_t> visitedExpressions(hir.exprCount(), 0U);
    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
        const auto &function = hir.getFn(pending[cursor]);
        const auto scanExpression = [&](auto &&self, const hir::HirExprId expression_id) -> void {
            if (expression_id == hir::kInvalidHirExpr || expression_id >= hir.exprCount() ||
                visitedExpressions[expression_id] != 0U)
                return;
            visitedExpressions[expression_id] = 1U;
            const auto &expression = hir.getExpr(expression_id);
            std::visit(
                [&](const auto &node) {
                    using T = std::decay_t<decltype(node)>;
                    if constexpr (std::is_same_v<T, hir::HirBinary>) {
                        self(self, node.lhs);
                        self(self, node.rhs);
                    } else if constexpr (std::is_same_v<T, hir::HirUnary>) {
                        self(self, node.operand);
                    } else if constexpr (std::is_same_v<T, hir::HirMakeSome> ||
                                         std::is_same_v<T, hir::HirCast> ||
                                         std::is_same_v<T, hir::HirUnionCast> ||
                                         std::is_same_v<T, hir::HirUnionCheck> ||
                                         std::is_same_v<T, hir::HirOpaqueCast> ||
                                         std::is_same_v<T, hir::HirOpaqueCheck>) {
                        self(self, node.value);
                    } else if constexpr (std::is_same_v<T, hir::HirLet>) {
                        self(self, node.init);
                    } else if constexpr (std::is_same_v<T, hir::HirCall>) {
                        enqueueDirectCall(node.resolved_fn);
                        self(self, node.callee);
                        for (const auto argument : node.args)
                            self(self, argument);
                    } else if constexpr (std::is_same_v<T, hir::HirRet>) {
                        self(self, node.value);
                    } else if constexpr (std::is_same_v<T, hir::HirBranch>) {
                        self(self, node.cond);
                    } else if constexpr (std::is_same_v<T, hir::HirPhi>) {
                        for (const auto incoming : node.incoming)
                            self(self, incoming);
                    } else if constexpr (std::is_same_v<T, hir::HirAssign>) {
                        self(self, node.target);
                        self(self, node.value);
                    } else if constexpr (std::is_same_v<T, hir::HirIndex>) {
                        self(self, node.object);
                        self(self, node.index);
                    } else if constexpr (std::is_same_v<T, hir::HirField>) {
                        self(self, node.object);
                    } else if constexpr (std::is_same_v<T, hir::HirStructLiteral>) {
                        for (const auto value : node.values)
                            self(self, value);
                    } else if constexpr (std::is_same_v<T, hir::HirArrayLiteral>) {
                        for (const auto value : node.elements)
                            self(self, value);
                    } else if constexpr (std::is_same_v<T, hir::HirSlotStore>) {
                        self(self, node.value);
                    } else if constexpr (std::is_same_v<T, hir::HirMakeSlice>) {
                        self(self, node.object);
                        self(self, node.lo);
                        self(self, node.hi);
                    } else if constexpr (std::is_same_v<T, hir::HirLayoutIntrinsic>) {
                        self(self, node.operand);
                    } else if constexpr (std::is_same_v<T, hir::HirStateTailCall>) {
                        enqueueDirectCall(node.call.resolved_fn);
                        self(self, node.call.callee);
                        for (const auto argument : node.call.args)
                            self(self, argument);
                    } else if constexpr (std::is_same_v<T, hir::HirCleanup>) {
                        for (const auto cleanup : node.exprs)
                            self(self, cleanup);
                    } else if constexpr (std::is_same_v<T, hir::HirMakeDyn>) {
                        enqueueVTable(node.vtable_name);
                        self(self, node.value);
                    } else if constexpr (std::is_same_v<T, hir::HirDynCall>) {
                        enqueueVTable(node.vtable_name);
                        self(self, node.receiver);
                        for (const auto argument : node.args)
                            self(self, argument);
                    } else if constexpr (std::is_same_v<T, hir::HirMakeOpaque>) {
                        self(self, node.value);
                    } else if constexpr (std::is_same_v<T, hir::HirPipe>) {
                        self(self, node.stage);
                    }
                },
                expression);
        };

        for (const auto &block : function.blocks) {
            for (const auto expression_id : block.insts)
                scanExpression(scanExpression, expression_id);
            scanExpression(scanExpression, block.terminator);
        }
    }

    return reachable;
}

} // namespace

auto lowerModule(const hir::HirModule &hir, const memory::StringInterner &interner,
                 const types::TypeIntern &types, memory::Arena &arena, Module &out) -> LowerResult {
    LowerResult result{true, {}};

    const auto reachable = reachableFunctions(hir, interner);
    std::vector<std::uint16_t> functionIndices(hir.getFnCount(), kUnassignedReg);
    std::size_t nextFunctionIndex = out.functions.size();
    for (std::size_t i = 0; i < reachable.size(); ++i) {
        if (reachable[i] == 0U)
            continue;
        functionIndices[i] = u16(nextFunctionIndex++, &result);
        if (!result.ok)
            return result;
    }

    std::vector<std::uint16_t> adapterIndices(hir.getFnCount(), kUnassignedReg);
    for (std::size_t tableIndex = 0; tableIndex < hir.getVTableCount(); ++tableIndex) {
        const auto &vtable = hir.getVTable(tableIndex);
        for (const auto function_id : vtable.slots) {
            if (function_id >= reachable.size() || reachable[function_id] == 0U ||
                adapterIndices[function_id] != kUnassignedReg)
                continue;
            const auto &target = hir.getFn(function_id);
            if (target.params.empty() || isAggregateType(types, target.return_type))
                continue;
            const auto receiverKind = types.kindOf(target.params[0]);
            if (receiverKind != types::TypeKind::Slice && receiverKind != types::TypeKind::Bool &&
                receiverKind != types::TypeKind::Int &&
                receiverKind != types::TypeKind::Float)
                continue;
            adapterIndices[function_id] = u16(nextFunctionIndex++, &result);
            if (!result.ok)
                return result;
        }
    }

    for (std::size_t i = out.functions.size(); i < nextFunctionIndex; ++i)
        (void)out.functions.emplace(arena);

    for (std::size_t i = 0; i < hir.getFnCount(); ++i) {
        const auto &hirFn  = hir.getFn(i);
        const auto linkage = interner.lookup(hirFn.name);
        if (reachable[i] == 0U) {
            if (!hirFn.hasBody && hirFn.isForeignC)
                (void)findOrAddExtern(out, sourceName(linkage), &result);
            continue;
        }

        auto &fn      = out.functions[functionIndices[i]];
        fn.name       = sourceName(linkage);
        fn.returnType = valueTypeOf(types, hirFn.return_type);

        LowerState state{arena,
                         hir,
                         interner,
                         types,
                         out,
                         functionIndices,
                         adapterIndices,
                         &hirFn,
                         &fn,
                         memory::DynArray<std::uint16_t>(arena),
                         memory::DynArray<std::uint16_t>(arena),
                         memory::DynArray<std::uint16_t>(arena),
                         memory::DynArray<std::uint8_t>(arena),
                         memory::DynArray<types::TypeId>(arena),
                         memory::DynArray<std::uint32_t>(arena),
                         memory::DynArray<std::uint8_t>(arena),
                         kUnassignedReg,
                         &result};
        state.regs.resize(hir.exprCount(), kUnassignedReg);
        if (!prepareFunctionRegisters(state))
            return result;

        // Allocate storage for every memory-backed slot once, at function
        // entry. A slot declared inside a loop would otherwise allocate on
        // every iteration, growing the frame arena without bound.
        for (std::size_t slot = 0; slot < state.slotBacked.size(); ++slot) {
            if (state.slotBacked[slot] == 0U || state.slotTypes[slot] == types::kInvalidType)
                continue;
            const auto destination = slotRegister(state, static_cast<hir::HirSlotId>(slot));
            if (destination == kUnassignedReg) {
                result.ok      = false;
                result.message = "invalid memory HIR slot in v2 lowering";
                return result;
            }
            const auto size = typeSize(state.types, state.slotTypes[slot]);
            if (size == 0U || size > 0xFFFFU) {
                result.ok      = false;
                result.message = "unsupported slot size in v2 lowering";
                return result;
            }
            const auto count = newTypedReg(state, ValueType::I64);
            emit(state, Instr::withImm(Op::LoadConstI64, count, static_cast<std::uint16_t>(size)));
            std::size_t slotAlignment = 1U;
            (void)layoutSize(state.types, state.slotTypes[slot], slotAlignment);
            slotAlignment =
                std::max(slotAlignment, static_cast<std::size_t>(state.slotAlignments[slot]));
            slotAlignment = std::max(slotAlignment, std::size_t{8});
            const auto align = emitI64Constant(state, static_cast<std::int64_t>(slotAlignment));
            emit(state, Instr::simple(Op::AllocBytes, destination, count, align));
        }

        struct PendingTarget {
            std::size_t pc       = 0;
            hir::HirBlockId block = hir::kInvalidHirBlock;
            bool falseBranch     = false;
        };
        std::vector<std::size_t> blockPcs(hirFn.blocks.size(), fn.body.size());
        std::vector<PendingTarget> pendingTargets;

        for (std::size_t blockIndex = 0; blockIndex < hirFn.blocks.size(); ++blockIndex) {
            const auto &block    = hirFn.blocks[blockIndex];
            blockPcs[blockIndex] = fn.body.size();
            for (const auto instId : block.insts) {
                const auto &expr = hir.getExpr(instId);
                if (const auto *alloca = std::get_if<hir::HirSlotAlloca>(&expr)) {
                    // Storage for every memory-backed slot was reserved once in
                    // the function prologue, so the HIR alloca is a no-op here.
                    (void)alloca;
                    continue;
                } else if (std::holds_alternative<hir::HirSlotStore>(expr)) {
                    const auto &store = std::get<hir::HirSlotStore>(expr);
                    if (lowerSlotStore(state, store) == kUnassignedReg && !result.ok)
                        return result;
                } else {
                    (void)lowerOperand(state, instId);
                }
            }
            if (block.terminator == hir::kInvalidHirExpr)
                continue;
            const auto &terminator = hir.getExpr(block.terminator);
            if (const auto *ret = std::get_if<hir::HirRet>(&terminator)) {
                if (state.fn->returnsAggregate) {
                    // Copy the returned aggregate into the caller-provided
                    // result slot. The VM return value is unused in this ABI.
                    if (ret->value != hir::kInvalidHirExpr) {
                        const auto source = aggregateAddress(state, ret->value);
                        if (source == kUnassignedReg) {
                            if (!result.ok)
                                return result;
                            result.ok      = false;
                            result.message = "unsupported aggregate return in v2 lowering for " +
                                             std::string(linkage);
                            return result;
                        }
                        copyBytes(state, state.sretReg, source,
                                  typeSize(state.types, hirFn.return_type));
                    }
                    emit(state, Instr{Op::Ret, static_cast<std::uint16_t>(state.sretReg), 0, 0, 0});
                    continue;
                }
                const auto value =
                    ret->value == hir::kInvalidHirExpr ? 0 : lowerOperand(state, ret->value);
                if (value == kUnassignedReg) {
                    if (!result.ok)
                        return result;
                    result.ok = false;
                    result.message =
                        "unsupported return value in v2 lowering for " + std::string(linkage);
                    return result;
                }
                emit(state, Instr{Op::Ret, static_cast<std::uint16_t>(value), 0, 0, 0});
            } else if (const auto *jump = std::get_if<hir::HirJump>(&terminator)) {
                if (jump->target >= hirFn.blocks.size()) {
                    result.ok      = false;
                    result.message = "invalid HIR jump target in v2 lowering";
                    return result;
                }
                pendingTargets.push_back({fn.body.size(), jump->target, false});
                emit(state, Instr{Op::Jump, 0, 0, 0, 0});
            } else if (const auto *branch = std::get_if<hir::HirBranch>(&terminator)) {
                const auto cond = lowerOperand(state, branch->cond);
                if (cond == kUnassignedReg && !result.ok)
                    return result;
                if (cond == kUnassignedReg || branch->then_block >= hirFn.blocks.size() ||
                    branch->else_block >= hirFn.blocks.size()) {
                    result.ok      = false;
                    result.message = "invalid HIR branch in v2 lowering";
                    return result;
                }
                pendingTargets.push_back({fn.body.size(), branch->then_block, false});
                pendingTargets.push_back({fn.body.size(), branch->else_block, true});
                emit(state, Instr{Op::Branch2, 0, cond, 0, 0, 0});
            } else if (const auto *tail = std::get_if<hir::HirStateTailCall>(&terminator)) {
                if (lowerCall(state, tail->call, true) == kUnassignedReg || !result.ok)
                    return result;
            } else if (const auto *panic = std::get_if<hir::HirRuntimePanic>(&terminator)) {
                if (panic->code > 0xFFFFU) {
                    result.ok      = false;
                    result.message = "runtime panic code does not fit the v2 opcode immediate";
                    return result;
                }
                // `R10003` keeps its dedicated op for readability; every other
                // runtime code lowers to the generic `Panic` op, whose `imm`
                // carries the code.
                if (panic->code == 10003U)
                    emit(state, Instr{Op::NullDeref, 0, 0, 0, 0});
                else
                    emit(state, Instr{Op::Panic, 0, 0, 0,
                                      static_cast<uint16_t>(panic->code)});
            }
        }

        for (const auto &pending : pendingTargets) {
            if (pending.block >= blockPcs.size() || blockPcs[pending.block] >= fn.body.size()) {
                result.ok      = false;
                result.message = "unresolved HIR block target in v2 lowering";
                return result;
            }
            auto &instruction = fn.body[pending.pc];
            if (pending.falseBranch)
                instruction.d = u16(blockPcs[pending.block], &result);
            else
                instruction.imm = u16(blockPcs[pending.block], &result);
        }
    }

    for (std::size_t function_id = 0; function_id < adapterIndices.size(); ++function_id) {
        const auto adapterIndex = adapterIndices[function_id];
        if (adapterIndex == kUnassignedReg)
            continue;

        const auto &target = hir.getFn(static_cast<hir::HirFunctionId>(function_id));
        if (target.params.empty() || target.params.size() != target.param_names.size()) {
            result.ok      = false;
            result.message = "invalid dynamic receiver signature in v2 lowering";
            return result;
        }

        auto &adapter      = out.functions[adapterIndex];
        adapter.name       = sourceName(interner.lookup(target.name));
        adapter.returnType = valueTypeOf(types, target.return_type);
        const bool returnsSlice =
            types.kindOf(target.return_type) == types::TypeKind::Slice;
        adapter.returnsSlice = returnsSlice;

        std::size_t explicitArgCount = 0U;
        for (std::size_t param = 1U; param < target.params.size(); ++param)
            explicitArgCount += valueRegisterWidth(types, target.params[param]);
        const auto receiverWidth = valueRegisterWidth(types, target.params[0]);
        const auto paramCount    = 1U + explicitArgCount;
        const auto callArgCount  = receiverWidth + explicitArgCount;
        const bool receiverIsSlice =
            types.kindOf(target.params[0]) == types::TypeKind::Slice;
        const auto scratchCount = receiverIsSlice ? 1U : 0U;
        const auto argBase      = paramCount;
        const auto returnReg    = argBase + callArgCount + scratchCount;
        // A slice-returning method returns a `{ptr, len}` pair, so reserve the
        // register after the return register for the length half.
        const auto regCount     = returnReg + (returnsSlice ? 2U : 1U);
        if (paramCount > 0xFFFFU || regCount > 0xFFFFU || callArgCount > 0xFFFFU) {
            result.ok      = false;
            result.message = "dynamic adapter register range exceeds v2 limits";
            return result;
        }

        adapter.regTypes.push(ValueType::Ptr);
        for (std::size_t param = 1U; param < target.params.size(); ++param) {
            const auto type  = target.params[param];
            const auto width = valueRegisterWidth(types, type);
            adapter.regTypes.push(valueTypeOf(types, type));
            if (width == 2U)
                adapter.regTypes.push(ValueType::I64);
        }
        adapter.paramCount = static_cast<std::uint16_t>(paramCount);

        adapter.regTypes.push(valueTypeOf(types, target.params[0]));
        if (receiverWidth == 2U)
            adapter.regTypes.push(ValueType::I64);
        for (std::size_t param = 1U; param < target.params.size(); ++param) {
            const auto type  = target.params[param];
            const auto width = valueRegisterWidth(types, type);
            adapter.regTypes.push(valueTypeOf(types, type));
            if (width == 2U)
                adapter.regTypes.push(ValueType::I64);
        }
        if (receiverIsSlice)
            adapter.regTypes.push(ValueType::Ptr);
        adapter.regTypes.push(adapter.returnType);
        if (returnsSlice)
            adapter.regTypes.push(ValueType::I64);
        adapter.regCount = static_cast<std::uint16_t>(regCount);

        if (receiverIsSlice) {
            adapter.body.push(Instr::simple(Op::LoadI64, static_cast<std::uint16_t>(argBase), 0));
            const auto lengthAddress =
                static_cast<std::uint16_t>(argBase + callArgCount);
            adapter.body.push(
                Instr{Op::FieldPtr, lengthAddress, 0, 0, 8U});
            adapter.body.push(
                Instr::simple(Op::LoadI64, static_cast<std::uint16_t>(argBase + 1U),
                              lengthAddress));
        } else {
            adapter.body.push(
                Instr::simple(Op::LoadI64, static_cast<std::uint16_t>(argBase), 0));
        }

        std::size_t sourceReg = 1U;
        std::size_t destReg   = argBase + receiverWidth;
        for (std::size_t param = 1U; param < target.params.size(); ++param) {
            const auto width = valueRegisterWidth(types, target.params[param]);
            for (std::size_t word = 0; word < width; ++word)
                adapter.body.push(
                    Instr::simple(Op::Move, static_cast<std::uint16_t>(destReg + word),
                                  static_cast<std::uint16_t>(sourceReg + word)));
            sourceReg += width;
            destReg += width;
        }

        adapter.body.push(Instr::callRange(
            static_cast<std::uint16_t>(returnReg), static_cast<std::uint16_t>(argBase),
            static_cast<std::uint16_t>(callArgCount), functionIndices[function_id]));
        adapter.body.push(
            Instr{Op::Ret, static_cast<std::uint16_t>(returnReg), 0, 0, 0});
    }

    return result;
}

} // namespace zith::vm
