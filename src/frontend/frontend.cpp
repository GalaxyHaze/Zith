#include "frontend/frontend.hpp"

#include <cctype>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>

namespace zith::frontend {

namespace {

[[nodiscard]] bool isIdentifierStart(char character) {
    const auto value = static_cast<unsigned char>(character);
    return std::isalpha(value) != 0 || character == '_';
}

[[nodiscard]] bool isIdentifierContinue(char character) {
    const auto value = static_cast<unsigned char>(character);
    return std::isalnum(value) != 0 || character == '_';
}

[[nodiscard]] bool isWhitespace(char character) {
    const auto value = static_cast<unsigned char>(character);
    return std::isspace(value) != 0;
}

[[nodiscard]] bool isOperator(char character) {
    constexpr std::string_view operators = "+-*/%=<>!&|^~?";
    return operators.find(character) != std::string_view::npos;
}

[[nodiscard]] bool isPunctuation(char character) {
    constexpr std::string_view punctuation = "()[]{}:;,.#@`";
    return punctuation.find(character) != std::string_view::npos;
}

[[nodiscard]] bool isKeyword(const std::string_view word) {
    constexpr std::string_view keywords[] = {
        "i8",      "i16",    "i32",    "i64",     "i128",      "u8",      "u16",       "u32",
        "u64",     "u128",   "f32",    "f64",     "bool",      "char",    "void",      "invalid",
        "unknown", "null",   "true",   "false",   "type",      "struct",  "component", "enum",
        "raw",     "unsafe", "union",  "trait",   "interface", "extends", "dyn",       "implement",
        "fn",      "import", "use",    "context", "macro",     "export",  "extern",    "from",
        "alias",   "as",     "let",    "var",     "auto",      "const",   "mut",       "global",
        "lend",    "share",  "view",   "unique",  "belong",    "yield",   "async",     "flow",
        "dock",    "never",  "pub",    "mod",     "if",        "else",    "for",       "in",
        "when",    "match",  "return", "break",   "continue",  "jump",    "while",     "marker",
        "spawn",   "await",  "with",   "catch",   "must",      "throw",   "fail",      "drop",
        "require", "is",     "prefix", "suffix",  "infix",     "nop",     "and",       "or",
        "not",     "xor",
    };
    for (const auto keyword : keywords)
        if (word == keyword)
            return true;
    return false;
}

} // namespace

void lex(FrontendSnapshot &snapshot) {
    const std::string_view source = snapshot.source_;
    uint32_t position             = 0;
    uint32_t eofTriviaStart       = 0;

    while (position < source.size()) {
        const uint32_t triviaStart = static_cast<uint32_t>(snapshot.trivia_.size());
        eofTriviaStart             = triviaStart;
        while (position < source.size()) {
            const uint32_t start = position;
            if (isWhitespace(source[position])) {
                do {
                    ++position;
                } while (position < source.size() && isWhitespace(source[position]));
                snapshot.trivia_.push_back(
                    Trivia{TriviaKind::Whitespace, TextSpan{start, position}});
                continue;
            }
            if (source.substr(position).starts_with("///")) {
                position += 3;
                while (position < source.size() && source[position] != '\n') {
                    ++position;
                }
                snapshot.trivia_.push_back(Trivia{TriviaKind::DocLine, TextSpan{start, position}});
                continue;
            }
            if (source.substr(position).starts_with("//")) {
                position += 2;
                while (position < source.size() && source[position] != '\n') {
                    ++position;
                }
                snapshot.trivia_.push_back(
                    Trivia{TriviaKind::LineComment, TextSpan{start, position}});
                continue;
            }
            if (source.substr(position).starts_with("/**") ||
                source.substr(position).starts_with("/*")) {
                const bool documentation = source.substr(position).starts_with("/**");
                position += documentation ? 3 : 2;
                const auto close = source.find("*/", position);
                if (close == std::string_view::npos) {
                    position = static_cast<uint32_t>(source.size());
                    snapshot.diagnostics_.push_back(
                        Diagnostic{TextSpan{start, position}, "unterminated block comment"});
                } else {
                    position = static_cast<uint32_t>(close + 2);
                }
                snapshot.trivia_.push_back(
                    Trivia{documentation ? TriviaKind::DocBlock : TriviaKind::BlockComment,
                           TextSpan{start, position}});
                continue;
            }
            break;
        }

        if (position == source.size()) {
            break;
        }

        const uint32_t start = position;
        if (isIdentifierStart(source[position])) {
            do {
                ++position;
            } while (position < source.size() && isIdentifierContinue(source[position]));
            const std::string_view word = source.substr(start, position - start);
            snapshot.tokens_.push_back(
                Token{isKeyword(word) ? TokenKind::Keyword : TokenKind::Identifier,
                      TextSpan{start, position}, triviaStart,
                      static_cast<uint32_t>(snapshot.trivia_.size()) - triviaStart});
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(source[position])) != 0) {
            do {
                ++position;
            } while (position < source.size() &&
                     (std::isalnum(static_cast<unsigned char>(source[position])) != 0 ||
                      source[position] == '.'));
            snapshot.tokens_.push_back(
                Token{TokenKind::Literal, TextSpan{start, position}, triviaStart,
                      static_cast<uint32_t>(snapshot.trivia_.size()) - triviaStart});
            continue;
        }

        if (source[position] == '"' || source[position] == '\'') {
            const char quote = source[position++];
            bool closed      = false;
            while (position < source.size()) {
                if (source[position] == '\\' && position + 1 < source.size()) {
                    position += 2;
                    continue;
                }
                if (source[position++] == quote) {
                    closed = true;
                    break;
                }
            }
            if (!closed) {
                snapshot.diagnostics_.push_back(
                    Diagnostic{TextSpan{start, position}, "unterminated string literal"});
            }
            snapshot.tokens_.push_back(
                Token{TokenKind::Literal, TextSpan{start, position}, triviaStart,
                      static_cast<uint32_t>(snapshot.trivia_.size()) - triviaStart});
            continue;
        }

        ++position;
        if (isOperator(source[start])) {
            snapshot.tokens_.push_back(
                Token{TokenKind::Operator, TextSpan{start, position}, triviaStart,
                      static_cast<uint32_t>(snapshot.trivia_.size()) - triviaStart});
        } else if (isPunctuation(source[start])) {
            snapshot.tokens_.push_back(
                Token{TokenKind::Punctuation, TextSpan{start, position}, triviaStart,
                      static_cast<uint32_t>(snapshot.trivia_.size()) - triviaStart});
        } else {
            snapshot.diagnostics_.push_back(Diagnostic{TextSpan{start, position}, "unknown token"});
            snapshot.tokens_.push_back(
                Token{TokenKind::Unknown, TextSpan{start, position}, triviaStart,
                      static_cast<uint32_t>(snapshot.trivia_.size()) - triviaStart});
        }
    }

