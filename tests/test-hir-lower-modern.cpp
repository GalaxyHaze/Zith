#include "cli/options.hpp"
#include "diagnostics/error-codes.hpp"
#include "hir/hir-expr.hpp"
#include "session/compilation-session.hpp"
#include "symbols/symbol-id.hpp"
#include "test-common.hpp"
#include "types/type-kind.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

using namespace zith;

namespace {

namespace fs = std::filesystem;

struct Workspace {
    fs::path root = fs::temp_directory_path() / "zith-hir-modern-tests";

    Workspace() {
        fs::remove_all(root);
        fs::create_directories(root);
    }

    ~Workspace() {
        fs::remove_all(root);
    }

    void writeFile(const fs::path &relative_path, std::string_view contents) const {
        const auto destination = root / relative_path;
        fs::create_directories(destination.parent_path());
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        output << contents;
    }
};

struct IsolatedWorkspace {
    fs::path root = fs::temp_directory_path() / "zith-hir-modern-canonical-persist-test";

    IsolatedWorkspace() {
        fs::remove_all(root);
        fs::create_directories(root);
    }

    ~IsolatedWorkspace() {
        fs::remove_all(root);
    }

    void writeFile(const fs::path &relative_path, std::string_view contents) const {
        const auto destination = root / relative_path;
        fs::create_directories(destination.parent_path());
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        output << contents;
    }
};

// HIR function names are qualified (`<namespace>.<Owner>.<name>(<params>)`) for everything
// except `extern fn` and `main`, so match on the bare source name.
std::string_view sourceName(std::string_view linkage_name) {
    auto paren = linkage_name.find('(');
    if (paren != std::string_view::npos)
        linkage_name = linkage_name.substr(0, paren);
    auto dot = linkage_name.rfind('.');
    if (dot != std::string_view::npos)
        linkage_name = linkage_name.substr(dot + 1);
    return linkage_name;
}

const hir::HirFunction *findFunction(const hir::HirModule &hir, memory::StringInterner &interner,
                                     std::string_view name) {
    for (size_t i = 0; i < hir.getFnCount(); ++i) {
        const auto &fn = hir.getFn(i);
        if (sourceName(interner.lookup(fn.name)) == name)
            return &fn;
    }
    return nullptr;
}

/// Finds the single HIR declaration whose source name matches `name` and whose
/// kind is `kind`. Test fixtures use unique names per kind, so the first match
/// is the intended declaration.
const hir::HirDeclaration *findDeclaration(const hir::HirModule &hir,
                                           memory::StringInterner &interner,
                                           std::string_view name,
                                           hir::HirDeclarationKind kind) {
    for (size_t i = 0; i < hir.declarationCount(); ++i) {
        const auto &decl = hir.getDeclaration(static_cast<hir::HirDeclarationId>(i));
        if (decl.kind == kind && interner.lookup(decl.name) == name)
            return &decl;
    }
    return nullptr;
}

hir::HirDeclarationId findDeclarationId(const hir::HirModule &hir,
                                        memory::StringInterner &interner,
                                        std::string_view name,
                                        hir::HirDeclarationKind kind) {
    for (size_t i = 0; i < hir.declarationCount(); ++i) {
        const auto &decl = hir.getDeclaration(static_cast<hir::HirDeclarationId>(i));
        if (decl.kind == kind && interner.lookup(decl.name) == name)
            return static_cast<hir::HirDeclarationId>(i);
    }
    return hir::kInvalidHirDeclaration;
}

size_t countExprKind(const hir::HirModule &hir, hir::HirExprKind kind) {
    size_t count = 0;
    for (size_t i = 0; i < hir.exprCount(); ++i) {
        if (hir::exprKind(hir.getExpr(static_cast<hir::HirExprId>(i))) == kind)
            ++count;
    }
    return count;
}

size_t countInstKind(const hir::HirModule &hir, const hir::HirFunction &fn, hir::HirExprKind kind) {
    size_t count = 0;
    for (const auto &block : fn.blocks) {
        for (auto inst : block.insts) {
            if (hir::exprKind(hir.getExpr(inst)) == kind)
                ++count;
        }
    }
    return count;
}

size_t countUnionCasts(const hir::HirModule &hir, const hir::HirFunction &fn) {
    (void)fn;
    size_t count = 0;
    // Nested operands are not listed as block instructions, so count every
    // expression created for this function. Test sessions contain only this
    // function plus externs, which carry no union casts.
    for (size_t i = 0; i < hir.exprCount(); ++i) {
        if (std::holds_alternative<hir::HirUnionCast>(hir.getExpr(static_cast<hir::HirExprId>(i))))
            ++count;
    }
    return count;
}

size_t countTerminatorKind(const hir::HirModule &hir, const hir::HirFunction &fn,
                           hir::HirExprKind kind) {
    size_t count = 0;
    for (const auto &block : fn.blocks) {
        if (block.terminator != hir::kInvalidHirExpr &&
            hir::exprKind(hir.getExpr(block.terminator)) == kind) {
            ++count;
        }
    }
    return count;
}

size_t countSlotAllocaOfType(const types::TypeIntern &types, const hir::HirModule &hir,
                             const hir::HirFunction &fn, types::TypeKind kind) {
    size_t count = 0;
    for (const auto &block : fn.blocks) {
        for (auto inst : block.insts) {
            if (const auto *alloca = std::get_if<hir::HirSlotAlloca>(&hir.getExpr(inst))) {
                if (alloca->type != types::kInvalidType && types.kindOf(alloca->type) == kind) {
                    ++count;
                }
            }
        }
    }
    return count;
}

std::string_view internedName(const memory::StringInterner &interner, memory::InternedId id) {
    const auto text = interner.lookup(id);
    return std::string_view(text.data(), text.size());
}

session::CompilationSession makeSession(const Workspace &workspace, memory::Arena &arena,
                                        Options &options, std::string_view file_name) {
    options.targetStage = session::Stage::HirLowered;
    session::CompilationSession session(options, (workspace.root / file_name).string());
    session.setBuffered(true);
    return session;
}

void test_extern_and_main_lower_to_hir() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "extern fn puts(msg: *char)\n"
                                     "fn main() {\n"
                                     "    puts(\"hello\");\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "modern lowering succeeds for extern + main");

    const auto &hir = session.hirModule();
    CHECK_EQ(hir.getFnCount(), 2u, "HIR contains exactly the extern function and main");

    const auto *puts = findFunction(hir, session.interner(), "puts");
    CHECK(puts != nullptr, "extern function is present in HIR");
    if (puts != nullptr)
        CHECK(puts->blocks.empty(), "extern function stays body-less in HIR");

    const auto *main = findFunction(hir, session.interner(), "main");
    CHECK(main != nullptr, "main function is present in HIR");
    if (main != nullptr) {
        CHECK_EQ(countInstKind(hir, *main, hir::HirExprKind::Call), 1u,
                 "main body contains one call instruction");
        CHECK_EQ(countTerminatorKind(hir, *main, hir::HirExprKind::Ret), 1u,
                 "main ends with one return terminator");
    }
}

void test_no_ownership_hir_has_empty_residual_attrs() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn add(a: i32, b: i32): i32 { a + b }\n"
                                     "fn main(): i32 {\n"
                                     "    var sum: i32 = add(3, 4);\n"
                                     "    sum\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "no-ownership program lowers to HIR");

    const auto &hir   = session.hirModule();
    const auto &attrs = hir.attrs();
    CHECK_EQ(attrs.slotCount(), 0u, "plain integer locals do not force slot attrs");
    CHECK_EQ(attrs.callCount(), 0u, "plain integer calls do not force call attrs");
    CHECK_EQ(attrs.fnCount(), 0u, "plain integer functions do not force fn attrs");
}

void test_ownership_hir_carries_residual_slot_attrs() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "struct P { x: i32 }\n"
                                     "fn read(p: view P): i32 { p.x }\n"
                                     "fn main(): i32 {\n"
                                     "    let v: view P = P { x: 41 };\n"
                                     "    read(view v) + 1\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "qualified ownership program lowers to HIR");

    const auto &hir   = session.hirModule();
    const auto &attrs = hir.attrs();
    CHECK(attrs.slotCount() > 0u, "qualified ownership emits residual slot attrs");

    bool saw_view = false;
    for (size_t slot = 0; slot < attrs.slotCount(); ++slot) {
        const auto *slot_attrs = attrs.trySlot(static_cast<hir::HirSlotId>(slot));
        saw_view |= slot_attrs != nullptr && slot_attrs->ownership == hir::HirOwnership::View;
    }
    CHECK(saw_view, "a slot annotated with 'view' carries the residual ownership fact");
}

void test_free_borrow_parameter_lowers_to_pointer_and_call_addr() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "struct P { x: i32 }\n"
                                     "fn bump(p: lend P): i32 { p.x = p.x + 1; p.x }\n"
                                     "fn main(): i32 {\n"
                                     "    var q: P = P { x: 41 };\n"
                                     "    bump(lend q)\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "free borrow parameter program lowers to HIR");

    const auto &hir  = session.hirModule();
    const auto *bump = findFunction(hir, session.interner(), "bump");
    const auto *main = findFunction(hir, session.interner(), "main");
    CHECK(bump != nullptr, "bump function is present in HIR");
    if (bump != nullptr) {
        CHECK_EQ(bump->params.size(), 1u, "bump has one lowered parameter");
        if (bump->params.size() == 1u)
            CHECK_EQ(static_cast<int>(session.types().kindOf(bump->params[0])),
                     static_cast<int>(types::TypeKind::Ptr),
                     "a lend parameter lowers to a pointer type");
    }
    CHECK(main != nullptr, "main is present in HIR");
    if (main != nullptr) {
        bool saw_addr_argument = false;
        for (const auto &block : main->blocks) {
            for (auto inst : block.insts) {
                const auto *call = std::get_if<hir::HirCall>(&hir.getExpr(inst));
                if (call == nullptr)
                    continue;
                for (const auto arg : call->args) {
                    const auto &arg_expr = hir.getExpr(arg);
                    if (std::get_if<hir::HirSlotAddr>(&arg_expr) != nullptr)
                        saw_addr_argument = true;
                }
            }
        }
        CHECK(saw_addr_argument,
              "a call with an annotated lend argument passes the binding address");
    }
}

void test_extern_variadic_lower_to_hir() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "extern fn printf(fmt: *char, ...): i32\n"
                                     "fn main() {\n"
                                     "    _ = printf(\"n=%d\\n\", 7);\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "modern lowering succeeds for extern variadic fn");

    const auto &hir    = session.hirModule();
    const auto *printf = findFunction(hir, session.interner(), "printf");
    CHECK(printf != nullptr, "variadic extern function is present in HIR");
    if (printf != nullptr)
        CHECK(printf->isVariadic, "HIR carries the variadic flag");
}

void test_external_symbol_alias_lower_to_hir() {
    Workspace workspace;
    workspace.writeFile("main.zithm",
                        "fn cAdd(a: i32, b: i32): i32 = extern zith_test_external_alias;\n"
                        "fn overloaded(x: i32): i32 = extern zith_test_external_alias_a;\n"
                        "fn overloaded(x: f64): f64 = extern zith_test_external_alias_b;\n"
                        "fn main(): i32 {\n"
                        "    cAdd(1, 2) + overloaded(3)\n"
                        "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "modern lowering succeeds for external symbol aliases");

    const auto &hir   = session.hirModule();
    const auto *alias = findFunction(hir, session.interner(), "zith_test_external_alias");
    CHECK(alias != nullptr, "external symbol alias becomes the HIR linkage name");
    if (alias != nullptr)
        CHECK(alias->blocks.empty(), "external symbol alias generates no body");

    const auto *a = findFunction(hir, session.interner(), "zith_test_external_alias_a");
    const auto *b = findFunction(hir, session.interner(), "zith_test_external_alias_b");
    CHECK(a != nullptr, "first overload uses its assigned C symbol");
    CHECK(b != nullptr, "second overload uses its assigned C symbol");
}

void test_bindings_lower_to_slots() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn add(a: i32, b: i32): i32 { a + b }\n"
                                     "fn main(): i32 {\n"
                                     "    var sum: i32 = add(3, 4);\n"
                                     "    sum\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "modern lowering succeeds for locals");

    const auto &hir  = session.hirModule();
    const auto *main = findFunction(hir, session.interner(), "main");
    CHECK(main != nullptr, "main is available for slot checks");
    if (main != nullptr) {
        CHECK_EQ(countInstKind(hir, *main, hir::HirExprKind::SlotAlloca), 1u,
                 "local binding allocates one slot");
        CHECK(countInstKind(hir, *main, hir::HirExprKind::SlotStore) >= 1u,
              "local binding stores its initializer into a slot");
        CHECK(countInstKind(hir, *main, hir::HirExprKind::SlotLoad) >= 1u,
              "reading the local lowers to a slot load");
    }
}

void test_const_global_lowers_to_load() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "const GLOBAL: i32 = 3;\n"
                                     "fn main(): i32 {\n"
                                     "    return GLOBAL;\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "modern lowering succeeds for a top-level const");

    const auto &hir = session.hirModule();
    CHECK_EQ(hir.getGlobalConstCount(), 1u, "HIR contains one const global");
    if (hir.getGlobalConstCount() != 1u)
        return;
    const auto &global = hir.getGlobalConst(0);
    CHECK(!session.interner().lookup(global.name).empty(), "const global has a linkage name");
    CHECK(global.init != hir::kInvalidHirExpr, "const global has an initializer expression");

    bool saw_load = false;
    for (size_t i = 0; i < hir.exprCount(); ++i) {
        if (std::holds_alternative<hir::HirGlobalConstLoad>(
                hir.getExpr(static_cast<hir::HirExprId>(i))))
            saw_load = true;
    }
    CHECK(saw_load, "referencing the const global lowers to a global const load");
}

void test_untyped_const_global_type_comes_from_initializer() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "const SIZE = 1024;");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "modern lowering succeeds for an untyped top-level const");

    const auto &hir = session.hirModule();
    CHECK_EQ(hir.getGlobalConstCount(), 1u, "HIR contains one const global");
    if (hir.getGlobalConstCount() != 1u)
        return;
    const auto &global = hir.getGlobalConst(0);
    CHECK(global.init != hir::kInvalidHirExpr, "const global has an initializer expression");
    CHECK(global.type != types::kVoidType,
          "untyped const global gets the type inferred from its initializer");
}

