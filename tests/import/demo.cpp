#include "common/import/import-graph.hpp"

#include <cstdio>
#include <cstdlib>

using zith::import::ImportGraph;
using zith::import::Module;
using zith::symbols::SymKind;
using zith::symbols::SymbolVisibility;
using zith::symbols::SymbolVisibilityKind;

int main() {
    common::memory::Arena arena;
    common::memory::StringInterner interner{arena};
    ImportGraph graph{arena, interner};

    auto &vendor  = *graph.addModule("vendor");
    auto &service = *graph.addModule("service");
    auto &app     = *graph.addModule("app");

    graph.addDependency(app, service);
    graph.addDependency(app, vendor);
    (void)graph.finalize();

    SymbolVisibility pub{};
    pub.kind = SymbolVisibilityKind::Public;
    (void)vendor.declare("log", SymKind::Fn, pub);

    SymbolVisibility moduleVis{};
    moduleVis.kind = SymbolVisibilityKind::Module;
    moduleVis.range.ancestors = 1;
    (void)vendor.declare("format", SymKind::Fn, moduleVis);

    SymbolVisibility priv{};
    priv.kind = SymbolVisibilityKind::Private;
    (void)service.declare("internal_db", SymKind::Variable, priv);
    (void)service.declare("query", SymKind::Fn, pub);

    app.enterScope();
    (void)app.declare("main", SymKind::Fn, pub);
    app.exitScope();

    std::printf("app module: name=%.*s symbols=%zu scopes=%zu\n",
                static_cast<int>(app.name().size()), app.name().data(),
                app.symbolCount(), app.scopeCount());

    for (auto *mod : {&vendor, &service, &app}) {
        std::printf("\nlocal symbols in module %.*s:\n",
                    static_cast<int>(mod->name().size()), mod->name().data());
        mod->forEachLocal([&](zith::symbols::SymId id, const Module::SymbolData &data) {
            std::printf("  sym[%u.%u] %.*s kind=%s visibility=%s\n",
                        id.module, id.local,
                        static_cast<int>(interner.lookup(data.name).size()),
                        interner.lookup(data.name).data(),
                        zith::symbols::symKindName(data.kind),
                        zith::symbols::visibilityKindName(data.visibility.kind));
        });
    }

    std::printf("\napp visible symbols (forEachAll):\n");
    app.forEachAll([&](zith::symbols::SymId id, const Module::SymbolData &data) {
        std::printf("  %.*s from module %u (%s)\n",
                    static_cast<int>(interner.lookup(data.name).size()),
                    interner.lookup(data.name).data(),
                    id.module,
                    zith::symbols::visibilityKindName(data.visibility.kind));
    });

    std::printf("\napp.lookupLocal(\"query\") -> %s\n",
                app.lookupLocal("query").isValid() ? "found" : "not found locally");

    std::printf("app resolved query via forEachAll:\n");
    app.forEachAll([&](zith::symbols::SymId id, const Module::SymbolData &data) {
        if (data.name == interner.findId("query").value()) {
            std::printf("  module %u local %u\n", id.module, id.local);
        }
    });

    return EXIT_SUCCESS;
}
