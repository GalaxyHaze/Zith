#include "cli/options.hpp"
#include "session/compilation-session.hpp"
#include "test-common.hpp"
#include "vm/hir-to-vm.hpp"
#include "vm/vm-v2.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using namespace zith;

namespace {

enum class VmPhase {
    HirLowering,
    VmLowering,
    Execution,
};

struct VmOutcome {
    VmPhase phase        = VmPhase::HirLowering;
    vm::RunStatus status = vm::RunStatus::Unsupported;
    int64_t exitCode     = 0;
    std::string detail;
};

void test_optional_some_presence_and_must();
void test_optional_none_is_null();
void test_optional_pointer_niche();
void test_optional_must_none_traps();
void test_generic_runtime_panic_code();
void test_implicit_numeric_enum();
void test_explicit_integer_enum();
void test_tagged_union_checks_and_narrowed_casts();
void test_raw_union_cast_in_raw_context();
void test_dyn_method_dispatch();
void test_opaque_checked_extraction();
void test_runtime_pipe_evaluates_source_once();
void test_runtime_do_keeps_chain_value();
void test_runtime_state_transition_returns_final_value();
void test_runtime_state_transfers_are_stackless();

auto runVmProgram(std::string_view name, std::string_view source) -> VmOutcome {
    const auto root = std::filesystem::temp_directory_path() / "zith-vm-v2-gap-tests" / name;
    std::filesystem::create_directories(root);
    const auto sourcePath = root / "main.zithm";
    {
        std::ofstream output(sourcePath, std::ios::binary | std::ios::trunc);
        output << source;
        if (!output.good())
            return {VmPhase::HirLowering, vm::RunStatus::Unsupported, 0,
                    "failed to write gap-test source"};
    }

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;
    session::CompilationSession compilation(options, sourcePath.string());
    compilation.setBuffered(true);
    if (!compilation.runTo(session::Stage::HirLowered)) {
        std::string detail;
        for (const auto &diag : compilation.diags().all()) {
            if (diag.severity != diagnostics::Severity::Error)
                continue;
            if (!detail.empty())
                detail += ": ";
            detail += diag.message;
        }
        return {VmPhase::HirLowering, vm::RunStatus::Unsupported, 0, std::move(detail)};
    }

    memory::Arena vmArena;
    vm::Module module(vmArena);
    const auto lowered = vm::lowerModule(compilation.hirModule(), compilation.interner(),
                                         compilation.types(), vmArena, module);
    if (!lowered.ok)
        return {VmPhase::VmLowering, vm::RunStatus::Unsupported, 0, lowered.message};

    vm::Vm machine;
    const auto result = machine.runMain(module);
    return {VmPhase::Execution, result.status, result.exitCode, result.message};
}

void test_vm_v2_gaps() {
    const auto root = std::filesystem::temp_directory_path() / "zith-vm-v2-gap-tests";
    std::filesystem::create_directories(root);
    const auto source = root / "main.zithm";
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        output << "fn main(): i32 { return 7; }\n";
        CHECK(output.good(), "gap smoke source is written");
    }

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession compilation(options, source.string());
    compilation.setBuffered(true);
    const bool hirReady = compilation.runTo(session::Stage::HirLowered);
    CHECK(hirReady, "pure Zith-- smoke source lowers through HIR");
    if (!hirReady)
        return;

    memory::Arena vmArena;
    vm::Module module(vmArena);
    const auto lowered = vm::lowerModule(compilation.hirModule(), compilation.interner(),
                                         compilation.types(), vmArena, module);
    CHECK(lowered.ok, "smoke source lowers into VM v2");
    if (!lowered.ok)
        return;

    vm::Vm machine;
    const auto result = machine.runMain(module);
    CHECK(result.status == vm::RunStatus::Ok, "VM v2 executes the smoke source");
    CHECK_EQ(result.exitCode, 7, "VM v2 returns the smoke source exit code");

