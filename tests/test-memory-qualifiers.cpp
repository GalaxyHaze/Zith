#include "cli/options.hpp"
#include "diagnostics/diagnostic-engine.hpp"
#include "diagnostics/error-codes.hpp"
#include "hir/hir-attrs.hpp"
#include "session/compilation-session.hpp"
#include "session/frontend-context.hpp"
#include "session/pipeline-plan.hpp"
#include "test-common.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace zith;

namespace {

namespace fs = std::filesystem;

struct Result {
    bool ok           = false;
    size_t errorCount = 0;
    std::vector<uint32_t> codes;

    bool hasErrorCode(diagnostics::ErrCode code) const {
        for (auto c : codes) {
            if (c == static_cast<uint32_t>(code))
                return true;
        }
        return false;
    }
};

struct Workspace {
    fs::path root = fs::temp_directory_path() / "zith-memory-qualifier-tests";

    Workspace() {
        fs::remove_all(root);
        fs::create_directories(root);
    }

    ~Workspace() {
        fs::remove_all(root);
    }

    void write(std::string_view name, std::string_view text) const {
        const auto path = root / name;
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << text;
    }
};

Result check(std::string_view source) {
    Workspace workspace;
    workspace.write("main.zith", source);

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::TypeChecked;

    session::FrontendConfig config;
    config.workspaceRoot      = workspace.root.string();
    config.maxFrontendWorkers = 1;
    config.compilerVersion    = "test";
    auto context              = std::make_shared<session::FrontendContext>(config);

    session::CompilationSession session(options, (workspace.root / "main.zith").string(), context);
    session.setBuffered(true);

    Result result;
    result.ok = session.runTo(session::Stage::TypeChecked);
    for (const auto &diagnostic : session.diags().all()) {
        if (diagnostic.severity != diagnostics::Severity::Error)
            continue;
        ++result.errorCount;
        result.codes.push_back(diagnostic.code);
        std::printf("    [Diag] Code: %u, Message: %s\n", diagnostic.code,
                    diagnostic.message.c_str());
    }
    result.ok = result.ok && result.errorCount == 0;
    return result;
}

struct HirResult {
    bool ok = false;
    std::vector<uint32_t> codes;
    std::unique_ptr<session::CompilationSession> session;
    memory::Arena arena;

