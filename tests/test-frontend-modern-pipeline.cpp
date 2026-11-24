#include "cli/options.hpp"
#include "hir/hir-expr.hpp"
#include "session/compilation-session.hpp"
#include "session/frontend-context.hpp"
#include "session/pipeline-plan.hpp"
#include "test-common.hpp"
#include "types/type-kind.hpp"

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

struct PipelineDiagnostic {
    uint32_t code = 0;
    diagnostics::Severity severity = diagnostics::Severity::Error;
    std::string message;
    uint32_t start = 0;
    uint32_t end = 0;
};

struct PipelineResult {
    bool ok = false;
    std::vector<PipelineDiagnostic> diagnostics;
};

PipelineResult checkSource(const Workspace &workspace, const std::string &file,
                           const std::string &source) {
    workspace.write(file, source);

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::TypeChecked;

    session::CompilationSession session(options, (workspace.root / file).string());
    session.setBuffered(true);

    PipelineResult result;
    result.ok = session.runTo(session::Stage::TypeChecked);
    for (const auto &diagnostic : session.diags().all()) {
        result.diagnostics.push_back({diagnostic.code, diagnostic.severity, diagnostic.message,
                                      diagnostic.primary.start, diagnostic.primary.end});
    }
    return result;
}

bool hasDiagnostic(const PipelineResult &result, uint32_t code,
                   diagnostics::Severity severity = diagnostics::Severity::Error) {
    for (const auto &diagnostic : result.diagnostics)
        if (diagnostic.code == code && diagnostic.severity == severity)
            return true;
    return false;
}

bool hasRawFunctionValueDiagnostic(const PipelineResult &result) {
    for (const auto &diagnostic : result.diagnostics)
        if (diagnostic.message.find("raw function cannot be used as a function value") !=
            std::string::npos)
            return true;
    return false;
}

