#include <catch2/catch_test_macros.hpp>

#include "../impl/import/module_registry.hpp"
#include "../impl/import/symbol_resolver.hpp"

#include <string>
#include <vector>

using namespace zith::import;

TEST_CASE("MODULE REGISTRY: register and retrieve module", "[module][registry]") {
    ModuleRegistry::instance().clear();

    Module mod("test.module_a", "tests/modules/module_a.zith", 1);
    mod.add_symbol(SymbolEntry("Point", SymbolKind::Struct, Visibility::Public,
                         SourceLocation("tests/modules/module_a.zith", 2)));
    mod.add_symbol(SymbolEntry("create_point", SymbolKind::Function, Visibility::Public,
                         SourceLocation("tests/modules/module_a.zith", 8)));

    REQUIRE(ModuleRegistry::instance().register_module(std::move(mod)) == true);
    REQUIRE(ModuleRegistry::instance().exists("test.module_a") == true);

    auto retrieved = ModuleRegistry::instance().get_module("test.module_a");
    REQUIRE(retrieved != nullptr);
    REQUIRE(retrieved->name() == "test.module_a");
    REQUIRE(retrieved->symbol_count() == 2);

    Module mod_duplicate("test.module_a", "tests/modules/module_a.zith");
    REQUIRE(ModuleRegistry::instance().register_module(std::move(mod_duplicate)) == false);

    ModuleRegistry::instance().clear();
}

TEST_CASE("MODULE REGISTRY: duplicate module registration fails", "[module][registry][error]") {
    ModuleRegistry::instance().clear();

    Module mod1("test.module_a", "tests/modules/module_a.zith");
    ModuleRegistry::instance().register_module(std::move(mod1));

    Module mod2("test.module_a", "tests/modules/module_a.zith");
    bool reg_result = ModuleRegistry::instance().register_module(std::move(mod2));
    REQUIRE(reg_result == false);

    ModuleRegistry::instance().clear();
}

TEST_CASE("MODULE: symbol management", "[module][symbols]") {
    Module mod("test.module", "test.zith");

    mod.add_symbol(SymbolEntry("Foo", SymbolKind::Struct, Visibility::Public));
    mod.add_symbol(SymbolEntry("bar", SymbolKind::Function, Visibility::Private));

    REQUIRE(mod.symbol_count() == 2);
    REQUIRE(mod.has_symbol("Foo") == true);
    REQUIRE(mod.has_symbol("bar") == true);
    REQUIRE(mod.has_symbol("baz") == false);

    auto* sym = mod.find_symbol("Foo");
    REQUIRE(sym != nullptr);
    REQUIRE(sym->kind() == SymbolKind::Struct);
    REQUIRE(sym->visibility() == Visibility::Public);

    auto* priv_sym = mod.find_symbol("bar");
    REQUIRE(priv_sym != nullptr);
    REQUIRE(priv_sym->visibility() == Visibility::Private);
}

TEST_CASE("SYMBOL RESOLVER: resolve fully qualified path", "[resolver][resolve]") {
    ModuleRegistry::instance().clear();
    SymbolResolver::instance().clear();

    Module mod("std.io", "std/io/console.zith");
    mod.add_symbol(SymbolEntry("log", SymbolKind::Function, Visibility::Public,
                         SourceLocation("std/io/console.zith", 10)));
    mod.add_symbol(SymbolEntry("Console", SymbolKind::Struct, Visibility::Public,
                         SourceLocation("std/io/console.zith", 1)));
    ModuleRegistry::instance().register_module(std::move(mod));

    auto result = SymbolResolver::instance().resolve("std.io.log");
    bool result_ok = static_cast<bool>(result);
    REQUIRE(result_ok == true);
    REQUIRE(result.symbol() != nullptr);
    REQUIRE(result.symbol()->name() == "log");
    REQUIRE(result.module_name() == "std.io");

    auto result2 = SymbolResolver::instance().resolve("std.io.Console");
    bool result2_ok = static_cast<bool>(result2);
    REQUIRE(result2_ok == true);
    REQUIRE(result2.symbol()->name() == "Console");

    ModuleRegistry::instance().clear();
}

