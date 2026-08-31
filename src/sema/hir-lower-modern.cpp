#include "sema/hir-lower-modern.hpp"

#include "common/overloaded.hpp"
#include "diagnostics/error-codes.hpp"
#include "sema/hir-lower-utils.hpp"
#include "sema/nra-facts.hpp"
#include "sema/op-mapping.hpp"
#include "support/debug-print.hpp"
#include "support/int-literal.hpp"

#include "types/type-kind.hpp"

#include <algorithm>
#include <cstdlib>
#include <span>
#include <string>

namespace zith::sema {
namespace modern {

HirLowerModern::HirLowerModern(memory::Arena &arena, diagnostics::DiagnosticEngine &diags,
                               const session::CompilationSnapshot &snapshot, SemaPipeline &sema,
                               types::TypeIntern &types, memory::StringInterner &interner,
                               const NraFacts *nra)
    : arena_(arena), diags_(diags), snapshot_(snapshot), sema_(sema), types_(types),
      interner_(interner), nra_(nra), hir_(arena), lowered_types_(), function_index_by_key_() {}

bool HirLowerModern::run() {
    return predeclareGlobalConsts() && predeclareFunctions() && lowerFunctionBodies() &&
           !diags_.hasErrors();
}

bool HirLowerModern::predeclareGlobalConsts() {
    for (const auto &module_ptr : snapshot_.modules()) {
        const auto &module = *module_ptr;
        auto *module_sema  = sema_.findModuleSema(module.key);
        if (module_sema == nullptr)
            continue;

        for (const auto &edge : snapshot_.importGraph()) {
            if (edge.importer != module.key ||
                edge.targetKind != session::ImportTargetKind::CHeader || edge.cHeader == nullptr ||
                !edge.error.empty())
                continue;
            const auto name_space = moduleNamespace(module.key, snapshot_.cacheKey());
            for (const auto &constant : edge.cHeader->constants) {
                const auto key = interner_.intern(constant.name);
                if (global_const_by_name_.get(key) != nullptr)
                    continue;

                std::string global_name = "_zith_";
                if (!name_space.empty()) {
                    global_name += name_space;
                    global_name += '.';
                }
                global_name += constant.name;

                auto &global = hir_.addGlobalConst();
                global.name  = interner_.intern(global_name);
                switch (constant.kind) {
                case cinterop::ConstantKind::Integer:
                    global.type =
                        types_.internInt(sema::mapIntegerWidth(constant.bits, constant.isSigned));
                    break;
                case cinterop::ConstantKind::Float:
                    global.type = types_.internFloat(sema::mapFloatWidth(constant.bits));
                    break;
                case cinterop::ConstantKind::Bool:
                    global.type = types::kBoolType;
                    break;
                case cinterop::ConstantKind::Char:
                    global.type = types::kCharType;
                    break;
                }
                hir::HirLiteral literal;
                literal.type = global.type;
                switch (constant.kind) {
                case cinterop::ConstantKind::Integer:
                case cinterop::ConstantKind::Char:
                    literal.i =
                        static_cast<int64_t>(constant.kind == cinterop::ConstantKind::Char
                                                 ? static_cast<unsigned char>(constant.charValue)
                                                 : constant.integerValue);
                    break;
                case cinterop::ConstantKind::Float:
                    literal.f = constant.floatValue;
                    break;
                case cinterop::ConstantKind::Bool:
                    literal.b = constant.boolValue;
                    break;
                }
                global.init = addExpr(std::move(literal));
                global_const_by_name_.insert(key, global.name);
            }
        }

        for (const auto &decl : module.frontend->declarations()) {
            if (decl.kind != frontend::DeclKind::Variable ||
                decl.bindingKind != frontend::BindingKind::Const || decl.name.empty())
                continue;

            const auto key = internFunctionKey(interner_, module.key, decl.id);
            if (global_const_by_key_.get(key) != nullptr)
                continue;

            const auto name_space   = moduleNamespace(module.key, snapshot_.cacheKey());
            std::string global_name = "_zith_";
            if (!name_space.empty()) {
                global_name += name_space;
                global_name += '.';
            }
            global_name += decl.name;

            auto &global = hir_.addGlobalConst();
            global.name  = interner_.intern(global_name);
            global.type  = lowerType(module_sema->typeOfDecl(decl.id));
            if (decl.initializer) {
                current_module_     = &module;
                current_resolution_ = snapshot_.findResolution(module.key);
                current_types_      = sema_.findTypedMap(module.key);
                global.init         = lowerExpr(decl.initializer);
                current_module_     = nullptr;
                current_resolution_ = nullptr;
                current_types_      = nullptr;
            }
            global_const_by_key_.insert(key, global.name);
        }
    }
    return !diags_.hasErrors();
}

bool HirLowerModern::predeclareFunctions() {
    for (const auto &module_ptr : snapshot_.modules()) {
        const auto &module = *module_ptr;
        auto *module_sema  = sema_.findModuleSema(module.key);
        if (module_sema == nullptr)
            continue;

        for (const auto &decl : module.frontend->declarations()) {
            if (decl.kind != frontend::DeclKind::Function || decl.name.empty())
                continue;
            if (!decl.genericParams.empty())
                continue;

            // Linkage name: `<namespace>.<Owner>.<name>(<param types>)`.  `extern fn`
            // (fixed C ABI) and `main` (needed verbatim by the linker) keep the
            // source name; `= extern <symbol>` aliases to the C linker symbol;
            // everything else is qualified so overloads and same-named
            // functions in different modules get distinct symbols.
            std::string fn_name = decl.externalSymbol.empty() ? decl.name : decl.externalSymbol;
            if (!decl.isExtern && decl.name != "main") {
                if (decl.externalSymbol.empty()) {
                    const auto name_space = moduleNamespace(module.key, snapshot_.cacheKey());
                    std::string qualified;
                    if (!name_space.empty())
                        qualified = name_space + ".";
                    if (!decl.ownerName.empty())
                        qualified += decl.ownerName + ".";
                    if (!decl.parentName.empty())
                        qualified += decl.parentName + "$local.";
                    qualified += decl.name;
                    qualified += frontend::functionSignature(*module.frontend, decl);
                    fn_name = std::move(qualified);
                }
            } else if (!decl.ownerName.empty() && decl.externalSymbol.empty()) {
                fn_name = decl.ownerName + "." + fn_name;
            }

            auto &hir_fn      = hir_.addFn(interner_.intern(fn_name));
            hir_fn.sym_id     = static_cast<symbols::SymId>(functions_.size() + next_sym_id_);
            hir_fn.decl_id    = static_cast<ast::DeclId>(decl.id.value);
            hir_fn.fnSpan     = memory::Span{0, decl.span.start, decl.span.end};
            hir_fn.isVariadic = decl.isVariadic;
            hir_fn.isState    = decl.functionKind == frontend::FunctionKind::State;
            if (!decl.parameters.empty() && decl.parameters.back().isVariadicSlice)
                hir_fn.variadicSliceParam = decl.parameters.size() - 1U;

            bool is_main_void  = false;
            const auto fn_type = module_sema->typeOfDecl(decl.id);
            if (const auto *fn = sema_.typeTable().function(fn_type)) {
                hir_fn.return_type = lowerType(fn->result);
                is_main_void = decl.name == "main" && !decl.isExtern && decl.ownerName.empty() &&
                               decl.visibility != frontend::Visibility::Public &&
                               module.key == snapshot_.rootModuleKey() &&
                               hir_fn.return_type == types::kVoidType;
                if (is_main_void)
                    hir_fn.return_type = types_.internInt(types::IntWidth::I32);
                if (hir_fn.isState) {
                    hir_fn.machineId  = decl.parentScope ? module_sema->stateMachineIdOf(decl)
                                                         : module_sema->stateMachineIdFor(decl);
                    hir_fn.usesTailCC = true;
                    hir_fn.machineReturnType = hir_fn.return_type;
                }
                for (size_t index = 0; index < decl.parameters.size(); ++index) {
                    const auto &parameter = decl.parameters[index];
                    const auto param_type = index < fn->params.size() ? lowerType(fn->params[index])
                                                                      : types::kErrorType;
                    hir_fn.params.push(param_type);
                    hir_fn.param_names.push(interner_.intern(parameter.name));
                }
            } else {
                hir_fn.return_type = types::kVoidType;
                if (hir_fn.isState) {
                    hir_fn.machineId  = decl.parentScope ? module_sema->stateMachineIdOf(decl)
                                                         : module_sema->stateMachineIdFor(decl);
                    hir_fn.usesTailCC = true;
                    hir_fn.machineReturnType = types::kVoidType;
                }
                for (const auto &parameter : decl.parameters) {
                    hir_fn.params.push(types::kErrorType);
                    hir_fn.param_names.push(interner_.intern(parameter.name));
                }
            }

            const auto key = internFunctionKey(interner_, module.key, decl.id);
            function_index_by_key_.insert(key, hir_.getFnCount() - 1U);
            functions_.push_back({key, module_ptr.get(), &decl, nullptr, nullptr, hir_fn.sym_id,
                                  hir_.getFnCount() - 1U, is_main_void});
        }
    }
    if (const auto *instantiations = sema_.instantiations(); instantiations != nullptr) {
        for (size_t index = 0; index < instantiations->instanceCount(); ++index) {
            const auto *instance = instantiations->at(index);
            if (instance != nullptr)
                predeclareInstantiation(instance->module, *instance);
        }
    }
    for (const auto &header : snapshot_.cHeaders()) {
        if (header == nullptr)
            continue;
        for (const auto &foreign : header->functions) {
            auto &hir_fn       = hir_.addFn(interner_.intern(foreign.linkageName));
            hir_fn.sym_id      = static_cast<symbols::SymId>(functions_.size() + next_sym_id_);
            hir_fn.return_type = lowerForeignType(foreign.result);
            hir_fn.isVariadic  = foreign.isVariadic;
            for (const auto &parameter : foreign.parameters) {
                hir_fn.params.push(lowerForeignType(parameter));
                hir_fn.param_names.push({});
            }
            functions_.push_back(
                {0, nullptr, nullptr, &foreign, nullptr, hir_fn.sym_id, hir_.getFnCount() - 1U});
        }
    }
    return true;
}

bool HirLowerModern::lowerFunctionBodies() {
    for (auto &function : functions_) {
        if (function.decl != nullptr && function.decl->body && !lowerFunctionBody(function))
            return false;
    }
    return !diags_.hasErrors();
}

void HirLowerModern::predeclareInstantiation(session::ModuleKey module_key,
                                             const comptime::InstantiationInstance &instance) {
    const auto *module      = snapshot_.findModule(module_key);
    const auto *module_sema = sema_.findModuleSema(module_key);
    if (module == nullptr)
        return;
    const auto *decl = findDecl(*module, instance.decl);
    if (module_sema == nullptr || decl == nullptr)
        return;

    const bool has_external_symbol = !decl->externalSymbol.empty();
    auto &hir_fn =
        hir_.addFn(interner_.intern(has_external_symbol ? decl->externalSymbol : instance.mangled));
    hir_fn.sym_id     = static_cast<symbols::SymId>(functions_.size() + next_sym_id_);
    hir_fn.decl_id    = static_cast<ast::DeclId>(decl->id.value);
    hir_fn.fnSpan     = memory::Span{0, decl->span.start, decl->span.end};
    hir_fn.isVariadic = decl->isVariadic;
    hir_fn.isState    = decl->functionKind == frontend::FunctionKind::State;
    if (!decl->parameters.empty() && decl->parameters.back().isVariadicSlice)
        hir_fn.variadicSliceParam = decl->parameters.size() - 1U;

    const auto template_type  = module_sema->typeOfDecl(decl->id);
    const auto *template_fn   = sema_.typeTable().function(template_type);
    const auto *instantiation = sema_.instantiations();
    const auto instance_type  = template_fn != nullptr && instantiation != nullptr
                                    ? instantiation->substituteFunction(*template_fn, instance.args)
                                    : kInvalidTypeId;
    const auto *fn            = sema_.typeTable().function(instance_type);
    if (fn != nullptr) {
        hir_fn.return_type = lowerType(fn->result);
        if (hir_fn.isState) {
            hir_fn.machineId         = module_sema->stateMachineIdOf(*decl);
            hir_fn.usesTailCC        = true;
            hir_fn.machineReturnType = hir_fn.return_type;
        }
        for (size_t index = 0; index < decl->parameters.size() && index < fn->params.size();
             ++index) {
            hir_fn.params.push(lowerType(fn->params[index]));
            hir_fn.param_names.push(interner_.intern(decl->parameters[index].name));
        }
    }

    const auto key = internFunctionKey(interner_, module_key, instance.decl);
    // The first lowered concrete symbol for a generic decl is the canonical
    // target; call-site bindings identify the exact instance, so the key also
    // needs to encode the type-argument tuple when multiple instances exist.
    if (!function_index_by_key_.contains(key))
        function_index_by_key_.insert(key, hir_.getFnCount() - 1U);
    functions_.push_back(
        {key, module, decl, nullptr, &instance, hir_fn.sym_id, hir_.getFnCount() - 1U});
}

bool HirLowerModern::lowerFunctionBody(FunctionInfo &info) {
    current_module_     = info.module;
    current_resolution_ = snapshot_.findResolution(info.module->key);
    current_types_      = sema_.findTypedMap(info.module->key);
    if (current_resolution_ == nullptr || current_types_ == nullptr) {
        diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                      "missing semantic data for module '" + info.module->key + "'", {});
        return false;
    }