void test_selective_imported_const_lowers_to_global_const_load() {
    Workspace workspace;
    workspace.writeFile("dep.zithm", "pub const VALUE: i32 = 7;\n");
    workspace.writeFile("main.zithm", "from dep { VALUE as selected }\n"
                                     "const COPY: i32 = selected;\n"
                                     "fn main(): i32 {\n"
                                     "    return selected;\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "selectively imported const lowers to HIR");

    const auto &hir = session.hirModule();
    CHECK(hir.getGlobalConstCount() >= 1u,
          "imported const is represented as a global const");

    bool saw_load = false;
    for (size_t i = 0; i < hir.exprCount(); ++i) {
        if (std::holds_alternative<hir::HirGlobalConstLoad>(
                hir.getExpr(static_cast<hir::HirExprId>(i))))
            saw_load = true;
    }
    CHECK(saw_load, "referencing the selectively imported const lowers to a global const load");
}

void test_from_imported_const_lowers_to_global_const_load() {
    Workspace workspace;
    workspace.writeFile("dep.zithm", "pub const VALUE: i32 = 7;\n");
    workspace.writeFile("main.zithm", "from dep\n"
                                     "fn main(): i32 {\n"
                                     "    return VALUE;\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "`from`-injected const lowers to HIR");

    const auto &hir = session.hirModule();
    CHECK(hir.getGlobalConstCount() >= 1u,
          "`from`-injected const is represented as a global const");

    bool saw_load = false;
    for (size_t i = 0; i < hir.exprCount(); ++i) {
        if (std::holds_alternative<hir::HirGlobalConstLoad>(
                hir.getExpr(static_cast<hir::HirExprId>(i))))
            saw_load = true;
    }
    CHECK(saw_load, "referencing the `from`-injected const lowers to a global const load");
}

void test_qualified_imported_const_lowers_to_global_const_load() {
    Workspace workspace;
    workspace.writeFile("dep.zithm", "pub const VALUE: i32 = 7;\n");
    workspace.writeFile("main.zithm", "import dep as ns\n"
                                     "fn main(): i32 {\n"
                                     "    return ns.VALUE;\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "namespace-qualified imported const lowers to HIR");

    const auto &hir = session.hirModule();
    CHECK(hir.getGlobalConstCount() >= 1u,
          "namespace-qualified imported const is represented as a global const");

    bool saw_load = false;
    for (size_t i = 0; i < hir.exprCount(); ++i) {
        if (std::holds_alternative<hir::HirGlobalConstLoad>(
                hir.getExpr(static_cast<hir::HirExprId>(i))))
            saw_load = true;
    }
    CHECK(saw_load,
          "referencing the namespace-qualified imported const lowers to a global const load");
}

void test_if_else_lowers_to_branch_and_merge() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn pick(flag: bool): i32 {\n"
                                     "    if (flag) {\n"
                                     "        1\n"
                                     "    } else {\n"
                                     "        2\n"
                                     "    }\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "modern lowering succeeds for if/else");

    const auto &hir  = session.hirModule();
    const auto *pick = findFunction(hir, session.interner(), "pick");
    CHECK(pick != nullptr, "if/else function is present in HIR");
    if (pick != nullptr) {
        CHECK(pick->blocks.size() >= 4u, "if/else creates entry, then, else, and merge blocks");
        CHECK(countInstKind(hir, *pick, hir::HirExprKind::SlotAlloca) >= 2u,
              "if/else uses slots for the parameter and merged result");
        CHECK(countInstKind(hir, *pick, hir::HirExprKind::SlotStore) >= 2u,
              "both branches store into the merge slot");
        CHECK(countInstKind(hir, *pick, hir::HirExprKind::SlotLoad) >= 1u,
              "merge block reloads the if/else result");
        CHECK_EQ(countTerminatorKind(hir, *pick, hir::HirExprKind::Branch), 1u,
                 "if/else emits one branch terminator");
        CHECK(countTerminatorKind(hir, *pick, hir::HirExprKind::Jump) >= 2u,
              "then and else blocks jump to merge");
    }
}

void test_else_condition_lowers_to_branch_and_merge() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn pick(a: bool, b: bool): i32 {\n"
                                     "    if (a) {\n"
                                     "        1\n"
                                     "    } else (b) {\n"
                                     "        2\n"
                                     "    } else {\n"
                                     "        3\n"
                                     "    }\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "modern lowering succeeds for else condition");

    const auto &hir  = session.hirModule();
    const auto *pick = findFunction(hir, session.interner(), "pick");
    CHECK(pick != nullptr, "else-condition function is present in HIR");
    if (pick != nullptr) {
        CHECK(pick->blocks.size() >= 4u,
              "else condition creates entry, then, else, and merge blocks");
        CHECK(countTerminatorKind(hir, *pick, hir::HirExprKind::Branch) >= 1u,
              "else condition emits at least the initial branch terminator");
        CHECK(countTerminatorKind(hir, *pick, hir::HirExprKind::Jump) >= 2u,
              "then/else blocks jump to merge");
    }
}

void test_while_continue_lowers_loop_cfg() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn main(): i32 {\n"
                                     "    var i: i32 = 0;\n"
                                     "    while (i < 10) {\n"
                                     "        i = i + 1;\n"
                                     "        continue;\n"
                                     "    }\n"
                                     "    i\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "modern lowering succeeds for while + continue");

    const auto &hir  = session.hirModule();
    const auto *main = findFunction(hir, session.interner(), "main");
    CHECK(main != nullptr, "loop function is present in HIR");
    if (main != nullptr) {
        CHECK(main->blocks.size() >= 4u,
              "loop lowering creates entry, header, body, and exit blocks");
        CHECK(countTerminatorKind(hir, *main, hir::HirExprKind::Branch) >= 1u,
              "loop lowering emits a branch terminator for the condition");
        CHECK(countTerminatorKind(hir, *main, hir::HirExprKind::Jump) >= 2u,
              "loop lowering emits jumps for the back-edge and entry");
        CHECK(countInstKind(hir, *main, hir::HirExprKind::SlotStore) >= 2u,
              "loop variable updates lower to slot stores");
    }
}

void test_while_break_lowers_loop_exit() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn main(): i32 {\n"
                                     "    var i: i32 = 0;\n"
                                     "    while (i < 10) {\n"
                                     "        i = i + 1;\n"
                                     "        break;\n"
                                     "    }\n"
                                     "    i\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "modern lowering succeeds for while + break");

    const auto &hir  = session.hirModule();
    const auto *main = findFunction(hir, session.interner(), "main");
    CHECK(main != nullptr, "break loop function is present in HIR");
    if (main != nullptr) {
        CHECK(countTerminatorKind(hir, *main, hir::HirExprKind::Branch) >= 1u,
              "break loop still branches on the while condition");
        CHECK(countTerminatorKind(hir, *main, hir::HirExprKind::Jump) >= 2u,
              "break loop emits jumps into and out of the loop");
    }
}

void test_labeled_nested_loop_lowers() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn main(): i32 {\n"
                                     "    outer: for (var i: i32 = 0), (i < 3), (i = i + 1) {\n"
                                     "        inner: for (var j: i32 = 0), (j < 3), (j = j + 1) {\n"
                                     "            if (j == 1) { continue outer; }\n"
                                     "            if (i == 1) { break outer; }\n"
                                     "        }\n"
                                     "    }\n"
                                     "    return 0;\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "modern lowering succeeds for labeled nested loops");

    const auto &hir  = session.hirModule();
    const auto *main = findFunction(hir, session.interner(), "main");
    CHECK(main != nullptr, "labeled loop function is present in HIR");
    if (main != nullptr) {
        CHECK(main->blocks.size() >= 10u,
              "two nested loops create entry, body, step, and exit blocks");
        CHECK(countTerminatorKind(hir, *main, hir::HirExprKind::Jump) >= 6u,
              "labeled jumps target nested loop exits/continue blocks");
    }
}

std::vector<hir::HirSlotId> allocatedSlots(const hir::HirModule &hir, const hir::HirFunction &fn) {
    std::vector<hir::HirSlotId> slots;
    for (const auto &block : fn.blocks) {
        for (auto inst : block.insts) {
            if (const auto *alloca = std::get_if<hir::HirSlotAlloca>(&hir.getExpr(inst)))
                slots.push_back(alloca->slot);
        }
    }
    return slots;
}

std::vector<hir::HirSlotId> loadedSlots(const hir::HirModule &hir, const hir::HirFunction &fn) {
    std::vector<hir::HirSlotId> slots;
    for (const auto &block : fn.blocks) {
        for (auto inst : block.insts) {
            if (const auto *load = std::get_if<hir::HirSlotLoad>(&hir.getExpr(inst)))
                slots.push_back(load->slot);
        }
    }
    return slots;
}

bool loadsOnlyOwnSlots(const hir::HirModule &hir, const hir::HirFunction &fn) {
    const auto allocated = allocatedSlots(hir, fn);
    bool result          = true;
    for (const auto slot : loadedSlots(hir, fn)) {
        bool found = false;
        for (const auto owned : allocated)
            found |= owned == slot;
        result &= found;
    }
    return result;
}

void test_same_named_parameters_use_distinct_slots() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn f(x: i32): i32 { return x; }\n"
                                     "fn g(x: i32): i32 { return x; }\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "modern lowering succeeds for same-named parameters");

    const auto &hir = session.hirModule();
    const auto *f   = findFunction(hir, session.interner(), "f");
    const auto *g   = findFunction(hir, session.interner(), "g");
    CHECK(f != nullptr && g != nullptr, "both functions are present in HIR");
    if (f == nullptr || g == nullptr)
        return;

    CHECK_EQ(allocatedSlots(hir, *f).size(), 1u, "f allocates exactly one parameter slot");
    CHECK_EQ(allocatedSlots(hir, *g).size(), 1u, "g allocates exactly one parameter slot");
    CHECK(loadsOnlyOwnSlots(hir, *f), "f only loads slots it allocated");
    CHECK(loadsOnlyOwnSlots(hir, *g), "g only loads slots it allocated");
}

/// Expression ids referenced by `fn`, plus every id below them. `HirModule` exposes no
/// expression count, and an operand is always added before the instruction consuming it, so
/// scanning `0..=max(referenced id)` reaches every expression the function built, including
/// operands that never appear in an instruction list.
std::vector<hir::HirExprId> reachableExprIds(const hir::HirFunction &fn) {
    bool any               = false;
    hir::HirExprId highest = 0;
    const auto note        = [&](hir::HirExprId id) {
        if (id == hir::kInvalidHirExpr)
            return;
        highest = any ? std::max(highest, id) : id;
        any     = true;
    };
    for (const auto &block : fn.blocks) {
        for (auto inst : block.insts)
            note(inst);
        note(block.terminator);
    }
    std::vector<hir::HirExprId> ids;
    if (!any)
        return ids;
    for (hir::HirExprId id = 0; id <= highest; ++id)
        ids.push_back(id);
    return ids;
}

/// Integer values of every literal expression built for `fn`. The callers below use sources
/// containing integer literals only, so every literal's active union member is `i`.
std::vector<int64_t> integerLiterals(const hir::HirModule &hir, const hir::HirFunction &fn) {
    std::vector<int64_t> values;
    for (auto id : reachableExprIds(fn)) {
        const auto *literal = std::get_if<hir::HirLiteral>(&hir.getExpr(id));
        if (literal != nullptr)
            values.push_back(literal->i);
    }
    return values;
}

#ifdef ZITH_ENABLE_C_INTEROP
/// Every C pointer must lower to `?*T` (optional of pointer), never a bare `*T`: C pointers
/// are nullable, and `is null` requires an optional operand. The niche layout keeps the
/// LLVM representation identical to a bare pointer.
void test_c_pointers_lower_to_optional_pointer() {
    Workspace workspace;
    workspace.writeFile("fixture.h", "struct Holder { char *label; };\n"
                                     "char *c_dup(const char *text);\n");
    workspace.writeFile("main.zithm", "import \"fixture.h\"\n"
                                     "fn main(): i32 {\n"
                                     "    _ = raw c_dup(\"x\");\n"
                                     "    return 0;\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "C header with pointers lowers to HIR");

    const auto &types = session.types();
    const auto *dup   = findFunction(session.hirModule(), session.interner(), "c_dup");
    CHECK(dup != nullptr, "the imported C function is present in HIR");
    if (dup == nullptr)
        return;

    // `char *` return.
    CHECK_EQ(static_cast<int>(types.kindOf(dup->return_type)),
             static_cast<int>(types::TypeKind::Optional), "a C pointer return lowers to ?*T");
    const auto &ret = types.lookup(dup->return_type);
    if (const auto *opt = std::get_if<types::TypeOptional>(&ret)) {
        CHECK_EQ(static_cast<int>(types.kindOf(opt->inner)), static_cast<int>(types::TypeKind::Ptr),
                 "the ?T inner type is a pointer");
        CHECK(types.kindOf(opt->inner) != types::TypeKind::Optional,
              "the pointee is not itself wrapped, so no ??*T is built");
    }

    // `const char *` parameter.
    CHECK_EQ(dup->params.size(), 1u, "c_dup takes one parameter");
    if (!dup->params.empty()) {
        CHECK_EQ(static_cast<int>(types.kindOf(dup->params[0])),
                 static_cast<int>(types::TypeKind::Optional),
                 "a C pointer parameter lowers to ?*T");
    }
}
#endif // ZITH_ENABLE_C_INTEROP

void test_radix_literals_lower_to_their_value() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn main(): i32 {\n"
                                     "    let h: i32 = 0xFF;\n"
                                     "    let b: i32 = 0b101;\n"
                                     "    let o: i32 = 0c17;\n"
                                     "    return h;\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "radix literal lowering succeeds");

    const auto &hir  = session.hirModule();
    const auto *main = findFunction(hir, session.interner(), "main");
    CHECK(main != nullptr, "main function is present");
    if (main == nullptr)
        return;

    const auto values = integerLiterals(hir, *main);
    const auto has    = [&](int64_t v) {
        return std::find(values.begin(), values.end(), v) != values.end();
    };
    CHECK(has(255), "0xFF lowers to 255, not 0");
    CHECK(has(5), "0b101 lowers to 5, not 0");
    CHECK(has(15), "0c17 lowers to 15, not 0");
}

void test_enum_discriminant_honours_radix() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "enum Flag { Lo = 0x10, Hi = 0x20 }\n"
                                     "fn main(): Flag { Flag.Lo }\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "hex enum discriminant lowering succeeds");

    const auto &hir  = session.hirModule();
    const auto *main = findFunction(hir, session.interner(), "main");
    CHECK(main != nullptr, "main function is present");
    if (main == nullptr)
        return;

    bool found_16 = false;
    for (auto id : reachableExprIds(*main)) {
        const auto *value = std::get_if<hir::HirEnumValue>(&hir.getExpr(id));
        if (value != nullptr && value->value == 16)
            found_16 = true;
    }
    CHECK(found_16, "enum discriminant '= 0x10' lowers to 16");
}

void test_const_enum_discriminant_expressions_lower_to_values() {
    Workspace workspace;
    workspace.writeFile(
        "main.zithm",
        "enum Flag { ONE = 1, SHIFT = 1 << 4, OR = 1 |. 4, NEG = -1, PREV = SHIFT + 1 }\n"
        "fn main(): Flag { Flag.PREV }\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "constant enum discriminant expressions lower to HIR");

    const auto &hir  = session.hirModule();
    const auto *main = findFunction(hir, session.interner(), "main");
    CHECK(main != nullptr, "main function is present");
    if (main == nullptr)
        return;

    const auto &types = session.types();
    const auto enum_type =
        std::get_if<types::TypeEnum>(&types.lookup(types.lookupNamedType("Flag")));
    CHECK(enum_type != nullptr, "Flag lowers to an enum type");
    if (enum_type == nullptr)
        return;
    const auto *def = types.lookupEnumDef(enum_type->def_id);
    CHECK(def != nullptr, "Flag has a lowered enum definition");
    if (def == nullptr)
        return;
    const auto has = [&](std::string_view name, int64_t value) {
        for (const auto &variant : def->variants) {
            if (types.interner().lookup(variant.name) == name)
                return variant.discriminant == value;
        }
        return false;
    };
    CHECK(has("ONE", 1), "computed enum discriminant ONE lowers to 1");
    CHECK(has("SHIFT", 16), "computed enum discriminant SHIFT lowers to 16");
    CHECK(has("OR", 5), "computed enum discriminant OR lowers to 5");
    CHECK(has("NEG", -1), "computed enum discriminant NEG lowers to -1");
    CHECK(has("PREV", 17), "computed enum discriminant PREV lowers to 17");
}

void test_struct_literal_lowers_to_hir() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "struct Foo { x: i32, y: i32 }\n"
                                     "fn mk(): Foo { Foo { x: 1, y: 2 } }\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "struct literal lowering succeeds");

    const auto &hir = session.hirModule();
    const auto *mk  = findFunction(hir, session.interner(), "mk");
    CHECK(mk != nullptr, "mk function is present");
    if (mk != nullptr) {
        CHECK_EQ(countInstKind(hir, *mk, hir::HirExprKind::StructLiteral), 1u,
                 "struct literal lowers to exactly one StructLiteral node");
    }
}

void test_anonymous_pack_literal_lowers_to_hir() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn main() {\n"
                                     "    let op = |5, 5, 5|;\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "anonymous pack literal lowering succeeds");

    const auto &hir  = session.hirModule();
    const auto *main = findFunction(hir, session.interner(), "main");
    CHECK(main != nullptr, "main function is present");
    if (main != nullptr) {
        bool found_pack = false;
        for (auto id : reachableExprIds(*main)) {
            if (std::get_if<hir::HirStructLiteral>(&hir.getExpr(id)) != nullptr) {
                found_pack = true;
                break;
            }
        }
        CHECK(found_pack, "anonymous pack lowers to an aggregate literal node");
    }
}

void test_dot_field_read_lowers_to_hir_field() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "struct Foo { x: i32, y: i32 }\n"
                                     "fn get_x(f: Foo): i32 { f.x }\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "dot field read lowering succeeds");

    const auto &hir   = session.hirModule();
    const auto *get_x = findFunction(hir, session.interner(), "get_x");
    CHECK(get_x != nullptr, "get_x function is present");
    if (get_x != nullptr) {
        CHECK_EQ(countInstKind(hir, *get_x, hir::HirExprKind::Field), 1u,
                 "field access lowers to one Field node");
        bool found_intrinsic_field = false;
        for (const auto &block : get_x->blocks) {
            for (auto inst : block.insts) {
                if (const auto *f = std::get_if<hir::HirField>(&hir.getExpr(inst))) {
                    CHECK_EQ(f->index, 0u, "field 'x' maps to index 0");
                    found_intrinsic_field = f->safety == hir::HirSafetyMode::Intrinsic;
                }
            }
        }
        CHECK(found_intrinsic_field, "ordinary struct field access is intrinsically safe");
    }
}

void test_addrof_and_deref_lowers_to_hir_unary() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn round_trip(x: i32): i32 {\n"
                                     "    let p: *i32 = &x;\n"
                                     "    *p\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "addrof/deref lowering succeeds");

    const auto &hir = session.hirModule();
    const auto *fn  = findFunction(hir, session.interner(), "round_trip");
    CHECK(fn != nullptr, "round_trip function is present");
    if (fn != nullptr) {
        // Ref/Deref may appear as sub-expressions of slot-store operands rather than
        // standalone instructions; check slot-store values and direct instruction list.
        size_t ref_count   = 0;
        size_t deref_count = 0;
        auto checkUnary    = [&](hir::HirExprId eid) {
            if (const auto *u = std::get_if<hir::HirUnary>(&hir.getExpr(eid))) {
                if (u->op == hir::HirUnaryOp::Ref)
                    ++ref_count;
                if (u->op == hir::HirUnaryOp::Deref)
                    ++deref_count;
                // Also check the operand of the unary (in case of nested ops)
                if (const auto *inner = std::get_if<hir::HirUnary>(&hir.getExpr(u->operand))) {
                    if (inner->op == hir::HirUnaryOp::Ref)
                        ++ref_count;
                    if (inner->op == hir::HirUnaryOp::Deref)
                        ++deref_count;
                }
            }
        };
        for (const auto &block : fn->blocks) {
            for (auto inst : block.insts) {
                checkUnary(inst);
                // Also inspect operands of slot stores (where &x initializers land)
                if (const auto *store = std::get_if<hir::HirSlotStore>(&hir.getExpr(inst)))
                    checkUnary(store->value);
            }
        }
        CHECK(ref_count >= 1u, "at least one Ref node for '&x'");
        CHECK(deref_count >= 1u, "at least one Deref node for '*p'");
    }
}

void test_arrow_access_lowers_to_deref_then_field() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "struct Foo { x: i32, y: i32 }\n"
                                     "fn via_ptr(p: *Foo): i32 { p->x }\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "arrow access lowering succeeds");

    const auto &hir     = session.hirModule();
    const auto *via_ptr = findFunction(hir, session.interner(), "via_ptr");
    CHECK(via_ptr != nullptr, "via_ptr function is present");
    if (via_ptr != nullptr) {
        CHECK_EQ(countInstKind(hir, *via_ptr, hir::HirExprKind::Field), 1u,
                 "arrow lowers to exactly one Field node");
        for (const auto &block : via_ptr->blocks) {
            for (auto inst : block.insts) {
                if (const auto *f = std::get_if<hir::HirField>(&hir.getExpr(inst)))
                    CHECK_EQ(f->index, 0u, "arrow->x maps to field index 0");
            }
        }
    }
}

