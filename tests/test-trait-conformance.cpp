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

void test_struct_and_interface_method_coexist() {
    SessionRunner t("conformance");
    auto r = t.run("interface Adds {\n"
                   "    fn add(self, other: i32): i32\n"
                   "}\n"
                   "struct Counter {\n"
                   "    value: i32,\n"
                   "    fn add(self, other: i32): i32 { return self.value + other }\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "    let c = Counter{ value: 1 };\n"
                   "    return c.add(2);\n"
                   "}\n");
    CHECK(r.ok, "struct and interface methods with the same name coexist");
    CHECK(!r.hasErrorCode(diagnostics::err::DuplicateDecl),
          "method bindings do not report duplicate top-level E2002");
}

void test_qualified_trait_method_selection() {
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
                   "    return p.A.pick() + p.B.pick();\n"
                   "}\n");
    CHECK(r.ok, "qualified trait method calls select the requested trait");
    CHECK(!r.hasErrorCode(diagnostics::err::AmbiguousCall),
          "qualified trait calls do not report E2008");
}

void test_qualified_trait_missing_member() {
    SessionRunner t("conformance");
    auto r = t.run("trait A {\n"
                   "    fn pick(self): i32 { return 1 }\n"
                   "}\n"
                   "struct Point { x: i32 }\n"
                   "implement Point as A {}\n"
                   "fn main(): i32 {\n"
                   "    let p = Point{ x: 0 };\n"
                   "    return p.Missing.pick() + p.A.unknown();\n"
                   "}\n");
    CHECK(!r.ok, "missing trait qualification fails");
    CHECK(r.hasErrorCode(diagnostics::err::NoMember),
          "missing trait or method reports E2013-style NoMember");
}

void test_primitive_optional_slice_conformance() {
    SessionRunner t("conformance");
    auto r = t.run("trait Value {\n"
                   "   fn value(self): i32\n"
                   "}\n"
                   "implement i32 as Value {\n"
                   "   fn value(self): i32 { return 7 }\n"
                   "}\n"
                   "trait OptionalValue {\n"
                   "   fn gets(self): i32 { return 11 }\n"
                   "}\n"
                   "implement ?char as OptionalValue {}\n"
                   "trait SliceValue {\n"
                   "   fn len(self): i32\n"
                   "}\n"
                   "implement []u8 as SliceValue {\n"
                   "   fn len(self): i32 { return 5 }\n"
                   "}\n"
                   "fn main(): i32 {\n"
                   "   let a: i32 = 1;\n"
                   "   let o: ?char = 'x';\n"
                   "   let arr: [2]u8 = [1u8, 2u8];\n"
                   "   let s: []u8 = raw arr[0..2];\n"
                   "   return a.value() + o.gets() + s.len();\n"
                   "}\n");
    CHECK(r.ok, "concrete method calls on i32, ?char and []u8 type-check");
    CHECK(!r.hasErrorCode(diagnostics::err::TraitRequirementMissing),
          "required trait methods on primitive and slice owners are implemented");
    CHECK(!r.hasErrorCode(diagnostics::err::TraitMethodSignatureMismatch),
          "implemented signatures match the trait requirements");
}

void test_primitive_optional_slice_duplicates_and_bounds() {
    SessionRunner t("conformance");
    auto duplicate = t.run("trait Value {}\n"
                           "implement i32 as Value {}\n"
                           "implement i32 as Value {}\n"
                           "fn main(): i32 { return 1 }\n");
    CHECK(!duplicate.ok, "a duplicate primitive implementation fails");
    CHECK(duplicate.hasErrorCode(diagnostics::err::DuplicateImplementation),
          "duplicate primitive implementation reports E2027");

    SessionRunner bound_t("bounds");
    auto bounds = bound_t.run("trait Show {\n"
                              "   fn show(self): i32\n"
                              "}\n"
                              "implement i32 as Show { fn show(self): i32 { return 3 } }\n"
                              "implement ?char as Show { fn show(self): i32 { return 4 } }\n"
                              "implement []u8 as Show { fn show(self): i32 { return 5 } }\n"
                              "fn pick<T: Show>(x: T): i32 { return x.show() }\n"
                              "fn main(): i32 {\n"
                              "   let a: i32 = 1;\n"
                              "   let o: ?char = 'x';\n"
                              "   let arr: [2]u8 = [1u8, 2u8];\n"
                              "   let s: []u8 = raw arr[0..2];\n"
                              "   return pick(a) + pick(o) + pick(s);\n"
                              "}\n");
    CHECK(bounds.ok, "generic bounds T: Trait accept i32, ?char and []u8");
}

void test_primitive_slice_missing_requirement() {
    SessionRunner t("conformance");
    auto r = t.run("trait SliceValue {\n"
                   "   fn len(self): i32\n"
                   "}\n"
                   "implement []char as SliceValue {}\n"
                   "fn main(): i32 { return 1 }\n");
    CHECK(!r.ok, "a missing slice requirement fails");
    CHECK(r.hasErrorCode(diagnostics::err::TraitRequirementMissing),
          "missing slice requirement reports E2021");
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
    test_struct_and_interface_method_coexist();
    test_qualified_trait_method_selection();
    test_qualified_trait_missing_member();
    test_primitive_optional_slice_conformance();
    test_primitive_optional_slice_duplicates_and_bounds();
    test_primitive_slice_missing_requirement();
}

} // namespace

TEST_MAIN(trait_conformance)
