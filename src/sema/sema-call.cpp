#include "sema/sema-modern-utils.hpp"
#include "sema/sema-modern.hpp"
#include <algorithm>
#include <string>

namespace zith::sema::modern {

const PerModuleSema::CallTarget *
PerModuleSema::resolvedCallTarget(frontend::ExprId callee) const noexcept {
    const auto found = call_targets_.find(callee.value);
    return found == call_targets_.end() ? nullptr : &found->second;
}
void PerModuleSema::setResolvedCallTarget(frontend::ExprId callee, session::ModuleKey target_module,
                                          frontend::DeclId decl) {
    if (!callee || !decl)
        return;
    call_targets_[callee.value] = CallTarget{std::move(target_module), decl};
}
TypeId PerModuleSema::typeOfResolvedBinding(const session::ResolvedName &binding) {
    if (binding.foreignConstant != nullptr)
        return lowerForeignConstantType(*binding.foreignConstant);
    if (binding.foreignFunction != nullptr) {
        auto &parameters = type_table.makeTypeStorage();
        for (const auto &parameter : binding.foreignFunction->parameters)
            parameters.push(lowerForeignType(parameter));
        return type_table.internFunction(parameters,
                                         lowerForeignType(binding.foreignFunction->result));
    }
    if (binding.declaration) {
        if (!binding.target.module.empty() && binding.target.module != module)
            return typeOfDeclInModule(binding.target.module, binding.declaration);
        return typeOfDecl(binding.declaration);
    }
    if (!binding.target.module.empty() && binding.target.localSymbol) {
        const frontend::DeclId decl{binding.target.localSymbol.value};
        if (binding.target.module == module)
            return typeOfDecl(decl);
        return typeOfDeclInModule(binding.target.module, decl);
    }
    return kInvalidTypeId;
}
bool PerModuleSema::literalAdaptsTo(frontend::ExprId value, TypeId target) const noexcept {
    if (!value || value.value > snapshot.expressions().size())
        return false;
    const auto &expr = snapshot.expressions()[value.value - 1U];
    if (expr.kind == frontend::ExprKind::Unary && expr.text == "-" && !expr.operands.empty())
        return literalAdaptsTo(expr.operands[0], target);
    if (expr.kind != frontend::ExprKind::Literal)
        return false;
    const TypeKind target_kind = type_table.kindOf(resolve(target));
    const bool integer_literal = looksInteger(expr.text);
    const bool float_literal   = looksFloat(expr.text);
    const auto *target_slice   = type_table.slice(resolve(target));
    const bool char_slice_target =
        target_slice != nullptr &&
        sameType(type_table.stripQualifiers(target_slice->element), char_type);
    if (!integer_literal && !float_literal)
        return char_slice_target && looksString(expr.text);
    // Probing is deliberately stricter than `adaptNumericLiteral`: an integer
    // literal only fits an integer parameter and a float literal only a float
    // one.  Otherwise `add(1, 2)` would match both the i32 and the f64 overload
    // and every such call would be ambiguous.
    if (target_kind == TypeKind::Integer)
        return integer_literal;
    if (target_kind == TypeKind::Float)
        return float_literal;
    return false;
}
void PerModuleSema::markSlicePtrCoercionEscaping(frontend::ExprId value) {
    if (!value || value.value > snapshot.expressions().size())
        return;
    const auto &expr = snapshot.expressions()[value.value - 1U];
    if (expr.kind == frontend::ExprKind::Unary && expr.text == "raw" && !expr.operands.empty()) {
        markSlicePtrCoercionEscaping(expr.operands[0]);
        return;
    }
    escapingPointerExprs_.insert(value.value);
}
bool PerModuleSema::isCharSliceToPointer(TypeId source, TypeId target) const noexcept {
    const TypeId source_resolved = type_table.stripQualifiers(source);
    const TypeId target_resolved = type_table.stripQualifiers(target);
    if (type_table.kindOf(source_resolved) != TypeKind::Slice ||
        type_table.kindOf(target_resolved) != TypeKind::Pointer)
        return false;
    const auto *slice = type_table.slice(source_resolved);
    const auto *ptr   = type_table.pointer(target_resolved);
    return slice != nullptr && ptr != nullptr &&
           sameType(type_table.stripQualifiers(slice->element), char_type) &&
           sameType(type_table.stripQualifiers(ptr->pointee), char_type);
}
const PerModuleSema::OverloadCandidate *
PerModuleSema::selectOverload(const frontend::Expression &call,
                              std::vector<OverloadCandidate> &candidates, size_t implicit_args,
                              bool &reported) {
    reported                  = false;
    const size_t written_args = call.operands.size() - 1U;
    std::vector<const OverloadCandidate *> viable;
    std::vector<const OverloadCandidate *> exact_matches;
    bool widened_pointer = false;
    for (auto &candidate : candidates) {
        if (candidate.fn == nullptr)
            continue;
        const bool slice_candidate = candidate.binding != nullptr &&
                                     candidate.binding->isVariadicSlice &&
                                     candidate.fn != nullptr && !candidate.fn->params.empty();
        candidate.variadicSlice = candidate.binding != nullptr &&
                                  candidate.binding->isVariadicSlice && candidate.fn != nullptr &&
                                  !candidate.fn->params.empty();
        const auto *candidate_decl =
            candidate.binding != nullptr ? declarationForResolved(*candidate.binding) : nullptr;
        const size_t fixed_params = candidate.fn->params.size() - (slice_candidate ? 1U : 0U);
        const bool defaults_cover =
            written_args < fixed_params && candidate_decl != nullptr &&
            missingArgsHaveDefaults(*candidate_decl, written_args, implicit_args,
                                    slice_candidate ? fixed_params : ~static_cast<size_t>(0));
        if (slice_candidate) {
            if (!defaults_cover && written_args < fixed_params)
                continue;
        } else {
            const size_t expected = written_args + implicit_args;
            if (expected > candidate.fn->params.size())
                continue;
            if (expected < candidate.fn->params.size() && !defaults_cover)
                continue;
        }
        bool fits       = true;
        bool exact      = true;
        bool widens_ptr = false;
        const size_t probe_params =
            slice_candidate ? fixed_params : std::min(written_args, candidate.fn->params.size());
        for (size_t index = 0; index < probe_params && fits; ++index) {
            const frontend::ExprId arg = call.operands[index + 1U];
            const TypeId arg_type      = inferExpr(arg);
            const size_t param_index   = index + implicit_args;
            const TypeId param_type    = candidate.fn->params[param_index];
            // Probe without mutating the recorded type: a rejected candidate must
            // not leave a literal retyped for a signature that was not chosen.
            std::vector<std::pair<frontend::ExprId, types::OwnershipKind>> seen_roots;
            const bool borrow_ok =
                checkOwnershipCoercion(arg, param_type, seen_roots, call.span, false);
            const bool type_fits =
                isBorrowParamType(param_type)
                    ? coerceValue(arg, param_type, arg_type)
                    : (coercesTo(param_type, arg_type) || literalAdaptsTo(arg, param_type));
            fits            = type_fits && borrow_ok;
            const bool same = sameType(param_type, arg_type);
            exact           = exact && same;
            widens_ptr      = widens_ptr || (!same && isVoidPointer(param_type) &&
                                        static_cast<bool>(pointerBase(arg_type)));
        }
        if (slice_candidate) {
            // Validate the auto-collected tail against the slice element. An
            // explicit `[]T` argument is a normal fixed-arity call and is left
            // to the caller's regular coercion loop. Numeric literals also
            // adapt here so `f(1, 2)` can match `f(xs: [...]i32)`.
            const auto *slice = type_table.slice(candidate.fn->params.back());
            if (slice != nullptr) {
                for (size_t index = fixed_params; index < written_args && fits; ++index) {
                    const frontend::ExprId arg = call.operands[index + 1U];
                    const TypeId arg_type      = inferExpr(arg);
                    fits =
                        coercesTo(slice->element, arg_type) || literalAdaptsTo(arg, slice->element);
                    if (fits)
                        exact = false;
                }
            } else {
                fits = false;
            }
        }
        if (fits) {
            viable.push_back(&candidate);
            if (exact)
                exact_matches.push_back(&candidate);
            if (widens_ptr)
                widened_pointer = true;
        }
    }
    // Any pointer coerces to a `void*` parameter, so `f(*i32)` and `f(raw opaque)` are both
    // viable for a `*i32` argument. Only that widening is ranked away by preferring the
    // exact signature; every other overload tie (including numeric literals) stays ambiguous.
    if (widened_pointer && !exact_matches.empty())
        viable = std::move(exact_matches);
    // A fixed-arity overload that matches exactly is preferred over a
    // variadic-slice overload even when the latter also fits. This is the
    // explicit design contract for adding `[...]T` tails beside existing `fn`s.
    const bool has_fixed_exact =
        std::any_of(exact_matches.begin(), exact_matches.end(),
                    [](const OverloadCandidate *c) { return c != nullptr && !c->variadicSlice; });
    if (has_fixed_exact) {
        viable.erase(std::remove_if(viable.begin(), viable.end(),
                                    [](const OverloadCandidate *c) {
                                        return c != nullptr && c->variadicSlice;
                                    }),
                     viable.end());
        exact_matches.erase(std::remove_if(exact_matches.begin(), exact_matches.end(),
                                           [](const OverloadCandidate *c) {
                                               return c != nullptr && c->variadicSlice;
                                           }),
                            exact_matches.end());
    }
    if (viable.size() == 1U)
        return viable.front();

    if (viable.empty()) {
        report(call.span, "no overload of this function accepts the given arguments",
               diagnostics::err::NoMatchingFn);
    } else {
        report(call.span, "call is ambiguous between several overloads",
               diagnostics::err::AmbiguousCall);
    }
    for (const auto &candidate : candidates)
        reportNote(candidate.span, "candidate declared here");
    reported = true;
    return nullptr;
}
TypeId PerModuleSema::inferCall(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.empty())
        return error_type;

