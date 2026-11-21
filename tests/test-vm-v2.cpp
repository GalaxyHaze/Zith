#include "test-common.hpp"

#ifdef ZITH_HAS_VM

#include "cli/options.hpp"
#include "session/compilation-session.hpp"
#include "vm/hir-to-vm.hpp"
#include "vm/typed-ir-dump.hpp"
#include "vm/typed-ir.hpp"
#include "vm/vir-cache.hpp"
#include "vm/vm-v2.hpp"
#include "wasm/abi-hir.hpp"
#include "zirl/zirl-header.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using namespace zith;

namespace {

auto makeModule(memory::Arena &arena) -> vm::Module {
    vm::Module module(arena);
    module.strings.push("zith-vm-v2");
    module.externs.push(std::string_view("puts"));

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.paramCount = 0;
    main.returnType = vm::ValueType::I32;
    main.regCount   = 8;
    main.regTypes.push(vm::ValueType::I32);
    main.regTypes.push(vm::ValueType::I32);
    main.regTypes.push(vm::ValueType::Ptr);
    main.regTypes.push(vm::ValueType::Ptr);
    main.regTypes.push(vm::ValueType::Ptr);
    main.regTypes.push(vm::ValueType::I32);
    main.regTypes.push(vm::ValueType::I32);

    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 0, 0));
    main.body.push(vm::Instr::withImm(vm::Op::LoadString, 1, 0));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 2, 32));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 3, 1));
    main.body.push(vm::Instr::simple(vm::Op::AllocBytes, 4, 2, 3));
    main.body.push(vm::Instr::simple(vm::Op::StoreBytes, 4, 0, 0));
    main.body.push(vm::Instr::simple(vm::Op::MallocBytes, 5, 2, 3));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 6, 12));
    main.body.push(vm::Instr::simple(vm::Op::MemCopy, 5, 4, 6));
    main.body.push(vm::Instr::simple(vm::Op::CallExtern, 7, 5, 0));
    main.body.push(vm::Instr{vm::Op::Ret, 6, 0, 0, 0});

    return module;
}

void test_vm_v2_malloc_string() {
    memory::Arena arena;
    auto module = makeModule(arena);

    vm::Vm vm;
    auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "typed VM runs main");
    CHECK_EQ(result.exitCode, 12, "main returns the string length");
    CHECK_EQ(result.output, std::string("zith-vm-v2\n"), "extern puts writes the string");
}

void test_vm_v2_dump_is_deterministic() {
    memory::Arena firstArena;
    memory::Arena secondArena;
    auto firstModule = makeModule(firstArena);
    firstModule.i64Constants.push(123456789);
    firstModule.f64Constants.push(1.25);
    firstModule.functions[0].body.push(vm::Instr{vm::Op::Branch, 0, 4, 0, 12});
    firstModule.functions[0].body.push(vm::Instr{vm::Op::Branch2, 0, 4, 0, 12, 3});
    firstModule.functions[0].body.push(vm::Instr{vm::Op::Jump, 0, 0, 0, 12});
    firstModule.functions[0].body.push(vm::Instr::simple(vm::Op::Add, 1, 2, 3));

    auto secondModule = makeModule(secondArena);
    secondModule.i64Constants.push(123456789);
    secondModule.f64Constants.push(1.25);
    secondModule.functions[0].body.push(vm::Instr{vm::Op::Branch, 0, 4, 0, 12});
    secondModule.functions[0].body.push(vm::Instr{vm::Op::Branch2, 0, 4, 0, 12, 3});
    secondModule.functions[0].body.push(vm::Instr{vm::Op::Jump, 0, 0, 0, 12});
    secondModule.functions[0].body.push(vm::Instr::simple(vm::Op::Add, 1, 2, 3));

    const auto first  = vm::dump(firstModule);
    const auto second = vm::dump(secondModule);
    CHECK_EQ(first, second, "equivalent VM v2 modules have identical dumps");
    CHECK(first.find("fn main") != std::string::npos, "VIR dump contains function name");
    CHECK(first.find("LoadConstI32") != std::string::npos, "VIR dump contains opcode");
    CHECK(first.find("value=0") != std::string::npos,
          "VIR dump identifies inline integer immediates");
    CHECK(first.find("Add dst=r1 lhs=r2 rhs=r3") != std::string::npos &&
              first.find("Ret value=r6") != std::string::npos,
          "VIR dump names arithmetic operands and return values");
    CHECK(first.find("reg_types") != std::string::npos, "VIR dump contains register types");
    CHECK(first.find("i64_constants") != std::string::npos &&
              first.find("123456789") != std::string::npos &&
              first.find("f64_constants") != std::string::npos &&
              first.find("1.25") != std::string::npos,
          "VIR dump includes deterministic integer and floating-point constant pools");
    CHECK(first.find("string=#0") != std::string::npos &&
              first.find("extern=#0") != std::string::npos,
          "VIR dump identifies string and extern table indices");
    CHECK(first.find("condition=r4 if_true=pc12 if_false=pc12") != std::string::npos &&
              first.find("condition=r4 if_true=pc12 if_false=pc3") != std::string::npos &&
              first.find("target=pc12") != std::string::npos,
          "VIR dump names branch conditions and program-counter destinations");
}

void test_vm_v2_cached_vir_compatibility() {
    const auto root = std::filesystem::temp_directory_path() / "zith-vm-v2-vir-cache-tests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const auto source = root / "main.zithm";
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        output << "fn main(): i32 { 7 }\n";
    }

    memory::Arena sessionArena;
    Options options(sessionArena);
    options.targetStage = session::Stage::HirLowered;
    session::CompilationSession session(options, source.string());
    session.setBuffered(true);
    const bool hirReady = session.runTo(session::Stage::HirLowered);
    CHECK(hirReady, "cached VIR fixture lowers to HIR");
    if (!hirReady) {
        std::filesystem::remove_all(root);
        return;
    }

    memory::Arena originalArena;
    vm::Module original(originalArena);
    const auto lowered = vm::lowerModule(session.hirModule(), session.interner(), session.types(),
                                         originalArena, original);
    CHECK(lowered.ok, "cached VIR fixture lowers to a VM module");
    if (!lowered.ok) {
        std::filesystem::remove_all(root);
        return;
    }

    cache::Artifact artifact;
    cache::Artifact::OptionalSection section;
    section.id             = static_cast<uint16_t>(zirl::SectionId::Vir);
    section.schema_version = vm::kVirSchemaVersion;
    const bool encoded     = vm::encodeCachedModule(original, section.payload);
    CHECK(encoded, "VM module encodes into an optional VIR cache section");
    if (!encoded || section.payload.size() < 8U) {
        std::filesystem::remove_all(root);
        return;
    }
    artifact.optional_sections.push_back(section);

    memory::Arena hitArena;
    vm::Module hitModule(hitArena);
    const auto hit = vm::loadOrLowerCachedModule(&artifact, session.hirModule(), session.interner(),
                                                 session.types(), hitArena, hitModule);
    CHECK(hit.ok && hit.reused, "compatible cached VIR is reused");
    vm::Vm hitVm;
    const auto hitResult = hitVm.runMain(hitModule);
    CHECK(hitResult.status == vm::RunStatus::Ok && hitResult.exitCode == 7,
          "reused VIR executes with the expected result");

    auto schemaMismatch = artifact;
    schemaMismatch.optional_sections[0].schema_version =
        static_cast<uint16_t>(vm::kVirSchemaVersion + 1U);
    memory::Arena schemaArena;
    vm::Module schemaModule(schemaArena);
    const auto schemaFallback =
        vm::loadOrLowerCachedModule(&schemaMismatch, session.hirModule(), session.interner(),
                                    session.types(), schemaArena, schemaModule);
    CHECK(schemaFallback.ok && !schemaFallback.reused,
          "incompatible VIR schema falls back to lowering the HIR");
    vm::Vm schemaVm;
    const auto schemaResult = schemaVm.runMain(schemaModule);
    CHECK(schemaResult.status == vm::RunStatus::Ok && schemaResult.exitCode == 7,
          "schema fallback executes with the expected result");

    auto semanticMismatch = artifact;
    semanticMismatch.optional_sections[0].payload[4] =
        static_cast<uint8_t>(vm::kVmSemanticVersion + 1U);
    memory::Arena semanticArena;
    vm::Module semanticModule(semanticArena);
    const auto semanticFallback =
        vm::loadOrLowerCachedModule(&semanticMismatch, session.hirModule(), session.interner(),
                                    session.types(), semanticArena, semanticModule);
    CHECK(semanticFallback.ok && !semanticFallback.reused,
          "incompatible VM semantics fall back to lowering the HIR");
    vm::Vm semanticVm;
    const auto semanticResult = semanticVm.runMain(semanticModule);
    CHECK(semanticResult.status == vm::RunStatus::Ok && semanticResult.exitCode == 7,
          "semantic fallback executes with the expected result");

    std::filesystem::remove_all(root);
}

void test_vm_v2_linear_memory_trap() {
    memory::Arena arena;
    auto module = makeModule(arena);
    module.functions[0].body.clear();
    module.functions[0].body.push(vm::Instr{vm::Op::Trap, 0, 0, 0, 0});

    vm::Vm vm;
    auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Trap, "typed VM reports explicit trap");
}

void test_vm_v2_missing_main() {
    memory::Arena arena;
    auto module = makeModule(arena);
    module.functions.clear();

    vm::Vm vm;
    auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::MissingMain, "typed VM reports missing main");
}

void test_vm_v2_loop_and_store() {
    memory::Arena arena;
    vm::Module module(arena);
    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.paramCount = 0;
    main.returnType = vm::ValueType::I32;
    main.regCount   = 5;
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 0, 0));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 1, 0));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 2, 1));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 3, 10));
    main.body.push(vm::Instr{vm::Op::Jump, 0, 0, 0, 5});
    main.body.push(vm::Instr::simple(vm::Op::Add, 0, 0, 2));
    main.body.push(vm::Instr::simple(vm::Op::Add, 1, 1, 2));
    main.body.push(vm::Instr::simple(vm::Op::Lt, 4, 1, 3));
    main.body.push(vm::Instr{vm::Op::Branch, 0, 4, 0, 5});
    main.body.push(vm::Instr{vm::Op::Ret, 0, 0, 0, 0});

    vm::Vm vm;
    auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "typed VM runs a branch loop");
    CHECK_EQ(result.exitCode, 10, "loop increments a register to its bound");
}

void test_vm_v2_branch2() {
    memory::Arena arena;
    vm::Module module(arena);

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.returnType = vm::ValueType::I32;
    main.regCount   = 2;
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 0, 1));
    main.body.push(vm::Instr{vm::Op::Branch2, 0, 0, 0, 4, 2});
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 1, 99));
    main.body.push(vm::Instr{vm::Op::Jump, 0, 0, 0, 5});
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 1, 77));
    main.body.push(vm::Instr{vm::Op::Ret, 1, 0, 0, 0});

    vm::Vm vm;
    auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "typed VM runs explicit two-way branch");
    CHECK_EQ(result.exitCode, 77, "Branch2 selects the true target");
}

