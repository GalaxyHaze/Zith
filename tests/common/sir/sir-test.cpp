#include "common/sir/sir.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using common::memory::Arena;
using toolkit::sir::ArgsDecl;
using toolkit::sir::Primitives;
using toolkit::sir::SirBuilder;

namespace {

bool check(bool ok, const char *message) {
    if (!ok)
        std::fprintf(stderr, "FAIL: %s\n", message);
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
           check(scope->function == &fnMain, "scope handle references its function");
}

bool smokeAdd() {
    Arena arena;
    SirBuilder builder{arena};
    auto &module = builder.createModule("math");
    auto &fnAdd = module.declareFn(
        "add", Primitives::i32, ArgsDecl{Primitives::i32, Primitives::i32});

    fnAdd.ret(fnAdd.add(fnAdd.param(0), fnAdd.param(1)));

    auto result = verify(module);
    return check(result.isOk(), "add module verifies") &&
           check(fnAdd.nameView() == "add", "function name is interned");
}

bool literalCallArgs() {
    Arena arena;
    SirBuilder builder{arena};
    auto &module = builder.createModule("literal");
    auto &fnAdd = module.declareFn(
        "add", Primitives::i32, ArgsDecl{Primitives::i32, Primitives::i32});

    auto args = fnAdd.makeArgs(4, 4);
    bool ok = args.size() == 2;
    ok = ok && args.at(0).type == Primitives::i32;
    ok = ok && args.at(1).type == Primitives::i32;
    ok = ok && args.at(0).opcode == toolkit::sir::Opcode::Constant;
    return check(ok, "integer literals materialize with parameter types");
}

bool floatLiteralCallArgs() {
    Arena arena;
    SirBuilder builder{arena};
    auto &module = builder.createModule("literal-float");
    auto &fnScale = module.declareFn("scale", Primitives::f64, ArgsDecl{Primitives::f64});

    auto args = fnScale.makeArgs(3.5);
    const bool ok = args.size() == 1 && args.at(0).type == Primitives::f64 &&
                    args.at(0).opcode == toolkit::sir::Opcode::Constant;
    return check(ok, "float literals materialize with parameter types");
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

bool abortScenarios() {
    bool ok = expectAbort(&wrongSignatureScenario);
    ok = expectAbort(&outsideScopeScenario) && ok;
    ok = expectAbort(&immutableStoreScenario) && ok;
    return check(ok, "abort-time misuse cases terminate with SIGABRT");
}

} // namespace

int main() {
    bool ok = smokeMain();
    ok = smokeAdd() && ok;
    ok = literalCallArgs() && ok;
    ok = floatLiteralCallArgs() && ok;
    ok = verifyRejectsMissingTerminator() && ok;
    ok = abortScenarios() && ok;

    if (!ok) {
        std::fprintf(stderr, "sir tests failed\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
