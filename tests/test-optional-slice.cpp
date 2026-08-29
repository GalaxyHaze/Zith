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
        const auto destination = root / "main.zith";
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        output << contents;
        return destination.string();
    }
};

struct CheckResult {
    bool ok = false;
    std::string messages;
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
    expectAccepted("fn f(a: [4]i32): i32 { return raw a[2]; }\n",
                   "a fixed-size array can be indexed");
    expectAccepted("fn f(s: []i32): i32 { let t: []i32 = s\n    return raw t[1]; }\n",
                   "a slice can be bound to a slice-typed local");
    expectRejected("fn f(s: []i32): i32 { return s[true]; }\n", "array index must be an integer",
                   "a boolean index is rejected");
    expectRejected("fn f(x: i32): i32 { return x[0]; }\n", "type is not indexable",
                   "indexing a non-indexable type is rejected");
}

} // namespace

static void test_optional_slice() {
    test_optional_returns_and_bindings();
    test_null_requires_an_annotation();
    test_optional_propagation_operator();
    test_optional_across_calls();
    test_slice_and_array_indexing();
}

TEST_MAIN(optional_slice)
