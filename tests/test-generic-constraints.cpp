#include "diagnostics/error-codes.hpp"
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
        : opts(arena),
          root(std::filesystem::temp_directory_path() / "zith-generic-constraints-tests") {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        opts.targetStage = session::Stage::TypeChecked;
    }

    ~SessionRunner() {
        std::filesystem::remove_all(root);
    }

    struct Result {
        bool ok = false;

        [[nodiscard]] bool hasErrorCode(diagnostics::ErrCode code) const {
            for (const auto &diag : diags) {
                if (diag.code == code)
                    return true;
            }
            return false;
        }

        [[nodiscard]] size_t errorCount() const {
            size_t count = 0;
            for (const auto &diag : diags) {
                if (diag.code != 0)
                    ++count;
            }
            return count;
        }

        struct Diag {
            diagnostics::ErrCode code{};
            std::string message;
        };
        std::vector<Diag> diags;
    };

    Result run(std::string_view input) {
        const auto path = root / "main.zith";
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << input;
        file.close();

        session::FrontendConfig config;
        config.workspaceRoot      = root.string();
        config.maxFrontendWorkers = 1;
        config.compilerVersion    = "test";
        auto context              = std::make_shared<session::FrontendContext>(config);
        session::CompilationSession session(opts, path.string(), std::move(context));
        session.setBuffered(true);
        const bool ok = session.runTo(session::Stage::TypeChecked);

        std::vector<Result::Diag> copied;
        for (const auto &diag : session.diags().all()) {
            if (diag.severity == diagnostics::Severity::Error) {
                copied.push_back({static_cast<diagnostics::ErrCode>(diag.code), diag.message});
                std::printf("    [GenericConstraintDiag] Code: %u, Message: %s\n", diag.code,
                            diag.message.c_str());
            }
        }
        return {ok && session.diags().errorCount() == 0, std::move(copied)};
    }
};

void test_trait_bound_accepts_conforming_type() {
    SessionRunner t;
    auto r = t.run("trait Printable {\n"
                   "    fn print(self): i32 { return 3 }\n"
                   "}\n"
                   "struct Point { x: i32 }\n"
                   "implement Point as Printable {}\n"
                   "fn log<T: Printable>(v: T): i32 { return v.print() }\n"
                   "fn main(): i32 {\n"
                   "    let p = Point{ x: 1 };\n"
                   "    return log<Point>(p);\n"
                   "}\n");
    CHECK(r.ok, "generic bound with conforming trait method type-checks");
}

void test_trait_bound_rejects_nonconforming_type() {
    SessionRunner t;
    auto r = t.run("trait Printable {\n"
                   "    fn print(self): i32\n"
                   "}\n"
                   "struct Point { x: i32 }\n"
                   "fn log<T: Printable>(v: T): i32 { return 1 }\n"
                   "fn main(): i32 {\n"
                   "    let p = Point{ x: 1 };\n"
                   "    return log<Point>(p);\n"
                   "}\n");
    CHECK(!r.ok, "nonconforming bound fails");
    CHECK(r.hasErrorCode(diagnostics::err::ConstraintNotSatisfied),
          "nonconforming bound reports E3009");
    CHECK_EQ(r.errorCount(), 1u, "nonconforming bound reports exactly one E3009");
}

void test_multiple_bounds_all_evaluated() {
    SessionRunner t;
    auto r = t.run("trait A {}\n"
                   "trait B {}\n"
                   "struct Point { x: i32 }\n"
                   "implement Point as A {}\n"
                   "fn useBoth<T: A + B>(v: T): i32 { return 1 }\n"
                   "fn main(): i32 {\n"
                   "    let p = Point{ x: 1 };\n"
                   "    return useBoth<Point>(p);\n"
                   "}\n");
    CHECK(!r.ok, "all declared bounds must be satisfied");
    CHECK(r.hasErrorCode(diagnostics::err::ConstraintNotSatisfied),
          "unsatisfied second bound reports E3009");
}

void test_method_only_visible_with_bound() {
    SessionRunner t;
    auto r = t.run("trait Printable {\n"
                   "    fn print(self): i32 { return 3 }\n"
                   "}\n"
                   "struct Point { x: i32 }\n"
                   "implement Point as Printable {}\n"
                   "fn log<T>(v: T): i32 { return v.print() }\n"
                   "fn main(): i32 {\n"
                   "    let p = Point{ x: 1 };\n"
                   "    return log<Point>(p);\n"
                   "}\n");
    CHECK(!r.ok, "trait method is not visible without the bound");
}

void test_generic_constraints() {
    test_trait_bound_accepts_conforming_type();
    test_trait_bound_rejects_nonconforming_type();
    test_multiple_bounds_all_evaluated();
    test_method_only_visible_with_bound();
}

} // namespace

TEST_MAIN(generic_constraints)
