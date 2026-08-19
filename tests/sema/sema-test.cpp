#include "session/session.hpp"

#include "frontend/parser/types.hpp"
#include "frontend/parser/parse.hpp"
#include "frontend/lexer/lexer.hpp"
#include "frontend/parser/parser.hpp"
#include "sema/sema.hpp"

#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Result {
    bool ok = false;
    std::vector<std::uint32_t> codes;
    std::vector<std::string> messages;

    [[nodiscard]] bool hasCode(std::uint32_t value) const {
        for (const std::uint32_t code : codes) {
            if (code == value)
                return true;
        }
        return false;
    }

    [[nodiscard]] bool hasMessage(std::string_view needle) const {
        for (const std::string &message : messages) {
            if (message.find(needle) != std::string::npos)
                return true;
        }
        return false;
    }

    [[nodiscard]] std::size_t errorCount() const { return codes.size(); }
};

Result run(std::string_view source) {
    common::memory::Arena arena;
    common::memory::StringInterner interner(arena);
    generated_lexer::TokenStream tokens =
        generated_lexer::tokenize(source);
    generated_parser::Parser<sample::ParseOutput> parser(arena);
    sample::ParseOutput output =
        hooks::parser::parseSource(parser, tokens, source);

    common::memory::DynArray<common::diagnostic::Diagnostic> diagnostics(arena);
    toolkit::sema::TypeCheckedInfo checked(arena, interner);
    const bool ok = toolkit::sema::typeCheckProgram(
        *output.ast.root, 1, arena, interner, diagnostics, checked);

    Result out;
    out.ok = ok;
    for (const auto &diag : diagnostics) {
        out.codes.push_back(diag.code);
        out.messages.push_back(diag.message);
    }
    return out;
}

bool check(bool condition, std::string_view message) {
    if (!condition)
        std::fprintf(stderr, "FAIL: %.*s\n",
                     static_cast<int>(message.size()), message.data());
    return condition;
}

bool expectsOk(std::string_view source, std::string_view message) {
    const Result result = run(source);
    if (!result.ok) {
        std::fprintf(stderr, "unexpected sema failure for %s\n",
                     std::string(message).c_str());
        for (const auto &diag : result.messages)
            std::fprintf(stderr, "  diag: %s\n", diag.c_str());
    }
    return check(result.ok, message);
}

bool expectsError(std::string_view source, std::uint32_t code,
                  std::string_view message) {
    const Result result = run(source);
    if (result.ok) {
        std::fprintf(stderr, "expected sema failure for %s\n",
                     std::string(message).c_str());
    }
    return check(!result.ok, message) && check(result.hasCode(code), message);
}

bool expectsParsedOnly(std::string_view source, std::string_view message) {
    common::memory::Arena arena;
    common::memory::StringInterner interner(arena);
    generated_lexer::TokenStream tokens =
        generated_lexer::tokenize(source);
    generated_parser::Parser<sample::ParseOutput> parser(arena);
    sample::ParseOutput output =
        hooks::parser::parseSource(parser, tokens, source);
    return check(output.diagnostics.empty(), message);
}

constexpr std::uint32_t kUndefinedIdent = 2001;
constexpr std::uint32_t kDuplicateDecl = 2002;
constexpr std::uint32_t kNoMember = 2006;
constexpr std::uint32_t kNoMatchingFn = 2007;
constexpr std::uint32_t kAmbiguousCall = 2008;
constexpr std::uint32_t kUnsupportedSyntax = 2010;
constexpr std::uint32_t kTypeMismatch = 3001;
constexpr std::uint32_t kCannotInfer = 3002;
constexpr std::uint32_t kInvalidCast = 3003;
constexpr std::uint32_t kWriteThroughView = 4004;

} // namespace

