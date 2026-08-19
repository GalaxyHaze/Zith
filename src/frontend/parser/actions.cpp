#include "frontend/parser/actions.hpp"
#include "frontend/parser/parse.hpp"

#include "common/memory/arena.hpp"
#include "common/memory/dyn-array.hpp"
#include "frontend/ast/ast.hpp"
#include "frontend/ast/walk.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using Parser = generated_parser::Parser<sample::ParseOutput>;
using Token = generated_lexer::Token;
using Arena = common::memory::Arena;

namespace hooks::parser {

void top(Parser &, const Token &) {}

} // namespace hooks::parser

namespace {

using namespace generated_ast;
using generated_lexer::TokenKind;

using Node = AstNode *;

struct Cursor {
    generated_lexer::TokenStream &tokens;
    std::string_view source;
    sample::ParseOutput &output;

    [[nodiscard]] bool has() const noexcept { return tokens.hasNext(); }
    [[nodiscard]] const Token &cur() const noexcept { return tokens.current(); }
    [[nodiscard]] const Token &peek(std::size_t distance = 1) const noexcept {
        std::size_t saved = tokens.offset;
        for (std::size_t i = 0; i < distance && tokens.offset + 1 < tokens.len; ++i)
            ++tokens.offset;
        const Token &result = tokens.current();
        tokens.offset = saved;
        return result;
    }

    [[nodiscard]] std::string_view text() const noexcept { return slice(cur().span); }
    [[nodiscard]] std::string_view textOf(const Token &token) const noexcept {
        return slice(token.span);
    }
    [[nodiscard]] std::string_view slice(Span span) const noexcept {
        if (span.end <= source.size() && span.start <= span.end)
            return source.substr(span.start, span.end - span.start);
        return {};
    }

    void advance() noexcept {
        if (tokens.hasNext())
            tokens.advance();
    }

    [[nodiscard]] bool atLexeme(std::string_view word) const noexcept {
        return has() && text() == word;
    }
    [[nodiscard]] bool peekAtLexeme(std::string_view word,
                                    std::size_t distance = 1) const noexcept {
        if (!has())
            return false;
        const Token &next = peek(distance);
        return textOf(next) == word;
    }
    [[nodiscard]] bool atPunc(char punctuation) const noexcept {
        return has() && cur().kind == TokenKind::Punctuation &&
               cur().punc == punctuation;
    }
    [[nodiscard]] bool peekAtPunc(char punctuation,
                                  std::size_t distance = 1) const noexcept {
        if (!has())
            return false;
        const Token &next = peek(distance);
        return next.kind == TokenKind::Punctuation && next.punc == punctuation;
    }
    [[nodiscard]] bool atOp(std::string_view op) const noexcept {
        return has() && cur().kind == TokenKind::Operators && text() == op;
    }
    [[nodiscard]] bool peekAtOp(std::string_view op,
                                std::size_t distance = 1) const noexcept {
        if (!has())
            return false;
        const Token &next = peek(distance);
        return next.kind == TokenKind::Operators && textOf(next) == op;
    }

    bool eatPunc(char punctuation) noexcept {
        if (!atPunc(punctuation))
            return false;
        advance();
        return true;
    }
    bool eatOp(std::string_view op) noexcept {
        if (!atOp(op))
            return false;
        advance();
        return true;
    }
    bool eatLexeme(std::string_view word) noexcept {
        if (!atLexeme(word))
            return false;
        advance();
        return true;
    }

    [[nodiscard]] Span curSpan() const noexcept {
        return has() ? cur().span : Span{0, 0};
    }

    [[nodiscard]] bool isIdentifier(const Token &token) const noexcept {
        const auto kind = token.kind;
        if (kind == TokenKind::Identifier || kind == TokenKind::LitVal)
            return true;
        constexpr std::string_view identifiers[] = {
            "as",   "asset", "import", "export", "from", "use",
            "fn",   "let",   "var",    "global", "const", "struct",
            "component", "enum", "union", "type", "alias", "trait",
            "interface", "marker", "stackful", "macro", "context",
            "implement", "impl", "prefix", "suffix", "infix", "nop",
            "state", "enter", "leave",
        };
        const std::string_view word = textOf(token);
        for (const auto candidate : identifiers)
            if (word == candidate)
                return true;
        return false;
    }

    void diagnose(Span span, std::string_view message) {
        sample::ParserDiagnostic diagnostic;
        diagnostic.span = span;
        diagnostic.message = std::string(message);
        output.diagnostics.push_back(std::move(diagnostic));
    }

    [[nodiscard]] std::string_view storeString(std::string_view value) {
        if (value.empty())
            return {};
        void *storage = output.arena.alloc(value.size());
        if (storage == nullptr)
            return {};
        std::memcpy(storage, value.data(), value.size());
        return {static_cast<const char *>(storage), value.size()};
    }
};

template <typename T, typename... Args>
T *make(Cursor &cursor, Args &&...args) {
    return generated_ast::make<T>(cursor.output.ast,
                                  std::forward<Args>(args)...);
}

Node parseExpression(Cursor &cursor, int minPrecedence = 0);
Node parsePrimary(Cursor &cursor);
Node parseType(Cursor &cursor);
Node parseBinding(Cursor &cursor, bool mutable_, Span start,
                  bool keepSemicolon);
Node parseStatement(Cursor &cursor);
Node parseBlock(Cursor &cursor, Span &span);

[[nodiscard]] Span nodeSpan(AstNode *node) noexcept {
    if (node == nullptr)
        return Span{0, 0};
    switch (node->kind) {
    case NodeKind::Program:
        return static_cast<Program *>(node)->span;
    case NodeKind::Declaration:
        return static_cast<Declaration *>(node)->span;
    case NodeKind::GenericParam:
        return static_cast<GenericParam *>(node)->span;
    case NodeKind::Parameter:
        return static_cast<Parameter *>(node)->span;
    case NodeKind::ImportDecl:
        return static_cast<ImportDecl *>(node)->span;
    case NodeKind::ImportPathSegment:
        return static_cast<ImportPathSegment *>(node)->span;
    case NodeKind::ImportSelector:
        return static_cast<ImportSelector *>(node)->span;
    case NodeKind::TypeExpr:
        return static_cast<TypeExpr *>(node)->span;
    case NodeKind::Expr:
        return static_cast<Expr *>(node)->span;
    case NodeKind::ExprField:
        return static_cast<ExprField *>(node)->span;
    case NodeKind::Stmt:
        return static_cast<Stmt *>(node)->span;
    case NodeKind::Binding:
        return static_cast<Binding *>(node)->span;
    }
    return Span{0, 0};
}

[[nodiscard]] bool sameSpan(Span left, Span right) noexcept {
    return left.start == right.start && left.end == right.end;
}

[[nodiscard]] bool startsDeclaration(const std::string_view word) {
    return word == "fn" || word == "const" || word == "raw" || word == "extern" ||
           word == "flow" || word == "let" || word == "var" || word == "global" ||
           word == "struct" || word == "component" || word == "enum" ||
           word == "union" || word == "type" || word == "alias" ||
           word == "trait" || word == "interface" || word == "state" ||
           word == "macro" || word == "context" || word == "implement" ||
           word == "impl" || word == "prefix" || word == "suffix" ||
           word == "infix" || word == "nop" || word == "pub" || word == "mod" ||
           word == "import" || word == "from" || word == "export";
}

[[nodiscard]] bool isDelimiterOpen(const Token &token) noexcept {
    return token.kind == TokenKind::Punctuation &&
           (token.punc == '(' || token.punc == '[' || token.punc == '{');
}

[[nodiscard]] bool isDelimiterClose(const Token &token) noexcept {
    return token.kind == TokenKind::Punctuation &&
           (token.punc == ')' || token.punc == ']' || token.punc == '}');
}

[[nodiscard]] Span rangeSpan(Span first, Span last) {
    return Span{first.start, last.end};
}

[[nodiscard]] bool isBinaryOperator(const std::string_view op) {
    return op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" ||
           op == ">=" || op == "<<" || op == ">>" || op == "+" || op == "-" ||
           op == "*" || op == "/" || op == "%" || op == "&" || op == "|" ||
           op == "^" || op == "|." || op == "&." || op == "^." ||
           op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=" ||
           op == "%=" || op == "<<=" || op == ">>=" || op == "&=" || op == "|=" ||
           op == "^=";
}

[[nodiscard]] bool isAssignmentOperator(const std::string_view op) {
    return op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=" ||
           op == "%=" || op == "<<=" || op == ">>=" || op == "&=" || op == "|=" ||
           op == "^=";
}

[[nodiscard]] int binaryPrecedence(const std::string_view op) {
    if (isAssignmentOperator(op))
        return 1;
    if (op == "|." || op == "&." || op == "^.")
        return 2;
    if (op == "|" || op == "^")
        return 3;
    if (op == "&")
        return 4;
    if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" ||
        op == ">=")
        return 5;
    if (op == "<<" || op == ">>")
        return 6;
    if (op == "+" || op == "-")
        return 7;
    if (op == "*" || op == "/" || op == "%")
        return 8;
    return 0;
}