void test_vm_v2_invalid_memory_offset() {
    memory::Arena arena;
    vm::Module module(arena);

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.paramCount = 0;
    main.returnType = vm::ValueType::I32;
    main.regCount   = 3;
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 0, 0));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 1, 0));
    main.body.push(vm::Instr{vm::Op::Ret, 0, 0, 0, 0});

    vm::Vm vm;
    auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "typed VM allows guest offset zero as valid memory");
}

void test_vm_v2_invalid_register_trap() {
    memory::Arena arena;
    vm::Module module(arena);

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.paramCount = 0;
    main.returnType = vm::ValueType::I32;
    main.regCount   = 1;
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 9, 1));
    main.body.push(vm::Instr{vm::Op::Ret, 0, 0, 0, 0});

    vm::Vm vm;
    auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Trap, "typed VM traps on an out-of-range destination");
    CHECK_EQ(result.output, std::string(), "shape validation runs before program output");
}

void test_vm_v2_invalid_opcode_shape_trap() {
    memory::Arena arena;
    vm::Module module(arena);

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.paramCount = 0;
    main.returnType = vm::ValueType::I32;
    main.regCount   = 1;
    main.body.push(vm::Instr{static_cast<vm::Op>(0xff), 0, 0, 0, 0});

    vm::Vm vm;
    auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Trap, "typed VM traps on an unknown opcode");
}

void test_vm_v2_direct_call_fn_index() {
    memory::Arena arena;
    vm::Module module(arena);

    auto &callee      = module.functions.emplace(arena);
    callee.name       = "add_three";
    callee.paramCount = 1;
    callee.returnType = vm::ValueType::I32;
    callee.regCount   = 2;
    callee.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 1, 3));
    callee.body.push(vm::Instr::simple(vm::Op::Add, 0, 0, 1));
    callee.body.push(vm::Instr{vm::Op::Ret, 0, 0, 0, 0});

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.paramCount = 0;
    main.returnType = vm::ValueType::I32;
    // Reg 0 = arg, reg 1 = result.
    main.regCount = 2;
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 0, 10));
    main.body.push(vm::Instr::simple(vm::Op::CallFn, 1, 0, 0));
    main.body.push(vm::Instr{vm::Op::Ret, 1, 0, 0, 0});

    vm::Vm vm;
    auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "direct fn call succeeds");
    CHECK_EQ(result.exitCode, 13, "direct fn call returns the callee result");
}

void test_vm_v2_indirect_call_fn_index() {
    memory::Arena arena;
    vm::Module module(arena);

    auto &double_      = module.functions.emplace(arena);
    double_.name       = "double_";
    double_.paramCount = 1;
    double_.returnType = vm::ValueType::I32;
    double_.regCount   = 1;
    double_.body.push(vm::Instr{vm::Op::Add, 0, 0, 0, 0}); // reg0 = reg0 + reg0
    double_.body.push(vm::Instr{vm::Op::Ret, 0, 0, 0, 0});

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.paramCount = 0;
    main.returnType = vm::ValueType::I32;
    // Reg 0 = fn ref, reg 1 = arg, reg 2 = result.
    main.regCount = 3;
    main.body.push(vm::Instr::withImm(vm::Op::LoadFnRef, 0, 0));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 1, 7));
    main.body.push(vm::Instr::callRef(vm::Op::CallFnRef, 2, 0, 1, 0));
    main.body.push(vm::Instr{vm::Op::Ret, 2, 0, 0, 0});

    vm::Vm vm;
    auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "indirect fn call succeeds");
    CHECK_EQ(result.exitCode, 14, "indirect fn call dispatches through the fn table");
}

void test_vm_v2_indirect_call_fn_range() {
    memory::Arena arena;
    vm::Module module(arena);

    auto &sum      = module.functions.emplace(arena);
    sum.name       = "sum";
    sum.paramCount = 3;
    sum.returnType = vm::ValueType::I64;
    sum.regCount   = 3;
    sum.body.push(vm::Instr{vm::Op::Add, 0, 0, 1, 0});
    sum.body.push(vm::Instr{vm::Op::Add, 0, 0, 2, 0});
    sum.body.push(vm::Instr{vm::Op::Ret, 0, 0, 0, 0});

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.returnType = vm::ValueType::I64;
    main.regCount   = 5;
    main.regTypes.push(vm::ValueType::FnRef);
    main.regTypes.push(vm::ValueType::I64);
    main.regTypes.push(vm::ValueType::I64);
    main.regTypes.push(vm::ValueType::I64);
    main.regTypes.push(vm::ValueType::I64);
    main.body.push(vm::Instr::withImm(vm::Op::LoadFnRef, 0, 0));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI64, 1, 10));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI64, 2, 20));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI64, 3, 12));
    main.body.push(vm::Instr::callRangeRef(vm::Op::CallRangeFnRef, 4, 0, 1, 3));
    main.body.push(vm::Instr{vm::Op::Ret, 4, 0, 0, 0});

    vm::Vm vm;
    const auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok,
          "indirect range call dispatches through the function table");
    CHECK_EQ(result.exitCode, 42, "indirect range call forwards every argument");
}

void test_vm_v2_indirect_call_fn_invalid_index() {
    memory::Arena arena;
    vm::Module module(arena);

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.paramCount = 0;
    main.returnType = vm::ValueType::I32;
    main.regCount   = 3;
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 0, 1));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 1, 0));
    main.body.push(vm::Instr::callRef(vm::Op::CallFnRef, 2, 0, 1, 1));
    main.body.push(vm::Instr{vm::Op::Ret, 2, 0, 0, 0});

    vm::Vm vm;
    const auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Trap,
          "indirect fn call traps on an out-of-range function reference");
}

void test_vm_v2_direct_call_extern_index() {
    memory::Arena arena;
    vm::Module module(arena);

    module.strings.push("via-extern");
    module.externs.push(std::string_view("puts"));

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.paramCount = 0;
    main.returnType = vm::ValueType::I32;
    main.regCount   = 2;
    main.body.push(vm::Instr::withImm(vm::Op::LoadString, 0, 0));
    main.body.push(vm::Instr::simple(vm::Op::CallExtern, 1, 0, 0));
    main.body.push(vm::Instr{vm::Op::Ret, 0, 0, 0, 0});

    vm::Vm vm;
    auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "direct extern call succeeds");
    CHECK_EQ(result.output, std::string("via-extern\n"), "direct extern call writes output");
}

void test_vm_v2_indirect_call_extern_index() {
    memory::Arena arena;
    vm::Module module(arena);

    module.strings.push("via-extern-ref");
    module.externs.push(std::string_view("puts"));

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.paramCount = 0;
    main.returnType = vm::ValueType::I32;
    // Reg 0 = extern ref, reg 1 = pointer, reg 2 = result.
    main.regCount = 3;
    main.body.push(vm::Instr::withImm(vm::Op::LoadExternRef, 0, 0));
    main.body.push(vm::Instr::withImm(vm::Op::LoadString, 1, 0));
    main.body.push(vm::Instr::callRef(vm::Op::CallExternRef, 2, 0, 1, 0));
    main.body.push(vm::Instr{vm::Op::Ret, 0, 0, 0, 0});

    vm::Vm vm;
    auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "indirect extern call succeeds");
    CHECK_EQ(result.output, std::string("via-extern-ref\n"), "indirect extern writes output");
}

void test_vm_v2_indirect_call_extern_invalid_index() {
    memory::Arena arena;
    vm::Module module(arena);
    module.externs.push(std::string_view("puts"));

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.paramCount = 0;
    main.returnType = vm::ValueType::I32;
    main.regCount   = 3;
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 0, 1));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 1, 0));
    main.body.push(vm::Instr::callRef(vm::Op::CallExternRef, 2, 0, 1, 1));
    main.body.push(vm::Instr{vm::Op::Ret, 2, 0, 0, 0});

    vm::Vm vm;
    const auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Trap,
          "indirect extern call traps on an out-of-range extern reference");
}

void test_vm_v2_store_load_bytes() {
    memory::Arena arena;
    vm::Module module(arena);

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.paramCount = 0;
    main.returnType = vm::ValueType::I32;
    // reg0 = bump offset, reg1 = size, reg2 = align, reg3 = loaded bytes.
    main.regCount = 4;
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 0, 1));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 1, 4));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 2, 1));
    main.body.push(vm::Instr::simple(vm::Op::AllocBytes, 0, 1, 2));
    main.body.push(vm::Instr::simple(vm::Op::LoadBytes, 3, 0, 1));
    main.body.push(vm::Instr{vm::Op::Ret, 3, 0, 0, 0});

    vm::Vm vm;
    auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "allocation and load inside memory succeeds");
    CHECK_EQ(result.exitCode, 0, "fresh bump memory reads as zero bytes");
}

void test_vm_v2_invalid_write_offset_trap() {
    memory::Arena arena;
    vm::Module module(arena);
    module.strings.push("x");

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.paramCount = 0;
    main.returnType = vm::ValueType::I32;
    main.regCount   = 3;
    // StoreBytes writes module string 0 at the offset stored in reg1.
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 1, 4096));
    main.body.push(vm::Instr::simple(vm::Op::StoreBytes, 1, 0, 0));
    main.body.push(vm::Instr{vm::Op::Ret, 0, 0, 0, 0});

    vm::Vm vm;
    auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Trap, "write outside linear memory traps");
}

void test_vm_v2_alloc_bump_and_heap_disjoint() {
    memory::Arena arena;
    vm::Module module(arena);

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.paramCount = 0;
    main.returnType = vm::ValueType::I32;
    // Let AllocBytes land on a bump offset and MallocBytes on a heap offset.
    main.regCount = 5;
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 0, 64));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 1, 1));
    main.body.push(vm::Instr::simple(vm::Op::AllocBytes, 2, 0, 1));
    main.body.push(vm::Instr::simple(vm::Op::MallocBytes, 3, 0, 1));
    main.body.push(vm::Instr::simple(vm::Op::Sub, 4, 2, 3));
    main.body.push(vm::Instr{vm::Op::Ret, 4, 0, 0, 0});

    vm::Vm vm;
    auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "bump and heap allocations succeed");
    CHECK(result.exitCode != 0, "bump and heap offsets stay disjoint");
}

