#include "frontend/ast-lowerer.hpp"

#include "diagnostics/error-codes.hpp"
#include "frontend/frontend.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
    constexpr std::string_view punctuation = "()[]{}:;,.@`";
    return punctuation.find(character) != std::string_view::npos;
}

[[nodiscard]] bool isKeyword(std::string_view word) {
    constexpr std::string_view keywords[] = {
        "i8",      "i16",    "i32",    "i64",     "i128",      "u8",       "u16",       "u32",
        "u64",     "u128",   "f32",    "f64",     "bool",      "char",     "void",      "invalid",
        "unknown", "null",   "true",   "false",   "type",      "struct",   "component", "enum",
        "raw",     "unsafe", "union",  "trait",   "interface", "extends",  "dyn",       "implement",
        "fn",      "import", "use",    "context", "macro",     "export",   "extern",    "from",
        "alias",   "as",     "let",    "var",     "auto",      "const",    "mut",       "global",
        "lend",    "share",  "view",   "unique",  "belong",    "yield",    "async",     "state",
        "dock",    "never",  "pub",    "mod",     "if",        "else",     "for",       "in",
        "when",    "match",  "return", "break",   "continue",  "jump",     "while",     "marker",
        "spawn",   "await",  "with",   "catch",   "must",      "throw",    "fail",      "drop",
        "require", "is",     "prefix", "suffix",  "infix",     "nop",      "and",       "or",
        "not",     "xor",    "tag",    "defer",   "opaque",    "optional",
    };
    return std::any_of(std::begin(keywords), std::end(keywords),
                       [&](const std::string_view keyword) { return word == keyword; });
}

