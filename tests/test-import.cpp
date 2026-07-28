#include "diagnostics/diagnostic-engine.hpp"
#include "diagnostics/error-codes.hpp"
#include "legacy-zith/ast/ast-builder.hpp"
#include "legacy-zith/lexer/lexer.hpp"
#include "legacy-zith/parser/parser.hpp"
#include "memory/arena.hpp"
#include "memory/source-map.hpp"
#include "memory/string-interner.hpp"
#include "symbols/import-manager.hpp"
#include "symbols/import-resolver.hpp"
#include "symbols/symbol-table.hpp"
#include "test-common.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>

using namespace zith;

#ifndef CHECK_GE
#define CHECK_GE(a, b, msg) CHECK((a) >= (b), msg)
#endif

struct ImportTest {
    memory::Arena arena;
    memory::StringInterner interner;
    memory::SourceMap sourceMap;
    diagnostics::DiagnosticEngine diags;

    ImportTest() : interner(arena), diags(arena) {}

    struct Result {
        ast::AstBuilder *builder;
        ast::ProgramNode *program;
        bool ok;
        size_t errorCount;
    };

    Result scan(std::string_view input) {
        auto addResult = sourceMap.addFile("test.zith", input);
        if (!addResult)
            return {nullptr, nullptr, false, 0};
        auto fileId      = addResult.value();
        auto tokenResult = lexer::tokenize(sourceMap, arena, fileId, diags);
        if (!tokenResult) {
            size_t errs = 0;
            for (auto &d : diags.all())
                if (d.severity == diagnostics::Severity::Error)
                    errs++;
            return {nullptr, nullptr, false, errs};
        }
        lexer::TokenStream tokens = std::move(tokenResult.value());
        auto *builder             = arena.make<ast::AstBuilder>(arena, interner);
        auto *prog                = arena.make<ast::ProgramNode>(arena);
        symbols::SymbolTable syms(arena, &interner);
        parser::Parser parser(&tokens, builder, &diags);
        parser::scan(parser, syms);
        *prog       = std::move(parser.program);
        size_t errs = 0;
        for (auto &d : diags.all())
            if (d.severity == diagnostics::Severity::Error)
                errs++;
        return {builder, prog, errs == 0, errs};
    }
};

// ── Helpers ──────────────────────────────────────────────────────────

static int count_decls(const ast::ProgramNode &prog) {
    int n = 0;
    for (auto id : prog.decls)
        if (id != ast::kInvalidDecl)
            n++;
    return n;
}

static const ast::ImportNode *get_import(const ast::ProgramNode &prog, ast::AstBuilder *bld,
                                         int idx) {
    int n = 0;
    for (auto id : prog.decls) {
        if (id == ast::kInvalidDecl)
            continue;
        auto &decl = bld->getDecl(id);
        auto *imp  = std::get_if<ast::ImportNode>(&decl);
        if (!imp)
            continue;
        if (n++ == idx)
            return imp;
    }
    return nullptr;
}

struct ImportIntegrationTest {
    memory::Arena arena;
    memory::StringInterner interner;
    memory::SourceMap source_map;
    diagnostics::DiagnosticEngine diags;
    std::filesystem::path root;
    symbols::ImportManager imports;

    ImportIntegrationTest()
        : interner(arena), diags(arena), root(makeRoot()),
          imports(arena, interner, source_map, diags, {root.string()}) {}

    ~ImportIntegrationTest() {
        std::filesystem::remove_all(root);
    }

    static std::filesystem::path makeRoot() {
        auto root = std::filesystem::temp_directory_path() / "zith_import_integration";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        return root;
    }

    void write(std::string_view relative_path, std::string_view content) const {
        auto path = root / relative_path;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path);
        output << content;
    }

    memory::DynArray<std::string_view> path(std::initializer_list<std::string_view> segments) {
        memory::DynArray<std::string_view> result(arena);
        for (auto segment : segments)
            result.push(segment);
        return result;
    }

    memory::DynArray<ast::ImportSymbol>
    symbols(std::initializer_list<ast::ImportSymbol> requested = {}) {
        memory::DynArray<ast::ImportSymbol> result(arena);
        for (auto symbol : requested)
            result.push(symbol);
        return result;
    }

    bool hasError(diagnostics::ErrCode code, std::string_view message = {}) const {
        for (const auto &diag : diags.all()) {
            if (diag.severity != diagnostics::Severity::Error || diag.code != code)
                continue;
            if (message.empty() || diag.message.find(message) != std::string::npos)
                return true;
        }
        return false;
    }
};

