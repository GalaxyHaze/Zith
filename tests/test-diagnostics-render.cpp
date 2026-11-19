#include "diagnostics/diagnostic.hpp"
#include "diagnostics/error-codes.hpp"
#include "memory/arena.hpp"
#include "test-common.hpp"

#include <cstring>
#include <string>
#include <string_view>

using namespace zith;
using namespace zith::diagnostics;

namespace {

// ── Error code table ──────────────────────────────────────────────

static void test_existing_codes_preserved() {
    // Contract: E0001, E2002, E1006, E3003, W1008 keep their numbers and meaning
    auto e0001 = lookupError(1001u);
    CHECK(e0001, "E0001 (1001) is registered");
    CHECK_EQ(std::string(e0001->category), "parse", "E0001 category matches");

    auto e2002 = lookupError(2002u);
    CHECK(e2002, "E2002 DuplicateDecl is registered");
    CHECK_EQ(std::string(e2002->category), "semantic", "E2002 category matches");
    CHECK_EQ(std::string(e2002->title), "Duplicate declaration", "E2002 title correct");

    auto e1006 = lookupError(1006u);
    CHECK(e1006, "E1006 (ImportError) is registered");

    auto e1009 = lookupError(1009u);
    CHECK(e1009, "E1009 CircularImport is registered");
    CHECK_EQ(e1009->prefix, 'E', "E1009 has error prefix");
    CHECK_EQ(std::string(e1009->title), "Circular import", "E1009 title correct");

    auto e1011 = lookupError(1011u);
    CHECK(e1011, "E1011 ExpectedConditionParens is registered");
    CHECK_EQ(std::string(e1011->title), "Expected parentheses around control expression",
             "E1011 title covers conditions and when subjects");
    CHECK(e1011->note.find("when (value)") != std::string_view::npos,
          "E1011 help includes a subject example for when");

    auto e1012 = lookupError(1012u);
    CHECK(e1012, "E1012 ExpectedControlBody is registered");
    CHECK_EQ(std::string(e1012->title), "Expected block after control header",
             "E1012 identifies a missing control-flow block");
    CHECK(e1012->note.find("enclosed in braces") != std::string_view::npos,
          "E1012 help explains that control-flow bodies require braces");

    CHECK(e1006->note.find("referenced file exists") != std::string_view::npos,
          "E1006 help applies to imports beyond modules");
    CHECK(e1006->why.find("C headers, and assets") != std::string_view::npos,
          "E1006 help distinguishes module, header, and asset roots");

    auto e3003 = lookupError(3003u);
    CHECK(e3003, "E3003 InvalidCast is registered");
    CHECK_EQ(std::string(e3003->category), "types", "E3003 category matches");

    auto w1008 = lookupError(1008u);
    CHECK(w1008, "W1008 DeprecatedSyntax is registered");
    CHECK_EQ(w1008->prefix, 'W', "W1008 has warning prefix");
}

// ── New dedicated type codes exist ────────────────────────────────

static void test_new_type_codes_registered() {
    // Contract: codes for type mismatch, coercion failure, optional/null
    // violation, cyclic type, and null-deref-unproven exist (3001–3005 range).

    CHECK(lookupError(3001u), "3001 (TypeMismatch) is registered");
    CHECK(lookupError(3002u), "3002 (CannotInfer) is registered");
    CHECK(lookupError(3003u), "3003 (InvalidCast) is registered");
    CHECK(lookupError(3004u), "3004 (CyclicType) is registered");
    CHECK(lookupError(3005u), "3005 (NullDerefUnproven) is registered");
}

// ── Diagnostic struct holds labels and suggestions ────────────────

static void test_diagnostic_holds_labels_and_suggestions() {
    memory::Arena arena;
    memory::Span span{0, 0, 5};

    Diagnostic diag(arena, Severity::Error, 3001, "type mismatch: expected i32, found bool", span);

    // Labels and suggestions are initially empty
    CHECK(diag.labels.empty(), "labels start empty");
    CHECK(diag.suggestions.empty(), "suggestions start empty");

    // Add a secondary label
    Label lab;
    lab.span    = memory::Span{10, 15};
    lab.message = "bool declared here";
    diag.labels.push(lab);
    CHECK(diag.labels.size() == 1, "one label after push");

    // Add a suggestion
    diag.suggestions.push("use .boolToInt() for explicit conversion");
    CHECK(diag.suggestions.size() == 1, "one suggestion after push");

    // Severity helpers
    CHECK(diag.isError(), "isError() true for Error severity");
    CHECK(!diag.isWarning(), "isWarning() false for Error severity");

    Diagnostic warn(arena, Severity::Warning, 1008, "while is deprecated", span);
    CHECK(warn.isWarning(), "isWarning() true for Warning severity");
    CHECK(!warn.isError(), "isError() false for Warning severity");
}

// ── One-line format contract ──────────────────────────────────────

static void test_one_line_format_contract() {
    // Contract: `path:line:col: severity[CODE]: message`
    // Tests that Diagnostic fields hold the data needed to produce this format.

    memory::Arena arena;
    memory::Span span{0, 10, 20};

    Diagnostic diag(arena, Severity::Error, 3001, "type mismatch", span);

    CHECK(diag.severity == Severity::Error, "severity is Error");
    CHECK(diag.code == 3001, "code is 3001");
    CHECK(diag.message == "type mismatch", "message preserved");
    CHECK(diag.primary.start == 10u, "span start preserved");
    CHECK(diag.primary.end == 20u, "span end preserved");

    // Loc struct fields for the format
    memory::Loc loc;
    loc.line = 5;
    loc.col  = 12;
    CHECK(loc.line == 5u, "Loc line is set");
    CHECK(loc.col == 12u, "Loc col is set");
}

// ── Suggestions rendering contract ────────────────────────────────

static void test_suggestions_contract() {
    // Suggestions are stored as DynArray<std::string> and exposed for rendering
    memory::Arena arena;
    memory::Span span{0, 0, 1};

    Diagnostic diag(arena, Severity::Error, 3005, "cannot dereference possibly-null pointer", span);
    diag.suggestions.push("add 'if (p is not null)' guard");
    diag.suggestions.push("use '?' propagation operator");

    CHECK(diag.suggestions.size() == 2, "two suggestions stored");
    CHECK(diag.suggestions[0].find("guard") != std::string::npos,
          "first suggestion contains expected text");
    CHECK(diag.suggestions[1].find("propagation") != std::string::npos,
          "second suggestion contains expected text");
}

// ── Lookup error for unknown code ─────────────────────────────────

static void test_lookup_unknown_code() {
    auto unknown = lookupError(99999u);
    CHECK(!unknown, "lookupError returns nullopt for unregistered code");
}

// ── Severity enum values ──────────────────────────────────────────

static void test_severity_values() {
    CHECK_EQ(static_cast<uint8_t>(Severity::Note), 0, "Note = 0");
    CHECK_EQ(static_cast<uint8_t>(Severity::Warning), 1, "Warning = 1");
    CHECK_EQ(static_cast<uint8_t>(Severity::Error), 2, "Error = 2");
    CHECK_EQ(static_cast<uint8_t>(Severity::Bug), 3, "Bug = 3");
}

// ── All test aggregation ──────────────────────────────────────────

static void test_diagnostics_render() {
    test_existing_codes_preserved();
    test_new_type_codes_registered();
    test_diagnostic_holds_labels_and_suggestions();
    test_one_line_format_contract();
    test_suggestions_contract();
    test_lookup_unknown_code();
    test_severity_values();
}

} // namespace

TEST_MAIN(diagnostics_render)
