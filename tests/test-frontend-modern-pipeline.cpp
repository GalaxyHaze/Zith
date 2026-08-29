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

const hir::HirFunction *findHirFunction(session::CompilationSession &session,
                                        std::string_view name) {
    const auto &hir = session.hirModule();
    for (size_t i = 0; i < hir.getFnCount(); ++i) {
        const auto &fn = hir.getFn(i);
        if (session.interner().lookup(fn.name) == name)
            return &fn;
    }
    return nullptr;
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

void test_c_header_import_lowers_macro_constants() {
    Workspace workspace;
    workspace.write("fixture.h", "#define ANSWER 42\n"
                                 "#define RATIO 1.5\n"
                                 "#define YES true\n"
                                 "#define LETTER 'A'\n"
                                 "int next(int value);\n");
    workspace.write("main.zith", "import \"fixture.h\"\n"
                                 "fn main(): i32 {\n"
                                 "    let answer: i32 = ANSWER;\n"
                                 "    let ratio: f64 = RATIO;\n"
                                 "    let yes: bool = YES;\n"
                                 "    let letter: char = LETTER;\n"
                                 "    (answer + next(1)) as i32\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zith").string());
    session.setBuffered(true);
    const bool ok = session.runTo(session::Stage::HirLowered);

#ifdef ZITH_ENABLE_C_INTEROP
    std::string message = "C header macro constants lower through the modern pipeline";
    if (!ok && session.snapshot() != nullptr && !session.snapshot()->diagnostics().empty())
        message += ": " + session.snapshot()->diagnostics().front().message;
    CHECK(ok, message.c_str());
    if (!ok)
        return;
    const auto *header = session.snapshot()->cHeaders().front().get();
    CHECK(header != nullptr, "fixture artifact is present");
    if (header == nullptr)
        return;
    CHECK_EQ(header->constants.size(), 4u, "fixture exposes four scalar constants");
    CHECK_EQ(header->functions.size(), 1u, "function import remains available with constants");
    CHECK(session.hirModule().getGlobalConstCount() >= 4u,
          "HIR contains one immutable global per imported constant");
    bool saw_global_load = false;
    for (size_t index = 0; index < session.hirModule().exprCount(); ++index)
        saw_global_load =
            saw_global_load || std::holds_alternative<hir::HirGlobalConstLoad>(
                                   session.hirModule().getExpr(static_cast<hir::HirExprId>(index)));
    CHECK(saw_global_load, "constant uses lower through HirGlobalConstLoad");
#else
    CHECK(!ok, "C header import fails without libclang support");
    CHECK(session.snapshot() != nullptr && !session.snapshot()->diagnostics().empty(),
          "libclang-disabled C header import keeps the existing diagnostic");
#endif
}

void test_c_header_constant_duplicate_diagnostic() {
    Workspace workspace;
    workspace.write("fixture.h", "#define ANSWER 42\n");
    workspace.write("main.zith", "import \"fixture.h\"\n"
                                 "fn ANSWER(): i32 { 1 }\n"
                                 "fn main(): i32 { ANSWER() }\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zith").string());
    session.setBuffered(true);
    (void)session.runTo(session::Stage::HirLowered);
#ifdef ZITH_ENABLE_C_INTEROP
    CHECK(!session.runTo(session::Stage::HirLowered),
          "local fn colliding with imported C constant reports an error");
    bool found_duplicate = false;
    if (session.snapshot()) {
        for (const auto &diagnostic : session.snapshot()->diagnostics())
            found_duplicate =
                found_duplicate || (diagnostic.code == diagnostics::err::DuplicateDecl &&
                                    diagnostic.message.find("ANSWER") != std::string::npos);
    }
    CHECK(found_duplicate, "duplicate says DuplicateDecl and mentions ANSWER");
#else
    CHECK(session.snapshot() != nullptr && !session.snapshot()->diagnostics().empty(),
          "without libclang the existing import diagnostic is still produced");