    snapshot.tokens_.push_back(Token{
        TokenKind::End,
        TextSpan{static_cast<uint32_t>(source.size()), static_cast<uint32_t>(source.size())},
        eofTriviaStart,
        static_cast<uint32_t>(snapshot.trivia_.size()) - eofTriviaStart,
    });
}

namespace {

[[nodiscard]] const GreenNode *makeNode(memory::Arena &arena, SyntaxKind kind, TextSpan span,
                                        const std::vector<GreenElement> &children) {
    GreenElement *elements = nullptr;
    if (!children.empty()) {
        if (children.size() > std::numeric_limits<size_t>::max() / sizeof(GreenElement)) {
            return nullptr;
        }
        elements = static_cast<GreenElement *>(
            arena.alloc(sizeof(GreenElement) * children.size(), alignof(GreenElement)));
        if (elements == nullptr) {
            return nullptr;
        }
        std::memcpy(elements, children.data(), sizeof(GreenElement) * children.size());
    }
    return arena.make<GreenNode>(
        GreenNode{kind, span, elements, static_cast<uint32_t>(children.size())});
}

} // namespace

void parseCst(FrontendSnapshot &snapshot) {
    std::vector<GreenElement> children;
    std::vector<TokenId> delimiters;
    children.reserve(snapshot.tokens_.size());

    for (uint32_t index = 0; index + 1 < snapshot.tokens_.size(); ++index) {
        const TokenId token{index + 1};
        const Token &current = snapshot.tokens_[index];
        if (current.kind == TokenKind::Punctuation) {
            const char punctuation = snapshot.source_[current.span.start];
            if (punctuation == '(' || punctuation == '[' || punctuation == '{') {
                delimiters.push_back(token);
            } else if (punctuation == ')' || punctuation == ']' || punctuation == '}') {
                bool matched = false;
                if (!delimiters.empty()) {
                    const TokenId opening         = delimiters.back();
                    const Token &openingToken     = snapshot.tokens_[opening.value - 1];
                    const char openingPunctuation = snapshot.source_[openingToken.span.start];
                    matched = (openingPunctuation == '(' && punctuation == ')') ||
                              (openingPunctuation == '[' && punctuation == ']') ||
                              (openingPunctuation == '{' && punctuation == '}');
                }
                if (!matched) {
                    snapshot.diagnostics_.push_back(
                        Diagnostic{current.span, "unmatched closing delimiter"});
                    const std::vector<GreenElement> errorChildren{{nullptr, token}};
                    const GreenNode *error =
                        makeNode(snapshot.arena_, SyntaxKind::Error, current.span, errorChildren);
                    children.push_back(GreenElement{error, {}});
                    continue;
                }
                delimiters.pop_back();
            }
        }
        children.push_back(GreenElement{nullptr, token});
    }

    for (const TokenId token : delimiters) {
        const Token &current = snapshot.tokens_[token.value - 1];
        snapshot.diagnostics_.push_back(Diagnostic{current.span, "unclosed delimiter"});
        const std::vector<GreenElement> errorChildren{{nullptr, token}};
        const GreenNode *error =
            makeNode(snapshot.arena_, SyntaxKind::Error, current.span, errorChildren);
        children.push_back(GreenElement{error, {}});
    }

    snapshot.root_ =
        makeNode(snapshot.arena_, SyntaxKind::Root,
                 TextSpan{0, static_cast<uint32_t>(snapshot.source_.size())}, children);
}

