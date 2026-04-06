// impl/parser/parser_expr.cpp — Pratt parser for expressions
//
// Refactored to use centralized modules.
#include "parser.h"
#include "../memory/arena.hpp"
#include <cstring>

using kalidous::ArenaList;

// Forward declarations
extern const KalidousToken *parser_peek(const Parser *p);
extern const KalidousToken *parser_peek_ahead(const Parser *p, size_t offset);
extern const KalidousToken *parser_advance(Parser *p);
extern bool parser_check(const Parser *p, KalidousTokenType type);
extern bool parser_match(Parser *p, KalidousTokenType type);
extern const KalidousToken *parser_expect(Parser *p, KalidousTokenType type, const char *msg);
extern void parser_error(Parser *p, const KalidousSourceLoc loc, const char *msg);
extern KalidousLiteral parse_lit_number(const char*, size_t, KalidousTokenType);

// ============================================================================
// Types
// ============================================================================

KalidousNode *parser_parse_type(Parser *p) {
    const KalidousSourceLoc loc = parser_peek(p)->loc;
    // Ownership modifiers
    if (parser_match(p, KALIDOUS_TOKEN_UNIQUE) || parser_match(p, KALIDOUS_TOKEN_SHARED) ||
        parser_match(p, KALIDOUS_TOKEN_VIEW) || parser_match(p, KALIDOUS_TOKEN_LEND)) {
        // TODO: Create ownership node
    }
    if (parser_match(p, KALIDOUS_TOKEN_MULTIPLY)) {
        KalidousNode *inner = parser_parse_type(p);
        return inner; // TODO: Create pointer node
    }
    if (parser_match(p, KALIDOUS_TOKEN_LBRACKET)) {
        if (!parser_check(p, KALIDOUS_TOKEN_RBRACKET)) parser_parse_expression(p); 
        parser_expect(p, KALIDOUS_TOKEN_RBRACKET, "expected ']' in array type");
        KalidousNode *inner = parser_parse_type(p);
        return inner; // TODO: Create array node
    }
    if (parser_check(p, KALIDOUS_TOKEN_TYPE) || parser_check(p, KALIDOUS_TOKEN_IDENTIFIER)) {
        const KalidousToken *t = parser_advance(p);
        KalidousNode *base = kalidous_ast_make_identifier(p->arena, loc, t->lexeme.data, t->lexeme.len);
        if (parser_match(p, KALIDOUS_TOKEN_QUESTION)) {} // Optional
        else if (parser_match(p, KALIDOUS_TOKEN_BANG)) {} // Result
        return base;
    }
    parser_error(p, loc, "expected type");
    return kalidous_ast_make_error(p->arena, loc, "expected type");
}

// ============================================================================
// Expressions (Pratt Parser)
// ============================================================================

typedef struct { int8_t left, right; } BindingPower;

static BindingPower infix_bp(KalidousTokenType op) {
    switch (op) {
        case KALIDOUS_TOKEN_OR: return {1, 2};
        case KALIDOUS_TOKEN_AND: return {3, 4};
        case KALIDOUS_TOKEN_EQUAL: case KALIDOUS_TOKEN_NOT_EQUAL: return {5, 6};
        case KALIDOUS_TOKEN_LESS_THAN: case KALIDOUS_TOKEN_GREATER_THAN:
        case KALIDOUS_TOKEN_LESS_THAN_OR_EQUAL: case KALIDOUS_TOKEN_GREATER_THAN_OR_EQUAL: return {7, 8};
        case KALIDOUS_TOKEN_PLUS: case KALIDOUS_TOKEN_MINUS: return {9, 10};
        case KALIDOUS_TOKEN_MULTIPLY: case KALIDOUS_TOKEN_DIVIDE: case KALIDOUS_TOKEN_MOD: return {11, 12};
        case KALIDOUS_TOKEN_ARROW: return {13, 14};
        case KALIDOUS_TOKEN_DOT: return {15, 16};
        default: return {-1, -1};
    }
}

static KalidousNode *parse_expr_bp(Parser *p, int min_bp);

