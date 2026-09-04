#include "sema/hir-lower-modern.hpp"

#include "common/overloaded.hpp"
#include "sema/hir-lower-utils.hpp"
#include "types/type-kind.hpp"

namespace zith::sema {
namespace modern {

hir::HirExprId HirLowerModern::lowerCall(const frontend::Expression &expr) {
    if (expr.operands.empty())
        return hir::kInvalidHirExpr;

    const frontend::ExprId callee_id = expr.operands[0];
    const frontend::Expression &callee_expr =
        current_module_->frontend->expressions()[callee_id.value - 1U];

    const bool is_console_format = [&]() {
        const auto *target = overloadTarget(callee_id);
        if (target == nullptr)
            return false;
        const auto *target_module = snapshot_.findModule(target->module);
        if (target_module == nullptr)
            return false;
        const auto *decl = findDecl(*target_module, target->decl);
        return decl != nullptr && decl->kind == frontend::DeclKind::Function &&
               (decl->name == "print" || decl->name == "println") &&
               moduleNamespace(target_module->key, snapshot_.cacheKey()) == "std.io.console";
    }();

    // A Field/Arrow callee is only a method call when a matching method
    // declaration actually exists on the receiver's struct type. Module
    // aliases and callable fields also parse as Field/Arrow, so resolve
    // first and fall back to a plain call when nothing matches.
    const frontend::Declaration *method_decl      = nullptr;
    const session::ModuleArtifact *owner_artifact = nullptr;
    if ((callee_expr.kind == frontend::ExprKind::Field ||
         callee_expr.kind == frontend::ExprKind::Arrow) &&
        !callee_expr.operands.empty()) {
        const sema::modern::TypeId base_type =
            sema_.typeTable().stripQualifiers(semaTypeOfExpr(callee_expr.operands[0]));
        sema::modern::TypeId pointee = base_type;
        if (base_type && sema_.typeTable().kindOf(base_type) == TypeKind::Pointer) {
            if (const auto *ptr = sema_.typeTable().pointer(base_type))
                pointee = sema_.typeTable().stripQualifiers(ptr->pointee);
        } else if (base_type && sema_.typeTable().kindOf(base_type) == TypeKind::Optional) {
            if (const auto *opt = sema_.typeTable().optional(base_type))
                pointee = sema_.typeTable().stripQualifiers(opt->inner);
        }
        const auto *st = sema_.typeTable().struct_type(pointee);
        if (st != nullptr) {
            std::string owner_name(st->name);
            if (const size_t angle = owner_name.find('<'); angle != std::string::npos)
                owner_name.resize(angle);
            // Search the calling module first (same behavior as sema), then
            // fall back to the declaring module when the owner was imported.
            const auto findIn = [&](const session::ModuleArtifact &module,
                                    const frontend::Declaration **out) {
                for (const auto &decl : module.frontend->declarations()) {
                    if (decl.kind != frontend::DeclKind::Function)
                        continue;
                    if (decl.ownerName != owner_name || decl.name != callee_expr.text)
                        continue;
                    *out = &decl;
                    return true;
                }
                return false;
            };
            const frontend::Declaration *candidate = nullptr;
            if (findIn(*current_module_, &candidate)) {
                method_decl    = candidate;
                owner_artifact = current_module_;
            } else {
                for (const auto &module_ptr : snapshot_.modules()) {
                    const auto &module = *module_ptr;
                    if (module.key == current_module_->key || module.frontend == nullptr)
                        continue;
                    if (findIn(module, &candidate)) {
                        method_decl    = candidate;
                        owner_artifact = &module;
                        break;
                    }
                }
            }
        }
    }

    // Sema records the exact overload target across module boundaries. Prefer
    // it so overload selection and generic instantiation use the same decl
    // (and module) that type-checked the call. This is also the only path that
    // accepts trait methods, which sema has already filtered by conformance.
    bool static_generic_bound_call = false;
    if (const auto *decl = methodDeclFromTarget(callee_id, &owner_artifact); decl != nullptr) {
        method_decl                        = decl;
        const std::string bound_trait_name = decl->traitName;
        const bool is_trait_requirement =
            !decl->body && !decl->traitName.empty() && decl->ownerName == decl->traitName;
        // Sema records the trait requirement for `T.parse(...)`. During
        // lowering of an instantiated body the base is no longer a generic
        // parameter, so derive the concrete owner from the generic argument.
        std::string concrete_owner;
        if (callee_expr.kind == frontend::ExprKind::Field &&
            !callee_expr.operands.empty()) {
            const auto &base =
                current_module_->frontend->expressions()[callee_expr.operands[0].value - 1U];
            if (base.kind == frontend::ExprKind::Name && current_instance_ != nullptr &&
                current_fn_decl_ != nullptr) {
                for (size_t index = 0; index < current_fn_decl_->genericParams.size(); ++index) {
                    if (current_fn_decl_->genericParams[index].name == base.text &&
                        index < current_instance_->args.size()) {
                        concrete_owner = sema_.typeTable().typeToString(
                            sema_.typeTable().stripQualifiers(current_instance_->args[index]));
                        break;
                    }
                }
            }
        }
        if (concrete_owner.empty())
            concrete_owner = decl->traitName;
        const bool nominal_trait = [&]() {
            if (owner_artifact == nullptr)
                return false;
            const auto *module_sema = sema_.findModuleSema(owner_artifact->key);
            const TypeId trait_type = sema_.typeTable().lookupNamed(decl->traitName);
            return module_sema != nullptr && trait_type &&
                   !module_sema->isInterfaceType(trait_type);
        }();
        // A nominal trait requirement reached through `T.parse(...)` lowers to
        // the concrete `implement Owner as Trait` declaration. Interface bounds
        // keep their existing concrete-struct rewrite path below.
        const bool generic_bound_call =
            is_trait_requirement && callee_expr.kind == frontend::ExprKind::Field &&
            nominal_trait && current_instance_ != nullptr && concrete_owner != decl->traitName;
        if (generic_bound_call) {
            // `T.parse(arg)` inside a generic body lowers only after
            // monomorphization. Sema resolved the callee to the trait
            // requirement, but the executable target is the concrete
            // `implement Owner as Trait` method for the instantiated `T`.
            const std::string owner_name = concrete_owner;
            const auto findConcrete = [&](const session::ModuleArtifact &module,
                                          const frontend::Declaration **out) {
                for (const auto &candidate : module.frontend->declarations()) {
                    if (candidate.kind != frontend::DeclKind::Function ||
                        candidate.ownerName != owner_name || candidate.name != callee_expr.text ||
                        !candidate.body)
                        continue;
                    if (!bound_trait_name.empty() && candidate.traitName != bound_trait_name)
                        continue;
                    const bool candidate_has_receiver = !candidate.parameters.empty() &&
                                                        candidate.parameters.front().name == "self";
                    const size_t declared_fixed = candidate.parameters.size();
                    const size_t min_fixed      = candidate.parameters.empty() ? 0U : [&]() {
                        size_t count = candidate_has_receiver ? candidate.parameters.size() - 1U
                                                                   : candidate.parameters.size();
                        for (size_t index = candidate.parameters.size(); index > 0U; --index) {
                            const auto &parameter = candidate.parameters[index - 1U];
                            if (!parameter.defaultValue)
                                break;
                            if (candidate_has_receiver && index == 1U)
                                break;
                            --count;
                        }
                        return count;
                    }();
                    const size_t provided_args  = expr.operands.size() - 1U;
                    if (provided_args < min_fixed || provided_args > declared_fixed)
                        continue;
                    *out = &candidate;
                    return true;
                }
                return false;
            };
            const frontend::Declaration *concrete = nullptr;
            if (findConcrete(*current_module_, &concrete)) {
                method_decl    = concrete;
                owner_artifact = current_module_;
            } else {
                for (const auto &module_ptr : snapshot_.modules()) {
                    const auto &module = *module_ptr;
                    if (module.key == current_module_->key || module.frontend == nullptr)
                        continue;
                    if (findConcrete(module, &concrete)) {
                        method_decl    = concrete;
                        owner_artifact = &module;
                        break;
                    }
                }
            }
            static_generic_bound_call = concrete != nullptr;
            if (!static_generic_bound_call)
                return hir::kInvalidHirExpr;
        } else if (!method_decl->body && callee_expr.kind == frontend::ExprKind::Field &&
                   !callee_expr.operands.empty()) {
            // Interface requirements are declaration-only. After monomorphization
            // of the generic body, resolve the call to the concrete struct method
            // that satisfies the structural interface bound.
            const sema::modern::TypeId base_type =
                sema_.typeTable().stripQualifiers(semaTypeOfExpr(callee_expr.operands[0]));
            sema::modern::TypeId pointee = base_type;
            if (base_type && sema_.typeTable().kindOf(base_type) == TypeKind::Pointer) {
                if (const auto *ptr = sema_.typeTable().pointer(base_type))
                    pointee = sema_.typeTable().stripQualifiers(ptr->pointee);
            } else if (base_type && sema_.typeTable().kindOf(base_type) == TypeKind::Optional) {
                if (const auto *opt = sema_.typeTable().optional(base_type))
                    pointee = sema_.typeTable().stripQualifiers(opt->inner);
            }
            const auto *st = sema_.typeTable().struct_type(pointee);
            if (st != nullptr) {
                std::string owner_name(st->name);
                if (const size_t angle = owner_name.find('<'); angle != std::string::npos)
                    owner_name.resize(angle);
                const auto findConcrete = [&](const session::ModuleArtifact &module,
                                              const frontend::Declaration **out) {
                    for (const auto &candidate : module.frontend->declarations()) {
                        if (candidate.kind != frontend::DeclKind::Function ||
                            candidate.ownerName != owner_name || candidate.name != callee_expr.text)
                            continue;
                        *out = &candidate;
                        return true;
                    }
                    return false;
                };
                const frontend::Declaration *concrete = nullptr;
                if (findConcrete(*current_module_, &concrete)) {
                    method_decl    = concrete;
                    owner_artifact = current_module_;
                } else {
                    for (const auto &module_ptr : snapshot_.modules()) {
                        const auto &module = *module_ptr;
                        if (module.key == current_module_->key || module.frontend == nullptr)
                            continue;
                        if (findConcrete(module, &concrete)) {
                            method_decl    = concrete;
                            owner_artifact = &module;
                            break;
                        }
                    }
                }
            }
        }
    }

    // `p.method()` on a `dyn`/fat-pointer receiver dispatches through the
    // vtable embedded in the receiver. Sema resolved the call to the trait
    // requirement, which also tells us the slot index in trait declaration
    // order.
    if (method_decl != nullptr &&
        (callee_expr.kind == frontend::ExprKind::Field ||
         callee_expr.kind == frontend::ExprKind::Arrow) &&
        !callee_expr.operands.empty()) {
        const sema::modern::TypeId dyn_base =
            sema_.typeTable().stripQualifiers(semaTypeOfExpr(callee_expr.operands[0]));
        if (sema_.typeTable().kindOf(dyn_base) == sema::modern::TypeKind::Dyn) {
            const auto *dyn_ty                  = sema_.typeTable().dyn_type(dyn_base);
            const sema::modern::TypeId trait_ty = sema_.typeTable().canonical(
                dyn_ty != nullptr ? dyn_ty->target : sema::modern::kInvalidTypeId);
            const auto *trait = sema_.typeTable().trait(trait_ty);
            if (trait == nullptr)
                return hir::kInvalidHirExpr;

            uint32_t slot_index = ~0U;
            uint32_t cursor     = 0;
            const auto findSlot = [&](const session::ModuleArtifact &module) {
                for (const auto &candidate : module.frontend->declarations()) {
                    if (candidate.kind != frontend::DeclKind::Function ||
                        candidate.ownerName != trait->name || candidate.name.empty() ||
                        candidate.name == "self")
                        continue;
                    if (candidate.name == callee_expr.text)
                        slot_index = cursor;
                    ++cursor;
                }
            };
            findSlot(*current_module_);
            for (const auto &module_ptr : snapshot_.modules()) {
                if (module_ptr->key == current_module_->key || module_ptr->frontend == nullptr)
                    continue;
                findSlot(*module_ptr);
            }
            if (slot_index == ~0U)
                return hir::kInvalidHirExpr;

            const auto *method_sema = owner_artifact != nullptr
                                          ? sema_.findModuleSema(owner_artifact->key)
                                          : sema_.findModuleSema(current_module_->key);
            const auto *fn =
                method_sema != nullptr
                    ? sema_.typeTable().function(method_sema->typeOfDecl(method_decl->id))
                    : nullptr;
            if (fn == nullptr)
                return hir::kInvalidHirExpr;

            const bool has_receiver =
                !method_decl->parameters.empty() && method_decl->parameters.front().name == "self";
            memory::DynArray<types::TypeId> lowered_params(arena_);
            for (size_t pi = has_receiver ? 1U : 0U; pi < fn->params.size(); ++pi)
                lowered_params.push(lowerType(fn->params[pi]));
            const types::TypeId lowered_fn = types_.internFn(lowered_params, lowerType(fn->result));

            hir::HirDynCall dyncall(arena_);
            dyncall.receiver     = lowerExpr(callee_expr.operands[0]);
            dyncall.vtable_name  = interner_.intern(trait->name);
            dyncall.slot_index   = slot_index;
            dyncall.has_receiver = has_receiver;
            dyncall.result_type  = lowerType(fn->result);
            dyncall.fn_type      = lowered_fn;

            const size_t dyn_slice_param =
                !method_decl->parameters.empty() && method_decl->parameters.back().isVariadicSlice
                    ? fn->params.size() - 1U
                    : fn->params.size();
            const bool dyn_explicit_slice =
                dyn_slice_param < fn->params.size() &&
                expr.operands.size() == dyn_slice_param + 1U && !expr.operands.empty() &&
                (types_.kindOf(typeOfExpr(expr.operands.back())) == types::TypeKind::Slice ||
                 types_.kindOf(typeOfExpr(expr.operands.back())) == types::TypeKind::Array);
            const bool dyn_collect_tail =
                dyn_slice_param < fn->params.size() && !dyn_explicit_slice;
            bool dyn_tail_lowered = false;
            for (size_t index = 1; index < expr.operands.size(); ++index) {
                const size_t call_index = has_receiver ? index : index - 1U;
                if (dyn_collect_tail && call_index >= dyn_slice_param) {
                    std::vector<frontend::ExprId> tail;
                    tail.reserve(expr.operands.size() - index);
                    for (size_t tail_index = index; tail_index < expr.operands.size(); ++tail_index)
                        tail.push_back(expr.operands[tail_index]);
                    auto slice = lowerVariadicSliceTail(fn->params[dyn_slice_param], tail);
                    if (slice == hir::kInvalidHirExpr)
                        return hir::kInvalidHirExpr;
                    dyncall.args.push(slice);
                    dyncall.arg_types.push(lowerType(fn->params[dyn_slice_param]));
                    dyn_tail_lowered = true;
                    break;
                }
                if (call_index >= fn->params.size())
                    continue;
                auto argument = lowerExpr(expr.operands[index]);
                if (argument == hir::kInvalidHirExpr)
                    return hir::kInvalidHirExpr;
                argument = lowerCoerceToTarget(lowerType(fn->params[call_index]),
                                               expr.operands[index], argument);
                const sema::modern::TypeId param_sema =
                    sema_.typeTable().kindOf(fn->params[call_index]) == sema::modern::TypeKind::Dyn
                        ? fn->params[call_index]
                        : sema_.typeTable().canonical(fn->params[call_index]);
                if (sema_.typeTable().kindOf(param_sema) == sema::modern::TypeKind::Dyn) {
                    dyncall.args.push(argument);
                    dyncall.arg_types.push(lowerType(fn->params[call_index]));
                    argument = lowerCoerceToDyn(fn->params[call_index], expr.operands[index],
                                                argument, fn->params[call_index]);
                    dyncall.args.back() = argument;
                } else {
                    dyncall.args.push(argument);
                    dyncall.arg_types.push(lowerType(fn->params[call_index]));
                    argument =
                        lowerCoerceToOpaque(fn->params[call_index], expr.operands[index], argument);
                    if (types_.kindOf(lowerType(fn->params[call_index])) ==
                            types::TypeKind::Optional &&
                        types_.kindOf(lowerType(param_sema)) != types::TypeKind::Optional)
                        argument =
                            lowerCoerceToOptional(lowerType(fn->params[call_index]), argument);
                    dyncall.args.back() = argument;
                }
            }
            if (method_decl != nullptr) {
                const session::ModuleArtifact *dyn_decl_module =
                    owner_artifact != nullptr ? owner_artifact : current_module_;
                const comptime::InstantiationInstance *dyn_decl_instance = nullptr;
                if (const auto *instantiations = sema_.instantiations();
                    instantiations != nullptr) {
                    if (const auto *binding =
                            instantiations->callBinding(current_module_->key, callee_id))
                        dyn_decl_instance = instantiations->at(binding->instance);
                }
                const size_t dyn_explicit_fixed =
                    has_receiver ? dyncall.args.size() - 1U : dyncall.args.size();
                for (size_t index = (has_receiver ? 1U : 0U) + dyn_explicit_fixed;
                     index < method_decl->parameters.size() && index < dyn_slice_param; ++index) {
                    if (!method_decl->parameters[index].defaultValue)
                        continue;
                    auto value = lowerDefaultWithTarget(*dyn_decl_module, dyn_decl_instance,
                                                        method_decl->parameters[index].defaultValue,
                                                        fn->params[index]);
                    if (value == hir::kInvalidHirExpr)
                        return hir::kInvalidHirExpr;
                    value = lowerCoerceToOpaque(fn->params[index], {}, value);
                    dyncall.args.push(lowerCoerceToDyn(fn->params[index],
                                                       method_decl->parameters[index].defaultValue,
                                                       value, fn->params[index]));
                    dyncall.arg_types.push(lowerType(fn->params[index]));
                }
            }
            if (dyn_collect_tail && !dyn_tail_lowered) {
                auto slice = lowerVariadicSliceTail(fn->params[dyn_slice_param], {});
                if (slice == hir::kInvalidHirExpr)
                    return hir::kInvalidHirExpr;
                slice = lowerCoerceToOpaque(fn->params[dyn_slice_param], {}, slice);
                dyncall.args.push(slice);
                dyncall.arg_types.push(lowerType(fn->params[dyn_slice_param]));
            }
            return addExpr(std::move(dyncall));
        }
    }

    // A method with a `self` receiver receives an implicit owner pointer
    // argument. Methods without self are static in this compiler. A bound
    // trait call through a generic parameter supplies its receiver as an
    // ordinary explicit argument, so it must not gain another implicit self.
    const bool is_receiver_method = !static_generic_bound_call && method_decl != nullptr &&
                                    !method_decl->parameters.empty() &&
                                    method_decl->parameters.front().name == "self";
    const bool is_method_call = is_receiver_method;

    hir::HirExprId callee = hir::kInvalidHirExpr;
    if (!is_method_call) {
        if (method_decl == nullptr) {
            callee = lowerExpr(callee_id);
            if (callee == hir::kInvalidHirExpr)
                return hir::kInvalidHirExpr;
        } // Static methods keep callee = kInvalid and resolve by symbol below.
    }

    memory::DynArray<hir::HirExprId> args(arena_);
    memory::DynArray<types::TypeId> arg_types(arena_);

    if (is_receiver_method) {
        frontend::ExprId base_id = callee_expr.operands[0];
        if (current_types_ != nullptr) {
            if (const auto *base = current_types_->traitQualifiedReceiverBase.get(base_id.value))
                base_id = frontend::ExprId{*base};
        }
        // `->` passes a pointer value; `.` passes the address of a value
        // receiver. Pointer-valued bases passed through `.method()` also pass
        // the pointer itself, matching sema's method-call argument type.
        hir::HirExprId self_arg = hir::kInvalidHirExpr;
        const auto base_sema_ty = semaTypeOfExpr(base_id);
        const auto base_hir_ty  = typeOfExpr(base_id);
        const sema::modern::TypeId base_sema_resolved =
            sema_.typeTable().stripQualifiers(base_sema_ty);
        // Sema retypes the receiver expression to `*Owner` when the method
        // declares an explicit pointer `self`, but the source value may still
        // be a plain owner. Keep the decision based on the actual lvalue type.
        const auto *base_resolved       = findResolvedExpr(base_id);
        const types::TypeId base_storage_ty =
            base_resolved != nullptr && base_resolved->local
                ? typeOfLocal(base_resolved->local)
                : base_hir_ty;
        const auto *base_optional = sema_.typeTable().optional(base_sema_resolved);
        const bool base_optional_aggregate =
            base_optional != nullptr &&
            sema_.typeTable().kindOf(sema_.typeTable().stripQualifiers(base_optional->inner)) !=
                TypeKind::Pointer;
        const bool optional_has_exact_owner =
            base_optional != nullptr && method_decl != nullptr &&
            sema_.typeTable().typeToString(base_sema_resolved) == method_decl->ownerName;
        const bool base_is_ptr = base_storage_ty != types::kErrorType &&
                                 base_storage_ty != types::kInvalidType &&
                                 (types_.kindOf(base_storage_ty) == types::TypeKind::Ptr ||
                                  types_.kindOf(base_storage_ty) == types::TypeKind::Optional);
        if (base_optional_aggregate && optional_has_exact_owner)
            self_arg = lowerLValueAddr(base_id);
        else if (base_optional_aggregate) {
            // The receiver points at the stored payload, never at the
            // aggregate `{payload, tag}` wrapper.
            const auto slot = next_slot_++;
            current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(slot, base_hir_ty));
            const auto base_value = lowerExpr(base_id);
            current_fn_->blocks[current_block_].insts.push(emitSlotStore(slot, base_value));
            const auto payload_type =
                lowerType(sema_.typeTable().stripQualifiers(base_optional->inner));
            const auto payload_field = addExpr(hir::HirField{
                addExpr(hir::HirSlotAddr{slot, base_hir_ty}), 0U, payload_type, base_hir_ty});
            self_arg                 = addExpr(
                hir::HirUnary{hir::HirUnaryOp::Ref, payload_field, types_.internPtr(payload_type)});
        } else if (callee_expr.kind == frontend::ExprKind::Arrow || base_is_ptr)
            self_arg = lowerExpr(base_id);
        else
            self_arg = lowerLValueAddr(base_id);
        if (self_arg == hir::kInvalidHirExpr) {
            const auto base_hir_type = typeOfExpr(base_id);
            const auto value         = lowerExpr(base_id);
            const auto self_slot     = next_slot_++;
            current_fn_->blocks[current_block_].insts.push(
                emitSlotAlloca(self_slot, base_hir_type));
            current_fn_->blocks[current_block_].insts.push(emitSlotStore(self_slot, value));
            self_arg = addExpr(hir::HirSlotAddr{self_slot, base_hir_type});
        }
        args.push(self_arg);
        const sema::modern::TypeId callee_sema_type0 = semaTypeOfExpr(callee_id);
        const auto *callee_fn0 = callee_sema_type0 != sema::modern::kInvalidTypeId
                                     ? sema_.typeTable().function(callee_sema_type0)
                                     : nullptr;
        const auto self_type   = callee_fn0 != nullptr && !callee_fn0->params.empty()
                                     ? lowerType(callee_fn0->params[0])
                                     : (base_sema_ty ? lowerType(base_sema_ty) : types::kInvalidType);
        if (self_type != types::kInvalidType)
            arg_types.push(self_type);
    }