namespace {

[[nodiscard]] std::string_view tokenText(const FrontendSnapshot &snapshot, uint32_t index) {
    const auto span = snapshot.tokens()[index].span;
    return std::string_view(snapshot.source()).substr(span.start, span.size());
}

[[nodiscard]] bool isPunctuation(const FrontendSnapshot &snapshot, uint32_t index, char character) {
    return snapshot.tokens()[index].kind == TokenKind::Punctuation &&
           tokenText(snapshot, index) == std::string_view(&character, 1);
}

[[nodiscard]] std::optional<DeclKind> declarationKind(const std::string_view word) {
    if (word == "fn")
        return DeclKind::Function;
    if (word == "type")
        return DeclKind::TypeAlias;
    if (word == "struct" || word == "component")
        return DeclKind::Struct;
    if (word == "enum")
        return DeclKind::Enum;
    if (word == "union")
        return DeclKind::Union;
    if (word == "trait")
        return DeclKind::Trait;
    if (word == "interface")
        return DeclKind::Interface;
    if (word == "let" || word == "var" || word == "const" || word == "global")
        return DeclKind::Variable;
    if (word == "context")
        return DeclKind::Context;
    if (word == "prefix" || word == "suffix" || word == "infix" || word == "nop")
        return DeclKind::Word;
    return std::nullopt;
}

} // namespace

class AstLowerer {
public:
    explicit AstLowerer(FrontendSnapshot &snapshot)
        : snapshot_(snapshot), token_count_(static_cast<uint32_t>(snapshot.tokens_.size() - 1U)) {}

