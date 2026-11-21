#include "cli/options.hpp"
#include "diagnostics/diagnostic-engine.hpp"
#include "diagnostics/error-codes.hpp"
#include "hir/hir-expr.hpp"
#include "hir/hir-module.hpp"
#include "session/compilation-session.hpp"
#include "session/frontend-context.hpp"
#include "test-common.hpp"

#include <filesystem>
#include <fstream>
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
        : opts(arena), root(std::filesystem::temp_directory_path() / "zith-parse-input-tests") {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        opts.targetStage = session::Stage::TypeChecked;
#ifdef ZITH_STDLIB_DIR
        opts.includeDirs.push(ZITH_STDLIB_DIR);
#endif
    }

    ~SessionRunner() {
        std::filesystem::remove_all(root);
    }

    void write(std::string_view name, std::string_view text) {
        const auto path = root / name;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
    }

    struct Result {
        bool ok         = false;
        bool reachedHir = false;

        [[nodiscard]] bool hasErrorCode(diagnostics::ErrCode code) const {
            for (const auto &diag : diags) {
                if (diag.code == code)
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool hasMessage(std::string_view needle) const {
            for (const auto &diag : diags) {
                if (diag.message.find(needle) != std::string::npos)
                    return true;
            }
            return false;
        }

        [[nodiscard]] size_t errorCount() const {
            return diags.size();
        }

        struct Diag {
            diagnostics::ErrCode code{};
            std::string message;
        };
        std::vector<Diag> diags;
        std::vector<std::string> hirVtables;
    };

    Result run(std::string_view input, session::Stage target = session::Stage::TypeChecked) {
        write("main.zithm", input);
        return runMain(target);
    }

    Result runMain(session::Stage target = session::Stage::TypeChecked) {
        const auto path = root / "main.zithm";
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

        std::vector<Result::Diag> copied;
        for (const auto &diag : session.diags().all()) {
            if (diag.severity != diagnostics::Severity::Error)
                continue;
            copied.push_back({static_cast<diagnostics::ErrCode>(diag.code), diag.message});
            std::printf("    [ParseInputDiag] Code: %u, Message: %s\n", diag.code,
                        diag.message.c_str());
        }
        Result result;
        result.ok = ok && copied.empty();
        result.reachedHir =
            target == session::Stage::HirLowered && session.hirModule().getFnCount() > 0U;
        result.diags = std::move(copied);
        if (target == session::Stage::HirLowered) {
            const auto &hir = session.hirModule();
            for (size_t i = 0; i < hir.getVTableCount(); ++i) {
                const auto name = session.interner().lookup(hir.getVTable(i).name);
                result.hirVtables.emplace_back(name.data(), name.size());
            }
        }
        return result;
    }
};

void test_primitive_casts_type_check() {
    SessionRunner t;
    auto r = t.run("from std/io/console\n"
                   "from soon/memory\n"
                   "fn main(): i32 {\n"
                   "    let heap = HeapAllocator {};\n"
                   "    let line = input(heap);\n"
                   "    let a = line.cast<i32>();\n"
                   "    let b = line.cast<bool>();\n"
                   "    let c = line.cast<f32>();\n"
                   "    let d = line.cast<f64>();\n"
                   "    let e = line.cast<u32>();\n"
                   "    return 0;\n"
                   "}\n",
                   session::Stage::HirLowered);
    CHECK(r.ok && r.reachedHir,
          "all primitive cast<T> calls type-check and lower through ParseInput");
}

void test_component_field_constraints() {
    SessionRunner valid;
    const auto accepted = valid.run("component Vec2 { x: f32, y: f32 }\n"
                                    "component Batch { values: [4]Vec2, samples: []f32 }\n"
                                    "fn main(): i32 { return 0; }\n");
    CHECK(accepted.ok, "components accept primitives, nested components, arrays, and slices");

    SessionRunner invalid;
    const auto rejected =
        invalid.run("struct Handle { value: i32 }\n"
                    "component HasStruct { value: Handle }\n"
                    "component HasPointer { value: *i32 }\n"
                    "component HasOptional { value: ?*i32 }\n"
                    "component HasBase: Handle { value: i32 }\n"
                    "trait Marker { fn mark(self): i32; }\n"
                    "component Marked { value: i32 }\n"
                    "implement Marked as Marker { fn mark(self): i32 { return self.value; } }\n"
                    "fn main(): i32 { return 0; }\n");
    CHECK(!rejected.ok, "components reject non-POD fields and base/trait declarations");
    CHECK(rejected.hasErrorCode(diagnostics::err::TypeMismatch),
          "invalid component declarations report a semantic type error");
    CHECK(rejected.hasMessage("components cannot implement traits"),
          "components cannot implement traits through an implement block");
}

void test_component_inline_method_constraints() {
    SessionRunner valid;
    const auto accepted =
        valid.run("component Vec2 {\n"
                  "    x: f32,\n"
                  "    y: f32,\n"
                  "    fn lengthSquared(self): f32 { self.x * self.x + self.y * self.y }\n"
                  "    fn dot(self, other: Self): f32 { self.x * other.x + self.y * other.y }\n"
                  "    fn sum(self): f32 { return self.x + self.y; }\n"
                  "}\n"
                  "fn main(): i32 { return 0; }\n");
    CHECK(accepted.ok, "component methods accept arithmetic and field transformations");

    SessionRunner invalid;
    const auto rejected =
        invalid.run("fn helper(value: i32): i32 { value + 1 }\n"
                    "component Invalid {\n"
                    "    value: i32,\n"
                    "    fn calls(self): i32 { helper(self.value) }\n"
                    "    fn mutates(var self): i32 { self.value = 1; self.value }\n"
                    "    fn noValue(self) { return; }\n"
                    "}\n"
                    "fn main(): i32 { return 0; }\n");
    CHECK(!rejected.ok, "component methods reject calls, mutation, and void results");
    CHECK(rejected.hasMessage("component method bodies may only"),
          "calls and mutation report the pure-transformation restriction");
    CHECK(rejected.hasMessage("component methods must return a value"),
          "void component methods report the missing value result");
}

void test_generic_component_rejects_non_pod_instantiation() {
    SessionRunner t;
    const auto rejected = t.run("component Box<T> { value: T }\n"
                                "fn take(box: Box<*i32>) {}\n"
                                "fn main(): i32 { return 0; }\n");
    CHECK(!rejected.ok, "generic component instantiation validates substituted field types");
    CHECK(rejected.hasErrorCode(diagnostics::err::TypeMismatch),
          "non-POD generic component instantiation reports a semantic type error");
    CHECK_EQ(rejected.errorCount(), 1u, "generic component violation is reported only once");
}

void test_imported_generic_component_constraints() {
    SessionRunner t;
    t.write("dep.zithm", "pub component Box<T> { value: T }\n");
    t.write("main.zithm", "from dep\n"
                         "fn take(box: Box<*i32>) {}\n"
                         "fn main(): i32 { return 0; }\n");
    const auto rejected = t.runMain();
    CHECK(!rejected.ok, "imported generic components enforce substituted field constraints");
    CHECK(rejected.hasErrorCode(diagnostics::err::TypeMismatch),
          "an invalid imported component instantiation reports a semantic type error");
}

void test_cast_does_not_consume_line() {
    SessionRunner t;
    auto r = t.run("from std/io/console\n"
                   "from soon/memory\n"
                   "fn parseDemo(line: view InputLine): i32 {\n"
                   "    let n = line.cast<i32>();\n"
                   "    if (n is null) {\n"
                   "        return 1;\n"
                   "    }\n"
                   "    return raw n;\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    let heap = HeapAllocator {};\n"
                   "    let line = input(heap);\n"
                   "    let status = parseDemo(view line);\n"
                   "    _ = line.destroy();\n"
                   "    return status;\n"
                   "}\n",
                   session::Stage::HirLowered);
    CHECK(r.ok && r.reachedHir, "InputLine survives cast<T> when passed by view");
}

void test_nonparsable_type_reports_constraint_error() {
    SessionRunner t;
    auto r = t.run("from std/io/console\n"
                   "from soon/memory\n"
                   "struct NotParsable {\n"
                   "    value: i32\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    let heap = HeapAllocator {};\n"
                   "    let line = input(heap);\n"
                   "    let n = line.cast<NotParsable>();\n"
                   "    return 0;\n"
                   "}\n");
    CHECK(!r.ok, "cast<T> rejects a type that does not implement ParseInput");
    CHECK(r.hasErrorCode(diagnostics::err::ConstraintNotSatisfied),
          "cast<T> reports E3009 for a non-parsable type");
    CHECK_EQ(r.errorCount(), 1u, "cast<T> reports exactly one E3009");
}

void test_println_with_i32_lowers_to_hir() {
    SessionRunner t;
    auto r = t.run("from std/io/console\n"
                   "from soon/memory\n"
                   "fn main(): i32 {\n"
                   "    let heap = HeapAllocator {};\n"
                   "    _ = println(heap, \"n=%#\", 7);\n"
                   "    return 0;\n"
                   "}\n",
                   session::Stage::HirLowered);
    CHECK(r.ok, "println with i32 type-checks and lowers through Formatable");
    bool has_i32_vtable = false;
    for (const auto &name : r.hirVtables) {
        if (name.find("Formatable.i32") != std::string::npos)
            has_i32_vtable = true;
    }
    CHECK(has_i32_vtable, "println HIR contains the Formatable vtable for i32");
}

void test_invalid_alignment_attributes_report_diagnostics() {
    SessionRunner t;
    const std::string invalid_cases[] = {
        "#[alignas(0)] struct Invalid { value: u8 }\n",
        "#[alignas(3)] struct Invalid { value: u8 }\n",
        "#[alignas(value)] struct Invalid { value: u8 }\n",
        "#[alignas(T)] struct Invalid<T> { value: T }\n",
        "#[alignas(8)] #[alignas(16)] struct Invalid { value: u8 }\n",
        "struct Invalid { #[alignas(8)] value: u8 }\n",
        "fn bad(#[alignas(8)] value: u8) {}\n",
        "#[alignas(8)] component Invalid { value: u8 }\n",
        "#[alignas(8)] union Invalid { u8 }\n",
        "#[alignas(8)] enum Invalid { Value }\n",
        "#[alignas(8)] extern var external: u8;\n",
    };

    for (const auto &source : invalid_cases) {
        auto r = t.run(source + "fn main(): i32 { return 0; }\n");
        CHECK(!r.ok, "invalid alignas value or placement is rejected");
        CHECK(r.hasMessage("alignas"),
              "invalid alignas request has an alignment-specific diagnostic");
    }
}

void test_constant_alignment_arguments_are_accepted() {
    SessionRunner t;
    auto r = t.run("#[alignas(64)] const MODULE_CACHE_LINE: i32 = 64;\n"
                   "const CACHE_LINE: i32 = MODULE_CACHE_LINE;\n"
                   "#[alignas(32 * 2)] struct Computed { value: u8 }\n"
                   "#[alignas(CACHE_LINE)] struct Named { value: u8 }\n"
                   "#[noalias] extern fn address(): *i32;\n"
                   "fn main(): i32 { return 0; }\n");
    CHECK(r.ok, "constant alignment and pointer-return noalias attributes are accepted");
}

void test_invalid_local_alignment_reports_once() {
    SessionRunner t;
    auto r = t.run("fn main(): i32 {\n"
                   "    #[alignas(3)] let bytes: [2]u8 = [0, 0];\n"
                   "    return 0;\n"
                   "}\n");
    CHECK(!r.ok, "invalid local alignas value is rejected");
    CHECK(r.hasMessage("alignas"), "invalid local alignas reports an alignment diagnostic");
    CHECK_EQ(r.errorCount(), 1u, "an invalid local alignment is diagnosed only once");
}

void test_noalias_requires_pointer_return() {
    SessionRunner t;
    auto r = t.run("#[noalias] fn bad(): i32 { return 0; }\n"
                   "fn main(): i32 { return 0; }\n");
    CHECK(!r.ok, "noalias on a non-pointer return type is rejected");
    CHECK(r.hasMessage("noalias"), "noalias rejection identifies the attribute");
}

void test_parse_input() {
    test_primitive_casts_type_check();
    test_component_field_constraints();
    test_component_inline_method_constraints();
    test_generic_component_rejects_non_pod_instantiation();
    test_imported_generic_component_constraints();
    test_cast_does_not_consume_line();
    test_nonparsable_type_reports_constraint_error();
    test_println_with_i32_lowers_to_hir();
    test_invalid_alignment_attributes_report_diagnostics();
    test_constant_alignment_arguments_are_accepted();
    test_invalid_local_alignment_reports_once();
    test_noalias_requires_pointer_return();
}

} // namespace

TEST_MAIN(parse_input)
