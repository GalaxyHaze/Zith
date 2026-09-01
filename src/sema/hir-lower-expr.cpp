#include "sema/hir-lower-modern.hpp"

#include "common/overloaded.hpp"
#include "diagnostics/error-codes.hpp"
#include "sema/hir-lower-utils.hpp"
#include "sema/op-mapping.hpp"
#include "support/int-literal.hpp"
#include "types/type-kind.hpp"

namespace zith::sema {
namespace modern {

hir::HirExprId HirLowerModern::lowerVisibleDefault(const session::ModuleArtifact &module,
                                                   const comptime::InstantiationInstance *instance,
                                                   frontend::ExprId default_id) {
    if (!default_id || module.frontend == nullptr ||
        default_id.value > module.frontend->expressions().size())
        return hir::kInvalidHirExpr;
    const session::ModuleResolution *decl_resolution = snapshot_.findResolution(module.key);
    const TypedMap *decl_types                       = sema_.findTypedMap(module.key);
    if (decl_resolution == nullptr || decl_types == nullptr)
        return hir::kInvalidHirExpr;

    const session::ModuleArtifact *saved_module           = current_module_;
    const session::ModuleResolution *saved_resolution     = current_resolution_;
    const TypedMap *saved_types                           = current_types_;
    const comptime::GenericInstantiationPass *saved_inst  = current_instantiation_;
    const comptime::InstantiationInstance *saved_instance = current_instance_;
    current_module_                                       = &module;
    current_resolution_                                   = decl_resolution;
    current_types_                                        = decl_types;
    current_instantiation_ = instance != nullptr ? sema_.instantiations() : nullptr;
    current_instance_      = instance;
    const auto value       = lowerExpr(default_id);
    current_module_        = saved_module;
    current_resolution_    = saved_resolution;
    current_types_         = saved_types;
    current_instantiation_ = saved_inst;
    current_instance_      = saved_instance;
    return value;
}

hir::HirExprId HirLowerModern::lowerDefaultWithTarget(
    const session::ModuleArtifact &module, const comptime::InstantiationInstance *instance,
    frontend::ExprId default_id, sema::modern::TypeId target_sema) {
    const session::ModuleResolution *decl_resolution = snapshot_.findResolution(module.key);
    const TypedMap *decl_types                       = sema_.findTypedMap(module.key);
    if (!default_id || module.frontend == nullptr ||
        default_id.value > module.frontend->expressions().size() || decl_resolution == nullptr ||
        decl_types == nullptr)
        return hir::kInvalidHirExpr;

    const session::ModuleArtifact *saved_module           = current_module_;
    const session::ModuleResolution *saved_resolution     = current_resolution_;
    const TypedMap *saved_types                           = current_types_;
    const comptime::GenericInstantiationPass *saved_inst  = current_instantiation_;
    const comptime::InstantiationInstance *saved_instance = current_instance_;
    current_module_                                       = &module;
    current_resolution_                                   = decl_resolution;
    current_types_                                        = decl_types;
    current_instantiation_ = instance != nullptr ? sema_.instantiations() : nullptr;
    current_instance_      = instance;
    const auto value       = current_types_ != nullptr
                                 ? lowerCoerceToOpaque(target_sema, default_id, lowerExpr(default_id))
                                 : hir::kInvalidHirExpr;
    current_module_        = saved_module;
    current_resolution_    = saved_resolution;
    current_types_         = saved_types;
    current_instantiation_ = saved_inst;
    current_instance_      = saved_instance;
    return value;
}

hir::HirExprId HirLowerModern::lowerExpr(frontend::ExprId id) {
    if (!id || current_module_ == nullptr ||
        id.value > current_module_->frontend->expressions().size())
        return hir::kInvalidHirExpr;

    const auto &expr = current_module_->frontend->expressions()[id.value - 1U];
    const auto type  = typeOfExpr(id);
    switch (expr.kind) {
    case frontend::ExprKind::OwnershipCoerce:
        return expr.operands.empty() ? hir::kInvalidHirExpr : lowerExpr(expr.operands[0]);
    case frontend::ExprKind::Literal: {
        // Implicit `T -> opaque` coercions record the concrete source type but
        // sema also types the whole value expression as `opaque`. The HIR
        // literal must keep the concrete type so it can be spilled into the
        // erased payload before wrapping.
        types::TypeId literal_type = type;
        if (current_types_ != nullptr) {
            if (const auto *source = current_types_->opaqueSourceTypes.get(id.value)) {
                const sema::modern::TypeId source_sema =
                    current_instantiation_ != nullptr && current_instance_ != nullptr
                        ? current_instantiation_->substituteType(*source, current_instance_->args)
                        : *source;
                const types::TypeId lowered = lowerType(source_sema);
                if (lowered != types::kErrorType && lowered != types::kInvalidType)
                    literal_type = lowered;
            }
        }
        return lowerLiteral(expr, literal_type);
    }
    case frontend::ExprKind::Name:
        return lowerName(expr);
    case frontend::ExprKind::Unary:
        return lowerUnary(expr, type);
    case frontend::ExprKind::Binary:
        return lowerBinary(expr, type);
    case frontend::ExprKind::Call:
        return lowerCall(expr);
    case frontend::ExprKind::DockCall:
        // `dock` is a plain call expression whose result carries the machine
        // return type; sema already resolved the target state declaration.
        return lowerCall(expr);
    case frontend::ExprKind::Block:
        return lowerBlock(expr);
    case frontend::ExprKind::If:
        return lowerIf(expr, type);
    case frontend::ExprKind::When:
        return lowerWhen(expr, type);
    case frontend::ExprKind::Range:
        return hir::kInvalidHirExpr;
    case frontend::ExprKind::While:
        return lowerWhile(expr);
    case frontend::ExprKind::For:
        return lowerFor(expr);
    case frontend::ExprKind::ForIn:
        return lowerForIn(expr);
    case frontend::ExprKind::Assign:
        return lowerAssign(expr, type);
    case frontend::ExprKind::OptionalProp:
        return lowerOptionalProp(expr, type);
    case frontend::ExprKind::Index:
        return lowerIndex(expr, type);
    case frontend::ExprKind::SliceRange:
        return lowerSliceRange(expr, type);
    case frontend::ExprKind::Field:
        return lowerField(expr, type);
    case frontend::ExprKind::Arrow:
        return lowerArrow(expr, type);
    case frontend::ExprKind::StructLiteral:
        return lowerStructLiteral(expr, type);
    case frontend::ExprKind::PackLiteral:
        return lowerPackLiteral(expr, type);
    case frontend::ExprKind::ArrayLiteral:
        return lowerArrayLiteral(expr, type);
    case frontend::ExprKind::Cast:
        return lowerCast(expr, type);
    case frontend::ExprKind::IsNull:
        return lowerIsNull(expr);
    case frontend::ExprKind::IsType:
        return lowerIsType(expr);
    case frontend::ExprKind::LayoutIntrinsic:
        return lowerLayoutIntrinsic(expr);
    case frontend::ExprKind::MacroCall: {
        // Transparent: delegate to expansion.
        if (expr.expansion)
            return lowerExpr(expr.expansion);
        return hir::kInvalidHirExpr;
    }
    case frontend::ExprKind::Return:
    case frontend::ExprKind::Placeholder:
    case frontend::ExprKind::Error:
        return hir::kInvalidHirExpr;
    }
    return hir::kInvalidHirExpr;
}

hir::HirExprId HirLowerModern::lowerLiteral(const frontend::Expression &expr,
                                            const types::TypeId type) {
    // null literal maps to HirMakeNone when the target type is optional
    if (expr.text == "null" && types_.kindOf(type) == types::TypeKind::Optional)
        return lowerMakeNone(type);
    hir::HirLiteral literal{};
    literal.type = type;
    switch (types_.kindOf(type)) {
    case types::TypeKind::Bool:
        literal.b = expr.text == "true";
        break;
    case types::TypeKind::Float:
        literal.f = std::strtod(expr.text.c_str(), nullptr);
        if (const auto *float_t = std::get_if<types::TypeFloat>(&types_.lookup(type));
            float_t != nullptr) {
            switch (float_t->width) {
            case types::FloatWidth::F32:
                literal.f = static_cast<double>(static_cast<float>(literal.f));
                break;
            default:
                break;
            }
        }
        break;
    case types::TypeKind::Char: {
        if (expr.text.size() < 3U || expr.text.front() != '\'' || expr.text.back() != '\'')
            return hir::kInvalidHirExpr;
        std::string decoded;
        const std::string_view body(expr.text.data() + 1U, expr.text.size() - 2U);
        if (!decodeEscapes(body, decoded) || decoded.size() != 1U) {
            if (current_module_ != nullptr) {
                diags_.report(
                    diagnostics::Severity::Error, diagnostics::err::InvalidEscape,
                    "invalid escape sequence in char literal",
                    memory::Span{current_module_->fileId, expr.span.start, expr.span.end});
            }
            return hir::kInvalidHirExpr;
        }
        literal.i = static_cast<unsigned char>(decoded.front());
        break;
    }
    case types::TypeKind::Ptr: {
        auto text = std::string_view(expr.text);
        if (text.size() >= 2U && text.front() == '"' && text.back() == '"') {
            std::string decoded;
            if (!decodeEscapes(std::string_view(text.data() + 1U, text.size() - 2U), decoded)) {
                if (current_module_ != nullptr) {
                    diags_.report(
                        diagnostics::Severity::Error, diagnostics::err::InvalidEscape,
                        "invalid escape sequence in string literal",
                        memory::Span{current_module_->fileId, expr.span.start, expr.span.end});
                }
                return hir::kInvalidHirExpr;
            }
            literal.str_val = interner_.intern(decoded);
        }
        break;
    }
    default:
        // Honour the radix prefix the lexer accepted (`0x`, `0c`, `0b`). Sema has already
        // rejected literals that do not fit 64 bits, so Overflow here leaves the value at 0.
        (void)support::parseIntegerLiteral(expr.text, literal.i);
        break;
    }
    return addExpr(std::move(literal));
}

hir::HirExprId HirLowerModern::lowerName(const frontend::Expression &expr) {
    // Prefer the per-expression resolution: it is keyed by span, so it cannot
    // pick up a same-named binding from another function.
    if (const auto *resolved = findResolvedExpr(expr.id)) {
        if (resolved->local) {
            const auto slot      = localSlot(resolved->local);
            const auto local_ty  = typeOfLocal(resolved->local);
            const auto expr_type = typeOfExpr(expr.id);
            for (auto it = narrowing_stack_.rbegin(); it != narrowing_stack_.rend(); ++it) {
                if (it->local == resolved->local) {
                    if (it->optionalPayload) {
                        // The slot still stores `?T`; extract field 0 so reads
                        // after `is null` use the payload type.
                        return addExpr(hir::HirField{addExpr(hir::HirSlotAddr{slot, local_ty}), 0U,
                                                     it->type, local_ty});
                    }
                    if (it->opaquePayload) {
                        hir::HirOpaqueCast cast;
                        cast.value       = emitSlotLoad(slot, local_ty);
                        cast.from        = local_ty;
                        cast.to          = it->type;
                        cast.opaque_type = local_ty;
                        cast.result_type = it->type;
                        cast.type_id     = stableConcreteTypeId(it->type);
                        cast.checked     = false;
                        return addExpr(std::move(cast));
                    }
                    const auto narrowed = emitSlotLoad(slot, it->type);
                    if (types_.kindOf(local_ty) == types::TypeKind::Union &&
                        types_.kindOf(it->type) != types::TypeKind::Union) {
                        hir::HirUnionCast cast;
                        cast.value        = narrowed;
                        cast.from         = local_ty;
                        cast.to           = it->type;
                        cast.member_index = taggedMemberIndex(local_ty, it->type);
                        cast.checked      = false;
                        return addExpr(std::move(cast));
                    }
                    return narrowed;
                }
            }
            const auto *local_union = types_.kindOf(local_ty) == types::TypeKind::Union
                                          ? std::get_if<types::TypeUnion>(&types_.lookup(local_ty))
                                          : nullptr;
            const auto *local_union_def =
                local_union != nullptr ? types_.lookupUnionDef(local_union->def_id) : nullptr;
            if (local_union_def != nullptr && local_union_def->is_tagged && expr_type != local_ty) {
                hir::HirUnionCast cast;
                cast.value        = emitSlotLoad(slot, local_ty);
                cast.from         = local_ty;
                cast.to           = expr_type;
                cast.member_index = taggedMemberIndex(local_ty, expr_type);
                cast.checked      = false;
                return addExpr(std::move(cast));
            }
            return emitSlotLoad(slot, local_ty);
        }
        if (resolved->declKind == frontend::DeclKind::Variable &&
            resolved->bindingKind == frontend::BindingKind::Const) {
            const session::ModuleArtifact *decl_module = current_module_;
            frontend::DeclId decl_id                   = resolved->declaration;
            if (!resolved->target.module.empty()) {
                decl_module = snapshot_.findModule(resolved->target.module);
                if (!decl_id && resolved->target.localSymbol)
                    decl_id = frontend::DeclId{resolved->target.localSymbol.value};
            }
            if (decl_module != nullptr && decl_id &&
                decl_id.value <= decl_module->frontend->declarations().size()) {
                const auto key = internFunctionKey(interner_, decl_module->key, decl_id);
                if (const auto *global_name = global_const_by_key_.get(key)) {
                    hir::HirGlobalConstLoad load;
                    load.name = *global_name;
                    load.type = typeOfExpr(expr.id);
                    return addExpr(std::move(load));
                }
            }
        }
        if (resolved->foreignConstant != nullptr) {
            const auto key = interner_.intern(resolved->foreignConstant->name);
            if (const auto *global_name = global_const_by_name_.get(key)) {
                hir::HirGlobalConstLoad load;
                load.name = *global_name;
                load.type = typeOfExpr(expr.id);
                return addExpr(std::move(load));
            }
        }
        if (resolved->foreignFunction != nullptr) {
            hir::HirVar var;
            var.name    = interner_.intern(resolved->foreignFunction->linkageName);
            var.version = 0;
            return addExpr(std::move(var));
        }
        const session::ModuleArtifact *decl_module = nullptr;
        if (const auto *decl = resolvedFunctionDecl(*resolved, &decl_module)) {
            hir::HirVar var;
            // Use the predeclared (qualified) linkage name so the reference and the
            // definition agree.
            var.name = interner_.intern(decl->name);
            if (decl_module != nullptr) {
                const auto key = internFunctionKey(interner_, decl_module->key, decl->id);
                if (const auto *function_index = function_index_by_key_.get(key))
                    var.name = hir_.getFn(*function_index).name;
            }
            var.version = 0;
            return addExpr(std::move(var));
        }
    }

    if (current_resolution_ != nullptr && current_module_ != nullptr &&
        current_module_->frontend != nullptr) {
        if (const auto *binding = session::lookupBinding(
                *current_resolution_, expr.text, expr.scope, current_module_->frontend->scopes())) {
            if (binding->local) {
                const auto slot = localSlot(binding->local);
                return emitSlotLoad(slot, typeOfLocal(binding->local));
            }
        }
    }

    hir::HirVar var;
    var.name    = interner_.intern(expr.text);
    var.version = 0;
    if (const auto *resolved = findResolvedExpr(expr.id);
        resolved != nullptr && resolved->foreignFunction != nullptr)
        var.name = interner_.intern(resolved->foreignFunction->linkageName);
    return addExpr(std::move(var));
}

hir::HirExprId HirLowerModern::lowerLValueAddr(frontend::ExprId id) {
    if (!id || current_module_ == nullptr || current_module_->frontend == nullptr ||
        id.value > current_module_->frontend->expressions().size())
        return hir::kInvalidHirExpr;
    const auto &expr = current_module_->frontend->expressions()[id.value - 1U];
    if (expr.kind == frontend::ExprKind::Name) {
        if (const auto *resolved = findResolvedExpr(id); resolved != nullptr && resolved->local) {
            const auto slot     = localSlot(resolved->local);
            const auto local_ty = typeOfLocal(resolved->local);
            for (auto it = narrowing_stack_.rbegin(); it != narrowing_stack_.rend(); ++it) {
                if (it->local == resolved->local && it->optionalPayload) {
                    const auto payload_field = addExpr(hir::HirField{
                        addExpr(hir::HirSlotAddr{slot, local_ty}), 0U, it->type, local_ty});
                    return addExpr(hir::HirUnary{hir::HirUnaryOp::Ref, payload_field,
                                                 types_.internPtr(it->type)});
                }
                if (it->local == resolved->local && it->opaquePayload) {
                    // A narrowed opaque value is still stored as `{ *void,
                    // typeId }`. Spill the unchecked payload extraction into a
                    // temporary so its address has the narrowed concrete type.
                    const auto temp_slot = next_slot_++;
                    current_fn_->blocks[current_block_].insts.push(
                        emitSlotAlloca(temp_slot, it->type));
                    hir::HirOpaqueCast cast;
                    cast.value       = emitSlotLoad(slot, local_ty);
                    cast.from        = local_ty;
                    cast.to          = it->type;
                    cast.opaque_type = local_ty;
                    cast.result_type = it->type;
                    cast.type_id     = stableConcreteTypeId(it->type);
                    cast.checked     = false;
                    current_fn_->blocks[current_block_].insts.push(
                        emitSlotStore(temp_slot, addExpr(std::move(cast))));
                    return addExpr(hir::HirSlotAddr{temp_slot, it->type});
                }
            }
            return addExpr(hir::HirSlotAddr{slot, local_ty});
        }
        return hir::kInvalidHirExpr;
    }
    if (expr.kind == frontend::ExprKind::Field || expr.kind == frontend::ExprKind::Index) {
        if (current_types_ != nullptr && expr.kind == frontend::ExprKind::Field) {
            if (const auto *base = current_types_->traitQualifiedReceiverBase.get(expr.id.value))
                return lowerLValueAddr(frontend::ExprId{*base});
        }
        const auto value = lowerExpr(id);
        if (value == hir::kInvalidHirExpr)
            return hir::kInvalidHirExpr;
        hir::HirUnary unary;
        unary.op      = hir::HirUnaryOp::Ref;
        unary.operand = value;
        unary.type    = types_.internPtr(typeOfExpr(id));
        return addExpr(std::move(unary));
    }
    if (expr.kind == frontend::ExprKind::Unary && expr.text == "*" && !expr.operands.empty())
        return lowerExpr(expr.operands[0]);
    return hir::kInvalidHirExpr;
}

hir::HirExprId HirLowerModern::lowerUnary(const frontend::Expression &expr,
                                          const types::TypeId type) {
    if (expr.operands.empty())
        return hir::kInvalidHirExpr;
    if (expr.text == "must")
        return lowerMust(expr, type);
    if (expr.text == "raw") {
        const auto operand_type = typeOfExpr(expr.operands[0]);
        if (types_.kindOf(operand_type) == types::TypeKind::Optional)
            return lowerRawOptional(expr, type);
        return lowerExpr(expr.operands[0]);
    }
    const auto operand = lowerExpr(expr.operands[0]);
    if (operand == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;

    hir::HirUnary unary;
    unary.op      = sema::mapUnaryOp(expr.text);
    unary.operand = operand;
    unary.type    = type;
    return addExpr(std::move(unary));
}

hir::HirExprId HirLowerModern::lowerBinary(const frontend::Expression &expr,
                                           const types::TypeId type) {
    if (expr.operands.size() != 2U)
        return hir::kInvalidHirExpr;
    const auto lhs = lowerExpr(expr.operands[0]);
    const auto rhs = lowerExpr(expr.operands[1]);
    if (lhs == hir::kInvalidHirExpr || rhs == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;

    hir::HirBinary binary;
    binary.lhs          = lhs;
    binary.rhs          = rhs;
    binary.op           = sema::mapBinaryOp(expr.text);
    binary.type         = type;
    binary.operand_type = typeOfExpr(expr.operands[0]);
    return addExpr(std::move(binary));
}

hir::HirExprId HirLowerModern::lowerAssign(const frontend::Expression &expr,
                                           const types::TypeId type) {
    (void)type;
    if (expr.operands.size() != 2U)
        return hir::kInvalidHirExpr;

    const auto &lhs_expr = current_module_->frontend->expressions()[expr.operands[0].value - 1U];
    if (lhs_expr.kind == frontend::ExprKind::Name) {
        if (const auto *resolved = findResolvedExpr(expr.operands[0]);
            resolved != nullptr && resolved->local) {
            const auto value = lowerExpr(expr.operands[1]);
            if (value == hir::kInvalidHirExpr)
                return hir::kInvalidHirExpr;
            const auto value_slice =
                lowerCoerceToTarget(typeOfLocal(resolved->local), expr.operands[1], value);
            return emitSlotStore(localSlot(resolved->local), value_slice);
        }
    }
    // For field/arrow lvalue targets, lower the lhs normally (produces HirField) then assign.
    if (lhs_expr.kind == frontend::ExprKind::Field || lhs_expr.kind == frontend::ExprKind::Arrow ||
        lhs_expr.kind == frontend::ExprKind::Index) {
        const auto target_type = typeOfExpr(expr.operands[0]);
        const auto target =
            lhs_expr.kind == frontend::ExprKind::Field   ? lowerField(lhs_expr, target_type)
            : lhs_expr.kind == frontend::ExprKind::Arrow ? lowerArrow(lhs_expr, target_type)
                                                         : lowerIndex(lhs_expr, target_type);
        const auto value = lowerExpr(expr.operands[1]);
        if (target == hir::kInvalidHirExpr || value == hir::kInvalidHirExpr)
            return hir::kInvalidHirExpr;
        const auto value_slice = lowerCoerceToTarget(target_type, expr.operands[1], value);
        hir::HirAssign assign;
        assign.target = target;
        assign.value  = value_slice;
        return addExpr(std::move(assign));
    }

    const auto target = lowerExpr(expr.operands[0]);
    const auto value  = lowerExpr(expr.operands[1]);
    if (target == hir::kInvalidHirExpr || value == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    const auto value_slice =
        lowerCoerceToTarget(typeOfExpr(expr.operands[0]), expr.operands[1], value);

    hir::HirAssign assign;
    assign.target = target;
    assign.value  = value_slice;
    return addExpr(std::move(assign));
}

hir::HirExprId HirLowerModern::lowerOptionalProp(const frontend::Expression &expr,
                                                 const types::TypeId type) {
    if (expr.operands.empty())
        return hir::kInvalidHirExpr;
    const auto operand = lowerExpr(expr.operands[0]);
    if (operand == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;

    const auto operand_type = typeOfExpr(expr.operands[0]);
    if (types_.kindOf(operand_type) != types::TypeKind::Optional)
        return hir::kInvalidHirExpr;

    // Spill the optional so its payload and tag can be addressed as fields.
    const auto slot = next_slot_++;
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(slot, operand_type));
    current_fn_->blocks[current_block_].insts.push(emitSlotStore(slot, operand));

    // The result of `x?` is stored here so both paths agree on one location.
    const auto result_slot = next_slot_++;
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(result_slot, type));

    const auto is_some = addExpr(hir::HirField{addExpr(hir::HirSlotAddr{slot, operand_type}), 1U,
                                               types::kBoolType, operand_type});

    const auto some_block = newBlock();
    const auto none_block = newBlock();

    hir::HirBranch branch;
    branch.cond       = is_some;
    branch.then_block = static_cast<hir::HirDeclId>(some_block);
    branch.else_block = static_cast<hir::HirDeclId>(none_block);
    setTerminator(addExpr(std::move(branch)));

    // None: propagate by returning None from the enclosing function.
    setCurrentBlock(none_block);
    current_fn_->blocks[none_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    {
        hir::HirRet ret;
        hir::HirMakeNone make_none;
        make_none.type = current_fn_->return_type;
        ret.value      = addExpr(std::move(make_none));
        setTerminator(addExpr(std::move(ret)));
    }

    // Some: unwrap the payload into the result slot and carry on.
    setCurrentBlock(some_block);
    current_fn_->blocks[some_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    const auto payload                    = addExpr(
        hir::HirField{addExpr(hir::HirSlotAddr{slot, operand_type}), 0U, type, operand_type});
    current_fn_->blocks[some_block].insts.push(emitSlotStore(result_slot, payload));
    return emitSlotLoad(result_slot, type);
}

hir::HirExprId HirLowerModern::lowerCondition(frontend::ExprId condition) {
    if (!condition)
        return hir::kInvalidHirExpr;
    const auto condition_type = typeOfExpr(condition);
    if (types_.kindOf(condition_type) == types::TypeKind::Optional)
        return lowerOptionalCondition(condition);
    return lowerExpr(condition);
}

hir::HirExprId HirLowerModern::lowerOptionalCondition(frontend::ExprId id) {
    if (!id)
        return hir::kInvalidHirExpr;
    const auto operand      = lowerExpr(id);
    const auto operand_type = typeOfExpr(id);
    if (operand == hir::kInvalidHirExpr || types_.kindOf(operand_type) != types::TypeKind::Optional)
        return hir::kInvalidHirExpr;

    const auto *optional = std::get_if<types::TypeOptional>(&types_.lookup(operand_type));
    const bool niche =
        optional != nullptr && types_.kindOf(optional->inner) == types::TypeKind::Ptr;
    if (niche) {
        // ?*T uses nullptr as the None sentinel; non-null is the owned boolean.
        hir::HirMakeNone none;
        none.type = operand_type;
        return addExpr(hir::HirBinary{operand, addExpr(std::move(none)), hir::HirBinaryOp::Ne,
                                      types::kBoolType});
    }

    // {payload, tag} layout: spill, read the tag (index 1), use it directly as bool.
    const auto slot = next_slot_++;
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(slot, operand_type));
    current_fn_->blocks[current_block_].insts.push(emitSlotStore(slot, operand));
    return addExpr(hir::HirField{addExpr(hir::HirSlotAddr{slot, operand_type}), 1U,
                                 types::kBoolType, operand_type});
}

/// Lowers extraction of an optional payload. When `checked` is true the None
/// branch terminates through `R10003`; false reads the payload unconditionally.

hir::HirExprId HirLowerModern::lowerOptionalPayload(const frontend::Expression &expr,
                                                    const types::TypeId type, bool checked) {
    if (expr.operands.empty())
        return hir::kInvalidHirExpr;
    const auto operand      = lowerExpr(expr.operands[0]);
    const auto operand_type = typeOfExpr(expr.operands[0]);
    if (operand == hir::kInvalidHirExpr || types_.kindOf(operand_type) != types::TypeKind::Optional)
        return hir::kInvalidHirExpr;
    if (!checked) {
        const auto *optional = std::get_if<types::TypeOptional>(&types_.lookup(operand_type));
        if (optional != nullptr && types_.kindOf(optional->inner) == types::TypeKind::Ptr)
            return operand;
        const auto slot = next_slot_++;
        current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(slot, operand_type));
        current_fn_->blocks[current_block_].insts.push(emitSlotStore(slot, operand));
        return addExpr(
            hir::HirField{addExpr(hir::HirSlotAddr{slot, operand_type}), 0U, type, operand_type});
    }

    const auto slot = next_slot_++;
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(slot, operand_type));
    current_fn_->blocks[current_block_].insts.push(emitSlotStore(slot, operand));

    const auto is_some = [&]() -> hir::HirExprId {
        const auto *optional = std::get_if<types::TypeOptional>(&types_.lookup(operand_type));
        if (optional != nullptr && types_.kindOf(optional->inner) == types::TypeKind::Ptr) {
            hir::HirMakeNone none;
            none.type = operand_type;
            return addExpr(hir::HirBinary{addExpr(hir::HirSlotLoad{slot, operand_type}),
                                          addExpr(std::move(none)), hir::HirBinaryOp::Ne,
                                          types::kBoolType});
        }
        return addExpr(hir::HirField{addExpr(hir::HirSlotAddr{slot, operand_type}), 1U,
                                     types::kBoolType, operand_type});
    };

    const auto some_block = newBlock();
    const auto none_block = newBlock();
    hir::HirBranch branch;
    branch.cond       = is_some();
    branch.then_block = static_cast<hir::HirDeclId>(some_block);
    branch.else_block = static_cast<hir::HirDeclId>(none_block);
    setTerminator(addExpr(std::move(branch)));

    setCurrentBlock(none_block);
    current_fn_->blocks[none_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    hir::HirRuntimePanic panic;
    panic.code = 10003U;
    setTerminator(addExpr(std::move(panic)));

    setCurrentBlock(some_block);
    current_fn_->blocks[some_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    const auto *optional = std::get_if<types::TypeOptional>(&types_.lookup(operand_type));
    if (optional != nullptr && types_.kindOf(optional->inner) == types::TypeKind::Ptr)
        return emitSlotLoad(slot, operand_type);
    return addExpr(
        hir::HirField{addExpr(hir::HirSlotAddr{slot, operand_type}), 0U, type, operand_type});
}

hir::HirExprId HirLowerModern::lowerMust(const frontend::Expression &expr,
                                         const types::TypeId type) {
    return lowerOptionalPayload(expr, type, /*checked=*/true);
}

hir::HirExprId HirLowerModern::lowerRawOptional(const frontend::Expression &expr,
                                                const types::TypeId type) {
    return lowerOptionalPayload(expr, type, /*checked=*/false);
}

hir::HirExprId HirLowerModern::lowerCast(const frontend::Expression &expr,
                                         const types::TypeId type) {
    if (expr.operands.empty())
        return hir::kInvalidHirExpr;
    auto value = lowerExpr(expr.operands[0]);
    if (value == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    auto from = typeOfExpr(expr.operands[0]);
    if (current_module_ != nullptr &&
        (sema_.isSelfReceiver(current_module_->key, expr.operands[0]) ||
         sema_.isBorrowParameter(current_module_->key, expr.operands[0]))) {
        const auto sema_from = sema_.typeTable().stripQualifiers(semaTypeOfExpr(expr.operands[0]));
        const auto *ptr      = sema_.typeTable().pointer(sema_from);
        if (ptr != nullptr) {
            const auto pointee     = sema_.typeTable().stripQualifiers(ptr->pointee);
            const auto pointee_hir = lowerType(pointee);
            if (pointee_hir != types::kInvalidType && pointee_hir != types::kErrorType) {
                from  = pointee_hir;
                value = addExpr(hir::HirUnary{hir::HirUnaryOp::Deref, value, pointee_hir});
            }
        }
    }
    if (from == type)
        return value;
    const bool from_opaque = types_.kindOf(from) == types::TypeKind::OpaqueTagged;
    const bool to_opaque   = types_.kindOf(type) == types::TypeKind::OpaqueTagged;

    // Bare `opaque` is handled before nominal/union casts: `T as opaque` must
    // erase the whole value, not treat a one-field aggregate as a wrapper.
    if (to_opaque) {
        if (current_module_ == nullptr || current_module_->key != snapshot_.rootModuleKey()) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::UnsupportedSyntax,
                          "bare 'opaque' type ids are module-local; imported or cached "
                          "'opaque' values are not supported in this version",
                          current_module_ != nullptr ? memory::Span{current_module_->fileId,
                                                                    expr.span.start, expr.span.end}
                                                     : memory::Span{});
            return hir::kInvalidHirExpr;
        }
        hir::HirMakeOpaque make;
        make.value       = value;
        make.source_type = from;
        make.opaque_type = type;
        make.type_id     = stableConcreteTypeId(from);
        return addExpr(std::move(make));
    }

    // Nominal casts (`T as Nominal` / `Nominal as T`) are lowered as the
    // one-field wrapper's literal/extraction, not as numeric conversions.
    if (types_.kindOf(type) == types::TypeKind::Struct && from != type) {
        const auto wrapper_count = types_.fieldCount(type);
        if (wrapper_count == 1U) {
            hir::HirStructLiteral literal(arena_);
            literal.type = type;
            literal.values.push(value);
            return addExpr(std::move(literal));
        }
    }
    if (types_.kindOf(from) == types::TypeKind::Struct && from != type) {
        const auto wrapper_count = types_.fieldCount(from);
        if (wrapper_count == 1U)
            return addExpr(hir::HirField{value, 0U, type, from});
    }

    // `opaque as T` lowers to a checked optional extraction. The HIR cast keeps
    // the type id so codegen can branch on it and emit Some/None payloads.
    if (from_opaque && !to_opaque) {
        if (current_module_ == nullptr || current_module_->key != snapshot_.rootModuleKey()) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::UnsupportedSyntax,
                          "bare 'opaque' type ids are module-local; imported or cached "
                          "'opaque' values are not supported in this version",
                          current_module_ != nullptr ? memory::Span{current_module_->fileId,
                                                                    expr.span.start, expr.span.end}
                                                     : memory::Span{});
            return hir::kInvalidHirExpr;
        }

        // `opaque as raw opaque` is the explicit unchecked way to reinterpret a
        // bare opaque as the `void*` it stores: it is a pointer extraction, not a
        // load of a concrete T payload.
        if (types_.kindOf(type) == types::TypeKind::Ptr) {
            const auto *ptr = std::get_if<types::TypePtr>(&types_.lookup(type));
            if (ptr != nullptr && ptr->pointee != types::kInvalidType &&
                types_.kindOf(ptr->pointee) == types::TypeKind::Void) {
                hir::HirOpaqueCast cast;
                cast.value       = value;
                cast.from        = from;
                cast.to          = type;
                cast.checked     = false;
                cast.type_id     = stableConcreteTypeId(from);
                cast.opaque_type = from;
                cast.result_type = type;
                cast.returns_ptr = true;
                return addExpr(std::move(cast));
            }
        }

        if (expr.is_raw) {
            hir::HirOpaqueCast cast;
            cast.value       = value;
            cast.from        = from;
            cast.to          = type;
            cast.checked     = false;
            cast.type_id     = stableConcreteTypeId(type);
            cast.opaque_type = from;
            cast.result_type = type;
            return addExpr(std::move(cast));
        }

        // Sema reports the checked extraction as `?T`, where `T` is the cast
        // target written by the user. The opaque tag was recorded for the
        // concrete payload type, so unwrap the optional here instead of hashing
        // the optional TypeId.
        const auto *optional_result = std::get_if<types::TypeOptional>(&types_.lookup(type));
        const auto payload_type     = optional_result != nullptr ? optional_result->inner : type;
        const auto result_type      = types_.internOptional(payload_type);
        const auto value_slot       = next_slot_++;
        const auto result_slot      = next_slot_++;
        current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(value_slot, from));
        current_fn_->blocks[current_block_].insts.push(emitSlotStore(value_slot, value));
        current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(result_slot, result_type));