    current_fn_                  = &hir_.getFn(info.hir_index);
    current_instantiation_       = info.instance != nullptr ? sema_.instantiations() : nullptr;
    current_instance_            = info.instance;
    current_fn_return_sema_type_ = sema::modern::kInvalidTypeId;
    current_fn_decl_             = info.decl;
    if (const auto *module_sema = sema_.findModuleSema(info.module->key);
        module_sema != nullptr && info.decl != nullptr) {
        sema::modern::TypeId fn_sema_type = module_sema->typeOfDecl(info.decl->id);
        if (const auto *fn = sema_.typeTable().function(fn_sema_type))
            current_fn_return_sema_type_ = fn->result;
    }
    info_decl_parent_scope_ = info.decl != nullptr ? info.decl->parentScope : frontend::ScopeId{};
    current_fn_is_state_ =
        info.decl != nullptr && info.decl->functionKind == frontend::FunctionKind::State;
    current_state_machine_id_ = current_fn_is_state_ ? current_fn_->machineId : 0;

    if (nra_ != nullptr && info.module != nullptr && info.decl != nullptr) {
        const auto *fn_fact = nra_->functionFact(info.module->key, info.decl->id);
        if (fn_fact != nullptr && fn_fact->hasResidual() && fn_fact->allReturnsParameter &&
            fn_fact->parameterIndex != ~0U) {
            auto &attrs          = hir_.attrs().fn(info.hir_index);
            attrs.returnConsumed = hir::HirConsumedState::NonConsumed;
            attrs.noAlias        = true;
        }
    }