    void run() {
        current_scope_ = addScope({}, {0, static_cast<uint32_t>(snapshot_.source_.size())});
        Visibility visibility = Visibility::Private;
        while (index_ < token_count_) {
            const uint32_t start = index_;
            const auto word      = text(index_);
            if (word == "pub") {
                visibility = Visibility::Public;
                ++index_;
                continue;
            }
            if (word == "mod") {
                visibility = Visibility::Module;
                ++index_;
                continue;
            }

            if (word == "export" || word == "from" || word == "import") {
                lowerImport(start, visibility);
                visibility = Visibility::Private;
                continue;
            }

            const auto kind = declarationKind(word);
            if (!kind) {
                ++index_;
                continue;
            }
            lowerDeclaration(start, *kind, visibility);
            visibility = Visibility::Private;
        }
    }

private:
    FrontendSnapshot &snapshot_;
    uint32_t token_count_;
    uint32_t index_ = 0;
    uint32_t next_scope_ = 1;
    ScopeId current_scope_;

    [[nodiscard]] std::string_view text(const uint32_t index) const {
        return tokenText(snapshot_, index);
    }

    [[nodiscard]] bool punctuation(const uint32_t index, const char character) const {
        return index < token_count_ && isPunctuation(snapshot_, index, character);
    }

    [[nodiscard]] TextSpan tokenSpan(const uint32_t index) const {
        return snapshot_.tokens_[index].span;
    }

    [[nodiscard]] TextSpan range(const uint32_t start, const uint32_t end) const {
        if (start >= token_count_)
            return {};
        return {tokenSpan(start).start, end > start ? tokenSpan(end - 1U).end : tokenSpan(start).end};
    }

    [[nodiscard]] ExprId addExpression(Expression expression) {
        expression.id = ExprId{static_cast<uint32_t>(snapshot_.expressions_.size() + 1U)};
        snapshot_.expressions_.push_back(std::move(expression));
        return snapshot_.expressions_.back().id;
    }

    [[nodiscard]] StmtId addStatement(Statement statement) {
        statement.id = StmtId{static_cast<uint32_t>(snapshot_.statements_.size() + 1U)};
        snapshot_.statements_.push_back(std::move(statement));
        return snapshot_.statements_.back().id;
    }

    [[nodiscard]] TypeExprId addType(TypeExpression type) {
        type.id = TypeExprId{static_cast<uint32_t>(snapshot_.type_expressions_.size() + 1U)};
        snapshot_.type_expressions_.push_back(std::move(type));
        return snapshot_.type_expressions_.back().id;
    }

    [[nodiscard]] ScopeId addScope(const ScopeId parent, const TextSpan span) {
        const ScopeId id{next_scope_++};
        snapshot_.scopes_.push_back({id, parent, span});
        return id;
    }

    [[nodiscard]] TypeExprId parseType() {
        if (index_ >= token_count_)
            return {};
        const uint32_t start = index_;
        TypeExpression type;
        type.kind = TypeExprKind::Error;
        if (punctuation(index_, '?')) {
            ++index_;
            type.kind = TypeExprKind::Optional;
            type.arguments.push_back(parseType());
        } else if (punctuation(index_, '*')) {
            ++index_;
            type.kind = TypeExprKind::Pointer;
            type.arguments.push_back(parseType());
        } else if (snapshot_.tokens_[index_].kind == TokenKind::Identifier ||
                   snapshot_.tokens_[index_].kind == TokenKind::Keyword) {
            type.kind = TypeExprKind::Name;
            type.name = std::string(text(index_++));
        } else {
            snapshot_.diagnostics_.push_back({tokenSpan(index_), "expected a type"});
            ++index_;
        }
        type.span = range(start, index_);
        return addType(std::move(type));
    }