#endif
}

void test_c_header_constant_let_duplicate_diagnostic() {
    Workspace workspace;
    workspace.write("fixture.h", "#define JOINED 1\n");
    workspace.write("main.zith", "import \"fixture.h\"\n"
                                 "const JOINED: i32 = 2;\n"
                                 "fn main(): i32 { JOINED }\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zith").string());
    session.setBuffered(true);
    (void)session.runTo(session::Stage::HirLowered);
#ifdef ZITH_ENABLE_C_INTEROP
    CHECK(!session.runTo(session::Stage::HirLowered),
          "local const colliding with imported C constant reports an error");
    bool found_duplicate = false;
    if (session.snapshot()) {
        for (const auto &diagnostic : session.snapshot()->diagnostics())
            found_duplicate =
                found_duplicate || (diagnostic.code == diagnostics::err::DuplicateDecl &&
                                    diagnostic.message.find("JOINED") != std::string::npos);
    }
    CHECK(found_duplicate, "duplicate says DuplicateDecl and mentions JOINED");
#else
    CHECK(session.snapshot() != nullptr && !session.snapshot()->diagnostics().empty(),
          "without libclang the existing import diagnostic is still produced");
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
    workspace.write("main.zith", "const base: i32 = 100;\n"
                                 "const inner: i32 = 9;\n"
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
    workspace.write("main.zith", "const base: i32 = 10;\n"
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

void test_pipeline_tag_macro_is_rejected() {
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
    CHECK(!session.runTo(session::Stage::HirLowered),
          "tag macros are rejected in the Zith-- pipeline");
    CHECK(session.snapshot() != nullptr && !session.snapshot()->diagnostics().empty(),
          "tag macro rejection surfaces a frontend diagnostic");
}

void test_pipeline_imported_state_machine_lowers() {
    Workspace workspace;
    workspace.write("helper.zith", "pub state target(v: i32): i32 {\n"
                                   "    if (v < 1) {\n"
                                   "        jump target(v + 1);\n"
                                   "    }\n"
                                   "    return v;\n"
                                   "}\n");
    workspace.write("main.zith", "from helper\n"
                                 "fn main(): i32 {\n"
                                 "    return dock target(0);\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zith").string());
    session.setBuffered(true);
    const bool lowered_ok = session.runTo(session::Stage::HirLowered);
    CHECK(lowered_ok, "imported state machine lowers through the modern pipeline");
    CHECK(session.snapshot() != nullptr && session.snapshot()->diagnostics().empty(),
          "imported state machine produces no diagnostics");
}

void test_modern_imported_static_method_hir() {
    Workspace workspace;
    workspace.write("lib.zith", "pub struct Box { pub value: i32 }\n"
                                "implement Box {\n"
                                "    fn make(v: i32): Box { Box { value: v } }\n"
                                "}\n");
    workspace.write("main.zith", "from lib\n"
                                 "fn main(): i32 {\n"
                                 "    let b = Box.make(42);\n"
                                 "    b.value\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zith").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "imported static method lowers through the modern pipeline");
    CHECK(session.hirModule().getFnCount() >= 2u, "HIR includes both the imported method and main");
}

void test_modern_imported_receiver_method_hir() {
    Workspace workspace;
    workspace.write("lib.zith", "pub struct Counter { pub value: i32 }\n"
                                "implement Counter {\n"
                                "    fn get(self: view Counter): i32 { self.value }\n"
                                "}\n");
    workspace.write("main.zith", "from lib\n"
                                 "fn main(): i32 {\n"
                                 "    let c = Counter { value: 42 };\n"
                                 "    c.get()\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zith").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "imported receiver method lowers through the modern pipeline");
    CHECK(session.hirModule().getFnCount() >= 2u,
          "HIR includes both the imported receiver method and main");
}

