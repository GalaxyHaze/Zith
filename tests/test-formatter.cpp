#include "cli/options.hpp"
#include "formatter/fmt-visitor.hpp"
#include "frontend/frontend.hpp"
#include "memory/arena.hpp"
#include "session/compilation-session.hpp"
#include "test-common.hpp"

#include <string>

using namespace zith;

static void test_formatter_normalizes_supported_snapshot_nodes() {
    const std::string source = "pub   fn  main( left:i32,right : i32):i32{var "
                               "total:i32=left+right;while(total>0){total=total-1;}if(total<0){"
                               "return 1;}else{return total;} }\n";
    auto snapshot            = frontend::parse(source);
    formatter::FmtVisitor formatter(snapshot);
    formatter.format();

    const std::string expected = "pub fn main(left: i32, right: i32): i32 {\n"
                                 "    var total: i32 = left + right;\n"
                                 "    while (total > 0) {\n"
                                 "        total = total - 1;\n"
                                 "    }\n"
                                 "    if (total < 0) {\n"
                                 "        return 1;\n"
                                 "    } else {\n"
                                 "        return total;\n"
                                 "    }\n"
                                 "}\n";

    CHECK_EQ(formatter.result(), expected, "formats stable modern-frontend syntax structurally");
}

static void test_formatter_normalizes_extern_and_unary() {
    const std::string source = "extern    fn   putchar(c:i32):i32\n"
                               "fn   main(){\n"
                               "    var flag: bool = true;\n"
                               "    if (not flag) {\n"
                               "        return 0;\n"
                               "    }\n"
                               "    return   1;\n"
                               "}\n";
    auto snapshot            = frontend::parse(source);
    formatter::FmtVisitor formatter(snapshot);
    formatter.format();

    const std::string expected = "extern fn putchar(c: i32): i32;\n"
                                 "\n"
                                 "fn main() {\n"
                                 "    var flag: bool = true;\n"
                                 "    if (not flag) {\n"
                                 "        return 0;\n"
                                 "    }\n"
                                 "    return 1;\n"
                                 "}\n";

    CHECK_EQ(formatter.result(), expected, "formats extern declarations without body");
}

static void test_formatter_normalizes_pointer_types() {
    const std::string source = "fn  write(ptr:*i32,val:i32){\n"
                               "    ptr = -val;\n"
                               "}\n";
    auto snapshot            = frontend::parse(source);
    formatter::FmtVisitor formatter(snapshot);
    formatter.format();

    const std::string expected = "fn write(ptr: *i32, val: i32) {\n"
                                 "    ptr = -val;\n"
                                 "}\n";

    CHECK_EQ(formatter.result(), expected, "formats pointer type annotations and unary negation");
}

static void test_formatter_nested_if_else() {
    const std::string source = "fn classify(x:i32):i32{if(x>0){if(x>10){return 2;}return "
                               "1;}else{if(x<0){return -1;}return 0;}}\n";
    auto snapshot            = frontend::parse(source);
    formatter::FmtVisitor formatter(snapshot);
    formatter.format();

    const std::string expected = "fn classify(x: i32): i32 {\n"
                                 "    if (x > 0) {\n"
                                 "        if (x > 10) {\n"
                                 "            return 2;\n"
                                 "        }\n"
                                 "        return 1;\n"
                                 "    } else {\n"
                                 "        if (x < 0) {\n"
                                 "            return -1;\n"
                                 "        }\n"
                                 "        return 0;\n"
                                 "    }\n"
                                 "}\n";

    CHECK_EQ(formatter.result(), expected, "formats deeply nested control flow");
}

static void test_formatter_normalizes_simple_import() {
    const std::string source = "import   std/io/console  as  con\n";
    auto snapshot            = frontend::parse(source);
    formatter::FmtVisitor formatter(snapshot);
    formatter.format();

    const std::string expected = "import std/io/console as con\n";

    CHECK_EQ(formatter.result(), expected, "formats simple import declarations");
}