    current_block_ = 0;
    next_slot_     = 0;
    loop_stack_.clear();
    narrowing_stack_.clear();
    local_slots_.clear();
    local_slots_.resize(1U);
    current_for_in_binding_stmt_  = {};
    current_for_in_binding_local_ = {};
    const bool is_main_void       = info.decl->name == "main" && info.forced_main_return_i32 &&
                              info.module != nullptr &&
                              info.module->key == snapshot_.rootModuleKey();
    current_main_void_ = is_main_void;

    current_fn_->blocks.emplace(arena_);
    current_fn_->blocks[0].insts = memory::DynArray<hir::HirExprId>(arena_);

    for (size_t index = 0; index < info.decl->parameters.size(); ++index) {
        const auto &parameter = info.decl->parameters[index];
        const auto slot       = localSlot(parameter.id);
        current_fn_->param_slots.push(slot);
        // The slot id is read by codegen from param_slots; publish attrs for
        // parameters even when the function body never names them, because the
        // ABI-level ownership decision depends on the parameter itself.
        if (nra_ != nullptr) {
            const auto *fact = nra_->localFact(parameter.id);
            if (fact != nullptr && fact->hasResidual()) {
                auto &attrs     = hir_.attrs().slot(slot);
                attrs.ownership = mapHirOwnership(fact->ownership);
                attrs.nonNull   = fact->nonNull;
            }
        }
        current_fn_->blocks[0].insts.push(emitSlotAlloca(slot, current_fn_->params[index]));

        hir::HirVar param_var;
        param_var.name        = current_fn_->param_names[index];
        param_var.version     = 0;
        const auto param_expr = addExpr(std::move(param_var));
        current_fn_->blocks[0].insts.push(emitSlotStore(slot, param_expr));
    }

