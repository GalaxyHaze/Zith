#include "cli/options.hpp"
#include "session/compilation-session.hpp"
#include "test-common.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using namespace zith;

namespace {

namespace fs = std::filesystem;

struct Workspace {
    fs::path root = fs::temp_directory_path() / "zith-optional-slice-tests";

    Workspace() {
        fs::remove_all(root);
        fs::create_directories(root);
    }

    ~Workspace() {
        fs::remove_all(root);
    }

    [[nodiscard]] std::string write(std::string_view contents) const {
        const auto destination = root / "main.zithm";
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        output << contents;
        return destination.string();
    }
};

struct CheckResult {
    bool ok = false;
    std::string messages;
    bool hasNullDeref = false;
};

/// Runs the modern file-based pipeline up to type checking.
CheckResult check(std::string_view source) {
    Workspace workspace;
    const auto path = workspace.write(source);

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::TypeChecked;
    session::CompilationSession session(options, path);
    session.setBuffered(true);

    CheckResult result;
    result.ok = session.runTo(session::Stage::TypeChecked);
    for (const auto &diagnostic : session.diags().all()) {
        if (diagnostic.severity != diagnostics::Severity::Error)
            continue;
        result.messages += diagnostic.message;
        result.messages += "\n";
        if (diagnostic.code == diagnostics::err::NullDerefUnproven)
            result.hasNullDeref = true;
    }
    return result;
}

void expectAccepted(std::string_view source, const char *message) {
    const auto result = check(source);
    if (!result.ok)
        std::printf("    diagnostics: %s", result.messages.c_str());
    CHECK(result.ok, message);
}

void expectRejected(std::string_view source, std::string_view expected, const char *message) {
    const auto result = check(source);
    const bool failed = !result.ok && result.messages.find(expected) != std::string::npos;
    if (!failed)
        std::printf("    diagnostics: %s", result.messages.c_str());
    CHECK(failed, message);
}

void expectNullDerefRejected(std::string_view source, std::string_view expected,
                             const char *message) {
    const auto result = check(source);
    const bool failed =
        result.hasNullDeref && !result.ok && result.messages.find(expected) != std::string::npos;
    if (!failed)
        std::printf("    diagnostics: %s", result.messages.c_str());
    CHECK(failed, message);
}

void test_optional_returns_and_bindings() {
    expectAccepted("fn f(): ?i32 { return 42; }\n", "a plain value coerces to an optional return");
    expectAccepted("fn f(): ?i32 { return null; }\n", "null is a valid optional return");
    expectAccepted("fn f(): i32 { let x: ?i32 = 42\n    return 1; }\n",
                   "a plain value coerces to an optional binding");
    expectAccepted("fn f(): i32 { let x: ?i32 = null\n    return 1; }\n",
                   "null initializes an optional binding");
    expectAccepted("fn f(): ?*i32 { return null; }\n", "null is a valid optional pointer return");
}

void test_null_requires_an_annotation() {
    expectRejected("fn f(): i32 { let x = null\n    return 1; }\n",
                   "null requires an optional type annotation",
                   "an unannotated null binding is rejected");
    expectRejected("fn f(): i32 { let x: i32 = null\n    return 1; }\n",
                   "cannot assign 'null' to a non-optional pointer; use '?*T'",
                   "null cannot initialize a non-optional binding");
}

void test_optional_propagation_operator() {
    expectAccepted("fn f(x: ?i32): ?i32 { return x?; }\n",
                   "'?' unwraps an optional inside an optional-returning function");
    expectRejected("fn f(x: ?i32): i32 { return x?; }\n",
                   "'?' operator used in a function that does not return an optional",
                   "'?' is rejected in a function that does not return an optional");
    expectRejected("fn f(x: bool): ?i32 { return x?; }\n",
                   "'?' operator requires an optional operand",
                   "'?' is rejected on a bool operand");
    expectRejected("fn f(x: i32): ?i32 { return x?; }\n",
                   "'?' operator requires an optional operand",
                   "'?' is rejected on an integer operand");
}

void test_optional_across_calls() {
    expectAccepted("fn g(x: ?i32): ?i32 { return x?; }\n"
                   "fn main(): ?i32 { return g(3); }\n",
                   "an integer argument coerces to an optional parameter");
}

void test_slice_and_array_indexing() {
    expectAccepted("fn f(s: []i32): i32 { return raw s[0]; }\n", "a slice can be indexed");
    expectAccepted("fn f(p: ?*i32): i32 { return raw p[0]; }\n",
                   "a raw index bypasses the nullable-pointer proof");
    expectAccepted("fn f(a: [4]i32): i32 { return raw a[2]; }\n",
                   "a fixed-size array can be indexed");
    expectAccepted("fn f(s: []i32): i32 { let t: []i32 = s\n    return raw t[1]; }\n",
                   "a slice can be bound to a slice-typed local");
    expectRejected("fn f(s: []i32): i32 { return s[true]; }\n", "array index must be an integer",
                   "a boolean index is rejected");
    expectRejected("fn f(x: i32): i32 { return x[0]; }\n", "type is not indexable",
                   "indexing a non-indexable type is rejected");
}

void test_range_boundaries_and_empty_ranges() {
    expectAccepted("fn f(): i32 {\n"
                   "    var total: i32 = 0;\n"
                   "    for (i in 5..5) { total = total + 1; }\n"
                   "    for (i in 5>..5) { total = total + 1; }\n"
                   "    for (i in 5..<5) { total = total + 1; }\n"
                   "    for (i in 5>..<5) { total = total + 1; }\n"
                   "    if (5 in 5..5) { return 1; }\n"
                   "    if (5 in 5>..5) { return 2; }\n"
                   "    if (5 in 5..<5) { return 3; }\n"
                   "    if (5 in 5>..<5) { return 4; }\n"
                   "    if (7 in 6>..<8) { return 5; }\n"
                   "    return total;\n"
                   "}\n",
                   "empty and open/closed range boundary semantics are accepted");
    expectRejected("fn f(): bool { return 3 in 1..<4.0; }\n",
                   "range bounds must have the same type", "mixed-type range bounds are rejected");
}

void test_nullable_pointer_narrowing() {
    expectAccepted("struct S { value: i32 }\n"
                   "fn deref(p: ?*i32): i32 {\n"
                   "    if (p is null) { return 0; } else { return *p; }\n"
                   "}\n",
                   "an optional pointer can be dereferenced in the else branch of 'is null'");
    expectAccepted("fn early_return(p: ?*i32): i32 {\n"
                   "    if (p is null) { return 0; }\n"
                   "    return *p;\n"
                   "}\n",
                   "an early-return 'is null' check keeps the pointer proven");
    expectAccepted("fn deref(p: ?*i32): i32 {\n"
                   "    if not (p is null) { return *p; }\n"
                   "    return 0;\n"
                   "}\n",
                   "an optional pointer can be dereferenced after 'not (is null)'");
    expectAccepted("struct S { value: i32 }\n"
                   "fn arrow(p: ?*S): i32 {\n"
                   "    if not (p is null) { return p->value; }\n"
                   "    return 0;\n"
                   "}\n",
                   "an optional pointer can be traversed after 'not (is null)'");
    expectAccepted("fn index(p: ?*i32): i32 {\n"
                   "    if not (p is null) { return p[0]; }\n"
                   "    return 0;\n"
                   "}\n",
                   "an optional pointer can be indexed after 'not (is null)'");
    expectAccepted("fn read(p: *i32): i32 { return *p; }\n"
                   "fn call(p: ?*i32): i32 {\n"
                   "    if not (p is null) { return read(p); }\n"
                   "    return 0;\n"
                   "}\n",
                   "a proven optional pointer coerces to a non-null pointer parameter");
    expectAccepted("fn aggregate(x: ?[2]i32): i32 {\n"
                   "    if (x is null) { return 0; } else { return raw x[0]; }\n"
                   "}\n",
                   "aggregate optional payloads keep narrowing after 'is null'");
    expectAccepted("trait Counter { fn get(self): i32; }\n"
                   "struct Cell { v: i32 }\n"
                   "implement Cell as Counter { fn get(self): i32 { return self.v; } }\n"
                   "fn early(c: ?dyn Counter): i32 {\n"
                   "    if (c is null) { return 0; }\n"
                   "    return c.get();\n"
                   "}\n",
                   "a narrowed ?dyn receiver dispatches after an early-return 'is null'");
    expectAccepted("trait Counter { fn get(self): i32; }\n"
                   "struct Cell { v: i32 }\n"
                   "implement Cell as Counter { fn get(self): i32 { return self.v; } }\n"
                   "fn via_else(c: ?dyn Counter): i32 {\n"
                   "    if (c is null) { return 0; } else { return c.get(); }\n"
                   "}\n",
                   "a narrowed ?dyn receiver dispatches in the else branch of 'is null'");

    expectNullDerefRejected("fn f(p: ?*i32): i32 { return *p; }\n",
                            "cannot dereference a possibly-null pointer",
                            "an unproven optional pointer dereference is rejected");
    expectNullDerefRejected("struct S { value: i32 }\n"
                            "fn f(p: ?*S): i32 { return p->value; }\n",
                            "cannot traverse a possibly-null pointer",
                            "an unproven optional pointer arrow access is rejected");
    expectNullDerefRejected("fn f(p: ?*i32): i32 { return p[0]; }\n",
                            "cannot index a possibly-null pointer",
                            "an unproven optional pointer index is rejected");
    expectNullDerefRejected("fn read(p: *i32): i32 { return *p; }\n"
                            "fn f(p: ?*i32): i32 { return read(p); }\n",
                            "cannot pass a possibly-null pointer",
                            "an unproven optional pointer coercion is rejected");
}

} // namespace

static void test_optional_slice() {
    test_optional_returns_and_bindings();
    test_null_requires_an_annotation();
    test_optional_propagation_operator();
    test_optional_across_calls();
    test_slice_and_array_indexing();
    test_range_boundaries_and_empty_ranges();
    test_nullable_pointer_narrowing();
}

TEST_MAIN(optional_slice)