    // If the callee is a field access, try to resolve it as a method call
    // first: `obj.method(args)` rewrites to a direct call with an implicit
    // `&obj` (or `obj` when already a pointer) as the first argument.
    const auto &callee = snapshot.expressions()[expr.operands[0].value - 1U];
    if ((callee.kind == frontend::ExprKind::Field || callee.kind == frontend::ExprKind::Arrow) &&
        !callee.operands.empty()) {
        if (const TypeId mt = inferMethodCall(expr, callee))
            return mt;
    }

    // An overload set: several `fn` declarations share the callee's name in the
    // nearest scope that declares it.  Pick by arity, then by argument fit.
    if (callee.kind == frontend::ExprKind::Name) {
        const auto bindings =
            session::lookupOverloads(resolution, callee.text, callee.scope, snapshot.scopes());
        if (bindings.size() > 1U) {
            std::vector<OverloadCandidate> candidates;
            candidates.reserve(bindings.size());
            for (const auto *binding : bindings) {
                OverloadCandidate candidate;
                candidate.binding = binding;
                candidate.type    = typeOfResolvedBinding(*binding);
                candidate.fn      = type_table.function(candidate.type);
                candidate.span    = binding->span;
                candidate.module = binding->target.module.empty() ? module : binding->target.module;
                candidate.decl   = binding->declaration;
                if (candidate.fn != nullptr && !typeContainsGeneric(candidate.fn))
                    candidates.push_back(candidate);
            }
            if (candidates.size() > 1U) {
                bool reported = false;
                if (const auto *chosen = selectOverload(expr, candidates, 0U, reported)) {
                    std::vector<std::pair<frontend::ExprId, types::OwnershipKind>> seen_roots;
                    const size_t probe_params = chosen->variadicSlice
                                                    ? chosen->fn->params.size() - 1U
                                                    : chosen->fn->params.size();
                    for (size_t index = 0;
                         index < probe_params && index < expr.operands.size() - 1U; ++index) {
                        const TypeId arg_type = inferExpr(expr.operands[index + 1U]);
                        (void)checkOwnershipCoercion(expr.operands[index + 1U],
                                                     chosen->fn->params[index], seen_roots,
                                                     expr.span, true);
                        if (!coerceValue(expr.operands[index + 1U], chosen->fn->params[index],
                                         arg_type)) {
                            reportCoercionFailure(expr.span, chosen->fn->params[index], arg_type,
                                                  "function call argument type mismatch",
                                                  diagnostics::err::NoMatchingFn);
                        }
                    }
                    setExprType(callee.id, chosen->type);
                    const auto &binding   = *chosen->binding;
                    frontend::DeclId decl = binding.declaration;
                    session::ModuleKey target =
                        binding.target.module.empty() ? module : binding.target.module;
                    if (!decl && binding.target.localSymbol)
                        decl = frontend::DeclId{binding.target.localSymbol.value};
                    setResolvedCallTarget(callee.id, target, decl);
                    return chosen->fn->result;
                }
                if (reported)
                    return error_type;
            }
        }
    }

