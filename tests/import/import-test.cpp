#include "common/import/import-graph.hpp"

#include <cstdio>
#include <cstdlib>
#include <string_view>

using zith::import::ImportGraph;
using zith::import::Module;
using zith::symbols::SymId;
using zith::symbols::SymKind;
using zith::symbols::SymbolVisibility;
using zith::symbols::SymbolVisibilityKind;

namespace {

bool check(bool ok, const char *msg) {
    if (!ok)
        std::fprintf(stderr, "FAIL: %s\n", msg);
    return ok;
}

} // namespace

int main() {
    bool ok = true;

    // --- Linear DAG: app -> svc -> data ---
    {
        common::memory::Arena arena;
        common::memory::StringInterner interner{arena};
        ImportGraph graph{arena, interner};

        auto &app  = *graph.addModule("app");
        auto &svc  = *graph.addModule("svc");
        auto &data = *graph.addModule("data");

        graph.addDependency(app, svc);
        graph.addDependency(svc, data);

        const auto result = graph.finalize();
        ok &= check(result.isOk(), "linear DAG finalize succeeds");

        ok &= check(graph.isAncestor(app, data), "app is ancestor of data");
        ok &= check(graph.isAncestor(app, svc), "app is ancestor of svc");
        ok &= check(graph.isAncestor(svc, data), "svc is ancestor of data");
        ok &= check(!graph.isAncestor(data, app), "data is not ancestor of app");
        ok &= check(!graph.isAncestor(svc, app), "svc is not ancestor of app");

        ok &= check(graph.distance(app, data) == 2, "distance app->data is 2");
        ok &= check(graph.distance(app, svc) == 1, "distance app->svc is 1");
        ok &= check(graph.distance(svc, data) == 1, "distance svc->data is 1");
        ok &= check(graph.distance(data, app) == -1, "distance data->app is -1");

        ok &= check(graph.depthOf(app) == 2, "depthOf(app) == 2");
        ok &= check(graph.depthOf(svc) == 1, "depthOf(svc) == 1");
        ok &= check(graph.depthOf(data) == 0, "depthOf(data) == 0");
    }

    // --- Diamond DAG ---
    {
        common::memory::Arena arena;
        common::memory::StringInterner interner{arena};
        ImportGraph graph{arena, interner};

        auto &a = *graph.addModule("a");
        auto &b = *graph.addModule("b");
        auto &c = *graph.addModule("c");
        auto &d = *graph.addModule("d");

        graph.addDependency(a, b);
        graph.addDependency(a, c);
        graph.addDependency(b, d);
        graph.addDependency(c, d);

        const auto result = graph.finalize();
        ok &= check(result.isOk(), "diamond DAG finalize succeeds");

        ok &= check(graph.isAncestor(a, d), "a is ancestor of d");
        ok &= check(graph.isAncestor(a, b), "a is ancestor of b");
        ok &= check(graph.isAncestor(a, c), "a is ancestor of c");
        ok &= check(graph.distance(a, d) == 2, "distance a->d is 2");

        ok &= check(!graph.isAncestor(b, c), "b is not ancestor of c (parallel)");
        ok &= check(!graph.isAncestor(c, b), "c is not ancestor of b (parallel)");
        ok &= check(graph.distance(b, c) == -1, "distance b->c is -1 (unrelated)");
    }

    // --- Cycle detection ---
    {
        common::memory::Arena arena;
        common::memory::StringInterner interner{arena};
        ImportGraph graph{arena, interner};

        auto &a = *graph.addModule("a");
        auto &b = *graph.addModule("b");
        auto &c = *graph.addModule("c");

        graph.addDependency(a, b);
        graph.addDependency(b, c);
        graph.addDependency(c, a);

        const auto result = graph.finalize();
        ok &= check(result.isError(), "cycle rejected");
        ok &= check(result.error().msg == "circular import",
                    "cycle error message is 'circular import'");
    }

    // --- Unrelated modules ---
    {
        common::memory::Arena arena;
        common::memory::StringInterner interner{arena};
        ImportGraph graph{arena, interner};

        auto &a = *graph.addModule("a");
        auto &b = *graph.addModule("b");
        auto &c = *graph.addModule("c");
        auto &d = *graph.addModule("d");

        graph.addDependency(a, b);
        graph.addDependency(c, d);

        const auto result = graph.finalize();
        ok &= check(result.isOk(), "disconnected DAG finalize succeeds");

        ok &= check(!graph.isAncestor(a, d), "a not ancestor of d (unrelated)");
        ok &= check(!graph.isAncestor(c, b), "c not ancestor of b (unrelated)");
        ok &= check(graph.distance(a, d) == -1, "distance a->d is -1 (unrelated)");
    }

    // --- Double finalize is no-op ---
    {
        common::memory::Arena arena;
        common::memory::StringInterner interner{arena};
        ImportGraph graph{arena, interner};

        auto &a = *graph.addModule("a");
        auto &b = *graph.addModule("b");
        graph.addDependency(a, b);

        auto r1 = graph.finalize();
        ok &= check(r1.isOk(), "first finalize succeeds");

        auto r2 = graph.finalize();
        ok &= check(r2.isOk(), "second finalize succeeds (no-op)");
    }

    // --- beginResolve / isLoaded ---
    {
        common::memory::Arena arena;
        common::memory::StringInterner interner{arena};
        ImportGraph graph{arena, interner};

        auto &mod = *graph.addModule("loader");
        ok &= check(!graph.isLoaded(mod), "module not loaded initially");

        auto guard = graph.beginResolve(mod);
        ok &= check(guard.isOk(), "beginResolve succeeds");
        ok &= check(guard.value().valid(), "guard is valid");
        ok &= check(&guard.value().module == &mod, "guard exposes module reference");

        auto guard2 = graph.beginResolve(mod);
        ok &= check(guard2.isError(), "beginResolve again fails (already resolving)");
    }

    // --- nodeCount tracks created modules ---
    {
        common::memory::Arena arena;
        common::memory::StringInterner interner{arena};
        ImportGraph graph{arena, interner};

        ok &= check(graph.nodeCount() == 0, "nodeCount starts at 0");
        (void)graph.addModule("m0");
        (void)graph.addModule();
        (void)graph.addModule("m2");
        ok &= check(graph.nodeCount() == 3, "nodeCount grows with addModule");
    }

    // --- Named add/lookup and duplicate handling ---
    {
        common::memory::Arena arena;
        common::memory::StringInterner interner{arena};
        ImportGraph graph{arena, interner};

        auto added = graph.addModule("mod");
        ok &= check(added.isValid(), "addModule(name) returns module");
        ok &= check(added->name() == "mod", "module has the requested name");

        auto duplicate = graph.addModule("mod");
        ok &= check(duplicate.isEmpty(), "duplicate module name is rejected");

        auto found = graph.lookupModule("mod");
        ok &= check(found.isValid() && &found.value() == &added.value(),
                    "lookupModule finds named module");

        auto missing = graph.lookupModule("nope");
        ok &= check(missing.isEmpty(), "lookupModule misses unknown name");
    }

    // --- Unnamed modules cannot be looked up and always create new nodes ---
    {
        common::memory::Arena arena;
        common::memory::StringInterner interner{arena};
        ImportGraph graph{arena, interner};

        auto &m0 = *graph.addModule();
        auto &m1 = *graph.addModule();
        ok &= check(&m0 != &m1, "unnamed addModule always creates a new module");
        ok &= check(m0.name().empty() && m1.name().empty(),
                    "unnamed modules have empty names");
        ok &= check(graph.lookupModule("").isEmpty(),
                    "empty name does not match unnamed module");
    }

    // --- Module: declare + lookup ---
    {
        common::memory::Arena arena;
        common::memory::StringInterner interner{arena};
        ImportGraph graph{arena, interner};

        auto &mod = *graph.addModule("symbols");
        const SymId id = mod.declare("x", SymKind::Variable);
        ok &= check(id.local == 0, "SymId.local == 0");

        const auto found = mod.lookup("x");
        ok &= check(found.isValid() && found->local == 0, "lookup finds declared symbol");

        const auto foundLocal = mod.lookupLocal("x");
        ok &= check(foundLocal.isValid() && foundLocal->local == 0,
                    "lookupLocal finds declared symbol");

        ok &= check(mod.symbolCount() == 1, "symbolCount is 1 after one declare");
    }

    // --- Module: forEachLocal ---
    {
        common::memory::Arena arena;
        common::memory::StringInterner interner{arena};
        ImportGraph graph{arena, interner};

        auto &mod = *graph.addModule();
        (void)mod.declare("a", SymKind::Variable);
        (void)mod.declare("b", SymKind::Fn);

        int count = 0;
        mod.forEachLocal([&](SymId, const Module::SymbolData &) { count++; });
        ok &= check(count == 2, "forEachLocal visits 2 symbols");
    }

    // --- Module: forEachAll with visibility ---
    {
        common::memory::Arena arena;
        common::memory::StringInterner interner{arena};
        ImportGraph graph{arena, interner};

        auto &a = *graph.addModule("a");
        auto &b = *graph.addModule("b");
        graph.addDependency(a, b);
        (void)graph.finalize();

        SymbolVisibility pubVis;
        pubVis.kind = SymbolVisibilityKind::Public;
        (void)b.declare("pub_x", SymKind::Variable, pubVis);

        SymbolVisibility privVis;
        privVis.kind = SymbolVisibilityKind::Private;
        (void)b.declare("priv_y", SymKind::Fn, privVis);

        int count = 0;
        a.forEachAll([&](SymId, const Module::SymbolData &) { count++; });
        ok &= check(count == 1, "forEachAll sees only pub_x (not priv_y)");
    }

    // --- Module: forEachAll with Module visibility range ---
    {
        common::memory::Arena arena;
        common::memory::StringInterner interner{arena};
        ImportGraph graph{arena, interner};

        auto &a = *graph.addModule("a");
        auto &b = *graph.addModule("b");
        auto &c = *graph.addModule("c");
        graph.addDependency(a, b);
        graph.addDependency(b, c);
        (void)graph.finalize();

        SymbolVisibility modVis;
        modVis.kind = SymbolVisibilityKind::Module;
        modVis.range.ancestors = 1;
        (void)c.declare("mod_z", SymKind::Variable, modVis);

        int countA = 0;
        a.forEachAll([&](SymId, const Module::SymbolData &) { countA++; });
        ok &= check(countA == 0, "mod_z not visible from a (ancestors=1, distance=2)");

        int countB = 0;
        b.forEachAll([&](SymId, const Module::SymbolData &) { countB++; });
        ok &= check(countB == 1, "mod_z visible from b (ancestors=1, distance=1)");
    }

    // --- Module: forEachAll shadowing ---
    {
        common::memory::Arena arena;
        common::memory::StringInterner interner{arena};
        ImportGraph graph{arena, interner};

        auto &a = *graph.addModule("a");
        auto &b = *graph.addModule("b");
        auto &c = *graph.addModule("c");
        graph.addDependency(a, b);
        graph.addDependency(a, c);
        (void)graph.finalize();

        SymbolVisibility pubVis;
        pubVis.kind = SymbolVisibilityKind::Public;
        (void)b.declare("x", SymKind::Variable, pubVis);
        (void)c.declare("x", SymKind::Fn, pubVis);

        int count = 0;
        a.forEachAll([&](SymId, const Module::SymbolData &) { count++; });
        ok &= check(count == 1, "shadowed x: only one x visible via forEachAll");
    }

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
