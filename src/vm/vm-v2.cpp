#include "vm/vm-v2.hpp"

#include "common/test-output-buffer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zith::vm {
namespace {

constexpr std::size_t allocationFailure = std::numeric_limits<std::size_t>::max();

struct Frame {
    LinearMemory &memory;
    std::size_t mark;

    explicit Frame(LinearMemory &frameMemory)
        : memory(frameMemory), mark(frameMemory.arenaWatermark()) {}

    Frame(const Frame &)                     = delete;
    auto operator=(const Frame &) -> Frame & = delete;

    ~Frame() {
        memory.restoreArena(mark);
    }
};

struct RunState {
    const Module *module = nullptr;
    LinearMemory *memory = nullptr;
    std::vector<std::size_t> stringAddresses;
    common::TestOutputBuffer output;
    std::string message;
    std::string_view lastFunction;
    std::size_t lastInstruction = 0;
    RunStatus status            = RunStatus::Ok;
};

auto statusText(RunStatus status) -> std::string_view {
    switch (status) {
    case RunStatus::Ok:
        return "ok";
    case RunStatus::MissingMain:
        return "missing main";
    case RunStatus::Trap:
        return "trap";
    case RunStatus::Unsupported:
        return "unsupported";
    case RunStatus::Oom:
        return "out of linear memory";
    }
    return "unknown status";
}

auto getReg(const std::vector<int64_t> &regs, uint16_t index) -> int64_t {
    if (index < regs.size())
        return regs[index];
    return 0;
}

auto isF64Register(const Function &fn, uint16_t index) -> bool {
    return index < fn.regTypes.size() && fn.regTypes[index] == ValueType::F64;
}

auto getF64Register(const std::vector<int64_t> &regs, uint16_t index) -> double {
    const int64_t bits = getReg(regs, index);
    double value       = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

auto f64RegisterBits(double value) -> int64_t {
    int64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

auto setReg(std::vector<int64_t> &regs, uint16_t index, int64_t value) -> bool {
    if (index >= regs.size())
        return false;
    regs[index] = value;
    return true;
}

auto findFunction(const Module &module, std::string_view name) -> const Function * {
    for (const auto &fn : module.functions)
        if (fn.name == name)
            return &fn;
    return nullptr;
}

auto missingRegister(const Function &fn, uint16_t index, bool pair) -> bool {
    const uint32_t count = static_cast<uint32_t>(fn.regCount);
    return index >= count || (pair && static_cast<uint32_t>(index) + 1 >= count);
}

auto invalidJumpTarget(const Function &fn, const Instr &instr) -> bool {
    return instr.imm >= fn.body.size();
}

auto formatVariadic(std::string_view format, std::span<const int64_t> args,
                    std::size_t firstArgument, std::string &output) -> bool {
    std::size_t nextArgument = firstArgument;
    for (std::size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '%') {
            output.push_back(format[i]);
            continue;
        }
        if (++i >= format.size())
            return false;
        if (format[i] == '%') {
            output.push_back('%');
            continue;
        }
        if (nextArgument >= args.size())
            return false;
        const auto value = args[nextArgument++];
        if (format[i] == 'u') {
            output += std::to_string(static_cast<std::uint64_t>(value));
        } else if (format[i] == 'd') {
            output += std::to_string(value);
        } else if (format[i] == 'g') {
            double floating = 0;
            std::memcpy(&floating, &value, sizeof(floating));
            output += std::to_string(floating);
        } else {
            return false;
        }
    }
    return true;
}

struct ExternCall {
    std::int64_t arg0 = 0;
    std::int64_t arg1 = 0;
    std::int64_t arg2 = 0;
    std::int64_t arg3 = 0;
};

using ExternHandler = auto (*)(RunState &, const ExternCall &, std::int64_t &) -> bool;

auto externPuts(RunState &state, const ExternCall &args, std::int64_t &result) -> bool {
    (void)result;
    const std::string_view text = state.memory->cstring(static_cast<std::size_t>(args.arg0));
    if (text.empty())
        return false;
    state.output.append(text);
    state.output.append("\n");
    return true;
}

auto externPutchar(RunState &state, const ExternCall &args, std::int64_t &result) -> bool {
    (void)result;
    const char value = static_cast<char>(args.arg0);
    state.output.append(std::string_view(&value, 1U));
    return true;
}

auto externWriteStdout(RunState &state, const ExternCall &args, std::int64_t &result) -> bool {
    (void)result;
    if (args.arg1 > 0) {
        const std::string_view text = state.memory->stringView(static_cast<std::size_t>(args.arg0),
                                                               static_cast<std::size_t>(args.arg1));
        state.output.append(text);
    }
    return true;
}

auto externMalloc(RunState &state, const ExternCall &args, std::int64_t &result) -> bool {
    result = static_cast<std::int64_t>(
        state.memory->mallocBytes(static_cast<std::size_t>(args.arg0), 1));
    return true;
}

auto externAlignedAlloc(RunState &state, const ExternCall &args, std::int64_t &result) -> bool {
    result = 0;
    if (args.arg0 <= 0 || args.arg1 <= 0)
        return true;

    const auto alignment = static_cast<std::uint64_t>(args.arg0);
    const auto size      = static_cast<std::uint64_t>(args.arg1);
    if (!std::in_range<std::size_t>(alignment) || !std::in_range<std::size_t>(size) ||
        (alignment & (alignment - 1U)) != 0U || size % alignment != 0U)
        return true;

    const auto offset = state.memory->mallocBytes(static_cast<std::size_t>(size),
                                                  static_cast<std::size_t>(alignment));
    if (offset == allocationFailure)
        return true;
    if (!std::in_range<std::int64_t>(offset)) {
        (void)state.memory->freeBytes(offset);
        return true;
    }
    result = static_cast<std::int64_t>(offset);
    return true;
}

/// Zero-initializing allocation used by the stdlib hash maps. A zero or
/// overflowing request returns null, matching the C contract instead of
/// trapping on the multiplication.
auto externCalloc(RunState &state, const ExternCall &args, std::int64_t &result) -> bool {
    const auto count = static_cast<std::uint64_t>(args.arg0);
    const auto size  = static_cast<std::uint64_t>(args.arg1);
    if (count == 0 || size == 0 || count > std::numeric_limits<std::uint64_t>::max() / size) {
        result = 0;
        return true;
    }
    const auto total         = count * size;
    const std::size_t offset = state.memory->mallocBytes(static_cast<std::size_t>(total), 1);
    if (offset == allocationFailure) {
        result = 0;
        return true;
    }
    const std::vector<std::uint8_t> zeros(static_cast<std::size_t>(total), 0);
    if (!state.memory->write(offset, zeros))
        return false;
    result = static_cast<std::int64_t>(offset);
    return true;
}

auto externFree(RunState &state, const ExternCall &args, std::int64_t &result) -> bool {
    (void)result;
    (void)state.memory->freeBytes(static_cast<std::size_t>(args.arg0));
    return true;
}

auto externRealloc(RunState &state, const ExternCall &args, std::int64_t &result) -> bool {
    result = static_cast<std::int64_t>(state.memory->reallocBytes(
        static_cast<std::size_t>(args.arg0), static_cast<std::size_t>(args.arg1), 1));
    return true;
}

auto externSnprintf(RunState &state, const ExternCall &args, std::int64_t &result) -> bool {
    const std::string_view text = state.memory->cstring(static_cast<std::size_t>(args.arg2));
    if (text.empty())
        return false;
    std::string formatted;
    if (text.find("%u") != std::string_view::npos) {
        formatted = std::to_string(static_cast<std::uint64_t>(args.arg3));
    } else if (text.find("%d") != std::string_view::npos) {
        formatted = std::to_string(args.arg3);
    } else if (text.find("%g") != std::string_view::npos) {
        double floating = 0;
        std::memcpy(&floating, &args.arg3, sizeof(floating));
        formatted = std::to_string(floating);
    } else {
        formatted = std::string(text);
    }
    if (args.arg1 <= 0)
        return false;
    if (formatted.size() >= static_cast<std::size_t>(args.arg1))
        formatted.resize(static_cast<std::size_t>(args.arg1) - 1U);
    if (!state.memory->writeString(static_cast<std::size_t>(args.arg0), formatted))
        return false;
    result = static_cast<std::int64_t>(formatted.size());
    return true;
}

auto externStrlen(RunState &state, const ExternCall &args, std::int64_t &result) -> bool {
    const std::string_view text = state.memory->cstring(static_cast<std::size_t>(args.arg0));
    result                      = static_cast<std::int64_t>(text.size());
    return true;
}

auto externMemcpy(RunState &state, const ExternCall &args, std::int64_t &result) -> bool {
    if (!state.memory->copy(static_cast<std::size_t>(args.arg0),
                            static_cast<std::size_t>(args.arg1),
                            static_cast<std::size_t>(args.arg2)))
        return false;
    result = args.arg0;
    return true;
}

/// Byte-wise comparison used by the stdlib input parsers. The comparison stops
/// at the first difference or at a NUL byte within `count` bytes.
auto externStrncmp(RunState &state, const ExternCall &args, std::int64_t &result) -> bool {
    const std::size_t lhsOffset = static_cast<std::size_t>(args.arg0);
    const std::size_t rhsOffset = static_cast<std::size_t>(args.arg1);
    const std::size_t count     = static_cast<std::size_t>(args.arg2);
    for (std::size_t i = 0; i < count; ++i) {
        const auto left  = state.memory->read(lhsOffset + i, 1);
        const auto right = state.memory->read(rhsOffset + i, 1);
        if (left.size() != 1 || right.size() != 1)
            return false;
        const auto lhs = static_cast<unsigned char>(left[0]);
        const auto rhs = static_cast<unsigned char>(right[0]);
        if (lhs != rhs) {
            result = lhs < rhs ? -1 : 1;
            return true;
        }
        if (lhs == 0)
            break;
    }
    result = 0;
    return true;
}

/// Copy the NUL-terminated string at `src` (arg1) into `dst` (arg0), including
/// the terminator. The caller owns `dst`; the handler assumes it has room for
/// the source bytes plus the terminator, matching the C contract.
auto externStrcpy(RunState &state, const ExternCall &args, std::int64_t &result) -> bool {
    const std::string_view text = state.memory->cstring(static_cast<std::size_t>(args.arg1));
    if (!state.memory->writeString(static_cast<std::size_t>(args.arg0), text))
        return false;
    result = args.arg0;
    return true;
}

/// Append the NUL-terminated string at `src` (arg1) to the string at `dst`
/// (arg0). The caller owns `dst`; the handler assumes it has room for the
/// combined bytes plus the terminator, matching the C contract.
auto externStrcat(RunState &state, const ExternCall &args, std::int64_t &result) -> bool {
    std::string combined(state.memory->cstring(static_cast<std::size_t>(args.arg0)));
    combined.append(state.memory->cstring(static_cast<std::size_t>(args.arg1)));
    if (!state.memory->writeString(static_cast<std::size_t>(args.arg0), combined))
        return false;
    result = args.arg0;
    return true;
}

/// Lexicographic comparison of two NUL-terminated strings, returning a
/// negative, zero, or positive value. Used by `soon/string` equality helpers.
auto externStrcmp(RunState &state, const ExternCall &args, std::int64_t &result) -> bool {
    const std::string_view lhs = state.memory->cstring(static_cast<std::size_t>(args.arg0));
    const std::string_view rhs = state.memory->cstring(static_cast<std::size_t>(args.arg1));
    const int comparison       = lhs.compare(rhs);
    result                     = comparison < 0 ? -1 : (comparison > 0 ? 1 : 0);
    return true;
}

/// Byte-wise comparison of two buffers over `count` (arg2) bytes. Unlike
/// `strncmp` this never stops at a NUL byte, matching `memcmp`.
auto externMemcmp(RunState &state, const ExternCall &args, std::int64_t &result) -> bool {
    const std::size_t lhsOffset = static_cast<std::size_t>(args.arg0);
    const std::size_t rhsOffset = static_cast<std::size_t>(args.arg1);
    const std::size_t count     = static_cast<std::size_t>(args.arg2);
    for (std::size_t i = 0; i < count; ++i) {
        const auto left  = state.memory->read(lhsOffset + i, 1);
        const auto right = state.memory->read(rhsOffset + i, 1);
        if (left.size() != 1 || right.size() != 1)
            return false;
        const auto lhs = static_cast<unsigned char>(left[0]);
        const auto rhs = static_cast<unsigned char>(right[0]);
        if (lhs != rhs) {
            result = lhs < rhs ? -1 : 1;
            return true;
        }
    }
    result = 0;
    return true;
}

/// The playground has no stdin stream, so `getchar` reports end of input. This
/// keeps the stdlib `input` loop deterministic instead of trapping.
auto externGetchar(RunState &state, const ExternCall &args, std::int64_t &result) -> bool {
    (void)state;
    (void)args;
    result = -1;
    return true;
}

struct ExternEntry {
    std::string_view name;
    ExternHandler handler;
};

/// Validated FFI subset. Adding an extern here is the only change needed to
/// support it on both `CallExtern` and `CallExternRef`; anything absent traps
/// with a message naming the missing extern.
constexpr std::array<ExternEntry, 17> kValidatedExterns{{
    {"puts", externPuts},
    {"putchar", externPutchar},
    {"write_stdout", externWriteStdout},
    {"malloc", externMalloc},
    {"aligned_alloc", externAlignedAlloc},
    {"calloc", externCalloc},
    {"free", externFree},
    {"realloc", externRealloc},
    {"snprintf", externSnprintf},
    {"strlen", externStrlen},
    {"memcpy", externMemcpy},
    {"strncmp", externStrncmp},
    {"strcpy", externStrcpy},
    {"strcat", externStrcat},
    {"strcmp", externStrcmp},
    {"memcmp", externMemcmp},
    {"getchar", externGetchar},
}};

auto dispatchScalarExtern(RunState &state, std::string_view name, const ExternCall &args,
                          std::int64_t &result) -> bool {
    for (const auto &entry : kValidatedExterns) {
        if (entry.name == name)
            return entry.handler(state, args, result);
    }
    state.message = "unsupported extern '" + std::string(name) + "' in VM v2";
    return false;
}

/// Return false when the module does not satisfy v2's invariant that every
/// operand names a register/label that exists. This is a static shape check,
/// so invalid guest IR reports Trap before any memory or I/O side effects.
auto validateFunctionShape(const Function &fn) -> bool {
    for (std::size_t pc = 0; pc < fn.body.size(); ++pc) {
        const auto &instr  = fn.body[pc];
        const uint16_t dst = instr.a;
        const uint16_t lhs = instr.b;
        const uint16_t rhs = instr.c;

        if (instr.op > Op::Panic)
            return false;
        switch (instr.op) {
        case Op::LoadConstI32:
        case Op::LoadConstI64:
        case Op::LoadConstI64FromPool:
        case Op::LoadConstF32:
        case Op::LoadConstF64:
        case Op::LoadString:
        case Op::LoadFnRef:
        case Op::LoadExternRef:
        case Op::Move:
        case Op::AllocBytes:
        case Op::MallocBytes:
            if (missingRegister(fn, dst, false))
                return false;
            if (instr.op == Op::Move) {
                if (missingRegister(fn, lhs, false))
                    return false;
            } else if (instr.op == Op::AllocBytes || instr.op == Op::MallocBytes) {
                if (missingRegister(fn, lhs, false) || missingRegister(fn, rhs, false))
                    return false;
            }
            break;
        case Op::StoreBytes:
            if (missingRegister(fn, dst, false))
                return false;
            break;
        case Op::StoreI64:
        case Op::StoreI8:
            if (missingRegister(fn, dst, false) || missingRegister(fn, lhs, false))
                return false;
            break;
        case Op::LoadBytes:
            if (missingRegister(fn, dst, false) || missingRegister(fn, lhs, false) ||
                missingRegister(fn, rhs, false))
                return false;
            break;
        case Op::LoadI64:
        case Op::LoadI8:
            if (missingRegister(fn, dst, false) || missingRegister(fn, lhs, false))
                return false;
            break;
        case Op::IndexLoad:
            if (missingRegister(fn, dst, false) || missingRegister(fn, lhs, false) ||
                missingRegister(fn, rhs, false) || instr.imm == 0 || instr.e > 7U ||
                ((instr.e & 2U) == 0U && (instr.e & 1U) == 0U && instr.d == 0U) ||
                ((instr.e & 1U) != 0U && missingRegister(fn, instr.d, false)))
                return false;
            break;
        case Op::FieldPtr:
            if (missingRegister(fn, dst, false) || missingRegister(fn, lhs, false))
                return false;
            break;
        case Op::MakeSlice:
            if (missingRegister(fn, dst, true) || missingRegister(fn, lhs, false) ||
                missingRegister(fn, rhs, false))
                return false;
            break;
        case Op::SlicePtr:
        case Op::SliceLen:
            if (missingRegister(fn, dst, false) || missingRegister(fn, lhs, true))
                return false;
            break;
        case Op::MemCopy:
            if (missingRegister(fn, dst, false) || missingRegister(fn, lhs, false) ||
                missingRegister(fn, rhs, false))
                return false;
            break;
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::Div:
        case Op::Rem:
        case Op::BitAnd:
        case Op::BitOr:
        case Op::BitXor:
        case Op::Shl:
        case Op::Shr:
        case Op::Neg:
        case Op::Not:
        case Op::BitNot:
        case Op::Eq:
        case Op::Ne:
        case Op::Lt:
        case Op::Le:
        case Op::Gt:
        case Op::Ge:
            if (missingRegister(fn, dst, false) || missingRegister(fn, lhs, false) ||
                missingRegister(fn, rhs, false))
                return false;
            break;
        case Op::CallExtern:
            if (missingRegister(fn, dst, false) || missingRegister(fn, lhs, false) ||
                missingRegister(fn, rhs, false) || missingRegister(fn, instr.d, false) ||
                missingRegister(fn, instr.e, false))
                return false;
            break;
        case Op::CallFn:
            if (missingRegister(fn, dst, false) || missingRegister(fn, lhs, false) ||
                missingRegister(fn, rhs, false))
                return false;
            break;
        case Op::CallRange:
        case Op::TailCallRange:
            if (instr.op == Op::CallRange && missingRegister(fn, dst, false))
                return false;
            if (instr.c > 0) {
                const auto last = static_cast<uint32_t>(instr.b) + instr.c - 1U;
                if (last > std::numeric_limits<uint16_t>::max() ||
                    missingRegister(fn, instr.b, false) ||
                    missingRegister(fn, static_cast<uint16_t>(last), false))
                    return false;
            }
            break;
        case Op::CallExternRange:
            if (missingRegister(fn, dst, false))
                return false;
            if (instr.c > 0) {
                const auto last = static_cast<uint32_t>(instr.b) + instr.c - 1U;
                if (last > std::numeric_limits<uint16_t>::max() ||
                    missingRegister(fn, instr.b, false) ||
                    missingRegister(fn, static_cast<uint16_t>(last), false))
                    return false;
            }
            break;
        case Op::CallRangeFnRef:
            if (missingRegister(fn, dst, false) || missingRegister(fn, lhs, false))
                return false;
            if (instr.d > 0) {
                const auto last = static_cast<uint32_t>(instr.c) + instr.d - 1U;
                if (last > std::numeric_limits<uint16_t>::max() ||
                    missingRegister(fn, instr.c, false) ||
                    missingRegister(fn, static_cast<uint16_t>(last), false))
                    return false;
            }
            break;
        case Op::CallExternRef:
        case Op::CallFnRef:
            if (missingRegister(fn, dst, false) || missingRegister(fn, lhs, false) ||
                missingRegister(fn, rhs, false) || missingRegister(fn, instr.imm, false))
                return false;
            break;
        case Op::Ret:
            if (missingRegister(fn, dst, false))
                return false;
            break;
        case Op::Branch:
            if (missingRegister(fn, lhs, false) || invalidJumpTarget(fn, instr))
                return false;
            break;
        case Op::Branch2:
            if (missingRegister(fn, lhs, false) || invalidJumpTarget(fn, instr) ||
                instr.d >= fn.body.size())
                return false;
            break;
        case Op::Jump:
            if (invalidJumpTarget(fn, instr))
                return false;
            break;
        case Op::NullDeref:
        case Op::Trap:
        case Op::Panic:
            break;
        }
    }
    return true;
}

/// Result of running one function frame. `hasAux` is set when the callee
/// returns a slice by value, in which case `aux` carries the length half that
/// lives in the register immediately after the return register.
struct FunctionReturn {
    bool ok       = false;
    int64_t value = 0;
    bool hasAux   = false;
    int64_t aux   = 0;
};

auto runFunction(RunState &state, const Function &fn, std::vector<int64_t> &regs)
    -> FunctionReturn {
    const Function *currentFunction = &fn;
    for (;;) {
        const Function *tailTarget = nullptr;
        std::vector<int64_t> tailRegs;
        {
            Frame frame(*state.memory);

            if (regs.size() < currentFunction->regCount)
                regs.resize(currentFunction->regCount, 0);

            std::size_t pc = 0;
            while (pc < currentFunction->body.size()) {
                state.lastFunction    = currentFunction->name;
                state.lastInstruction = pc;
                const auto &instr     = currentFunction->body[pc];
                const uint16_t dst    = instr.a;
                const uint16_t lhs    = instr.b;
                const uint16_t rhs    = instr.c;

                switch (instr.op) {
                case Op::LoadConstI32:
                case Op::LoadConstI64:
                    if (!setReg(regs, dst, instr.imm))
                        return {false, 0};
                    pc++;
                    break;
                case Op::LoadConstI64FromPool:
                    if (instr.imm >= state.module->i64Constants.size() ||
                        !setReg(regs, dst, state.module->i64Constants[instr.imm]))
                        return {false, 0};
                    pc++;
                    break;
                case Op::LoadConstF32:
                case Op::LoadConstF64: {
                    if (instr.imm >= state.module->f64Constants.size())
                        return {false, 0};
                    int64_t bits = 0;
                    std::memcpy(&bits, &state.module->f64Constants[instr.imm], sizeof(bits));
                    if (!setReg(regs, dst, bits))
                        return {false, 0};
                    pc++;
                    break;
                }
                case Op::LoadString:
                    if (instr.imm >= state.stringAddresses.size())
                        return {false, 0};
                    if (!setReg(regs, dst, static_cast<int64_t>(state.stringAddresses[instr.imm])))
                        return {false, 0};
                    pc++;
                    break;
                case Op::LoadFnRef:
                    if (instr.imm >= state.module->functions.size())
                        return {false, 0};
                    if (!setReg(regs, dst, instr.imm))
                        return {false, 0};
                    pc++;
                    break;
                case Op::LoadExternRef:
                    if (instr.imm >= state.module->externs.size())
                        return {false, 0};
                    if (!setReg(regs, dst, instr.imm))
                        return {false, 0};
                    pc++;
                    break;
                case Op::Move:
                    if (!setReg(regs, dst, getReg(regs, lhs)))
                        return {false, 0};
                    pc++;
                    break;
                case Op::AllocBytes:
                case Op::MallocBytes: {
                    const std::size_t count  = static_cast<std::size_t>(getReg(regs, lhs));
                    const std::size_t align  = static_cast<std::size_t>(getReg(regs, rhs));
                    const std::size_t offset = instr.op == Op::AllocBytes
                                                   ? state.memory->allocBytes(count, align)
                                                   : state.memory->mallocBytes(count, align);
                    if (offset == allocationFailure)
                        return {false, 0};
                    if (!setReg(regs, dst, static_cast<int64_t>(offset)))
                        return {false, 0};
                    pc++;
                    break;
                }
                case Op::StoreBytes: {
                    if (instr.imm >= state.module->strings.size())
                        return {false, 0};
                    const std::string_view text = state.module->strings[instr.imm];
                    const std::size_t offset    = static_cast<std::size_t>(getReg(regs, dst));
                    std::vector<uint8_t> bytes(text.begin(), text.end());
                    if (!state.memory->write(offset, bytes))
                        return {false, 0};
                    pc++;
                    break;
                }
                case Op::StoreI64: {
                    const auto offset = static_cast<std::size_t>(getReg(regs, dst));
                    const auto value  = static_cast<std::uint64_t>(getReg(regs, lhs));
                    std::array<std::uint8_t, sizeof(value)> bytes{};
                    for (std::size_t i = 0; i < bytes.size(); ++i)
                        bytes[i] = static_cast<std::uint8_t>(value >> (i * 8U));
                    if (!state.memory->write(offset, bytes))
                        return {false, 0};
                    pc++;
                    break;
                }
                case Op::StoreI8: {
                    const auto offset = static_cast<std::size_t>(getReg(regs, dst));
                    const std::array<std::uint8_t, 1> bytes{
                        static_cast<std::uint8_t>(getReg(regs, lhs))};
                    if (!state.memory->write(offset, bytes))
                        return {false, 0};
                    pc++;
                    break;
                }
                case Op::LoadBytes: {
                    const std::size_t offset = static_cast<std::size_t>(getReg(regs, lhs));
                    const std::size_t count  = static_cast<std::size_t>(getReg(regs, rhs));
                    auto bytes               = state.memory->read(offset, count);
                    if (bytes.size() != count)
                        return {false, 0};
                    int64_t value = 0;
                    for (std::size_t i = 0; i < count && i < sizeof(value); ++i)
                        value = (value << 8) | bytes[i];
                    if (!setReg(regs, dst, value))
                        return {false, 0};
                    pc++;
                    break;
                }
                case Op::LoadI64: {
                    const auto offset = static_cast<std::size_t>(getReg(regs, lhs));
                    const auto bytes  = state.memory->read(offset, sizeof(std::uint64_t));
                    if (bytes.size() != sizeof(std::uint64_t))
                        return {false, 0};
                    std::uint64_t value = 0;
                    for (std::size_t i = 0; i < bytes.size(); ++i)
                        value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8U);
                    if (!setReg(regs, dst, static_cast<std::int64_t>(value)))
                        return {false, 0};
                    pc++;
                    break;
                }
                case Op::LoadI8: {
                    const auto offset = static_cast<std::size_t>(getReg(regs, lhs));
                    const auto bytes  = state.memory->read(offset, 1U);
                    if (bytes.size() != 1U || !setReg(regs, dst, bytes[0]))
                        return {false, 0};
                    pc++;
                    break;
                }
                case Op::IndexLoad: {
                    const auto index         = getReg(regs, rhs);
                    const bool raw           = (instr.e & 2U) != 0U;
                    const bool addressResult = (instr.e & 4U) != 0U;
                    if (index < 0)
                        return {false, 0};
                    if (!raw) {
                        const auto signedSize =
                            (instr.e & 1U) != 0U ? getReg(regs, instr.d) : instr.d;
                        if (signedSize < 0 || static_cast<std::uint64_t>(index) >=
                                                  static_cast<std::uint64_t>(signedSize))
                            return {false, 0};
                    }
                    const auto base = getReg(regs, lhs);
                    if (base < 0)
                        return {false, 0};
                    const auto element       = static_cast<std::uint64_t>(instr.imm);
                    const auto unsignedIndex = static_cast<std::uint64_t>(index);
                    const auto baseOffset    = static_cast<std::uint64_t>(base);
                    if (unsignedIndex > (std::numeric_limits<std::uint64_t>::max() / element))
                        return {false, 0};
                    const auto relative = unsignedIndex * element;
                    if (baseOffset > std::numeric_limits<std::uint64_t>::max() - relative)
                        return {false, 0};
                    const auto address = baseOffset + relative;
                    const auto offset  = static_cast<std::size_t>(address);
                    if (static_cast<std::uint64_t>(offset) != address)
                        return {false, 0};
                    if (addressResult) {
                        if (!setReg(regs, dst, static_cast<std::int64_t>(address)))
                            return {false, 0};
                        pc++;
                        break;
                    }
                    const auto bytes = state.memory->read(offset, sizeof(std::uint64_t));
                    if (bytes.size() != sizeof(std::uint64_t))
                        return {false, 0};
                    std::uint64_t value = 0;
                    for (std::size_t i = 0; i < bytes.size(); ++i)
                        value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8U);
                    if (!setReg(regs, dst, static_cast<std::int64_t>(value)))
                        return {false, 0};
                    pc++;
                    break;
                }
                case Op::FieldPtr: {
                    const std::size_t base = static_cast<std::size_t>(getReg(regs, lhs));
                    if (!setReg(regs, dst, static_cast<int64_t>(base + instr.imm)))
                        return {false, 0};
                    pc++;
                    break;
                }
                case Op::MakeSlice:
                    if (!setReg(regs, dst, getReg(regs, lhs)))
                        return {false, 0};
                    if (!setReg(regs, static_cast<uint16_t>(dst + 1), getReg(regs, rhs)))
                        return {false, 0};
                    pc++;
                    break;
                case Op::SlicePtr:
                    if (lhs + 1 >= regs.size())
                        return {false, 0};
                    if (!setReg(regs, dst, regs[lhs]))
                        return {false, 0};
                    pc++;
                    break;
                case Op::SliceLen:
                    if (lhs + 1 >= regs.size())
                        return {false, 0};
                    if (!setReg(regs, dst, regs[lhs + 1]))
                        return {false, 0};
                    pc++;
                    break;
                case Op::MemCopy: {
                    const std::size_t dstOff = static_cast<std::size_t>(getReg(regs, dst));
                    const std::size_t srcOff = static_cast<std::size_t>(getReg(regs, lhs));
                    const std::size_t count  = static_cast<std::size_t>(getReg(regs, rhs));
                    if (!state.memory->copy(dstOff, srcOff, count))
                        return {false, 0};
                    pc++;
                    break;
                }
                case Op::Add:
                case Op::Sub:
                case Op::Mul:
                case Op::Div:
                case Op::Rem:
                case Op::BitAnd:
                case Op::BitOr:
                case Op::BitXor:
                case Op::Shl:
                case Op::Shr: {
                    if (isF64Register(fn, lhs) &&
                        (instr.op == Op::Add || instr.op == Op::Sub || instr.op == Op::Mul ||
                         instr.op == Op::Div || instr.op == Op::Rem)) {
                        const double left  = getF64Register(regs, lhs);
                        const double right = getF64Register(regs, rhs);
                        double result      = 0;
                        if (instr.op == Op::Add)
                            result = left + right;
                        else if (instr.op == Op::Sub)
                            result = left - right;
                        else if (instr.op == Op::Mul)
                            result = left * right;
                        else if (instr.op == Op::Div)
                            result = left / right;
                        else
                            result = std::fmod(left, right);
                        if (!setReg(regs, dst, f64RegisterBits(result)))
                            return {false, 0};
                    } else {
                        const int64_t left  = getReg(regs, lhs);
                        const int64_t right = getReg(regs, rhs);
                        int64_t result      = 0;
                        if (instr.op == Op::Div || instr.op == Op::Rem) {
                            if (right == 0)
                                return {false, 0};
                            result = instr.op == Op::Div ? left / right : left % right;
                        } else if (instr.op == Op::Add) {
                            result = left + right;
                        } else if (instr.op == Op::Sub) {
                            result = left - right;
                        } else if (instr.op == Op::Mul) {
                            result = left * right;
                        } else if (instr.op == Op::BitAnd) {
                            result = left & right;
                        } else if (instr.op == Op::BitOr) {
                            result = left | right;
                        } else if (instr.op == Op::BitXor) {
                            result = left ^ right;
                        } else if (instr.op == Op::Shl) {
                            result = left << (right & 63);
                        } else {
                            result = left >> (right & 63);
                        }
                        if (!setReg(regs, dst, result))
                            return {false, 0};
                    }
                    pc++;
                    break;
                }
                case Op::Neg: {
                    const int64_t result = isF64Register(fn, lhs)
                                               ? f64RegisterBits(-getF64Register(regs, lhs))
                                               : -getReg(regs, lhs);
                    if (!setReg(regs, dst, result))
                        return {false, 0};
                    pc++;
                    break;
                }
                case Op::Not:
                    if (!setReg(regs, dst, getReg(regs, lhs) == 0 ? 1 : 0))
                        return {false, 0};
                    pc++;
                    break;
                case Op::BitNot:
                    if (!setReg(regs, dst, ~getReg(regs, lhs)))
                        return {false, 0};
                    pc++;
                    break;
                case Op::Eq:
                case Op::Ne:
                case Op::Lt:
                case Op::Le:
                case Op::Gt:
                case Op::Ge: {
                    bool result = false;
                    if (isF64Register(fn, lhs) && isF64Register(fn, rhs)) {
                        const double left  = getF64Register(regs, lhs);
                        const double right = getF64Register(regs, rhs);
                        switch (instr.op) {
                        case Op::Eq:
                            result = left <= right && left >= right;
                            break;
                        case Op::Ne:
                            result = !(left <= right && left >= right);
                            break;
                        case Op::Lt:
                            result = left < right;
                            break;
                        case Op::Le:
                            result = left <= right;
                            break;
                        case Op::Gt:
                            result = left > right;
                            break;
                        case Op::Ge:
                            result = left >= right;
                            break;
                        default:
                            break;
                        }
                    } else {
                        const int64_t left  = getReg(regs, lhs);
                        const int64_t right = getReg(regs, rhs);
                        switch (instr.op) {
                        case Op::Eq:
                            result = left == right;
                            break;
                        case Op::Ne:
                            result = left != right;
                            break;
                        case Op::Lt:
                            result = left < right;
                            break;
                        case Op::Le:
                            result = left <= right;
                            break;
                        case Op::Gt:
                            result = left > right;
                            break;
                        case Op::Ge:
                            result = left >= right;
                            break;
                        default:
                            break;
                        }
                    }
                    if (!setReg(regs, dst, result ? 1 : 0))
                        return {false, 0};
                    pc++;
                    break;
                }
                case Op::CallExtern: {
                    if (instr.imm >= state.module->externs.size())
                        return {false, 0};
                    const std::string_view name = state.module->externs[instr.imm];
                    const ExternCall args{getReg(regs, lhs), getReg(regs, rhs),
                                          getReg(regs, instr.d), getReg(regs, instr.e)};
                    std::int64_t result = 0;
                    if (!dispatchScalarExtern(state, name, args, result))
                        return {false, 0};
                    if (!setReg(regs, dst, result))
                        return {false, 0};
                    pc++;
                    break;
                }
                case Op::CallExternRange: {
                    if (instr.imm >= state.module->externs.size())
                        return {false, 0};
                    const auto base  = static_cast<std::size_t>(instr.b);
                    const auto count = static_cast<std::size_t>(instr.c);
                    if (base > regs.size() || count > regs.size() - base)
                        return {false, 0};
                    const std::string_view name = state.module->externs[instr.imm];
                    if (name != "printf" && name != "snprintf") {
                        state.message =
                            "unsupported extern '" + std::string(name) + "' in VM v2 variadic call";
                        return {false, 0};
                    }
                    if ((name == "printf" && count < 1) || (name == "snprintf" && count < 3))
                        return {false, 0};

                    const auto args = std::span<const int64_t>(regs.data() + base, count);
                    std::string formatted;
                    if (name == "printf") {
                        const auto format =
                            state.memory->cstring(static_cast<std::size_t>(args[0]));
                        if (!formatVariadic(format, args, 1, formatted))
                            return {false, 0};
                        state.output.append(formatted);
                    } else {
                        const auto buffer   = args[0];
                        const auto capacity = args[1];
                        const auto format =
                            state.memory->cstring(static_cast<std::size_t>(args[2]));
                        if (capacity <= 0 || !formatVariadic(format, args, 3, formatted))
                            return {false, 0};
                        if (formatted.size() >= static_cast<std::size_t>(capacity))
                            formatted.resize(static_cast<std::size_t>(capacity) - 1U);
                        if (!state.memory->writeString(static_cast<std::size_t>(buffer), formatted))
                            return {false, 0};
                    }
                    if (!setReg(regs, dst, static_cast<int64_t>(formatted.size())))
                        return {false, 0};
                    pc++;
                    break;
                }
                case Op::CallExternRef: {
                    const int64_t ref = getReg(regs, lhs);
                    if (ref < 0 || static_cast<std::size_t>(ref) >= state.module->externs.size())
                        return {false, 0};
                    const std::string_view name =
                        state.module->externs[static_cast<std::size_t>(ref)];
                    const ExternCall args{getReg(regs, rhs), getReg(regs, instr.imm),
                                          getReg(regs, instr.d), getReg(regs, instr.e)};
                    std::int64_t result = 0;
                    if (!dispatchScalarExtern(state, name, args, result))
                        return {false, 0};
                    if (!setReg(regs, dst, result))
                        return {false, 0};
                    pc++;
                    break;
                }
                case Op::CallFn: {
                    if (instr.imm >= state.module->functions.size())
                        return {false, 0};
                    const auto *callee = &state.module->functions[instr.imm];
                    std::vector<int64_t> calleeRegs(callee->regCount, 0);
                    if (callee->paramCount > 0)
                        calleeRegs[0] = getReg(regs, lhs);
                    if (callee->paramCount > 1)
                        calleeRegs[1] = getReg(regs, rhs);
                    const auto result = runFunction(state, *callee, calleeRegs);
                    if (!result.ok)
                        return {false, 0};
                    if (!setReg(regs, dst, result.value))
                        return {false, 0};
                    if (result.hasAux && !setReg(regs, static_cast<uint16_t>(dst + 1), result.aux))
                        return {false, 0};
                    pc++;
                    break;
                }
                case Op::CallRange: {
                    if (instr.imm >= state.module->functions.size())
                        return {false, 0};
                    const auto *callee = &state.module->functions[instr.imm];
                    std::vector<int64_t> calleeRegs(callee->regCount, 0);
                    const std::size_t count = std::min<std::size_t>(instr.c, callee->paramCount);
                    for (std::size_t i = 0; i < count; ++i) {
                        calleeRegs[i] = getReg(regs, static_cast<uint16_t>(instr.b + i));
                    }
                    const auto result = runFunction(state, *callee, calleeRegs);
                    if (!result.ok)
                        return {false, 0};
                    if (!setReg(regs, dst, result.value))
                        return {false, 0};
                    if (result.hasAux && !setReg(regs, static_cast<uint16_t>(dst + 1), result.aux))
                        return {false, 0};
                    pc++;
                    break;
                }
                case Op::TailCallRange: {
                    if (instr.imm >= state.module->functions.size())
                        return {false, 0};
                    const auto *callee = &state.module->functions[instr.imm];
                    if (instr.c != callee->paramCount || callee->paramCount > callee->regCount)
                        return {false, 0};
                    tailRegs.assign(callee->regCount, 0);
                    for (std::size_t i = 0; i < instr.c; ++i)
                        tailRegs[i] = getReg(regs, static_cast<uint16_t>(instr.b + i));
                    tailTarget = callee;
                    pc         = currentFunction->body.size();
                    break;
                }
                case Op::CallFnRef: {
                    const int64_t ref = getReg(regs, lhs);
                    if (ref < 0 || static_cast<std::size_t>(ref) >= state.module->functions.size())
                        return {false, 0};
                    const auto *callee = &state.module->functions[static_cast<std::size_t>(ref)];
                    std::vector<int64_t> calleeRegs(callee->regCount, 0);
                    if (callee->paramCount > 0)
                        calleeRegs[0] = getReg(regs, rhs);
                    if (callee->paramCount > 1)
                        calleeRegs[1] = getReg(regs, instr.imm);
                    const auto result = runFunction(state, *callee, calleeRegs);
                    if (!result.ok)
                        return {false, 0};
                    if (!setReg(regs, dst, result.value))
                        return {false, 0};
                    if (result.hasAux && !setReg(regs, static_cast<uint16_t>(dst + 1), result.aux))
                        return {false, 0};
                    pc++;
                    break;
                }
                case Op::CallRangeFnRef: {
                    const int64_t ref = getReg(regs, lhs);
                    if (ref < 0 || static_cast<std::size_t>(ref) >= state.module->functions.size())
                        return {false, 0};
                    const auto *callee = &state.module->functions[static_cast<std::size_t>(ref)];
                    if (instr.d != callee->paramCount || callee->paramCount > callee->regCount)
                        return {false, 0};
                    std::vector<int64_t> calleeRegs(callee->regCount, 0);
                    for (std::size_t i = 0; i < instr.d; ++i) {
                        calleeRegs[i] = getReg(regs, static_cast<uint16_t>(instr.c + i));
                    }
                    const auto result = runFunction(state, *callee, calleeRegs);
                    if (!result.ok)
                        return {false, 0};
                    if (!setReg(regs, dst, result.value))
                        return {false, 0};
                    if (result.hasAux && !setReg(regs, static_cast<uint16_t>(dst + 1), result.aux))
                        return {false, 0};
                    pc++;
                    break;
                }
                case Op::Ret: {
                    const int64_t value = getReg(regs, dst);
                    if (currentFunction->returnsSlice)
                        return {true, value, true, getReg(regs, static_cast<uint16_t>(dst + 1))};
                    return {true, value};
                }
                case Op::Branch:
                    if (getReg(regs, lhs) != 0)
                        pc = instr.imm;
                    else
                        pc++;
                    break;
                case Op::Branch2:
                    pc = getReg(regs, lhs) != 0 ? instr.imm : instr.d;
                    break;
                case Op::Jump:
                    pc = instr.imm;
                    break;
                case Op::NullDeref:
                    state.message = "R10003: optional value is null";
                    return {false, 0};
                case Op::Panic:
                    state.message = "R" + std::to_string(instr.imm);
                    return {false, 0};
                case Op::Trap:
                    return {false, 0};
                }
            }
            if (tailTarget == nullptr)
                return {false, 0};
        }
        currentFunction = tailTarget;
        regs            = std::move(tailRegs);
    }
}

} // namespace