[[nodiscard]] bool isUnaryPrefix(const Token &token, std::string_view text) {
    if (token.kind == TokenKind::Operators)
        return text == "-" || text == "!" || text == "~" || text == "&" ||
               text == "*";
    return token.kind == TokenKind::Typedef && text == "not";
}

[[nodiscard]] bool isWordUnaryPrefix(Cursor &cursor) {
    return cursor.atLexeme("not");
}

[[nodiscard]] Expr *makeExpr(Cursor &cursor, Span span, int kind,
                             std::string_view op, std::string_view text,
                             Node castType = nullptr,
                             Node alternate = nullptr) {
    return make<Expr>(cursor, span, kind, op, text, castType, alternate);
}

[[nodiscard]] TypeExpr *makeType(Cursor &cursor, Span span, int kind,
                                 int ownership, std::string_view name,
                                 uint64_t length, bool isMut, bool hasMut) {
    return make<TypeExpr>(cursor, span, kind, ownership, name, length, isMut,
                          hasMut);
}

[[nodiscard]] Stmt *makeStatement(Cursor &cursor, Span span, int kind,
                                  std::string_view label, bool stackful,
                                  Node expression, Node binding,
                                  Node returnValue = nullptr) {
    return make<Stmt>(cursor, span, kind, label, stackful, expression,
                      returnValue, binding);
}

[[nodiscard]] Binding *makeBinding(Cursor &cursor, Span span,
                                   std::string_view name, bool mutable_,
                                   Node type, Node initializer) {
    return make<Binding>(cursor, span, name, mutable_, type, initializer);
}

void skipToDeclarationBoundary(Cursor &cursor) {
    int depth = 0;
    while (cursor.has() && cursor.cur().kind != TokenKind::End) {
        if (depth == 0 && cursor.atPunc(';'))
            break;
        if (isDelimiterOpen(cursor.cur()))
            ++depth;
        else if (isDelimiterClose(cursor.cur())) {
            if (depth > 0)
                --depth;
        }
        cursor.advance();
    }
    if (cursor.atPunc(';'))
        cursor.advance();
}

void coalesceGarbage(Cursor &cursor, Span start) {
    cursor.diagnose(start, "unexpected token at top level");
    skipToDeclarationBoundary(cursor);
}

template <typename T>
void appendAll(common::memory::DynArray<AstNode *> &target,
               std::vector<T> &values) {
    for (auto *value : values)
        target.push(static_cast<AstNode *>(value));
}

Node parseBlockOrExpression(Cursor &cursor) {
    if (cursor.atPunc('{')) {
        Span blockSpan;
        return parseBlock(cursor, blockSpan);
    }
    return parseExpression(cursor);
}

Node parseCallArgs(Cursor &cursor, Node callee, Span start) {
    cursor.advance();
    Expr *call = makeExpr(cursor, start,
                          static_cast<int>(sample::ExprKind::Call), "",
                          static_cast<Expr *>(callee)->text, nullptr);
    call->operands.push(callee);
    while (cursor.has() && !cursor.atPunc(')')) {
        if (cursor.atPunc(',')) {
            cursor.advance();
            continue;
        }
        call->operands.push(parseExpression(cursor));
        if (!cursor.atPunc(',') && !cursor.atPunc(')'))
            cursor.diagnose(cursor.cur().span,
                            "expected ',' or ')' in call arguments");
        if (cursor.atPunc(','))
            cursor.advance();
    }
    if (!cursor.eatPunc(')'))
        cursor.diagnose(cursor.cur().span, "expected ')' after call arguments");
    call->span = rangeSpan(start, cursor.curSpan());
    return call;
}

Node parsePostfix(Cursor &cursor, Node primary, Span start) {
    Node node = primary;
    while (cursor.has()) {
        if (cursor.atPunc('(')) {
            node = parseCallArgs(cursor, node, start);
            continue;
        }
        if (cursor.atPunc('[')) {
            cursor.advance();
            Node index = parseExpression(cursor, 1);
            if (!cursor.eatPunc(']'))
                cursor.diagnose(cursor.cur().span, "expected ']' after index expression");
            Expr *indexed = makeExpr(cursor, rangeSpan(nodeSpan(node), cursor.curSpan()),
                                     static_cast<int>(sample::ExprKind::Index),
                                     "", "", nullptr);
            indexed->operands.push(node);
            indexed->operands.push(index);
            node = indexed;
            continue;
        }
        if (cursor.atOp("->") || cursor.atPunc('.')) {
            const bool arrow = cursor.atOp("->");
            cursor.advance();
            if (!cursor.has() || !cursor.isIdentifier(cursor.cur())) {
                cursor.diagnose(cursor.curSpan(),
                                arrow ? "expected field name after '->'"
                                      : "expected field name after '.'");
                continue;
            }
            const std::string_view name = cursor.text();
            cursor.advance();
            if (cursor.atPunc('?')) {
                cursor.diagnose(cursor.cur().span,
                                "optional propagation is parsed but not evaluated yet");
                cursor.advance();
            }
            Expr *field = makeExpr(cursor, rangeSpan(nodeSpan(node), cursor.curSpan()),
                                   arrow ? static_cast<int>(sample::ExprKind::Arrow)
                                         : static_cast<int>(sample::ExprKind::Field),
                                   arrow ? "->" : ".", name, nullptr);
            field->operands.push(node);
            node = field;
            continue;
        }
        if (cursor.atLexeme("as")) {
            cursor.advance();
            Node type = parseType(cursor);
            Expr *cast = makeExpr(cursor, rangeSpan(nodeSpan(node),
                                                    type != nullptr
                                                        ? nodeSpan(type)
                                                        : cursor.curSpan()),
                                  static_cast<int>(sample::ExprKind::Cast), "as",
                                  "", type);
            cast->operands.push(node);
            node = cast;
            continue;
        }
        break;
    }
    return node;
}

Node parseArrayLiteral(Cursor &cursor, Span start) {
    cursor.advance();
    Expr *array = makeExpr(cursor, start,
                           static_cast<int>(sample::ExprKind::ArrayLiteral),
                           "", "");
    while (cursor.has() && !cursor.atPunc(']')) {
        if (cursor.atPunc(',')) {
            cursor.advance();
            continue;
        }
        array->operands.push(parseExpression(cursor));
        if (!cursor.atPunc(',') && !cursor.atPunc(']'))
            cursor.diagnose(cursor.cur().span,
                            "expected ',' or ']' after array element");
        if (cursor.atPunc(','))
            cursor.advance();
    }
    if (!cursor.eatPunc(']'))
        cursor.diagnose(cursor.cur().span, "expected ']' after array literal");
    array->span = rangeSpan(start, cursor.curSpan());
    return array;
}

Node parseStructLiteral(Cursor &cursor, std::string_view name, Span start) {
    cursor.advance();
    Expr *literal = makeExpr(cursor, start,
                             static_cast<int>(sample::ExprKind::StructLiteral),
                             "", name);
    bool sawNamed = false;
    bool sawPositional = false;
    while (cursor.has() && !cursor.atPunc('}')) {
        if (cursor.atPunc(',')) {
            cursor.advance();
            continue;
        }
        const bool named = cursor.isIdentifier(cursor.cur()) &&
                           cursor.peekAtPunc(':');
        if (named) {
            if (sawPositional)
                cursor.diagnose(cursor.cur().span,
                                "cannot mix positional and named struct literal fields");
            sawNamed = true;
            const std::string_view fieldName = cursor.text();
            const Span fieldSpan = cursor.cur().span;
            cursor.advance();
            cursor.advance();
            Node value = cursor.atLexeme("_")
                             ? makeExpr(cursor, cursor.cur().span,
                                        static_cast<int>(sample::ExprKind::Placeholder),
                                        "", "_", nullptr)
                             : parseExpression(cursor);
            if (cursor.atLexeme("_") && sameSpan(cursor.cur().span, fieldSpan))
                cursor.advance();
            ExprField *field =
                make<ExprField>(cursor,
                                rangeSpan(fieldSpan,
                                          value != nullptr
                                              ? nodeSpan(value)
                                              : cursor.curSpan()),
                                fieldName, value);
            literal->fieldNames.push(field);
        } else {
            if (sawNamed)
                cursor.diagnose(cursor.cur().span,
                                "cannot mix positional and named struct literal fields");
            sawPositional = true;
            literal->operands.push(parseExpression(cursor));
        }
        if (cursor.atPunc(','))
            cursor.advance();
    }
    if (!cursor.eatPunc('}'))
        cursor.diagnose(cursor.cur().span,
                        "expected '}' after struct literal fields");
    literal->span = rangeSpan(start, cursor.curSpan());
    return literal;
}

