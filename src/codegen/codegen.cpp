#include "codegen/codegen.hpp"

#include "common/memory/flat-map.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace toolkit::codegen {

using common::memory::FlatMap;
using common::memory::Error;
using common::memory::Result;
using toolkit::sir::Function;
using toolkit::sir::Module;
using toolkit::sir::Opcode;
using toolkit::sir::Type;
using toolkit::sir::TypeKind;
using toolkit::sir::Value;
using toolkit::sir::Variable;

namespace {

constexpr std::size_t invalidIndex = static_cast<std::size_t>(-1);

[[nodiscard]] bool isScratchType(Type type) noexcept {
    return type.kind == TypeKind::I32 || type.kind == TypeKind::I64 ||
           type.kind == TypeKind::F64;
}

[[nodiscard]] std::size_t typeWidth(Type type) noexcept {
    switch (type.kind) {
    case TypeKind::I32:
    case TypeKind::F64:
        return type.kind == TypeKind::F64 ? 8 : 4;
    case TypeKind::I64:
        return 8;
    default:
        return 0;
    }
}

[[nodiscard]] bool sameFunction(const Function &function, const Value &value) noexcept {
    return value.function == &function;
}

[[nodiscard]] bool sameFunction(const Function &function, const Variable &variable) noexcept {
    return variable.function == &function;
}

[[nodiscard]] Error flattenError(std::string_view message) {
    return Error{std::string(message)};
}

class FlattenValidator {
public:
    [[nodiscard]] Result<void> validate(Module &module) {
        if (!module.arena || !module.interner)
            return flattenError("codegen requires a module with arena and interner");

        for (auto *function : module.functions) {
            if (!function)
                return flattenError("codegen found a null function");
            auto checked = validateFunction(*function);
            if (!checked)
                return checked;
        }
        return {};
    }

private:
    [[nodiscard]] Result<void> validateFunction(Function &function) {
        if (!function.baseScope)
            return flattenError("function has no scope");
        if (!hasTerminator(function))
            return flattenError("function has no terminator");

        for (auto *value : function.values) {
            if (!value)
                return flattenError("function contains a null value");
            auto checked = validateValue(function, *value);
            if (!checked)
                return checked;
        }

        for (auto *variable : function.variables) {
            if (!variable)
                return flattenError("function contains a null variable");
            if (!sameFunction(function, *variable))
                return flattenError("function owns a variable from another function");
            if (variable->initializer && variable->initializer->type != variable->type)
                return flattenError("variable initializer type does not match variable type");
        }

        for (auto *instruction : function.instructions) {
            if (!instruction)
                return flattenError("function contains a null instruction");
            if (instruction->opcode != Opcode::Return)
                return flattenError("instruction stream contains a non-terminator");
            if (instruction->function != &function)
                return flattenError("instruction stream contains a foreign instruction");
            if (!(instruction->type == function.returnType))
                return flattenError("terminator type does not match function return type");

            if (instruction->type.isVoid()) {
                if (instruction->value || instruction->variable)
                    return flattenError("void terminator carries an unexpected value");
                continue;
            }

            if (!isScratchType(instruction->type))
                return flattenError("non-void terminator has an unsupported type");
            if (instruction->value) {
                if (!sameFunction(function, *instruction->value))
                    return flattenError("terminator returns a foreign value");
                if (!(instruction->type == instruction->value->type))
                    return flattenError("terminator value type does not match function type");
            } else if (instruction->variable) {
                if (!sameFunction(function, *instruction->variable))
                    return flattenError("terminator returns a foreign variable");
                if (!(instruction->type == instruction->variable->type))
                    return flattenError("terminator variable type does not match function type");
            } else {
                return flattenError("non-void terminator has no value");
            }
        }
        return {};
    }

    [[nodiscard]] bool hasTerminator(const Function &function) const noexcept {
        for (auto *instruction : function.instructions)
            if (instruction && instruction->opcode == Opcode::Return)
                return true;
        return false;
    }

    [[nodiscard]] Result<void> validateValue(const Function &function, const Value &value) {
        if (!sameFunction(function, value))
            return flattenError("verification found a value owned by another function");

        switch (value.opcode) {
        case Opcode::Constant:
            if (!isScratchType(value.type))
                return flattenError("constant has an unsupported type");
            if (value.type.kind == TypeKind::F64)
                return {};
            if (!value.type.isInteger())
                return flattenError("integer constant requires an integer type");
            return {};
        case Opcode::Param:
            if (!value.variable)
                return flattenError("parameter value has no variable");
            if (!sameFunction(function, *value.variable))
                return flattenError("parameter value references a foreign variable");
            if (!isScratchType(value.type))
                return flattenError("parameter value has an unsupported type");
            return {};
        case Opcode::Add:
        case Opcode::Sub:
        case Opcode::Mul:
            return validateBinary(function, value);
        case Opcode::Store:
        case Opcode::Return:
            return flattenError("value stream contains a non-value opcode");
        }
        return flattenError("value has an unknown opcode");
    }