        hir::HirOpaqueCheck check;
        check.value         = emitSlotLoad(value_slot, from);
        check.opaque_type   = from;
        check.type_id       = stableConcreteTypeId(payload_type);
        const auto check_id = addExpr(std::move(check));

        const auto some_block  = newBlock();
        const auto none_block  = newBlock();
        const auto merge_block = newBlock();
        hir::HirBranch branch;
        branch.cond       = check_id;
        branch.then_block = static_cast<hir::HirDeclId>(some_block);
        branch.else_block = static_cast<hir::HirDeclId>(none_block);
        setTerminator(addExpr(std::move(branch)));

        setCurrentBlock(some_block);
        current_fn_->blocks[some_block].insts = memory::DynArray<hir::HirExprId>(arena_);
        hir::HirOpaqueCast cast;
        cast.value         = emitSlotLoad(value_slot, from);
        cast.from          = from;
        cast.to            = payload_type;
        cast.checked       = false;
        cast.type_id       = stableConcreteTypeId(payload_type);
        cast.opaque_type   = from;
        cast.result_type   = payload_type;
        const auto payload = addExpr(std::move(cast));
        hir::HirMakeSome some;
        some.type  = result_type;
        some.value = payload;
        current_fn_->blocks[some_block].insts.push(
            emitSlotStore(result_slot, addExpr(std::move(some))));
        emitJump(merge_block);