void test_numeric_cast_lowers_to_hir_cast() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn widen(n: i32): f64 { n as f64 }\n"
                                     "fn main(): i32 { 0 }\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "numeric cast lowering succeeds");

    const auto &hir   = session.hirModule();
    const auto *widen = findFunction(hir, session.interner(), "widen");
    CHECK(widen != nullptr, "widen function is present");
    if (widen != nullptr) {
        CHECK_EQ(countInstKind(hir, *widen, hir::HirExprKind::Cast), 1u,
                 "'as' lowers to exactly one HirCast node");
    }
}

void test_opaque_casts_lower_to_hir_nodes() {
    Workspace workspace;
    workspace.writeFile("main.zithm",
                        "fn erased(value: i32): opaque { value as opaque }\n"
                        "fn checks(value: opaque): bool { value is i32 }\n"
                        "fn extracts(value: opaque): ?i32 { value as i32 }\n"
                        "fn raw_extracts(value: opaque): i32 { raw value as i32 }\n"
                        "fn raw_ptr(value: opaque): raw opaque { value as raw opaque }\n"
                        "fn main(): i32 { 0 }\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "bare 'opaque' operations lower to HIR");

    const auto &hir = session.hirModule();
    CHECK(countExprKind(hir, hir::HirExprKind::MakeOpaque) > 0u, "erasure lowers to HirMakeOpaque");
    CHECK(countExprKind(hir, hir::HirExprKind::OpaqueCheck) > 0u,
          "'opaque is T' lowers to HirOpaqueCheck");
    CHECK(countExprKind(hir, hir::HirExprKind::OpaqueCast) > 0u,
          "extraction lowers to HirOpaqueCast");

    bool saw_canonical_id = false;
    for (size_t id = 0; id < hir.exprCount(); ++id) {
        const auto &expr = hir.getExpr(static_cast<hir::HirExprId>(id));
        const auto *make = std::get_if<hir::HirMakeOpaque>(&expr);
        const auto *cast = std::get_if<hir::HirOpaqueCast>(&expr);
        if (make != nullptr && make->canonical_id != types::kInvalidCanonicalId)
            saw_canonical_id = true;
        if (cast != nullptr && cast->canonical_id != types::kInvalidCanonicalId)
            saw_canonical_id = true;
    }
    CHECK(saw_canonical_id, "opaque HIR nodes carry a canonical type id");

    const auto *raw_extracts = findFunction(hir, session.interner(), "raw_extracts");
    CHECK(raw_extracts != nullptr, "raw extraction function is present");
    if (raw_extracts != nullptr) {
        bool saw_raw = false;
        for (size_t id = 0; id < hir.exprCount(); ++id) {
            const auto *cast =
                std::get_if<hir::HirOpaqueCast>(&hir.getExpr(static_cast<hir::HirExprId>(id)));
            if (cast != nullptr && cast->safety == hir::HirSafetyMode::Raw)
                saw_raw = true;
        }
        CHECK(saw_raw, "raw extraction lowers to an unchecked HirOpaqueCast");
    }

    const auto *raw_ptr = findFunction(hir, session.interner(), "raw_ptr");
    CHECK(raw_ptr != nullptr, "bare opaque to raw opaque function is present");
    if (raw_ptr != nullptr) {
        bool saw_ptr        = false;
        bool correct_target = false;
        for (size_t id = 0; id < hir.exprCount(); ++id) {
            const auto *cast =
                std::get_if<hir::HirOpaqueCast>(&hir.getExpr(static_cast<hir::HirExprId>(id)));
            if (cast == nullptr || !cast->returns_ptr)
                continue;
            saw_ptr        = true;
            correct_target = cast->to == cast->result_type && cast->result_type != cast->from &&
                             cast->from == cast->opaque_type;
        }
        CHECK(saw_ptr, "opaque as raw opaque lowers to a pointer-returning HirOpaqueCast");
        CHECK(correct_target, "the raw opaque cast targets pointer-to-void, not OpaqueTagged");
    }
}

void test_canonical_type_lowers_to_hir_node() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "struct Pair { a: i64, b: i8 }\n"
                                     "fn main(): u128 {\n"
                                     "    @canonicalType(Pair)\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "'@canonicalType(T)' lowers to a dedicated HIR node");

    const auto &hir = session.hirModule();
    CHECK(countExprKind(hir, hir::HirExprKind::CanonicalType) >= 1u,
          "'@canonicalType(T)' emits HirCanonicalType");

    for (size_t id = 0; id < hir.exprCount(); ++id) {
        const auto *canonical =
            std::get_if<hir::HirCanonicalType>(&hir.getExpr(static_cast<hir::HirExprId>(id)));
        if (canonical == nullptr)
            continue;
        CHECK(canonical->canonical_id != types::kInvalidCanonicalId,
              "canonical type id is populated");
        CHECK(canonical->type != types::kInvalidType, "canonical type expression has a HIR type");
    }
}

void test_implicit_opaque_coercion_and_narrow_lowering() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn consume(v: opaque): bool {\n"
                                     "    if (v is u32) { let x: u32 = v; return x == 42u32; }\n"
                                     "    return false;\n"
                                     "}\n"
                                     "fn make(): opaque { 42u32 }\n"
                                     "fn main(): bool { let o: opaque = 1u32; consume(o) }\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "implicit concrete-to-opaque lowering succeeds");

    const auto &hir = session.hirModule();
    CHECK(countExprKind(hir, hir::HirExprKind::MakeOpaque) >= 2u,
          "implicit binding and return coercions emit HirMakeOpaque");

    bool saw_narrowed_cast = false;
    for (size_t id = 0; id < hir.exprCount(); ++id) {
        const auto &expr = hir.getExpr(static_cast<hir::HirExprId>(id));
        if (const auto *cast = std::get_if<hir::HirOpaqueCast>(&expr)) {
            if (cast->safety == hir::HirSafetyMode::Guarded && cast->from == cast->opaque_type &&
                cast->from != cast->to &&
                cast->to == cast->result_type)
                saw_narrowed_cast = true;
        }
    }
    CHECK(saw_narrowed_cast,
          "reading an opaque local inside 'is T' lowers to an unchecked HirOpaqueCast");
}

void test_opaque_canonical_tags_persist_across_sessions() {
    IsolatedWorkspace workspace;
    const std::string source =
        "fn make(): opaque { 42u32 }\n"
        "fn checks(v: opaque): bool { v is u32 }\n"
        "fn main(): ?u32 { let v = make(); if (checks(v)) { raw v as u32 } else { null } }\n";
    workspace.writeFile("main.zithm", source);

    memory::Arena firstArena;
    Options firstOptions(firstArena);
    firstOptions.noCache     = false;
    firstOptions.targetStage = session::Stage::Cached;
    session::CompilationSession first(firstOptions, (workspace.root / "main.zithm").string());
    first.setBuffered(true);
    const bool first_ok = first.run();
    CHECK(first_ok, "first opaque session lowers and writes the persistent cache");
    CHECK(first.hirModule().getFnCount() > 0u, "first session produced HIR");

    memory::Arena arena;
    Options options(arena);
    options.noCache = false;
    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "second opaque session hydrates from the persistent artifact");
    const auto &hir = session.hirModule();

    bool sawMake      = false;
    bool sawCheck     = false;
    uint32_t makeTag  = 0;
    uint32_t checkTag = 0;
    for (size_t id = 0; id < hir.exprCount(); ++id) {
        const auto &expr = hir.getExpr(static_cast<hir::HirExprId>(id));
        if (const auto *make = std::get_if<hir::HirMakeOpaque>(&expr)) {
            sawMake = true;
            makeTag = make->type_id;
        }
        if (const auto *check = std::get_if<hir::HirOpaqueCheck>(&expr)) {
            sawCheck = true;
            checkTag = check->type_id;
        }
    }
    CHECK(sawMake && sawCheck, "hydrated opaque HIR contains make and check nodes");
    CHECK(makeTag != 0u && makeTag == checkTag,
          "hydrated opaque nodes agree on the stable runtime tag");
}

void test_imported_type_canonical_id_persists_across_sessions() {
    IsolatedWorkspace workspace;
    workspace.writeFile("dep.zithm", "pub struct Counter {\n"
                                    "    pub n: i32,\n"
                                    "    pub flag: bool,\n"
                                    "}\n");
    workspace.writeFile("main.zithm", "from dep\n"
                                     "\n"
                                     "fn id(): u128 { @canonicalType(Counter) }\n"
                                     "fn make(): opaque { Counter { n: 1, flag: false } }\n"
                                     "fn main(): i32 { 0 }\n");

    memory::Arena firstArena;
    Options firstOptions(firstArena);
    firstOptions.noCache     = false;
    firstOptions.targetStage = session::Stage::Cached;
    session::CompilationSession first(firstOptions, (workspace.root / "main.zithm").string());
    first.setBuffered(true);
    const bool first_ok = first.run();
    CHECK(first_ok, "imported opaque first session lowers and writes the cache");

    memory::Arena arena;
    Options options(arena);
    options.noCache = false;
    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "imported opaque second session hydrates from the cache");

    const auto &hir      = session.hirModule();
    bool sawCanonical    = false;
    bool sawMake         = false;
    uint64_t canonicalHi = 0;
    uint64_t canonicalLo = 0;
    for (size_t id = 0; id < hir.exprCount(); ++id) {
        const auto &expr = hir.getExpr(static_cast<hir::HirExprId>(id));
        if (const auto *canonical = std::get_if<hir::HirCanonicalType>(&expr)) {
            sawCanonical = true;
            canonicalHi  = canonical->canonical_id.hi;
            canonicalLo  = canonical->canonical_id.lo;
        }
        if (const auto *make = std::get_if<hir::HirMakeOpaque>(&expr))
            sawMake = true;
    }
    CHECK(sawCanonical, "hydrated HIR keeps the imported canonical type intrinsic");
    CHECK(sawMake, "hydrated HIR keeps the imported opaque make");
    CHECK(canonicalHi != 0ull || canonicalLo != 0ull, "canonical type id is stable and non-zero");
}

void test_imported_type_canonical_id_uses_defining_module() {
    Workspace workspace;
    workspace.writeFile("dep.zithm", "pub struct Counter {\n"
                                    "    pub n: i32,\n"
                                    "    pub flag: bool,\n"
                                    "}\n"
                                    "pub fn dep_id(): u128 { @canonicalType(Counter) }\n");
    workspace.writeFile("main.zithm", "from dep\n"
                                     "\n"
                                     "pub fn main_id(): u128 { @canonicalType(Counter) }\n"
                                     "fn main(): i32 { 0 }\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "defining-module canonical type id session lowers successfully");

    const auto &hir = session.hirModule();
    bool sawDepId   = false;
    bool sawMainId  = false;
    uint64_t depHi  = 0;
    uint64_t depLo  = 0;
    for (size_t id = 0; id < hir.exprCount(); ++id) {
        const auto *canonical =
            std::get_if<hir::HirCanonicalType>(&hir.getExpr(static_cast<hir::HirExprId>(id)));
        if (canonical == nullptr)
            continue;
        if (!sawDepId) {
            sawDepId = true;
            depHi    = canonical->canonical_id.hi;
            depLo    = canonical->canonical_id.lo;
            continue;
        }
        sawMainId = true;
        CHECK(canonical->canonical_id.hi == depHi && canonical->canonical_id.lo == depLo,
              "canonical type id is identical in defining and consumer modules");
        CHECK(canonical->canonical_id != types::kInvalidCanonicalId,
              "canonical type id is populated for the consumer");
        break;
    }
    CHECK(sawDepId && sawMainId, "canonical type intrinsic is lowered from both modules");
}

void test_is_null_on_pointer_optional_uses_niche() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn empty(p: ?*i32): bool { p is null }\n"
                                     "fn main(): i32 { 0 }\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "'is null' on ?*T lowering succeeds");

    const auto &hir   = session.hirModule();
    const auto *empty = findFunction(hir, session.interner(), "empty");
    CHECK(empty != nullptr, "empty function is present");
    if (empty != nullptr) {
        bool found_eq_against_none = false;
        for (const auto &block : empty->blocks) {
            for (auto inst : block.insts) {
                const auto *binary = std::get_if<hir::HirBinary>(&hir.getExpr(inst));
                if (binary == nullptr || binary->op != hir::HirBinaryOp::Eq)
                    continue;
                if (hir::exprKind(hir.getExpr(binary->rhs)) == hir::HirExprKind::MakeNone)
                    found_eq_against_none = true;
            }
        }
        CHECK(found_eq_against_none,
              "'is null' on ?*T lowers to an equality comparison against MakeNone");
    }
}

void test_is_null_on_value_optional_reads_tag() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn empty(v: ?i32): bool { v is null }\n"
                                     "fn main(): i32 { 0 }\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "'is null' on ?T lowering succeeds");

    const auto &hir   = session.hirModule();
    const auto *empty = findFunction(hir, session.interner(), "empty");
    CHECK(empty != nullptr, "empty function is present");
    if (empty != nullptr) {
        bool found_not_of_tag = false;
        for (const auto &block : empty->blocks) {
            for (auto inst : block.insts) {
                const auto *unary = std::get_if<hir::HirUnary>(&hir.getExpr(inst));
                if (unary == nullptr || unary->op != hir::HirUnaryOp::Not)
                    continue;
                const auto *field = std::get_if<hir::HirField>(&hir.getExpr(unary->operand));
                if (field != nullptr && field->index == 1u &&
                    field->safety == hir::HirSafetyMode::Intrinsic)
                    found_not_of_tag = true;
            }
        }
        CHECK(found_not_of_tag,
              "'is null' on ?T negates the intrinsically safe discriminant at field index 1");
    }
}

