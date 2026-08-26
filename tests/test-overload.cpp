#include "cli/options.hpp"
#include "diagnostics/diagnostic-engine.hpp"
#include "diagnostics/error-codes.hpp"
#include "hir/hir-expr.hpp"
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
    std::vector<std::string> hirFunctionNames;

    bool hasErrorCode(diagnostics::ErrCode code) const {
        for (auto c : codes) {
            if (c == static_cast<uint32_t>(code))
                return true;
        }
        return false;
    }
};

/// One temporary workspace per test, so `from dep` imports resolve on disk and
/// the module namespace of `main.zith` is the stem `main`.
struct Workspace {
    fs::path root = fs::temp_directory_path() / "zith-overload-tests";

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

Result runStage(const Workspace &workspace, session::Stage stage) {
    memory::Arena arena;
    Options options(arena);
    options.targetStage = stage;

    session::FrontendConfig config;
    config.workspaceRoot      = workspace.root.string();
    config.maxFrontendWorkers = 1;
    config.compilerVersion    = "test";
    auto context              = std::make_shared<session::FrontendContext>(config);

    session::CompilationSession session(options, (workspace.root / "main.zith").string(), context);
    session.setBuffered(true);

    Result result;
    result.ok = session.runTo(stage);
    for (const auto &diagnostic : session.diags().all()) {
        if (diagnostic.severity != diagnostics::Severity::Error)
            continue;
        ++result.errorCount;
        result.codes.push_back(diagnostic.code);
        std::printf("    [Diag] Code: %u, Message: %s\n", diagnostic.code,
                    diagnostic.message.c_str());
    }
    result.ok = result.ok && result.errorCount == 0;

    if (stage == session::Stage::HirLowered) {
        const auto &hir = session.hirModule();
        for (size_t i = 0; i < hir.getFnCount(); ++i)
            result.hirFunctionNames.emplace_back(session.interner().lookup(hir.getFn(i).name));
    }
    return result;
}

Result check(const Workspace &workspace) {
    return runStage(workspace, session::Stage::TypeChecked);
}

bool hasFunctionNamed(const Result &result, std::string_view name) {
    for (const auto &candidate : result.hirFunctionNames) {
        if (candidate == name)
            return true;
    }
    return false;
}

// ── Accepted overload sets ───────────────────────────────────

void overloadsDifferingByType() {
    Workspace workspace;
    workspace.write("main.zith", "fn add(a: i32, b: i32): i32 { a + b }\n"
                                 "fn add(a: f64, b: f64): f64 { a + b }\n"
                                 "fn main(): i32 {\n"
                                 "    let x: i32 = add(1, 2);\n"
                                 "    let y: f64 = add(1.0, 2.0);\n"
                                 "    return x;\n"
                                 "}\n");
    auto r = check(workspace);
    CHECK(r.ok, "two functions differing in parameter types coexist and resolve");
}

void overloadsDifferingByArity() {
    Workspace workspace;
    workspace.write("main.zith", "fn add(a: i32, b: i32): i32 { a + b }\n"
                                 "fn add(a: i32, b: i32, c: i32): i32 { a + b + c }\n"
                                 "fn main(): i32 {\n"
                                 "    let two: i32 = add(1, 2);\n"
                                 "    let three: i32 = add(1, 2, 3);\n"
                                 "    return two + three;\n"
                                 "}\n");
    auto r = check(workspace);
    CHECK(r.ok, "two functions differing in arity coexist and resolve");
}

void selectionPicksTheIntegerOverload() {
    Workspace workspace;
    workspace.write("main.zith", "fn pick(a: i32): i32 { a }\n"
                                 "fn pick(a: f64): f64 { a }\n"
                                 "fn main(): i32 {\n"
                                 "    let n: i32 = pick(1);\n"
                                 "    return n;\n"
                                 "}\n");
    auto r = check(workspace);
    CHECK(r.ok, "an integer argument selects the i32 overload (result types as i32)");
}

void selectionPicksTheFloatOverload() {
    Workspace workspace;
    workspace.write("main.zith", "fn pick(a: i32): i32 { a }\n"
                                 "fn pick(a: f64): f64 { a }\n"
                                 "fn main(): i32 {\n"
                                 "    let f: f64 = pick(1.0);\n"
                                 "    return 0;\n"
                                 "}\n");
    auto r = check(workspace);
    CHECK(r.ok, "a float argument selects the f64 overload (result types as f64)");
}

void selectionResultTypeIsEnforced() {
    Workspace workspace;
    workspace.write("main.zith", "fn pick(a: i32): i32 { a }\n"
                                 "fn pick(a: f64): f64 { a }\n"
                                 "fn main(): i32 {\n"
                                 "    let wrong: bool = pick(1);\n"
                                 "    return 0;\n"
                                 "}\n");
    auto r = check(workspace);
    CHECK(!r.ok, "the selected overload's return type is checked against the annotation");
    CHECK(r.hasErrorCode(diagnostics::err::TypeMismatch),
          "binding the i32 overload result to bool reports TypeMismatch");
}

void overloadedMethodsInOneImplementBlock() {
    Workspace workspace;
    workspace.write("main.zith", "struct P { x: i32, y: i32 }\n"
                                 "implement P {\n"
                                 "    fn get(self): i32 { return self->x; }\n"
                                 "    fn get(self, d: i32): i32 { return self->x + d; }\n"
                                 "}\n"
                                 "fn main(): i32 {\n"
                                 "    let p: P = P { x: 1, y: 2 };\n"
                                 "    let q: P = P { x: 1, y: 2 };\n"
                                 "    return p.get() + q.get(5);\n"
                                 "}\n");
    auto r = check(workspace);
    CHECK(r.ok, "two same-named methods in one implement block resolve by arity");
}

void overloadSetSurvivesImport() {
    Workspace workspace;
    workspace.write("dep.zith", "pub fn scaled(a: i32): i32 { a * 2 }\n"
                                "pub fn scaled(a: f64): f64 { a * 2.0 }\n");
    workspace.write("main.zith", "from dep\n"
                                 "fn main(): i32 {\n"
                                 "    let a: i32 = scaled(3);\n"
                                 "    let b: f64 = scaled(1.5);\n"
                                 "    return a;\n"
                                 "}\n");
    auto r = check(workspace);
    CHECK(r.ok, "an overload set survives a 'from dep' import");
}

void innerScopeShadowsOverloadSet() {
    Workspace workspace;
    workspace.write("main.zith", "fn dup(a: i32): i32 { a }\n"
                                 "fn dup(a: f64): f64 { a }\n"
                                 "fn main(): i32 {\n"
                                 "    let dup: i32 = 9;\n"
                                 "    return dup;\n"
                                 "}\n");
    auto r = check(workspace);
    CHECK(r.ok, "a local binding shadows the whole overload set without ambiguity");
}

// ── Rejected overload sets ───────────────────────────────────

void identicalSignaturesAreDuplicates() {
    Workspace workspace;
    workspace.write("main.zith", "fn add(a: i32, b: i32): i32 { a + b }\n"
                                 "fn add(a: i32, b: i32): i32 { a - b }\n"
                                 "fn main(): i32 { return add(1, 2); }\n");
    auto r = check(workspace);
    CHECK(!r.ok, "two functions with identical signatures are rejected");
    CHECK(r.hasErrorCode(diagnostics::err::DuplicateDecl),
          "identical signatures report DuplicateDecl (E2002)");
}

void qualifiersDoNotDiscriminateOverloads() {
    Workspace workspace;
    workspace.write("main.zith", "struct P { x: i32 }\n"
                                 "fn use_it(p: lend P): i32 { return p.x; }\n"
                                 "fn use_it(p: view P): i32 { return p.x; }\n"
                                 "fn main(): i32 { return 0; }\n");
    auto r = check(workspace);
    CHECK(!r.ok, "'lend P' vs 'view P' does not form an overload pair");
    CHECK(r.hasErrorCode(diagnostics::err::DuplicateDecl),
          "signatures differing only in qualifiers report DuplicateDecl (E2002)");
}

void externFunctionsCannotOverload() {
    Workspace workspace;
    workspace.write("main.zith", "extern fn puts(msg: *char)\n"
                                 "extern fn puts(msg: *i32)\n"
                                 "fn main(): i32 { return 0; }\n");
    auto r = check(workspace);
    CHECK(!r.ok, "two extern functions with the same name are rejected (fixed C linkage name)");
    CHECK(r.hasErrorCode(diagnostics::err::DuplicateDecl),
          "duplicate extern declarations report DuplicateDecl (E2002)");
}

void functionAndNonFunctionCollide() {
    Workspace workspace;
    workspace.write("main.zith", "fn v(): i32 { 1 }\n"
                                 "let v: i32 = 2;\n"
                                 "fn main(): i32 { return 0; }\n");
    auto r = check(workspace);
    CHECK(!r.ok, "a function and a global binding with the same name still collide");
    CHECK(r.hasErrorCode(diagnostics::err::DuplicateDecl),
          "fn versus let reports DuplicateDecl (E2002)");
}

void noViableCandidate() {
    Workspace workspace;
    workspace.write("main.zith", "fn f(a: i32): i32 { a }\n"
                                 "fn f(a: bool): i32 { 1 }\n"
                                 "fn main(): i32 { return f(1.5); }\n");
    auto r = check(workspace);
    CHECK(!r.ok, "a call with no viable candidate is rejected");
    CHECK(r.hasErrorCode(diagnostics::err::NoMatchingFn),
          "a call with no viable candidate reports NoMatchingFn (E2007)");
}

void ambiguousCall() {
    Workspace workspace;
    workspace.write("main.zith", "fn f(a: i32): i32 { a }\n"
                                 "fn f(a: i64): i32 { 1 }\n"
                                 "fn main(): i32 { return f(1); }\n");
    auto r = check(workspace);
    CHECK(!r.ok, "a call matching two candidates is rejected instead of silently ranked");
    CHECK(r.hasErrorCode(diagnostics::err::AmbiguousCall),
          "two viable candidates report AmbiguousCall (E2008)");
}

// ── Linkage names ────────────────────────────────────────────

void linkageNamesAreQualified() {
    Workspace workspace;
    workspace.write("main.zith", "extern fn puts(msg: *char)\n"
                                 "struct P { x: i32 }\n"
                                 "implement P {\n"
                                 "    fn get(self): i32 { return self->x; }\n"
                                 "}\n"
                                 "fn add(a: i32, b: i32): i32 { a + b }\n"
                                 "fn add(a: f64, b: f64): f64 { a + b }\n"
                                 "fn main(): i32 { return add(1, 2); }\n");

    auto r = runStage(workspace, session::Stage::HirLowered);
    CHECK(r.ok, "an overloaded module lowers to HIR");
    if (!r.ok)
        return;

    CHECK(hasFunctionNamed(r, "main.add(i32,i32)"), "the i32 overload gets a qualified HIR name");
    CHECK(hasFunctionNamed(r, "main.add(f64,f64)"),
          "the f64 overload gets a distinct qualified HIR name");
    CHECK(hasFunctionNamed(r, "main.P.get(*P)"), "a method's qualified HIR name carries its owner");
    CHECK(hasFunctionNamed(r, "main"), "main keeps its pure source name");
    CHECK(hasFunctionNamed(r, "puts"), "an extern function keeps its pure C linkage name");
}

} // namespace

void test_overload() {
    overloadsDifferingByType();
    overloadsDifferingByArity();
    selectionPicksTheIntegerOverload();
    selectionPicksTheFloatOverload();
    selectionResultTypeIsEnforced();
    overloadedMethodsInOneImplementBlock();
    overloadSetSurvivesImport();
    innerScopeShadowsOverloadSet();
    identicalSignaturesAreDuplicates();
    qualifiersDoNotDiscriminateOverloads();
    externFunctionsCannotOverload();
    functionAndNonFunctionCollide();
    noViableCandidate();
    ambiguousCall();
    linkageNamesAreQualified();
}

TEST_MAIN(overload)