void test_modern_imported_external_symbol_method_hir() {
    Workspace workspace;
    workspace.write("video.zith",
                    "pub struct Window {}\n"
                    "implement Window {\n"
                    "    pub fn destroy(self: lend Window): void = extern SDL_DestroyWindow;\n"
                    "}\n");
    workspace.write("main.zith", "from video\n"
                                 "fn main(): i32 {\n"
                                 "    var w: Window = Window {};\n"
                                 "    w.destroy();\n"
                                 "    return 0;\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zith").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "imported external symbol method lowers to HIR");

    const auto &hir             = session.hirModule();
    const hir::HirFunction *sdl = nullptr;
    for (size_t i = 0; i < hir.getFnCount(); ++i) {
        const auto &fn = hir.getFn(i);
        if (session.interner().lookup(fn.name) == "SDL_DestroyWindow")
            sdl = &fn;
    }
    CHECK(sdl != nullptr, "imported external method keeps its C linkage name");
    if (sdl != nullptr) {
        CHECK(sdl->blocks.empty(), "imported external method is body-less");
        CHECK_EQ(sdl->params.size(), 1u,
                 "external method retains its receiver as the first ABI parameter");
    }
}

void test_modern_imported_method_struct_literal_body_hir() {
    Workspace workspace;
    workspace.write("lib.zith", "pub struct Box { pub value: i32 }\n"
                                "implement Box {\n"
                                "    fn make(v: i32): Box { Box { value: v } }\n"
                                "}\n");
    workspace.write("main.zith", "from lib\n"
                                 "fn main(): i32 {\n"
                                 "    let b = Box.make(42);\n"
                                 "    b.value\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zith").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "imported method body with a struct literal lowers through the modern pipeline");
    CHECK(session.snapshot() != nullptr && session.snapshot()->diagnostics().empty(),
          "imported method body with a struct literal produces no diagnostics");
}

void test_modern_optional_method_narrowing_hir() {
    Workspace workspace;
    workspace.write("lib.zith", "pub struct Box { pub value: i32 }\n"
                                "implement Box {\n"
                                "    fn get(self: view Box): i32 { self.value }\n"
                                "}\n");
    workspace.write("main.zith", "from lib\n"
                                 "fn main(): i32 {\n"
                                 "    var b: ?Box = null;\n"
                                 "    if (b is null) { return 1; }\n"
                                 "    return b.get();\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zith").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "optional method after 'is null' lowers through the modern pipeline");
    CHECK(session.hirModule().getFnCount() >= 2u, "HIR includes the imported Box method and main");

    const auto &hir  = session.hirModule();
    const auto *main = findHirFunction(session, "main");
    CHECK(main != nullptr, "main function is present after optional lowering");
    if (main == nullptr)
        return;
    bool saw_payload_receiver = false;
    for (size_t index = 0; index < hir.exprCount(); ++index) {
        const auto *call =
            std::get_if<hir::HirCall>(&hir.getExpr(static_cast<hir::HirExprId>(index)));
        if (call == nullptr)
            continue;
        CHECK_EQ(call->args.size(), 1u, "receiver methods pass exactly one self argument");
        if (call->args.empty())
            continue;
        const auto *unary = std::get_if<hir::HirUnary>(&hir.getExpr(call->args[0]));
        if (unary != nullptr && unary->op == hir::HirUnaryOp::Ref) {
            const auto *field = std::get_if<hir::HirField>(&hir.getExpr(unary->operand));
            saw_payload_receiver |= field != nullptr && field->index == 0U;
        }
    }
    CHECK(saw_payload_receiver,
          "optional receiver calls pass the address of optional payload field 0");
}

