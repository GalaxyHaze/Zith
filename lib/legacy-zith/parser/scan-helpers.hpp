#pragma once

#include "legacy-zith/ast/ast-builder.hpp"
#include "legacy-zith/ast/ast-nodes.hpp"
#include "diagnostics/diagnostic-engine.hpp"
#include "diagnostics/error-codes.hpp"
#include "legacy-zith/lexer/token.hpp"
#include "memory/arena.hpp"
#include "memory/dyn-array.hpp"
#include "memory/span.hpp"
#include "symbols/symbol-table.hpp"
#include <string>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zith::parser {

struct Parser;

// Skip Comments and Docs tokens — used throughout all parser passes
inline void skipComments(lexer::TokenStream &tok) {
    while (!tok.is_empty()) {
        auto &t = tok.peek();
        if (t.is_not(lexer::TokenKind::Comments) && t.is_not(lexer::TokenKind::Docs))
            break;
        tok.advance();
    }
}

// Parse delimited list: parser() items separated by commas, stops at `close`
template <typename Fn>
auto parseDelimited(lexer::TokenStream &tok, memory::Arena &arena, char close, Fn parser)
    -> memory::DynArray<decltype(parser())> {
    using T     = decltype(parser());
    auto result = memory::DynArray<T>{arena};
    skipComments(tok);
    if (tok.peek().punc != close) {
        result.push(parser());
        while (!tok.is_empty()) {
            skipComments(tok);
            if (tok.peek().punc == ',') {
                tok.advance();
                skipComments(tok);
                if (tok.peek().punc == close)
                    break;
                result.push(parser());
            } else {
                break;
            }
        }
    }
    return result;
}