    // Resolve the callee signature once so variadic-slice auto-collection can
    // be computed from the same declaration used by sema/overload selection.
    sema::modern::TypeId callee_sema_type = semaTypeOfExpr(callee_id);
    if (static_generic_bound_call && owner_artifact != nullptr) {
        const auto *concrete_sema = sema_.findModuleSema(owner_artifact->key);
        if (concrete_sema != nullptr)
            callee_sema_type = concrete_sema->typeOfDecl(method_decl->id);
    }
    if (callee_sema_type == sema::modern::kInvalidTypeId) {
        if (const auto *target = overloadTarget(callee_id)) {
            const auto *target_sema = sema_.findModuleSema(target->module);
            if (target_sema != nullptr)
                callee_sema_type = target_sema->typeOfDecl(target->decl);
        } else if (const auto *resolved = findResolvedExpr(callee_id)) {
            const session::ModuleArtifact *decl_module = nullptr;
            const auto *decl = resolvedFunctionDecl(*resolved, &decl_module);
            if (decl != nullptr && decl_module != nullptr) {
                if (const auto *decl_sema = sema_.findModuleSema(decl_module->key))
                    callee_sema_type = decl_sema->typeOfDecl(decl->id);
            }
        }
    }
    const auto *callee_fn = callee_sema_type != sema::modern::kInvalidTypeId
                                ? sema_.typeTable().function(callee_sema_type)
                                : nullptr;
    size_t slice_param    = ~static_cast<size_t>(0);
    if (callee_fn != nullptr && method_decl != nullptr && !method_decl->parameters.empty() &&
        method_decl->parameters.back().isVariadicSlice) {
        slice_param = callee_fn->params.size() - 1U;
    } else if (const auto *resolved = findResolvedExpr(callee_id);
               resolved != nullptr && resolved->isVariadicSlice && callee_fn != nullptr &&
               !callee_fn->params.empty()) {
        slice_param = callee_fn->params.size() - 1U;
    } else if (const auto *target = overloadTarget(callee_id);
               target != nullptr && callee_fn != nullptr && !callee_fn->params.empty()) {
        const auto *target_artifact = snapshot_.findModule(target->module);
        if (target_artifact != nullptr && target_artifact->frontend != nullptr &&
            target->decl.value <= target_artifact->frontend->declarations().size()) {
            const auto &target_decl =
                target_artifact->frontend->declarations()[target->decl.value - 1U];
            if (!target_decl.parameters.empty() && target_decl.parameters.back().isVariadicSlice)
                slice_param = callee_fn->params.size() - 1U;
        }
    }