void test_modern_optional_method_not_is_null_narrowing_hir() {
    Workspace workspace;
    workspace.write("lib.zith", "pub struct Box { pub value: i32 }\n"
                                "implement Box {\n"
                                "    fn get(self: view Box): i32 { self.value }\n"
                                "}\n");
    workspace.write("main.zith", "from lib\n"
                                 "fn main(): i32 {\n"
                                 "    var b: ?Box = null;\n"
                                 "    if not (b is null) { return b.get(); }\n"
                                 "    return 1;\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zith").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "optional method after 'not (is null)' lowers through the modern pipeline");
    CHECK(session.hirModule().getFnCount() >= 2u, "HIR includes the imported Box method and main");

    const auto &hir  = session.hirModule();
    const auto *main = findHirFunction(session, "main");
    CHECK(main != nullptr, "main function is present after not-is-null optional lowering");
    if (main == nullptr)
        return;
    bool saw_payload_receiver = false;
    for (size_t index = 0; index < hir.exprCount(); ++index) {
        const auto *call =
            std::get_if<hir::HirCall>(&hir.getExpr(static_cast<hir::HirExprId>(index)));
        if (call == nullptr)
            continue;
        CHECK_EQ(call->args.size(), 1u, "receiver methods pass exactly one self argument");
        if (call->args.empty())
            continue;
        const auto *unary = std::get_if<hir::HirUnary>(&hir.getExpr(call->args[0]));
        if (unary != nullptr && unary->op == hir::HirUnaryOp::Ref) {
            const auto *field = std::get_if<hir::HirField>(&hir.getExpr(unary->operand));
            saw_payload_receiver |= field != nullptr && field->index == 0U;
        }
    }
    CHECK(saw_payload_receiver,
          "not-is-null receiver calls pass the address of optional payload field 0");
}

void test_modern_optional_pointer_external_method_hir() {
    Workspace workspace;
    workspace.write("video.zith",
                    "pub struct Window {}\n"
                    "implement Window {\n"
                    "    pub fn destroy(self: lend Window): void = extern SDL_DestroyWindow;\n"
                    "}\n");
    workspace.write("main.zith", "from video\n"
                                 "fn main(): i32 {\n"
                                 "    var window: ?*Window = null;\n"
                                 "    if (window is null) { return 1; }\n"
                                 "    window.destroy();\n"
                                 "    return 0;\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zith").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "optional pointer receiver external method lowers through the modern pipeline");

    const auto &hir             = session.hirModule();
    const hir::HirFunction *sdl = nullptr;
    for (size_t i = 0; i < hir.getFnCount(); ++i) {
        const auto &fn = hir.getFn(i);
        if (session.interner().lookup(fn.name) == "SDL_DestroyWindow")
            sdl = &fn;
    }
    CHECK(sdl != nullptr, "imported external method remains body-less for optional pointers");
    if (sdl == nullptr)
        return;
    CHECK(sdl->blocks.empty(), "external alias method has no HIR body");
    CHECK_EQ(sdl->params.size(), 1u,
             "external alias method passes the optional pointer value as its receiver");

    bool saw_pointer_self = false;
    for (size_t index = 0; index < hir.exprCount(); ++index) {
        const auto *call =
            std::get_if<hir::HirCall>(&hir.getExpr(static_cast<hir::HirExprId>(index)));
        if (call == nullptr || call->args.empty())
            continue;
        const auto *load = std::get_if<hir::HirSlotLoad>(&hir.getExpr(call->args[0]));
        // SlotLoad (not Ref of a payload HirField) is the shape used when the
        // receiver is an optional pointer value and the C pointer itself is passed.
        saw_pointer_self |= load != nullptr;
    }
    CHECK(saw_pointer_self,
          "optional pointer receiver lowering passes the pointer value, not a payload field");
}

void test_pipeline_imported_macro_lowers() {
    Workspace workspace;
    workspace.write("helper.zith", "pub macro twice(v: expr) { v + v }\n"
                                   "pub macro qualify(v: expr) { v }\n");
    workspace.write("main.zith", "from helper\n"
                                 "fn main(): i32 { @twice(21) }\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zith").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "imported public macros expand and lower through the modern pipeline");
    CHECK(session.snapshot() != nullptr && session.snapshot()->diagnostics().empty(),
          "imported macro expansion produces no diagnostics");
}