void test_optional_boolean_on_value_reads_tag() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn present(v: ?i32): i32 {\n"
                                     "    let x: ?i32 = 7;\n"
                                     "    if (x) { return 1; }\n"
                                     "    return 0;\n"
                                     "}\n"
                                     "fn main(): i32 { present(7) }\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "implicit ?T boolean lowering succeeds");

    const auto &hir  = session.hirModule();
    const auto *main = findFunction(hir, session.interner(), "present");
    CHECK(main != nullptr, "present function is present");
    if (main != nullptr) {
        bool read_tag = false;
        for (const auto &block : main->blocks) {
            for (auto inst : block.insts) {
                const auto *field = std::get_if<hir::HirField>(&hir.getExpr(inst));
                if (field != nullptr && field->index == 1u)
                    read_tag = true;
            }
            if (block.terminator == hir::kInvalidHirExpr)
                continue;
            const auto *branch = std::get_if<hir::HirBranch>(&hir.getExpr(block.terminator));
            if (branch != nullptr) {
                const auto *cond = std::get_if<hir::HirField>(&hir.getExpr(branch->cond));
                if (cond != nullptr && cond->index == 1u)
                    read_tag = true;
            }
        }
        CHECK(read_tag, "implicit ?T conditions read the optional tag at field index 1");
        CHECK_EQ(countTerminatorKind(hir, *main, hir::HirExprKind::Ret), 2u,
                 "implicit ?T conditions do not lower to optional returns");
    }
}

void test_optional_boolean_on_pointer_uses_niche() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn present(p: ?*i32): i32 {\n"
                                     "    if (p) { return 1; }\n"
                                     "    return 0;\n"
                                     "}\n"
                                     "fn main(): i32 { 0 }\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "implicit ?*T boolean lowering succeeds");

    const auto &hir  = session.hirModule();
    const auto *main = findFunction(hir, session.interner(), "present");
    CHECK(main != nullptr, "present function is present");
    if (main != nullptr) {
        bool found_ne_against_none = false;
        for (const auto &block : main->blocks) {
            if (block.terminator == hir::kInvalidHirExpr)
                continue;
            const auto *branch = std::get_if<hir::HirBranch>(&hir.getExpr(block.terminator));
            if (branch != nullptr) {
                const auto *binary = std::get_if<hir::HirBinary>(&hir.getExpr(branch->cond));
                if (binary != nullptr && binary->op == hir::HirBinaryOp::Ne &&
                    hir::exprKind(hir.getExpr(binary->rhs)) == hir::HirExprKind::MakeNone) {
                    found_ne_against_none = true;
                }
            }
            for (auto inst : block.insts) {
                const auto *binary = std::get_if<hir::HirBinary>(&hir.getExpr(inst));
                if (binary == nullptr || binary->op != hir::HirBinaryOp::Ne)
                    continue;
                if (hir::exprKind(hir.getExpr(binary->rhs)) == hir::HirExprKind::MakeNone)
                    found_ne_against_none = true;
            }
        }
        CHECK(found_ne_against_none,
              "implicit ?*T conditions lower to a non-null comparison against MakeNone");
    }
}

void test_for_condition_lowers_like_while() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn main(): i32 {\n"
                                     "    var i: i32 = 0;\n"
                                     "    for (i < 10) {\n"
                                     "        i = i + 1;\n"
                                     "    }\n"
                                     "    i\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "'for (cond)' lowering succeeds");

    const auto &hir  = session.hirModule();
    const auto *main = findFunction(hir, session.interner(), "main");
    CHECK(main != nullptr, "loop function is present in HIR");
    if (main != nullptr) {
        CHECK(main->blocks.size() >= 4u,
              "'for (cond)' creates entry, header, body, and exit blocks like 'while'");
        CHECK(countTerminatorKind(hir, *main, hir::HirExprKind::Branch) >= 1u,
              "'for (cond)' emits a branch terminator for the condition");
        CHECK(countTerminatorKind(hir, *main, hir::HirExprKind::Jump) >= 2u,
              "'for (cond)' emits jumps for the back-edge and entry");
    }
}

void test_static_method_call_has_resolved_callee_only() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "struct Point { x: i32 }\n"
                                     "trait Sample {}\n"
                                     "implement Point as Sample {\n"
                                     "    fn foo(): i32 { 7 }\n"
                                     "}\n"
                                     "fn main(): i32 {\n"
                                     "    Point.foo()\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "static method call lowers to HIR without a receiver");

    const auto &hir  = session.hirModule();
    const auto *main = findFunction(hir, session.interner(), "main");
    CHECK(main != nullptr, "main function is present in HIR");
    if (main == nullptr)
        return;
    for (const auto &block : main->blocks) {
        for (auto inst : block.insts) {
            const auto &expr = hir.getExpr(inst);
            const auto *call = std::get_if<hir::HirCall>(&expr);
            if (call == nullptr)
                continue;
            CHECK(call->callee == hir::kInvalidHirExpr,
                  "static method call keeps callee = kInvalidHirExpr");
            CHECK(call->resolved_fn != hir::kInvalidHirFunction,
                  "static method call carries a resolved HIR function id");
            CHECK_EQ(call->args.size(), 0u, "static method call passes no receiver");
        }
    }
}

void test_state_machine_lowers_to_hir() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "state Loop(n: i32): i32 {\n"
                                     "    if (n < 10) {\n"
                                     "        jump Loop(n + 1);\n"
                                     "    }\n"
                                     "    return n;\n"
                                     "}\n"
                                     "fn main(): i32 {\n"
                                     "    let result: i32 = dock Loop(0);\n"
                                     "    return result;\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "state machine lowers to HIR");

    const auto &hir  = session.hirModule();
    const auto *loop = findFunction(hir, session.interner(), "Loop");
    const auto *main = findFunction(hir, session.interner(), "main");
    CHECK(loop != nullptr, "state function is present in HIR");
    CHECK(main != nullptr, "main function is present in HIR");
    if (loop != nullptr) {
        CHECK(loop->isState, "state HIR function records isState");
        CHECK(loop->machineId != 0u, "state HIR function records a machine id");
        CHECK(countTerminatorKind(hir, *loop, hir::HirExprKind::StateTailCall) >= 1u,
              "state jump lowers to a StateTailCall terminator");
    }
    if (main != nullptr) {
        CHECK(!main->isState, "ordinary functions remain non-state");
        CHECK_EQ(countExprKind(hir, hir::HirExprKind::Call), 1u,
                 "dock lowers to a plain call expression");
    }
}

void test_state_tail_call_is_direct_and_musttail() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "state Start(v: i32): i32 {\n"
                                     "    jump Done(v + 1);\n"
                                     "}\n"
                                     "state Done(v: i32): i32 {\n"
                                     "    return v;\n"
                                     "}\n"
                                     "fn main(): i32 {\n"
                                     "    return dock Start(41);\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "state tail call lowers to HIR");

    const auto &hir   = session.hirModule();
    const auto *start = findFunction(hir, session.interner(), "Start");
    CHECK(start != nullptr, "start state is present in HIR");
    if (start == nullptr)
        return;

    bool saw_tail_call = false;
    for (const auto &block : start->blocks) {
        if (block.terminator == hir::kInvalidHirExpr)
            continue;
        const auto &expr = hir.getExpr(block.terminator);
        if (const auto *tail = std::get_if<hir::HirStateTailCall>(&expr)) {
            saw_tail_call = true;
            CHECK(tail->call.musttail, "state tail call carries the musttail bit");
            CHECK(tail->call.usesTailCC, "state tail call carries the tailcc bit");
            CHECK(tail->call.resolved_fn != hir::kInvalidHirFunction,
                  "state tail call resolves a direct HIR function id");
            CHECK(tail->call.callee == hir::kInvalidHirExpr,
                  "state tail call has no indirect callee expression");
        }
    }
    CHECK(saw_tail_call, "jump lowers to a terminating StateTailCall");
}

void test_multiple_states_share_machine_id_by_prototype() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "state Add(x: i32, y: i32): i32 {\n"
                                     "    jump Done(x + y, y);\n"
                                     "}\n"
                                     "state Done(x: i32, y: i32): i32 {\n"
                                     "    return x + y;\n"
                                     "}\n"
                                     "fn first(): i32 {\n"
                                     "    return dock Add(3, 4);\n"
                                     "}\n"
                                     "fn second(): i32 {\n"
                                     "    return dock Done(5, 6);\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "same-prototype states lower into one machine");

    const auto &hir   = session.hirModule();
    const auto *add   = findFunction(hir, session.interner(), "Add");
    const auto *done  = findFunction(hir, session.interner(), "Done");
    const auto *first = findFunction(hir, session.interner(), "first");
    CHECK(add != nullptr && done != nullptr && first != nullptr,
          "state machine functions and caller are present in HIR");
    if (add == nullptr || done == nullptr || first == nullptr)
        return;
    CHECK(add->isState && done->isState, "both machine members are state functions");
    CHECK_EQ(add->machineId, done->machineId, "same-prototype states share a machine id");
    CHECK_EQ(countExprKind(hir, hir::HirExprKind::Call), 2u,
             "each dock remains a plain call expression in its caller");
}

void test_diverging_state_parameters_share_machine_id() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "state Start(n: i32): i32 {\n"
                                     "    jump Done(n, n + 1);\n"
                                     "}\n"
                                     "state Done(n: i32, acc: i32): i32 {\n"
                                     "    return acc;\n"
                                     "}\n"
                                     "fn main(): i32 {\n"
                                     "    return dock Start(41);\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "states with diverging parameters lower into one machine");

    const auto &hir   = session.hirModule();
    const auto *start = findFunction(hir, session.interner(), "Start");
    const auto *done  = findFunction(hir, session.interner(), "Done");
    CHECK(start != nullptr && done != nullptr, "diverging state functions are present in HIR");
    if (start == nullptr || done == nullptr)
        return;
    CHECK_EQ(start->machineId, done->machineId, "diverging states share a machine id");
    CHECK(start->usesTailCC && done->usesTailCC, "state functions carry the tailcc flag");
    CHECK_EQ(start->machineReturnType, done->machineReturnType,
             "state functions carry the shared machine return type");
}

void test_local_states_qualify_and_lower_without_collisions() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn first(): i32 {\n"
                                     "    state Step(n: i32): i32 {\n"
                                     "        if (n == 0) {\n"
                                     "            return 1;\n"
                                     "        }\n"
                                     "        jump Step(n - 1);\n"
                                     "    }\n"
                                     "    return dock Step(3);\n"
                                     "}\n"
                                     "fn second(): i32 {\n"
                                     "    state Step(n: i32): i32 {\n"
                                     "        if (n == 0) {\n"
                                     "            return 2;\n"
                                     "        }\n"
                                     "        jump Step(n - 1);\n"
                                     "    }\n"
                                     "    return dock Step(3);\n"
                                     "}\n"
                                     "fn main(): i32 {\n"
                                     "    return first() + second();\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "local states with identical names lower to HIR");

    const auto &hir         = session.hirModule();
    size_t first_local      = 0;
    size_t second_local     = 0;
    uint32_t first_machine  = 0;
    uint32_t second_machine = 0;
    for (size_t i = 0; i < hir.getFnCount(); ++i) {
        const auto &fn = hir.getFn(i);
        if (!fn.isState)
            continue;
        const auto name = internedName(session.interner(), fn.name);
        if (name.find("first$local.Step") != std::string_view::npos) {
            ++first_local;
            first_machine = fn.machineId;
            CHECK(countTerminatorKind(hir, fn, hir::HirExprKind::StateTailCall) >= 1u,
                  "local state jump lowers to a StateTailCall");
        } else if (name.find("second$local.Step") != std::string_view::npos) {
            ++second_local;
            second_machine = fn.machineId;
        }
    }
    CHECK_EQ(first_local, 1u, "first local state exists with an owner-qualified HIR name");
    CHECK_EQ(second_local, 1u, "second local state exists with an owner-qualified HIR name");
    CHECK(first_machine != 0u && second_machine != 0u && first_machine != second_machine,
          "local machines in different parents are distinct");

    const auto *first  = findFunction(hir, session.interner(), "first");
    const auto *second = findFunction(hir, session.interner(), "second");
    CHECK(first != nullptr && second != nullptr, "parent functions lower normally");
}

void test_generic_function_lowers_to_concrete_instances() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn identity<T>(x: T): T { return x; }\n"
                                     "fn main(): i32 {\n"
                                     "    _ = identity<i32>(7);\n"
                                     "    return identity(9);\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "generic function with explicit and inferred type arguments lowers to HIR");

    const auto &hir  = session.hirModule();
    const auto *main = findFunction(hir, session.interner(), "main");
    CHECK(main != nullptr, "main is present after generic lowering");
    if (main == nullptr)
        return;

    size_t call_count       = 0;
    hir::HirFunctionId resolved = hir::kInvalidHirFunction;
    const auto countCall    = [&](hir::HirExprId id) {
        if (id == hir::kInvalidHirExpr)
            return;
        const auto *call = std::get_if<hir::HirCall>(&hir.getExpr(id));
        if (call == nullptr)
            return;
        ++call_count;
        resolved = call->resolved_fn;
        CHECK(call->callee == hir::kInvalidHirExpr ||
                  call->resolved_fn != hir::kInvalidHirFunction,
                 "generic call has a resolved concrete symbol or a valid callee expr");
    };
    for (const auto &block : main->blocks) {
        for (auto inst : block.insts)
            countCall(inst);
        if (block.terminator != hir::kInvalidHirExpr) {
            if (const auto *ret = std::get_if<hir::HirRet>(&hir.getExpr(block.terminator)))
                countCall(ret->value);
        }
    }
    CHECK_EQ(call_count, 2u, "both generic calls remain in main");
    CHECK(resolved != hir::kInvalidHirFunction,
          "at least one generic call resolves to the instance");

    const auto *identity = findFunction(hir, session.interner(), "identity<i32>");
    CHECK(identity != nullptr,
          "the generic function is monomorphized into an identity<i32> instance");
    if (identity != nullptr) {
        // One concrete parameter slot, one return terminator; body return type is the
        // concrete i32 lowered to the integer type id used by main.
        CHECK_EQ(allocatedSlots(hir, *identity).size(), 1u,
                 "identity<i32> allocates only its concrete parameter");
        CHECK_EQ(countTerminatorKind(hir, *identity, hir::HirExprKind::Ret), 1u,
                 "identity<i32> returns from its concrete body");
    }
}

void test_generic_template_survives_persistent_cache() {
    IsolatedWorkspace workspace;
    workspace.writeFile("main.zithm", "pub fn identity<T>(value: T): T { return value; }\n"
                                     "fn main(): i32 { return identity(9); }\n");

    memory::Arena first_arena;
    Options first_options(first_arena);
    first_options.noCache     = false;
    first_options.targetStage = session::Stage::Cached;
    session::CompilationSession first(first_options, (workspace.root / "main.zithm").string());
    first.setBuffered(true);
    CHECK(first.run(), "cold generic compilation writes a persistent artifact");

    const auto *written = first.persistentArtifact();
    CHECK(written != nullptr, "cold generic compilation exposes its artifact");
    if (written == nullptr)
        return;
    CHECK_EQ(written->templates.size(), 1u,
             "artifact stores the generic function blueprint");
    if (written->templates.size() != 1u)
        return;
    CHECK_EQ(written->templates[0].name, "identity",
             "artifact stores the generic function's canonical name");
    CHECK_EQ(written->templates[0].params.size(), 1u,
             "artifact stores the generic parameter list");
    CHECK_EQ(written->templates[0].params[0].name, "T",
             "artifact stores the generic parameter name");
    CHECK(written->templates[0].declaration_index < written->decls.size(),
          "template stores an explicit declaration index");
    CHECK_EQ(written->templates[0].function_index, ~uint32_t{0},
             "template without a shared HIR body stores the invalid function sentinel");
    CHECK(written->templates[0].return_type_id < written->types.size(),
          "template stores its owned return type");
    CHECK_EQ(written->templates[0].param_type_ids.size(), 1u,
             "template stores its owned function parameter type");
    CHECK_EQ(written->templates[0].param_name_ids.size(), 1u,
             "template stores its owned function parameter name");
    if (written->templates[0].declaration_index < written->decls.size()) {
        const auto &owner = written->decls[written->templates[0].declaration_index];
        CHECK_EQ(owner.template_index, 0u,
                 "declaration and template retain their reciprocal explicit relation");
    }

    memory::Arena second_arena;
    Options second_options(second_arena);
    second_options.noCache     = false;
    second_options.targetStage = session::Stage::HirLowered;
    session::CompilationSession second(second_options, (workspace.root / "main.zithm").string());
    second.setBuffered(true);
    CHECK(second.runTo(session::Stage::HirLowered),
          "warm generic compilation reads the persistent artifact");

    const auto *hydrated = second.persistentArtifact();
    CHECK(hydrated != nullptr, "warm generic compilation exposes its artifact");
    if (hydrated != nullptr)
        CHECK_EQ(hydrated->templates.size(), 1u,
                 "cache reader preserves the generic function blueprint");
    CHECK_EQ(second.hirModule().templateCount(), 1u,
             "cache hydration reconstructs the HIR template table");
    if (second.hirModule().templateCount() == 1u) {
        const auto &template_record = second.hirModule().getTemplate(0);
        CHECK_EQ(template_record.declaration_id,
                 written->templates[0].declaration_index,
                 "hydrated HIR preserves the explicit template declaration ID");
        CHECK_EQ(template_record.params.size(), 1u,
                 "hydrated HIR preserves generic parameters");
        CHECK_EQ(template_record.parameter_types.size(), 1u,
                 "hydrated HIR preserves the owned function signature");
    }
}