Vm::Vm(std::size_t linearMemoryCapacity) : memory_(linearMemoryCapacity) {}

auto Vm::runMain(const Module &module) -> RunResult {
    RunResult result;
    const auto *main = findFunction(module, "main");
    if (main == nullptr) {
        result.status  = RunStatus::MissingMain;
        result.message = "the typed IR has no main function";
        return result;
    }
    for (const auto &fn : module.functions) {
        if (!validateFunctionShape(fn)) {
            result.status  = RunStatus::Trap;
            result.message = "invalid typed IR shape";
            return result;
        }
    }

    RunState state;
    state.module = &module;
    state.memory = &memory_;
    state.stringAddresses.reserve(module.strings.size());
    for (std::size_t i = 0; i < module.strings.size(); ++i) {
        const std::string_view text = module.strings[i];
        const std::size_t offset    = memory_.allocBytes(text.size() + 1, 1);
        if (offset == allocationFailure) {
            result.status  = RunStatus::Oom;
            result.message = "cannot materialize string constants";
            return result;
        }
        std::vector<uint8_t> bytes(text.begin(), text.end());
        bytes.push_back(0);
        if (!memory_.write(offset, bytes)) {
            result.status  = RunStatus::Trap;
            result.message = "cannot write string constant";
            return result;
        }
        state.stringAddresses.push_back(offset);
    }
    std::vector<int64_t> regs(main->regCount, 0);
    const auto outcome = runFunction(state, *main, regs);
    if (!outcome.ok) {
        result.status  = RunStatus::Trap;
        result.message = state.message.empty()
                             ? std::string(statusText(RunStatus::Trap)) + " in " +
                                   std::string(state.lastFunction) + " at instruction " +
                                   std::to_string(state.lastInstruction)
                             : state.message;
        return result;
    }

    result.status   = RunStatus::Ok;
    result.exitCode = outcome.value;
    result.output   = state.output.take();
    return result;
}

} // namespace zith::vm