TEST_CASE("SYMBOL RESOLVER: resolve nested path", "[resolver][resolve][nested]") {
    ModuleRegistry::instance().clear();

    Module mod("std.io.console", "std/io/console.zith");
    mod.add_symbol(SymbolEntry("print", SymbolKind::Function, Visibility::Public));
    ModuleRegistry::instance().register_module(std::move(mod));

    auto result = SymbolResolver::instance().resolve("std.io.console.print");
    bool result_ok = static_cast<bool>(result);
    REQUIRE(result_ok == true);
    REQUIRE(result.symbol() != nullptr);
    REQUIRE(result.symbol()->name() == "print");

    ModuleRegistry::instance().clear();
}

TEST_CASE("SYMBOL RESOLVER: module not found error", "[resolver][error]") {
    ModuleRegistry::instance().clear();
    SymbolResolver::instance().clear_errors();

    auto result = SymbolResolver::instance().resolve("nonexistent.symbol");
    bool result_notfound = static_cast<bool>(result);
    REQUIRE(result_notfound == false);

    const auto& errors = SymbolResolver::instance().errors();
    REQUIRE(errors.size() > 0);
    REQUIRE(errors[0].code == ErrorCode::ModuleNotFound);

    ModuleRegistry::instance().clear();
}

TEST_CASE("SYMBOL RESOLVER: symbol not found error", "[resolver][error]") {
    ModuleRegistry::instance().clear();
    SymbolResolver::instance().clear_errors();

    Module mod("test.mod", "test.zith");
    ModuleRegistry::instance().register_module(std::move(mod));

    auto result = SymbolResolver::instance().resolve("test.mod.missing");
    bool result2_notfound = static_cast<bool>(result);
    REQUIRE(result2_notfound == false);

    const auto& errors = SymbolResolver::instance().errors();
    REQUIRE(errors.size() > 0);

    ModuleRegistry::instance().clear();
}

TEST_CASE("SYMBOL RESOLVER: alias support", "[resolver][alias]") {
    ModuleRegistry::instance().clear();
    SymbolResolver::instance().clear();

    Module mod("std.io", "std/io.zith");
    mod.add_symbol(SymbolEntry("log", SymbolKind::Function, Visibility::Public,
                         SourceLocation("std/io.zith", 1)));
    ModuleRegistry::instance().register_module(std::move(mod));

    bool alias_added = SymbolResolver::instance().add_alias("print", "std.io.log",
                                              SourceLocation("test.zith", 5));
    REQUIRE(alias_added == true);
    REQUIRE(SymbolResolver::instance().alias_exists("print") == true);

    auto aliased = SymbolResolver::instance().resolve_alias("print");
    REQUIRE(aliased.has_value() == true);
    REQUIRE(aliased->name() == "log");

    auto result = SymbolResolver::instance().resolve("std.io.log");
    bool result_ok = static_cast<bool>(result);
    REQUIRE(result_ok == true);

    bool alias_removed = SymbolResolver::instance().remove_alias("print");
    REQUIRE(alias_removed == true);
    REQUIRE(SymbolResolver::instance().alias_exists("print") == false);

    ModuleRegistry::instance().clear();
}

TEST_CASE("SYMBOL RESOLVER: duplicate detection", "[resolver][duplicate]") {
    ModuleRegistry::instance().clear();
    SymbolResolver::instance().clear_errors();

    Module mod("test.dup", "test.zith");
    // Add symbol with a signature
    SymbolEntry existing("Foo", SymbolKind::Struct, Visibility::Public,
                        SourceLocation("test.zith", 1));
    existing.set_signature(TypeSignature({}, "int"));
    mod.add_symbol(std::move(existing));
    ModuleRegistry::instance().register_module(std::move(mod));

    // Try to add duplicate with same signature
    SymbolEntry new_sym("Foo", SymbolKind::Struct, Visibility::Public,
                   SourceLocation("test.zith", 10));
    new_sym.set_signature(TypeSignature({}, "int"));

    auto error = SymbolResolver::instance().detect_duplicate("test.dup", new_sym);
    REQUIRE(error.has_value() == true);
    REQUIRE(error->code == ErrorCode::DuplicateSymbol);

    ModuleRegistry::instance().clear();
}

