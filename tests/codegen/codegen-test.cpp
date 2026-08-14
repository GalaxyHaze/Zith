#include "codegen/codegen.hpp"
#include "codegen/vm/vm.hpp"

#include "common/memory/arena.hpp"
#include "common/memory/result.hpp"
#include "common/sir/sir.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <span>
#include <string_view>

using common::memory::Arena;
using toolkit::codegen::ByteCode;
using toolkit::codegen::CodegenState;
using toolkit::codegen::FunctionCode;
using toolkit::codegen::Op;
using toolkit::codegen::vm::StdReturn;
using toolkit::codegen::vm::VM;
using toolkit::sir::ArgsDecl;
using toolkit::sir::Primitives;
using toolkit::sir::SirBuilder;
using toolkit::sir::Type;

namespace {

bool check(bool ok, std::string_view message) {
    if (!ok)
        std::fprintf(stderr, "FAIL: %.*s\n", static_cast<int>(message.size()), message.data());
    return ok;
}

template <typename Number>
std::array<uint8_t, sizeof(Number)> bytesOf(Number value) {
    std::array<uint8_t, sizeof(Number)> bytes{};
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}

template <typename Number>
Number numberFrom(const uint8_t *raw) {
    Number value{};
    std::memcpy(&value, raw, sizeof(value));
    return value;
}

template <typename Number>
bool checkReturn(StdReturn result, Number expected) {
    const Number value = numberFrom<Number>(result.raw);
    if constexpr (std::is_floating_point_v<Number>)
        return check(std::fabs(value - expected) < 1e-9, "raw byte return matches");
    return check(value == expected, "raw byte return matches");
}

bool smokeMain() {
    Arena arena;
    SirBuilder builder{arena};
    auto &module = builder.createModule("codegen-main");
    auto &fnMain = module.declareFn("main", Primitives::i32, {});
    auto scope = fnMain.pushScope();

    auto &x = scope.declVar("x", Primitives::i32)
                  .makeConstant<Primitives::i32>(5)
                  .setImmutable();
    fnMain.ret(x);

    auto bytecode = CodegenState{arena}.flatten(module);
    if (!check(bytecode.isOk(), "main codegen succeeds"))
        return false;
    if (!check(bytecode.value().functions.size() == 1, "main has one function"))
        return false;

    VM vm;
    FunctionCode &fn = bytecode.value().functions[0];
    StdReturn result = vm.run(fn, {});
    return checkReturn<std::int32_t>(result, 5);
}

bool smokeAdd() {
    Arena arena;
    SirBuilder builder{arena};
    auto &module = builder.createModule("codegen-add");
    auto &fnAdd = module.declareFn(
        "add", Primitives::i32, ArgsDecl{Primitives::i32, Primitives::i32});
    fnAdd.ret(fnAdd.add(fnAdd.param(0), fnAdd.param(1)));

    auto bytecode = CodegenState{arena}.flatten(module);
    if (!check(bytecode.isOk(), "add codegen succeeds"))
        return false;

    VM vm;
    FunctionCode &fn = bytecode.value().functions[0];
    const auto left = bytesOf<std::int32_t>(3);
    const auto right = bytesOf<std::int32_t>(4);
    std::array<uint8_t, 8> args{left[0], left[1], left[2], left[3],
                                right[0], right[1], right[2], right[3]};
    StdReturn result = vm.run(fn, std::span<const uint8_t>{args});
    return checkReturn<std::int32_t>(result, 7);
}

bool floatReturn() {
    Arena arena;
    SirBuilder builder{arena};
    auto &module = builder.createModule("codegen-float");
    auto &fnScale = module.declareFn("scale", Primitives::f64, ArgsDecl{Primitives::f64});
    fnScale.ret(fnScale.mul(fnScale.param(0), 2.5));

    auto bytecode = CodegenState{arena}.flatten(module);
    if (!check(bytecode.isOk(), "float codegen succeeds"))
        return false;

    VM vm;
    FunctionCode &fn = bytecode.value().functions[0];
    const auto arg = bytesOf<double>(2.0);
    StdReturn result = vm.run(fn, std::span<const uint8_t>{arg});
    return checkReturn<double>(result, 5.0);
}

bool voidReturn() {
    Arena arena;
    SirBuilder builder{arena};
    auto &module = builder.createModule("codegen-void");
    auto &fnLog = module.declareFn("log");
    fnLog.retVoid();

    auto bytecode = CodegenState{arena}.flatten(module);
    if (!check(bytecode.isOk(), "void codegen succeeds"))
        return false;

    VM vm;
    FunctionCode &fn = bytecode.value().functions[0];
    StdReturn result = vm.run(fn, {});
    bool zeroed = true;
    for (uint8_t byte : result.raw)
        zeroed = zeroed && byte == 0;
    return check(zeroed, "void VM result is zeroed");
}

bool literalCallArgs() {
    Arena arena;
    SirBuilder builder{arena};
    auto &module = builder.createModule("codegen-literals");
    auto &fnAdd = module.declareFn(
        "add", Primitives::i32, ArgsDecl{Primitives::i32, Primitives::i32});

    auto intArgs = fnAdd.makeArgs(4, 4);
    const bool intOk = intArgs.size() == 2 && intArgs.at(0).type == Primitives::i32 &&
                       intArgs.at(1).type == Primitives::i32;

    auto &fnScale = module.declareFn("scale", Primitives::f64, ArgsDecl{Primitives::f64});
    auto floatArgs = fnScale.makeArgs(3.5);
    const bool floatOk = floatArgs.size() == 1 && floatArgs.at(0).type == Primitives::f64;
    return check(intOk, "integer literals materialize with arg types") &&
           check(floatOk, "float literal materializes with arg type");
}

bool mutableStore() {
    Arena arena;
    SirBuilder builder{arena};
    auto &module = builder.createModule("codegen-store");
    auto &fnMain = module.declareFn("main", Primitives::i32, {});
    auto scope = fnMain.pushScope();

    auto &x = scope.declVar("x", Primitives::i32);
    (void)x.storeConstant(7);
    fnMain.ret(x);

    auto bytecode = CodegenState{arena}.flatten(module);
    if (!check(bytecode.isOk(), "mutable store codegen succeeds"))
        return false;

    VM vm;
    FunctionCode &fn = bytecode.value().functions[0];
    StdReturn result = vm.run(fn, {});
    return checkReturn<std::int32_t>(result, 7);
}

bool widthArithmetic() {
    Arena arena;
    SirBuilder builder{arena};
    auto &module = builder.createModule("codegen-widths");

    auto &fnI32 = module.declareFn(
        "addI32", Primitives::i32, ArgsDecl{Primitives::i32, Primitives::i32});
    fnI32.ret(fnI32.add(fnI32.param(0), fnI32.param(1)));

    auto &fnI64 = module.declareFn(
        "addI64", Primitives::i64, ArgsDecl{Primitives::i64, Primitives::i64});
    fnI64.ret(fnI64.add(fnI64.param(0), fnI64.param(1)));

    auto &fnF64 = module.declareFn(
        "addF64", Primitives::f64, ArgsDecl{Primitives::f64, Primitives::f64});
    fnF64.ret(fnF64.add(fnF64.param(0), fnF64.param(1)));

    auto bytecode = CodegenState{arena}.flatten(module);
    if (!check(bytecode.isOk(), "width codegen succeeds"))
        return false;

    bool i32Op = false;
    bool i64Op = false;
    bool f64Op = false;
    for (const FunctionCode &fn : bytecode.value().functions) {
        for (const auto &instruction : fn.instructions) {
            if (instruction.op == Op::AddI32)
                i32Op = true;
            if (instruction.op == Op::AddI64)
                i64Op = true;
            if (instruction.op == Op::AddF64)
                f64Op = true;
        }
    }
    if (!check(i32Op && i64Op && f64Op, "flatten picks typed width opcodes"))
        return false;

    VM vm;
    auto lhs32 = bytesOf<std::int32_t>(21);
    auto rhs32 = bytesOf<std::int32_t>(21);
    std::array<uint8_t, 8> args32{lhs32[0], lhs32[1], lhs32[2], lhs32[3],
                                  rhs32[0], rhs32[1], rhs32[2], rhs32[3]};
    if (!checkReturn(vm.run(bytecode.value().functions[0],
                            std::span<const uint8_t>{args32}),
                     std::int32_t(42)))
        return false;

    auto lhs64 = bytesOf<std::int64_t>(20);
    auto rhs64 = bytesOf<std::int64_t>(22);
    std::array<uint8_t, 16> args64{};
    std::memcpy(args64.data(), lhs64.data(), lhs64.size());
    std::memcpy(args64.data() + 8, rhs64.data(), rhs64.size());
    if (!checkReturn(vm.run(bytecode.value().functions[1],
                            std::span<const uint8_t>{args64}),
                     std::int64_t(42)))
        return false;

    auto lhsF = bytesOf<double>(1.25);
    auto rhsF = bytesOf<double>(2.75);
    std::array<uint8_t, 16> argsF{};
    std::memcpy(argsF.data(), lhsF.data(), lhsF.size());
    std::memcpy(argsF.data() + 8, rhsF.data(), rhsF.size());
    return checkReturn(vm.run(bytecode.value().functions[2], std::span<const uint8_t>{argsF}),
                       4.0);
}

bool flattenRejectsMissingTerminator() {
    Arena arena;
    SirBuilder builder{arena};
    auto &module = builder.createModule("codegen-missing-ret");
    auto &fnMissing = module.declareFn("missing", Primitives::i32, ArgsDecl{Primitives::i32});
    (void)fnMissing.pushScope();

    auto result = CodegenState{arena}.flatten(module);
    return check(result.isError(), "missing terminator is a flatten Result error");
}

bool flattenRejectsForeignOperand() {
    Arena arena;
    SirBuilder builder{arena};
    auto &module = builder.createModule("codegen-foreign");
    auto &fnForeign = module.declareFn("foreign", Primitives::i32, {});
    auto foreignScope = fnForeign.pushScope();
    auto &foreign = foreignScope.constInt64(9, Primitives::i32);
    fnForeign.ret(foreign);

    auto &fnTarget = module.declareFn("target", Primitives::i32, {});
    (void)fnTarget.pushScope();
    auto &local = fnTarget.constInt64(4, Primitives::i32);
    fnTarget.ret(local);
    fnTarget.instructions.back()->value = &foreign;

    auto result = CodegenState{arena}.flatten(module);
    return check(result.isError(), "foreign operand is a flatten Result error");
}

bool flattenRejectsWrongInitializerType() {
    Arena arena;
    SirBuilder builder{arena};
    auto &module = builder.createModule("codegen-wrong-init");
    auto &fnMain = module.declareFn("main", Primitives::i32, {});
    auto scope = fnMain.pushScope();
    auto &x = scope.declVar("x", Primitives::i32);
    x.initializer = &scope.constInt64(7, Primitives::i64);
    fnMain.ret(x);

    auto result = CodegenState{arena}.flatten(module);
    return check(result.isError(), "wrong initializer type is a flatten Result error");
}

bool backendDefault() {
    Arena arena;
    SirBuilder builder{arena};
    auto &module = builder.createModule("codegen-backend");
    auto &fnMain = module.declareFn("main", Primitives::i32, {});
    auto scope = fnMain.pushScope();
    auto &x = scope.declVar("x", Primitives::i32).makeConstant<Primitives::i32>(1);
    fnMain.ret(x);

    auto bytecode = CodegenState{arena}.runBackend(module);
    return check(bytecode.isOk() && bytecode.value().functions.size() == 1,
                 "runBackend uses VM by default");
}

} // namespace

int main() {
    bool ok = smokeMain();
    ok = smokeAdd() && ok;
    ok = floatReturn() && ok;
    ok = voidReturn() && ok;
    ok = literalCallArgs() && ok;
    ok = mutableStore() && ok;
    ok = widthArithmetic() && ok;
    ok = flattenRejectsMissingTerminator() && ok;
    ok = flattenRejectsForeignOperand() && ok;
    ok = flattenRejectsWrongInitializerType() && ok;
    ok = backendDefault() && ok;

    if (!ok) {
        std::fprintf(stderr, "codegen tests failed\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
