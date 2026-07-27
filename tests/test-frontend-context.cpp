#include "cli/options.hpp"
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
        std::ofstream output(root / name, std::ios::binary | std::ios::trunc);
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
    workspace.write("main.zith", "from dep\nfn main() { dep_fn() }\n");
    workspace.write("dep.zith", "fn dep_fn() { }\n");

    FrontendContext serial(workspace.config(1));
    auto serial_result = serial.analyzeFile(workspace.path("main.zith"));
    CHECK(serial_result.isOk(), "serial frontend analysis succeeds");
    if (!serial_result)
        return;

    FrontendContext parallel(workspace.config(2));
    auto parallel_result = parallel.analyzeFile(workspace.path("main.zith"));
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
    workspace.write("main.zith", "from dep\nfn main() { dep_fn() }\n");
    workspace.write("dep.zith", "fn dep_fn() { }\n");

    FrontendContext context(workspace.config(2));
    auto first = context.analyzeFile(workspace.path("main.zith"));
    CHECK(first.isOk(), "initial cached analysis succeeds");
    if (!first)
        return;
    const auto first_metrics = context.metrics().cache;

    auto second = context.analyzeFile(workspace.path("main.zith"));
    CHECK(second.isOk(), "repeated analysis succeeds");
    CHECK(context.metrics().cache.hits > first_metrics.hits,
          "repeated revision reuses module artifacts");

    workspace.write("dep.zith", "fn dep_fn() { }\nfn changed() { }\n");
    context.invalidatePath(workspace.path("dep.zith"));
    auto changed = context.analyzeFile(workspace.path("main.zith"));
    CHECK(changed.isOk(), "transitively invalidated analysis succeeds");
    CHECK(context.metrics().cache.invalidated >= 2u,
          "dependency invalidation removes the dependency and its root dependent");

    context.setOverlay(workspace.path("dep.zith"), "fn overlay() { }\n");
    auto overlay = context.analyzeFile(workspace.path("main.zith"));
    CHECK(overlay.isOk(), "open-document overlay analysis succeeds");
    if (overlay) {
        const auto *dep =
            overlay.value()->findModule(SourceCatalog::canonicalPath(workspace.path("dep.zith")));
        CHECK(dep != nullptr, "overlay dependency is in snapshot");
        if (dep)
            CHECK(dep->source->text.find("overlay") != std::string::npos,
                  "open document content takes precedence over disk");
    }

    context.removeOverlay(workspace.path("dep.zith"));
    auto closed = context.analyzeFile(workspace.path("main.zith"));
    CHECK(closed.isOk(), "analysis after closing overlay succeeds");
    if (closed) {
        const auto *dep =
            closed.value()->findModule(SourceCatalog::canonicalPath(workspace.path("dep.zith")));
        CHECK(dep != nullptr, "disk dependency remains in snapshot after close");
        if (dep)
            CHECK(dep->source->text.find("changed") != std::string::npos,
                  "closing an overlay restores disk content");
    }
}

void test_partial_artifact_cycle_and_session_snapshot() {
    Workspace workspace;
    workspace.write("a.zith", "from b\nfn a() { }\n");
    workspace.write("b.zith", "from a\nfn b() { }\n");

    auto context = std::make_shared<FrontendContext>(workspace.config(2));
    auto cycle   = context->analyzeFile(workspace.path("a.zith"));
    CHECK(cycle.isOk(), "cycle analysis returns a partial snapshot");
    if (cycle)
        CHECK(cycle.value()->hasErrors(), "cycle diagnostic is recorded in snapshot");

    auto bad = context->analyzeText(workspace.path("broken.zith"), "fn broken( {\n");
    CHECK(bad.isOk(), "syntax failure still produces a cacheable partial artifact");
    if (bad)
        CHECK(bad.value()->hasErrors(), "partial artifact retains syntax diagnostics");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = Stage::Scanned;
    CompilationSession session(options, workspace.path("a.zith"), context);
    session.setBuffered(true);
    (void)session.runTo(Stage::Scanned);
    CHECK(session.snapshot() != nullptr, "session exposes shared immutable frontend snapshot");
}

} // namespace

static void test_frontend_context() {
    test_worker_counts_and_determinism();
    test_cache_invalidation_and_overlays();
    test_partial_artifact_cycle_and_session_snapshot();
}

TEST_MAIN(frontend_context)