void test_default_session_uses_modern_pipeline() {
    Workspace workspace;
    workspace.write("main.zithm", "extern fn puts(msg: *char)\n"
                                 "fn main() {\n"
                                 "    puts(\"hello\");\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "session lowers through the modern frontend path by default");
    CHECK(session.snapshot() != nullptr, "default session materializes a frontend snapshot");
    CHECK_EQ(session.hirModule().getFnCount(), 2u,
             "modern HIR includes both the extern declaration and main");
}

void test_shared_context_reuses_frontend_cache() {
    Workspace workspace;
    workspace.write("dep.zithm", "pub fn dep_fn(): i32 { 7 }\n");
    workspace.write("main.zithm", "from dep\n"
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

    session::CompilationSession first(options, (workspace.root / "main.zithm").string(), context);
    first.setBuffered(true);
    CHECK(first.runTo(session::Stage::HirLowered), "first shared-context build succeeds");
    const auto first_hits = context->metrics().cache.hits;

    session::CompilationSession second(options, (workspace.root / "main.zithm").string(), context);
    second.setBuffered(true);
    CHECK(second.runTo(session::Stage::HirLowered), "second shared-context build succeeds");
    CHECK(context->metrics().cache.hits > first_hits,
          "repeated modern pipeline builds reuse frontend artifacts");
}

void test_pipeline_error_surfaces_diagnostic() {
    Workspace workspace;
    workspace.write("main.zithm", "fn main(): bool {\n"
                                 "    42\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    CHECK(!session.runTo(session::Stage::HirLowered),
          "modern pipeline fails when the body type does not match the declared return type");
    CHECK(session.snapshot() != nullptr,
          "failed pipeline still materializes the frontend snapshot");
}

void test_inferred_return_type_is_available_to_earlier_callers() {
    Workspace workspace;
    workspace.write("main.zithm", "fn main(): i32 {\n"
                                 "    add(20, 22)\n"
                                 "}\n"
                                 "fn add(a: i32, b: i32) {\n"
                                 "    a + b\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "caller can use the inferred return type of a later declaration");
}

void test_pipeline_discard_non_void_call_result() {
    Workspace workspace;
    workspace.write("bad.zithm", "fn make(): i32 { return 1; }\n"
                                "fn main(): i32 {\n"
                                "    make();\n"
                                "    return 0;\n"
                                "}\n");
    workspace.write("good.zithm", "fn make(): i32 { return 1; }\n"
                                 "fn main(): i32 {\n"
                                 "    _ = make();\n"
                                 "    _ = make();\n"
                                 "    return 0;\n"
                                 "}\n");
    workspace.write("assigned.zithm", "fn make(): i32 { return 1; }\n"
                                     "fn main(): i32 {\n"
                                     "    let value: i32 = make();\n"
                                     "    return value;\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession bad(options, (workspace.root / "bad.zithm").string());
    bad.setBuffered(true);
    CHECK(!bad.runTo(session::Stage::HirLowered),
          "non-void call expression statement fails sema");

    bool saw_discarded_result = false;
    for (const auto &diagnostic : bad.diags().all()) {
        if (diagnostic.code == diagnostics::err::DiscardedResult)
            saw_discarded_result = true;
    }
    CHECK(saw_discarded_result,
          "plain non-void call statement reports DiscardedResult");

    session::CompilationSession good(options, (workspace.root / "good.zithm").string());
    good.setBuffered(true);
    CHECK(good.runTo(session::Stage::HirLowered),
          "explicit `_ = call();` discard lowers through the pipeline");

    session::CompilationSession assigned(options, (workspace.root / "assigned.zithm").string());
    assigned.setBuffered(true);
    CHECK(assigned.runTo(session::Stage::HirLowered),
          "call result used in a binding initializer remains legal");
}

void test_pipeline_discardable_attribute_allows_call() {
    Workspace workspace;
    workspace.write("main.zithm", "#[discardable] fn make(): i32 { return 1; }\n"
                                 "fn main(): i32 {\n"
                                 "    make();\n"
                                 "    return 0;\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "discardable call expression statement lowers through the pipeline");
    CHECK(session.hirModule().attrs().fnCount() >= 1,
          "HIR carries function attribute side tables");
}

void test_pipeline_unknown_attribute_warns_and_targets_are_checked() {
    Workspace workspace;
    workspace.write("main.zithm", "#[unknown] fn f(): i32 { return 1; }\n"
                                 "#[discardable] struct Bad { value: i32 }\n"
                                 "fn main(): i32 { return f(); }\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "unknown and misplaced attributes are warnings, not hard failures");
    CHECK(session.snapshot() != nullptr,
          "warning diagnostics still materialize the frontend snapshot");
    bool saw_unknown = false;
    bool saw_misplaced = false;
    for (const auto &diagnostic : session.diags().all()) {
        if (diagnostic.code == diagnostics::err::UnknownAttribute)
            saw_unknown = true;
        if (diagnostic.code == diagnostics::err::AttributeNotApplicable)
            saw_misplaced = true;
    }
    CHECK(saw_unknown, "unknown attribute reports UnknownAttribute");
    CHECK(saw_misplaced, "misplaced known attribute reports AttributeNotApplicable");
}

void test_imported_console_void_main_discardable() {
    Workspace workspace;
    workspace.write("main.zithm", "from std/io/console\n"
                                 "from soon/memory\n"
                                 "fn main() {\n"
                                 "    let heap = HeapAllocator {};\n"
                                 "    println(heap, \"Hello World!\");\n"
                                 "}\n");

    session::FrontendConfig config;
    config.workspaceRoot      = workspace.root.string();
    config.maxFrontendWorkers = 1;
    config.compilerVersion    = "test";
#ifdef ZITH_STDLIB_DIR
    config.includeRoots.push_back(ZITH_STDLIB_DIR);
#endif
    auto context = std::make_shared<session::FrontendContext>(config);

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string(),
                                        context);
    session.setBuffered(true);
    const bool lowered = session.runTo(session::Stage::HirLowered);
    for (const auto &diagnostic : session.diags().all()) {
        if (diagnostic.severity != diagnostics::Severity::Error)
            continue;
        std::string module_name = "<unknown>";
        if (session.snapshot()) {
            for (const auto &module : session.snapshot()->modules()) {
                if (module && module->fileId == diagnostic.primary.file) {
                    module_name = module->key;
                    break;
                }
            }
        }
        const std::string message =
            module_name + ":" + std::to_string(diagnostic.primary.start) + ": " +
            diagnostic.message;
        CHECK(false, message.c_str());
    }
    CHECK(lowered,
          "discardable imported println used as the last statement of a void main lowers");
}

void test_untyped_uninitialized_binding_is_rejected() {
    Workspace workspace;
    workspace.write("bad.zithm", "fn main() {\n"
                                "    let error;\n"
                                "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "bad.zithm").string());
    session.setBuffered(true);
    CHECK(!session.runTo(session::Stage::HirLowered),
          "untyped uninitialized binding fails before HIR lowering");

    bool saw_cannot_infer = false;
    for (const auto &diagnostic : session.diags().all()) {
        if (diagnostic.code == diagnostics::err::CannotInfer)
            saw_cannot_infer = true;
    }
    CHECK(saw_cannot_infer, "untyped uninitialized binding reports CannotInfer");
}

void test_typed_uninitialized_binding_with_later_assignment_lowers() {
    Workspace workspace;
    workspace.write("main.zithm", "struct Foo {\n"
                                 "    x: i32\n"
                                 "}\n"
                                 "fn main() {\n"
                                 "    let value: Foo;\n"
                                 "    value = Foo { x: 1 };\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "typed binding without an initializer lowers when assigned before use");
}

void test_typed_scalar_binding_first_assignment_with_explicit_return_lowers() {
    // Exact shape that regressed in CI: the enclosing function declares its
    // return type, so it skips return-type inference. The first assignment to a
    // typed `let` must stay legal after the provisional probe pass.
    Workspace workspace;
    workspace.write("main.zithm", "fn main(): i32 {\n"
                                 "    let q: i32;\n"
                                 "    q = 42;\n"
                                 "    q\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "first assignment to a typed let is legal in a function with an explicit return type");
}

void test_typed_scalar_binding_second_assignment_is_rejected() {
    Workspace workspace;
    workspace.write("bad.zithm", "fn main(): i32 {\n"
                                "    let q: i32;\n"
                                "    q = 42;\n"
                                "    q = 43;\n"
                                "    q\n"
                                "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "bad.zithm").string());
    session.setBuffered(true);
    CHECK(!session.runTo(session::Stage::HirLowered),
          "second assignment to an immutable let is rejected");

    bool saw_immutable_write = false;
    for (const auto &diagnostic : session.diags().all()) {
        if (diagnostic.message.find("cannot assign to an immutable let/const binding") !=
            std::string::npos) {
            saw_immutable_write = true;
        }
    }
    CHECK(saw_immutable_write,
          "second assignment reports the immutable-binding diagnostic");
}

void test_typed_uninitialized_unused_binding_lowers() {
    Workspace workspace;
    workspace.write("main.zithm", "struct Foo {\n"
                                 "    x: i32\n"
                                 "}\n"
                                 "fn main() {\n"
                                 "    let value: Foo;\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "typed binding without an initializer is legal when it is never read");
}

void test_typed_uninitialized_binding_use_before_assignment_fails() {
    Workspace workspace;
    workspace.write("bad.zithm", "struct Foo {\n"
                                "    x: i32\n"
                                "}\n"
                                "fn main() {\n"
                                "    let value: Foo;\n"
                                "    _ = value;\n"
                                "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "bad.zithm").string());
    session.setBuffered(true);
    CHECK(!session.runTo(session::Stage::HirLowered),
          "typed binding read before assignment fails sema");

    bool saw_use_before_initialized = false;
    for (const auto &diagnostic : session.diags().all()) {
        if (diagnostic.message.find("used before it is initialized") != std::string::npos)
            saw_use_before_initialized = true;
    }
    CHECK(saw_use_before_initialized,
          "typed binding read before assignment reports the initialization error");
}

void test_inferred_return_type_from_value_tail() {
    Workspace workspace;
    workspace.write("main.zithm", "fn foo() {\n"
                                 "    5\n"
                                 "}\n"
                                 "fn main(): i32 {\n"
                                 "    foo()\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "value tail infers a non-void function return type");
}

void test_inferred_return_type_ignores_terminated_assignment_tail() {
    Workspace workspace;
    workspace.write("main.zithm", "fn clear(var value: i32) {\n"
                                 "    value = 0;\n"
                                 "}\n"
                                 "fn main(): i32 {\n"
                                 "    clear(1);\n"
                                 "    return 0;\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::TypeChecked;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::TypeChecked),
          "a semicolon-terminated assignment tail infers void");
}

void test_inferred_return_type_from_explicit_returns() {
    Workspace workspace;
    workspace.write("main.zithm", "fn choose(flag: bool) {\n"
                                 "    if (flag) { return 17; }\n"
                                 "    return 23;\n"
                                 "}\n"
                                 "fn main(): i32 {\n"
                                 "    choose(true)\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "return statements infer a type used by callers");
}

void test_inferred_return_type_rejects_direct_recursion() {
    Workspace workspace;
    workspace.write("bad.zithm", "fn recurse(n: i32) {\n"
                                "    if (n == 0) { return 0; }\n"
                                "    recurse(n - 1)\n"
                                "}\n"
                                "fn main(): i32 { recurse(1) }\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "bad.zithm").string());
    session.setBuffered(true);
    CHECK(!session.runTo(session::Stage::HirLowered),
          "direct recursion requires an explicit return type");

    bool saw_recursive_inference_error = false;
    for (const auto &diagnostic : session.diags().all()) {
        if (diagnostic.code == diagnostics::err::CannotInfer &&
            diagnostic.message.find("recursive call cycle") != std::string::npos)
            saw_recursive_inference_error = true;
    }
    CHECK(saw_recursive_inference_error,
          "direct recursive inference reports CannotInfer at the function declaration");
}

void test_inferred_return_type_rejects_method_recursion() {
    Workspace workspace;
    workspace.write("bad.zithm", "struct Counter { value: i32 }\n"
                                "implement Counter {\n"
                                "    fn recurse(self) { self.recurse() }\n"
                                "}\n"
                                "fn main(): i32 {\n"
                                "    let counter = Counter{ value: 0 };\n"
                                "    counter.recurse();\n"
                                "    return 0;\n"
                                "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "bad.zithm").string());
    session.setBuffered(true);
    CHECK(!session.runTo(session::Stage::HirLowered),
          "method recursion requires an explicit return type");

    bool saw_recursive_inference_error = false;
    for (const auto &diagnostic : session.diags().all()) {
        if (diagnostic.code == diagnostics::err::CannotInfer &&
            diagnostic.message.find("recursive call cycle") != std::string::npos)
            saw_recursive_inference_error = true;
    }
    CHECK(saw_recursive_inference_error,
          "recursive method inference reports CannotInfer at the declaration");
}

void test_inferred_return_type_rejects_indirect_recursion() {
    Workspace workspace;
    workspace.write("bad.zithm", "fn first(n: i32) {\n"
                                "    if (n == 0) { return 0; }\n"
                                "    second(n - 1)\n"
                                "}\n"
                                "fn second(n: i32) { first(n) }\n"
                                "fn main(): i32 { first(1) }\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "bad.zithm").string());
    session.setBuffered(true);
    CHECK(!session.runTo(session::Stage::HirLowered),
          "indirect recursion requires explicit return types");

    size_t recursive_inference_errors = 0;
    for (const auto &diagnostic : session.diags().all()) {
        if (diagnostic.code == diagnostics::err::CannotInfer &&
            diagnostic.message.find("recursive call cycle") != std::string::npos)
            ++recursive_inference_errors;
    }
    CHECK_EQ(recursive_inference_errors, 2u,
             "every inferred function in an indirect cycle reports CannotInfer");
}

void test_inferred_return_type_rejects_incompatible_returns() {
    Workspace workspace;
    workspace.write("bad.zithm", "fn choose(flag: bool) {\n"
                                "    if (flag) { return 1; }\n"
                                "    return true;\n"
                                "}\n"
                                "fn main(): i32 { choose(true) }\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "bad.zithm").string());
    session.setBuffered(true);
    CHECK(!session.runTo(session::Stage::HirLowered),
          "incompatible explicit returns fail inferred return analysis");

    bool saw_incompatible_returns = false;
    for (const auto &diagnostic : session.diags().all()) {
        if (diagnostic.code == diagnostics::err::TypeMismatch &&
            diagnostic.message.find("return expressions do not agree") != std::string::npos)
            saw_incompatible_returns = true;
    }
    CHECK(saw_incompatible_returns,
          "incompatible inferred returns report a type mismatch after provisional analysis");
}

void test_pipeline_multifile_module_dependency() {
    Workspace workspace;
    workspace.write("math.zithm", "pub fn add(a: i32, b: i32): i32 { a + b }\n");
    workspace.write("main.zithm", "from math\n"
                                 "fn main(): i32 {\n"
                                 "    var x: i32 = add(3, 4);\n"
                                 "    x\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "modern pipeline lowers a multi-file module graph");
    CHECK(session.hirModule().getFnCount() >= 2u,
          "HIR module includes functions from both the root and its dependency");
}

void test_pipeline_export_platform_import() {
    Workspace workspace;
    workspace.write("entry.zithm", "export platform\n");
    workspace.write("main.zithm", "from entry\n"
                                 "fn main(): i32 { platform.platform_value() }\n");
    workspace.write("platform.x86_64.linux.zithm", "pub fn platform_value(): i32 { 7 }\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage  = session::Stage::HirLowered;
    options.targetTriple = "x86_64-unknown-linux-gnu";

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "modern pipeline re-exports the target-specific platform module");
    if (!session.snapshot())
        return;

    bool saw_platform_variant = false;
    for (const auto &edge : session.snapshot()->importGraph()) {
        if (edge.request.isExport) {
            for (const auto &target : edge.targets)
                saw_platform_variant |=
                    target == session::SourceCatalog::canonicalPath(
                                  (workspace.root / "platform.x86_64.linux.zithm").string());
        }
    }
    CHECK(saw_platform_variant, "export edge points at the resolved platform-specific module");
}

void test_pipeline_export_shared_prefix() {
    Workspace workspace;
    workspace.write("lib/one.zithm", "pub fn one(): i32 { return 1; }\n");
    workspace.write("lib/two.zithm", "pub fn two(): i32 { return 2; }\n");
    workspace.write("facade.zithm", "export lib/one\n"
                                   "export lib/two\n");
    workspace.write("main.zithm", "from facade\n"
                                 "fn main(): i32 { one() + two() }\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "facade exports both modules that share a namespace prefix");
}

void test_pipeline_while_loop_lowers() {
    Workspace workspace;
    workspace.write("main.zithm", "fn main(): i32 {\n"
                                 "    var i: i32 = 0;\n"
                                 "    while (i < 10) {\n"
                                 "        i = i + 1;\n"
                                 "    }\n"
                                 "    i\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "modern pipeline lowers a while loop without errors");
}

void test_pipeline_if_else_lowers() {
    Workspace workspace;
    workspace.write("main.zithm", "fn pick(n: i32): i32 {\n"
                                 "    if (n > 0) {\n"
                                 "        1\n"
                                 "    } else {\n"
                                 "        0\n"
                                 "    }\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
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
    workspace.write("main.zithm", "import \"fixture.h\"\n"
                                 "fn main(): i32 { raw c_add(20, 22) }\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
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

void test_raw_call_markers_cover_direct_and_method_invocations() {
    Workspace workspace;
    const auto result = checkSource(
        workspace, "main.zithm",
        "raw fn direct(value: i32): i32 { value }\n"
        "struct Cell {}\n"
        "implement Cell {\n"
        "    raw fn read(self: view Cell): i32 { 7 }\n"
        "}\n"
        "fn main(): i32 {\n"
        "    let cell = Cell {};\n"
        "    return raw direct(raw cell.read());\n"
        "}\n");
    CHECK(result.ok, "explicit raw markers allow direct and method calls");
    CHECK(result.diagnostics.empty(), "marked raw calls produce no diagnostics");
}

void test_raw_call_marker_diagnostic_and_normal_function_rejection() {
    Workspace workspace;
    const std::string source =
        "raw fn dangerous(): i32 { 1 }\n"
        "fn ordinary(): i32 { 2 }\n"
        "fn main(): i32 {\n"
        "    let value = dangerous();\n"
        "    return raw ordinary();\n"
        "}\n";
    const auto result = checkSource(workspace, "main.zithm", source);
    CHECK(!result.ok, "missing and unnecessary raw markers reject the source");

    bool found_missing_marker = false;
    bool found_normal_marker_error = false;
    const size_t expected_start = source.find("dangerous()", source.find("let value"));
    for (const auto &diagnostic : result.diagnostics) {
        if (diagnostic.code == 2030u) {
            found_missing_marker = true;
            CHECK_EQ(diagnostic.start, static_cast<uint32_t>(expected_start),
                     "E2030 starts at the unmarked call");
            CHECK_EQ(diagnostic.end,
                     static_cast<uint32_t>(expected_start + std::string("dangerous()").size()),
                     "E2030 covers the unmarked call and not its surrounding statement");
        }
        found_normal_marker_error =
            found_normal_marker_error ||
            (diagnostic.severity == diagnostics::Severity::Error &&
             diagnostic.message.find("raw") != std::string::npos &&
             diagnostic.message.find("ordinary") != std::string::npos);
    }
    CHECK(found_missing_marker, "unmarked raw function calls report E2030");
    CHECK(found_normal_marker_error, "marking an ordinary function call raw is rejected");
}

void test_raw_method_call_requires_marker() {
    Workspace workspace;
    const std::string source =
        "struct Cell {}\n"
        "implement Cell {\n"
        "    raw fn read(self: view Cell): i32 { 7 }\n"
        "}\n"
        "fn main(): i32 {\n"
        "    let cell = Cell {};\n"
        "    return cell.read();\n"
        "}\n";
    const auto result = checkSource(workspace, "main.zithm", source);
    CHECK(!result.ok, "calling a raw method without its marker fails");

    const size_t expected_start = source.find("cell.read()");
    bool found_missing_marker = false;
    for (const auto &diagnostic : result.diagnostics) {
        if (diagnostic.code != diagnostics::err::RawCallRequired)
            continue;
        found_missing_marker = true;
        CHECK_EQ(diagnostic.start, static_cast<uint32_t>(expected_start),
                 "method E2030 starts at the receiver expression");
        CHECK_EQ(diagnostic.end,
                 static_cast<uint32_t>(expected_start + std::string("cell.read()").size()),
                 "method E2030 covers the complete method call");
    }
    CHECK(found_missing_marker, "unmarked raw methods report E2030");
}

void test_trust_does_not_waive_raw_method_marker() {
    Workspace workspace;
    const auto unmarked = checkSource(
        workspace, "unmarked.zithm",
        "trait Trust {}\n"
        "trait Place {\n"
        "    raw fn sample(self): i32;\n"
        "}\n"
        "fn safeCaller<T: Place + Trust>(a: T): i32 {\n"
        "    a.sample()\n"
        "}\n");
    CHECK(!unmarked.ok, "a Trust-bounded generic cannot make an unmarked raw call");
    CHECK(hasDiagnostic(unmarked, diagnostics::err::RawCallRequired),
          "raw trait dispatch through a Trust bound still reports E2030");

    const auto marked = checkSource(
        workspace, "marked.zithm",
        "trait Trust {}\n"
        "trait Place {\n"
        "    raw fn sample(self): i32;\n"
        "}\n"
        "fn safeCaller<T: Place + Trust>(a: T): i32 {\n"
        "    raw a.sample()\n"
        "}\n");
    CHECK(marked.ok, "an explicit marker permits raw trait dispatch through a Trust bound");
    CHECK(marked.diagnostics.empty(),
          "marked raw trait dispatch through a Trust bound produces no diagnostics");
}

void test_raw_attribute_reason_and_target_validation() {
    Workspace workspace;
    const std::string source =
        "#[raw(\"requires an audited caller\")]\n"
        "raw fn audited(): i32 { 1 }\n"
        "fn missingReason(): i32 {\n"
        "    return audited();\n"
        "}\n";
    const auto missing = checkSource(workspace, "main.zithm", source);
    bool found_reason = false;
    for (const auto &diagnostic : missing.diagnostics)
        found_reason = found_reason ||
                       (diagnostic.severity == diagnostics::Severity::Note &&
                        diagnostic.message.find("requires an audited caller") !=
                            std::string::npos);
    CHECK(hasDiagnostic(missing, 2030u), "raw attribute does not replace the call marker");
    CHECK(found_reason, "raw reason is attached as a note to the missing-marker diagnostic");

    const auto empty_reason =
        checkSource(workspace, "empty.zithm", "#[raw(\"\")] raw fn empty(): i32 { 1 }\n");
    CHECK(!empty_reason.ok, "raw attribute requires a non-empty reason");

    const auto normal_target =
        checkSource(workspace, "normal.zithm", "#[raw(\"reason\")] fn normal() {}\n");
    CHECK(hasDiagnostic(normal_target, diagnostics::err::AttributeNotApplicable,
                        diagnostics::Severity::Warning),
          "raw attribute on a normal function reports W2029");
}

void test_raw_function_bodies_and_arguments_remain_checked() {
    Workspace workspace;
    const auto result = checkSource(
        workspace, "main.zithm",
        "raw fn checked(value: i32): i32 { true }\n"
        "fn main(): i32 {\n"
        "    return raw checked(true);\n"
        "}\n");
    CHECK(!result.ok, "raw marker does not disable argument or body checking");
    CHECK(hasDiagnostic(result, diagnostics::err::NoMatchingFn),
          "a raw call's argument is checked against the function signature");
    CHECK(hasDiagnostic(result, diagnostics::err::TypeMismatch),
          "a raw function body is checked against its declared return type");
}

void test_raw_functions_are_not_function_values() {
    Workspace workspace;
    const auto inferred = checkSource(
        workspace, "inferred.zithm",
        "raw fn callback(value: i32): i32 { value }\n"
        "fn main(): i32 {\n"
        "    let inferred = callback;\n"
        "    0\n"
        "}\n");
    CHECK(!inferred.ok, "a raw function cannot initialize an inferred binding");
    CHECK(hasRawFunctionValueDiagnostic(inferred),
          "an inferred binding reports that raw functions cannot be values");

    const auto higher_order = checkSource(
        workspace, "higher-order.zithm",
        "raw fn callback(value: i32): i32 { value }\n"
        "fn apply(function: fn(i32): i32): i32 { function(1) }\n"
        "fn main(): i32 { apply(callback) }\n");
    CHECK(!higher_order.ok, "a raw function cannot be passed as a callback");
    CHECK(hasRawFunctionValueDiagnostic(higher_order),
          "a higher-order argument reports that raw functions cannot be values");

    const auto returned = checkSource(
        workspace, "returned.zithm",
        "raw fn callback(value: i32): i32 { value }\n"
        "fn returnCallback(): fn(i32): i32 { return callback; }\n"
        "fn main(): i32 { 0 }\n");
    CHECK(!returned.ok, "a raw function cannot be returned as a value");
    CHECK(hasRawFunctionValueDiagnostic(returned),
          "a return expression reports that raw functions cannot be values");
}

void test_c_header_raw_call_warnings_and_safe_alias() {
#ifdef ZITH_ENABLE_C_INTEROP
    Workspace workspace;
    workspace.write("fixture.h", "int c_add(int left, int right);\n");
    const std::string source =
        "import \"fixture.h\"\n"
        "#[safe] fn audited_add(left: i32, right: i32): i32 = extern c_add;\n"
        "fn main(): i32 {\n"
        "    let unmarked = c_add(1, 2);\n"
        "    let marked = raw c_add(3, 4);\n"
        "    let audited = audited_add(5, 6);\n"
        "    return unmarked + marked + audited;\n"
        "}\n";
    const auto result = checkSource(workspace, "main.zithm", source);

    size_t external_warnings = 0;
    size_t missing_markers = 0;
    const size_t unmarked_start = source.find("c_add(1, 2)");
    const size_t marked_start   = source.find("raw c_add(3, 4)");
    for (const auto &diagnostic : result.diagnostics) {
        if (diagnostic.code == diagnostics::err::ExternalCallBoundary) {
            ++external_warnings;
            if (diagnostic.start == unmarked_start) {
                CHECK_EQ(diagnostic.end,
                         static_cast<uint32_t>(unmarked_start + std::string("c_add(1, 2)").size()),
                         "unmarked C warning covers the call expression");
            } else {
                CHECK_EQ(diagnostic.start, static_cast<uint32_t>(marked_start),
                         "marked C warning starts at the raw call marker");
                CHECK_EQ(diagnostic.end,
                         static_cast<uint32_t>(marked_start +
                                               std::string("raw c_add(3, 4)").size()),
                         "marked C warning covers the raw call expression");
            }
        }
        if (diagnostic.code == diagnostics::err::RawCallRequired) {
            ++missing_markers;
            CHECK_EQ(diagnostic.start, static_cast<uint32_t>(unmarked_start),
                     "missing-marker error starts at the unmarked C call");
            CHECK_EQ(diagnostic.end,
                     static_cast<uint32_t>(unmarked_start + std::string("c_add(1, 2)").size()),
                     "missing-marker error covers the unmarked C call");
        }
    }
    CHECK_EQ(external_warnings, 2u,
             "C-header calls warn at each invocation, including explicitly marked calls");
    CHECK_EQ(missing_markers, 1u, "unmarked C-header calls still require the raw marker");
    CHECK(!result.ok, "the unmarked C-header call remains an error");

    const auto safe_result =
        checkSource(workspace, "safe.zithm",
                    "import \"fixture.h\"\n"
                    "#[safe] fn audited_add(left: i32, right: i32): i32 = extern c_add;\n"
                    "fn main(): i32 {\n"
                    "    audited_add(5, 6)\n"
                    "}\n");
    CHECK(safe_result.ok, "the safe C alias permits an ordinary call");
    CHECK(safe_result.diagnostics.empty(),
          "the safe C alias suppresses both the raw marker requirement and boundary warning");
#endif
}

void test_c_header_import_lowers_macro_constants() {
    Workspace workspace;
    workspace.write("fixture.h", "#define ANSWER 42\n"
                                 "#define RATIO 1.5\n"
                                 "#define YES true\n"
                                 "#define LETTER 'A'\n"
                                 "int next(int value);\n");
    workspace.write("main.zithm", "import \"fixture.h\"\n"
                                 "fn main(): i32 {\n"
                                 "    let answer: i32 = ANSWER;\n"
                                 "    let ratio: f64 = RATIO;\n"
                                 "    let yes: bool = YES;\n"
                                 "    let letter: char = LETTER;\n"
                                 "    (answer + raw next(1)) as i32\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
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

void test_c_header_globals_bind_and_lower_to_external_addresses() {
#ifdef ZITH_ENABLE_C_INTEROP
    Workspace workspace;
    workspace.write("fixture.h", "typedef struct _zith_file FILE;\n"
                                 "extern FILE *stream;\n"
                                 "extern int counter;\n"
                                 "extern const int *pointee_const;\n"
                                 "extern int *const object_const;\n"
                                 "extern const int immutable_value;\n");
    workspace.write("main.zithm", "import \"fixture.h\"\n"
                                 "extern var manual: i32 = extern c_manual;\n"
                                 "fn accepts_file(file: *FILE) {}\n"
                                 "fn main(): i32 {\n"
                                 "    counter = 9;\n"
                                 "    manual = counter;\n"
                                 "    pointee_const = pointee_const;\n"
                                 "    return counter;\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    const bool ok = session.runTo(session::Stage::HirLowered);
    std::string message = "C globals, aliases, and reachable typedefs lower through HIR";
    if (!ok) {
        for (const auto &diagnostic : session.diags().all()) {
            if (diagnostic.severity == diagnostics::Severity::Error) {
                message += ": " + diagnostic.message;
                break;
            }
        }
    }
    CHECK(ok, message.c_str());
    if (!ok)
        return;

    bool saw_counter = false;
    bool saw_manual  = false;
    bool saw_read    = false;
    bool saw_address = false;
    for (size_t index = 0; index < session.hirModule().exprCount(); ++index) {
        const auto &expr = session.hirModule().getExpr(static_cast<hir::HirExprId>(index));
        if (const auto *address = std::get_if<hir::HirExternalGlobalAddr>(&expr)) {
            const auto name = session.interner().lookup(address->name);
            saw_counter    = saw_counter || name == "counter";
            saw_manual     = saw_manual || name == "c_manual";
            saw_address    = true;
            CHECK(address->type != types::kInvalidType,
                  "external global address retains its object type");
        }
        if (const auto *unary = std::get_if<hir::HirUnary>(&expr);
            unary != nullptr && unary->op == hir::HirUnaryOp::Deref &&
            unary->operand < session.hirModule().exprCount() &&
            std::holds_alternative<hir::HirExternalGlobalAddr>(
                session.hirModule().getExpr(unary->operand)))
            saw_read = true;
    }
    CHECK(saw_counter, "imported C global keeps its C linker symbol in HIR");
    CHECK(saw_manual, "manual extern alias keeps its C linker symbol in HIR");
    CHECK(saw_address, "referenced external globals lower to addressable HIR");
    CHECK(saw_read, "external global reads lower through an address and load");

    workspace.write("bad.zithm", "import \"fixture.h\"\n"
                                "fn main() {\n"
                                "    immutable_value = 1;\n"
                                "}\n");
    session::CompilationSession immutable(options, (workspace.root / "bad.zithm").string());
    immutable.setBuffered(true);
    CHECK(!immutable.runTo(session::Stage::HirLowered),
          "assignment to an imported const C global is rejected");
    bool found_const_error = false;
    for (const auto &diagnostic : immutable.diags().all())
        found_const_error =
            found_const_error || diagnostic.message.find("const C global") != std::string::npos;
    CHECK(found_const_error, "const global diagnostic identifies the immutable C object");

    workspace.write("bad-pointer.zithm", "import \"fixture.h\"\n"
                                        "fn main() {\n"
                                        "    object_const = null;\n"
                                        "}\n");
    session::CompilationSession const_pointer(options,
                                               (workspace.root / "bad-pointer.zithm").string());
    const_pointer.setBuffered(true);
    CHECK(!const_pointer.runTo(session::Stage::HirLowered),
          "top-level const pointer object is immutable");

    workspace.write("mutable-pointer.zithm", "import \"fixture.h\"\n"
                                            "fn main() {\n"
                                            "    pointee_const = pointee_const;\n"
                                            "}\n");
    session::CompilationSession mutable_pointer(
        options, (workspace.root / "mutable-pointer.zithm").string());
    mutable_pointer.setBuffered(true);
    CHECK(mutable_pointer.runTo(session::Stage::HirLowered),
          "const pointee qualification does not make the pointer object immutable");
#else
    CHECK(true, "C global binding tests require libclang");
#endif
}

void test_external_const_declaration_needs_no_initializer() {
    // `extern const name: T;` references read-only storage defined outside the
    // module, so it must not be treated like a module-scope `const`, which
    // requires a constant initializer. Regression for the sema check that
    // rejected the documented form.
    Workspace workspace;
    workspace.write("main.zithm", "extern const rate: f64;\n"
                                 "extern var counter: i32;\n"
                                 "fn read_rate(): f64 { return rate; }\n"
                                 "fn bump(): i32 { counter = counter + 1; return counter; }\n"
                                 "fn main(): i32 { return 0; }\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    const bool ok = session.runTo(session::Stage::HirLowered);
    if (!ok) {
        for (const auto &diagnostic : session.diags().all()) {
            if (diagnostic.severity == diagnostics::Severity::Error)
                std::printf("    [ExternConstDiag] %s\n", diagnostic.message.c_str());
        }
    }
    CHECK(ok, "extern const declarations without an initializer lower");
}

void test_c_header_constant_duplicate_diagnostic() {
    Workspace workspace;
    workspace.write("fixture.h", "#define ANSWER 42\n");
    workspace.write("main.zithm", "import \"fixture.h\"\n"
                                 "fn ANSWER(): i32 { 1 }\n"
                                 "fn main(): i32 { ANSWER() }\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
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
    workspace.write("main.zithm", "import \"fixture.h\"\n"
                                 "const JOINED: i32 = 2;\n"
                                 "fn main(): i32 { JOINED }\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
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
    workspace.write("main.zithm", "macro twice(v: expr) { v + v }\n"
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

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
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
    workspace.write("main.zithm", "raw macro swap(a: identifier, b: identifier) {\n"
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

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "raw macro statements splice and lower through the modern pipeline");
    CHECK(session.snapshot() != nullptr && session.snapshot()->diagnostics().empty(),
          "raw macro expansion produces no semantic diagnostics");
}

void test_pipeline_named_stage_lowers() {
    Workspace workspace;
    workspace.write("main.zithm", "fn main(): i32 {\n"
                                 "    let x = 5\n"
                                 "        |> step1: { it + 1 }\n"
                                 "        |> step2: { it * 2 };\n"
                                 "    x\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "named pipeline stages lower through the modern pipeline");
    CHECK(session.snapshot() != nullptr && session.snapshot()->diagnostics().empty(),
          "plain named pipeline stages produce no diagnostics");
}

void test_pipeline_marked_named_stage_lowers() {
    Workspace workspace;
    workspace.write("main.zithm",
                    "pub trait Guard { fn leave(var self): void; }\n"
                    "pub trait Scope { fn enter(var self): dyn Guard; }\n"
                    "struct G { }\n"
                    "struct R { }\n"
                    "implement G as Guard { fn leave(var self): void { } }\n"
                    "implement R as Scope { fn enter(var self): dyn Guard { return G { }; } }\n"
                    "#[scope] pub macro scoped(resources: identifier...) { }\n"
                    "fn main(): i32 {\n"
                    "    var r = R { };\n"
                    "    let x = 5\n"
                    "        |> render: @scoped(r) { it * 2 };\n"
                    "    x\n"
                    "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "a marked named pipeline stage lowers through the modern pipeline");
    CHECK(session.snapshot() != nullptr && session.snapshot()->diagnostics().empty(),
          "a marked named pipeline stage produces no diagnostics");
}

void test_named_block_invalid_position_is_diagnosed() {
    Workspace workspace;
    const auto result = checkSource(workspace, "main.zithm", "fn main() {\n"
                                                             "    if (true) {\n"
                                                             "        setup: { }\n"
                                                             "    }\n"
                                                             "}\n");
    CHECK(!result.ok, "a named block nested in `if` fails type checking");
    CHECK(hasDiagnostic(result, diagnostics::err::NamedBlockInvalidPosition),
          "a misplaced named block reports NamedBlockInvalidPosition");
}

void test_named_block_duplicate_name_is_diagnosed() {
    Workspace workspace;
    const auto result = checkSource(workspace, "main.zithm", "fn main() {\n"
                                                             "    setup: { }\n"
                                                             "    setup: { }\n"
                                                             "}\n");
    CHECK(!result.ok, "duplicate named block names fail type checking");
    CHECK(hasDiagnostic(result, diagnostics::err::NamedBlockDuplicateName),
          "a duplicate named block reports NamedBlockDuplicateName");
}

void test_named_block_label_collision_is_symmetric() {
    // A named block and a loop label share the per-body name space, so the
    // collision must be rejected in either declaration order.
    {
        Workspace workspace;
        const auto result = checkSource(workspace, "main.zithm", "fn main() {\n"
                                                                 "    outer: for (i in 0..3) { }\n"
                                                                 "    outer: { }\n"
                                                                 "}\n");
        CHECK(!result.ok, "a named block after a loop label of the same name fails");
        CHECK(hasDiagnostic(result, diagnostics::err::NamedBlockDuplicateName),
              "label-then-block collision reports NamedBlockDuplicateName");
    }
    {
        Workspace workspace;
        const auto result = checkSource(workspace, "main.zithm", "fn main() {\n"
                                                                 "    setup: { }\n"
                                                                 "    setup: for (i in 0..3) { }\n"
                                                                 "}\n");
        CHECK(!result.ok, "a loop label after a named block of the same name fails");
        CHECK(hasDiagnostic(result, diagnostics::err::NamedBlockDuplicateName),
              "block-then-label collision reports NamedBlockDuplicateName");
    }
}

void test_named_block_invalid_positions_are_diagnosed() {
    const char *const bodies[] = {
        "fn main() {\n    for (i in 0..3) {\n        setup: { }\n    }\n}\n",
        "fn main() {\n    while (true) {\n        setup: { }\n        break;\n    }\n}\n",
        "fn main() {\n    {\n        setup: { }\n    }\n}\n",
        "fn main() {\n    outer: {\n        inner: { }\n    }\n}\n",
    };
    for (const char *body : bodies) {
        Workspace workspace;
        const auto result = checkSource(workspace, "main.zithm", body);
        CHECK(!result.ok, "a nested named block fails type checking");
        CHECK(hasDiagnostic(result, diagnostics::err::NamedBlockInvalidPosition),
              "a nested named block reports NamedBlockInvalidPosition");
    }
}

void test_named_block_in_state_body_is_allowed() {
    Workspace workspace;
    const auto result = checkSource(workspace, "main.zithm", "state Done() {\n"
                                                             "    setup: {\n"
                                                             "        var v: i32 = 1;\n"
                                                             "        _ = v;\n"
                                                             "    }\n"
                                                             "    return;\n"
                                                             "}\n"
                                                             "fn main() { dock Done(); }\n");
    CHECK(result.ok, "a named block is valid directly in a state body");
}

void test_break_to_named_block_is_rejected() {
    Workspace workspace;
    const auto result = checkSource(workspace, "main.zithm", "fn main() {\n"
                                                             "    setup: {\n"
                                                             "        break setup;\n"
                                                             "    }\n"
                                                             "}\n");
    CHECK(!result.ok, "a break targeting a named block fails type checking");
    CHECK(!hasDiagnostic(result, diagnostics::err::NamedBlockDuplicateName),
          "a named block is not a duplicate-name error target");
    // The name is descriptive, not a loop label, so the break cannot resolve.
    CHECK(hasDiagnostic(result, diagnostics::err::UnsupportedSyntax),
          "break to a named block reports UnsupportedSyntax");
}

void test_error_inside_named_block_names_the_block() {
    Workspace workspace;
    const auto result = checkSource(workspace, "main.zithm", "fn main(): i32 {\n"
                                                             "    setup: {\n"
                                                             "        let x: i32 = \"hello\";\n"
                                                             "    }\n"
                                                             "    0\n"
                                                             "}\n");
    CHECK(!result.ok, "a type error inside a named block fails type checking");
    bool named = false;
    for (const auto &diagnostic : result.diagnostics)
        named |= diagnostic.message.find("in block 'setup':") != std::string::npos;
    CHECK(named, "an error inside a named block mentions the block name");
}

void test_named_block_binding_does_not_leak_out_of_the_block() {
    Workspace workspace;
    const auto result = checkSource(workspace, "main.zithm", "fn main(): i32 {\n"
                                                             "    setup: {\n"
                                                             "        let inner: i32 = 7;\n"
                                                             "        _ = inner;\n"
                                                             "    }\n"
                                                             "    inner\n"
                                                             "}\n");
    CHECK(!result.ok, "a binding declared in a named block is not visible after it");
    CHECK(hasDiagnostic(result, diagnostics::err::UndefinedIdent),
          "reading a named-block local outside the block reports UndefinedIdent");
}

void test_named_pipeline_stage_keeps_it_consumption_rule() {
    // A named stage is a normal pipeline stage, so the `it`-consumption
    // contract is unchanged by the name.
    {
        Workspace workspace;
        const auto result = checkSource(workspace, "main.zithm", "fn main(): i32 {\n"
                                                                 "    let x = 5 |> step: { 42 };\n"
                                                                 "    x\n"
                                                                 "}\n");
        CHECK(!result.ok, "a named stage that ignores 'it' fails type checking");
        CHECK(hasDiagnostic(result, diagnostics::err::ExpectedExpr),
              "a named stage that ignores 'it' reports the pipeline consumption error");
    }
    {
        Workspace workspace;
        const auto result = checkSource(workspace, "main.zithm", "fn main(): i32 {\n"
                                                                 "    let x = 5 |> step: { it + it };\n"
                                                                 "    x\n"
                                                                 "}\n");
        CHECK(!result.ok, "a named stage that consumes 'it' twice fails type checking");
        CHECK(hasDiagnostic(result, diagnostics::err::UnsupportedSyntax),
              "a named stage that consumes 'it' twice reports the pipeline consumption error");
    }
    {
        Workspace workspace;
        const auto result = checkSource(workspace, "main.zithm",
                                        "fn main(): i32 {\n"
                                        "    let x = 5 |> step: {\n"
                                        "        var s = 0;\n"
                                        "        for (i in 0..3) {\n"
                                        "            if (i == 2) { break; }\n"
                                        "            s = s + i;\n"
                                        "        }\n"
                                        "        it + s\n"
                                        "    };\n"
                                        "    x\n"
                                        "}\n");
        CHECK(result.ok, "local loop control flow inside a named stage stays valid");
    }
}

void test_named_block_marker_accepts_each_place_kind() {
    Workspace workspace;
    const auto result = checkSource(
        workspace, "main.zithm", "pub trait Guard { fn leave(var self): void; }\n"
                                 "pub trait Scope { fn enter(var self): dyn Guard; }\n"
                                 "struct G { }\n"
                                 "struct R { }\n"
                                 "struct Holder { r: R }\n"
                                 "implement G as Guard { fn leave(var self): void { } }\n"
                                 "implement R as Scope {\n"
                                 "    fn enter(var self): dyn Guard { return G { }; }\n"
                                 "}\n"
                                 "#[scope] pub macro scoped(resources: identifier...) { }\n"
                                 "const global_r: R = R { };\n"
                                 "fn use_param(param: R) {\n"
                                 "    from_param: @scoped(param) { }\n"
                                 "}\n"
                                 "fn main() {\n"
                                 "    var holder = Holder { r: R { } };\n"
                                 "    var binding = R { };\n"
                                 "    from_binding: @scoped(binding) { }\n"
                                 "    from_field: @scoped(holder.r) { }\n"
                                 "    from_global: @scoped(global_r) { }\n"
                                 "    use_param(R { });\n"
                                 "}\n");
    CHECK(result.ok,
          "a marker accepts a binding, a parameter, a global, and a field as arguments");
}

void test_named_block_marker_argument_must_implement_scope() {
    Workspace workspace;
    const auto result = checkSource(
        workspace, "main.zithm", "pub trait Guard { fn leave(var self): void; }\n"
                                 "pub trait Scope { fn enter(var self): dyn Guard; }\n"
                                 "struct NotScope { }\n"
                                 "#[scope] pub macro scoped(resources: identifier...) { }\n"
                                 "fn main() {\n"
                                 "    var x = NotScope { };\n"
                                 "    render: @scoped(x) { }\n"
                                 "}\n");
    CHECK(!result.ok, "a marker argument that is not a Scope fails type checking");
    CHECK(hasDiagnostic(result, diagnostics::err::NamedBlockArgNotScope),
          "a non-Scope marker argument reports NamedBlockArgNotScope");
    size_t scope_errors = 0;
    for (const auto &diagnostic : result.diagnostics)
        scope_errors += diagnostic.code == diagnostics::err::NamedBlockArgNotScope ? 1U : 0U;
    CHECK_EQ(scope_errors, 1U, "exactly one dedicated marker diagnostic, with no cascade");
}

void test_named_block_marker_macro_must_be_scope_aware() {
    Workspace workspace;
    const auto result = checkSource(workspace, "main.zithm", "pub macro plain(x: identifier) { }\n"
                                                             "fn main() {\n"
                                                             "    var a = 1;\n"
                                                             "    render: @plain(a) { }\n"
                                                             "}\n");
    CHECK(!result.ok, "a non-scope macro in the outer position fails type checking");
    CHECK(hasDiagnostic(result, diagnostics::err::NamedBlockInvalidPosition),
          "a non-scope macro marker reports NamedBlockInvalidPosition");
    // The argument check must not run for a rejected marker: it would only add
    // a cascading NamedBlockArgNotScope on top of the dedicated diagnostic.
    size_t arg_errors = 0;
    for (const auto &diagnostic : result.diagnostics)
        arg_errors += diagnostic.code == diagnostics::err::NamedBlockArgNotScope ? 1U : 0U;
    CHECK_EQ(arg_errors, 0U, "a rejected marker produces no cascading argument diagnostic");
}

void test_named_block_marker_rejects_temporary_argument() {
    Workspace workspace;
    const auto result = checkSource(
        workspace, "main.zithm", "pub trait Guard { fn leave(var self): void; }\n"
                                 "pub trait Scope { fn enter(var self): dyn Guard; }\n"
                                 "struct G { }\n"
                                 "struct R { }\n"
                                 "implement G as Guard { fn leave(var self): void { } }\n"
                                 "implement R as Scope {\n"
                                 "    fn enter(var self): dyn Guard { return G { }; }\n"
                                 "}\n"
                                 "#[scope] pub macro scoped(resources: identifier...) { }\n"
                                 "fn make() : R { return R { }; }\n"
                                 "fn main() {\n"
                                 "    render: @scoped(make()) { }\n"
                                 "}\n");
    CHECK(!result.ok, "a temporary marker argument fails type checking");
    CHECK(hasDiagnostic(result, diagnostics::err::NamedBlockArgNotScope),
          "a temporary marker argument reports NamedBlockArgNotScope");
    size_t scope_errors = 0;
    for (const auto &diagnostic : result.diagnostics)
        scope_errors += diagnostic.code == diagnostics::err::NamedBlockArgNotScope ? 1U : 0U;
    CHECK_EQ(scope_errors, 1U, "a temporary argument reports one diagnostic, with no cascade");
}

void test_pipeline_normal_macro_hygiene_and_call_site_scope() {
    Workspace workspace;
    workspace.write("main.zithm", "const base: i32 = 100;\n"
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

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "normal macro resolution prefers call-site scope and keeps bindings hygienic");
    CHECK(session.snapshot() != nullptr && session.snapshot()->diagnostics().empty(),
          "normal macro hygiene/scope case produces no semantic diagnostics");
}

void test_pipeline_raw_macro_sees_call_site_then_module() {
    Workspace workspace;
    workspace.write("main.zithm", "const base: i32 = 10;\n"
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

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "raw macro body resolves from its splice scope and can see the module fallback");
    CHECK(session.snapshot() != nullptr && session.snapshot()->diagnostics().empty(),
          "raw macro call-site/global scope case produces no semantic diagnostics");
}

void test_pipeline_tag_macro_is_rejected() {
    Workspace workspace;
    workspace.write("main.zithm", "tag macro RunTwice(attributes, body: body) {\n"
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

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    CHECK(!session.runTo(session::Stage::HirLowered),
          "tag macros are rejected in the Zith-- pipeline");
    CHECK(session.snapshot() != nullptr && !session.snapshot()->diagnostics().empty(),
          "tag macro rejection surfaces a frontend diagnostic");
}

void test_pipeline_imported_state_machine_lowers() {
    Workspace workspace;
    workspace.write("helper.zithm", "pub state target(v: i32): i32 {\n"
                                   "    if (v < 1) {\n"
                                   "        jump target(v + 1);\n"
                                   "    }\n"
                                   "    return v;\n"
                                   "}\n");
    workspace.write("main.zithm", "from helper\n"
                                 "fn main(): i32 {\n"
                                 "    return dock target(0);\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    const bool lowered_ok = session.runTo(session::Stage::HirLowered);
    CHECK(lowered_ok, "imported state machine lowers through the modern pipeline");
    CHECK(session.snapshot() != nullptr && session.snapshot()->diagnostics().empty(),
          "imported state machine produces no diagnostics");
}

void test_modern_imported_static_method_hir() {
    Workspace workspace;
    workspace.write("lib.zithm", "pub struct Box { pub value: i32 }\n"
                                "implement Box {\n"
                                "    fn make(v: i32): Box { Box { value: v } }\n"
                                "}\n");
    workspace.write("main.zithm", "from lib\n"
                                 "fn main(): i32 {\n"
                                 "    let b = Box.make(42);\n"
                                 "    b.value\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "imported static method lowers through the modern pipeline");
    CHECK(session.hirModule().getFnCount() >= 2u, "HIR includes both the imported method and main");
}

void test_modern_imported_receiver_method_hir() {
    Workspace workspace;
    workspace.write("lib.zithm", "pub struct Counter { pub value: i32 }\n"
                                "implement Counter {\n"
                                "    fn get(self: view Counter): i32 { self.value }\n"
                                "}\n");
    workspace.write("main.zithm", "from lib\n"
                                 "fn main(): i32 {\n"
                                 "    let c = Counter { value: 42 };\n"
                                 "    c.get()\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "imported receiver method lowers through the modern pipeline");
    CHECK(session.hirModule().getFnCount() >= 2u,
          "HIR includes both the imported receiver method and main");
}

void test_modern_imported_external_symbol_method_hir() {
    Workspace workspace;
    workspace.write("video.zithm",
                    "pub struct Window {}\n"
                    "implement Window {\n"
                    "    pub fn destroy(self: lend Window): void = extern SDL_DestroyWindow;\n"
                    "}\n");
    workspace.write("main.zithm", "from video\n"
                                 "fn main(): i32 {\n"
                                 "    var w: Window = Window {};\n"
                                 "    w.destroy();\n"
                                 "    return 0;\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
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
    workspace.write("lib.zithm", "pub struct Box { pub value: i32 }\n"
                                "implement Box {\n"
                                "    fn make(v: i32): Box { Box { value: v } }\n"
                                "}\n");
    workspace.write("main.zithm", "from lib\n"
                                 "fn main(): i32 {\n"
                                 "    let b = Box.make(42);\n"
                                 "    b.value\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "imported method body with a struct literal lowers through the modern pipeline");
    CHECK(session.snapshot() != nullptr && session.snapshot()->diagnostics().empty(),
          "imported method body with a struct literal produces no diagnostics");
}

void test_modern_optional_method_narrowing_hir() {
    Workspace workspace;
    workspace.write("lib.zithm", "pub struct Box { pub value: i32 }\n"
                                "implement Box {\n"
                                "    fn get(self: view Box): i32 { self.value }\n"
                                "}\n");
    workspace.write("main.zithm", "from lib\n"
                                 "fn main(): i32 {\n"
                                 "    var b: ?Box = null;\n"
                                 "    if (b is null) { return 1; }\n"
                                 "    return b.get();\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
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
    workspace.write("lib.zithm", "pub struct Box { pub value: i32 }\n"
                                "implement Box {\n"
                                "    fn get(self: view Box): i32 { self.value }\n"
                                "}\n");
    workspace.write("main.zithm", "from lib\n"
                                 "fn main(): i32 {\n"
                                 "    var b: ?Box = null;\n"
                                 "    if not (b is null) { return b.get(); }\n"
                                 "    return 1;\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
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
    workspace.write("video.zithm",
                    "pub struct Window {}\n"
                    "implement Window {\n"
                    "    pub fn destroy(self: lend Window): void = extern SDL_DestroyWindow;\n"
                    "}\n");
    workspace.write("main.zithm", "from video\n"
                                 "fn main(): i32 {\n"
                                 "    var window: ?*Window = null;\n"
                                 "    if (window is null) { return 1; }\n"
                                 "    window.destroy();\n"
                                 "    return 0;\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
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
    workspace.write("helper.zithm", "pub macro twice(v: expr) { v + v }\n"
                                   "pub macro qualify(v: expr) { v }\n");
    workspace.write("main.zithm", "from helper\n"
                                 "fn main(): i32 { @twice(21) }\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "imported public macros expand and lower through the modern pipeline");
    CHECK(session.snapshot() != nullptr && session.snapshot()->diagnostics().empty(),
          "imported macro expansion produces no diagnostics");
}

void test_imported_macro_unknown_without_import() {
    Workspace workspace;
    workspace.write("helper.zithm", "pub macro twice(v: expr) { v + v }\n");
    workspace.write("main.zithm", "fn main(): i32 { @twice(21) }\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::Imported;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
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
    workspace.write("helper.zithm", "pub macro twice(v: expr) { v + v }\n");
    workspace.write("main.zithm", "from helper { twice as double }\n"
                                 "fn main(): i32 { @double(10) }\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "selective imported macro alias expands and lowers");
    CHECK(session.snapshot() != nullptr && session.snapshot()->diagnostics().empty(),
          "selective imported macro alias has no diagnostics");
}

void test_imported_macro_namespace_alias() {
    Workspace workspace;
    workspace.write("helper.zithm", "pub macro twice(v: expr) { v + v }\n");
    workspace.write("main.zithm", "import helper as h\n"
                                 "fn main(): i32 { @h.twice(10) }\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "imported macro under namespace alias expands and lowers");
    CHECK(session.snapshot() != nullptr && session.snapshot()->diagnostics().empty(),
          "imported namespace macro has no diagnostics");
}

void test_imported_macro_duplicate_diagnostic() {
    Workspace workspace;
    workspace.write("helper.zithm", "pub macro twice(v: expr) { v + v }\n");
    workspace.write("main.zithm", "from helper { twice }\n"
                                 "macro twice(v: expr) { v }\n"
                                 "fn main(): i32 { @twice(1) }\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::Imported;

    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
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
    test_inferred_return_type_is_available_to_earlier_callers();
    test_pipeline_discard_non_void_call_result();
    test_pipeline_discardable_attribute_allows_call();
    test_pipeline_unknown_attribute_warns_and_targets_are_checked();
    test_raw_call_markers_cover_direct_and_method_invocations();
    test_raw_call_marker_diagnostic_and_normal_function_rejection();
    test_raw_method_call_requires_marker();
    test_trust_does_not_waive_raw_method_marker();
    test_raw_attribute_reason_and_target_validation();
    test_raw_function_bodies_and_arguments_remain_checked();
    test_raw_functions_are_not_function_values();
    test_c_header_raw_call_warnings_and_safe_alias();
    test_imported_console_void_main_discardable();
    test_untyped_uninitialized_binding_is_rejected();
    test_typed_uninitialized_binding_with_later_assignment_lowers();
    test_typed_scalar_binding_first_assignment_with_explicit_return_lowers();
    test_typed_scalar_binding_second_assignment_is_rejected();
    test_typed_uninitialized_unused_binding_lowers();
    test_typed_uninitialized_binding_use_before_assignment_fails();
    test_inferred_return_type_from_value_tail();
    test_inferred_return_type_ignores_terminated_assignment_tail();
    test_inferred_return_type_from_explicit_returns();
    test_inferred_return_type_rejects_direct_recursion();
    test_inferred_return_type_rejects_method_recursion();
    test_inferred_return_type_rejects_indirect_recursion();
    test_inferred_return_type_rejects_incompatible_returns();
    test_pipeline_multifile_module_dependency();
    test_pipeline_export_platform_import();
    test_pipeline_export_shared_prefix();
    test_pipeline_while_loop_lowers();
    test_pipeline_if_else_lowers();
    test_c_header_import_lowers_external_function();
    test_c_header_import_lowers_macro_constants();
    test_c_header_globals_bind_and_lower_to_external_addresses();
    test_external_const_declaration_needs_no_initializer();
    test_c_header_constant_duplicate_diagnostic();
    test_c_header_constant_let_duplicate_diagnostic();
    test_pipeline_macro_expansion_lowers();
    test_pipeline_raw_macro_lowers();
    test_pipeline_named_stage_lowers();
    test_pipeline_marked_named_stage_lowers();
    test_named_block_invalid_position_is_diagnosed();
    test_named_block_duplicate_name_is_diagnosed();
    test_named_block_label_collision_is_symmetric();
    test_named_block_invalid_positions_are_diagnosed();
    test_named_block_in_state_body_is_allowed();
    test_break_to_named_block_is_rejected();
    test_error_inside_named_block_names_the_block();
    test_named_block_binding_does_not_leak_out_of_the_block();
    test_named_pipeline_stage_keeps_it_consumption_rule();
    test_named_block_marker_accepts_each_place_kind();
    test_named_block_marker_argument_must_implement_scope();
    test_named_block_marker_macro_must_be_scope_aware();
    test_named_block_marker_rejects_temporary_argument();
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