Node parseParenthesized(Cursor &cursor, Span start) {
    cursor.advance();
    Node inner = parseExpression(cursor);
    if (!cursor.eatPunc(')'))
        cursor.diagnose(cursor.cur().span, "expected ')' after expression");
    return parsePostfix(cursor, inner, start);
}

Node parseIf(Cursor &cursor, Span start) {
    cursor.advance();
    Expr *ifExpr = makeExpr(cursor, start,
                            static_cast<int>(sample::ExprKind::If), "", "",
                            nullptr, nullptr);
    if (cursor.atPunc('(')) {
        cursor.advance();
        ifExpr->conditions.push(parseExpression(cursor));
        if (!cursor.eatPunc(')'))
            cursor.diagnose(cursor.cur().span, "expected ')' after if condition");
    } else {
        ifExpr->conditions.push(parseExpression(cursor));
    }
    if (cursor.atPunc('{')) {
        Span blockSpan;
        ifExpr->statements.push(parseBlock(cursor, blockSpan));
    } else {
        cursor.diagnose(cursor.cur().span, "expected '{' after if condition");
    }
    if (cursor.eatLexeme("else")) {
        if (cursor.atLexeme("if")) {
            ifExpr->alternate = parseIf(cursor, cursor.cur().span);
        } else if (cursor.atPunc('{')) {
            Span blockSpan;
            ifExpr->alternate = parseBlock(cursor, blockSpan);
        } else {
            cursor.diagnose(cursor.cur().span, "expected block after 'else'");
        }
    }
    ifExpr->span = rangeSpan(start, cursor.curSpan());
    return ifExpr;
}

Node parseFor(Cursor &cursor, Span start) {
    cursor.advance();
    Expr *loop = makeExpr(cursor, start,
                          static_cast<int>(sample::ExprKind::For), "", "",
                          nullptr, nullptr);
    if (cursor.atPunc('{')) {
        Span blockSpan;
        loop->statements.push(parseBlock(cursor, blockSpan));
    } else if (cursor.atPunc('(')) {
        cursor.advance();
        const Span clauseStart = cursor.curSpan();
        Node init = nullptr;
        if (!cursor.atPunc(';')) {
            if (cursor.atLexeme("var") || cursor.atLexeme("let") ||
                cursor.atLexeme("const")) {
                const bool mutable_ = cursor.atLexeme("var");
                cursor.advance();
                init = parseBinding(cursor, mutable_, clauseStart, true);
            } else {
                init = parseExpression(cursor);
            }
        }
        if (cursor.atPunc(';')) {
            cursor.advance();
            Node condition = nullptr;
            if (!cursor.atPunc(';'))
                condition = parseExpression(cursor);
            if (!cursor.eatPunc(';'))
                cursor.diagnose(cursor.cur().span,
                                "expected ';' after for condition");
            Node step = nullptr;
            if (!cursor.atPunc(')'))
                step = parseExpression(cursor);
            if (!cursor.eatPunc(')'))
                cursor.diagnose(cursor.cur().span,
                                "expected ')' after for clauses");
            if (!cursor.atPunc('{')) {
                cursor.diagnose(cursor.cur().span,
                                "expected '{' after for clauses");
            } else {
                Span blockSpan;
                loop->statements.push(parseBlock(cursor, blockSpan));
            }
            if (init != nullptr) {
                Stmt *initStatement = makeStatement(
                    cursor, nodeSpan(init),
                    static_cast<int>(sample::StmtKind::Expression), "", false,
                    init, nullptr);
                loop->operands.push(initStatement);
            }
            if (condition == nullptr) {
                condition =
                    makeExpr(cursor, rangeSpan(start, cursor.curSpan()),
                             static_cast<int>(sample::ExprKind::Literal), "",
                             "true", nullptr, nullptr);
            }
            loop->conditions.push(condition);
            if (step != nullptr)
                loop->operands.push(step);
        } else if (cursor.atLexeme("in") || cursor.atOp("in")) {
            cursor.advance();
            cursor.diagnose(rangeSpan(clauseStart, cursor.curSpan()),
                            "unsupported iterator 'for ... in ...' form");
            parseExpression(cursor);
            if (!cursor.eatPunc(')'))
                cursor.diagnose(cursor.cur().span,
                                "expected ')' after for iterator target");
            if (cursor.atPunc('{')) {
                Span blockSpan;
                parseBlock(cursor, blockSpan);
            } else {
                cursor.diagnose(cursor.cur().span,
                                "expected '{' after for iterator form");
            }
        } else {
            if (init != nullptr)
                loop->conditions.push(init);
            if (!cursor.eatPunc(')'))
                cursor.diagnose(cursor.cur().span,
                                "expected ')' after for condition");
            if (cursor.atPunc('{')) {
                Span blockSpan;
                loop->statements.push(parseBlock(cursor, blockSpan));
            } else {
                cursor.diagnose(cursor.cur().span,
                                "expected '{' after for condition");
            }
        }
    } else if (cursor.isIdentifier(cursor.cur())) {
        const Span iteratorStart = cursor.cur().span;
        cursor.advance();
        if (cursor.atLexeme("in")) {
            cursor.advance();
            cursor.diagnose(rangeSpan(iteratorStart, cursor.curSpan()),
                            "unsupported iterator 'for ... in ...' form");
            parseExpression(cursor);
            if (cursor.atPunc('{')) {
                Span blockSpan;
                parseBlock(cursor, blockSpan);
            }
        } else {
            cursor.diagnose(cursor.cur().span, "expected '{' or '(' after 'for'");
        }
    } else {
        cursor.diagnose(cursor.cur().span, "expected '{' or '(' after 'for'");
    }
    loop->span = rangeSpan(start, cursor.curSpan());
    return loop;
}

Node parseWhen(Cursor &cursor, Span start) {
    cursor.advance();
    Expr *whenExpr = makeExpr(cursor, start,
                              static_cast<int>(sample::ExprKind::When), "", "",
                              nullptr, nullptr);
    Node subject = nullptr;
    if (cursor.atPunc('(')) {
        cursor.advance();
        subject = parseExpression(cursor);
        if (!cursor.eatPunc(')'))
            cursor.diagnose(cursor.cur().span,
                            "expected ')' after when/match subject");
    } else {
        subject = parseExpression(cursor);
    }
    whenExpr->conditions.push(subject);
    if (!cursor.eatPunc('{'))
        cursor.diagnose(cursor.cur().span,
                        "expected '{' after when/match subject");
    while (cursor.has() && !cursor.atPunc('}')) {
        if (cursor.atPunc(',')) {
            cursor.advance();
            continue;
        }
        Node pattern = nullptr;
        const bool parenthesizedPattern = cursor.atPunc('(');
        if (parenthesizedPattern)
            cursor.advance();
        const bool underscorePattern =
            cursor.atLexeme("_") &&
            (cursor.peekAtPunc(')') || cursor.peekAtOp("~>"));
        if (underscorePattern) {
            pattern = makeExpr(cursor, cursor.cur().span,
                               static_cast<int>(sample::ExprKind::Placeholder),
                               "", "_", nullptr, nullptr);
            cursor.advance();
        } else {
            pattern = parseExpression(cursor);
        }
        if (parenthesizedPattern) {
            if (!cursor.eatPunc(')'))
                cursor.diagnose(cursor.cur().span,
                                "expected ')' after 'when' case pattern");
        }
        if (cursor.atOp("~>")) {
            cursor.advance();
            Node body = parseBlockOrExpression(cursor);
            if (pattern != nullptr &&
                pattern->kind == generated_ast::NodeKind::Expr &&
                static_cast<Expr *>(pattern)->kind ==
                    static_cast<int>(sample::ExprKind::Placeholder)) {
                if (whenExpr->alternate == nullptr)
                    whenExpr->alternate = body;
                else
                    cursor.diagnose(cursor.cur().span,
                                    "multiple default cases in when/match");
            } else {
                whenExpr->cases.push(pattern);
                whenExpr->cases.push(body);
            }
        } else {
            cursor.diagnose(cursor.cur().span, "expected '~>' in when case");
            if (!underscorePattern)
                whenExpr->cases.push(pattern);
        }
        if (cursor.atPunc(','))
            cursor.advance();
    }
    if (!cursor.eatPunc('}'))
        cursor.diagnose(cursor.cur().span, "expected '}' after when/match cases");
    whenExpr->span = rangeSpan(start, cursor.curSpan());
    return whenExpr;
}