    const auto *resolved_callee = findResolvedExpr(callee.id);
    const frontend::Declaration *callee_decl =
        resolved_callee != nullptr ? declarationForResolved(*resolved_callee) : nullptr;
    const bool is_variadic = resolved_callee != nullptr && bindingIsVariadic(*resolved_callee);
    const bool is_variadic_slice =
        resolved_callee != nullptr && bindingIsVariadicSlice(*resolved_callee);
    TypeId callee_type = inferExpr(expr.operands[0]);
    const auto *fn     = type_table.function(callee_type);
    if (!fn) {
        report(expr.span, "callee is not a function", diagnostics::err::NoMatchingFn);
        return error_type;
    }
    size_t arg_count       = expr.operands.size() - 1;
    size_t fixed_arg_count = is_variadic ? fn->params.size() : 0;
    size_t slice_index =
        is_variadic_slice ? variadicSliceParam(resolved_callee, fn) : fn->params.size();
    const bool explicit_slice_arg = [&]() {
        if (!is_variadic_slice || arg_count != slice_index + 1U || expr.operands.size() < 2U)
            return false;
        (void)inferExpr(expr.operands.back());
        return variadicFinalArgIsExplicitSlice(fn->params[slice_index], expr.operands, slice_index);
    }();
    const bool auto_collected_tail =
        is_variadic_slice &&
        (arg_count > slice_index + 1U || (arg_count == slice_index + 1U && !explicit_slice_arg));
    if (is_variadic_slice) {
        // The slice parameter is not auto-collected when the caller passes an
        // explicit slice as the final argument. That keeps `fn f(xs: [...]T)`
        // callable with an existing `[]T` value as well as with bare elements.
        if (slice_index >= fn->params.size()) {
            report(expr.span, "variadic slice function has no slice parameter",
                   diagnostics::err::NoMatchingFn);
            return fn->result;
        }
        if (arg_count < slice_index) {
            report(expr.span, "variadic slice function call has too few arguments",
                   diagnostics::err::NoMatchingFn);
            return fn->result;
        }
    } else if (is_variadic) {
        if (arg_count < fixed_arg_count) {
            report(expr.span, "variadic function call has too few arguments",
                   diagnostics::err::NoMatchingFn);
            return fn->result;
        }
    } else if (arg_count != fn->params.size()) {
        if (arg_count < fn->params.size() &&
            (callee_decl == nullptr || !missingArgsHaveDefaults(*callee_decl, arg_count, 0U))) {
            report(expr.span, "function call arity mismatch", diagnostics::err::NoMatchingFn);
            return fn->result;
        }
        if (arg_count > fn->params.size()) {
            report(expr.span, "function call arity mismatch", diagnostics::err::NoMatchingFn);
            return fn->result;
        }
    }