static auto resolve(ImportIntegrationTest &test, std::initializer_list<std::string_view> segments,
                    bool is_from = false, bool is_export = false,
                    std::initializer_list<ast::ImportSymbol> requested = {},
                    std::string_view source_file = {}) -> memory::Result<size_t> {
    auto path    = test.path(segments);
    auto symbols = test.symbols(requested);
    return test.imports.resolve(path, symbols, is_from, is_export, false, {}, 1, source_file);
}

// ── Tests ────────────────────────────────────────────────────────────

static void test_from_simple() {
    ImportTest t;
    auto r = t.scan("from std/io/console");
    CHECK(r.ok, "from std/io/console scan succeeds");
    CHECK_EQ(count_decls(*r.program), 1, "1 decl");
    auto *imp = get_import(*r.program, r.builder, 0);
    CHECK(imp != nullptr, "import node exists");
    CHECK(imp->is_from, "is_from is true");
    CHECK(!imp->is_export, "is_export is false");
    CHECK(!imp->is_asset, "is_asset is false");
    CHECK(imp->alias.empty(), "no alias");
    CHECK_EQ(imp->path.size(), 3u, "3 path segments");
    CHECK(imp->symbols.empty(), "no selective symbols");
}

static void test_import_simple() {
    ImportTest t;
    auto r = t.scan("import std/collections/map");
    CHECK(r.ok, "import std/collections/map scan succeeds");
    auto *imp = get_import(*r.program, r.builder, 0);
    CHECK(imp != nullptr, "import node exists");
    CHECK(!imp->is_from, "is_from is false");
    CHECK(!imp->is_export, "is_export is false");
    CHECK(imp->alias.empty(), "no alias");
    CHECK_EQ(imp->path.size(), 3u, "3 path segments");
}

static void test_export_behaves_like_import() {
    ImportTest t;
    auto r = t.scan("export std/io/console");
    CHECK(r.ok, "export std/io/console scan succeeds");
    auto *imp = get_import(*r.program, r.builder, 0);
    CHECK(imp != nullptr, "import node exists");
    CHECK(!imp->is_from, "export has is_from = false");
    CHECK(imp->is_export, "export has is_export = true");
}

static void test_from_with_alias() {
    ImportTest t;
    auto r = t.scan("from std/io/console as con");
    CHECK(r.ok, "from ... as alias scan succeeds");
    auto *imp = get_import(*r.program, r.builder, 0);
    CHECK(imp != nullptr, "import node exists");
    CHECK(imp->is_from, "is_from is true");
    CHECK_EQ(imp->alias, "con", "alias is 'con'");
}

static void test_import_with_alias() {
    ImportTest t;
    auto r = t.scan("import std/collections/map as map_mod");
    CHECK(r.ok, "import ... as alias scan succeeds");
    auto *imp = get_import(*r.program, r.builder, 0);
    CHECK(imp != nullptr, "import node exists");
    CHECK(!imp->is_from, "is_from is false");
    CHECK_EQ(imp->alias, "map_mod", "alias is 'map_mod'");
}

static void test_from_with_selective() {
    ImportTest t;
    auto r = t.scan("from std/io/console { println, log }");
    CHECK(r.ok, "from ... { sym1, sym2 } scan succeeds");
    auto *imp = get_import(*r.program, r.builder, 0);
    CHECK(imp != nullptr, "import node exists");
    CHECK(!imp->symbols.empty(), "has selective symbols");
    CHECK_EQ(imp->symbols.size(), 2u, "2 selective symbols");
    CHECK_EQ(imp->symbols[0].name, "println", "first symbol is 'println'");
    CHECK(imp->symbols[0].alias.empty(), "first symbol has no alias");
    CHECK_EQ(imp->symbols[1].name, "log", "second symbol is 'log'");
}

static void test_from_with_selective_alias() {
    ImportTest t;
    auto r = t.scan("from std/io/console { println as p, log }");
    CHECK(r.ok, "from ... { sym1 as a, sym2 } scan succeeds");
    auto *imp = get_import(*r.program, r.builder, 0);
    CHECK(imp != nullptr, "import node exists");
    CHECK_EQ(imp->symbols.size(), 2u, "2 selective symbols");
    CHECK_EQ(imp->symbols[0].name, "println", "first symbol name");
    CHECK_EQ(imp->symbols[0].alias, "p", "first symbol alias");
    CHECK_EQ(imp->symbols[1].name, "log", "second symbol name");
    CHECK(imp->symbols[1].alias.empty(), "second symbol has no alias");
}