    [[nodiscard]] ExprId parsePrimary() {
        if (index_ >= token_count_)
            return {};

        const uint32_t start = index_;
        if (punctuation(index_, '{'))
            return parseBlock();

        Expression expression;
        expression.scope = current_scope_;
        if (text(index_) == "if")
            return parseIf();
        if (text(index_) == "while")
            return parseWhile();
        if (punctuation(index_, '(')) {
            ++index_;
            auto nested = parseExpression();
            if (!punctuation(index_, ')'))
                snapshot_.diagnostics_.push_back({range(start, index_), "expected ')'"});
            else
                ++index_;
            return nested;
        }

        const auto kind = snapshot_.tokens_[index_].kind;
        if (kind == TokenKind::Identifier || kind == TokenKind::Keyword) {
            expression.kind = ExprKind::Name;
            expression.text = std::string(text(index_++));
        } else if (kind == TokenKind::Literal) {
            expression.kind = ExprKind::Literal;
            expression.text = std::string(text(index_++));
        } else {
            expression.kind = ExprKind::Error;
            expression.text = std::string(text(index_++));
            snapshot_.diagnostics_.push_back({range(start, index_), "expected an expression"});
        }
        expression.span = range(start, index_);
        auto result      = addExpression(std::move(expression));

        while (punctuation(index_, '(')) {
            const uint32_t call_start = start;
            ++index_;
            Expression call;
            call.kind  = ExprKind::Call;
            call.scope = current_scope_;
            call.operands.push_back(result);
            while (index_ < token_count_ && !punctuation(index_, ')')) {
                call.operands.push_back(parseExpression());
                if (!punctuation(index_, ','))
                    break;
                ++index_;
            }
            if (!punctuation(index_, ')'))
                snapshot_.diagnostics_.push_back({range(call_start, index_), "expected ')'"});
            else
                ++index_;
            call.span = range(call_start, index_);
            result    = addExpression(std::move(call));
        }
        return result;
    }

    [[nodiscard]] static int precedence(const std::string_view op) {
        if (op == "=")
            return 1;
        if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=")
            return 2;
        if (op == "+" || op == "-")
            return 3;
        if (op == "*" || op == "/" || op == "%")
            return 4;
        return -1;
    }

    [[nodiscard]] ExprId parseExpression(const int minimum_precedence = 0) {
        if (index_ >= token_count_)
            return {};

        const uint32_t start = index_;
        ExprId left;
        if (snapshot_.tokens_[index_].kind == TokenKind::Operator &&
            (text(index_) == "-" || text(index_) == "!" || text(index_) == "not")) {
            const auto op = std::string(text(index_++));
            Expression unary;
            unary.kind  = ExprKind::Unary;
            unary.text  = op;
            unary.scope = current_scope_;
            unary.operands.push_back(parseExpression(5));
            unary.span = range(start, index_);
            left       = addExpression(std::move(unary));
        } else {
            left = parsePrimary();
        }

        while (index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Operator) {
            const auto op         = text(index_);
            const int op_priority = precedence(op);
            if (op_priority < minimum_precedence)
                break;
            ++index_;
            const auto right = parseExpression(op_priority + 1);
            Expression binary;
            binary.kind  = op == "=" ? ExprKind::Assign : ExprKind::Binary;
            binary.text  = std::string(op);
            binary.scope = current_scope_;
            binary.operands.push_back(left);
            binary.operands.push_back(right);
            binary.span = range(start, index_);
            left        = addExpression(std::move(binary));
        }
        return left;
    }

    [[nodiscard]] ExprId parseBlock() {
        const uint32_t start = index_;
        Expression block;
        block.kind  = ExprKind::Block;
        const ScopeId parent = current_scope_;
        if (punctuation(index_, '{'))
            ++index_;
        current_scope_ = addScope(parent, tokenSpan(start));
        block.scope    = current_scope_;
        while (index_ < token_count_ && !punctuation(index_, '}'))
            block.statements.push_back(parseStatement());
        if (!punctuation(index_, '}'))
            snapshot_.diagnostics_.push_back({range(start, index_), "expected '}'"});
        else
            ++index_;
        block.span = range(start, index_);
        snapshot_.scopes_.back().span = block.span;
        current_scope_                = parent;
        return addExpression(std::move(block));
    }

    [[nodiscard]] ExprId parseIf() {
        const uint32_t start = index_++;
        if (punctuation(index_, '('))
            ++index_;
        const ExprId condition = parseExpression();
        if (punctuation(index_, ')'))
            ++index_;
        else if (!punctuation(index_, '{'))
            snapshot_.diagnostics_.push_back({range(start, index_), "expected ')' after condition"});

        Expression expression;
        expression.kind  = ExprKind::If;
        expression.scope = current_scope_;
        expression.operands.push_back(condition);
        if (punctuation(index_, '{'))
            expression.operands.push_back(parseBlock());
        else
            snapshot_.diagnostics_.push_back({range(start, index_), "expected if body"});
        if (index_ < token_count_ && text(index_) == "else") {
            ++index_;
            if (punctuation(index_, '{'))
                expression.operands.push_back(parseBlock());
            else if (index_ < token_count_ && text(index_) == "if")
                expression.operands.push_back(parseIf());
            else
                snapshot_.diagnostics_.push_back({range(start, index_), "expected else body"});
        }
        expression.span = range(start, index_);
        return addExpression(std::move(expression));
    }