Node parsePrimary(Cursor &cursor) {
    if (!cursor.has())
        return makeExpr(cursor, Span{0, 0},
                        static_cast<int>(sample::ExprKind::Error), "", "");
    const Span start = cursor.cur().span;

    if (cursor.atPunc('{'))
        return parseBlock(cursor, const_cast<Span &>(start));
    if (cursor.atPunc('('))
        return parseParenthesized(cursor, start);
    if (cursor.atPunc('['))
        return parseArrayLiteral(cursor, start);
    if (cursor.atLexeme("if"))
        return parseIf(cursor, start);
    if (cursor.atLexeme("while")) {
        const Span span = cursor.cur().span;
        cursor.advance();
        if (cursor.atPunc('(')) {
            cursor.advance();
            (void)parseExpression(cursor);
            if (!cursor.eatPunc(')'))
                cursor.diagnose(cursor.cur().span,
                                "expected ')' after while condition");
        }
        if (cursor.atPunc('{')) {
            Span blockSpan;
            (void)parseBlock(cursor, blockSpan);
        } else {
            cursor.diagnose(cursor.cur().span,
                            "expected '{' after while condition");
        }
        cursor.diagnose(span, "while is no longer supported");
        return makeExpr(cursor, rangeSpan(span, cursor.curSpan()),
                        static_cast<int>(sample::ExprKind::Error), "", "");
    }
    if (cursor.atLexeme("for"))
        return parseFor(cursor, start);
    if (cursor.atLexeme("when") || cursor.atLexeme("match"))
        return parseWhen(cursor, start);
    if (cursor.atPunc('@')) {
        cursor.advance();
        if (!cursor.has() || !cursor.isIdentifier(cursor.cur())) {
            cursor.diagnose(start, "expected a macro name after '@'");
            return parsePostfix(cursor,
                                makeExpr(cursor, start,
                                         static_cast<int>(sample::ExprKind::Error),
                                         "", ""),
                                start);
        }
        const std::string_view name = cursor.text();
        cursor.advance();
        Expr *call = makeExpr(cursor, rangeSpan(start, cursor.curSpan()),
                              static_cast<int>(sample::ExprKind::MacroCall), "",
                              name, nullptr);
        if (cursor.atPunc('(')) {
            cursor.advance();
            while (cursor.has() && !cursor.atPunc(')')) {
                if (cursor.atPunc(',')) {
                    cursor.advance();
                    continue;
                }
                call->operands.push(parseExpression(cursor));
                if (cursor.atPunc(','))
                    cursor.advance();
            }
            if (!cursor.eatPunc(')'))
                cursor.diagnose(cursor.cur().span,
                                "expected ')' after macro arguments");
        }
        call->span = rangeSpan(start, cursor.curSpan());
        return parsePostfix(cursor, call, start);
    }
    if (cursor.atLexeme("true") || cursor.atLexeme("false") ||
        cursor.atLexeme("null") || cursor.atLexeme("unknown") ||
        cursor.atLexeme("invalid")) {
        cursor.advance();
        Node literal = makeExpr(cursor, start,
                                static_cast<int>(sample::ExprKind::Literal), "",
                                cursor.slice(start));
        return parsePostfix(cursor, literal, start);
    }
    if (cursor.cur().kind == TokenKind::LitVal) {
        cursor.advance();
        Node literal = makeExpr(cursor, start,
                                static_cast<int>(sample::ExprKind::Literal), "",
                                cursor.slice(start));
        return parsePostfix(cursor, literal, start);
    }
    if (cursor.cur().kind == TokenKind::Operators &&
        isUnaryPrefix(cursor.cur(), cursor.text())) {
        cursor.advance();
        Node operand = parseExpression(cursor, 9);
        Expr *unary = makeExpr(cursor, rangeSpan(start,
                                                 operand != nullptr
                                                     ? nodeSpan(operand)
                                                     : cursor.curSpan()),
                               static_cast<int>(sample::ExprKind::Unary),
                               cursor.slice(start), "", nullptr);
        if (operand != nullptr)
            unary->operands.push(operand);
        return parsePostfix(cursor, unary, start);
    }
    if (isWordUnaryPrefix(cursor)) {
        cursor.advance();
        Node operand = parseExpression(cursor, 9);
        Expr *unary = makeExpr(cursor, rangeSpan(start,
                                                 operand != nullptr
                                                     ? nodeSpan(operand)
                                                     : cursor.curSpan()),
                               static_cast<int>(sample::ExprKind::Unary),
                               cursor.slice(start), "", nullptr);
        if (operand != nullptr)
            unary->operands.push(operand);
        return parsePostfix(cursor, unary, start);
    }
    if (cursor.isIdentifier(cursor.cur())) {
        const std::string_view name = cursor.text();
        cursor.advance();
        if (cursor.atPunc('{'))
            return parsePostfix(cursor, parseStructLiteral(cursor, name, start),
                                start);
        Node nameExpr = makeExpr(cursor, start,
                                 static_cast<int>(sample::ExprKind::Name), "",
                                 cursor.slice(start));
        return parsePostfix(cursor, nameExpr, start);
    }

    cursor.diagnose(cursor.cur().span, "expected an expression");
    Node error = makeExpr(cursor, start,
                          static_cast<int>(sample::ExprKind::Error), "",
                          cursor.slice(start));
    cursor.advance();
    return parsePostfix(cursor, error, start);
}

Node parseExpression(Cursor &cursor, int minPrecedence) {
    Node left = parsePrimary(cursor);
    while (cursor.has()) {
        if (cursor.atPunc(';') || cursor.atPunc(',') || cursor.atPunc(')') ||
            cursor.atPunc(']') || cursor.atPunc('}') || cursor.atOp("~>"))
            break;
        if (cursor.atLexeme("and") || cursor.atLexeme("or") ||
            cursor.atLexeme("&&") || cursor.atLexeme("||")) {
            cursor.diagnose(cursor.cur().span,
                            "unsupported logical operator; use bitwise operators");
            cursor.advance();
            parseExpression(cursor, 1);
            continue;
        }
        if (cursor.atLexeme("is")) {
            const Span opSpan = cursor.cur().span;
            cursor.advance();
            if (cursor.eatLexeme("null")) {
                Expr *isNull = makeExpr(cursor, rangeSpan(nodeSpan(left),
                                                          cursor.curSpan()),
                                        static_cast<int>(sample::ExprKind::IsNull),
                                        "is", "null", nullptr);
                isNull->operands.push(left);
                left = isNull;
                continue;
            }
            cursor.diagnose(rangeSpan(opSpan, cursor.cur().span),
                            "unsupported 'is' form; only 'is null' is supported");
            parseType(cursor);
            continue;
        }
        if (cursor.atLexeme("as")) {
            cursor.advance();
            Node type = parseType(cursor);
            Expr *cast = makeExpr(cursor, rangeSpan(nodeSpan(left),
                                                    type != nullptr
                                                        ? nodeSpan(type)
                                                        : cursor.curSpan()),
                                  static_cast<int>(sample::ExprKind::Cast), "as",
                                  "", type);
            cast->operands.push(left);
            left = cast;
            continue;
        }
        if (cursor.cur().kind != TokenKind::Operators ||
            !isBinaryOperator(cursor.text())) {
            break;
        }
        const std::string_view op = cursor.text();
        const int precedence = binaryPrecedence(op);
        if (precedence < minPrecedence)
            break;
        cursor.advance();
        Node right = parseExpression(cursor, precedence + 1);
        const int kind = isAssignmentOperator(op)
                             ? static_cast<int>(sample::ExprKind::Assign)
                             : static_cast<int>(sample::ExprKind::Binary);
        Expr *binary = makeExpr(cursor, rangeSpan(nodeSpan(left),
                                                  right != nullptr
                                                      ? nodeSpan(right)
                                                      : cursor.curSpan()),
                                kind, op, "", nullptr);
        binary->operands.push(left);
        if (right != nullptr)
            binary->operands.push(right);
        left = binary;
    }
    return left;
}

