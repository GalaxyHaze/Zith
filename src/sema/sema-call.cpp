#include "sema/sema-modern-utils.hpp"
#include "sema/sema-modern.hpp"
#include "support/int-literal.hpp"
#include <algorithm>
#include <functional>
#include <string>

namespace zith::sema::modern {

const PerModuleSema::CallTarget *
PerModuleSema::resolvedCallTarget(frontend::ExprId callee) const noexcept {
    const auto *found = call_targets_.get(callee.value);
    return found ? found : nullptr;
}
void PerModuleSema::setResolvedCallTarget(frontend::ExprId callee, session::ModuleKey target_module,
                                          frontend::DeclId decl) {
    if (!callee || !decl)
        return;
    call_targets_[callee.value] = CallTarget{std::move(target_module), decl};
}
TypeId PerModuleSema::typeOfResolvedBinding(const session::ResolvedName &binding) {
    if (binding.foreignGlobal != nullptr)
        return lowerForeignType(binding.foreignGlobal->type);
    if (binding.foreignTypedef != nullptr)
        return lowerForeignType(binding.foreignTypedef->type);
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
        candidate.variadicSlice =
            candidate.variadicSlice ||
            (candidate.binding != nullptr && candidate.binding->isVariadicSlice);
        const bool slice_candidate = candidate.variadicSlice && !candidate.fn->params.empty();
        const auto *candidate_sema =
            candidate.module.empty() || candidate.module == module || owner == nullptr
                ? this
                : owner->findModuleSema(candidate.module);
        if (candidate_sema == nullptr)
            candidate_sema = this;
        const frontend::Declaration *candidate_decl =
            candidate.binding != nullptr ? declarationForResolved(*candidate.binding)
                                         : (candidate.decl &&
                                                    candidate.decl.value <=
                                                        candidate_sema->snapshot.declarations().size()
                                                ? &candidate_sema->snapshot.declarations()
                                                       [candidate.decl.value - 1U]
                                                : nullptr);
        const size_t fixed_params = candidate.fn->params.size() - (slice_candidate ? 1U : 0U);
        if (fixed_params < implicit_args)
            continue;
        const size_t fixed_explicit_args = fixed_params - implicit_args;
        const bool has_spread =
            std::any_of(call.argIsSpread.begin(), call.argIsSpread.end(), [](bool spread) {
                return spread;
            });
        if (!spreadMarkersAreValid(call.argIsSpread, written_args, fixed_explicit_args) ||
            (has_spread && !slice_candidate))
            continue;
        candidate.variadicPlan =
            makeVariadicCallPlan(call.span, call.operands, candidate.fn, slice_candidate,
                                 fixed_explicit_args, call.argIsSpread);
        const bool defaults_cover =
            written_args < fixed_explicit_args && candidate_decl != nullptr &&
            missingArgsHaveDefaults(*candidate_decl, written_args, implicit_args,
                                    slice_candidate ? fixed_params : ~static_cast<size_t>(0));
        if (slice_candidate) {
            if (!defaults_cover && written_args < fixed_explicit_args)
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
            std::min(written_args, slice_candidate
                                       ? fixed_explicit_args
                                       : candidate.fn->params.size() - implicit_args);
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
            const auto *slice = type_table.slice(candidate.fn->params.back());
            if (slice != nullptr) {
                if (candidate.variadicPlan.explicitSliceArg) {
                    const frontend::ExprId arg = call.operands.back();
                    const TypeId arg_type      = inferExpr(arg);
                    fits = fits && coercesTo(candidate.fn->params.back(), arg_type);
                    exact = exact && sameType(candidate.fn->params.back(), arg_type);
                } else {
                    // Numeric literals also adapt here so `f(1, 2)` can match
                    // `f(xs: [...]i32)`.
                    for (size_t index = fixed_explicit_args; index < written_args && fits;
                         ++index) {
                        const frontend::ExprId arg = call.operands[index + 1U];
                        const TypeId arg_type      = inferExpr(arg);
                        fits = coercesTo(slice->element, arg_type) ||
                               literalAdaptsTo(arg, slice->element);
                        if (fits)
                            exact = false;
                    }
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
TypeId PerModuleSema::inferCall(frontend::ExprId id, TypeId expected_result) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.empty())
        return error_type;

    const auto report_call_boundary = [&](const frontend::Declaration *declaration,
                                          bool is_c_header_function) {
        const bool is_raw =
            is_c_header_function ||
            (declaration != nullptr && declaration->kind == frontend::DeclKind::Function &&
             declaration->functionKind == frontend::FunctionKind::Raw);
        if (expr.isRawCall && !is_raw) {
            const auto &callee_expr = snapshot.expressions()[expr.operands[0].value - 1U];
            report(expr.span, "raw marker used on a non-raw call to '" + callee_expr.text + "'",
                   diagnostics::err::UnsupportedSyntax);
        } else if (!expr.isRawCall && is_raw) {
            report(expr.span,
                   "call to raw function '" +
                       (declaration != nullptr
                            ? declaration->name
                            : snapshot.expressions()[expr.operands[0].value - 1U].text) +
                       "' requires the 'raw' marker",
                   diagnostics::err::RawCallRequired);
            if (declaration != nullptr && !declaration->rawReason.empty())
                reportNote(expr.span, "raw call reason: " + declaration->rawReason);
        }
        if (is_c_header_function) {
            reportWarning(expr.span,
                          "call crosses an external C boundary; the compiler cannot guarantee "
                          "the function's behavior",
                          diagnostics::err::ExternalCallBoundary);
        }
    };

    // If the callee is a field access, try to resolve it as a method call
    // first: `obj.method(args)` rewrites to a direct call with an implicit
    // `&obj` (or `obj` when already a pointer) as the first argument.
    const auto &callee = snapshot.expressions()[expr.operands[0].value - 1U];
    semaProbe("[probe] inferCall callee kind=%d text='%s' operands=%zu generic=%zu node=%u\n",
              static_cast<int>(callee.kind), callee.text.c_str(), callee.operands.size(),
              callee.genericArgs.size(), expr.id.value);
    if ((callee.kind == frontend::ExprKind::Field || callee.kind == frontend::ExprKind::Arrow) &&
        !callee.operands.empty()) {
        semaProbe("[probe] inferCall trying method for callee '%s'\n", callee.text.c_str());
        if (const TypeId mt = inferMethodCall(expr, callee, expected_result)) {
            const auto *target                       = resolvedCallTarget(callee.id);
            const frontend::Declaration *target_decl = nullptr;
            if (target != nullptr) {
                const PerModuleSema *target_sema = this;
                if (target->module != module && owner != nullptr)
                    target_sema = owner->findModuleSema(target->module);
                if (target_sema != nullptr) {
                    for (const auto &decl : target_sema->snapshot.declarations()) {
                        if (decl.id == target->decl) {
                            target_decl = &decl;
                            break;
                        }
                    }
                }
            }
            report_call_boundary(target_decl, false);
            return mt;
        }
        if (expr.isRawCall)
            report_call_boundary(nullptr, false);
        semaProbe("[probe] inferCall method failed for callee '%s'\n", callee.text.c_str());
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
                    const VariadicCallPlan chosen_plan = chosen->variadicPlan;
                    typed_map.variadicCallPlans.insert(expr.id.value, chosen_plan);
                    if (chosen_plan.explicitSliceArg) {
                        const frontend::ExprId arg = expr.operands.back();
                        const TypeId arg_type      = inferExpr(arg);
                        std::vector<std::pair<frontend::ExprId, types::OwnershipKind>> roots;
                        (void)checkOwnershipCoercion(arg, chosen_plan.sliceType, roots, expr.span,
                                                     true);
                        if (!coerceValue(arg, chosen_plan.sliceType, arg_type))
                            reportCoercionFailure(expr.span, chosen_plan.sliceType, arg_type,
                                                  "variadic slice argument type mismatch",
                                                  diagnostics::err::NoMatchingFn);
                    }
                    if (chosen_plan.autoCollectTail)
                        (void)checkVariadicTail(expr.span, expr.operands, chosen->fn,
                                                chosen_plan.sliceParam, true);
                    report_call_boundary(declarationForResolved(*chosen->binding),
                                         chosen->binding->foreignFunction != nullptr);
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
    const bool direct_function = callee.kind == frontend::ExprKind::Name &&
                                 resolved_callee != nullptr && !resolved_callee->local &&
                                 (resolved_callee->declKind == frontend::DeclKind::Function ||
                                  resolved_callee->foreignFunction != nullptr);
    if (direct_function) {
        report_call_boundary(callee_decl, resolved_callee->foreignFunction != nullptr);
    } else if (expr.isRawCall && callee.kind != frontend::ExprKind::Field &&
               callee.kind != frontend::ExprKind::Arrow) {
        report_call_boundary(nullptr, false);
    }
    const bool is_variadic = resolved_callee != nullptr && bindingIsVariadic(*resolved_callee);
    const bool is_variadic_slice =
        resolved_callee != nullptr && bindingIsVariadicSlice(*resolved_callee);
    TypeId callee_type =
        direct_function ? typeOfResolvedBinding(*resolved_callee) : inferExpr(expr.operands[0]);
    if (direct_function)
        setExprType(callee.id, callee_type);
    const auto *fn = type_table.function(callee_type);
    if (!fn) {
        report(expr.span, "callee is not a function", diagnostics::err::NoMatchingFn);
        return error_type;
    }
    size_t arg_count       = expr.operands.size() - 1;
    size_t fixed_arg_count = is_variadic ? fn->params.size() : 0;
    size_t slice_index =
        is_variadic_slice ? variadicSliceParam(resolved_callee, fn) : fn->params.size();
    const VariadicCallPlan plan =
        makeVariadicCallPlan(expr.span, expr.operands, fn, is_variadic_slice, slice_index,
                             expr.argIsSpread);
    const bool explicit_slice_arg  = plan.explicitSliceArg;
    const bool auto_collected_tail = plan.autoCollectTail;
    typed_map.variadicCallPlans.insert(expr.id.value, plan);
    if (!spreadMarkersAreValid(expr.argIsSpread, arg_count,
                               is_variadic_slice ? slice_index : arg_count)) {
        report(expr.span, "spread is only allowed on the final variadic slice argument",
               diagnostics::err::NoMatchingFn);
        return fn->result;
    }
    if (!is_variadic_slice &&
        std::any_of(expr.argIsSpread.begin(), expr.argIsSpread.end(), [](bool spread) {
            return spread;
        })) {
        report(expr.span, "spread requires a variadic slice parameter",
               diagnostics::err::NoMatchingFn);
        return fn->result;
    }
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
        session::ModuleKey target_module = module;
        if (resolved_callee != nullptr && !resolved_callee->target.module.empty())
            target_module = resolved_callee->target.module;
        const PerModuleSema *decl_sema =
            owner != nullptr ? owner->findModuleSema(target_module) : nullptr;
        if (decl_sema == nullptr)
            decl_sema = this;
        const frontend::Declaration *generic_decl = nullptr;
        if (decl_id && decl_id.value <= decl_sema->snapshot.declarations().size())
            generic_decl = &decl_sema->snapshot.declarations()[decl_id.value - 1U];

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
        std::vector<bool> weak_args;
        weak_args.reserve(fn->params.size());
        // An unsuffixed numeric literal is a weak inference source: its default
        // type (`i32`/`f64`) must not outvote a contextual expected type, since
        // the call-site coercion would adapt the literal anyway. A suffixed
        // literal or a named binding is a strong source and keeps priority.
        std::function<bool(frontend::ExprId)> weakLiteral = [&](frontend::ExprId arg_id) -> bool {
            if (!arg_id || arg_id.value > snapshot.expressions().size())
                return false;
            const auto &arg_expr = snapshot.expressions()[arg_id.value - 1U];
            if (arg_expr.kind == frontend::ExprKind::Unary && arg_expr.text == "-" &&
                !arg_expr.operands.empty())
                return weakLiteral(arg_expr.operands[0]);
            if (arg_expr.kind != frontend::ExprKind::Literal)
                return false;
            if (!looksInteger(arg_expr.text) && !looksFloat(arg_expr.text))
                return false;
            return support::integerSuffix(arg_expr.text).empty();
        };
        for (size_t index = 0; index < fn->params.size(); ++index) {
            if (is_variadic_slice && index == slice_index) {
                // If the caller passes a single explicit slice value, keep it
                // so `f([]T)` still instantiates `T`; otherwise infer `[]T`
                // from the first auto-collected element.
                if (plan.explicitSliceArg && index + 1U < expr.operands.size()) {
                    weak_args.push_back(false);
                    argument_types.push_back(
                        inferExpr(expr.operands[static_cast<size_t>(index + 1U)]));
                } else if (arg_count > slice_index && index + 1U < expr.operands.size()) {
                    weak_args.push_back(weakLiteral(expr.operands[static_cast<size_t>(index + 1U)]));
                    const TypeId element_sample =
                        inferExpr(expr.operands[static_cast<size_t>(index + 1U)]);
                    argument_types.push_back(type_table.internSlice(element_sample));
                } else {
                    weak_args.push_back(false);
                    argument_types.push_back(kInvalidTypeId);
                }
            } else if (index + 1U < expr.operands.size()) {
                weak_args.push_back(weakLiteral(expr.operands[static_cast<size_t>(index + 1U)]));
                argument_types.push_back(inferExpr(expr.operands[static_cast<size_t>(index + 1U)]));
            } else if (callee_decl != nullptr && index < callee_decl->parameters.size() &&
                       callee_decl->parameters[index].defaultValue) {
                weak_args.push_back(false);
                argument_types.push_back(functionDefaultType(*callee_decl, index, *decl_sema));
            } else {
                weak_args.push_back(false);
                argument_types.push_back(kInvalidTypeId);
            }
        }

        std::vector<TypeId> args;
        const comptime::GenericResolveStatus resolved = instantiations->resolveArgs(
            *fn, generic_decl != nullptr ? generic_decl->genericParams.size() : 0U, decl_id.value,
            explicit_types, argument_types, expected_result, weak_args, args);
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
        const TypeId instance_type           = instantiations->substituteFunction(*fn, args);
        const auto *instance_fn              = type_table.function(instance_type);
        const VariadicCallPlan instance_plan = makeVariadicCallPlan(
            expr.span, expr.operands, instance_fn, is_variadic_slice, slice_index,
            expr.argIsSpread);
        typed_map.variadicCallPlans.insert(expr.id.value, instance_plan);
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
        if (instance_plan.autoCollectTail && instance_fn != nullptr) {
            (void)checkVariadicTail(expr.span, expr.operands, instance_fn, instance_plan.sliceParam,
                                    true);
        } else if (instance_plan.explicitSliceArg && instance_fn != nullptr) {
            const frontend::ExprId arg = expr.operands.back();
            const TypeId arg_type      = inferExpr(arg);
            std::vector<std::pair<frontend::ExprId, types::OwnershipKind>> roots;
            (void)checkOwnershipCoercion(arg, instance_fn->params[instance_plan.sliceParam], roots,
                                         expr.span, true);
            if (!coerceValue(arg, instance_fn->params[instance_plan.sliceParam], arg_type))
                reportCoercionFailure(expr.span, instance_fn->params[instance_plan.sliceParam],
                                      arg_type, "generic variadic slice argument type mismatch",
                                      diagnostics::err::NoMatchingFn);
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
VariadicCallPlan PerModuleSema::makeVariadicCallPlan(frontend::TextSpan span,
                                                     const std::vector<frontend::ExprId> &args,
                                                     const FunctionType *signature,
                                                     bool variadic_slice,
                                                     size_t fixed_explicit_args,
                                                     const std::vector<bool> &spread_args,
                                                     bool has_callee_prefix) {
    VariadicCallPlan plan;
    plan.span = span;
    if (signature == nullptr)
        return plan;
    plan.sliceParam = signature->params.size();
    if (!variadic_slice || signature->params.empty())
        return plan;
    const size_t slice_index   = signature->params.size() - 1U;
    plan.sliceParam            = slice_index;
    plan.sliceType             = signature->params[slice_index];
    plan.isVariadicSlice       = true;
    const size_t prefix_args = has_callee_prefix ? 1U : 0U;
    const size_t written_args = args.size() >= prefix_args ? args.size() - prefix_args : 0U;
    if (written_args == fixed_explicit_args + 1U && !args.empty()) {
        if (spread_args.size() == written_args && spread_args.back()) {
            plan.explicitSliceArg = true;
        }
    }
    plan.autoCollectTail = !plan.explicitSliceArg;
    return plan;
}
bool PerModuleSema::spreadMarkersAreValid(const std::vector<bool> &spread_args,
                                          size_t written_args,
                                          size_t fixed_explicit_args) const noexcept {
    if (spread_args.size() > written_args)
        return false;
    size_t spread_count = 0U;
    for (size_t index = 0; index < spread_args.size(); ++index) {
        if (!spread_args[index])
            continue;
        ++spread_count;
        if (index + 1U != written_args || index < fixed_explicit_args)
            return false;
    }
    return spread_count <= 1U;
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
            // A reified generic struct carries its concrete arguments in
            // `args` even when the parameters do not appear in any field
            // (for example `HashMap<K, V>` whose only field is a counter).
            // Without this scan such a struct would be classified as
            // non-generic and generic argument inference would never run.
            for (const auto arg : st->args)
                if (self(self, arg, depth + 1U))
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
