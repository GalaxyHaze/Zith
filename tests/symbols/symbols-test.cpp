#include "common/import/import-graph.hpp"
#include "symbols/symbols.hpp"

#include <cstdio>
#include <cstdlib>
#include <string_view>

using zith::import::ImportGraph;
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

    // --- SymKind ---
    ok &= check(static_cast<uint8_t>(SymKind::Fn) == 0, "SymKind::Fn == 0");
    ok &= check(static_cast<uint8_t>(SymKind::Context) == 12, "SymKind::Context == 12");
    ok &= check(zith::symbols::kSymKindCount == 13, "kSymKindCount == 13");
    ok &= check(std::string_view(zith::symbols::symKindName(SymKind::Fn)) == "Fn",
                "symKindName(Fn)");
    ok &= check(std::string_view(zith::symbols::symKindName(SymKind::Module)) == "Module",
                "symKindName(Module)");

    // --- Generated symbol helpers ---
    {
        common::memory::Arena arena;
        common::memory::StringInterner interner{arena};

        const auto name = interner.intern("factory");
        SymbolVisibility pub;
        pub.kind = SymbolVisibilityKind::Public;
        const auto data = zith::symbols::makeSymbol(arena, name, SymKind::Fn, pub);

        ok &= check(data.name == name, "makeSymbol stores interned name");
        ok &= check(data.kind == SymKind::Fn, "makeSymbol stores kind");
        ok &= check(data.visibility.kind == SymbolVisibilityKind::Public,
                    "makeSymbol stores visibility");
        ok &= check(std::string_view(zith::symbols::visibilityName(data.visibility)) == "Public",
                    "visibilityName returns Public");
        ok &= check(std::string_view(zith::symbols::visibilityKindName(SymbolVisibilityKind::Module))
                        == "Module",
                    "visibilityKindName returns Module");

        interner.findId("factory");
        ok &= check(interner.findId("factory").isValid(), "findId finds interned name");
        ok &= check(interner.findId("missing").isEmpty(), "findId misses unknown name");
    }

    // --- Declare + lookup ---
    {
        common::memory::Arena arena;
        common::memory::StringInterner interner{arena};
        ImportGraph graph{arena, interner};
        auto &mod = *graph.addModule("syms");

        const SymId idA = mod.declare("a", SymKind::Variable);
        ok &= check(idA.local == 0, "first declare gets local 0");
        ok &= check(mod.lookup("a").isValid(), "lookup finds 'a'");

        const SymId idB = mod.declare("b", SymKind::Fn);
        ok &= check(idB.local == 1, "second declare gets local 1");

        const auto foundA = mod.lookup("a");
        ok &= check(foundA.isValid(), "lookup finds 'a'");
        ok &= check(foundA->local == 0, "lookup returns correct index for 'a'");

        const auto foundB = mod.lookup("b");
        ok &= check(foundB.isValid(), "lookup finds 'b'");
        ok &= check(foundB->local == 1, "lookup returns correct index for 'b'");

        const auto missing = mod.lookup("c");
        ok &= check(missing.isEmpty(), "lookup does not find 'c'");
    }

    // --- Nested scopes + shadowing ---
    {
        common::memory::Arena arena;
        common::memory::StringInterner interner{arena};
        ImportGraph graph{arena, interner};
        auto &mod = *graph.addModule();

        (void)mod.declare("x", SymKind::Variable); // scope 0, sym 0

        mod.enterScope();
        (void)mod.declare("x", SymKind::Alias);    // scope 1, sym 1 (shadows)

        const auto found = mod.lookup("x");
        ok &= check(found.isValid() && found->local == 1,
                    "lookup finds inner shadow");

        mod.exitScope();
        const auto afterExit = mod.lookup("x");
        ok &= check(afterExit.isValid() && afterExit->local == 0,
                    "after exitScope, lookup finds outer");
    }

    // --- lookupAll ---
    {
        common::memory::Arena arena;
        common::memory::StringInterner interner{arena};
        ImportGraph graph{arena, interner};
        auto &mod = *graph.addModule();

        (void)mod.declare("h", SymKind::Variable); // scope 0

        mod.enterScope();
        (void)mod.declare("h", SymKind::Struct);   // scope 1

        mod.enterScope();
        (void)mod.declare("h", SymKind::Trait);    // scope 2

        auto all = mod.lookupAll("h");
        ok &= check(all.size() == 3, "lookupAll finds 3 entries");
        ok &= check(all[0].local == 0, "lookupAll[0] = scope 0");
        ok &= check(all[1].local == 1, "lookupAll[1] = scope 1");
        ok &= check(all[2].local == 2, "lookupAll[2] = scope 2");
    }

    // --- get + counts ---
    {
        common::memory::Arena arena;
        common::memory::StringInterner interner{arena};
        ImportGraph graph{arena, interner};
        auto &mod = *graph.addModule();

        const SymId id = mod.declare("v", SymKind::Variable);

        auto &data = mod.get(id);
        ok &= check(data.name == interner.findId("v").value(), "get returns correct name");
        ok &= check(data.kind == SymKind::Variable, "get returns correct kind");

        ok &= check(mod.scopeCount() == 1, "starts with one scope");
        mod.enterScope();
        ok &= check(mod.scopeCount() == 2, "enterScope increases count");
        mod.exitScope();
        ok &= check(mod.scopeCount() == 1, "exitScope decreases count");
    }

    // --- Defaults and invalid ids ---
    {
        zith::symbols::ModuleVisibilityRange r;
        ok &= check(r.ancestors == -1, "default ancestors == -1");
        ok &= check(r.descendants == -1, "default descendants == -1");

        SymbolVisibility v;
        ok &= check(v.kind == SymbolVisibilityKind::Private, "default visibility is Private");

        ok &= check(zith::symbols::kInvalidModule == ~0u, "kInvalidModule is ~0u");
        ok &= check(zith::symbols::kInvalidSymId.local == 0, "kInvalidSymId.local == 0");
        ok &= check(zith::symbols::kInvalidSymId.module == zith::symbols::kInvalidModule,
                    "kInvalidSymId.module == kInvalidModule");
    }

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