static void test_formatter_break_continue() {
    const std::string source = "fn search(): i32 {\n"
                               "    outer: while (true) {\n"
                               "        break;\n"
                               "        continue;\n"
                               "        break outer;\n"
                               "        continue outer;\n"
                               "    }\n"
                               "    return 0;\n"
                               "}\n";
    auto snapshot            = frontend::parse(source);
    formatter::FmtVisitor formatter(snapshot);
    formatter.format();

    const std::string &output = formatter.result();

    CHECK(output.find("break;\n") != std::string::npos, "keeps break statement");
    CHECK(output.find("continue;\n") != std::string::npos, "keeps continue statement");
    CHECK(output.find("outer: while (true) {\n") != std::string::npos,
          "keeps labeled loop prefix in formatted output");
    CHECK(output.find("break outer;\n") != std::string::npos, "keeps labeled break");
    CHECK(output.find("continue outer;\n") != std::string::npos, "keeps labeled continue");
}

static void test_formatter_empty_file_produces_newline() {
    const std::string source = "\n";
    auto snapshot            = frontend::parse(source);
    formatter::FmtVisitor formatter(snapshot);
    formatter.format();

    CHECK_EQ(formatter.result(), "\n", "empty source produces a single newline");
}

static void test_formatter_multiple_top_level_decls_with_comments() {
    const std::string source = "// Copyright notice\n"
                               "\n"
                               "fn one(): i32 {\n"
                               "    1\n"
                               "}\n"
                               "\n"
                               "/// Doc comment\n"
                               "fn two(): i32 {\n"
                               "    2\n"
                               "}\n";
    auto snapshot            = frontend::parse(source);
    formatter::FmtVisitor formatter(snapshot);
    formatter.format();
    const std::string expected = "// Copyright notice\n"
                                 "\n"
                                 "fn one(): i32 {\n"
                                 "    1;\n"
                                 "}\n"
                                 "\n"
                                 "/// Doc comment\n"
                                 "fn two(): i32 {\n"
                                 "    2;\n"
                                 "}\n";

    CHECK_EQ(formatter.result(), expected,
             "multiple declarations separated by blank line preserve comments");
}

static void test_formatter_parse_error_produces_empty() {
    const std::string source = "fn broken { missing paren";
    auto snapshot            = frontend::parse(source);
    CHECK(!snapshot.diagnostics().empty(), "source with syntax errors still parses");
    formatter::FmtVisitor formatter(snapshot);
    formatter.format();

    CHECK(!formatter.result().empty(), "formatter still produces output for parse error input");
}

static void test_formatter_preserves_unsupported_subtrees() {
    const std::string source = "pub   struct   Pair{ left :i32,\n"
                               "    right : i32,\n"
                               "}\n";
    auto snapshot            = frontend::parse(source);
    formatter::FmtVisitor formatter(snapshot);
    formatter.format();
    const std::string &output = formatter.result();

    CHECK(output.starts_with("pub struct Pair {"), "normalizes the supported outer declaration");
    CHECK(output.find("{ left :i32,\n    right : i32,\n}") != std::string::npos,
          "preserves the unsupported subtree verbatim");
}

static void test_formatter_preserves_comments() {
    const std::string source = "/// docs\n"
                               "fn main() {\n"
                               "    // keep line\n"
                               "    return 42;\n"
                               "}\n"
                               "\n"
                               "struct Pair{/* keep block */ left:i32 }\n";
    auto snapshot            = frontend::parse(source);
    formatter::FmtVisitor formatter(snapshot);
    formatter.format();
    const std::string &output = formatter.result();

    CHECK(output.find("/// docs") != std::string::npos, "preserves doc comments");
    CHECK(output.find("// keep line") != std::string::npos, "preserves line comments");
    CHECK(output.find("/* keep block */") != std::string::npos, "preserves block comments");
}

static void test_formatter_index_and_optional_round_trip() {
    const std::string source = "fn f(s: []i32, x: ?i32): ?i32 {\n"
                               "    var a: i32 = s[1]\n"
                               "    return x?\n"
                               "}\n";
    auto snapshot            = frontend::parse(source);
    formatter::FmtVisitor formatter(snapshot);
    formatter.format();
    const std::string &output = formatter.result();

    CHECK(output.find("s[1]") != std::string::npos, "formats index expressions without parens");
    CHECK(output.find("return x?;") != std::string::npos,
          "formats the postfix '?' without wrapping the operand");

    auto reparsed = frontend::parse(output);
    CHECK(reparsed.diagnostics().empty(), "formatter output re-parses without diagnostics");

    formatter::FmtVisitor second(reparsed);
    second.format();
    CHECK_EQ(second.result(), output, "formatting is idempotent for index and optional");
}