    [[nodiscard]] ExprId parseWhile() {
        const uint32_t start = index_++;
        if (punctuation(index_, '('))
            ++index_;
        const ExprId condition = parseExpression();
        if (punctuation(index_, ')'))
            ++index_;
        else if (!punctuation(index_, '{'))
            snapshot_.diagnostics_.push_back({range(start, index_), "expected ')' after condition"});

        Expression expression;
        expression.kind  = ExprKind::While;
        expression.scope = current_scope_;
        expression.operands.push_back(condition);
        if (punctuation(index_, '{'))
            expression.operands.push_back(parseBlock());
        else
            snapshot_.diagnostics_.push_back({range(start, index_), "expected while body"});
        expression.span = range(start, index_);
        return addExpression(std::move(expression));
    }

    [[nodiscard]] StmtId parseStatement() {
        const uint32_t start = index_;
        Statement statement;
        statement.kind = StmtKind::Expression;
        if (index_ >= token_count_)
            return addStatement(std::move(statement));

        const auto word = text(index_);
        if (word == "let" || word == "var" || word == "const") {
            statement.kind                    = StmtKind::Binding;
            statement.binding.mutableBinding  = word == "var";
            ++index_;
            if (index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Identifier) {
                statement.binding.id   = LocalId{statementCountLocals_++};
                statement.binding.name = std::string(text(index_));
                statement.binding.span = tokenSpan(index_++);
            } else {
                snapshot_.diagnostics_.push_back({range(start, index_), "expected a binding name"});
            }
            if (punctuation(index_, ':')) {
                ++index_;
                statement.binding.type = parseType();
            }
            if (index_ < token_count_ && text(index_) == "=") {
                ++index_;
                statement.binding.initializer = parseExpression();
            }
        } else if (word == "return") {
            statement.kind = StmtKind::Return;
            ++index_;
            if (index_ < token_count_ && !punctuation(index_, ';') && !punctuation(index_, '}'))
                statement.expression = parseExpression();
        } else if (word == "break") {
            statement.kind = StmtKind::Break;
            ++index_;
        } else if (word == "continue") {
            statement.kind = StmtKind::Continue;
            ++index_;
        } else {
            statement.expression = parseExpression();
        }
        if (punctuation(index_, ';'))
            ++index_;
        statement.span = range(start, index_);
        return addStatement(std::move(statement));
    }

    void lowerImport(const uint32_t start, const Visibility visibility) {
        Declaration declaration;
        declaration.id                 = DeclId{static_cast<uint32_t>(snapshot_.declarations_.size() + 1U)};
        declaration.kind               = DeclKind::Import;
        declaration.visibility         = visibility;
        declaration.import.isExport    = text(index_) == "export";
        declaration.import.isFrom      = text(index_) == "from";
        ++index_;
        if (index_ < token_count_ && text(index_) == "asset") {
            declaration.import.isAsset = true;
            ++index_;
        }
        while (index_ < token_count_) {
            const auto segment = text(index_);
            if (snapshot_.tokens_[index_].kind == TokenKind::Identifier) {
                if (segment == "as" || segment == "use")
                    break;
                declaration.import.path.emplace_back(segment);
                ++index_;
                continue;
            }
            if (punctuation(index_, '.') || punctuation(index_, '/')) {
                ++index_;
                continue;
            }
            break;
        }
        declaration.span = range(start, index_);
        if (declaration.import.path.empty()) {
            snapshot_.diagnostics_.push_back({declaration.span, "expected an import path"});
            declaration.kind = DeclKind::Error;
        }
        snapshot_.declarations_.push_back(std::move(declaration));
    }

