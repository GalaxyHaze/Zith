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

ExprId AstLowerer::parseBlock() {
    const uint32_t start = index_;
    Expression block;
    block.kind           = ExprKind::Block;
    const ScopeId parent = current_scope_;
    if (punctuation(index_, '{'))
        ++index_;
    current_scope_ = addScope(parent, tokenSpan(start));
    block.scope    = current_scope_;
    if (expecting_function_body_scope_) {
        current_function_body_scope_   = block.scope;
        expecting_function_body_scope_ = false;
    }
    while (index_ < token_count_ && !punctuation(index_, '}')) {
        const auto before     = index_;
        const auto statements = parseStatements();
        block.statements.insert(block.statements.end(), statements.begin(), statements.end());
        if (index_ == before)
            ++index_; // never spin on an unconsumed token
    }
    if (!punctuation(index_, '}'))
        snapshot_.diagnostics_.push_back({range(start, index_), "expected '}'"});
    else
        ++index_;
    block.span                    = range(start, index_);
    snapshot_.scopes_.back().span = block.span;
    current_scope_                = parent;
    return addExpression(std::move(block));
}
void AstLowerer::parseArgumentList(std::vector<ExprId> &out) {
    if (!punctuation(index_, '(')) {
        snapshot_.diagnostics_.push_back({tokenSpan(index_), "expected '(' with target arguments"});
        return;
    }
    ++index_;
    while (index_ < token_count_ && !punctuation(index_, ')')) {
        if (punctuation(index_, ',')) {
            ++index_;
            continue;
        }
        out.push_back(parseCallArgument());
        if (punctuation(index_, ','))
            ++index_;
        else if (!punctuation(index_, ')'))
            break;
    }
    if (punctuation(index_, ')'))
        ++index_;
    else
        snapshot_.diagnostics_.push_back(
            {range(index_ - 5, index_), "expected ')' after target arguments"});
}
ExprId AstLowerer::parseElseTail(uint32_t start) {
    if (punctuation(index_, '{'))
        return parseBlock();
    if (index_ < token_count_ && text(index_) == "if") {
        snapshot_.diagnostics_.push_back({tokenSpan(index_ - 1U),
                                          "'else if (cond)' is deprecated; use 'else (cond) { }'",
                                          true, diagnostics::err::DeprecatedSyntax});
        return parseIf();
    }

    Expression tail;
    tail.kind  = ExprKind::If;
    tail.scope = current_scope_;
    tail.span  = range(start, index_);
    if (punctuation(index_, '(')) {
        ++index_;
        tail.operands.push_back(parseConditionExpression());
        if (punctuation(index_, ')'))
            ++index_;
        else if (!punctuation(index_, '{'))
            snapshot_.diagnostics_.push_back(
                {range(start, index_), "expected ')' after condition"});
        if (punctuation(index_, '{'))
            tail.operands.push_back(parseBlock());
        else
            snapshot_.diagnostics_.push_back({range(start, index_), "expected else body"});
        if (index_ < token_count_ && text(index_) == "else") {
            ++index_;
            tail.span           = range(start, index_);
            const ExprId nested = parseElseTail(index_ - 1U);
            tail.span           = range(start, index_);
            tail.operands.push_back(nested);
        }
        tail.span = range(start, index_);
        return addExpression(std::move(tail));
    }

    snapshot_.diagnostics_.push_back({range(start, index_), "expected else body"});
    return addExpression(std::move(tail));
}
ExprId AstLowerer::parseIf() {
    const uint32_t start = index_++;
    if (punctuation(index_, '('))
        ++index_;
    const ExprId condition = parseConditionExpression();
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
        else if (index_ < token_count_ && text(index_) == "if") {
            snapshot_.diagnostics_.push_back(
                {tokenSpan(index_ - 1U), "'else if (cond)' is deprecated; use 'else (cond) { }'",
                 true, diagnostics::err::DeprecatedSyntax});
            expression.operands.push_back(parseIf());
        } else if (punctuation(index_, '(')) {
            const uint32_t else_start = index_ - 1U;
            ++index_; // consume '('
            const ExprId else_condition = parseConditionExpression();
            if (punctuation(index_, ')'))
                ++index_;
            else if (!punctuation(index_, '{'))
                snapshot_.diagnostics_.push_back(
                    {range(start, index_), "expected ')' after condition"});
            const ExprId else_body = punctuation(index_, '{') ? parseBlock() : ExprId{};
            if (!else_body)
                snapshot_.diagnostics_.push_back({range(start, index_), "expected else body"});
            if (index_ < token_count_ && text(index_) == "else") {
                ++index_;
                const ExprId tail = parseElseTail(index_ - 1U);
                Expression chained;
                chained.kind     = ExprKind::If;
                chained.scope    = current_scope_;
                chained.operands = {else_condition, else_body};
                if (tail)
                    chained.operands.push_back(tail);
                chained.span = range(else_start, index_ + 1U);
                expression.operands.push_back(addExpression(std::move(chained)));
            } else {
                expression.operands.push_back(else_condition);
                expression.operands.push_back(else_body);
            }
        } else
            snapshot_.diagnostics_.push_back({range(start, index_), "expected else body"});
    }
    expression.span = range(start, index_);
    return addExpression(std::move(expression));
}
ExprId AstLowerer::parseWhen() {
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
        snapshot_.diagnostics_.push_back({range(start, index_), "expected ')' after when subject"});
    if (punctuation(index_, '{'))
        ++index_;
    else
        snapshot_.diagnostics_.push_back({range(start, index_), "expected '{' after when subject"});

    while (index_ < token_count_ && !punctuation(index_, '}')) {
        if (punctuation(index_, ',')) {
            ++index_;
            continue;
        }
        const uint32_t case_start = index_;
        ExprId condition;
        if (punctuation(index_, '(')) {
            ++index_;
            if (text(index_) == "_" && punctuation(index_ + 1U, ')')) {
                ++index_; // '_'
                ++index_; // ')'
                expression.conditions.push_back(ExprId{});
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
                    range.span = {cond_node.span.start, snapshot_.tokens_[index_ - 1U].span.end};
                    condition  = addExpression(std::move(range));
                }
                if (punctuation(index_, ')'))
                    ++index_;
                else
                    snapshot_.diagnostics_.push_back(
                        {range(case_start, index_), "expected ')' after when case condition"});
                expression.conditions.push_back(condition);
            }
        } else {
            snapshot_.diagnostics_.push_back(
                {range(case_start, index_), "expected '(' before when case condition"});
            while (index_ < token_count_ && !punctuation(index_, ',') && !punctuation(index_, '}'))
                ++index_;
            expression.conditions.push_back(ExprId{});
        }
        if (isOperatorToken("~") && index_ + 1U < token_count_ &&
            snapshot_.tokens_[index_ + 1U].kind == TokenKind::Operator &&
            text(index_ + 1U) == ">") {
            index_ += 2;
        } else {
            snapshot_.diagnostics_.push_back(
                {range(case_start, index_), "expected '~>' after when case condition"});
        }
        expression.operands.push_back(parseExpression()); // case body
        if (punctuation(index_, ','))
            ++index_;
    }
    if (punctuation(index_, '}'))
        ++index_;
    else
        snapshot_.diagnostics_.push_back({range(start, index_), "expected '}' after when cases"});
    expression.span = range(start, index_);
    return parsePostfix(addExpression(std::move(expression)), start);
}
void AstLowerer::applyLoopLabel(ExprId id, std::string_view label) {
    if (!id || id.value > snapshot_.expressions_.size())
        return;
    auto &expr = snapshot_.expressions_[id.value - 1U];
    if (expr.kind == ExprKind::While || expr.kind == ExprKind::For ||
        expr.kind == ExprKind::ForIn) {
        expr.label = std::string(label);
        return;
    }
    if (expr.kind != ExprKind::Block)
        return;
    for (auto it = expr.statements.rbegin(); it != expr.statements.rend(); ++it) {
        if (!*it || it->value > snapshot_.statements_.size())
            continue;
        const auto &inner = snapshot_.statements_[it->value - 1U];
        if (inner.kind == frontend::StmtKind::Expression && inner.expression) {
            applyLoopLabel(inner.expression, label);
            return;
        }
    }
}
ExprId AstLowerer::parseFor() {
    const uint32_t start = index_++;
    Expression expression;
    expression.kind  = ExprKind::While;
    expression.scope = current_scope_;

    if (index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Identifier &&
        punctuation(index_ + 1U, ':')) {
        expression.label = std::string(text(index_));
        index_ += 2;
    }

    if (punctuation(index_, '{')) {
        // `for { ... }` desugars to `while (true) { ... }`.
        Expression always;
        always.kind  = ExprKind::Literal;
        always.text  = "true";
        always.scope = current_scope_;
        always.span  = tokenSpan(start);
        expression.operands.push_back(addExpression(std::move(always)));
    } else if (isKeywordToken("optional")) {
        expression.operands.push_back(parseConditionExpression());
    } else if (isKeywordToken("not")) {
        expression.operands.push_back(parseConditionExpression());
    } else if (punctuation(index_, '(')) {
        ++index_;
        const uint32_t clause_start = index_;
        StmtId init_stmt;
        ExprId init_expr;
        const bool has_var = index_ < token_count_ && text(index_) == "var";
        if (has_var)
            ++index_;
        if (has_var || (index_ + 1U < token_count_ && punctuation(index_ + 1U, ':'))) {
            if (index_ >= token_count_ ||
                (snapshot_.tokens_[index_].kind != TokenKind::Identifier &&
                 snapshot_.tokens_[index_].kind != TokenKind::Keyword)) {
                snapshot_.diagnostics_.push_back(
                    {range(start, index_), "expected a loop variable name"});
            }
            Statement stmt;
            stmt.kind                = StmtKind::Binding;
            stmt.binding.bindingKind = has_var ? BindingKind::Var : BindingKind::Let;
            stmt.binding.id          = LocalId{statementCountLocals_++};
            stmt.binding.name        = std::string(text(index_));
            stmt.binding.span        = tokenSpan(index_++);
            if (punctuation(index_, ':')) {
                ++index_;
                stmt.binding.type = parseType();
            }
            if (index_ < token_count_ && text(index_) == "=") {
                ++index_;
                stmt.binding.initializer = parseExpression();
            }
            stmt.span = range(clause_start, index_);
            const bool second_var_decl =
                punctuation(index_, ',') && index_ + 1U < token_count_ &&
                snapshot_.tokens_[index_ + 1U].kind == TokenKind::Keyword &&
                (text(index_ + 1U) == "var" || text(index_ + 1U) == "let");
            if (second_var_decl) {
                const std::string_view header = text(index_ + 1U) == "var" ? "var" : "let";
                snapshot_.diagnostics_.push_back(
                    {range(start, index_),
                     "for expects (init), (cond), (step) or a single loop variable; '" +
                         std::string(header) +
                         "' declarations cannot be comma-separated in a for header",
                     false, diagnostics::err::UnsupportedSyntax});
                while (index_ < token_count_ && !punctuation(index_, ')'))
                    ++index_;
                if (punctuation(index_, ')'))
                    ++index_;
                if (!punctuation(index_, '{'))
                    snapshot_.diagnostics_.push_back({range(start, index_), "expected for body"});
                expression.kind = ExprKind::Error;
                expression.span = range(start, index_);
                return addExpression(std::move(expression));
            }
            init_stmt = addStatement(std::move(stmt));
        } else if (!punctuation(index_, ',') && !punctuation(index_, ')')) {
            init_expr = parseConditionExpression();
        }

        // Flat 3-clause form: `for (init, cond, step)`.  The first comma
        // inside the paren group is the discriminator from the iterator and
        // conditional forms.
        if (punctuation(index_, ',')) {
            ++index_;
            if (init_expr) {
                Statement stmt;
                stmt.kind       = StmtKind::Expression;
                stmt.expression = init_expr;
                stmt.span       = tokenSpan(clause_start);
                init_stmt       = addStatement(std::move(stmt));
            }
            ExprId cond_expr;
            bool cond_omitted = false;
            if (!punctuation(index_, ',') && !punctuation(index_, ')')) {
                cond_expr = parseConditionExpression();
            } else {
                cond_omitted = true;
            }
            if (!punctuation(index_, ','))
                snapshot_.diagnostics_.push_back(
                    {range(start, index_), "expected ',' between for clauses"});
            else
                ++index_;
            ExprId step_expr;
            if (!punctuation(index_, ')'))
                step_expr = parseConditionExpression();
            if (punctuation(index_, ')'))
                ++index_;
            else
                snapshot_.diagnostics_.push_back(
                    {range(start, index_), "expected ')' after for clauses"});
            if (!punctuation(index_, '{'))
                snapshot_.diagnostics_.push_back({range(start, index_), "expected for body"});
            const ExprId body = parseBlock();
            if (cond_omitted || !bool(cond_expr)) {
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
            ++index_;
            const ExprId iterable = parseExpression();
            if (punctuation(index_, ')'))
                ++index_;
            else
                snapshot_.diagnostics_.push_back(
                    {range(start, index_), "expected ')' after for iterable"});
            if (!punctuation(index_, '{')) {
                snapshot_.diagnostics_.push_back({range(start, index_), "expected for body"});
                expression.kind = ExprKind::Error;
                expression.span = range(start, index_);
                return addExpression(std::move(expression));
            }
            const ExprId body = parseBlock();
            expression.kind   = ExprKind::ForIn;
            expression.span   = range(start, index_);
            expression.operands.push_back(iterable);
            expression.operands.push_back(body);
            const ExprId for_in_id = addExpression(std::move(expression));

            const LocalId local = init_stmt ? snapshot_.statements_[init_stmt.value - 1U].binding.id
                                            : LocalId{statementCountLocals_++};
            auto &for_in        = snapshot_.expressions_[for_in_id.value - 1U];
            for_in.forInBinding = local;
            if (init_stmt) {
                for_in.text      = snapshot_.statements_[init_stmt.value - 1U].binding.name;
                for_in.cast_type = snapshot_.statements_[init_stmt.value - 1U].binding.type;
            }
            if (init_expr) {
                auto &name_expr = snapshot_.expressions_[init_expr.value - 1U];
                if (name_expr.kind != ExprKind::Name) {
                    snapshot_.diagnostics_.push_back(
                        {name_expr.span, "expected a loop variable name before 'in'"});
                    for_in.kind = ExprKind::Error;
                    for_in.span = range(start, index_);
                    return for_in_id;
                }
                for_in.text = name_expr.text;
                // The header name is the loop variable, not a lookup in the
                // enclosing scope: point it at the body block so the
                // synthetic binding below resolves both occurrences.
                name_expr.scope = snapshot_.expressions_[body.value - 1U].scope;
            }

            // Materialize a synthetic `let` in the loop body so local
            // resolution, sema, and HIR slots all see the element binding.
            Statement binding;
            binding.kind                = StmtKind::Binding;
            binding.binding.bindingKind = BindingKind::Let;
            binding.binding.id          = local;
            binding.binding.name        = for_in.text;
            binding.binding.span        = init_stmt
                                              ? snapshot_.statements_[init_stmt.value - 1U].binding.span
                                              : tokenSpan(clause_start);
            binding.binding.type =
                init_stmt ? snapshot_.statements_[init_stmt.value - 1U].binding.type : TypeExprId{};
            binding.span              = binding.binding.span;
            const StmtId binding_stmt = addStatement(std::move(binding));
            for_in.forInBindingStmt   = binding_stmt;
            snapshot_.expressions_[body.value - 1U].statements.insert(
                snapshot_.expressions_[body.value - 1U].statements.begin(), binding_stmt);
            return for_in_id;
        }

        if (punctuation(index_, ')')) {
            ++index_;
            if (punctuation(index_, ',')) {
                // Parenthesized clause form: `for (init), (cond), (step)`.
                ++index_;
                ExprId cond_expr;
                ExprId step_expr;
                auto parseGroup = [&]() -> ExprId {
                    if (!punctuation(index_, '('))
                        return {};
                    ++index_;
                    const ExprId group = parseConditionExpression();
                    if (punctuation(index_, ')'))
                        ++index_;
                    else
                        snapshot_.diagnostics_.push_back(
                            {range(start, index_), "expected ')' after for clause"});
                    if (punctuation(index_, ','))
                        ++index_;
                    return group;
                };
                cond_expr = parseGroup();
                step_expr = parseGroup();
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
                if (init_expr) {
                    Statement stmt;
                    stmt.kind       = StmtKind::Expression;
                    stmt.expression = init_expr;
                    stmt.span       = tokenSpan(clause_start);
                    init_stmt       = addStatement(std::move(stmt));
                }
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

            // Conditional form: `for (cond) { }`.
            if (!bool(init_expr)) {
                snapshot_.diagnostics_.push_back(
                    {range(start, index_), "expected a condition after 'for ('"});
                expression.kind = ExprKind::Error;
                expression.span = range(start, index_);
                return addExpression(std::move(expression));
            }
            expression.operands.push_back(init_expr);
            if (punctuation(index_, '{'))
                expression.operands.push_back(parseBlock());
            else
                snapshot_.diagnostics_.push_back({range(start, index_), "expected for body"});
            expression.span = range(start, index_);
            return addExpression(std::move(expression));
        }

        if (!bool(init_expr))
            snapshot_.diagnostics_.push_back(
                {range(start, index_), "expected a condition or clause after 'for ('"});
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
ExprId AstLowerer::parseWhile() {
    const uint32_t start = index_++;
    snapshot_.diagnostics_.push_back(
        {tokenSpan(start), "'while' is deprecated; use 'for (cond) { }'", true});
    Expression expression;
    expression.kind  = ExprKind::While;
    expression.scope = current_scope_;
    if (index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Identifier &&
        punctuation(index_ + 1U, ':')) {
        expression.label = std::string(text(index_));
        index_ += 2;
    }
    if (punctuation(index_, '('))
        ++index_;
    const ExprId condition = parseConditionExpression();
    if (punctuation(index_, ')'))
        ++index_;
    else if (!punctuation(index_, '{'))
        snapshot_.diagnostics_.push_back({range(start, index_), "expected ')' after condition"});

    expression.operands.push_back(condition);
    if (punctuation(index_, '{'))
        expression.operands.push_back(parseBlock());
    else
        snapshot_.diagnostics_.push_back({range(start, index_), "expected while body"});
    expression.span = range(start, index_);
    return addExpression(std::move(expression));
}
ExprId AstLowerer::parseConditionExpression() {
    const uint32_t start = index_;
    if (isKeywordToken("optional")) {
        snapshot_.diagnostics_.push_back(
            {range(start, index_ + 1U),
             "optional conditions are implicit; use 'if (x)' where x has type ?T", false,
             diagnostics::err::UnsupportedSyntax});
        ++index_;
        const bool saved         = suppress_struct_literal_;
        suppress_struct_literal_ = true;
        const ExprId operand     = parseExpression();
        suppress_struct_literal_ = saved;
        Expression error_expr;
        error_expr.kind  = ExprKind::Error;
        error_expr.text  = "optional";
        error_expr.scope = current_scope_;
        error_expr.span  = range(start, index_);
        error_expr.operands.push_back(operand);
        return addExpression(std::move(error_expr));
    }
    if (isKeywordToken("not") && index_ + 1U < token_count_ && !punctuation(index_ + 1U, ')') &&
        !punctuation(index_ + 1U, ',') && !punctuation(index_ + 1U, '{')) {
        ++index_;
        Expression unary;
        unary.kind               = ExprKind::Unary;
        unary.text               = "not";
        unary.scope              = current_scope_;
        const bool saved         = suppress_struct_literal_;
        suppress_struct_literal_ = true;
        unary.operands.push_back(parseExpression());
        suppress_struct_literal_ = saved;
        unary.span               = range(start, index_);
        return addExpression(std::move(unary));
    }
    return parseExpression();
}
bool AstLowerer::isOperatorAt(uint32_t offset, std::string_view op) const noexcept {
    const auto i = index_ + offset;
    return i < token_count_ && snapshot_.tokens_[i].kind == TokenKind::Operator && text(i) == op;
}
bool AstLowerer::isTagMacroOpen() const noexcept {
    if (!isOperatorToken("<") || isGenericApplication())
        return false;
    if (index_ + 1U >= token_count_)
        return false;
    if (snapshot_.tokens_[index_ + 1U].kind != TokenKind::Identifier)
        return false;
    const auto name = text(index_ + 1U);
    if (name.empty() || !(name.front() >= 'A' && name.front() <= 'Z'))
        return false;
    // The opening tag must close with `>` before the statement ends.
    for (uint32_t i = index_ + 2U; i < token_count_; ++i) {
        const auto &token = snapshot_.tokens_[i];
        if (token.kind == TokenKind::End)
            return false;
        if (token.kind == TokenKind::Operator && text(i) == ">")
            return true;
        if (token.kind == TokenKind::Punctuation && (text(i) == ";" || text(i) == "}"))
            return false;
    }
    return false;
}
ExprId AstLowerer::parseTagMacroCall() {
    const uint32_t start = index_;
    ++index_; // consume '<'
    Expression expr;
    expr.kind           = ExprKind::MacroCall;
    expr.scope          = current_scope_;
    expr.text           = std::string(text(index_));
    const auto nameSpan = tokenSpan(index_);
    ++index_;

    // Attributes: `name: value` pairs separated by commas.
    while (index_ < token_count_ && !isOperatorToken(">")) {
        if (punctuation(index_, ',')) {
            ++index_;
            continue;
        }
        if (snapshot_.tokens_[index_].kind != TokenKind::Identifier ||
            !punctuation(index_ + 1U, ':')) {
            snapshot_.diagnostics_.push_back(
                {tokenSpan(index_), "expected 'name: value' in tag attributes"});
            break;
        }
        expr.attributeNames.push_back(std::string(text(index_)));
        index_ += 2; // name + ':'
        expr.attributes.push_back(parseAttributeValue());
        if (punctuation(index_, ','))
            ++index_;
    }
    if (isOperatorToken(">"))
        ++index_;
    else
        snapshot_.diagnostics_.push_back({range(start, index_), "expected '>' to close tag"});

    // Body: statements until `</`.
    Expression body;
    body.kind             = ExprKind::Block;
    const auto bodyStart  = index_;
    const auto savedScope = current_scope_;
    body.scope            = addScope(savedScope, tokenSpan(bodyStart));
    current_scope_        = body.scope;
    while (index_ < token_count_ && snapshot_.tokens_[index_].kind != TokenKind::End &&
           !(isOperatorToken("<") && isOperatorAt(1U, "/"))) {
        const auto before     = index_;
        const auto statements = parseStatements();
        body.statements.insert(body.statements.end(), statements.begin(), statements.end());
        if (index_ == before)
            ++index_; // never spin on an unconsumed token
    }
    current_scope_ = savedScope;
    body.span      = range(bodyStart, index_);
    expr.operands.push_back(addExpression(std::move(body)));
    expr.argIsUnevaluated.push_back(false);
    expr.argSpans.push_back(range(bodyStart, index_));

    // Closing tag: `</Name>`.
    if (isOperatorToken("<") && isOperatorAt(1U, "/")) {
        index_ += 2;
        if (index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Identifier) {
            if (text(index_) != expr.text) {
                snapshot_.diagnostics_.push_back({tokenSpan(index_),
                                                  "closing tag '" + std::string(text(index_)) +
                                                      "' does not match '" + expr.text + "'",
                                                  false, diagnostics::err::MacroTagMismatch});
            }
            ++index_;
        } else {
            snapshot_.diagnostics_.push_back({tokenSpan(index_), "expected a tag name after '</'"});
        }
        if (isOperatorToken(">"))
            ++index_;
        else
            snapshot_.diagnostics_.push_back(
                {range(start, index_), "expected '>' to close the closing tag"});
    } else {
        snapshot_.diagnostics_.push_back({nameSpan, "unterminated tag '" + expr.text + "'", false,
                                          diagnostics::err::MacroTagMismatch});
    }

    expr.span = range(start, index_);
    return addExpression(std::move(expr));
}
std::vector<StmtId> AstLowerer::parseStatements() {
    const uint32_t start = index_;
    Statement statement;
    statement.kind = StmtKind::Expression;
    if (index_ >= token_count_)
        return {addStatement(std::move(statement))};

    const auto word = text(index_);
    // Loop labels are statement attributes, so `outer: for ...` is parsed
    // here rather than as a `name :` expression. `parseFor()` returns an
    // outer block for 3-clause forms; its final statement is the loop.
    if (word != "let" && word != "var" && word != "const" &&
        snapshot_.tokens_[index_].kind == TokenKind::Identifier && index_ + 1U < token_count_ &&
        punctuation(index_ + 1U, ':') && index_ + 2U < token_count_ &&
        (text(index_ + 2U) == "for" || text(index_ + 2U) == "while")) {
        const std::string loop_label = std::string(word);
        index_ += 2U; // leave the loop keyword in place for the loop parser
        ExprId loop_expr;
        if (text(index_) == "for")
            loop_expr = parseFor();
        else
            loop_expr = parseWhile();
        if (loop_expr && loop_expr.value <= snapshot_.expressions_.size())
            applyLoopLabel(loop_expr, loop_label);
        statement.expression = loop_expr;
        statement.span       = range(start, index_);
        if (punctuation(index_, ';'))
            ++index_;
        return {addStatement(std::move(statement))};
    }
    if (word == "let" || word == "var" || word == "const") {
        statement.kind                = StmtKind::Binding;
        statement.binding.bindingKind = bindingKind(word);
        ++index_;
        if (punctuation(index_, '[')) {
            // `let [x, y] = pack;` desugars to a temporary pack binding
            // followed by positional element bindings. Keeping the pattern
            // as ordinary statements lets resolver/sema/HIR reuse the exact
            // same local-binding machinery without a new IR statement kind.
            ++index_;
            std::vector<std::string> names;
            while (index_ < token_count_ && !punctuation(index_, ']')) {
                if (punctuation(index_, ',')) {
                    ++index_;
                    continue;
                }
                if (snapshot_.tokens_[index_].kind != TokenKind::Identifier) {
                    snapshot_.diagnostics_.push_back({tokenSpan(index_),
                                                      "expected a pack element binding name", false,
                                                      diagnostics::err::ExpectedExpr});
                    ++index_;
                    continue;
                }
                names.push_back(std::string(text(index_++)));
                if (punctuation(index_, ','))
                    ++index_;
                else if (!punctuation(index_, ']')) {
                    snapshot_.diagnostics_.push_back({range(start, index_),
                                                      "expected ',' or ']' in binding pattern",
                                                      false, diagnostics::err::ExpectedExpr});
                    break;
                }
            }
            if (punctuation(index_, ']'))
                ++index_;
            else
                snapshot_.diagnostics_.push_back({range(start, index_),
                                                  "expected ']' after binding pattern", false,
                                                  diagnostics::err::ExpectedExpr});
            TypeExprId annotation = TypeExprId{};
            if (punctuation(index_, ':')) {
                ++index_;
                annotation = parseType();
            }
            if (!(index_ < token_count_ && text(index_) == "=")) {
                snapshot_.diagnostics_.push_back(
                    {range(start, index_), "destructuring binding requires '=' and a pack value",
                     false, diagnostics::err::ExpectedExpr});
                return {addStatement(std::move(statement))};
            }
            ++index_;
            const ExprId initializer = parseExpression();

            Statement pack_stmt;
            pack_stmt.kind                = StmtKind::Binding;
            pack_stmt.binding.bindingKind = statement.binding.bindingKind;
            pack_stmt.binding.id          = LocalId{statementCountLocals_++};
            pack_stmt.binding.name        = "__pack" + std::to_string(pack_stmt.binding.id.value);
            pack_stmt.binding.span        = range(start, index_);
            pack_stmt.binding.type        = annotation;
            pack_stmt.binding.initializer = initializer;
            pack_stmt.span                = range(start, index_);
            const StmtId pack_id          = addStatement(std::move(pack_stmt));

            std::vector<StmtId> lowered;
            lowered.push_back(pack_id);
            for (size_t element = 0; element < names.size(); ++element) {
                Expression object_expr;
                object_expr.kind       = ExprKind::Name;
                object_expr.scope      = current_scope_;
                object_expr.text       = "__pack" + std::to_string(pack_stmt.binding.id.value);
                object_expr.span       = range(start, index_);
                const ExprId object_id = addExpression(std::move(object_expr));
                Expression index_expr;
                index_expr.kind  = ExprKind::Index;
                index_expr.scope = current_scope_;
                index_expr.span  = range(start, index_);
                index_expr.operands.push_back(object_id);
                Expression index_literal;
                index_literal.kind  = ExprKind::Literal;
                index_literal.scope = current_scope_;
                index_literal.text  = std::to_string(element);
                index_literal.span  = range(start, index_);
                index_expr.operands.push_back(addExpression(std::move(index_literal)));
                const ExprId index_id = addExpression(std::move(index_expr));

                Statement element_stmt;
                element_stmt.kind                = StmtKind::Binding;
                element_stmt.binding.bindingKind = statement.binding.bindingKind;
                element_stmt.binding.id          = LocalId{statementCountLocals_++};
                element_stmt.binding.name        = names[element];
                element_stmt.binding.span        = range(start, index_);
                element_stmt.binding.initializer = index_id;
                element_stmt.span                = range(start, index_);
                lowered.push_back(addStatement(std::move(element_stmt)));
            }
            if (punctuation(index_, ';'))
                ++index_;
            return lowered;
        } else if (index_ < token_count_ &&
                   snapshot_.tokens_[index_].kind == TokenKind::Identifier) {
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
    } else if (word == "state") {
        if (current_local_parent_is_state_) {
            snapshot_.diagnostics_.push_back(
                {tokenSpan(index_),
                 "Zith--: 'state' declarations cannot be nested inside another 'state' "
                 "function",
                 false, diagnostics::err::UnsupportedSyntax});
            skipNestedUnsupportedDeclaration();
            statement.kind = StmtKind::Error;
        } else {
            const DeclId id = lowerDeclaration(
                start, DeclKind::Function, Visibility::Private, {}, {}, false, FunctionKind::State,
                {}, false, true, current_function_body_scope_, current_local_parent_name_);
            if (id) {
                statement.kind        = StmtKind::Declaration;
                statement.declaration = id;
            } else {
                statement.kind = StmtKind::Error;
            }
        }
    } else if (word == "fn" || word == "struct" || word == "enum" ||
               ((word == "const" || word == "raw" || word == "extern") &&
                index_ + 1U < token_count_ && text(index_ + 1U) == "fn")) {
        snapshot_.diagnostics_.push_back(
            {tokenSpan(index_),
             "Zith--: nested 'fn', 'struct' and 'enum' declarations are not supported "
             "inside function bodies; only 'state' may be declared here",
             false, diagnostics::err::UnsupportedSyntax});
        skipNestedUnsupportedDeclaration();
        statement.kind = StmtKind::Error;
    } else if (word == "return") {
        statement.kind = StmtKind::Return;
        ++index_;
        if (index_ < token_count_ && !punctuation(index_, ';') && !punctuation(index_, '}')) {
            statement.expression = parseExpression();
            if (index_ < token_count_ && !punctuation(index_, ';') &&
                !punctuation(index_, '}'))
                snapshot_.diagnostics_.push_back({tokenSpan(index_),
                                                  "a return expression must be terminated with ';'",
                                                  false, diagnostics::err::ExpectedSemicolon});
        }
    } else if (word == "break") {
        statement.kind = StmtKind::Break;
        ++index_;
        if (index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Identifier) {
            statement.label = std::string(text(index_));
            ++index_;
        }
    } else if (word == "continue") {
        statement.kind = StmtKind::Continue;
        ++index_;
        if (index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Identifier) {
            statement.label = std::string(text(index_));
            ++index_;
        }
    } else if (word == "marker" || (word == "stackful" && index_ + 1U < token_count_ &&
                                    text(index_ + 1U) == "marker")) {
        statement.kind = StmtKind::Error;
        snapshot_.diagnostics_.push_back(
            {range(start, index_ + (word == "stackful" ? 2U : 1U)),
             "Zith--: 'marker' is not supported; use a 'state' function", false,
             diagnostics::err::UnsupportedSyntax});
        if (word == "stackful")
            ++index_;
        ++index_;
        while (index_ < token_count_ && !punctuation(index_, ';') && !punctuation(index_, '}'))
            ++index_;
    } else if (word == "defer") {
        statement.kind = StmtKind::Defer;
        ++index_;
        if (index_ < token_count_ && punctuation(index_, '{')) {
            statement.expression = parseBlock();
        } else {
            const std::string_view deferred_word =
                index_ < token_count_ ? text(index_) : std::string_view{};
            if (deferred_word == "return" || deferred_word == "break" ||
                deferred_word == "continue" || deferred_word == "jump") {
                const uint32_t control_start = index_;
                while (index_ < token_count_ && !punctuation(index_, ';') &&
                       !punctuation(index_, '}'))
                    ++index_;
                snapshot_.diagnostics_.push_back(
                    {range(control_start, index_),
                     "deferred body cannot contain return, break, continue, or jump", false,
                     diagnostics::err::ExpectedExpr});
                Expression error_expr;
                error_expr.kind      = ExprKind::Error;
                error_expr.scope     = current_scope_;
                error_expr.span      = range(control_start, index_);
                statement.expression = addExpression(std::move(error_expr));
            } else {
                statement.expression = parseExpression();
            }
            if (index_ < token_count_ && !punctuation(index_, ';')) {
                snapshot_.diagnostics_.push_back({range(start, index_),
                                                  "defer expression must end with ';'", false,
                                                  diagnostics::err::ExpectedExpr});
            }
        }
    } else if (word == "jump") {
        statement.kind = StmtKind::Jump;
        ++index_;
        if (index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Identifier) {
            statement.label = std::string(text(index_));
            ++index_;
        } else {
            snapshot_.diagnostics_.push_back({range(start, index_), "expected a jump target name"});
        }
        parseArgumentList(statement.arguments);
    } else if (word == "use") {
        // `use` statements select from a context; not implemented yet.
        snapshot_.diagnostics_.push_back({range(start, index_),
                                          "use statements are not supported in this version", false,
                                          diagnostics::err::UnsupportedSyntax});
        while (index_ < token_count_ && !punctuation(index_, ';'))
            ++index_;
    } else if (isTagMacroOpen()) {
        statement.expression = parseTagMacroCall();
    } else {
        statement.expression = parseExpression();
        // Word-operator sequences such as `1 nop 2` are not implemented yet.
        if (index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Keyword &&
            (text(index_) == "nop" || text(index_) == "prefix" || text(index_) == "suffix" ||
             text(index_) == "infix")) {
            snapshot_.diagnostics_.push_back(
                {range(start, index_), "word operator sequences are not supported in this version",
                 false, diagnostics::err::UnsupportedSyntax});
            while (index_ < token_count_ && !punctuation(index_, ';'))
                ++index_;
        }
    }
    if (punctuation(index_, ';'))
        ++index_;
    statement.span = range(start, index_);
    return {addStatement(std::move(statement))};
}
} // namespace zith::frontend