static void test_compilation_session_fmt_uses_frontend_snapshot() {
    memory::Arena arena;
    Options opts(arena);
    opts.command = Options::Command::Fmt;

    session::CompilationSession session(opts, "formatter-session.zith");
    session.setBuffered(true);
    session.setContent("fn main(){return 42;}\n");

    const std::string formatted = session.fmtStage();

    CHECK(!formatted.empty(), "fmt stage produces output through the modern formatter");
    CHECK(session.snapshot() != nullptr, "fmt stage builds a modern frontend snapshot");
}

static void test_formatter_memory_qualifier_round_trip() {
    // Every qualifier must survive `zithc fmt`; losing one would silently change
    // ownership and mutability.
    // Zith-- keeps `lend`/`view`; removed qualifiers are still formatter-tolerated
    // for source preservation, but their re-parse now reports diagnostics.
    const char *qualified[] = {
        "fn f(p: lend i32) {}\n",     "fn f(p: view i32) {}\n",   "fn f(p: unique i32) {}\n",
        "fn f(p: share i32) {}\n",    "fn f(p: belong i32) {}\n", "fn f(p: mut i32) {}\n",
        "fn f(p: mut lend i32) {}\n", "fn f(p: lend *i32) {}\n",  "fn f(p: ?view i32) {}\n",
        "fn f(): view i32 {}\n",
    };

    for (const char *source : qualified) {
        auto snapshot = frontend::parse(source);
        formatter::FmtVisitor formatter(snapshot);
        formatter.format();
        CHECK_EQ(formatter.result(), std::string(source), "qualified type round-trips through fmt");

        auto reparsed = frontend::parse(formatter.result());
        if (std::string_view(source).find("unique") == std::string_view::npos &&
            std::string_view(source).find("share") == std::string_view::npos &&
            std::string_view(source).find("belong") == std::string_view::npos &&
            std::string_view(source).find("mut") == std::string_view::npos) {
            CHECK(reparsed.diagnostics().empty(), "formatted qualified type re-parses cleanly");
        }
        formatter::FmtVisitor second(reparsed);
        second.format();
        CHECK_EQ(second.result(), formatter.result(), "qualifier formatting is idempotent");
    }
}

static void test_formatter_variadic_declaration_round_trip() {
    const std::string source = "extern fn printf(fmt: *char, ...): i32;\n";
    auto snapshot            = frontend::parse(source);
    formatter::FmtVisitor formatter(snapshot);
    formatter.format();
    CHECK_EQ(formatter.result(), source, "variadic tail round-trips through fmt");

    auto reparsed = frontend::parse(formatter.result());
    CHECK(reparsed.diagnostics().empty(), "formatted variadic declaration re-parses cleanly");
}

static void test_formatter_function_kinds_round_trip() {
    const std::string source   = "fn standard() {}\n"
                                 "raw fn unsafe_op() {}\n"
                                 "extern fn putchar(c: i32): i32;\n"
                                 "state structured(): i32 { return 0; }\n";
    const std::string expected = "fn standard() {}\n"
                                 "\n"
                                 "raw fn unsafe_op() {}\n"
                                 "\n"
                                 "extern fn putchar(c: i32): i32;\n"
                                 "\n"
                                 "state structured(): i32 {\n"
                                 "    return 0;\n"
                                 "}\n";
    auto snapshot              = frontend::parse(source);
    CHECK(snapshot.diagnostics().empty(), "function-kind source parses cleanly");
    formatter::FmtVisitor formatter(snapshot);
    formatter.format();
    CHECK_EQ(formatter.result(), expected, "all function kinds round-trip through fmt");

    auto reparsed = frontend::parse(formatter.result());
    CHECK(reparsed.diagnostics().empty(), "formatted function kinds re-parse cleanly");
}

static void test_formatter_state_machine_round_trip() {
    const std::string source = "state Start(v: i32): i32 {\n"
                               "    jump Done(v + 1);\n"
                               "}\n"
                               "\n"
                               "state Done(v: i32): i32 {\n"
                               "    return v;\n"
                               "}\n"
                               "\n"
                               "fn main(): i32 {\n"
                               "    let result: i32 = dock Start(41);\n"
                               "    return result;\n"
                               "}\n";
    auto snapshot            = frontend::parse(source);
    CHECK(snapshot.diagnostics().empty(), "state-machine source parses cleanly");
    formatter::FmtVisitor formatter(snapshot);
    formatter.format();
    CHECK_EQ(formatter.result(), source, "state, dock, and jump round-trip through fmt");

    auto reparsed = frontend::parse(formatter.result());
    CHECK(reparsed.diagnostics().empty(), "formatted state-machine source re-parses cleanly");
    formatter::FmtVisitor second(reparsed);
    second.format();
    CHECK_EQ(second.result(), formatter.result(), "state-machine formatting is idempotent");
}