    [[nodiscard]] Result<void> validateBinary(const Function &function, const Value &value) {
        if (!value.left || !value.right)
            return flattenError("arithmetic value has a missing operand");
        if (!sameFunction(function, *value.left) || !sameFunction(function, *value.right))
            return flattenError("arithmetic value has a foreign operand");
        if (!value.type.isNumeric() || !value.left->type.isNumeric() ||
            !value.right->type.isNumeric())
            return flattenError("arithmetic value has an invalid operand type");
        if (!isScratchType(value.type) || !isScratchType(value.left->type) ||
            !isScratchType(value.right->type))
            return flattenError("arithmetic value uses an unsupported width");
        if (!(value.left->type == value.right->type) || !(value.type == value.left->type))
            return flattenError("arithmetic operand types must agree with the result type");
        return {};
    }
};

class CodeBuilder {
public:
    CodeBuilder(Function &function, Arena &arena) : function_(function), code_(arena) {
        code_.name = function.nameView();
    }

    [[nodiscard]] Result<FunctionCode> build() {
        mapVariables();
        emitLocalInitializers();

        for (auto *instruction : function_.instructions)
            emitTerminator(*instruction);

        return FunctionCode{std::move(code_)};
    }

private:
    [[nodiscard]] std::size_t addInstruction(Op op, std::size_t width = 0) {
        Instruction instruction;
        instruction.op = op;
        instruction.width = width;
        code_.instructions.push(instruction);
        return code_.instructions.size() - 1;
    }

    void mapVariables() {
        for (auto *variable : function_.variables) {
            if (!variable)
                continue;
            const std::size_t width = typeWidth(variable->type);
            if (width == 0)
                continue;
            bool isParam = false;
            for (auto *param : function_.params) {
                if (param == variable) {
                    isParam = true;
                    break;
                }
            }
            if (isParam) {
                slotOffsets_.insert(variable, code_.paramBytes);
                code_.paramBytes += width;
            } else {
                slotOffsets_.insert(variable, code_.localBytes);
                code_.localBytes += width;
            }
        }
    }

    void emitLocalInitializers() {
        for (auto *variable : function_.variables) {
            if (!variable || !variable->initializer)
                continue;
            const std::size_t offset = slotOffset(*variable);
            if (offset == invalidIndex)
                continue;
            const std::size_t src = valueRegister(*variable->initializer);
            Instruction &store = code_.instructions[addInstruction(Op::Store, typeWidth(variable->type))];
            store.slot = offset;
            store.src = src;
            store.local = !isParameter(*variable);
        }
    }

    [[nodiscard]] bool isParameter(const Variable &variable) const noexcept {
        for (auto *param : function_.params)
            if (param == &variable)
                return true;
        return false;
    }

    [[nodiscard]] std::size_t slotOffset(const Variable &variable) const noexcept {
        auto *offset = slotOffsets_.get(const_cast<Variable *>(&variable));
        return offset ? *offset : invalidIndex;
    }

    [[nodiscard]] std::size_t valueRegister(Value &value) {
        if (auto *existing = valueRegs_.get(&value))
            return *existing;

        std::size_t reg = invalidIndex;
        switch (value.opcode) {
        case Opcode::Constant:
            reg = emitConstant(value);
            break;
        case Opcode::Param:
            if (value.variable)
                reg = variableRegister(*value.variable);
            break;
        case Opcode::Add:
        case Opcode::Sub:
        case Opcode::Mul:
            reg = emitBinary(value);
            break;
        case Opcode::Store:
        case Opcode::Return:
            break;
        }

        if (reg == invalidIndex)
            return invalidIndex;
        valueRegs_.insert(&value, reg);
        return reg;
    }

    [[nodiscard]] std::size_t variableRegister(Variable &variable) {
        if (auto *existing = variableRegs_.get(&variable))
            return *existing;

        const std::size_t offset = slotOffset(variable);
        if (offset == invalidIndex)
            return invalidIndex;

        const std::size_t reg = nextRegister();
        Instruction &load = code_.instructions[addInstruction(Op::Load, typeWidth(variable.type))];
        load.slot = offset;
        load.dst = reg;
        load.local = !isParameter(variable);
        variableRegs_.insert(&variable, reg);
        return reg;
    }