TEST_CASE("SYMBOL RESOLVER: valid function overloading", "[resolver][overload]") {
    ModuleRegistry::instance().clear();
    SymbolResolver::instance().clear_errors();

    Module mod("test.overload", "test.zith");

    SymbolEntry func1("add", SymbolKind::Function, Visibility::Public);
    func1.set_signature(TypeSignature{{"int", "int"}, "int"});
    mod.add_symbol(std::move(func1));

    SymbolEntry func2("add", SymbolKind::Function, Visibility::Public);
    func2.set_signature(TypeSignature{{"float", "float"}, "float"});
    mod.add_symbol(std::move(func2));

    ModuleRegistry::instance().register_module(std::move(mod));

    SymbolEntry func3("add", SymbolKind::Function, Visibility::Public);
    func3.set_signature(TypeSignature{{"string", "string"}, "string"});

    bool valid = SymbolResolver::instance().is_valid_overload("test.overload", func3);
    REQUIRE(valid == true);

    ModuleRegistry::instance().clear();
}

TEST_CASE("SYMBOL RESOLVER: invalid function overloading (duplicate signature)", "[resolver][overload][error]") {
    ModuleRegistry::instance().clear();

    Module mod("test.overload", "test.zith");

    SymbolEntry func1("add", SymbolKind::Function, Visibility::Public);
    func1.set_signature(TypeSignature{{"int", "int"}, "int"});
    mod.add_symbol(std::move(func1));

    ModuleRegistry::instance().register_module(std::move(mod));

    SymbolEntry func2("add", SymbolKind::Function, Visibility::Public);
    func2.set_signature(TypeSignature{{"int", "int"}, "int"});

    bool valid = SymbolResolver::instance().is_valid_overload("test.overload", func2);
    REQUIRE(valid == false);

    ModuleRegistry::instance().clear();
}

TEST_CASE("SYMBOL RESOLVER: resolve_all finds multiple modules", "[resolver][resolve]") {
    ModuleRegistry::instance().clear();

    Module mod1("module.one", "one.zith");
    mod1.add_symbol(SymbolEntry("Helper", SymbolKind::Struct, Visibility::Public));
    ModuleRegistry::instance().register_module(std::move(mod1));

    Module mod2("module.two", "two.zith");
    mod2.add_symbol(SymbolEntry("Helper", SymbolKind::Struct, Visibility::Public));
    ModuleRegistry::instance().register_module(std::move(mod2));

    auto results = SymbolResolver::instance().resolve_all("Helper");
    REQUIRE(results.size() == 2);

    ModuleRegistry::instance().clear();
}

TEST_CASE("SYMBOL RESOLVER: case sensitivity", "[resolver][case]") {
    ModuleRegistry::instance().clear();

    Module mod("test.case", "test.zith");
    mod.add_symbol(SymbolEntry("Log", SymbolKind::Function, Visibility::Public));
    ModuleRegistry::instance().register_module(std::move(mod));

    auto result = SymbolResolver::instance().resolve("test.case.Log");
    bool result_ok = static_cast<bool>(result);
    REQUIRE(result_ok == true);

    auto result_lower = SymbolResolver::instance().resolve("test.case.log");
    bool result_lower_ok = static_cast<bool>(result_lower);
    REQUIRE(result_lower_ok == false);

    ModuleRegistry::instance().clear();
}

TEST_CASE("TYPE SIGNATURE: comparison", "[typesig]") {
    TypeSignature sig1{{"int", "int"}, "int"};
    TypeSignature sig2{{"int", "int"}, "int"};
    TypeSignature sig3{{"float", "float"}, "float"};
    TypeSignature sig4{{"int"}, "int"};

    REQUIRE(sig1 == sig2);
    REQUIRE(sig1 != sig3);
    REQUIRE(sig1 != sig4);

    REQUIRE(sig1.is_compatible_with(sig2) == true);
    REQUIRE(sig1.is_compatible_with(sig3) == false);
    REQUIRE(sig1.is_compatible_with(sig4) == false);
}

TEST_CASE("SOURCE LOCATION: construction", "[location]") {
    SourceLocation loc1;
    REQUIRE(loc1.line == 0);
    REQUIRE(loc1.column == 0);

    SourceLocation loc2("test.zith", 10, 5);
    REQUIRE(loc2.file_path == "test.zith");
    REQUIRE(loc2.line == 10);
    REQUIRE(loc2.column == 5);
}