        setCurrentBlock(none_block);
        current_fn_->blocks[none_block].insts = memory::DynArray<hir::HirExprId>(arena_);
        hir::HirMakeNone none;
        none.type = result_type;
        current_fn_->blocks[none_block].insts.push(
            emitSlotStore(result_slot, addExpr(std::move(none))));
        emitJump(merge_block);

        setCurrentBlock(merge_block);
        current_fn_->blocks[merge_block].insts = memory::DynArray<hir::HirExprId>(arena_);
        return emitSlotLoad(result_slot, result_type);
    }

    if (types_.kindOf(from) == types::TypeKind::Union &&
        types_.kindOf(type) != types::TypeKind::Union) {
        const auto *union_type = std::get_if<types::TypeUnion>(&types_.lookup(from));
        const auto *def =
            union_type != nullptr ? types_.lookupUnionDef(union_type->def_id) : nullptr;
        if (def != nullptr && def->is_tagged) {
            hir::HirUnionCast cast;
            cast.value        = value;
            cast.from         = from;
            cast.to           = type;
            cast.member_index = taggedMemberIndex(from, type);
            cast.checked      = !expr.is_raw;
            return addExpr(std::move(cast));
        }
    }
    if (types_.kindOf(from) == types::TypeKind::Union ||
        types_.kindOf(type) == types::TypeKind::Union)
        return addExpr(hir::HirUnionCast{value, from, type});