    test_optional_some_presence_and_must();
    test_optional_none_is_null();
    test_optional_pointer_niche();
    test_optional_must_none_traps();
    test_generic_runtime_panic_code();
    test_implicit_numeric_enum();
    test_explicit_integer_enum();
    test_tagged_union_checks_and_narrowed_casts();
    test_raw_union_cast_in_raw_context();
    test_dyn_method_dispatch();
    test_opaque_checked_extraction();
    test_runtime_pipe_evaluates_source_once();
    test_runtime_do_keeps_chain_value();
    test_runtime_state_transition_returns_final_value();
    test_runtime_state_transfers_are_stackless();
}

void test_optional_some_presence_and_must() {
    const auto result =
        runVmProgram("optional-some-presence", "fn main(): i32 {\n"
                                               "    let value: ?i32 = 41;\n"
                                               "    if (value) { return must value; }\n"
                                               "    return 0;\n"
                                               "}\n");
    CHECK(result.phase == VmPhase::Execution, "optional Some and must lower through HIR and VM v2");
    CHECK(result.status == vm::RunStatus::Ok, "must on Some completes successfully");
    CHECK_EQ(result.exitCode, 41, "must on Some returns its payload");
}

void test_optional_none_is_null() {
    const auto result =
        runVmProgram("optional-none-is-null", "fn main(): i32 {\n"
                                              "    let value: ?i32 = null;\n"
                                              "    if (value is null) { return 17; }\n"
                                              "    return 0;\n"
                                              "}\n");
    CHECK(result.phase == VmPhase::Execution, "optional None lowers through VM v2");
    CHECK(result.status == vm::RunStatus::Ok, "is null on None completes successfully");
    CHECK_EQ(result.exitCode, 17, "is null detects an absent optional payload");
}

void test_optional_pointer_niche() {
    const auto result =
        runVmProgram("optional-pointer-niche", "fn main(): i32 {\n"
                                               "    var target: i32 = 23;\n"
                                               "    let absent: ?*i32 = null;\n"
                                               "    if (absent is null) {\n"
                                               "        let present: ?*i32 = &target;\n"
                                               "        return *must present;\n"
                                               "    }\n"
                                               "    return 0;\n"
                                               "}\n");
    CHECK(result.phase == VmPhase::Execution, "?*T niche values lower through VM v2");
    CHECK(result.status == vm::RunStatus::Ok, "None and Some pointer checks complete");
    CHECK_EQ(result.exitCode, 23, "must on a present ?*T returns its pointee value");
}

void test_optional_must_none_traps() {
    const auto result = runVmProgram("optional-must-none", "fn main(): i32 {\n"
                                                           "    let absent: ?i32 = null;\n"
                                                           "    return must absent;\n"
                                                           "}\n");
    CHECK(result.phase == VmPhase::Execution, "must None reaches the VM runtime");
    CHECK(result.status == vm::RunStatus::Trap, "must None traps instead of returning");
    CHECK(result.detail.find("R10003") != std::string::npos,
          "must None reports the specified R10003 failure");
}

void test_generic_runtime_panic_code() {
    // `@assert(false)` outside a test lowers to a `HirRuntimePanic` with the
    // generic `R10004` code. The v2 lowering used to accept only the
    // optional-extraction `R10003` and reported the panic as unsupported; it
    // must now lower any runtime code and report it as a trap.
    const auto result = runVmProgram("generic-runtime-panic", "fn main(): i32 {\n"
                                                              "    @assert(false);\n"
                                                              "    return 0;\n"
                                                              "}\n");
    CHECK(result.phase == VmPhase::Execution, "a generic runtime panic reaches the VM runtime");
    CHECK(result.status == vm::RunStatus::Trap, "a generic runtime panic traps");
    CHECK(result.detail.find("R10004") != std::string::npos,
          "a generic runtime panic reports its R10004 code");
}

void test_implicit_numeric_enum() {
    const auto result =
        runVmProgram("implicit-numeric-enum", "enum Legacy { Base = -4, Next }\n"
                                              "fn main(): i32 {\n"
                                              "    let value: Legacy = Legacy.Next;\n"
                                              "    if (value as i32 == -3) { return 19; }\n"
                                              "    return 0;\n"
                                              "}\n");
    CHECK(result.phase == VmPhase::Execution, "implicit numeric enum lowers through VM v2");
    CHECK(result.status == vm::RunStatus::Ok, "implicit numeric enum executes");
    CHECK_EQ(result.exitCode, 19, "implicit enum discriminants preserve signed values");
}