    // A single trailing slice is an explicit `[]T` argument, not one element
    // to auto-collect. The frontend marks it with the same type as the slice
    // parameter, so the check mirrors sema's `explicit_slice_arg` decision.
    const bool explicit_slice_arg =
        slice_param != ~static_cast<size_t>(0) && expr.operands.size() > 1 && [&]() {
            const size_t last_index = expr.operands.size() - 1U;
            const size_t last_call  = is_receiver_method ? last_index : last_index - 1U;
            if (last_call != slice_param)
                return false;
            const auto last_type = typeOfExpr(expr.operands.back());
            const bool is_concrete_last_slice =
                types_.kindOf(last_type) == types::TypeKind::Slice ||
                types_.kindOf(last_type) == types::TypeKind::Array;
            const sema::modern::TypeId slice_sema = callee_fn->params[slice_param];
            const auto *slice                     = sema_.typeTable().slice(
                sema_.typeTable().stripQualifiers(sema_.typeTable().canonical(slice_sema)));
            const bool slice_is_dyn =
                slice != nullptr &&
                sema_.typeTable().kindOf(slice->element) == sema::modern::TypeKind::Dyn;
            const auto *last_slice = std::get_if<types::TypeSlice>(&types_.lookup(last_type));
            const bool last_is_already_dyn =
                last_slice != nullptr && types_.kindOf(last_slice->elem) == types::TypeKind::Dyn;
            return is_concrete_last_slice && (!slice_is_dyn || last_is_already_dyn);
        }();
    const bool collect_tail = slice_param != ~static_cast<size_t>(0) && !explicit_slice_arg;
    bool tail_lowered       = false;

