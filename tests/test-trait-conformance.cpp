#include "diagnostics/error-codes.hpp"
#include "session/compilation-session.hpp"
#include "session/frontend-context.hpp"
#include "test-common.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

using namespace zith;

namespace {

struct SessionRunner {
    memory::Arena arena;
    Options opts;
    std::filesystem::path root;

    SessionRunner(std::string_view label)
        : opts(arena), root(std::filesystem::temp_directory_path() /
                            ("zith-" + std::string(label) + "-trait-tests")) {
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
            copied.push_back({static_cast<diagnostics::ErrCode>(diag.code), diag.message});
            if (diag.severity == diagnostics::Severity::Error) {
                std::printf("    [TraitConformanceDiag] Code: %u, Message: %s\n", diag.code,
                            diag.message.c_str());
            }
        }
        return {ok && session.diags().errorCount() == 0, std::move(copied)};
    }
};

void test_requirement_default_and_override() {
    SessionRunner t("conformance");
    auto r = t.run("trait Printable {\n"
                   "    fn print(self): i32 { return 7 }\n"
                   "}\n"
                   "struct Point { x: i32 }\n"
                   "implement Point as Printable {\n"
                   "    fn print(self): i32 { return self->x }\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    let p = Point{ x: 3 };\n"
                   "    return p.print();\n"
                   "}\n");
    CHECK(r.ok, "trait requirement/impl with matching signature type-checks");
    CHECK(!r.hasErrorCode(diagnostics::err::TraitMethodSignatureMismatch),
          "matching impl signature does not report E2022");
}

void test_default_method_available() {
    SessionRunner t("conformance");
    auto r = t.run("trait Named {\n"
                   "    fn name(self): i32 { return 42 }\n"
                   "}\n"
                   "struct Point { x: i32 }\n"
                   "implement Point as Named {}\n"
                   "fn main(): i32 {\n"
                   "    let p = Point{ x: 1 };\n"
                   "    return p.name();\n"
                   "}\n");
    CHECK(r.ok, "trait default method is callable on a conforming type");
}

void test_self_substitution() {
    SessionRunner t("conformance");
    auto r = t.run("trait IntoValue {\n"
                   "    fn value(self): i32 { return 0 }\n"
                   "}\n"
                   "struct Point { x: i32 }\n"
                   "implement Point as IntoValue {}\n"
                   "fn consume(v: IntoValue): i32 { return 1 }\n"
                   "fn main(): i32 {\n"
                   "    let p = Point{ x: 5 };\n"
                   "    return p.value();\n"
                   "}\n");
    CHECK(r.ok, "Self-bearing defaults are accepted on a conforming owner");
}

void test_duplicate_implementation() {
    SessionRunner t("conformance");
    auto r = t.run("trait Named {}\n"
                   "struct Point { x: i32 }\n"
                   "implement Point as Named {}\n"
                   "implement Point as Named {}\n"
                   "fn main(): i32 { return 1 }\n");
    CHECK(!r.ok, "duplicate implementation fails");
    CHECK(r.hasErrorCode(diagnostics::err::DuplicateImplementation),
          "duplicate implementation reports E2027");
}

void test_missing_requirement() {
    SessionRunner t("conformance");
    auto r = t.run("trait Named {\n"
                   "    fn name(self): i32\n"
                   "}\n"
                   "struct Point { x: i32 }\n"
                   "implement Point as Named {}\n"
                   "fn main(): i32 { return 1 }\n");
    CHECK(!r.ok, "missing requirement fails");
    CHECK(r.hasErrorCode(diagnostics::err::TraitRequirementMissing),
          "missing requirement reports E2021");
}

void test_signature_mismatch() {
    SessionRunner t("conformance");
    auto r = t.run("trait Named {\n"
                   "    fn name(self): i32\n"
                   "}\n"
                   "struct Point { x: i32 }\n"
                   "implement Point as Named {\n"
                   "    fn name(self): f64 { return 1.0 }\n"
                   "}\n"
                   "fn main(): i32 { return 1 }\n");
    CHECK(!r.ok, "signature mismatch fails");
    CHECK(r.hasErrorCode(diagnostics::err::TraitMethodSignatureMismatch),
          "signature mismatch reports E2022");
}

void test_explicit_interface_impl_rejected() {
    SessionRunner t("conformance");
    auto r = t.run("interface Positioned { [x, y]: f32 }\n"
                   "struct Point { x: f32, y: f32 }\n"
                   "implement Point as Positioned {}\n"
                   "fn main(): i32 { return 1 }\n");
    CHECK(!r.ok, "explicit interface implementation fails");
    CHECK(r.hasErrorCode(diagnostics::err::InterfaceMethodNotAllowed),
          "explicit interface implementation reports E2025");
}

void test_higiene() {
    SessionRunner t("conformance");
    auto r = t.run("trait A {\n"
                   "    fn pick(self): i32 { return 1 }\n"
                   "}\n"
                   "trait B {\n"
                   "    fn pick(self): i32 { return 2 }\n"
                   "}\n"
                   "struct Point { x: i32 }\n"
                   "implement Point as A {}\n"
                   "implement Point as B {}\n"
                   "fn main(): i32 {\n"
                   "    let p = Point{ x: 0 };\n"
                   "    return p.pick();\n"
                   "}\n");
    CHECK(!r.ok, "same-named trait defaults without explicit disambiguation are ambiguous");
    CHECK(r.hasErrorCode(diagnostics::err::AmbiguousCall),
          "ambiguous trait default call reports E2008");
}

void test_trait_conformance() {
    test_requirement_default_and_override();
    test_default_method_available();
    test_self_substitution();
    test_duplicate_implementation();
    test_missing_requirement();
    test_signature_mismatch();
    test_explicit_interface_impl_rejected();
    test_higiene();
}

} // namespace

TEST_MAIN(trait_conformance)
