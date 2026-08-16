#include "common/sir/sir.hpp"

#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using common::memory::Arena;
using toolkit::sir::ArgsDecl;
using toolkit::sir::Block;
using toolkit::sir::Opcode;
using toolkit::sir::Primitives;
using toolkit::sir::SirBuilder;

namespace {

bool check(bool ok, std::string_view message) {
    if (!ok)
        std::fprintf(stderr, "FAIL: %.*s\n", static_cast<int>(message.size()), message.data());
    return ok;
}

bool expectAbort(void (*scenario)()) {
    const pid_t child = fork();
    if (child == 0) {
        scenario();
        std::fprintf(stderr, "FAIL: scenario did not abort\n");
        std::exit(EXIT_FAILURE);
    }
    if (child < 0) {
        std::perror("fork");
        return false;
    }

    int status = 0;
    if (waitpid(child, &status, 0) < 0) {
        std::perror("waitpid");
        return false;
    }
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

bool smokeMain() {
    Arena arena;
    SirBuilder builder{arena};
    auto &module = builder.createModule("sampleName");
    auto &fnMain = module.declareFn("main");
    auto scope = fnMain.pushScope();

    auto &x = scope.declVar("x", Primitives::i32)
                  .makeConstant<Primitives::i32>(5)
                  .setImmutable();
    fnMain.ret(x);

    auto result = verify(module);
    return check(result.isOk(), "main module verifies") &&
           check(x.isImmutable(), "x is immutable") &&
           check(scope->function == &fnMain, "scope handle references its function") &&
           check(fnMain.blocks.size() == 1, "function owns its entry block");
}

bool smokeAdd() {
    Arena arena;
    SirBuilder builder{arena};
    auto &module = builder.createModule("math");
    auto &fnAdd = module.declareFn(
        "add", Primitives::i32, ArgsDecl{Primitives::i32, Primitives::i32});

    auto &sum = fnAdd.add(fnAdd.param(0), fnAdd.param(1));
    fnAdd.ret(sum);

    auto result = verify(module);
    return check(result.isOk(), "add module verifies") &&
           check(fnAdd.nameView() == "add", "function name is interned") &&
           check(sum.opcode == Opcode::Add, "add builder emits Add") &&
           check(sum.block != nullptr, "value owns a block");
}

bool expandedOperations() {
    Arena arena;
    SirBuilder builder{arena};
    auto &module = builder.createModule("expanded-ops");
    auto &fn = module.declareFn("ops", Primitives::i32,
                                ArgsDecl{Primitives::i32, Primitives::i32});

    auto &div = fn.div(fn.param(0), 2);
    auto &rem = fn.rem(fn.param(0), fn.param(1));
    auto &andV = fn.bitAnd(fn.param(0), 1);
    auto &orV = fn.bitOr(fn.param(0), 2);
    auto &xorV = fn.bitXor(fn.param(0), 3);
    auto &shlV = fn.shl(fn.param(0), 1);
    auto &shrV = fn.shr(fn.param(0), 1);
    auto &eq = fn.eq(fn.param(0), fn.param(1));
    auto &ne = fn.ne(fn.param(0), fn.param(1));
    auto &lt = fn.lt(fn.param(0), fn.param(1));
    auto &le = fn.le(fn.param(0), fn.param(1));
    auto &gt = fn.gt(fn.param(0), fn.param(1));
    auto &ge = fn.ge(fn.param(0), fn.param(1));
    fn.ret(fn.add(fn.add(fn.add(andV, orV),
                         fn.add(div, rem)),
                  fn.add(shlV, shrV)));

    const bool opcodes =
        div.opcode == Opcode::Div && rem.opcode == Opcode::Rem &&
        andV.opcode == Opcode::BitAnd && orV.opcode == Opcode::BitOr &&
        xorV.opcode == Opcode::BitXor && shlV.opcode == Opcode::Shl &&
        shrV.opcode == Opcode::Shr && eq.opcode == Opcode::Eq &&
        ne.opcode == Opcode::Ne && lt.opcode == Opcode::Lt &&
        le.opcode == Opcode::Le && gt.opcode == Opcode::Gt &&
        ge.opcode == Opcode::Ge;
    const bool types = div.type == Primitives::i32 && rem.type == Primitives::i32 &&
                       andV.type == Primitives::i32 && orV.type == Primitives::i32 &&
                       xorV.type == Primitives::i32 && shlV.type == Primitives::i32 &&
                       shrV.type == Primitives::i32 && eq.type == Primitives::i1 &&
                       ne.type == Primitives::i1 && lt.type == Primitives::i1 &&
                       le.type == Primitives::i1 && gt.type == Primitives::i1 &&
                       ge.type == Primitives::i1;
    return check(opcodes, "expanded ops produce expected opcodes") &&
           check(types, "expanded ops produce expected types") &&
           check(verify(module).isOk(), "expanded ops module verifies");
}

bool memoryOps() {
    Arena arena;
    SirBuilder builder{arena};
    auto &module = builder.createModule("memory");
    auto &fn = module.declareFn("memory", Primitives::i32, {});
    auto scope = fn.pushScope();
    auto &slot = scope.declVar("slot", Primitives::i32);
    scope.store(slot, 7);
    auto &loaded = scope.load(slot);
    fn.ret(fn.add(loaded, 1));

    auto result = verify(module);
    if (!result.isOk())
        return check(false, "memory module verifies");

    bool storeFound = false;
    bool loadFound = false;
    for (auto *value : fn.values) {
        if (value->opcode == Opcode::Store)
            storeFound = true;
        if (value->opcode == Opcode::Load)
            loadFound = true;
    }
    return check(storeFound && loadFound, "store and load values are recorded") &&
           check(loaded.opcode == Opcode::Load, "load emits Load") &&
           check(loaded.type == Primitives::i32, "load carries the slot type");
}

bool calls() {
    Arena arena;
    SirBuilder builder{arena};
    auto &module = builder.createModule("calls");
    auto &callee = module.declareFn(
        "callee", Primitives::i32, ArgsDecl{Primitives::i32, Primitives::i32});
    callee.ret(callee.add(callee.param(0), callee.param(1)));

    auto &caller = module.declareFn("caller", Primitives::i32, {});
    const toolkit::sir::Operand callOperands[] = {6, 7};
    auto &call = caller.call(callee, callOperands);
    caller.ret(call);

    auto result = verify(module);
    return check(result.isOk(), "call module verifies") &&
           check(call.opcode == Opcode::Call, "call builder emits Call") &&
           check(call.arguments != nullptr, "call has an argument list") &&
           check(call.arguments->size() == 2, "call records argument count") &&
           check(call.callee == &callee, "call resolves the callee reference");
}

bool branchFlow() {
    Arena arena;
    SirBuilder builder{arena};
    auto &module = builder.createModule("branch");
    auto &fn = module.declareFn("branch", Primitives::i32,
                                ArgsDecl{Primitives::i32, Primitives::i32});

    auto thenBlock = fn.pushBlock();
    auto elseBlock = fn.pushBlock();
    auto join = fn.pushBlock();

    Block entry{fn.baseBlock};
    fn.selectBlock(entry);
    auto &cond = fn.lt(fn.param(0), fn.param(1));
    fn.condBranch(cond, thenBlock, elseBlock);

    fn.selectBlock(thenBlock);
    fn.ret(1);

    fn.selectBlock(elseBlock);
    fn.ret(2);

    fn.selectBlock(join);
    fn.ret(0);
    auto result = verify(module);
    return check(result.isOk(), "branch module verifies") &&
           check(fn.blocks.size() == 4, "branch module records blocks") &&
           check(fn.blocks[0]->terminator->opcode == Opcode::CondBranch,
                 "entry block uses a conditional terminator") &&
           check(fn.blocks[1]->id == 1 && fn.blocks[2]->id == 2,
                 "branch targets keep their block ids");
}

bool verifyRejectsMissingTerminator() {
    Arena arena;
    SirBuilder builder{arena};
    auto &module = builder.createModule("missing-ret");
    auto &fnMissing = module.declareFn("missing", Primitives::i32, ArgsDecl{Primitives::i32});
    (void)fnMissing.pushScope();

    auto result = verify(module);
    return check(result.isError(), "persistent structural error reaches verify");
}

void wrongSignatureScenario() {
    Arena arena;
    SirBuilder builder{arena};
    auto &module = builder.createModule("bad");
    auto &fnAdd = module.declareFn(
        "add", Primitives::i32, ArgsDecl{Primitives::i32, Primitives::i32});
    (void)fnAdd.makeArgs(1, 2, 3);
}

void outsideScopeScenario() {
    Arena arena;
    SirBuilder builder{arena};
    auto &module = builder.createModule("bad-scope");
    auto &fnMain = module.declareFn("main");
    auto inner = fnMain.pushScope();
    auto &y = inner.declVar("y", Primitives::i32).makeConstant<Primitives::i32>(1);
    auto sibling = fnMain.pushScope();
    (void)sibling.add(y, 1);
}

void immutableStoreScenario() {
    Arena arena;
    SirBuilder builder{arena};
    auto &module = builder.createModule("immutable");
    auto &fnMain = module.declareFn("main");
    auto scope = fnMain.pushScope();
    auto &x = scope.declVar("x", Primitives::i32).setImmutable();
    (void)x.storeConstant(7);
}

void floatBitwiseScenario() {
    Arena arena;
    SirBuilder builder{arena};
    auto &module = builder.createModule("float-bits");
    auto &fn = module.declareFn("bits", Primitives::f64,
                                ArgsDecl{Primitives::f64, Primitives::f64});
    (void)fn.bitAnd(fn.param(0), fn.param(1));
}

void missingCallTargetScenario() {
    Arena arena;
    SirBuilder builder{arena};
    auto &module = builder.createModule("missing-call");
    auto &fn = module.declareFn("missing-call", Primitives::i32, {});
    const toolkit::sir::InternedId missing = fn.interner->intern("missing");
    const toolkit::sir::Operand callOperand = 1;
    (void)fn.call(missing, std::span<const toolkit::sir::Operand>{&callOperand, 1});
}

void duplicateTerminatorBlockScenario() {
    Arena arena;
    SirBuilder builder{arena};
    auto &module = builder.createModule("dup-term");
    auto &fn = module.declareFn("dup", Primitives::i32, {});
    fn.ret(1);
    fn.ret(2);
}

bool abortScenarios() {
    bool ok = expectAbort(&wrongSignatureScenario);
    ok = expectAbort(&outsideScopeScenario) && ok;
    ok = expectAbort(&immutableStoreScenario) && ok;
    ok = expectAbort(&floatBitwiseScenario) && ok;
    ok = expectAbort(&missingCallTargetScenario) && ok;
    ok = expectAbort(&duplicateTerminatorBlockScenario) && ok;
    return check(ok, "abort-time misuse cases terminate with SIGABRT");
}

} // namespace

int main() {
    bool ok = smokeMain();
    ok = smokeAdd() && ok;
    ok = expandedOperations() && ok;
    ok = memoryOps() && ok;
    ok = calls() && ok;
    ok = branchFlow() && ok;
    ok = verifyRejectsMissingTerminator() && ok;
    ok = abortScenarios() && ok;

    if (!ok) {
        std::fprintf(stderr, "sir tests failed\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
