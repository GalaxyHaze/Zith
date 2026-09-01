#include "sema/sema-modern-utils.hpp"
#include "sema/sema-modern.hpp"
#include <string>

namespace zith::sema::modern {

uint32_t PerModuleSema::stateMachineIdFor(const frontend::Declaration &decl) {
    const auto existing = stateMachineByDecl_.get(decl.id.value);
    if (existing)
        return *existing;
    if (decl.parentScope) {
        const auto local = localStateMachineByParent_.get(decl.parentScope.value);
        if (local && *local != 0U) {
            stateMachineByDecl_[decl.id.value] = *local;
            return *local;
        }
        const uint32_t machine_id = nextStateMachineId_++;
        localStateMachineByParent_.insert(decl.parentScope.value, machine_id);
        stateMachineByDecl_[decl.id.value] = machine_id;
        return machine_id;
    }
    const TypeId fn_type = typeOfDecl(decl.id);
    const auto *fn       = type_table.function(fn_type);
    if (fn == nullptr)
        return 0;
    const uint32_t key = fn->result.intern_seq;
    auto &machine_id   = stateMachineByReturn_[key];
    if (machine_id == 0)
        machine_id = nextStateMachineId_++;
    stateMachineByDecl_[decl.id.value] = machine_id;
    return machine_id;
}
uint32_t PerModuleSema::stateMachineIdOf(const frontend::Declaration &decl) const noexcept {
    const auto existing = stateMachineByDecl_.get(decl.id.value);
    return existing ? *existing : 0U;
}

const frontend::Declaration *
PerModuleSema::declarationForResolved(const session::ResolvedName &resolved) const noexcept {
    return findDeclarationForResolved(*this, resolved);
}
void PerModuleSema::inferDockCall(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    TypeId result    = error_type;
    if (!expr.operands.empty()) {
        const auto *resolved                = findResolvedExpr(expr.operands[0]);
        const frontend::Declaration *target = nullptr;
        TypeId target_type                  = kInvalidTypeId;
        if (resolved != nullptr) {
            target = findDeclarationForResolved(*this, *resolved);
        }
        if (resolved != nullptr)
            target_type = typeOfResolvedName(expr.operands[0]);
        // `dock S(args)` may target either a direct `state` declaration or a
        // value whose type is `state(params): ret`.
        const bool direct_state = target != nullptr &&
                                  target->kind == frontend::DeclKind::Function &&
                                  target->functionKind == frontend::FunctionKind::State;
        const bool state_value =
            target_type &&
            type_table.kindOf(resolve(type_table.stripQualifiers(target_type))) == TypeKind::State;
        if (!direct_state && !state_value) {
            report(expr.span, "dock target must be a state function",
                   diagnostics::err::UnsupportedSyntax);
        } else {
            if (const auto *fn = type_table.function(target_type)) {
                result = fn->result;
                const bool target_is_slice =
                    resolved != nullptr && resolved->isVariadicSlice && !fn->params.empty();
                const size_t slice_index =
                    target_is_slice ? fn->params.size() - 1U : fn->params.size();
                const bool defaults_cover =
                    target != nullptr && expr.operands.size() - 1U < fn->params.size() &&
                    missingArgsHaveDefaults(*target, expr.operands.size() - 1U, 0U, slice_index);
                if (!target_is_slice && expr.operands.size() - 1U != fn->params.size() &&
                    !defaults_cover) {
                    report(expr.span, "dock call arity mismatch", diagnostics::err::NoMatchingFn);
                }
                const bool explicit_slice_arg =
                    target_is_slice && expr.operands.size() - 1U == slice_index + 1U &&
                    type_table.slice(resolve(inferExpr(expr.operands[slice_index + 1U]))) !=
                        nullptr;
                const bool auto_collected =
                    target_is_slice &&
                    (expr.operands.size() - 1U > slice_index + 1U ||
                     (expr.operands.size() - 1U == slice_index + 1U && !explicit_slice_arg));
                if (target_is_slice && expr.operands.size() - 1U < slice_index &&
                    !(target != nullptr &&
                      missingArgsHaveDefaults(*target, expr.operands.size() - 1U, 0U,
                                              slice_index))) {
                    report(expr.span, "dock call arity mismatch", diagnostics::err::NoMatchingFn);
                    result = fn->result;
                } else if (target_is_slice && !auto_collected && !defaults_cover &&
                           expr.operands.size() - 1U != fn->params.size()) {
                    report(expr.span, "dock call arity mismatch", diagnostics::err::NoMatchingFn);
                } else {
                    const size_t checked_params = target_is_slice ? slice_index : fn->params.size();
                    for (size_t index = 0; index < checked_params; ++index) {
                        if (index + 1U < expr.operands.size()) {
                            const TypeId arg_type = inferExpr(expr.operands[index + 1U]);
                            if (!coerceValue(expr.operands[index + 1U], fn->params[index],
                                             arg_type)) {
                                reportCoercionFailure(expr.span, fn->params[index], arg_type,
                                                      "dock argument type mismatch",
                                                      diagnostics::err::NoMatchingFn);
                            }
                        } else if (target != nullptr && index < target->parameters.size() &&
                                   target->parameters[index].defaultValue) {
                            const TypeId default_type =
                                typeOfExpr(target->parameters[index].defaultValue);
                            if (!coerceValue(target->parameters[index].defaultValue,
                                             fn->params[index], default_type)) {
                                reportCoercionFailure(expr.span, fn->params[index], default_type,
                                                      "dock default argument type mismatch",
                                                      diagnostics::err::NoMatchingFn);
                            }
                        }
                    }
                    if (auto_collected)
                        (void)checkVariadicTail(expr.span, expr.operands, fn, slice_index, true);
                }
                setExprType(expr.operands[0], target_type);
                if (direct_state && target != nullptr)
                    setResolvedCallTarget(expr.operands[0],
                                          resolved != nullptr ? resolved->target.module
                                                              : session::ModuleKey{},
                                          target->id);
            }
        }
    }
    setExprType(id, result);
}
void PerModuleSema::inferJump(const frontend::Statement &stmt) {
    if (!inStateBody_) {
        report(stmt.span, "jump is only allowed inside a state function",
               diagnostics::err::UnsupportedSyntax);
        return;
    }
    const auto *resolved =
        stmt.label.empty()
            ? nullptr
            : session::lookupBinding(resolution, stmt.label, currentBodyScope_, snapshot.scopes());
    const frontend::Declaration *target = nullptr;
    if (resolved != nullptr)
        target = findDeclarationForResolved(*this, *resolved);
    if (target == nullptr) {
        report(stmt.span, "jump target must be a state function: '" + stmt.label + "'",
               diagnostics::err::UndefinedIdent);
        return;
    }
    if (target->functionKind != frontend::FunctionKind::State) {
        report(stmt.span, "jump target must be a state function",
               diagnostics::err::UnsupportedSyntax);
        return;
    }
    const uint32_t target_machine = stateMachineIdFor(*target);
    if (target_machine != currentStateMachineId_) {
        report(stmt.span, "jump target must be in the same state machine",
               diagnostics::err::UnsupportedSyntax);
        return;
    }
    const TypeId target_type = typeOfResolvedBinding(*resolved);
    const auto *fn           = type_table.function(target_type);
    if (fn == nullptr)
        return;
    const bool target_is_slice =
        resolved != nullptr && resolved->isVariadicSlice && !fn->params.empty();
    const size_t slice_index = target_is_slice ? fn->params.size() - 1U : fn->params.size();
    const bool defaults_cover =
        stmt.arguments.size() < fn->params.size() &&
        missingArgsHaveDefaults(*target, stmt.arguments.size(), 0U, slice_index);
    if (!target_is_slice && stmt.arguments.size() != fn->params.size() && !defaults_cover) {
        report(stmt.span, "state transition arity mismatch", diagnostics::err::NoMatchingFn);
        return;
    }
    const bool explicit_slice_arg =
        target_is_slice && stmt.arguments.size() == slice_index + 1U &&
        type_table.slice(resolve(inferExpr(stmt.arguments[slice_index]))) != nullptr;
    const bool auto_collected =
        target_is_slice && (stmt.arguments.size() > slice_index + 1U ||
                            (stmt.arguments.size() == slice_index + 1U && !explicit_slice_arg));
    if (target_is_slice && stmt.arguments.size() < slice_index &&
        !missingArgsHaveDefaults(*target, stmt.arguments.size(), 0U, slice_index)) {
        report(stmt.span, "state transition arity mismatch", diagnostics::err::NoMatchingFn);
        return;
    } else if (target_is_slice && !auto_collected && stmt.arguments.size() != fn->params.size()) {
        report(stmt.span, "state transition arity mismatch", diagnostics::err::NoMatchingFn);
        return;
    }
    const size_t checked_params = target_is_slice ? slice_index : fn->params.size();
    for (size_t index = 0; index < checked_params; ++index) {
        if (index < stmt.arguments.size()) {
            const TypeId arg_type = inferExpr(stmt.arguments[index]);
            if (!coerceValue(stmt.arguments[index], fn->params[index], arg_type)) {
                reportCoercionFailure(stmt.span, fn->params[index], arg_type,
                                      "state transition argument type mismatch",
                                      diagnostics::err::NoMatchingFn);
            }
        } else if (index < target->parameters.size() && target->parameters[index].defaultValue) {
            const TypeId default_type = typeOfExpr(target->parameters[index].defaultValue);
            if (!coerceValue(target->parameters[index].defaultValue, fn->params[index],
                             default_type)) {
                reportCoercionFailure(stmt.span, fn->params[index], default_type,
                                      "state transition default argument type mismatch",
                                      diagnostics::err::NoMatchingFn);
            }
        }
    }
    if (auto_collected)
        (void)checkVariadicTail(stmt.span, stmt.arguments, fn, slice_index, true);
}
void PerModuleSema::checkReturnStatement(const frontend::Statement &stmt) {
    if (!stmt.expression) {
        // `return;` in a function that promises a value is still a mismatch.
        if (currentReturnType_ && resolve(currentReturnType_) != void_type &&
            resolve(currentReturnType_) != error_type) {
            report(stmt.span, "return without a value in a function with a declared return type",
                   diagnostics::err::TypeMismatch);
        }
        return;
    }
    const TypeId value = inferExpr(stmt.expression);
    if (pointerAliasEscapesScope(stmt.expression)) {
        report(stmt.span, "pointer to local storage cannot escape the current scope",
               diagnostics::err::PointerEscapesScope);
    }
    if (!currentReturnType_ || !value || value == error_type)
        return;
    if (!coerceValue(stmt.expression, currentReturnType_, value)) {
        reportCoercionFailure(stmt.span, currentReturnType_, value,
                              "return type does not match declared return type");
        return;
    }
    // A slice converted to `*char` is an explicit escape and must be checked
    // after the coercion has recorded the aliased expression.
    if (pointerAliasEscapesScope(stmt.expression)) {
        report(stmt.span, "pointer to local storage cannot escape the current scope",
               diagnostics::err::PointerEscapesScope);
    }
}
TypeId PerModuleSema::inferReturn(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    TypeId value     = expr.operands.empty() ? void_type : inferExpr(expr.operands[0]);
    if (currentReturnType_ && value && value != error_type && !expr.operands.empty() &&
        !coerceValue(expr.operands[0], currentReturnType_, value)) {
        reportCoercionFailure(expr.span, currentReturnType_, value,
                              "return type does not match declared return type");
    } else if (!expr.operands.empty() && pointerAliasEscapesScope(expr.operands[0])) {
        report(expr.span, "pointer to local storage cannot escape the current scope",
               diagnostics::err::PointerEscapesScope);
    }
    return type_table.internName("never", TypeKind::Never);
}

} // namespace zith::sema::modern