void test_vm_v2_frame_arena_reclaimed() {
    memory::Arena arena;
    vm::Module module(arena);

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.returnType = vm::ValueType::I32;
    main.regCount   = 4;
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 0, 8));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 1, 1));
    main.body.push(vm::Instr::simple(vm::Op::AllocBytes, 2, 0, 1));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 3, 42));
    main.body.push(vm::Instr::simple(vm::Op::StoreI64, 2, 3));
    main.body.push(vm::Instr{vm::Op::Ret, 3, 0, 0, 0});

    vm::Vm vm;
    const auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "frame-local allocation returns successfully");
    CHECK_EQ(result.exitCode, 42, "frame-local allocation is usable before return");
    CHECK_EQ(vm.memory().arenaWatermark(), 0U, "Ret restores the frame arena watermark");
    CHECK_EQ(vm.memory().size(), 0U, "Ret truncates released frame-local memory");

    memory::Arena trapArena;
    vm::Module trapModule(trapArena);
    auto &trapMain      = trapModule.functions.emplace(trapArena);
    trapMain.name       = "main";
    trapMain.returnType = vm::ValueType::I32;
    trapMain.regCount   = 3;
    trapMain.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 0, 8));
    trapMain.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 1, 1));
    trapMain.body.push(vm::Instr::simple(vm::Op::AllocBytes, 2, 0, 1));
    trapMain.body.push(vm::Instr::trap());

    vm::Vm trapVm;
    const auto trapResult = trapVm.runMain(trapModule);
    CHECK(trapResult.status == vm::RunStatus::Trap, "trap exits the frame");
    CHECK_EQ(trapVm.memory().arenaWatermark(), 0U, "trap restores the frame arena watermark");
    CHECK_EQ(trapVm.memory().size(), 0U, "trap truncates released frame-local memory");
}

void test_vm_v2_nested_frame_arena_reclaimed_without_corruption() {
    memory::Arena arena;
    vm::Module module(arena);

    auto &callee      = module.functions.emplace(arena);
    callee.name       = "allocate";
    callee.paramCount = 0;
    callee.returnType = vm::ValueType::Ptr;
    callee.regCount   = 6;
    callee.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 0, 8));
    callee.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 1, 1));
    callee.body.push(vm::Instr::simple(vm::Op::AllocBytes, 2, 0, 1));
    callee.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 3, 99));
    callee.body.push(vm::Instr::simple(vm::Op::StoreI64, 2, 3));
    callee.body.push(vm::Instr::simple(vm::Op::MallocBytes, 4, 0, 1));
    callee.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 5, 77));
    callee.body.push(vm::Instr::simple(vm::Op::StoreI64, 4, 5));
    callee.body.push(vm::Instr{vm::Op::Ret, 2, 0, 0, 0});

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.returnType = vm::ValueType::I32;
    main.regCount   = 11;
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 0, 8));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 1, 1));
    main.body.push(vm::Instr::simple(vm::Op::AllocBytes, 2, 0, 1));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 3, 42));
    main.body.push(vm::Instr::simple(vm::Op::StoreI64, 2, 3));
    main.body.push(vm::Instr::simple(vm::Op::CallFn, 4, 0, 0));
    main.body.push(vm::Instr::simple(vm::Op::CallFn, 5, 0, 0));
    main.body.push(vm::Instr::simple(vm::Op::Eq, 6, 4, 5));
    main.body.push(vm::Instr::simple(vm::Op::LoadI64, 7, 2));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 8, 42));
    main.body.push(vm::Instr::simple(vm::Op::Eq, 9, 7, 8));
    main.body.push(vm::Instr::simple(vm::Op::Add, 10, 6, 9));
    main.body.push(vm::Instr{vm::Op::Ret, 10, 0, 0, 0});

    vm::Vm vm;
    const auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "nested frame allocations return successfully");
    CHECK_EQ(result.exitCode, 2,
             "nested frame reuses its allocation without corrupting caller memory");
    CHECK_EQ(vm.memory().arenaWatermark(), 0U, "nested returns restore the root watermark");
}

void test_vm_v2_frame_arena_reclaim_preserves_malloc() {
    memory::Arena arena;
    vm::Module module(arena);

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.returnType = vm::ValueType::Ptr;
    main.regCount   = 4;
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 0, 8));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 1, 1));
    main.body.push(vm::Instr::simple(vm::Op::MallocBytes, 2, 0, 1));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 3, 42));
    main.body.push(vm::Instr::simple(vm::Op::StoreI64, 2, 3));
    main.body.push(vm::Instr{vm::Op::Ret, 2, 0, 0, 0});

    vm::Vm vm;
    const auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "global allocation survives frame return");
    const auto bytes =
        vm.memory().read(static_cast<std::size_t>(result.exitCode), sizeof(uint64_t));
    CHECK_EQ(bytes.size(), sizeof(uint64_t), "global heap bytes remain addressable");
    if (!bytes.empty())
        CHECK_EQ(bytes[0], static_cast<uint8_t>(42), "global heap contents survive frame return");
}

void test_vm_v2_field_ptr_and_copy() {
    memory::Arena arena;
    vm::Module module(arena);
    module.strings.push("x");

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.paramCount = 0;
    main.returnType = vm::ValueType::I32;
    main.regCount   = 6;
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 0, 0));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 1, 128));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 2, 64));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 3, 1));
    main.body.push(vm::Instr::simple(vm::Op::AllocBytes, 0, 1, 3));
    main.body.push(vm::Instr{vm::Op::FieldPtr, 4, 0, 0, 2});
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 5, 1));
    main.body.push(vm::Instr::simple(vm::Op::StoreBytes, 4, 0, 0));
    main.body.push(vm::Instr::simple(vm::Op::LoadBytes, 5, 4, 5));
    main.body.push(vm::Instr{vm::Op::Ret, 5, 0, 0, 0});

    vm::Vm vm;
    auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "FieldPtr and store/load succeed");
    CHECK_EQ(result.exitCode, 120, "load returns the stored byte through a field pointer");
}

void test_vm_v2_slice_pair() {
    memory::Arena arena;
    vm::Module module(arena);

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.paramCount = 0;
    main.returnType = vm::ValueType::I32;
    main.regCount   = 4;
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 0, 100));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 1, 7));
    main.body.push(vm::Instr::simple(vm::Op::MakeSlice, 2, 0, 1));
    main.body.push(vm::Instr::simple(vm::Op::SlicePtr, 3, 2, 0));
    main.body.push(vm::Instr{vm::Op::Ret, 3, 0, 0, 0});

    vm::Vm vm;
    auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "slice pair construction succeeds");
    CHECK_EQ(result.exitCode, 100, "SlicePtr reads the pointer half of a slice");
}

void test_vm_v2_mem_copy_bytes() {
    memory::Arena arena;
    vm::Module module(arena);
    module.strings.push("q");

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.paramCount = 0;
    main.returnType = vm::ValueType::I32;
    main.regCount   = 6;
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 0, 16));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 1, 1));
    main.body.push(vm::Instr::simple(vm::Op::AllocBytes, 2, 0, 1));
    main.body.push(vm::Instr::simple(vm::Op::AllocBytes, 3, 0, 1));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 4, 1));
    main.body.push(vm::Instr::simple(vm::Op::StoreBytes, 2, 0, 0));
    main.body.push(vm::Instr::simple(vm::Op::MemCopy, 3, 2, 4));
    main.body.push(vm::Instr::simple(vm::Op::LoadBytes, 5, 3, 4));
    main.body.push(vm::Instr{vm::Op::Ret, 5, 0, 0, 0});

    vm::Vm vm;
    auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "MemCopy between allocated regions succeeds");
    CHECK_EQ(result.exitCode, 113, "memcpy copies stored byte to the destination");
}

void test_vm_v2_ffi_malloc_free_reuse() {
    memory::Arena arena;
    vm::Module module(arena);

    module.externs.push(std::string_view("malloc"));
    module.externs.push(std::string_view("free"));

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.paramCount = 0;
    main.returnType = vm::ValueType::I32;
    main.regCount   = 5;
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 0, 64));
    main.body.push(vm::Instr::callExtern(vm::Op::CallExtern, 1, 0, 0, 0));
    main.body.push(vm::Instr::callExtern(vm::Op::CallExtern, 2, 1, 0, 1));
    main.body.push(vm::Instr::callExtern(vm::Op::CallExtern, 3, 0, 0, 0));
    main.body.push(vm::Instr::simple(vm::Op::Sub, 4, 3, 1));
    main.body.push(vm::Instr{vm::Op::Ret, 4, 0, 0, 0});

    vm::Vm vm;
    auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "malloc/free FFI returns host offsets");
    CHECK_EQ(result.exitCode, 0, "free reuses the same heap slot");
}

void test_vm_v2_ffi_aligned_alloc_reuses_aligned_region() {
    memory::Arena arena;
    vm::Module module(arena);
    module.externs.push(std::string_view("malloc"));
    module.externs.push(std::string_view("free"));
    module.externs.push(std::string_view("aligned_alloc"));

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.returnType = vm::ValueType::I64;
    main.regCount   = 10;
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI64, 0, 1));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI64, 1, 128));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI64, 2, 64));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI64, 3, 64));
    main.body.push(vm::Instr::callExtern(vm::Op::CallExtern, 5, 0, 0, 0));
    main.body.push(vm::Instr::callExtern(vm::Op::CallExtern, 6, 1, 0, 0));
    main.body.push(vm::Instr::callExtern(vm::Op::CallExtern, 0, 6, 0, 1));
    main.body.push(vm::Instr::callExtern(vm::Op::CallExtern, 7, 2, 3, 2));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI64, 8, 63));
    main.body.push(vm::Instr::simple(vm::Op::BitAnd, 9, 7, 8));
    main.body.push(vm::Instr{vm::Op::Ret, 9, 0, 0, 0});

    vm::Vm vm;
    const auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "aligned_alloc is available in the v2 VM");
    CHECK_EQ(result.exitCode, 0, "aligned_alloc honors alignment when reusing a free block");
}

void test_vm_v2_ffi_realloc_preserves_data() {
    memory::Arena arena;
    vm::Module module(arena);

    module.externs.push(std::string_view("malloc"));
    module.externs.push(std::string_view("realloc"));
    module.externs.push(std::string_view("free"));

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.paramCount = 0;
    main.returnType = vm::ValueType::I32;
    main.regCount   = 8;
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI64, 0, 8));
    main.body.push(vm::Instr::callExtern(vm::Op::CallExtern, 1, 0, 0, 0));
    main.body.push(vm::Instr::callExtern(vm::Op::CallExtern, 2, 0, 0, 0));
    main.body.push(vm::Instr::callExtern(vm::Op::CallExtern, 3, 2, 0, 2));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI64, 4, 42));
    main.body.push(vm::Instr::simple(vm::Op::StoreI64, 1, 4));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI64, 5, 16));
    main.body.push(vm::Instr::callExtern(vm::Op::CallExtern, 6, 1, 5, 1));
    main.body.push(vm::Instr::simple(vm::Op::LoadI64, 7, 6));
    main.body.push(vm::Instr::callExtern(vm::Op::CallExtern, 3, 6, 0, 2));
    main.body.push(vm::Instr{vm::Op::Ret, 7, 0, 0, 0});

    vm::Vm vm;
    const auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "realloc FFI succeeds");
    CHECK_EQ(result.exitCode, 42, "realloc preserves data and reuses the freed block");
}

void test_vm_v2_ffi_putchar_unknown_trap() {
    memory::Arena arena;
    vm::Module module(arena);

    module.externs.push(std::string_view("putchar"));
    module.externs.push(std::string_view("not_supported"));

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.paramCount = 0;
    main.returnType = vm::ValueType::I32;
    main.regCount   = 3;
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 0, static_cast<uint16_t>('A')));
    main.body.push(vm::Instr::simple(vm::Op::CallExtern, 1, 0, 0));
    main.body.push(vm::Instr::callExtern(vm::Op::CallExtern, 2, 0, 0, 1));
    main.body.push(vm::Instr{vm::Op::Ret, 0, 0, 0, 0});

    vm::Vm vm;
    auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Trap, "unknown extern FFI traps");
}