    const auto body_expr = lowerExpr(info.decl->body);
    if (current_fn_->blocks[current_block_].terminator == hir::kInvalidHirExpr) {
        hir::HirRet ret;
        if (current_main_void_) {
            hir::HirLiteral zero;
            zero.type = types_.internInt(types::IntWidth::I32);
            zero.i    = 0;
            ret.value = addExpr(std::move(zero));
        } else if (current_fn_->return_type != types::kVoidType &&
                   body_expr != hir::kInvalidHirExpr) {
            if (types_.kindOf(current_fn_->return_type) == types::TypeKind::Optional &&
                info.decl->body) {
                const auto val_sema_type = semaTypeOfExpr(info.decl->body);
                if (current_fn_return_sema_type_ != sema::modern::kInvalidTypeId &&
                    val_sema_type != sema::modern::kInvalidTypeId)
                    ret.value = lowerCoerceToOptionalDepth(current_fn_->return_type,
                                                           current_fn_return_sema_type_,
                                                           val_sema_type, body_expr);
                else if (sema_.typeTable().kindOf(val_sema_type) != TypeKind::Optional)
                    ret.value = lowerCoerceToOptional(current_fn_->return_type, body_expr);
                else {
                    ret.value = body_expr;
                }
            } else {
                ret.value = body_expr;
            }
            if (current_fn_return_sema_type_ != sema::modern::kInvalidTypeId)
                ret.value = lowerCoerceToDyn(current_fn_return_sema_type_, info.decl->body,
                                             ret.value, current_fn_return_sema_type_);
            if (current_fn_return_sema_type_ != sema::modern::kInvalidTypeId)
                ret.value =
                    lowerCoerceToOpaque(current_fn_return_sema_type_, info.decl->body, ret.value);
            ret.value = lowerCoerceToTarget(current_fn_->return_type, info.decl->body, ret.value);
        }
        current_fn_->blocks[current_block_].terminator = addExpr(std::move(ret));
    }

