#include "cli/options.hpp"
#include "session/compilation-session.hpp"
#include "session/frontend-context.hpp"
#include "session/pipeline-plan.hpp"
#include "test-common.hpp"

#include <filesystem>
#include <fstream>
#include <memory>

using namespace zith;

namespace {

namespace fs = std::filesystem;

struct Workspace {
    fs::path root = fs::temp_directory_path() / "zith-modern-pipeline-tests";

    Workspace() {
        fs::remove_all(root);
        fs::create_directories(root);
    }

    ~Workspace() {
        fs::remove_all(root);
    }

    void write(const std::string &name, const std::string &text) const {
        const auto destination = root / name;
        fs::create_directories(destination.parent_path());
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        output << text;
    }
};

void test_default_session_uses_modern_pipeline() {
    Workspace workspace;
    workspace.write("main.zith", "extern fn puts(msg: *char)\n"
                                 "fn main() {\n"
                                 "    puts(\"hello\");\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zith").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "session lowers through the modern frontend path by default");
    CHECK(session.snapshot() != nullptr, "default session materializes a frontend snapshot");
    CHECK_EQ(session.hirModule().getFnCount(), 2u,
             "modern HIR includes both the extern declaration and main");
}

void test_shared_context_reuses_frontend_cache() {
    Workspace workspace;
    workspace.write("dep.zith", "pub fn dep_fn(): i32 { 7 }\n");
    workspace.write("main.zith", "from dep\n"
                                 "fn main(): i32 {\n"
                                 "    dep_fn()\n"
                                 "}\n");

    session::FrontendConfig config;
    config.workspaceRoot      = workspace.root.string();
    config.maxFrontendWorkers = 1;
    config.compilerVersion    = "test";
    auto context              = std::make_shared<session::FrontendContext>(config);

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession first(options, (workspace.root / "main.zith").string(), context);
    first.setBuffered(true);
    CHECK(first.runTo(session::Stage::HirLowered), "first shared-context build succeeds");
    const auto first_hits = context->metrics().cache.hits;

    session::CompilationSession second(options, (workspace.root / "main.zith").string(), context);
    second.setBuffered(true);
    CHECK(second.runTo(session::Stage::HirLowered), "second shared-context build succeeds");
    CHECK(context->metrics().cache.hits > first_hits,
          "repeated modern pipeline builds reuse frontend artifacts");
}

void test_pipeline_error_surfaces_diagnostic() {
    Workspace workspace;
    workspace.write("main.zith", "fn main(): bool {\n"
                                 "    42\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zith").string());
    session.setBuffered(true);
    CHECK(!session.runTo(session::Stage::HirLowered),
          "modern pipeline fails when the body type does not match the declared return type");
    CHECK(session.snapshot() != nullptr,
          "failed pipeline still materializes the frontend snapshot");
}

void test_pipeline_multifile_module_dependency() {
    Workspace workspace;
    workspace.write("math.zith", "pub fn add(a: i32, b: i32): i32 { a + b }\n");
    workspace.write("main.zith", "from math\n"
                                 "fn main(): i32 {\n"
                                 "    var x: i32 = add(3, 4);\n"
                                 "    x\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zith").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "modern pipeline lowers a multi-file module graph");
    CHECK(session.hirModule().getFnCount() >= 2u,
          "HIR module includes functions from both the root and its dependency");
}

void test_pipeline_while_loop_lowers() {
    Workspace workspace;
    workspace.write("main.zith", "fn main(): i32 {\n"
                                 "    var i: i32 = 0;\n"
                                 "    while (i < 10) {\n"
                                 "        i = i + 1;\n"
                                 "    }\n"
                                 "    i\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zith").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "modern pipeline lowers a while loop without errors");
}

void test_pipeline_if_else_lowers() {
    Workspace workspace;
    workspace.write("main.zith", "fn pick(n: i32): i32 {\n"
                                 "    if (n > 0) {\n"
                                 "        1\n"
                                 "    } else {\n"
                                 "        0\n"
                                 "    }\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zith").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "modern pipeline lowers if/else with no errors");
}

void test_c_header_import_lowers_external_function() {
    Workspace workspace;
    workspace.write("fixture.h", "int c_add(int left, int right);\n");
    workspace.write("main.zith", "import \"fixture.h\"\n"
                                 "fn main(): i32 { c_add(20, 22) }\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zith").string());
    session.setBuffered(true);
    const bool ok = session.runTo(session::Stage::HirLowered);

#ifdef ZITH_ENABLE_C_INTEROP
    std::string message = "C header import lowers through the modern pipeline";
    if (!ok && session.snapshot() != nullptr && !session.snapshot()->diagnostics().empty())
        message += ": " + session.snapshot()->diagnostics().front().message;
    CHECK(ok, message.c_str());
    if (!ok)
        return;

    CHECK_EQ(session.snapshot()->cHeaders().size(), 1u,
             "C header import stores one immutable binding artifact");
    CHECK_EQ(session.snapshot()->cHeaders().front()->functions.size(), 1u,
             "C header artifact exposes the declared external function");
    CHECK_EQ(session.hirModule().getFnCount(), 2u,
             "HIR contains the imported external function and main");