    return addExpr(hir::HirCast{value, from, type});
}

hir::HirExprId HirLowerModern::lowerIsNull(const frontend::Expression &expr) {
    if (expr.operands.empty())
        return hir::kInvalidHirExpr;
    const auto operand = lowerExpr(expr.operands[0]);
    if (operand == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    const auto operand_type = typeOfExpr(expr.operands[0]);
    if (types_.kindOf(operand_type) != types::TypeKind::Optional)
        return hir::kInvalidHirExpr;

    const auto *optional = std::get_if<types::TypeOptional>(&types_.lookup(operand_type));
    const bool niche =
        optional != nullptr && types_.kindOf(optional->inner) == types::TypeKind::Ptr;
    if (niche) {
        // ?*T uses nullptr as the None sentinel: compare against MakeNone directly.
        hir::HirMakeNone none;
        none.type = operand_type;
        return addExpr(hir::HirBinary{operand, addExpr(std::move(none)), hir::HirBinaryOp::Eq,
                                      types::kBoolType});
    }

    // {payload, tag} layout: spill, read the tag, and negate it.
    const auto slot = next_slot_++;
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(slot, operand_type));
    current_fn_->blocks[current_block_].insts.push(emitSlotStore(slot, operand));
    const auto tag = addExpr(hir::HirField{addExpr(hir::HirSlotAddr{slot, operand_type}), 1U,
                                           types::kBoolType, operand_type});
    return addExpr(hir::HirUnary{hir::HirUnaryOp::Not, tag, types::kBoolType});
}

hir::HirExprId HirLowerModern::lowerIsType(const frontend::Expression &expr) {
    if (expr.operands.empty() || !expr.cast_type || current_module_ == nullptr ||
        current_module_->frontend == nullptr)
        return hir::kInvalidHirExpr;
    auto operand = lowerExpr(expr.operands[0]);
    if (operand == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    auto operand_type = typeOfExpr(expr.operands[0]);
    if ((sema_.isSelfReceiver(current_module_->key, expr.operands[0]) ||
         sema_.isBorrowParameter(current_module_->key, expr.operands[0]))) {
        const auto sema_operand =
            sema_.typeTable().stripQualifiers(semaTypeOfExpr(expr.operands[0]));
        const auto *ptr = sema_.typeTable().pointer(sema_operand);
        if (ptr != nullptr) {
            const auto pointee     = sema_.typeTable().stripQualifiers(ptr->pointee);
            const auto pointee_hir = lowerType(pointee);
            if (pointee_hir != types::kInvalidType && pointee_hir != types::kErrorType) {
                operand      = addExpr(hir::HirUnary{hir::HirUnaryOp::Deref, operand, pointee_hir});
                operand_type = pointee_hir;
            }
        }
    }
    if (types_.kindOf(operand_type) == types::TypeKind::OpaqueTagged) {
        const auto target = lowerType(lowerTypeExprConcrete(expr.cast_type));
        if (target == types::kErrorType || target == types::kInvalidType)
            return hir::kInvalidHirExpr;
        hir::HirOpaqueCheck check;
        check.value       = operand;
        check.opaque_type = operand_type;
        check.type_id     = stableConcreteTypeId(target);
        return addExpr(std::move(check));
    }
    if (types_.kindOf(operand_type) != types::TypeKind::Union)
        return hir::kInvalidHirExpr;
    const auto *union_type = std::get_if<types::TypeUnion>(&types_.lookup(operand_type));
    if (union_type == nullptr)
        return hir::kInvalidHirExpr;
    const auto *def = types_.lookupUnionDef(union_type->def_id);
    if (def == nullptr || !def->is_tagged)
        return hir::kInvalidHirExpr;
    const auto target = lowerType(lowerTypeExprConcrete(expr.cast_type));
    if (target == types::kErrorType || target == types::kInvalidType)
        return hir::kInvalidHirExpr;
    uint32_t member_index = 0;
    for (const auto member : def->members) {
        if (member == target)
            break;
        ++member_index;
    }
    if (member_index >= def->members.size())
        return hir::kInvalidHirExpr;

    const auto slot = next_slot_++;
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(slot, operand_type));
    current_fn_->blocks[current_block_].insts.push(emitSlotStore(slot, operand));
    const auto tag_type = tagType(types_, static_cast<uint32_t>(def->members.size()));
    const auto tag      = addExpr(
        hir::HirField{addExpr(hir::HirSlotAddr{slot, operand_type}), 1U, tag_type, operand_type});
    hir::HirUnionCheck check;
    check.value        = tag;
    check.union_type   = operand_type;
    check.member_index = member_index;
    return addExpr(std::move(check));
}

hir::HirExprId HirLowerModern::lowerLayoutIntrinsic(const frontend::Expression &expr) {
    hir::HirLayoutIntrinsic intrinsic;
    if (expr.text == "lengthOf" || expr.text == "ptrOf") {
        if (expr.operands.empty())
            return hir::kInvalidHirExpr;
        if (expr.text == "lengthOf") {
            // Decode escapes once so @lengthOf on a string literal returns the
            // in-memory character count rather than source bytes plus quotes.
            std::string decoded;
            std::string_view text(
                expr.operands[0].value <= current_module_->frontend->expressions().size()
                    ? current_module_->frontend->expressions()[expr.operands[0].value - 1U].text
                    : std::string_view{});
            if (!text.empty() && text.front() == '"' && text.back() == '"')
                (void)decodeEscapes(std::string_view(text.data() + 1U, text.size() - 2U), decoded,
                                    true);
            intrinsic.string_length = decoded.empty() ? 0 : decoded.size();
        }
        intrinsic.which        = expr.text == "lengthOf" ? hir::HirLayoutIntrinsic::Which::LengthOf
                                                         : hir::HirLayoutIntrinsic::Which::PtrOf;
        intrinsic.operand      = lowerExpr(expr.operands[0]);
        intrinsic.operand_type = typeOfExpr(expr.operands[0]);
        intrinsic.type         = typeOfExpr(expr.id);
        if (intrinsic.operand == hir::kInvalidHirExpr || intrinsic.type == types::kErrorType ||
            intrinsic.type == types::kInvalidType)
            return hir::kInvalidHirExpr;
        return addExpr(std::move(intrinsic));
    }
    if (expr.text == "sizeOf")
        intrinsic.which = hir::HirLayoutIntrinsic::Which::SizeOf;
    else if (expr.text == "alignOf")
        intrinsic.which = hir::HirLayoutIntrinsic::Which::AlignOf;
    else
        intrinsic.which = hir::HirLayoutIntrinsic::Which::OffsetOf;
    if (current_module_ == nullptr || current_module_->frontend == nullptr || !expr.cast_type)
        return hir::kInvalidHirExpr;
    const sema::modern::TypeId sema_type = lowerTypeExprConcrete(expr.cast_type);
    if (!sema_type)
        return hir::kInvalidHirExpr;
    const types::TypeId struct_type = lowerType(sema_type);
    if (struct_type == types::kErrorType)
        return hir::kInvalidHirExpr;
    intrinsic.type = struct_type;
    if (!expr.field_names.empty()) {
        const size_t field_index = types_.fieldIndex(struct_type, expr.field_names[0]);
        if (field_index == static_cast<size_t>(-1))
            return hir::kInvalidHirExpr;
        intrinsic.field_index = static_cast<uint32_t>(field_index);
    }
    return addExpr(std::move(intrinsic));
}