Node parseType(Cursor &cursor) {
    if (!cursor.has())
        return makeType(cursor, Span{0, 0},
                        static_cast<int>(sample::TypeExprKind::Error), 0, "",
                        0, false, false);
    const Span start = cursor.cur().span;
    int ownership = 0;
    bool hasOwnership = false;
    bool hasMut = false;
    if (cursor.atLexeme("lend") || cursor.atLexeme("share") ||
        cursor.atLexeme("view") || cursor.atLexeme("unique") ||
        cursor.atLexeme("belong")) {
        ownership = cursor.atLexeme("lend")   ? 1
                    : cursor.atLexeme("share") ? 2
                    : cursor.atLexeme("view")  ? 3
                    : cursor.atLexeme("unique") ? 4
                                                : 5;
        hasOwnership = true;
        cursor.advance();
    }
    while (cursor.atLexeme("mut")) {
        if (hasMut)
            cursor.diagnose(cursor.cur().span,
                            "duplicate 'mut' qualifier on this type");
        hasMut = true;
        cursor.advance();
    }
    if (hasOwnership && ownership == 3 && hasMut)
        cursor.diagnose(start,
                        "'view' is read-only and cannot be combined with 'mut'");

    bool isMut = hasMut || ownership == 1 || ownership == 2 || ownership == 4 ||
                 ownership == 5;
    int kind = static_cast<int>(sample::TypeExprKind::Name);
    uint64_t length = 0;
    std::string_view name;
    Node inner = nullptr;

    if (cursor.atPunc('*') ||
        (cursor.cur().kind == TokenKind::Operators &&
         cursor.text() == "*")) {
        cursor.advance();
        kind = static_cast<int>(sample::TypeExprKind::Pointer);
        inner = parseType(cursor);
    } else if (cursor.atPunc('?') ||
               (cursor.cur().kind == TokenKind::Operators &&
                cursor.text() == "?")) {
        cursor.advance();
        kind = static_cast<int>(sample::TypeExprKind::Optional);
        inner = parseType(cursor);
    } else if (cursor.atPunc('[')) {
        cursor.advance();
        if (cursor.atPunc(']')) {
            cursor.advance();
            kind = static_cast<int>(sample::TypeExprKind::Slice);
        } else {
            kind = static_cast<int>(sample::TypeExprKind::Array);
            if (cursor.cur().kind == TokenKind::LitVal) {
                length = std::strtoull(std::string(cursor.text()).c_str(),
                                       nullptr, 10);
                cursor.advance();
            } else {
                cursor.diagnose(cursor.cur().span, "expected an array length");
            }
            if (!cursor.eatPunc(']'))
                cursor.diagnose(cursor.cur().span,
                                "expected ']' after array length");
        }
        inner = parseType(cursor);
    } else if (cursor.atLexeme("raw") && cursor.peekAtLexeme("opaque")) {
        cursor.advance();
        cursor.advance();
        kind = static_cast<int>(sample::TypeExprKind::Opaque);
    } else if (cursor.isIdentifier(cursor.cur()) ||
               cursor.cur().kind == TokenKind::Type ||
               cursor.cur().kind == TokenKind::Struct ||
               cursor.cur().kind == TokenKind::Typedef ||
               cursor.cur().kind == TokenKind::Trait ||
               cursor.cur().kind == TokenKind::Interface ||
               cursor.cur().kind == TokenKind::Raw ||
               cursor.cur().kind == TokenKind::Mutable ||
               cursor.cur().kind == TokenKind::Ownership) {
        name = cursor.text();
        cursor.advance();
    } else {
        cursor.diagnose(cursor.cur().span, "expected a type");
        cursor.advance();
        kind = static_cast<int>(sample::TypeExprKind::Error);
    }

    Node type = makeType(cursor, rangeSpan(start, cursor.curSpan()), kind,
                         ownership, name, length, isMut, hasMut);
    if (inner != nullptr)
        static_cast<TypeExpr *>(type)->arguments.push(inner);
    return type;
}

Node parseBinding(Cursor &cursor, bool mutable_, Span start,
                  bool keepSemicolon = false) {
    if (!cursor.has() || !cursor.isIdentifier(cursor.cur())) {
        cursor.diagnose(cursor.cur().span, "expected binding name");
        const Span bad = cursor.curSpan();
        return makeBinding(cursor, Span{start.start, bad.end}, "", mutable_,
                           nullptr, nullptr);
    }
    const std::string_view name = cursor.text();
    cursor.advance();
    Node type = nullptr;
    if (cursor.atPunc(':')) {
        cursor.advance();
        type = parseType(cursor);
    }
    Node initializer = nullptr;
    if (cursor.atOp("=")) {
        cursor.advance();
        initializer = parseExpression(cursor, 1);
    }
    if (cursor.atPunc(';') && !keepSemicolon)
        cursor.advance();
    return makeBinding(cursor, rangeSpan(start, cursor.curSpan()), name,
                       mutable_, type, initializer);
}

Node parseStatement(Cursor &cursor) {
    if (!cursor.has())
        return makeStatement(cursor, Span{0, 0},
                             static_cast<int>(sample::StmtKind::Expression), "",
                             false, nullptr, nullptr);
    const Span start = cursor.cur().span;
    const std::string_view word = cursor.text();

    if (word == "let" || word == "var" || word == "const" || word == "global") {
        const bool mutable_ = word != "const";
        cursor.advance();
        Node binding = parseBinding(cursor, mutable_, start);
        return makeStatement(cursor, rangeSpan(start, cursor.curSpan()),
                             static_cast<int>(sample::StmtKind::Binding), "",
                             false, nullptr, binding);
    }
    if (word == "return" || word == "break" || word == "continue") {
        const int kind = word == "return"
                             ? static_cast<int>(sample::StmtKind::Return)
                             : word == "break"
                                   ? static_cast<int>(sample::StmtKind::Break)
                                   : static_cast<int>(sample::StmtKind::Continue);
        cursor.advance();
        Node value = nullptr;
        if (word == "return" && !cursor.atPunc(';') && !cursor.atPunc('}')) {
            value = parseExpression(cursor);
            if ((cursor.cur().kind == TokenKind::Operators &&
                 (cursor.text() == "?" || cursor.text() == "!"))) {
                const Span markerSpan = cursor.cur().span;
                const std::string_view marker = cursor.text();
                cursor.advance();
                if (value != nullptr &&
                    value->kind == generated_ast::NodeKind::Expr) {
                    Expr *valueExpr = static_cast<Expr *>(value);
                    valueExpr->text = cursor.storeString(
                        std::string(valueExpr->text) + std::string(marker));
                    valueExpr->span =
                        rangeSpan(nodeSpan(valueExpr), markerSpan);
                }
            }
        }
        if (cursor.atPunc(';'))
            cursor.advance();
        return makeStatement(cursor, rangeSpan(start, cursor.curSpan()), kind,
                             "", false, value, nullptr, value);
    }
    if (word == "dock" || word == "marker" || word == "stackful") {
        cursor.diagnose(start,
                        std::string(word) +
                            " is no longer the flow syntax; use state/enter/leave/jump");
        while (cursor.has() && !cursor.atPunc(';')) {
            if (cursor.atPunc('{') || cursor.atPunc('(') ||
                cursor.atPunc(')') || cursor.atPunc('}'))
                cursor.advance();
            else
                parseExpression(cursor);
        }
        if (cursor.atPunc(';'))
            cursor.advance();
        return makeStatement(cursor, rangeSpan(start, cursor.curSpan()),
                             static_cast<int>(sample::StmtKind::Expression), "",
                             false, nullptr, nullptr);
    }
    if (word == "enter" || word == "jump") {
        const int kind = word == "enter"
                             ? static_cast<int>(sample::StmtKind::Enter)
                             : static_cast<int>(sample::StmtKind::Jump);
        cursor.advance();
        std::string_view target;
        if (cursor.isIdentifier(cursor.cur())) {
            target = cursor.text();
            cursor.advance();
        }
        Node targetExpr = makeExpr(cursor, rangeSpan(start, cursor.curSpan()),
                                   static_cast<int>(sample::ExprKind::Name), "",
                                   target, nullptr);
        Stmt *statement = makeStatement(cursor, start, kind,
                                        target, false, targetExpr, nullptr);
        if (!cursor.atPunc('('))
            cursor.diagnose(cursor.cur().span,
                            std::string(word) + " target requires '('");
        else {
            cursor.advance();
            while (cursor.has() && !cursor.atPunc(')')) {
                if (cursor.atPunc(',')) {
                    cursor.advance();
                    continue;
                }
                statement->arguments.push(parseExpression(cursor));
                if (cursor.atPunc(','))
                    cursor.advance();
            }
            if (!cursor.eatPunc(')'))
                cursor.diagnose(cursor.cur().span,
                                "expected ')' after " + std::string(word) +
                                    " arguments");
        }
        if (cursor.atPunc(';'))
            cursor.advance();
        statement->span = rangeSpan(start, cursor.curSpan());
        return statement;
    }
    if (word == "leave") {
        cursor.advance();
        Node value = nullptr;
        if (!cursor.atPunc(';') && !cursor.atPunc('}'))
            value = parseExpression(cursor);
        if (cursor.atPunc(';'))
            cursor.advance();
        Stmt *statement = makeStatement(cursor, rangeSpan(start, cursor.curSpan()),
                                        static_cast<int>(sample::StmtKind::Leave),
                                        "", false, value, nullptr);
        return statement;
    }
    if (word == "use") {
        cursor.diagnose(start, "unsupported 'use' statement");
        while (cursor.has() && !cursor.atPunc(';'))
            cursor.advance();
        if (cursor.atPunc(';'))
            cursor.advance();
        return makeStatement(cursor, rangeSpan(start, cursor.curSpan()),
                             static_cast<int>(sample::StmtKind::Expression), "",
                             false, nullptr, nullptr);
    }

    Node expression = parseExpression(cursor);
    if (cursor.atPunc(';'))
        cursor.advance();
    return makeStatement(cursor, rangeSpan(start, cursor.curSpan()),
                         static_cast<int>(sample::StmtKind::Expression), "",
                         false, expression, nullptr);
}

