#include "sema/hir-lower-modern.hpp"

#include "diagnostics/error-codes.hpp"
#include "sema/hir-lower-utils.hpp"
#include "types/type-kind.hpp"

namespace zith::sema {
namespace modern {

hir::HirExprId HirLowerModern::lowerBlock(const frontend::Expression &expr) {
    cleanup_stack_.push_back(CleanupFrame(arena_));
    pending_defers_.emplace_back();
    hir::HirExprId last = hir::kInvalidHirExpr;
    for (const auto statement : expr.statements) {
        if (!pending_defers_.back().empty() && statement) {
            const auto &stmt = current_module_->frontend->statements()[statement.value - 1U];
            if (stmt.kind == frontend::StmtKind::Return || stmt.kind == frontend::StmtKind::Break ||
                stmt.kind == frontend::StmtKind::Continue ||
                stmt.kind == frontend::StmtKind::Jump) {
                if (!flushPendingDefers())
                    return hir::kInvalidHirExpr;
            }
        }
        if (current_fn_->blocks[current_block_].terminator != hir::kInvalidHirExpr)
            break;
        if (!lowerStatement(statement, last))
            return hir::kInvalidHirExpr;
    }
    if (!flushPendingDefers())
        return hir::kInvalidHirExpr;
    pending_defers_.pop_back();

    auto frame = std::move(cleanup_stack_.back());
    cleanup_stack_.pop_back();
    if (!frame.exprs.empty() &&
        current_fn_->blocks[current_block_].terminator == hir::kInvalidHirExpr) {
        hir::HirCleanup cleanup(arena_);
        cleanup.exprs = std::move(frame.exprs);
        current_fn_->blocks[current_block_].insts.push(addExpr(std::move(cleanup)));
    }
    return last;
}

bool HirLowerModern::flushPendingDefers() {
    if (pending_defers_.empty())
        return true;
    for (const auto id : pending_defers_.back()) {
        if (current_fn_->blocks[current_block_].terminator != hir::kInvalidHirExpr)
            break;
        if (!lowerDeferBody(id))
            return false;
    }
    return true;
}

bool HirLowerModern::lowerDeferBody(frontend::StmtId id) {
    if (!id || current_module_ == nullptr ||
        id.value > current_module_->frontend->statements().size())
        return true;
    const auto &statement = current_module_->frontend->statements()[id.value - 1U];
    if (!statement.expression) {
        diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                      "defer requires an expression or block", {});
        return false;
    }
    if (const auto *body =
            current_module_->frontend->expressions()[statement.expression.value - 1U].kind ==
                    frontend::ExprKind::Block
                ? &current_module_->frontend->expressions()[statement.expression.value - 1U]
                : nullptr;
        body != nullptr) {
        const auto body_id = lowerDeferBlock(*body);
        if (body_id == hir::kInvalidHirExpr) {
            // An empty block still produces a cleanup expression.
            return false;
        }
        if (defer_body_sink_ != nullptr)
            defer_body_sink_->push(body_id);
        else if (!cleanup_stack_.empty())
            cleanup_stack_.back().exprs.push(body_id);
        else
            current_fn_->blocks[current_block_].insts.push(body_id);
        return true;
    }
    const auto deferred = lowerExpr(statement.expression);
    if (deferred == hir::kInvalidHirExpr)
        return false;
    if (defer_body_sink_ != nullptr) {
        // A nested `defer expr;` inside `defer { ... }` runs as an ordinary
        // deferred body statement in source order.
        defer_body_sink_->push(deferred);
    } else if (!cleanup_stack_.empty()) {
        cleanup_stack_.back().exprs.push(deferred);
    } else {
        current_fn_->blocks[current_block_].insts.push(deferred);
    }
    return true;
}

void HirLowerModern::emitCleanupFrom(size_t first) {
    if (first >= cleanup_stack_.size())
        return;
    if (current_fn_->blocks[current_block_].terminator != hir::kInvalidHirExpr)
        return;

    bool any = false;
    for (size_t index = cleanup_stack_.size(); index-- > first;) {
        if (!cleanup_stack_[index].exprs.empty())
            any = true;
    }
    if (!any)
        return;

    hir::HirCleanup cleanup(arena_);
    for (size_t index = cleanup_stack_.size(); index-- > first;) {
        const auto &frame = cleanup_stack_[index].exprs;
        for (size_t inner = frame.size(); inner > 0U; --inner)
            cleanup.exprs.push(frame[inner - 1U]);
    }
    current_fn_->blocks[current_block_].insts.push(addExpr(std::move(cleanup)));
}

hir::HirExprId HirLowerModern::lowerDeferBlock(const frontend::Expression &expr) {
    auto *saved_sink = defer_body_sink_;
    memory::DynArray<hir::HirExprId> sink(arena_);
    defer_body_sink_    = &sink;
    hir::HirExprId last = hir::kInvalidHirExpr;
    for (const auto statement : expr.statements) {
        if (current_fn_->blocks[current_block_].terminator != hir::kInvalidHirExpr)
            break;
        if (!lowerStatement(statement, last)) {
            defer_body_sink_ = saved_sink;
            return hir::kInvalidHirExpr;
        }
    }
    defer_body_sink_ = saved_sink;

    hir::HirCleanup cleanup(arena_);
    for (const auto expr_id : sink)
        cleanup.exprs.push(expr_id);
    return addExpr(std::move(cleanup));
}

hir::HirExprId HirLowerModern::lowerIf(const frontend::Expression &expr, const types::TypeId type) {
    if (expr.operands.size() < 2U)
        return hir::kInvalidHirExpr;

    const auto cond = lowerCondition(expr.operands[0]);
    if (cond == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;

    const bool has_else    = expr.operands.size() >= 3U;
    const bool has_value   = has_else && type != types::kVoidType && type != types::kErrorType;
    const auto result_slot = has_value ? next_slot_++ : hir::kInvalidHirSlot;
    if (has_value)
        current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(result_slot, type));

    const auto then_block  = newBlock();
    const auto else_block  = newBlock();
    const auto merge_block = newBlock();

    hir::HirBranch branch;
    branch.cond       = cond;
    branch.then_block = static_cast<hir::HirDeclId>(then_block);
    branch.else_block = static_cast<hir::HirDeclId>(has_else ? else_block : merge_block);
    setTerminator(addExpr(std::move(branch)));

    frontend::LocalId narrowed_local = {};
    types::TypeId narrowed_type      = types::kInvalidType;
    bool narrow_then                 = false;
    bool narrowed_optional_payload   = false;
    bool narrowed_opaque_payload     = false;
    const auto &condition = current_module_->frontend->expressions()[expr.operands[0].value - 1U];
    const auto makeOptionalNarrowing = [&](frontend::ExprId operand) {
        const auto *resolved = findResolvedExpr(operand);
        if (resolved == nullptr || !resolved->local)
            return;
        const sema::modern::TypeId local_sema = semaTypeOfLocal(resolved->local);
        const auto *optional =
            sema_.typeTable().optional(sema_.typeTable().stripQualifiers(local_sema));
        if (optional == nullptr || sema_.typeTable().kindOf(sema_.typeTable().stripQualifiers(
                                       optional->inner)) == TypeKind::Pointer)
            return;
        narrowed_local            = resolved->local;
        narrowed_type             = lowerType(sema_.typeTable().stripQualifiers(optional->inner));
        narrowed_optional_payload = true;
    };
    if (condition.kind == frontend::ExprKind::IsNull && !condition.operands.empty()) {
        makeOptionalNarrowing(condition.operands[0]);
    } else if (condition.kind == frontend::ExprKind::IsType && !condition.operands.empty() &&
               condition.cast_type) {
        const auto *resolved = findResolvedExpr(condition.operands[0]);
        if (resolved != nullptr && resolved->local) {
            const sema::modern::TypeId local_sema = semaTypeOfLocal(resolved->local);
            if (sema_.typeTable().kindOf(sema_.typeTable().stripQualifiers(local_sema)) ==
                TypeKind::Opaque) {
                narrowed_local          = resolved->local;
                narrowed_type           = lowerType(sema_.typeTable().lowerTypeExpr(
                    *current_module_->frontend, condition.cast_type));
                narrow_then             = true;
                narrowed_opaque_payload = true;
            }
        }
    } else if (condition.kind == frontend::ExprKind::Unary && condition.text == "not" &&
               !condition.operands.empty()) {
        // Frontend exposes `if not (...) { ... }` either as a direct Unary or
        // as a bare `not(...)` call-shaped node depending on parse path. Match
        // the text so both spellings receive payload narrowing.
        frontend::ExprId inner_id = condition.operands[0];
        if (inner_id && inner_id.value <= current_module_->frontend->expressions().size()) {
            const auto &inner =
                current_module_->frontend->expressions()[inner_id.value - 1U];
            if (inner.kind == frontend::ExprKind::IsNull && !inner.operands.empty()) {
                makeOptionalNarrowing(inner.operands[0]);
                narrow_then = true;
            }
        }
    }

    setCurrentBlock(then_block);
    current_fn_->blocks[then_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    cleanup_stack_.push_back(CleanupFrame(arena_));
    const size_t then_cleanup = cleanup_stack_.size() - 1U;
    if (narrowed_local && narrow_then) {
        narrowing_stack_.push_back(Narrowing{narrowed_local, narrowed_type,
                                             narrowed_optional_payload, narrowed_opaque_payload});
    }
    const auto then_value = lowerExpr(expr.operands[1]);
    if (narrowed_local && narrow_then)
        narrowing_stack_.pop_back();
    emitCleanupFrom(then_cleanup);
    cleanup_stack_.pop_back();
    if (has_value && then_value != hir::kInvalidHirExpr &&
        current_fn_->blocks[current_block_].terminator == hir::kInvalidHirExpr) {
        current_fn_->blocks[current_block_].insts.push(emitSlotStore(result_slot, then_value));
    }
    if (current_fn_->blocks[current_block_].terminator == hir::kInvalidHirExpr)
        emitJump(merge_block);

    const bool has_else_condition        = expr.operands.size() > 3U;
    const frontend::ExprId else_value_id = has_else_condition ? expr.operands[3] : expr.operands[2];
    if (has_else) {
        setCurrentBlock(else_block);
        current_fn_->blocks[else_block].insts = memory::DynArray<hir::HirExprId>(arena_);
        cleanup_stack_.push_back(CleanupFrame(arena_));
        const size_t else_cleanup = cleanup_stack_.size() - 1U;
        if (narrowed_local && !narrow_then)
            narrowing_stack_.push_back(Narrowing{
                narrowed_local, narrowed_type, narrowed_optional_payload, narrowed_opaque_payload});
        if (has_else_condition) {
            const auto else_cond = lowerCondition(expr.operands[2]);
            if (else_cond != hir::kInvalidHirExpr) {
                current_fn_->blocks[current_block_].insts.push(else_cond);
                hir::HirBranch else_branch;
                else_branch.cond       = else_cond;
                else_branch.then_block = static_cast<hir::HirDeclId>(merge_block);
                else_branch.else_block = static_cast<hir::HirDeclId>(merge_block);
                setTerminator(addExpr(std::move(else_branch)));
            }
        }
        const auto else_value = lowerExpr(else_value_id);
        if (narrowed_local && !narrow_then)
            narrowing_stack_.pop_back();
        emitCleanupFrom(else_cleanup);
        cleanup_stack_.pop_back();
        if (has_value && else_value != hir::kInvalidHirExpr &&
            current_fn_->blocks[current_block_].terminator == hir::kInvalidHirExpr) {
            current_fn_->blocks[current_block_].insts.push(emitSlotStore(result_slot, else_value));
        }
        if (current_fn_->blocks[current_block_].terminator == hir::kInvalidHirExpr)
            emitJump(merge_block);
    }

    setCurrentBlock(merge_block);
    current_fn_->blocks[merge_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    return has_value ? emitSlotLoad(result_slot, type) : hir::kInvalidHirExpr;
}

hir::HirExprId HirLowerModern::lowerWhen(const frontend::Expression &expr,
                                         const types::TypeId type) {
    const size_t case_count = expr.operands.size() - 1U;
    if (case_count == 0)
        return hir::kInvalidHirExpr;

    // The subject is evaluated exactly once and spilled so every case can compare.
    const auto subject      = lowerExpr(expr.operands[0]);
    const auto subject_type = typeOfExpr(expr.operands[0]);
    if (subject == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    const auto subject_slot = next_slot_++;
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(subject_slot, subject_type));
    current_fn_->blocks[current_block_].insts.push(emitSlotStore(subject_slot, subject));

    const bool has_value   = type != types::kVoidType && type != types::kErrorType;
    const auto result_slot = has_value ? next_slot_++ : hir::kInvalidHirSlot;
    if (has_value)
        current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(result_slot, type));

    const auto merge_block = newBlock();
    size_t chain_block     = current_block_;
    for (size_t i = 0; i < case_count; ++i) {
        const size_t body_index = i + 1U;
        const bool is_last      = i + 1U == case_count;
        const bool is_default   = i < expr.conditions.size() && !expr.conditions[i];

        if (i > 0U) {
            setCurrentBlock(chain_block);
            current_fn_->blocks[chain_block].insts = memory::DynArray<hir::HirExprId>(arena_);
        }

        const auto body_block = newBlock();
        if (is_default) {
            emitJump(body_block);
            if (!is_last)
                chain_block = newBlock(); // unreachable continuation for a misplaced default
        } else {
            const size_t else_block = is_last ? merge_block : newBlock();
            const auto condition =
                lowerWhenCondition(expr.conditions[i], subject_slot, subject_type);
            hir::HirBranch branch;
            branch.cond       = condition;
            branch.then_block = static_cast<hir::HirDeclId>(body_block);
            branch.else_block = static_cast<hir::HirDeclId>(else_block);
            setTerminator(addExpr(std::move(branch)));
            chain_block = else_block;
        }

        setCurrentBlock(body_block);
        current_fn_->blocks[body_block].insts = memory::DynArray<hir::HirExprId>(arena_);
        // An `(f is Member)` case narrows reads of `f` to the member type.
        frontend::LocalId narrowed_local = {};
        types::TypeId narrowed_type      = types::kInvalidType;
        bool narrowed_opaque_payload     = false;
        if (i < expr.conditions.size() && expr.conditions[i]) {
            const auto &condition =
                current_module_->frontend->expressions()[expr.conditions[i].value - 1U];
            if (condition.kind == frontend::ExprKind::IsType && !condition.operands.empty() &&
                condition.cast_type) {
                const auto *resolved = findResolvedExpr(condition.operands[0]);
                if (resolved != nullptr && resolved->local) {
                    narrowed_local = resolved->local;
                    narrowed_type  = lowerType(sema_.typeTable().lowerTypeExpr(
                        *current_module_->frontend, condition.cast_type));
                    const sema::modern::TypeId local_sema = semaTypeOfLocal(resolved->local);
                    narrowed_opaque_payload =
                        sema_.typeTable().kindOf(sema_.typeTable().stripQualifiers(local_sema)) ==
                        TypeKind::Opaque;
                }
            }
        }
        if (narrowed_local) {
            narrowing_stack_.push_back(Narrowing{
                narrowed_local, narrowed_type, /*optionalPayload=*/false, narrowed_opaque_payload});
        }
        const auto body_value = lowerExpr(expr.operands[body_index]);
        if (narrowed_local) {
            narrowing_stack_.pop_back();
        }
        if (has_value && body_value != hir::kInvalidHirExpr &&
            current_fn_->blocks[current_block_].terminator == hir::kInvalidHirExpr) {
            current_fn_->blocks[current_block_].insts.push(emitSlotStore(result_slot, body_value));
        }
        if (current_fn_->blocks[current_block_].terminator == hir::kInvalidHirExpr)
            emitJump(merge_block);
    }

    setCurrentBlock(merge_block);
    current_fn_->blocks[merge_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    return has_value ? emitSlotLoad(result_slot, type) : hir::kInvalidHirExpr;
}

hir::HirExprId HirLowerModern::lowerWhenCondition(frontend::ExprId condition,
                                                  const hir::HirSlotId subject_slot,
                                                  const types::TypeId subject_type) {
    if (!condition || current_module_ == nullptr || current_module_->frontend == nullptr ||
        condition.value > current_module_->frontend->expressions().size())
        return hir::kInvalidHirExpr;
    const auto &node = current_module_->frontend->expressions()[condition.value - 1U];
    if (node.kind == frontend::ExprKind::Range) {
        const auto lower_bound = lowerExpr(node.operands[0]);
        const auto upper_bound = lowerExpr(node.operands[1]);
        const auto subject     = emitSlotLoad(subject_slot, subject_type);
        if (lower_bound == hir::kInvalidHirExpr || upper_bound == hir::kInvalidHirExpr)
            return hir::kInvalidHirExpr;

        hir::HirBinary ge;
        ge.lhs           = subject;
        ge.rhs           = lower_bound;
        ge.op            = hir::HirBinaryOp::Ge;
        ge.type          = types::kBoolType;
        const auto ge_id = addExpr(std::move(ge));

        hir::HirBinary le;
        le.lhs           = subject;
        le.rhs           = upper_bound;
        le.op            = hir::HirBinaryOp::Le;
        le.type          = types::kBoolType;
        const auto le_id = addExpr(std::move(le));

        hir::HirBinary conjunction;
        conjunction.lhs  = ge_id;
        conjunction.rhs  = le_id;
        conjunction.op   = hir::HirBinaryOp::And;
        conjunction.type = types::kBoolType;
        return addExpr(std::move(conjunction));
    }

    // A boolean or optional condition is tested directly through the same
    // implicit test rule as `if`/`while`; any other condition is an equality
    // pattern (`(0)` means `subject == 0`).
    const auto condition_type = typeOfExpr(condition);
    if (types_.kindOf(condition_type) == types::TypeKind::Bool)
        return lowerExpr(condition);
    if (types_.kindOf(condition_type) == types::TypeKind::Optional)
        return lowerOptionalCondition(condition);

    const auto value   = lowerExpr(condition);
    const auto subject = emitSlotLoad(subject_slot, subject_type);
    if (value == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    hir::HirBinary equality;
    equality.lhs  = subject;
    equality.rhs  = value;
    equality.op   = hir::HirBinaryOp::Eq;
    equality.type = types::kBoolType;
    return addExpr(std::move(equality));
}

hir::HirExprId HirLowerModern::lowerWhile(const frontend::Expression &expr) {
    if (expr.operands.size() < 2U)
        return hir::kInvalidHirExpr;

    const auto header_block = newBlock();
    const auto body_block   = newBlock();
    const auto exit_block   = newBlock();

    emitJump(header_block);

    setCurrentBlock(header_block);
    current_fn_->blocks[header_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    const auto cond                         = lowerCondition(expr.operands[0]);
    hir::HirBranch branch;
    branch.cond       = cond;
    branch.then_block = static_cast<hir::HirDeclId>(body_block);
    branch.else_block = static_cast<hir::HirDeclId>(exit_block);
    setTerminator(addExpr(std::move(branch)));

    loop_stack_.push_back({header_block, exit_block, expr.label, 0U});
    setCurrentBlock(body_block);
    current_fn_->blocks[body_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    cleanup_stack_.push_back(CleanupFrame(arena_));
    loop_stack_.back().cleanup_depth = cleanup_stack_.size() - 1U;
    (void)lowerExpr(expr.operands[1]);
    emitCleanupFrom(cleanup_stack_.size() - 1U);
    cleanup_stack_.pop_back();
    // The body may have created nested control flow; the back edge belongs on the
    // block the body actually ended in (current_block_), not necessarily body_block.
    if (current_fn_->blocks[current_block_].terminator == hir::kInvalidHirExpr)
        emitJump(header_block);
    loop_stack_.pop_back();

    setCurrentBlock(exit_block);
    current_fn_->blocks[exit_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    return hir::kInvalidHirExpr;
}

hir::HirExprId HirLowerModern::lowerFor(const frontend::Expression &expr) {
    // operands: [cond, body, step]; step may be invalid.
    if (expr.operands.size() < 2U)
        return hir::kInvalidHirExpr;

    const auto header_block = newBlock();
    const auto body_block   = newBlock();
    const auto step_block   = newBlock();
    const auto exit_block   = newBlock();

    emitJump(header_block);

    setCurrentBlock(header_block);
    current_fn_->blocks[header_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    const auto cond                         = lowerCondition(expr.operands[0]);
    hir::HirBranch branch;
    branch.cond       = cond;
    branch.then_block = static_cast<hir::HirDeclId>(body_block);
    branch.else_block = static_cast<hir::HirDeclId>(exit_block);
    setTerminator(addExpr(std::move(branch)));

    // `continue` runs the step, so the step is the continue target; `break` exits.
    loop_stack_.push_back({step_block, exit_block, expr.label, 0U});
    setCurrentBlock(body_block);
    current_fn_->blocks[body_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    cleanup_stack_.push_back(CleanupFrame(arena_));
    loop_stack_.back().cleanup_depth = cleanup_stack_.size() - 1U;
    (void)lowerExpr(expr.operands[1]);
    emitCleanupFrom(cleanup_stack_.size() - 1U);
    cleanup_stack_.pop_back();
    // The body may have created nested control flow; the step edge belongs on the
    // block the body actually ended in (current_block_), not necessarily body_block.
    if (current_fn_->blocks[current_block_].terminator == hir::kInvalidHirExpr)
        emitJump(step_block);
    loop_stack_.pop_back();

    setCurrentBlock(step_block);
    current_fn_->blocks[step_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    if (expr.operands.size() >= 3U && expr.operands[2]) {
        const auto step = lowerExpr(expr.operands[2]);
        if (step != hir::kInvalidHirExpr)
            current_fn_->blocks[step_block].insts.push(step);
    }
    if (current_fn_->blocks[step_block].terminator == hir::kInvalidHirExpr)
        emitJump(header_block);

    setCurrentBlock(exit_block);
    current_fn_->blocks[exit_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    return hir::kInvalidHirExpr;
}

hir::HirExprId HirLowerModern::lowerForIn(const frontend::Expression &expr) {
    if (expr.operands.size() < 2U)
        return hir::kInvalidHirExpr;

    // The `next` method and the two union member indexes were validated by
    // sema. If any is missing this lowering ran before a successful sema pass.
    const auto *next_ptr =
        current_types_ != nullptr ? current_types_->forInNext.get(expr.id.value) : nullptr;
    const auto *element_index_ptr =
        current_types_ != nullptr ? current_types_->forInElementIndex.get(expr.id.value) : nullptr;
    const auto *end_index_ptr =
        current_types_ != nullptr ? current_types_->forInEndIndex.get(expr.id.value) : nullptr;
    const auto *union_sema_type_ptr =
        current_types_ != nullptr ? current_types_->forInUnionType.get(expr.id.value) : nullptr;
    const auto *optional_sema_type_ptr =
        current_types_ != nullptr ? current_types_->forInOptionalType.get(expr.id.value) : nullptr;
    const bool has_optional = optional_sema_type_ptr != nullptr && !!*optional_sema_type_ptr;
    const bool has_union =
        element_index_ptr != nullptr && end_index_ptr != nullptr && union_sema_type_ptr != nullptr;
    if (next_ptr == nullptr || !next_ptr->decl || !expr.forInBinding ||
        (!has_optional && !has_union)) {
        return hir::kInvalidHirExpr;
    }

    const frontend::DeclId next_decl     = next_ptr->decl;
    const session::ModuleKey next_module = next_ptr->module;
    const auto loop_type                 = typeOfLocal(expr.forInBinding);
    if (loop_type == types::kErrorType)
        return hir::kInvalidHirExpr;

    // Keep the iterable alive for the whole loop. A value receiver is copied
    // into a slot and its address is passed as the implicit self argument; a
    // pointer receiver can pass the loaded value directly.
    const auto iterable = lowerExpr(expr.operands[0]);
    if (iterable == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    const auto iterable_type                 = typeOfExpr(expr.operands[0]);
    const sema::modern::TypeId sema_iterable = semaTypeOfExpr(expr.operands[0]);
    const auto iterable_slot                 = next_slot_++;
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(iterable_slot, iterable_type));
    current_fn_->blocks[current_block_].insts.push(emitSlotStore(iterable_slot, iterable));
    const auto iterable_addr = addExpr(hir::HirSlotAddr{iterable_slot, iterable_type});

    const auto makeMethodCall = [&](const session::ModuleKey &decl_module,
                                    const frontend::DeclId decl_id) -> hir::HirExprId {
        if (decl_module.empty() || !decl_id)
            return hir::kInvalidHirExpr;
        const auto *module_artifact = snapshot_.findModule(decl_module);
        const auto *method_decl =
            module_artifact != nullptr ? findDecl(*module_artifact, decl_id) : nullptr;
        if (module_artifact == nullptr || method_decl == nullptr)
            return hir::kInvalidHirExpr;
        const auto key             = internFunctionKey(interner_, module_artifact->key, decl_id);
        const auto *function_index = function_index_by_key_.get(key);
        if (function_index == nullptr)
            return hir::kInvalidHirExpr;

        memory::DynArray<hir::HirExprId> args(arena_);
        memory::DynArray<types::TypeId> arg_types(arena_);
        (void)sema_iterable;
        const auto self_type = iterable_type;
        args.push(iterable_addr);
        arg_types.push(self_type);

        hir::HirCall call{hir::kInvalidHirExpr, std::move(args), std::move(arg_types)};
        call.resolved_fn = functions_[*function_index].sym_id;
        return addExpr(std::move(call));
    };

    const auto header_block = newBlock();
    const auto body_block   = newBlock();
    const auto exit_block   = newBlock();

    emitJump(header_block);

    // Header: `let step = iter.next(); if (step is End) break;`.
    setCurrentBlock(header_block);
    current_fn_->blocks[header_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    const auto next_call                    = makeMethodCall(next_module, next_decl);
    if (next_call == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    const auto next_slot = next_slot_++;
    const auto next_type =
        has_optional ? lowerType(*optional_sema_type_ptr) : lowerType(*union_sema_type_ptr);
    current_fn_->blocks[header_block].insts.push(emitSlotAlloca(next_slot, next_type));
    current_fn_->blocks[header_block].insts.push(emitSlotStore(next_slot, next_call));
    hir::HirExprId cond_expr = addExpr(hir::HirMakeNone{next_type});
    if (has_optional) {
        // `next(self): ?T`: `None` is End. For aggregate optionals the tag is
        // field 1; for optional pointers the value itself is the sentinel.
        const auto *optional = std::get_if<types::TypeOptional>(&types_.lookup(next_type));
        const bool niche =
            optional != nullptr && types_.kindOf(optional->inner) == types::TypeKind::Ptr;
        if (!niche) {
            const auto next_addr = addExpr(hir::HirSlotAddr{next_slot, next_type});
            cond_expr = addExpr(hir::HirField{next_addr, 1U, types::kBoolType, next_type});
            cond_expr = addExpr(hir::HirUnary{hir::HirUnaryOp::Not, cond_expr, types::kBoolType});
        } else {
            const auto loaded = addExpr(hir::HirSlotLoad{next_slot, next_type});
            cond_expr         = addExpr(hir::HirBinary{loaded, addExpr(hir::HirMakeNone{next_type}),
                                               hir::HirBinaryOp::Eq, types::kBoolType});
        }
    } else {
        const uint32_t end_index = *end_index_ptr;
        const auto union_type    = lowerType(*union_sema_type_ptr);
        const auto *union_def    = std::get_if<types::TypeUnion>(&types_.lookup(union_type));
        const auto *def = union_def != nullptr ? types_.lookupUnionDef(union_def->def_id) : nullptr;
        if (def == nullptr || !def->is_tagged)
            return hir::kInvalidHirExpr;
        const auto tag_type = tagType(types_, static_cast<uint32_t>(def->members.size()));
        const auto tag = addExpr(hir::HirField{addExpr(hir::HirSlotAddr{next_slot, union_type}), 1U,
                                               tag_type, union_type});
        hir::HirUnionCheck check;
        check.value        = tag;
        check.union_type   = union_type;
        check.member_index = end_index;
        cond_expr          = addExpr(std::move(check));
    }
    hir::HirBranch branch;
    branch.cond       = cond_expr;
    branch.then_block = static_cast<hir::HirDeclId>(exit_block);
    branch.else_block = static_cast<hir::HirDeclId>(body_block);
    setTerminator(addExpr(std::move(branch)));

    // Body: loop = step as Element; lower the user block.
    setCurrentBlock(body_block);
    current_fn_->blocks[body_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    auto payload                          = emitSlotLoad(next_slot, next_type);
    if (has_optional) {
        // Optional payload extraction: field 0 for aggregate `?T`; the value
        // itself for nullable pointers. `??T` extracts the outer payload,
        // which is still `?T` and becomes the loop variable type directly.
        const auto *optional = std::get_if<types::TypeOptional>(&types_.lookup(next_type));
        if (optional != nullptr && types_.kindOf(optional->inner) != types::TypeKind::Ptr) {
            payload = addExpr(hir::HirField{addExpr(hir::HirSlotAddr{next_slot, next_type}), 0U,
                                            loop_type, next_type});
        }
    } else {
        hir::HirUnionCast cast;
        cast.value        = payload;
        cast.from         = lowerType(*union_sema_type_ptr);
        cast.to           = loop_type;
        cast.member_index = *element_index_ptr;
        cast.checked      = false;
        payload           = addExpr(std::move(cast));
    }
    const auto loop_slot = localSlot(expr.forInBinding);
    current_fn_->blocks[body_block].insts.push(emitSlotAlloca(loop_slot, loop_type));
    current_fn_->blocks[body_block].insts.push(emitSlotStore(loop_slot, payload));

    // `continue` re-tests by jumping to the header, which calls `next` again.
    loop_stack_.push_back({header_block, exit_block, expr.label, 0U});
    const frontend::StmtId saved_for_in_stmt = current_for_in_binding_stmt_;
    current_for_in_binding_stmt_             = expr.forInBindingStmt;
    current_for_in_binding_local_            = expr.forInBinding;
    cleanup_stack_.push_back(CleanupFrame(arena_));
    loop_stack_.back().cleanup_depth = cleanup_stack_.size() - 1U;
    (void)lowerExpr(expr.operands[1]);
    emitCleanupFrom(cleanup_stack_.size() - 1U);
    cleanup_stack_.pop_back();
    current_for_in_binding_stmt_  = saved_for_in_stmt;
    current_for_in_binding_local_ = {};
    if (current_fn_->blocks[current_block_].terminator == hir::kInvalidHirExpr)
        emitJump(header_block);
    loop_stack_.pop_back();

    setCurrentBlock(exit_block);
    current_fn_->blocks[exit_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    return hir::kInvalidHirExpr;
}

} // namespace modern
} // namespace zith::sema