void test_imported_macro_unknown_without_import() {
    Workspace workspace;
    workspace.write("helper.zith", "pub macro twice(v: expr) { v + v }\n");
    workspace.write("main.zith", "fn main(): i32 { @twice(21) }\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::Imported;

    session::CompilationSession session(options, (workspace.root / "main.zith").string());
    session.setBuffered(true);
    (void)session.runTo(session::Stage::Imported);
    CHECK(session.snapshot() != nullptr, "macro-unknown snapshot still materializes");
    if (!session.snapshot())
        return;
    bool has_unknown = false;
    for (const auto &diagnostic : session.snapshot()->diagnostics()) {
        if (diagnostic.code == diagnostics::err::MacroUnknown)
            has_unknown = true;
    }
    CHECK(has_unknown, "unimported public macro produces MacroUnknown");
}

void test_imported_macro_selector_alias() {
    Workspace workspace;
    workspace.write("helper.zith", "pub macro twice(v: expr) { v + v }\n");
    workspace.write("main.zith", "from helper { twice as double }\n"
                                 "fn main(): i32 { @double(10) }\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zith").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "selective imported macro alias expands and lowers");
    CHECK(session.snapshot() != nullptr && session.snapshot()->diagnostics().empty(),
          "selective imported macro alias has no diagnostics");
}

void test_imported_macro_namespace_alias() {
    Workspace workspace;
    workspace.write("helper.zith", "pub macro twice(v: expr) { v + v }\n");
    workspace.write("main.zith", "import helper as h\n"
                                 "fn main(): i32 { @h.twice(10) }\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zith").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "imported macro under namespace alias expands and lowers");
    CHECK(session.snapshot() != nullptr && session.snapshot()->diagnostics().empty(),
          "imported namespace macro has no diagnostics");
}

void test_imported_macro_duplicate_diagnostic() {
    Workspace workspace;
    workspace.write("helper.zith", "pub macro twice(v: expr) { v + v }\n");
    workspace.write("main.zith", "from helper { twice }\n"
                                 "macro twice(v: expr) { v }\n"
                                 "fn main(): i32 { @twice(1) }\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::Imported;

    session::CompilationSession session(options, (workspace.root / "main.zith").string());
    session.setBuffered(true);
    (void)session.runTo(session::Stage::Imported);
    CHECK(session.snapshot() != nullptr, "duplicate-macro snapshot materializes");
    if (!session.snapshot())
        return;
    bool has_duplicate = false;
    for (const auto &diagnostic : session.snapshot()->diagnostics())
        if (diagnostic.code == diagnostics::err::MacroDuplicate)
            has_duplicate = true;
    CHECK(has_duplicate, "local macro colliding with import reports MacroDuplicate");
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
    test_c_header_import_lowers_macro_constants();
    test_c_header_constant_duplicate_diagnostic();
    test_c_header_constant_let_duplicate_diagnostic();
    test_pipeline_macro_expansion_lowers();
    test_pipeline_raw_macro_lowers();
    test_pipeline_normal_macro_hygiene_and_call_site_scope();
    test_pipeline_raw_macro_sees_call_site_then_module();
    test_pipeline_tag_macro_is_rejected();
    test_pipeline_imported_state_machine_lowers();
    test_modern_imported_static_method_hir();
    test_modern_imported_receiver_method_hir();
    test_modern_imported_external_symbol_method_hir();
    test_modern_imported_method_struct_literal_body_hir();
    test_modern_optional_method_narrowing_hir();
    test_modern_optional_method_not_is_null_narrowing_hir();
    test_modern_optional_pointer_external_method_hir();
    test_pipeline_imported_macro_lowers();
    test_imported_macro_unknown_without_import();
    test_imported_macro_selector_alias();
    test_imported_macro_namespace_alias();
    test_imported_macro_duplicate_diagnostic();
}

TEST_MAIN(frontend_modern_pipeline)
