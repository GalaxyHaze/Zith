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

/// Parses one element of a call argument list. `lend`/`view` are only
/// meaningful here, so the ownership annotation is recognized only in this
/// entry point instead of in `parsePrimary`.
ExprId AstLowerer::parseCallArgument() {
    const uint32_t start = index_;
    OwnershipKind kind   = OwnershipKind::Default;
    if (index_ < token_count_ && ownershipKeyword(text(index_), kind)) {
        if (kind == OwnershipKind::Lend || kind == OwnershipKind::View) {
            ++index_;
            Expression coerce;
            coerce.kind      = ExprKind::OwnershipCoerce;
            coerce.ownership = kind;
            coerce.scope     = current_scope_;
            coerce.operands.push_back(parseExpression());
            coerce.span = range(start, index_);
            return addExpression(std::move(coerce));
        }
        snapshot_.diagnostics_.push_back({tokenSpan(index_),
                                          "invalid ownership annotation on a call argument; use "
                                          "'lend' or 'view'",
                                          false, diagnostics::err::InvalidCallOwnership});
        ++index_;
        const ExprId inner = parseExpression();
        Expression error_expr;
        error_expr.kind  = ExprKind::Error;
        error_expr.scope = current_scope_;
        error_expr.span  = range(start, index_);
        if (inner)
            error_expr.operands.push_back(inner);
        return addExpression(std::move(error_expr));
    }
    return parseExpression();
}
ExprId AstLowerer::parsePrimary() {
    if (index_ >= token_count_)
        return {};

    const uint32_t start = index_;
    if (punctuation(index_, '{'))
        return parseBlock();

    // `dock State(args)` is a primary call expression whose result is the
    // state machine's eventual return value.
    if (isKeywordToken("dock")) {
        const uint32_t dock_start = index_++;
        Expression dock;
        dock.kind  = ExprKind::DockCall;
        dock.scope = current_scope_;
        if (index_ < token_count_ && (snapshot_.tokens_[index_].kind == TokenKind::Identifier ||
                                      snapshot_.tokens_[index_].kind == TokenKind::Keyword)) {
            Expression callee;
            callee.kind  = ExprKind::Name;
            callee.scope = current_scope_;
            callee.text  = std::string(text(index_));
            callee.span  = tokenSpan(index_);
            dock.operands.push_back(addExpression(std::move(callee)));
            ++index_;
        } else {
            snapshot_.diagnostics_.push_back({range(dock_start, index_),
                                              "dock syntax is 'dock State(args)'", false,
                                              diagnostics::err::UnsupportedSyntax});
        }
        if (punctuation(index_, '(')) {
            ++index_;
            while (index_ < token_count_ && !punctuation(index_, ')')) {
                if (punctuation(index_, ',')) {
                    ++index_;
                    continue;
                }
                dock.operands.push_back(parseCallArgument());
                if (punctuation(index_, ','))
                    ++index_;
                else if (!punctuation(index_, ')'))
                    break;
            }
            if (punctuation(index_, ')'))
                ++index_;
            else
                snapshot_.diagnostics_.push_back({range(dock_start, index_),
                                                  "expected ')' after dock arguments", false,
                                                  diagnostics::err::ExpectedExpr});
        } else if (!punctuation(index_, ';') && !punctuation(index_, '}')) {
            snapshot_.diagnostics_.push_back({range(dock_start, index_),
                                              "dock syntax is 'dock State(args)'", false,
                                              diagnostics::err::UnsupportedSyntax});
            while (index_ < token_count_ && !punctuation(index_, ';') && !punctuation(index_, '}'))
                ++index_;
        }
        dock.span = range(dock_start, index_);
        return parsePostfix(addExpression(std::move(dock)), dock_start);
    }

    // `<Name ...> ... </Name>` tag-macro invocation in expression position is
    // parsed as a MacroCall so value-position checks can reject it cleanly.
    if (isTagMacroOpen()) {
        const auto tag = parseTagMacroCall();
        return parsePostfix(tag, start);
    }

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

    // `|a, b|` is a pack literal. The leading `||` binary operator is not
    // valid Zith-- syntax, and `@name|attrs|` macro forms are consumed by
    // the MacroCall path above, so a bare primary `|` is unambiguous.
    if (matchesToken(snapshot_, index_, "|")) {
        const uint32_t pack_start = index_++;
        Expression pack_lit;
        pack_lit.kind  = ExprKind::PackLiteral;
        pack_lit.scope = current_scope_;
        while (index_ < token_count_ && !matchesToken(snapshot_, index_, "|")) {
            if (matchesToken(snapshot_, index_, ",")) {
                ++index_;
                continue;
            }
            pack_lit.operands.push_back(parseExpression());
            if (!matchesToken(snapshot_, index_, ",") && !matchesToken(snapshot_, index_, "|"))
                break;
            if (matchesToken(snapshot_, index_, ","))
                ++index_;
        }
        if (matchesToken(snapshot_, index_, "|"))
            ++index_;
        else
            snapshot_.diagnostics_.push_back({range(pack_start, index_),
                                              "expected '|' after pack literal elements", false,
                                              diagnostics::err::ExpectedExpr});
        pack_lit.span = range(pack_start, index_);
        return parsePostfix(addExpression(std::move(pack_lit)), pack_start);
    }

    // `@name` — intrinsics (from kIntrinsicNames) or user macros.
    if (punctuation(index_, '@')) {
        ++index_;
        if (index_ < token_count_ && (snapshot_.tokens_[index_].kind == TokenKind::Identifier ||
                                      snapshot_.tokens_[index_].kind == TokenKind::Keyword)) {
            const auto name = std::string(text(index_++));
            // Qualified macro names: `@ns.macro`. The expander resolves the
            // full dotted name, which lets `import path as ns` expose public
            // macros under the module namespace.
            std::string qualified = name;
            while (punctuation(index_, '.') && index_ + 1U < token_count_ &&
                   (snapshot_.tokens_[index_ + 1U].kind == TokenKind::Identifier ||
                    snapshot_.tokens_[index_ + 1U].kind == TokenKind::Keyword)) {
                ++index_; // '.'
                qualified += ".";
                qualified += std::string(text(index_++));
            }
            Expression expr;
            expr.scope = current_scope_;
            if (isIntrinsicName(name)) {
                expr.kind = ExprKind::LayoutIntrinsic;
                expr.text = name;
                // @lengthOf/@ptrOf take a value operand; layout intrinsics
                // take a type and optionally a field name.
                if ((name == "lengthOf" || name == "ptrOf") && punctuation(index_, '(')) {
                    ++index_;
                    expr.operands.push_back(parseExpression());
                    if (punctuation(index_, ')'))
                        ++index_;
                    else
                        snapshot_.diagnostics_.push_back(
                            {range(start, index_), "expected ')' after intrinsic arguments"});
                } else if ((name == "offsetOf" || name == "alignOf" || name == "sizeOf" ||
                            name == "canonicalType") &&
                           punctuation(index_, '(')) {
                    ++index_;
                    expr.cast_type = parseType();
                    if (punctuation(index_, ',')) {
                        ++index_;
                        if (index_ < token_count_ &&
                            (snapshot_.tokens_[index_].kind == TokenKind::Identifier ||
                             snapshot_.tokens_[index_].kind == TokenKind::Keyword)) {
                            expr.field_names.push_back(std::string(text(index_++)));
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
                }
            } else {
                // User macro call @name(|attrs|)(args) or @name(args).
                expr.kind = ExprKind::MacroCall;
                expr.text = qualified;
                // Optional `|attributes|` before arguments. An empty list is written
                // `||`, which the lexer munches as one token, so accept it directly.
                if (text(index_) == "||") {
                    ++index_;
                } else if (text(index_) == "|") {
                    ++index_; // opening |
                    while (index_ < token_count_ && !(text(index_) == "|")) {
                        if (punctuation(index_, ',')) {
                            ++index_;
                            continue;
                        }
                        std::string attr_name;
                        if (snapshot_.tokens_[index_].kind == TokenKind::Identifier &&
                            index_ + 1 < token_count_ && text(index_ + 1) == ":") {
                            attr_name = std::string(text(index_));
                            index_ += 2; // skip name + ':'
                            expr.attributes.push_back(parseAttributeValue());
                        } else {
                            expr.attributes.push_back(parseAttributeValue());
                        }
                        expr.attributeNames.push_back(std::move(attr_name));
                        if (punctuation(index_, ','))
                            ++index_;
                        else if (!(text(index_) == "|"))
                            break;
                    }
                    if (text(index_) == "|")
                        ++index_; // closing |
                }
                // Arguments: `(arg1, arg2, ...)`.
                if (punctuation(index_, '(')) {
                    ++index_;
                    while (index_ < token_count_ && !punctuation(index_, ')')) {
                        if (punctuation(index_, ',')) {
                            ++index_;
                            continue;
                        }
                        bool uneval              = false;
                        const uint32_t arg_start = index_;
                        if (text(index_) == "=") {
                            uneval = true;
                            ++index_;
                        }
                        if (punctuation(index_, '{')) {
                            expr.operands.push_back(parseBlock());
                        } else {
                            expr.operands.push_back(parseExpression());
                        }
                        expr.argIsUnevaluated.push_back(uneval);
                        expr.argSpans.push_back(range(arg_start, index_));
                        if (punctuation(index_, ','))
                            ++index_;
                        else if (!punctuation(index_, ')'))
                            break;
                    }
                    if (punctuation(index_, ')'))
                        ++index_;
                    else
                        snapshot_.diagnostics_.push_back(
                            {range(start, index_), "expected ')' after macro arguments"});
                }
            }
            expr.span = range(start, index_);
            return parsePostfix(addExpression(std::move(expr)), start);
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
        // A dotted name followed immediately by `{` is a qualified struct
        // literal (`std.counter.Counter{...}`). Call chains such as
        // `std.io.console.println(...)` remain ordinary Field nodes so
        // sema/HIR method resolution keeps the existing receiver logic.
        if (!suppress_struct_literal_ && !is_literal) {
            uint32_t probe             = index_;
            std::string qualified_name = expression.text;
            bool ends_at_brace         = false;
            while (probe + 1U < token_count_ && punctuation(probe, '.') &&
                   (snapshot_.tokens_[probe + 1U].kind == TokenKind::Identifier ||
                    snapshot_.tokens_[probe + 1U].kind == TokenKind::Keyword)) {
                qualified_name += ".";
                qualified_name += std::string(text(probe + 1U));
                probe += 2U;
            }
            ends_at_brace = punctuation(probe, '{');
            if (ends_at_brace && qualified_name != expression.text) {
                index_ = probe;
                ++index_; // '{'
                Expression struct_lit;
                struct_lit.kind            = ExprKind::StructLiteral;
                struct_lit.scope           = current_scope_;
                struct_lit.text            = std::move(qualified_name);
                bool saw_named             = false;
                bool saw_positional        = false;
                const auto parseFieldValue = [&]() -> ExprId {
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
                        ++index_; // ':'
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
        }
        // Struct literal: Name { field: expr, ... }
        // Only treat as struct literal when immediately followed by '{' after a Name.
        if (!suppress_struct_literal_ && !is_literal && punctuation(index_, '{')) {
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
                const bool is_named = (snapshot_.tokens_[index_].kind == TokenKind::Identifier ||
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
bool AstLowerer::isOperatorToken(std::string_view op) const noexcept {
    return index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Operator &&
           text(index_) == op;
}

/// True when the current `<` opens a generic application `name<A, B>(...)`
/// or a generic struct literal `name<A, B>{ ... }`: the matching `>`
/// (counting nested `<`/`>`) must be immediately followed by `(` or `{`.
/// Plain comparisons like `a < b` do not match because they are not followed
/// by a call or literal brace.
bool AstLowerer::isGenericApplication() const noexcept {
    if (!isOperatorToken("<"))
        return false;
    int depth = 0;
    for (uint32_t i = index_; i < token_count_; ++i) {
        const auto &token = snapshot_.tokens_[i];
        if (token.kind == TokenKind::Operator) {
            if (text(i) == "<")
                ++depth;
            else if (text(i) == ">") {
                --depth;
                if (depth == 0) {
                    if (i + 1U >= token_count_ ||
                        snapshot_.tokens_[i + 1U].kind != TokenKind::Punctuation)
                        return false;
                    const auto next = text(i + 1U);
                    return next == "(" || next == "{";
                }
            }
        } else if (token.kind == TokenKind::Punctuation && text(i) == "(") {
            // Nested calls/grouping inside the args would break the heuristic; only
            // accept angle brackets without stray parens.
            return false;
        } else if (token.kind == TokenKind::End) {
            return false;
        }
    }
    return false;
}
bool AstLowerer::isKeywordToken(std::string_view word) const noexcept {
    return index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Keyword &&
           text(index_) == word;
}

bool AstLowerer::isVisibilityPrefix() const noexcept {
    if (index_ >= token_count_)
        return false;
    const auto word = text(index_);
    return word == "pub" || word == "mod";
}

/// Parses a valid function-kind prefix for this lowerer's current position:
/// `fn`, `const fn`, `raw fn`, `extern fn`, or `flow fn`.  Returns false when
/// the current token is not `fn` or a kind prefix followed by `fn`.
std::optional<FunctionKind> AstLowerer::functionKindPrefix() {
    return ::zith::frontend::functionKindPrefix(snapshot_, index_, token_count_);
}
ExprId AstLowerer::parseAttributeValue() {
    const uint32_t start = index_;
    const auto primary   = parsePrimary();
    return parsePostfix(primary, start);
}
ExprId AstLowerer::parsePostfix(ExprId result, uint32_t start) {
    while (punctuation(index_, '(') || punctuation(index_, '[') || punctuation(index_, '.') ||
           isOperatorToken("->") || isKeywordToken("as") || isGenericApplication()) {
        // Dot field access: expr.field
        if (punctuation(index_, '.')) {
            if (range_mode_ && punctuation(index_ + 1U, '.'))
                break; // leave `lo..hi` for the when-case range pattern
            ++index_;
            if (index_ < token_count_ && (snapshot_.tokens_[index_].kind == TokenKind::Identifier ||
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
            if (isGenericApplication()) {
                const uint32_t gen_start = index_;
                ++index_; // '<'
                Expression generic_call;
                generic_call.kind  = ExprKind::Call;
                generic_call.scope = current_scope_;
                generic_call.operands.push_back(result);
                while (index_ < token_count_ && !isOperatorToken(">")) {
                    generic_call.genericArgs.push_back(parseType());
                    if (!punctuation(index_, ','))
                        break;
                    ++index_;
                }
                if (isOperatorToken(">"))
                    ++index_;
                else
                    snapshot_.diagnostics_.push_back(
                        {range(gen_start, index_), "expected '>' after generic arguments"});
                if (punctuation(index_, '(')) {
                    ++index_;
                    while (index_ < token_count_ && !punctuation(index_, ')')) {
                        generic_call.operands.push_back(parseCallArgument());
                        if (!punctuation(index_, ','))
                            break;
                        ++index_;
                    }
                    if (punctuation(index_, ')'))
                        ++index_;
                    else
                        snapshot_.diagnostics_.push_back(
                            {range(gen_start, index_), "expected ')'"});
                }
                generic_call.span = range(start, index_);
                result            = addExpression(std::move(generic_call));
                continue;
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
            if (index_ < token_count_ && (snapshot_.tokens_[index_].kind == TokenKind::Identifier ||
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
        // Generic application `name<A, B>(args)`: the angle-bracket list holds type
        // expressions recorded on the Call node. The same list on a `{ ... }`
        // literal is carried by StructLiteral for sema and HIR.
        if (isGenericApplication()) {
            const uint32_t gen_start = start;
            ++index_; // '<'
            Expression call;
            call.kind  = ExprKind::Call;
            call.scope = current_scope_;
            call.operands.push_back(result);
            while (index_ < token_count_ && !isOperatorToken(">")) {
                call.genericArgs.push_back(parseType());
                if (!punctuation(index_, ','))
                    break;
                ++index_;
            }
            if (isOperatorToken(">"))
                ++index_;
            else
                snapshot_.diagnostics_.push_back(
                    {range(gen_start, index_), "expected '>' after generic arguments"});
            const bool generic_struct_literal =
                !suppress_struct_literal_ && result && punctuation(index_, '{');
            if (generic_struct_literal) {
                // Generic struct literal: `Pair<i32, f64>{ left: 1, right: 2.0 }`
                // keeps the generic type arguments on a StructLiteral node.
                const std::string struct_name = result.value <= snapshot_.expressions_.size()
                                                    ? snapshot_.expressions_[result.value - 1U].text
                                                    : std::string{};
                Expression struct_lit;
                struct_lit.kind             = ExprKind::StructLiteral;
                struct_lit.scope            = current_scope_;
                struct_lit.text             = struct_name;
                struct_lit.genericArgs      = std::move(call.genericArgs);
                const uint32_t struct_start = index_;
                ++index_; // '{'
                while (index_ < token_count_ && !punctuation(index_, '}')) {
                    if (punctuation(index_, ',')) {
                        ++index_;
                        continue;
                    }
                    const bool is_named =
                        (snapshot_.tokens_[index_].kind == TokenKind::Identifier ||
                         snapshot_.tokens_[index_].kind == TokenKind::Keyword) &&
                        punctuation(index_ + 1U, ':');
                    if (is_named) {
                        const std::string field_name = std::string(text(index_++));
                        ++index_; // ':'
                        struct_lit.field_names.push_back(field_name);
                        struct_lit.operands.push_back(text(index_) == "_" ? parsePrimary()
                                                                          : parseExpression());
                    } else {
                        struct_lit.operands.push_back(parseExpression());
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
                        {range(struct_start, index_),
                         "expected '}' after generic struct literal fields"});
                struct_lit.span = range(gen_start, index_);
                result          = addExpression(std::move(struct_lit));
                continue;
            } else if (punctuation(index_, '(')) {
                ++index_;
                while (index_ < token_count_ && !punctuation(index_, ')')) {
                    call.operands.push_back(parseCallArgument());
                    if (!punctuation(index_, ','))
                        break;
                    ++index_;
                }
                if (punctuation(index_, ')'))
                    ++index_;
                else
                    snapshot_.diagnostics_.push_back({range(gen_start, index_), "expected ')'"});
            } else {
                snapshot_.diagnostics_.push_back(
                    {range(gen_start, index_), "expected '(' after generic arguments"});
            }
            call.span = range(gen_start, index_);
            result    = addExpression(std::move(call));
            continue;
        }
        if (punctuation(index_, '[')) {
            const uint32_t index_start = start;
            ++index_;
            const bool saved_range_mode = range_mode_;
            range_mode_                 = true;
            const ExprId lower          = parseExpression();
            range_mode_                 = saved_range_mode;
            Expression indexing;
            indexing.kind  = ExprKind::Index;
            indexing.scope = current_scope_;
            indexing.operands.push_back(result);
            indexing.operands.push_back(lower);
            if (!punctuation(index_, ']')) {
                if (punctuation(index_, '.') && punctuation(index_ + 1U, '.')) {
                    // `expr[lo..hi]` is an array/slice view, not a plain index.
                    Expression slicing;
                    slicing.kind  = ExprKind::SliceRange;
                    slicing.text  = "..";
                    slicing.scope = current_scope_;
                    slicing.operands.push_back(result);
                    slicing.operands.push_back(lower);
                    index_ += 2; // consume `..`
                    slicing.operands.push_back(parseExpression());
                    if (!punctuation(index_, ']'))
                        snapshot_.diagnostics_.push_back(
                            {range(index_start, index_), "expected ']' after slice upper bound"});
                    else
                        ++index_;
                    slicing.span = range(index_start, index_);
                    result       = addExpression(std::move(slicing));
                    continue;
                }
                snapshot_.diagnostics_.push_back(
                    {range(index_start, index_), "expected ']' after index"});
            } else {
                ++index_;
            }
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
            call.operands.push_back(parseCallArgument());
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
        snapshot_.diagnostics_.push_back({range(start, index_),
                                          "failable propagation is not supported in this version",
                                          false, diagnostics::err::UnsupportedSyntax});
        Expression error_expr;
        error_expr.kind  = ExprKind::Error;
        error_expr.scope = current_scope_;
        error_expr.span  = range(start, index_);
        result           = addExpression(std::move(error_expr));
    }
    return result;
}
int AstLowerer::precedence(std::string_view op) noexcept {
    if (isAssignmentOp(op))
        return 1;
    if (op == "or")
        return 2;
    if (op == "and")
        return 3;
    if (op == "xor")
        return 4;
    if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=")
        return 5;
    if (op == "|.")
        return 6;
    if (op == "^.")
        return 7;
    if (op == "&.")
        return 8;
    if (op == "<<" || op == ">>")
        return 9;
    if (op == "+" || op == "-")
        return 10;
    if (op == "*" || op == "/" || op == "%")
        return 11;
    return -1;
}
bool AstLowerer::isAssignmentOp(std::string_view op) noexcept {
    return op == "=" || compoundBaseOp(op) != std::string_view{};
}

/// For a compound assignment, the base operator it desugars to. Empty for
/// anything else. The bitwise compounds drop the `.` of their base spelling
/// because there is no ambiguity in assignment position.
std::string_view AstLowerer::compoundBaseOp(std::string_view op) noexcept {
    if (op == "+=")
        return "+";
    if (op == "-=")
        return "-";
    if (op == "*=")
        return "*";
    if (op == "/=")
        return "/";
    if (op == "%=")
        return "%";
    if (op == "<<=")
        return "<<";
    if (op == ">>=")
        return ">>";
    if (op == "&=")
        return "&.";
    if (op == "|=")
        return "|.";
    if (op == "^=")
        return "^.";
    return {};
}
ExprId AstLowerer::parseExpression(int minimum_precedence) {
    if (index_ >= token_count_)
        return {};

    const uint32_t start = index_;
    ExprId left;
    // Prefix `?` fallback/failable propagation is not implemented yet.
    if (snapshot_.tokens_[index_].kind == TokenKind::Operator && text(index_) == "?") {
        const auto op = std::string(text(index_++));
        (void)parseExpression(kUnaryPrecedence); // consume the operand
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
    if (isKeywordToken("raw")) {
        // Raw prefix marks an unchecked operation on the next postfix
        // expression. It is valid on indexing/slicing and on tagged-union
        // extraction (`raw v as T`); a declaration form is disambiguated by
        // parseStatement/parseDeclaration before this point.
        ++index_;
        const ExprId operand = parseExpression(kUnaryPrecedence);
        if (operand.value <= snapshot_.expressions_.size()) {
            auto &child = snapshot_.expressions_[operand.value - 1U];
            if (child.kind == ExprKind::Index || child.kind == ExprKind::SliceRange ||
                child.kind == ExprKind::Cast) {
                child.is_raw = true;
                child.span   = range(start, index_);
                left         = operand;
            } else {
                // `raw items[0].field` applies to the underlying index expression:
                // the postfix chain keeps the rest of the lvalue path intact.
                frontend::ExprId root = operand;
                unsigned guard        = 0;
                while (guard++ < 16U && root.value <= snapshot_.expressions_.size()) {
                    auto &chain = snapshot_.expressions_[root.value - 1U];
                    if (chain.kind != frontend::ExprKind::Field &&
                        chain.kind != frontend::ExprKind::Arrow)
                        break;
                    if (chain.operands.empty())
                        break;
                    root = chain.operands[0];
                }
                if (root.value <= snapshot_.expressions_.size()) {
                    auto &raw_root = snapshot_.expressions_[root.value - 1U];
                    if (raw_root.kind == ExprKind::Index || raw_root.kind == ExprKind::SliceRange) {
                        raw_root.is_raw = true;
                        left            = operand;
                    } else if (raw_root.kind == ExprKind::Name) {
                        // `raw x` is the explicit unchecked read escape for a
                        // binding initialized later in the block, and raw
                        // optional extraction when `x` resolves to `?T`.
                        raw_root.isRawName = true;
                        Expression raw_unary;
                        raw_unary.kind  = ExprKind::Unary;
                        raw_unary.text  = "raw";
                        raw_unary.scope = current_scope_;
                        raw_unary.operands.push_back(operand);
                        raw_unary.span = range(start, index_);
                        left           = addExpression(std::move(raw_unary));
                    } else {
                        snapshot_.diagnostics_.push_back(
                            {range(start, index_),
                             "raw prefix is only valid on an index, slice, or cast expression",
                             false, diagnostics::err::TypeMismatch});
                        Expression raw_error;
                        raw_error.kind  = ExprKind::Error;
                        raw_error.span  = range(start, index_);
                        raw_error.scope = current_scope_;
                        left            = addExpression(std::move(raw_error));
                    }
                } else {
                    left = {};
                }
            }
        } else {
            left = {};
        }
    } else if (isKeywordToken("must")) {
        ++index_;
        Expression unary;
        unary.kind  = ExprKind::Unary;
        unary.text  = "must";
        unary.scope = current_scope_;
        unary.operands.push_back(parseExpression(kUnaryPrecedence));
        unary.span = range(start, index_);
        left       = addExpression(std::move(unary));
    } else if ((snapshot_.tokens_[index_].kind == TokenKind::Operator &&
                (text(index_) == "-" || text(index_) == "&" || text(index_) == "*" ||
                 text(index_) == "~")) ||
               text(index_) == "not") {
        const auto op = std::string(text(index_++));
        Expression unary;
        unary.kind  = ExprKind::Unary;
        unary.text  = op;
        unary.scope = current_scope_;
        unary.operands.push_back(parseExpression(kUnaryPrecedence));
        unary.span = range(start, index_);
        left       = addExpression(std::move(unary));
    } else {
        left = parsePrimary();
    }

    while (index_ < token_count_) {
        // `x is null` and tagged-union `x is Type` sit at comparison precedence.
        if (isKeywordToken("is")) {
            if (5 < minimum_precedence)
                break;
            ++index_;
            Expression is_expr;
            is_expr.scope = current_scope_;
            if (isKeywordToken("null")) {
                ++index_;
                is_expr.kind = ExprKind::IsNull;
                is_expr.operands.push_back(left);
            } else if (const TypeExprId type = parseType()) {
                is_expr.kind = ExprKind::IsType;
                is_expr.operands.push_back(left);
                is_expr.cast_type = type;
            } else {
                is_expr.kind = ExprKind::Error;
                snapshot_.diagnostics_.push_back({range(start, index_),
                                                  "'is' requires 'null' or a member type", false,
                                                  diagnostics::err::UnsupportedSyntax});
            }
            is_expr.span = range(start, index_);
            left         = addExpression(std::move(is_expr));
            continue;
        }
        const bool is_keyword_operator =
            snapshot_.tokens_[index_].kind == TokenKind::Keyword &&
            (text(index_) == "and" || text(index_) == "or" || text(index_) == "xor");
        if (snapshot_.tokens_[index_].kind != TokenKind::Operator && !is_keyword_operator)
            break;
        const auto op = text(index_);
        // `&&` / `||` are lexed only to be rejected here: Zith spells them `and` / `or`.
        if (op == "&&" || op == "||") {
            const std::string spelling(op);
            ++index_;
            (void)parseExpression(); // consume the rhs so no cascading errors follow
            Expression error_expr;
            error_expr.kind  = ExprKind::Error;
            error_expr.text  = spelling;
            error_expr.scope = current_scope_;
            error_expr.span  = range(start, index_);
            snapshot_.diagnostics_.push_back(
                {error_expr.span,
                 "'" + spelling + "' is not a Zith operator; use '" +
                     (spelling == "&&" ? std::string("and") : std::string("or")) + "'",
                 false, diagnostics::err::UnsupportedSyntax});
            left = addExpression(std::move(error_expr));
            continue;
        }
        const int op_priority = precedence(op);
        if (op_priority < minimum_precedence)
            break;
        const std::string spelling(op);
        ++index_;
        // Assignment is right-associative (`a = b = 1` is `a = (b = 1)`), so the rhs is
        // parsed at the same precedence rather than one above it.
        const bool is_assignment = isAssignmentOp(spelling);
        const auto right         = parseExpression(is_assignment ? op_priority : op_priority + 1);
        if (const std::string_view base = compoundBaseOp(spelling); !base.empty()) {
            // `x op= v` desugars to `x = x op v`, so it yields a value exactly like `=`.
            // The lhs expression id is shared by the assignment target and the binary
            // operand: sema types it once, HIR lowers it twice. That is safe for every
            // lvalue form supported today (name, field, arrow, index, deref) because
            // lowering an lvalue is side-effect-free address computation. A future lvalue
            // that can have side effects must spill its address to a slot first.
            Expression operation;
            operation.kind  = ExprKind::Binary;
            operation.text  = std::string(base);
            operation.scope = current_scope_;
            operation.operands.push_back(left);
            operation.operands.push_back(right);
            operation.span      = range(start, index_);
            const ExprId folded = addExpression(std::move(operation));
            Expression assign;
            // The original compound spelling is preserved so `fmt` round-trips it.
            assign.kind  = ExprKind::Assign;
            assign.text  = spelling;
            assign.scope = current_scope_;
            assign.operands.push_back(left);
            assign.operands.push_back(folded);
            assign.span = range(start, index_);
            left        = addExpression(std::move(assign));
            continue;
        }
        Expression binary;
        binary.kind  = spelling == "=" ? ExprKind::Assign : ExprKind::Binary;
        binary.text  = spelling;
        binary.scope = current_scope_;
        binary.operands.push_back(left);
        binary.operands.push_back(right);
        binary.span = range(start, index_);
        left        = addExpression(std::move(binary));
    }
    return static_cast<ExprId>(left);
}
} // namespace zith::frontend