    if (!expr.genericArgs.empty() || typeContainsGeneric(fn)) {
        if (instantiations == nullptr) {
            report(expr.span, "generic function calls require the instantiation pass",
                   diagnostics::err::GenericCannotInfer);
            return error_type;
        }

        frontend::DeclId decl_id{};
        if (resolved_callee != nullptr) {
            if (resolved_callee->declaration)
                decl_id = resolved_callee->declaration;
            else if (resolved_callee->target.localSymbol)
                decl_id = frontend::DeclId{resolved_callee->target.localSymbol.value};
        }
        const frontend::Declaration *generic_decl = nullptr;
        if (decl_id && decl_id.value <= snapshot.declarations().size())
            generic_decl = &snapshot.declarations()[decl_id.value - 1U];
        session::ModuleKey target_module = module;
        if (resolved_callee != nullptr && !resolved_callee->target.module.empty())
            target_module = resolved_callee->target.module;
        const PerModuleSema *decl_sema =
            owner != nullptr ? owner->findModuleSema(target_module) : nullptr;
        if (decl_sema == nullptr)
            decl_sema = this;

        std::vector<TypeId> explicit_types;
        explicit_types.reserve(expr.genericArgs.size());
        for (const auto generic_arg : expr.genericArgs) {
            const TypeId lowered = lowerTypeExpr(generic_arg);
            if (!lowered) {
                report(expr.span, "generic argument is not a concrete type",
                       diagnostics::err::GenericCannotInfer);
                return error_type;
            }
            explicit_types.push_back(lowered);
        }

        std::vector<TypeId> argument_types;
        argument_types.reserve(fn->params.size());
        for (size_t index = 0; index < fn->params.size(); ++index) {
            if (is_variadic_slice && index == slice_index) {
                // If the caller passes a single explicit slice value, keep it
                // so `f([]T)` still instantiates `T`; otherwise infer `[]T`
                // from the first auto-collected element.
                if (arg_count == slice_index + 1U && index + 1U < expr.operands.size()) {
                    const TypeId last_arg =
                        inferExpr(expr.operands[static_cast<size_t>(index + 1U)]);
                    if (const auto *arg_slice = type_table.slice(resolve(last_arg))) {
                        (void)arg_slice;
                        argument_types.push_back(last_arg);
                    } else {
                        argument_types.push_back(type_table.internSlice(last_arg));
                    }
                } else if (arg_count > slice_index + 1U && index + 1U < expr.operands.size()) {
                    const TypeId element_sample =
                        inferExpr(expr.operands[static_cast<size_t>(index + 1U)]);
                    argument_types.push_back(type_table.internSlice(element_sample));
                } else {
                    argument_types.push_back(kInvalidTypeId);
                }
            } else if (index + 1U < expr.operands.size()) {
                argument_types.push_back(inferExpr(expr.operands[static_cast<size_t>(index + 1U)]));
            } else if (callee_decl != nullptr && index < callee_decl->parameters.size() &&
                       callee_decl->parameters[index].defaultValue) {
                argument_types.push_back(functionDefaultType(*callee_decl, index, *decl_sema));
            } else {
                argument_types.push_back(kInvalidTypeId);
            }
        }

        std::vector<TypeId> args;
        const comptime::GenericResolveStatus resolved = instantiations->resolveArgs(
            *fn, generic_decl != nullptr ? generic_decl->genericParams.size() : 0U, decl_id.value,
            explicit_types, argument_types, args);
        switch (resolved) {
        case comptime::GenericResolveStatus::Arity:
            report(expr.span, "wrong generic argument count", diagnostics::err::GenericArity);
            return error_type;
        case comptime::GenericResolveStatus::CannotInfer:
            report(expr.span, "cannot infer generic argument; provide explicit type arguments",
                   diagnostics::err::GenericCannotInfer);
            return error_type;
        case comptime::GenericResolveStatus::Explosion:
            report(expr.span, "too many generic instantiations",
                   diagnostics::err::GenericExplosion);
            return error_type;
        case comptime::GenericResolveStatus::Ok:
            break;
        }
        if (generic_decl != nullptr)
            checkGenericConstraints(*generic_decl, args, expr.span);

        const size_t instance_index =
            instantiations->bindCall(module, callee.id, target_module, decl_id, args);
        if (instance_index == ~size_t{0}) {
            report(expr.span, "too many generic instantiations",
                   diagnostics::err::GenericExplosion);
            return error_type;
        }
        const TypeId instance_type = instantiations->substituteFunction(*fn, args);
        const auto *instance_fn    = type_table.function(instance_type);
        std::vector<std::pair<frontend::ExprId, types::OwnershipKind>> seen_roots;
        const size_t checked_params = is_variadic_slice ? slice_index : fn->params.size();
        for (size_t index = 0;
             index < checked_params && instance_fn != nullptr && index < instance_fn->params.size();
             ++index) {
            const TypeId arg_type = argument_types[index];
            if (index + 1U < expr.operands.size()) {
                (void)checkOwnershipCoercion(expr.operands[index + 1U], instance_fn->params[index],
                                             seen_roots, expr.span, true);
                if (!coerceValue(expr.operands[index + 1U], instance_fn->params[index], arg_type))
                    reportCoercionFailure(expr.span, instance_fn->params[index], arg_type,
                                          "generic function call argument type mismatch",
                                          diagnostics::err::NoMatchingFn);
            } else if (index < callee_decl->parameters.size() &&
                       callee_decl->parameters[index].defaultValue) {
                if (!coerceValue(callee_decl->parameters[index].defaultValue,
                                 instance_fn->params[index], arg_type))
                    reportCoercionFailure(expr.span, instance_fn->params[index], arg_type,
                                          "generic function default argument type mismatch",
                                          diagnostics::err::NoMatchingFn);
            }
        }
        if (auto_collected_tail && instance_fn != nullptr) {
            (void)checkVariadicTail(expr.span, expr.operands, instance_fn, slice_index, true);
        }
        setExprType(callee.id, instance_type);
        setResolvedCallTarget(callee.id, target_module, decl_id);
        return instance_fn != nullptr ? instance_fn->result : error_type;
    }

