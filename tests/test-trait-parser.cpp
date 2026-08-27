#include "diagnostics/error-codes.hpp"
#include "frontend/frontend.hpp"
#include "test-common.hpp"

#include <string>
#include <string_view>

using namespace zith;

static bool hasErrorCode(const frontend::FrontendSnapshot &snapshot, uint32_t code) {
    for (const auto &diagnostic : snapshot.diagnostics()) {
        if (diagnostic.isCode(code))
            return true;
    }
    return false;
}

static void test_trait_member_storage() {
    auto snapshot = frontend::parse("trait Printable {\n"
                                    "    fn print(self);\n"
                                    "    fn describe(self): i32 { return 1; }\n"
                                    "}\n"
                                    "interface Positioned { [x, y]: f32 }\n");

    CHECK(snapshot.diagnostics().empty(), "trait and interface bodies parse without diagnostics");

    const frontend::Declaration *trait = nullptr;
    const frontend::Declaration *iface = nullptr;
    for (const auto &decl : snapshot.declarations()) {
        if (decl.kind == frontend::DeclKind::Trait && decl.name == "Printable")
            trait = &decl;
        else if (decl.kind == frontend::DeclKind::Interface && decl.name == "Positioned")
            iface = &decl;
    }
    CHECK(trait != nullptr, "trait declaration is stored");
    CHECK(iface != nullptr, "interface declaration is stored");

    size_t methods = 0;
    bool saw_req   = false;
    bool saw_def   = false;
    for (const auto &decl : snapshot.declarations()) {
        if (decl.kind != frontend::DeclKind::Function || decl.ownerName != "Printable")
            continue;
        ++methods;
        if (decl.name == "print" && !decl.body)
            saw_req = true;
        if (decl.name == "describe" && decl.body)
            saw_def = true;
        CHECK_EQ(decl.ownerName, std::string("Printable"),
                 "trait method owner matches the trait name");
    }
    CHECK_EQ(methods, 2u, "trait stores two nested function declarations");
    CHECK(saw_req, "requirement method without a body is stored");
    CHECK(saw_def, "default method body is stored");

    if (iface != nullptr) {
        CHECK_EQ(iface->parameters.size(), 2u, "grouped interface fields expand to two fields");
        CHECK_EQ(iface->parameters[0].name, std::string("x"), "first interface field is kept");
        CHECK_EQ(iface->parameters[1].name, std::string("y"), "second interface field is kept");
        CHECK(iface->parameters[0].type, "interface field carries a type");
    }
}

static void test_interface_method_diagnostic() {
    auto snapshot = frontend::parse("interface Bad { fn method(self); }\n");

    CHECK_EQ(snapshot.diagnostics().size(), 1u, "interface method reports exactly one diagnostic");
    CHECK(hasErrorCode(snapshot, diagnostics::err::InterfaceMethodNotAllowed),
          "interface method uses E2025 InterfaceMethodNotAllowed");

    auto ungrouped = frontend::parse("interface Ungrouped { x: i32 }\n");
    CHECK(!ungrouped.diagnostics().empty(), "ungrouped interface field is rejected");
    CHECK(hasErrorCode(ungrouped, diagnostics::err::UnsupportedSyntax),
          "ungrouped interface field reports E2010 UnsupportedSyntax");
}

static void test_implement_trait_name_checks() {
    auto undefined_trait      = frontend::parse("implement Foo as MissingTrait { fn m(self); }\n");
    bool saw_implement_record = false;
    for (const auto &record : undefined_trait.implementRecords()) {
        if (record.traitName == "MissingTrait")
            saw_implement_record = true;
    }
    CHECK(saw_implement_record, "missing trait name is preserved for the later semantic pass");
    auto not_trait = frontend::parse("struct NotATrait {}\n"
                                     "struct Foo {}\n"
                                     "implement Foo as NotATrait { fn m(self); }\n");
    CHECK(hasErrorCode(not_trait, diagnostics::err::NotATrait),
          "non-trait implemented name reports E2023 NotATrait");
}

static void test_trait_parser() {
    test_trait_member_storage();
    test_interface_method_diagnostic();
    test_implement_trait_name_checks();
}

TEST_MAIN(trait_parser)