[[nodiscard]] const GreenNode *makeNode(memory::Arena &arena, SyntaxKind kind, TextSpan span,
                                        const std::vector<GreenElement> &children) {
    GreenElement *elements = nullptr;
    if (!children.empty()) {
        if (children.size() > std::numeric_limits<size_t>::max() / sizeof(GreenElement))
            return nullptr;
        elements = static_cast<GreenElement *>(
            arena.alloc(sizeof(GreenElement) * children.size(), alignof(GreenElement)));
        if (elements == nullptr)
            return nullptr;
        std::memcpy(elements, children.data(), sizeof(GreenElement) * children.size());
    }
    return arena.make<GreenNode>(
        GreenNode{kind, span, elements, static_cast<uint32_t>(children.size())});
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
                while (position < source.size() && source[position] != '\n')
                    ++position;
                snapshot.trivia_.push_back(Trivia{TriviaKind::DocLine, TextSpan{start, position}});
                continue;
            }
            if (source.substr(position).starts_with("//")) {
                position += 2;
                while (position < source.size() && source[position] != '\n')
                    ++position;
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

        if (position == source.size())
            break;

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
            if (!closed)
                snapshot.diagnostics_.push_back(
                    Diagnostic{TextSpan{start, position}, "unterminated string literal"});
            snapshot.tokens_.push_back(
                Token{TokenKind::Literal, TextSpan{start, position}, triviaStart,
                      static_cast<uint32_t>(snapshot.trivia_.size()) - triviaStart});
            continue;
        }

        if (source.substr(position).starts_with("...")) {
            position += 3;
            snapshot.tokens_.push_back(
                Token{TokenKind::Punctuation, TextSpan{start, position}, triviaStart,
                      static_cast<uint32_t>(snapshot.trivia_.size()) - triviaStart});
            continue;
        }

        ++position;
        if (isOperator(source[start])) {
            static constexpr std::string_view kThreeChar[] = {"<<=", ">>="};
            static constexpr std::string_view kTwoChar[]   = {
                "==", "!=", "<=", ">=", "->", "~>", "<<", ">>", "+=", "-=", "*=",
                "/=", "%=", "&=", "|=", "^=", "&.", "|.", "^.", "&&", "||", "??"};
            bool munched = false;
            if (start + 3U <= source.size()) {
                const std::string_view triple = source.substr(start, 3);
                for (const auto candidate : kThreeChar) {
                    if (triple == candidate) {
                        munched  = true;
                        position = start + 3U;
                        break;
                    }
                }
            }
            if (!munched && start + 2U <= source.size()) {
                const std::string_view pair = source.substr(start, 2);
                for (const auto candidate : kTwoChar) {
                    if (pair == candidate) {
                        if (candidate == "??") {
                            position = start + 1U;
                            munched  = true;
                            break;
                        }
                        position = start + 2U;
                        break;
                    }
                }
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

void parseCst(FrontendSnapshot &snapshot) {
    std::vector<GreenElement> children;
    std::vector<TokenId> delimiters;
    children.reserve(snapshot.tokens_.size());

    for (uint32_t index = 0; index + 1 < snapshot.tokens_.size(); ++index) {
        const TokenId token{index + 1};
        const Token &current = snapshot.tokens_[index];
        if (current.kind == TokenKind::Punctuation) {
            const char punctuation = snapshot.source_[current.span.start];
            if (punctuation == '(' || punctuation == '[' || punctuation == '{')
                delimiters.push_back(token);
            else if (punctuation == ')' || punctuation == ']' || punctuation == '}') {
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

std::string_view tokenText(const FrontendSnapshot &snapshot, uint32_t index) noexcept {
    const auto span = snapshot.tokens()[index].span;
    return std::string_view(snapshot.source()).substr(span.start, span.size());
}

bool isPunctuation(const FrontendSnapshot &snapshot, uint32_t index, char character) noexcept {
    return snapshot.tokens()[index].kind == TokenKind::Punctuation &&
           tokenText(snapshot, index) == std::string_view(&character, 1);
}

bool matchesToken(const FrontendSnapshot &snapshot, uint32_t index,
                  std::string_view text) noexcept {
    return index < snapshot.tokens().size() && tokenText(snapshot, index) == text;
}

std::optional<DeclKind> declarationKind(std::string_view word) noexcept {
    if (word == "fn")
        return DeclKind::Function;
    if (word == "state")
        return DeclKind::Function;
    if (word == "type" || word == "alias")
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
    if (word == "let" || word == "var" || word == "const")
        return DeclKind::Variable;
    if (word == "context")
        return DeclKind::Context;
    if (word == "prefix" || word == "suffix" || word == "infix" || word == "nop")
        return DeclKind::Word;
    return std::nullopt;
}

std::optional<FunctionKind> functionKindPrefix(const FrontendSnapshot &snapshot, uint32_t &index,
                                               uint32_t token_count) noexcept {
    if (index >= token_count)
        return std::nullopt;
    const auto word = tokenText(snapshot, index);
    if (word == "fn")
        return FunctionKind::Standard;

    static constexpr std::string_view kFunctionKindPrefixes[] = {"const", "raw", "extern"};
    for (const auto prefix : kFunctionKindPrefixes) {
        if (word != prefix || index + 1U >= token_count || tokenText(snapshot, index + 1U) != "fn")
            continue;
        ++index;
        if (prefix == "const")
            return FunctionKind::Const;
        if (prefix == "raw")
            return FunctionKind::Raw;
        if (prefix == "extern")
            return FunctionKind::Extern;
    }
    return std::nullopt;
}

BindingKind bindingKind(std::string_view word) noexcept {
    if (word == "let")
        return BindingKind::Let;
    if (word == "var")
        return BindingKind::Var;
    if (word == "const")
        return BindingKind::Const;
    return BindingKind::Const;
}

AstLowerer::AstLowerer(FrontendSnapshot &snapshot)
    : snapshot_(snapshot), token_count_(static_cast<uint32_t>(snapshot.tokens_.size() - 1U)) {}

std::string_view AstLowerer::text(uint32_t index) const noexcept {
    return tokenText(snapshot_, index);
}

bool AstLowerer::punctuation(uint32_t index, char character) const noexcept {
    return index < token_count_ && isPunctuation(snapshot_, index, character);
}

TextSpan AstLowerer::tokenSpan(uint32_t index) const noexcept {
    return snapshot_.tokens_[index].span;
}

TextSpan AstLowerer::range(uint32_t start, uint32_t end) const noexcept {
    if (start >= token_count_)
        return {};
    return {tokenSpan(start).start, end > start ? tokenSpan(end - 1U).end : tokenSpan(start).end};
}

ExprId AstLowerer::addExpression(Expression expression) {
    expression.id = ExprId{static_cast<uint32_t>(snapshot_.expressions_.size() + 1U)};
    snapshot_.expressions_.push_back(std::move(expression));
    return snapshot_.expressions_.back().id;
}

StmtId AstLowerer::addStatement(Statement statement) {
    statement.id = StmtId{static_cast<uint32_t>(snapshot_.statements_.size() + 1U)};
    snapshot_.statements_.push_back(std::move(statement));
    return snapshot_.statements_.back().id;
}

TypeExprId AstLowerer::addType(TypeExpression type) {
    type.id = TypeExprId{static_cast<uint32_t>(snapshot_.type_expressions_.size() + 1U)};
    snapshot_.type_expressions_.push_back(std::move(type));
    return snapshot_.type_expressions_.back().id;
}

ScopeId AstLowerer::addScope(ScopeId parent, TextSpan span) {
    const ScopeId id{next_scope_++};
    snapshot_.scopes_.push_back({id, parent, span});
    return id;
}

bool AstLowerer::ownershipKeyword(std::string_view word, OwnershipKind &out) noexcept {
    if (word == "lend")
        out = OwnershipKind::Lend;
    else if (word == "view")
        out = OwnershipKind::View;
    else if (word == "unique")
        out = OwnershipKind::Unique;
    else if (word == "share")
        out = OwnershipKind::Share;
    else if (word == "belong")
        out = OwnershipKind::Belong;
    else
        return false;
    return true;
}

void AstLowerer::run() {
    root_scope_           = addScope({}, {0, static_cast<uint32_t>(snapshot_.source_.size())});
    current_scope_        = root_scope_;
    Visibility visibility = Visibility::Private;
    // Set by a preceding `extern` keyword; consumed by the next declaration.
    bool is_extern = false;
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

        // Function-kind prefixes must win over `const` (binding), `raw`
        // (unsafe marker), `extern` (interop prefix), and `flow`/`fn`.
        if (const auto parsed_kind = functionKindPrefix()) {
            flushBadRun();
            declaration_is_nominal_ = false;
            auto function_kind      = *parsed_kind;
            if (is_extern && function_kind != FunctionKind::Extern)
                snapshot_.diagnostics_.push_back(
                    {range(start, index_),
                     "invalid function-kind prefix: external declarations use 'extern fn'", false,
                     diagnostics::err::UnsupportedSyntax});
            if (function_kind == FunctionKind::Standard && is_extern)
                function_kind = FunctionKind::Extern;
            lowerDeclaration(start, DeclKind::Function, visibility, {}, {}, is_extern,
                             function_kind);
            declaration_is_nominal_ = false;
            visibility              = Visibility::Private;
            is_extern               = false;
            continue;
        }

        if (word == "state") {
            flushBadRun();
            lowerDeclaration(start, DeclKind::Function, visibility, {}, {}, is_extern,
                             FunctionKind::State);
            declaration_is_nominal_ = false;
            visibility              = Visibility::Private;
            is_extern               = false;
            continue;
        }

        if (word == "flow" && index_ + 1U < token_count_ && text(index_ + 1U) == "fn") {
            flushBadRun();
            snapshot_.diagnostics_.push_back(
                {range(start, index_ + 2U),
                 "Zith--: 'flow fn' is not supported; use a 'state' function", false,
                 diagnostics::err::UnsupportedSyntax});
            ++index_;
            lowerDeclaration(start, DeclKind::Function, visibility, {}, {}, is_extern,
                             FunctionKind::Standard);
            visibility = Visibility::Private;
            is_extern  = false;
            continue;
        }

        if (word == "marker" ||
            (word == "stackful" && index_ + 1U < token_count_ && text(index_ + 1U) == "marker")) {
            flushBadRun();
            snapshot_.diagnostics_.push_back(
                {range(start, index_ + (word == "stackful" ? 2U : 1U)),
                 "Zith--: 'marker' is not supported; declare a 'state' function instead", false,
                 diagnostics::err::UnsupportedSyntax});
            if (word == "stackful")
                ++index_;
            ++index_;
            lowerDeclaration(start, DeclKind::Function, visibility, {}, {}, is_extern,
                             FunctionKind::Standard);
            declaration_is_nominal_ = false;
            visibility              = Visibility::Private;
            is_extern               = false;
            continue;
        }

        // `raw union Name { ... }` is an untagged C-style union. Keep the
        // keyword consumed so the body is parsed by the union branch below.
        if (word == "raw" && index_ + 1 < token_count_ && text(index_ + 1) == "union") {
            flushBadRun();
            ++index_; // consume 'raw'
            lowerDeclaration(start, DeclKind::Union, visibility, {}, {}, false,
                             FunctionKind::Standard, {}, true);
            visibility = Visibility::Private;
            continue;
        }

        const auto kind = declarationKind(word);
        if (kind) {
            flushBadRun();
            declaration_is_nominal_ = word == "type";
            lowerDeclaration(start, *kind, visibility, {}, {}, is_extern);
            declaration_is_nominal_ = false;
            visibility              = Visibility::Private;
            is_extern               = false;
            continue;
        }

        // `raw` before a macro declaration: `raw macro name(...) { }`.
        if (word == "raw" && index_ + 1 < token_count_ && text(index_ + 1) == "macro") {
            flushBadRun();
            ++index_; // consume 'raw'
            lowerMacroDeclaration(start, visibility, true, false);
            visibility = Visibility::Private;
            continue;
        }

        // `tag macro Name(...) { }` — invoked as `<Name ...> ... </Name>`.
        // A bare `tag` elsewhere stays an ordinary identifier.
        if (word == "tag" && index_ + 1 < token_count_ && text(index_ + 1) == "macro") {
            flushBadRun();
            ++index_; // consume 'tag'
            lowerMacroDeclaration(start, visibility, false, true);
            visibility = Visibility::Private;
            continue;
        }

        // Zith-- declares static storage with a top-level `const`; the
        // legacy `global` keyword is rejected before the variable parser.
        if (word == "global") {
            flushBadRun();
            snapshot_.diagnostics_.push_back(
                {tokenSpan(index_),
                 "Zith--: 'global' is not supported; use `const NAME: T = value`", false,
                 diagnostics::err::UnsupportedSyntax});
            lowerDeclaration(start, DeclKind::Variable, visibility, {}, {}, is_extern,
                             FunctionKind::Standard, {}, false, true);
            visibility = Visibility::Private;
            is_extern  = false;
            continue;
        }

        // `macro` declaration: `macro name(...) { body }`.
        if (word == "macro") {
            flushBadRun();
            lowerMacroDeclaration(start, visibility, false, false);
            visibility = Visibility::Private;
            continue;
        }

        if (word == "extern" || word == "use" || word == "unsafe" || word == ";") {
            flushBadRun();
            if (word == "extern" && !functionKindPrefix())
                is_extern = true;
            ++index_;
            continue;
        }

        // `implement Type { ... }` or `impl Type { ... }`: method-bearing type
        // bodies and trait implementations.
        if (word == "implement" || word == "impl") {
            flushBadRun();
            lowerImplementBlock(start, visibility);
            visibility = Visibility::Private;
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

void AstLowerer::skipMacroInvocation() {
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
        else if (punctuation(index_, ')') || punctuation(index_, ']') || punctuation(index_, '}')) {
            if (depth > 0)
                --depth;
        }
        ++index_;
    }
    if (index_ < token_count_ && punctuation(index_, ';'))
        ++index_;
}

} // namespace zith::frontend