static void test_formatter_compound_assign_round_trip() {
    // Compound assignment is stored desugared as `x = x op v`; fmt must recover the
    // source spelling rather than printing the expanded form.
    const std::string source = "fn main(): i32 {\n"
                               "    var x: i32 = 1;\n"
                               "    var y: i32 = 2;\n"
                               "    x += 2;\n"
                               "    x -= 1;\n"
                               "    x *= 3;\n"
                               "    x /= 2;\n"
                               "    x %= 5;\n"
                               "    x <<= 1;\n"
                               "    x >>= 1;\n"
                               "    x &= 3;\n"
                               "    x |= 4;\n"
                               "    x ^= 1;\n"
                               "    x = (y += 1);\n"
                               "    return x;\n"
                               "}\n";
    auto snapshot            = frontend::parse(source);
    CHECK(snapshot.diagnostics().empty(), "compound assignment source parses cleanly");
    formatter::FmtVisitor formatter(snapshot);
    formatter.format();
    CHECK_EQ(formatter.result(), source, "every compound assignment round-trips through fmt");

    auto reparsed = frontend::parse(formatter.result());
    formatter::FmtVisitor second(reparsed);
    second.format();
    CHECK_EQ(second.result(), formatter.result(), "compound assignment formatting is idempotent");
}

static void test_formatter_bitwise_operator_round_trip() {
    // Parenthesization here is driven by the parser and formatter agreeing on the new
    // precedences: `&.` binds tighter than `^.`, which binds tighter than `|.`.
    const std::string source = "fn main(): i32 {\n"
                               "    var a: i32 = 6;\n"
                               "    var b: i32 = 3;\n"
                               "    var c: i32 = a |. b ^. a &. b;\n"
                               "    var d: i32 = a &. (b |. a);\n"
                               "    var e: i32 = ~a + 1;\n"
                               "    var f: bool = a &. b == 0;\n"
                               "    return c;\n"
                               "}\n";
    auto snapshot            = frontend::parse(source);
    CHECK(snapshot.diagnostics().empty(), "bitwise operator source parses cleanly");
    formatter::FmtVisitor formatter(snapshot);
    formatter.format();
    CHECK_EQ(formatter.result(), source, "bitwise operators and '~' round-trip through fmt");
}

static void test_formatter_logical_operator_round_trip() {
    const std::string source = "fn main(): bool {\n"
                               "    var a: bool = true;\n"
                               "    var b: bool = false;\n"
                               "    var c: bool = false;\n"
                               "    if (a and b or c) { return true; }\n"
                               "    if (not (a and b)) { return true; }\n"
                               "    return false;\n"
                               "}\n";
    auto snapshot            = frontend::parse(source);
    CHECK(snapshot.diagnostics().empty(), "logical operator source parses cleanly");
    formatter::FmtVisitor formatter(snapshot);
    formatter.format();
    CHECK(!formatter.result().empty(), "logical boolean operators are retained by fmt");
    CHECK(formatter.result().find("a and b or c") != std::string::npos,
          "logical boolean operators round-trip through fmt");
    CHECK(formatter.result().find("not (a and b)") != std::string::npos,
          "unary not round-trips through fmt");
}

static void test_formatter_condition_keywords_round_trip() {
    const std::string source = "fn main(): bool {\n"
                               "    var x: ?i32 = 7;\n"
                               "    if (optional x) { return true; }\n"
                               "    if (not x is null) { return true; }\n"
                               "    return false;\n"
                               "}\n";
    auto snapshot            = frontend::parse(source);
    CHECK(snapshot.diagnostics().empty(), "condition keyword source parses cleanly");
    formatter::FmtVisitor formatter(snapshot);
    formatter.format();
    CHECK(formatter.result().find("optional x") != std::string::npos,
          "condition keyword sugar round-trips through fmt");
}

