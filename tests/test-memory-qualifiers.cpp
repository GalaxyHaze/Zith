#include "cli/options.hpp"
#include "diagnostics/diagnostic-engine.hpp"
#include "diagnostics/error-codes.hpp"
#include "session/compilation-session.hpp"
#include "session/frontend-context.hpp"
#include "session/pipeline-plan.hpp"
#include "test-common.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

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

// ── Accepted placements ──────────────────────────────────────

void lendAndViewAreAccepted() {
    auto r = check("struct P { x: i32 }\n"
                   "fn a(p: lend P): i32 { return p.x; }\n"
                   "fn b(p: view P): i32 { return p.x; }\n"
                   "fn main(): i32 { return 0; }\n");
    CHECK(r.ok, "'lend' and 'view' qualifiers are accepted on parameter types");

    auto returns = check("fn a(v: i32): view i32 { return v; }\n"
                         "fn b(v: i32): lend i32 { return v; }\n"
                         "fn main(): i32 { return 0; }\n");
    CHECK(returns.ok, "'lend' and 'view' return types remain accepted");

    auto fields = check("struct P { x: view i32, y: lend i32 }\n"
                        "fn main(): i32 { return 0; }\n");
    CHECK(fields.ok, "'lend' and 'view' struct fields remain accepted");

    auto locals = check("fn main(): i32 {\n"
                        "    let n: view i32 = 3;\n"
                        "    var m: lend i32 = 4;\n"
                        "    return n + m;\n"
                        "}\n");
    CHECK(locals.ok, "'lend'/'view' annotations on let/var bindings remain accepted");

    auto constructors = check("struct P { x: i32 }\n"
                              "fn a(o: ?lend P): i32 { return 0; }\n"
                              "fn b(s: []view i32): i32 { return 0; }\n"
                              "fn main(): i32 { return 0; }\n");
    CHECK(constructors.ok, "'lend'/'view' still compose with existing type constructors");
}

