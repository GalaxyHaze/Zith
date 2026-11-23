#include "frontend/ast-lowerer.hpp"

#include "diagnostics/error-codes.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace zith::frontend {

ExprId AstLowerer::parseFunctionLiteral() {
    const uint32_t start = index_;
    // The literal is a no-capture function value. Parse its body in a scope
    // whose parent is the module root, so an enclosing local is not visible:
    // a reference to one is reported as an unknown identifier instead of
    // silently producing a broken function that needs a closure environment.
    const ScopeId literal_scope = current_scope_;
    const ScopeId saved_scope   = current_scope_;
    const bool saved_pipe_stage = in_pipe_stage_;
    current_scope_              = root_scope_;
    in_pipe_stage_              = false;
    const DeclId decl = lowerDeclaration(start, DeclKind::Function, Visibility::Private, {}, {},
                                         false, FunctionKind::Standard, {}, false, true,
                                         literal_scope, current_local_parent_name_, {}, true);
    current_scope_    = saved_scope;
    in_pipe_stage_    = saved_pipe_stage;

    Expression literal;
    literal.kind        = ExprKind::FunctionLiteral;
    literal.scope       = literal_scope;
    literal.declaration = decl;
    literal.span        = range(start, index_);
    return parsePostfix(addExpression(std::move(literal)), start);
}

ExprId AstLowerer::parsePrimary() {
    if (index_ >= token_count_)
        return {};

    const uint32_t start = index_;
    // `fn (params): R { body }` in expression position is a no-capture
    // anonymous function literal. A `fn` in primary position is always the
    // literal form: a declaration `fn` is consumed by the statement/declaration
    // parser before an expression is ever attempted.
    if (isKeywordToken("fn"))
        return parseFunctionLiteral();
    if (in_pipe_stage_ && isKeywordToken("it")) {
        ++index_;
        Expression current;
        current.kind  = ExprKind::PipeCurrent;
        current.text  = "it";
        current.scope = current_scope_;
        current.span  = range(start, index_);
        return parsePostfix(addExpression(std::move(current)), start);
    }
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
                bool is_spread = false;
                dock.operands.push_back(parseCallArgument(&is_spread));
                dock.argIsSpread.push_back(is_spread);
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
                if ((name == "lengthOf" || name == "ptrOf" || name == "assert" || name == "check" ||
                     name == "log") &&
                    punctuation(index_, '(')) {
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
            // Qualified generic struct literal `ns.Box<i64, i64>{ ... }`: the
            // dotted name may be followed by a `<...>` type-argument list before
            // the opening brace.
            uint32_t generic_open  = 0;
            uint32_t generic_close = 0;
            if (qualified_name != expression.text && probe < token_count_ &&
                snapshot_.tokens_[probe].kind == TokenKind::Operator && text(probe) == "<") {
                int depth = 0;
                for (uint32_t scan = probe; scan < token_count_; ++scan) {
                    const auto &tok = snapshot_.tokens_[scan];
                    if (tok.kind == TokenKind::End)
                        break;
                    if (tok.kind != TokenKind::Operator)
                        continue;
                    const auto op = text(scan);
                    if (op == "<") {
                        ++depth;
                    } else if (op == ">") {
                        --depth;
                        if (depth == 0) {
                            generic_open  = probe;
                            generic_close = scan;
                            break;
                        }
                    }
                }
                ends_at_brace = generic_close != 0 && punctuation(generic_close + 1U, '{');
            } else {
                ends_at_brace = punctuation(probe, '{');
            }
            if (ends_at_brace && qualified_name != expression.text) {
                Expression struct_lit;
                struct_lit.kind            = ExprKind::StructLiteral;
                struct_lit.scope           = current_scope_;
                struct_lit.text            = std::move(qualified_name);
                if (generic_close != 0) {
                    index_ = generic_open;
                    ++index_; // '<'
                    while (index_ < token_count_ && !isOperatorToken(">")) {
                        struct_lit.genericArgs.push_back(parseType());
                        if (!punctuation(index_, ','))
                            break;
                        ++index_;
                    }
                    if (isOperatorToken(">"))
                        ++index_;
                    else
                        snapshot_.diagnostics_.push_back(
                            {range(start, index_), "expected '>' after generic arguments"});
                } else {
                    index_ = probe;
                }
                ++index_; // '{'
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
            if (snapshot_.tokens_[index_ + 1U].kind == TokenKind::Dots &&
                text(index_ + 1U) == "..")
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
                        bool is_spread = false;
                        generic_call.operands.push_back(parseCallArgument(&is_spread));
                        generic_call.argIsSpread.push_back(is_spread);
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
                    bool is_spread = false;
                    call.operands.push_back(parseCallArgument(&is_spread));
                    call.argIsSpread.push_back(is_spread);
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
            const bool saved_no_range   = no_range_literal_;
            range_mode_                 = true;
            no_range_literal_           = true;
            const ExprId lower          = parseExpression();
            range_mode_                 = saved_range_mode;
            no_range_literal_           = saved_no_range;
            Expression indexing;
            indexing.kind  = ExprKind::Index;
            indexing.scope = current_scope_;
            indexing.operands.push_back(result);
            indexing.operands.push_back(lower);
            if (!punctuation(index_, ']')) {
                if (snapshot_.tokens_[index_].kind == TokenKind::Dots &&
                    text(index_) == "..") {
                    // `expr[lo..hi]` is an array/slice view, not a plain index.
                    Expression slicing;
                    slicing.kind  = ExprKind::SliceRange;
                    slicing.text  = "..";
                    slicing.scope = current_scope_;
                    slicing.operands.push_back(result);
                    slicing.operands.push_back(lower);
                    ++index_; // consume `..`
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
            bool is_spread = false;
            call.operands.push_back(parseCallArgument(&is_spread));
            call.argIsSpread.push_back(is_spread);
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

} // namespace zith::frontend