    const size_t checked_params = is_variadic_slice
                                      ? (explicit_slice_arg ? slice_index + 1U : slice_index)
                                  : is_variadic ? std::min(fixed_arg_count, fn->params.size())
                                                : fn->params.size();
    std::vector<std::pair<frontend::ExprId, types::OwnershipKind>> seen_roots;
    for (size_t i = 0; i < checked_params; ++i) {
        if (i + 1U < expr.operands.size()) {
            TypeId arg_type = inferExpr(expr.operands[i + 1]);
            (void)checkOwnershipCoercion(expr.operands[i + 1], fn->params[i], seen_roots, expr.span,
                                         true);
            if (!coerceValue(expr.operands[i + 1], fn->params[i], arg_type))
                reportCoercionFailure(expr.span, fn->params[i], arg_type,
                                      "function call argument type mismatch",
                                      diagnostics::err::NoMatchingFn);
        } else if (callee_decl != nullptr && i < callee_decl->parameters.size() &&
                   callee_decl->parameters[i].defaultValue) {
            const TypeId default_type = typeOfExpr(callee_decl->parameters[i].defaultValue);
            if (!coerceValue(callee_decl->parameters[i].defaultValue, fn->params[i], default_type))
                reportCoercionFailure(expr.span, fn->params[i], default_type,
                                      "function default argument type mismatch",
                                      diagnostics::err::NoMatchingFn);
        }
    }
    if (auto_collected_tail) {
        (void)checkVariadicTail(expr.span, expr.operands, fn, slice_index, true);
    }
    // The trailing variadic arguments have no declared Zith type: infer them
    // (for diagnostics) but do not require a conversion or a fixed arity.
    for (size_t i = std::max<size_t>(1, checked_params + 1); i < expr.operands.size(); ++i)
        (void)inferExpr(expr.operands[i]);
    return fn->result;
}
bool PerModuleSema::bindingIsVariadic(const session::ResolvedName &binding) noexcept {
    return binding.isVariadic ||
           (binding.foreignFunction != nullptr && binding.foreignFunction->isVariadic);
}
bool PerModuleSema::bindingIsVariadicSlice(const session::ResolvedName &binding) noexcept {
    return binding.isVariadicSlice;
}
size_t PerModuleSema::variadicSliceParam(const session::ResolvedName *binding,
                                         const FunctionType *fn) const {
    if (binding == nullptr || fn == nullptr || fn->params.empty() || !binding->isVariadicSlice)
        return fn != nullptr ? fn->params.size() : 0U;
    return fn->params.size() - 1U;
}
TypeId PerModuleSema::checkVariadicTail(frontend::TextSpan span,
                                        const std::vector<frontend::ExprId> &args,
                                        const FunctionType *fn, size_t slice_index,
                                        bool allow_literals) {
    if (fn == nullptr || slice_index >= fn->params.size()) {
        report(span, "variadic slice function has no slice parameter",
               diagnostics::err::NoMatchingFn);
        return error_type;
    }
    const TypeId slice_type = resolve(fn->params[slice_index]);
    const auto *slice       = type_table.slice(slice_type);
    if (slice == nullptr) {
        report(span, "variadic slice parameter is not a slice type",
               diagnostics::err::NoMatchingFn);
        return error_type;
    }
    return checkVariadicTailArgs(span, args, fn->params[slice_index], slice_index + 1U,
                                 allow_literals);
}
TypeId PerModuleSema::checkVariadicTailArgs(frontend::TextSpan span,
                                            const std::vector<frontend::ExprId> &args,
                                            TypeId slice_type, size_t first_tail_index,
                                            bool allow_literals) {
    const TypeId resolved_slice = resolve(slice_type);
    const auto *slice           = type_table.slice(resolved_slice);
    if (slice == nullptr) {
        report(span, "variadic slice parameter is not a slice type",
               diagnostics::err::NoMatchingFn);
        return error_type;
    }
    TypeId element = slice->element;
    std::vector<std::pair<frontend::ExprId, types::OwnershipKind>> seen_roots;
    for (size_t index = first_tail_index; index < args.size(); ++index) {
        const frontend::ExprId arg = args[index];
        TypeId arg_type            = inferExpr(arg);
        (void)checkOwnershipCoercion(arg, slice->element, seen_roots, span, true);
        if (!coerceValue(arg, slice->element, arg_type)) {
            // A concrete value can erase to `dyn Trait` when the conformance
            // table knows the edge. `coerceValue` handles named types through
            // `satisfiesConformance`, but `checkVariadicTailArgs` must also
            // prime instantiations for enum/union implementations.
            if (type_table.kindOf(resolve(slice->element)) == TypeKind::Dyn &&
                satisfiesConformance(arg_type,
                                     resolve(type_table.dyn_type(slice->element)->target))) {
                primeDynImplementations(slice->element, arg_type);
                typed_map.dynSourceTypes.insert(arg.value, arg_type);
                setExprType(arg, slice->element);
                continue;
            }
            if (!allow_literals || !adaptNumericLiteral(arg, slice->element))
                reportCoercionFailure(span, slice->element, arg_type,
                                      "variadic slice argument type mismatch",
                                      diagnostics::err::NoMatchingFn);
        }
    }
    return type_table.internSlice(element);
}
bool PerModuleSema::typeContainsGeneric(const FunctionType *fn) const noexcept {
    const auto contains = [&](auto &&self, TypeId type, unsigned depth = 0U) -> bool {
        if (!type || depth >= 16U)
            return false;
        type = type_table.stripQualifiers(type);
        if (type_table.kindOf(type) == TypeKind::GenericParam)
            return true;
        if (const auto *alias = type_table.alias(type))
            return self(self, alias->target, depth + 1U);
        if (const auto *nominal = type_table.nominal(type))
            return self(self, nominal->target, depth + 1U);
        if (const auto *opt = type_table.optional(type))
            return self(self, opt->inner, depth + 1U);
        if (const auto *ptr = type_table.pointer(type))
            return self(self, ptr->pointee, depth + 1U);
        if (const auto *slice = type_table.slice(type))
            return self(self, slice->element, depth + 1U);
        if (const auto *array = type_table.array(type))
            return self(self, array->element, depth + 1U);
        if (const auto *nested = type_table.function(type)) {
            for (const auto param : nested->params)
                if (self(self, param, depth + 1U))
                    return true;
            return self(self, nested->result, depth + 1U);
        }
        if (const auto *failable = type_table.failable(type))
            return self(self, failable->inner, depth + 1U);
        if (const auto *dyn = type_table.dyn_type(type))
            return self(self, dyn->target, depth + 1U);
        if (const auto *st = type_table.struct_type(type)) {
            for (const auto field : st->fields)
                if (self(self, field, depth + 1U))
                    return true;
            return false;
        }
        if (const auto *ut = type_table.union_type(type)) {
            for (const auto member : ut->members)
                if (self(self, member, depth + 1U))
                    return true;
            return false;
        }
        if (const auto *sum = type_table.sum(type)) {
            for (const auto member : sum->members)
                if (self(self, member, depth + 1U))
                    return true;
            return false;
        }
        if (const auto *pack = type_table.pack(type)) {
            for (const auto member : pack->members)
                if (self(self, member, depth + 1U))
                    return true;
            return false;
        }
        return false;
    };

    for (const auto param : fn->params)
        if (contains(contains, param))
            return true;
    return contains(contains, fn->result);
}

} // namespace zith::sema::modern
