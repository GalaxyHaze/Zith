// impl/parser/parser_expr.cpp — Pratt parser for expressions
//
// Refactored to use centralized modules.
#include "../memory/arena.hpp"
#include "parser.h"
#include <cstring>

using zith::ArenaList;

// Forward declarations
extern const ZithToken *parser_peek(const Parser *p);
extern const ZithToken *parser_peek_ahead(const Parser *p, size_t offset);
extern const ZithToken *parser_advance(Parser *p);
extern bool parser_check(const Parser *p, ZithTokenType type);
extern bool parser_match(Parser *p, ZithTokenType type);
extern const ZithToken *parser_expect(Parser *p, ZithTokenType type, const char *msg);
extern void parser_error(Parser *p, const ZithSourceLoc loc, const char *msg);
extern ZithLiteral parse_lit_number(const char*, size_t, ZithTokenType);

// ============================================================================
// Types
// ============================================================================

ZithNode *parser_parse_type(Parser *p) {
    const ZithSourceLoc loc = parser_peek(p)->loc;
    if (parser_match(p, ZITH_TOKEN_QUESTION)) {
        ZithNode *inner = parser_parse_type(p);
        return zith_ast_make_unary_op(p->arena, loc, ZITH_TOKEN_QUESTION, inner, true);
    }
    if (parser_match(p, ZITH_TOKEN_BANG)) {
        ZithNode *inner = parser_parse_type(p);
        return zith_ast_make_unary_op(p->arena, loc, ZITH_TOKEN_BANG, inner, true);
    }
    // Ownership modifiers
    if (parser_match(p, ZITH_TOKEN_UNIQUE) || parser_match(p, ZITH_TOKEN_SHARED) ||
        parser_match(p, ZITH_TOKEN_VIEW) || parser_match(p, ZITH_TOKEN_LEND)) {
        // TODO: Create ownership node
    }
    if (parser_match(p, ZITH_TOKEN_MULTIPLY)) {
        ZithNode *inner = parser_parse_type(p);
        return inner; // TODO: Create pointer node
    }
    if (parser_match(p, ZITH_TOKEN_LBRACKET)) {
        if (!parser_check(p, ZITH_TOKEN_RBRACKET)) parser_parse_expression(p); 
        parser_expect(p, ZITH_TOKEN_RBRACKET, "expected ']' in array type");
        ZithNode *inner = parser_parse_type(p);
        return inner; // TODO: Create array node
    }
    if (parser_check(p, ZITH_TOKEN_TYPE) || parser_check(p, ZITH_TOKEN_IDENTIFIER)) {
        const ZithToken *t = parser_advance(p);
        ZithNode *base = zith_ast_make_identifier(p->arena, loc, t->lexeme.data, t->lexeme.len);
        while (true) {
            if (parser_match(p, ZITH_TOKEN_QUESTION)) {
                base = zith_ast_make_unary_op(p->arena, loc, ZITH_TOKEN_QUESTION, base, true);
                continue;
            }
            if (parser_match(p, ZITH_TOKEN_BANG)) {
                base = zith_ast_make_unary_op(p->arena, loc, ZITH_TOKEN_BANG, base, true);
                continue;
            }
            break;
        }
        return base;
    }
    parser_error(p, loc, "expected type");
    return zith_ast_make_error(p->arena, loc, "expected type");
}

// ============================================================================
// Expressions (Pratt Parser)
// ============================================================================

typedef struct { int8_t left, right; } BindingPower;

static BindingPower infix_bp(ZithTokenType op) {
    switch (op) {
        case ZITH_TOKEN_OR: return {1, 2};
        case ZITH_TOKEN_AND: return {3, 4};
        case ZITH_TOKEN_EQUAL: case ZITH_TOKEN_NOT_EQUAL: return {5, 6};
        case ZITH_TOKEN_LESS_THAN: case ZITH_TOKEN_GREATER_THAN:
        case ZITH_TOKEN_LESS_THAN_OR_EQUAL: case ZITH_TOKEN_GREATER_THAN_OR_EQUAL: return {7, 8};
        case ZITH_TOKEN_PLUS: case ZITH_TOKEN_MINUS: return {9, 10};
        case ZITH_TOKEN_MULTIPLY: case ZITH_TOKEN_DIVIDE: case ZITH_TOKEN_MOD: return {11, 12};
        case ZITH_TOKEN_ARROW: return {13, 14};
        case ZITH_TOKEN_DOT: return {15, 16};
        default: return {-1, -1};
    }
}

static ZithNode *parse_expr_bp(Parser *p, int min_bp);

