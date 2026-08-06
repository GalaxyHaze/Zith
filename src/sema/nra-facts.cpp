#include "sema/nra-facts.hpp"

#include "diagnostics/error-codes.hpp"

#include <algorithm>

namespace zith::sema::modern {

NraFacts::NraFacts(memory::Arena &, diagnostics::DiagnosticEngine &diagnostics,
                   const session::CompilationSnapshot &snapshot, const SemaPipeline &sema)
    : diagnostics_(diagnostics), snapshot_(snapshot), sema_(sema), local_facts_(), call_facts_(),
      narrowing_facts_(), current_condition_(), any_return_(false),
      all_returns_same_parameter(true), returned_parameter_(~0U), has_errors_(false) {}

bool NraFacts::run() {
    for (const auto &module_ptr : snapshot_.modules()) {
        const auto &module      = *module_ptr;
        const auto *module_sema = sema_.findModuleSema(module.key);
        const auto *typed       = sema_.findTypedMap(module.key);
        if (module_sema == nullptr || typed == nullptr || module.frontend == nullptr)
            continue;
        analyzeModule(module, *module_sema, *typed);
    }
    return !hasErrors();
}

void NraFacts::analyzeModule(const session::ModuleArtifact &module,
                             const PerModuleSema &module_sema, const TypedMap &typed) {
    const auto *frontend = module.frontend.get();
    if (frontend == nullptr)
        return;

    current_module_ = &module;
    current_sema_   = &module_sema;
    current_typed_  = &typed;
    collectNarrowing(*frontend);

    for (const auto &decl : frontend->declarations()) {
        if (decl.kind != frontend::DeclKind::Function)
            continue;

        current_key_ = module.key + "#" + std::to_string(decl.id.value);
        current_params_.clear();
        for (const auto &parameter : decl.parameters) {
            current_params_.push_back(parameter.id);
            NraLocalFact fact;
            const auto *type = typed.localTypes.get(parameter.id.value);
            if (type != nullptr) {
                fact.declaredType = *type;
                fact.ownership    = ownershipOfLocal(parameter.id);
            }
            local_facts_.insert(parameter.id.value, fact);
        }

        any_return_                = false;
        all_returns_same_parameter = true;
        returned_parameter_        = ~0U;
        walkFunction(decl);
        collectFunctionFact();
    }

    // Resolve call facts in a second walk, after every function's return-origin
    // fact exists in the same module.
    for (const auto &other : frontend->declarations()) {
        if (other.kind != frontend::DeclKind::Function || !other.body)
            continue;
        resolveCallsInFunction(other);
    }

    current_module_ = nullptr;
    current_sema_   = nullptr;
    current_typed_  = nullptr;
    current_key_.clear();
}

void NraFacts::resolveCallsInFunction(const frontend::Declaration &decl) {
    current_key_ = current_module_->key + "#" + std::to_string(decl.id.value);
    current_params_.clear();
    for (const auto &parameter : decl.parameters)
        current_params_.push_back(parameter.id);
    const auto *body = expr(decl.body);
    if (body != nullptr)
        walkBody(*body);
}

void NraFacts::collectFunctionFact() {
    if (!any_return_)
        return;
    NraFunctionFact fact;
    fact.allReturnsParameter      = all_returns_same_parameter;
    fact.parameterIndex           = returned_parameter_;
    function_facts_[current_key_] = fact;
}

void NraFacts::collectNarrowing(const frontend::FrontendSnapshot &frontend) {
    (void)frontend;
}

void NraFacts::walkExpr(frontend::ExprId id) {
    const auto *expression = expr(id);
    if (expression == nullptr)
        return;
    if (expression->kind == frontend::ExprKind::Call) {
        analyzeCall(*expression);
    } else if (expression->kind == frontend::ExprKind::Return) {
        analyzeReturn(*expression);
    } else if (expression->kind == frontend::ExprKind::If && expression->operands.size() >= 2U) {
        walkConditionExpression(expression->operands[0], std::nullopt);
        const auto before_then     = narrowing_facts_;
        const auto saved_condition = current_condition_;
        current_condition_         = expression->operands[0];
        walkExpr(expression->operands[1]);
        const auto then_facts = narrowing_facts_;
        narrowing_facts_      = before_then;
        if (expression->operands.size() >= 3U)
            walkExpr(expression->operands[2]);
        current_condition_ = saved_condition;
        for (const auto &entry : then_facts) {
            const auto current = narrowing_facts_.find(entry.first);
            if (current == narrowing_facts_.end() || !current->second.nonNull)
                continue;
            narrowing_facts_[entry.first].nonNull = entry.second.nonNull;
            narrowing_facts_[entry.first].isNullChecked =
                narrowing_facts_[entry.first].isNullChecked || entry.second.isNullChecked;
        }
        narrowing_facts_[0].nonNull = false;
        narrowing_facts_.erase(0);
        if (expression->statements.empty() && expression->operands.empty() == false)
            return;
        // The operand walk below handles any merged value expression.
        for (size_t operand = 1; operand < expression->operands.size(); ++operand)
            walkExpr(expression->operands[operand]);
        return;
    } else if (expression->kind == frontend::ExprKind::IsNull && !expression->operands.empty()) {
        const frontend::ExprId root = expression->operands[0];
        const auto root_expr        = expr(root);
        if (root_expr != nullptr) {
            const frontend::LocalId local = localOfName(*root_expr);
            if (local) {
                auto &narrowed         = narrowing_facts_[local.value];
                narrowed.isNullChecked = true;
            }
        }
        walkExpr(expression->operands[0]);
        return;
    }

    for (const auto &operand : expression->operands)
        walkExpr(operand);
    for (const auto &statement : expression->statements)
        walkStatement(statement);
    if (expression->expansion)
        walkExpr(expression->expansion);
}

void NraFacts::walkConditionExpression(frontend::ExprId id,
                                       std::optional<frontend::ExprId> negative) {
    const auto *expression = expr(id);
    if (expression == nullptr)
        return;
    if (expression->kind == frontend::ExprKind::IsNull && !expression->operands.empty()) {
        const auto *root_expr = expr(expression->operands[0]);
        if (root_expr == nullptr)
            return;
        const frontend::LocalId local = localOfName(*root_expr);
        if (local) {
            auto &narrowed = narrowing_facts_[local.value];
            if (!negative)
                narrowed.knownNull = true;
            if (negative)
                narrowed.nonNull = true;
        }
        return;
    }
    if (expression->kind == frontend::ExprKind::Unary && expression->text == "not" &&
        !expression->operands.empty()) {
        walkConditionExpression(expression->operands[0],
                                negative ? std::nullopt
                                         : std::optional<frontend::ExprId>(expression->id));
        return;
    }
}

void NraFacts::applyCurrentNarrowing() {
    if (!current_condition_)
        return;
    const auto *condition = expr(current_condition_);
    if (condition == nullptr)
        return;
    if (condition->kind == frontend::ExprKind::Unary && condition->text == "not") {
        if (condition->operands.size() == 1U) {
            const auto *inner = expr(condition->operands[0]);
            if (inner != nullptr && inner->kind == frontend::ExprKind::IsNull &&
                !inner->operands.empty()) {
                const auto *root_expr = expr(inner->operands[0]);
                const frontend::LocalId narrowed =
                    root_expr != nullptr ? localOfName(*root_expr) : frontend::LocalId{};
                if (narrowed) {
                    auto &fact                               = local_facts_[narrowed.value];
                    fact.nonNull                             = true;
                    narrowing_facts_[narrowed.value].nonNull = true;
                }
            }
        }
    }
}

void NraFacts::walkStatement(frontend::StmtId id) {
    if (!id || current_module_ == nullptr || current_module_->frontend == nullptr ||
        id.value > current_module_->frontend->statements().size())
        return;
    const auto &statement = current_module_->frontend->statements()[id.value - 1U];
    if (statement.expression) {
        applyCurrentNarrowing();
        walkExpr(statement.expression);
    }
    if (statement.binding.initializer)
        walkExpr(statement.binding.initializer);
    if (statement.binding.id) {
        NraLocalFact fact;
        const auto *type = current_typed_ != nullptr
                               ? current_typed_->localTypes.get(statement.binding.id.value)
                               : nullptr;
        if (type != nullptr) {
            fact.declaredType = *type;
            fact.ownership    = ownershipOfLocal(statement.binding.id);
        }
        local_facts_.insert(statement.binding.id.value, fact);
    }
}

void NraFacts::walkFunction(const frontend::Declaration &decl) {
    if (!decl.body)
        return;

    // Walk every top-level statement/expression once. Statement bindings are
    // registered before nested expressions so ownership qualifiers resolve.
    const auto *body = expr(decl.body);
    if (body == nullptr)
        return;

    current_condition_ = {};
    walkBody(*body);
    analyzeImplicitReturn(*body);
    current_condition_ = {};
}

void NraFacts::walkBody(const frontend::Expression &body) {
    for (const auto statement : body.statements)
        walkStatement(statement);
    for (const auto operand : body.operands)
        walkExpr(operand);
}

void NraFacts::analyzeReturn(const frontend::Expression &ret) {
    any_return_ = true;
    if (ret.operands.empty())
        return;
    const auto *value = expr(ret.operands[0]);
    if (value == nullptr)
        return;
    const frontend::LocalId local = localOfName(*value);
    if (!local)
        return;
    const auto found = std::find(current_params_.begin(), current_params_.end(), local);
    if (found == current_params_.end())
        return;
    const uint32_t index = static_cast<uint32_t>(found - current_params_.begin());
    if (returned_parameter_ == ~0U)
        returned_parameter_ = index;
    else if (returned_parameter_ != index)
        all_returns_same_parameter = false;
}

void NraFacts::analyzeImplicitReturn(const frontend::Expression &body) {
    if (!body.statements.empty()) {
        const auto &last =
            current_module_->frontend->statements()[body.statements.back().value - 1U];
        if (last.kind == frontend::StmtKind::Expression && last.expression) {
            const auto *value = expr(last.expression);
            if (value != nullptr) {
                any_return_                   = true;
                const frontend::LocalId local = localOfName(*value);
                if (local) {
                    const auto found =
                        std::find(current_params_.begin(), current_params_.end(), local);
                    if (found != current_params_.end()) {
                        const uint32_t index =
                            static_cast<uint32_t>(found - current_params_.begin());
                        if (returned_parameter_ == ~0U)
                            returned_parameter_ = index;
                        else if (returned_parameter_ != index)
                            all_returns_same_parameter = false;
                    }
                }
            }
            return;
        }
    }
    if (!body.operands.empty()) {
        const auto *value = expr(body.operands.back());
        if (value != nullptr && value->kind != frontend::ExprKind::Return) {
            any_return_                   = true;
            const frontend::LocalId local = localOfName(*value);
            if (local) {
                const auto found = std::find(current_params_.begin(), current_params_.end(), local);
                if (found != current_params_.end()) {
                    const uint32_t index = static_cast<uint32_t>(found - current_params_.begin());
                    if (returned_parameter_ == ~0U)
                        returned_parameter_ = index;
                    else if (returned_parameter_ != index)
                        all_returns_same_parameter = false;
                }
            }
            analyzeReturn(*value);
        }
        return;
    }
    if (body.kind != frontend::ExprKind::Block && body.kind != frontend::ExprKind::Return)
        analyzeReturn(body);
}

void NraFacts::analyzeCall(const frontend::Expression &call) {
    if (!call.id || current_module_ == nullptr || current_module_->frontend == nullptr ||
        call.operands.size() < 2U)
        return;

    NraCallFact fact;
    fact.argEscapes.resize(call.operands.size() - 1U, NraArgEscape::Borrow);
    fact.argRepeated.resize(call.operands.size() - 1U, 0U);
    std::vector<frontend::LocalId> argument_locals;
    argument_locals.reserve(call.operands.size() - 1U);
    for (size_t index = 1; index < call.operands.size(); ++index) {
        const auto *arg = expr(call.operands[index]);
        if (arg == nullptr)
            continue;
        const frontend::LocalId local = localOfArgument(*arg);
        argument_locals.push_back(local);
        const types::OwnershipKind ownership = ownershipOfArgument(*arg);
        if (ownership == types::OwnershipKind::Belong) {
            const auto arg_slot = index - 1U;
            if (fact.argEscapes.size() > arg_slot)
                fact.argEscapes[arg_slot] = NraArgEscape::Escape;
        }
    }

    // A duplicated `share`/`view` argument is a logic alert but stays borrowable;
    // default/unique/lend duplicates are residual move facts.
    for (size_t a = 0; a < argument_locals.size(); ++a) {
        if (!argument_locals[a])
            continue;
        for (size_t b = a + 1U; b < argument_locals.size(); ++b) {
            if (argument_locals[a] != argument_locals[b])
                continue;
            const auto ownership = ownershipOfLocal(argument_locals[a]);
            NraArgEscape escape  = NraArgEscape::Borrow;
            if (ownership != types::OwnershipKind::Share &&
                ownership != types::OwnershipKind::View) {
                escape = NraArgEscape::Move;
            } else {
                fact.duplicatedShareOrView = true;
            }
            if (a < fact.argEscapes.size())
                fact.argEscapes[a] = escape;
            if (b < fact.argEscapes.size())
                fact.argEscapes[b] = escape;
            if (a < fact.argRepeated.size())
                fact.argRepeated[a] = 1U;
            if (b < fact.argRepeated.size())
                fact.argRepeated[b] = 1U;
        }
    }

    // Return-node equivalence is recorded per function. A function whose every
    // return path forwards the same parameter is NonConsumed at the call site;
    // lowering consults that fact when it emits call/return attributes.
    const auto *callee = expr(call.operands[0]);
    if (callee != nullptr && callee->kind == frontend::ExprKind::Name) {
        const auto *callee_resolved                  = resolved(callee->id);
        const session::ModuleArtifact *callee_module = current_module_;
        frontend::DeclId callee_decl                 = {};
        if (callee_resolved != nullptr) {
            callee_decl = callee_resolved->declaration;
            if (!callee_resolved->target.module.empty()) {
                callee_module = snapshot_.findModule(callee_resolved->target.module);
                if (!callee_decl && callee_resolved->target.localSymbol)
                    callee_decl = frontend::DeclId{callee_resolved->target.localSymbol.value};
            }
        } else if (const auto *target = current_sema_->resolvedCallTarget(callee->id)) {
            callee_module = snapshot_.findModule(target->module);
            callee_decl   = target->decl;
        }
        if (callee_module != nullptr && callee_decl) {
            const auto *fn_fact = functionFact(callee_module->key, callee_decl);
            if (fn_fact != nullptr && fn_fact->allReturnsParameter &&
                fn_fact->parameterIndex < fact.argEscapes.size()) {
                fact.returnsArgument = fn_fact->parameterIndex;
            }
        } else if (callee_module != nullptr && callee_decl) {
            // Do not invent facts for callees analysed in a later module; the
            // lowering verifier only checks that residue is consistent.
        }
    }
    call_facts_.insert(call.id.value, std::move(fact));
}

const frontend::Expression *NraFacts::expr(frontend::ExprId id) const noexcept {
    if (!id || current_module_ == nullptr || current_module_->frontend == nullptr ||
        id.value > current_module_->frontend->expressions().size())
        return nullptr;
    return &current_module_->frontend->expressions()[id.value - 1U];
}

frontend::LocalId NraFacts::localOfName(const frontend::Expression &name) const noexcept {
    if (name.kind != frontend::ExprKind::Name)
        return {};
    if (current_module_ == nullptr)
        return {};
    const auto *resolution = snapshot_.findResolution(current_module_->key);
    if (resolution == nullptr)
        return {};
    const auto *resolved = session::lookupExprResolution(*resolution, name.id);
    if (resolved == nullptr)
        return {};
    // A declaration name (function, enum, struct) is not a slot local. The
    // resolver records `local` only for real bindings; preserve that contract.
    if (resolved->declaration && !resolved->local &&
        resolved->declKind != frontend::DeclKind::Function)
        return {};
    return resolved->local;
}

frontend::LocalId NraFacts::localOfArgument(const frontend::Expression &arg) const noexcept {
    const frontend::LocalId direct = localOfName(arg);
    if (direct)
        return frontend::LocalId{direct.value};
    if (arg.kind == frontend::ExprKind::Field || arg.kind == frontend::ExprKind::Arrow) {
        if (!arg.operands.empty()) {
            const auto *root = expr(arg.operands[0]);
            if (root != nullptr)
                return localOfArgument(*root);
        }
    }
    return {};
}

types::OwnershipKind NraFacts::ownershipOfArgument(const frontend::Expression &arg) const noexcept {
    const auto direct = localOfName(arg);
    if (direct)
        return ownershipOfLocal(direct);
    if (arg.kind == frontend::ExprKind::Field || arg.kind == frontend::ExprKind::Arrow) {
        const auto type =
            current_typed_ != nullptr ? current_typed_->exprTypes.get(arg.id.value) : nullptr;
        if (type != nullptr) {
            const auto *qualified = sema_.typeTable().qualified(sema_.typeTable().canonical(*type));
            if (qualified != nullptr && qualified->ownership != types::OwnershipKind::Default)
                return qualified->ownership;
        }

        if (current_sema_ != nullptr && !arg.operands.empty()) {
            const frontend::LocalId root = localOfArgument(*expr(arg.operands[0]));
            if (root) {
                const auto root_type = current_sema_->typeOfLocal(root);
                auto base_type       = sema_.typeTable().stripQualifiers(root_type);
                if (arg.kind == frontend::ExprKind::Arrow) {
                    if (const auto *pt = sema_.typeTable().pointer(base_type))
                        base_type = sema_.typeTable().stripQualifiers(pt->pointee);
                }
                if (const auto *st = sema_.typeTable().struct_type(base_type)) {
                    const int field = sema_.typeTable().fieldIndex(base_type, arg.text);
                    if (field >= 0 && static_cast<size_t>(field) < st->fields.size()) {
                        const auto *field_qualified =
                            sema_.typeTable().qualified(st->fields[static_cast<size_t>(field)]);
                        if (field_qualified != nullptr)
                            return field_qualified->ownership;
                    }
                }
            }
        }

        if (!arg.operands.empty()) {
            const auto *root = expr(arg.operands[0]);
            if (root != nullptr)
                return ownershipOfArgument(*root);
        }
    }
    return types::OwnershipKind::Default;
}

types::OwnershipKind NraFacts::ownershipOfLocal(frontend::LocalId id) const noexcept {
    const auto *fact = local_facts_.get(id.value);
    if (fact != nullptr && id.value != 0)
        return fact->ownership;
    if (current_typed_ == nullptr)
        return types::OwnershipKind::Default;
    const auto *type = current_typed_->localTypes.get(id.value);
    if (type == nullptr)
        return types::OwnershipKind::Default;
    const auto *qualified = sema_.typeTable().qualified(sema_.typeTable().canonical(*type));
    return qualified != nullptr ? qualified->ownership : types::OwnershipKind::Default;
}

const session::ResolvedName *NraFacts::resolved(frontend::ExprId id) const noexcept {
    if (!id || current_module_ == nullptr)
        return nullptr;
    const auto *resolution = snapshot_.findResolution(current_module_->key);
    return resolution == nullptr ? nullptr : session::lookupExprResolution(*resolution, id);
}

} // namespace zith::sema::modern
