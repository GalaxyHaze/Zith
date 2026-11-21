// Guard that every shipped stdlib module type-checks on its own.
//
// The `std/` and `soon/` trees are documented as shipped surfaces, but no
// existing test compiled each module in isolation, so syntax and semantic
// regressions (missing `return expr;` terminators, `mod(N)` on a top-level
// function, nullable-to-non-nullable field writes) could sit undetected. This
// test walks `stdlib/**.zithm` and runs the real frontend/sema pipeline to the
// type-checked stage, failing on the first module that reports an error.

#include "diagnostics/error-codes.hpp"
#include "session/compilation-session.hpp"
#include "session/frontend-context.hpp"
#include "test-common.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace zith;

namespace {

#ifndef ZITH_STDLIB_DIR
int main() {
    std::printf("stdlib module tests skipped: ZITH_STDLIB_DIR is not defined\n");
    return 0;
}
#else

namespace fs = std::filesystem;

std::vector<fs::path> stdlibModules(const fs::path &root) {
    std::vector<fs::path> modules;
    for (const auto &entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file())
            continue;
        if (entry.path().extension() != ".zithm")
            continue;
        modules.push_back(entry.path());
    }
    std::sort(modules.begin(), modules.end());
    return modules;
}

struct CheckResult {
    bool ok = false;
    std::vector<std::string> errors;
};

CheckResult checkModule(const fs::path &module, const std::string &stdlibDir) {
    memory::Arena arena;
    Options opts(arena);
    opts.targetStage = session::Stage::TypeChecked;
    opts.includeDirs.push(stdlibDir);

    session::FrontendConfig config;
    config.workspaceRoot      = module.parent_path().string();
    config.maxFrontendWorkers = 1;
    config.compilerVersion    = "test";
    config.includeRoots.push_back(stdlibDir);
    auto context = std::make_shared<session::FrontendContext>(config);
    session::CompilationSession session(opts, module.string(), std::move(context));
    session.setBuffered(true);

    const bool ok = session.runTo(session::Stage::TypeChecked);

    CheckResult result;
    result.ok = ok && session.diags().errorCount() == 0;
    for (const auto &diag : session.diags().all()) {
        if (diag.severity != diagnostics::Severity::Error)
            continue;
        result.errors.push_back(diag.message);
        std::printf("    [StdlibModuleDiag] %s: %s\n", module.filename().string().c_str(),
                    diag.message.c_str());
    }
    return result;
}

void test_all_stdlib_modules_type_check() {
    const fs::path root{ZITH_STDLIB_DIR};
    const auto modules = stdlibModules(root);
    CHECK(!modules.empty(), "the stdlib tree contains .zithm modules");

    for (const auto &module : modules) {
        const auto relative = fs::relative(module, root).generic_string();
        const auto result   = checkModule(module, root.string());
        CHECK(result.ok, ("stdlib module type-checks: " + relative).c_str());
    }
}

#endif

} // namespace

#ifdef ZITH_STDLIB_DIR
int main() {
    std::printf("stdlib module tests\n");
    std::printf("===================\n\n");
    g_test_passed = 0;
    g_test_failed = 0;
    test_all_stdlib_modules_type_check();
    std::printf("\nResults: %d passed, %d failed\n", g_test_passed, g_test_failed);
    return g_test_failed > 0 ? 1 : 0;
}
#endif