static ZithNode *parse_nud(Parser *p) {
    const ZithSourceLoc loc = parser_peek(p)->loc;
    const ZithToken *t = parser_advance(p);
    switch (t->type) {
        case ZITH_TOKEN_NUMBER: case ZITH_TOKEN_FLOAT: case ZITH_TOKEN_HEXADECIMAL:
        case ZITH_TOKEN_BINARY: case ZITH_TOKEN_OCTAL:
            return zith_ast_make_literal(p->arena, loc, parse_lit_number(t->lexeme.data, t->lexeme.len, t->type));
        case ZITH_TOKEN_STRING:
            return zith_ast_make_literal(p->arena, loc, {ZITH_LIT_STRING, {.string = {t->lexeme.data + 1, t->lexeme.len - 2}}});
        case ZITH_TOKEN_IDENTIFIER: {
            ZithNode *ident = zith_ast_make_identifier(p->arena, loc, t->lexeme.data, t->lexeme.len);
            if (!parser_match(p, ZITH_TOKEN_LPAREN)) return ident;
            ArenaList<ZithNode *> args_b; args_b.init(p->arena, 8);
            while (!parser_check(p, ZITH_TOKEN_RPAREN) && !parser_is_at_end(p)) {
                args_b.push(p->arena, parser_parse_expression(p));
                if (!parser_match(p, ZITH_TOKEN_COMMA)) break;
            }
            parser_expect(p, ZITH_TOKEN_RPAREN, "expected ')'");
            size_t count = 0; ZithNode **args = args_b.flatten(p->arena, &count);
            return zith_ast_make_call(p->arena, loc, ident, args, count);
        }
        case ZITH_TOKEN_MINUS: case ZITH_TOKEN_BANG:
            return zith_ast_make_unary_op(p->arena, loc, t->type, parse_expr_bp(p, 13), false);
        case ZITH_TOKEN_LPAREN: {
            ZithNode *expr = parser_parse_expression(p);
            parser_expect(p, ZITH_TOKEN_RPAREN, "expected ')'");
            return expr;
        }
        case ZITH_TOKEN_SPAWN:
            return zith_ast_make_spawn(p->arena, loc, parser_parse_expression(p), false);
        default: {
            char buf[128]; snprintf(buf, sizeof(buf), "unexpected token '%.*s'", (int)t->lexeme.len, t->lexeme.data);
            parser_error(p, loc, buf); return zith_ast_make_error(p->arena, loc, buf);
        }
    }
}

static ZithNode *parse_expr_bp(Parser *p, int min_bp) {
    ZithNode *left = parse_nud(p);
    while (true) {
        const ZithTokenType op = parser_peek(p)->type;
        const BindingPower bp = infix_bp(op);
        if (bp.left < min_bp) break;
        const ZithSourceLoc loc = parser_peek(p)->loc;
        parser_advance(p);
        if (op == ZITH_TOKEN_DOT) {
            const ZithToken *member = parser_expect(p, ZITH_TOKEN_IDENTIFIER, "expected member name");
            ZithNode *rhs = zith_ast_make_identifier(p->arena, member->loc, member->lexeme.data, member->lexeme.len);
            left = zith_ast_make_member(p->arena, loc, left, rhs);
            if (parser_match(p, ZITH_TOKEN_LPAREN)) {
                ArenaList<ZithNode *> args_b; args_b.init(p->arena, 8);
                while (!parser_check(p, ZITH_TOKEN_RPAREN) && !parser_is_at_end(p)) {
                    args_b.push(p->arena, parser_parse_expression(p));
                    if (!parser_match(p, ZITH_TOKEN_COMMA)) break;
                }
                parser_expect(p, ZITH_TOKEN_RPAREN, "expected ')'");
                size_t ac = 0; ZithNode **args = args_b.flatten(p->arena, &ac);
                left = zith_ast_make_call(p->arena, loc, left, args, ac);
            }
            continue;
        }
        if (op == ZITH_TOKEN_ARROW) {
            left = zith_ast_make_arrow_call(p->arena, loc, left, parse_expr_bp(p, bp.right));
            continue;
        }
        left = zith_ast_make_binary_op(p->arena, loc, op, left, parse_expr_bp(p, bp.right));
    }
    while (parser_match(p, ZITH_TOKEN_QUESTION))
        left = zith_ast_make_unary_op(p->arena, parser_peek(p)->loc, ZITH_TOKEN_QUESTION, left, true);
    return left;
}

ZithNode *parser_parse_expression(Parser *p) { return parse_expr_bp(p, 0); }