namespace scan_detail {

inline memory::Span spanFromOffset(uint32_t start, uint32_t end) {
    return {0, start, end};
}

inline uint32_t skipBody(lexer::TokenStream &tok) {
    if (tok.is_empty())
        return tok.offset;
    uint32_t depth = 1;
    tok.advance();
    while (!tok.is_empty() && depth > 0) {
        if (tok.peek().kind == lexer::TokenKind::End)
            break;
        if (tok.peek().punc == '{')
            depth++;
        else if (tok.peek().punc == '}')
            depth--;
        tok.advance();
    }
    return tok.offset;
}

inline const char *symKindName(symbols::SymKind k) {
    switch (k) {
    case symbols::SymKind::Fn:
        return "a fn";
    case symbols::SymKind::Struct:
        return "a struct";
    case symbols::SymKind::Trait:
        return "a trait";
    case symbols::SymKind::Interface:
        return "a interface";
    case symbols::SymKind::Enum:
        return "an enum";
    case symbols::SymKind::Alias:
        return "an alias";
    case symbols::SymKind::Variable:
        return "a variable";
    case symbols::SymKind::Module:
        return "a module";
    case symbols::SymKind::Component:
        return "a component";
    case symbols::SymKind::Union:
        return "a union";
    case symbols::SymKind::Asset:
        return "an asset";
    case symbols::SymKind::Word:
        return "a word";
    case symbols::SymKind::Context:
        return "a context";
    }
    return "unknown";
}

// skip past a balanced pair: ( ... ), { ... }, [ ... ]
// cursor must be ON the opening delimiter; it is consumed first.
inline void skipBalanced(lexer::TokenStream &tok, char open, char close) {
    int depth = 1;
    tok.advance();
    while (!tok.is_empty()) {
        if (tok.peek().is_eof())
            break;
        if (tok.peek().punc == open)
            depth++;
        else if (tok.peek().punc == close)
            depth--;
        tok.advance();
        if (depth == 0)
            break;
    }
}

// skip past a type expression during scan — advances past tokens until a
// terminator (comma, brace, bracket, semicolon, equals, paren) is found.
inline void skipTypeExpr(lexer::TokenStream &tok) {
    while (!tok.is_empty()) {
        auto &t = tok.peek();
        if (t.is_eof())
            break;
        if (t.punc == ',' || t.punc == '}' || t.punc == ']' || t.punc == ';' || t.punc == '=' ||
            t.punc == ')' || t.punc == '|' || t.punc == '{' || t.punc == '>')
            break;
        if (t.punc == '(') {
            skipBalanced(tok, '(', ')');
            continue;
        }
        if (t.punc == '<') {
            skipBalanced(tok, '<', '>');
            continue;
        }
        if (t.punc == '|') {
            skipBalanced(tok, '|', '|');
            continue;
        }
        if (t.punc == '[') {
            skipBalanced(tok, '[', ']');
            continue;
        }
        tok.advance();
    }
}

// skip past an expression during scan until a terminator
inline void skipExpr(lexer::TokenStream &tok) {
    while (!tok.is_empty()) {
        auto &t = tok.peek();
        if (t.is_eof())
            break;
        if (t.punc == ',' || t.punc == '}' || t.punc == ';' || t.punc == ')')
            break;
        if (t.punc == '(') {
            skipBalanced(tok, '(', ')');
            continue;
        }
        if (t.punc == '{') {
            skipBalanced(tok, '{', '}');
            continue;
        }
        if (t.punc == '[') {
            skipBalanced(tok, '[', ']');
            continue;
        }
        tok.advance();
    }
}

// Try to consume optional fn-kind modifier and position at the `fn` keyword.
// Modifiers: async, flow (both TokenKind::Fn + lexeme), raw (TokenKind::Raw), const
// (TokenKind::Variable + lexeme). Returns true if at `fn` ready for caller to advance past it.
// Returns false if no fn/pattern found (stream unchanged).
inline bool consumeFnModifiers(lexer::TokenStream &tok) {
    auto lex = tok.lexeme();

    // plain fn keyword
    if (lex == "fn")
        return true;

    // async fn or flow fn
    if ((lex == "async" || lex == "flow") && tok.peek(1).is(lexer::TokenKind::Fn)) {
        tok.advance();
        return true;
    }

    // raw fn
    if (tok.peek().is(lexer::TokenKind::Raw) && tok.peek(1).is(lexer::TokenKind::Fn)) {
        tok.advance();
        return true;
    }

    // const fn
    if (lex == "const" && tok.peek(1).is(lexer::TokenKind::Fn)) {
        tok.advance();
        return true;
    }

    // extern fn
    if (lex == "extern" && tok.peek(1).is(lexer::TokenKind::Fn)) {
        tok.advance();
        return true;
    }

    return false;
}

inline void skipMethodMember(lexer::TokenStream &tok) {
    tok.advance(); // consume 'fn'
    if (tok.peek().is(lexer::TokenKind::Identifier))
        tok.advance();
    while (!tok.is_empty() && !tok.peek().is_eof()) {
        if (tok.peek().punc == ';') {
            tok.advance();
            break;
        }
        if (tok.peek().punc == '{') {
            skipBalanced(tok, '{', '}');
            break;
        }
        if (tok.peek().punc == '}')
            break;
        tok.advance();
    }
}

struct FieldItemOptions {
    bool support_destructure = false;
    bool skip_qualifiers     = false;
    bool parse_bind          = false;
};

template <typename ParseTypeFn, typename ParseDefaultFn, typename MissingDefaultFn,
          typename OnFieldFn>
inline bool consumeFieldItem(lexer::TokenStream &tok, const FieldItemOptions &options,
                             ParseTypeFn &&parse_type, ParseDefaultFn &&parse_default,
                             MissingDefaultFn &&missing_default, OnFieldFn &&on_field) {
    auto bind = ast::FieldBind::Auto;

    if (options.skip_qualifiers) {
        while (tok.peek().is(lexer::TokenKind::Variable) ||
               tok.peek().is(lexer::TokenKind::Mutable) ||
               tok.peek().is(lexer::TokenKind::Ownership)) {
            if (options.parse_bind && tok.peek().is(lexer::TokenKind::Variable)) {
                auto lex = tok.lexeme();
                if (lex == "auto")
                    bind = ast::FieldBind::Auto;
                else if (lex == "const")
                    bind = ast::FieldBind::Const;
                else if (lex == "let")
                    bind = ast::FieldBind::Let;
                else if (lex == "var")
                    bind = ast::FieldBind::Var;
                else if (lex == "global")
                    bind = ast::FieldBind::Global;
                else if (lex == "comptime")
                    bind = ast::FieldBind::Comptime;
            }
            tok.advance();
        }
    }

    if (options.support_destructure && tok.peek().punc == '[') {
        tok.advance();
        std::vector<std::pair<std::string_view, memory::Span>> names;
        while (!tok.is_empty() && tok.peek().punc != ']') {
            if (tok.peek().is(lexer::TokenKind::Identifier)) {
                names.push_back({tok.lexeme(), tok.peek().span});
                tok.advance();
            } else {
                tok.advance();
            }
            if (tok.peek().punc == ',')
                tok.advance();
        }
        if (tok.peek().punc == ']')
            tok.advance();

        auto type_expr     = parse_type();
        auto default_value = missing_default();
        if (tok.peek().punc == '=') {
            tok.advance();
            default_value = parse_default();
        }

        for (auto &[name, name_span] : names)
            on_field(name, name_span, type_expr, default_value, bind);

        if (tok.peek().punc == ',')
            tok.advance();
        return true;
    }

    if (tok.peek().is(lexer::TokenKind::Identifier)) {
        auto name      = tok.lexeme();
        auto name_span = tok.peek().span;
        tok.advance();
        auto type_expr     = parse_type();
        auto default_value = missing_default();
        if (tok.peek().punc == '=') {
            tok.advance();
            default_value = parse_default();
        }

        on_field(name, name_span, type_expr, default_value, bind);

        if (tok.peek().punc == ',')
            tok.advance();
        return true;
    }

    if (tok.peek().is(lexer::TokenKind::Annotation)) {
        tok.advance();
        return true;
    }

    return false;
}

inline bool reportIfDuplicate(symbols::SymbolTable &syms, diagnostics::DiagnosticEngine &diag,
                              std::string_view name, memory::Span span,
                              symbols::SymId skip_id = symbols::kInvalidSym) {
    auto existing = syms.lookup(name);
    if (existing == symbols::kInvalidSym || existing == skip_id)
        return false;
    auto &prev      = syms.get(existing);
    std::string msg = "duplicate symbol '";
    msg += name;
    msg += "' — previous declaration is ";
    msg += symKindName(prev.kind);
    diag.report(diagnostics::Severity::Error, diagnostics::err::DuplicateDecl, std::move(msg),
                span);
    return true;
}

// Scan a top-level global/const declaration.
// Expects `tok` positioned AFTER the `global`/`const` keyword.
ast::DeclId scanGlobalOrConst(Parser &parser, symbols::SymbolTable &syms,
                              memory::DynArray<ast::DeclId> &program_decls, bool is_const,
                              memory::Span kw_span, symbols::SymbolVisibility vis,
                              int32_t mod_depth, memory::Span doc_span);

// Unified body scanner — handles common infrastructure for struct/enum/union/component.
// Manages brace_depth, skipComments, EOF, closing `}`, visibility, and methods.
// `on_item` is called for each member-specific token inside `{...}`.
// `must_have_body` controls whether methods require a `{...}` body.
template <typename MemberFn, typename MethodFn>
inline void scanBody(lexer::TokenStream &tok, MemberFn &&on_item, MethodFn &&on_method,
                     uint32_t &token_start, memory::Span &body_span, ast::AstBuilder &bld,
                     ast::ExprId &body_node) {
    tok.advance(); // consume '{'
    uint32_t brace_depth = 1;
    while (!tok.is_empty() && brace_depth > 0) {
        skipComments(tok);
        if (tok.peek().is_eof())
            break;
        if (tok.peek().punc == '}') {
            if (--brace_depth == 0) {
                tok.advance();
                break;
            }
            tok.advance();
            continue;
        }

        // visibility (common to all 4 body types)
        if (tok.peek().is(lexer::TokenKind::Visibility)) {
            auto vkw = tok.lexeme();
            tok.advance();
            if (vkw == "mod" && tok.peek().punc == '(')
                skipBalanced(tok, '(', ')');
            continue;
        }

        // method (common to all 4 body types)
        if (consumeFnModifiers(tok)) {
            on_method(tok);
            continue;
        }

        on_item(tok);
    }
    body_span = {body_span.file, body_span.start, tok.offset};
    body_node = bld.unbody(body_span, token_start, tok.offset);
}

} // namespace scan_detail
} // namespace zith::parser