Node parseBlock(Cursor &cursor, Span &span) {
    if (!cursor.atPunc('{'))
        return nullptr;
    const Span start = cursor.cur().span;
    cursor.advance();
    Expr *block = makeExpr(cursor, start,
                           static_cast<int>(sample::ExprKind::Block), "", "");
    while (cursor.has() && !cursor.atPunc('}')) {
        if (cursor.atPunc(';')) {
            cursor.advance();
            continue;
        }
        block->statements.push(parseStatement(cursor));
    }
    if (!cursor.eatPunc('}'))
        cursor.diagnose(cursor.cur().span, "expected '}' after block");
    block->span = rangeSpan(start, cursor.curSpan());
    span = block->span;
    return block;
}

void parseParameterList(Cursor &cursor, Declaration *declaration) {
    if (!cursor.atPunc('('))
        return;
    cursor.advance();
    while (cursor.has() && !cursor.atPunc(')')) {
        if (cursor.atPunc(',') || cursor.atOp("...")) {
            cursor.advance();
            continue;
        }
        const Span paramSpan = cursor.cur().span;
        const std::string_view name =
            cursor.isIdentifier(cursor.cur()) ? cursor.text() : std::string_view{};
        if (cursor.isIdentifier(cursor.cur()))
            cursor.advance();
        Node type = nullptr;
        if (cursor.atPunc(':')) {
            cursor.advance();
            type = parseType(cursor);
        }
        Node defaultValue = nullptr;
        if (cursor.atOp("=")) {
            cursor.advance();
            defaultValue = parseExpression(cursor, 1);
        }
        declaration->parameters.push(make<Parameter>(
            cursor, rangeSpan(paramSpan, cursor.curSpan()), name, type,
            defaultValue));
        if (cursor.atPunc(','))
            cursor.advance();
    }
    if (!cursor.eatPunc(')'))
        cursor.diagnose(cursor.cur().span, "expected ')' after parameter list");
}

void parseGenericParams(Cursor &cursor, Declaration *declaration) {
    if (!cursor.atOp("<"))
        return;
    cursor.advance();
    while (cursor.has() && !cursor.atOp(">")) {
        if (cursor.atPunc(',')) {
            cursor.advance();
            continue;
        }
        const Span paramSpan = cursor.cur().span;
        const std::string_view name =
            cursor.isIdentifier(cursor.cur()) ? cursor.text() : std::string_view{};
        if (cursor.isIdentifier(cursor.cur()))
            cursor.advance();
        Node constraint = nullptr;
        if (cursor.atPunc(':')) {
            cursor.advance();
            constraint = parseType(cursor);
        }
        declaration->genericParams.push(make<GenericParam>(
            cursor, rangeSpan(paramSpan, cursor.curSpan()), name, constraint));
        if (cursor.atPunc(','))
            cursor.advance();
    }
    if (!cursor.eatOp(">"))
        cursor.diagnose(cursor.cur().span, "expected '>' after generic parameters");
}

int declarationKindFromWord(const std::string_view word) {
    if (word == "state")
        return static_cast<int>(sample::DeclKind::State);
    if (word == "type" || word == "alias")
        return static_cast<int>(sample::DeclKind::TypeAlias);
    if (word == "struct" || word == "component")
        return static_cast<int>(sample::DeclKind::Struct);
    if (word == "enum")
        return static_cast<int>(sample::DeclKind::Enum);
    if (word == "union")
        return static_cast<int>(sample::DeclKind::Union);
    if (word == "trait")
        return static_cast<int>(sample::DeclKind::Trait);
    if (word == "interface")
        return static_cast<int>(sample::DeclKind::Interface);
    if (word == "let" || word == "var" || word == "const" || word == "global")
        return static_cast<int>(sample::DeclKind::Variable);
    if (word == "context")
        return static_cast<int>(sample::DeclKind::Context);
    if (word == "prefix" || word == "suffix" || word == "infix" || word == "nop")
        return static_cast<int>(sample::DeclKind::Word);
    return static_cast<int>(sample::DeclKind::Error);
}

void parseTypeBody(Cursor &cursor, Declaration *declaration, Span start) {
    if (!cursor.atPunc('{'))
        return;
    cursor.advance();
    while (cursor.has() && !cursor.atPunc('}')) {
        if (cursor.atPunc(',')) {
            cursor.advance();
            continue;
        }
        const Span fieldSpan = cursor.cur().span;
        const std::string_view fieldName =
            cursor.isIdentifier(cursor.cur()) ? cursor.text() : std::string_view{};
        cursor.advance();
        if (cursor.atPunc(':')) {
            cursor.advance();
            Node fieldType = parseType(cursor);
            declaration->parameters.push(make<Parameter>(
                cursor, rangeSpan(fieldSpan, cursor.curSpan()), fieldName,
                fieldType, nullptr));
        } else if (cursor.atOp("=")) {
            cursor.diagnose(fieldSpan,
                            "unsupported: field '" + std::string(fieldName) +
                                " = <expr>'");
            parseExpression(cursor);
        } else {
            cursor.diagnose(fieldSpan, "expected ':' after field name '" +
                                           std::string(fieldName) + "'");
        }
        if (cursor.atPunc(','))
            cursor.advance();
    }
    if (!cursor.eatPunc('}'))
        cursor.diagnose(cursor.cur().span, "expected '}' after type body");
}

bool maybeFunctionKind(Cursor &cursor, int &functionKind, bool &isExtern) {
    if (cursor.eatLexeme("fn")) {
        functionKind = static_cast<int>(sample::FunctionKind::Standard);
        return true;
    }
    const std::string_view prefixes[] = {"const", "raw", "extern"};
    for (const auto prefix : prefixes) {
        if (!cursor.atLexeme(prefix) || !cursor.peekAtLexeme("fn"))
            continue;
        functionKind = prefix == "const"
                           ? static_cast<int>(sample::FunctionKind::Const)
                       : prefix == "raw"
                           ? static_cast<int>(sample::FunctionKind::Raw)
                           : static_cast<int>(sample::FunctionKind::Extern);
        if (prefix == "extern")
            isExtern = true;
        cursor.advance();
        cursor.advance();
        return true;
    }
    return false;
}

