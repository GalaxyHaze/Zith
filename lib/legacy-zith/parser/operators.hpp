#pragma once

#include "legacy-zith/ast/ast-nodes.hpp"
#include "legacy-zith/lexer/token.hpp"

namespace zith::parser::operators {

struct WordOpEntry {
    std::string_view word;
    ast::BinaryOp op;
    uint8_t prec;
};

static constexpr WordOpEntry word_ops[] = {
    {"and", ast::BinaryOp::And, 3},
    {"or", ast::BinaryOp::Or, 2},
    {"xor", ast::BinaryOp::Xor, 4},
};

inline uint8_t infixPrec(const lexer::Token &t) {
    switch (t.punc) {
    case '|':
        return 6;
    case '^':
        return 7;
    case '&':
        return 8;
    case '<':
        return 10;
    case '>':
        return 10;
    case '+':
        return 12;
    case '-':
        return 12;
    case '*':
        return 14;
    case '/':
        return 14;
    case '%':
        return 14;
    default:
        return 0;
    }
}

inline ast::BinaryOp binaryOpForWord(std::string_view w) {
    for (auto &entry : word_ops)
        if (entry.word == w)
            return entry.op;
    return ast::BinaryOp::Add;
}

inline uint8_t wordPrec(std::string_view w) {
    for (auto &entry : word_ops)
        if (entry.word == w)
            return entry.prec;
    return 0;
}

inline uint8_t logicalPrec() {
    return 5;
} // is / as
inline uint8_t chainPrec() {
    return 1;
} // -> chain flow (deferred)

inline bool tryCompoundOp(const lexer::Token &first, const lexer::Token &second,
                          ast::BinaryOp &out_op) {
    if (!first.is(lexer::TokenKind::Operators) || !second.is(lexer::TokenKind::Operators))
        return false;
    if (first.punc == '=' && second.punc == '=') {
        out_op = ast::BinaryOp::Eq;
        return true;
    }
    if (first.punc == '!' && second.punc == '=') {
        out_op = ast::BinaryOp::Ne;
        return true;
    }
    if (first.punc == '<' && second.punc == '=') {
        out_op = ast::BinaryOp::Le;
        return true;
    }
    if (first.punc == '>' && second.punc == '=') {
        out_op = ast::BinaryOp::Ge;
        return true;
    }
    if (first.punc == '<' && second.punc == '<') {
        out_op = ast::BinaryOp::Shl;
        return true;
    }
    if (first.punc == '>' && second.punc == '>') {
        out_op = ast::BinaryOp::Shr;
        return true;
    }
    if (first.punc == '=' && second.punc == '>') {
        out_op = ast::BinaryOp::Eq;
        return true;
    }
    return false;
}

inline ast::BinaryOp binaryOpForChar(char c) {
    switch (c) {
    case '+':
        return ast::BinaryOp::Add;
    case '-':
        return ast::BinaryOp::Sub;
    case '*':
        return ast::BinaryOp::Mul;
    case '/':
        return ast::BinaryOp::Div;
    case '%':
        return ast::BinaryOp::Rest;
    case '<':
        return ast::BinaryOp::Lt;
    case '>':
        return ast::BinaryOp::Gt;
    case '=':
        return ast::BinaryOp::Eq;
    case '!':
        return ast::BinaryOp::Ne;
    case '&':
        return ast::BinaryOp::And;
    case '|':
        return ast::BinaryOp::Or;
    default:
        return ast::BinaryOp::Add;
    }
}

} // namespace zith::parser::operators