void removedQualifiersAreRejected() {
    for (const std::string_view source :
         {"fn f(p: unique P): i32 { return p.x; }\n", "fn f(p: share P): i32 { return p.x; }\n",
          "fn f(p: belong P): i32 { return p.x; }\n", "fn f(p: mut i32): i32 { return p; }\n",
          "fn main(): i32 { let u: unique i32 = 4; return u; }\n", "struct P { x: mut i32 }\n"}) {
        auto r = check(source);
        CHECK(
            !r.ok,
            (std::string("removed Zith-- qualifier is rejected: ") + std::string(source)).c_str());
        CHECK(r.hasErrorCode(diagnostics::err::UnsupportedSyntax),
              (std::string("removed Zith-- qualifier reports UnsupportedSyntax: ") +
               std::string(source))
                  .c_str());
    }
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
                   "    return bump(view q);\n"
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

// ── Residual lend/view behavior ─────────────────────────────

void lendKeepsWriteThroughResidualBehavior() {
    auto r = check("struct P { x: i32 }\n"
                   "fn bump(p: lend P): i32 {\n"
                   "    p.x = 5;\n"
                   "    return p.x;\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    var q: P = P { x: 1 };\n"
                   "    return bump(lend q);\n"
                   "}\n");
    CHECK(r.ok, "writing through a 'lend' binding remains allowed");
}

void callSiteLendAndViewAnnotationsAreEnforced() {
    auto missing_lend = check("struct P { x: i32 }\n"
                              "fn update(p: lend P): i32 { p.x }\n"
                              "fn main(): i32 {\n"
                              "    var q: P = P { x: 1 };\n"
                              "    return update(q);\n"
                              "}\n");
    CHECK(!missing_lend.ok, "a default binding needs an explicit 'lend' annotation");
    CHECK(missing_lend.hasErrorCode(diagnostics::err::OwnershipCoercionRequired),
          "the missing 'lend' annotation reports E4005");

    auto missing_view = check("struct P { x: i32 }\n"
                              "fn read(p: view P): i32 { p.x }\n"
                              "fn main(): i32 {\n"
                              "    var q: P = P { x: 1 };\n"
                              "    return read(q);\n"
                              "}\n");
    CHECK(!missing_view.ok, "a default binding needs an explicit 'view' annotation");
    CHECK(missing_view.hasErrorCode(diagnostics::err::OwnershipCoercionRequired),
          "the missing 'view' annotation reports E4005");

    auto mismatch_lend = check("struct P { x: i32 }\n"
                               "fn update(p: lend P): i32 { p.x }\n"
                               "fn main(): i32 {\n"
                               "    var q: P = P { x: 1 };\n"
                               "    return update(view q);\n"
                               "}\n");
    CHECK(!mismatch_lend.ok, "'view' is rejected for a 'lend' parameter");
    CHECK(mismatch_lend.hasErrorCode(diagnostics::err::OwnershipCoercionRequired),
          "the 'lend'/'view' mismatch reports E4005");

    auto mismatch_view = check("struct P { x: i32 }\n"
                               "fn read(p: view P): i32 { p.x }\n"
                               "fn main(): i32 {\n"
                               "    var q: P = P { x: 1 };\n"
                               "    return read(lend q);\n"
                               "}\n");
    CHECK(!mismatch_view.ok, "'lend' is rejected for a 'view' parameter");
    CHECK(mismatch_view.hasErrorCode(diagnostics::err::OwnershipCoercionRequired),
          "the 'view'/'lend' mismatch reports E4005");
}

void callSiteOwnershipExclusivityIsEnforced() {
    auto duplicate = check("struct P { x: i32 }\n"
                           "fn f(a: lend P, b: lend P): i32 { a.x }\n"
                           "fn main(): i32 {\n"
                           "    var q: P = P { x: 1 };\n"
                           "    return f(lend q, lend q);\n"
                           "}\n");
    CHECK(!duplicate.ok, "a binding cannot be lent twice in one call");
    CHECK(duplicate.hasErrorCode(diagnostics::err::OwnershipCoercionRequired),
          "duplicate lend reports E4005");

    auto mixed = check("struct P { x: i32 }\n"
                       "fn f(a: lend P, b: view P): i32 { a.x }\n"
                       "fn main(): i32 {\n"
                       "    var q: P = P { x: 1 };\n"
                       "    return f(lend q, view q);\n"
                       "}\n");
    CHECK(!mixed.ok, "the same binding cannot be lent and viewed in one call");
    CHECK(mixed.hasErrorCode(diagnostics::err::OwnershipCoercionRequired),
          "lend+view conflict reports E4005");

    auto distinct_paths = check("struct P { x: i32, y: i32 }\n"
                                "fn f(a: lend P, b: lend P): i32 { a.x + a.y }\n"
                                "fn main(): i32 {\n"
                                "    var q: P = P { x: 1, y: 2 };\n"
                                "    return f(lend q, lend q);\n"
                                "}\n");
    CHECK(!distinct_paths.ok, "duplicate whole-struct lend is rejected");
    CHECK(distinct_paths.hasErrorCode(diagnostics::err::OwnershipCoercionRequired),
          "duplicate whole-struct lend reports E4005");
}

void callSiteAcceptedAnnotationsAndTemporaries() {
    auto annotated = check("struct P { x: i32 }\n"
                           "fn update(p: lend P): i32 { p.x = 5; p.x }\n"
                           "fn read(p: view P): i32 { p.x }\n"
                           "fn main(): i32 {\n"
                           "    var q: P = P { x: 1 };\n"
                           "    update(lend q);\n"
                           "    read(view q);\n"
                           "    read(P { x: 3 });\n"
                           "    return read(view q) + update(lend q);\n"
                           "}\n");
    CHECK(annotated.ok, "accepts correct lend/view annotations and temporary rvalues");

    auto already_view = check("struct P { x: i32 }\n"
                              "fn read(p: view P): i32 { p.x }\n"
                              "fn main(): i32 {\n"
                              "    let v: view P = P { x: 1 };\n"
                              "    return read(v);\n"
                              "}\n");
    CHECK(already_view.ok, "a binding already annotated `view` passes without a new annotation");
}

void invalidCallOwnershipAnnotationsAreRejected() {
    auto unique_arg = check("struct P { x: i32 }\n"
                            "fn read(p: view P): i32 { p.x }\n"
                            "fn main(): i32 {\n"
                            "    var q: P = P { x: 1 };\n"
                            "    return read(unique q);\n"
                            "}\n");
    CHECK(!unique_arg.ok, "'unique' in a call argument is rejected");
    CHECK(unique_arg.hasErrorCode(diagnostics::err::InvalidCallOwnership),
          "invalid call ownership reports E4007");

    auto outside_call = check("struct P { x: i32 }\n"
                              "fn main(): i32 {\n"
                              "    var q: P = P { x: 1 };\n"
                              "    let r = lend q;\n"
                              "    return r.x;\n"
                              "}\n");
    CHECK(!outside_call.ok, "an ownership annotation outside a call is rejected");
    CHECK(!outside_call.hasErrorCode(diagnostics::err::InvalidCallOwnership),
          "outside-call annotation does not claim to be a call argument");
}

void nonNullNarrowingSurvivesAsSlotFact() {
    auto r = check("fn main(): i32 {\n"
                   "    var p: ?*i32 = null;\n"
                   "    if not (p is null) {\n"
                   "        return 7;\n"
                   "    }\n"
                   "    return 0;\n"
                   "}\n");
    CHECK(r.ok, "non-null narrowing source type-checks");
}

} // namespace

void test_memory_qualifiers() {
    lendAndViewAreAccepted();
    removedQualifiersAreRejected();
    writeThroughViewIsRejected();
    writeThroughViewLocalIsRejected();
    twoOwnershipQualifiersAreRejected();
    mutViewIsRejected();
    duplicateMutIsRejected();
    lendKeepsWriteThroughResidualBehavior();
    callSiteLendAndViewAnnotationsAreEnforced();
    callSiteOwnershipExclusivityIsEnforced();
    callSiteAcceptedAnnotationsAndTemporaries();
    invalidCallOwnershipAnnotationsAreRejected();
    nonNullNarrowingSurvivesAsSlotFact();
}

TEST_MAIN(memory_qualifiers)