hir::HirExprId HirLowerModern::lowerIndex(const frontend::Expression &expr,
                                          const types::TypeId type) {
    if (expr.operands.size() < 2U)
        return hir::kInvalidHirExpr;
    const auto object = lowerExpr(expr.operands[0]);
    const auto index  = lowerExpr(expr.operands[1]);
    if (object == hir::kInvalidHirExpr || index == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;

    const auto object_type = typeOfExpr(expr.operands[0]);
    const auto sema_object = sema_.typeTable().stripQualifiers(semaTypeOfExpr(expr.operands[0]));
    if (sema_.typeTable().kindOf(sema_object) == sema::modern::TypeKind::Pack) {
        const auto *pack = sema_.typeTable().pack(sema_object);
        if (pack != nullptr) {
            int64_t index_value     = 0;
            const auto *module_sema = sema_.findModuleSema(current_module_->key);
            if (module_sema == nullptr ||
                !module_sema->constantIntegerValue(expr.operands[1], index_value) ||
                index_value < 0 || static_cast<uint64_t>(index_value) >= pack->members.size())
                return hir::kInvalidHirExpr;
            return addExpr(
                hir::HirField{object, static_cast<uint32_t>(index_value), type, object_type});
        }
    }
    hir::HirIndex indexing;
    indexing.object   = object;
    indexing.index    = index;
    indexing.type     = type;
    indexing.obj_type = object_type;
    indexing.is_array = types_.kindOf(object_type) == types::TypeKind::Array;
    if (expr.is_raw)
        return addExpr(std::move(indexing));

    const auto *optional    = std::get_if<types::TypeOptional>(&types_.lookup(type));
    const auto element_type = optional != nullptr ? optional->inner : type;
    const auto index_type   = typeOfExpr(expr.operands[1]);
    if (optional == nullptr)
        return addExpr(std::move(indexing));

    // Evaluate the object and index once, then branch on the dynamic bounds checks.
    const auto object_slot = next_slot_++;
    const auto index_slot  = next_slot_++;
    const auto result_slot = next_slot_++;
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(object_slot, object_type));
    current_fn_->blocks[current_block_].insts.push(emitSlotStore(object_slot, object));
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(
        index_slot,
        types_.kindOf(index_type) == types::TypeKind::Int ? index_type : types::kErrorType));
    current_fn_->blocks[current_block_].insts.push(emitSlotStore(index_slot, index));
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(result_slot, type));

    const auto loaded_index = emitSlotLoad(index_slot, index_type);
    hir::HirLiteral zero;
    zero.type          = index_type;
    zero.i             = 0;
    hir::HirExprId len = hir::kInvalidHirExpr;
    const auto *array  = std::get_if<types::TypeArray>(&types_.lookup(object_type));
    if (array != nullptr) {
        hir::HirLiteral len_lit;
        len_lit.type = index_type;
        len_lit.i    = static_cast<int64_t>(array->count);
        len          = addExpr(std::move(len_lit));
    } else {
        const auto loaded = emitSlotLoad(object_slot, object_type);
        auto len_i64 =
            addExpr(hir::HirField{loaded, 1U, types_.internInt(types::IntWidth::I64), object_type});
        const auto len64 = types_.internInt(types::IntWidth::I64);
        if (types_.kindOf(index_type) != types::TypeKind::Int) {
            len = hir::kInvalidHirExpr;
        } else if (index_type != len64) {
            len = addExpr(hir::HirCast{len_i64, len64, index_type});
        } else {
            len = len_i64;
        }
    }
    if (len == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;

    hir::HirBinary ge_zero;
    ge_zero.lhs           = loaded_index;
    ge_zero.rhs           = addExpr(std::move(zero));
    ge_zero.op            = hir::HirBinaryOp::Ge;
    ge_zero.type          = types::kBoolType;
    ge_zero.operand_type  = index_type;
    const auto ge_zero_id = addExpr(std::move(ge_zero));

    hir::HirBinary lt_len;
    lt_len.lhs           = loaded_index;
    lt_len.rhs           = len;
    lt_len.op            = hir::HirBinaryOp::Lt;
    lt_len.type          = types::kBoolType;
    lt_len.operand_type  = index_type;
    const auto lt_len_id = addExpr(std::move(lt_len));

    hir::HirBinary all_ok;
    all_ok.lhs          = ge_zero_id;
    all_ok.rhs          = lt_len_id;
    all_ok.op           = hir::HirBinaryOp::And;
    all_ok.type         = types::kBoolType;
    all_ok.operand_type = types::kBoolType;

    const auto some_block  = newBlock();
    const auto none_block  = newBlock();
    const auto merge_block = newBlock();
    hir::HirBranch branch;
    branch.cond       = addExpr(std::move(all_ok));
    branch.then_block = static_cast<hir::HirDeclId>(some_block);
    branch.else_block = static_cast<hir::HirDeclId>(none_block);
    setTerminator(addExpr(std::move(branch)));

    setCurrentBlock(some_block);
    current_fn_->blocks[some_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    {
        hir::HirIndex ok_index;
        ok_index.object   = emitSlotLoad(object_slot, object_type);
        ok_index.index    = emitSlotLoad(index_slot, index_type);
        ok_index.type     = element_type;
        ok_index.obj_type = object_type;
        ok_index.is_array = indexing.is_array;
        hir::HirMakeSome some;
        some.type  = type;
        some.value = addExpr(std::move(ok_index));
        current_fn_->blocks[some_block].insts.push(
            emitSlotStore(result_slot, addExpr(std::move(some))));
        emitJump(merge_block);
    }

    setCurrentBlock(none_block);
    current_fn_->blocks[none_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    {
        hir::HirMakeNone none;
        none.type = type;
        current_fn_->blocks[none_block].insts.push(
            emitSlotStore(result_slot, addExpr(std::move(none))));
        emitJump(merge_block);
    }

    setCurrentBlock(merge_block);
    current_fn_->blocks[merge_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    return emitSlotLoad(result_slot, type);
}

hir::HirExprId HirLowerModern::lowerCoerceToTarget(types::TypeId target,
                                                   frontend::ExprId expression,
                                                   hir::HirExprId value) {
    if (value == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    const auto source_type = typeOfExpr(expression);
    // Sema retypes a string literal accepted for a `[]char` target, so the
    // expression's source type is no longer `*char` here. Detect the literal
    // directly and reconstruct its pointer payload plus decoded bounds.
    if (types_.kindOf(target) == types::TypeKind::Slice && expression &&
        expression.value <= current_module_->frontend->expressions().size()) {
        const auto &literal = current_module_->frontend->expressions()[expression.value - 1U];
        if (literal.kind == frontend::ExprKind::Literal && literal.text.size() >= 2U &&
            literal.text.front() == '"' && literal.text.back() == '"') {
            const auto lowered_slice = std::get_if<types::TypeSlice>(&types_.lookup(target));
            if (lowered_slice != nullptr &&
                types_.kindOf(lowered_slice->elem) == types::TypeKind::Char) {
                const sema::modern::TypeId char_sema = sema_.typeTable().lookupNamed("char");
                const types::TypeId pointer_hir =
                    char_sema ? types_.internPtr(lowerType(char_sema)) : types::kErrorType;
                std::string decoded;
                if (pointer_hir != types::kErrorType &&
                    decodeEscapes(
                        std::string_view(literal.text.data() + 1U, literal.text.size() - 2U),
                        decoded, true)) {
                    hir::HirLiteral pointer_literal;
                    pointer_literal.type     = pointer_hir;
                    pointer_literal.str_val  = interner_.intern(std::string_view(decoded));
                    const auto pointer_value = addExpr(std::move(pointer_literal));
                    hir::HirMakeSlice slice;
                    slice.object      = pointer_value;
                    slice.type        = target;
                    slice.object_type = pointer_hir;
                    slice.bound_type  = types_.internInt(types::IntWidth::I64);
                    slice.is_pointer  = true;
                    hir::HirLiteral lo;
                    lo.type = slice.bound_type;
                    lo.i    = 0;
                    hir::HirLiteral hi;
                    hi.type  = slice.bound_type;
                    hi.i     = static_cast<int64_t>(decoded.size());
                    slice.lo = addExpr(std::move(lo));
                    slice.hi = addExpr(std::move(hi));
                    return addExpr(std::move(slice));
                }
            }
        }
    }
    if (types_.kindOf(source_type) != types::TypeKind::Array ||
        types_.kindOf(target) != types::TypeKind::Slice) {
        if (types_.kindOf(source_type) == types::TypeKind::Slice &&
            types_.kindOf(target) == types::TypeKind::Ptr) {
            hir::HirLayoutIntrinsic intrinsic;
            intrinsic.which        = hir::HirLayoutIntrinsic::Which::PtrOf;
            intrinsic.type         = target;
            intrinsic.operand      = value;
            intrinsic.operand_type = source_type;
            return addExpr(std::move(intrinsic));
        }
        return value;
    }

    hir::HirMakeSlice slice;
    slice.object      = value;
    slice.type        = target;
    slice.object_type = source_type;
    slice.is_array    = true;
    slice.is_pointer  = false;
    slice.checked     = false;

    const auto array      = std::get_if<types::TypeArray>(&types_.lookup(source_type));
    const auto count      = array != nullptr ? static_cast<int64_t>(array->count) : 0;
    const auto index_type = types_.internInt(types::IntWidth::I64);
    slice.bound_type      = index_type;
    hir::HirLiteral lo;
    lo.type = index_type;
    lo.i    = 0;
    hir::HirLiteral hi;
    hi.type  = index_type;
    hi.i     = count;
    slice.lo = addExpr(std::move(lo));
    slice.hi = addExpr(std::move(hi));
    return addExpr(std::move(slice));
}

hir::HirExprId

HirLowerModern::lowerVariadicSliceTail(sema::modern::TypeId slice_sema_type,
                                       const std::vector<frontend::ExprId> &tail_exprs) {
    const types::TypeId slice_type = lowerType(slice_sema_type);
    const auto *slice              = std::get_if<types::TypeSlice>(&types_.lookup(slice_type));
    if (slice == nullptr)
        return hir::kInvalidHirExpr;

    const types::TypeId element_type = slice->elem;
    const size_t tail_count          = tail_exprs.size();
    const types::TypeId array_type =
        types_.internArray(element_type, static_cast<uint32_t>(tail_count));

    hir::HirArrayLiteral array(arena_);
    array.type = array_type;
    for (size_t index = 0; index < tail_count; ++index) {
        const auto &arg_expr =
            current_module_->frontend->expressions()[tail_exprs[index].value - 1U];
        const bool annotated =
            arg_expr.kind == frontend::ExprKind::OwnershipCoerce && !arg_expr.operands.empty();
        const frontend::ExprId inner_id = annotated ? arg_expr.operands[0] : tail_exprs[index];
        auto argument                   = lowerExpr(inner_id);
        if (annotated) {
            auto address = lowerLValueAddr(inner_id);
            if (address == hir::kInvalidHirExpr) {
                const auto inner_type = typeOfExpr(inner_id);
                const auto slot       = next_slot_++;
                current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(slot, inner_type));
                current_fn_->blocks[current_block_].insts.push(emitSlotStore(slot, argument));
                address = addExpr(hir::HirSlotAddr{slot, inner_type});
            }
            argument = address;
        }
        if (argument == hir::kInvalidHirExpr)
            return hir::kInvalidHirExpr;
        array.elements.push(argument);
    }

    const auto array_id = addExpr(std::move(array));
    // Keep the temporary array alive for the lifetime of the slice. Callers
    // lower every argument into the current slot/block model, so the array is
    // materialised as a slot before the slice takes its address.
    const auto array_slot = next_slot_++;
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(array_slot, array_type));
    current_fn_->blocks[current_block_].insts.push(emitSlotStore(array_slot, array_id));
    const auto array_slot_addr = addExpr(hir::HirSlotAddr{array_slot, array_type});

    hir::HirMakeSlice result;
    result.type        = slice_type;
    result.object      = array_slot_addr;
    result.object_type = array_type;
    result.bound_type  = types_.internInt(types::IntWidth::I64);
    result.is_array    = true;
    result.is_pointer  = false;
    result.checked     = false;
    hir::HirLiteral lo;
    lo.type = result.bound_type;
    lo.i    = 0;
    hir::HirLiteral hi;
    hi.type   = result.bound_type;
    hi.i      = static_cast<int64_t>(tail_count);
    result.lo = addExpr(std::move(lo));
    result.hi = addExpr(std::move(hi));
    return addExpr(std::move(result));
}

hir::HirExprId HirLowerModern::lowerCoerceToDyn(sema::modern::TypeId target,
                                                frontend::ExprId expression, hir::HirExprId value,
                                                sema::modern::TypeId target_sema) {
    const types::TypeId target_hir = lowerType(target);
    if (value == hir::kInvalidHirExpr || types_.kindOf(target_hir) != types::TypeKind::Dyn ||
        current_module_ == nullptr)
        return value;

    const sema::modern::TypeId concrete_sema =
        sema_.typeTable().stripQualifiers(semaTypeOfExpr(expression));
    const auto *concrete_struct = sema_.typeTable().struct_type(concrete_sema);
    const auto *concrete_enum   = sema_.typeTable().enum_type(concrete_sema);
    const auto *concrete_union  = sema_.typeTable().union_type(concrete_sema);
    if (concrete_struct == nullptr && concrete_enum == nullptr && concrete_union == nullptr)
        return value;

    const sema::modern::TypeId target_ty = [&]() -> sema::modern::TypeId {
        const sema::modern::TypeId canonical_target =
            sema_.typeTable().stripQualifiers(sema_.typeTable().canonical(target_sema));
        const auto *target_dyn = sema_.typeTable().dyn_type(canonical_target);
        if (target_dyn != nullptr)
            return sema_.typeTable().canonical(target_dyn->target);
        return canonical_target;
    }();
    const auto *target_trait = sema_.typeTable().trait(target_ty);
    if (target_trait == nullptr)
        return value;

    const auto findDeclAcrossModules = [&](std::string_view name, frontend::DeclKind kind) {
        for (const auto &decl : current_module_->frontend->declarations()) {
            if (decl.kind == kind && decl.name == name)
                return &decl;
        }
        for (const auto &module_ptr : snapshot_.modules()) {
            if (module_ptr->key == current_module_->key || module_ptr->frontend == nullptr)
                continue;
            for (const auto &decl : module_ptr->frontend->declarations()) {
                if (decl.kind == kind && decl.name == name)
                    return &decl;
            }
        }
        return static_cast<const frontend::Declaration *>(nullptr);
    };
    const bool is_interface =
        findDeclAcrossModules(target_trait->name, frontend::DeclKind::Interface) != nullptr;
    const frontend::Declaration *dyn_decl =
        findDeclAcrossModules(target_trait->name, is_interface ? frontend::DeclKind::Interface
                                                               : frontend::DeclKind::Trait);
    if (dyn_decl == nullptr)
        return value;

    const auto concreteArgs = [&]() -> std::vector<sema::modern::TypeId> {
        std::vector<sema::modern::TypeId> args;
        if (concrete_enum != nullptr && concrete_enum->name.find('<') != std::string_view::npos) {
            const auto *template_decl =
                findDeclAcrossModules(concrete_enum->name.substr(0, concrete_enum->name.find('<')),
                                      frontend::DeclKind::Enum);
            if (template_decl == nullptr)
                return args;
            const size_t degree         = template_decl->genericParams.size();
            const std::string_view name = concrete_enum->name;
            const char *open            = name.data();
            const char *end             = open + name.size();
            const char *lt              = std::find(open, end, '<');
            const char *close           = std::find(lt + 1, end, '>');
            const char *cursor          = lt + 1;
            while (cursor < close) {
                const char *comma  = std::find(cursor, close, ',');
                const auto text    = std::string_view(cursor, static_cast<size_t>(comma - cursor));
                const auto trimmed = [text]() {
                    size_t first = 0;
                    size_t last  = text.size();
                    while (first < last && (text[first] == ' ' || text[first] == '\t'))
                        ++first;
                    while (last > first && (text[last - 1U] == ' ' || text[last - 1U] == '\t'))
                        --last;
                    return std::string_view(text.data() + first, last - first);
                }();
                args.push_back(sema_.typeTable().lookupNamed(trimmed));
                if (comma == close)
                    break;
                cursor = comma + 1;
            }
            while (args.size() < degree)
                args.push_back(sema::modern::kInvalidTypeId);
        } else if (concrete_union != nullptr) {
            args.assign(concrete_union->members.begin(), concrete_union->members.end());
        }
        return args;
    }();

    struct ReqInfo {
        const frontend::Declaration *req = nullptr;
        const frontend::Declaration *fn  = nullptr;
        session::ModuleKey req_module{};
        session::ModuleKey fn_module{};
        std::vector<sema::modern::TypeId> fn_args;
    };
    memory::DynArray<ReqInfo> requirements(arena_);
    const auto reqKey = [](const ReqInfo &info) {
        return std::make_pair(info.req_module, static_cast<uint32_t>(info.req->id.value));
    };
    const auto collectRequirement = [&](const session::ModuleArtifact &artifact) {
        for (const auto &candidate : artifact.frontend->declarations()) {
            if (candidate.kind != frontend::DeclKind::Function ||
                candidate.ownerName != target_trait->name || candidate.name.empty() ||
                candidate.name == "self")
                continue;
            bool seen = false;
            for (const auto &info : requirements)
                if (info.req != nullptr && info.req_module == artifact.key &&
                    info.req->id.value == candidate.id.value)
                    seen = true;
            if (seen)
                continue;
            ReqInfo info;
            info.req        = &candidate;
            info.req_module = artifact.key;
            requirements.push(info);
            (void)reqKey;
        }
    };
    collectRequirement(*current_module_);
    for (const auto &module_ptr : snapshot_.modules()) {
        if (module_ptr->key == current_module_->key || module_ptr->frontend == nullptr)
            continue;
        collectRequirement(*module_ptr);
    }

    std::string struct_name;
    if (concrete_struct != nullptr)
        struct_name = concrete_struct->name;
    else if (concrete_enum != nullptr)
        struct_name = concrete_enum->name;
    else if (concrete_union != nullptr)
        struct_name = concrete_union->name;
    if (const size_t angle = struct_name.find('<'); angle != std::string::npos)
        struct_name.resize(angle);

    const auto findMethod = [&](const session::ModuleArtifact &artifact, std::string_view name,
                                const frontend::Declaration **out, session::ModuleKey *out_module) {
        for (const auto &candidate : artifact.frontend->declarations()) {
            if (candidate.kind != frontend::DeclKind::Function ||
                candidate.ownerName != struct_name || candidate.name != name)
                continue;
            *out        = &candidate;
            *out_module = artifact.key;
            return true;
        }
        return false;
    };
    const auto findTraitMethod = [&](const session::ModuleArtifact &artifact, std::string_view name,
                                     const frontend::Declaration **out,
                                     session::ModuleKey *out_module) {
        for (const auto &candidate : artifact.frontend->declarations()) {
            if (candidate.kind != frontend::DeclKind::Function ||
                candidate.ownerName != target_trait->name || candidate.name != name)
                continue;
            *out        = &candidate;
            *out_module = artifact.key;
            return true;
        }
        return false;
    };
    const auto findMethodEverywhere = [&](const auto &finder, std::string_view name,
                                          const frontend::Declaration **out,
                                          session::ModuleKey *out_module) {
        if (finder(*current_module_, name, out, out_module))
            return true;
        for (const auto &module_ptr : snapshot_.modules()) {
            if (module_ptr->key == current_module_->key || module_ptr->frontend == nullptr)
                continue;
            if (finder(*module_ptr, name, out, out_module))
                return true;
        }
        return false;
    };

    for (auto &info : requirements) {
        const frontend::Declaration *concrete = nullptr;
        session::ModuleKey concrete_module{};
        // For nominal traits, an `implement Owner as Trait` method is the
        // concrete override; otherwise the trait default is the slot. For
        // structural interfaces, the concrete struct method is the slot.
        if (is_interface) {
            if (!findMethodEverywhere(findMethod, info.req->name, &concrete, &concrete_module)) {
                diags_.report(diagnostics::Severity::Error, diagnostics::err::NoMember,
                              "dyn interface implementation has no method '" + info.req->name + "'",
                              {});
                return hir::kInvalidHirExpr;
            }
        } else if (!findMethodEverywhere(findMethod, info.req->name, &concrete, &concrete_module)) {
            if (!findMethodEverywhere(findTraitMethod, info.req->name, &concrete,
                                      &concrete_module)) {
                diags_.report(diagnostics::Severity::Error, diagnostics::err::NoMember,
                              "dyn target implementation has no method '" + info.req->name + "'",
                              {});
                return hir::kInvalidHirExpr;
            }
        }
        info.fn        = concrete;
        info.fn_module = concrete_module;
        info.fn_args   = concreteArgs;
    }

    const std::string vtable_name =
        "_zith.vtable." + std::string(target_trait->name) + "." + struct_name;
    const memory::InternedId vtable_id = interner_.intern(vtable_name);
    hir::HirVTable *vtable             = nullptr;
    for (size_t vi = 0; vi < hir_.getVTableCount(); ++vi)
        if (hir_.getVTable(vi).name == vtable_id) {
            vtable = const_cast<hir::HirVTable *>(&hir_.getVTable(vi));
            break;
        }
    if (vtable == nullptr) {
        auto &added = hir_.addVTable(vtable_id);
        for (const auto &info : requirements) {
            if (info.fn == nullptr)
                continue;
            symbols::SymId slot_sym = symbols::kInvalidSym;
            const auto key          = internFunctionKey(interner_, info.fn_module, info.fn->id);
            if (auto *instantiations = sema_.instantiations(); instantiations != nullptr) {
                size_t instance_index = ~size_t{0};
                for (size_t index = 0; index < instantiations->instanceCount(); ++index) {
                    const auto *instance = instantiations->at(index);
                    if (instance != nullptr && instance->module == info.fn_module &&
                        instance->decl == info.fn->id && instance->args == info.fn_args) {
                        instance_index = index;
                        break;
                    }
                }
                if (instance_index == ~size_t{0} && !info.fn_args.empty()) {
                    const auto *module_sema = sema_.findModuleSema(info.fn_module);
                    if (module_sema != nullptr) {
                        const auto *fn_sema =
                            sema_.typeTable().function(module_sema->typeOfDecl(info.fn->id));
                        if (fn_sema != nullptr) {
                            const bool existed = [&]() {
                                for (size_t index = 0; index < instantiations->instanceCount();
                                     ++index) {
                                    const auto *candidate = instantiations->at(index);
                                    if (candidate != nullptr &&
                                        candidate->module == info.fn_module &&
                                        candidate->decl == info.fn->id &&
                                        candidate->args == info.fn_args)
                                        return true;
                                }
                                return false;
                            }();
                            instance_index =
                                instantiations->bindCall(current_module_->key, frontend::ExprId{},
                                                         info.fn_module, info.fn->id, info.fn_args);
                            if (!existed) {
                                const auto *instance = instantiations->at(instance_index);
                                if (instance != nullptr)
                                    predeclareInstantiation(instance->module, *instance);
                            }
                            (void)fn_sema;
                        }
                    }
                }
                if (instance_index != ~size_t{0}) {
                    const auto *instance = instantiations->at(instance_index);
                    for (const auto &function : functions_) {
                        if (function.instance != nullptr && function.module != nullptr &&
                            function.module->key == info.fn_module &&
                            function.instance->decl == info.fn->id && instance != nullptr &&
                            function.instance->args == instance->args) {
                            slot_sym = function.sym_id;
                            break;
                        }
                    }
                }
            }
            if (slot_sym == symbols::kInvalidSym) {
                if (const auto *fn_index = function_index_by_key_.get(key))
                    slot_sym = functions_[*fn_index].sym_id;
            }
            if (slot_sym != symbols::kInvalidSym)
                added.slots.push(slot_sym);
        }
        vtable = &added;
    }

    hir::HirMakeDyn make;
    make.value       = value;
    make.source_type = lowerType(concrete_sema);
    make.dyn_type    = target_hir;
    make.vtable_name = vtable_id;
    return addExpr(std::move(make));
}

hir::HirExprId HirLowerModern::lowerCoerceToOpaque(sema::modern::TypeId target,
                                                   frontend::ExprId expression,
                                                   hir::HirExprId value) {
    const types::TypeId target_hir = lowerType(target);
    if (value == hir::kInvalidHirExpr || !expression || current_module_ == nullptr ||
        current_types_ == nullptr || types_.kindOf(target_hir) != types::TypeKind::OpaqueTagged)
        return value;

    const auto *source_id = current_types_->opaqueSourceTypes.get(expression.value);
    if (source_id == nullptr)
        return value;
    const sema::modern::TypeId source_sema =
        current_instantiation_ != nullptr && current_instance_ != nullptr
            ? current_instantiation_->substituteType(*source_id, current_instance_->args)
            : *source_id;
    const types::TypeId source_type = lowerType(source_sema);
    if (source_type == types::kErrorType || source_type == types::kInvalidType)
        return value;

    hir::HirMakeOpaque make;
    make.value       = value;
    make.source_type = source_type;
    make.opaque_type = target_hir;
    make.type_id     = stableConcreteTypeId(source_type);
    return addExpr(std::move(make));
}

hir::HirExprId HirLowerModern::lowerSliceRange(const frontend::Expression &expr,
                                               const types::TypeId type) {
    if (expr.operands.size() < 3U)
        return hir::kInvalidHirExpr;
    const auto object = lowerExpr(expr.operands[0]);
    const auto lo     = lowerExpr(expr.operands[1]);
    const auto hi     = lowerExpr(expr.operands[2]);
    if (object == hir::kInvalidHirExpr || lo == hir::kInvalidHirExpr ||
        hi == hir::kInvalidHirExpr) {
        return hir::kInvalidHirExpr;
    }

    const auto object_type = typeOfExpr(expr.operands[0]);
    const auto *optional   = std::get_if<types::TypeOptional>(&types_.lookup(type));
    const auto slice_type  = optional != nullptr ? optional->inner : type;
    const auto bound_type  = typeOfExpr(expr.operands[1]);
    hir::HirMakeSlice slice;
    slice.object                           = object;
    slice.lo                               = lo;
    slice.hi                               = hi;
    slice.type                             = slice_type;
    slice.object_type                      = object_type;
    slice.bound_type                       = bound_type;
    slice.is_array                         = types_.kindOf(object_type) == types::TypeKind::Array;
    const sema::modern::TypeId sema_object = semaTypeOfExpr(expr.operands[0]);
    const auto *sema_optional =
        sema_.typeTable().optional(sema_.typeTable().stripQualifiers(sema_object));
    slice.is_pointer =
        types_.kindOf(object_type) == types::TypeKind::Ptr ||
        (sema_optional != nullptr && sema_.typeTable().pointer(sema_.typeTable().stripQualifiers(
                                         sema_optional->inner)) != nullptr);
    slice.checked = false;
    if (optional == nullptr || expr.is_raw)
        return addExpr(std::move(slice));

    // Evaluate the object and both bounds once, then branch on the dynamic checks.
    const auto object_slot = next_slot_++;
    const auto lo_slot     = next_slot_++;
    const auto hi_slot     = next_slot_++;
    const auto result_slot = next_slot_++;
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(object_slot, object_type));
    current_fn_->blocks[current_block_].insts.push(emitSlotStore(object_slot, object));
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(
        lo_slot,
        types_.kindOf(bound_type) == types::TypeKind::Int ? bound_type : types::kErrorType));
    current_fn_->blocks[current_block_].insts.push(emitSlotStore(lo_slot, lo));
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(
        hi_slot,
        types_.kindOf(bound_type) == types::TypeKind::Int ? bound_type : types::kErrorType));
    current_fn_->blocks[current_block_].insts.push(emitSlotStore(hi_slot, hi));
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(result_slot, type));

    const auto bound_load = emitSlotLoad(lo_slot, bound_type);
    const auto hi_load    = emitSlotLoad(hi_slot, bound_type);
    hir::HirLiteral zero;
    zero.type          = bound_type;
    zero.i             = 0;
    hir::HirExprId len = hir::kInvalidHirExpr;
    const auto *array  = std::get_if<types::TypeArray>(&types_.lookup(object_type));
    if (array != nullptr) {
        hir::HirLiteral len_lit;
        len_lit.type = bound_type;
        len_lit.i    = static_cast<int64_t>(array->count);
        len          = addExpr(std::move(len_lit));
    } else {
        const auto loaded = emitSlotLoad(object_slot, object_type);
        auto len_i64 =
            addExpr(hir::HirField{loaded, 1U, types_.internInt(types::IntWidth::I64), object_type});
        const auto len64 = types_.internInt(types::IntWidth::I64);
        if (types_.kindOf(bound_type) != types::TypeKind::Int) {
            len = hir::kInvalidHirExpr;
        } else if (bound_type != len64) {
            len = addExpr(hir::HirCast{len_i64, len64, bound_type});
        } else {
            len = len_i64;
        }
    }
    if (len == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;

    hir::HirBinary lo_ge_zero;
    lo_ge_zero.lhs           = bound_load;
    lo_ge_zero.rhs           = addExpr(std::move(zero));
    lo_ge_zero.op            = hir::HirBinaryOp::Ge;
    lo_ge_zero.type          = types::kBoolType;
    lo_ge_zero.operand_type  = bound_type;
    const auto lo_ge_zero_id = addExpr(std::move(lo_ge_zero));

    hir::HirBinary hi_le_len;
    hi_le_len.lhs           = hi_load;
    hi_le_len.rhs           = len;
    hi_le_len.op            = hir::HirBinaryOp::Le;
    hi_le_len.type          = types::kBoolType;
    hi_le_len.operand_type  = bound_type;
    const auto hi_le_len_id = addExpr(std::move(hi_le_len));

    hir::HirBinary lo_le_hi;
    lo_le_hi.lhs           = bound_load;
    lo_le_hi.rhs           = hi_load;
    lo_le_hi.op            = hir::HirBinaryOp::Le;
    lo_le_hi.type          = types::kBoolType;
    lo_le_hi.operand_type  = bound_type;
    const auto lo_le_hi_id = addExpr(std::move(lo_le_hi));

    hir::HirBinary first_and;
    first_and.lhs           = lo_ge_zero_id;
    first_and.rhs           = hi_le_len_id;
    first_and.op            = hir::HirBinaryOp::And;
    first_and.type          = types::kBoolType;
    first_and.operand_type  = types::kBoolType;
    const auto first_and_id = addExpr(std::move(first_and));

    hir::HirBinary all_ok;
    all_ok.lhs          = first_and_id;
    all_ok.rhs          = lo_le_hi_id;
    all_ok.op           = hir::HirBinaryOp::And;
    all_ok.type         = types::kBoolType;
    all_ok.operand_type = types::kBoolType;

    const auto some_block  = newBlock();
    const auto none_block  = newBlock();
    const auto merge_block = newBlock();
    hir::HirBranch branch;
    branch.cond       = addExpr(std::move(all_ok));
    branch.then_block = static_cast<hir::HirDeclId>(some_block);
    branch.else_block = static_cast<hir::HirDeclId>(none_block);
    setTerminator(addExpr(std::move(branch)));

    setCurrentBlock(some_block);
    current_fn_->blocks[some_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    {
        hir::HirMakeSlice ok_slice;
        ok_slice.object      = emitSlotLoad(object_slot, object_type);
        ok_slice.lo          = emitSlotLoad(lo_slot, bound_type);
        ok_slice.hi          = emitSlotLoad(hi_slot, bound_type);
        ok_slice.type        = slice_type;
        ok_slice.object_type = object_type;
        ok_slice.bound_type  = bound_type;
        ok_slice.is_array    = slice.is_array;
        ok_slice.is_pointer  = slice.is_pointer;
        ok_slice.checked     = false;
        hir::HirMakeSome some;
        some.type  = type;
        some.value = addExpr(std::move(ok_slice));
        current_fn_->blocks[some_block].insts.push(
            emitSlotStore(result_slot, addExpr(std::move(some))));
        emitJump(merge_block);
    }

    setCurrentBlock(none_block);
    current_fn_->blocks[none_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    {
        hir::HirMakeNone none;
        none.type = type;
        current_fn_->blocks[none_block].insts.push(
            emitSlotStore(result_slot, addExpr(std::move(none))));
        emitJump(merge_block);
    }

    setCurrentBlock(merge_block);
    current_fn_->blocks[merge_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    return emitSlotLoad(result_slot, type);
}

memory::Optional<int64_t> HirLowerModern::enumVariantValue(frontend::ExprId operand,
                                                           std::string_view variant) {
    if (!operand || current_module_ == nullptr || current_module_->frontend == nullptr ||
        operand.value > current_module_->frontend->expressions().size()) {
        return {};
    }
    const auto &op = current_module_->frontend->expressions()[operand.value - 1U];
    if (op.kind != frontend::ExprKind::Name)
        return {};
    const auto *resolved = findResolvedExpr(operand);
    if (resolved == nullptr || !resolved->declaration)
        return {};
    const auto *decl = findDecl(*current_module_, resolved->declaration);
    if (decl == nullptr || decl->kind != frontend::DeclKind::Enum)
        return {};
    // Enum variants are constants, but generic enum templates lower through a
    // concrete instance. Resolve the discriminant from the expression's actual
    // sema type (`Status<i32>`) instead of the template declaration.
    const auto sema_enum_type = sema_.typeTable().stripQualifiers(semaTypeOfExpr(operand));
    const auto *et            = sema_.typeTable().enum_type(sema_enum_type);
    if (et == nullptr)
        return {};
    for (size_t i = 0; i < et->variant_names.size(); ++i) {
        if (et->variant_names[i] == variant)
            return et->discriminants[i];
    }
    return {};
}

hir::HirExprId HirLowerModern::lowerField(const frontend::Expression &expr,
                                          const types::TypeId type) {
    if (expr.operands.empty())
        return hir::kInvalidHirExpr;
    if (current_types_ != nullptr) {
        if (const auto *base = current_types_->traitQualifiedReceiverBase.get(expr.id.value))
            return lowerExpr(frontend::ExprId{*base});
    }
    // `console.println` where `console` is an import alias: the field expression resolves
    // to the imported symbol, so emit the same HirVar a plain name would produce and never
    // lower the alias base (which would fail to resolve 'console').
    if (const auto *resolved = findResolvedExpr(expr.id);
        resolved != nullptr && resolved->kind == session::ResolutionKind::Import) {
        const frontend::Declaration *decl = resolvedFunctionDecl(*resolved);
        if (decl != nullptr) {
            hir::HirVar var;
            var.name    = interner_.intern(decl->name);
            var.version = 0;
            return addExpr(std::move(var));
        }
        if (resolved->foreignFunction != nullptr) {
            hir::HirVar var;
            var.name    = interner_.intern(resolved->foreignFunction->linkageName);
            var.version = 0;
            return addExpr(std::move(var));
        }
        return hir::kInvalidHirExpr;
    }
    // `std.io.console.println(...)`, `std.counter.Counter`-style chains, and
    // other multi-segment module paths have intermediate Field nodes that
    // resolve as ModuleAlias. Those nodes carry no value; the final member is
    // the Import binding above, so intervening aliases must not lower.
    if (const auto *resolved = findResolvedExpr(expr.id);
        resolved != nullptr && resolved->kind == session::ResolutionKind::ModuleAlias) {
        const frontend::Expression *chain = &expr;
        while (chain->kind == frontend::ExprKind::Field && !chain->operands.empty()) {
            const auto &base =
                current_module_->frontend->expressions()[chain->operands[0].value - 1U];
            if (const auto *base_resolved = findResolvedExpr(chain->operands[0]);
                base_resolved != nullptr &&
                base_resolved->kind == session::ResolutionKind::Import) {
                const frontend::Declaration *decl = resolvedFunctionDecl(*base_resolved);
                if (decl != nullptr) {
                    hir::HirVar var;
                    var.name    = interner_.intern(decl->name);
                    var.version = 0;
                    return addExpr(std::move(var));
                }
                return hir::kInvalidHirExpr;
            }
            chain = &base;
        }
    }
    // `Color.Green` resolves to an enum variant constant, not a struct field read.
    if (const auto variant = enumVariantValue(expr.operands[0], expr.text))
        return addExpr(hir::HirEnumValue{*variant, type});
    auto object      = lowerExpr(expr.operands[0]);
    auto object_type = typeOfExpr(expr.operands[0]);
    if (object == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    // Resolve the sema struct type to find the field index by name
    auto sema_type = sema_.typeTable().stripQualifiers(semaTypeOfExpr(expr.operands[0]));
    // `self.field` / `p.field` auto-derefs an implicit receiver or borrow
    // parameter in sema; mirror it here so the field access still lowers to a
    // loaded value.
    if (const auto *sem_ptr = sema_.typeTable().pointer(sema_type)) {
        const auto struct_type = lowerType(sema_.typeTable().stripQualifiers(sem_ptr->pointee));
        object      = addExpr(hir::HirUnary{hir::HirUnaryOp::Deref, object, struct_type});
        object_type = struct_type;
        sema_type   = sema_.typeTable().stripQualifiers(sem_ptr->pointee);
    }
    const int idx = sema_.typeTable().fieldIndex(sema_type, expr.text);
    if (idx >= 0)
        return addExpr(hir::HirField{object, static_cast<uint32_t>(idx), type, object_type});
    if (sema_.typeTable().kindOf(sema_type) == sema::modern::TypeKind::Pack) {
        const auto *pack = sema_.typeTable().pack(sema_type);
        if (pack != nullptr) {
            for (size_t index = 0; index < pack->names.size(); ++index) {
                if (pack->names[index] != expr.text)
                    continue;
                return addExpr(
                    hir::HirField{object, static_cast<uint32_t>(index), type, object_type});
            }
        }
    }
    if (current_module_ != nullptr &&
        sema_.typeTable().kindOf(sema_type) == sema::modern::TypeKind::GenericParam) {
        const auto *module_sema = sema_.findModuleSema(current_module_->key);
        if (module_sema != nullptr) {
            for (const TypeId bound : module_sema->boundsForGenericParam(sema_type)) {
                if (!module_sema->isInterfaceType(bound))
                    continue;
                const auto *trait_ty = sema_.typeTable().trait(module_sema->resolve(bound));
                if (trait_ty == nullptr)
                    continue;
                const auto *iface =
                    module_sema->findDeclNamed(trait_ty->name, frontend::DeclKind::Interface);
                if (iface == nullptr)
                    continue;
                for (size_t field = 0; field < iface->parameters.size(); ++field) {
                    if (iface->parameters[field].name != expr.text)
                        continue;
                    return addExpr(
                        hir::HirField{object, static_cast<uint32_t>(field), type, object_type});
                }
            }
        }
    }
    return hir::kInvalidHirExpr;
}

hir::HirExprId HirLowerModern::lowerArrow(const frontend::Expression &expr,
                                          const types::TypeId type) {
    if (expr.operands.empty())
        return hir::kInvalidHirExpr;
    const auto ptr = lowerExpr(expr.operands[0]);
    if (ptr == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    // Deref the pointer first
    auto sema_ptr_type = sema_.typeTable().stripQualifiers(semaTypeOfExpr(expr.operands[0]));
    // Match sema: `?*T` behaves as `*T` here because None is represented as nullptr.
    if (const auto *opt = sema_.typeTable().optional(sema_ptr_type))
        sema_ptr_type = sema_.typeTable().stripQualifiers(opt->inner);
    const auto *pt = sema_.typeTable().pointer(sema_ptr_type);
    if (pt == nullptr)
        return hir::kInvalidHirExpr;
    const auto sema_struct = sema_.typeTable().stripQualifiers(pt->pointee);
    const auto struct_type = lowerType(sema_struct);
    const auto deref       = addExpr(hir::HirUnary{hir::HirUnaryOp::Deref, ptr, struct_type});
    const int idx          = sema_.typeTable().fieldIndex(sema_struct, expr.text);
    if (idx < 0)
        return hir::kInvalidHirExpr;
    return addExpr(hir::HirField{deref, static_cast<uint32_t>(idx), type, struct_type});
}

hir::HirExprId HirLowerModern::lowerStructLiteral(const frontend::Expression &expr,
                                                  const types::TypeId type) {
    if (types_.kindOf(type) == types::TypeKind::Union) {
        if (expr.operands.size() != 1U)
            return hir::kInvalidHirExpr;
        const auto value = lowerExpr(expr.operands[0]);
        if (value == hir::kInvalidHirExpr)
            return hir::kInvalidHirExpr;
        const auto from = typeOfExpr(expr.operands[0]);
        hir::HirUnionCast cast;
        cast.value        = value;
        cast.from         = from;
        cast.to           = type;
        cast.member_index = taggedMemberIndex(type, from);
        cast.checked      = false;
        return addExpr(std::move(cast));
    }
    hir::HirStructLiteral lit(arena_);
    lit.type = type;
    const size_t field_count =
        types_.kindOf(type) == types::TypeKind::Struct ? types_.fieldCount(type) : 0U;
    // Values are emitted in declaration order, not in the order the literal was written.
    std::vector<hir::HirExprId> ordered(field_count == 0U ? expr.operands.size() : field_count,
                                        hir::kInvalidHirExpr);
    for (size_t i = 0; i < expr.operands.size(); ++i) {
        auto value = lowerExpr(expr.operands[i]);
        if (value == hir::kInvalidHirExpr)
            continue;
        size_t slot_index = i;
        if (field_count != 0U && i < expr.field_names.size()) {
            slot_index = types_.fieldIndex(type, expr.field_names[i]);
            if (slot_index >= field_count)
                continue;
        }
        if (field_count != 0U) {
            // A bare `T` value assigned to a `?T` field must be wrapped in Some.
            const auto field_type = types_.getField(type, slot_index).type;
            const auto value_type = typeOfExpr(expr.operands[i]);
            if (types_.kindOf(field_type) == types::TypeKind::Optional &&
                types_.kindOf(value_type) != types::TypeKind::Optional) {
                value = lowerCoerceToOptional(field_type, value);
            }
            value = lowerCoerceToTarget(field_type, expr.operands[i], value);
        }
        if (slot_index < ordered.size())
            ordered[slot_index] = value;
    }
    // Fill slots left empty by a `_` placeholder or an omitted field with the
    // struct declaration's default; slots without a default stay zero-initialized.
    if (field_count != 0U) {
        for (size_t slot_index = 0; slot_index < field_count; ++slot_index) {
            if (ordered[slot_index] != hir::kInvalidHirExpr)
                continue;
            const auto default_id = lowerFieldDefault(expr.text, slot_index);
            if (!default_id)
                continue;
            const auto default_value = lowerExpr(default_id);
            if (default_value == hir::kInvalidHirExpr)
                continue;
            const auto field_type = types_.getField(type, slot_index).type;
            ordered[slot_index] =
                types_.kindOf(field_type) == types::TypeKind::Optional &&
                        types_.kindOf(typeOfExpr(default_id)) != types::TypeKind::Optional
                    ? lowerCoerceToOptional(field_type, default_value)
                    : lowerCoerceToTarget(field_type, default_id, default_value);
        }
    }
    // Keep every slot (missing ones are zero at codegen); the array is index-aligned.
    for (const auto value : ordered)
        lit.values.push(value);
    return addExpr(std::move(lit));
}

hir::HirExprId HirLowerModern::lowerPackLiteral(const frontend::Expression &expr,
                                                const types::TypeId type) {
    hir::HirStructLiteral lit(arena_);
    lit.type = type;
    for (const auto operand : expr.operands) {
        const auto value = lowerExpr(operand);
        if (value == hir::kInvalidHirExpr)
            return hir::kInvalidHirExpr;
        lit.values.push(value);
    }
    return addExpr(std::move(lit));
}

hir::HirExprId HirLowerModern::lowerArrayLiteral(const frontend::Expression &expr,
                                                 const types::TypeId type) {
    hir::HirArrayLiteral lit(arena_);
    lit.type                 = type;
    const auto *target_array = std::get_if<types::TypeArray>(&types_.lookup(type));
    for (size_t index = 0; index < expr.operands.size(); ++index) {
        const frontend::ExprId operand = expr.operands[index];
        auto value                     = lowerExpr(operand);
        if (value == hir::kInvalidHirExpr)
            return hir::kInvalidHirExpr;
        if (target_array != nullptr)
            value = lowerCoerceToTarget(target_array->elem, operand, value);
        lit.elements.push(value);
    }
    return addExpr(std::move(lit));
}

frontend::ExprId HirLowerModern::lowerFieldDefault(std::string_view struct_name,
                                                   size_t field_index) const noexcept {
    if (current_module_ == nullptr || current_module_->frontend == nullptr)
        return {};
    const auto &decls = current_module_->frontend->declarations();
    for (const auto &decl : decls) {
        if (decl.kind != frontend::DeclKind::Struct || decl.name != struct_name)
            continue;
        if (field_index < decl.parameters.size())
            return decl.parameters[field_index].defaultValue;
        break;
    }
    return {};
}

hir::HirExprId HirLowerModern::lowerCoerceToOptional(types::TypeId target, hir::HirExprId value) {
    if (value == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    // This is a simple wrapper: wrap any T value into ?T (Some)
    hir::HirMakeSome some;
    some.type  = target;
    some.value = value;
    return addExpr(std::move(some));
}

hir::HirExprId HirLowerModern::lowerMakeNone(types::TypeId target) {
    hir::HirMakeNone make_none;
    make_none.type = target;
    return addExpr(std::move(make_none));
}

hir::HirExprId HirLowerModern::lowerCoerceToOptionalDepth(types::TypeId target,
                                                          sema::modern::TypeId target_sema,
                                                          sema::modern::TypeId source_sema,
                                                          hir::HirExprId value) {
    if (value == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    if (target == types::kInvalidType || target == types::kErrorType ||
        target_sema == sema::modern::kInvalidTypeId || source_sema == sema::modern::kInvalidTypeId)
        return value;

    // Apply the missing optional layers one at a time. Each layer is a `Some`
    // around the previous value, so codegen sees the exact LLVM aggregate or
    // pointer representation of each nested optional type.
    auto current      = value;
    auto current_sema = source_sema;
    const auto *module_sema =
        current_module_ != nullptr ? sema_.findModuleSema(current_module_->key) : nullptr;
    while (sema_.typeTable().kindOf(sema_.typeTable().stripQualifiers(target_sema)) ==
               sema::modern::TypeKind::Optional &&
           module_sema != nullptr && !module_sema->sameType(target_sema, current_sema) &&
           module_sema->coercesTo(target_sema, current_sema)) {
        const auto *target_opt =
            sema_.typeTable().optional(sema_.typeTable().stripQualifiers(target_sema));
        if (target_opt == nullptr)
            return value;

        const bool source_is_optional =
            sema_.typeTable().kindOf(sema_.typeTable().stripQualifiers(current_sema)) ==
            sema::modern::TypeKind::Optional;
        // The first wrap from a bare `T` creates the nearest optional layer.
        // Once the value is already optional, the remaining wraps apply the
        // full outer optional type.
        const auto layer_sema =
            (source_is_optional ||
             sema_.typeTable().kindOf(target_opt->inner) != sema::modern::TypeKind::Optional)
                ? target_sema
                : target_opt->inner;
        const auto layer_hir = lowerType(layer_sema);
        if (layer_hir == types::kErrorType || layer_hir == types::kInvalidType)
            return value;

        hir::HirMakeSome some;
        some.type    = layer_hir;
        some.value   = current;
        current      = addExpr(std::move(some));
        current_sema = layer_sema;
        if (source_is_optional)
            target_sema = target_opt->inner;
    }
    return current;
}

} // namespace modern
} // namespace zith::sema
