#include "cli/options.hpp"
#include "hir/hir-expr.hpp"
#include "session/compilation-session.hpp"
#include "test-common.hpp"

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

const hir::HirFunction *findFunction(const hir::HirModule &hir, memory::StringInterner &interner,
                                     std::string_view name) {
    for (size_t i = 0; i < hir.getFnCount(); ++i) {
        const auto &fn = hir.getFn(i);
        if (interner.lookup(fn.name) == name)
            return &fn;
    }
    return nullptr;
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

session::CompilationSession makeSession(const Workspace &workspace, memory::Arena &arena,
                                        Options &options, std::string_view file_name) {
    options.targetStage = session::Stage::HirLowered;
    session::CompilationSession session(options, (workspace.root / file_name).string());
    session.setBuffered(true);
    return session;
}

void test_extern_and_main_lower_to_hir() {
    Workspace workspace;
    workspace.writeFile("main.zith", "extern fn puts(msg: *char)\n"
                                     "fn main() {\n"
                                     "    puts(\"hello\");\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zith");

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

void test_bindings_lower_to_slots() {
    Workspace workspace;
    workspace.writeFile("main.zith", "fn add(a: i32, b: i32): i32 { a + b }\n"
                                     "fn main(): i32 {\n"
                                     "    var sum: i32 = add(3, 4);\n"
                                     "    sum\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zith");

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

void test_if_else_lowers_to_branch_and_merge() {
    Workspace workspace;
    workspace.writeFile("main.zith", "fn pick(flag: bool): i32 {\n"
                                     "    if (flag) {\n"
                                     "        1\n"
                                     "    } else {\n"
                                     "        2\n"
                                     "    }\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zith");

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

void test_while_continue_lowers_loop_cfg() {
    Workspace workspace;
    workspace.writeFile("main.zith", "fn main(): i32 {\n"
                                     "    var i: i32 = 0;\n"
                                     "    while (i < 10) {\n"
                                     "        i = i + 1;\n"
                                     "        continue;\n"
                                     "    }\n"
                                     "    i\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zith");

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
    workspace.writeFile("main.zith", "fn main(): i32 {\n"
                                     "    var i: i32 = 0;\n"
                                     "    while (i < 10) {\n"
                                     "        i = i + 1;\n"
                                     "        break;\n"
                                     "    }\n"
                                     "    i\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zith");

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
    workspace.writeFile("main.zith", "fn f(x: i32): i32 { return x }\n"
                                     "fn g(x: i32): i32 { return x }\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zith");

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

void test_struct_literal_lowers_to_hir() {
    Workspace workspace;
    workspace.writeFile("main.zith", "struct Foo { x: i32, y: i32 }\n"
                                     "fn mk(): Foo { Foo { x: 1, y: 2 } }\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zith");

    CHECK(session.runTo(session::Stage::HirLowered), "struct literal lowering succeeds");

    const auto &hir = session.hirModule();
    const auto *mk  = findFunction(hir, session.interner(), "mk");
    CHECK(mk != nullptr, "mk function is present");
    if (mk != nullptr) {
        CHECK_EQ(countInstKind(hir, *mk, hir::HirExprKind::StructLiteral), 1u,
                 "struct literal lowers to exactly one StructLiteral node");
    }
}

void test_dot_field_read_lowers_to_hir_field() {
    Workspace workspace;
    workspace.writeFile("main.zith", "struct Foo { x: i32, y: i32 }\n"
                                     "fn get_x(f: Foo): i32 { f.x }\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zith");

    CHECK(session.runTo(session::Stage::HirLowered), "dot field read lowering succeeds");

    const auto &hir   = session.hirModule();
    const auto *get_x = findFunction(hir, session.interner(), "get_x");
    CHECK(get_x != nullptr, "get_x function is present");
    if (get_x != nullptr) {
        CHECK_EQ(countInstKind(hir, *get_x, hir::HirExprKind::Field), 1u,
                 "field access lowers to one Field node");
        for (const auto &block : get_x->blocks) {
            for (auto inst : block.insts) {
                if (const auto *f = std::get_if<hir::HirField>(&hir.getExpr(inst)))
                    CHECK_EQ(f->index, 0u, "field 'x' maps to index 0");
            }
        }
    }
}

void test_addrof_and_deref_lowers_to_hir_unary() {
    Workspace workspace;
    workspace.writeFile("main.zith", "fn round_trip(x: i32): i32 {\n"
                                     "    let p: *i32 = &x;\n"
                                     "    *p\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zith");

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
    workspace.writeFile("main.zith", "struct Foo { x: i32, y: i32 }\n"
                                     "fn via_ptr(p: *Foo): i32 { p->x }\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zith");

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
    workspace.writeFile("main.zith", "fn widen(n: i32): f64 { n as f64 }\n"
                                     "fn main(): i32 { 0 }\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zith");

    CHECK(session.runTo(session::Stage::HirLowered), "numeric cast lowering succeeds");

    const auto &hir   = session.hirModule();
    const auto *widen = findFunction(hir, session.interner(), "widen");
    CHECK(widen != nullptr, "widen function is present");
    if (widen != nullptr) {
        CHECK_EQ(countInstKind(hir, *widen, hir::HirExprKind::Cast), 1u,
                 "'as' lowers to exactly one HirCast node");
    }
}

void test_is_null_on_pointer_optional_uses_niche() {
    Workspace workspace;
    workspace.writeFile("main.zith", "fn empty(p: ?*i32): bool { p is null }\n"
                                     "fn main(): i32 { 0 }\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zith");

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
    workspace.writeFile("main.zith", "fn empty(v: ?i32): bool { v is null }\n"
                                     "fn main(): i32 { 0 }\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zith");

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
                if (field != nullptr && field->index == 1u)
                    found_not_of_tag = true;
            }
        }
        CHECK(found_not_of_tag, "'is null' on ?T negates the discriminant read at field index 1");
    }
}

void test_for_condition_lowers_like_while() {
    Workspace workspace;
    workspace.writeFile("main.zith", "fn main(): i32 {\n"
                                     "    var i: i32 = 0;\n"
                                     "    for (i < 10) {\n"
                                     "        i = i + 1;\n"
                                     "    }\n"
                                     "    i\n"
                                     "}\n");

    memory::Arena arena;
    Options options(arena);
    auto session = makeSession(workspace, arena, options, "main.zith");

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

} // namespace

static void test_hir_lower_modern() {
    test_extern_and_main_lower_to_hir();
    test_bindings_lower_to_slots();
    test_if_else_lowers_to_branch_and_merge();
    test_while_continue_lowers_loop_cfg();
    test_while_break_lowers_loop_exit();
    test_same_named_parameters_use_distinct_slots();
    test_struct_literal_lowers_to_hir();
    test_dot_field_read_lowers_to_hir_field();
    test_addrof_and_deref_lowers_to_hir_unary();
    test_arrow_access_lowers_to_deref_then_field();
    test_numeric_cast_lowers_to_hir_cast();
    test_is_null_on_pointer_optional_uses_niche();
    test_is_null_on_value_optional_reads_tag();
    test_for_condition_lowers_like_while();
}

TEST_MAIN(hir_lower_modern)
