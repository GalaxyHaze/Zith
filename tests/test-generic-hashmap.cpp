#include "diagnostics/error-codes.hpp"
#include "hir/hir-expr.hpp"
#include "session/compilation-session.hpp"
#include "session/frontend-context.hpp"
#include "test-common.hpp"
#include "types/type-kind.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace zith;

namespace {

struct SessionRunner {
    memory::Arena arena;
    Options opts;
    std::filesystem::path root;

    SessionRunner()
        : opts(arena), root(std::filesystem::temp_directory_path() / "zith-generic-hashmap-tests") {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        opts.targetStage = session::Stage::TypeChecked;
#ifdef ZITH_STDLIB_DIR
        // Mirror `zithc --include stdlib` so tests can import std/alloc and friends.
        opts.includeDirs.push(ZITH_STDLIB_DIR);
#endif
    }

    ~SessionRunner() {
        std::filesystem::remove_all(root);
    }

    bool run(std::string_view input, session::Stage target = session::Stage::TypeChecked,
             const std::function<bool(const session::CompilationSession &)> &inspect = {}) {
        const auto path = root / "main.zithm";
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << input;
        file.close();

        session::FrontendConfig config;
        config.workspaceRoot      = root.string();
        config.maxFrontendWorkers = 1;
        config.compilerVersion    = "test";
        for (const auto &dir : opts.includeDirs)
            config.includeRoots.push_back(dir);
        auto context = std::make_shared<session::FrontendContext>(config);
        session::CompilationSession session(opts, path.string(), std::move(context));
        session.setBuffered(true);
        const bool ok = session.runTo(target);
        for (const auto &d : session.diags().all()) {
            if (d.severity == diagnostics::Severity::Error) {
                std::printf("    [Diag] Code: %u, Message: %s\n", static_cast<unsigned>(d.code),
                            d.message.c_str());
            }
        }
        return ok && session.diags().errorCount() == 0 && (!inspect || inspect(session));
    }
};

void test_nested_generic_calls_propagate_bounds() {
    SessionRunner t;
    const bool ok = t.run("trait Hashable {\n"
                          "    fn hash(self): u64;\n"
                          "}\n"
                          "implement i32 as Hashable {\n"
                          "    fn hash(self): u64 { return self as u64; }\n"
                          "}\n"
                          "struct Entry<T> { key: T }\n"
                          "fn contains<K: Hashable>(self: view Entry<K>): u64 {\n"
                          "    return self->key.hash();\n"
                          "}\n"
                          "fn put<K: Hashable>(self: lend Entry<K>) {\n"
                          "    let x = contains(view self);\n"
                          "    self->key = 0 as K;\n"
                          "}\n"
                          "fn main(): i32 { return 0; }\n");
    CHECK(ok, "nested generic call propagates caller bounds and args");
}

void test_reified_struct_reuses_args_identity() {
    SessionRunner t;
    const bool ok = t.run("struct Pair<A, B> { left: A, right: B }\n"
                          "fn get<A, B>(p: Pair<A, B>): A { return p.left; }\n"
                          "fn apply<A, B>(p: Pair<A, B>) {\n"
                          "    let x = get(p);\n"
                          "}\n"
                          "fn main(): i32 { return 0; }\n");
    CHECK(ok, "generic inference matches reified struct type args by index");
}

void test_imported_generic_functions_resolve_explicit_arguments() {
    SessionRunner t;
    std::ofstream module(t.root / "generic_helper.zithm", std::ios::binary | std::ios::trunc);
    module << "from soon/memory\n"
              "pub fn echo<T>(value: T): T { return value; }\n"
              "pub fn create<T>(allocator: dyn Allocator): i32 { return 42; }\n";
    module.close();

    const bool ok = t.run("import generic_helper as helper\n"
                          "from std/memory\n"
                          "fn main(): i32 {\n"
                          "    let heap = HeapAllocator {};\n"
                          "    let value = helper.echo<i32>(42);\n"
                          "    return helper.create<i32>(heap) - value;\n"
                          "}\n");
    CHECK(ok, "imported generic functions accept explicit types and coercible arguments");
}

void test_imported_generic_layout_intrinsic_lowers_with_module_context() {
    SessionRunner t;
    const bool ok =
        t.run("import soon/collections/dyn_array as array\n"
              "fn main(): i32 { return 0; }\n",
              session::Stage::HirLowered, [](const session::CompilationSession &result) {
                  bool found_template = false;
                  bool generic_field  = false;
                  for (uint32_t index = 0; index < result.types().structDefCount(); ++index) {
                      const auto *def = result.types().lookupStructDef(index);
                      if (def == nullptr || result.interner().lookup(def->name).find(
                                                "DynArraySlot") == std::string_view::npos)
                          continue;
                      found_template = true;
                      for (const auto &field : def->fields)
                          generic_field |=
                              result.types().kindOf(field.type) == types::TypeKind::GenericParam;
                  }
                  return found_template && generic_field;
              });
    CHECK(ok, "imported generic layout intrinsics retain their declaring module's type context");
}

void test_imported_generic_call_with_explicit_self_lowers() {
    SessionRunner t;
    std::ofstream module(t.root / "generic_helper.zithm", std::ios::binary | std::ios::trunc);
    module << "pub fn reserve<T>(self: i32, value: T): bool { return true; }\n"
              "pub fn grow<T>(value: T): bool { return reserve<T>(0, value); }\n";
    module.close();

    const bool ok = t.run("import generic_helper as helper\n"
                          "fn main(): i32 {\n"
                          "    if helper.grow<i32>(1) { return 0; }\n"
                          "    return 1;\n"
                          "}\n",
                          session::Stage::HirLowered);
    CHECK(ok, "imported generic calls with an explicit self argument lower to HIR");
}

void test_generic_inferred_return_ignores_assignment_tail() {
    SessionRunner t;
    const bool ok =
        t.run("fn mutate<T>(var value: T) {\n"
              "    value = value;\n"
              "}\n"
              "fn main(): i32 {\n"
              "    var number: i32 = 7;\n"
              "    mutate<i32>(number);\n"
              "    return 0;\n"
              "}\n",
              session::Stage::HirLowered, [](const session::CompilationSession &result) {
                  bool found_void_instance = false;
                  for (size_t index = 0; index < result.hirModule().getFnCount(); ++index) {
                      const auto &function = result.hirModule().getFn(index);
                      const auto name      = result.interner().lookup(function.name);
                      if (name.find("mutate") != std::string_view::npos &&
                          result.types().kindOf(function.return_type) == types::TypeKind::Void) {
                          if (function.blocks.empty())
                              continue;
                          const auto terminator = function.blocks.back().terminator;
                          if (terminator == hir::kInvalidHirExpr)
                              continue;
                          const auto *ret =
                              std::get_if<hir::HirRet>(&result.hirModule().getExpr(terminator));
                          found_void_instance =
                              ret != nullptr && ret->value == hir::kInvalidHirExpr;
                      }
                  }
                  return found_void_instance;
              });
    CHECK(ok, "a generic assignment statement infers and lowers a valueless return");
}

void test_explicit_generic_arguments_still_validate_runtime_arguments() {
    SessionRunner t;
    const bool ok = t.run("fn echo<T>(value: T): T { return value; }\n"
                          "fn main(): i32 {\n"
                          "    let value = echo<i32>(\"not an integer\");\n"
                          "    return 0;\n"
                          "}\n");
    CHECK(!ok, "explicit generic arguments do not bypass runtime argument type checking");
}

void test_hashmap_stdlib_checks() {
    SessionRunner t;
    const std::string input =
        "import \"stdlib.h\"\n"
        "pub trait Hashable { fn hash(self): u64; }\n"
        "implement i32 as Hashable { fn hash(self): u64 { return self as u64; } }\n"
        "pub struct Entry<K, V> { key: K, value: V, occupied: bool = false, next: u64 = 0 }\n"
        "pub struct HashMap<K, V> {\n"
        "    count: u64 = 0,\n"
        "    capacity: u64 = 0,\n"
        "    table: ?*Entry<K, V> = null,\n"
        "    head: u64 = 0,\n"
        "}\n"
        "pub fn reserve<K, V>(var self: lend HashMap<K, V>, capacity: u64): bool {\n"
        "    let mem: ?raw opaque = calloc(capacity, @sizeOf(Entry<K, V>));\n"
        "    if (mem is null) { return false; }\n"
        "    let table: ?*Entry<K, V> = mem as ?*Entry<K, V>;\n"
        "    if (table is null) { free(mem); return false; }\n"
        "    free(self->table);\n"
        "    self->table = table;\n"
        "    self->capacity = capacity;\n"
        "    self->count = 0u64;\n"
        "    self->head = 0u64;\n"
        "    return true;\n"
        "}\n"
        "pub fn init<K, V>(var self: lend HashMap<K, V>): bool {\n"
        "    return reserve<K, V>(lend self, 64u64);\n"
        "}\n"
        "pub fn contains<K: Hashable, V>(self: view HashMap<K, V>, key: K): bool {\n"
        "    if (self->table is null or self->count == 0u64) { return false; }\n"
        "    var entries: []Entry<K, V> = raw self->table[0u64..self->capacity];\n"
        "    var index = self->head;\n"
        "    for (index != 0u64) {\n"
        "        let current = raw entries[index - 1u64];\n"
        "        if (current.occupied and current.key.hash() == key.hash() and current.key == key) "
        "{\n"
        "            return true;\n"
        "        }\n"
        "        index = current.next;\n"
        "    }\n"
        "    return false;\n"
        "}\n"
        "pub fn put<K: Hashable, V>(var self: lend HashMap<K, V>, key: K, value: V): bool {\n"
        "    if (self->table is null) {\n"
        "        if not(init<K, V>(lend self)) { return false; }\n"
        "    }\n"
        "    if (not contains<K, V>(view self, key)) {\n"
        "        if (self->count >= self->capacity) { return false; }\n"
        "        var entries: []Entry<K, V> = raw self->table[0u64..self->capacity];\n"
        "        let index = self->count + 1u64;\n"
        "        entries[index - 1u64] = Entry<K, V>{ key, value, true, self->head };\n"
        "        self->head = index;\n"
        "        self->count = self->count + 1u64;\n"
        "    }\n"
        "    return true;\n"
        "}\n"
        "fn main(): i32 { return 0; }\n";
    CHECK(t.run(input), "hash-map shaped stdlib surface type-checks");
}

void test_inplace_trait_passes_check() {
    SessionRunner t;
    const bool ok = t.run("from soon/memory\n"
                          "\n"
                          "struct Box { value: i64 }\n"
                          "\n"
                          "implement Box as InPlace {\n"
                          "    fn inplace(var self, args: opaque): bool {\n"
                          "        return true;\n"
                          "    }\n"
                          "    fn clean(var self) {}\n"
                          "}\n"
                          "\n"
                          "fn main(): i32 {\n"
                          "    var box = Box { value: 7 };\n"
                          "    if not box.InPlace.inplace(0 as opaque) { return 1; }\n"
                          "    box.InPlace.clean();\n"
                          "    return 0;\n"
                          "}\n");
    CHECK(ok, "stdlib InPlace imports, Box conforms, and the trait calls type-check");
}

void test_generic_hashmap() {
    test_nested_generic_calls_propagate_bounds();
    test_reified_struct_reuses_args_identity();
    test_imported_generic_functions_resolve_explicit_arguments();
    test_imported_generic_layout_intrinsic_lowers_with_module_context();
    test_imported_generic_call_with_explicit_self_lowers();
    test_generic_inferred_return_ignores_assignment_tail();
    test_explicit_generic_arguments_still_validate_runtime_arguments();
    test_hashmap_stdlib_checks();
    test_inplace_trait_passes_check();
}

} // namespace

TEST_MAIN(generic_hashmap)