int main() {
    bool ok = true;

    ok &= expectsOk("fn main() { }\n", "empty function type-checks");
    ok &= expectsOk("fn main(): i32 { return 42; }\n",
                    "literal return type-checks");
    ok &= expectsOk("fn add(a: i32, b: i32): i32 { return a + b; }\n"
                    "fn main(): i32 { return add(1, 2); }\n",
                    "regular call with correct types type-checks");

    ok &= expectsError("fn main(): i32 { var x: i32 = true; return x; }\n",
                       kTypeMismatch, "type mismatch on binding");
    ok &= expectsError("fn main(): bool { return 42; }\n",
                       kTypeMismatch, "return type mismatch");
    ok &= expectsError("fn add(a: i32, b: i32): i32 { return a + b; }\n"
                       "fn main() { var x: i32 = add(1); }\n",
                       kNoMatchingFn, "call arity reports NoMatchingFn");
    ok &= expectsError("fn main() { var x: i32 = y; }\n",
                       kUndefinedIdent, "undefined identifier reports UndefinedIdent");
    ok &= expectsError("fn main() { 1 + true; }\n",
                       kTypeMismatch, "incompatible arithmetic reports TypeMismatch");
    ok &= expectsError("fn main() { var x: i32 = 0; x[0]; }\n",
                       kTypeMismatch, "non-indexable type reports TypeMismatch");
    ok &= expectsError("fn main() { var x: i32 = 0; x.value; }\n",
                       kTypeMismatch, "field access on non-struct reports TypeMismatch");
    ok &= expectsError("fn main() { var x: i32 = 0; x = true; }\n",
                       kTypeMismatch, "assignment type mismatch");
    ok &= expectsError("fn main() { var x = missing; let y = x; }\n",
                       kUndefinedIdent, "missing local reports UndefinedIdent");
    ok &= expectsError("fn main() { }\n"
                       "fn main() { }\n",
                       kDuplicateDecl, "duplicate function reports DuplicateDecl");

    ok &= expectsOk("alias MyInt = i32;\n"
                    "fn main(): MyInt { var x: MyInt = 100; return x; }\n",
                    "alias resolves to primitive");
    ok &= expectsError("alias MyInt = i32;\n"
                       "fn main() { var x: MyInt = true; }\n",
                       kTypeMismatch, "alias assignment mismatch");

    ok &= expectsOk("struct Point { x: i32, y: i32 }\n"
                    "fn main(): i32 {\n"
                    "    var p: Point = Point { x: 1, y: 2 };\n"
                    "    return p.x + p.y;\n"
                    "}\n",
                    "struct literal and field access type-check");
    ok &= expectsError("struct Point { x: i32, y: i32 }\n"
                       "fn main() { var p: Point = Point { x: 1, missing: 2 }; }\n",
                       kNoMember, "unknown struct literal field reports NoMember");
    ok &= expectsError("struct Point { x: i32, y: i32 }\n"
                       "fn main() { var p: Point = Point { x: 1, x: 2 }; }\n",
                       kTypeMismatch, "duplicate struct literal field is rejected");

    ok &= expectsOk("enum Color { red: i32, green: i32, blue: i32 }\n"
                    "fn main(): Color { return Color.red; }\n",
                    "enum variant field syntax type-checks");

    ok &= expectsOk("fn main() { var xs = [1, 2, 3]; let n = xs[1]; }\n",
                    "array literal and index type-check");
    ok &= expectsError("fn main() { var xs = [1, \"a\"]; }\n",
                       kTypeMismatch, "array element mismatch is rejected");
    ok &= expectsError("fn main() { var xs = [1, 2]; xs[\"a\"]; }\n",
                       kTypeMismatch, "non-integer index is rejected");

    ok &= expectsOk("fn main(): bool { var x: bool = true; return not x; }\n",
                    "boolean unary operator type-checks");

    ok &= expectsError("fn main() { var x: *i32 = null; }\n",
                       kTypeMismatch, "null requires optional pointer");
    ok &= expectsOk("fn main() { var x: ?*i32 = null; }\n",
                    "nullable pointer type-checks");
    ok &= expectsOk("fn main(): bool { var x: ?i32 = null; return x is null; }\n",
                    "is null on optional type-checks");
    ok &= expectsError("fn main(): bool { var x: i32 = 0; return x is null; }\n",
                       kTypeMismatch, "is null requires optional operand");

    ok &= expectsOk("fn main(): f64 { return 1 as f64; }\n",
                    "numeric cast type-checks");
    ok &= expectsError("fn main(): bool { return 1 as bool; }\n",
                       kInvalidCast, "non-numeric scalar cast is rejected");
    ok &= expectsError("fn main() { let p: *i32 = 0 as raw opaque; let q: *char = p as *char; }\n",
                       kInvalidCast, "cast between incompatible concrete pointers is rejected");

    ok &= expectsOk("state Ready(x: i32) {}\n"
                    "fn main() { }\n",
                    "state declaration type-checks");
    ok &= expectsError("state Ready(x: i32) {}\n"
                       "fn main() { enter Ready(1); }\n",
                       kUnsupportedSyntax, "state/enter flow execution is deferred as unsupported");
    ok &= expectsOk("fn main() { let x = 1; }\n",
                    "local binding with no annotation type-checks");
    ok &= expectsOk(
        "fn f(p: view *i32): i32 { return 0; }\n"
        "fn main(): i32 { let x: mut i32 = 1; return f(&x); }\n",
        "memory qualifiers parse and annotate parameters");
    ok &= expectsError(
        "fn read(p: view *i32): i32 { return 0; }\n"
        "fn main(): i32 {\n"
        "    let x: mut i32 = 1;\n"
        "    let v: view *i32 = &x;\n"
        "    *v = 2;\n"
        "    return read(v);\n"
        "}\n",
        kWriteThroughView, "assignment through a 'view' binding reports WriteThroughView");
    ok &= expectsError("fn main() { let x; let y = x; }\n",
                       kCannotInfer, "reading unannotated binding reports CannotInfer");

    ok &= expectsOk("fn f(x: i32): i32 { return 1; }\n"
                    "fn g(x: bool): bool { return false; }\n"
                    "fn main(): i32 { return f(1); }\n",
                    "function set with one matching function type-checks");
    ok &= expectsError("fn f(x: i32): i32 { return 1; }\n"
                       "fn h(x: i32): i32 { return 1; }\n"
                       "fn h(x: u32): u32 { return 2; }\n"
                       "fn main(): u32 { return h(1); }\n",
                       kAmbiguousCall, "literal selects two integer overloads and reports ambiguous");

    ok &= expectsParsedOnly("type Ptr = *i32;\n"
                            "type Opt = ?i32;\n"
                            "type Slice = []i32;\n"
                            "type Arr = [4]i32;\n"
                            "type Opaque = raw opaque;\n"
                            "type Mut = mut i32;\n",
                            "type syntax parses");

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
