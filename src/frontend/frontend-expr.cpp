#include "frontend/ast-lowerer.hpp"

#include "diagnostics/error-codes.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace zith::frontend {

/// Parses one element of a call argument list. `lend`/`view` are only
/// meaningful here, so the ownership annotation is recognized only in this
/// entry point instead of in `parsePrimary`.
ExprId AstLowerer::parseCallArgument(bool *is_spread) {
    const uint32_t start = index_;
    if (is_spread != nullptr)
        *is_spread = false;
    if (index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Dots &&
        text(index_) == "...") {
        ++index_;
        if (is_spread != nullptr)
            *is_spread = true;
    }
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

ExprId AstLowerer::parseExpression(int minimum_precedence) {
    if (index_ >= token_count_)
        return {};

    const uint32_t start = index_;
    ParserDepthGuard depth_guard(*this);
    if (!depth_guard.entered())
        return parserDepthErrorExpression(start);

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
    if (snapshot_.tokens_[index_].kind == TokenKind::Operator &&
        (text(index_) == "++" || text(index_) == "--")) {
        const std::string_view spelling = text(index_);
        ++index_;
        (void)parseExpression(kUnaryPrecedence); // consume the operand
        Expression error_expr;
        error_expr.kind  = ExprKind::Error;
        error_expr.text  = std::string(spelling);
        error_expr.scope = current_scope_;
        error_expr.span  = range(start, index_);
        snapshot_.diagnostics_.push_back(
            {error_expr.span,
             "'" + std::string(spelling) +
                 "' is not a Zith operator; use explicit assignment to an updated value",
             false, diagnostics::err::UnsupportedSyntax});
        return addExpression(std::move(error_expr));
    }
    if (isKeywordToken("raw")) {
        // Raw prefixes mark one operation: a call, index/slice, cast, or raw
        // name read. Declaration forms are disambiguated before expressions.
        ++index_;
        const ExprId operand = parseExpression(kUnaryPrecedence);
        if (operand.value <= snapshot_.expressions_.size()) {
            auto &child = snapshot_.expressions_[operand.value - 1U];
            if (child.kind == ExprKind::Call) {
                child.isRawCall = true;
                child.span      = range(start, index_);
                left            = operand;
            } else if (child.kind == ExprKind::Index || child.kind == ExprKind::SliceRange ||
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
                             "raw prefix is only valid on a call, index, slice, cast, or name "
                             "expression",
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
        const bool range_open_lo = isRangeOpenAt(0);
        if ((isRangeDotAt(0) || range_open_lo) && !no_range_literal_) {
            Expression range_expr;
            range_expr.kind  = ExprKind::Range;
            range_expr.scope = current_scope_;
            range_expr.operands.push_back(left);
            if (range_open_lo) {
                range_expr.openAtLo = true;
                index_ += 2U; // `>` and `..` are two tokens
            } else {
                ++index_; // `..` is one Dots token
            }
            if (isOperatorToken("<")) {
                range_expr.openAtHi = true;
                ++index_;
            }
            range_expr.operands.push_back(parseExpression(kUnaryPrecedence));
            range_expr.span = range(start, index_);
            left            = addExpression(std::move(range_expr));
            continue;
        }
        // `|>` and `also` are the canonical chain operators. Both are left
        // associative and sit below assignment; the stage is parsed with a
        // higher minimum so the next chain stage stays at this level.
        const bool is_pipe       = isOperatorToken("|>");
        const bool is_also_stage = isKeywordToken("also");
        if ((is_pipe || is_also_stage) && 0 >= minimum_precedence) {
            ++index_;
            Expression chain;
            chain.kind  = is_pipe ? ExprKind::Pipe : ExprKind::PipeDo;
            chain.text  = is_pipe ? "|>" : "also";
            chain.scope = current_scope_;
            chain.operands.push_back(left);
            const bool saved_pipe_stage = in_pipe_stage_;
            in_pipe_stage_              = true;
            // A stage may be a named block (`left |> step: { ... }`) or a
            // marked named block (`left |> step: @scoped(a) { ... }`). The name
            // is descriptive; the block keeps every pipeline rule (consumes or
            // observes `it`, control flow stays local to the stage).
            if (index_ + 1U < token_count_ &&
                snapshot_.tokens_[index_].kind == TokenKind::Identifier &&
                punctuation(index_ + 1U, ':') && index_ + 2U < token_count_ &&
                (punctuation(index_ + 2U, '{') || text(index_ + 2U) == "@")) {
                const std::string stage_name = std::string(text(index_));
                index_ += 2U; // consume `name :`
                ExprId stage_marker;
                if (text(index_) == "@")
                    stage_marker = parseExpression();
                if (punctuation(index_, '{')) {
                    ExprId stage_block  = parseBlock();
                    auto &blk           = snapshot_.expressions_[stage_block.value - 1U];
                    blk.block_name      = stage_name;
                    blk.marker          = stage_marker;
                    chain.operands.push_back(stage_block);
                } else {
                    snapshot_.diagnostics_.push_back(
                        {range(start, index_),
                         "expected '{' to open named block '" + stage_name + "'", false,
                         diagnostics::err::ExpectedControlBody});
                    chain.operands.push_back(parseExpression(1));
                }
            } else {
                chain.operands.push_back(parseExpression(1));
            }
            in_pipe_stage_ = saved_pipe_stage;
            chain.span = range(start, index_);
            left       = addExpression(std::move(chain));
            continue;
        }
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
        const bool is_keyword_operator = snapshot_.tokens_[index_].kind == TokenKind::Keyword &&
                                         (text(index_) == "and" || text(index_) == "or" ||
                                          text(index_) == "xor" || text(index_) == "in");
        if (snapshot_.tokens_[index_].kind != TokenKind::Operator && !is_keyword_operator)
            break;
        const std::string_view op = text(index_);
        if (op == "++" || op == "--") {
            const std::string spelling(op);
            ++index_;
            Expression error_expr;
            error_expr.kind  = ExprKind::Error;
            error_expr.text  = spelling;
            error_expr.scope = current_scope_;
            error_expr.span  = range(start, index_);
            snapshot_.diagnostics_.push_back(
                {error_expr.span,
                 "'" + spelling +
                     "' is not a Zith operator; use explicit assignment to an updated value",
                 false, diagnostics::err::UnsupportedSyntax});
            left = addExpression(std::move(error_expr));
            continue;
        }
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
