#include "sema/hir-lower-modern.hpp"

#include "diagnostics/error-codes.hpp"
#include "sema/hir-lower-utils.hpp"
#include "types/type-kind.hpp"

namespace zith::sema {
namespace modern {

bool HirLowerModern::lowerStatement(frontend::StmtId id, hir::HirExprId &last_value) {
    if (!id || current_module_ == nullptr ||
        id.value > current_module_->frontend->statements().size())
        return true;

    const auto &statement = current_module_->frontend->statements()[id.value - 1U];
    switch (statement.kind) {
    case frontend::StmtKind::Expression:
        if (statement.expression &&
            statement.expression.value <= current_module_->frontend->expressions().size()) {
            last_value = lowerExpr(statement.expression);
            if (last_value == hir::kInvalidHirExpr &&
                typeOfExpr(statement.expression) != types::kVoidType &&
                typeOfExpr(statement.expression) != types::kErrorType && !diags_.hasErrors()) {
                diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                              "expression statement could not be lowered", memory::Span{});
                return false;
            }
            if (last_value != hir::kInvalidHirExpr) {
                if (defer_body_sink_ != nullptr)
                    defer_body_sink_->push(last_value);
                else
                    current_fn_->blocks[current_block_].insts.push(last_value);
            }
        }
        return true;
    case frontend::StmtKind::Declaration:
        // Local states have already been predeclared and are not executed.
        return true;
    case frontend::StmtKind::Binding: {
        // A for-in element binding is materialized by the loop lowering: it
        // only stores the per-iteration `value()` result, so allocating the
        // same slot again here would clobber that store in codegen.
        if (statement.id == current_for_in_binding_stmt_ ||
            (current_for_in_binding_local_ &&
             statement.binding.id == current_for_in_binding_local_)) {
            last_value = hir::kInvalidHirExpr;
            return true;
        }
        const auto slot   = localSlot(statement.binding.id);
        const auto type   = typeOfLocal(statement.binding.id);
        const auto alloca = emitSlotAlloca(slot, type);
        if (defer_body_sink_ != nullptr)
            defer_body_sink_->push(alloca);
        else
            current_fn_->blocks[current_block_].insts.push(alloca);
        if (statement.binding.initializer) {
            auto init = lowerExpr(statement.binding.initializer);
            // Sema now accepts `T -> ?T`, `?T -> ??T`, and deeper optional
            // coercions. The HIR target is canonical by construction only when
            // the coerced source has the same flattened payload layout, so we
            // wrap according to the sema source type instead of forcing a
            // `?T` value into a `??T` slot.
            if (init != hir::kInvalidHirExpr && types_.kindOf(type) == types::TypeKind::Optional) {
                const auto binding_sema_type = semaTypeOfLocal(statement.binding.id);
                const auto init_sema_type    = semaTypeOfExpr(statement.binding.initializer);
                if (binding_sema_type != sema::modern::kInvalidTypeId &&
                    init_sema_type != sema::modern::kInvalidTypeId)
                    init =
                        lowerCoerceToOptionalDepth(type, binding_sema_type, init_sema_type, init);
            }
            init = lowerCoerceToTarget(type, statement.binding.initializer, init);
            const sema::modern::TypeId binding_sema = semaTypeOfLocal(statement.binding.id);
            if (binding_sema != sema::modern::kInvalidTypeId &&
                sema_.typeTable().kindOf(binding_sema) == sema::modern::TypeKind::Dyn)
                init = lowerCoerceToDyn(binding_sema, statement.binding.initializer, init,
                                        binding_sema);
            else if (binding_sema != sema::modern::kInvalidTypeId)
                init = lowerCoerceToOpaque(binding_sema, statement.binding.initializer, init);
            if (init != hir::kInvalidHirExpr) {
                const auto store = emitSlotStore(slot, init);
                if (defer_body_sink_ != nullptr)
                    defer_body_sink_->push(store);
                else
                    current_fn_->blocks[current_block_].insts.push(store);
            }
        }
        last_value = hir::kInvalidHirExpr;
        return true;
    }
    case frontend::StmtKind::Defer:
        // In ordinary lexical blocks, defers are flushed at end-of-block after
        // all direct bindings have emitted their slots. Inside a deferred body
        // (`defer { defer x(); }`) they lower immediately into that body in
        // source order.
        if (defer_body_sink_ != nullptr)
            return lowerDeferBody(id);
        if (pending_defers_.empty())
            pending_defers_.emplace_back();
        pending_defers_.back().push_back(id);
        last_value = hir::kInvalidHirExpr;
        return true;
    case frontend::StmtKind::Return: {
        emitCleanupFrom(0);
        hir::HirRet ret;
        if (statement.expression) {
            auto value = lowerExpr(statement.expression);
            // Coerce T → ?T when the current block's return statement carries an
            // expression whose sema type is not optional.  The implicit-return
            // path below performs the same coercion when the function returns
            // the block value.
            if (value != hir::kInvalidHirExpr &&
                types_.kindOf(current_fn_->return_type) == types::TypeKind::Optional) {
                const auto val_sema_type = semaTypeOfExpr(statement.expression);
                if (current_fn_return_sema_type_ != sema::modern::kInvalidTypeId &&
                    val_sema_type != sema::modern::kInvalidTypeId)
                    value = lowerCoerceToOptionalDepth(current_fn_->return_type,
                                                       current_fn_return_sema_type_, val_sema_type,
                                                       value);
                else if (sema_.typeTable().kindOf(val_sema_type) != TypeKind::Optional)
                    value = lowerCoerceToOptional(current_fn_->return_type, value);
            }
            value = lowerCoerceToTarget(current_fn_->return_type, statement.expression, value);
            if (current_fn_return_sema_type_ != sema::modern::kInvalidTypeId)
                value = lowerCoerceToDyn(current_fn_return_sema_type_, statement.expression, value,
                                         current_fn_return_sema_type_);
            if (current_fn_return_sema_type_ != sema::modern::kInvalidTypeId)
                value =
                    lowerCoerceToOpaque(current_fn_return_sema_type_, statement.expression, value);
            ret.value = value;
        }
        if (!statement.expression && current_main_void_) {
            // The bare `return;` in a void main returns success from the C
            // entry point, whose HIR signature is i32 for the linker.
            hir::HirLiteral zero;
            zero.type = types_.internInt(types::IntWidth::I32);
            zero.i    = 0;
            ret.value = addExpr(std::move(zero));
        }
        setTerminator(addExpr(std::move(ret)));
        return true;
    }
    case frontend::StmtKind::Break:
        if (loop_stack_.empty()) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                          "break used outside of a loop", {});
            return false;
        }
        {
            const LoopTarget *target = &loop_stack_.back();
            if (!statement.label.empty()) {
                target = nullptr;
                for (auto it = loop_stack_.rbegin(); it != loop_stack_.rend(); ++it) {
                    if (it->label == statement.label) {
                        target = &*it;
                        break;
                    }
                }
                if (target == nullptr) {
                    diags_.report(
                        diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                        "break label '" + statement.label + "' does not name an active loop", {});
                    return false;
                }
            }
            emitCleanupFrom(target->cleanup_depth);
            emitJump(target->break_block);
        }
        setCurrentBlock(newBlock());
        current_fn_->blocks[current_block_].insts = memory::DynArray<hir::HirExprId>(arena_);
        return true;
    case frontend::StmtKind::Continue:
        if (loop_stack_.empty()) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                          "continue used outside of a loop", {});
            return false;
        }
        {
            const LoopTarget *target = &loop_stack_.back();
            if (!statement.label.empty()) {
                target = nullptr;
                for (auto it = loop_stack_.rbegin(); it != loop_stack_.rend(); ++it) {
                    if (it->label == statement.label) {
                        target = &*it;
                        break;
                    }
                }
                if (target == nullptr) {
                    diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                                  "continue label '" + statement.label +
                                      "' does not name an active loop",
                                  {});
                    return false;
                }
            }
            emitCleanupFrom(target->cleanup_depth);
            emitJump(target->continue_block);
        }
        setCurrentBlock(newBlock());
        current_fn_->blocks[current_block_].insts = memory::DynArray<hir::HirExprId>(arena_);
        return true;
    case frontend::StmtKind::Jump: {
        if (!current_fn_is_state_) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                          "jump is only allowed inside a state function", {});
            return false;
        }
        const frontend::Declaration *target = nullptr;
        if (current_module_ != nullptr && current_module_->frontend != nullptr) {
            for (const auto &decl : current_module_->frontend->declarations()) {
                if (decl.kind != frontend::DeclKind::Function || decl.name != statement.label ||
                    decl.functionKind != frontend::FunctionKind::State ||
                    decl.parentScope != info_decl_parent_scope_) {
                    continue;
                }
                target = &decl;
                break;
            }
        }
        if (target == nullptr) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                          "jump target must be a state function: '" + statement.label + "'", {});
            return false;
        }
        emitCleanupFrom(0);
        const auto target_key      = internFunctionKey(interner_, current_module_->key, target->id);
        const auto *function_index = function_index_by_key_.get(target_key);
        if (function_index == nullptr) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                          "state target was not predeclared: '" + statement.label + "'", {});
            return false;
        }

        const auto *module_sema = sema_.findModuleSema(current_module_->key);
        if (module_sema == nullptr ||
            module_sema->stateMachineIdOf(*target) != current_state_machine_id_) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                          "state transition target is in a different state machine", {});
            return false;
        }

        hir::HirStateTailCall tail(arena_);
        tail.call.callee          = hir::kInvalidHirExpr;
        const auto target_fn_type = module_sema->typeOfDecl(target->id);
        const auto *target_fn     = sema_.typeTable().function(target_fn_type);
        if (target_fn == nullptr) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                          "state target has no function type: '" + statement.label + "'", {});
            return false;
        }
        const bool target_is_slice =
            target_fn->params.size() > 0 && target->parameters.back().isVariadicSlice;
        const size_t slice_index =
            target_is_slice ? target_fn->params.size() - 1U : target_fn->params.size();
        const bool explicit_slice_arg =
            target_is_slice && statement.arguments.size() == slice_index + 1U &&
            !statement.arguments.empty() &&
            (types_.kindOf(typeOfExpr(statement.arguments.back())) == types::TypeKind::Slice ||
             types_.kindOf(typeOfExpr(statement.arguments.back())) == types::TypeKind::Array);
        const bool auto_collect_tail = target_is_slice && !explicit_slice_arg;

        // Sema already rejected arity/type mismatches before HIR lowering.
        // Lower only the arguments that exist; a variadic slice tail is packed
        // below, and an explicit slice argument is kept as-is.
        size_t lowered_fixed = 0;
        for (; lowered_fixed < std::min(statement.arguments.size(), slice_index); ++lowered_fixed) {
            const auto &arg_expr =
                current_module_->frontend
                    ->expressions()[statement.arguments[lowered_fixed].value - 1U];
            const bool annotated =
                arg_expr.kind == frontend::ExprKind::OwnershipCoerce && !arg_expr.operands.empty();
            const frontend::ExprId inner_id =
                annotated ? arg_expr.operands[0] : statement.arguments[lowered_fixed];
            auto argument = lowerExpr(inner_id);
            if (annotated) {
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
            if (argument == hir::kInvalidHirExpr)
                return false;
            argument = lowerCoerceToTarget(lowerType(target_fn->params[lowered_fixed]),
                                           statement.arguments[lowered_fixed], argument);
            argument = lowerCoerceToOpaque(target_fn->params[lowered_fixed],
                                           statement.arguments[lowered_fixed], argument);
            tail.call.argument_types.push(lowerType(target_fn->params[lowered_fixed]));
            tail.call.args.push(argument);
        }
        if (target != nullptr && current_module_ != nullptr) {
            const comptime::InstantiationInstance *target_instance = nullptr;
            if (const auto *instantiations = sema_.instantiations(); instantiations != nullptr) {
                const auto *func_index2 = function_index_by_key_.get(target_key);
                if (func_index2 != nullptr) {
                    const auto &function = functions_[*func_index2];
                    if (function.instance != nullptr && function.instance->decl == target->id) {
                        for (size_t index = 0; index < instantiations->instanceCount(); ++index) {
                            const auto *instance = instantiations->at(index);
                            if (instance->decl == target->id &&
                                instance->args == function.instance->args) {
                                target_instance = instance;
                                break;
                            }
                        }
                    }
                }
            }
            for (size_t index = lowered_fixed;
                 index < target->parameters.size() && index < slice_index; ++index) {
                if (!target->parameters[index].defaultValue)
                    continue;
                auto value = lowerDefaultWithTarget(*current_module_, target_instance,
                                                    target->parameters[index].defaultValue,
                                                    target_fn->params[index]);
                if (value == hir::kInvalidHirExpr)
                    return false;
                tail.call.args.push(value);
                tail.call.argument_types.push(lowerType(target_fn->params[index]));
            }
        }
        if (auto_collect_tail && statement.arguments.size() > slice_index) {
            std::vector<frontend::ExprId> tail_exprs(statement.arguments.begin() +
                                                         static_cast<ptrdiff_t>(slice_index),
                                                     statement.arguments.end());
            auto slice = lowerVariadicSliceTail(target_fn->params[slice_index], tail_exprs);
            if (slice == hir::kInvalidHirExpr)
                return false;
            tail.call.argument_types.push(lowerType(target_fn->params[slice_index]));
            tail.call.args.push(slice);
        } else if (explicit_slice_arg ||
                   (target_is_slice && statement.arguments.size() == slice_index)) {
            // The slice parameter is vacuous (`f()`) or passed as a single
            // explicit slice value.
            const auto slice_type = lowerType(target_fn->params[slice_index]);
            if (!explicit_slice_arg) {
                auto slice = lowerVariadicSliceTail(target_fn->params[slice_index], {});
                if (slice == hir::kInvalidHirExpr)
                    return false;
                tail.call.argument_types.push(lowerType(target_fn->params[slice_index]));
                tail.call.args.push(slice);
            } else {
                auto argument = lowerExpr(statement.arguments.back());
                if (argument == hir::kInvalidHirExpr)
                    return false;
                argument = lowerCoerceToTarget(slice_type, statement.arguments.back(), argument);
                argument = lowerCoerceToOpaque(target_fn->params[slice_index],
                                               statement.arguments.back(), argument);
                tail.call.argument_types.push(lowerType(target_fn->params[slice_index]));
                tail.call.args.push(argument);
            }
        }
        tail.call.resolved_fn = functions_[*function_index].sym_id;
        tail.call.musttail    = true;
        tail.call.usesTailCC  = true;
        setTerminator(addExpr(std::move(tail)));
        last_value = hir::kInvalidHirExpr;
        return true;
    }
    case frontend::StmtKind::Error:
        return false;
    }
    return true;
}

} // namespace modern
} // namespace zith::sema
