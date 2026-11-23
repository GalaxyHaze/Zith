// Shared stdlib parity harness (VM v2 issue #72).
//
// The same stdlib-dependent programs run through the native `zithc` path and
// through the VM v2 path, and the observable behavior (stdout plus exit code)
// is compared. A program that the VM lowers to the unsupported playground
// status (5) while the native path succeeds is a documented divergence and is
// allowed only when the case carries an explicit reason in the table below.
// Any other divergence, including one that runs to completion with different
// output, fails the suite.
#include "test-common.hpp"

#if defined(ZITH_HAS_VM) && defined(ZITHC_BINARY) && defined(ZITH_ENABLE_C_INTEROP)

#include "cli/options.hpp"
#include "session/compilation-session.hpp"
#include "vm/hir-to-vm.hpp"
#include "vm/vm-v2.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace zith;

namespace {

namespace fs = std::filesystem;

// The playground ABI statuses mirror `src/wasm/playground.cpp`. The harness
// runs the VM directly, so it maps `vm::RunStatus` onto that scale to keep the
// "status 5" language in the issue meaningful.
constexpr int kStatusOk          = 0;
constexpr int kStatusTrap        = 3;
constexpr int kStatusOom         = 4;
constexpr int kStatusUnsupported = 5;
constexpr int kStatusCancelled = 6;

struct ParityCase {
    const char *name;
    const char *source;
    // Set only for excluded cases where status 5 is an accepted divergence.
    // The observed VM phase and detail are reported separately.
    const char *allowStatus5Reason;
};

constexpr ParityCase kCases[] = {
    {"optional_none_is_null",
     "fn main(): i32 {\n"
     "    let value: ?i32 = null;\n"
     "    if (value is null) { return 17; }\n"
     "    return 0;\n"
     "}\n",
     nullptr},
    {"optional_some_presence_and_must",
     "fn main(): i32 {\n"
     "    let value: ?i32 = 41;\n"
     "    if (value) { return must value; }\n"
     "    return 0;\n"
     "}\n",
     nullptr},
    {"optional_pointer_niche",
     "fn main(): i32 {\n"
     "    var target: i32 = 23;\n"
     "    let absent: ?*i32 = null;\n"
     "    if (absent is null) {\n"
     "        let present: ?*i32 = &target;\n"
     "        return *must present;\n"
     "    }\n"
     "    return 0;\n"
     "}\n",
     nullptr},
    {"implicit_numeric_enum",
     "enum Legacy { Base = -4, Next }\n"
     "fn main(): i32 {\n"
     "    let value: Legacy = Legacy.Next;\n"
     "    if (value as i32 == -3) { return 19; }\n"
     "    return 0;\n"
     "}\n",
     nullptr},
    {"explicit_integer_enum",
     "enum ByteFlag: u8 { Off = 2, On = 255 }\n"
     "fn main(): i32 {\n"
     "    let value: ByteFlag = ByteFlag.On;\n"
     "    if (value as i32 == 255) { return 29; }\n"
     "    return 0;\n"
     "}\n",
     nullptr},
    {"tagged_union_checks_and_narrowed_casts",
     "union Value { i32, bool }\n"
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
     "}\n",
     nullptr},
    {"raw_union_cast_in_raw_context",
     "raw union Bits { i32, i64 }\n"
     "fn main(): i32 {\n"
     "    let bits: Bits = Bits { 42 };\n"
     "    let value: i64 = raw bits as i64;\n"
     "    return value as i32;\n"
     "}\n",
     nullptr},
    {"dyn_method_dispatch",
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
     "}\n",
     nullptr},
    {"opaque_checked_extraction",
     "fn main(): i32 {\n"
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
     "}\n",
     nullptr},
    {"runtime_pipe_source_once",
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
     "}\n",
     nullptr},
    {"runtime_do_keeps_chain_value",
     "fn main(): i32 {\n"
     "    var observed: i32 = 0;\n"
     "    let result = 41 do { observed = ..; };\n"
     "    if (observed == 41 and result == 41) { return 0; }\n"
     "    return 1;\n"
     "}\n",
     nullptr},
    {"runtime_state_transition",
     "state Done(value: i32): i32 {\n"
     "    return value;\n"
     "}\n"
     "state Start(value: i32): i32 {\n"
     "    jump Done(value + 42);\n"
     "}\n"
     "fn main(): i32 {\n"
     "    return dock Start(0);\n"
     "}\n",
     nullptr},
    // Excluded console/allocator cases remain diagnostic, not parity promises.
    {"console_println_literal",
     "from std/io/console\n"
     "from soon/memory\n"
     "\n"
     "fn main(): i32 {\n"
     "    let heap = HeapAllocator {};\n"
     "    _ = println(heap, \"hello parity\");\n"
     "    return 0;\n"
     "}\n",
     "excluded: console output and dyn Allocator layout depend on external FFI "
     "and allocator behavior"},
    {"console_print_no_newline",
     "from std/io/console\n"
     "from soon/memory\n"
     "\n"
     "fn main(): i32 {\n"
     "    let heap = HeapAllocator {};\n"
     "    print(heap, \"a\");\n"
     "    print(heap, \"b\");\n"
     "    return 0;\n"
     "}\n",
     "excluded: console output and dyn Allocator layout depend on external FFI "
     "and allocator behavior"},
    {"console_println_two_literals",
     "from std/io/console\n"
     "from soon/memory\n"
     "\n"
     "fn main(): i32 {\n"
     "    let heap = HeapAllocator {};\n"
     "    _ = println(heap, \"a\", \"b\");\n"
     "    return 0;\n"
     "}\n",
     "excluded: console output and dyn Allocator layout depend on external FFI "
     "and allocator behavior"},
    {"console_println_format_int",
     "from std/io/console\n"
     "from soon/memory\n"
     "\n"
     "fn main(): i32 {\n"
     "    let heap = HeapAllocator {};\n"
     "    _ = println(heap, \"bucket=#\", 73);\n"
     "    return 0;\n"
     "}\n",
     "excluded: console allocator and variadic Formatable behavior are outside "
     "this parity slice"},

    // These stdlib cases are excluded from acceptance and remain diagnostic.
    {"hash_map_u64_put_get",
     "import soon/collections/hash_map_u64 as hm\n"
     "from soon/memory\n"
     "\n"
     "fn main(): i32 {\n"
     "    let heap = HeapAllocator {};\n"
     "    var map = hm.create(heap);\n"
     "    if not(hm.HashMap.reserve(lend map, 4u64)) {\n"
     "        return 1;\n"
     "    }\n"
     "    if not(hm.HashMap.put(lend map, 1u64, 42u64)) {\n"
     "        return 2;\n"
     "    }\n"
     "    if not(hm.HashMap.contains(view map, 1u64)) {\n"
     "        return 3;\n"
     "    }\n"
     "    let value = hm.HashMap.get(view map, 1u64);\n"
     "    if (value is null or raw value != 42u64) {\n"
     "        return 5;\n"
     "    }\n"
     "    hm.HashMap.destroy(lend map);\n"
     "    return 0;\n"
     "}\n",
     "excluded: stdlib hash-map behavior depends on allocator and external FFI "
     "contracts"},
    {"allocator_heap_dyn_dispatch",
     "from soon/memory\n"
     "\n"
     "fn main(): i32 {\n"
     "    let h = HeapAllocator {};\n"
     "    let mem = allocate(h, 64u64, 1u64);\n"
     "    if (mem is null) {\n"
     "        return 1;\n"
     "    }\n"
     "    deallocate(h, mem, 64u64, 1u64);\n"
     "    return 42;\n"
     "}\n",
     "excluded: dyn Allocator behavior is outside this parity slice"},
    {"inplace_opaque_contract",
     "from soon/memory\n"
     "\n"
     "struct Box {\n"
     "    value: i64,\n"
     "}\n"
     "\n"
     "implement Box as InPlace {\n"
     "    fn inplace(var self, args: opaque): bool {\n"
     "        return true;\n"
     "    }\n"
     "    fn clean(var self) {}\n"
     "}\n"
     "\n"
     "fn main(): i32 {\n"
     "    var box = Box { value: 42 };\n"
     "    if not box.inplace(0) {\n"
     "        return 1;\n"
     "    }\n"
     "    return 42;\n"
     "}\n",
     "excluded: allocator and InPlace contracts are outside this parity slice"},
    {"slice_return_by_value",
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
     nullptr},
    {"pointer_dynamic_index",
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
     nullptr},
    // `soon/string`'s iterator returns a tagged union (`stringCharStep`) whose
    // `End` payload is tested with a qualified `is` from the importing module.
    // This guards the cross-module tagged-union member check on both backends.
    {"soon_string_iterator_end_member",
     "import soon/string as s\n"
     "from soon/memory\n"
     "\n"
     "fn main(): i32 {\n"
     "    let heap = HeapAllocator {};\n"
     "    let a = s.string.init(heap, \"abc\");\n"
     "    var str = raw a;\n"
     "    var it = str.iter();\n"
     "    var seen: i32 = 0;\n"
     "    for (var i = 0), (i < 10), (i = i + 1) {\n"
     "        var step = it.next();\n"
     "        if (step is s.End) { break; }\n"
     "        seen = seen + 1;\n"
     "    }\n"
     "    str.destroy();\n"
     "    return seen;\n"
     "}\n",
     nullptr},
    // An optional aggregate (`?S`) proven present by an early-return
    // `if (x is null) { return ...; }` is read as its payload. Native and VM
    // must agree on the extracted field value.
    {"optional_aggregate_early_return_field",
     "struct Pair { first: i32, second: i32 }\n"
     "\n"
     "fn sum(x: ?Pair): i32 {\n"
     "    if (x is null) { return 0; }\n"
     "    return x.first + x.second;\n"
     "}\n"
     "\n"
     "fn main(): i32 {\n"
     "    let p = Pair { first: 4, second: 5 };\n"
     "    let o: ?Pair = p;\n"
     "    return sum(o);\n"
     "}\n",
     nullptr},
    // A `null` member in an annotated pack literal is retyped from the pack's
    // declared member type, then erased into `opaque` and read back. Native and
    // VM must agree on the surviving `len` member.
    {"null_pack_member_roundtrip",
     "fn readAnnotated(x: opaque): u64 {\n"
     "    let p = x as |bytes: ?*u8, len: u64|;\n"
     "    if (p is null) { return 0u64; }\n"
     "    return p.len;\n"
     "}\n"
     "\n"
     "fn main(): i32 {\n"
     "    let a: |bytes: ?*u8, len: u64| = |null, 7u64|;\n"
     "    if (readAnnotated(a) != 7u64) { return 1; }\n"
     "    return 0;\n"
     "}\n",
     nullptr},
};

std::string readFile(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct Observed {
    enum class VmPhase {
        NotStarted,
        HirLowering,
        VmLowering,
        VmExecution,
    };

    int status    = -1; // playground status: 0 ok, 3 trap, 4 oom, 5 unsupported
    int exitCode  = -1;
    VmPhase phase = VmPhase::NotStarted;
    std::string output;
    std::string detail;
};

const char *vmPhaseName(Observed::VmPhase phase) {
    switch (phase) {
    case Observed::VmPhase::NotStarted:
        return "not-started";
    case Observed::VmPhase::HirLowering:
        return "hir-lowering";
    case Observed::VmPhase::VmLowering:
        return "vm-lowering";
    case Observed::VmPhase::VmExecution:
        return "vm-execution";
    }
    return "unknown";
}

int playgroundStatus(vm::RunStatus status) {
    switch (status) {
    case vm::RunStatus::Ok:
        return kStatusOk;
    case vm::RunStatus::MissingMain:
    case vm::RunStatus::Trap:
        return kStatusTrap;
    case vm::RunStatus::Unsupported:
        return kStatusUnsupported;
    case vm::RunStatus::Oom:
        return kStatusOom;
    case vm::RunStatus::Cancelled:
        return kStatusCancelled;
    }
    return kStatusTrap;
}

const char *playgroundStatusName(int status) {
    switch (status) {
    case kStatusOk:
        return "ok";
    case kStatusTrap:
        return "trap";
    case kStatusOom:
        return "oom";
    case kStatusUnsupported:
        return "status-5";
    case kStatusCancelled:
        return "cancelled";
    }
    return "unknown";
}

/// `zithc` writes `target/` and `cache/` next to the source, so each program
/// runs from its own work directory to keep the tree clean.
Observed runNative(const fs::path &workdir, const fs::path &source) {
    std::error_code ec;
    fs::remove_all(workdir / "cache", ec);
    fs::remove_all(workdir / "target", ec);
    fs::remove_all(workdir / ".zith-cache", ec);

    const fs::path stdoutPath = workdir / "native.stdout";
    const fs::path stderrPath = workdir / "native.stderr";
    const std::string command = std::string("cd \"") + workdir.string() + "\" && \"" +
                                ZITHC_BINARY + "\" --include \"" + ZITH_STDLIB_DIR + "\" run \"" +
                                source.string() + "\" > \"" + stdoutPath.string() + "\" 2> \"" +
                                stderrPath.string() + "\"";
    const int status          = std::system(command.c_str());

    Observed observed;
    if (status < 0) {
        observed.detail = "std::system failed to launch zithc";
        return observed;
    }
    if ((status & 0x7F) != 0) {
        observed.status   = -1;
        observed.exitCode = -1;
        observed.detail   = "zithc terminated by signal";
        return observed;
    }
    observed.status   = kStatusOk;
    observed.exitCode = (status >> 8) & 0xFF;
    observed.output   = readFile(stdoutPath);
    return observed;
}

Observed runVm(const fs::path &source) {
    memory::Arena arena;
    Options options(arena);
    options.includeDirs.push(ZITH_STDLIB_DIR);
    options.targetStage = session::Stage::HirLowered;

    session::CompilationSession session(options, source.string());
    session.setBuffered(true);
    Observed observed;
    observed.phase = Observed::VmPhase::HirLowering;
    if (!session.runTo(session::Stage::HirLowered)) {
        observed.status = kStatusUnsupported;
        for (const auto &diag : session.diags().all()) {
            if (diag.severity != diagnostics::Severity::Error)
                continue;
            if (observed.detail.empty()) {
                observed.detail = diag.message;
            } else {
                observed.detail += ": " + diag.message;
            }
        }
        if (observed.detail.empty())
            observed.detail = "HIR lowering failed without a diagnostic";
        return observed;
    }

    memory::Arena vmArena;
    vm::Module module(vmArena);
    observed.phase = Observed::VmPhase::VmLowering;
    const auto lowered =
        vm::lowerModule(session.hirModule(), session.interner(), session.types(), vmArena, module);
    if (!lowered.ok) {
        observed.status = kStatusUnsupported;
        observed.detail = lowered.message;
        if (observed.detail.empty())
            observed.detail = "VM lowering failed without a diagnostic";
        return observed;
    }

    observed.phase = Observed::VmPhase::VmExecution;
    vm::Vm machine;
    const auto result = machine.runMain(module);
    observed.status   = playgroundStatus(result.status);
    observed.exitCode = static_cast<int>(result.exitCode);
    observed.output   = result.output;
    observed.detail   = result.message;
    if (result.status != vm::RunStatus::Ok && observed.detail.empty())
        observed.detail =
            std::string("VM execution failed with status ") + playgroundStatusName(observed.status);
    return observed;
}

bool sameBehavior(const Observed &native, const Observed &vmRun) {
    return native.status == kStatusOk && vmRun.status == kStatusOk &&
           native.exitCode == vmRun.exitCode && native.output == vmRun.output;
}

void test_parity_requires_success_status() {
    Observed native;
    native.status   = kStatusOk;
    native.exitCode = 0;

    Observed vmRun;
    vmRun.exitCode = native.exitCode;
    vmRun.output   = native.output;

    vmRun.status = kStatusTrap;
    CHECK(!sameBehavior(native, vmRun), "a VM trap cannot match native success");
    vmRun.status = kStatusOom;
    CHECK(!sameBehavior(native, vmRun), "VM OOM cannot match native success");
    vmRun.status = kStatusUnsupported;
    CHECK(!sameBehavior(native, vmRun), "VM unsupported cannot match native success");
    CHECK(playgroundStatus(vm::RunStatus::Trap) == kStatusTrap,
          "VM trap keeps its distinct parity status");
    CHECK(playgroundStatus(vm::RunStatus::Oom) == kStatusOom,
          "VM OOM keeps its distinct parity status");
    CHECK(playgroundStatus(vm::RunStatus::Unsupported) == kStatusUnsupported,
          "VM unsupported keeps its distinct parity status");
    CHECK(playgroundStatus(vm::RunStatus::Cancelled) == kStatusCancelled,
          "VM cancellation keeps its distinct status");
}

} // namespace

void test_stdlib_parity() {
    const fs::path root = fs::temp_directory_path() / "zith-stdlib-parity-tests";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    for (const ParityCase &testCase : kCases) {
        const fs::path workdir = root / testCase.name;
        fs::create_directories(workdir, ec);
        const fs::path source = workdir / "main.zithm";
        {
            std::ofstream out(source, std::ios::binary | std::ios::trunc);
            out << testCase.source;
        }

        const Observed native   = runNative(workdir, source);
        const Observed vmRun    = runVm(source);
        const std::string label = std::string("parity: ") + testCase.name;

        if (native.status != kStatusOk) {
            const std::string message = label + " native path failed to run: " + native.detail +
                                        " (VM phase=" + vmPhaseName(vmRun.phase) +
                                        ", detail=" + vmRun.detail + ")";
            CHECK(false, message.c_str());
            continue;
        }

        if (sameBehavior(native, vmRun)) {
            CHECK(true, label.c_str());
            continue;
        }

        if (vmRun.status == kStatusUnsupported) {
            if (testCase.allowStatus5Reason != nullptr) {
                std::printf("  ALLOW: %s (status 5: %s; VM phase=%s, detail=%s)\n", testCase.name,
                            testCase.allowStatus5Reason, vmPhaseName(vmRun.phase),
                            vmRun.detail.c_str());
                CHECK(true, label.c_str());
            } else {
                const std::string message = label + " diverges with status 5 during " +
                                            vmPhaseName(vmRun.phase) +
                                            " and has no allow-list reason: " + vmRun.detail;
                CHECK(false, message.c_str());
            }
            continue;
        }

        // A divergence where the VM ran but produced different behavior is
        // never acceptable, even for allow-listed programs.
        std::printf("  native(exit=%d out=<%s>) vm(%s phase=%s exit=%d detail=<%s> out=<%s>)\n",
                    native.exitCode, native.output.c_str(), playgroundStatusName(vmRun.status),
                    vmPhaseName(vmRun.phase), vmRun.exitCode, vmRun.detail.c_str(),
                    vmRun.output.c_str());
        const std::string message = label + " diverges during " + vmPhaseName(vmRun.phase) + ": " +
                                    playgroundStatusName(vmRun.status) + " vs native";
        CHECK(false, message.c_str());
    }
}

int main() {
    std::printf("stdlib_parity tests\n");
    std::printf("=====================\n\n");
    g_test_passed = 0;
    g_test_failed = 0;
    test_parity_requires_success_status();
    test_stdlib_parity();
    std::printf("\nResults: %d passed, %d failed\n", g_test_passed, g_test_failed);
    return g_test_failed > 0 ? 1 : 0;
}

#else

int main() {
    std::printf(
        "test-stdlib-parity skipped: needs the VM v2 slice, a zithc target and C interop\n");
    return 77;
}

#endif
