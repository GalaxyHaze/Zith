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

// ── Residual lend/view behavior ─────────────────────────────

void lendKeepsWriteThroughResidualBehavior() {
    auto r = check("struct P { x: i32 }\n"
                   "fn bump(p: lend P): i32 {\n"
                   "    p.x = 5;\n"
                   "    return p.x;\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    var q: P = P { x: 1 };\n"
                   "    return bump(q);\n"
                   "}\n");
    CHECK(r.ok, "writing through a 'lend' binding remains allowed");
}

void nonNullNarrowingSurvivesAsSlotFact() {
    auto r = check("fn main(): i32 {\n"
                   "    var p: ?*i32 = null;\n"
                   "    if (not (p is null)) {\n"
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
    nonNullNarrowingSurvivesAsSlotFact();
}

TEST_MAIN(memory_qualifiers)