bool parseImportPath(Cursor &cursor, sample::ImportDecl &meta) {
    const Span start = cursor.cur().span;
    Span pathSpan = start;
    std::string rawPath;
    std::vector<std::string_view> segments;
    std::vector<Span> segmentSpans;
    std::vector<sample::ImportSelector> selectors;

    if (cursor.cur().kind == TokenKind::LitVal && !cursor.text().empty() &&
        cursor.text().front() == '\"') {
        const std::string_view text = cursor.text();
        meta.headerPath = text;
        meta.rawPath = text;
        meta.isHeader = true;
        meta.pathSpan = text.length() >= 2
                            ? Span{
                                  static_cast<uint32_t>(
                                      text.data() - cursor.source.data()),
                                  static_cast<uint32_t>(
                                      text.data() - cursor.source.data() +
                                      static_cast<std::ptrdiff_t>(text.length()))}
                            : start;
        cursor.advance();
    } else {
        while (cursor.has()) {
            if (cursor.atPunc('.')) {
                if (cursor.peekAtPunc('.')) {
                    rawPath += "..";
                    segments.push_back("..");
                    segmentSpans.push_back(
                        rangeSpan(cursor.cur().span, cursor.peek().span));
                    cursor.advance();
                    cursor.advance();
                } else {
                    rawPath += ".";
                    cursor.advance();
                }
                continue;
            }
            if (cursor.atPunc('/') ||
                (cursor.cur().kind == TokenKind::Operators &&
                 cursor.text() == "/")) {
                rawPath += "/";
                cursor.advance();
                continue;
            }
            if (cursor.cur().kind == TokenKind::As ||
                cursor.atLexeme("as")) {
                break;
            }
            if (cursor.isIdentifier(cursor.cur())) {
                const Span segmentSpan = cursor.cur().span;
                const std::string_view segment = cursor.text();
                rawPath += std::string(segment);
                segments.push_back(segment);
                segmentSpans.push_back(segmentSpan);
                cursor.advance();
                pathSpan = rangeSpan(start, cursor.curSpan());
                continue;
            }
            break;
        }
        meta.pathSpan = pathSpan;
        meta.rawPath = cursor.storeString(rawPath);
    }

    if (segments.empty() && meta.headerPath.empty()) {
        cursor.diagnose(start, "expected import path");
        return false;
    }

    if (cursor.atPunc('(')) {
        cursor.advance();
        if (cursor.atOp("..")) {
            meta.depth = -1;
            cursor.advance();
        } else if (cursor.cur().kind == TokenKind::LitVal) {
            meta.depth = static_cast<int32_t>(
                std::strtol(std::string(cursor.text()).c_str(), nullptr, 10));
            cursor.advance();
        } else {
            cursor.diagnose(cursor.cur().span, "expected import depth");
        }
        if (!cursor.eatPunc(')'))
            cursor.diagnose(cursor.cur().span, "expected ')' after import depth");
    }
    if (cursor.eatLexeme("as")) {
        if (!cursor.isIdentifier(cursor.cur()))
            cursor.diagnose(cursor.cur().span, "expected alias after 'as'");
        else {
            meta.alias = cursor.storeString(cursor.text());
            meta.aliasSpan = cursor.cur().span;
            cursor.advance();
        }
    }
    if (cursor.atPunc('{')) {
        cursor.advance();
        while (cursor.has() && !cursor.atPunc('}')) {
            if (cursor.atPunc(',')) {
                cursor.advance();
                continue;
            }
            const Span nameSpan = cursor.cur().span;
            const std::string_view selectorName =
                cursor.isIdentifier(cursor.cur()) ? cursor.text()
                                                  : std::string_view{};
            cursor.advance();
            std::string_view selectorAlias;
            Span aliasSpan;
            if (cursor.eatLexeme("as")) {
                selectorAlias = cursor.isIdentifier(cursor.cur())
                                    ? cursor.text()
                                    : std::string_view{};
                aliasSpan = cursor.cur().span;
                if (cursor.has())
                    cursor.advance();
            }
            meta.selectors.push_back(
                sample::ImportSelector{selectorName, selectorAlias, nameSpan,
                                       aliasSpan});
            if (cursor.atPunc(','))
                cursor.advance();
        }
        if (!cursor.eatPunc('}'))
            cursor.diagnose(cursor.cur().span,
                            "expected '}' after import selectors");
    }
    if (!cursor.eatPunc(';'))
        cursor.diagnose(cursor.cur().span,
                        "expected ';' after import declaration");
    meta.span = rangeSpan(start, cursor.curSpan());
    meta.path.assign(segments.begin(), segments.end());
    meta.pathSpans.assign(segmentSpans.begin(), segmentSpans.end());
    return true;
}

Node lowerImport(Cursor &cursor, Span start, bool isFrom, bool isExport) {
    bool isAsset = false;
    if (cursor.eatLexeme("asset"))
        isAsset = true;
    sample::ImportDecl meta;
    meta.isFrom = isFrom;
    meta.isExport = isExport;
    meta.isAsset = isAsset;
    if (!parseImportPath(cursor, meta))
        return nullptr;
    if (!meta.isAsset &&
        (meta.rawPath.rfind("assets/", 0) == 0 ||
         meta.rawPath.rfind(".json", meta.rawPath.size() - 5) !=
             std::string_view::npos))
        meta.isAsset = true;

    ImportDecl *node = make<ImportDecl>(
        cursor, meta.span, meta.pathSpan, meta.aliasSpan, meta.rawPath,
        meta.headerPath, meta.alias, meta.isFrom, meta.isExport, meta.isAsset,
        meta.isHeader, meta.depth);
    for (std::size_t i = 0; i < meta.path.size(); ++i)
        node->path.push(make<ImportPathSegment>(cursor, meta.pathSpans[i],
                                                meta.path[i]));
    for (const auto &selector : meta.selectors)
        node->selectors.push(make<ImportSelector>(
            cursor, selector.span, selector.aliasSpan, selector.name,
            selector.alias));
    cursor.output.imports.push_back(std::move(meta));
    return node;
}

Node parseDeclaration(Cursor &cursor, int visibility,
                      int visibilityAncestors, int visibilityDescendants,
                      bool externPending) {
    if (!cursor.has())
        return nullptr;
    const Span start = cursor.cur().span;
    const std::string_view word = cursor.text();

    if (word == "import" || word == "from" || word == "export") {
        const bool from = word == "from";
        const bool export_ = word == "export";
        cursor.advance();
        return lowerImport(cursor, start, from, export_);
    }

    int functionKind = static_cast<int>(sample::FunctionKind::Standard);
    bool isExtern = externPending;
    if (maybeFunctionKind(cursor, functionKind, isExtern)) {
        std::string_view name;
        if (cursor.cur().kind == TokenKind::Identifier) {
            name = cursor.text();
            cursor.advance();
        }
        Declaration *declaration = make<Declaration>(
            cursor, start,
            static_cast<int>(sample::DeclKind::Function), visibility,
            visibilityAncestors, visibilityDescendants,
            functionKind, name, "", "", isExtern, false, false, false, false,
            false, false, nullptr, nullptr, nullptr);
        parseGenericParams(cursor, declaration);
        parseParameterList(cursor, declaration);
        if (cursor.atPunc(':') || cursor.atOp("->")) {
            cursor.advance();
            declaration->declaredType = parseType(cursor);
        }
        if (cursor.atPunc('{')) {
            Span blockSpan;
            declaration->body = parseBlock(cursor, blockSpan);
        }
        if (cursor.atPunc(';'))
            cursor.advance();
        declaration->span = rangeSpan(start, cursor.curSpan());
        return declaration;
    }

    if (word == "let" || word == "var" || word == "global" ||
    (word == "const" && !cursor.peekAtLexeme("fn"))) {
        cursor.advance();
        Node binding = parseBinding(cursor, word != "const", start);
        Declaration *declaration = make<Declaration>(
            cursor, rangeSpan(start, cursor.curSpan()),
            static_cast<int>(sample::DeclKind::Variable), visibility,
            visibilityAncestors, visibilityDescendants,
            static_cast<int>(sample::FunctionKind::Standard),
            static_cast<Binding *>(binding)->name, "", "", false, false, false,
            false, false, false, false, nullptr, nullptr, nullptr);
        declaration->initializer = static_cast<Binding *>(binding)->initializer;
        return declaration;
    }

    const int kind = declarationKindFromWord(word);
    if (word != "const" &&
        (word == "state" || word == "type" || word == "alias" ||
         word == "struct" || word == "component" || word == "enum" ||
         word == "union" || word == "trait" || word == "interface" ||
         word == "context" || word == "prefix" || word == "suffix" ||
         word == "infix" || word == "nop")) {
        cursor.advance();
        std::string_view name;
        if (cursor.isIdentifier(cursor.cur())) {
            name = cursor.text();
            cursor.advance();
        }
        Declaration *declaration = make<Declaration>(
            cursor, start, kind, visibility,
            visibilityAncestors, visibilityDescendants,
            static_cast<int>(sample::FunctionKind::Standard), name, "", "",
            false, word == "type", false, false, false, false, false,
            nullptr, nullptr, nullptr);
        if (word == "state") {
            parseParameterList(cursor, declaration);
            if (cursor.atPunc('{')) {
                Span blockSpan;
                declaration->body = parseBlock(cursor, blockSpan);
            }
        } else if (word == "type" || word == "alias") {
            if (cursor.atOp("="))
                cursor.advance();
            declaration->declaredType = parseType(cursor);
            if (cursor.atPunc(';'))
                cursor.advance();
        } else {
            parseGenericParams(cursor, declaration);
            parseTypeBody(cursor, declaration, start);
        }
        declaration->span = rangeSpan(start, cursor.curSpan());
        return declaration;
    }

    if (word == "macro") {
        cursor.advance();
        std::string_view name;
        if (cursor.isIdentifier(cursor.cur())) {
            name = cursor.text();
            cursor.advance();
        }
        Declaration *declaration = make<Declaration>(
            cursor, start,
            static_cast<int>(sample::DeclKind::Macro), visibility,
            visibilityAncestors, visibilityDescendants,
            static_cast<int>(sample::FunctionKind::Standard), name, "", "",
            false, false, false, false, false, false, false, nullptr, nullptr,
            nullptr);
        parseParameterList(cursor, declaration);
        if (cursor.atPunc('{')) {
            Span blockSpan;
            declaration->body = parseBlock(cursor, blockSpan);
        }
        declaration->span = rangeSpan(start, cursor.curSpan());
        return declaration;
    }

    if (word == "implement" || word == "impl") {
        cursor.advance();
        std::string_view name;
        if (cursor.isIdentifier(cursor.cur())) {
            name = cursor.text();
            cursor.advance();
        }
        Declaration *declaration = make<Declaration>(
            cursor, start,
            static_cast<int>(sample::DeclKind::TypeAlias), visibility,
            visibilityAncestors, visibilityDescendants,
            static_cast<int>(sample::FunctionKind::Standard), "", name, "",
            false, false, false, false, false, false, false, nullptr, nullptr,
            nullptr);
        parseTypeBody(cursor, declaration, start);
        declaration->span = rangeSpan(start, cursor.curSpan());
        return declaration;
    }

    if (word == "@") {
        // Top-level macro invocation: tolerate it and skip to the next boundary.
        while (cursor.has() && !cursor.atPunc(';'))
            cursor.advance();
        if (cursor.atPunc(';'))
            cursor.advance();
        return nullptr;
    }

    coalesceGarbage(cursor, start);
    return nullptr;
}