static void test_import_with_selective() {
    ImportTest t;
    auto r = t.scan("import std/collections/map { HashMap, TreeMap }");
    CHECK(r.ok, "import ... { sym1, sym2 } scan succeeds");
    auto *imp = get_import(*r.program, r.builder, 0);
    CHECK(imp != nullptr, "import node exists");
    CHECK(!imp->is_from, "is_from is false");
    CHECK_EQ(imp->symbols.size(), 2u, "2 selective symbols");
}

static void test_export_with_selective() {
    ImportTest t;
    auto r = t.scan("export std/io/console { println }");
    CHECK(r.ok, "export ... { sym } scan succeeds");
    auto *imp = get_import(*r.program, r.builder, 0);
    CHECK(imp != nullptr, "import node exists");
    CHECK(imp->is_export, "is_export is true");
    CHECK_EQ(imp->symbols.size(), 1u, "1 selective symbol");
}

static void test_from_with_depth() {
    ImportTest t;
    auto r = t.scan("from std/io(3)");
    CHECK(r.ok, "from ...(depth) scan succeeds");
    auto *imp = get_import(*r.program, r.builder, 0);
    CHECK(imp != nullptr, "import node exists");
    CHECK_EQ(imp->import_depth, 3, "import depth is 3");
}

static void test_from_with_unlimited_depth() {
    ImportTest t;
    auto r = t.scan("from std/io(..)");
    CHECK(r.ok, "from ...(..) scan succeeds");
    auto *imp = get_import(*r.program, r.builder, 0);
    CHECK(imp != nullptr, "import node exists");
    CHECK_EQ(imp->import_depth, -1, "import depth is -1 (unlimited)");
}

static void test_from_asset() {
    ImportTest t;
    auto r = t.scan("from assets/data.json as Data");
    CHECK(r.ok, "from assets/... as alias scan succeeds");
    auto *imp = get_import(*r.program, r.builder, 0);
    CHECK(imp != nullptr, "import node exists");
    CHECK(imp->is_asset, "is_asset is true");
    CHECK_EQ(imp->path[0], "assets", "first path segment is 'assets'");
    CHECK_EQ(imp->alias, "Data", "alias is 'Data'");
}

static void test_from_asset_requires_alias() {
    ImportTest t;
    auto r = t.scan("from assets/data.json");
    CHECK(!r.ok, "from assets/... without alias produces error");
    CHECK_GE(r.errorCount, 1u, "at least 1 error for missing alias");
}

static void test_relative_import() {
    ImportTest t;
    auto r = t.scan("from ../lib/utils");
    CHECK(r.ok, "from ../relative/path scan succeeds");
    auto *imp = get_import(*r.program, r.builder, 0);
    CHECK(imp != nullptr, "import node exists");
    CHECK_EQ(imp->path.size(), 3u, "3 path segments");
    CHECK_EQ(imp->path[0], "..", "first segment is '..'");
}

static void test_from_empty_path() {
    ImportTest t;
    auto r = t.scan("from");
    CHECK(!r.ok, "from with no path produces error");
    CHECK_GE(r.errorCount, 1u, "at least 1 error");
}

static void test_import_empty_path() {
    ImportTest t;
    auto r = t.scan("import");
    CHECK(!r.ok, "import with no path produces error");
    CHECK_GE(r.errorCount, 1u, "at least 1 error");
}

static void test_from_selective_empty() {
    ImportTest t;
    auto r = t.scan("from std/io/console {}");
    CHECK(r.ok, "from ... {} (empty list) scan succeeds");
    auto *imp = get_import(*r.program, r.builder, 0);
    CHECK(imp != nullptr, "import node exists");
    CHECK(imp->symbols.empty(), "empty symbols list");
}

static void test_selective_trailing_comma() {
    ImportTest t;
    auto r = t.scan("from std/io { println, }");
    CHECK(r.ok, "from ... { sym, } scan succeeds");
    auto *imp = get_import(*r.program, r.builder, 0);
    CHECK(imp != nullptr, "import node exists");
    CHECK_EQ(imp->symbols.size(), 1u, "1 symbol (trailing comma ignored)");
}

static void test_import_relative() {
    ImportTest t;
    auto r = t.scan("import ../helper");
    CHECK(r.ok, "import ../helper scan succeeds");
}

static void test_export_relative() {
    ImportTest t;
    auto r = t.scan("export ../lib");
    CHECK(r.ok, "export ../lib scan succeeds");
}

static void test_multiple_imports() {
    ImportTest t;
    auto r = t.scan("from std/io/console\nimport std/collections/map\nexport std/math");
    CHECK(r.ok, "multiple imports scan succeeds");
    CHECK_EQ(count_decls(*r.program), 3, "3 decls");
}

