#include "parser.hpp"

#include "diagnostics/error-codes.hpp"
#include "legacy-zith/parser/operators.hpp"
#include "legacy-zith/parser/scan-helpers.hpp"

#include <array>
#include <cstdlib>
#include <string>
#include <string_view>

namespace zith::parser {

using diagnostics::Severity;
using lexer::TokenKind;

namespace {

struct IntrinsicEntry {
    std::string_view name;
    ast::IntrinsicKind kind;
    int arg_count;
};

static constexpr IntrinsicEntry intrinsics[] = {
    {"fields", ast::IntrinsicKind::Fields, 1},
    {"sizeOf", ast::IntrinsicKind::SizeOf, 1},
    {"alignOf", ast::IntrinsicKind::AlignOf, 1},
    {"hasTrait", ast::IntrinsicKind::HasTrait, 2},
    {"struct", ast::IntrinsicKind::Struct, 0},
    {"component", ast::IntrinsicKind::Component, 0},
    {"union", ast::IntrinsicKind::Union, 0},
    {"enum", ast::IntrinsicKind::Enum, 0},
    {"nullable", ast::IntrinsicKind::Nullable, 0},
    {"primitive", ast::IntrinsicKind::Primitive, 0},
    {"allocate", ast::IntrinsicKind::Allocate, 2},
    {"pack", ast::IntrinsicKind::Pack, 0},
    {"toStruct", ast::IntrinsicKind::ToStruct, 1},
    {"toPack", ast::IntrinsicKind::ToPack, 2},
    {"appendField", ast::IntrinsicKind::AppendField, 2},
    {"removeField", ast::IntrinsicKind::RemoveField, 2},
    {"appendMethod", ast::IntrinsicKind::AppendMethod, 2},
    {"file", ast::IntrinsicKind::File, 0},
    {"line", ast::IntrinsicKind::Line, 0},
    {"fnName", ast::IntrinsicKind::FnName, 0},
    {"location", ast::IntrinsicKind::Location, 0},
    {"ok", ast::IntrinsicKind::Ok, 1},
    {"err", ast::IntrinsicKind::Err, 1},
    {"offsetOf", ast::IntrinsicKind::OffsetOf, 2},
};

const IntrinsicEntry *findIntrinsic(std::string_view name) {
    for (auto &entry : intrinsics)
        if (entry.name == name)
            return &entry;
    return nullptr;
}

int intrinsicArgCount(ast::IntrinsicKind kind) {
    for (auto &entry : intrinsics)
        if (entry.kind == kind)
            return entry.arg_count;
    return 0;
}

} // anonymous namespace

// ── expression parsing ─────────────────────────────────────────────────

ast::ExprId Parser::parsePrimary() {
    skipComments(*tok);

    // block: { stmt; stmt; trailing_expr? }
    if (consume('{'))
        return parseBlock();

    // if expression
    auto if_span = peek().span;
    if (match("if")) {
        auto cond               = parseExpr();
        ast::ExprId then_branch = ast::kInvalidExpr;
        if (check('{')) {
            advance();
            then_branch = parseBlock();
        } else if (!check(';') && !check('}') && !eof()) {
            auto stmt  = parseStmt();
            auto stmts = memory::DynArray<ast::StmtId>{bld->arena()};
            stmts.push(stmt);
            then_branch = bld->block(std::move(stmts));
        }
        ast::ExprId else_branch = ast::kInvalidExpr;
        if (match("else")) {
            if (check('{')) {
                advance();
                else_branch = parseBlock();
            } else if (!check(';') && !check('}') && !eof()) {
                auto stmt  = parseStmt();
                auto stmts = memory::DynArray<ast::StmtId>{bld->arena()};
                stmts.push(stmt);
                else_branch = bld->block(std::move(stmts));
            }
        }
        return bld->ifExpr(cond, then_branch, else_branch, spanFrom(if_span));
    }

    // when expression
    auto when_span = peek().span;
    if (match("when")) {
        consume('(');
        auto subject = parseExpr();
        consume(')');
        consume('{');

        memory::DynArray<ast::WhenCase> cases{bld->arena()};
        while (!eof() && !check('}')) {
            skipComments(*tok);
            if (check('}'))
                break;

            consume('(');
            auto cond = parseExpr();
            consume(')');

            // ~> delimiter
            auto tilde_punc = peek();
            if (!(tilde_punc.is(lexer::TokenKind::Operators) && tilde_punc.punc == '~' &&
                  peek(1).is(lexer::TokenKind::Operators) && peek(1).punc == '>')) {
                errorExpected("'~>' after when case condition");
                return ast::kInvalidExpr;
            }
            advance(2); // consume ~ and >

            ast::ExprId body = ast::kInvalidExpr;
            if (check('{')) {
                advance();
                body = parseBlock();
            } else {
                body = parseExpr();
            }

            auto case_span = memory::Span{when_span.file, bld->exprSpan(cond).start,
                                          bld->exprSpan(body).end};
            cases.push({cond, body, false, case_span});

            if (!consume(',')) {
                // No comma — end of cases
                break;
            }
        }
        consume('}');

        // Mark `_` wildcard as default case
        if (!cases.empty()) {
            auto &last = cases[cases.size() - 1];
            if (auto *ident = std::get_if<ast::IdentNode>(&bld->getExpr(last.condition))) {
                if (ident->name == "_")
                    last.is_default = true;
            }
        }

        return bld->whenExpr(subject, std::move(cases), spanFrom(when_span));
    }

    // while expression
    auto while_span = peek().span;
    if (match("while")) {
        consume('(');
        auto cond = parseExpr();
        consume(')');
        ast::ExprId body = ast::kInvalidExpr;
        if (check('{')) {
            advance();
            body = parseBlock();
        } else if (!check(';') && !check('}') && !eof()) {
            body = parseStmt();
        }
        return bld->whileExpr(cond, body, spanFrom(while_span));
    }

    // 'for' expression — desugar into while
    if (match("for")) {
        auto desugar_block = bld->block(memory::DynArray<ast::StmtId>{bld->arena()});
        auto *block        = std::get_if<ast::BlockNode>(&bld->getExpr(desugar_block));

        if (check('(')) {
            advance();
            // init statement
            if (check(TokenKind::Variable)) {
                advance();
                if (check(TokenKind::Identifier)) {
                    auto name = lexeme();
                    advance();
                    ast::ExprId init_val = ast::kInvalidExpr;
                    if (check('=')) {
                        advance();
                        init_val = parseExpr();
                    }
                    block->stmts.push(bld->letStmt(name, false, init_val));
                }
            } else if (!check(';')) {
                auto expr = parseExpr();
                block->stmts.push(bld->addStmt(expr, bld->exprSpan(expr)));
            }
            consume(';');

            // condition (default true)
            ast::ExprId cond = ast::kInvalidExpr;
            if (!check(';')) {
                cond = parseExpr();
            }
            consume(';');

            // step expression
            ast::ExprId step = ast::kInvalidExpr;
            if (!check(')')) {
                step = parseExpr();
            }
            consume(')');

            // body
            ast::ExprId loop_body = ast::kInvalidExpr;
            if (check('{')) {
                advance();
                loop_body = parseBlock();
            }

            auto body_block = bld->block(memory::DynArray<ast::StmtId>{bld->arena()});
            auto *bb        = std::get_if<ast::BlockNode>(&bld->getExpr(body_block));
            if (loop_body != ast::kInvalidExpr) {
                auto *lb = std::get_if<ast::BlockNode>(&bld->getExpr(loop_body));
                for (auto s : lb->stmts)
                    bb->stmts.push(s);
                bb->trailing = lb->trailing;
            }
            if (step != ast::kInvalidExpr)
                bb->stmts.push(bld->addStmt(step, bld->exprSpan(step)));

            ast::ExprId while_cond =
                cond != ast::kInvalidExpr ? cond : bld->litExpr(ast::LitKind::Bool, "true");
            auto while_expr = bld->whileExpr(while_cond, body_block);
            block->stmts.push(bld->addStmt(while_expr, bld->exprSpan(while_expr)));
        }

        return desugar_block;
    }

    // @ — macro calls and compiler intrinsics
    if (consume('@')) {
        skipComments(*tok);
        auto start_span = peek().span;
        if (!check(TokenKind::Identifier)) {
            errorExpected("identifier after '@'", diagnostics::err::ExpectedIdent);
            return ast::kInvalidExpr;
        }
        auto name = lexeme();
        advance();

        auto *intrinsic = findIntrinsic(name);
        if (check('(')) {
            advance();
            auto args = parseDelimited(*tok, bld->arena(), ')', [this] { return parseExpr(); });
            consume(')');
            if (intrinsic)
                return bld->intrinsic(intrinsic->kind, std::move(args), spanFrom(start_span));
            return bld->macroCall(name, std::move(args), spanFrom(start_span));
        }

        // Intrinsic: @name or @name arg1, arg2
        if (!intrinsic) {
            errorExpected("known intrinsic or '@' macro with parentheses");
            return ast::kInvalidExpr;
        }

        auto kind = intrinsic->kind;
        memory::DynArray<ast::ExprId> args{bld->arena()};
        int expected = intrinsicArgCount(kind);
        if (expected > 0 && !eof() && !check(';') && !check('}') && !check(')') && !check(',')) {
            auto arg = parseExpr();
            if (arg != ast::kInvalidExpr) {
                args.push(arg);
                if (expected > 1 && check(',')) {
                    advance();
                    args.push(parseExpr());
                }
            }
        }
        return bld->intrinsic(kind, std::move(args), spanFrom(start_span));
    }

    // parens
    if (consume('(')) {
        auto expr = parseExpr();
        consume(')');
        return expr;
    }

    // literals
    if (check(TokenKind::LitVal)) {
        auto lit_span = peek().span;
        auto lit      = lexeme();
        advance();
        auto kind = ast::LitKind::Int;
        if (lit == "true" || lit == "false")
            kind = ast::LitKind::Bool;
        else if (lit == "null")
            kind = ast::LitKind::Nil;
        else if (lit.size() > 0 && lit[0] == '"')
            kind = ast::LitKind::String;
        else if (lit.size() > 0 && lit[0] == '\'')
            kind = ast::LitKind::Char;
        else if (lit.find('.') != std::string_view::npos ||
                 lit.find('e') != std::string_view::npos || lit.find('E') != std::string_view::npos)
            kind = ast::LitKind::Float;
        return bld->litExpr(kind, lit, lit_span);
    }

    // :: — scope escape (access outer scope)
    if (peek().is(lexer::TokenKind::Punctuation) && peek().punc == ':' &&
        peek(1).is(lexer::TokenKind::Punctuation) && peek(1).punc == ':' &&
        (peek(2).is(TokenKind::Identifier) || peek(2).is(TokenKind::Type))) {
        advance(2);
        auto name = lexeme();
        auto span = peek().span;
        advance();
        return bld->ident(name, span, true);
    }

    // identifiers — includes type keywords like i32, f64, etc.
    if (check(TokenKind::Identifier) || check(TokenKind::Type)) {
        auto name = lexeme();
        auto span = peek().span;
        advance();
        return bld->ident(name, span);
    }

    errorExpected("expression");
    return ast::kInvalidExpr;
}

ast::ExprId Parser::parsePrefix() {
    skipComments(*tok);

    // Prefix ? — fallback operator
    if (check('?')) {
        auto op_span = peek().span;
        advance();
        auto operand = parsePrefix();
        return bld->unary(ast::UnaryOp::FallbackOpt, operand, spanFrom(op_span));
    }
    // Prefix ! — fallback operator (not !=)
    if (check('!') && peek(1).punc != '=') {
        auto op_span = peek().span;
        advance();
        auto operand = parsePrefix();
        return bld->unary(ast::UnaryOp::FallbackRes, operand, spanFrom(op_span));
    }

    if (check('-')) {
        auto op_span = peek().span;
        advance();
        auto operand = parsePrefix();
        return bld->unary(ast::UnaryOp::Neg, operand, spanFrom(op_span));
    }
    auto not_span = peek().span;
    if (match("not")) {
        auto operand = parsePrefix();
        return bld->unary(ast::UnaryOp::Not, operand, spanFrom(not_span));
    }
    if (check('&')) {
        auto op_span = peek().span;
        advance();
        auto operand = parsePrefix();
        return bld->unary(ast::UnaryOp::Ref, operand, spanFrom(op_span));
    }
    if (check('*')) {
        auto op_span = peek().span;
        advance();
        auto operand = parsePrefix();
        return bld->unary(ast::UnaryOp::Deref, operand, spanFrom(op_span));
    }

    return parsePrimary();
}

ast::ExprId Parser::parseExpr(int min_prec) {
    skipComments(*tok);
    auto lhs = parsePrefix();

    // SeqNode collection for custom word operators (Word token kind)
    memory::DynArray<ast::ExprId> seq_operands{bld->arena()};
    memory::DynArray<ast::OpMarker> seq_ops{bld->arena()};
    bool in_seq = false;

    while (true) {
        skipComments(*tok);
        if (eof())
            break;

        if (lhs == ast::kInvalidExpr)
            break;

        auto &cur     = peek();
        auto lhs_span = bld->exprSpan(lhs);

        // ── Postfix: call ( args ) ──────────────────────────────
        if (cur.is(lexer::TokenKind::Punctuation) && cur.punc == '(') {
            advance();
            auto args = parseDelimited(*tok, bld->arena(), ')', [this] { return parseExpr(); });
            consume(')');
            lhs = bld->call(lhs, std::move(args),
                            memory::Span{lhs_span.file, lhs_span.start, previous().span.end});
            continue;
        }

        // ── Struct literal: Type{value, ...} or Type{x: 1, ...} ─────────────
        if (cur.is(lexer::TokenKind::Punctuation) && cur.punc == '{') {
            auto *type_name = std::get_if<ast::IdentNode>(&bld->getExpr(lhs));
            if (!type_name)
                break;
            advance();

            memory::DynArray<ast::StructFieldInit> fields{bld->arena()};
            if (!tok->is_empty() && tok->peek().punc != '}') {
                do {
                    if (tok->peek().punc == '}')
                        break;

                    if (tok->peek().is(lexer::TokenKind::Identifier) && tok->peek(1).punc == ':') {
                        // Nominal
                        auto field_name = lexeme();
                        advance(); // ident
                        advance(); // :
                        ast::ExprId val = ast::kInvalidExpr;
                        if (tok->peek().is(lexer::TokenKind::Identifier) && lexeme() == "_") {
                            advance();
                        } else {
                            val = parseExpr();
                        }
                        fields.push({field_name, val});
                    } else {
                        // Positional
                        auto val = parseExpr();
                        fields.push({"", val});
                    }
                } while (consume(','));
            }
            consume('}');
            lhs = bld->structLiteral(
                type_name->name, std::move(fields),
                memory::Span{lhs_span.file, lhs_span.start, previous().span.end});
            continue;
        }

        // ── Range operators: ..  >..  ..<  >..< ────────────────────
        // Must come before .field since `..` overlaps with `.`.
        {
            ast::RangeBounds bounds = ast::RangeBounds::Closed;
            uint32_t advance_n      = 0;
            auto &c0                = peek(0);
            auto &c1                = peek(1);
            auto &c2                = peek(2);

            // >.. or >..< — left-open
            if (c0.is(lexer::TokenKind::Operators) && c0.punc == '>' &&
                c1.is(lexer::TokenKind::Punctuation) && c1.punc == '.' &&
                c2.is(lexer::TokenKind::Punctuation) && c2.punc == '.') {
                advance_n = 3; // consume > . .
                if (peek(3).is(lexer::TokenKind::Operators) && peek(3).punc == '<') {
                    advance_n = 4;
                    bounds    = ast::RangeBounds::Open;
                } else {
                    bounds = ast::RangeBounds::OpenLeft;
                }
            }
            // .. or ..< — right-inclusive or right-open
            else if (c0.is(lexer::TokenKind::Punctuation) && c0.punc == '.' &&
                     c1.is(lexer::TokenKind::Punctuation) && c1.punc == '.') {
                advance_n = 2; // consume . .
                if (c2.is(lexer::TokenKind::Operators) && c2.punc == '<') {
                    advance_n = 3;
                    bounds    = ast::RangeBounds::OpenRight;
                } else {
                    bounds = ast::RangeBounds::Closed;
                }
            }

            if (advance_n > 0) {
                advance(advance_n);
                auto rhs = parseExpr();
                lhs      = bld->range(lhs, rhs, bounds,
                                      memory::Span{lhs_span.file, lhs_span.start,
                                                   bld->exprSpan(rhs).end});
                continue;
            }
        }

        // ── Postfix: .field ──────────────────────────────────────────
        if (cur.is(lexer::TokenKind::Punctuation) && cur.punc == '.') {
            advance();
            auto field_span = peek().span;
            auto field_name = lexeme();
            advance();
            auto *ident          = std::get_if<ast::IdentNode>(&bld->getExpr(lhs));
            bool is_enum_variant = false;
            if (ident && syms) {
                auto sym_id = syms->lookup(ident->name);
                if (sym_id != symbols::kInvalidSym) {
                    if (syms->get(sym_id).kind == symbols::SymKind::Enum) {
                        is_enum_variant = true;
                    }
                }
            }
            if (is_enum_variant) {
                lhs = bld->enumValue(ident->name, field_name,
                                     memory::Span{lhs_span.file, lhs_span.start, field_span.end});
            } else {
                lhs = bld->field(lhs, field_name,
                                 memory::Span{lhs_span.file, lhs_span.start, field_span.end});
            }
            continue;
        }

        // ── Postfix: [index] ────────────────────────────────────
        if (cur.is(lexer::TokenKind::Punctuation) && cur.punc == '[') {
            advance();
            auto index = parseExpr();
            if (index != ast::kInvalidExpr && !consume(']'))
                errorExpected("']'");
            lhs = bld->index(lhs, index,
                             memory::Span{lhs_span.file, lhs_span.start, previous().span.end});
            continue;
        }

        // ── Postfix: ?/! propagate ────────────────────────────────────
        if (cur.is(lexer::TokenKind::Operators) && (cur.punc == '!' || cur.punc == '?') &&
            !peek(1).is(lexer::TokenKind::Operators)) {
            advance();
            auto op =
                (previous().punc == '!') ? ast::UnaryOp::PropagateRes : ast::UnaryOp::PropagateOpt;
            lhs = bld->unary(op, lhs,
                             memory::Span{lhs_span.file, lhs_span.start, previous().span.end});
            continue;
        }

        // ── Terminators ──────────────────────────────────────────────
        if (check(';') || check(',') || check(')') || check(']') || check('}'))
            break;

        // ── Word infix: and / or / xor (Logical tokens) ─────────
        if (cur.is(lexer::TokenKind::Logical)) {
            auto word         = lexeme();
            uint8_t word_prec = operators::wordPrec(word);
            if (word_prec == 0 || word_prec < min_prec)
                break;
            advance();
            auto rhs = parseExpr(word_prec + 1);
            lhs      = bld->binary(lhs, operators::binaryOpForWord(word), rhs, spanFrom(lhs, rhs));
            continue;
        }

        // ── Custom word operators (Word token: nop, etc.) ────────
        if (cur.is(lexer::TokenKind::Word)) {
            auto word  = lexeme();
            uint8_t wp = operators::wordPrec(word);
            if (wp < min_prec)
                break;

            if (!in_seq) {
                seq_operands.push(lhs);
                in_seq = true;
            } else {
                seq_operands.push(lhs);
            }

            // Adjacency error: two non-nop word ops adjacent
            if (!seq_ops.empty() && seq_ops.back().is_word && seq_ops.back().word_name != "nop" &&
                word != "nop") {
                diag->report(Severity::Error, diagnostics::err::ExpectedExpr,
                             std::string("ambiguous word operators; use parentheses: (a ")
                                 .append(seq_ops.back().word_name)
                                 .append(" b) ")
                                 .append(word)
                                 .append(" c or a ")
                                 .append(seq_ops.back().word_name)
                                 .append(" (b ")
                                 .append(word)
                                 .append(" c)"),
                             memory::Span{lhs_span.file, lhs_span.start, peek().span.end});
            }

            seq_ops.push({memory::Span{lhs_span.file, lhs_span.start, peek().span.end},
                          ast::BinaryOp::Add, word, wp, true});
            advance();
            lhs = parseExpr(wp + 1);
            continue;
        }

        // ── is / as (type check / cast) ──────────────────────────
        if (cur.is(lexer::TokenKind::Is)) {
            uint8_t prec = operators::logicalPrec();
            if (prec < min_prec)
                break;
            advance();
            auto rhs = parseExpr(prec + 1);
            lhs      = bld->binary(lhs, ast::BinaryOp::Is, rhs, spanFrom(lhs, rhs));
            continue;
        }
        if (cur.is(lexer::TokenKind::As)) {
            uint8_t prec = operators::logicalPrec();
            if (prec < min_prec)
                break;
            advance();
            auto rhs = parseExpr(prec + 1);
            lhs      = bld->binary(lhs, ast::BinaryOp::As, rhs, spanFrom(lhs, rhs));
            continue;
        }

        // ── Compound operator: ==, !=, <=, >=, <<, >> ───
        ast::BinaryOp compound_op;
        if (operators::tryCompoundOp(cur, peek(1), compound_op)) {
            uint8_t prec = operators::infixPrec(cur);
            if (prec < min_prec)
                break;
            advance(2);
            auto rhs = parseExpr(prec + 1);
            lhs      = bld->binary(lhs, compound_op, rhs, spanFrom(lhs, rhs));
            continue;
        }

        // ── Infix binary operator (single char) ─────────────────
        uint8_t prec = operators::infixPrec(cur);
        if (prec == 0 || prec < min_prec)
            break;

        // Check that it's really an infix operator, not part of a compound
        ast::BinaryOp compound_check;
        if (operators::tryCompoundOp(cur, peek(1), compound_check))
            break;

        advance();
        auto rhs = parseExpr(prec + 1);
        lhs      = bld->binary(lhs, operators::binaryOpForChar(cur.punc), rhs, spanFrom(lhs, rhs));
    }

    if (in_seq) {
        seq_operands.push(lhs);
        memory::Span seq_span = {};
        if (!seq_operands.empty())
            seq_span = bld->exprSpan(seq_operands[0]);
        return bld->seq(std::move(seq_operands), std::move(seq_ops), seq_span);
    }
    return lhs;
}

ast::ExprId Parser::parseExpr() {
    return parseExpr(0);
}

ast::ExprId Parser::parseBlock() {
    auto start_span = peek().span;
    // '{' already consumed by advance() in caller
    memory::DynArray<ast::StmtId> stmts{bld->arena()};
    ast::ExprId trailing = ast::kInvalidExpr;
    while (!eof() && !check('}')) {
        skipComments(*tok);
        if (check('}'))
            break;
        stmts.push(parseStmt());
    }
    // If the last statement is an expression-statement, it's the trailing expression
    if (!stmts.empty()) {
        auto last_id    = stmts.back();
        auto &last_node = bld->getStmt(last_id);
        if (auto *expr_stmt = ast::asExprStmt(last_node)) {
            trailing = expr_stmt->expr;
            stmts.pop_back();
        }
    }
    if (check('}'))
        advance();
    return bld->block(std::move(stmts), trailing, spanFrom(start_span));
}

} // namespace zith::parser
