#include "sema/sema-modern-utils.hpp"
#include "sema/sema-modern.hpp"
#include <algorithm>
#include <string>

namespace zith::sema::modern {

std::vector<PerModuleSema::ResolvedMethod>
PerModuleSema::findMethodsForOwner(std::string_view owner_name,
                                   std::string_view method_name) const {
    std::vector<ResolvedMethod> methods;
    const auto ownerMatches = [owner_name](std::string_view candidate) {
        if (candidate == owner_name)
            return true;
        const auto baseName = [](std::string_view name) {
            if (const size_t angle = name.find('<'); angle != std::string_view::npos)
                return name.substr(0, angle);
            return name;
        };
        const std::string_view want = baseName(owner_name);
        return !want.empty() && want == baseName(candidate);
    };
    const auto matches = [&](const frontend::Declaration &decl) {
        return decl.kind == frontend::DeclKind::Function && ownerMatches(decl.ownerName) &&
               decl.name == method_name;
    };

    // Keep the existing local-first behavior: a method declared in the current
    // module must continue to take precedence over same-named imports.
    for (const auto &decl : snapshot.declarations()) {
        if (matches(decl)) {
            const bool is_trait_method = !decl.ownerName.empty() && !decl.traitName.empty() &&
                                         decl.ownerName == decl.traitName;
            methods.push_back(ResolvedMethod{module, &decl, decl.traitName, is_trait_method});
        }
    }
    if (owner != nullptr) {
        for (const auto &artifact_ptr : owner->modules()) {
            const auto &artifact = *artifact_ptr;
            if (artifact.key == module || artifact.frontend == nullptr)
                continue;
            for (const auto &decl : artifact.frontend->declarations()) {
                if (matches(decl)) {
                    const bool is_trait_method = !decl.ownerName.empty() &&
                                                 !decl.traitName.empty() &&
                                                 decl.ownerName == decl.traitName;
                    methods.push_back(
                        ResolvedMethod{artifact.key, &decl, decl.traitName, is_trait_method});
                }
            }
        }
    }

    // Trait defaults are exposed on every owner that satisfies their trait.
    // They are collected after owner methods so local/impl methods dominate
    // method-name overload selection.
    const TypeId owner_type     = ownerTypeFromName(owner_name);
    const auto addTraitDefaults = [&](const frontend::FrontendSnapshot &snap,
                                      session::ModuleKey snap_module) {
        for (const auto &decl : snap.declarations()) {
            if (decl.kind != frontend::DeclKind::Function || !decl.body || decl.ownerName.empty() ||
                decl.traitName != decl.ownerName || decl.name != method_name)
                continue;
            const TypeId trait_type = type_table.lookupNamed(decl.traitName);
            if (!trait_type || !satisfiesConformance(owner_type, trait_type))
                continue;
            methods.push_back(ResolvedMethod{snap_module, &decl, decl.traitName, true});
        }
    };
    addTraitDefaults(snapshot, module);
    if (owner != nullptr) {
        for (const auto &artifact_ptr : owner->modules()) {
            const auto &artifact = *artifact_ptr;
            if (artifact.frontend == nullptr || artifact.key == module)
                continue;
            addTraitDefaults(*artifact.frontend, artifact.key);
        }
    }
    return methods;
}
TypeId PerModuleSema::inferMethodCall(const frontend::Expression &call,
                                      const frontend::Expression &callee) {
    // `p.Trait.method()` is parsed as `Call(Field(Field(p, Trait), method))`.
    // The intermediate `p.Trait` is not a real field; it names a trait or
    // interface that the receiver type satisfies, so resolve the method within
    // that trait before falling back to the ordinary method lookup.
    frontend::ExprId receiver_id = callee.operands[0];
    std::string qualifying_trait;
    if (receiver_id && receiver_id.value <= snapshot.expressions().size()) {
        const auto &outer = snapshot.expressions()[receiver_id.value - 1U];
        if ((outer.kind == frontend::ExprKind::Field || outer.kind == frontend::ExprKind::Arrow) &&
            !outer.operands.empty()) {
            const TypeId owner_base     = inferExpr(outer.operands[0]);
            const TypeId pointee        = resolve(owner_base);
            const TypeKind pointee_kind = type_table.kindOf(pointee);
            if (pointee && (pointee_kind == TypeKind::Struct || pointee_kind == TypeKind::Enum ||
                            pointee_kind == TypeKind::Union)) {
                const TypeId trait_type = type_table.lookupNamed(outer.text);
                if (trait_type && type_table.kindOf(trait_type) == TypeKind::Trait &&
                    satisfiesConformance(pointee, trait_type)) {
                    qualifying_trait = outer.text;
                    typed_map.traitQualifiedReceiverBase.insert(receiver_id.value,
                                                                outer.operands[0].value);
                    setExprType(receiver_id, pointee);
                    receiver_id = outer.operands[0];
                }
            }
        }
    }

    const TypeId base_type = inferExpr(receiver_id);
    if (!base_type || type_table.kindOf(base_type) == TypeKind::Error)
        return kInvalidTypeId;

    // Unwrap pointer/optional to find the struct name. `resolve` also strips
    // memory qualifiers, so `p: lend Point` still finds Point's methods.
    TypeId pointee  = resolve(base_type);
    bool is_pointer = false;
    if (type_table.kindOf(pointee) == TypeKind::Pointer) {
        if (!findMethodsForOwner(ownerNameOf(pointee), callee.text).empty()) {
            // `implement *char` owns the pointer type itself, so a `*char`
            // receiver is passed by value rather than being unwrapped to its
            // pointee.
            is_pointer = true;
        } else if (const auto *ptr = type_table.pointer(pointee)) {
            pointee    = resolve(ptr->pointee);
            is_pointer = true;
        }
    } else if (type_table.kindOf(pointee) == TypeKind::Optional) {
        if (const auto *opt = type_table.optional(pointee)) {
            // An exact `?T` implementation owns the aggregate; otherwise keep
            // the existing `?Struct` -> `Struct` fallback and the `?*T` C
            // pointer receiver behavior.
            const TypeId optional_owner = resolve(base_type);
            if (!findMethodsForOwner(ownerNameOf(optional_owner), callee.text).empty()) {
                pointee = optional_owner;
            } else {
                pointee = resolve(opt->inner);
                // C pointers are modeled as `?*T`. While the optional wrapper
                // is still present in the expression/lvalue type, method calls
                // pass the pointer value itself as the receiver argument.
                if (const auto *ptr = type_table.pointer(pointee)) {
                    pointee    = resolve(ptr->pointee);
                    is_pointer = true;
                }
            }
        }
    }

    const TypeId resolved_base = resolve(base_type);
    if (type_table.kindOf(resolved_base) == TypeKind::Dyn)
        return inferDynMethodCall(call, callee, resolved_base);

    const StructType *st = type_table.struct_type(pointee);
    if (st == nullptr && type_table.kindOf(pointee) == TypeKind::GenericParam) {
        // A generic parameter is only callable as `T.method()` when one of its
        // bounds is a trait whose method set contains `method`. Substitution of
        // `Self` makes the trait signature usable for `T`.
        const std::vector<TypeId> bounds = boundsForGenericParam(pointee);
        std::vector<const frontend::Declaration *> bound_decls;
        std::vector<session::ModuleKey> bound_modules;
        std::vector<TypeId> bound_traits;
        for (const TypeId bound : bounds) {
            const auto *trait_ty = type_table.trait(resolve(bound));
            if (trait_ty == nullptr)
                continue;
            for (const auto &decl : snapshot.declarations()) {
                if (decl.kind != frontend::DeclKind::Function || decl.ownerName != trait_ty->name ||
                    decl.name != callee.text)
                    continue;
                bound_decls.push_back(&decl);
                bound_modules.push_back(module);
                bound_traits.push_back(bound);
                break;
            }
            if (owner != nullptr) {
                for (const auto &artifact_ptr : owner->modules()) {
                    const auto &artifact = *artifact_ptr;
                    if (artifact.frontend == nullptr || artifact.key == module)
                        continue;
                    bool found = false;
                    for (const auto &decl : artifact.frontend->declarations()) {
                        if (decl.kind != frontend::DeclKind::Function ||
                            decl.ownerName != trait_ty->name || decl.name != callee.text)
                            continue;
                        bound_decls.push_back(&decl);
                        bound_modules.push_back(artifact.key);
                        bound_traits.push_back(bound);
                        found = true;
                        break;
                    }
                    if (found)
                        break;
                }
            }
        }
        if (bound_decls.empty())
            return kInvalidTypeId;

        // A single declaration can be reachable from multiple bounds (for
        // example `T: A + B` where both bounds declare the same default
        // method). Only report ambiguity when distinct declarations exist.
        std::vector<const frontend::Declaration *> unique_decls;
        for (const auto *decl : bound_decls) {
            const bool seen = std::any_of(
                unique_decls.begin(), unique_decls.end(),
                [decl](const frontend::Declaration *existing) { return existing->id == decl->id; });
            if (!seen)
                unique_decls.push_back(decl);
        }
        if (unique_decls.size() != 1U) {
            report(call.span,
                   "method call is ambiguous on generic parameter '" +
                       type_table.typeToString(pointee) + "'",
                   diagnostics::err::AmbiguousCall);
            return error_type;
        }

        const frontend::Declaration *method_decl = bound_decls.front();
        const session::ModuleKey method_module   = bound_modules.front();
        PerModuleSema *method_sema =
            owner != nullptr ? owner->findModuleSema(method_module) : nullptr;
        const TypeId fn_type = method_sema != nullptr ? method_sema->typeOfDecl(method_decl->id)
                                                      : typeOfDecl(method_decl->id);
        const auto *fn       = type_table.function(fn_type);
        if (fn == nullptr)
            return kInvalidTypeId;

        // `T.parse(...)` reaches this receiver path for trait methods whose
        // first parameter is an explicit self. The base of the Field callee
        // is the generic type parameter itself, and the provided call arity
        // matches the raw trait signature: one receiver parameter plus any
        // explicit args. Ordinary `v.parse(...)` field calls still substitute
        // the concrete owner and add an implicit receiver, so they must keep
        // the old path.
        const bool static_bound_base = [&]() {
            if (callee.operands.empty())
                return false;
            const auto &base = snapshot.expressions()[callee.operands[0].value - 1U];
            return base.kind == frontend::ExprKind::Name &&
                   sameType(genericParamTypeByName(base.text), pointee);
        }();
        bool static_bound_call =
            static_bound_base && !isInterfaceType(resolve(bound_traits.front()));
        const bool trait_requirement = method_decl->traitName == method_decl->ownerName;
        const bool has_receiver =
            !method_decl->parameters.empty() && method_decl->parameters.front().name == "self";
        const size_t provided_args = call.operands.size() - 1U;
        static_bound_call = static_bound_call && trait_requirement && has_receiver && fn != nullptr &&
                            provided_args >= 1U &&
                            type_table.kindOf(pointee) == TypeKind::GenericParam;
        const TypeId substituted = [&]() {
            if (!static_bound_call)
                return substituteSelf(fn_type, pointee, bound_traits.front());
            auto &params = type_table.makeTypeStorage();
            for (const TypeId param : fn->params)
                params.push(param);
            return type_table.internFunction(
                params, substituteSelf(fn->result, pointee, bound_traits.front()));
        }();
        const auto *sub_fn = type_table.function(substituted);
        if (sub_fn == nullptr)
            return kInvalidTypeId;

        const bool explicit_receiver_arg = static_bound_call && has_receiver;
        const bool target_is_slice =
            !method_decl->parameters.empty() && method_decl->parameters.back().isVariadicSlice;
        const size_t slice_param_index =
            target_is_slice ? sub_fn->params.size() - 1U : sub_fn->params.size();
        const size_t fixed_explicit_args = target_is_slice
                                               ? slice_param_index - (has_receiver ? 1U : 0U)
                                               : sub_fn->params.size() - (has_receiver ? 1U : 0U);
        const size_t checked_explicit_args =
            explicit_receiver_arg ? provided_args - 1U : provided_args;
        const bool explicit_slice_arg = [&]() {
            if (!target_is_slice || provided_args != fixed_explicit_args + 1U ||
                call.operands.empty())
                return false;
            (void)inferExpr(call.operands.back());
            return variadicFinalArgIsExplicitSlice(sub_fn->params[slice_param_index], call.operands,
                                                   fixed_explicit_args);
        }();
        const bool auto_collected_tail = target_is_slice && !explicit_slice_arg;
        const bool defaults_cover =
            provided_args < fixed_explicit_args &&
            missingArgsHaveDefaults(*method_decl, provided_args, has_receiver ? 1U : 0U,
                                    target_is_slice ? slice_param_index : ~static_cast<size_t>(0));
        if (defaults_cover) {
            // Missing trailing fixed arguments are supplied from defaults.
        } else if (checked_explicit_args < fixed_explicit_args ||
                   (!target_is_slice && checked_explicit_args != fixed_explicit_args) ||
                   (target_is_slice && !auto_collected_tail && !explicit_slice_arg &&
                    checked_explicit_args != fixed_explicit_args)) {
            report(call.span, "method call arity mismatch", diagnostics::err::NoMatchingFn);
            return error_type;
        }
        std::vector<std::pair<frontend::ExprId, types::OwnershipKind>> seen_roots;
        if (explicit_receiver_arg) {
            // `T.parse(self)` calls the trait requirement with an explicit
            // receiver argument. The trait's first parameter is checked
            // against the calling expression like any ordinary argument.
            const TypeId arg_type = inferExpr(call.operands[1]);
            (void)checkOwnershipCoercion(call.operands[1], fn->params[0], seen_roots, call.span,
                                         true);
            if (!coerceValue(call.operands[1], fn->params[0], arg_type))
                reportCoercionFailure(call.span, fn->params[0], arg_type,
                                      "method call argument type mismatch",
                                      diagnostics::err::NoMatchingFn);
        }
        for (size_t explicit_index = 0U; explicit_index < fixed_explicit_args; ++explicit_index) {
            const size_t param_index = has_receiver ? explicit_index + 1U : explicit_index;
            const size_t arg_index =
                explicit_receiver_arg ? explicit_index + 2U : explicit_index + 1U;
            if (arg_index < call.operands.size()) {
                const TypeId arg_type = inferExpr(call.operands[arg_index]);
                (void)checkOwnershipCoercion(call.operands[arg_index], sub_fn->params[param_index],
                                             seen_roots, call.span, true);
                if (!coerceValue(call.operands[arg_index], sub_fn->params[param_index], arg_type))
                    reportCoercionFailure(call.span, sub_fn->params[param_index], arg_type,
                                          "method call argument type mismatch",
                                          diagnostics::err::NoMatchingFn);
            }
        }
        if (auto_collected_tail && target_is_slice)
            (void)checkVariadicTailArgs(call.span, call.operands, sub_fn->params[slice_param_index],
                                        fixed_explicit_args + 1U, true);
        setExprType(callee.id, static_bound_call
                                   ? substituteSelf(fn_type, pointee, bound_traits.front())
                                   : substituted);
        setResolvedCallTarget(callee.id, method_module, method_decl->id);
        const TypeId result_type =
            static_bound_call ? substituteSelf(sub_fn->result, pointee, bound_traits.front())
                              : sub_fn->result;
        return result_type;
    }
    const TypeKind pointee_kind = type_table.kindOf(pointee);
    if (st == nullptr && pointee_kind != TypeKind::Enum && pointee_kind != TypeKind::Union) {
        // Primitive, optional and slice receivers are allowed on implementation
        // methods. If no method exists, fall through to the normal field-access
        // error path instead of inventing a field.
        if (pointee_kind == TypeKind::Integer || pointee_kind == TypeKind::Float ||
            pointee_kind == TypeKind::Bool || pointee_kind == TypeKind::Char ||
            pointee_kind == TypeKind::String || pointee_kind == TypeKind::Optional ||
            pointee_kind == TypeKind::Slice || pointee_kind == TypeKind::Pointer) {
            return resolveStructMethodCall(call, callee,
                                           findMethodsForOwner(ownerNameOf(pointee), callee.text),
                                           base_type, pointee, is_pointer);
        }
        return kInvalidTypeId; // not a struct receiver: let normal call resolution run
    }

    if (qualifying_trait.empty())
        return resolveStructMethodCall(call, callee,
                                       findMethodsForOwner(ownerNameOf(pointee), callee.text),
                                       base_type, pointee, is_pointer);

    // `p.Trait.method()` selects only declarations owned by that trait. For a
    // declaration-only requirement, the concrete owner method that satisfies it
    // is the callable target; an explicit impl method has the trait name and
    // must be resolved through the trait too.
    const std::string owner_name = ownerNameOf(pointee);
    const std::string trait_name = [&]() {
        std::string name = qualifying_trait;
        if (const size_t angle = name.find('<'); angle != std::string::npos)
            name.resize(angle);
        return name;
    }();
    const TypeId trait_type     = type_table.lookupNamed(trait_name);
    const bool interface_target = trait_type && isInterfaceType(trait_type);
    std::vector<ResolvedMethod> qualified;
    for (const auto &method : findMethodsForOwner(owner_name, callee.text)) {
        if (method.decl == nullptr)
            continue;
        const bool from_trait =
            !method.decl->traitName.empty() && method.decl->traitName == trait_name;
        const bool trait_requirement  = method.isTraitMethod && method.traitName == trait_name;
        const bool interface_concrete = interface_target && !method.isTraitMethod;
        if (from_trait || trait_requirement || interface_concrete)
            qualified.push_back(method);
    }
    if (qualified.empty() || !trait_type) {
        report(call.span,
               "type '" + type_table.typeToString(pointee) + "' has no member '" + callee.text +
                   "' through trait '" + trait_name + "'",
               diagnostics::err::NoMember);
        return error_type;
    }
    return resolveStructMethodCall(call, callee, qualified, base_type, pointee, is_pointer);
}
TypeId PerModuleSema::inferDynMethodCall(const frontend::Expression &call,
                                         const frontend::Expression &callee, TypeId dyn_type) {
    const auto *dyn = type_table.dyn_type(dyn_type);
    if (dyn == nullptr)
        return kInvalidTypeId;
    const TypeId target  = resolve(dyn->target);
    const auto *trait_ty = type_table.trait(target);
    if (trait_ty == nullptr)
        return kInvalidTypeId;

    const auto findMethod =
        [&](const frontend::FrontendSnapshot &snap, session::ModuleKey snap_module,
            const frontend::Declaration **out_decl, session::ModuleKey *out_module) {
            for (const auto &decl : snap.declarations()) {
                if (decl.kind != frontend::DeclKind::Function || decl.ownerName != trait_ty->name ||
                    decl.name != callee.text)
                    continue;
                *out_decl   = &decl;
                *out_module = snap_module;
                return true;
            }
            return false;
        };

    const frontend::Declaration *method_decl = nullptr;
    session::ModuleKey method_module;
    if (findMethod(snapshot, module, &method_decl, &method_module)) {
        // already set below through the general local-first path.
    } else if (owner != nullptr) {
        for (const auto &artifact_ptr : owner->modules()) {
            const auto &artifact = *artifact_ptr;
            if (artifact.frontend == nullptr || artifact.key == module)
                continue;
            if (findMethod(*artifact.frontend, artifact.key, &method_decl, &method_module))
                break;
        }
    }
    if (method_decl == nullptr) {
        report(call.span,
               "dyn type '" + type_table.typeToString(target) + "' has no method '" + callee.text +
                   "'",
               diagnostics::err::NoMember);
        return error_type;
    }

    PerModuleSema *method_sema = owner != nullptr ? owner->findModuleSema(method_module) : nullptr;
    const TypeId method_type   = method_sema != nullptr ? method_sema->typeOfDecl(method_decl->id)
                                                        : typeOfDecl(method_decl->id);
    const auto *fn             = type_table.function(method_type);
    if (fn == nullptr)
        return kInvalidTypeId;

    const bool has_receiver =
        !method_decl->parameters.empty() && method_decl->parameters.front().name == "self";
    const size_t provided_args = call.operands.size() - 1U;
    const bool target_is_slice =
        !method_decl->parameters.empty() && method_decl->parameters.back().isVariadicSlice;
    const size_t slice_param_index   = target_is_slice ? fn->params.size() - 1U : fn->params.size();
    const size_t fixed_explicit_args = target_is_slice
                                           ? slice_param_index - (has_receiver ? 1U : 0U)
                                           : fn->params.size() - (has_receiver ? 1U : 0U);
    const bool explicit_slice_arg    = [&]() {
        if (!target_is_slice || provided_args != fixed_explicit_args + 1U || call.operands.empty())
            return false;
        (void)inferExpr(call.operands.back());
        return variadicFinalArgIsExplicitSlice(fn->params[slice_param_index], call.operands,
                                                  fixed_explicit_args);
    }();
    const bool auto_collected_tail = target_is_slice && !explicit_slice_arg;
    const bool defaults_cover =
        provided_args < fixed_explicit_args &&
        missingArgsHaveDefaults(*method_decl, provided_args, has_receiver ? 1U : 0U,
                                target_is_slice ? slice_param_index : ~static_cast<size_t>(0));
    if (defaults_cover) {
        // Missing trailing fixed arguments are supplied from defaults.
    } else if (provided_args < fixed_explicit_args ||
               (!target_is_slice && provided_args != fixed_explicit_args) ||
               (target_is_slice && !auto_collected_tail && !explicit_slice_arg &&
                provided_args != fixed_explicit_args)) {
        report(call.span, "method call arity mismatch", diagnostics::err::NoMatchingFn);
        return error_type;
    }

    std::vector<std::pair<frontend::ExprId, types::OwnershipKind>> seen_roots;
    for (size_t explicit_index = 0U; explicit_index < fixed_explicit_args; ++explicit_index) {
        const size_t param_index = has_receiver ? explicit_index + 1U : explicit_index;
        const size_t arg_index   = explicit_index + 1U;
        if (arg_index < call.operands.size()) {
            const TypeId arg_type = inferExpr(call.operands[arg_index]);
            (void)checkOwnershipCoercion(call.operands[arg_index], fn->params[param_index],
                                         seen_roots, call.span, true);
            if (!coerceValue(call.operands[arg_index], fn->params[param_index], arg_type))
                reportCoercionFailure(call.span, fn->params[param_index], arg_type,
                                      "method call argument type mismatch",
                                      diagnostics::err::NoMatchingFn);
        }
    }
    if (auto_collected_tail && target_is_slice)
        (void)checkVariadicTailArgs(call.span, call.operands, fn->params[slice_param_index],
                                    fixed_explicit_args + 1U, true);

    setExprType(callee.id, method_type);
    setResolvedCallTarget(callee.id, method_module, method_decl->id);
    return fn->result;
}
bool PerModuleSema::satisfiesConformance(TypeId type, TypeId trait_or_interface) const {
    const TypeId concrete = resolve(type);
    const TypeId target   = resolve(trait_or_interface);
    if (!concrete || !target)
        return false;
    if (type_table.conformanceTable().satisfies(concrete, target))
        return true;

    // Interfaces are structural: compare the declared fields and methods of the
    // interface against the members of the concrete type. The interface must be
    // a named Interface declaration; its `parameters` are the field list and
    // its method requirements are Function declarations with `ownerName` set.
    if (type_table.kindOf(target) != TypeKind::Trait)
        return false;
    const auto *interface_ty = type_table.trait(target);
    if (interface_ty == nullptr || !isInterfaceType(target))
        return false;

    const frontend::Declaration *interface_decl =
        findDeclNamed(interface_ty->name, frontend::DeclKind::Interface);
    if (interface_decl == nullptr)
        return false;

    const auto *struct_type = type_table.struct_type(concrete);
    const auto *nominal     = type_table.nominal(concrete);
    if (struct_type == nullptr && nominal == nullptr)
        return false;

    // Generic/interface-only types cannot be checked structurally.
    if (struct_type == nullptr)
        return false;

    for (const auto &required : interface_decl->parameters) {
        const int index = type_table.fieldIndex(concrete, required.name);
        if (index < 0)
            return false;
        const TypeId required_type = lowerTypeExprConst(required.type);
        const TypeId actual_type   = index < static_cast<int>(struct_type->fields.size())
                                         ? struct_type->fields[static_cast<size_t>(index)]
                                         : kInvalidTypeId;
        if (required_type && actual_type && !sameType(required_type, actual_type))
            return false;
    }
    const auto checkMethod = [&](const frontend::Declaration &requirement,
                                 const frontend::Declaration &candidate,
                                 session::ModuleKey requirement_module,
                                 session::ModuleKey candidate_module) {
        if (requirement.parameters.size() != candidate.parameters.size())
            return false;
        const auto req_sema =
            owner != nullptr ? owner->findModuleSema(requirement_module) : nullptr;
        const auto cand_sema = owner != nullptr ? owner->findModuleSema(candidate_module) : nullptr;
        const auto *req_type =
            type_table.function(req_sema != nullptr ? req_sema->typeOfDecl(requirement.id)
                                                    : typeOfDecl(requirement.id));
        const auto *cand_type = type_table.function(
            cand_sema != nullptr ? cand_sema->typeOfDecl(candidate.id) : typeOfDecl(candidate.id));
        if (req_type == nullptr || cand_type == nullptr ||
            req_type->params.size() != cand_type->params.size())
            return false;
        if (!sameType(substituteSelf(cand_type->result, concrete, target),
                      substituteSelf(req_type->result, concrete, target)))
            return false;
        for (size_t index = 0; index < req_type->params.size(); ++index) {
            const TypeId expected = substituteSelf(req_type->params[index], concrete, target);
            if (expected && !sameType(expected, cand_type->params[index]))
                return false;
        }
        return true;
    };
    const auto checkRequirements = [&](const frontend::FrontendSnapshot &snap,
                                       session::ModuleKey snap_module) {
        for (const auto &requirement : snap.declarations()) {
            if (requirement.kind != frontend::DeclKind::Function ||
                requirement.ownerName != interface_ty->name || requirement.body)
                continue;
            const auto methods =
                findMethodsForOwner(std::string_view{struct_type->name}, requirement.name);
            bool matched = false;
            for (const auto &method : methods) {
                if (method.isTraitMethod)
                    continue;
                if (checkMethod(requirement, *method.decl, snap_module, method.module)) {
                    matched = true;
                    break;
                }
            }
            if (!matched)
                return false;
        }
        return true;
    };
    if (!checkRequirements(snapshot, module))
        return false;
    if (owner != nullptr) {
        for (const auto &artifact_ptr : owner->modules()) {
            const auto &artifact = *artifact_ptr;
            if (artifact.key == module || artifact.frontend == nullptr)
                continue;
            if (!checkRequirements(*artifact.frontend, artifact.key))
                return false;
        }
    }
    return true;
}
void PerModuleSema::checkGenericConstraints(const frontend::Declaration &generic_decl,
                                            const std::vector<TypeId> &args,
                                            frontend::TextSpan span) {
    const auto *found = genericParams_.get(generic_decl.id.value);
    if (!found)
        return;
    for (size_t index = 0; index < args.size() && index < found->size(); ++index) {
        for (const TypeId bound : (*found)[index].bounds) {
            if (!satisfiesConformance(args[index], bound)) {
                const diagnostics::ErrCode code = isInterfaceType(bound)
                                                      ? diagnostics::err::InterfaceNotSatisfied
                                                      : diagnostics::err::ConstraintNotSatisfied;
                report(span,
                       "type '" + type_table.typeToString(args[index]) +
                           "' does not satisfy constraint '" + type_table.typeToString(bound) + "'",
                       code);
            }
        }
    }
}
std::vector<TypeId> PerModuleSema::boundsForGenericParam(TypeId generic_type) const {
    uint32_t decl_id   = 0;
    uint32_t param_idx = 0;
    type_table.genericParamOrigin(generic_type, &decl_id, &param_idx);
    const auto *found = genericParams_.get(decl_id);
    if (!found || param_idx >= found->size())
        return {};
    return (*found)[param_idx].bounds;
}

/// Try to resolve `expr` (a Call whose callee is a Field/Arrow) as a
/// method call on the base type. Returns the result TypeId on success,
/// or `kInvalidTypeId` (with a diagnostic already reported) when the
/// field is not a method.
std::string PerModuleSema::ownerNameOf(TypeId pointee) const {
    const TypeKind kind = type_table.kindOf(pointee);
    if (kind == TypeKind::Integer || kind == TypeKind::Float || kind == TypeKind::Bool ||
        kind == TypeKind::Char || kind == TypeKind::String || kind == TypeKind::Optional ||
        kind == TypeKind::Slice || kind == TypeKind::Pointer) {
        return type_table.typeToString(type_table.canonical(pointee));
    }
    const auto *st = type_table.struct_type(pointee);
    const auto *et = type_table.enum_type(pointee);
    const auto *ut = type_table.union_type(pointee);
    if (st == nullptr && et == nullptr && ut == nullptr)
        return {};
    std::string owner_name;
    if (st != nullptr)
        owner_name = st->name;
    else if (et != nullptr)
        owner_name = et->name;
    else if (ut != nullptr)
        owner_name = ut->name;
    if (const size_t angle = owner_name.find('<'); angle != std::string::npos)
        owner_name.resize(angle);
    return owner_name;
}
TypeId PerModuleSema::genericParamTypeByName(std::string_view name) const {
    if (currentDeclId_ == 0U)
        return kInvalidTypeId;
    const auto *found = genericParams_.get(currentDeclId_);
    if (found == nullptr)
        return kInvalidTypeId;
    for (const auto &binding : *found) {
        if (binding.name == name)
            return binding.type;
    }
    return kInvalidTypeId;
}
bool PerModuleSema::isGenericTypeParamName(std::string_view name, uint32_t decl_id) const noexcept {
    if (name.empty() || decl_id == 0U)
        return false;
    const auto *found = genericParams_.get(decl_id);
    if (!found)
        return false;
    for (const auto &binding : *found) {
        if (binding.name == name)
            return true;
    }
    return false;
}
std::vector<TypeId> PerModuleSema::unionArgsFor(TypeId type) const noexcept {
    const auto *union_data = type_table.union_type(type);
    if (union_data == nullptr)
        return {};
    std::vector<TypeId> args;
    args.reserve(union_data->members.size());
    for (const auto member : union_data->members)
        args.push_back(member);
    return args;
}
TypeId PerModuleSema::ownerTypeFromName(std::string_view owner_name) const {
    // Named owners have to be re-resolved after `lowerDeclarationTypes`
    // registers the completed struct/alias/union/enum under the name. The
    // pre-lowering placeholder stored in `implementOwnerTypes_` is only
    // suitable as a fallback and, for composites, is always interned before
    // the method signatures are lowered.
    if (const TypeId named = type_table.lookupNamed(owner_name))
        return named;
    if (const auto *found = implementOwnerTypes_.get(owner_name))
        return *found;
    return type_table.lookupNamed(owner_name);
}
TypeId PerModuleSema::resolveStructMethodCall(const frontend::Expression &call,
                                              const frontend::Expression &callee,
                                              const std::vector<ResolvedMethod> &resolved_methods,
                                              TypeId base_type, TypeId pointee, bool is_pointer) {
    // Collect every method of this owner with the callee's name: methods take part
    // in the same overload resolution as free functions.
    std::vector<const frontend::Declaration *> method_decls;
    std::vector<session::ModuleKey> method_modules;
    std::vector<std::string> method_trait_names;
    std::vector<const frontend::Declaration *> default_decls;
    std::vector<session::ModuleKey> default_modules;
    std::vector<std::string> default_trait_names;
    const bool has_owner_local_method =
        std::any_of(resolved_methods.begin(), resolved_methods.end(), [](const ResolvedMethod &m) {
            return !m.isTraitMethod && m.decl != nullptr && m.decl->traitName.empty();
        });
    for (const auto &method : resolved_methods) {
        const bool is_trait_decl = method.isTraitMethod;
        if (is_trait_decl) {
            const TypeId trait_type = type_table.lookupNamed(method.traitName);
            if (!trait_type || !satisfiesConformance(pointee, trait_type))
                continue;
            default_decls.push_back(method.decl);
            default_modules.push_back(method.module);
            default_trait_names.push_back(method.traitName);
            continue;
        }
        // An owner-local method shadows an impl method of the same owner when
        // both are visible through an ordinary `obj.method()` call. Qualified
        // trait calls still reach impl methods because they supply no
        // owner-local candidate and keep the impl method in `resolved_methods`.
        if (has_owner_local_method && !method.traitName.empty())
            continue;
        method_decls.push_back(method.decl);
        method_modules.push_back(method.module);
        method_trait_names.push_back(method.traitName);
    }
    // Concrete owner/impl methods override trait defaults with the same name.
    if (method_decls.empty()) {
        method_decls       = std::move(default_decls);
        method_modules     = std::move(default_modules);
        method_trait_names = std::move(default_trait_names);
    }
    if (method_decls.empty())
        return kInvalidTypeId; // no such method: may still be a callable field

    if (method_decls.size() == 1U && !method_decls.front()->genericParams.empty()) {
        const frontend::Declaration *method_decl = method_decls.front();
        const session::ModuleKey method_module   = method_modules.front();
        PerModuleSema *method_sema =
            owner != nullptr ? owner->findModuleSema(method_module) : nullptr;
        const auto saved_decl_id   = currentDeclId_;
        const auto saved_kind      = currentFunctionKind_;
        const size_t provided_args = call.operands.size() - 1U;
        const bool has_receiver_entry =
            !method_decl->parameters.empty() && method_decl->parameters.front().name == "self";
        const bool generic_decl_is_slice =
            !method_decl->parameters.empty() && method_decl->parameters.back().isVariadicSlice;
        const auto *generic_method_fn =
            method_sema != nullptr ? type_table.function(method_sema->typeOfDecl(method_decl->id))
                                   : nullptr;
        const size_t generic_slice_param_index =
            generic_decl_is_slice && generic_method_fn != nullptr
                ? generic_method_fn->params.size() - 1U
                : (generic_method_fn != nullptr ? generic_method_fn->params.size()
                                                : method_decl->parameters.size());
        const size_t generic_fixed_explicit =
            generic_decl_is_slice ? generic_slice_param_index - (has_receiver_entry ? 1U : 0U)
                                  : generic_slice_param_index - (has_receiver_entry ? 1U : 0U);
        const bool generic_explicit_slice = [&]() {
            if (!generic_decl_is_slice || provided_args != generic_fixed_explicit + 1U ||
                call.operands.empty())
                return false;
            (void)inferExpr(call.operands.back());
            return variadicFinalArgIsExplicitSlice(
                generic_method_fn->params[generic_slice_param_index], call.operands,
                generic_fixed_explicit);
        }();
        const bool generic_auto_collect = generic_decl_is_slice && !generic_explicit_slice;
        const bool defaults_cover =
            provided_args < generic_fixed_explicit &&
            missingArgsHaveDefaults(*method_decl, provided_args, has_receiver_entry ? 1U : 0U,
                                    generic_decl_is_slice ? generic_slice_param_index
                                                          : ~static_cast<size_t>(0));
        if (defaults_cover) {
            // Missing trailing fixed arguments are supplied from defaults.
        } else if (provided_args < generic_fixed_explicit ||
                   (!generic_decl_is_slice && provided_args != generic_fixed_explicit) ||
                   (generic_decl_is_slice && !generic_auto_collect && !generic_explicit_slice &&
                    provided_args != generic_fixed_explicit)) {
            report(call.span, "method call arity mismatch", diagnostics::err::NoMatchingFn);
            return error_type;
        }
        currentDeclId_       = method_decl->id.value;
        currentFunctionKind_ = method_decl->kind == frontend::DeclKind::Function
                                   ? method_decl->functionKind
                                   : frontend::FunctionKind::Standard;

        std::vector<TypeId> explicit_types;
        explicit_types.reserve(call.genericArgs.size());
        for (const auto generic_arg : call.genericArgs) {
            const TypeId lowered = lowerTypeExpr(generic_arg);
            if (!lowered) {
                report(call.span, "generic argument is not a concrete type",
                       diagnostics::err::GenericCannotInfer);
                currentDeclId_       = saved_decl_id;
                currentFunctionKind_ = saved_kind;
                return error_type;
            }
            explicit_types.push_back(lowered);
        }

        // Inline methods and `implement Owner<T>` methods inherit the owner's
        // generic parameters. Their lowering reuses the owner-decl generic
        // TypeId, so a concrete receiver carries the concrete arguments that
        // must be supplied to the monomorphizer before method-level inference.
        // Struct fields and union members retain concrete substituted types;
        // enums with positional C-style variants retain only discriminants, so
        // the concrete receiver name is the source of truth for their args.
        const frontend::Declaration *owner_template = nullptr;
        for (const auto &candidate : snapshot.declarations()) {
            if (candidate.name == ownerNameOf(pointee) && !candidate.genericParams.empty()) {
                owner_template = &candidate;
                break;
            }
        }
        const size_t call_generic_degree = method_decl->genericParams.size();
        const size_t owner_generic_count =
            owner_template != nullptr ? owner_template->genericParams.size() : call_generic_degree;
        std::vector<TypeId> inherited_args;
        if (owner_template != nullptr) {
            if (const auto *owner_ut = type_table.union_type(pointee)) {
                inherited_args.assign(owner_ut->members.begin(), owner_ut->members.end());
            } else if (const auto *owner_st = type_table.struct_type(pointee)) {
                inherited_args.assign(owner_st->fields.begin(), owner_st->fields.end());
            } else if (const auto *owner_et = type_table.enum_type(pointee)) {
                const char *begin = owner_et->name.data();
                const char *end   = begin + owner_et->name.size();
                const char *open  = std::find(begin, end, '<');
                const char *close = end;
                if (open != end) {
                    close = std::find(open + 1, end, '>');
                    while (open + 1 != close) {
                        const char *comma = std::find(open + 1, close, ',');
                        const std::string_view arg_text(open + 1,
                                                        static_cast<size_t>(comma - open - 1));
                        const TypeId named  = type_table.lookupNamed(arg_text);
                        TypeId concrete_arg = named;
                        if (!concrete_arg) {
                            if (arg_text == "i8")
                                concrete_arg = type_table.lookupNamed("i8");
                            else if (arg_text == "i16")
                                concrete_arg = type_table.lookupNamed("i16");
                            else if (arg_text == "i32")
                                concrete_arg = i32_type;
                            else if (arg_text == "i64")
                                concrete_arg = type_table.lookupNamed("i64");
                            else if (arg_text == "u8")
                                concrete_arg = type_table.lookupNamed("u8");
                            else if (arg_text == "u16")
                                concrete_arg = type_table.lookupNamed("u16");
                            else if (arg_text == "u32")
                                concrete_arg = type_table.lookupNamed("u32");
                            else if (arg_text == "u64")
                                concrete_arg = type_table.lookupNamed("u64");
                            else if (arg_text == "f32")
                                concrete_arg = type_table.lookupNamed("f32");
                            else if (arg_text == "f64")
                                concrete_arg = type_table.lookupNamed("f64");
                            else if (arg_text == "bool")
                                concrete_arg = bool_type;
                            else if (arg_text == "char")
                                concrete_arg = char_type;
                        }
                        if (concrete_arg)
                            inherited_args.push_back(concrete_arg);
                        if (comma == close)
                            break;
                        open = comma;
                    }
                }
            }
            if (inherited_args.size() > owner_generic_count)
                inherited_args.resize(owner_generic_count);
            while (inherited_args.size() < owner_generic_count)
                inherited_args.push_back(invalid_type);
        }
        bool all_inherited_valid = true;
        for (const TypeId arg : inherited_args) {
            if (!arg || arg == invalid_type) {
                all_inherited_valid = false;
                break;
            }
        }
        const bool inherited_resolved =
            !inherited_args.empty() && inherited_args.size() == owner_generic_count &&
            owner_generic_count == call_generic_degree && all_inherited_valid;

        std::vector<TypeId> argument_types;
        if (has_receiver_entry) {
            const TypeId self_type =
                method_decl->parameters.front().type
                    ? methodSelfParamType(method_decl->parameters.front())
                    : (is_pointer ? base_type : type_table.internPointer(pointee));
            if (!inherited_resolved)
                argument_types.push_back(self_type);
        }
        for (size_t index = has_receiver_entry ? 1U : 0U; index < method_decl->parameters.size();
             ++index) {
            if (generic_decl_is_slice && index == method_decl->parameters.size() - 1U) {
                // Make the variadic slice available to generic inference as a
                // `[]T` shape. When the caller passed an explicit slice/array,
                // use its full type; otherwise infer `T` from the first tail
                // element so `pick<i32>(10, 20, 30)` stays valid.
                if (generic_explicit_slice && !call.operands.empty()) {
                    argument_types.push_back(inferExpr(call.operands.back()));
                } else if (call.operands.size() > generic_fixed_explicit + 1U) {
                    const TypeId element = inferExpr(call.operands[generic_fixed_explicit + 1U]);
                    argument_types.push_back(type_table.internSlice(element));
                } else {
                    argument_types.push_back(type_table.internSlice(kInvalidTypeId));
                }
            } else if (index + 1U < call.operands.size())
                argument_types.push_back(inferExpr(call.operands[index + 1U]));
        }

        const auto *method_fn = method_sema != nullptr
                                    ? type_table.function(method_sema->typeOfDecl(method_decl->id))
                                    : nullptr;
        std::vector<TypeId> inferred_args;
        comptime::GenericResolveStatus resolved = comptime::GenericResolveStatus::CannotInfer;
        if (method_fn != nullptr) {
            resolved = inherited_resolved ? comptime::GenericResolveStatus::Ok
                       : instantiations != nullptr
                           ? instantiations->resolveArgs(*method_fn, call_generic_degree,
                                                         method_decl->id.value, explicit_types,
                                                         argument_types, inferred_args)
                           : comptime::GenericResolveStatus::CannotInfer;
        }
        if (inherited_resolved)
            inferred_args = inherited_args;
        currentDeclId_       = saved_decl_id;
        currentFunctionKind_ = saved_kind;
        switch (resolved) {
        case comptime::GenericResolveStatus::Arity:
            report(call.span, "wrong generic argument count", diagnostics::err::GenericArity);
            return error_type;
        case comptime::GenericResolveStatus::CannotInfer:
            report(call.span, "cannot infer generic argument; provide explicit type arguments",
                   diagnostics::err::GenericCannotInfer);
            return error_type;
        case comptime::GenericResolveStatus::Explosion:
            report(call.span, "too many generic instantiations",
                   diagnostics::err::GenericExplosion);
            return error_type;
        case comptime::GenericResolveStatus::Ok:
            break;
        }
        if (method_fn != nullptr && !inherited_resolved) {
            // `checkGenericConstraints` reads generic metadata bound only to
            // the module that owns the declaration. For an imported method
            // its bounds must still be reported at the real call site, so use
            // that module only for metadata lookup.
            const auto *bound_sema = method_sema != nullptr ? method_sema : this;
            const auto *found      = bound_sema->genericParams_.get(method_decl->id.value);
            if (found != nullptr) {
                for (size_t index = 0; index < inferred_args.size() && index < found->size();
                     ++index) {
                    for (const TypeId bound : (*found)[index].bounds) {
                        if (!satisfiesConformance(inferred_args[index], bound)) {
                            const diagnostics::ErrCode code =
                                isInterfaceType(bound) ? diagnostics::err::InterfaceNotSatisfied
                                                       : diagnostics::err::ConstraintNotSatisfied;
                            report(call.span,
                                   "type '" + type_table.typeToString(inferred_args[index]) +
                                       "' does not satisfy constraint '" +
                                       type_table.typeToString(bound) + "'",
                                   code);
                        }
                    }
                }
            }
        }

        if (method_fn != nullptr) {
            const size_t instance_index =
                instantiations != nullptr
                    ? instantiations->bindCall(module, callee.id, method_module, method_decl->id,
                                               inferred_args)
                    : ~size_t{0};
            if (instance_index == ~size_t{0}) {
                report(call.span, "too many generic instantiations",
                       diagnostics::err::GenericExplosion);
                return error_type;
            }
            const TypeId instance_type =
                instantiations->substituteFunction(*method_fn, inferred_args);
            setExprType(callee.id, instance_type);
            setResolvedCallTarget(callee.id, method_module, method_decl->id);
            std::vector<std::pair<frontend::ExprId, types::OwnershipKind>> seen_roots;
            const auto *instance_fn = type_table.function(instance_type);
            if (instance_fn != nullptr) {
                const size_t instance_slice_param_index = generic_decl_is_slice
                                                              ? instance_fn->params.size() - 1U
                                                              : instance_fn->params.size();
                for (size_t explicit_index = 0U; explicit_index < generic_fixed_explicit;
                     ++explicit_index) {
                    const size_t param_index =
                        has_receiver_entry ? explicit_index + 1U : explicit_index;
                    const size_t arg_index = explicit_index + 1U;
                    if (arg_index < call.operands.size())
                        (void)checkOwnershipCoercion(call.operands[arg_index],
                                                     instance_fn->params[param_index], seen_roots,
                                                     call.span, true);
                }
                if (generic_auto_collect && generic_decl_is_slice)
                    (void)checkVariadicTailArgs(call.span, call.operands,
                                                instance_fn->params[instance_slice_param_index],
                                                generic_fixed_explicit + 1U, true);
            }
            return instance_fn != nullptr ? instance_fn->result : error_type;
        }
        return error_type;
    }

    const frontend::Declaration *method_decl = method_decls.front();
    session::ModuleKey method_module         = method_modules.front();
    if (method_decls.size() > 1U) {
        // The lowered method type carries the receiver as its first parameter, so
        // selection runs with one implicit argument.
        std::vector<OverloadCandidate> candidates;
        candidates.reserve(method_decls.size());
        for (size_t index = 0; index < method_decls.size(); ++index) {
            const auto *decl    = method_decls[index];
            const auto decl_mod = method_modules[index];
            OverloadCandidate candidate;
            candidate.variadicSlice =
                !decl->parameters.empty() && decl->parameters.back().isVariadicSlice;
            const auto *decl_sema = owner != nullptr ? owner->findModuleSema(decl_mod) : nullptr;
            candidate.type =
                decl_sema != nullptr ? decl_sema->typeOfDecl(decl->id) : typeOfDecl(decl->id);
            candidate.fn     = type_table.function(candidate.type);
            candidate.span   = decl->span;
            candidate.module = decl_mod;
            candidate.decl   = decl->id;
            if (candidate.fn != nullptr && !typeContainsGeneric(candidate.fn))
                candidates.push_back(candidate);
        }
        if (candidates.size() > 1U) {
            bool reported      = false;
            const auto *chosen = selectOverload(call, candidates, 1U, reported);
            if (chosen == nullptr)
                return reported ? error_type : kInvalidTypeId;
            for (const auto *decl : method_decls) {
                if (decl->span == chosen->span) {
                    method_decl = decl;
                    break;
                }
            }
            const auto found = std::find(method_decls.begin(), method_decls.end(), method_decl);
            if (found != method_decls.end()) {
                const size_t chosen_index = static_cast<size_t>(found - method_decls.begin());
                method_module             = method_modules[chosen_index];
            }
        }
    }
    setResolvedCallTarget(callee.id, method_module, method_decl->id);
    PerModuleSema *method_sema = owner != nullptr ? owner->findModuleSema(method_module) : nullptr;

    // Build the effective argument list. A method only receives an implicit
    // self argument when it actually declares a receiver parameter (either
    // `self` with no explicit type or `self: Type`). Methods declared with a
    // normal parameter list such as `fn foo(): i32` are static in this
    // compiler: `Point.foo()` passes no receiver and the resolved method
    // signature has no owner pointer.
    const auto &fn_params = method_decl->parameters;
    // A method receives an implicit self argument only when it declares a
    // receiver parameter (`self`, or `self: Type`). A method with no declared
    // parameters is static: `Type.method()` passes zero arguments.
    const bool has_receiver  = !fn_params.empty() && fn_params.front().name == "self";
    bool has_implicit_self   = has_receiver && !fn_params.front().type;
    TypeId self_type         = kInvalidTypeId;
    const auto saved_decl_id = currentDeclId_;
    const auto saved_kind    = currentFunctionKind_;
    currentDeclId_           = method_decl->id.value;
    currentFunctionKind_     = method_decl->kind == frontend::DeclKind::Function
                                   ? method_decl->functionKind
                                   : frontend::FunctionKind::Standard;
    if (has_implicit_self) {
        // First param is named self with no type: fill in *Owner.
        self_type = is_pointer ? base_type : type_table.internPointer(pointee);
    } else if (has_receiver) {
        // Explicit self param: use its declared type.
        self_type = is_pointer ? base_type : methodSelfParamType(fn_params.front());
    }

    // Check explicit arguments against remaining params.
    // A method with no receiver is static: it expects exactly the explicit
    // call arguments, so `Point.foo()` resolves with zero args.
    const size_t provided_args            = call.operands.size() - 1;
    const bool method_is_vslice           = !fn_params.empty() && fn_params.back().isVariadicSlice;
    const TypeId method_fn_type_for_slice = method_sema != nullptr
                                                ? method_sema->typeOfDecl(method_decl->id)
                                                : typeOfDecl(method_decl->id);
    const auto *method_fn_for_slice       = type_table.function(method_fn_type_for_slice);
    const size_t method_slice_param_index =
        method_is_vslice && method_fn_for_slice != nullptr
            ? method_fn_for_slice->params.size() - 1U
            : (method_fn_for_slice != nullptr ? method_fn_for_slice->params.size()
                                              : fn_params.size());
    const size_t method_fixed_explicit = method_is_vslice
                                             ? method_slice_param_index - (has_receiver ? 1U : 0U)
                                             : method_slice_param_index - (has_receiver ? 1U : 0U);
    const bool method_explicit_slice   = [&]() {
        if (!method_is_vslice || provided_args != method_fixed_explicit + 1U ||
            call.operands.empty())
            return false;
        (void)inferExpr(call.operands.back());
        const TypeId method_slice_sema = method_sema != nullptr && method_fn_for_slice != nullptr
                                               ? method_fn_for_slice->params[method_slice_param_index]
                                               : kInvalidTypeId;
        return variadicFinalArgIsExplicitSlice(method_slice_sema, call.operands,
                                                 method_fixed_explicit);
    }();
    const bool method_auto_collect = method_is_vslice && !method_explicit_slice;
    const bool defaults_cover =
        provided_args < method_fixed_explicit &&
        missingArgsHaveDefaults(*method_decl, provided_args, has_receiver ? 1U : 0U,
                                method_is_vslice ? method_slice_param_index
                                                 : ~static_cast<size_t>(0));
    if (defaults_cover) {
        // Missing trailing fixed arguments are supplied from defaults.
    } else if (provided_args < method_fixed_explicit ||
               (!method_is_vslice && provided_args != method_fixed_explicit) ||
               (method_is_vslice && !method_auto_collect && !method_explicit_slice &&
                provided_args != method_fixed_explicit)) {
        report(call.span, "method call arity mismatch", diagnostics::err::NoMatchingFn);
        currentDeclId_       = saved_decl_id;
        currentFunctionKind_ = saved_kind;
        return error_type;
    }
    std::vector<std::pair<frontend::ExprId, types::OwnershipKind>> seen_roots;
    const auto method_fn   = method_sema != nullptr ? method_sema->typeOfDecl(method_decl->id)
                                                    : typeOfDecl(method_decl->id);
    const auto *method_ffn = type_table.function(method_fn);
    for (size_t i = 0; i < method_fixed_explicit; ++i) {
        TypeId arg_type          = inferExpr(call.operands[i + 1]);
        TypeId param_type        = error_type;
        const size_t param_index = has_receiver ? i + 1U : i;
        if (method_ffn != nullptr && param_index < method_ffn->params.size()) {
            param_type = method_ffn->params[param_index];
        } else if (has_receiver && i + 1U < fn_params.size()) {
            param_type = borrowParamType(fn_params[i + 1]);
        } else if (!has_receiver && i < fn_params.size()) {
            param_type = borrowParamType(fn_params[i]);
        }
        if (param_type) {
            (void)checkOwnershipCoercion(call.operands[i + 1], param_type, seen_roots, call.span,
                                         true);
        }
        if (param_type && arg_type && !coerceValue(call.operands[i + 1], param_type, arg_type))
            reportCoercionFailure(call.span, param_type, arg_type,
                                  "method call argument type mismatch",
                                  diagnostics::err::NoMatchingFn);
    }
    if (method_auto_collect && method_is_vslice) {
        const TypeId slice_type = method_ffn != nullptr
                                      ? method_ffn->params[method_slice_param_index]
                                      : typeOfExpr(call.operands.back());
        (void)checkVariadicTailArgs(call.span, call.operands, slice_type,
                                    method_fixed_explicit + 1U, true);
    }

    const TypeId method_type    = method_sema != nullptr ? method_sema->typeOfDecl(method_decl->id)
                                                         : typeOfDecl(method_decl->id);
    const auto *method_fn_final = type_table.function(method_type);
    TypeId result =
        method_fn_final != nullptr
            ? method_fn_final->result
            : (method_sema != nullptr ? kInvalidTypeId : lowerTypeExpr(method_decl->declaredType));
    if (!result)
        result = void_type;
    currentDeclId_       = saved_decl_id;
    currentFunctionKind_ = saved_kind;
    // Record a type for the Field/Arrow callee: it is a resolved method, not a
    // struct field, and the later standalone-expression sweep would otherwise
    // re-infer it as a field access and report "unknown field".
    if (method_type)
        setExprType(callee.id, method_type);
    else
        setExprType(callee.id, result);
    const bool implicit_self = has_receiver && !fn_params.front().type;
    const bool var_self =
        has_receiver && fn_params.front().bindingKind == frontend::BindingKind::Var;
    const auto *receiver_fn         = type_table.function(method_type);
    const bool explicit_borrow_self = has_receiver && receiver_fn != nullptr &&
                                      !receiver_fn->params.empty() &&
                                      isBorrowParamType(receiver_fn->params[0]);
    // A `*char` implementation owns the pointer value itself and passes it by
    // value, so calling a method on the receiver does not invalidate the local
    // pointer for subsequent `.method()` / `->method()` calls.
    const bool pointer_owner = is_pointer && type_table.kindOf(pointee) == TypeKind::Pointer;
    if ((implicit_self || var_self) && !pointer_owner && !explicit_borrow_self)
        invalidateReceiverRoot(callee.operands[0]);
    if ((has_receiver && method_decl->parameters.front().type) && !implicit_self &&
        !explicit_borrow_self && !is_pointer) {
        // A `self: ?char` implementation method owns the optional aggregate and
        // is called with the payload pointer. Keep the callee base expression
        // typed as the aggregate so later `o.get()` calls see the same exact
        // `?T` owner instead of the inner char.
        const TypeId receiver_owner =
            type_table.kindOf(pointee) == TypeKind::Optional &&
                    type_table.kindOf(type_table.stripQualifiers(base_type)) == TypeKind::Optional
                ? base_type
                : self_type;
        setExprType(callee.operands[0], receiver_owner);
    }
    return result;
}
void PerModuleSema::invalidateReceiverRoot(frontend::ExprId base) {
    if (!base || base.value > snapshot.expressions().size())
        return;
    const auto *resolved = findResolvedExpr(base);
    if (resolved == nullptr || !resolved->local || isBorrowParameter(base))
        return;
    movedLocals_.insert(resolved->local.value);
}

} // namespace zith::sema::modern
