#include "cli/options.hpp"
#include "diagnostics/diagnostic-engine.hpp"
#include "diagnostics/error-codes.hpp"
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
    bool hirVariadic = false;

    bool hasErrorCode(diagnostics::ErrCode code) const {
        for (const auto c : codes) {
            if (c == static_cast<uint32_t>(code))
                return true;
        }
        return false;
    }
};

struct Workspace {
    fs::path root = fs::temp_directory_path() / "zith-variadic-tests";

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
        for (size_t i = 0; i < hir.getFnCount(); ++i) {
            const auto &fn = hir.getFn(i);
            if (session.interner().lookup(fn.name) == "printf")
                result.hirVariadic = fn.isVariadic;
        }
    }
    return result;
}

Result check(const Workspace &workspace) {
    return runStage(workspace, session::Stage::TypeChecked);
}

void externVariadicAcceptsTailArguments() {
    Workspace workspace;
    workspace.write("main.zith", "extern fn printf(fmt: *char, ...): i32\n"
                                 "fn main(): i32 {\n"
                                 "    printf(\"n=%d\\n\", 7, 1.5);\n"
                                 "    return 0;\n"
                                 "}\n");
    auto r = check(workspace);
    CHECK(r.ok, "extern variadic declarations accept any number of tail arguments");
}

void externVariadicRequiresFixedArguments() {
    Workspace workspace;
    workspace.write("main.zith", "extern fn printf(fmt: *char, ...): i32\n"
                                 "fn main(): i32 {\n"
                                 "    printf();\n"
                                 "    return 0;\n"
                                 "}\n");
    auto r = check(workspace);
    CHECK(!r.ok, "omitting a required fixed variadic argument is rejected");
    CHECK(r.hasErrorCode(diagnostics::err::NoMatchingFn),
          "too few variadic arguments report NoMatchingFn (E2007)");
}

void variadicRequiresExtern() {
    Workspace workspace;
    workspace.write("main.zith", "fn variadic(x: i32, ...): i32 { return x; }\n");
    auto r = check(workspace);
    CHECK(!r.ok, "a non-extern function cannot declare a variadic tail");
    CHECK(r.hasErrorCode(diagnostics::err::ExpectedExpr),
          "variadic on a plain fn reports a parse diagnostic");
}

void variadicMustBeLast() {
    Workspace workspace;
    workspace.write("main.zith", "extern fn bad(fmt: *char, ..., x: i32): i32\n");
    auto r = check(workspace);
    CHECK(!r.ok, "a variadic tail followed by another parameter is rejected");
}

void variadicLoweredFlag() {
    Workspace workspace;
    workspace.write("main.zith", "extern fn printf(fmt: *char, ...): i32\n"
                                 "fn main(): i32 { return 0; }\n");
    auto r = runStage(workspace, session::Stage::HirLowered);
    CHECK(r.ok, "extern variadic declaration lowers to HIR");
    CHECK(r.hirVariadic, "HIR carries isVariadic for printf");
}

} // namespace

void test_variadic() {
    externVariadicAcceptsTailArguments();
    externVariadicRequiresFixedArguments();
    variadicRequiresExtern();
    variadicMustBeLast();
    variadicLoweredFlag();
}

TEST_MAIN(variadic)
