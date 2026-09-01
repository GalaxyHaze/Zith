#include "frontend/ast-lowerer.hpp"

#include "diagnostics/error-codes.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zith::frontend {

TypeExprId AstLowerer::parseType() {
    if (index_ >= token_count_)
        return {};
    const uint32_t qualifier_start = index_;
    OwnershipKind ownership        = OwnershipKind::Default;
    bool has_ownership             = false;
    bool has_mut                   = false;
    // Zero or more memory qualifiers may precede the type itself; the prefix
    // annotates the type instead of introducing a new node, so `?lend T`,
    // `[]view T` and `lend *T` all keep working through the recursion below.
    while (index_ < token_count_) {
        const auto word = text(index_);
        OwnershipKind parsed{};
        if (word == "mut") {
            snapshot_.diagnostics_.push_back(
                {tokenSpan(index_),
                 "Zith--: 'mut' is not supported; use 'var' for a mutable local binding", false,
                 diagnostics::err::UnsupportedSyntax});
            if (has_mut) {
                snapshot_.diagnostics_.push_back({tokenSpan(index_),
                                                  "duplicate 'mut' qualifier on this type", false,
                                                  diagnostics::err::ExpectedExpr});
            }
            has_mut = true;
            ++index_;
            continue;
        }
        if (!ownershipKeyword(word, parsed))
            break;
        if (has_ownership) {
            snapshot_.diagnostics_.push_back({tokenSpan(index_),
                                              "a type may carry only one ownership qualifier",
                                              false, diagnostics::err::ExpectedExpr});
        }
        if (parsed == OwnershipKind::Unique || parsed == OwnershipKind::Share ||
            parsed == OwnershipKind::Belong) {
            snapshot_.diagnostics_.push_back(
                {tokenSpan(index_),
                 "Zith--: unique/share/belong ownership is not supported; use lend or view", false,
                 diagnostics::err::UnsupportedSyntax});
        }
        ownership     = parsed;
        has_ownership = true;
        ++index_;
    }
    // `mut view T` is contradictory: `view` is read-only by definition.
    if (has_mut && ownership == OwnershipKind::View) {
        snapshot_.diagnostics_.push_back({range(qualifier_start, index_),
                                          "'view' is read-only and cannot be combined with "
                                          "'mut'",
                                          false, diagnostics::err::ExpectedExpr});
    }
    if (index_ >= token_count_)
        return {};
    const uint32_t start = index_;
    TypeExpression type;
    type.kind          = TypeExprKind::Error;
    type.ownership     = ownership;
    type.hasMutKeyword = has_mut;
    switch (ownership) {
    case OwnershipKind::Lend:
    case OwnershipKind::Unique:
    case OwnershipKind::Share:
    case OwnershipKind::Belong:
        type.isMut = true;
        break;
    case OwnershipKind::View:
        type.isMut = false;
        break;
    case OwnershipKind::Default:
        type.isMut = has_mut;
        break;
    }
    // `raw opaque` is a single type, not a qualifier plus a name. `raw` followed by
    // anything else in type position falls through to the diagnostics below.
    if (punctuation(index_, '(')) {
        // Parenthesized type expressions let nested composites be written
        // unambiguously, for example `?(?T)`.
        ++index_;
        type.kind = TypeExprKind::Parenthesized;
        type.arguments.push_back(parseType());
        if (punctuation(index_, ')'))
            ++index_;
        else
            snapshot_.diagnostics_.push_back({range(start, index_),
                                              "expected ')' after parenthesized type", false,
                                              diagnostics::err::ExpectedExpr});
    } else if (isKeywordToken("raw") && index_ + 1U < token_count_ &&
               text(index_ + 1U) == "opaque") {
        index_ += 2U;
        type.kind = TypeExprKind::Opaque;
    } else if (isKeywordToken("opaque")) {
        ++index_;
        type.kind = TypeExprKind::OpaqueTagged;
    } else if (matchesToken(snapshot_, index_, "?")) {
        ++index_;
        type.kind = TypeExprKind::Optional;
        type.arguments.push_back(parseType());
    } else if (matchesToken(snapshot_, index_, "[")) {
        ++index_;
        if (matchesToken(snapshot_, index_, "...")) {
            ++index_;
            // Accept the documented `[...]T` spelling and the legacy
            // tokenized `[ ... ]T` with trivia between tokens. The old
            // parsing path only accepted the second form.
            type.kind            = TypeExprKind::Slice;
            type.isVariadicSlice = true;
            if (isPunctuation(snapshot_, index_, ']')) {
                ++index_;
            } else {
                snapshot_.diagnostics_.push_back(
                    {range(start, index_), "expected ']' after variadic slice marker '...'"});
            }
        } else if (matchesToken(snapshot_, index_, "]")) {
            ++index_;
            type.kind = TypeExprKind::Slice;
        } else {
            // `[N]T` is a fixed-size array; the length must be an integer literal.
            type.kind = TypeExprKind::Array;
            if (index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Literal) {
                type.arrayLength = std::strtoull(std::string(text(index_)).c_str(), nullptr, 10);
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
    } else if (matchesToken(snapshot_, index_, "|")) {
        ++index_;
        type.kind = TypeExprKind::Pack;
        while (index_ < token_count_ && !matchesToken(snapshot_, index_, "|")) {
            if (matchesToken(snapshot_, index_, ",")) {
                ++index_;
                continue;
            }
            if (snapshot_.tokens_[index_].kind != TokenKind::Identifier) {
                snapshot_.diagnostics_.push_back({tokenSpan(index_),
                                                  "pack type members must be named", false,
                                                  diagnostics::err::ExpectedExpr});
                ++index_;
                continue;
            }
            type.member_names.push_back(std::string(text(index_++)));
            if (punctuation(index_, ':')) {
                ++index_;
                const TypeExprId member_type = parseType();
                if (!member_type)
                    break;
                type.arguments.push_back(member_type);
            } else {
                snapshot_.diagnostics_.push_back({tokenSpan(index_),
                                                  "expected ':' after pack member name", false,
                                                  diagnostics::err::ExpectedExpr});
                break;
            }
            if (matchesToken(snapshot_, index_, ","))
                ++index_;
            else if (!matchesToken(snapshot_, index_, "|")) {
                snapshot_.diagnostics_.push_back({range(start, index_),
                                                  "expected ',' or '|' in pack type", false,
                                                  diagnostics::err::ExpectedExpr});
                break;
            }
        }
        if (matchesToken(snapshot_, index_, "|"))
            ++index_;
        else
            snapshot_.diagnostics_.push_back({range(start, index_), "expected '|' after pack type",
                                              false, diagnostics::err::ExpectedExpr});
    } else if (isKeywordToken("dyn")) {
        ++index_;
        type.kind              = TypeExprKind::Dyn;
        const TypeExprId inner = parseType();
        if (inner)
            type.arguments.push_back(inner);
        else
            snapshot_.diagnostics_.push_back({range(start, index_),
                                              "expected a trait or interface after 'dyn'", false,
                                              diagnostics::err::ExpectedExpr});
    } else if (isKeywordToken("fn") || (isKeywordToken("state") && index_ + 1U < token_count_ &&
                                        punctuation(index_ + 1U, '('))) {
        // A function type value: `fn(params): result` or the state value
        // type `state(params): result`. Parameters are
        // parsed as bare types; the result follows `:` and is appended as
        // the final argument so sema and canonical printing share the
        // existing `arguments = [params..., result]` convention.
        if (isKeywordToken("state"))
            type.isStateFunctionType = true;
        ++index_;
        if (!punctuation(index_, '(')) {
            snapshot_.diagnostics_.push_back({tokenSpan(index_),
                                              "expected '(' after function-type keyword", false,
                                              diagnostics::err::ExpectedExpr});
        } else {
            ++index_;
            while (index_ < token_count_ && !punctuation(index_, ')')) {
                TypeExprId parameter = parseType();
                if (!parameter)
                    break;
                type.arguments.push_back(parameter);
                if (punctuation(index_, ',')) {
                    ++index_;
                    continue;
                }
                if (!punctuation(index_, ')')) {
                    snapshot_.diagnostics_.push_back(
                        {tokenSpan(index_), "expected ',' or ')' in function type parameters",
                         false, diagnostics::err::ExpectedExpr});
                }
                break;
            }
            if (punctuation(index_, ')'))
                ++index_;
            else
                snapshot_.diagnostics_.push_back({range(start, index_),
                                                  "expected ')' in function type", false,
                                                  diagnostics::err::ExpectedExpr});
        }
        if (punctuation(index_, ':')) {
            ++index_;
            type.arguments.push_back(parseType());
        } else {
            snapshot_.diagnostics_.push_back({range(start, index_),
                                              "function/state type requires a return type "
                                              "after ':'",
                                              false, diagnostics::err::ExpectedExpr});
        }
        type.kind = TypeExprKind::Function;
    } else if (snapshot_.tokens_[index_].kind == TokenKind::Identifier ||
               snapshot_.tokens_[index_].kind == TokenKind::Keyword) {
        type.kind = TypeExprKind::Name;
        type.name = std::string(text(index_++));
        // Dotted qualified names in type position are written without
        // spaces (`std.counter.Counter`); each `.` is a separate operator
        // token in the lexer. Record the full path so sema can route it
        // through the import resolution graph instead of a bare name.
        std::vector<std::string> segments{type.name};
        while (index_ + 1U < token_count_ && punctuation(index_, '.') &&
               (snapshot_.tokens_[index_ + 1U].kind == TokenKind::Identifier ||
                snapshot_.tokens_[index_ + 1U].kind == TokenKind::Keyword)) {
            ++index_; // '.'
            const auto segment = std::string(text(index_++));
            segments.push_back(segment);
        }
        if (segments.size() > 1U) {
            type.segments = std::move(segments);
            std::string dotted;
            for (size_t i = 0; i < type.segments.size(); ++i) {
                if (i != 0U)
                    dotted += ".";
                dotted += type.segments[i];
            }
            type.name = std::move(dotted);
        }
        // Generic applications in type position: `Pair<T, U>`, `Node<i32>`.
        // The parser accepts a balanced `<...>` after a type name; sema reports
        // unresolved generic templates when they cannot be monomorphized.
        if (isOperatorToken("<")) {
            int depth     = 0;
            bool balanced = true;
            for (uint32_t i = index_; i < token_count_; ++i) {
                const auto &token = snapshot_.tokens_[i];
                if (token.kind == TokenKind::Operator) {
                    if (text(i) == "<")
                        ++depth;
                    else if (text(i) == ">") {
                        --depth;
                        if (depth == 0)
                            break;
                    }
                } else if (token.kind == TokenKind::End) {
                    balanced = false;
                    break;
                }
            }
            if (balanced) {
                ++index_; // '<'
                while (index_ < token_count_ && !isOperatorToken(">")) {
                    type.arguments.push_back(parseType());
                    if (!punctuation(index_, ','))
                        break;
                    ++index_;
                }
                if (isOperatorToken(">"))
                    ++index_;
                else
                    snapshot_.diagnostics_.push_back(
                        {range(start, index_), "expected '>' after generic type arguments"});
            }
        }
    } else {
        snapshot_.diagnostics_.push_back({tokenSpan(index_), "expected a type"});
        ++index_;
    }
    type.span = range(qualifier_start, index_);
    return addType(std::move(type));
}
bool AstLowerer::isIntrinsicName(std::string_view name) noexcept {
    for (const auto *k : kIntrinsicNames)
        if (name == k)
            return true;
    return false;
}
} // namespace zith::frontend