#else
    CHECK(!ok, "C header import fails without libclang support");
    CHECK(session.snapshot() != nullptr && !session.snapshot()->diagnostics().empty(),
          "libclang-disabled C header import produces an actionable diagnostic");
#endif
}

void test_pipeline_macro_expansion_lowers() {
    Workspace workspace;
    workspace.write("main.zith", "macro twice(v: expr) { v + v }\n"
                                 "macro mk(v: expr) { let inner = v; inner }\n"
                                 "fn add_one(n: i32): i32 { n + 1 }\n"
                                 "macro call1(x: expr) { add_one(x) }\n"
                                 "fn main(): i32 {\n"
                                 "    let x = @twice(21);\n"
                                 "    let inner = 5;\n"
                                 "    let y = @mk(inner);\n"
                                 "    let z = @call1(@mk(4));\n"
                                 "    x + y + z\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zith").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "macro expansion survives sema and lowers to HIR");
    CHECK(session.hirModule().getFnCount() >= 2u,
          "HIR contains user functions plus generated code paths");
    CHECK(session.snapshot() != nullptr && session.snapshot()->diagnostics().empty(),
          "expanded macro bodies produce no semantic diagnostics");
}

void test_pipeline_raw_macro_lowers() {
    Workspace workspace;
    workspace.write("main.zith", "raw macro swap(a: identifier, b: identifier) {\n"
                                 "    let tmp = a;\n"
                                 "    a = b;\n"
                                 "    b = tmp;\n"
                                 "}\n"
                                 "fn main(): i32 {\n"
                                 "    var first = 3;\n"
                                 "    var second = 7;\n"
                                 "    @swap(first, second);\n"
                                 "    first - second\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zith").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "raw macro statements splice and lower through the modern pipeline");
    CHECK(session.snapshot() != nullptr && session.snapshot()->diagnostics().empty(),
          "raw macro expansion produces no semantic diagnostics");
}

void test_pipeline_normal_macro_hygiene_and_call_site_scope() {
    Workspace workspace;
    workspace.write("main.zith", "global base: i32 = 100;\n"
                                 "global inner: i32 = 9;\n"
                                 "macro norm() { base }\n"
                                 "macro inject() { let inner = 99; inner }\n"
                                 "fn main(): i32 {\n"
                                 "    let base: i32 = 7;\n"
                                 "    let first = @norm();\n"
                                 "    let second = @inject();\n"
                                 "    first + second + inner\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zith").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "normal macro resolution prefers call-site scope and keeps bindings hygienic");
    CHECK(session.snapshot() != nullptr && session.snapshot()->diagnostics().empty(),
          "normal macro hygiene/scope case produces no semantic diagnostics");
}

void test_pipeline_raw_macro_sees_call_site_then_module() {
    Workspace workspace;
    workspace.write("main.zith", "global base: i32 = 10;\n"
                                 "raw macro pick_raw() { target = base }\n"
                                 "raw macro inject() { let inner = 99; inner }\n"
                                 "fn main(): i32 {\n"
                                 "    let base: i32 = 7;\n"
                                 "    var target: i32 = 0;\n"
                                 "    @pick_raw();\n"
                                 "    @inject();\n"
                                 "    target\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zith").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "raw macro body resolves from its splice scope and can see the module fallback");
    CHECK(session.snapshot() != nullptr && session.snapshot()->diagnostics().empty(),
          "raw macro call-site/global scope case produces no semantic diagnostics");
}

void test_pipeline_tag_macro_expands() {
    Workspace workspace;
    workspace.write("main.zith", "tag macro RunTwice(attributes, body: body) {\n"
                                 "    let title = attributes.title;\n"
                                 "    body;\n"
                                 "    body;\n"
                                 "}\n"
                                 "fn main(): i32 {\n"
                                 "    var total = 0;\n"
                                 "    <RunTwice title: 1>\n"
                                 "        total = total + 1;\n"
                                 "    </RunTwice>\n"
                                 "    total\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zith").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "tag macro body and attributes lower through the modern pipeline");
    CHECK(session.snapshot() != nullptr && session.snapshot()->diagnostics().empty(),
          "tag macro expansion produces no semantic diagnostics");
}

} // namespace

static void test_frontend_modern_pipeline() {
    test_default_session_uses_modern_pipeline();
    test_shared_context_reuses_frontend_cache();
    test_pipeline_error_surfaces_diagnostic();
    test_pipeline_multifile_module_dependency();
    test_pipeline_while_loop_lowers();
    test_pipeline_if_else_lowers();
    test_c_header_import_lowers_external_function();
    test_pipeline_macro_expansion_lowers();
    test_pipeline_raw_macro_lowers();
    test_pipeline_normal_macro_hygiene_and_call_site_scope();
    test_pipeline_raw_macro_sees_call_site_then_module();
    test_pipeline_tag_macro_expands();
}

TEST_MAIN(frontend_modern_pipeline)