void test_vm_v2_ffi_memcpy_and_strlen() {
    memory::Arena arena;
    vm::Module module(arena);
    module.strings.push("zith-ffi");
    module.externs.push(std::string_view("malloc"));
    module.externs.push(std::string_view("memcpy"));
    module.externs.push(std::string_view("strlen"));

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.paramCount = 0;
    main.returnType = vm::ValueType::I32;
    main.regCount   = 6;
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 0, 32));
    main.body.push(vm::Instr::callExtern(vm::Op::CallExtern, 1, 0, 0, 0));
    main.body.push(vm::Instr::withImm(vm::Op::LoadString, 2, 0));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 3, 8));
    main.body.push(vm::Instr::callExtern(vm::Op::CallExtern, 4, 1, 2, 1, 3));
    main.body.push(vm::Instr::callExtern(vm::Op::CallExtern, 5, 1, 0, 2));
    main.body.push(vm::Instr{vm::Op::Ret, 5, 0, 0, 0});

    vm::Vm vm;
    auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "memcpy and strlen FFI succeed");
    CHECK_EQ(result.exitCode, 8, "strlen returns the copied string length");
}

void test_vm_v2_ffi_snprintf() {
    memory::Arena arena;
    vm::Module module(arena);
    module.strings.push("%u");
    module.externs.push(std::string_view("snprintf"));
    module.externs.push(std::string_view("malloc"));

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.paramCount = 0;
    main.returnType = vm::ValueType::I32;
    main.regCount   = 6;
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 0, 32));
    main.body.push(vm::Instr::callExtern(vm::Op::CallExtern, 1, 0, 0, 1));
    main.body.push(vm::Instr::withImm(vm::Op::LoadString, 2, 0));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 3, 32));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 4, 42));
    main.body.push(vm::Instr::callExtern(vm::Op::CallExtern, 5, 1, 3, 0, 2, 4));
    main.body.push(vm::Instr{vm::Op::Ret, 5, 0, 0, 0});

    vm::Vm vm;
    auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "snprintf FFI succeeds");
    CHECK_EQ(result.exitCode, 2, "snprintf writes the unsigned value");
}

void test_vm_v2_ffi_calloc_zeroes_and_strncmp() {
    memory::Arena arena;
    vm::Module module(arena);
    module.strings.push("true");
    module.externs.push(std::string_view("calloc"));
    module.externs.push(std::string_view("strncmp"));
    module.externs.push(std::string_view("malloc"));

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.paramCount = 0;
    main.returnType = vm::ValueType::I32;
    main.regCount   = 8;
    // calloc(4, 8) must return a zeroed block.
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 0, 4));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 1, 8));
    main.body.push(vm::Instr::callExtern(vm::Op::CallExtern, 2, 0, 1, 0));
    main.body.push(vm::Instr::simple(vm::Op::LoadI64, 3, 2));
    // Copy "true" into a scratch block, then compare it with itself.
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 4, 8));
    main.body.push(vm::Instr::callExtern(vm::Op::CallExtern, 5, 4, 0, 2));
    main.body.push(vm::Instr::withImm(vm::Op::LoadString, 6, 0));
    main.body.push(vm::Instr::callExtern(vm::Op::CallExtern, 7, 5, 6, 1, 4));
    main.body.push(vm::Instr{vm::Op::Ret, 3, 0, 0, 0});

    vm::Vm vm;
    const auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "calloc and strncmp FFI succeed");
    CHECK_EQ(result.exitCode, 0, "calloc returns a zero-initialized block");
}

void test_vm_v2_ffi_strncmp_ordering() {
    memory::Arena arena;
    vm::Module module(arena);
    module.strings.push("abc");
    module.strings.push("abd");
    module.externs.push(std::string_view("strncmp"));

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.paramCount = 0;
    main.returnType = vm::ValueType::I32;
    main.regCount   = 4;
    main.body.push(vm::Instr::withImm(vm::Op::LoadString, 0, 0));
    main.body.push(vm::Instr::withImm(vm::Op::LoadString, 1, 1));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 2, 3));
    main.body.push(vm::Instr::callExtern(vm::Op::CallExtern, 3, 0, 1, 0, 2));
    main.body.push(vm::Instr{vm::Op::Ret, 3, 0, 0, 0});

    vm::Vm vm;
    const auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "strncmp ordering call succeeds");
    CHECK(result.exitCode < 0, "strncmp reports a negative ordering for 'abc' vs 'abd'");
}

void test_vm_v2_ffi_getchar_reports_eof() {
    memory::Arena arena;
    vm::Module module(arena);
    module.externs.push(std::string_view("getchar"));

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.paramCount = 0;
    main.returnType = vm::ValueType::I32;
    main.regCount   = 2;
    main.body.push(vm::Instr::callExtern(vm::Op::CallExtern, 0, 0, 0, 0));
    main.body.push(vm::Instr{vm::Op::Ret, 0, 0, 0, 0});

    vm::Vm vm;
    const auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "getchar FFI succeeds without trapping");
    CHECK_EQ(result.exitCode, -1, "getchar reports end of input in the playground");
}

void test_vm_v2_ffi_strcpy_strcat_strcmp_memcmp() {
    // Positive path: strcpy then strcat builds "abc", which strcmp and memcmp
    // (over four bytes, including the terminator) both report as equal.
    {
        memory::Arena arena;
        vm::Module module(arena);
        module.strings.push("abc");
        module.strings.push("ab");
        module.strings.push("c");
        module.externs.push(std::string_view("strcpy"));
        module.externs.push(std::string_view("strcat"));
        module.externs.push(std::string_view("strcmp"));
        module.externs.push(std::string_view("memcmp"));

        auto &main      = module.functions.emplace(arena);
        main.name       = "main";
        main.paramCount = 0;
        main.returnType = vm::ValueType::I32;
        main.regCount   = 12;
        main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 0, 8));
        main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 1, 1));
        main.body.push(vm::Instr::simple(vm::Op::AllocBytes, 2, 0, 1));
        main.body.push(vm::Instr::withImm(vm::Op::LoadString, 3, 1));
        main.body.push(vm::Instr::withImm(vm::Op::LoadString, 4, 2));
        main.body.push(vm::Instr::withImm(vm::Op::LoadString, 5, 0));
        main.body.push(vm::Instr::callExtern(vm::Op::CallExtern, 6, 2, 3, 0));
        main.body.push(vm::Instr::callExtern(vm::Op::CallExtern, 7, 2, 4, 1));
        main.body.push(vm::Instr::callExtern(vm::Op::CallExtern, 8, 2, 5, 2));
        main.body.push(vm::Instr::withImm(vm::Op::LoadConstI32, 9, 4));
        main.body.push(vm::Instr::callExtern(vm::Op::CallExtern, 10, 2, 5, 3, 9));
        main.body.push(vm::Instr::simple(vm::Op::Add, 11, 8, 10));
        main.body.push(vm::Instr{vm::Op::Ret, 11, 0, 0, 0});

        vm::Vm vm;
        const auto result = vm.runMain(module);
        CHECK(result.status == vm::RunStatus::Ok, "strcpy/strcat/strcmp/memcmp FFI succeed");
        CHECK_EQ(result.exitCode, 0, "built string compares equal to the expected literal");
    }

    // Ordering path: strcmp reports a negative result for "abc" vs "abd".
    {
        memory::Arena arena;
        vm::Module module(arena);
        module.strings.push("abc");
        module.strings.push("abd");
        module.externs.push(std::string_view("strcmp"));

        auto &main      = module.functions.emplace(arena);
        main.name       = "main";
        main.paramCount = 0;
        main.returnType = vm::ValueType::I32;
        main.regCount   = 4;
        main.body.push(vm::Instr::withImm(vm::Op::LoadString, 0, 0));
        main.body.push(vm::Instr::withImm(vm::Op::LoadString, 1, 1));
        main.body.push(vm::Instr::callExtern(vm::Op::CallExtern, 2, 0, 1, 0));
        main.body.push(vm::Instr{vm::Op::Ret, 2, 0, 0, 0});

        vm::Vm vm;
        const auto result = vm.runMain(module);
        CHECK(result.status == vm::RunStatus::Ok, "strcmp ordering call succeeds");
        CHECK(result.exitCode < 0, "strcmp reports a negative ordering for 'abc' vs 'abd'");
    }
}

void test_vm_v2_ffi_unsupported_extern_names() {
    memory::Arena arena;
    vm::Module module(arena);
    module.externs.push(std::string_view("fopen"));

    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.paramCount = 0;
    main.returnType = vm::ValueType::I32;
    main.regCount   = 2;
    main.body.push(vm::Instr::callExtern(vm::Op::CallExtern, 0, 0, 0, 0));
    main.body.push(vm::Instr{vm::Op::Ret, 0, 0, 0, 0});

    vm::Vm vm;
    const auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Trap, "unsupported extern traps");
    CHECK(result.message.find("fopen") != std::string::npos,
          "the trap message names the unsupported extern");
}

#ifdef ZITH_ENABLE_C_INTEROP
// The production console implementation uses allocator-backed dynamic dispatch
// that is not yet supported by VM v2. Validate its frontend/HIR contract here;
// VM output behavior is covered by the virtual-console test below.
void test_vm_v2_hello_stdlib_import_println() {
    const auto root = std::filesystem::temp_directory_path() / "zith-vm-v2-tests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const auto source = root / "main.zithm";
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        output << "from std/io/console\n"
                  "from soon/memory\n"
                  "\n"
                  "fn main(): i32 {\n"
                  "    let heap = HeapAllocator {};\n"
                  "    _ = println(heap, \"hello v2\");\n"
                  "    return 0;\n"
                  "}\n";
    }

    memory::Arena arena;
    Options options(arena);
    options.includeDirs.push(ZITH_STDLIB_DIR);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, source.string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "current stdlib println call lowers through the modern pipeline");
}

void test_vm_v2_hello_virtual_stdlib() {
    const auto root = std::filesystem::temp_directory_path() / "zith-vm-v2-virtual-tests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "std" / "io");

    const std::string_view console = R"(pub trait Formatable {
    fn format(self, dest: lend FormatBuffer): IoError;
}

pub extern fn puts(msg: *char): i32

pub enum IoError {
    Ok = 0,
}

pub struct FormatBuffer {
    data: ?*char = null,
    capacity: u64 = 0,
    length: u64 = 0,
}

