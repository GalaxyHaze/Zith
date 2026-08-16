#include "common/sir/flat/flat.hpp"

#include "cache/cache-buffer.hpp"
#include "common/memory/arena.hpp"
#include "common/memory/dyn-array.hpp"
#include "common/memory/flat-map.hpp"
#include "common/memory/result.hpp"
#include "common/memory/string-interner.hpp"
#include "common/sir/sir.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace toolkit::sir::flat {

using common::memory::Error;
using common::memory::FlatMap;
using toolkit::cache::ByteReader;
using toolkit::cache::ByteWriter;

namespace {

constexpr std::string_view kFormatMagic = "ZCTSF1";
constexpr std::uint8_t kFormatVersion = 1;
constexpr std::uint32_t kMaxCount = 1u << 20u;

[[nodiscard]] bool knownKind(TypeKind kind) noexcept {
    switch (kind) {
    case TypeKind::Void:
    case TypeKind::Bool:
    case TypeKind::Char:
    case TypeKind::I1:
    case TypeKind::I8:
    case TypeKind::I16:
    case TypeKind::I32:
    case TypeKind::I64:
    case TypeKind::F32:
    case TypeKind::F64:
    case TypeKind::Array:
    case TypeKind::Slice:
    case TypeKind::Pointer:
    case TypeKind::UserDefined:
        return true;
    }
    return false;
}

[[nodiscard]] bool knownOp(FlatOp value) noexcept {
    const auto raw = static_cast<std::uint8_t>(value);
    return raw >= static_cast<std::uint8_t>(FlatOp::Constant) &&
           raw <= static_cast<std::uint8_t>(FlatOp::CondBranch);
}

[[nodiscard]] FlatOp mapOp(Opcode opcode) noexcept {
    return static_cast<FlatOp>(opcode);
}

[[nodiscard]] bool isTerminator(FlatOp op) noexcept {
    return op == FlatOp::Return || op == FlatOp::Branch || op == FlatOp::CondBranch;
}

[[nodiscard]] bool isBinary(FlatOp op) noexcept {
    switch (op) {
    case FlatOp::Add:
    case FlatOp::Sub:
    case FlatOp::Mul:
    case FlatOp::Div:
    case FlatOp::Rem:
    case FlatOp::BitAnd:
    case FlatOp::BitOr:
    case FlatOp::BitXor:
    case FlatOp::Shl:
    case FlatOp::Shr:
    case FlatOp::Eq:
    case FlatOp::Ne:
    case FlatOp::Lt:
    case FlatOp::Le:
    case FlatOp::Gt:
    case FlatOp::Ge:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool isBitwiseOp(FlatOp op) noexcept {
    return op == FlatOp::BitAnd || op == FlatOp::BitOr || op == FlatOp::BitXor ||
           op == FlatOp::Shl || op == FlatOp::Shr;
}

[[nodiscard]] bool isComparisonOp(FlatOp op) noexcept {
    return op == FlatOp::Eq || op == FlatOp::Ne || op == FlatOp::Lt ||
           op == FlatOp::Le || op == FlatOp::Gt || op == FlatOp::Ge;
}

[[nodiscard]] bool isScalar(TypeKind kind) noexcept {
    return kind == TypeKind::Bool || kind == TypeKind::Char || kind == TypeKind::I1 ||
           kind == TypeKind::I8 || kind == TypeKind::I16 || kind == TypeKind::I32 ||
           kind == TypeKind::I64 || kind == TypeKind::F32 || kind == TypeKind::F64;
}

[[nodiscard]] bool isIntegerKind(TypeKind kind) noexcept {
    return kind == TypeKind::Bool || kind == TypeKind::Char || kind == TypeKind::I1 ||
           kind == TypeKind::I8 || kind == TypeKind::I16 || kind == TypeKind::I32 ||
           kind == TypeKind::I64;
}

[[nodiscard]] bool readCount(ByteReader &reader, std::uint32_t &count) {
    if (!reader.readU32(count))
        return false;
    return count <= kMaxCount;
}

[[nodiscard]] bool readStringInto(ByteReader &reader, std::string &value) {
    return reader.readString(value);
}

[[nodiscard]] InternedId internFlatName(const StringInterner &source, StringInterner &target,
                                       InternedId name) noexcept {
    if (name == 0)
        return 0;
    const std::string_view text = source.lookup(name);
    return text.empty() ? 0 : target.intern(text);
}

[[nodiscard]] bool internFlatType(FlatModule &module, const StringInterner &sourceInterner,
                                  const Type &source, std::uint32_t *out) {
    const InternedId flatName = internFlatName(sourceInterner, *module.interner, source.nameId);
    for (std::uint32_t index = 0; index < module.types.size(); ++index) {
        const FlatType &existing = module.types[index];
        if (existing.kind != source.kind || existing.nameId != flatName ||
            existing.arrayLength != source.arrayLength)
            continue;
        if (source.elementType == nullptr && source.pointeeType == nullptr &&
            existing.elementType == invalidIndex && existing.pointeeType == invalidIndex) {
            *out = index;
            return true;
        }
        if (source.elementType != nullptr && existing.elementType != invalidIndex) {
            const FlatType &child = module.types[existing.elementType];
            const InternedId childName =
                internFlatName(sourceInterner, *module.interner,
                               source.elementType->nameId);
            if (child.kind == source.elementType->kind && child.nameId == childName &&
                child.arrayLength == source.elementType->arrayLength) {
                *out = index;
                return true;
            }
        }
        if (source.pointeeType != nullptr && existing.pointeeType != invalidIndex) {
            const FlatType &child = module.types[existing.pointeeType];
            const InternedId childName =
                internFlatName(sourceInterner, *module.interner,
                               source.pointeeType->nameId);
            if (child.kind == source.pointeeType->kind && child.nameId == childName &&
                child.arrayLength == source.pointeeType->arrayLength) {
                *out = index;
                return true;
            }
        }
    }

    FlatType created{};
    created.kind = source.kind;
    created.nameId = flatName;
    created.arrayLength = source.arrayLength;
    if (source.elementType) {
        if (!internFlatType(module, sourceInterner, *source.elementType,
                            &created.elementType))
            return false;
    }
    if (source.pointeeType) {
        if (!internFlatType(module, sourceInterner, *source.pointeeType,
                            &created.pointeeType))
            return false;
    }

    for (std::uint32_t index = 0; index < module.types.size(); ++index) {
        const FlatType &existing = module.types[index];
        if (existing.kind == created.kind && existing.nameId == created.nameId &&
            existing.arrayLength == created.arrayLength &&
            existing.elementType == created.elementType &&
            existing.pointeeType == created.pointeeType) {
            *out = index;
            return true;
        }
    }
    module.types.push(created);
    *out = static_cast<std::uint32_t>(module.types.size() - 1);
    return true;
}

[[nodiscard]] FlatFunction &newFlatFunction(FlatModule &module) {
    auto *flat = module.arena->make<FlatFunction>();
    if (!flat)
        throw std::runtime_error("allocating flat function");
    flat->paramTypes = module.arena->make<DynArray<std::uint32_t>>(*module.arena);
    flat->paramNames = module.arena->make<DynArray<InternedId>>(*module.arena);
    flat->variables = module.arena->make<DynArray<FlatVariable>>(*module.arena);
    flat->blocks = module.arena->make<DynArray<FlatBlock>>(*module.arena);
    flat->values = module.arena->make<DynArray<FlatValue>>(*module.arena);
    if (!flat->paramTypes || !flat->paramNames || !flat->variables ||
        !flat->blocks || !flat->values)
        throw std::runtime_error("allocating flat function arrays");
    module.functions.push(flat);
    return *flat;
}

[[nodiscard]] bool sizedArrayWrite(ByteWriter &writer, std::uint32_t size) {
    if (size > kMaxCount)
        return false;
    writer.writeU32(size);
    return true;
}

} // namespace

Result<FlatModule> flattenModule(Module &module) {
    if (!module.arena || !module.interner)
        return Error{"sir module has a missing arena or interner"};

    auto sourceVerify = verify(module);
    if (sourceVerify.isError())
        return sourceVerify.error();

    FlatModule flat(*module.arena);
    flat.name = module.interner->intern(module.nameView());

    try {
        for (auto *sourceType : module.types) {
            if (sourceType) {
                std::uint32_t ignoredType = invalidIndex;
                if (!internFlatType(flat, *module.interner, *sourceType, &ignoredType))
                    return Error{"interning flat declared type"};
            }
        }
        for (auto *sourceFunction : module.functions) {
            if (!sourceFunction)
                return Error{"sir module contains a null function"};
            FlatFunction &flatFunction = newFlatFunction(flat);
            flatFunction.name =
                internFlatName(*module.interner, *flat.interner, sourceFunction->name);
            if (!internFlatType(flat, *module.interner, sourceFunction->returnType,
                                &flatFunction.returnType))
                return Error{"interning flat return type"};

            FlatMap<const Variable *, std::uint32_t> variableIndex;
            FlatMap<const BlockData *, std::uint32_t> blockIndex;
            FlatMap<const Value *, std::uint32_t> valueIndex;

            for (std::size_t index = 0; index < sourceFunction->variables.size(); ++index) {
                const Variable *variable = sourceFunction->variables[index];
                if (!variable)
                    return Error{"sir function contains a null variable"};
                const std::uint32_t variableNumber =
                    static_cast<std::uint32_t>(flatFunction.variables->size());
                FlatVariable flatVariable{};
                flatVariable.name =
                    internFlatName(*module.interner, *flat.interner, variable->name);
                flatVariable.mutability = variable->mutability;
                if (!internFlatType(flat, *module.interner, variable->type,
                                    &flatVariable.type))
                    return Error{"interning flat variable type"};
                flatFunction.variables->push(flatVariable);
                variableIndex.insert(variable, variableNumber);

                if (index < sourceFunction->params.size() && sourceFunction->params[index] == variable) {
                    flatFunction.paramTypes->push(flatVariable.type);
                    flatFunction.paramNames->push(flatVariable.name);
                }
            }

            for (auto *sourceBlock : sourceFunction->blocks) {
                if (!sourceBlock)
                    return Error{"sir function contains a null block"};
                const std::uint32_t blockNumber =
                    static_cast<std::uint32_t>(flatFunction.blocks->size());
                FlatBlock flatBlock{};
                flatBlock.valueIndices =
                    module.arena->make<DynArray<std::uint32_t>>(*module.arena);
                if (!flatBlock.valueIndices)
                    return Error{"allocating flat block values"};
                flatFunction.blocks->push(flatBlock);
                blockIndex.insert(sourceBlock, blockNumber);
                if (sourceBlock == sourceFunction->baseBlock)
                    flatFunction.baseBlock = blockNumber;
            }

            for (auto *sourceValue : sourceFunction->values) {
                if (!sourceValue)
                    return Error{"sir function contains a null value"};
                const std::uint32_t valueNumber =
                    static_cast<std::uint32_t>(flatFunction.values->size());
                valueIndex.insert(sourceValue, valueNumber);
                if (!sourceValue->block) {
                    // A well-formed SIR module keeps every value in a block; tolerate only
                    // source values that verify earlier because flattening is index-driven.
                    continue;
                }

                FlatValue value{};
                value.op = mapOp(sourceValue->opcode);
                if (!internFlatType(flat, *module.interner, sourceValue->type,
                                    &value.typeIndex))
                    return Error{"interning flat value type"};
                if (sourceValue->variable) {
                    auto *existing = variableIndex.get(sourceValue->variable);
                    if (!existing)
                        return Error{"flat value references a missing variable"};
                    value.variableIndex = *existing;
                }
                if (sourceValue->left) {
                    auto *existing = valueIndex.get(sourceValue->left);
                    if (!existing)
                        return Error{"flat value references an out-of-order operand"};
                    value.left = *existing;
                }
                if (sourceValue->right) {
                    auto *existing = valueIndex.get(sourceValue->right);
                    if (!existing)
                        return Error{"flat value references an out-of-order operand"};
                    value.right = *existing;
                }
                if (sourceValue->address) {
                    auto *existing = valueIndex.get(sourceValue->address);
                    if (!existing)
                        return Error{"flat value references an out-of-order address"};
                    value.address = *existing;
                }
                if (sourceValue->callee) {
                    bool found = false;
                    for (std::uint32_t index = 0; index < module.functions.size(); ++index) {
                        if (module.functions[index] == sourceValue->callee) {
                            value.calleeFunction = index;
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                        return Error{"flat call references a foreign function"};
                }
                if (sourceValue->arguments) {
                    value.args = module.arena->make<DynArray<std::uint32_t>>(*module.arena);
                    if (!value.args)
                        return Error{"allocating flat call arguments"};
                    for (std::size_t index = 0; index < sourceValue->arguments->count; ++index) {
                        const Value *argument = sourceValue->arguments->values[index];
                        if (!argument)
                            return Error{"flat call has a null argument"};
                        auto *existing = valueIndex.get(argument);
                        if (!existing)
                            return Error{"flat call argument is out of order"};
                        value.args->push(*existing);
                    }
                }
                value.intValue = sourceValue->intValue;
                value.floatValue = sourceValue->floatValue;
                flatFunction.values->push(value);

                auto *blockNumber = blockIndex.get(sourceValue->block);
                if (!blockNumber)
                    return Error{"flat value references a foreign block"};
                const std::uint32_t blockNumberOfValue = *blockNumber;
                (*flatFunction.blocks)[blockNumberOfValue].valueIndices->push(valueNumber);
            }

            for (auto *sourceBlock : sourceFunction->blocks) {
                if (!sourceBlock || !sourceBlock->terminator)
                    continue;
                const std::uint32_t blockNumber = *blockIndex.get(sourceBlock);
                FlatBlock &flatBlock = (*flatFunction.blocks)[blockNumber];
                flatBlock.terminator.op = mapOp(sourceBlock->terminator->opcode);
                if (sourceBlock->terminator->value) {
                    auto *existing = valueIndex.get(sourceBlock->terminator->value);
                    if (!existing)
                        return Error{"flat terminator references a missing return value"};
                    flatBlock.terminator.value = *existing;
                }
                if (sourceBlock->terminator->variable) {
                    auto *existing = variableIndex.get(sourceBlock->terminator->variable);
                    if (!existing)
                        return Error{"flat terminator references a missing return variable"};
                    flatBlock.terminator.variable = *existing;
                }
                if (sourceBlock->condition) {
                    auto *existing = valueIndex.get(sourceBlock->condition);
                    if (!existing)
                        return Error{"flat terminator references a missing condition"};
                    flatBlock.terminator.condition = *existing;
                }
                if (sourceBlock->trueTarget) {
                    auto *existing = blockIndex.get(sourceBlock->trueTarget);
                    if (!existing)
                        return Error{"flat branch target references a foreign block"};
                    flatBlock.terminator.trueTarget = *existing;
                }
                if (sourceBlock->falseTarget) {
                    auto *existing = blockIndex.get(sourceBlock->falseTarget);
                    if (!existing)
                        return Error{"flat branch false target references a foreign block"};
                    flatBlock.terminator.falseTarget = *existing;
                }
            }
        }

        auto flatStatus = verify(flat);
        if (flatStatus.isError())
            return flatStatus.error();
        return flat;
    } catch (const std::runtime_error &error) {
        return Error{error.what()};
    }
}

Result<void> verify(const FlatModule &module) {
    if (!module.arena || !module.interner)
        return Error{"flat module has a missing arena or interner"};
    if (module.name == invalidIndex)
        return Error{"flat module has an invalid name"};

    for (std::uint32_t typeIndex = 0; typeIndex < module.types.size(); ++typeIndex) {
        const FlatType &type = module.types[typeIndex];
        if (!knownKind(type.kind))
            return Error{"flat module has an unknown type kind"};
        if (type.elementType != invalidIndex) {
            if (type.kind != TypeKind::Array && type.kind != TypeKind::Slice)
                return Error{"non-aggregate type has an element child"};
            if (type.elementType >= module.types.size())
                return Error{"aggregate type has an out-of-range element"};
        }
        if (type.pointeeType != invalidIndex) {
            if (type.kind != TypeKind::Pointer)
                return Error{"non-pointer type has a pointee child"};
            if (type.pointeeType >= module.types.size())
                return Error{"pointer type has an out-of-range pointee"};
        }
    }

    for (auto *function : module.functions) {
        if (!function)
            return Error{"flat module has a null function"};
        if (function->name == invalidIndex)
            return Error{"flat function has an invalid name"};
        if (function->returnType == invalidIndex ||
            function->returnType >= module.types.size())
            return Error{"flat function has an invalid return type"};
        if (!function->paramTypes || !function->paramNames || !function->variables ||
            !function->blocks || !function->values)
            return Error{"flat function has missing arrays"};
        if (function->paramTypes->size() != function->paramNames->size())
            return Error{"flat function has mismatched param arrays"};
        for (std::uint32_t typeCount = 0; typeCount < function->paramTypes->size(); ++typeCount) {
            if ((*function->paramTypes)[typeCount] >= module.types.size())
                return Error{"flat function has an out-of-range parameter type"};
        }
        for (const FlatVariable &variable : *function->variables) {
            if (variable.type >= module.types.size())
                return Error{"flat function has an out-of-range variable type"};
            if (variable.mutability != Mutability::Mutable &&
                variable.mutability != Mutability::Immutable)
                return Error{"flat function has an invalid variable mutability"};
        }
        if (function->baseBlock == invalidIndex ||
            function->baseBlock >= function->blocks->size())
            return Error{"flat function has an invalid base block"};

        bool hasReturn = false;
        for (std::uint32_t blockNumber = 0; blockNumber < function->blocks->size(); ++blockNumber) {
            const FlatBlock &block = (*function->blocks)[blockNumber];
            if (!block.valueIndices)
                return Error{"flat block has no value array"};
            for (std::uint32_t valueNumber : *block.valueIndices) {
                if (valueNumber >= function->values->size())
                    return Error{"flat block references an out-of-range value"};
            }
            if (!knownOp(block.terminator.op))
                return Error{"flat block has an unknown terminator opcode"};
            if (!isTerminator(block.terminator.op))
                return Error{"flat block has a non-terminator opcode"};

            if (block.terminator.op == FlatOp::Return) {
                hasReturn = true;
                if (module.types[function->returnType].kind == TypeKind::Void) {
                    if (block.terminator.value != invalidIndex ||
                        block.terminator.variable != invalidIndex)
                        return Error{"void flat return carries a value"};
                } else if (block.terminator.value == invalidIndex &&
                           block.terminator.variable == invalidIndex) {
                    return Error{"non-void flat return has no value"};
                }
                if (block.terminator.value != invalidIndex) {
                    if (block.terminator.value >= function->values->size())
                        return Error{"flat return value is out of range"};
                    if ((*function->values)[block.terminator.value].typeIndex !=
                        function->returnType)
                        return Error{"flat return value has a mismatched type"};
                }
                if (block.terminator.variable != invalidIndex &&
                    block.terminator.variable >= function->variables->size())
                    return Error{"flat return variable is out of range"};
                if (block.terminator.condition != invalidIndex ||
                    block.terminator.trueTarget != invalidIndex ||
                    block.terminator.falseTarget != invalidIndex)
                    return Error{"flat return carries control fields"};
            } else if (block.terminator.op == FlatOp::Branch) {
                if (block.terminator.trueTarget == invalidIndex)
                    return Error{"flat branch has no target"};
                if (block.terminator.trueTarget >= function->blocks->size())
                    return Error{"flat branch target is out of range"};
                if (block.terminator.condition != invalidIndex ||
                    block.terminator.falseTarget != invalidIndex ||
                    block.terminator.value != invalidIndex ||
                    block.terminator.variable != invalidIndex)
                    return Error{"flat branch carries unexpected fields"};
            } else {
                if (block.terminator.trueTarget == invalidIndex ||
                    block.terminator.falseTarget == invalidIndex)
                    return Error{"flat conditional branch has missing targets"};
                if (block.terminator.trueTarget >= function->blocks->size() ||
                    block.terminator.falseTarget >= function->blocks->size())
                    return Error{"flat conditional branch target is out of range"};
                if (block.terminator.condition == invalidIndex)
                    return Error{"flat conditional branch has no condition"};
                if (block.terminator.condition >= function->values->size())
                    return Error{"flat conditional branch condition is out of range"};
                const FlatValue &condition = (*function->values)[block.terminator.condition];
                if (condition.typeIndex >= module.types.size() ||
                    module.types[condition.typeIndex].kind != TypeKind::I1)
                    return Error{"flat conditional branch condition is not i1"};
                if (block.terminator.value != invalidIndex ||
                    block.terminator.variable != invalidIndex)
                    return Error{"flat conditional branch carries value fields"};
            }
        }
        if (!hasReturn)
            return Error{"flat function has no return terminator"};

        for (std::uint32_t valueNumber = 0; valueNumber < function->values->size(); ++valueNumber) {
            const FlatValue &value = (*function->values)[valueNumber];
            if (!knownOp(value.op))
                return Error{"flat value has an unknown opcode"};
            if (value.typeIndex == invalidIndex || value.typeIndex >= module.types.size())
                return Error{"flat value has an invalid type"};
            if (isTerminator(value.op))
                return Error{"terminator opcode appears in a value stream"};

            switch (value.op) {
            case FlatOp::Constant: {
                const FlatType &type = module.types[value.typeIndex];
                if (!isScalar(type.kind))
                    return Error{"flat constant has a non-numeric type"};
                break;
            }
            case FlatOp::Param:
                if (value.variableIndex == invalidIndex ||
                    value.variableIndex >= function->variables->size())
                    return Error{"flat parameter has an invalid variable"};
                break;
            case FlatOp::Load:
            case FlatOp::Store:
                if (value.address == invalidIndex || value.address >= function->values->size())
                    return Error{"flat memory op has an invalid address"};
                if (value.variableIndex == invalidIndex ||
                    value.variableIndex >= function->variables->size())
                    return Error{"flat memory op has an invalid variable"};
                if ((*function->values)[value.address].op != FlatOp::Param)
                    return Error{"flat memory address is not a parameter"};
                if ((*function->values)[value.address].variableIndex != value.variableIndex)
                    return Error{"flat memory address does not match the variable slot"};
                if (!isScalar(module.types[value.typeIndex].kind))
                    return Error{"flat memory op uses an unsupported width"};
                break;
            case FlatOp::Call:
                if (value.calleeFunction == invalidIndex ||
                    value.calleeFunction >= module.functions.size())
                    return Error{"flat call has an invalid callee"};
                if (!value.args)
                    return Error{"flat call has no argument array"};
                {
                    const FlatFunction &callee = *module.functions[value.calleeFunction];
                    if (value.args->size() != callee.paramTypes->size())
                        return Error{"flat call has a mismatched argument count"};
                    for (std::size_t index = 0; index < value.args->size(); ++index) {
                        const std::uint32_t argument = (*value.args)[index];
                        if (argument >= function->values->size())
                            return Error{"flat call argument is out of range"};
                        const FlatValue &argumentValue = (*function->values)[argument];
                        if (argumentValue.typeIndex != (*callee.paramTypes)[index])
                            return Error{"flat call argument type does not match the callee"};
                    }
                    if (value.typeIndex != callee.returnType)
                        return Error{"flat call result type does not match the callee"};
                }
                break;
            default:
                if (!isBinary(value.op))
                    return Error{"flat value has an unrecognized opcode"};
                if (value.left == invalidIndex || value.right == invalidIndex ||
                    value.left >= function->values->size() ||
                    value.right >= function->values->size())
                    return Error{"flat binary op has invalid operands"};
                {
                    const FlatValue &left = (*function->values)[value.left];
                    const FlatValue &right = (*function->values)[value.right];
                    if (left.typeIndex == invalidIndex || right.typeIndex == invalidIndex ||
                        left.typeIndex >= module.types.size() ||
                        right.typeIndex >= module.types.size())
                        return Error{"flat binary op has an invalid operand type"};
                    const FlatType &leftType = module.types[left.typeIndex];
                    const FlatType &rightType = module.types[right.typeIndex];
                    if (!isScalar(leftType.kind) || !isScalar(rightType.kind))
                        return Error{"flat binary op requires scalar operands"};
                    if (left.typeIndex != right.typeIndex)
                        return Error{"flat binary op requires matching operand types"};
                    if (isBitwiseOp(value.op) &&
                        !isIntegerKind(module.types[value.typeIndex].kind))
                        return Error{"flat bitwise op has a non-integer result"};
                    if (isComparisonOp(value.op) &&
                        module.types[value.typeIndex].kind != TypeKind::I1)
                        return Error{"flat comparison result is not i1"};
                }
                break;
            }
        }
    }
    return {};
}

Result<std::vector<std::uint8_t>> serializeFlatModule(const FlatModule &module) {
    auto status = verify(module);
    if (status.isError())
        return status.error();

    ByteWriter writer;
    writer.writeBytes(kFormatMagic);
    writer.writeU8(kFormatVersion);
    writer.writeString(module.interner->lookup(module.name));

    if (!sizedArrayWrite(writer, static_cast<std::uint32_t>(module.types.size())))
        return Error{"too many flat types"};
    for (const FlatType &type : module.types) {
        writer.writeU8(static_cast<std::uint8_t>(type.kind));
        writer.writeString(module.interner->lookup(type.nameId));
        writer.writeU32(type.elementType);
        writer.writeU32(type.pointeeType);
        writer.writeU64(type.arrayLength);
    }

    if (!sizedArrayWrite(writer, static_cast<std::uint32_t>(module.functions.size())))
        return Error{"too many flat functions"};
    for (auto *function : module.functions) {
        writer.writeString(module.interner->lookup(function->name));
        writer.writeU32(function->returnType);

        if (!sizedArrayWrite(writer, static_cast<std::uint32_t>(function->paramTypes->size())))
            return Error{"too many flat parameters"};
        for (std::size_t index = 0; index < function->paramTypes->size(); ++index) {
            writer.writeU32((*function->paramTypes)[index]);
            writer.writeString(module.interner->lookup((*function->paramNames)[index]));
        }

        if (!sizedArrayWrite(writer, static_cast<std::uint32_t>(function->variables->size())))
            return Error{"too many flat variables"};
        for (const FlatVariable &variable : *function->variables) {
            writer.writeString(module.interner->lookup(variable.name));
            writer.writeU32(variable.type);
            writer.writeU8(static_cast<std::uint8_t>(variable.mutability));
        }

        if (!sizedArrayWrite(writer, static_cast<std::uint32_t>(function->values->size())))
            return Error{"too many flat values"};
        for (const FlatValue &value : *function->values) {
            writer.writeU8(static_cast<std::uint8_t>(value.op));
            writer.writeU32(value.typeIndex);
            writer.writeU32(value.variableIndex);
            writer.writeU32(value.left);
            writer.writeU32(value.right);
            writer.writeU32(value.address);
            writer.writeU32(value.calleeFunction);
            if (!sizedArrayWrite(writer, value.args ? static_cast<std::uint32_t>(value.args->size())
                                                    : 0))
                return Error{"too many flat call arguments"};
            if (value.args) {
                for (std::uint32_t argument : *value.args)
                    writer.writeU32(argument);
            }
            writer.writeI64(value.intValue);
            writer.writeDouble(value.floatValue);
        }

        if (!sizedArrayWrite(writer, static_cast<std::uint32_t>(function->blocks->size())))
            return Error{"too many flat blocks"};
        for (const FlatBlock &block : *function->blocks) {
            if (!sizedArrayWrite(writer,
                                 static_cast<std::uint32_t>(block.valueIndices->size())))
                return Error{"too many flat block values"};
            for (std::uint32_t valueNumber : *block.valueIndices)
                writer.writeU32(valueNumber);
            writer.writeU8(static_cast<std::uint8_t>(block.terminator.op));
            writer.writeU32(block.terminator.value);
            writer.writeU32(block.terminator.variable);
            writer.writeU32(block.terminator.condition);
            writer.writeU32(block.terminator.trueTarget);
            writer.writeU32(block.terminator.falseTarget);
        }
        writer.writeU32(function->baseBlock);
    }
    return writer.takeBytes();
}

Result<FlatModule> deserializeFlatModule(Arena &arena, std::string_view bytes) {
    ByteReader reader(bytes);
    std::string_view magic;
    if (!reader.readBytes(kFormatMagic.size(), magic) || magic != kFormatMagic)
        return Error{"flat byte stream has a bad magic"};
    std::uint8_t version = 0;
    if (!reader.readU8(version) || version != kFormatVersion)
        return Error{"flat byte stream has an unsupported version"};

    FlatModule module(arena);
    std::string moduleName;
    if (!readStringInto(reader, moduleName))
        return Error{"flat byte stream lacks a module name"};
    module.name = module.interner->intern(moduleName);

    std::uint32_t typeCount = 0;
    if (!readCount(reader, typeCount))
        return Error{"flat byte stream has an invalid type count"};
    for (std::uint32_t typeNumber = 0; typeNumber < typeCount; ++typeNumber) {
        FlatType type{};
        std::uint8_t rawKind = 0;
        std::string typeName;
        if (!reader.readU8(rawKind) || !readStringInto(reader, typeName) ||
            !reader.readU32(type.elementType) || !reader.readU32(type.pointeeType) ||
            !reader.readU64(type.arrayLength))
            return Error{"flat byte stream has a truncated type"};
        type.kind = static_cast<TypeKind>(rawKind);
        type.nameId = module.interner->intern(typeName);
        if (!knownKind(type.kind))
            return Error{"flat byte stream has an unknown type kind"};
        module.types.push(type);
    }

    std::uint32_t functionCount = 0;
    if (!readCount(reader, functionCount))
        return Error{"flat byte stream has an invalid function count"};
    for (std::uint32_t functionNumber = 0; functionNumber < functionCount; ++functionNumber) {
        std::string functionName;
        if (!readStringInto(reader, functionName))
            return Error{"flat byte stream lacks a function name"};
        FlatFunction &function = newFlatFunction(module);
        function.name = module.interner->intern(functionName);
        if (!reader.readU32(function.returnType))
            return Error{"flat byte stream lacks a function return type"};

        std::uint32_t paramCount = 0;
        if (!readCount(reader, paramCount))
            return Error{"flat byte stream has an invalid parameter count"};
        for (std::uint32_t index = 0; index < paramCount; ++index) {
            std::uint32_t paramType = 0;
            std::string paramName;
            if (!reader.readU32(paramType) || !readStringInto(reader, paramName))
                return Error{"flat byte stream has a truncated parameter"};
            function.paramTypes->push(paramType);
            function.paramNames->push(module.interner->intern(paramName));
        }

        std::uint32_t variableCount = 0;
        if (!readCount(reader, variableCount))
            return Error{"flat byte stream has an invalid variable count"};
        for (std::uint32_t index = 0; index < variableCount; ++index) {
            FlatVariable variable{};
            std::string variableName;
            std::uint8_t mutability = 0;
            if (!readStringInto(reader, variableName) || !reader.readU32(variable.type) ||
                !reader.readU8(mutability))
                return Error{"flat byte stream has a truncated variable"};
            variable.name = module.interner->intern(variableName);
            variable.mutability = static_cast<Mutability>(mutability);
            function.variables->push(variable);
        }

        std::uint32_t valueCount = 0;
        if (!readCount(reader, valueCount))
            return Error{"flat byte stream has an invalid value count"};
        for (std::uint32_t index = 0; index < valueCount; ++index) {
            FlatValue value{};
            std::uint8_t rawOp = 0;
            std::uint32_t argCount = 0;
            if (!reader.readU8(rawOp) || !reader.readU32(value.typeIndex) ||
                !reader.readU32(value.variableIndex) || !reader.readU32(value.left) ||
                !reader.readU32(value.right) || !reader.readU32(value.address) ||
                !reader.readU32(value.calleeFunction) || !readCount(reader, argCount))
                return Error{"flat byte stream has a truncated value"};
            value.op = static_cast<FlatOp>(rawOp);
            if (!knownOp(value.op))
                return Error{"flat byte stream has an unknown opcode"};
            value.args = arena.make<DynArray<std::uint32_t>>(arena);
            if (!value.args)
                return Error{"allocating flat call arguments"};
            for (std::uint32_t argument = 0; argument < argCount; ++argument) {
                std::uint32_t argumentIndex = 0;
                if (!reader.readU32(argumentIndex))
                    return Error{"flat byte stream has a truncated call argument"};
                value.args->push(argumentIndex);
            }
            if (!reader.readI64(value.intValue) || !reader.readDouble(value.floatValue))
                return Error{"flat byte stream has a truncated value payload"};
            function.values->push(value);
        }

        std::uint32_t blockCount = 0;
        if (!readCount(reader, blockCount))
            return Error{"flat byte stream has an invalid block count"};
        for (std::uint32_t index = 0; index < blockCount; ++index) {
            FlatBlock block{};
            block.valueIndices = arena.make<DynArray<std::uint32_t>>(arena);
            if (!block.valueIndices)
                return Error{"allocating flat block values"};
            std::uint32_t valueIndexCount = 0;
            if (!readCount(reader, valueIndexCount))
                return Error{"flat byte stream has an invalid block value count"};
            for (std::uint32_t valueNumber = 0; valueNumber < valueIndexCount; ++valueNumber) {
                std::uint32_t localValue = 0;
                if (!reader.readU32(localValue))
                    return Error{"flat byte stream has a truncated block value"};
                block.valueIndices->push(localValue);
            }
            std::uint8_t rawTerminator = 0;
            if (!reader.readU8(rawTerminator) ||
                !reader.readU32(block.terminator.value) ||
                !reader.readU32(block.terminator.variable) ||
                !reader.readU32(block.terminator.condition) ||
                !reader.readU32(block.terminator.trueTarget) ||
                !reader.readU32(block.terminator.falseTarget))
                return Error{"flat byte stream has a truncated block"};
            block.terminator.op = static_cast<FlatOp>(rawTerminator);
            function.blocks->push(block);
        }
        if (!reader.readU32(function.baseBlock))
            return Error{"flat byte stream lacks a base block index"};
    }

    if (reader.remaining() != 0)
        return Error{"flat byte stream has trailing bytes"};

    try {
        auto status = verify(module);
        if (status.isError())
            return status.error();
    } catch (const std::runtime_error &error) {
        return Error{error.what()};
    }
    return module;
}

} // namespace toolkit::sir::flat