void test_explicit_integer_enum() {
    const auto result =
        runVmProgram("explicit-integer-enum", "enum ByteFlag: u8 { Off = 2, On = 255 }\n"
                                              "fn main(): i32 {\n"
                                              "    let value: ByteFlag = ByteFlag.On;\n"
                                              "    if (value as i32 == 255) { return 29; }\n"
                                              "    return 0;\n"
                                              "}\n");
    CHECK(result.phase == VmPhase::Execution, "explicit integer enum lowers through VM v2");
    CHECK(result.status == vm::RunStatus::Ok, "explicit integer enum executes");
    CHECK_EQ(result.exitCode, 29, "explicit u8 enum preserves its discriminant");
}

void test_tagged_union_checks_and_narrowed_casts() {
    const auto result =
        runVmProgram("tagged-union-checks", "union Value { i32, bool }\n"
                                            "fn main(): i32 {\n"
                                            "    let number: Value = Value { 42 };\n"
                                            "    if (number is bool) { return 1; }\n"
                                            "    if (number is i32) {\n"
                                            "        let n: i32 = number;\n"
                                            "        let flag: Value = Value { true };\n"
                                            "        if (flag is bool) {\n"
                                            "            let yes: bool = flag;\n"
                                            "            if (yes) { return n; }\n"
                                            "        }\n"
                                            "    }\n"
                                            "    return 0;\n"
                                            "}\n");
    CHECK(result.phase == VmPhase::Execution,
          "tagged union construction, checks, and narrowed casts lower through VM v2");
    CHECK(result.status == vm::RunStatus::Ok, "tagged union member checks complete");
    CHECK_EQ(result.exitCode, 42, "tagged union checks preserve both member payloads");
}

void test_raw_union_cast_in_raw_context() {
    const auto result = runVmProgram("raw-union-cast", "raw union Bits { i32, i64 }\n"
                                                       "fn main(): i32 {\n"
                                                       "    let bits: Bits = Bits { 42 };\n"
                                                       "    let value: i64 = raw bits as i64;\n"
                                                       "    return value as i32;\n"
                                                       "}\n");
    CHECK(result.phase == VmPhase::Execution,
          "raw union construction and cast lower through VM v2");
    CHECK(result.status == vm::RunStatus::Ok, "raw union cast completes successfully");
    CHECK_EQ(result.exitCode, 42, "raw union cast preserves the member payload");
}

void test_dyn_method_dispatch() {
    const auto result =
        runVmProgram("dyn-method-dispatch",
                     "interface Biased {\n"
                     "    fn value(self, bias: i32): i32\n"
                     "}\n"
                     "struct First {\n"
                     "    base: i32,\n"
                     "    fn value(self, bias: i32): i32 { self.base + bias }\n"
                     "}\n"
                     "struct Second {\n"
                     "    base: i32,\n"
                     "    fn value(self, bias: i32): i32 { self.base * bias }\n"
                     "}\n"
                     "fn apply(value: dyn Biased, bias: i32): i32 {\n"
                     "    return value.value(bias);\n"
                     "}\n"
                     "fn main(): i32 {\n"
                     "    return apply(First { base: 10 }, 3) + apply(Second { base: 20 }, 3);\n"
                     "}\n");
    const std::string failure = "D1 dynamic dispatch reaches VM execution: " + result.detail;
    CHECK(result.phase == VmPhase::Execution, failure.c_str());
    CHECK(result.status == vm::RunStatus::Ok, "D1 dynamic dispatch completes successfully");
    CHECK_EQ(result.exitCode, 73, "D1 calls the implementation selected by each dyn value");
}