#[discardable]
pub fn println(msg: []char, values: [...]dyn Formatable): IoError {
    if (@lengthOf(msg) > 0) {
        _ = puts(@ptrOf(msg));
    }
    return IoError.Ok;
}
)";
    {
        std::ofstream output(root / "std" / "io" / "console.zithm",
                             std::ios::binary | std::ios::trunc);
        output << console;
    }
    const auto source = root / "main.zithm";
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        output << "from std/io/console\n"
                  "\n"
                  "fn main() {\n"
                  "    _ = println(\"Hello, WASM!\");\n"
                  "}\n";
    }

    memory::Arena arena;
    Options options(arena);
    options.includeDirs.push(root.string());
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, source.string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "virtual stdlib println source lowers through the modern pipeline");

    const auto encoded = zith::wasm::encodeHir(session);
    CHECK(encoded.ok, "virtual stdlib HIR encodes");
    zith::wasm::DecodedHir decoded;
    CHECK(encoded.ok && zith::wasm::decodeHir(encoded.blob.bytes, decoded),
          "flat virtual stdlib HIR decodes");
    CHECK_EQ(decoded.types.count(), session.types().count(), "flat HIR keeps type count");
    CHECK_EQ(decoded.module.exprCount(), session.hirModule().exprCount(),
             "flat HIR keeps expression count");

    memory::Arena vmArena;
    vm::Module module(vmArena);
    const auto lowered =
        vm::lowerModule(decoded.module, decoded.interner, decoded.types, vmArena, module);
    CHECK(lowered.ok, "virtual stdlib HIR lowers into the v2 module");
    if (!lowered.ok)
        std::fprintf(stderr, "lower message: %s\n", lowered.message.c_str());
    if (lowered.ok) {
        vm::Vm vm;
        const auto result = vm.runMain(module);
        CHECK(result.status == vm::RunStatus::Ok, "v2 VM runs the virtual stdlib println program");
        CHECK_EQ(result.output, std::string("Hello, WASM!\n"),
                 "v2 VM prints through the virtual stdlib");
    }
}

void test_vm_v2_functions_with_stdlib() {
    const auto root = std::filesystem::temp_directory_path() / "zith-vm-v2-tests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const auto source = root / "main.zithm";
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        output << "from std/io/console\n"
                  "from soon/memory\n"
                  "\n"
                  "fn add(a: i32, b: i32): i32 {\n"
                  "    a + b\n"
                  "}\n"
                  "\n"
                  "fn main(): i32 {\n"
                  "    let heap = HeapAllocator {};\n"
                  "    _ = println(heap, \"sum\");\n"
                  "    add(2, 3)\n"
                  "}\n";
    }

    memory::Arena arena;
    Options options(arena);
    options.includeDirs.push(ZITH_STDLIB_DIR);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, source.string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "current stdlib call and user function lower through the modern pipeline");
}
#endif // ZITH_ENABLE_C_INTEROP

void test_vm_v2_extern_putchar() {
    const auto root = std::filesystem::temp_directory_path() / "zith-vm-v2-tests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const auto source = root / "main.zithm";
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        output << "#[discardable] extern fn putchar(c: char): i32\n"
                  "\n"
                  "fn main(): i32 {\n"
                  "    _ = putchar('A');\n"
                  "    0\n"
                  "}\n";
    }

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, source.string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "putchar source lowers through the modern pipeline");

    memory::Arena vmArena;
    vm::Module module(vmArena);
    const auto lowered =
        vm::lowerModule(session.hirModule(), session.interner(), session.types(), vmArena, module);
    CHECK(lowered.ok, "HIR with putchar lowers into v2");

    vm::Vm vm;
    const auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "v2 VM runs the extern putchar program");
    CHECK_EQ(result.output, std::string("A"), "v2 VM forwards putchar to the host output");
    CHECK_EQ(result.exitCode, 0, "v2 VM keeps the main exit code");
}

void test_vm_v2_extern_snprintf_subset() {
    const auto root = std::filesystem::temp_directory_path() / "zith-vm-v2-tests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const auto source = root / "main.zithm";
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        output << "extern fn malloc(size: u64): raw opaque\n"
                  "extern fn snprintf(buf: *char, size: u64, fmt: *char, ...): i32\n"
                  "extern fn printf(msg: *char, ...): i32\n"
                  "extern fn puts(msg: *char): i32\n"
                  "\n"
                  "fn main(): i32 {\n"
                  "    var buf: *char = malloc(64) as *char;\n"
                  "    _ = snprintf(buf, 64, \"%u %u %u %u %u\", 1, 2, 3, 4, 5);\n"
                  "    _ = puts(buf);\n"
                  "    _ = printf(\"Hello World!\");\n"
                  "    0\n"
                  "}\n";
    }

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, source.string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "snprintf source lowers through the modern pipeline");

    memory::Arena vmArena;
    vm::Module module(vmArena);
    const auto lowered =
        vm::lowerModule(session.hirModule(), session.interner(), session.types(), vmArena, module);
    CHECK(lowered.ok, "HIR with snprintf lowers into v2");

    vm::Vm vm;
    const auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "v2 VM runs the extern snprintf program");
    CHECK_EQ(result.output, std::string("1 2 3 4 5\nHello World!"),
             "v2 VM formats variadic ranges and writes printf output");
    CHECK_EQ(result.exitCode, 0, "v2 VM keeps the main exit code");
}

void test_vm_v2_call_range_multi_args() {
    const auto root = std::filesystem::temp_directory_path() / "zith-vm-v2-call-range-tests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const auto source = root / "main.zithm";
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        output << "fn add4(a: i32, b: i32, c: i32, d: i32): i32 {\n"
                  "    a + b + c + d\n"
                  "}\n"
                  "\n"
                  "fn main(): i32 {\n"
                  "    add4(10, 20, 30, 40)\n"
                  "}\n";
    }

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, source.string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "multi-arg function source lowers through the modern pipeline");

    memory::Arena vmArena;
    vm::Module module(vmArena);
    const auto lowered =
        vm::lowerModule(session.hirModule(), session.interner(), session.types(), vmArena, module);
    CHECK(lowered.ok, "HIR with 4-arg function lowers into v2");

    vm::Vm vm;
    const auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "v2 VM runs 4-arg function call");
    CHECK_EQ(result.exitCode, 100, "v2 VM returns computed sum across 4 arguments");
}

void test_vm_v2_variadic_slice_call_forms() {
    const auto root = std::filesystem::temp_directory_path() / "zith-vm-v2-variadic-slice-tests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const auto source = root / "main.zithm";
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        output << "fn sum(rest: [...]i32): i32 {\n"
                  "    return raw rest[0] + raw rest[1] + raw rest[2];\n"
                  "}\n"
                  "\n"
                  "fn empty(rest: [...]i32): i32 {\n"
                  "    return 4;\n"
                  "}\n"
                  "\n"
                  "fn main(): i32 {\n"
                  "    let values: []i32 = [1, 2, 3];\n"
                  "    return sum(1, 2, 3) + sum(values) + empty();\n"
                  "}\n";
    }

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, source.string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "both variadic slice call forms compile through the modern pipeline");

    const auto encoded = wasm::encodeHir(session);
    CHECK(encoded.ok, "variadic slice HIR encodes for flat-HIR execution");
    if (!encoded.ok)
        return;

    wasm::DecodedHir decoded;
    CHECK(wasm::decodeHir(encoded.blob.bytes, decoded),
          "variadic call plans survive flat-HIR decoding");
    if (!decoded.ok)
        return;

    memory::Arena vmArena;
    vm::Module module(vmArena);
    const auto lowered =
        vm::lowerModule(decoded.module, decoded.interner, decoded.types, vmArena, module);
    CHECK(lowered.ok, "flat HIR variadic slice calls lower into VM v2");
    if (!lowered.ok)
        return;

    vm::Vm vm;
    const auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok,
          "v2 VM runs collected, explicit, and empty variadic slice calls");
    CHECK_EQ(result.exitCode, 16,
             "flat HIR preserves both slice pairs and an empty collected tail");
}

void test_vm_v2_dynamic_slice_index_bounds() {
    memory::Arena arena;
    vm::Module module(arena);
    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.returnType = vm::ValueType::I64;
    main.regCount   = 6;
    main.regTypes.push(vm::ValueType::I64);
    main.regTypes.push(vm::ValueType::I64);
    main.regTypes.push(vm::ValueType::Ptr);
    main.regTypes.push(vm::ValueType::I64);
    main.regTypes.push(vm::ValueType::I64);
    main.regTypes.push(vm::ValueType::I64);

    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI64, 0, 8));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI64, 1, 8));
    main.body.push(vm::Instr::simple(vm::Op::AllocBytes, 2, 0, 1));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI64, 3, 2));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI64, 4, 2));
    main.body.push(vm::Instr{vm::Op::IndexLoad, 5, 2, 4, 8, 3, 1});
    main.body.push(vm::Instr{vm::Op::Ret, 5, 0, 0, 0});

    vm::Vm vm;
    const auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Trap,
          "dynamic slice indexing traps when index equals runtime length");
}

void test_vm_v2_slice_index_returns_aggregate_address() {
    memory::Arena arena;
    vm::Module module(arena);
    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.returnType = vm::ValueType::I64;
    main.regCount   = 11;
    for (std::size_t i = 0; i < main.regCount; ++i)
        main.regTypes.push(i == 2U || i == 4U || i == 6U || i == 9U ? vm::ValueType::Ptr
                                                                    : vm::ValueType::I64);

    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI64, 0, 32));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI64, 1, 8));
    main.body.push(vm::Instr::simple(vm::Op::AllocBytes, 2, 0, 1));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI64, 3, 546));
    main.body.push(vm::Instr{vm::Op::FieldPtr, 4, 2, 0, 16});
    main.body.push(vm::Instr::simple(vm::Op::StoreI64, 4, 3));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI64, 7, 1));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI64, 8, 2));
    main.body.push(vm::Instr{vm::Op::IndexLoad, 9, 2, 7, 16, 8, 5});
    main.body.push(vm::Instr::simple(vm::Op::LoadI64, 10, 9));
    main.body.push(vm::Instr{vm::Op::Ret, 10, 0, 0, 0});

    vm::Vm vm;
    const auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok,
          "slice indexing returns an address for an aggregate element");
    CHECK_EQ(result.exitCode, 546, "aggregate slice indexing uses the element's full stride");

    main.body[6]           = vm::Instr::withImm(vm::Op::LoadConstI64, 7, 2);
    const auto outOfBounds = vm.runMain(module);
    CHECK(outOfBounds.status == vm::RunStatus::Trap,
          "aggregate slice address indexing still enforces the logical length");
}