    [[nodiscard]] std::size_t emitConstant(Value &value) {
        const bool isFloat = value.type.kind == TypeKind::F64;
        Op op = isFloat ? Op::ConstF64
                 : value.type.kind == TypeKind::I64 ? Op::ConstI64
                                                      : Op::ConstI32;
        const std::size_t reg = nextRegister();
        Instruction &constant =
            code_.instructions[addInstruction(op, typeWidth(value.type))];
        constant.dst = reg;
        if (isFloat)
            std::memcpy(&constant.constant, &value.floatValue, sizeof(value.floatValue));
        else
            constant.constant = value.intValue;
        return reg;
    }

    [[nodiscard]] std::size_t emitBinary(Value &value) {
        if (!value.left || !value.right)
            return invalidIndex;

        const std::size_t left = valueRegister(*value.left);
        const std::size_t right = valueRegister(*value.right);
        if (left == invalidIndex || right == invalidIndex)
            return invalidIndex;

        Op op;
        switch (value.type.kind) {
        case TypeKind::I32:
            op = value.opcode == Opcode::Add   ? Op::AddI32
                 : value.opcode == Opcode::Sub ? Op::SubI32
                                                : Op::MulI32;
            break;
        case TypeKind::I64:
            op = value.opcode == Opcode::Add   ? Op::AddI64
                 : value.opcode == Opcode::Sub ? Op::SubI64
                                                : Op::MulI64;
            break;
        case TypeKind::F64:
            op = value.opcode == Opcode::Add   ? Op::AddF64
                 : value.opcode == Opcode::Sub ? Op::SubF64
                                                : Op::MulF64;
            break;
        default:
            return invalidIndex;
        }

        const std::size_t dst = nextRegister();
        Instruction &instruction =
            code_.instructions[addInstruction(op, typeWidth(value.type))];
        instruction.reg = left;
        instruction.src = right;
        instruction.dst = dst;
        return dst;
    }

    void emitTerminator(toolkit::sir::Instruction &instruction) {
        if (instruction.opcode != Opcode::Return)
            return;

        if (instruction.type.isVoid()) {
            (void)addInstruction(Op::RetVoid);
            return;
        }

        Op op;
        switch (instruction.type.kind) {
        case TypeKind::I32:
            op = Op::RetI32;
            break;
        case TypeKind::I64:
            op = Op::RetI64;
            break;
        case TypeKind::F64:
            op = Op::RetF64;
            break;
        default:
            return;
        }

        const std::size_t reg = instruction.value
                                    ? valueRegister(*instruction.value)
                                    : instruction.variable
                                          ? variableRegister(*instruction.variable)
                                          : invalidIndex;
        if (reg == invalidIndex)
            return;

        Instruction &ret = code_.instructions[addInstruction(op, typeWidth(instruction.type))];
        ret.reg = reg;
    }

    [[nodiscard]] std::size_t nextRegister() noexcept {
        return registersPressed_++;
    }

    Function &function_;
    FunctionCode code_;
    FlatMap<Value *, std::size_t> valueRegs_;
    FlatMap<Variable *, std::size_t> variableRegs_;
    FlatMap<Variable *, std::size_t> slotOffsets_;
    std::size_t registersPressed_ = 0;
};

} // namespace

Result<ByteCode> CodegenState::flatten(Module &module) {
    if (!arena_)
        return flattenError("codegen state has no arena");

    FlattenValidator validator;
    auto validation = validator.validate(module);
    if (!validation)
        return Error{validation.error().msg};

    ByteCode bytecode{*arena_};
    for (auto *function : module.functions) {
        if (!function)
            return flattenError("codegen found a null function");
        CodeBuilder builder{*function, *arena_};
        auto generated = builder.build();
        if (!generated)
            return Error{generated.error().msg};
        bytecode.functions.emplace(std::move(generated.value()));
    }
    return Result<ByteCode>{std::move(bytecode)};
}

Result<ByteCode> CodegenState::runBackend(Module &module, Backend backend) {
    if (backend != Backend::VM)
        return flattenError("selected codegen backend is not available");
    return flatten(module);
}

Result<ByteCode> codegen(Module &module) {
    CodegenState state{*module.arena};
    return state.runBackend(module);
}

} // namespace toolkit::codegen
