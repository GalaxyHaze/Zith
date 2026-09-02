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
        const auto path = root / "main.zith";
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
                   "fn main(): i32 {\n"
                   "    let line = input();\n"
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

void test_cast_does_not_consume_line() {
    SessionRunner t;
    auto r = t.run("from std/io/console\n"
                   "fn parseDemo(line: view InputLine): i32 {\n"
                   "    let n = line.cast<i32>();\n"
                   "    if (n is null) {\n"
                   "        return 1;\n"
                   "    }\n"
                   "    return raw n;\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    let line = input();\n"
                   "    let status = parseDemo(view line);\n"
                   "    line.destroy();\n"
                   "    return status;\n"
                   "}\n",
                   session::Stage::HirLowered);
    CHECK(r.ok && r.reachedHir, "InputLine survives cast<T> when passed by view");
}

void test_nonparsable_type_reports_constraint_error() {
    SessionRunner t;
    auto r = t.run("from std/io/console\n"
                   "struct NotParsable {\n"
                   "    value: i32\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    let line = input();\n"
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
                   "fn main(): i32 {\n"
                   "    println(\"n=%#\", 7);\n"
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

void test_parse_input() {
    test_primitive_casts_type_check();
    test_cast_does_not_consume_line();
    test_nonparsable_type_reports_constraint_error();
    test_println_with_i32_lowers_to_hir();
}

} // namespace

TEST_MAIN(parse_input)