void test_alias_qualified_free_generic_self_is_not_receiver() {
    Workspace workspace;
    workspace.writeFile("dep.zithm", "pub fn identity<T>(self: T): T { return self; }\n");
    workspace.writeFile("main.zithm", "import dep as da\n"
                                     "fn main(): i32 {\n"
                                     "    return da.identity<i32>(41);\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "alias-qualified free generic call with a self-named parameter lowers");

    const auto &hir  = session.hirModule();
    const auto *main = findFunction(hir, session.interner(), "main");
    CHECK(main != nullptr, "main is present after lowering the alias-qualified call");
    if (main == nullptr)
        return;

    const hir::HirCall *call = nullptr;
    for (const auto &block : main->blocks) {
        for (const auto inst : block.insts) {
            if (const auto *candidate = std::get_if<hir::HirCall>(&hir.getExpr(inst))) {
                call = candidate;
                break;
            }
        }
        if (call == nullptr && block.terminator != hir::kInvalidHirExpr) {
            if (const auto *ret = std::get_if<hir::HirRet>(&hir.getExpr(block.terminator))) {
                if (ret->value != hir::kInvalidHirExpr)
                    call = std::get_if<hir::HirCall>(&hir.getExpr(ret->value));
            }
        }
        if (call != nullptr)
            break;
    }

    CHECK(call != nullptr, "alias-qualified generic call is represented by HirCall");
    if (call != nullptr) {
        CHECK_EQ(call->args.size(), 1u,
                 "the free function receives only its explicit argument, not the module alias");
    }
}

void test_generic_bound_and_value_param_lower_to_hir() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "trait Foo {}\n"
                                     "interface Transform { [x]: i32 }\n"
                                     "struct Temple { x: i32 }\n"
                                     "struct Point { x: i32 }\n"
                                     "implement Temple as Foo {}\n"
                                     "fn foolish<T: Foo>(a: T): i32 { return 1; }\n"
                                     "fn interLab<T: Transform>(a: T): i32 { return a.x; }\n"
                                     "fn main(): i32 {\n"
                                     "    let p = Point { x: 7 };\n"
                                     "    return foolish<Temple>(Temple { x: 1 }) + interLab(p);\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "generic bound declarations lower without unknown-type diagnostics");

    const auto &hir = session.hirModule();
    for (size_t i = 0; i < hir.getFnCount(); ++i) {
        const auto &fn = hir.getFn(i);
        if (fn.param_names.empty())
            continue;
        CHECK_EQ(fn.param_slots.size(), fn.param_names.size(),
                 "every HIR function carries a slot id for each parameter");
    }
}

void test_interface_method_bound_lower_to_hir() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "interface Positioned {\n"
                                     "    x: i32,\n"
                                     "    fn getX(self): i32\n"
                                     "}\n"
                                     "struct Point {\n"
                                     "    x: i32,\n"
                                     "    fn getX(self): i32 { return self.x; }\n"
                                     "}\n"
                                     "fn transform<T: Positioned>(p: T): i32 { return p.getX(); }\n"
                                     "fn main(): i32 {\n"
                                     "    let p = Point { x: 7 };\n"
                                     "    return transform<Point>(p);\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "interface method in a generic bound lowers to HIR");
    const auto &hir       = session.hirModule();
    const auto *transform = findFunction(hir, session.interner(), "transform<Point>");
    CHECK(transform != nullptr, "generic transform instance is present");
    bool calls_point_getx = false;
    for (size_t index = 0; index < hir.exprCount(); ++index) {
        const auto &expr = hir.getExpr(static_cast<hir::HirExprId>(index));
        if (!std::holds_alternative<hir::HirCall>(expr))
            continue;
        const auto &call = std::get<hir::HirCall>(expr);
        if (call.resolved_fn == hir::kInvalidHirFunction)
            continue;
        for (size_t fn_index = 0; fn_index < hir.getFnCount(); ++fn_index) {
            const auto &fn = hir.getFn(fn_index);
            if (static_cast<hir::HirFunctionId>(fn_index) != call.resolved_fn)
                continue;
            const std::string_view fn_name(session.interner().lookup(fn.name));
            if (fn_name.find("Point.getX") != std::string_view::npos ||
                fn_name.find(".getX") != std::string_view::npos) {
                calls_point_getx = true;
                break;
            }
        }
        if (calls_point_getx)
            break;
    }
    CHECK(calls_point_getx, "generic transform call resolves to the concrete Point.getX");
}

// The owner's ordered method list is the frontend-owned `DeclId` sequence; the
// producer must translate it into HIR declaration ids in source order without
// ever comparing names. This fixture writes two methods on a struct and one on
// a trait, then checks the exact ids and their order.
void test_owner_declaration_method_list_is_ordered_and_explicit() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "struct Counter {\n"
                                     "    value: i32,\n"
                                     "    fn increment(self): i32 { return self.value + 1; }\n"
                                     "    fn reset(self): i32 { return 0; }\n"
                                     "}\n"
                                     "trait Named {\n"
                                     "    fn name(self): i32\n"
                                     "}\n"
                                     "struct Box<T> {\n"
                                     "    value: T,\n"
                                     "    fn get(self): T { return self->value; }\n"
                                     "}\n"
                                     "fn main(): i32 { return 0; }\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "struct and trait method bodies lower to HIR");

    const auto &hir       = session.hirModule();
    const auto *counter   = findDeclaration(hir, session.interner(), "Counter",
                                            hir::HirDeclarationKind::Struct);
    CHECK(counter != nullptr, "the Counter struct declaration is present in HIR");
    if (counter != nullptr) {
        CHECK_EQ(counter->method_declarations.size(), 2u,
                 "the struct declaration carries exactly its two inline methods");
        if (counter->method_declarations.size() == 2u) {
            const auto increment_id =
                findDeclarationId(hir, session.interner(), "increment",
                                  hir::HirDeclarationKind::Function);
            const auto reset_id =
                findDeclarationId(hir, session.interner(), "reset",
                                  hir::HirDeclarationKind::Function);
            CHECK_EQ(counter->method_declarations[0], increment_id,
                     "the first method relation contains the exact HIR declaration id");
            CHECK_EQ(counter->method_declarations[1], reset_id,
                     "the second method relation contains the exact HIR declaration id");
            const auto &first = hir.getDeclaration(counter->method_declarations[0]);
            const auto &second = hir.getDeclaration(counter->method_declarations[1]);
            CHECK_EQ(session.interner().lookup(first.name), "increment",
                     "the first method in the owner's list is the first written");
            CHECK_EQ(session.interner().lookup(second.name), "reset",
                     "the second method in the owner's list is the second written");
            CHECK(first.kind == hir::HirDeclarationKind::Function &&
                      second.kind == hir::HirDeclarationKind::Function,
                  "owner method lists reference function declarations");
            CHECK(counter->method_declarations[0] != counter->method_declarations[1],
                  "the two method ids are distinct");
        }
    }

    const auto *named = findDeclaration(hir, session.interner(), "Named",
                                        hir::HirDeclarationKind::Trait);
    CHECK(named != nullptr, "the Named trait declaration is present in HIR");
    if (named != nullptr) {
        CHECK_EQ(named->method_declarations.size(), 1u,
                 "the trait declaration carries its single requirement method");
        if (named->method_declarations.size() == 1u) {
            const auto &method = hir.getDeclaration(named->method_declarations[0]);
            CHECK_EQ(session.interner().lookup(method.name), "name",
                     "the trait's method list names the declared requirement");
        }
    }

    const auto *box = findDeclaration(hir, session.interner(), "Box",
                                      hir::HirDeclarationKind::Struct);
    CHECK(box != nullptr, "the generic Box declaration is present in HIR");
    if (box != nullptr) {
        CHECK(box->template_id != hir::kInvalidHirTemplate,
              "the generic Box declaration links to its template record");
        CHECK_EQ(box->method_declarations.size(), 1u,
                 "the generic owner carries its inline method declaration id");
        if (box->method_declarations.size() == 1u &&
            box->template_id != hir::kInvalidHirTemplate) {
            const auto method_id = findDeclarationId(hir, session.interner(), "get",
                                                     hir::HirDeclarationKind::Function);
            CHECK_EQ(box->method_declarations[0], method_id,
                     "the generic owner's method relation is an exact HIR declaration id");
            const auto &template_record = hir.getTemplate(box->template_id);
            CHECK_EQ(template_record.method_declarations.size(), 1u,
                     "the template preserves its owner's ordered inline methods");
            if (template_record.method_declarations.size() == 1u)
                CHECK_EQ(template_record.method_declarations[0], method_id,
                         "the template method list preserves the exact declaration id");
        }
    }

    // A declaration with no inline method body must keep an empty method list;
    // `main` is a free function and never an owner.
    const auto *main_decl = findDeclaration(hir, session.interner(), "main",
                                            hir::HirDeclarationKind::Function);
    CHECK(main_decl != nullptr, "the main declaration is present in HIR");
    if (main_decl != nullptr)
        CHECK(main_decl->method_declarations.empty(),
              "a declaration with no inline method body has an empty method list");
}

// Every owner kind that can carry an inline method body records its ordered
// method `DeclId` list, not only structs and traits. This fixture gives the
// interface requirement list and the non-generic enum/union inline methods
// dedicated assertions so each supported owner kind is covered explicitly.
void test_owner_method_lists_cover_interface_enum_and_union() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "interface Shape {\n"
                                     "    fn area(self): i32\n"
                                     "}\n"
                                     "enum Mode {\n"
                                     "    Off = 0,\n"
                                     "    On = 1,\n"
                                     "    fn code(self): i32 { return 3; }\n"
                                     "}\n"
                                     "union Slot {\n"
                                     "    i32,\n"
                                     "    f64,\n"
                                     "    fn pick(self): i32 { return 7; }\n"
                                     "}\n"
                                     "fn main(): i32 { return 0; }\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "interface, enum, and union inline methods lower to HIR");

    const auto &hir = session.hirModule();

    const auto *shape = findDeclaration(hir, session.interner(), "Shape",
                                        hir::HirDeclarationKind::Interface);
    CHECK(shape != nullptr, "the Shape interface declaration is present in HIR");
    if (shape != nullptr) {
        CHECK_EQ(shape->method_declarations.size(), 1u,
                 "the interface carries its single requirement method");
        if (shape->method_declarations.size() == 1u)
            CHECK_EQ(session.interner().lookup(
                         hir.getDeclaration(shape->method_declarations[0]).name),
                     "area", "the interface method list names the declared requirement");
    }

    const auto *mode = findDeclaration(hir, session.interner(), "Mode",
                                       hir::HirDeclarationKind::Enum);
    CHECK(mode != nullptr, "the Mode enum declaration is present in HIR");
    if (mode != nullptr) {
        CHECK_EQ(mode->method_declarations.size(), 1u,
                 "the enum carries its single inline method");
        if (mode->method_declarations.size() == 1u)
            CHECK_EQ(session.interner().lookup(
                         hir.getDeclaration(mode->method_declarations[0]).name),
                     "code", "the enum method list names the inline method");
    }

    const auto *slot = findDeclaration(hir, session.interner(), "Slot",
                                       hir::HirDeclarationKind::Union);
    CHECK(slot != nullptr, "the Slot union declaration is present in HIR");
    if (slot != nullptr) {
        CHECK_EQ(slot->method_declarations.size(), 1u,
                 "the union carries its single inline method");
        if (slot->method_declarations.size() == 1u)
            CHECK_EQ(session.interner().lookup(
                         hir.getDeclaration(slot->method_declarations[0]).name),
                     "pick", "the union method list names the inline method");
    }
}

// A generic owner that implements a trait records the trait's explicit HIR
// type on its template. The relation comes from the frontend-resolved
// implement-record declaration ids, so a name match is never involved, and the
// stored type is a real `HirTypeKind.Trait`, not `Unknown`.
void test_generic_owner_template_records_trait_types() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "trait Value {\n"
                                     "    fn value(self): i32\n"
                                     "}\n"
                                     "trait Named {\n"
                                     "    fn name(self): i32\n"
                                     "}\n"
                                     "struct Box<T> {\n"
                                     "    value: T,\n"
                                     "}\n"
                                     "implement Box<T> as Value {\n"
                                     "    fn value(self): i32 { return 1; }\n"
                                     "}\n"
                                     "fn main(): i32 { return 0; }\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "generic owner with an implement block lowers to HIR");

    const auto &hir = session.hirModule();
    const auto *box = findDeclaration(hir, session.interner(), "Box",
                                      hir::HirDeclarationKind::Struct);
    CHECK(box != nullptr, "the generic Box declaration is present in HIR");
    if (box == nullptr)
        return;
    CHECK(box->template_id != hir::kInvalidHirTemplate,
          "the generic Box declaration links to its template record");
    if (box->template_id == hir::kInvalidHirTemplate)
        return;

    const auto &template_record = hir.getTemplate(box->template_id);
    CHECK_EQ(template_record.trait_types.size(), 1u,
             "the template records exactly the implemented trait");
    if (template_record.trait_types.size() != 1u)
        return;

    const auto &types = session.types();
    const auto trait_type = template_record.trait_types[0];
    const auto *trait = std::get_if<types::TypeTrait>(&types.lookup(trait_type));
    CHECK(trait != nullptr,
          "the recorded trait reference is a real HIR trait type, not Unknown");
    if (trait != nullptr)
        CHECK_EQ(types.interner().lookup(trait->name), "Value",
                 "the recorded trait reference names the implemented trait");
}

void test_inline_method_ids_reach_cache_artifact_writer() {
    IsolatedWorkspace workspace;
    workspace.writeFile("main.zithm", "trait Value {\n"
                                     "    fn value(self): i32\n"
                                     "}\n"
                                     "struct Box<T> {\n"
                                     "    value: T,\n"
                                     "    fn get(self): T { return self->value; }\n"
                                     "}\n"
                                     "implement Box<T> as Value {\n"
                                     "    fn value(self): i32 { return 1; }\n"
                                     "}\n"
                                     "fn main(): i32 { return 0; }\n");

    memory::Arena arena;
    Options options(arena);
    options.noCache = false;
    options.targetStage = session::Stage::Cached;
    session::CompilationSession compiled(options, (workspace.root / "main.zithm").string());
    compiled.setBuffered(true);
    CHECK(compiled.run(), "compilation writes an artifact through the cache writer");

    const auto *artifact = compiled.persistentArtifact();
    CHECK(artifact != nullptr, "the cache writer exposes its completed artifact");
    if (artifact == nullptr)
        return;

    const auto findArtifactDecl = [&](std::string_view name) -> uint32_t {
        for (size_t index = 0; index < artifact->decls.size(); ++index) {
            if (artifact->decls[index].name == name)
                return static_cast<uint32_t>(index);
        }
        return ~uint32_t{0};
    };
    const auto box_id = findArtifactDecl("Box");
    const auto get_id = findArtifactDecl("get");
    CHECK(box_id != ~uint32_t{0}, "the artifact contains the Box declaration");
    CHECK(get_id != ~uint32_t{0}, "the artifact contains the get method");
    if (box_id != ~uint32_t{0}) {
        const auto &box = artifact->decls[box_id];
        CHECK_EQ(box.method_decl_indices.size(), 1u,
                 "the cache writer preserves the generic owner's method index");
        if (box.method_decl_indices.size() == 1u)
            CHECK_EQ(box.method_decl_indices[0], get_id,
                     "the cache writer preserves the exact owner method index");
    }
    const auto template_it = std::find_if(
        artifact->templates.begin(), artifact->templates.end(),
        [](const cache::TemplateBlueprint &record) { return record.name == "Box"; });
    CHECK(template_it != artifact->templates.end(),
          "the artifact contains the generic Box template");
    if (template_it != artifact->templates.end()) {
        CHECK_EQ(template_it->method_decl_indices.size(), 1u,
                 "the cache writer preserves the template's method list");
        if (template_it->method_decl_indices.size() == 1u)
            CHECK_EQ(template_it->method_decl_indices[0], get_id,
                     "the template method relation preserves the exact artifact index");
        CHECK_EQ(template_it->trait_type_ids.size(), 1u,
                 "the cache writer preserves the template's trait reference");
        if (template_it->trait_type_ids.size() == 1u) {
            const auto trait_index = template_it->trait_type_ids[0];
            CHECK(trait_index < artifact->types.size(),
                  "the template trait reference is a valid artifact type index");
            if (trait_index < artifact->types.size())
                CHECK(artifact->types[trait_index].kind == cache::CompactTypeKind::Trait,
                      "the persisted trait reference keeps its Trait type kind");
        }
    }
}

