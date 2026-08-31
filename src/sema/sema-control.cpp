#include "sema/sema-modern-utils.hpp"
#include "sema/sema-modern.hpp"
#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace zith::sema::modern {

void PerModuleSema::inferExpressionTypes() {
    inferExpressionTypesForDecls();
}
void PerModuleSema::inferExpressionTypesForDecls() {
    stateMachineByDecl_.clear();
    stateMachineByReturn_.clear();
    localStateMachineByParent_.clear();
    nextStateMachineId_        = 1;
    bool has_non_template_code = false;
    for (const auto &decl : snapshot.declarations()) {
        // A macro declaration is a template, not code: its body only becomes
        // real code once cloned into a call site.
        if (decl.kind == frontend::DeclKind::Macro)
            continue;
        if (decl.kind != frontend::DeclKind::Import)
            has_non_template_code = true;
        if (decl.kind == frontend::DeclKind::Function &&
            decl.functionKind == frontend::FunctionKind::State && !decl.parentScope) {
            (void)stateMachineIdFor(decl);
        }
        if (decl.kind == frontend::DeclKind::Function && decl.parentScope &&
            decl.functionKind == frontend::FunctionKind::State) {
            // Local states form one machine per parent body, independent of the
            // top-level return-type grouping.
            const auto [it, inserted] =
                localStateMachineByParent_.emplace(decl.parentScope.value, 0U);
            if (inserted)
                it->second = nextStateMachineId_++;
            stateMachineByDecl_[decl.id.value] = it->second;
        }
        currentDeclId_       = decl.id.value;
        currentFunctionKind_ = decl.kind == frontend::DeclKind::Function
                                   ? decl.functionKind
                                   : frontend::FunctionKind::Standard;
        currentBodyScope_    = decl.kind == frontend::DeclKind::Function && decl.body
                                   ? snapshot.expressions()[decl.body.value - 1U].scope
                                   : frontend::ScopeId{};
        movedLocals_.clear();
        escapingPointerExprs_.clear();
        escapingPointerLocals_.clear();
        uninitializedLocals_.clear();
        preinitializedLocals_.clear();
        inStateBody_ = decl.kind == frontend::DeclKind::Function &&
                       decl.functionKind == frontend::FunctionKind::State;
        currentStateMachineId_ =
            inStateBody_ ? (decl.parentScope ? stateMachineIdOf(decl) : stateMachineIdFor(decl))
                         : 0;
        if (decl.kind == frontend::DeclKind::Function) {
            TypeId fn_type     = typeOfDecl(decl.id);
            const auto *fn     = type_table.function(fn_type);
            currentReturnType_ = fn ? fn->result : kInvalidTypeId;
        } else {
            currentReturnType_ = kInvalidTypeId;
        }
        if (decl.body) {
            currentDeclId_ = decl.id.value;
            if (decl.parentScope && decl.functionKind == frontend::FunctionKind::State) {
                currentStateMachineId_ = stateMachineIdOf(decl);
            }
            (void)inferExpr(decl.body);
        }
        if (decl.initializer) {
            (void)inferExpr(decl.initializer);
        }
    }
    if (!has_non_template_code) {
        currentDeclId_         = 0;
        currentBodyScope_      = {};
        currentFunctionKind_   = frontend::FunctionKind::Standard;
        inStateBody_           = false;
        currentStateMachineId_ = 0;
        return;
    }
    // Infer iterator loops before the standalone sweep: the synthetic binding
    // in the loop body must be typed by the iterable before a body read is
    // visited as an independent expression.
    for (const auto &expr : snapshot.expressions()) {
        if (snapshot.isMacroTemplateExpr(expr.id))
            continue;
        if (expr.kind == frontend::ExprKind::ForIn) {
            setExprType(expr.id, inferForIn(expr.id));
        }
    }
    // Also infer standalone expressions, skipping macro template bodies.  The
    // expanded clones of imported state declarations are validated in their
    // own module context, so a detached clone re-validated out of order does
    // not carry this module's state-machine grouping.
    for (const auto &expr : snapshot.expressions()) {
        if (snapshot.isMacroTemplateExpr(expr.id))
            continue;
        if (!typeOfExpr(expr.id))
            (void)inferExpr(expr.id);
    }
    currentDeclId_         = 0;
    currentBodyScope_      = {};
    currentFunctionKind_   = frontend::FunctionKind::Standard;
    inStateBody_           = false;
    currentStateMachineId_ = 0;
}
void PerModuleSema::checkReturnsAndCalls() {
    for (const auto &decl : snapshot.declarations()) {
        if (decl.kind != frontend::DeclKind::Function)
            continue;
        TypeId fn_type = typeOfDecl(decl.id);
        const auto *fn = type_table.function(fn_type);
        if (!fn)
            continue;
        TypeId ret_type = fn->result;
        if (decl.body) {
            TypeId body_type = typeOfExpr(decl.body);
            if (!sameType(body_type, void_type) && ret_type != void_type) {
                const bool implicit_ret_ok =
                    decl.body && type_table.kindOf(resolve(ret_type)) == TypeKind::Opaque
                        ? coerceValue(decl.body, ret_type, body_type)
                        : (decl.body ? coerceValue(decl.body, ret_type, body_type)
                                     : coercesTo(ret_type, body_type));
                if (!implicit_ret_ok) {
                    reportCoercionFailure(snapshot.expressions()[decl.body.value - 1U].span,
                                          ret_type, body_type,
                                          "function body type does not match declared return type");
                }
            } else if (sameType(body_type, void_type) && !exprAlwaysTerminates(decl.body) &&
                       ret_type != void_type && ret_type != error_type) {
                reportCoercionFailure(
                    snapshot.expressions()[decl.body.value - 1U].span, ret_type, body_type,
                    "function body can fall through without returning a value of the declared "
                    "return type");
            }
        }
    }

    for (const auto &expr : snapshot.expressions()) {
        if (expr.kind == frontend::ExprKind::Return) {
            // Return type is checked during inferReturn.
        } else if (expr.kind == frontend::ExprKind::Call) {
            TypeId call_type = typeOfExpr(expr.id);
            (void)call_type;
        }
    }
}
bool PerModuleSema::statementAlwaysTerminates(const frontend::Statement &stmt) const noexcept {
    switch (stmt.kind) {
    case frontend::StmtKind::Return:
    case frontend::StmtKind::Jump:
        return true;
    case frontend::StmtKind::Expression:
        return stmt.expression && exprAlwaysTerminates(stmt.expression);
    default:
        return false;
    }
}
bool PerModuleSema::conditionIsAlwaysLiteralTrue(frontend::ExprId id) const noexcept {
    if (!id || id.value > snapshot.expressions().size())
        return false;
    const auto &condition = snapshot.expressions()[id.value - 1U];
    return condition.kind == frontend::ExprKind::Literal && condition.text == "true";
}
bool PerModuleSema::statementContainsBreak(const frontend::Statement &stmt) const noexcept {
    if (stmt.kind == frontend::StmtKind::Break)
        return true;
    return stmt.expression && exprContainsBreak(stmt.expression);
}
bool PerModuleSema::exprContainsBreak(frontend::ExprId id) const noexcept {
    if (!id || id.value > snapshot.expressions().size())
        return false;
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.kind == frontend::ExprKind::Block) {
        for (const auto stmt_id : expr.statements) {
            if (!stmt_id || stmt_id.value > snapshot.statements().size())
                continue;
            if (statementContainsBreak(snapshot.statements()[stmt_id.value - 1U]))
                return true;
        }
    }
    for (const auto operand : expr.operands) {
        if (exprContainsBreak(operand))
            return true;
    }
    for (const auto statement : expr.statements) {
        if (!statement || statement.value > snapshot.statements().size())
            continue;
        if (statementContainsBreak(snapshot.statements()[statement.value - 1U]))
            return true;
    }
    return false;
}
bool PerModuleSema::blockAlwaysTerminates(frontend::ExprId id) const noexcept {
    if (!id || id.value > snapshot.expressions().size())
        return false;
    for (const frontend::StmtId stmt_id : snapshot.expressions()[id.value - 1U].statements) {
        if (!stmt_id || stmt_id.value > snapshot.statements().size())
            continue;
        const auto &stmt = snapshot.statements()[stmt_id.value - 1U];
        if (statementAlwaysTerminates(stmt))
            return true;
    }
    return false;
}
bool PerModuleSema::exprAlwaysTerminates(frontend::ExprId id) const noexcept {
    if (!id || id.value > snapshot.expressions().size())
        return false;
    const auto &expr = snapshot.expressions()[id.value - 1U];
    switch (expr.kind) {
    case frontend::ExprKind::Block:
        return blockAlwaysTerminates(id);
    case frontend::ExprKind::If:
        if (expr.operands.size() < 3U)
            return false;
        if (!exprAlwaysTerminates(expr.operands[1]))
            return false;
        return exprAlwaysTerminates(expr.operands.size() > 3U ? expr.operands[3]
                                                              : expr.operands[2]);
    case frontend::ExprKind::When:
        if (expr.conditions.empty())
            return false;
        if (expr.conditions.back())
            return false; // no default case
        for (size_t index = 1U; index < expr.operands.size(); ++index) {
            if (!exprAlwaysTerminates(expr.operands[index]))
                return false;
        }
        return true;
    case frontend::ExprKind::While:
        if (expr.operands.empty())
            return false;
        return conditionIsAlwaysLiteralTrue(expr.operands[0]) &&
               !exprContainsBreak(expr.operands[1]);
    case frontend::ExprKind::For:
        if (expr.operands.empty())
            return false;
        return conditionIsAlwaysLiteralTrue(expr.operands[0]) &&
               !exprContainsBreak(expr.operands[1]);
    case frontend::ExprKind::ForIn:
        // A for-in loop is finite unless the body itself prevents termination.
        // A `return`, `jump`, or infinite inner loop can keep every iteration
        // from falling through; a plain final value cannot because a finite
        // iterator will eventually reach that value and then return normally.
        if (expr.operands.size() < 2U || !expr.operands[1])
            return false;
        return blockAlwaysTerminates(expr.operands[1]);
    default:
        return false;
    }
}
TypeId PerModuleSema::inferBlock(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    TypeId last      = void_type;
    if (inStateBody_) {
        // A `jump` consumes the block terminator. The standalone sweep still
        // visits expressions after it, so stop inferring once we have lowered
        // a terminating state transfer.
        bool terminated_by_state_transfer = false;
        std::vector<frontend::StmtId> pending_defers;
        for (const auto &stmt_id : expr.statements) {
            if (!stmt_id)
                continue;
            if (stmt_id.value > snapshot.statements().size())
                continue;
            const auto &stmt = snapshot.statements()[stmt_id.value - 1U];
            if (snapshot.isMacroTemplateStmt(stmt.id))
                continue;
            if (terminated_by_state_transfer)
                break;
            if (stmt.kind == frontend::StmtKind::Expression && stmt.expression) {
                last = inferExpr(stmt.expression);
            } else if (stmt.kind == frontend::StmtKind::Defer) {
                pending_defers.push_back(stmt_id);
                last = void_type;
            } else if (stmt.kind == frontend::StmtKind::Binding) {
                TypeId ann_type = lowerTypeExpr(stmt.binding.type);
                TypeId init_type =
                    stmt.binding.initializer ? inferExpr(stmt.binding.initializer) : invalid_type;
                if (ann_type && stmt.binding.initializer &&
                    !coerceValue(stmt.binding.initializer, ann_type, init_type)) {
                    reportCoercionFailure(stmt.span, ann_type, init_type,
                                          "binding initializer type does not match annotation");
                }
                if (!ann_type && stmt.binding.initializer) {
                    if (resolve(init_type) == null_type) {
                        report(stmt.span, "null requires an optional type annotation",
                               diagnostics::err::TypeMismatch);
                        init_type = error_type;
                    }
                }
                const TypeId existing_type = typeOfLocal(stmt.binding.id);
                if (ann_type || stmt.binding.initializer)
                    setLocalType(stmt.binding.id, ann_type ? ann_type : init_type);
                else if (!existing_type)
                    setLocalType(stmt.binding.id, invalid_type);
                if (stmt.binding.initializer ||
                    preinitializedLocals_.contains(stmt.binding.id.value))
                    uninitializedLocals_.erase(stmt.binding.id.value);
                else
                    uninitializedLocals_.insert(stmt.binding.id.value);
                if (stmt.binding.initializer &&
                    pointerAliasEscapesScope(stmt.binding.initializer)) {
                    const TypeId local_type =
                        stmt.binding.type ? lowerTypeExpr(stmt.binding.type) : invalid_type;
                    const TypeId binding_type =
                        local_type ? local_type : typeOfLocal(stmt.binding.id);
                    const TypeId stripped = type_table.stripQualifiers(binding_type);
                    if (!isPointerStorageType(stripped)) {
                        report(stmt.span,
                               "pointer to local storage cannot escape the current scope",
                               diagnostics::err::PointerEscapesScope);
                    }
                    escapingPointerLocals_.insert(stmt.binding.id.value);
                }
                last = void_type;
            } else if (stmt.kind == frontend::StmtKind::Return) {
                checkReturnStatement(stmt);
                last = void_type;
            } else if (stmt.kind == frontend::StmtKind::Break) {
                checkLoopControl(stmt, true);
                last = void_type;
            } else if (stmt.kind == frontend::StmtKind::Continue) {
                checkLoopControl(stmt, false);
                last = void_type;
            } else if (stmt.kind == frontend::StmtKind::Jump) {
                inferJump(stmt);
                terminated_by_state_transfer = true;
                last                         = void_type;
            } else if (stmt.kind == frontend::StmtKind::Declaration) {
                last = void_type;
            } else if (stmt.kind == frontend::StmtKind::Expression && stmt.expression &&
                       stmt.expression.value <= snapshot.expressions().size() &&
                       snapshot.expressions()[stmt.expression.value - 1U].kind ==
                           frontend::ExprKind::DockCall) {
                // Handled by the regular expression case above; keep the code
                // structure simple and let the fallthrough infer the call.
            }
        }
        for (const auto stmt_id : pending_defers) {
            if (stmt_id && stmt_id.value <= snapshot.statements().size()) {
                const auto &defer_stmt = snapshot.statements()[stmt_id.value - 1U];
                checkDeferStatement(defer_stmt);
                checkDeferCaptures(defer_stmt, expr);
            }
        }
        return last;
    }
    std::vector<frontend::StmtId> pending_defers;
    for (const auto &stmt_id : expr.statements) {
        if (!stmt_id)
            continue;
        if (stmt_id.value > snapshot.statements().size())
            continue;
        const auto &stmt = snapshot.statements()[stmt_id.value - 1U];
        if (snapshot.isMacroTemplateStmt(stmt.id))
            continue;
        if (stmt.kind == frontend::StmtKind::Expression && stmt.expression) {
            last = inferExpr(stmt.expression);
        } else if (stmt.kind == frontend::StmtKind::Defer) {
            pending_defers.push_back(stmt_id);
            last = void_type;
        } else if (stmt.kind == frontend::StmtKind::Binding) {
            TypeId ann_type = lowerTypeExpr(stmt.binding.type);
            TypeId init_type =
                stmt.binding.initializer ? inferExpr(stmt.binding.initializer) : invalid_type;
            if (ann_type && stmt.binding.initializer &&
                !coerceValue(stmt.binding.initializer, ann_type, init_type)) {
                reportCoercionFailure(stmt.span, ann_type, init_type,
                                      "binding initializer type does not match annotation");
            }
            if (!ann_type && stmt.binding.initializer) {
                if (resolve(init_type) == null_type) {
                    report(stmt.span, "null requires an optional type annotation",
                           diagnostics::err::TypeMismatch);
                    init_type = error_type;
                }
            }
            const TypeId existing_type = typeOfLocal(stmt.binding.id);
            // A for-in element binding is typed by the loop before the body is
            // lowered; keep that type rather than marking the synthetic
            // binding untyped.
            if (ann_type || stmt.binding.initializer)
                setLocalType(stmt.binding.id, ann_type ? ann_type : init_type);
            else if (!existing_type)
                setLocalType(stmt.binding.id, invalid_type);
            if (stmt.binding.initializer || preinitializedLocals_.contains(stmt.binding.id.value))
                uninitializedLocals_.erase(stmt.binding.id.value);
            else
                uninitializedLocals_.insert(stmt.binding.id.value);
            if (stmt.binding.initializer && pointerAliasEscapesScope(stmt.binding.initializer)) {
                const TypeId local_type =
                    stmt.binding.type ? lowerTypeExpr(stmt.binding.type) : invalid_type;
                const TypeId binding_type = local_type ? local_type : typeOfLocal(stmt.binding.id);
                const TypeId stripped     = type_table.stripQualifiers(binding_type);
                if (!isPointerStorageType(stripped)) {
                    report(stmt.span, "pointer to local storage cannot escape the current scope",
                           diagnostics::err::PointerEscapesScope);
                }
                escapingPointerLocals_.insert(stmt.binding.id.value);
            }
            last = void_type;
        } else if (stmt.kind == frontend::StmtKind::Return) {
            checkReturnStatement(stmt);
            last = void_type;
        } else if (stmt.kind == frontend::StmtKind::Break) {
            checkLoopControl(stmt, true);
            last = void_type;
        } else if (stmt.kind == frontend::StmtKind::Continue) {
            checkLoopControl(stmt, false);
            last = void_type;
        } else if (stmt.kind == frontend::StmtKind::Jump) {
            report(stmt.span, "jump is only allowed inside a state function",
                   diagnostics::err::UnsupportedSyntax);
            last = void_type;
        } else if (stmt.kind == frontend::StmtKind::Declaration) {
            last = void_type;
        }
    }
    for (const auto stmt_id : pending_defers) {
        if (stmt_id && stmt_id.value <= snapshot.statements().size()) {
            const auto &defer_stmt = snapshot.statements()[stmt_id.value - 1U];
            checkDeferStatement(defer_stmt);
            checkDeferCaptures(defer_stmt, expr);
        }
    }
    return last;
}
void PerModuleSema::checkLoopControl(const frontend::Statement &stmt, bool is_break) {
    const char *kind = is_break ? "break" : "continue";
    if (active_loop_labels_.empty()) {
        report(stmt.span, std::string(kind) + " is only allowed inside a loop",
               diagnostics::err::UnsupportedSyntax);
        return;
    }
    if (!stmt.label.empty()) {
        if (std::find(active_loop_labels_.begin(), active_loop_labels_.end(), stmt.label) ==
            active_loop_labels_.end()) {
            report(stmt.span,
                   std::string(kind) + " label '" + stmt.label + "' does not name an active loop",
                   diagnostics::err::UnsupportedSyntax);
        }
    }
}
bool PerModuleSema::deferBodyHasControlFlow(frontend::ExprId id) const noexcept {
    if (!id || id.value > snapshot.expressions().size())
        return false;
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.kind == frontend::ExprKind::Block) {
        for (const auto stmt_id : expr.statements) {
            if (!stmt_id || stmt_id.value > snapshot.statements().size())
                continue;
            const auto &stmt = snapshot.statements()[stmt_id.value - 1U];
            if (stmt.kind == frontend::StmtKind::Return || stmt.kind == frontend::StmtKind::Break ||
                stmt.kind == frontend::StmtKind::Continue ||
                stmt.kind == frontend::StmtKind::Jump) {
                return true;
            }
            if (stmt.expression && deferBodyHasControlFlow(stmt.expression))
                return true;
        }
        return false;
    }
    for (const auto operand : expr.operands) {
        if (deferBodyHasControlFlow(operand))
            return true;
    }
    return false;
}
bool PerModuleSema::deferStatementHasControlFlow(const frontend::Statement &stmt) const noexcept {
    if (stmt.kind == frontend::StmtKind::Return || stmt.kind == frontend::StmtKind::Break ||
        stmt.kind == frontend::StmtKind::Continue || stmt.kind == frontend::StmtKind::Jump) {
        return true;
    }
    return stmt.expression && deferBodyHasControlFlow(stmt.expression);
}
void PerModuleSema::checkDeferStatement(const frontend::Statement &stmt) {
    if (!stmt.expression) {
        report(stmt.span, "defer requires an expression or block", diagnostics::err::ExpectedExpr);
        return;
    }
    if (deferStatementHasControlFlow(stmt)) {
        report(stmt.span, "deferred body cannot contain return, break, continue, or jump",
               diagnostics::err::UnsupportedSyntax);
    }
    if (stmt.expression.value > snapshot.expressions().size())
        return;
    const auto &expr = snapshot.expressions()[stmt.expression.value - 1U];
    if (expr.kind == frontend::ExprKind::Block) {
        // A cleanup block itself produces no value even when its last
        // statement is a value-producing expression.
        (void)inferExpr(stmt.expression);
        setExprType(stmt.expression, void_type);
    } else {
        (void)inferExpr(stmt.expression);
    }
}
void PerModuleSema::checkDeferCaptures(const frontend::Statement &stmt,
                                       const frontend::Expression &block) {
    if (!stmt.expression)
        return;
    if (pointerAliasEscapesScope(stmt.expression)) {
        report(stmt.span, "pointer to local storage cannot escape the current scope",
               diagnostics::err::PointerEscapesScope);
    }

    struct DirectBinding {
        size_t index  = 0;
        bool has_init = false;
        std::string name;
    };
    std::unordered_map<uint32_t, DirectBinding> direct;
    size_t defer_index = 0;
    bool saw_defer     = false;
    for (size_t index = 0; index < block.statements.size(); ++index) {
        const auto stmt_id = block.statements[index];
        if (!stmt_id || stmt_id.value > snapshot.statements().size())
            continue;
        const auto &candidate = snapshot.statements()[stmt_id.value - 1U];
        if (candidate.id == stmt.id && !saw_defer) {
            defer_index = index;
            saw_defer   = true;
        }
        if (candidate.kind == frontend::StmtKind::Binding && candidate.binding.id) {
            direct[candidate.binding.id.value] = DirectBinding{
                index, candidate.binding.initializer ? true : false, candidate.binding.name};
        }
    }
    if (!saw_defer)
        return;

    const auto hasEarlyExit = [&](const size_t before_index) {
        for (size_t index = 0; index < before_index; ++index) {
            const auto stmt_id = block.statements[index];
            if (!stmt_id || stmt_id.value > snapshot.statements().size())
                continue;
            const auto &before = snapshot.statements()[stmt_id.value - 1U];
            if (before.kind == frontend::StmtKind::Return ||
                before.kind == frontend::StmtKind::Break ||
                before.kind == frontend::StmtKind::Continue ||
                before.kind == frontend::StmtKind::Jump) {
                return true;
            }
            // Control flow nested inside expressions may or may not run before
            // the binding initializer; only direct exits are guaranteed to be
            // an uninitialized-capture hazard per the current validation rule.
            if (before.expression && deferBodyHasControlFlow(before.expression))
                return true;
        }
        return false;
    };

    const auto walk = [&](const auto &self, const frontend::ExprId expr_id) -> void {
        if (!expr_id || expr_id.value > snapshot.expressions().size())
            return;
        const auto &expr = snapshot.expressions()[expr_id.value - 1U];
        if (expr.kind == frontend::ExprKind::Name) {
            const auto *resolved = findResolvedExpr(expr_id);
            if (resolved != nullptr && resolved->local) {
                const auto found = direct.find(resolved->local.value);
                if (found != direct.end() && found->second.index > defer_index) {
                    if (!found->second.has_init || hasEarlyExit(found->second.index)) {
                        report(expr.span,
                               "defer may run before captured binding '" + found->second.name +
                                   "' is initialized",
                               diagnostics::err::UnsupportedSyntax);
                    }
                }
            }
        }
        for (const auto operand : expr.operands)
            self(self, operand);
        for (const auto stmt_id : expr.statements) {
            if (!stmt_id || stmt_id.value > snapshot.statements().size())
                continue;
            const auto &inner_stmt = snapshot.statements()[stmt_id.value - 1U];
            if (inner_stmt.expression)
                self(self, inner_stmt.expression);
            if (inner_stmt.kind == frontend::StmtKind::Binding && inner_stmt.binding.initializer) {
                self(self, inner_stmt.binding.initializer);
            }
        }
    };
    // Traverse macro expansions and raw splices too; they are real deferred
    // code once expanded and resolved.
    walk(walk, stmt.expression);
    const auto &defer_expr = snapshot.expressions()[stmt.expression.value - 1U];
    if (defer_expr.expansion)
        walk(walk, defer_expr.expansion);
}
TypeId PerModuleSema::inferIf(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.size() < 2)
        return error_type;
    (void)inferCondition(expr.operands[0], "if condition must be boolean", expr.span);
    const auto &condition = snapshot.expressions()[expr.operands[0].value - 1U];
    frontend::LocalId narrowed_local;
    TypeId original_local_type   = kInvalidTypeId;
    TypeId narrowed_type         = kInvalidTypeId;
    bool narrow_then             = false;
    bool narrowed_opaque_payload = false;
    if (condition.kind == frontend::ExprKind::IsType && !condition.operands.empty() &&
        condition.cast_type) {
        const auto *resolved = findResolvedExpr(condition.operands[0]);
        if (resolved != nullptr && resolved->local) {
            narrowed_local      = resolved->local;
            original_local_type = typeOfLocal(narrowed_local);
            narrowed_type       = lowerTypeExpr(condition.cast_type);
            const TypeKind local_kind =
                type_table.kindOf(resolve(type_table.stripQualifiers(original_local_type)));
            // `is T` on a tagged union or bare opaque narrows the checked
            // payload. For opaque, reads in the then branch are safe because
            // the HIR emits an unchecked payload extraction after the tag test.
            if (narrowed_type &&
                (local_kind == TypeKind::Opaque || local_kind == TypeKind::Union)) {
                narrow_then             = true;
                narrowed_opaque_payload = local_kind == TypeKind::Opaque;
            }
        }
    } else if (condition.kind == frontend::ExprKind::IsNull && !condition.operands.empty()) {
        const auto *resolved = findResolvedExpr(condition.operands[0]);
        if (resolved != nullptr && resolved->local) {
            const TypeId optional = resolve(typeOfLocal(resolved->local));
            if (const auto *opt = type_table.optional(optional)) {
                narrowed_local      = resolved->local;
                original_local_type = typeOfLocal(narrowed_local);
                narrowed_type       = type_table.stripQualifiers(opt->inner);
                // `x is null` proves the payload type in the `else` branch.
            }
        }
    } else if (condition.kind == frontend::ExprKind::Unary && condition.text == "not" &&
               !condition.operands.empty()) {
        const auto &inner = snapshot.expressions()[condition.operands[0].value - 1U];
        if (inner.kind == frontend::ExprKind::IsNull && !inner.operands.empty()) {
            const auto *resolved = findResolvedExpr(inner.operands[0]);
            if (resolved != nullptr && resolved->local) {
                const TypeId optional = resolve(typeOfLocal(resolved->local));
                if (const auto *opt = type_table.optional(optional)) {
                    narrowed_local      = resolved->local;
                    original_local_type = typeOfLocal(narrowed_local);
                    narrowed_type       = type_table.stripQualifiers(opt->inner);
                    narrow_then         = true;
                }
            }
        }
    }

    if (narrowed_local && narrowed_type && narrow_then) {
        setLocalType(narrowed_local, narrowed_type);
        if (narrowed_opaque_payload) {
            // Standalone Name expressions are inferred later by the sweep
            // without the `if` flow context. Pre-type them so `let x: T =
            // opaque` and body reads see the payload type during the first
            // inference and remain correct afterwards.
            const auto &then_expr = snapshot.expressions()[expr.operands[1].value - 1U];
            for (const auto &body_name : snapshot.expressions()) {
                if (body_name.kind != frontend::ExprKind::Name)
                    continue;
                if (body_name.span.start < then_expr.span.start ||
                    body_name.span.end > then_expr.span.end)
                    continue;
                const auto *name_resolved = findResolvedExpr(body_name.id);
                if (name_resolved != nullptr && name_resolved->local == narrowed_local)
                    typed_map.exprTypes.insert(body_name.id.value, narrowed_type);
            }
        }
    }
    TypeId then_type = inferExpr(expr.operands[1]);
    if (narrowed_local && narrowed_type)
        setLocalType(narrowed_local, original_local_type);
    if (narrowed_local && narrowed_type && !narrow_then)
        setLocalType(narrowed_local, narrowed_type);
    TypeId else_cond_type = void_type;
    TypeId else_type      = void_type;
    if (expr.operands.size() >= 3U)
        else_type =
            expr.operands.size() > 3U ? inferExpr(expr.operands[3]) : inferExpr(expr.operands[2]);
    if (expr.operands.size() > 3U) {
        else_cond_type =
            inferCondition(expr.operands[2], "if condition must be boolean", expr.span);
    }
    if (narrowed_local && narrowed_type) {
        setLocalType(narrowed_local, original_local_type);
    }
    // An `if` without `else` is a statement even when its body has a value; only
    // an `if/else` expression can produce a value for the surrounding expression.
    const frontend::ExprId else_value =
        expr.operands.size() > 3U ? expr.operands[3] : expr.operands[2];
    if (expr.operands.size() < 3U || !else_value)
        return void_type;
    if (sameType(then_type, else_type))
        return then_type;
    return then_type;
}
TypeId PerModuleSema::inferWhile(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (!expr.label.empty()) {
        if (std::find(active_loop_labels_.begin(), active_loop_labels_.end(), expr.label) !=
            active_loop_labels_.end()) {
            report(expr.span, "duplicate loop label '" + expr.label + "'",
                   diagnostics::err::UnsupportedSyntax);
        }
    }
    active_loop_labels_.push_back(expr.label);
    if (!expr.operands.empty()) {
        (void)inferCondition(expr.operands[0], "loop condition must be boolean", expr.span);
    }
    // The body must be inferred too, otherwise locals declared inside the loop never get a type.
    if (expr.operands.size() >= 2U)
        (void)inferExpr(expr.operands[1]);
    active_loop_labels_.pop_back();
    return void_type;
}
TypeId PerModuleSema::inferFor(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (!expr.label.empty()) {
        if (std::find(active_loop_labels_.begin(), active_loop_labels_.end(), expr.label) !=
            active_loop_labels_.end()) {
            report(expr.span, "duplicate loop label '" + expr.label + "'",
                   diagnostics::err::UnsupportedSyntax);
        }
    }
    active_loop_labels_.push_back(expr.label);
    // operands: [cond, body, step].
    if (!expr.operands.empty()) {
        (void)inferCondition(expr.operands[0], "loop condition must be boolean", expr.span);
    }
    if (expr.operands.size() >= 2U && expr.operands[1])
        (void)inferExpr(expr.operands[1]);
    if (expr.operands.size() >= 3U && expr.operands[2])
        (void)inferExpr(expr.operands[2]);
    active_loop_labels_.pop_back();
    return void_type;
}
TypeId PerModuleSema::inferForIn(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.size() < 2U)
        return void_type;
    if (!expr.label.empty()) {
        if (std::find(active_loop_labels_.begin(), active_loop_labels_.end(), expr.label) !=
            active_loop_labels_.end()) {
            report(expr.span, "duplicate loop label '" + expr.label + "'",
                   diagnostics::err::UnsupportedSyntax);
        }
    }
    active_loop_labels_.push_back(expr.label);

    const TypeId iterable_type = inferExpr(expr.operands[0]);
    if (!iterable_type || type_table.kindOf(resolve(iterable_type)) == TypeKind::Error) {
        active_loop_labels_.pop_back();
        return void_type;
    }

    TypeId pointee = resolve(type_table.stripQualifiers(iterable_type));
    if (type_table.kindOf(pointee) == TypeKind::Pointer) {
        if (const auto *ptr = type_table.pointer(pointee)) {
            pointee = resolve(type_table.stripQualifiers(ptr->pointee));
        }
    } else if (type_table.kindOf(pointee) == TypeKind::Optional) {
        if (const auto *opt = type_table.optional(pointee)) {
            pointee = resolve(type_table.stripQualifiers(opt->inner));
            if (type_table.kindOf(pointee) == TypeKind::Pointer) {
                if (const auto *ptr = type_table.pointer(pointee))
                    pointee = resolve(type_table.stripQualifiers(ptr->pointee));
            }
        }
    }

    const StructType *st = type_table.struct_type(pointee);
    if (st == nullptr && (type_table.kindOf(pointee) != TypeKind::Integer &&
                          type_table.kindOf(pointee) != TypeKind::Float &&
                          type_table.kindOf(pointee) != TypeKind::Bool &&
                          type_table.kindOf(pointee) != TypeKind::Char &&
                          type_table.kindOf(pointee) != TypeKind::String &&
                          type_table.kindOf(pointee) != TypeKind::Optional &&
                          type_table.kindOf(pointee) != TypeKind::Slice)) {
        active_loop_labels_.pop_back();
        report(expr.span, "iterated value is not a struct with iterator methods",
               diagnostics::err::TypeMismatch);
        return void_type;
    }
    const std::string owner_name = ownerNameOf(pointee);

    const auto resolved_methods            = findMethodsForOwner(owner_name, "next");
    const frontend::Declaration *next_decl = nullptr;
    session::ModuleKey next_module;
    for (const auto &method : resolved_methods) {
        if (method.decl == nullptr || method.decl->parameters.empty())
            continue;
        if (method.decl->parameters.front().name != "self")
            continue;
        next_decl   = method.decl;
        next_module = method.module;
        break;
    }

    if (next_decl == nullptr)
        report(expr.span,
               "iterator type '" + type_table.typeToString(iterable_type) +
                   "' is missing a 'next' method",
               diagnostics::err::TypeMismatch);

    TypeId element_type    = error_type;
    TypeId union_type      = error_type;
    uint32_t element_index = 0;
    uint32_t end_index     = static_cast<uint32_t>(-1);
    bool found_end         = false;
    bool valid_union       = false;
    TypeId optional_type   = error_type;
    if (next_decl != nullptr) {
        const PerModuleSema *next_sema =
            owner != nullptr ? owner->findModuleSema(next_module) : nullptr;
        const TypeId next_fn =
            next_sema != nullptr ? next_sema->typeOfDecl(next_decl->id) : typeOfDecl(next_decl->id);
        const auto *fn = type_table.function(next_fn);
        if (fn == nullptr) {
            report(expr.span, "iterator 'next' method has no function type",
                   diagnostics::err::TypeMismatch);
        } else {
            const TypeId result = resolve(fn->result);
            const auto *uf      = type_table.union_type(result);
            const auto *opt     = type_table.optional(result);
            if (opt != nullptr) {
                // Canonical protocol: `next(self): ?T` where `null` is End and
                // `Some(T)` is an element. `??T` is `?T` payload so the loop
                // variable itself remains `?T`.
                element_type  = opt->inner;
                optional_type = fn->result;
            } else if (uf == nullptr || !uf->is_tagged) {
                report(expr.span,
                       "iterator 'next' method must return a tagged union with one value member "
                       "and 'End', or an optional '?T'",
                       diagnostics::err::TypeMismatch);
            } else if (uf->members.size() != 2U) {
                report(
                    expr.span,
                    "iterator 'next' return union must have exactly two members: a value and 'End'",
                    diagnostics::err::TypeMismatch);
            } else {
                union_type = fn->result;
                for (uint32_t index = 0; index < uf->members.size(); ++index) {
                    const TypeId member_type = resolve(uf->members[index]);
                    if (sameType(member_type, end_type)) {
                        end_index = index;
                        found_end = true;
                    } else {
                        element_index = index;
                        element_type  = uf->members[index];
                    }
                }
                const bool has_end   = found_end;
                const bool has_value = element_type != error_type;
                if (has_end && has_value) {
                    valid_union = true;
                } else {
                    report(expr.span,
                           "iterator 'next' return union must contain a value member and 'End'",
                           diagnostics::err::TypeMismatch);
                }
            }
        }
    }

    if (next_decl == nullptr || (optional_type == error_type && !valid_union)) {
        active_loop_labels_.pop_back();
        return void_type;
    }

    typed_map.forInNext.insert(id.value, TypedMap::ForInNext{next_module, next_decl->id});
    if (optional_type != error_type) {
        typed_map.forInOptionalType.insert(id.value, optional_type);
    } else {
        typed_map.forInElementIndex.insert(id.value, element_index);
        typed_map.forInEndIndex.insert(id.value, end_index);
        typed_map.forInUnionType.insert(id.value, union_type);
    }

    if (expr.forInBinding) {
        const TypeId ann    = lowerTypeExpr(expr.cast_type);
        const TypeId actual = element_type;
        if (ann && !sameType(ann, actual)) {
            report(expr.span, "iterator element type does not match loop variable annotation",
                   diagnostics::err::TypeMismatch);
            setLocalType(expr.forInBinding, actual);
        } else {
            setLocalType(expr.forInBinding, ann ? ann : actual);
        }
        preinitializedLocals_.insert(expr.forInBinding.value);
    }
    (void)inferExpr(expr.operands[1]);
    active_loop_labels_.pop_back();
    return void_type;
}

} // namespace zith::sema::modern
