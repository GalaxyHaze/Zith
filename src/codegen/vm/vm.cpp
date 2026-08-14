#include "codegen/vm/vm.hpp"

#include "common/memory/dyn-array.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>

namespace toolkit::codegen::vm {

using common::memory::DynArray;

namespace {

[[noreturn]] void vmAbort(std::string_view message) {
    std::fprintf(stderr, "[error] vm: %.*s\n", static_cast<int>(message.size()), message.data());
    std::abort();
}

[[nodiscard]] std::size_t opWidth(Op op) noexcept {
    switch (op) {
    case Op::ConstI32:
    case Op::AddI32:
    case Op::SubI32:
    case Op::MulI32:
        return 4;
    case Op::ConstI64:
    case Op::AddI64:
    case Op::SubI64:
    case Op::MulI64:
    case Op::ConstF64:
    case Op::AddF64:
    case Op::SubF64:
    case Op::MulF64:
        return 8;
    case Op::Load:
    case Op::Store:
    case Op::RetVoid:
        return 1;
    case Op::RetI32:
        return 4;
    case Op::RetI64:
    case Op::RetF64:
        return 8;
    default:
        return 0;
    }
}

template <typename Number>
[[nodiscard]] Number load(const uint8_t *bytes) {
    Number value = 0;
    std::memcpy(&value, bytes, sizeof(Number));
    return value;
}

template <typename Number>
void store(uint8_t *bytes, Number value) {
    std::memcpy(bytes, &value, sizeof(Number));
}

template <typename Number>
void evaluateArithmetic(Op op, const uint8_t *left, const uint8_t *right, uint8_t *result) {
    const Number lhs = load<Number>(left);
    const Number rhs = load<Number>(right);
    switch (op) {
    case Op::AddI32:
    case Op::AddI64:
    case Op::AddF64:
        store<Number>(result, lhs + rhs);
        break;
    case Op::SubI32:
    case Op::SubI64:
    case Op::SubF64:
        store<Number>(result, lhs - rhs);
        break;
    case Op::MulI32:
    case Op::MulI64:
    case Op::MulF64:
        store<Number>(result, lhs * rhs);
        break;
    default:
        vmAbort("arithmetic opcode reached VM with an unsupported width");
    }
}

void evaluate(Op op, const uint8_t *left, const uint8_t *right, uint8_t *result) {
    switch (op) {
    case Op::AddI32:
    case Op::SubI32:
    case Op::MulI32:
        return evaluateArithmetic<std::int32_t>(op, left, right, result);
    case Op::AddI64:
    case Op::SubI64:
    case Op::MulI64:
        return evaluateArithmetic<std::int64_t>(op, left, right, result);
    case Op::AddF64:
    case Op::SubF64:
    case Op::MulF64:
        return evaluateArithmetic<double>(op, left, right, result);
    default:
        vmAbort("non-arithmetic op reached VM arithmetic");
    }
}

[[nodiscard]] const uint8_t *slotPtr(const DynArray<uint8_t> &slots, std::size_t offset,
                                     std::size_t width) {
    if (offset > slots.size() || width > slots.size() - offset)
        vmAbort("slot access exceeds the byte buffer");
    return slots.data() + offset;
}

uint8_t *slotPtr(DynArray<uint8_t> &slots, std::size_t offset, std::size_t width) {
    if (offset > slots.size() || width > slots.size() - offset)
        vmAbort("slot access exceeds the byte buffer");
    return slots.data() + offset;
}

[[nodiscard]] std::size_t registerCount(const FunctionCode &function) noexcept {
    std::size_t count = 0;
    for (const Instruction &instruction : function.instructions) {
        if (instruction.dst + 1 > count)
            count = instruction.dst + 1;
        if (instruction.reg + 1 > count)
            count = instruction.reg + 1;
        if (instruction.src + 1 > count)
            count = instruction.src + 1;
    }
    return count;
}

} // namespace

bool paramsMatchLayout(const FunctionCode &function, std::span<const uint8_t> args) noexcept {
    return args.size_bytes() == function.paramBytes;
}

StdReturn VM::run(FunctionCode &function, std::span<const uint8_t> args) {
    if (!function.arena)
        vmAbort("function bytecode has no owning arena");
    if (!paramsMatchLayout(function, args))
        vmAbort("argument bytes do not match function parameter layout");

    DynArray<uint8_t> slots{*function.arena};
    slots.resize(function.paramBytes);
    if (!args.empty())
        std::memcpy(slots.data(), args.data(), args.size_bytes());

    DynArray<uint8_t> locals{*function.arena};
    locals.resize(function.localBytes);

    DynArray<uint8_t> registers{*function.arena};
    registers.resize(registerCount(function) * 8);

    StdReturn result;

    for (const Instruction &instruction : function.instructions) {
        const std::size_t width = opWidth(instruction.op);
        if (width == 0)
            vmAbort("instruction reached VM without a supported width");

        switch (instruction.op) {
        case Op::ConstI32: {
            const auto value = static_cast<std::int32_t>(instruction.constant);
            store(registers.data() + instruction.dst * 8, value);
            break;
        }
        case Op::ConstI64: {
            store(registers.data() + instruction.dst * 8, instruction.constant);
            break;
        }
        case Op::ConstF64: {
            double value = 0.0;
            std::memcpy(&value, &instruction.constant, sizeof(value));
            store(registers.data() + instruction.dst * 8, value);
            break;
        }
        case Op::Load: {
            DynArray<uint8_t> &source = instruction.local ? locals : slots;
            const uint8_t *from = slotPtr(source, instruction.slot, instruction.width);
            std::memcpy(registers.data() + instruction.dst * 8, from, instruction.width);
            break;
        }
        case Op::Store: {
            DynArray<uint8_t> &target = instruction.local ? locals : slots;
            uint8_t *to = slotPtr(target, instruction.slot, instruction.width);
            std::memcpy(to, registers.data() + instruction.src * 8, instruction.width);
            break;
        }
        case Op::AddI32:
        case Op::AddI64:
        case Op::AddF64:
        case Op::SubI32:
        case Op::SubI64:
        case Op::SubF64:
        case Op::MulI32:
        case Op::MulI64:
        case Op::MulF64: {
            const uint8_t *left = registers.data() + instruction.reg * 8;
            const uint8_t *right = registers.data() + instruction.src * 8;
            evaluate(instruction.op, left, right, registers.data() + instruction.dst * 8);
            break;
        }
        case Op::RetI32:
        case Op::RetI64:
        case Op::RetF64: {
            std::memcpy(result.raw, registers.data() + instruction.reg * 8, width);
            return result;
        }
        case Op::RetVoid:
            return result;
        }
    }

    vmAbort("function bytecode did not return");
}

} // namespace toolkit::codegen::vm