void test_indirect_function_call_has_callee_and_fn_type() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "extern fn apply(f: fn(i32): i32, x: i32): i32\n"
                                     "extern fn double(x: i32): i32\n"
                                     "fn main(): i32 {\n"
                                     "    var f: fn(i32): i32 = double;\n"
                                     "    return f(7);\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "function value and indirect call lower to HIR");

    const auto &hir  = session.hirModule();
    const auto *main = findFunction(hir, session.interner(), "main");
    CHECK(main != nullptr, "main function is present after function-value lowering");
    if (main == nullptr)
        return;

    const hir::HirCall *call = nullptr;
    for (const auto &block : main->blocks) {
        for (auto inst : block.insts)
            if (const auto *candidate = std::get_if<hir::HirCall>(&hir.getExpr(inst))) {
                call = candidate;
                break;
            }
        if (call == nullptr && block.terminator != hir::kInvalidHirExpr) {
            if (const auto *ret = std::get_if<hir::HirRet>(&hir.getExpr(block.terminator))) {
                if (ret->value != hir::kInvalidHirExpr)
                    if (const auto *candidate =
                            std::get_if<hir::HirCall>(&hir.getExpr(ret->value))) {
                        call = candidate;
                        break;
                    }
            }
        }
        if (call != nullptr)
            break;
    }
    CHECK(call != nullptr, "indirect call is lowered as a HirCall instruction");
    if (call == nullptr)
        return;
    CHECK(call->callee != hir::kInvalidHirExpr, "indirect call carries a callee expression");
    CHECK(call->resolved_fn == hir::kInvalidHirFunction,
          "indirect call does not resolve to a direct HIR function");
    CHECK(call->fn_type != types::kInvalidType, "indirect call records the lowered function type");
}

void test_array_to_slice_coercion_lowers_to_make_slice() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn whole(a: [3]i32): []i32 { a }\n"
                                     "fn main(): i32 {\n"
                                     "    var values: [3]i32 = [10, 20, 30];\n"
                                     "    return raw whole(values)[1];\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");
    CHECK(session.runTo(session::Stage::HirLowered), "array-to-slice coercion lowers to HIR");

    const auto &hir   = session.hirModule();
    const auto *whole = findFunction(hir, session.interner(), "whole");
    CHECK(whole != nullptr, "whole function is present after lowering");
    if (whole == nullptr)
        return;

    const hir::HirMakeSlice *slice = nullptr;
    for (size_t index = 0; index < hir.exprCount(); ++index) {
        if (const auto *candidate =
                std::get_if<hir::HirMakeSlice>(&hir.getExpr(static_cast<hir::HirExprId>(index)))) {
            slice = candidate;
            if (slice->is_array)
                break;
        }
    }
    CHECK(slice != nullptr, "array return coercion emits HirMakeSlice");
    if (slice != nullptr) {
        CHECK(slice->is_array, "the coercion marks the object as an array");
        CHECK(slice->safety == hir::HirSafetyMode::Intrinsic,
              "full-array coercion is intrinsically safe");
        const auto &lo = hir.getExpr(slice->lo);
        const auto &hi = hir.getExpr(slice->hi);
        CHECK(std::get_if<hir::HirLiteral>(&lo) != nullptr &&
                  std::get_if<hir::HirLiteral>(&hi) != nullptr,
              "full-array coercion stores literal bounds");
        if (const auto *lo_lit = std::get_if<hir::HirLiteral>(&lo))
            CHECK_EQ(lo_lit->i, 0, "coercion lower bound is zero");
        if (const auto *hi_lit = std::get_if<hir::HirLiteral>(&hi))
            CHECK_EQ(hi_lit->i, 3, "coercion upper bound is the array size");
    }
}

void test_slice_range_lowers_to_checked_optional_view() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn get(i: i32): ?[]i32 {\n"
                                     "    var values: [3]i32 = [10, 20, 30];\n"
                                     "    return values[i..(i + 1)];\n"
                                     "}\n"
                                     "fn main(): i32 { 0 }\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");
    CHECK(session.runTo(session::Stage::HirLowered), "array slice range lowers to HIR");

    const auto &hir = session.hirModule();
    const auto *get = findFunction(hir, session.interner(), "get");
    CHECK(get != nullptr, "get function is present after lowering");
    if (get == nullptr)
        return;

    size_t make_none_count         = 0;
    size_t make_some_count         = 0;
    const hir::HirMakeSlice *slice = nullptr;
    for (size_t index = 0; index < hir.exprCount(); ++index) {
        const auto &expr = hir.getExpr(static_cast<hir::HirExprId>(index));
        if (const auto *some = std::get_if<hir::HirMakeSome>(&expr))
            ++make_some_count;
        if (const auto *none = std::get_if<hir::HirMakeNone>(&expr))
            ++make_none_count;
        if (slice == nullptr) {
            const auto *candidate = std::get_if<hir::HirMakeSlice>(&expr);
            if (candidate != nullptr)
                slice = candidate;
        }
    }
    CHECK(slice != nullptr, "slice range lowers to HirMakeSlice");
    if (slice != nullptr) {
        CHECK(slice->is_array, "array slicing marks the object as an array");
        CHECK(slice->safety == hir::HirSafetyMode::Guarded,
              "checked slice range marks the valid-path slice as guarded");
        CHECK(slice->lo != hir::kInvalidHirExpr && slice->hi != hir::kInvalidHirExpr,
              "slice range carries lowered bounds");
        CHECK(slice->type != types::kInvalidType, "slice range records the slice type");
        CHECK(slice->object_type != types::kInvalidType, "slice range records the object type");
    }
    CHECK(make_some_count > 0u, "dynamic slice ranges wrap the valid path in Some");
    CHECK(make_none_count > 0u, "dynamic slice ranges wrap the invalid path in None");
}

void test_checked_index_lowers_to_optional_with_some_and_none() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn get(i: i32): ?i32 {\n"
                                     "    var values: [3]i32 = [10, 20, 30];\n"
                                     "    return values[i];\n"
                                     "}\n"
                                     "fn main(): i32 { 0 }\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");
    CHECK(session.runTo(session::Stage::HirLowered), "checked index lowers to HIR");

    const auto &hir = session.hirModule();
    const auto *get = findFunction(hir, session.interner(), "get");
    CHECK(get != nullptr, "get function is present after lowering");
    if (get == nullptr)
        return;

    size_t make_none_count     = 0;
    size_t make_some_count     = 0;
    const hir::HirIndex *index = nullptr;
    for (size_t index_id = 0; index_id < hir.exprCount(); ++index_id) {
        const auto &expr = hir.getExpr(static_cast<hir::HirExprId>(index_id));
        if (const auto *some = std::get_if<hir::HirMakeSome>(&expr))
            ++make_some_count;
        if (const auto *none = std::get_if<hir::HirMakeNone>(&expr))
            ++make_none_count;
        if (index == nullptr) {
            const auto *candidate = std::get_if<hir::HirIndex>(&expr);
            if (candidate != nullptr)
                index = candidate;
        }
    }
    CHECK(index != nullptr, "checked index valid path lowers to HirIndex");
    if (index != nullptr) {
        CHECK(index->safety == hir::HirSafetyMode::Guarded,
              "checked index marks the valid-path access as guarded");
        CHECK(index->type != types::kInvalidType, "checked index carries the element type");
        CHECK(index->obj_type != types::kInvalidType, "checked index carries the object type");
    }
    CHECK(make_some_count > 0u, "dynamic checked indexes wrap the valid path in Some");
    CHECK(make_none_count > 0u, "dynamic checked indexes wrap the invalid path in None");
}

void test_optional_narrowing_marks_payload_guarded() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn read(value: ?i32): i32 {\n"
                                     "    if (value is null) { return 0; } else { return value; }\n"
                                     "}\n"
                                     "fn main(): i32 { 0 }\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");
    CHECK(session.runTo(session::Stage::HirLowered), "optional narrowing lowers to HIR");

    const auto &hir  = session.hirModule();
    const auto *read = findFunction(hir, session.interner(), "read");
    CHECK(read != nullptr, "read function is present after optional narrowing");
    if (read == nullptr)
        return;

    bool found_guarded_payload = false;
    for (const auto &block : read->blocks) {
        if (block.terminator == hir::kInvalidHirExpr)
            continue;
        const auto *ret = std::get_if<hir::HirRet>(&hir.getExpr(block.terminator));
        if (ret == nullptr || ret->value == hir::kInvalidHirExpr)
            continue;
        const auto *field = std::get_if<hir::HirField>(&hir.getExpr(ret->value));
        if (field != nullptr && field->index == 0u &&
            field->safety == hir::HirSafetyMode::Guarded) {
            found_guarded_payload = true;
            break;
        }
    }
    CHECK(found_guarded_payload, "a proven-present optional payload is marked guarded");
}

void test_raw_slice_and_index_lower_without_optional_checks() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn main(): i32 {\n"
                                     "    var values: [3]i32 = [10, 20, 30];\n"
                                     "    let s: []i32 = raw values[1..3];\n"
                                     "    return raw s[0];\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");
    CHECK(session.runTo(session::Stage::HirLowered), "raw slice/index expressions lower to HIR");

    const auto &hir  = session.hirModule();
    const auto *main = findFunction(hir, session.interner(), "main");
    CHECK(main != nullptr, "main function is present after raw slice lowering");
    if (main == nullptr)
        return;

    const hir::HirMakeSlice *slice = nullptr;
    for (size_t index = 0; index < hir.exprCount(); ++index) {
        const auto &expr = hir.getExpr(static_cast<hir::HirExprId>(index));
        if (const auto *candidate = std::get_if<hir::HirMakeSlice>(&expr))
            slice = candidate;
        CHECK(std::get_if<hir::HirMakeSome>(&expr) == nullptr, "raw slicing has no Some wrapper");
        CHECK(std::get_if<hir::HirMakeNone>(&expr) == nullptr, "raw slicing has no None wrapper");
    }
    CHECK(slice != nullptr, "raw array slicing still lowers to HirMakeSlice");
    if (slice != nullptr) {
        CHECK(slice->is_array, "raw slice marks the object as an array");
        CHECK(slice->safety == hir::HirSafetyMode::Raw,
              "raw slice preserves its explicit unchecked safety mode");
    }
}

void test_generic_instance_deduplication() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn same<T>(x: T): T { x }\n"
                                     "fn main(): i32 {\n"
                                     "    _ = same<i32>(1);\n"
                                     "    _ = same(2);\n"
                                     "    return 0;\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "two equivalent generic calls lower without a sema error");

    size_t same_count = 0;
    for (size_t i = 0; i < session.hirModule().getFnCount(); ++i) {
        const auto &fn  = session.hirModule().getFn(i);
        const auto name = internedName(session.interner(), fn.name);
        if (name.find("same<i32>") != std::string_view::npos)
            ++same_count;
    }
    CHECK_EQ(same_count, 1u, "the same concrete instance appears only once in HIR");
}

void test_call_with_unlowerable_argument_fails_pipeline() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn consume(value: char) {}\n"
                                     "fn main() {\n"
                                     "    consume('\\q');\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(!session.runTo(session::Stage::HirLowered),
          "call with an unlowerable argument fails the pipeline");
    CHECK(session.hasErrors(), "the failing call records compiler errors");

    const auto &hir  = session.hirModule();
    const auto *main = findFunction(hir, session.interner(), "main");
    CHECK(main == nullptr || countInstKind(hir, *main, hir::HirExprKind::Call) == 0u,
          "no incomplete HIR call survives the failure");
}

// `check` stops at TypeChecked, so an invalid escape must be reported by sema
// itself, not only by HIR lowering.
void test_invalid_escape_reports_at_type_check() {
    const auto expect_invalid_escape = [](std::string_view source, std::string_view what) {
        Workspace workspace;
        workspace.writeFile("main.zithm", source);

        memory::Arena arena;
        Options options(arena);
        options.targetStage = session::Stage::TypeChecked;
        session::CompilationSession session(options, (workspace.root / "main.zithm").string());
        session.setBuffered(true);

        CHECK(!session.runTo(session::Stage::TypeChecked), std::string(what).c_str());
        bool found = false;
        for (const auto &diagnostic : session.diags().all()) {
            if (diagnostic.code == diagnostics::err::InvalidEscape) {
                found = true;
                break;
            }
        }
        CHECK(found, "the invalid literal reports E0003 during type checking");
    };

    expect_invalid_escape("fn main() {\n"
                          "    _ = \"bad\\q\";\n"
                          "}\n",
                          "an unknown string escape fails at type check");
    expect_invalid_escape("fn main() {\n"
                          "    var c: char = '\\q';\n"
                          "}\n",
                          "an unknown char escape fails at type check");
    expect_invalid_escape("fn main() {\n"
                          "    var c: char = 'ab';\n"
                          "}\n",
                          "a multi-character char literal fails at type check");
}

void test_raw_union_lowers_members_and_casts() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "raw union Bits { u8, u32 }\n"
                                     "fn main() {\n"
                                     "    var b: Bits = Bits { 255u8 };\n"
                                     "    var word: u32 = b as u32;\n"
                                     "    var byte: u8 = word as Bits as u8;\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "raw union declaration and casts lower to HIR");

    const auto &hir  = session.hirModule();
    const auto *main = findFunction(hir, session.interner(), "main");
    CHECK(main != nullptr, "main function is present in HIR");
    if (main != nullptr) {
        CHECK_EQ(countUnionCasts(hir, *main), 4u,
                 "union literal and member round-trip emit four HirUnionCast nodes");
    }
}

void test_is_type_pointer_lowers_union_check_and_keeps_terminators() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "import \"stdio.h\"\n"
                                     "union Foo { *char, i32, f64 }\n"
                                     "fn main(): i32 {\n"
                                     "    let f = Foo{\"hello\"};\n"
                                     "    if (f is *char) {\n"
                                     "        _ = raw printf(\"%s\", f);\n"
                                     "    } else {\n"
                                     "        _ = raw printf(\"abu\\n\");\n"
                                     "    }\n"
                                     "    return 0;\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "tagged union is-pointer test with a compound member lowers");

    const auto &hir  = session.hirModule();
    const auto *main = findFunction(hir, session.interner(), "main");
    CHECK(main != nullptr, "main is available for is-pointer checks");
    if (main != nullptr) {
        size_t union_checks        = 0;
        size_t guarded_union_casts = 0;
        for (size_t i = 0; i < hir.exprCount(); ++i) {
            const auto &expr = hir.getExpr(static_cast<hir::HirExprId>(i));
            if (std::holds_alternative<hir::HirUnionCheck>(expr))
                ++union_checks;
            if (const auto *cast = std::get_if<hir::HirUnionCast>(&expr);
                cast != nullptr && cast->safety == hir::HirSafetyMode::Guarded)
                ++guarded_union_casts;
        }
        CHECK_EQ(union_checks, 1u, "`is *char` emits one tagged-union check node");
        CHECK(guarded_union_casts > 0u, "reading a narrowed tagged-union member is marked guarded");
        CHECK_EQ(countTerminatorKind(hir, *main, hir::HirExprKind::Branch), 1u,
                 "the if/is condition retains its branch terminator");
        CHECK(countTerminatorKind(hir, *main, hir::HirExprKind::Jump) >= 2u,
              "then and else branches jump to merge");
        CHECK(countInstKind(hir, *main, hir::HirExprKind::Call) >= 2u,
              "both printf calls remain in the lowered body");
    }
}