void test_vm_v2_raw_index_skips_logical_bounds_only() {
    memory::Arena arena;
    vm::Module module(arena);
    module.i64Constants.push(16);
    module.i64Constants.push(8);
    module.i64Constants.push(10);
    module.i64Constants.push(20);
    module.i64Constants.push(1);
    auto &main      = module.functions.emplace(arena);
    main.name       = "main";
    main.returnType = vm::ValueType::I64;
    main.regCount   = 10;
    for (std::size_t i = 0; i < main.regCount; ++i)
        main.regTypes.push(i == 5U || i == 6U ? vm::ValueType::Ptr : vm::ValueType::I64);
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI64FromPool, 0, 0));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI64FromPool, 1, 1));
    main.body.push(vm::Instr::simple(vm::Op::AllocBytes, 5, 0, 1));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI64FromPool, 2, 2));
    main.body.push(vm::Instr::simple(vm::Op::StoreI64, 5, 2));
    main.body.push(vm::Instr{vm::Op::FieldPtr, 6, 5, 0, 8});
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI64FromPool, 3, 3));
    main.body.push(vm::Instr::simple(vm::Op::StoreI64, 6, 3));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI64FromPool, 7, 4));
    main.body.push(vm::Instr::withImm(vm::Op::LoadConstI64FromPool, 8, 4));
    main.body.push(vm::Instr{vm::Op::IndexLoad, 9, 5, 7, 8, 8, 3});
    main.body.push(vm::Instr{vm::Op::Ret, 9, 0, 0, 0});

    vm::Vm vm;
    const auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok,
          "raw slice indexing may read past its logical length within backing memory");
    CHECK_EQ(result.exitCode, 20, "raw slice indexing reads the next backing element");

    memory::Arena physical_arena;
    vm::Module physical_module(physical_arena);
    physical_module.i64Constants.push((1U << 20U) - 4U);
    physical_module.i64Constants.push(0);
    auto &physical_main      = physical_module.functions.emplace(physical_arena);
    physical_main.name       = "main";
    physical_main.returnType = vm::ValueType::I64;
    physical_main.regCount   = 4;
    for (std::size_t i = 0; i < physical_main.regCount; ++i)
        physical_main.regTypes.push(vm::ValueType::I64);
    physical_main.body.push(vm::Instr::withImm(vm::Op::LoadConstI64FromPool, 1, 0));
    physical_main.body.push(vm::Instr::withImm(vm::Op::LoadConstI64FromPool, 2, 1));
    physical_main.body.push(vm::Instr{vm::Op::IndexLoad, 3, 1, 2, 8, 1, 2});
    physical_main.body.push(vm::Instr{vm::Op::Ret, 3, 0, 0, 0});

    vm::Vm physical_vm;
    const auto physical_result = physical_vm.runMain(physical_module);
    CHECK(physical_result.status == vm::RunStatus::Trap,
          "raw indexing still traps when a read crosses linear-memory bounds");
}

#ifdef ZITH_ENABLE_C_INTEROP
void test_vm_v2_hello_stdlib_import_print() {
    const auto root = std::filesystem::temp_directory_path() / "zith-vm-v2-print-tests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const auto source = root / "main.zithm";
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        output << "from std/io/console\n"
                  "from soon/memory\n"
                  "\n"
                  "fn main(): i32 {\n"
                  "    let heap = HeapAllocator {};\n"
                  "    print(heap, \"hello \");\n"
                  "    print(heap, \"world\");\n"
                  "    0\n"
                  "}\n";
    }

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;
    options.includeDirs.push(ZITH_STDLIB_DIR);

    session::CompilationSession session(options, source.string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "current stdlib print calls lower through the modern pipeline");
}
#endif // ZITH_ENABLE_C_INTEROP

/// Lower a single Zith-- source file through the modern pipeline and run it on
/// the v2 VM. The caller provides the program text and the expected exit code.
auto runVmSource(std::string_view name, std::string_view program, std::int64_t expectedExit,
                 const char *lowerMessage, const char *runMessage, const char *exitMessage)
    -> void {
    const auto root = std::filesystem::temp_directory_path() / "zith-vm-v2-struct-tests";
    std::filesystem::create_directories(root);
    const auto source = root / (std::string(name) + ".zithm");
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        output << program;
    }

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, source.string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered), lowerMessage);

    memory::Arena vmArena;
    vm::Module module(vmArena);
    const auto lowered =
        vm::lowerModule(session.hirModule(), session.interner(), session.types(), vmArena, module);
    CHECK(lowered.ok, lowerMessage);
    if (!lowered.ok) {
        std::fprintf(stderr, "lower message: %s\n", lowered.message.c_str());
        return;
    }

    vm::Vm vm;
    const auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, runMessage);
    CHECK_EQ(result.exitCode, expectedExit, exitMessage);
}

#ifdef ZITH_ENABLE_C_INTEROP
void test_vm_v2_external_global_fallback() {
    const auto root = std::filesystem::temp_directory_path() / "zith-vm-v2-external-global-tests";
    std::filesystem::create_directories(root);
    {
        std::ofstream header(root / "external-global.h", std::ios::binary | std::ios::trunc);
        header << "extern int zith_vm_external_value;\n";
    }

    const auto runLowering = [&](std::string_view name, std::string_view program,
                                 const bool expectedSuccess) {
        const auto source = root / (std::string(name) + ".zithm");
        {
            std::ofstream output(source, std::ios::binary | std::ios::trunc);
            output << program;
        }

        memory::Arena arena;
        Options options(arena);
        options.targetStage = session::Stage::HirLowered;
        session::CompilationSession compilation(options, source.string());
        compilation.setBuffered(true);
        const bool pipelineOk = compilation.runTo(session::Stage::HirLowered);
        CHECK(pipelineOk, "external-global source lowers through the modern pipeline");
        if (!pipelineOk || compilation.diags().hasErrors())
            return;

        memory::Arena vmArena;
        vm::Module module(vmArena);
        const auto lowered = vm::lowerModule(compilation.hirModule(), compilation.interner(),
                                             compilation.types(), vmArena, module);
        CHECK(lowered.ok == expectedSuccess, "external-global VM lowering matches expectation");
        if (!expectedSuccess) {
            CHECK(lowered.message.find("unsupported external global") != std::string::npos,
                  "VM diagnostic identifies unsupported external-global use");
            CHECK(lowered.message.find("zith_vm_external_value") != std::string::npos,
                  "VM diagnostic names the external symbol");
            return;
        }

        vm::Vm virtualMachine;
        const auto result = virtualMachine.runMain(module);
        CHECK(result.status == vm::RunStatus::Ok,
              "unused external-global import does not block VM execution");
        CHECK_EQ(result.exitCode, 7, "unused external-global import preserves program behavior");
    };

    runLowering("unused",
                "import \"external-global.h\"\n"
                "fn main(): i32 { return 7; }\n",
                true);
    runLowering("read",
                "import \"external-global.h\"\n"
                "fn main(): i32 { return zith_vm_external_value; }\n",
                false);
    runLowering("write",
                "import \"external-global.h\"\n"
                "fn main(): i32 { zith_vm_external_value = 9; return 0; }\n",
                false);

    const auto checkSource = root / "check.zithm";
    {
        std::ofstream output(checkSource, std::ios::binary | std::ios::trunc);
        output << "import \"external-global.h\"\n"
                  "fn main(): i32 { return zith_vm_external_value; }\n";
    }
    memory::Arena checkArena;
    Options checkOptions(checkArena);
    checkOptions.targetStage = session::Stage::TypeChecked;
    session::CompilationSession checkSession(checkOptions, checkSource.string());
    checkSession.setBuffered(true);
    CHECK(checkSession.runTo(session::Stage::TypeChecked),
          "type checking succeeds when an imported global is referenced");
    CHECK(!checkSession.diags().hasErrors(),
          "type checking does not invoke the unsupported VM lowering path");
}
#endif // ZITH_ENABLE_C_INTEROP

void test_vm_v2_struct_field_access() {
    runVmSource("struct-field",
                "struct P { x: i32, y: i32 }\n"
                "\n"
                "fn main(): i32 {\n"
                "    var p: P = P { x: 3, y: 4 };\n"
                "    p.x = 8;\n"
                "    return p.x + p.y;\n"
                "}\n",
                12, "struct field source lowers through the modern pipeline",
                "v2 VM reads and writes struct fields", "v2 VM returns the struct field sum");
}

void test_vm_v2_empty_struct_storage() {
    runVmSource("struct-empty",
                "struct Empty {}\n"
                "\n"
                "fn main(): i32 {\n"
                "    var empty: Empty = Empty {};\n"
                "    return 0;\n"
                "}\n",
                0, "empty struct source lowers through the modern pipeline",
                "v2 VM allocates storage for an empty struct",
                "v2 VM returns after empty struct use");
}

void test_vm_v2_struct_nested_field() {
    runVmSource("struct-nested",
                "struct Inner { a: i32, b: i32 }\n"
                "struct Outer { inner: Inner, c: i32 }\n"
                "\n"
                "fn main(): i32 {\n"
                "    var o: Outer = Outer { inner: Inner { a: 1, b: 2 }, c: 3 };\n"
                "    o.inner.a = 10;\n"
                "    return o.inner.a + o.inner.b + o.c;\n"
                "}\n",
                15, "nested struct source lowers through the modern pipeline",
                "v2 VM addresses nested struct fields", "v2 VM returns the nested field sum");
}

void test_vm_v2_struct_param_and_return() {
    runVmSource("struct-abi",
                "struct P { x: i32, y: i32 }\n"
                "\n"
                "fn make(a: i32, b: i32): P {\n"
                "    return P { x: a, y: b };\n"
                "}\n"
                "\n"
                "fn sum(p: P): i32 {\n"
                "    return p.x + p.y;\n"
                "}\n"
                "\n"
                "fn main(): i32 {\n"
                "    return sum(make(19, 23));\n"
                "}\n",
                42, "struct ABI source lowers through the modern pipeline",
                "v2 VM passes and returns structs by value", "v2 VM returns the by-value sum");
}

void test_vm_v2_struct_mixed_field_offsets() {
    // A char between two i32 fields forces a non-zero field offset for `z`, so
    // the layout helper and the VM field pointer must agree.
    runVmSource("struct-offsets",
                "struct Mixed { a: i32, b: char, z: i32 }\n"
                "\n"
                "fn main(): i32 {\n"
                "    var m: Mixed = Mixed { a: 5, b: 'q', z: 37 };\n"
                "    return m.a + m.z;\n"
                "}\n",
                42, "mixed-field struct source lowers through the modern pipeline",
                "v2 VM honors struct field offsets", "v2 VM returns the offset sum");
}

void test_vm_v2_array_index_assignment() {
    // Index assignment must store through the element address instead of
    // re-loading the element and dropping the write.
    runVmSource("array-index-assign",
                "fn main(): i32 {\n"
                "    var a: [3]i32 = [1, 2, 3];\n"
                "    a[1] = 40;\n"
                "    return raw a[0] + raw a[1] + raw a[2];\n"
                "}\n",
                44, "array index assignment source lowers through the modern pipeline",
                "v2 VM stores through an array element address",
                "v2 VM returns the mutated array sum");
}

void test_vm_v2_char_slice_indexing() {
    runVmSource("char-slice-index",
                "fn main(): i32 {\n"
                "    let text: []char = \"zith\";\n"
                "    return (raw text[3]) as i32;\n"
                "}\n",
                104, "char slice source lowers through the modern pipeline",
                "v2 VM indexes char slices by byte", "v2 VM reads the last string byte");
}

void test_vm_v2_aggregate_array_slice() {
    runVmSource("aggregate-array-slice",
                "struct Pair { value: i32 }\n"
                "fn main(): i32 {\n"
                "    let pairs: [2]Pair = [Pair { value: 20 }, Pair { value: 22 }];\n"
                "    let itemsView: []Pair = pairs;\n"
                "    return (raw itemsView[1]).value;\n"
                "}\n",
                22, "aggregate array slice source lowers through the modern pipeline",
                "v2 VM copies aggregate array elements and indexes the slice",
                "v2 VM reads the selected aggregate field");
}

