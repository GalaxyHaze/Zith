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
          root(std::filesystem::temp_directory_path() / "zith-interface-satisfaction-tests") {
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
                std::printf("    [InterfaceSatDiag] Code: %u, Message: %s\n", diag.code,
                            diag.message.c_str());
            }
        }
        return {ok && session.diags().errorCount() == 0, std::move(copied)};
    }
};

void test_structural_satisfaction() {
    SessionRunner t;
    auto r = t.run("interface Positioned { [x, y]: f32 }\n"
                   "struct Point { x: f32, y: f32 }\n"
                   "fn main(): i32 {\n"
                   "    let p = Point{ x: 1.0, y: 2.0 };\n"
                   "    return 1;\n"
                   "}\n");
    CHECK(r.ok, "struct with all interface fields parses and type-checks");
}

void test_interface_bound_accepts_conforming_struct() {
    SessionRunner t;
    auto r = t.run("interface Positioned { [x, y]: f32 }\n"
                   "struct Point { x: f32, y: f32 }\n"
                   "fn moveTo<T: Positioned>(p: T): f32 { return 0.0; }\n"
                   "fn main(): i32 {\n"
                   "    let p = Point{ x: 1.0, y: 2.0 };\n"
                   "    moveTo<Point>(p);\n"
                   "    return 1;\n"
                   "}\n");
    CHECK(r.ok, "struct satisfying an interface bound type-checks");
}

void test_missing_field() {
    SessionRunner t;
    auto r = t.run("interface Positioned { [x, y]: f32 }\n"
                   "struct A { x: f32 }\n"
                   "fn moveTo<T: Positioned>(p: T): f32 { return 0.0; }\n"
                   "fn main(): i32 {\n"
                   "    moveTo<A>(A{ x: 1.0 });\n"
                   "    return 1;\n"
                   "}\n");
    CHECK(!r.ok, "missing interface field fails");
    CHECK(r.hasErrorCode(diagnostics::err::InterfaceNotSatisfied),
          "missing interface field reports E2024");
}

void test_explicit_impl_rejected() {
    SessionRunner t;
    auto r = t.run("interface Positioned { [x, y]: f32 }\n"
                   "struct Point { x: f32, y: f32 }\n"
                   "implement Point as Positioned {}\n"
                   "fn main(): i32 { return 1; }\n");
    CHECK(!r.ok, "explicit interface implementation fails");
    CHECK(r.hasErrorCode(diagnostics::err::InterfaceMethodNotAllowed),
          "explicit interface implementation reports E2025");
}

void test_method_satisfaction() {
    SessionRunner t;
    auto r = t.run("interface Positioned {\n"
                   "    x: f32,\n"
                   "    fn getX(self): f32\n"
                   "}\n"
                   "struct Point {\n"
                   "    x: f32,\n"
                   "    fn getX(self): f32 { return self.x; }\n"
                   "}\n"
                   "fn distance<T: Positioned>(p: T): f32 { return p.x + p.getX(); }\n"
                   "fn main(): i32 {\n"
                   "    let p = Point{ x: 2.0 };\n"
                   "    distance<Point>(p);\n"
                   "    return 1;\n"
                   "}\n");
    CHECK(r.ok, "a type with matching interface fields and methods satisfies the bound");
}

void test_missing_method() {
    SessionRunner t;
    auto r = t.run("interface Positioned {\n"
                   "    x: f32,\n"
                   "    fn getX(self): f32\n"
                   "}\n"
                   "struct Point { x: f32 }\n"
                   "fn distance<T: Positioned>(p: T): f32 { return 0.0; }\n"
                   "fn main(): i32 {\n"
                   "    distance<Point>(Point{ x: 1.0 });\n"
                   "    return 1;\n"
                   "}\n");
    CHECK(!r.ok, "a type missing an interface method fails the bound");
    CHECK(r.hasErrorCode(diagnostics::err::InterfaceNotSatisfied), "missing method reports E2024");
}

void test_method_signature_mismatch() {
    SessionRunner t;
    auto r = t.run("interface Positioned {\n"
                   "    x: f32,\n"
                   "    fn getX(self): f32\n"
                   "}\n"
                   "struct Point {\n"
                   "    x: f32,\n"
                   "    fn getX(self): i32 { return 1; }\n"
                   "}\n"
                   "fn distance<T: Positioned>(p: T): f32 { return 0.0; }\n"
                   "fn main(): i32 {\n"
                   "    distance<Point>(Point{ x: 1.0 });\n"
                   "    return 1;\n"
                   "}\n");
    CHECK(!r.ok, "a type with an incompatible interface method fails the bound");
    CHECK(r.hasErrorCode(diagnostics::err::InterfaceNotSatisfied),
          "signature mismatch reports E2024");
}

void test_qualified_interface_method_selection() {
    SessionRunner t;
    auto r = t.run("interface Left {\n"
                   "    fn add(self, other: i32): i32\n"
                   "}\n"
                   "interface Right {\n"
                   "    fn add(self, other: i32): i32\n"
                   "}\n"
                   "struct Counter {\n"
                   "    value: i32,\n"
                   "    fn add(self, other: i32): i32 { return self.value + other; }\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    let c = Counter{ value: 3 };\n"
                   "    return c.Left.add(1) + c.Right.add(2);\n"
                   "}\n");
    CHECK(r.ok, "interface-qualified method calls select the concrete method for each interface");
    CHECK(!r.hasErrorCode(diagnostics::err::AmbiguousCall),
          "interface-qualified calls do not report E2008");
}

void test_interface_satisfaction() {
    test_structural_satisfaction();
    test_interface_bound_accepts_conforming_struct();
    test_missing_field();
    test_explicit_impl_rejected();
    test_method_satisfaction();
    test_missing_method();
    test_method_signature_mismatch();
    test_qualified_interface_method_selection();
}

} // namespace

TEST_MAIN(interface_satisfaction)