void test_distinct_generic_instances_have_distinct_symbols() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn pick<T>(x: T): T { x }\n"
                                     "fn main(): i32 {\n"
                                     "    _ = pick<i32>(1);\n"
                                     "    _ = pick<f64>(1.5);\n"
                                     "    return 0;\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "two generic instances with different type arguments lower to HIR");

    const auto &hir           = session.hirModule();
    size_t pick_i32           = 0;
    size_t pick_f64           = 0;
    hir::HirFunctionId first_function  = hir::kInvalidHirFunction;
    hir::HirFunctionId second_function = hir::kInvalidHirFunction;
    for (size_t i = 0; i < hir.getFnCount(); ++i) {
        const auto name = internedName(session.interner(), hir.getFn(i).name);
        if (name.find("pick<i32>") != std::string_view::npos) {
            ++pick_i32;
            first_function = static_cast<hir::HirFunctionId>(i);
        } else if (name.find("pick<f64>") != std::string_view::npos) {
            ++pick_f64;
            second_function = static_cast<hir::HirFunctionId>(i);
        }
    }
    CHECK_EQ(pick_i32, 1u, "one pick<i32> instance is lowered");
    CHECK_EQ(pick_f64, 1u, "one pick<f64> instance is lowered");
    CHECK(first_function != hir::kInvalidHirFunction &&
              second_function != hir::kInvalidHirFunction &&
              first_function != second_function,
          "distinct type-argument tuples map to distinct HIR function ids");
}

void test_defer_lowers_to_cleanup_nodes() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "#[discardable] extern fn putchar(c: i32): i32\n"
                                     "fn main(): i32 {\n"
                                     "    defer putchar(66);\n"
                                     "    defer putchar(65);\n"
                                     "    _ = putchar(48);\n"
                                     "    return 0;\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "defer statements lower to HIR");

    const auto &hir = session.hirModule();
    auto *main      = findFunction(hir, session.interner(), "main");
    CHECK(main != nullptr, "main is present in HIR");
    if (main != nullptr) {
        CHECK(countInstKind(hir, *main, hir::HirExprKind::Cleanup) >= 1u,
              "defer emits at least one HIR cleanup instruction");
    }
}

void test_defer_captures_later_binding_after_slot_alloca() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "extern fn mark(x: i32)\n"
                                     "fn main(): i32 {\n"
                                     "    defer mark(2);\n"
                                     "    var v: i32 = 1;\n"
                                     "    defer mark(v);\n"
                                     "    return 0;\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "modern defer before a later same-block binding lowers to HIR");

    const auto &hir  = session.hirModule();
    const auto *main = findFunction(hir, session.interner(), "main");
    CHECK(main != nullptr, "later-binding defer function is present in HIR");
    if (main != nullptr) {
        CHECK(countInstKind(hir, *main, hir::HirExprKind::Cleanup) >= 1u,
              "later-binding defer emits HIR cleanup");
        CHECK(countExprKind(hir, hir::HirExprKind::SlotLoad) >= 1u,
              "deferred body later reads a bound slot");
    }
}

void test_state_without_return_type_lowers_to_hir() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "state Done() { return; }\n"
                                     "state Start() { jump Done(); }\n"
                                     "fn main() { dock Start(); }\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "void state declarations lower to HIR");
    const auto &hir   = session.hirModule();
    const auto *start = findFunction(hir, session.interner(), "Start");
    CHECK(start != nullptr && start->isState, "void state remains a state function in HIR");
    if (start != nullptr) {
        CHECK_EQ(start->return_type, types::kVoidType,
                 "state without a return type lowers with void return type");
    }
}

void test_variadic_slice_lowers_to_hir() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn sum(rest: [...]i32): i32 {\n"
                                     "    return raw rest[0] + raw rest[1] + raw rest[2];\n"
                                     "}\n"
                                     "fn main(): i32 {\n"
                                     "    return sum(1, 2, 3);\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "variadic slice calls lower to HIR");

    const auto &hir = session.hirModule();
    const auto *sum = findFunction(hir, session.interner(), "sum");
    CHECK(sum != nullptr, "variadic slice function is present in HIR");
    if (sum != nullptr) {
        CHECK_EQ(sum->variadicSliceParam, 0u, "HIR records the variadic slice parameter index");
    }

    const auto *main = findFunction(hir, session.interner(), "main");
    CHECK(main != nullptr, "main is present after variadic slice lowering");
    if (main == nullptr)
        return;

    size_t array_literal_count     = 0;
    size_t make_slice_count        = 0;
    const hir::HirMakeSlice *slice = nullptr;
    for (size_t index = 0; index < hir.exprCount(); ++index) {
        const auto &expr = hir.getExpr(static_cast<hir::HirExprId>(index));
        if (std::holds_alternative<hir::HirArrayLiteral>(expr))
            ++array_literal_count;
        if (const auto *candidate = std::get_if<hir::HirMakeSlice>(&expr)) {
            ++make_slice_count;
            slice = candidate;
        }
    }
    CHECK_EQ(array_literal_count, 1u, "auto-collection materializes one temporary array");
    CHECK_EQ(make_slice_count, 1u, "auto-collection wraps the temporary array in a slice");
    if (slice != nullptr) {
        CHECK(slice->type != types::kInvalidType, "the variadic slice carries its slice type");
        CHECK(slice->object_type != types::kInvalidType,
              "the variadic slice carries its temporary array type");
        CHECK(slice->is_array, "auto-collection marks the temporary array as an array");
        CHECK(slice->safety == hir::HirSafetyMode::Intrinsic,
              "auto-collection has statically known bounds");
    }
}

void test_variadic_slice_state_lowers_to_hir() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "state Start(n: i32, rest: [...]i32): i32 {\n"
                                     "    if (n == 0) {\n"
                                     "        return raw rest[0];\n"
                                     "    }\n"
                                     "    jump Start(n - 1, 4, 5);\n"
                                     "}\n"
                                     "fn main(): i32 {\n"
                                     "    return dock Start(2, 1, 2);\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "variadic slice state/dock/jump calls lower to HIR");

    const auto &hir   = session.hirModule();
    const auto *start = findFunction(hir, session.interner(), "Start");
    CHECK(start != nullptr, "variadic slice state is present in HIR");
    if (start != nullptr)
        CHECK_EQ(start->variadicSliceParam, 1u, "state records the variadic slice parameter index");

    const auto *main = findFunction(hir, session.interner(), "main");
    CHECK(main != nullptr, "main is present after variadic state lowering");
    if (main == nullptr)
        return;

    bool dock_collects = false;
    bool jump_collects = false;
    for (size_t index = 0; index < hir.exprCount(); ++index) {
        if (const auto *slice =
                std::get_if<hir::HirMakeSlice>(&hir.getExpr(static_cast<hir::HirExprId>(index)))) {
            const auto lo = std::get_if<hir::HirLiteral>(&hir.getExpr(slice->lo));
            const auto hi = std::get_if<hir::HirLiteral>(&hir.getExpr(slice->hi));
            if (lo != nullptr && hi != nullptr && hi->i == 2)
                dock_collects = true;
            if (lo != nullptr && hi != nullptr && hi->i == 2)
                jump_collects = true;
        }
    }
    CHECK(dock_collects, "dock auto-collects its variadic slice into a temporary");
    CHECK(jump_collects, "jump auto-collects its variadic slice into a temporary");
}

void test_variadic_slice_explicit_vs_auto_free_function() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn sum(rest: [...]i32): i32 {\n"
                                     "    return raw rest[0] + raw rest[1];\n"
                                     "}\n"
                                     "extern fn values(): []i32\n"
                                     "fn main(): i32 {\n"
                                     "    return sum(...values()) + sum(1, 2);\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "explicit and auto-collected variadic slices lower to HIR");

    const auto &hir  = session.hirModule();
    const auto *main = findFunction(hir, session.interner(), "main");
    CHECK(main != nullptr, "main is present after explicit and auto-collected variadic calls");
    if (main == nullptr)
        return;

    size_t array_literal_count = 0;
    for (size_t index = 0; index < hir.exprCount(); ++index) {
        if (std::holds_alternative<hir::HirArrayLiteral>(
                hir.getExpr(static_cast<hir::HirExprId>(index))))
            ++array_literal_count;
    }
    CHECK_EQ(array_literal_count, 1u,
             "only the auto-collected call materializes a temporary array");
}

void test_variadic_slice_call_plan_survives_cache_hydration() {
    IsolatedWorkspace workspace;
    workspace.writeFile("main.zithm", "fn sum(rest: [...]i32): i32 {\n"
                                     "    return raw rest[0] + raw rest[1];\n"
                                     "}\n"
                                     "extern fn values(): []i32\n"
                                     "fn main(): i32 {\n"
                                     "    return sum(...values()) + sum(1, 2);\n"
                                     "}\n");

    memory::Arena first_arena;
    Options first_options(first_arena);
    first_options.noCache     = false;
    first_options.targetStage = session::Stage::Cached;
    session::CompilationSession first(first_options, (workspace.root / "main.zithm").string());
    first.setBuffered(true);
    CHECK(first.run(), "cold variadic-slice compilation writes the persistent artifact");

    memory::Arena second_arena;
    Options second_options(second_arena);
    second_options.noCache     = false;
    second_options.targetStage = session::Stage::HirLowered;
    session::CompilationSession second(second_options, (workspace.root / "main.zithm").string());
    second.setBuffered(true);
    CHECK(second.runTo(session::Stage::HirLowered),
          "warm variadic-slice compilation hydrates the persistent artifact");

    bool saw_explicit_slice = false;
    bool saw_auto_collected_slice = false;
    const auto &hydrated = second.hirModule();
    for (size_t id = 0; id < hydrated.exprCount(); ++id) {
        const auto *call =
            std::get_if<hir::HirCall>(&hydrated.getExpr(static_cast<hir::HirExprId>(id)));
        if (call == nullptr || !call->isVariadicSlice)
            continue;
        saw_explicit_slice |= !call->autoCollectTail && call->variadicSliceParam == 0U;
        saw_auto_collected_slice |= call->autoCollectTail && call->variadicSliceParam == 0U;
    }
    CHECK(saw_explicit_slice, "hydration preserves the explicit variadic-slice call plan");
    CHECK(saw_auto_collected_slice, "hydration preserves the auto-collection call plan");
}

void test_variadic_slice_explicit_vs_auto_method() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "struct Agg {\n"
                                     "    total: i32,\n"
                                     "    fn add(self, rest: [...]i32): i32 {\n"
                                     "        return self.total + raw rest[0] + raw rest[1];\n"
                                     "    }\n"
                                     "}\n"
                                     "fn slice1(): []i32 {\n"
                                     "    var values: [2]i32 = [1, 2];\n"
                                     "    return raw values[0..2];\n"
                                     "}\n"
                                     "fn main(): i32 {\n"
                                     "    let s: []i32 = slice1();\n"
                                     "    let a1: Agg = Agg { total: 100 };\n"
                                     "    let a2: Agg = Agg { total: 100 };\n"
                                     "    return a1.add(...s) - a2.add(1, 2);\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "explicit and auto-collected method variadic tails lower to HIR");

    const auto &hir = session.hirModule();
    CHECK(countExprKind(hir, hir::HirExprKind::MakeSlice) >= 1u,
          "auto-collected method tail lowers to a slice temporary");

    const auto *add = findFunction(hir, session.interner(), "add");
    CHECK(add != nullptr, "method with a variadic slice tail is present in HIR");
    if (add != nullptr)
        CHECK_EQ(add->variadicSliceParam, 1u,
                 "method records the variadic slice parameter after self");
}

void test_variadic_slice_dyn_tail_is_sema_decided() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "trait Value {\n"
                                     "    fn value(self): i32 { return 0; }\n"
                                     "}\n"
                                     "struct Box {\n"
                                     "    n: i32,\n"
                                     "    fn value(self): i32 { return self.n; }\n"
                                     "}\n"
                                     "implement Box as Value {\n"
                                     "    fn value(self): i32 { return self.n; }\n"
                                     "}\n"
                                     "fn total(rest: [...]dyn Value): i32 {\n"
                                     "    let a: dyn Value = raw rest[0];\n"
                                     "    return a.value();\n"
                                     "}\n"
                                     "fn collect(a: dyn Value, b: dyn Value): i32 {\n"
                                     "    return total(a, b);\n"
                                     "}\n"
                                     "fn take(ds: []dyn Value): i32 {\n"
                                     "    return total(...ds);\n"
                                     "}\n"
                                     "fn main(): i32 {\n"
                                     "    let a: dyn Value = Box { n: 5 };\n"
                                     "    let b: dyn Value = Box { n: 7 };\n"
                                     "    let ds: []dyn Value = [a, b];\n"
                                     "    return take(ds) - collect(a, b);\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "dyn variadic slice tails follow the sema plan");

    const auto &hir = session.hirModule();
    CHECK(countExprKind(hir, hir::HirExprKind::MakeSlice) >= 1u,
          "auto-collected dyn tail materializes a temporary []dyn slice");
}

void test_variadic_slice_explicit_vs_auto_state() {
    Workspace workspace;
    workspace.writeFile("main.zithm",
                        "state Start(rest: [...]i32): i32 {\n"
                        "    if (@lengthOf(rest) == 0) {\n"
                        "        return 0;\n"
                        "    }\n"
                        "    return raw rest[0];\n"
                        "}\n"
                        "extern fn values(): []i32\n"
                        "fn main(): i32 {\n"
                        "    return dock Start(...values()) + dock Start(1) + dock Start();\n"
                        "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "explicit, auto-collected, and empty state variadic tails lower to HIR");

    const auto &hir  = session.hirModule();
    const auto *main = findFunction(hir, session.interner(), "main");
    CHECK(main != nullptr, "main is present after state variadic tail calls");
    if (main == nullptr)
        return;

    size_t array_literal_count = 0;
    for (size_t index = 0; index < hir.exprCount(); ++index) {
        if (std::holds_alternative<hir::HirArrayLiteral>(
                hir.getExpr(static_cast<hir::HirExprId>(index))))
            ++array_literal_count;
    }
    CHECK_EQ(array_literal_count, 2u,
             "auto-collected and empty state tails materialize temporary arrays");
}

void test_optional_for_in_lowers_without_union_nodes() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "struct Range {\n"
                                     "    current: i32,\n"
                                     "    limit: i32,\n"
                                     "    fn next(var self): ?i32 {\n"
                                     "        if (self->current >= self->limit) {\n"
                                     "            return null;\n"
                                     "        }\n"
                                     "        let value = self->current;\n"
                                     "        self->current = self->current + 1;\n"
                                     "        return value;\n"
                                     "    }\n"
                                     "}\n"
                                     "fn main(): i32 {\n"
                                     "    var total: i32 = 0;\n"
                                     "    let r: Range = Range { current: 0, limit: 3 };\n"
                                     "    for (x in r) {\n"
                                     "        total = total + x;\n"
                                     "    }\n"
                                     "    return total;\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "optional for-in lowers to HIR");

    const auto &hir  = session.hirModule();
    const auto *main = findFunction(hir, session.interner(), "main");
    CHECK(main != nullptr, "main is present after optional for-in lowering");
    if (main == nullptr)
        return;
    CHECK_EQ(countUnionCasts(hir, *main), 0u, "?T for-in emits no legacy union cast");
    CHECK(countExprKind(hir, hir::HirExprKind::MakeSome) > 0u,
          "?T for-in lowers optional payload construction");
    CHECK(countExprKind(hir, hir::HirExprKind::Field) > 0u,
          "?T for-in reads the optional tag/payload fields");
}

