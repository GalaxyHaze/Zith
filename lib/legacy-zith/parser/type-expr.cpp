#include "parser.hpp"

#include "diagnostics/error-codes.hpp"
#include "legacy-zith/parser/operators.hpp"
#include "legacy-zith/parser/scan-helpers.hpp"
#include "support/int-literal.hpp"

#include <array>
#include <limits>
#include <cstdlib>
#include <string>
#include <string_view>

namespace zith::parser {

using lexer::TokenKind;
using namespace operators;

namespace {

using ast::kInvalidDecl;
using ast::kInvalidExpr;
using ast::kInvalidStmt;
using diagnostics::Severity;

struct BuiltinTypeEntry {
    std::string_view name;
    ast::BuiltinType type;
};

static constexpr BuiltinTypeEntry builtin_types[] = {
    {"i8", ast::BuiltinType::I8},           {"i16", ast::BuiltinType::I16},
    {"i32", ast::BuiltinType::I32},         {"i64", ast::BuiltinType::I64},
    {"i128", ast::BuiltinType::I128},       {"u8", ast::BuiltinType::U8},
    {"u16", ast::BuiltinType::U16},         {"u32", ast::BuiltinType::U32},
    {"u64", ast::BuiltinType::U64},         {"u128", ast::BuiltinType::U128},
    {"f32", ast::BuiltinType::F32},         {"f64", ast::BuiltinType::F64},
    {"bool", ast::BuiltinType::Bool},       {"char", ast::BuiltinType::Char},
    {"void", ast::BuiltinType::Void},       {"never", ast::BuiltinType::Never},
    {"noreturn", ast::BuiltinType::Never},  {"invalid", ast::BuiltinType::Invalid},
    {"unknown", ast::BuiltinType::Unknown}, {"opaque", ast::BuiltinType::Opaque},
};

bool matchBuiltinType(std::string_view name, ast::BuiltinType &out) {
    for (auto &entry : builtin_types) {
        if (entry.name == name) {
            out = entry.type;
            return true;
        }
    }
    return false;
}

} // anonymous namespace

ast::TypeExprId Parser::parsePrimaryType() {
    // ── raw opaque (void*) ───────────────────────────────────────────
    if (match(TokenKind::Raw)) {
        if (!match("opaque")) {
            errorExpected("'opaque' after 'raw'", diagnostics::err::ExpectedIdent);
            return bld->inferExpr();
        }
        auto opaque_expr = bld->builtinExpr(ast::BuiltinType::Opaque);
        return bld->addTypeExpr(ast::TypePtrExpr{opaque_expr, false, ast::OwnershipKw::Default});
    }

    // ── builtin types (i32, bool, void, …) ──────────────────────────
    if (check(TokenKind::Type)) {
        ast::BuiltinType bt;
        if (matchBuiltinType(lexeme(), bt)) {
            advance();
            return bld->builtinExpr(bt);
        }
        auto segments = memory::DynArray<std::string_view>{bld->arena()};
        segments.push(lexeme());
        advance();
        return bld->pathExpr(std::move(segments));
    }

    // ── pointer / ownership-qualified types ─────────────────────────
    {
        bool has_mut               = false;
        ast::OwnershipKw ownership = ast::OwnershipKw::Default;

        if (consume(TokenKind::Mutable))
            has_mut = true;

        if (check(TokenKind::Ownership)) {
            auto kw = lexeme();
            if (kw == "unique")
                ownership = ast::OwnershipKw::Unique;
            if (kw == "share")
                ownership = ast::OwnershipKw::Share;
            if (kw == "lend")
                ownership = ast::OwnershipKw::Lend;
            if (kw == "view")
                ownership = ast::OwnershipKw::View;
            if (kw == "belong")
                ownership = ast::OwnershipKw::Belong;
            advance();

            if (!check('*')) {
                auto inner = parsePrimaryType();
                return bld->addTypeExpr(ast::TypePtrExpr{inner, has_mut, ownership});
            }
        }

        if (consume('*')) {
            auto inner = parsePrimaryType();
            return bld->addTypeExpr(ast::TypePtrExpr{inner, has_mut, ownership});
        }

        if (has_mut) {
            auto inner = parsePrimaryType();
            return bld->addTypeExpr(ast::TypeMut{inner});
        }
        if (ownership != ast::OwnershipKw::Default) {
            errorExpected("pointer type after ownership qualifier");
            return bld->inferExpr();
        }
    }

    // ── dyn: dynamic dispatch type ───────────────────────────────
    if (check(TokenKind::Trait) && lexeme() == "dyn") {
        advance();
        return bld->dynExpr(parsePrimaryType());
    }

    // ── union type hint ────────────────────────────────────────────
    if (check(TokenKind::Struct) && lexeme() == "union") {
        advance();
        return bld->unionExpr();
    }

    // ── optional type: ?T ───────────────────────────────────────────
    if (consume('?')) {
        auto inner = parsePrimaryType();
        return bld->addTypeExpr(ast::TypeOptional{inner});
    }

    // ── pack type: |T, U| or |name: T, U| ─────────────────────────
    if (peek().punc == '|') {
        advance();
        memory::DynArray<ast::TypePackMember> members{bld->arena()};
        while (!peek().is_eof() && !check('|')) {
            if (check(TokenKind::Identifier)) {
                if (peek(1).punc == ':') {
                    auto name = lexeme();
                    advance();
                    advance();
                    members.push({name, parsePrimaryType()});
                } else {
                    members.push({std::string_view{}, parsePrimaryType()});
                }
            } else {
                members.push({std::string_view{}, parsePrimaryType()});
            }
            if (check(','))
                advance();
        }
        if (check('|'))
            advance();
        return bld->addTypeExpr(ast::TypePack{std::move(members)});
    }

    // ── parenthesized: (T) or fn(...) ─────────────────────────────
    if (consume('(')) {
        auto inner = parseTypeExpr();
        if (check(')')) {
            advance();
        } else {
            // didn't find ')' — skip to it for recovery
            while (!peek().is_eof() && !check(')'))
                advance();
            if (check(')'))
                advance();
        }
        return inner;
    }

    // ── slice / array: []T or [N]T ────────────────────────────────
    if (consume('[')) {
        if (check(']')) {
            advance();
            return bld->addTypeExpr(ast::TypeSlice{parsePrimaryType()});
        }
        uint32_t count_val = 0;
        if (peek().is(lexer::TokenKind::LitVal)) {
            auto lex = lexeme();
            std::int64_t parsed = 0;
            if (support::parseIntegerLiteral(lex, parsed) == support::IntLiteralStatus::Ok &&
                parsed >= 0 && parsed <= std::numeric_limits<uint32_t>::max()) {
                count_val = static_cast<uint32_t>(parsed);
            }
        }
        auto count_expr = bld->inferExpr();
        while (!peek().is_eof() && !check(']'))
            advance();
        if (check(']'))
            advance();
        return bld->addTypeExpr(ast::TypeArray{parsePrimaryType(), count_expr, count_val});
    }

    // ── fn type: fn(T): U ─────────────────────────────────────────
    if (consume(TokenKind::Fn)) {
        memory::DynArray<ast::TypeExprId> params{bld->arena()};
        if (consume('(')) {
            while (!peek().is_eof() && !check(')')) {
                params.push(parseTypeExpr());
                if (check(','))
                    advance();
            }
            if (check(')'))
                advance();
        }
        auto ret = bld->inferExpr();
        if (consume(':'))
            ret = parseTypeExpr();
        return bld->addTypeExpr(ast::TypeFnExpr{std::move(params), ret});
    }

    // ── infer type: _ ──────────────────────────────────────────────
    if (consume('_'))
        return bld->inferExpr();

    // ── partial specialization hint: <T1, T2> (no base) ─────────
    if (check('<')) {
        advance();
        auto args = parseDelimited(*tok, bld->arena(), '>', [this] { return parseTypeExpr(); });
        if (check('>'))
            advance();
        return bld->typeSpecialization(bld->inferExpr(), std::move(args));
    }

    // ── identifier / path: Vec, std.vec.Vec ────────────────────────
    if (check(TokenKind::Identifier)) {
        auto id_span = peek().span;
        auto id      = lexeme();
        ast::BuiltinType bt;
        if (peek(1).punc != '.' && matchBuiltinType(id, bt)) {
            advance();
            return bld->builtinExpr(bt);
        }
        auto seg = id;
        advance();
        memory::DynArray<std::string_view> segments{bld->arena()};
        segments.push(seg);
        auto path_span = id_span;
        while (check('.')) {
            advance();
            if (check(TokenKind::Identifier)) {
                path_span.end = peek().span.end;
                segments.push(lexeme());
                advance();
            } else {
                break;
            }
        }
        auto base = bld->pathExpr(std::move(segments), path_span);
        // Generic specialization: Base<Args>
        if (check('<')) {
            advance();
            auto args = parseDelimited(*tok, bld->arena(), '>', [this] { return parseTypeExpr(); });
            if (check('>'))
                advance();
            return bld->typeSpecialization(base, std::move(args));
        }
        return base;
    }

    errorExpected("type expression");
    return bld->inferExpr();
}

ast::TypeExprId Parser::parseOrExpr() {
    auto left = parsePrimaryType();

    while (true) {
        // failable: Type!Error
        if (check('!')) {
            advance();
            auto right = parsePrimaryType();
            left       = bld->addTypeExpr(ast::TypeFailable{left, right});
            continue;
        }

        // sum: Type or Error
        if (match("or")) {
            auto right = parsePrimaryType();

            auto &node = bld->getTypeExpr(left);
            if (auto *sum = std::get_if<ast::TypeSum>(&node)) {
                sum->members.push(right);
            } else {
                memory::DynArray<ast::TypeExprId> members{bld->arena()};
                members.push(left);
                members.push(right);
                left = bld->addTypeExpr(ast::TypeSum{std::move(members)});
            }
            continue;
        }

        break;
    }

    return left;
}

ast::TypeExprId Parser::parseTypeExpr() {
    // typeExpr → orExpr ('+' orExpr)*   (AND of constraints)
    // For now: '+' not yet implemented (requires trait system)
    // Just parse a single orExpr
    return parseOrExpr();
}

} // namespace zith::parser