    current_module_              = nullptr;
    current_resolution_          = nullptr;
    current_types_               = nullptr;
    current_instantiation_       = nullptr;
    current_instance_            = nullptr;
    current_fn_return_sema_type_ = sema::modern::kInvalidTypeId;
    info_decl_parent_scope_      = {};
    current_fn_                  = nullptr;
    current_fn_is_state_         = false;
    current_state_machine_id_    = 0;
    current_main_void_           = false;
    return true;
}

const frontend::Declaration *HirLowerModern::findDecl(const session::ModuleArtifact &module,
                                                      frontend::DeclId id) const noexcept {
    if (!id || module.frontend == nullptr || id.value > module.frontend->declarations().size())
        return nullptr;
    return &module.frontend->declarations()[id.value - 1U];
}

const PerModuleSema::CallTarget *
HirLowerModern::overloadTarget(frontend::ExprId callee) const noexcept {
    if (current_module_ == nullptr)
        return nullptr;
    const auto *module_sema = sema_.findModuleSema(current_module_->key);
    return module_sema == nullptr ? nullptr : module_sema->resolvedCallTarget(callee);
}

const frontend::Declaration *
HirLowerModern::methodDeclFromTarget(frontend::ExprId callee,
                                     const session::ModuleArtifact **module_out) const noexcept {
    const auto *target = overloadTarget(callee);
    if (target == nullptr)
        return nullptr;
    const auto *module = snapshot_.findModule(target->module);
    if (module == nullptr)
        return nullptr;
    const auto *decl = findDecl(*module, target->decl);
    if (decl == nullptr || decl->kind != frontend::DeclKind::Function)
        return nullptr;
    if (module_out != nullptr)
        *module_out = module;
    return decl;
}

const session::ResolvedName *HirLowerModern::findResolvedExpr(frontend::ExprId id) const noexcept {
    if (current_module_ == nullptr || current_resolution_ == nullptr || !id ||
        id.value > current_module_->frontend->expressions().size()) {
        return nullptr;
    }

    return session::lookupExprResolution(*current_resolution_, id);
}

const frontend::Declaration *
HirLowerModern::resolvedFunctionDecl(const session::ResolvedName &resolved,
                                     const session::ModuleArtifact **module_out) const noexcept {
    const session::ModuleArtifact *module = current_module_;
    frontend::DeclId decl                 = resolved.declaration;
    if (!resolved.target.module.empty()) {
        module = snapshot_.findModule(resolved.target.module);
        if (!decl && resolved.target.localSymbol)
            decl = frontend::DeclId{resolved.target.localSymbol.value};
    }
    if (module_out != nullptr)
        *module_out = module;
    if (module == nullptr)
        return nullptr;
    return findDecl(*module, decl);
}

symbols::SymId
HirLowerModern::resolvedFunctionSym(const session::ResolvedName &resolved) const noexcept {
    if (resolved.foreignFunction != nullptr) {
        for (const auto &function : functions_) {
            if (function.foreign == resolved.foreignFunction)
                return function.sym_id;
        }
        return symbols::kInvalidSym;
    }
    const session::ModuleArtifact *module = nullptr;
    const auto *decl                      = resolvedFunctionDecl(resolved, &module);
    if (decl == nullptr || module == nullptr)
        return symbols::kInvalidSym;
    const auto key = internFunctionKey(interner_, module->key, decl->id);
    if (const auto *function_index = function_index_by_key_.get(key))
        return functions_[*function_index].sym_id;
    return symbols::kInvalidSym;
}

hir::HirSlotId HirLowerModern::localSlot(frontend::LocalId id) {
    if (!id)
        return hir::kInvalidHirSlot;
    if (id.value >= local_slots_.size())
        local_slots_.resize(id.value + 1U, hir::kInvalidHirSlot);
    if (local_slots_[id.value] == hir::kInvalidHirSlot) {
        local_slots_[id.value] = next_slot_++;
        if (nra_ != nullptr) {
            const auto *fact = nra_->localFact(id);
            if (fact != nullptr && fact->hasResidual()) {
                auto &attrs     = hir_.attrs().slot(local_slots_[id.value]);
                attrs.ownership = mapHirOwnership(fact->ownership);
                attrs.nonNull   = fact->nonNull;
                attrs.consumed  = fact->knownAlive ? hir::HirConsumedState::NonConsumed
                                                   : hir::HirConsumedState::Consumed;
            }
        }
    }
    return local_slots_[id.value];
}

hir::HirExprId HirLowerModern::addExpr(hir::HirExpr expr) {
    return hir_.addExpr(std::move(expr));
}

hir::HirExprId HirLowerModern::emitSlotAlloca(const hir::HirSlotId slot, const types::TypeId type) {
    return addExpr(hir::HirSlotAlloca{slot, type});
}

hir::HirExprId HirLowerModern::emitSlotStore(const hir::HirSlotId slot,
                                             const hir::HirExprId value) {
    return addExpr(hir::HirSlotStore{slot, value});
}

hir::HirExprId HirLowerModern::emitSlotLoad(const hir::HirSlotId slot, const types::TypeId type) {
    return addExpr(hir::HirSlotLoad{slot, type});
}

size_t HirLowerModern::newBlock() {
    const auto block = current_fn_->blocks.size();
    current_fn_->blocks.emplace(arena_);
    return block;
}

void HirLowerModern::setCurrentBlock(const size_t block) {
    current_block_ = block;
}

void HirLowerModern::setTerminator(const hir::HirExprId term) {
    current_fn_->blocks[current_block_].terminator = term;
}

void HirLowerModern::emitJump(const size_t target) {
    hir::HirJump jump;
    jump.target = static_cast<hir::HirDeclId>(target);
    setTerminator(addExpr(std::move(jump)));
}

} // namespace modern
} // namespace zith::sema