    bool hasErrorCode(diagnostics::ErrCode code) const {
        for (const auto entry : codes) {
            if (entry == static_cast<uint32_t>(code))
                return true;
        }
        return false;
    }
};

HirResult checkHir(std::string_view source) {
    Workspace workspace;
    workspace.write("main.zith", source);

    HirResult result;
    Options options(result.arena);
    options.targetStage = session::Stage::HirLowered;

    session::FrontendConfig config;
    config.workspaceRoot      = workspace.root.string();
    config.maxFrontendWorkers = 1;
    config.compilerVersion    = "test";
    auto context              = std::make_shared<session::FrontendContext>(config);

    result.session = std::make_unique<session::CompilationSession>(
        options, (workspace.root / "main.zith").string(), context);
    result.session->setBuffered(true);
    result.ok = result.session->runTo(session::Stage::HirLowered);
    for (const auto &diagnostic : result.session->diags().all()) {
        if (diagnostic.severity != diagnostics::Severity::Error)
            continue;
        result.codes.push_back(diagnostic.code);
        std::printf("    [Diag] Code: %u, Message: %s\n", diagnostic.code,
                    diagnostic.message.c_str());
    }
    result.ok = result.ok && result.codes.empty();
    return result;
}

const hir::HirSlotAttrs *findSlotAttr(const hir::HirAttrs &attrs, hir::HirOwnership ownership) {
    for (size_t slot = 0; slot < attrs.slotCount(); ++slot) {
        const auto *slot_attrs = attrs.trySlot(static_cast<hir::HirSlotId>(slot));
        if (slot_attrs != nullptr && slot_attrs->ownership == ownership)
            return slot_attrs;
    }
    return nullptr;
}

// ── Accepted placements ──────────────────────────────────────

void qualifiersOnParameters() {
    auto r = check("struct P { x: i32 }\n"
                   "fn a(p: lend P): i32 { return p.x; }\n"
                   "fn b(p: view P): i32 { return p.x; }\n"
                   "fn c(p: unique P): i32 { return p.x; }\n"
                   "fn d(p: share P): i32 { return p.x; }\n"
                   "fn e(p: belong P): i32 { return p.x; }\n"
                   "fn f(p: mut P): i32 { return p.x; }\n"
                   "fn main(): i32 { return 0; }\n");
    CHECK(r.ok, "all six qualifiers are accepted on a parameter type");
}

void qualifiersOnReturnTypes() {
    auto r = check("fn a(v: i32): view i32 { return v; }\n"
                   "fn b(v: i32): lend i32 { return v; }\n"
                   "fn c(v: i32): mut i32 { return v; }\n"
                   "fn main(): i32 { return 0; }\n");
    CHECK(r.ok, "qualifiers are accepted on a return type");
}

void qualifiersOnStructFields() {
    auto r = check("struct P { x: mut i32, y: view i32, z: lend i32 }\n"
                   "fn main(): i32 { return 0; }\n");
    CHECK(r.ok, "qualifiers are accepted on struct fields");
}

void qualifiersOnLetAnnotations() {
    auto r = check("fn main(): i32 {\n"
                   "    let n: view i32 = 3;\n"
                   "    var m: mut i32 = 4;\n"
                   "    let u: unique i32 = 5;\n"
                   "    return n + m + u;\n"
                   "}\n");
    CHECK(r.ok, "qualifiers are accepted on a 'let'/'var' annotation");
}

void qualifiersCombineWithTypeConstructors() {
    auto r = check("struct P { x: i32 }\n"
                   "fn a(o: ?lend P): i32 { return 0; }\n"
                   "fn b(s: []view i32): i32 { return 0; }\n"
                   "fn c(q: lend *i32): i32 { return *q; }\n"
                   "fn d(o: view ?i32): i32 { return 0; }\n"
                   "fn main(): i32 { return 0; }\n");
    CHECK(r.ok, "qualifiers compose with '?', '[]' and '*'");
}

void writeThroughLendIsAllowed() {
    auto r = check("struct P { x: i32 }\n"
                   "fn bump(p: lend P): i32 {\n"
                   "    p.x = 5;\n"
                   "    return p.x;\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    var q: P = P { x: 1 };\n"
                   "    return bump(q);\n"
                   "}\n");
    CHECK(r.ok, "writing through a 'lend' binding is allowed");
}

// ── Rejected uses ────────────────────────────────────────────

void writeThroughViewIsRejected() {
    auto r = check("struct P { x: i32 }\n"
                   "fn bump(p: view P): i32 {\n"
                   "    p.x = 5;\n"
                   "    return p.x;\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    var q: P = P { x: 1 };\n"
                   "    return bump(q);\n"
                   "}\n");
    CHECK(!r.ok, "writing through a 'view' binding is rejected");
    CHECK(r.hasErrorCode(diagnostics::err::WriteThroughView),
          "a write through 'view' reports WriteThroughView (E4004)");
}

void writeThroughViewLocalIsRejected() {
    auto r = check("fn main(): i32 {\n"
                   "    var n: view i32 = 1;\n"
                   "    n = 2;\n"
                   "    return n;\n"
                   "}\n");
    CHECK(!r.ok, "assigning to a 'view' local is rejected");
    CHECK(r.hasErrorCode(diagnostics::err::WriteThroughView),
          "assigning to a 'view' local reports WriteThroughView (E4004)");
}

void twoOwnershipQualifiersAreRejected() {
    auto r = check("fn f(a: lend view i32): i32 { return a; }\n"
                   "fn main(): i32 { return 0; }\n");
    CHECK(!r.ok, "two ownership qualifiers on one type are rejected");
    CHECK(r.hasErrorCode(diagnostics::err::ExpectedExpr),
          "two ownership qualifiers report a parse error (E1001)");
}

void mutViewIsRejected() {
    auto r = check("fn f(a: mut view i32): i32 { return a; }\n"
                   "fn main(): i32 { return 0; }\n");
    CHECK(!r.ok, "'mut view T' is rejected");
    CHECK(r.hasErrorCode(diagnostics::err::ExpectedExpr),
          "'mut view T' reports a parse error (E1001)");
}

void duplicateMutIsRejected() {
    auto r = check("fn f(a: mut mut i32): i32 { return a; }\n"
                   "fn main(): i32 { return 0; }\n");
    CHECK(!r.ok, "a repeated 'mut' qualifier is rejected");
    CHECK(r.hasErrorCode(diagnostics::err::ExpectedExpr),
          "a repeated 'mut' reports a parse error (E1001)");
}

// ── Residual HIR facts ──────────────────────────────────────

void forwardingParameterReturnIsNonConsumed() {
    auto r = checkHir("struct P { x: i32 }\n"
                      "fn forward(p: unique P): unique P { p }\n"
                      "fn main(): i32 {\n"
                      "    var owner: unique P = P { x: 1 };\n"
                      "    var back: unique P = forward(owner);\n"
                      "    return back.x;\n"
                      "}\n");
    CHECK(r.ok, "a function forwarding its unique parameter lowers");

    const auto &hir   = r.session->hirModule();
    const auto &attrs = hir.attrs();
    bool saw_unique   = false;
    for (size_t slot = 0; slot < attrs.slotCount(); ++slot) {
        const auto *slot_attrs = attrs.trySlot(static_cast<hir::HirSlotId>(slot));
        saw_unique |= slot_attrs != nullptr && slot_attrs->ownership == hir::HirOwnership::Unique;
    }
    CHECK(saw_unique, "forwarded unique slots carry residual ownership");

    bool saw_forward_call = false;
    for (size_t call = 0; call < attrs.callCount(); ++call) {
        const auto *call_attrs = attrs.tryCall(static_cast<hir::HirExprId>(call));
        saw_forward_call |= call_attrs != nullptr && call_attrs->returnsArg == 0u;
    }
    CHECK(saw_forward_call, "forwarding call marks the returned node as argument 0");
}

void duplicatedArgumentsDistinguishMoveFromShare() {
    auto r = checkHir("fn take_two(a: i32, b: i32): i32 { a }\n"
                      "fn main(): i32 {\n"
                      "    let n: i32 = 3;\n"
                      "    take_two(n, n) + 1\n"
                      "}\n");
    CHECK(r.ok, "duplicated arguments in a call lower");

    const auto &hir   = r.session->hirModule();
    const auto &attrs = hir.attrs();
    bool saw_move     = false;
    bool saw_borrow   = false;
    for (size_t call = 0; call < attrs.callCount(); ++call) {
        const auto *call_attrs = attrs.tryCall(static_cast<hir::HirExprId>(call));
        if (call_attrs == nullptr)
            continue;
        for (const auto &arg : call_attrs->args) {
            saw_move |= arg.escape == hir::HirCallEscape::Move;
            saw_borrow |= arg.escape == hir::HirCallEscape::Borrow;
        }
    }
    CHECK(saw_move, "a duplicated default/unique/lend argument carries a move fact");
    CHECK(!saw_borrow, "the duplicate is not silently left as an ordinary borrow");
}

void shareDuplicateStaysBorrowable() {
    auto r = checkHir("fn take_two(a: share P, b: share P): i32 { a.x }\n"
                      "struct P { x: i32 }\n"
                      "fn main(): i32 {\n"
                      "    let p: share P = P { x: 2 };\n"
                      "    take_two(p, p) + 1\n"
                      "}\n");
    CHECK(r.ok, "duplicated share arguments lower");

    const auto &hir   = r.session->hirModule();
    const auto &attrs = hir.attrs();
    bool saw_move     = false;
    bool saw_borrow   = false;
    for (size_t call = 0; call < attrs.callCount(); ++call) {
        const auto *call_attrs = attrs.tryCall(static_cast<hir::HirExprId>(call));
        if (call_attrs == nullptr)
            continue;
        for (const auto &arg : call_attrs->args) {
            saw_move |= arg.escape == hir::HirCallEscape::Move;
            saw_borrow |= arg.escape == hir::HirCallEscape::Borrow;
        }
    }
    CHECK(!saw_move, "duplicated share arguments do not become moves");
    CHECK(saw_borrow, "duplicated share arguments stay borrowable");
}

void belongEscapeIsRecorded() {
    auto r = checkHir("struct Child { v: i32 }\n"
                      "struct Owner { child: belong Child }\n"
                      "fn keep(c: Child): i32 { c.v }\n"
                      "fn main(): i32 {\n"
                      "    let own: Owner = Owner { child: Child { v: 4 } };\n"
                      "    keep(own.child) + 1\n"
                      "}\n");
    CHECK(r.ok, "a belong field passed to a call lowers");

    const auto &hir   = r.session->hirModule();
    const auto &attrs = hir.attrs();
    bool saw_escape   = false;
    for (size_t call = 0; call < attrs.callCount(); ++call) {
        const auto *call_attrs = attrs.tryCall(static_cast<hir::HirExprId>(call));
        if (call_attrs == nullptr)
            continue;
        for (const auto &arg : call_attrs->args) {
            saw_escape |= arg.escape == hir::HirCallEscape::Escape;
        }
    }
    CHECK(saw_escape, "escaping a belong field records an escape fact");
}

void nonNullNarrowingSurvivesAsSlotFact() {
    auto r = checkHir("fn main(): i32 {\n"
                      "    var p: ?*i32 = null;\n"
                      "    if (not (p is null)) {\n"
                      "        return 7;\n"
                      "    }\n"
                      "    return 0;\n"
                      "}\n");
    CHECK(r.ok, "non-null narrowing source lowers");

    const auto &hir   = r.session->hirModule();
    const auto &attrs = hir.attrs();
    bool saw_non_null = false;
    for (size_t slot = 0; slot < attrs.slotCount(); ++slot) {
        const auto *slot_attrs = attrs.trySlot(static_cast<hir::HirSlotId>(slot));
        saw_non_null |= slot_attrs != nullptr && slot_attrs->nonNull;
    }
    CHECK(saw_non_null, "NonNull survives as a residual slot fact");
}

} // namespace

void test_memory_qualifiers() {
    qualifiersOnParameters();
    qualifiersOnReturnTypes();
    qualifiersOnStructFields();
    qualifiersOnLetAnnotations();
    qualifiersCombineWithTypeConstructors();
    writeThroughLendIsAllowed();
    writeThroughViewIsRejected();
    writeThroughViewLocalIsRejected();
    twoOwnershipQualifiersAreRejected();
    mutViewIsRejected();
    duplicateMutIsRejected();
    forwardingParameterReturnIsNonConsumed();
    duplicatedArgumentsDistinguishMoveFromShare();
    shareDuplicateStaysBorrowable();
    belongEscapeIsRecorded();
    nonNullNarrowingSurvivesAsSlotFact();
}

TEST_MAIN(memory_qualifiers)
