#include "frontend/frontend.hpp"
#include "diagnostics/error-codes.hpp"

#include <cctype>
#include <cstdlib>
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
            // A '.' only continues a float when it is not the first half of a
            // `..` range operator: `1.5` is a float, `1..3` is two literals and
            // two dots.
            do {
                ++position;
            } while (position < source.size() &&
                     (std::isalnum(static_cast<unsigned char>(source[position])) != 0 ||
                      (source[position] == '.' &&
                       !(position + 1 < source.size() && source[position + 1] == '.'))));
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
            // Maximal munch over a closed set of two-character operators. Deliberately limited to
            // these seven: tokens the expression parser has no precedence for (`&&`, `+=`, ...)
            // must stay single-character, otherwise the binary loop would stop without consuming
            // them.
            if (position < source.size()) {
                const std::string_view pair = source.substr(start, 2);
                if (pair == "==" || pair == "!=" || pair == "<=" || pair == ">=" || pair == "->" ||
                    pair == "<<" || pair == ">>")
                    ++position;
            }
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

[[nodiscard]] bool matchesToken(const FrontendSnapshot &snapshot, uint32_t index,
                                std::string_view text) {
    return index < snapshot.tokens().size() && tokenText(snapshot, index) == text;
}

[[nodiscard]] std::optional<DeclKind> declarationKind(const std::string_view word) {
    if (word == "fn")
        return DeclKind::Function;
    if (word == "type")
        return DeclKind::TypeAlias;
    if (word == "alias")
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
        current_scope_        = addScope({}, {0, static_cast<uint32_t>(snapshot_.source_.size())});
        Visibility visibility = Visibility::Private;
        // Start index of the current run of unexpected top-level tokens; the run is
        // coalesced into a single diagnostic.  token_count_ means "no active run".
        uint32_t bad_run_start = token_count_;
        while (index_ < token_count_) {
            const uint32_t start = index_;
            const auto word      = text(index_);

            const auto flushBadRun = [&]() {
                if (bad_run_start == token_count_)
                    return;
                snapshot_.diagnostics_.push_back({range(bad_run_start, index_),
                                                  "unexpected token at top level", false,
                                                  diagnostics::err::UnsupportedSyntax});
                bad_run_start = token_count_;
            };

            if (word == "pub") {
                flushBadRun();
                visibility = Visibility::Public;
                ++index_;
                continue;
            }
            if (word == "mod") {
                flushBadRun();
                visibility = Visibility::Module;
                ++index_;
                continue;
            }

            if (word == "export" || word == "from" || word == "import") {
                flushBadRun();
                lowerImport(start, visibility);
                visibility = Visibility::Private;
                continue;
            }

            const auto kind = declarationKind(word);
            if (kind) {
                flushBadRun();
                lowerDeclaration(start, *kind, visibility);
                visibility = Visibility::Private;
                continue;
            }

            // Unimplemented-but-planned top-level constructs are tolerated without a
            // diagnostic to preserve current behavior (`extern fn` still parses, and
            // `use`/`implement`/`macro`/`unsafe`/`raw`/`;` stay silent).
            if (word == "extern" || word == "use" || word == "implement" || word == "macro" ||
                word == "unsafe" || word == "raw" || word == ";") {
                flushBadRun();
                ++index_;
                continue;
            }

            // `@name args;` is a top-level macro invocation (e.g. `@appendField Custom, x: i32;`)
            // and stays tolerated; a bare `@` not followed by an identifier is diagnosed.
            if (word == "@" && index_ + 1 < token_count_ &&
                snapshot_.tokens_[index_ + 1].kind == TokenKind::Identifier) {
                flushBadRun();
                skipMacroInvocation();
                continue;
            }

            // Unexpected top-level token: start (or extend) the coalesced run.
            if (bad_run_start == token_count_)
                bad_run_start = start;
            ++index_;
        }
        if (bad_run_start != token_count_)
            snapshot_.diagnostics_.push_back({range(bad_run_start, index_),
                                              "unexpected token at top level", false,
                                              diagnostics::err::UnsupportedSyntax});
    }

    // Consume a top-level macro invocation starting at the current `@`: the `@`,
    // the macro name, and any arguments up to the terminating top-level `;` (or
    // up to the next token that begins a declaration), whichever comes first.
    void skipMacroInvocation() {
        ++index_; // '@'
        if (index_ < token_count_)
            ++index_; // macro name
        uint32_t depth = 0;
        while (index_ < token_count_) {
            const auto word = text(index_);
            if (depth == 0) {
                if (punctuation(index_, ';'))
                    break;
                if (word == "pub" || word == "mod" || word == "export" || word == "from" ||
                    word == "import" || declarationKind(word))
                    break;
            }
            if (punctuation(index_, '(') || punctuation(index_, '[') || punctuation(index_, '{'))
                ++depth;
            else if (punctuation(index_, ')') || punctuation(index_, ']') ||
                     punctuation(index_, '}')) {
                if (depth > 0)
                    --depth;
            }
            ++index_;
        }
        if (index_ < token_count_ && punctuation(index_, ';'))
            ++index_;
    }