[[nodiscard]] int parseVisibilityBound(std::string_view text) {
    if (text == "0" || text == "=")
        return 0;
    if (text.empty())
        return -1;
    char *end = nullptr;
    const long value = std::strtol(std::string(text).c_str(), &end, 10);
    if (end == nullptr || end != std::string(text).c_str() + text.size() ||
        value < 0 || value > 1024)
        return -2;
    return static_cast<int>(value);
}

bool parseVisibilityArgs(Cursor &cursor, int &visibility, int &ancestors,
                         int &descendants) {
    const Span start = cursor.cur().span;
    cursor.advance();
    if (!cursor.atPunc('(')) {
        visibility = static_cast<int>(sample::VisibilityKind::Public);
        ancestors = -1;
        descendants = -1;
        return true;
    }

    cursor.advance();
    const Span innerStart = cursor.curSpan();
    while (cursor.has() && cursor.cur().kind != TokenKind::End &&
           !cursor.atPunc(')'))
        cursor.advance();

    if (!cursor.atPunc(')')) {
        cursor.diagnose(start, "expected ')' after visibility range");
        return false;
    }
    const Span innerEnd = cursor.cur().span;
    cursor.advance();

    std::string_view raw =
        cursor.slice(Span{innerStart.start, innerEnd.start});
    while (!raw.empty() &&
           (raw.front() == ' ' || raw.front() == '\t' ||
            raw.front() == '\n' || raw.front() == '\r'))
        raw.remove_prefix(1);
    while (!raw.empty() &&
           (raw.back() == ' ' || raw.back() == '\t' ||
            raw.back() == '\n' || raw.back() == '\r'))
        raw.remove_suffix(1);

    visibility = static_cast<int>(sample::VisibilityKind::Module);
    ancestors = -1;
    descendants = -1;

    if (raw == "..") {
        visibility = static_cast<int>(sample::VisibilityKind::Public);
        return true;
    }
    if (raw == "siblings" || raw == "neighbors") {
        ancestors = 0;
        descendants = 0;
        return true;
    }
    if (raw == "0..") {
        ancestors = 0;
        descendants = -1;
        return true;
    }
    if (raw == "0..=" || raw == "0..0" || raw == "=..") {
        ancestors = 0;
        descendants = 0;
        return true;
    }
    if (raw == "=..0") {
        ancestors = 1;
        descendants = 0;
        return true;
    }
    if (raw == "=..=") {
        ancestors = 0;
        descendants = 1;
        return true;
    }

    const std::size_t dots = raw.find("..");
    if (dots == std::string_view::npos ||
        raw.find("..", dots + 2) != std::string_view::npos) {
        cursor.diagnose(start, "malformed visibility range '" +
                                   std::string(raw) + "'");
        return false;
    }

    const std::string_view left = raw.substr(0, dots);
    const std::string_view right = raw.substr(dots + 2);
    ancestors = parseVisibilityBound(left);
    descendants = parseVisibilityBound(right);
    if (ancestors < 0 || descendants < 0) {
        cursor.diagnose(start, "malformed visibility range '" +
                                   std::string(raw) + "'");
        return false;
    }
    return true;
}

} // namespace

namespace hooks::parser {

[[nodiscard]] sample::ParseOutput parseSource(
    generated_parser::Parser<sample::ParseOutput> &parser,
    generated_lexer::TokenStream &tokens,
    std::string_view source,
    std::vector<sample::ParserDiagnostic> *outDiagnostics) {
    parser.reset(tokens, source);
    sample::ParseOutput output(source);
    output.ast.root =
        generated_ast::make<Program>(output.ast,
                                     Span{0, static_cast<uint32_t>(source.size())});

    Cursor cursor{parser.tokenStream(), source, output};
    int visibility = static_cast<int>(sample::VisibilityKind::Private);
    int visibilityAncestors = -1;
    int visibilityDescendants = -1;
    bool externPending = false;
    Span garbageStart{0, 0};
    bool garbageActive = false;

    while (cursor.has() && cursor.cur().kind != TokenKind::End) {
        if (cursor.atLexeme("pub")) {
            if (garbageActive)
                coalesceGarbage(cursor, garbageStart);
            garbageActive = false;
            if (!parseVisibilityArgs(cursor, visibility, visibilityAncestors,
                                     visibilityDescendants))
                cursor.diagnose(cursor.cur().span,
                                "malformed visibility declaration");
            continue;
        }
        if (cursor.atLexeme("priv")) {
            cursor.diagnose(cursor.cur().span,
                            "private is the default; 'priv' keyword is not supported");
            cursor.advance();
            continue;
        }
        if (cursor.atLexeme("mod")) {
            if (garbageActive)
                coalesceGarbage(cursor, garbageStart);
            garbageActive = false;
            visibility = static_cast<int>(sample::VisibilityKind::Module);
            visibilityAncestors = -1;
            visibilityDescendants = -1;
            cursor.advance();
            continue;
        }
        if (cursor.atLexeme("extern") && !cursor.peekAtLexeme("fn")) {
            if (garbageActive)
                coalesceGarbage(cursor, garbageStart);
            garbageActive = false;
            externPending = true;
            cursor.advance();
            continue;
        }
        if (cursor.atPunc(';')) {
            const bool hadGarbage = garbageActive;
            if (garbageActive)
                coalesceGarbage(cursor, garbageStart);
            garbageActive = false;
            if (!hadGarbage)
                cursor.advance();
            continue;
        }

        const Span declStart = cursor.cur().span;
        if (startsDeclaration(cursor.text())) {
            if (garbageActive)
                coalesceGarbage(cursor, garbageStart);
            garbageActive = false;
            if (cursor.cur().kind == TokenKind::End)
                continue;
            Node declaration = parseDeclaration(cursor, visibility,
                                                visibilityAncestors,
                                                visibilityDescendants,
                                                externPending);
            if (declaration != nullptr)
                output.ast.root->body.push(declaration);
            visibility = static_cast<int>(sample::VisibilityKind::Private);
            visibilityAncestors = -1;
            visibilityDescendants = -1;
            externPending = false;
            continue;
        }

        if (!garbageActive) {
            garbageStart = cursor.cur().span;
            garbageActive = true;
        }
        cursor.advance();
    }
    if (garbageActive)
        coalesceGarbage(cursor, garbageStart);
    output.ast.root->span =
        Span{0, static_cast<uint32_t>(source.size())};
    if (outDiagnostics != nullptr) {
        outDiagnostics->clear();
        outDiagnostics->assign(output.diagnostics.begin(), output.diagnostics.end());
    }
    return output;
}

} // namespace hooks::parser
