#include "common/sir/flat/flat.hpp"
#include "common/sir/sir.hpp"

#include "common/memory/arena.hpp"
#include "common/memory/result.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

using common::memory::Arena;
using common::memory::Result;
using toolkit::sir::ArgsDecl;
using toolkit::sir::Block;
using toolkit::sir::Mutability;
using toolkit::sir::Primitives;
using toolkit::sir::SirBuilder;
using toolkit::sir::flat::FlatFunction;
using toolkit::sir::flat::FlatModule;
using toolkit::sir::flat::FlatOp;
using toolkit::sir::flat::deserializeFlatModule;
using toolkit::sir::flat::serializeFlatModule;

namespace {

bool check(bool ok, std::string_view message) {
    if (!ok)
        std::fprintf(stderr, "FAIL: %.*s\n", static_cast<int>(message.size()), message.data());
    return ok;
}

bool roundtripMatches(const FlatModule &source, const FlatModule &roundtrip) {
    if (!check(roundtrip.arena != nullptr, "roundtrip has an arena"))
        return false;
    if (!check(roundtrip.interner != nullptr, "roundtrip has an interner"))
        return false;
    if (!check(roundtrip.interner->lookup(roundtrip.name) == source.interner->lookup(source.name),
               "module name round-trips"))
        return false;
    if (!check(roundtrip.types.size() == source.types.size(), "type count round-trips"))
        return false;
    if (!check(roundtrip.functions.size() == source.functions.size(),
               "function count round-trips"))
        return false;
    return true;
}

bool scalarOpsFlatten() {
    Arena arena;
    SirBuilder builder(arena);
    auto &module = builder.createModule("scalar-flat");
    auto &fn = module.declareFn("scalar", Primitives::i32,
                                ArgsDecl{Primitives::i32, Primitives::i32});
    auto &two = fn.constInt64(2, Primitives::i32);
    auto &three = fn.constInt64(3, Primitives::i32);
    fn.ret(fn.add(fn.sub(fn.mul(fn.param(0), fn.param(1)), fn.div(fn.param(0), two)),
                  fn.rem(fn.param(1), three)));

    auto flat = toolkit::sir::flat::flattenModule(module);
    if (!check(flat.isOk(), "scalar module flattens"))
        return false;
    const FlatModule &flatModule = flat.value();
    if (!check(flatModule.functions.size() == 1, "scalar flat has one function"))
        return false;
    const FlatFunction &flatFunction = *flatModule.functions[0];
    if (!check(flatFunction.values->size() >= 9, "scalar flat materializes constants"))
        return false;

    bool foundAdd = false;
    bool foundDiv = false;
    for (const auto &value : *flatFunction.values) {
        if (value.op == FlatOp::Add)
            foundAdd = true;
        if (value.op == FlatOp::Div)
            foundDiv = true;
    }
    return check(foundAdd && foundDiv, "scalar flat keeps arithmetic opcodes");
}

bool allExpandedOpsFlatten() {
    Arena arena;
    SirBuilder builder(arena);
    auto &module = builder.createModule("expanded-flat");
    auto &fn = module.declareFn("expanded", Primitives::i32,
                                ArgsDecl{Primitives::i32, Primitives::i32});
    auto &two = fn.constInt64(2, Primitives::i32);
    auto &three = fn.constInt64(3, Primitives::i32);
    auto &div = fn.div(fn.param(0), two);
    auto &rem = fn.rem(fn.param(0), three);
    auto &andValue = fn.bitAnd(fn.param(0), fn.param(1));
    auto &orValue = fn.bitOr(fn.param(0), fn.param(1));
    auto &xorValue = fn.bitXor(fn.param(0), fn.param(1));
    auto &shl = fn.shl(fn.param(0), fn.param(1));
    auto &shr = fn.shr(fn.param(0), fn.param(1));
    (void)fn.eq(fn.param(0), fn.param(1));
    (void)fn.ne(fn.param(0), fn.param(1));
    (void)fn.lt(fn.param(0), fn.param(1));
    (void)fn.le(fn.param(0), fn.param(1));
    (void)fn.gt(fn.param(0), fn.param(1));
    (void)fn.ge(fn.param(0), fn.param(1));
    fn.ret(fn.add(fn.add(fn.add(fn.add(fn.add(fn.add(div, rem),
                                              andValue),
                                        orValue),
                                  xorValue),
                            shl),
                      shr));

    auto flat = toolkit::sir::flat::flattenModule(module);
    if (!check(flat.isOk(), "expanded module flattens"))
        return false;
    const FlatFunction &function = *flat.value().functions[0];
    int expected = 0;
    for (const auto &value : *function.values) {
        if (value.op == FlatOp::Div)
            ++expected;
        if (value.op == FlatOp::Rem)
            ++expected;
        if (value.op == FlatOp::BitAnd)
            ++expected;
        if (value.op == FlatOp::BitOr)
            ++expected;
        if (value.op == FlatOp::BitXor)
            ++expected;
        if (value.op == FlatOp::Shl)
            ++expected;
        if (value.op == FlatOp::Shr)
            ++expected;
        if (value.op == FlatOp::Eq)
            ++expected;
        if (value.op == FlatOp::Ne)
            ++expected;
        if (value.op == FlatOp::Lt)
            ++expected;
        if (value.op == FlatOp::Le)
            ++expected;
        if (value.op == FlatOp::Gt)
            ++expected;
        if (value.op == FlatOp::Ge)
            ++expected;
    }
    return check(expected == 13, "expanded flat keeps all SIR opcodes");
}

bool memoryAndCallFlatten() {
    Arena arena;
    SirBuilder builder(arena);
    auto &module = builder.createModule("mem-call-flat");
    auto &callee = module.declareFn("callee", Primitives::i32,
                                    ArgsDecl{Primitives::i32, Primitives::i32});
    callee.ret(callee.add(callee.param(0), callee.param(1)));

    auto &caller = module.declareFn("caller", Primitives::i32, {});
    auto scope = caller.pushScope();
    auto &slot = scope.declVar("slot", Primitives::i32);
    scope.store(slot, 7);
    auto &loaded = scope.load(slot);
    const toolkit::sir::Operand operands[] = {loaded, 1};
    auto &call = caller.call(callee, operands);
    caller.ret(call);

    auto flat = toolkit::sir::flat::flattenModule(module);
    if (!check(flat.isOk(), "memory/call module flattens"))
        return false;
    const FlatModule &flatModule = flat.value();
    if (!check(flatModule.functions.size() == 2, "memory/call keeps callee and caller"))
        return false;
    const FlatFunction &callerFlat = *flatModule.functions[1];
    bool storeFound = false;
    bool loadFound = false;
    bool callFound = false;
    for (const auto &value : *callerFlat.values) {
        if (value.op == FlatOp::Store)
            storeFound = true;
        if (value.op == FlatOp::Load)
            loadFound = true;
        if (value.op == FlatOp::Call) {
            callFound = true;
            if (!check(value.calleeFunction == 0, "flat call resolves the callee index"))
                return false;
            if (!check(value.args != nullptr && value.args->size() == 2,
                       "flat call keeps arity"))
                return false;
        }
    }
    return check(storeFound && loadFound && callFound, "memory and call ops flatten");
}

bool branchRoundtrip() {
    Arena arena;
    SirBuilder builder(arena);
    auto &module = builder.createModule("branch-flat");
    auto &fn = module.declareFn("branch", Primitives::i32,
                                ArgsDecl{Primitives::i32, Primitives::i32});

    auto thenBlock = fn.pushBlock();
    auto elseBlock = fn.pushBlock();
    auto join = fn.pushBlock();

    Block entry{fn.baseBlock};
    fn.selectBlock(entry);
    auto &condition = fn.lt(fn.param(0), fn.param(1));
    fn.condBranch(condition, thenBlock, elseBlock);

    fn.selectBlock(thenBlock);
    fn.ret(1);
    fn.selectBlock(elseBlock);
    fn.ret(2);
    fn.selectBlock(join);
    fn.ret(0);

    auto flat = toolkit::sir::flat::flattenModule(module);
    if (!check(flat.isOk(), "branch module flattens"))
        return false;
    const FlatModule &flatModule = flat.value();
    const FlatFunction &function = *flatModule.functions[0];
    if (!check(function.blocks->size() == 4, "branch flat has four blocks"))
        return false;
    if (!check((*function.blocks)[0].terminator.op == FlatOp::CondBranch,
               "entry flat uses CondBranch"))
        return false;
    if (!check((*function.blocks)[0].terminator.condition != toolkit::sir::flat::invalidIndex,
               "branch condition is indexed"))
        return false;
    if (!check((*function.blocks)[0].terminator.trueTarget == 1 &&
               (*function.blocks)[0].terminator.falseTarget == 2,
               "branch targets keep block order"))
        return false;

    auto bytes = serializeFlatModule(flatModule);
    if (!check(bytes.isOk(), "branch module serializes"))
        return false;
    Arena roundtripArena;
    auto decoded = deserializeFlatModule(roundtripArena,
                                         std::string_view(reinterpret_cast<const char *>(
                                                              bytes.value().data()),
                                                          bytes.value().size()));
    if (!check(decoded.isOk(), "branch module deserializes"))
        return false;
    if (!check(decoded.value().functions[0]->blocks->size() == 4,
               "deserialized branch keeps block count"))
        return false;
    return check((*decoded.value().functions[0]->blocks)[0].terminator.op == FlatOp::CondBranch,
                 "deserialized branch keeps CondBranch");
}

bool structuralTypesRoundtrip() {
    Arena arena;
    SirBuilder builder(arena);
    auto &module = builder.createModule("types-flat");
    (void)module.arrayType(Primitives::i32, 16);
    (void)module.sliceType(module.arrayType(Primitives::i32, 4));
    (void)module.pointerType(module.arrayType(Primitives::i32, 8));
    auto &fn = module.declareFn("typed", Primitives::voidT, ArgsDecl{});
    fn.retVoid();

    auto flat = toolkit::sir::flat::flattenModule(module);
    if (!check(flat.isOk(), "structural types flatten"))
        return false;
    if (!check(flat.value().types.size() >= 5, "structural types add type records"))
        return false;
    auto bytes = serializeFlatModule(flat.value());
    if (!check(bytes.isOk(), "structural types serialize"))
        return false;
    Arena roundtripArena;
    auto decoded = deserializeFlatModule(
        roundtripArena,
        std::string_view(reinterpret_cast<const char *>(bytes.value().data()),
                         bytes.value().size()));
    return check(decoded.isOk(), "structural types deserialize") &&
           roundtripMatches(flat.value(), decoded.value());
}

bool malformedRejected() {
    const std::vector<std::uint8_t> bytes{1, 2, 3, 4};
    Arena arena;
    auto decoded = deserializeFlatModule(arena,
                                         std::string_view(reinterpret_cast<const char *>(
                                                              bytes.data()),
                                                          bytes.size()));
    if (!check(decoded.isError(), "malformed flat stream is rejected"))
        return false;

    Arena validArena;
    SirBuilder builder(validArena);
    auto &module = builder.createModule("truncated-flat");
    auto &fn = module.declareFn("one", Primitives::i32, ArgsDecl{});
    fn.ret(1);
    auto flat = serializeFlatModule(toolkit::sir::flat::flattenModule(module).value());
    if (!check(flat.isOk(), "valid flat stream serializes for truncation test"))
        return false;
    auto full = flat.value();
    const std::size_t size = full.empty() ? 0 : full.size() - 1;
    Arena truncationArena;
    auto truncated = deserializeFlatModule(
        truncationArena,
        std::string_view(reinterpret_cast<const char *>(full.data()), size));
    return check(truncated.isError(), "truncated flat stream is rejected");
}

} // namespace

int main() {
    bool ok = scalarOpsFlatten();
    ok = allExpandedOpsFlatten() && ok;
    ok = memoryAndCallFlatten() && ok;
    ok = branchRoundtrip() && ok;
    ok = structuralTypesRoundtrip() && ok;
    ok = malformedRejected() && ok;

    if (!ok) {
        std::fprintf(stderr, "sir-flat tests failed\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