static void test_formatter_else_condition_spelling_round_trip() {
    const std::string source = "fn pick(a: bool, b: bool): i32 {\n"
                               "    if (a) {\n"
                               "        return 1;\n"
                               "    } else (b) {\n"
                               "        return 2;\n"
                               "    } else if (not a) {\n"
                               "        return 3;\n"
                               "    } else {\n"
                               "        return 4;\n"
                               "    }\n"
                               "}\n";
    auto snapshot            = frontend::parse(source);
    formatter::FmtVisitor formatter(snapshot);
    formatter.format();

    const std::string &output = formatter.result();
    CHECK(output.find("} else (b) {") != std::string::npos,
          "else condition spelling survives formatting");
    CHECK(output.find("} else if (not a) {") != std::string::npos,
          "legacy else if spelling survives formatting");

    auto reparsed             = frontend::parse(output);
    std::size_t warning_count = 0;
    for (const auto &diagnostic : reparsed.diagnostics()) {
        if (diagnostic.isWarning && diagnostic.code == diagnostics::err::DeprecatedSyntax)
            ++warning_count;
    }
    std::size_t source_warning_count = 0;
    for (const auto &diagnostic : snapshot.diagnostics()) {
        if (diagnostic.isWarning && diagnostic.code == diagnostics::err::DeprecatedSyntax)
            ++source_warning_count;
    }
    CHECK_EQ(warning_count, source_warning_count,
             "formatted output preserves only the original deprecation warning");
}

static void test_formatter_raw_opaque_round_trip() {
    const std::string source = "fn thru(p: raw opaque): raw opaque {\n"
                               "    return p;\n"
                               "}\n";
    auto snapshot            = frontend::parse(source);
    formatter::FmtVisitor formatter(snapshot);
    formatter.format();
    CHECK_EQ(formatter.result(), source, "'raw opaque' round-trips through fmt");

    auto reparsed = frontend::parse(formatter.result());
    CHECK(reparsed.diagnostics().empty(), "formatted 'raw opaque' re-parses cleanly");
}

static void test_formatter_bare_opaque_round_trip() {
    const std::string source = "fn erased(value: i32): opaque {\n"
                               "    return value as opaque;\n"
                               "}\n";
    auto snapshot            = frontend::parse(source);
    CHECK(snapshot.diagnostics().empty(), "bare 'opaque' source parses cleanly");

    formatter::FmtVisitor formatter(snapshot);
    formatter.format();
    CHECK_EQ(formatter.result(), source, "'opaque' round-trips through fmt");

    auto reparsed = frontend::parse(formatter.result());
    CHECK(reparsed.diagnostics().empty(), "formatted bare 'opaque' re-parses cleanly");
}

static void test_formatter_defer_round_trip() {
    const std::string source = "fn main(){\n"
                               "    defer release();\n"
                               "    defer { first(); second(); }\n"
                               "    return 0;\n"
                               "}\n";
    auto snapshot            = frontend::parse(source);
    formatter::FmtVisitor formatter(snapshot);
    formatter.format();

    const std::string expected = "fn main() {\n"
                                 "    defer release();\n"
                                 "    defer {\n"
                                 "        first();\n"
                                 "        second();\n"
                                 "    }\n"
                                 "    return 0;\n"
                                 "}\n";
    CHECK_EQ(formatter.result(), expected, "formats defer expr and defer block");

    auto reparsed = frontend::parse(formatter.result());
    CHECK(reparsed.diagnostics().empty(), "formatted defer source re-parses cleanly");
}

static void test_formatter() {
    test_formatter_normalizes_supported_snapshot_nodes();
    test_formatter_normalizes_extern_and_unary();
    test_formatter_normalizes_pointer_types();
    test_formatter_nested_if_else();
    test_formatter_normalizes_simple_import();
    test_formatter_break_continue();
    test_formatter_empty_file_produces_newline();
    test_formatter_multiple_top_level_decls_with_comments();
    test_formatter_parse_error_produces_empty();
    test_formatter_preserves_unsupported_subtrees();
    test_formatter_preserves_comments();
    test_formatter_index_and_optional_round_trip();
    test_formatter_memory_qualifier_round_trip();
    test_formatter_variadic_declaration_round_trip();
    test_formatter_function_kinds_round_trip();
    test_formatter_state_machine_round_trip();
    test_formatter_compound_assign_round_trip();
    test_formatter_bitwise_operator_round_trip();
    test_formatter_logical_operator_round_trip();
    test_formatter_condition_keywords_round_trip();
    test_formatter_else_condition_spelling_round_trip();
    test_formatter_raw_opaque_round_trip();
    test_formatter_bare_opaque_round_trip();
    test_compilation_session_fmt_uses_frontend_snapshot();
    test_formatter_defer_round_trip();
}

TEST_MAIN(formatter)