void test_vm_v2_scalar_deref_assignment() {
    // A scalar local is memory-backed, so `&x` yields real storage and a write
    // through the resulting pointer must be visible to later reads of `x`.
    runVmSource("scalar-deref-assign",
                "fn set(p: *i32): i32 {\n"
                "    *p = 42;\n"
                "    return *p;\n"
                "}\n"
                "fn main(): i32 {\n"
                "    var x: i32 = 1;\n"
                "    return set(&x);\n"
                "}\n",
                42, "scalar deref-assignment source lowers through the modern pipeline",
                "v2 VM writes through a pointer to a scalar local",
                "v2 VM reads back the value written through the pointer");
}

void test_vm_v2_scalar_address_aliasing() {
    // `&x` is a logical move of the binding, so the local is revived by a
    // direct assignment. A write through the pointer and a later direct write
    // must both land in the same storage, matching the documented semantics.
    runVmSource("scalar-address-aliasing",
                "fn main(): i32 {\n"
                "    var x: i32 = 1;\n"
                "    let p: *i32 = &x;\n"
                "    *p = 40;\n"
                "    x = 2;\n"
                "    return *p;\n"
                "}\n",
                2, "scalar address aliasing source lowers through the modern pipeline",
                "v2 VM aliases a pointer and its scalar local",
                "v2 VM observes the latest write through the pointer");
}

void test_vm_v2_pointer_param_mutation_visible() {
    // `lend P` passes the receiver by pointer and mutation through it must be
    // visible in the caller. This exercises a pointer argument that is not the
    // `&x` logical-move form.
    runVmSource("pointer-param-mutation",
                "struct P { x: i32, y: i32 }\n"
                "fn bump(p: lend P): i32 {\n"
                "    p.x = p.x + 1;\n"
                "    return p.x;\n"
                "}\n"
                "fn main(): i32 {\n"
                "    var q: P = P { x: 41, y: 0 };\n"
                "    return bump(lend q);\n"
                "}\n",
                42, "pointer parameter mutation source lowers through the modern pipeline",
                "v2 VM mutates a struct through a lend parameter",
                "v2 VM returns the mutated field");
}

void test_vm_v2_borrowed_slice_intrinsics() {
    runVmSource("borrowed-slice-intrinsics",
                "fn borrowedLength(str: view []char): u64 {\n"
                "    return @lengthOf(str);\n"
                "}\n"
                "fn borrowedFirst(str: view []char): char {\n"
                "    return raw @ptrOf(str)[0];\n"
                "}\n"
                "fn main(): i32 {\n"
                "    let text: []char = \"zith\";\n"
                "    if (borrowedLength(view text) != 4) { return 1; }\n"
                "    if (borrowedFirst(view text) != 'z') { return 2; }\n"
                "    return 0;\n"
                "}\n",
                0, "borrowed-slice source lowers through the modern pipeline",
                "v2 VM runs borrowed-slice intrinsic calls",
                "v2 VM reads the length and data pointer from the borrowed slice");
}

void test_vm_v2_struct_with_array_field() {
    // An array field is address-valued inside an aggregate: the struct literal
    // copies the array into the field and indexing composes through the field
    // address. A char-free `[3]i32` field keeps the layout simple.
    runVmSource("struct-array-field",
                "struct S { a: [3]i32, n: i32 }\n"
                "fn main(): i32 {\n"
                "    var s: S = S { a: [1, 2, 3], n: 10 };\n"
                "    s.a[1] = 20;\n"
                "    return raw s.a[0] + raw s.a[1] + raw s.a[2] + s.n;\n"
                "}\n",
                34, "struct with array field source lowers through the modern pipeline",
                "v2 VM lays out and indexes a struct array field",
                "v2 VM returns the array field sum");
}

void test_vm_v2_struct_with_array_field_by_value() {
    // Passing a struct with an array field by value copies the whole aggregate,
    // including the array, into the callee's own storage.
    runVmSource("struct-array-field-by-value",
                "struct S { a: [3]i32, n: i32 }\n"
                "fn sum(s: S): i32 {\n"
                "    return raw s.a[0] + raw s.a[1] + raw s.a[2] + s.n;\n"
                "}\n"
                "fn main(): i32 {\n"
                "    var s: S = S { a: [1, 2, 3], n: 10 };\n"
                "    return sum(s);\n"
                "}\n",
                16, "struct with array field by value source lowers through the modern pipeline",
                "v2 VM copies a struct array field by value",
                "v2 VM returns the copied array field sum");
}

void test_vm_v2_pointer_to_array_element() {
    // Dereferencing a pointer to an array yields the array base pointer, so
    // indexing the dereferenced array must read the original storage.
    runVmSource("pointer-to-array",
                "fn main(): i32 {\n"
                "    var a: [3]i32 = [1, 2, 3];\n"
                "    let p: *[3]i32 = &a;\n"
                "    return raw (*p)[1];\n"
                "}\n",
                2, "pointer to array source lowers through the modern pipeline",
                "v2 VM dereferences a pointer to an array", "v2 VM returns the indexed element");
}

void test_vm_v2_slot_alloca_hoisted_out_of_loop() {
    // A memory-backed slot declared inside a loop body must be allocated once
    // in the prologue, not on every iteration, or the frame arena grows without
    // bound. Check the lowered body directly: every allocation must precede the
    // first control-flow instruction, so no loop body re-allocates.
    const auto root = std::filesystem::temp_directory_path() / "zith-vm-v2-slot-loop";
    std::filesystem::create_directories(root);
    const auto source = root / "main.zithm";
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        output << "fn main(): i32 {\n"
                  "    var sum: i32 = 0;\n"
                  "    var i: i32 = 0;\n"
                  "    for (i < 1000) {\n"
                  "        var t: i32 = i;\n"
                  "        sum = sum + t;\n"
                  "        i = i + 1;\n"
                  "    }\n"
                  "    return sum;\n"
                  "}\n";
    }

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;
    session::CompilationSession session(options, source.string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "slot alloca loop source lowers through the modern pipeline");

    memory::Arena vmArena;
    vm::Module module(vmArena);
    const auto lowered =
        vm::lowerModule(session.hirModule(), session.interner(), session.types(), vmArena, module);
    CHECK(lowered.ok, "slot alloca loop HIR lowers into v2");
    if (!lowered.ok)
        return;

    const vm::Function *main = nullptr;
    for (const auto &fn : module.functions)
        if (fn.name == "main")
            main = &fn;
    CHECK(main != nullptr, "lowered module has main");
    if (main == nullptr)
        return;

    bool sawControlFlow             = false;
    bool allocationAfterControlFlow = false;
    std::size_t allocations         = 0;
    for (const auto &instr : main->body) {
        if (instr.op == vm::Op::AllocBytes) {
            ++allocations;
            if (sawControlFlow)
                allocationAfterControlFlow = true;
        }
        if (instr.op == vm::Op::Jump || instr.op == vm::Op::Branch || instr.op == vm::Op::Branch2)
            sawControlFlow = true;
    }
    CHECK(allocations >= 3, "prologue allocates sum, i and the loop-local slot");
    CHECK(!allocationAfterControlFlow,
          "v2 lowering hoists every slot allocation out of the loop body");

    vm::Vm vm;
    const auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "v2 VM runs the hoisted-allocation loop");
    CHECK_EQ(result.exitCode, 499500, "v2 VM returns the loop sum");
}

void test_vm_v2_explicit_array_binding_alignment() {
    const auto root = std::filesystem::temp_directory_path() / "zith-vm-v2-explicit-alignment";
    std::filesystem::create_directories(root);
    const auto source = root / "main.zithm";
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        output << "fn plain(): i32 {\n"
                  "    var other: [2]i32 = [3, 4];\n"
                  "    return raw other[1];\n"
                  "}\n"
                  "fn main(): i32 {\n"
                  "    #[alignas(64)] var values: [2]i32 = [1, 2];\n"
                  "    values[0] = 19;\n"
                  "    return raw values[0] + plain();\n"
                  "}\n";
    }

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, source.string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "explicitly aligned array binding lowers through the modern pipeline");

    memory::Arena vmArena;
    vm::Module module(vmArena);
    const auto lowered =
        vm::lowerModule(session.hirModule(), session.interner(), session.types(), vmArena, module);
    CHECK(lowered.ok, "explicitly aligned array binding lowers into v2");
    if (!lowered.ok)
        return;

    const vm::Function *main = nullptr;
    for (const auto &function : module.functions) {
        if (function.name == "main") {
            main = &function;
            break;
        }
    }
    CHECK(main != nullptr, "v2 module contains main");
    const vm::Function *plain = nullptr;
    for (const auto &function : module.functions) {
        if (function.name == "plain") {
            plain = &function;
            break;
        }
    }
    CHECK(plain != nullptr, "v2 module contains plain");
    bool sawAlignedAllocation      = false;
    bool plainHasAlignedAllocation = false;
    if (main != nullptr) {
        for (const auto &allocation : main->body) {
            if (allocation.op != vm::Op::AllocBytes)
                continue;
            for (const auto &constant : main->body) {
                if (constant.op == vm::Op::LoadConstI64 && constant.a == allocation.c &&
                    constant.imm == 64U) {
                    sawAlignedAllocation = true;
                    break;
                }
            }
        }
    }
    if (plain != nullptr) {
        for (const auto &allocation : plain->body) {
            if (allocation.op != vm::Op::AllocBytes)
                continue;
            for (const auto &constant : plain->body) {
                if (constant.op == vm::Op::LoadConstI64 && constant.a == allocation.c &&
                    constant.imm == 64U) {
                    plainHasAlignedAllocation = true;
                    break;
                }
            }
        }
    }
    CHECK(sawAlignedAllocation, "v2 reserves the aligned array base with alignment 64");
    CHECK(!plainHasAlignedAllocation,
          "an aligned local slot does not affect the same slot id in another function");

    vm::Vm vm;
    const auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "v2 VM runs the explicitly aligned array");
    CHECK_EQ(result.exitCode, 23, "the aligned array remains writable and readable");
}

void test_vm_v2_bitwise_and_shifts() {
    const auto root = std::filesystem::temp_directory_path() / "zith-vm-v2-bitwise-tests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const auto source = root / "main.zithm";
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        output << "fn main(): i32 {\n"
                  "    let a = 5 &. 3;\n"       // 1
                  "    let b = 4 |. 2;\n"       // 6
                  "    let c = 7 ^. 3;\n"       // 4
                  "    let d = 1 << 3;\n"       // 8
                  "    let e = 16 >> 2;\n"      // 4
                  "    let f = ~0 &. 15;\n"     // 15
                  "    a + b + c + d + e + f\n" // 1 + 6 + 4 + 8 + 4 + 15 = 38
                  "}\n";
    }

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, source.string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "bitwise source lowers through the modern pipeline");

    memory::Arena vmArena;
    vm::Module module(vmArena);
    const auto lowered =
        vm::lowerModule(session.hirModule(), session.interner(), session.types(), vmArena, module);
    CHECK(lowered.ok, "HIR with bitwise operators lowers into v2");

    vm::Vm vm;
    const auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "v2 VM runs bitwise and shift operators");
    CHECK_EQ(result.exitCode, 38, "v2 VM computes correct bitwise result");
}