private:
    FrontendSnapshot &snapshot_;
    uint32_t token_count_;
    uint32_t index_      = 0;
    uint32_t next_scope_ = 1;
    ScopeId current_scope_;
    /// When set (while parsing a when-case condition), a '.' followed by another
    /// '.' is left unconsumed by postfix so the caller can form a `lo..hi` range.
    bool range_mode_ = false;

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
        return {tokenSpan(start).start,
                end > start ? tokenSpan(end - 1U).end : tokenSpan(start).end};
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
        if (matchesToken(snapshot_, index_, "?")) {
            ++index_;
            type.kind = TypeExprKind::Optional;
            type.arguments.push_back(parseType());
        } else if (matchesToken(snapshot_, index_, "[")) {
            ++index_;
            if (matchesToken(snapshot_, index_, "]")) {
                ++index_;
                type.kind = TypeExprKind::Slice;
            } else {
                // `[N]T` is a fixed-size array; the length must be an integer literal.
                type.kind = TypeExprKind::Array;
                if (index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Literal) {
                    type.arrayLength =
                        std::strtoull(std::string(text(index_)).c_str(), nullptr, 10);
                    ++index_;
                } else {
                    snapshot_.diagnostics_.push_back(
                        {range(start, index_), "expected an array length"});
                }
                if (matchesToken(snapshot_, index_, "]"))
                    ++index_;
                else
                    snapshot_.diagnostics_.push_back(
                        {range(start, index_), "expected ']' after array length"});
            }
            type.arguments.push_back(parseType());
        } else if (matchesToken(snapshot_, index_, "*")) {
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

        // `[a, b, c]` is an array literal at primary position; `[` after an
        // expression is postfix indexing (handled in parsePostfix).
        if (punctuation(index_, '[')) {
            const uint32_t array_start = index_++;
            Expression array_lit;
            array_lit.kind  = ExprKind::ArrayLiteral;
            array_lit.scope = current_scope_;
            while (index_ < token_count_ && !punctuation(index_, ']')) {
                if (punctuation(index_, ',')) {
                    ++index_;
                    continue;
                }
                array_lit.operands.push_back(parseExpression());
                if (!punctuation(index_, ',') && !punctuation(index_, ']'))
                    break;
                if (punctuation(index_, ','))
                    ++index_;
            }
            if (punctuation(index_, ']'))
                ++index_;
            else
                snapshot_.diagnostics_.push_back(
                    {range(array_start, index_), "expected ']' after array literal elements"});
            array_lit.span = range(array_start, index_);
            return parsePostfix(addExpression(std::move(array_lit)), array_start);
        }

        // `@offsetOf(Type, field)` / `@alignOf(Type)` / `@sizeOf(Type)` are layout
        // intrinsics; any other `@name(...)` is an unimplemented user macro.
        if (punctuation(index_, '@')) {
            ++index_;
            if (index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Identifier) {
                const auto intrinsic = std::string(text(index_++));
                Expression intrinsic_expr;
                intrinsic_expr.scope = current_scope_;
                if (intrinsic == "offsetOf" || intrinsic == "alignOf" || intrinsic == "sizeOf") {
                    intrinsic_expr.kind = ExprKind::LayoutIntrinsic;
                    intrinsic_expr.text = intrinsic;
                    if (punctuation(index_, '('))
                        ++index_;
                    else
                        snapshot_.diagnostics_.push_back(
                            {range(start, index_), "expected '(' after '@" + intrinsic + "'"});
                    intrinsic_expr.cast_type = parseType();
                    if (punctuation(index_, ',')) {
                        ++index_;
                        if (index_ < token_count_ &&
                            (snapshot_.tokens_[index_].kind == TokenKind::Identifier ||
                             snapshot_.tokens_[index_].kind == TokenKind::Keyword)) {
                            intrinsic_expr.field_names.push_back(std::string(text(index_++)));
                        } else {
                            snapshot_.diagnostics_.push_back(
                                {range(start, index_), "expected a field name"});
                        }
                    }
                    if (punctuation(index_, ')'))
                        ++index_;
                    else
                        snapshot_.diagnostics_.push_back(
                            {range(start, index_), "expected ')' after intrinsic arguments"});
                } else {
                    // User macro call: consume the balanced arguments and only warn.
                    intrinsic_expr.kind = ExprKind::Error;
                    if (punctuation(index_, '(')) {
                        uint32_t depth = 0;
                        do {
                            if (punctuation(index_, '('))
                                ++depth;
                            else if (punctuation(index_, ')'))
                                --depth;
                            ++index_;
                        } while (index_ < token_count_ && depth != 0);
                    }
                    snapshot_.diagnostics_.push_back(
                        Diagnostic{range(start, index_), "user macros are not implemented yet",
                                   true, diagnostics::err::NotImplemented});
                }
                intrinsic_expr.span = range(start, index_);
                return parsePostfix(addExpression(std::move(intrinsic_expr)), start);
            }
            Expression error_expr;
            error_expr.kind  = ExprKind::Error;
            error_expr.scope = current_scope_;
            error_expr.span  = range(start, index_);
            snapshot_.diagnostics_.push_back(
                {range(start, index_), "expected an intrinsic or macro name after '@'"});
            return parsePostfix(addExpression(std::move(error_expr)), start);
        }

        Expression expression;
        expression.scope = current_scope_;
        if (text(index_) == "if")
            return parseIf();
        if (text(index_) == "while")
            return parseWhile();
        if (text(index_) == "for")
            return parseFor();
        if (text(index_) == "when" || text(index_) == "match")
            return parseWhen();
        if (punctuation(index_, '(')) {
            ++index_;
            auto nested = parseExpression();
            if (!punctuation(index_, ')'))
                snapshot_.diagnostics_.push_back({range(start, index_), "expected ')'"});
            else
                ++index_;
            return parsePostfix(nested, start);
        }

        const auto kind = snapshot_.tokens_[index_].kind;
        if (kind == TokenKind::Identifier || kind == TokenKind::Keyword) {
            const auto token_text = text(index_);
            const bool is_literal =
                token_text == "true" || token_text == "false" || token_text == "null";
            expression.kind = is_literal ? ExprKind::Literal : ExprKind::Name;
            expression.text = std::string(token_text);
            ++index_;
            // Struct literal: Name { field: expr, ... }
            // Only treat as struct literal when immediately followed by '{' after a Name.
            if (!is_literal && punctuation(index_, '{')) {
                const std::string struct_name = expression.text;
                ++index_; // consume '{'
                Expression struct_lit;
                struct_lit.kind            = ExprKind::StructLiteral;
                struct_lit.scope           = current_scope_;
                struct_lit.text            = struct_name;
                bool saw_named             = false;
                bool saw_positional        = false;
                const auto parseFieldValue = [&]() -> ExprId {
                    // `_` is the placeholder marker; it fills the field's default (or zero).
                    if (index_ < token_count_ && text(index_) == "_" &&
                        snapshot_.tokens_[index_].kind == TokenKind::Identifier) {
                        Expression placeholder;
                        placeholder.kind  = ExprKind::Placeholder;
                        placeholder.text  = "_";
                        placeholder.scope = current_scope_;
                        placeholder.span  = tokenSpan(index_++);
                        return addExpression(std::move(placeholder));
                    }
                    return parseExpression();
                };
                while (index_ < token_count_ && !punctuation(index_, '}')) {
                    if (punctuation(index_, ',')) {
                        ++index_;
                        continue;
                    }
                    // `ident:` starts a named field; anything else is positional.
                    const bool is_named =
                        (snapshot_.tokens_[index_].kind == TokenKind::Identifier ||
                         snapshot_.tokens_[index_].kind == TokenKind::Keyword) &&
                        punctuation(index_ + 1U, ':');
                    if (is_named) {
                        if (saw_positional) {
                            snapshot_.diagnostics_.push_back(
                                {range(start, index_),
                                 "cannot mix positional and named struct literal fields", false,
                                 diagnostics::err::TypeMismatch});
                        }
                        saw_named                    = true;
                        const std::string field_name = std::string(text(index_++));
                        ++index_; // consume ':'
                        struct_lit.field_names.push_back(field_name);
                        struct_lit.operands.push_back(parseFieldValue());
                    } else {
                        if (saw_named) {
                            snapshot_.diagnostics_.push_back(
                                {range(start, index_),
                                 "cannot mix positional and named struct literal fields", false,
                                 diagnostics::err::TypeMismatch});
                        }
                        saw_positional = true;
                        struct_lit.operands.push_back(parseFieldValue());
                    }
                    if (punctuation(index_, ','))
                        ++index_;
                    else if (!punctuation(index_, '}'))
                        break;
                }
                if (punctuation(index_, '}'))
                    ++index_;
                else
                    snapshot_.diagnostics_.push_back(
                        {range(start, index_), "expected '}' after struct literal fields"});
                struct_lit.span = range(start, index_);
                return parsePostfix(addExpression(std::move(struct_lit)), start);
            }
        } else if (kind == TokenKind::Literal) {
            expression.kind = ExprKind::Literal;
            expression.text = std::string(text(index_++));
        } else {
            expression.kind = ExprKind::Error;
            expression.text = std::string(text(index_++));
            snapshot_.diagnostics_.push_back({range(start, index_), "expected an expression"});
        }
        expression.span = range(start, index_);
        auto result     = addExpression(std::move(expression));
        return parsePostfix(result, start);
    }

    [[nodiscard]] bool isOperatorToken(const std::string_view op) const {
        return index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Operator &&
               text(index_) == op;
    }

    [[nodiscard]] bool isKeywordToken(const std::string_view word) const {
        return index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Keyword &&
               text(index_) == word;
    }

    /// Parses the postfix chain (calls, indexing, casts, `?`) applied to `result`.
    [[nodiscard]] ExprId parsePostfix(ExprId result, const uint32_t start) {
        while (punctuation(index_, '(') || punctuation(index_, '[') || punctuation(index_, '.') ||
               isOperatorToken("->") || isKeywordToken("as")) {
            // Dot field access: expr.field
            if (punctuation(index_, '.')) {
                if (range_mode_ && punctuation(index_ + 1U, '.'))
                    break; // leave `lo..hi` for the when-case range pattern
                ++index_;
                if (index_ < token_count_ &&
                    (snapshot_.tokens_[index_].kind == TokenKind::Identifier ||
                     snapshot_.tokens_[index_].kind == TokenKind::Keyword)) {
                    Expression field_expr;
                    field_expr.kind  = ExprKind::Field;
                    field_expr.scope = current_scope_;
                    field_expr.text  = std::string(text(index_++));
                    field_expr.operands.push_back(result);
                    field_expr.span = range(start, index_);
                    result          = addExpression(std::move(field_expr));
                } else {
                    snapshot_.diagnostics_.push_back(
                        {range(start, index_), "expected field name after '.'"});
                }
                continue;
            }
            // Cast: expr as Type
            if (isKeywordToken("as")) {
                ++index_;
                Expression cast_expr;
                cast_expr.kind      = ExprKind::Cast;
                cast_expr.scope     = current_scope_;
                cast_expr.cast_type = parseType();
                cast_expr.operands.push_back(result);
                cast_expr.span = range(start, index_);
                result         = addExpression(std::move(cast_expr));
                continue;
            }
            // Arrow access: expr->field (sugar for (*expr).field)
            if (isOperatorToken("->")) {
                ++index_;
                if (index_ < token_count_ &&
                    (snapshot_.tokens_[index_].kind == TokenKind::Identifier ||
                     snapshot_.tokens_[index_].kind == TokenKind::Keyword)) {
                    Expression arrow_expr;
                    arrow_expr.kind  = ExprKind::Arrow;
                    arrow_expr.scope = current_scope_;
                    arrow_expr.text  = std::string(text(index_++));
                    arrow_expr.operands.push_back(result);
                    arrow_expr.span = range(start, index_);
                    result          = addExpression(std::move(arrow_expr));
                } else {
                    snapshot_.diagnostics_.push_back(
                        {range(start, index_), "expected field name after '->'"});
                }
                continue;
            }
            if (punctuation(index_, '[')) {
                const uint32_t index_start = start;
                ++index_;
                Expression indexing;
                indexing.kind  = ExprKind::Index;
                indexing.scope = current_scope_;
                indexing.operands.push_back(result);
                indexing.operands.push_back(parseExpression());
                if (!punctuation(index_, ']'))
                    snapshot_.diagnostics_.push_back(
                        {range(index_start, index_), "expected ']' after index"});
                else
                    ++index_;
                indexing.span = range(index_start, index_);
                result        = addExpression(std::move(indexing));
                continue;
            }

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
        // Postfix '?' for optional propagation
        if (index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Operator &&
            text(index_) == "?") {
            ++index_;
            Expression prop;
            prop.kind  = ExprKind::OptionalProp;
            prop.scope = current_scope_;
            prop.operands.push_back(result);
            prop.span = range(start, index_);
            result    = addExpression(std::move(prop));
        }
        // Postfix '!' for failable propagation; not implemented yet.
        if (index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Operator &&
            text(index_) == "!") {
            ++index_;
            snapshot_.diagnostics_.push_back(
                {range(start, index_), "failable propagation is not supported in this version",
                 false, diagnostics::err::UnsupportedSyntax});
            Expression error_expr;
            error_expr.kind  = ExprKind::Error;
            error_expr.scope = current_scope_;
            error_expr.span  = range(start, index_);
            result           = addExpression(std::move(error_expr));
        }
        return result;
    }

    [[nodiscard]] static int precedence(const std::string_view op) {
        if (op == "=")
            return 1;
        if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=")
            return 2;
        if (op == "<<" || op == ">>")
            return 3;
        if (op == "+" || op == "-")
            return 4;
        if (op == "*" || op == "/" || op == "%")
            return 5;
        return -1;
    }

    [[nodiscard]] ExprId parseExpression(const int minimum_precedence = 0) {
        if (index_ >= token_count_)
            return {};

        const uint32_t start = index_;
        ExprId left;
        // Prefix `?` (fallback) and `!` (failable propagation) are not implemented yet.
        if (snapshot_.tokens_[index_].kind == TokenKind::Operator &&
            (text(index_) == "?" || text(index_) == "!")) {
            const auto op = std::string(text(index_++));
            (void)parseExpression(5); // consume the operand
            Expression error_expr;
            error_expr.kind  = ExprKind::Error;
            error_expr.text  = op;
            error_expr.scope = current_scope_;
            error_expr.span  = range(start, index_);
            snapshot_.diagnostics_.push_back(
                {range(start, index_),
                 "fallback and propagation operators are not supported in this version", false,
                 diagnostics::err::UnsupportedSyntax});
            return addExpression(std::move(error_expr));
        }
        if ((snapshot_.tokens_[index_].kind == TokenKind::Operator &&
             (text(index_) == "-" || text(index_) == "!" || text(index_) == "&" ||
              text(index_) == "*")) ||
            text(index_) == "not") {
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

        while (index_ < token_count_) {
            // `x is null` sits at comparison precedence; no other `is` form exists yet.
            if (isKeywordToken("is")) {
                if (2 < minimum_precedence)
                    break;
                ++index_;
                Expression is_null;
                is_null.scope = current_scope_;
                if (isKeywordToken("null")) {
                    ++index_;
                    is_null.kind = ExprKind::IsNull;
                    is_null.operands.push_back(left);
                } else {
                    is_null.kind = ExprKind::Error;
                    snapshot_.diagnostics_.push_back(
                        {range(start, index_), "'is' expressions are not supported in this version",
                         false, diagnostics::err::UnsupportedSyntax});
                    (void)parseExpression(); // consume the operand so no cascading errors follow
                }
                is_null.span = range(start, index_);
                left         = addExpression(std::move(is_null));
                continue;
            }
            if (snapshot_.tokens_[index_].kind != TokenKind::Operator)
                break;
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
        return static_cast<ExprId>(left);
    }

    [[nodiscard]] ExprId parseBlock() {
        const uint32_t start = index_;
        Expression block;
        block.kind           = ExprKind::Block;
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
        block.span                    = range(start, index_);
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
            snapshot_.diagnostics_.push_back(
                {range(start, index_), "expected ')' after condition"});

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

    /// `when (subject) { (cond) ~> body, ... , (_) ~> default }` — `match` is a
    /// synonym. Case conditions may be a range pattern `lo..hi` (case-local only;
    /// standalone ranges are rejected elsewhere). The default case `(_)` must be last.
    [[nodiscard]] ExprId parseWhen() {
        const uint32_t start = index_++;
        Expression expression;
        expression.kind  = ExprKind::When;
        expression.scope = current_scope_;
        if (punctuation(index_, '('))
            ++index_;
        expression.operands.push_back(parseExpression()); // subject
        if (punctuation(index_, ')'))
            ++index_;
        else
            snapshot_.diagnostics_.push_back(
                {range(start, index_), "expected ')' after when subject"});
        if (punctuation(index_, '{'))
            ++index_;
        else
            snapshot_.diagnostics_.push_back(
                {range(start, index_), "expected '{' after when subject"});

        while (index_ < token_count_ && !punctuation(index_, '}')) {
            if (punctuation(index_, ',')) {
                ++index_;
                continue;
            }
            const uint32_t case_start = index_;
            ExprId condition;
            bool is_default = false;
            if (punctuation(index_, '(')) {
                ++index_;
                if (text(index_) == "_" && punctuation(index_ + 1U, ')')) {
                    ++index_; // '_'
                    ++index_; // ')'
                    is_default = true;
                } else {
                    range_mode_ = true;
                    condition   = parseExpression();
                    range_mode_ = false;
                    // Range pattern `lo..hi`: only recognized inside a when case.
                    if (punctuation(index_, '.') && punctuation(index_ + 1U, '.')) {
                        const auto &cond_node = snapshot_.expressions_[condition.value - 1U];
                        index_ += 2;
                        Expression range;
                        range.kind  = ExprKind::Range;
                        range.text  = "..";
                        range.scope = current_scope_;
                        range.operands.push_back(condition);
                        range.operands.push_back(parseExpression());
                        range.span = {cond_node.span.start,
                                      snapshot_.tokens_[index_ - 1U].span.end};
                        condition  = addExpression(std::move(range));
                    }
                    if (punctuation(index_, ')'))
                        ++index_;
                    else
                        snapshot_.diagnostics_.push_back(
                            {range(case_start, index_), "expected ')' after when case condition"});
                }
            } else {
                snapshot_.diagnostics_.push_back(
                    {range(case_start, index_), "expected '(' before when case condition"});
                while (index_ < token_count_ && !punctuation(index_, ',') &&
                       !punctuation(index_, '}'))
                    ++index_;
            }
            if (isOperatorToken("~") && index_ + 1U < token_count_ &&
                snapshot_.tokens_[index_ + 1U].kind == TokenKind::Operator &&
                text(index_ + 1U) == ">") {
                index_ += 2;
            } else {
                snapshot_.diagnostics_.push_back(
                    {range(case_start, index_), "expected '~>' after when case condition"});
            }
            expression.conditions.push_back(is_default ? ExprId{} : condition);
            expression.operands.push_back(parseExpression()); // case body
            if (punctuation(index_, ','))
                ++index_;
        }
        if (punctuation(index_, '}'))
            ++index_;
        else
            snapshot_.diagnostics_.push_back(
                {range(start, index_), "expected '}' after when cases"});
        expression.span = range(start, index_);
        return parsePostfix(addExpression(std::move(expression)), start);
    }

    /// `for` is the canonical loop: `for { }` (infinite) and `for (cond) { }` (conditional).
    /// Iterator forms are not implemented yet and report a dedicated diagnostic.
    [[nodiscard]] ExprId parseFor() {
        const uint32_t start = index_++;
        Expression expression;
        expression.kind  = ExprKind::While;
        expression.scope = current_scope_;

        if (punctuation(index_, '{')) {
            // `for { ... }` desugars to `while (true) { ... }`.
            Expression always;
            always.kind  = ExprKind::Literal;
            always.text  = "true";
            always.scope = current_scope_;
            always.span  = tokenSpan(start);
            expression.operands.push_back(addExpression(std::move(always)));
        } else if (punctuation(index_, '(')) {
            ++index_;
            const uint32_t clause_start = index_;
            // init clause: `var x = e`, a bare expression, or empty (`;`).
            StmtId init_stmt;
            ExprId init_expr;
            if (index_ < token_count_ && text(index_) == "var") {
                ++index_;
                Statement stmt;
                stmt.kind                   = StmtKind::Binding;
                stmt.binding.mutableBinding = true;
                stmt.binding.id             = LocalId{statementCountLocals_++};
                stmt.binding.name           = std::string(text(index_));
                stmt.binding.span           = tokenSpan(index_++);
                if (punctuation(index_, ':')) {
                    ++index_;
                    stmt.binding.type = parseType();
                }
                if (index_ < token_count_ && text(index_) == "=") {
                    ++index_;
                    stmt.binding.initializer = parseExpression();
                }
                stmt.span = range(clause_start, index_);
                init_stmt = addStatement(std::move(stmt));
            } else if (!punctuation(index_, ';')) {
                init_expr = parseExpression();
            }
            if (punctuation(index_, ';')) {
                // 3-clause form: for (init; cond; step) { body }.  Init desugars to a
                // preceding statement of the enclosing block; cond defaults to `true`.
                // The step runs after each body iteration (and after `continue`), so it
                // is kept as a dedicated operand of the For node, not merged into the body.
                ++index_;
                if (bool(init_expr)) {
                    Statement stmt;
                    stmt.kind       = StmtKind::Expression;
                    stmt.expression = init_expr;
                    stmt.span       = tokenSpan(clause_start);
                    init_stmt       = addStatement(std::move(stmt));
                }
                // cond clause (default `true`).
                ExprId cond_expr;
                if (!punctuation(index_, ';'))
                    cond_expr = parseExpression();
                if (punctuation(index_, ';'))
                    ++index_;
                else
                    snapshot_.diagnostics_.push_back(
                        {range(start, index_), "expected ';' after for condition"});
                // step clause (optional).
                ExprId step_expr;
                if (!punctuation(index_, ')'))
                    step_expr = parseExpression();
                if (punctuation(index_, ')'))
                    ++index_;
                else
                    snapshot_.diagnostics_.push_back(
                        {range(start, index_), "expected ')' after for clauses"});
                if (!punctuation(index_, '{'))
                    snapshot_.diagnostics_.push_back({range(start, index_), "expected for body"});
                const ExprId body = parseBlock();
                if (!bool(cond_expr)) {
                    Expression always;
                    always.kind  = ExprKind::Literal;
                    always.text  = "true";
                    always.scope = current_scope_;
                    always.span  = range(start, index_);
                    cond_expr    = addExpression(std::move(always));
                }

                // Outer `{ init; for(cond){ body } step }`.
                Expression outer;
                outer.kind      = ExprKind::Block;
                outer.scope     = current_scope_;
                outer.span      = range(start, index_);
                expression.kind = ExprKind::For;
                expression.span = range(start, index_);
                expression.operands.push_back(cond_expr);
                expression.operands.push_back(body);
                expression.operands.push_back(step_expr);
                const ExprId for_id = addExpression(std::move(expression));
                if (bool(init_stmt))
                    outer.statements.push_back(init_stmt);
                Statement loop_stmt;
                loop_stmt.kind       = StmtKind::Expression;
                loop_stmt.expression = for_id;
                loop_stmt.span       = range(start, index_);
                outer.statements.push_back(addStatement(std::move(loop_stmt)));
                return addExpression(std::move(outer));
            }
            if (isKeywordToken("in")) {
                snapshot_.diagnostics_.push_back(
                    {range(clause_start, index_), "for iterator form is not implemented yet"});
                expression.span = range(start, index_);
                expression.kind = ExprKind::Error;
                return addExpression(std::move(expression));
            }
            if (punctuation(index_, ')'))
                ++index_;
            else
                snapshot_.diagnostics_.push_back(
                    {range(start, index_), "expected ')' after condition"});
            if (!bool(init_expr))
                snapshot_.diagnostics_.push_back(
                    {range(start, index_), "expected a condition after 'for ('"});
            expression.operands.push_back(init_expr);
        } else {
            snapshot_.diagnostics_.push_back(
                {range(start, index_), "expected '(' or a block after 'for'"});
            expression.span = range(start, index_);
            expression.kind = ExprKind::Error;
            return addExpression(std::move(expression));
        }

        if (punctuation(index_, '{'))
            expression.operands.push_back(parseBlock());
        else
            snapshot_.diagnostics_.push_back({range(start, index_), "expected for body"});
        expression.span = range(start, index_);
        return addExpression(std::move(expression));
    }

    [[nodiscard]] ExprId parseWhile() {
        const uint32_t start = index_++;
        snapshot_.diagnostics_.push_back(
            {tokenSpan(start), "'while' is deprecated; use 'for (cond) { }'", true});
        if (punctuation(index_, '('))
            ++index_;
        const ExprId condition = parseExpression();
        if (punctuation(index_, ')'))
            ++index_;
        else if (!punctuation(index_, '{'))
            snapshot_.diagnostics_.push_back(
                {range(start, index_), "expected ')' after condition"});

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
            statement.kind                   = StmtKind::Binding;
            statement.binding.mutableBinding = word == "var";
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
        } else if (word == "marker") {
            statement.kind = StmtKind::Marker;
            ++index_;
            if (index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Identifier) {
                statement.label = std::string(text(index_));
                ++index_;
            } else {
                snapshot_.diagnostics_.push_back({range(start, index_), "expected a marker name"});
            }
            statement.expression = parseBlock();
        } else if (word == "jump") {
            statement.kind = StmtKind::Jump;
            ++index_;
            if (index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Identifier) {
                statement.label = std::string(text(index_));
                ++index_;
            } else {
                snapshot_.diagnostics_.push_back(
                    {range(start, index_), "expected a jump target name"});
            }
        } else if (word == "use") {
            // `use` statements select from a context; not implemented yet.
            snapshot_.diagnostics_.push_back({range(start, index_),
                                              "use statements are not supported in this version",
                                              false, diagnostics::err::UnsupportedSyntax});
            while (index_ < token_count_ && !punctuation(index_, ';'))
                ++index_;
        } else {
            statement.expression = parseExpression();
            // Word-operator sequences such as `1 nop 2` are not implemented yet.
            if (index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Keyword &&
                (text(index_) == "nop" || text(index_) == "prefix" || text(index_) == "suffix" ||
                 text(index_) == "infix")) {
                snapshot_.diagnostics_.push_back(
                    {range(start, index_),
                     "word operator sequences are not supported in this version", false,
                     diagnostics::err::UnsupportedSyntax});
                while (index_ < token_count_ && !punctuation(index_, ';'))
                    ++index_;
            }
        }
        if (punctuation(index_, ';'))
            ++index_;
        statement.span = range(start, index_);
        return addStatement(std::move(statement));
    }

    void lowerImport(const uint32_t start, const Visibility visibility) {
        Declaration declaration;
        declaration.id         = DeclId{static_cast<uint32_t>(snapshot_.declarations_.size() + 1U)};
        declaration.kind       = DeclKind::Import;
        declaration.visibility = visibility;
        declaration.import.isExport = text(index_) == "export";
        declaration.import.isFrom   = text(index_) == "from";
        ++index_;
        if (index_ < token_count_ && text(index_) == "asset") {
            declaration.import.isAsset = true;
            ++index_;
        }
        const uint32_t path_start = index_;
        parseImportPath(declaration.import);
        declaration.import.pathSpan = range(path_start, index_);
        if (!declaration.import.isHeader) {
            declaration.import.rawPath = std::string(snapshot_.source_.substr(
                declaration.import.pathSpan.start, declaration.import.pathSpan.size()));
        }
        if (!declaration.import.path.empty() && declaration.import.path.front() == "assets")
            declaration.import.isAsset = true;
        parseImportDepth(declaration.import);
        parseImportSelectors(declaration.import);
        if (index_ < token_count_ && text(index_) == "as") {
            ++index_;
            if (index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Identifier) {
                declaration.import.alias     = std::string(text(index_));
                declaration.import.aliasSpan = tokenSpan(index_++);
            } else {
                snapshot_.diagnostics_.push_back(
                    {range(start, index_), "expected an import alias after 'as'"});
            }
        }
        declaration.span = range(start, index_);
        if (declaration.import.path.empty() && !declaration.import.isHeader) {
            snapshot_.diagnostics_.push_back({declaration.span, "expected an import path"});
            declaration.kind = DeclKind::Error;
        } else if (declaration.import.isAsset && declaration.import.alias.empty()) {
            snapshot_.diagnostics_.push_back(
                {declaration.span, "assets import requires an alias using 'as'"});
        }
        if (declaration.import.isHeader) {
            if (declaration.import.headerPath.ends_with(".hpp")) {
                snapshot_.diagnostics_.push_back(
                    {declaration.import.pathSpan, "C++ headers are not supported in this version"});
            }
            if (declaration.import.isFrom || declaration.import.isExport ||
                !declaration.import.selectors.empty() || declaration.import.depth != 1) {
                snapshot_.diagnostics_.push_back(
                    {declaration.span,
                     "C header imports only support 'import \"header.h\"' and an optional 'as' "
                     "alias"});
            }
        }
        snapshot_.declarations_.push_back(std::move(declaration));
    }

    void parseImportPath(ImportDecl &import) {
        if (index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Literal) {
            const auto literal = text(index_);
            if (literal.size() >= 2U && literal.front() == '"' && literal.back() == '"') {
                import.isHeader   = true;
                import.headerPath = std::string(literal.substr(1U, literal.size() - 2U));
                import.rawPath    = import.headerPath;
                import.pathSpans.push_back(tokenSpan(index_++));
                return;
            }
        }
        bool expect_segment = true;
        while (index_ < token_count_) {
            const auto segment = text(index_);
            const auto kind    = snapshot_.tokens_[index_].kind;
            if (kind == TokenKind::Identifier) {
                import.path.emplace_back(segment);
                import.pathSpans.push_back(tokenSpan(index_++));
                expect_segment = false;
                continue;
            }
            if (segment == "." || segment == "/") {
                if (segment == "." && expect_segment && index_ + 1U < token_count_ &&
                    text(index_ + 1U) == ".") {
                    import.path.emplace_back("..");
                    import.pathSpans.push_back(range(index_, index_ + 2U));
                    index_ += 2U;
                    expect_segment = false;
                    continue;
                }
                if (segment == "." && expect_segment) {
                    import.path.emplace_back(".");
                    import.pathSpans.push_back(tokenSpan(index_++));
                    expect_segment = false;
                    continue;
                }
                ++index_;
                expect_segment = true;
                continue;
            }
            break;
        }
    }

    void parseImportDepth(ImportDecl &import) {
        if (!punctuation(index_, '('))
            return;
        const uint32_t depth_start = index_++;
        if (index_ < token_count_ && text(index_) == "." && index_ + 1U < token_count_ &&
            text(index_ + 1U) == ".") {
            import.depth = -1;
            index_ += 2U;
        } else if (index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Literal) {
            try {
                import.depth = std::stoi(std::string(text(index_)));
            } catch (...) {
                snapshot_.diagnostics_.push_back({tokenSpan(index_), "invalid import depth"});
            }
            ++index_;
        } else {
            snapshot_.diagnostics_.push_back({range(depth_start, index_), "expected import depth"});
        }
        if (punctuation(index_, ')'))
            ++index_;
        else
            snapshot_.diagnostics_.push_back(
                {range(depth_start, index_), "expected ')' after import depth"});
    }

    void parseImportSelectors(ImportDecl &import) {
        if (!punctuation(index_, '{'))
            return;
        ++index_;
        while (index_ < token_count_ && !punctuation(index_, '}')) {
            if (punctuation(index_, ',')) {
                ++index_;
                continue;
            }
            if (snapshot_.tokens_[index_].kind != TokenKind::Identifier &&
                snapshot_.tokens_[index_].kind != TokenKind::Keyword) {
                snapshot_.diagnostics_.push_back(
                    {tokenSpan(index_), "expected symbol name in import selector"});
                ++index_;
                continue;
            }
            ImportSelector selector;
            selector.name = std::string(text(index_));
            selector.span = tokenSpan(index_++);
            if (index_ < token_count_ && text(index_) == "as") {
                ++index_;
                if (index_ < token_count_ &&
                    snapshot_.tokens_[index_].kind == TokenKind::Identifier) {
                    selector.alias     = std::string(text(index_));
                    selector.aliasSpan = tokenSpan(index_++);
                    selector.span.end  = selector.aliasSpan.end;
                } else {
                    snapshot_.diagnostics_.push_back(
                        {selector.span, "expected alias in import selector"});
                }
            }
            import.selectors.push_back(std::move(selector));
            if (punctuation(index_, ','))
                ++index_;
        }
        if (punctuation(index_, '}'))
            ++index_;
        else
            snapshot_.diagnostics_.push_back(
                {import.pathSpan, "expected '}' after import selectors"});
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
                    snapshot_.diagnostics_.push_back(
                        {tokenSpan(index_), "expected a parameter name"});
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
        if (punctuation(index_, ':') || isOperatorToken("->")) {
            ++index_; // `:` or `->` arrow return type
            declaration.declaredType = parseType();
        } else if (kind == DeclKind::TypeAlias && index_ < token_count_ && text(index_) == "=") {
            ++index_;
            declaration.declaredType = parseType();
        }
        if (kind == DeclKind::Function && punctuation(index_, '{')) {
            declaration.body = parseBlock();
        } else if (kind == DeclKind::Variable && index_ < token_count_ && text(index_) == "=") {
            ++index_;
            declaration.initializer = parseExpression();
        } else if (kind == DeclKind::Struct &&
                   punctuation(index_,
                               '{')) { // Parse struct field declarations: { name: Type, ... }
            ++index_;
            while (index_ < token_count_ && !punctuation(index_, '}')) {
                if (punctuation(index_, ',')) {
                    ++index_;
                    continue;
                }
                if (snapshot_.tokens_[index_].kind != TokenKind::Identifier) {
                    snapshot_.diagnostics_.push_back({tokenSpan(index_), "expected a field name"});
                    ++index_;
                    continue;
                }
                Parameter field;
                field.name = std::string(text(index_));
                field.span = tokenSpan(index_++);
                if (punctuation(index_, ':')) {
                    ++index_;
                    field.type = parseType();
                    if (index_ < token_count_ && text(index_) == "=") {
                        ++index_;
                        field.defaultValue = parseExpression();
                    }
                }
                declaration.parameters.push_back(std::move(field));
                if (punctuation(index_, ','))
                    ++index_;
                else if (!punctuation(index_, '}'))
                    break;
            }
            if (punctuation(index_, '}'))
                ++index_;
            else
                snapshot_.diagnostics_.push_back(
                    {range(start, index_), "expected '}' after struct fields"});
        } else if (kind == DeclKind::Enum && punctuation(index_, '{')) {
            // C-style enum body: `enum Name[: IntType] { Variant [= <int literal>], ... }`.
            // Each variant is stored as a Parameter; an explicit `= N` becomes its defaultValue.
            ++index_;
            while (index_ < token_count_ && !punctuation(index_, '}')) {
                if (punctuation(index_, ',')) {
                    ++index_;
                    continue;
                }
                if (snapshot_.tokens_[index_].kind != TokenKind::Identifier) {
                    snapshot_.diagnostics_.push_back(
                        {tokenSpan(index_), "expected a variant name"});
                    ++index_;
                    continue;
                }
                Parameter variant;
                variant.name = std::string(text(index_));
                variant.span = tokenSpan(index_++);
                if (index_ < token_count_ && text(index_) == "=") {
                    if (punctuation(index_ + 1, '{')) {
                        snapshot_.diagnostics_.push_back(
                            {range(index_, index_ + 2),
                             "struct-backed enum variants are not supported in this version", false,
                             diagnostics::err::UnsupportedSyntax});
                        ++index_;
                    } else {
                        ++index_;
                        variant.defaultValue = parseExpression();
                    }
                }
                declaration.parameters.push_back(std::move(variant));
                if (punctuation(index_, ','))
                    ++index_;
                else if (!punctuation(index_, '}'))
                    break;
            }
            if (punctuation(index_, '}'))
                ++index_;
            else
                snapshot_.diagnostics_.push_back(
                    {range(start, index_), "expected '}' after enum variants"});
        } else if (punctuation(index_, '{')) {
            skipDelimited('{', '}');
        }
        if (kind == DeclKind::Word || kind == DeclKind::Context) {
            const auto what = kind == DeclKind::Word ? "word declarations" : "context declarations";
            snapshot_.diagnostics_.push_back(
                {range(start, index_), std::string(what) + " are not supported in this version",
                 false, diagnostics::err::UnsupportedSyntax});
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