    void lowerDeclaration(const uint32_t start, const DeclKind kind, const Visibility visibility) {
        Declaration declaration;
        declaration.id         = DeclId{static_cast<uint32_t>(snapshot_.declarations_.size() + 1U)};
        declaration.kind       = kind;
        declaration.visibility = visibility;
        ++index_;
        if (index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Identifier) {
            declaration.name = std::string(text(index_++));
        } else {
            snapshot_.diagnostics_.push_back({tokenSpan(start), "expected a declaration name"});
            declaration.kind = DeclKind::Error;
        }

        if (kind == DeclKind::Function && punctuation(index_, '(')) {
            ++index_;
            while (index_ < token_count_ && !punctuation(index_, ')')) {
                Parameter parameter;
                if (snapshot_.tokens_[index_].kind == TokenKind::Identifier) {
                    parameter.id   = LocalId{statementCountLocals_++};
                    parameter.name = std::string(text(index_));
                    parameter.span = tokenSpan(index_++);
                    if (punctuation(index_, ':')) {
                        ++index_;
                        parameter.type = parseType();
                    }
                    declaration.parameters.push_back(std::move(parameter));
                } else {
                    snapshot_.diagnostics_.push_back({tokenSpan(index_), "expected a parameter name"});
                    ++index_;
                }
                if (punctuation(index_, ','))
                    ++index_;
                else if (!punctuation(index_, ')'))
                    break;
            }
            if (punctuation(index_, ')'))
                ++index_;
            else
                snapshot_.diagnostics_.push_back({range(start, index_), "expected ')'"});
        }
        if (punctuation(index_, ':')) {
            ++index_;
            declaration.declaredType = parseType();
        }
        if (kind == DeclKind::Function && punctuation(index_, '{')) {
            declaration.body = parseBlock();
        } else if (kind == DeclKind::Variable && index_ < token_count_ && text(index_) == "=") {
            ++index_;
            declaration.initializer = parseExpression();
        } else if (punctuation(index_, '{')) {
            skipDelimited('{', '}');
        }
        if (punctuation(index_, ';'))
            ++index_;
        declaration.span = range(start, index_);
        snapshot_.declarations_.push_back(std::move(declaration));
    }

    void skipDelimited(const char open, const char close) {
        if (!punctuation(index_, open))
            return;
        uint32_t depth = 0;
        do {
            if (punctuation(index_, open))
                ++depth;
            else if (punctuation(index_, close))
                --depth;
            ++index_;
        } while (index_ < token_count_ && depth != 0);
    }

    uint32_t statementCountLocals_ = 1;
};

void lowerAst(FrontendSnapshot &snapshot) {
    AstLowerer(snapshot).run();
}

const Token &SyntaxToken::token() const noexcept {
    return (*tokens_)[id_.value - 1];
}

std::string_view SyntaxToken::text() const noexcept {
    const TextSpan span = token().span;
    return std::string_view(*source_).substr(span.start, span.size());
}

const GreenElement &SyntaxNode::child(uint32_t index) const noexcept {
    return green_->children[index];
}

SyntaxToken SyntaxNode::token(uint32_t index) const noexcept {
    return SyntaxToken(*tokens_, *source_, child(index).token);
}

FrontendSnapshot::FrontendSnapshot(std::string source) : source_(std::move(source)) {}

SyntaxNode FrontendSnapshot::root() const noexcept {
    return SyntaxNode(*root_, tokens_, source_);
}

std::string FrontendSnapshot::reconstruct() const {
    std::string result;
    result.reserve(source_.size());
    for (const Token &token : tokens_) {
        for (uint32_t index = 0; index < token.leadingTriviaCount; ++index) {
            const TextSpan span = trivia_[token.leadingTriviaStart + index].span;
            result.append(source_, span.start, span.size());
        }
        if (token.kind != TokenKind::End) {
            result.append(source_, token.span.start, token.span.size());
        }
    }
    return result;
}

FrontendSnapshot parse(std::string source) {
    FrontendSnapshot snapshot(std::move(source));
    lex(snapshot);
    parseCst(snapshot);
    lowerAst(snapshot);
    return snapshot;
}

} // namespace zith::frontend