static void test_physical_file_deduplication() {
    ImportIntegrationTest t;
    t.write("util.zith", "pub fn helper() {}\n");
    t.write("main.zith", "");

    auto first     = resolve(t, {"util"}, false, false, {}, (t.root / "main.zith").string());
    auto root_name = t.root.filename().string();
    auto second =
        resolve(t, {"..", root_name, "util"}, false, false, {}, (t.root / "main.zith").string());

    CHECK(first && second, "alternate import spellings resolve");
    CHECK_EQ(first.value(), second.value(), "alternate spellings reuse the same loaded file");
    CHECK_EQ(t.imports.fileCount(), 1u, "physical file is loaded once");
}

static void test_cycle_through_alternate_spelling() {
    ImportIntegrationTest t;
    auto root_name = t.root.filename().string();
    t.write("a.zith", "from b\n");
    t.write("b.zith", "from ../" + root_name + "/a\n");
    t.write("main.zith", "");

    auto result = resolve(t, {"a"}, false, false, {}, (t.root / "main.zith").string());
    CHECK(result, "outer import completes while reporting its broken transitive import");
    CHECK(t.hasError(diagnostics::err::ImportError, "circular import"),
          "alternate-spelling cycle is diagnosed");
}

static void test_non_relative_import_cannot_escape_visible_root() {
    ImportIntegrationTest t;
    auto outside = t.root.parent_path() / "zith-import-outside";
    std::filesystem::remove_all(outside);
    std::filesystem::create_directories(outside);
    std::ofstream(outside / "mod.zith") << "pub fn outside() {}\n";

    auto found =
        symbols::import_resolver::findFile("../zith-import-outside/mod", {}, {t.root.string()});
    CHECK(!found, "non-relative import with '..' cannot escape visible root");

    std::filesystem::remove_all(outside);
}

static void test_imported_symbol_collision_is_error() {
    ImportIntegrationTest t;
    t.write("one.zith", "pub fn foo() {}\n");
    t.write("two.zith", "pub fn foo() {}\n");

    auto one = resolve(t, {"one"}, true);
    auto two = resolve(t, {"two"}, true);
    symbols::SymbolTable main_symbols(t.arena, &t.interner);
    t.imports.mergeInto(main_symbols);

    CHECK(one && two, "colliding modules resolve");
    CHECK(t.hasError(diagnostics::err::DuplicateDecl, "conflicts between"),
          "symbol collision reports DuplicateDecl");
}

static void test_broken_re_export_is_error() {
    ImportIntegrationTest t;
    t.write("broken.zith", "export missing\n");

    auto result = resolve(t, {"broken"});
    CHECK(result, "module with broken re-export loads for diagnostic collection");
    CHECK(t.hasError(diagnostics::err::ImportError, "re-export of 'missing' failed"),
          "broken re-export is an error");
}

static void test_missing_selective_import_symbol_is_error() {
    ImportIntegrationTest t;
    t.write("module.zith", "pub fn present() {}\n");

    auto result = resolve(t, {"module"}, true, false, {{"missing", {}}});
    symbols::SymbolTable main_symbols(t.arena, &t.interner);
    t.imports.mergeInto(main_symbols);

    CHECK(result, "selective module resolves");
    CHECK(t.hasError(diagnostics::err::ImportError, "selector 'missing' was not found"),
          "missing selective symbol is an error");
}

static void test_empty_directory_import_has_no_cache_entry() {
    ImportIntegrationTest t;
    std::filesystem::create_directories(t.root / "empty");

    auto result = resolve(t, {"empty"});
    CHECK(!result, "empty directory import fails");
    CHECK(!t.imports.isLoaded("empty"), "failed directory import does not leave a cache entry");
}

// ── Main ─────────────────────────────────────────────────────────────
static void test_import() {
    test_from_simple();
    test_import_simple();
    test_export_behaves_like_import();
    test_from_with_alias();
    test_import_with_alias();
    test_from_with_selective();
    test_from_with_selective_alias();
    test_import_with_selective();
    test_export_with_selective();
    test_from_with_depth();
    test_from_with_unlimited_depth();
    test_from_asset();
    test_from_asset_requires_alias();
    test_relative_import();
    test_from_empty_path();
    test_import_empty_path();
    test_from_selective_empty();
    test_selective_trailing_comma();
    test_import_relative();
    test_export_relative();
    test_multiple_imports();
    test_physical_file_deduplication();
    test_cycle_through_alternate_spelling();
    test_non_relative_import_cannot_escape_visible_root();
    test_imported_symbol_collision_is_error();
    test_broken_re_export_is_error();
    test_missing_selective_import_symbol_is_error();
    test_empty_directory_import_has_no_cache_entry();
}

TEST_MAIN(import)