    for (size_t index = 1; index < expr.operands.size(); ++index) {
        const size_t call_index          = is_receiver_method ? index : index - 1U;
        const bool is_first_tail_element = collect_tail && call_index >= slice_param;
        if (is_first_tail_element) {
            std::vector<frontend::ExprId> tail;
            tail.reserve(expr.operands.size() - index);
            for (size_t tail_index = index; tail_index < expr.operands.size(); ++tail_index)
                tail.push_back(expr.operands[tail_index]);
            const sema::modern::TypeId slice_sema = callee_fn->params[slice_param];
            auto slice                            = lowerVariadicSliceTail(slice_sema, tail);
            if (slice == hir::kInvalidHirExpr)
                return hir::kInvalidHirExpr;
            // `lowerVariadicSliceTail` already erases each tail element to the
            // slice element type (`dyn Trait` or `opaque`). Do not erase the
            // whole `[]dyn` slice again as if it were a single value.
            slice = lowerCoerceToOpaque(slice_sema, expr.operands[index], slice);
            args.push(slice);
            arg_types.push(lowerType(slice_sema));
            tail_lowered = true;
            break;
        }
        const auto &arg_expr =
            current_module_->frontend->expressions()[expr.operands[index].value - 1U];
        const bool annotated =
            arg_expr.kind == frontend::ExprKind::OwnershipCoerce && !arg_expr.operands.empty();
        const frontend::ExprId inner_id = annotated ? arg_expr.operands[0] : expr.operands[index];
        auto argument                   = lowerExpr(inner_id);
        if (is_console_format && index == 1)
            argument = lowerFormatMessage(inner_id, argument);
        const bool inner_is_borrow_pointer =
            sema_.isBorrowParameter(current_module_->key, inner_id) ||
            types_.kindOf(typeOfExpr(inner_id)) == types::TypeKind::Ptr;
        if (annotated) {
            if (inner_is_borrow_pointer) {
                // A borrow parameter already carries the ABI pointer; `lend
                // self`/`lend p` reborrows the pointee and must pass that
                // pointer value, not the address of the local slot.
                argument = lowerExpr(inner_id);
            } else {
                auto address = lowerLValueAddr(inner_id);
                if (address == hir::kInvalidHirExpr) {
                    const auto inner_type = typeOfExpr(inner_id);
                    const auto slot       = next_slot_++;
                    current_fn_->blocks[current_block_].insts.push(
                        emitSlotAlloca(slot, inner_type));
                    current_fn_->blocks[current_block_].insts.push(emitSlotStore(slot, argument));
                    address = addExpr(hir::HirSlotAddr{slot, inner_type});
                }
                argument = address;
            }
        }
        if (argument == hir::kInvalidHirExpr)
            return hir::kInvalidHirExpr;
        const auto argument_type = typeOfExpr(expr.operands[index]);
        if (argument_type != types::kInvalidType)
            arg_types.push(argument_type);
        const auto param_sema_raw = callee_fn != nullptr && call_index < callee_fn->params.size()
                                        ? callee_fn->params[call_index]
                                        : sema::modern::kInvalidTypeId;
        const auto param_type     = param_sema_raw != sema::modern::kInvalidTypeId
                                        ? lowerType(param_sema_raw)
                                        : types::kInvalidType;
        hir::HirExprId lowered_argument =
            lowerCoerceToTarget(param_type, expr.operands[index], argument);
        if (param_sema_raw != sema::modern::kInvalidTypeId) {
            const auto param_sema = param_sema_raw;
            if (sema_.typeTable().kindOf(param_sema) == sema::modern::TypeKind::Dyn ||
                sema_.typeTable().kindOf(sema_.typeTable().canonical(param_sema)) ==
                    sema::modern::TypeKind::Dyn)
                lowered_argument = lowerCoerceToDyn(param_sema, expr.operands[index],
                                                    lowered_argument, param_sema);
            else
                lowered_argument =
                    lowerCoerceToOpaque(param_sema, expr.operands[index], lowered_argument);
            const auto arg_sema = semaTypeOfExpr(expr.operands[index]);
            if (types_.kindOf(param_type) == types::TypeKind::Optional)
                lowered_argument =
                    lowerCoerceToOptionalDepth(param_type, param_sema, arg_sema, lowered_argument);
        }
        args.push(lowered_argument);
    }
    // Default arguments are validated by sema but not present in the caller's
    // expression nodes. Materialize missing fixed parameters as ordinary
    // arguments once the explicit (or explicit-slice) tail is known.
    const session::ModuleArtifact *decl_module       = owner_artifact;
    const comptime::InstantiationInstance *decl_inst = nullptr;
    const frontend::Declaration *param_decl          = method_decl;
    const size_t receiver_offset                     = is_receiver_method ? 1U : 0U;
    if (param_decl == nullptr) {
        if (const auto *target = overloadTarget(callee_id); target != nullptr) {
            decl_module = snapshot_.findModule(target->module);
            param_decl  = decl_module != nullptr ? findDecl(*decl_module, target->decl) : nullptr;
        } else if (const auto *resolved = findResolvedExpr(callee_id)) {
            param_decl = resolvedFunctionDecl(*resolved, &decl_module);
        }
    }
    if (decl_module == nullptr)
        decl_module = current_module_;
    if (const auto *instantiations = sema_.instantiations(); instantiations != nullptr) {
        if (const auto *binding = instantiations->callBinding(current_module_->key, callee_id))
            decl_inst = instantiations->at(binding->instance);
    }
    if (param_decl != nullptr && decl_module != nullptr) {
        const size_t explicit_fixed = args.size() - receiver_offset;
        const size_t max_fixed =
            slice_param != ~static_cast<size_t>(0) ? slice_param : callee_fn->params.size();
        for (size_t index = receiver_offset + explicit_fixed;
             index < param_decl->parameters.size() && index < max_fixed; ++index) {
            if (!param_decl->parameters[index].defaultValue)
                continue;
            auto value = lowerDefaultWithTarget(*decl_module, decl_inst,
                                                param_decl->parameters[index].defaultValue,
                                                callee_fn->params[index]);
            if (value == hir::kInvalidHirExpr)
                return hir::kInvalidHirExpr;
            args.push(value);
            arg_types.push(lowerType(callee_fn->params[index]));
        }
    }
    if (collect_tail && !tail_lowered) {
        // A variadic-slice callee is still callable with an empty tail. Lower
        // `[0, 0]` so the call keeps the slice parameter in its signature.
        const sema::modern::TypeId slice_sema = callee_fn->params[slice_param];
        auto slice                            = lowerVariadicSliceTail(slice_sema, {});
        if (slice == hir::kInvalidHirExpr)
            return hir::kInvalidHirExpr;
        args.push(slice);
        arg_types.push(lowerType(slice_sema));
    }