static KalidousNode *parse_nud(Parser *p) {
    const KalidousSourceLoc loc = parser_peek(p)->loc;
    const KalidousToken *t = parser_advance(p);
    switch (t->type) {
        case KALIDOUS_TOKEN_NUMBER: case KALIDOUS_TOKEN_FLOAT: case KALIDOUS_TOKEN_HEXADECIMAL:
        case KALIDOUS_TOKEN_BINARY: case KALIDOUS_TOKEN_OCTAL:
            return kalidous_ast_make_literal(p->arena, loc, parse_lit_number(t->lexeme.data, t->lexeme.len, t->type));
        case KALIDOUS_TOKEN_STRING:
            return kalidous_ast_make_literal(p->arena, loc, {KALIDOUS_LIT_STRING, {.string = {t->lexeme.data + 1, t->lexeme.len - 2}}});
        case KALIDOUS_TOKEN_IDENTIFIER: {
            KalidousNode *ident = kalidous_ast_make_identifier(p->arena, loc, t->lexeme.data, t->lexeme.len);
            if (!parser_match(p, KALIDOUS_TOKEN_LPAREN)) return ident;
            ArenaList<KalidousNode *> args_b; args_b.init(p->arena, 8);
            while (!parser_check(p, KALIDOUS_TOKEN_RPAREN) && !parser_is_at_end(p)) {
                args_b.push(p->arena, parser_parse_expression(p));
                if (!parser_match(p, KALIDOUS_TOKEN_COMMA)) break;
            }
            parser_expect(p, KALIDOUS_TOKEN_RPAREN, "expected ')'");
            size_t count = 0; KalidousNode **args = args_b.flatten(p->arena, &count);
            return kalidous_ast_make_call(p->arena, loc, ident, args, count);
        }
        case KALIDOUS_TOKEN_MINUS: case KALIDOUS_TOKEN_BANG:
            return kalidous_ast_make_unary_op(p->arena, loc, t->type, parse_expr_bp(p, 13), false);
        case KALIDOUS_TOKEN_LPAREN: {
            KalidousNode *expr = parser_parse_expression(p);
            parser_expect(p, KALIDOUS_TOKEN_RPAREN, "expected ')'");
            return expr;
        }
        case KALIDOUS_TOKEN_SPAWN:
            return kalidous_ast_make_spawn(p->arena, loc, parser_parse_expression(p), false);
        default: {
            char buf[128]; snprintf(buf, sizeof(buf), "unexpected token '%.*s'", (int)t->lexeme.len, t->lexeme.data);
            parser_error(p, loc, buf); return kalidous_ast_make_error(p->arena, loc, buf);
        }
    }
}

static KalidousNode *parse_expr_bp(Parser *p, int min_bp) {
    KalidousNode *left = parse_nud(p);
    while (true) {
        const KalidousTokenType op = parser_peek(p)->type;
        const BindingPower bp = infix_bp(op);
        if (bp.left < min_bp) break;
        const KalidousSourceLoc loc = parser_peek(p)->loc;
        parser_advance(p);
        if (op == KALIDOUS_TOKEN_DOT) {
            const KalidousToken *member = parser_expect(p, KALIDOUS_TOKEN_IDENTIFIER, "expected member name");
            KalidousNode *rhs = kalidous_ast_make_identifier(p->arena, member->loc, member->lexeme.data, member->lexeme.len);
            left = kalidous_ast_make_member(p->arena, loc, left, rhs);
            if (parser_match(p, KALIDOUS_TOKEN_LPAREN)) {
                ArenaList<KalidousNode *> args_b; args_b.init(p->arena, 8);
                while (!parser_check(p, KALIDOUS_TOKEN_RPAREN) && !parser_is_at_end(p)) {
                    args_b.push(p->arena, parser_parse_expression(p));
                    if (!parser_match(p, KALIDOUS_TOKEN_COMMA)) break;
                }
                parser_expect(p, KALIDOUS_TOKEN_RPAREN, "expected ')'");
                size_t ac = 0; KalidousNode **args = args_b.flatten(p->arena, &ac);
                left = kalidous_ast_make_call(p->arena, loc, left, args, ac);
            }
            continue;
        }
        if (op == KALIDOUS_TOKEN_ARROW) {
            left = kalidous_ast_make_arrow_call(p->arena, loc, left, parse_expr_bp(p, bp.right));
            continue;
        }
        left = kalidous_ast_make_binary_op(p->arena, loc, op, left, parse_expr_bp(p, bp.right));
    }
    while (parser_match(p, KALIDOUS_TOKEN_QUESTION))
        left = kalidous_ast_make_unary_op(p->arena, parser_peek(p)->loc, KALIDOUS_TOKEN_QUESTION, left, true);
    return left;
}

KalidousNode *parser_parse_expression(Parser *p) { return parse_expr_bp(p, 0); }