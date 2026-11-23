#include "sema/sema-modern-utils.hpp"
#include "sema/sema-modern.hpp"
#include <algorithm>
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
                const ExpandedCallArguments expanded =
                    expandCallArguments(expr.operands, expr.argIsSpread);
                if (expanded.invalidSpread) {
                    const auto &spread = snapshot.expressions()[expanded.invalidSpread.value - 1U];
                    report(spread.span, "spread operand must be a pack or a final variadic slice",
                           diagnostics::err::NoMatchingFn);
                    setExprType(id, result);
                    return;
                }
                const bool target_is_slice =
                    resolved != nullptr && resolved->isVariadicSlice && !fn->params.empty();
                const size_t slice_index =
                    target_is_slice ? fn->params.size() - 1U : fn->params.size();
                std::vector<bool> slice_spreads(expr.argIsSpread.size(), false);
                size_t slice_spread_count = 0U;
                size_t slice_spread_index = ~size_t{0};
                for (const auto &arg : expanded.values) {
                    if (!arg.isSliceForwarding)
                        continue;
                    ++slice_spread_count;
                    slice_spread_index = arg.sourceArgument;
                    if (arg.sourceArgument < slice_spreads.size())
                        slice_spreads[arg.sourceArgument] = true;
                }
                if (slice_spread_count > 1U ||
                    (slice_spread_count != 0U &&
                     (slice_spread_index + 1U != expr.operands.size() - 1U ||
                      !target_is_slice))) {
                    report(expr.span, "slice forwarding requires the final variadic slice argument",
                           diagnostics::err::NoMatchingFn);
                    setExprType(id, result);
                    return;
                }
                const size_t provided_args = expanded.values.size();
                const bool explicit_slice = slice_spread_count != 0U;
                const size_t fixed_provided = explicit_slice ? provided_args - 1U : provided_args;
                const bool defaults_cover =
                    target != nullptr && fixed_provided < (target_is_slice ? slice_index
                                                                           : fn->params.size()) &&
                    missingArgsHaveDefaults(*target, fixed_provided, 0U, slice_index);
                if (!target_is_slice && provided_args != fn->params.size() &&
                    !defaults_cover) {
                    report(expr.span, "dock call arity mismatch", diagnostics::err::NoMatchingFn);
                }
                const VariadicCallPlan plan = makeVariadicCallPlan(expr.span, expr.operands, fn,
                                                                   target_is_slice, slice_index,
                                                                   slice_spreads);
                const bool auto_collected   = plan.autoCollectTail;
                typed_map.variadicCallPlans.insert(expr.id.value, plan);
                typed_map.expandedCallArguments.insert(id.value, expanded.values);
                if (target_is_slice && fixed_provided < slice_index &&
                    !(target != nullptr &&
                      missingArgsHaveDefaults(*target, fixed_provided, 0U, slice_index))) {
                    report(expr.span, "dock call arity mismatch", diagnostics::err::NoMatchingFn);
                    result = fn->result;
                } else if (target_is_slice && explicit_slice && fixed_provided > slice_index) {
                    report(expr.span, "dock call arity mismatch", diagnostics::err::NoMatchingFn);
                } else if (target_is_slice && !auto_collected && !explicit_slice &&
                           provided_args != slice_index && !defaults_cover) {
                    report(expr.span, "dock call arity mismatch", diagnostics::err::NoMatchingFn);
                } else {
                    const size_t checked_params =
                        target_is_slice ? (plan.explicitSliceArg ? slice_index + 1U : slice_index)
                                        : fn->params.size();
                    for (size_t index = 0; index < checked_params; ++index) {
                        if (index < expanded.values.size()) {
                            const auto &arg = expanded.values[index];
                            TypeId arg_type = arg.type;
                            checkPipeObservationArgument(arg.source, fn->params[index], expr.span);
                            const bool fits =
                                arg.isPackMember()
                                    ? coercesTo(fn->params[index], arg_type)
                                    : coerceValue(arg.source, fn->params[index], arg_type);
                            if (!fits) {
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
                    if (plan.autoCollectTail)
                        (void)checkExpandedVariadicTail(expr.span, expanded.values,
                                                        fn->params[plan.sliceParam],
                                                        plan.sliceParam, true);
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
    const ExpandedCallArguments expanded =
        expandCallArguments(stmt.arguments, stmt.argumentIsSpread, false);
    if (expanded.invalidSpread) {
        const auto &spread = snapshot.expressions()[expanded.invalidSpread.value - 1U];
        report(spread.span, "spread operand must be a pack or a final variadic slice",
               diagnostics::err::NoMatchingFn);
        return;
    }
    std::vector<bool> slice_spreads(stmt.argumentIsSpread.size(), false);
    size_t slice_spread_count = 0U;
    size_t slice_spread_index = ~size_t{0};
    for (const auto &arg : expanded.values) {
        if (!arg.isSliceForwarding)
            continue;
        ++slice_spread_count;
        slice_spread_index = arg.sourceArgument;
        if (arg.sourceArgument < slice_spreads.size())
            slice_spreads[arg.sourceArgument] = true;
    }
    if (slice_spread_count > 1U ||
        (slice_spread_count != 0U &&
         (slice_spread_index + 1U != stmt.arguments.size() || !target_is_slice))) {
        report(stmt.span, "slice forwarding requires the final variadic slice argument",
               diagnostics::err::NoMatchingFn);
        return;
    }
    const size_t provided_args = expanded.values.size();
    const bool explicit_slice = slice_spread_count != 0U;
    const size_t fixed_provided = explicit_slice ? provided_args - 1U : provided_args;
    const bool defaults_cover =
        fixed_provided < (target_is_slice ? slice_index : fn->params.size()) &&
        missingArgsHaveDefaults(*target, fixed_provided, 0U, slice_index);
    if (!target_is_slice && provided_args != fn->params.size() && !defaults_cover) {
        report(stmt.span, "state transition arity mismatch", diagnostics::err::NoMatchingFn);
        return;
    }
    const VariadicCallPlan plan =
        makeVariadicCallPlan(stmt.span, stmt.arguments, fn, target_is_slice, slice_index,
                             slice_spreads, false);
    const bool auto_collected = plan.autoCollectTail;
    typed_map.variadicStmtPlans.insert(stmt.id.value, plan);
    typed_map.expandedStmtArguments.insert(stmt.id.value, expanded.values);
    if (target_is_slice && fixed_provided < slice_index &&
        !missingArgsHaveDefaults(*target, fixed_provided, 0U, slice_index)) {
        report(stmt.span, "state transition arity mismatch", diagnostics::err::NoMatchingFn);
        return;
    } else if (target_is_slice && explicit_slice && fixed_provided > slice_index) {
        report(stmt.span, "state transition arity mismatch", diagnostics::err::NoMatchingFn);
        return;
    } else if (target_is_slice && !auto_collected && !explicit_slice &&
               provided_args != slice_index && !defaults_cover) {
        report(stmt.span, "state transition arity mismatch", diagnostics::err::NoMatchingFn);
        return;
    }
    const size_t checked_params =
        target_is_slice ? (plan.explicitSliceArg ? slice_index + 1U : slice_index)
                        : fn->params.size();
    std::vector<std::pair<frontend::ExprId, types::OwnershipKind>> seen_roots;
    for (size_t index = 0; index < checked_params; ++index) {
        if (index < expanded.values.size()) {
            const auto &arg = expanded.values[index];
            TypeId arg_type = arg.type;
            checkPipeObservationArgument(arg.source, fn->params[index], stmt.span);
            (void)checkOwnershipCoercion(arg.source, fn->params[index], seen_roots, stmt.span,
                                         true);
            const bool fits = arg.isPackMember()
                                  ? coercesTo(fn->params[index], arg_type)
                                  : coerceValue(arg.source, fn->params[index], arg_type);
            if (!fits) {
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
    if (plan.autoCollectTail)
        (void)checkExpandedVariadicTail(stmt.span, expanded.values, fn->params[plan.sliceParam],
                                        plan.sliceParam, true);
}
void PerModuleSema::checkReturnStatement(const frontend::Statement &stmt) {
    if (inferringReturnType_) {
        if (!stmt.expression) {
            recordInferredReturn(void_type, {}, stmt.span);
            return;
        }
        const TypeId value = inferExpr(stmt.expression);
        recordInferredReturn(value, stmt.expression, stmt.span);
        return;
    }
    if (!stmt.expression) {
        // `return;` in a function that promises a value is still a mismatch.
        if (currentReturnType_ && resolve(currentReturnType_) != void_type &&
            resolve(currentReturnType_) != error_type) {
            report(stmt.span, "return without a value in a function with a declared return type",
                   diagnostics::err::TypeMismatch);
        }
        return;
    }
    const TypeId value = inferExprExpected(stmt.expression, currentReturnType_);
    if (stmt.expression && stmt.expression.value <= snapshot.expressions().size() &&
        snapshot.expressions()[stmt.expression.value - 1U].kind == frontend::ExprKind::Index) {
        // Returning `p[0]` returns the pointee value, not the local pointer.
    } else if (pointerAliasEscapesScope(stmt.expression)) {
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
    if (stmt.expression && stmt.expression.value <= snapshot.expressions().size() &&
        snapshot.expressions()[stmt.expression.value - 1U].kind == frontend::ExprKind::Index) {
        // A successfully coerced pointee value still does not escape the pointer.
    } else if (pointerAliasEscapesScope(stmt.expression)) {
        report(stmt.span, "pointer to local storage cannot escape the current scope",
               diagnostics::err::PointerEscapesScope);
    }
}
TypeId PerModuleSema::inferReturn(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    TypeId value     = expr.operands.empty()
                           ? void_type
                           : inferExprExpected(expr.operands[0], currentReturnType_);
    if (inferringReturnType_) {
        recordInferredReturn(value, expr.operands.empty() ? frontend::ExprId{} : expr.operands[0],
                             expr.span);
        return type_table.internName("never", TypeKind::Never);
    }
    if (currentReturnType_ && value && value != error_type && !expr.operands.empty() &&
        !coerceValue(expr.operands[0], currentReturnType_, value)) {
        reportCoercionFailure(expr.span, currentReturnType_, value,
                              "return type does not match declared return type");
    } else if (!expr.operands.empty() && snapshot.expressions()[expr.operands[0].value - 1U].kind ==
                                             frontend::ExprKind::Index) {
        // `return p[0]` returns the pointee value, not the local pointer.
    } else if (!expr.operands.empty() && pointerAliasEscapesScope(expr.operands[0])) {
        report(expr.span, "pointer to local storage cannot escape the current scope",
               diagnostics::err::PointerEscapesScope);
    }
    return type_table.internName("never", TypeKind::Never);
}

} // namespace zith::sema::modern
