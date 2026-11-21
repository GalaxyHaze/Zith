#include "cli/options.hpp"
#include "diagnostics/error-codes.hpp"
#include "session/compilation-session.hpp"
#include "session/frontend-context.hpp"
#include "session/pipeline-plan.hpp"
#include "test-common.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace zith;
using namespace zith::session;

namespace {

namespace fs = std::filesystem;

struct Workspace {
    fs::path root = fs::temp_directory_path() / "zith-frontend-context-tests";

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

    [[nodiscard]] std::string path(const std::string &name) const {
        return (root / name).string();
    }

    [[nodiscard]] FrontendConfig config(const size_t workers) const {
        FrontendConfig result;
        result.workspaceRoot      = root.string();
        result.maxFrontendWorkers = workers;
        result.compilerVersion    = "test";
        return result;
    }
};

std::vector<std::string> moduleKeys(const CompilationSnapshot &snapshot) {
    std::vector<std::string> keys;
    for (const auto &module : snapshot.modules())
        keys.push_back(module->key + ":" + module->fingerprint.toString());
    return keys;
}

std::vector<std::string> diagnosticKeys(const CompilationSnapshot &snapshot) {
    std::vector<std::string> keys;
    for (const auto &diagnostic : snapshot.diagnostics()) {
        keys.push_back(std::to_string(diagnostic.code) + ":" + std::to_string(diagnostic.start) +
                       ":" + diagnostic.message);
    }
    return keys;
}

void test_worker_counts_and_determinism() {
    Workspace workspace;
    workspace.write("main.zithm", "from dep\nfn main() { dep_fn() }\n");
    workspace.write("dep.zithm", "fn dep_fn() { }\n");

    FrontendContext serial(workspace.config(1));
    auto serial_result = serial.analyzeFile(workspace.path("main.zithm"));
    CHECK(serial_result.isOk(), "serial frontend analysis succeeds");
    if (!serial_result)
        return;

    FrontendContext parallel(workspace.config(2));
    auto parallel_result = parallel.analyzeFile(workspace.path("main.zithm"));
    CHECK(parallel_result.isOk(), "parallel frontend analysis succeeds");
    if (!parallel_result)
        return;

    CHECK_EQ(serial.maxFrontendWorkers(), 1u, "one frontend worker remains serial");
    CHECK(parallel.maxFrontendWorkers() >= 1u && parallel.maxFrontendWorkers() <= 2u,
          "frontend worker count is clamped to hardware");
    CHECK_EQ(moduleKeys(*serial_result.value()), moduleKeys(*parallel_result.value()),
             "module order and fingerprints are deterministic across worker counts");
    CHECK_EQ(diagnosticKeys(*serial_result.value()), diagnosticKeys(*parallel_result.value()),
             "diagnostic order is deterministic across worker counts");
    CHECK_EQ(serial_result.value()->modules().size(), 2u, "import closure has root and dependency");
}

void test_cache_invalidation_and_overlays() {
    Workspace workspace;
    workspace.write("main.zithm", "from dep\nfn main() { dep_fn() }\n");
    workspace.write("dep.zithm", "fn dep_fn() { }\n");

    FrontendContext context(workspace.config(2));
    auto first = context.analyzeFile(workspace.path("main.zithm"));
    CHECK(first.isOk(), "initial cached analysis succeeds");
    if (!first)
        return;
    const auto first_metrics = context.metrics().cache;

    auto second = context.analyzeFile(workspace.path("main.zithm"));
    CHECK(second.isOk(), "repeated analysis succeeds");
    CHECK(context.metrics().cache.hits > first_metrics.hits,
          "repeated revision reuses module artifacts");

    workspace.write("dep.zithm", "fn dep_fn() { }\nfn changed() { }\n");
    context.invalidatePath(workspace.path("dep.zithm"));
    auto changed = context.analyzeFile(workspace.path("main.zithm"));
    CHECK(changed.isOk(), "transitively invalidated analysis succeeds");
    CHECK(context.metrics().cache.invalidated >= 2u,
          "dependency invalidation removes the dependency and its root dependent");

    context.setOverlay(workspace.path("dep.zithm"), "fn overlay() { }\n");
    auto overlay = context.analyzeFile(workspace.path("main.zithm"));
    CHECK(overlay.isOk(), "open-document overlay analysis succeeds");
    if (overlay) {
        const auto *dep =
            overlay.value()->findModule(SourceCatalog::canonicalPath(workspace.path("dep.zithm")));
        CHECK(dep != nullptr, "overlay dependency is in snapshot");
        if (dep)
            CHECK(dep->source->text.find("overlay") != std::string::npos,
                  "open document content takes precedence over disk");
    }

    context.removeOverlay(workspace.path("dep.zithm"));
    auto closed = context.analyzeFile(workspace.path("main.zithm"));
    CHECK(closed.isOk(), "analysis after closing overlay succeeds");
    if (closed) {
        const auto *dep =
            closed.value()->findModule(SourceCatalog::canonicalPath(workspace.path("dep.zithm")));
        CHECK(dep != nullptr, "disk dependency remains in snapshot after close");
        if (dep)
            CHECK(dep->source->text.find("changed") != std::string::npos,
                  "closing an overlay restores disk content");
    }
}

void test_partial_artifact_cycle_and_session_snapshot() {
    Workspace workspace;
    workspace.write("a.zithm", "from b\nfn a() { }\n");
    workspace.write("b.zithm", "from a\nfn b() { }\n");

    auto context = std::make_shared<FrontendContext>(workspace.config(2));
    auto cycle   = context->analyzeFile(workspace.path("a.zithm"));
    CHECK(cycle.isOk(), "cycle analysis returns a partial snapshot");
    if (cycle) {
        CHECK(cycle.value()->hasErrors(), "cycle diagnostic is recorded in snapshot");
        bool saw_cycle = false;
        for (const auto &diag : cycle.value()->diagnostics()) {
            if (diag.code == diagnostics::err::CircularImport &&
                diag.message.find("/tmp/zith-frontend-context-tests/a.zithm -> "
                                  "/tmp/zith-frontend-context-tests/b.zithm -> "
                                  "/tmp/zith-frontend-context-tests/a.zithm") != std::string::npos) {
                saw_cycle = true;
                break;
            }
        }
        CHECK(saw_cycle, "cycle diagnostic names the complete import path");
    }

    auto bad = context->analyzeText(workspace.path("broken.zithm"), "fn broken( {\n");
    CHECK(bad.isOk(), "syntax failure still produces a cacheable partial artifact");
    if (bad)
        CHECK(bad.value()->hasErrors(), "partial artifact retains syntax diagnostics");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = Stage::Scanned;
    CompilationSession session(options, workspace.path("a.zithm"), context);
    session.setBuffered(true);
    (void)session.runTo(Stage::Scanned);
    CHECK(session.snapshot() != nullptr, "session exposes shared immutable frontend snapshot");
}

void test_import_graph_and_resolution_table() {
    Workspace workspace;
    workspace.write("main.zithm", "from dep { public_fn as renamed }\n"
                                 "import package as namespace\n"
                                 "from assets/data.txt as Data\n"
                                 "fn main() { renamed() }\n");
    workspace.write("dep.zithm", "pub fn public_fn() { }\n");
    workspace.write("package.zithm", "pub fn exported() { }\n");
    workspace.write("assets/data.txt", "asset contents\n");

    auto config = workspace.config(1);
    config.assetRoots.push_back((workspace.root / "assets").string());
    FrontendContext context(std::move(config));
    auto result = context.analyzeFile(workspace.path("main.zithm"));
    CHECK(result.isOk(), "frontend import graph analysis succeeds");
    if (!result)
        return;

    const auto &snapshot = *result.value();
    CHECK_EQ(snapshot.importGraph().size(), 3u, "each root import has one graph edge");
    CHECK_EQ(snapshot.modules().size(), 3u, "asset dependency does not create a Zith module");
    CHECK(!snapshot.hasErrors(), "resolved imports and selectors have no diagnostics");

    bool saw_asset     = false;
    bool saw_selector  = false;
    bool saw_namespace = false;
    for (const auto &edge : snapshot.importGraph()) {
        if (edge.targetKind == ImportTargetKind::Asset)
            saw_asset = edge.request.alias == "Data" && edge.targets.size() == 1u;
        if (!edge.request.selectors.empty())
            saw_selector = edge.request.selectors[0].alias == "renamed";
        if (edge.request.alias == "namespace")
            saw_namespace = edge.targets.size() == 1u;
    }
    CHECK(saw_asset, "asset edge keeps its alias and canonical target");
    CHECK(saw_selector, "selector alias is represented by the graph request");
    CHECK(saw_namespace, "module alias edge has its canonical target");

    const auto *resolution =
        snapshot.findResolution(SourceCatalog::canonicalPath(workspace.path("main.zithm")));
    CHECK(resolution != nullptr, "root module has a resolution table");
    if (!resolution)
        return;
    bool has_renamed   = false;
    bool has_namespace = false;
    for (const auto &binding : resolution->bindings) {
        has_renamed |= binding.name == "renamed" && binding.kind == ResolutionKind::Import;
        has_namespace |= binding.name == "namespace" && binding.kind == ResolutionKind::ModuleAlias;
    }
    CHECK(has_renamed, "selective imported symbol resolves to its source module symbol");
    CHECK(has_namespace, "module alias resolves as a namespace binding");
}

void test_default_import_exposes_full_path_namespace() {
    Workspace workspace;
    workspace.write("string.zithm", "pub struct string { pub data: i32 }\n"
                                   "implement string {\n"
                                   "    pub fn make(): string { string { data: 1 } }\n"
                                   "}\n");
    workspace.write("main.zithm", "import string\n"
                                 "fn main() { return string.string.make().data; }\n");

    FrontendContext context(workspace.config(1));
    auto result = context.analyzeFile(workspace.path("main.zithm"));
    CHECK(result.isOk(), "frontend import graph analysis succeeds");
    if (!result)
        return;

    const auto &snapshot = *result.value();
    CHECK(!snapshot.hasErrors(), "colliding default import has no frontend diagnostics");
    const auto *resolution =
        snapshot.findResolution(SourceCatalog::canonicalPath(workspace.path("main.zithm")));
    CHECK(resolution != nullptr, "root module has a resolution table");
    if (!resolution)
        return;

    bool has_default_alias   = false;
    bool has_imported_symbol = false;
    for (const auto &binding : resolution->bindings) {
        if (binding.name == "string") {
            has_default_alias |= binding.kind == ResolutionKind::ModuleAlias;
        }
        if (binding.name == "make")
            has_imported_symbol |= binding.kind == ResolutionKind::Import;
    }
    CHECK(has_default_alias, "default import creates the first path segment as a module alias");
    CHECK(!has_imported_symbol, "default import does not inject symbols into the caller scope");
}

void test_export_shared_prefix_fans_out_to_every_submodule() {
    Workspace workspace;
    workspace.write("p/facade.zithm", "export p/t1\n"
                                     "export p/t2\n");
    workspace.write("p/t1.zithm", "pub struct TypeA { pub x: i32 }\n");
    workspace.write("p/t2.zithm", "pub struct TypeB { pub y: i32 }\n");
    workspace.write("main.zithm", "import p/facade\n"
                                 "fn main(): i32 {\n"
                                 "    let a: p.t1.TypeA = p.t1.TypeA { x: 1 };\n"
                                 "    let b: p.t2.TypeB = p.t2.TypeB { y: 2 };\n"
                                 "    a.x + b.y\n"
                                 "}\n");

    FrontendContext context(workspace.config(1));
    auto result = context.analyzeFile(workspace.path("main.zithm"));
    CHECK(result.isOk(), "facade export analysis succeeds");
    if (!result)
        return;
    CHECK(!result.value()->hasErrors(), "shared-prefix facade has no frontend diagnostics");

    const auto &snapshot = *result.value();
    const auto *resolution =
        snapshot.findResolution(SourceCatalog::canonicalPath(workspace.path("main.zithm")));
    CHECK(resolution != nullptr, "root module has a resolution table");
    if (resolution == nullptr)
        return;

    bool has_short_alias = false;
    bool has_deep_alias  = false;
    for (const auto &binding : resolution->bindings) {
        if (binding.kind != ResolutionKind::ModuleAlias || binding.modulePath.empty() ||
            binding.modulePath.front() != "p")
            continue;
        if (binding.modulePath == std::vector<std::string>{"p", "t1"})
            has_short_alias = true;
        if (binding.modulePath == std::vector<std::string>{"p", "t2"})
            has_deep_alias = true;
    }
    CHECK(has_short_alias, "facade keeps a per-fanout alias for the first subtree");
    CHECK(has_deep_alias, "facade keeps a per-fanout alias for the second subtree");

    for (const auto &binding : resolution->bindings) {
        if (binding.kind != ResolutionKind::ModuleAlias || binding.name != "p")
            continue;
        const auto t1_key = SourceCatalog::canonicalPath(workspace.path("p/t1.zithm"));
        const auto t2_key = SourceCatalog::canonicalPath(workspace.path("p/t2.zithm"));
        if (binding.modulePath == std::vector<std::string>{"p", "t1"})
            CHECK_EQ(binding.target.module, t1_key, "first fanout alias targets its own module");
        if (binding.modulePath == std::vector<std::string>{"p", "t2"})
            CHECK_EQ(binding.target.module, t2_key, "second fanout alias targets its own module");
    }

    const auto cached = context.analyzeFile(workspace.path("main.zithm"));
    CHECK(cached.isOk(), "repeated facade analysis succeeds from the populated cache");
    if (cached)
        CHECK(!cached.value()->hasErrors(),
              "shared-prefix facade still resolves after cache reuse");

    memory::Arena arena;
    Options options(arena);
    options.includeDirs.push(workspace.root.string());
    options.targetStage  = Stage::HirLowered;
    auto session_context = std::make_shared<FrontendContext>(workspace.config(1));
    CompilationSession session(options, workspace.path("main.zithm"), std::move(session_context));
    session.setBuffered(true);
    CHECK(session.runTo(Stage::HirLowered),
          "shared-prefix facade type expressions and struct literals lower to HIR");
    CHECK(session.diags().errorCount() == 0,
          "shared-prefix facade has no sema/lowering diagnostics");
}

void test_session_materializes_dependency_overlays() {
    Workspace workspace;
    workspace.write("main.zithm", "from dep\nfn main() { overlay_fn() }\n");
    workspace.write("dep.zithm", "pub fn disk_fn() { }\n");

    auto context = std::make_shared<FrontendContext>(workspace.config(1));
    context->setOverlay(workspace.path("dep.zithm"), "pub fn overlay_fn() { }\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = Stage::Imported;
    CompilationSession session(options, workspace.path("main.zithm"), context);
    session.setBuffered(true);
    CHECK(session.runTo(Stage::Imported), "session import stage accepts a frontend snapshot");
    CHECK(session.symbolTable().lookup("overlay_fn") != symbols::kInvalidSym,
          "legacy compatibility materializer consumes dependency overlay text");
    CHECK(session.symbolTable().lookup("disk_fn") == symbols::kInvalidSym,
          "legacy compatibility materializer does not reload the disk dependency");
}

size_t duplicateDeclCount(const CompilationSnapshot &snapshot) {
    size_t count = 0;
    for (const auto &diagnostic : snapshot.diagnostics()) {
        if (diagnostic.code == diagnostics::err::DuplicateDecl)
            ++count;
    }
    return count;
}

const ModuleResolution *rootResolution(const CompilationSnapshot &snapshot,
                                       const Workspace &workspace) {
    return snapshot.findResolution(SourceCatalog::canonicalPath(workspace.path("main.zithm")));
}

void test_parameter_names_are_scoped_per_function() {
    Workspace workspace;
    workspace.write("main.zithm",
                    "fn f(x: i32): i32 { return x; }\nfn g(x: i32): i32 { return x; }\n");

    FrontendContext context(workspace.config(1));
    auto result = context.analyzeFile(workspace.path("main.zithm"));
    CHECK(result.isOk(), "analysis of two functions with the same parameter name succeeds");
    if (!result)
        return;

    const auto &snapshot = *result.value();
    CHECK_EQ(duplicateDeclCount(snapshot), 0u,
             "same parameter name in two functions is not a duplicate declaration");

    const auto *resolution = rootResolution(snapshot, workspace);
    CHECK(resolution != nullptr, "root module has a resolution table");
    if (!resolution)
        return;
    std::vector<uint32_t> scopes;
    for (const auto &binding : resolution->bindings) {
        if (binding.name == "x")
            scopes.push_back(binding.scope.value);
    }
    CHECK_EQ(scopes.size(), 2u, "both parameters named 'x' are recorded as bindings");
    CHECK(scopes.size() == 2u && scopes[0] != scopes[1],
          "parameters named 'x' live in distinct scopes");
    CHECK(scopes.size() == 2u && scopes[0] != 0u && scopes[1] != 0u,
          "parameters are not placed in the module scope");
}

void test_local_bindings_are_scoped() {
    Workspace workspace;
    workspace.write("main.zithm", "fn f(): i32 { let v = 1\nreturn v; }\n"
                                 "fn g(): i32 { let v = 2\nreturn v; }\n");

    FrontendContext context(workspace.config(1));
    auto result = context.analyzeFile(workspace.path("main.zithm"));
    CHECK(result.isOk(), "analysis of the same local name in two functions succeeds");
    if (!result)
        return;
    CHECK_EQ(duplicateDeclCount(*result.value()), 0u,
             "the same local name in two functions is not a duplicate declaration");

    Workspace duplicate;
    duplicate.write("main.zithm", "fn f(): i32 { let v = 1\nlet v = 2\nreturn v; }\n");
    FrontendContext duplicate_context(duplicate.config(1));
    auto duplicate_result = duplicate_context.analyzeFile(duplicate.path("main.zithm"));
    CHECK(duplicate_result.isOk(), "analysis still produces a snapshot for duplicate locals");
    if (!duplicate_result)
        return;
    CHECK_EQ(duplicateDeclCount(*duplicate_result.value()), 1u,
             "two locals named 'v' in one block report exactly one duplicate declaration");
}

void test_nested_block_shadowing_is_allowed() {
    Workspace workspace;
    workspace.write("main.zithm", "fn f(): i32 { let v = 1\n{ let v = 2\n}\nreturn v; }\n");

    FrontendContext context(workspace.config(1));
    auto result = context.analyzeFile(workspace.path("main.zithm"));
    CHECK(result.isOk(), "analysis of a shadowing nested block succeeds");
    if (!result)
        return;
    CHECK_EQ(duplicateDeclCount(*result.value()), 0u,
             "shadowing a local in a nested block is allowed");
}

void test_parameter_shadowing_in_body_is_a_duplicate() {
    Workspace workspace;
    workspace.write("main.zithm", "fn f(x: i32): i32 { let x = 2\nreturn x; }\n");

    FrontendContext context(workspace.config(1));
    auto result = context.analyzeFile(workspace.path("main.zithm"));
    CHECK(result.isOk(), "analysis still produces a snapshot when a parameter is shadowed");
    if (!result)
        return;
    CHECK_EQ(duplicateDeclCount(*result.value()), 1u,
             "a body local may not shadow a parameter of the same function");
}

void test_lookup_binding_walks_module_fallback() {
    // Regression for the macro/scope wave: the internal helper must keep the
    // nearest-scope-first contract used by expanded macro names.
    Workspace workspace;
    workspace.write("main.zithm", "fn f(x: i32): i32 { x }\n");

    FrontendContext context(workspace.config(1));
    auto result = context.analyzeFile(workspace.path("main.zithm"));
    CHECK(result.isOk(), "analysis succeeds");
    if (!result)
        return;

    const auto *resolution = rootResolution(*result.value(), workspace);
    CHECK(resolution != nullptr, "root module has a resolution table");
    if (!resolution)
        return;

    const auto *module =
        result.value()->findModule(SourceCatalog::canonicalPath(workspace.path("main.zithm")));
    CHECK(module != nullptr, "root module artifact exists");
    if (!module || module->frontend == nullptr)
        return;

    const frontend::ScopeId first_scope =
        module->frontend->expressions()[module->frontend->declarations()[0].body.value - 1U].scope;
    const auto *bound = lookupBinding(*resolution, "x", first_scope, module->frontend->scopes());
    CHECK(bound != nullptr, "parameter resolves from the function body scope");
    if (bound)
        CHECK_EQ(bound->local.value, module->frontend->declarations()[0].parameters[0].id.value,
                 "lookupBinding finds the nearest local binding first");
}

void test_system_include_roots() {
    Workspace workspace;
    workspace.write("main.zithm", "import \"stdint.h\"\nfn main() { }\n");

    auto disabled_cfg                  = workspace.config(1);
    disabled_cfg.useSystemIncludeRoots = false;
    FrontendContext disabled(std::move(disabled_cfg));
    auto disabled_result = disabled.analyzeFile(workspace.path("main.zithm"));
    CHECK(disabled_result.isOk(), "analysis runs with system includes disabled");
    if (!disabled_result)
        return;
    bool saw_unresolved = false;
    for (const auto &diagnostic : disabled_result.value()->diagnostics())
        saw_unresolved |=
            diagnostic.message.find("could not find header 'stdint.h'") != std::string::npos &&
            diagnostic.message.find("configured include roots") != std::string::npos;
    CHECK(saw_unresolved, "system header import fails without system include roots");

    FrontendContext enabled(workspace.config(1));
    CHECK(enabled.cacheKey().identity() != disabled.cacheKey().identity(),
          "system include roots change the cache identity");
    CHECK(!enabled.cacheKey().systemIncludeRoots.empty(),
          "the context populates system include roots");
    CHECK(disabled.cacheKey().systemIncludeRoots.empty(),
          "disabling clears the system include roots");

#ifdef ZITH_ENABLE_C_INTEROP
    auto enabled_result = enabled.analyzeFile(workspace.path("main.zithm"));
    CHECK(enabled_result.isOk(), "analysis runs with system includes enabled");
    if (!enabled_result)
        return;
    CHECK(!enabled_result.value()->hasErrors(), "system header import resolves by default");
    bool saw_c_header = false;
    for (const auto &edge : enabled_result.value()->importGraph())
        saw_c_header |= edge.targetKind == ImportTargetKind::CHeader;
    CHECK(saw_c_header, "the resolved system header is a C header edge");
#endif
}

void test_workspace_header_shadows_system_header() {
    Workspace workspace;
    workspace.write("main.zithm", "import \"stdint.h\"\nfn main() { }\n");
    workspace.write("stdint.h", "int zith_local_marker(void);\n");

    FrontendContext context(workspace.config(1));
    auto result = context.analyzeFile(workspace.path("main.zithm"));
    CHECK(result.isOk(), "analysis with a workspace-local header succeeds");
    if (!result)
        return;

    const auto expected = SourceCatalog::canonicalPath(workspace.path("stdint.h"));
    bool shadowed       = false;
    for (const auto &edge : result.value()->importGraph()) {
        if (edge.targetKind != ImportTargetKind::CHeader)
            continue;
        for (const auto &target : edge.targets)
            shadowed |= target == expected;
    }
    CHECK(shadowed, "a workspace-local header shadows the system one");
}

void test_platform_import_resolution_order() {
    Workspace workspace;
    workspace.write("main.zithm", "from platform\nfn main() { }\n");
    workspace.write("platform.x86_64.linux.zithm", "pub fn exact() { }\n");
    workspace.write("platform.x86_64.zithm", "pub fn arch_only() { }\n");
    workspace.write("platform.linux.zithm", "pub fn os_only() { }\n");
    workspace.write("platform.zithm", "pub fn generic() { }\n");

    auto config         = workspace.config(1);
    config.targetTriple = "x86_64-unknown-linux-gnu";
    FrontendContext context(std::move(config));
    auto result = context.analyzeFile(workspace.path("main.zithm"));
    CHECK(result.isOk(), "platform import analysis succeeds");
    if (!result)
        return;

    bool saw_exact = false;
    for (const auto &edge : result.value()->importGraph()) {
        for (const auto &target : edge.targets)
            saw_exact |= target ==
                         SourceCatalog::canonicalPath(workspace.path("platform.x86_64.linux.zithm"));
    }
    CHECK(saw_exact, "exact arch.os variant wins over generic import");
    CHECK(!result.value()->hasErrors(), "resolved platform import has no diagnostics");
}

void test_platform_import_arch_only() {
    Workspace workspace;
    workspace.write("main.zithm", "from platform\nfn main() { }\n");
    workspace.write("platform.x86_64.zithm", "pub fn arch_only() { }\n");
    workspace.write("platform.linux.zithm", "pub fn os_only() { }\n");
    workspace.write("platform.zithm", "pub fn generic() { }\n");

    auto config         = workspace.config(1);
    config.targetTriple = "x86_64-unknown-linux-gnu";
    FrontendContext context(std::move(config));
    auto result = context.analyzeFile(workspace.path("main.zithm"));
    CHECK(result.isOk(), "arch-only platform import analysis succeeds");
    if (!result)
        return;

    bool saw_arch = false;
    bool saw_os   = false;
    for (const auto &edge : result.value()->importGraph()) {
        for (const auto &target : edge.targets) {
            saw_arch |=
                target == SourceCatalog::canonicalPath(workspace.path("platform.x86_64.zithm"));
            saw_os |= target == SourceCatalog::canonicalPath(workspace.path("platform.linux.zithm"));
        }
    }
    CHECK(saw_arch, "arch-only variant wins when exact arch.os is absent");
    CHECK(!saw_os, "os-only variant does not outrank arch-only");
}

void test_platform_import_os_only() {
    Workspace workspace;
    workspace.write("main.zithm", "from platform\nfn main() { }\n");
    workspace.write("platform.linux.zithm", "pub fn os_only() { }\n");
    workspace.write("platform.zithm", "pub fn generic() { }\n");

    auto config         = workspace.config(1);
    config.targetTriple = "x86_64-unknown-linux-gnu";
    FrontendContext context(std::move(config));
    auto result = context.analyzeFile(workspace.path("main.zithm"));
    CHECK(result.isOk(), "os-only platform import analysis succeeds");
    if (!result)
        return;

    bool saw_os = false;
    for (const auto &edge : result.value()->importGraph()) {
        for (const auto &target : edge.targets)
            saw_os |= target == SourceCatalog::canonicalPath(workspace.path("platform.linux.zithm"));
    }
    CHECK(saw_os, "os-only variant wins when arch variants are absent");
}

void test_platform_import_generic_fallback() {
    Workspace workspace;
    workspace.write("main.zithm", "from platform\nfn main() { }\n");
    workspace.write("platform.zithm", "pub fn generic() { }\n");

    auto config         = workspace.config(1);
    config.targetTriple = "aarch64-unknown-darwin";
    FrontendContext context(std::move(config));
    auto result = context.analyzeFile(workspace.path("main.zithm"));
    CHECK(result.isOk(), "generic platform import analysis succeeds");
    if (!result)
        return;

    bool saw_generic = false;
    for (const auto &edge : result.value()->importGraph()) {
        for (const auto &target : edge.targets)
            saw_generic |= target == SourceCatalog::canonicalPath(workspace.path("platform.zithm"));
    }
    CHECK(saw_generic, "generic module is chosen when no variant matches");
}

void test_platform_import_missing_diagnostic() {
    Workspace workspace;
    workspace.write("main.zithm", "from platform\nfn main() { }\n");

    auto config         = workspace.config(1);
    config.targetTriple = "x86_64-unknown-linux-gnu";
    FrontendContext context(std::move(config));
    auto result = context.analyzeFile(workspace.path("main.zithm"));
    CHECK(result.isOk(), "missing platform import still produces a snapshot");
    if (!result)
        return;

    bool saw_actionable = false;
    for (const auto &diagnostic : result.value()->diagnostics()) {
        saw_actionable |=
            diagnostic.message.find("generic module or a platform variant matching the target") !=
            std::string::npos;
    }
    CHECK(saw_actionable, "missing platform import explains which module variants to add");
}

void test_missing_module_import_diagnostic() {
    Workspace workspace;
    workspace.write("main.zithm", "from missing/module\nfn main() { }\n");

    FrontendContext context(workspace.config(1));
    auto result = context.analyzeFile(workspace.path("main.zithm"));
    CHECK(result.isOk(), "missing module import still produces a snapshot");
    if (!result)
        return;

    const auto &diagnostics = result.value()->diagnostics();
    CHECK_EQ(diagnostics.size(), 1u, "a missing module import produces one diagnostic");
    if (diagnostics.size() == 1U) {
        CHECK_EQ(diagnostics[0].code, diagnostics::err::ImportError,
                 "missing module import uses E1006");
        CHECK(diagnostics[0].message.find("could not find module 'missing/module'") !=
                  std::string::npos,
              "missing module diagnostic names the unresolved module");
        CHECK(diagnostics[0].message.find("configured import roots") != std::string::npos,
              "missing module diagnostic tells the user where to check resolution");
    }
}

void test_missing_header_and_asset_import_diagnostics() {
    Workspace workspace;
    workspace.write("main.zithm", "import \"missing.h\"\n"
                                 "from assets/missing.json as Missing\n"
                                 "fn main() { }\n");

    FrontendContext context(workspace.config(1));
    auto result = context.analyzeFile(workspace.path("main.zithm"));
    CHECK(result.isOk(), "missing header and asset imports still produce a snapshot");
    if (!result)
        return;

    const auto &diagnostics = result.value()->diagnostics();
    CHECK_EQ(diagnostics.size(), 2u, "missing header and asset each produce one diagnostic");
    bool saw_header = false;
    bool saw_asset  = false;
    for (const auto &diagnostic : diagnostics) {
        if (diagnostic.code != diagnostics::err::ImportError)
            continue;
        saw_header |=
            diagnostic.message.find("could not find header 'missing.h'") != std::string::npos &&
            diagnostic.message.find("configured include roots") != std::string::npos;
        saw_asset |= diagnostic.message.find("could not find asset 'assets/missing.json'") !=
                         std::string::npos &&
                     diagnostic.message.find("configured asset roots") != std::string::npos;
    }
    CHECK(saw_header, "missing C header diagnostic names include roots to check");
    CHECK(saw_asset, "missing asset diagnostic names asset roots to check");
}

void test_platform_import_cache_key_separation() {
    Workspace workspace;
    workspace.write("main.zithm", "from platform\n"
                                 "fn main(): i32 { platform_fn() }\n");
    workspace.write("platform.x86_64.linux.zithm", "pub fn platform_fn(): i32 { 11 }\n");
    workspace.write("platform.aarch64.darwin.zithm", "pub fn platform_fn(): i32 { 22 }\n");
    workspace.write("platform.zithm", "pub fn platform_fn(): i32 { 33 }\n");

    auto linux_config          = workspace.config(1);
    linux_config.targetTriple  = "x86_64-unknown-linux-gnu";
    auto darwin_config         = workspace.config(1);
    darwin_config.targetTriple = "aarch64-apple-darwin";

    FrontendContext linux_context(std::move(linux_config));
    auto linux_result = linux_context.analyzeFile(workspace.path("main.zithm"));
    CHECK(linux_result.isOk(), "linux-target platform import analysis succeeds");
    if (!linux_result)
        return;

    FrontendContext darwin_context(std::move(darwin_config));
    auto darwin_result = darwin_context.analyzeFile(workspace.path("main.zithm"));
    CHECK(darwin_result.isOk(), "darwin-target platform import analysis succeeds");
    if (!darwin_result)
        return;

    const auto linux_first_metrics  = linux_context.metrics().cache;
    const auto darwin_first_metrics = darwin_context.metrics().cache;
    (void)linux_context.analyzeFile(workspace.path("main.zithm"));
    (void)darwin_context.analyzeFile(workspace.path("main.zithm"));
    CHECK(linux_context.metrics().cache.hits > linux_first_metrics.hits,
          "linux context reuses its own target cache entries");
    CHECK(darwin_context.metrics().cache.hits > darwin_first_metrics.hits,
          "darwin context reuses its own target cache entries");

    bool saw_linux  = false;
    bool saw_darwin = false;
    for (const auto &edge : linux_result.value()->importGraph())
        for (const auto &target : edge.targets)
            saw_linux |= target ==
                         SourceCatalog::canonicalPath(workspace.path("platform.x86_64.linux.zithm"));
    for (const auto &edge : darwin_result.value()->importGraph())
        for (const auto &target : edge.targets)
            saw_darwin |= target == SourceCatalog::canonicalPath(
                                        workspace.path("platform.aarch64.darwin.zithm"));
    CHECK(saw_linux, "linux target resolves the linux platform variant");
    CHECK(saw_darwin, "darwin target resolves the darwin platform variant");
}

void test_syntax_errors_reach_the_diagnostic_engine() {
    Workspace workspace;
    workspace.write("main.zithm", "fn f( {\n");

    auto context = std::make_shared<FrontendContext>(workspace.config(1));
    memory::Arena arena;
    Options options(arena);
    options.targetStage = Stage::TypeChecked;
    CompilationSession session(options, workspace.path("main.zithm"), context);
    session.setBuffered(true);
    CHECK(!session.runTo(Stage::TypeChecked), "a syntax error fails the sema stage");

    size_t errors = 0;
    for (const auto &diagnostic : session.diags().all()) {
        if (diagnostic.severity == diagnostics::Severity::Error && !diagnostic.message.empty())
            ++errors;
    }
    CHECK(errors > 0u, "snapshot syntax errors are forwarded to the diagnostic engine");
}

} // namespace

static void test_frontend_context() {
    test_worker_counts_and_determinism();
    test_cache_invalidation_and_overlays();
    test_partial_artifact_cycle_and_session_snapshot();
    test_import_graph_and_resolution_table();
    test_default_import_exposes_full_path_namespace();
    test_export_shared_prefix_fans_out_to_every_submodule();
    test_session_materializes_dependency_overlays();
    test_parameter_names_are_scoped_per_function();
    test_local_bindings_are_scoped();
    test_nested_block_shadowing_is_allowed();
    test_parameter_shadowing_in_body_is_a_duplicate();
    test_lookup_binding_walks_module_fallback();
    test_syntax_errors_reach_the_diagnostic_engine();
    test_system_include_roots();
    test_workspace_header_shadows_system_header();
    test_platform_import_resolution_order();
    test_platform_import_arch_only();
    test_platform_import_os_only();
    test_platform_import_generic_fallback();
    test_platform_import_missing_diagnostic();
    test_missing_module_import_diagnostic();
    test_missing_header_and_asset_import_diagnostics();
    test_platform_import_cache_key_separation();
}

TEST_MAIN(frontend_context)