void test_nested_optional_for_in_loop_variable_is_optional() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "struct Range {\n"
                                     "    current: i32,\n"
                                     "    limit: i32,\n"
                                     "    fn next(var self): ??i32 {\n"
                                     "        if (self->current >= self->limit) {\n"
                                     "            return null;\n"
                                     "        }\n"
                                     "        let value = self->current;\n"
                                     "        self->current = self->current + 1;\n"
                                     "        return value;\n"
                                     "    }\n"
                                     "}\n"
                                     "fn main(): i32 {\n"
                                     "    var total: i32 = 0;\n"
                                     "    let r: Range = Range { current: 0, limit: 3 };\n"
                                     "    for (x: ?i32 in r) {\n"
                                     "        if (x is null) {\n"
                                     "            total = total + 1;\n"
                                     "        }\n"
                                     "    }\n"
                                     "    return total;\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "nested optional for-in lowers to HIR");

    const auto &hir  = session.hirModule();
    const auto *main = findFunction(hir, session.interner(), "main");
    CHECK(main != nullptr, "main is present after nested optional for-in lowering");
    if (main == nullptr)
        return;
    size_t optional_allocas = 0;
    for (const auto &block : main->blocks) {
        for (auto inst : block.insts) {
            if (const auto *alloca = std::get_if<hir::HirSlotAlloca>(&hir.getExpr(inst))) {
                if (alloca->type != types::kInvalidType) {
                    const bool is_optional = std::get_if<types::TypeOptional>(
                                                 &session.types().lookup(alloca->type)) != nullptr;
                    if (is_optional)
                        ++optional_allocas;
                }
            }
        }
    }
    CHECK_EQ(optional_allocas, 2u,
             "??T for-in allocates its result and loop variable without a guard copy");
    CHECK_EQ(countUnionCasts(hir, *main), 0u, "??T for-in emits no legacy union cast");
}

void test_state_value_dock_lowers_to_indirect_tailcc_call() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "state Machine(n: i32): i32 {\n"
                                     "    return n;\n"
                                     "}\n"
                                     "fn main(): i32 {\n"
                                     "    let S: state(i32): i32 = Machine;\n"
                                     "    return dock S(42);\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered), "state value dock lowers to HIR");

    const auto &hir  = session.hirModule();
    const auto *main = findFunction(hir, session.interner(), "main");
    CHECK(main != nullptr, "main is present after state value dock lowering");
    if (main == nullptr)
        return;

    const hir::HirCall *call = nullptr;
    for (const auto &block : main->blocks) {
        for (auto inst : block.insts)
            if (const auto *candidate = std::get_if<hir::HirCall>(&hir.getExpr(inst))) {
                call = candidate;
                break;
            }
        if (call == nullptr && block.terminator != hir::kInvalidHirExpr) {
            if (const auto *ret = std::get_if<hir::HirRet>(&hir.getExpr(block.terminator))) {
                if (ret->value != hir::kInvalidHirExpr)
                    if (const auto *candidate =
                            std::get_if<hir::HirCall>(&hir.getExpr(ret->value))) {
                        call = candidate;
                        break;
                    }
            }
        }
        if (call != nullptr)
            break;
    }
    CHECK(call != nullptr, "state value dock emits a HirCall");
    if (call == nullptr)
        return;
    CHECK(call->callee != hir::kInvalidHirExpr,
          "state value dock has an indirect callee expression");
    CHECK(call->resolved_fn == hir::kInvalidHirFunction,
          "state value dock does not resolve to a direct HIR function");
    CHECK(call->fn_type != types::kInvalidType,
          "state value dock records the lowered state function type");
    CHECK(call->usesTailCC, "state value dock keeps LLVM tailcc");
}

void test_when_guards_lower_contextually_without_diagnostics() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn classify(n: i32): i32 {\n"
                                     "    return when (n) {\n"
                                     "        (0) 1,\n"
                                     "        (1..3) 2,\n"
                                     "        (n > 100) 3,\n"
                                     "        (_) 4,\n"
                                     "    };\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "when with literal, range, and boolean guards lowers to HIR");
    CHECK(!session.hasErrors(), "when guard lowering reports no diagnostics");

    const auto &hir = session.hirModule();
    const auto *fn  = findFunction(hir, session.interner(), "classify");
    CHECK(fn != nullptr, "classify is present in HIR");
}

void test_pipe_lowers_to_hir_pipeline_nodes() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn addOne(x: i32): i32 { x + 1 }\n"
                                     "fn observe(x: view i32): void {}\n"
                                     "fn main(): i32 {\n"
                                     "    10 |> addOne(it) also observe(view it) |> addOne(it)\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "pipeline expression lowers through sema and HIR");
    CHECK(!session.hasErrors(), "pipeline lowering reports no diagnostics");
    const auto &hir = session.hirModule();
    CHECK(countExprKind(hir, hir::HirExprKind::Pipe) >= 2u,
          "pipeline stages lower to HirPipe");
    CHECK(countExprKind(hir, hir::HirExprKind::PipeCurrent) >= 2u,
          "'it' references lower to HirPipeCurrent reads");
}

void test_pipe_block_effect_stage_lowers() {
    Workspace workspace;
    workspace.writeFile(
        "main.zithm", "fn addOne(x: i32): i32 { x + 1 }\n"
                    "fn double(x: i32): i32 { x * 2 }\n"
                    "fn observe(x: view i32): void {}\n"
                    "fn main(): i32 {\n"
                    "    10 |> addOne(it) also { observe(view it); } |> double(it)\n"
                    "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "also block stage lowers through sema and HIR");
    CHECK(!session.hasErrors(), "also block stage reports no diagnostics");
}

namespace {

void checkPipeDiagnostic(std::string_view source, std::string_view expected_message) {
    Workspace workspace;
    workspace.writeFile("main.zithm", source);

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(!session.runTo(session::Stage::HirLowered),
          "invalid pipeline stage must fail the lowering stage");
    CHECK(session.hasErrors(), "invalid pipeline stage reports diagnostics");

    bool found = false;
    for (const auto &diagnostic : session.diags().all()) {
        if (diagnostic.message.find(expected_message) != std::string::npos) {
            found = true;
            break;
        }
    }
    CHECK(found, "expected pipeline diagnostic is present");
}

} // namespace

void test_pipe_rejects_missing_placeholder() {
    checkPipeDiagnostic(
        "fn addOne(x: i32): i32 { x + 1 }\n"
        "fn main(): i32 { 10 |> addOne(0) }\n",
        "pipeline stage must consume 'it' exactly once on every execution path");
}

void test_pipe_rejects_multiple_placeholders() {
    checkPipeDiagnostic(
        "fn add(x: i32, y: i32): i32 { x + y }\n"
        "fn main(): i32 { 10 |> add(it, it) }\n",
        "pipeline stage must consume 'it' exactly once on every execution path");
}

void test_pipe_accepts_mutually_exclusive_consumption() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn identity(value: i32): i32 { value }\n"
                                     "fn choose(flag: bool): i32 {\n"
                                     "    10 |> if (flag) { let value = identity(it); value }\n"
                                     "             else { let value = identity(it); value }\n"
                                     "}\n"
                                     "fn main(): i32 { choose(true) }\n");
    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");
    CHECK(session.runTo(session::Stage::HirLowered),
          "one consumption in each exclusive branch is accepted");
    CHECK(!session.hasErrors(), "exclusive branch consumption reports no diagnostics");
}

void test_pipe_rejects_short_circuit_consumption() {
    checkPipeDiagnostic(
        "fn main(): i32 { false |> if (false and it) { 1 } else { 0 } }\n",
        "pipeline stage must consume 'it' exactly once on every execution path");
}

void test_pipe_rejects_placeholder_outside_stage() {
    checkPipeDiagnostic(
        "fn main(): i32 { .. }\n",
        "expected an expression");
}

void test_pipe_also_requires_view_parameters() {
    checkPipeDiagnostic(
        "fn observe(value: i32): void {}\n"
        "fn main(): i32 { 10 also observe(it) }\n",
        "an 'also' stage may pass 'it' only to read-only 'view' parameters");
}

void test_pipe_block_accepts_effect_without_current() {
    Workspace workspace;
    workspace.writeFile("main.zithm",
                        "fn addOne(x: i32): i32 { x + 1 }\n"
                        "fn main(): i32 {\n"
                        "    10 also { let ignored = addOne(1); }\n"
                        "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "'also' without 'it' lowers as an observing stage");
    CHECK(!session.hasErrors(), "'also' without 'it' reports no diagnostics");
}

void test_pipe_also_allows_multiple_observations() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn observe(value: view i32): void {}\n"
                                     "fn main(): i32 {\n"
                                     "    10 also { observe(view it); observe(view it); }\n"
                                     "    0\n"
                                     "}\n");
    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");
    CHECK(session.runTo(session::Stage::HirLowered),
          "also stages allow repeated observations");
    CHECK(!session.hasErrors(), "repeated also observations report no diagnostics");
}

void test_pipe_variadic_slice_current_lowers() {
    Workspace workspace;
    workspace.writeFile("main.zithm", "fn sum(rest: [...]i32): i32 { 0 }\n"
                                     "fn main(): i32 {\n"
                                     "    10 |> sum(it)\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zithm");

    CHECK(session.runTo(session::Stage::HirLowered),
          "pipeline current as a variadic slice tail lowers to HIR");
    CHECK(!session.hasErrors(), "pipeline variadic slice reports no diagnostics");

    const auto &hir = session.hirModule();
    CHECK(countExprKind(hir, hir::HirExprKind::Pipe) >= 1u,
          "pipeline stage lowers to HirPipe");
    CHECK(countExprKind(hir, hir::HirExprKind::PipeCurrent) >= 1u,
          "'it' lowers to HirPipeCurrent");
    CHECK(countExprKind(hir, hir::HirExprKind::MakeSlice) >= 1u,
          "variadic tail auto-collection lowers to HirMakeSlice");
}

void test_pipe_block_rejects_control_flow() {
    checkPipeDiagnostic(
        "fn addOne(x: i32): i32 { x + 1 }\n"
        "fn main(): i32 { 10 also { return addOne(it); } }\n",
        "'also { ... }' cannot transfer control out of the observing stage");
}

void test_imported_bare_opaque_cross_module() {
    IsolatedWorkspace workspace;
    workspace.writeFile("dep.zithm", "pub fn dep_make(): opaque { 42 as opaque }\n");
    workspace.writeFile("main.zithm", "from dep\n"
                                     "\n"
                                     "fn consume(v: opaque): ?i32 { v as i32 }\n"
                                     "fn check(v: opaque): bool { v is i32 }\n"
                                     "fn main(): ?i32 {\n"
                                     "    let o = dep_make();\n"
                                     "    if (check(o)) { return consume(o); } else { return null; }\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    session::CompilationSession session(options, (workspace.root / "main.zithm").string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "importing a module that returns bare opaque lowers across module boundaries");
    CHECK(!session.hasErrors(), "imported bare opaque cross-module use reports no diagnostics");

    const auto &hir   = session.hirModule();
    bool sawMake      = false;
    bool sawCheck     = false;
    bool sawExtract   = false;
    uint32_t makeTag  = 0;
    uint32_t checkTag = 0;
    for (size_t id = 0; id < hir.exprCount(); ++id) {
        const auto &expr = hir.getExpr(static_cast<hir::HirExprId>(id));
        if (const auto *make = std::get_if<hir::HirMakeOpaque>(&expr)) {
            sawMake = true;
            makeTag = make->type_id;
        }
        if (const auto *check = std::get_if<hir::HirOpaqueCheck>(&expr)) {
            sawCheck = true;
            checkTag = check->type_id;
        }
        if (std::get_if<hir::HirMakeSome>(&expr) != nullptr)
            sawExtract = true;
    }
    CHECK(sawMake && sawCheck && sawExtract,
          "imported bare opaque make/check and checked extraction lower to HIR");
    CHECK(makeTag != 0u && makeTag == checkTag,
          "imported bare opaque operations keep a stable canonical tag");
}

} // namespace

static void test_hir_lower_modern() {
    test_extern_and_main_lower_to_hir();
    test_external_symbol_alias_lower_to_hir();
    test_no_ownership_hir_has_empty_residual_attrs();
    test_ownership_hir_carries_residual_slot_attrs();
    test_free_borrow_parameter_lowers_to_pointer_and_call_addr();
    test_opaque_casts_lower_to_hir_nodes();
    test_canonical_type_lowers_to_hir_node();
    test_implicit_opaque_coercion_and_narrow_lowering();
    test_opaque_canonical_tags_persist_across_sessions();
    test_imported_type_canonical_id_persists_across_sessions();
    test_imported_type_canonical_id_uses_defining_module();
    test_imported_bare_opaque_cross_module();
    test_extern_variadic_lower_to_hir();
    test_bindings_lower_to_slots();
    test_if_else_lowers_to_branch_and_merge();
    test_else_condition_lowers_to_branch_and_merge();
    test_while_continue_lowers_loop_cfg();
    test_while_break_lowers_loop_exit();
    test_labeled_nested_loop_lowers();
    test_same_named_parameters_use_distinct_slots();
#ifdef ZITH_ENABLE_C_INTEROP
    test_c_pointers_lower_to_optional_pointer();
#endif
    test_radix_literals_lower_to_their_value();
    test_enum_discriminant_honours_radix();
    test_const_enum_discriminant_expressions_lower_to_values();
    test_struct_literal_lowers_to_hir();
    test_dot_field_read_lowers_to_hir_field();
    test_addrof_and_deref_lowers_to_hir_unary();
    test_arrow_access_lowers_to_deref_then_field();
    test_numeric_cast_lowers_to_hir_cast();
    test_is_null_on_pointer_optional_uses_niche();
    test_is_null_on_value_optional_reads_tag();
    test_optional_boolean_on_value_reads_tag();
    test_optional_boolean_on_pointer_uses_niche();
    test_for_condition_lowers_like_while();
    test_static_method_call_has_resolved_callee_only();
    test_state_machine_lowers_to_hir();
    test_state_tail_call_is_direct_and_musttail();
    test_multiple_states_share_machine_id_by_prototype();
    test_diverging_state_parameters_share_machine_id();
    test_local_states_qualify_and_lower_without_collisions();
    test_generic_function_lowers_to_concrete_instances();
    test_generic_template_survives_persistent_cache();
    test_alias_qualified_free_generic_self_is_not_receiver();
    test_generic_bound_and_value_param_lower_to_hir();
    test_interface_method_bound_lower_to_hir();
    test_owner_declaration_method_list_is_ordered_and_explicit();
    test_owner_method_lists_cover_interface_enum_and_union();
    test_generic_owner_template_records_trait_types();
    test_inline_method_ids_reach_cache_artifact_writer();
    test_const_global_lowers_to_load();
    test_untyped_const_global_type_comes_from_initializer();
    test_selective_imported_const_lowers_to_global_const_load();
    test_from_imported_const_lowers_to_global_const_load();
    test_qualified_imported_const_lowers_to_global_const_load();
    test_indirect_function_call_has_callee_and_fn_type();
    test_array_to_slice_coercion_lowers_to_make_slice();
    test_slice_range_lowers_to_checked_optional_view();
    test_checked_index_lowers_to_optional_with_some_and_none();
    test_optional_narrowing_marks_payload_guarded();
    test_raw_slice_and_index_lower_without_optional_checks();
    test_generic_instance_deduplication();
    test_call_with_unlowerable_argument_fails_pipeline();
    test_invalid_escape_reports_at_type_check();
    test_raw_union_lowers_members_and_casts();
    test_is_type_pointer_lowers_union_check_and_keeps_terminators();
    test_distinct_generic_instances_have_distinct_symbols();
    test_defer_lowers_to_cleanup_nodes();
    test_defer_captures_later_binding_after_slot_alloca();
    test_state_without_return_type_lowers_to_hir();
    test_variadic_slice_lowers_to_hir();
    test_variadic_slice_state_lowers_to_hir();
    test_variadic_slice_explicit_vs_auto_free_function();
    test_variadic_slice_call_plan_survives_cache_hydration();
    test_variadic_slice_explicit_vs_auto_method();
    test_variadic_slice_dyn_tail_is_sema_decided();
    test_variadic_slice_explicit_vs_auto_state();
    test_optional_for_in_lowers_without_union_nodes();
    test_nested_optional_for_in_loop_variable_is_optional();
    test_state_value_dock_lowers_to_indirect_tailcc_call();
    test_when_guards_lower_contextually_without_diagnostics();
    test_pipe_lowers_to_hir_pipeline_nodes();
    test_pipe_block_effect_stage_lowers();
    test_pipe_rejects_missing_placeholder();
    test_pipe_rejects_multiple_placeholders();
    test_pipe_accepts_mutually_exclusive_consumption();
    test_pipe_rejects_short_circuit_consumption();
    test_pipe_rejects_placeholder_outside_stage();
    test_pipe_also_requires_view_parameters();
    test_pipe_block_accepts_effect_without_current();
    test_pipe_also_allows_multiple_observations();
    test_pipe_variadic_slice_current_lowers();
    test_pipe_block_rejects_control_flow();
    test_anonymous_pack_literal_lowers_to_hir();
}

TEST_MAIN(hir_lower_modern)