void test_opaque_checked_extraction() {
    const auto result =
        runVmProgram("opaque-checked-extraction", "fn main(): i32 {\n"
                                                  "    let value: i32 = 41;\n"
                                                  "    let erased: opaque = value as opaque;\n"
                                                  "    if not (erased is i32) { return 1; }\n"
                                                  "    if (erased is bool) { return 2; }\n"
                                                  "    let matching: ?i32 = erased as i32;\n"
                                                  "    if not (matching is null) {\n"
                                                  "        let payload: i32 = matching;\n"
                                                  "        if (payload != 41) { return 4; }\n"
                                                  "    } else {\n"
                                                  "        return 3;\n"
                                                  "    }\n"
                                                  "    let mismatching: ?bool = erased as bool;\n"
                                                  "    if not (mismatching is null) { return 5; }\n"
                                                  "    return 0;\n"
                                                  "}\n");
    const std::string failure = "D2 opaque checks reach VM execution: " + result.detail;
    CHECK(result.phase == VmPhase::Execution, failure.c_str());
    CHECK(result.status == vm::RunStatus::Ok, "D2 checked opaque extraction completes");
    CHECK_EQ(result.exitCode, 0, "D2 matching and mismatching opaque tags are preserved");
}

void test_runtime_pipe_evaluates_source_once() {
    const auto result = runVmProgram("runtime-pipe-source-once",
                                     "struct Counter {\n"
                                     "    value: i32,\n"
                                     "    fn next(self: lend Counter): i32 {\n"
                                     "        self->value = self->value + 1;\n"
                                     "        self->value\n"
                                     "    }\n"
                                     "}\n"
                                     "fn addOne(value: i32): i32 { value + 1 }\n"
                                     "fn main(): i32 {\n"
                                     "    var counter = Counter { value: 0 };\n"
                                     "    let result = counter.next() |> addOne(..);\n"
                                     "    if (result == 2 and counter.value == 1) { return 0; }\n"
                                     "    return 1;\n"
                                     "}\n");
    const std::string failure = "P1 runtime pipe reaches VM execution: " + result.detail;
    CHECK(result.phase == VmPhase::Execution, failure.c_str());
    CHECK(result.status == vm::RunStatus::Ok, "P1 runtime pipe completes successfully");
    CHECK_EQ(result.exitCode, 0, "P1 stage result is 2 and source is evaluated exactly once");
}

void test_runtime_do_keeps_chain_value() {
    const auto result = runVmProgram("runtime-do-keeps-chain-value",
                                     "fn main(): i32 {\n"
                                     "    var observed: i32 = 0;\n"
                                     "    let result = 41 do { observed = ..; };\n"
                                     "    if (observed == 41 and result == 41) { return 0; }\n"
                                     "    return 1;\n"
                                     "}\n");
    const std::string failure = "P2 runtime do reaches VM execution: " + result.detail;
    CHECK(result.phase == VmPhase::Execution, failure.c_str());
    CHECK(result.status == vm::RunStatus::Ok, "P2 runtime do completes successfully");
    CHECK_EQ(result.exitCode, 0, "P2 effect observes 41 and preserves the chain value");
}

void test_runtime_state_transition_returns_final_value() {
    const auto result = runVmProgram("runtime-state-transition", "state Done(value: i32): i32 {\n"
                                                                 "    return value;\n"
                                                                 "}\n"
                                                                 "state Start(value: i32): i32 {\n"
                                                                 "    jump Done(value + 42);\n"
                                                                 "}\n"
                                                                 "fn main(): i32 {\n"
                                                                 "    return dock Start(0);\n"
                                                                 "}\n");
    const std::string failure = "S1 state transition reaches VM execution: " + result.detail;
    CHECK(result.phase == VmPhase::Execution, failure.c_str());
    CHECK(result.status == vm::RunStatus::Ok, "S1 state transition completes successfully");
    CHECK_EQ(result.exitCode, 42, "S1 state transfer returns Done's value");
}

void test_runtime_state_transfers_are_stackless() {
    const auto result =
        runVmProgram("runtime-state-stackless", "state Count(value: i32): i32 {\n"
                                                "    if (value == 0) { return 42; }\n"
                                                "    jump Count(value - 1);\n"
                                                "}\n"
                                                "fn main(): i32 {\n"
                                                "    return dock Count(100000);\n"
                                                "}\n");
    const std::string failure = "state transitions remain stackless: " + result.detail;
    CHECK(result.phase == VmPhase::Execution, failure.c_str());
    CHECK(result.status == vm::RunStatus::Ok, "long state transition chain completes successfully");
    CHECK_EQ(result.exitCode, 42, "long state transition chain returns its terminal value");
}

} // namespace

TEST_MAIN(vm_v2_gaps)