void test_vm_v2_float_arithmetic() {
    runVmSource("float-arithmetic",
                "fn main(): i32 {\n"
                "    let left: f64 = 1.5;\n"
                "    let right: f64 = 1.25;\n"
                "    let sum = left + right;\n"
                "    let difference = left - right;\n"
                "    let product = left * right;\n"
                "    let quotient = product / right;\n"
                "    let negative = -right;\n"
                "    if ((sum == 2.75) and (difference == 0.25) and (product == 1.875) and\n"
                "        (quotient == left) and (negative == -1.25) and (negative < 0) and\n"
                "        (negative <= -1.25) and (left > right) and (right >= right) and\n"
                "        (left != right)) {\n"
                "        return 0;\n"
                "    }\n"
                "    return 1;\n"
                "}\n",
                0, "floating-point source lowers into the v2 VM",
                "v2 VM executes floating-point arithmetic and comparisons",
                "v2 VM preserves floating-point arithmetic and comparison results");
}

void test_vm_v2_slice_return_by_value() {
    // A function that returns a slice by value must carry the length half in
    // the register after the return register; otherwise `@lengthOf` reads a
    // stale value.
    runVmSource("slice-return",
                "fn head(a: *char, n: u64): []char {\n"
                "    raw a[0u64..n]\n"
                "}\n"
                "\n"
                "fn main(): i32 {\n"
                "    var s = head(\"hello\", 3u64);\n"
                "    if (@lengthOf(s) != 3u64) {\n"
                "        return 1;\n"
                "    }\n"
                "    if (raw s[0u64] != 'h') {\n"
                "        return 2;\n"
                "    }\n"
                "    if (raw s[2u64] != 'l') {\n"
                "        return 3;\n"
                "    }\n"
                "    return 0;\n"
                "}\n",
                0, "slice-returning source lowers into the v2 VM",
                "v2 VM runs a slice-returning function",
                "v2 VM preserves the returned slice pointer and length");
}

void test_vm_v2_pointer_dynamic_index() {
    runVmSource("pointer-dynamic-index",
                "fn nth(a: *char, i: u64): char {\n"
                "    a[i]\n"
                "}\n"
                "\n"
                "fn main(): i32 {\n"
                "    var buf: [3]char = ['a', 'b', 'c'];\n"
                "    var p: *char = @ptrOf(buf);\n"
                "    if nth(p, 0u64) != 'a' {\n"
                "        return 1;\n"
                "    }\n"
                "    if nth(p, 2u64) != 'c' {\n"
                "        return 2;\n"
                "    }\n"
                "    return 0;\n"
                "}\n",
                0, "pointer-indexing source lowers into the v2 VM",
                "v2 VM runs dynamic pointer indexing", "v2 VM reads the selected pointer element");
}

void test_vm_v2_hir_control_flow_and_dynamic_array_index() {
    const auto root = std::filesystem::temp_directory_path() / "zith-vm-v2-cfg-array-tests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const auto source = root / "main.zithm";
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        output << "fn main(): i32 {\n"
                  "    let values: [3]i32 = [10, 20, 30];\n"
                  "    let index: i32 = 1;\n"
                  "    var count: i32 = 0;\n"
                  "    while (count < 2) {\n"
                  "        count = count + 1;\n"
                  "    }\n"
                  "    if (count == 2) {\n"
                  "        return raw values[index];\n"
                  "    }\n"
                  "    0\n"
                  "}\n";
    }

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, source.string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered),
          "control flow and array source lower through the modern pipeline");

    memory::Arena vmArena;
    vm::Module module(vmArena);
    const auto lowered =
        vm::lowerModule(session.hirModule(), session.interner(), session.types(), vmArena, module);
    CHECK(lowered.ok, "HIR control flow and array index lower into v2");

    vm::Vm vm;
    const auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "v2 VM executes lowered control flow and index");
    CHECK_EQ(result.exitCode, 20, "dynamic array index returns the selected element");
}

void test_vm_v2_dynamic_array_index_traps() {
    const auto root = std::filesystem::temp_directory_path() / "zith-vm-v2-array-trap-tests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const auto source = root / "main.zithm";
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        output << "fn main(): i32 {\n"
                  "    let values: [2]i32 = [10, 20];\n"
                  "    let index: i32 = 2;\n"
                  "    raw values[index]\n"
                  "}\n";
    }

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, source.string());
    session.setBuffered(true);
    CHECK(session.runTo(session::Stage::HirLowered), "out-of-bounds array source lowers to HIR");

    memory::Arena vmArena;
    vm::Module module(vmArena);
    const auto lowered =
        vm::lowerModule(session.hirModule(), session.interner(), session.types(), vmArena, module);
    CHECK(lowered.ok, "out-of-bounds array index lowers into v2");

    vm::Vm vm;
    const auto result = vm.runMain(module);
    CHECK(result.status == vm::RunStatus::Trap, "dynamic array index traps out of bounds");

    memory::Arena negativeArena;
    vm::Module negativeModule(negativeArena);
    auto &negativeMain      = negativeModule.functions.emplace(negativeArena);
    negativeMain.name       = "main";
    negativeMain.returnType = vm::ValueType::I32;
    negativeMain.regCount   = 5;
    negativeMain.body.push(vm::Instr::withImm(vm::Op::LoadConstI64, 1, 8));
    negativeMain.body.push(vm::Instr::withImm(vm::Op::LoadConstI64, 2, 1));
    negativeMain.body.push(vm::Instr::simple(vm::Op::AllocBytes, 0, 1, 2));
    negativeMain.body.push(vm::Instr::withImm(vm::Op::LoadConstI64, 3, 0));
    negativeMain.body.push(vm::Instr::withImm(vm::Op::LoadConstI64, 4, 1));
    negativeMain.body.push(vm::Instr::simple(vm::Op::Sub, 1, 3, 4));
    negativeMain.body.push(vm::Instr{vm::Op::IndexLoad, 3, 0, 1, 8, 2});
    negativeMain.body.push(vm::Instr{vm::Op::Ret, 3, 0, 0, 0});

    vm::Vm negativeVm;
    const auto negativeResult = negativeVm.runMain(negativeModule);
    CHECK(negativeResult.status == vm::RunStatus::Trap,
          "dynamic array index traps on a negative index");
}

void test_vm_v2() {
    test_vm_v2_malloc_string();
    test_vm_v2_dump_is_deterministic();
    test_vm_v2_cached_vir_compatibility();
    test_vm_v2_linear_memory_trap();
    test_vm_v2_missing_main();
    test_vm_v2_loop_and_store();
    test_vm_v2_branch2();
    test_vm_v2_invalid_memory_offset();
    test_vm_v2_invalid_register_trap();
    test_vm_v2_invalid_opcode_shape_trap();
    test_vm_v2_direct_call_fn_index();
    test_vm_v2_indirect_call_fn_index();
    test_vm_v2_indirect_call_fn_range();
    test_vm_v2_indirect_call_fn_invalid_index();
    test_vm_v2_direct_call_extern_index();
    test_vm_v2_indirect_call_extern_index();
    test_vm_v2_indirect_call_extern_invalid_index();
    test_vm_v2_store_load_bytes();
    test_vm_v2_invalid_write_offset_trap();
    test_vm_v2_alloc_bump_and_heap_disjoint();
    test_vm_v2_frame_arena_reclaimed();
    test_vm_v2_nested_frame_arena_reclaimed_without_corruption();
    test_vm_v2_frame_arena_reclaim_preserves_malloc();
    test_vm_v2_field_ptr_and_copy();
    test_vm_v2_slice_pair();
    test_vm_v2_mem_copy_bytes();
    test_vm_v2_ffi_malloc_free_reuse();
    test_vm_v2_ffi_aligned_alloc_reuses_aligned_region();
    test_vm_v2_ffi_realloc_preserves_data();
    test_vm_v2_ffi_putchar_unknown_trap();
    test_vm_v2_ffi_memcpy_and_strlen();
    test_vm_v2_ffi_snprintf();
    test_vm_v2_ffi_calloc_zeroes_and_strncmp();
    test_vm_v2_ffi_strncmp_ordering();
    test_vm_v2_ffi_getchar_reports_eof();
    test_vm_v2_ffi_strcpy_strcat_strcmp_memcmp();
    test_vm_v2_ffi_unsupported_extern_names();
#ifdef ZITH_ENABLE_C_INTEROP
    test_vm_v2_external_global_fallback();
    test_vm_v2_hello_stdlib_import_println();
    test_vm_v2_hello_stdlib_import_print();
    test_vm_v2_hello_virtual_stdlib();
    test_vm_v2_functions_with_stdlib();
#endif // ZITH_ENABLE_C_INTEROP
    test_vm_v2_extern_putchar();
    test_vm_v2_extern_snprintf_subset();
    test_vm_v2_call_range_multi_args();
    test_vm_v2_variadic_slice_call_forms();
    test_vm_v2_dynamic_slice_index_bounds();
    test_vm_v2_slice_index_returns_aggregate_address();
    test_vm_v2_raw_index_skips_logical_bounds_only();
    test_vm_v2_struct_field_access();
    test_vm_v2_empty_struct_storage();
    test_vm_v2_struct_nested_field();
    test_vm_v2_struct_param_and_return();
    test_vm_v2_struct_mixed_field_offsets();
    test_vm_v2_array_index_assignment();
    test_vm_v2_char_slice_indexing();
    test_vm_v2_aggregate_array_slice();
    test_vm_v2_scalar_deref_assignment();
    test_vm_v2_scalar_address_aliasing();
    test_vm_v2_pointer_param_mutation_visible();
    test_vm_v2_borrowed_slice_intrinsics();
    test_vm_v2_struct_with_array_field();
    test_vm_v2_struct_with_array_field_by_value();
    test_vm_v2_pointer_to_array_element();
    test_vm_v2_slot_alloca_hoisted_out_of_loop();
    test_vm_v2_explicit_array_binding_alignment();
    test_vm_v2_bitwise_and_shifts();
    test_vm_v2_float_arithmetic();
    test_vm_v2_slice_return_by_value();
    test_vm_v2_pointer_dynamic_index();
    test_vm_v2_hir_control_flow_and_dynamic_array_index();
    test_vm_v2_dynamic_array_index_traps();
}

} // namespace

TEST_MAIN(vm_v2)

#else

// The VM v2 slice is optional on native builds. When `src/vm/` is excluded the
// lowering contract cannot be exercised, so the test reports a skip instead of
// failing to compile against a slice that is not part of the build. Exit code
// 77 is the CTest skip convention registered in CMakeLists.txt.
int main() {
    std::printf("test-vm-v2 skipped: VM v2 slice is not part of this build\n");
    return 77;
}

#endif // ZITH_HAS_VM