    hir::HirCall call{callee, std::move(args), std::move(arg_types)};
    call.resolved_fn = symbols::kInvalidSym;
    if (callee != hir::kInvalidHirExpr) {
        const auto callee_fn_type = typeOfExpr(callee_id);
        if (callee_fn_type != types::kInvalidType &&
            types_.kindOf(callee_fn_type) == types::TypeKind::Fn)
            call.fn_type = callee_fn_type;
    }
    if (method_decl != nullptr) {
        const auto *method_module_artifact =
            owner_artifact != nullptr ? owner_artifact : current_module_;
        const auto key = internFunctionKey(interner_, method_module_artifact->key, method_decl->id);
        if (const auto *instantiations = sema_.instantiations(); instantiations != nullptr) {
            if (const auto *binding =
                    instantiations->callBinding(current_module_->key, callee_id)) {
                const auto *instance = instantiations->at(binding->instance);
                for (const auto &function : functions_) {
                    if (function.instance != nullptr && function.module != nullptr &&
                        function.module->key == binding->module &&
                        function.instance->decl == method_decl->id && instance != nullptr &&
                        function.instance->args == instance->args) {
                        call.resolved_fn = function.sym_id;
                        break;
                    }
                }
            }
        }
        if (call.resolved_fn == symbols::kInvalidSym) {
            if (const auto *function_index = function_index_by_key_.get(key))
                call.resolved_fn = functions_[*function_index].sym_id;
        }
    } else {
        const auto *target = overloadTarget(callee_id);
        // Sema already picked one declaration out of an overload set; re-resolving
        // here would silently fall back to the first candidate.
        if (target != nullptr) {
            const auto key = internFunctionKey(interner_, target->module, target->decl);
            if (auto *instantiations = sema_.instantiations(); instantiations != nullptr) {
                if (const auto *binding =
                        instantiations->callBinding(current_module_->key, callee_id)) {
                    const auto *instance = instantiations->at(binding->instance);
                    for (const auto &function : functions_) {
                        if (function.instance != nullptr && function.module != nullptr &&
                            function.module->key == binding->module &&
                            function.instance->decl == target->decl && instance != nullptr &&
                            function.instance->args == instance->args) {
                            call.resolved_fn = function.sym_id;
                            break;
                        }
                    }
                }
            }
            if (call.resolved_fn == symbols::kInvalidSym) {
                if (const auto *function_index = function_index_by_key_.get(key))
                    call.resolved_fn = functions_[*function_index].sym_id;
            }
        } else if (const auto *resolved = findResolvedExpr(callee_id)) {
            if (auto *instantiations = sema_.instantiations(); instantiations != nullptr) {
                if (const auto *binding =
                        instantiations->callBinding(current_module_->key, callee_id)) {
                    const session::ModuleArtifact *resolved_module = nullptr;
                    const auto *decl     = resolvedFunctionDecl(*resolved, &resolved_module);
                    const auto *instance = instantiations->at(binding->instance);
                    if (decl != nullptr && resolved_module != nullptr && instance != nullptr) {
                        for (const auto &function : functions_) {
                            if (function.instance != nullptr && function.module != nullptr &&
                                function.module->key == resolved_module->key &&
                                function.instance->decl == decl->id &&
                                function.instance->args == instance->args) {
                                call.resolved_fn = function.sym_id;
                                break;
                            }
                        }
                    }
                }
            }
            if (call.resolved_fn == symbols::kInvalidSym)
                call.resolved_fn = resolvedFunctionSym(*resolved);
        }
    }
    const hir::HirExprId call_id = addExpr(std::move(call));
    if (call_id != hir::kInvalidHirExpr) {
        const session::ModuleArtifact *resolved_module = nullptr;
        const auto *resolved                           = findResolvedExpr(callee_id);
        const auto *target_decl =
            resolved != nullptr ? resolvedFunctionDecl(*resolved, &resolved_module) : nullptr;
        const auto sema_state_callee =
            semaTypeOfExpr(callee_id) != sema::modern::kInvalidTypeId &&
            sema_.typeTable().kindOf(semaTypeOfExpr(callee_id)) == sema::modern::TypeKind::State;
        if ((target_decl != nullptr && target_decl->kind == frontend::DeclKind::Function &&
             target_decl->functionKind == frontend::FunctionKind::State) ||
            sema_state_callee) {
            auto &hir_call      = std::get<hir::HirCall>(hir_.getExprMut(call_id));
            hir_call.usesTailCC = true;
            if (current_fn_ != nullptr)
                current_fn_->usesTailCC = true;
        }
    }
    if (nra_ != nullptr) {
        const auto *call_fact = nra_->callFact(expr.id);
        if (call_fact != nullptr && call_fact->hasResidual()) {
            auto &attrs = hir_.attrs().call(call_id);
            if (call_fact->returnsArgument != ~0U)
                attrs.returnsArg = call_fact->returnsArgument;
            for (const auto escape : call_fact->argEscapes)
                attrs.args.emplace(hir::HirCallArgAttr{mapHirEscape(escape)});
            if (call_fact->duplicatedShareOrView) {
                while (attrs.args.size() < call_fact->argEscapes.size())
                    attrs.args.emplace(hir::HirCallArgAttr{hir::HirCallEscape::None});
            }
        }
    }
    return call_id;
}

} // namespace modern
} // namespace zith::sema
