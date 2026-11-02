#include "sema/sema-modern.hpp"

#include "diagnostics/error-codes.hpp"
#include "sema/op-mapping.hpp"
#include "sema/sema-modern-utils.hpp"
#include "support/int-literal.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>

#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <unordered_map>

namespace zith::sema::modern {

PerModuleSema::PerModuleSema(session::ModuleKey mod, const frontend::FrontendSnapshot &snap,
                             const session::ModuleResolution &res, TypeTable &tt, TypedMap &tm,
                             memory::Arena &a, memory::FileId file_id, SemaPipeline *owner_)
    : module(std::move(mod)), fileId(file_id), snapshot(snap), resolution(res), type_table(tt),
      typed_map(tm), arena(a), diagnostics(a), owner(owner_), error_type(kInvalidTypeId),
      invalid_type(kInvalidTypeId), void_type(kInvalidTypeId), bool_type(kInvalidTypeId),
      char_type(kInvalidTypeId), i32_type(kInvalidTypeId), i64_type(kInvalidTypeId),
      f32_type(kInvalidTypeId), f64_type(kInvalidTypeId), null_type(kInvalidTypeId),
      end_type(kInvalidTypeId) {}

bool PerModuleSema::run() {
    if (!prepareTypes())
        return false;
    if (!checkExpressions())
        return false;
    return !hasErrors();
}

bool PerModuleSema::prepareTypes() {
    registerPrimitiveTypes();
    registerNamedTypes();
    prepareImplementOwners();
    lowerDeclarationTypes();
    checkImplementBlocks();
    return true;
}

bool PerModuleSema::checkExpressions() {
    inferExpressionTypes();
    checkAlignmentAttributes();
    checkStructFieldDefaults();
    checkFunctionDefaults();
    checkConstFieldAssignments();
    checkZithDeclarations();
    checkReturnsAndCalls();
    checkComponentMethods();
    return !hasErrors();
}

void PerModuleSema::resetProvisionalInferenceResults() {
    typed_map.exprTypes.clear();
    // The first-assignment ledger is provisional inference state: it is filled
    // by probeFunctionCallTargets and by a function's own return inference. It
    // must not leak into the definitive checkExpressions pass, where an empty
    // ledger is what makes the first write to a `let` legal. Without this, a
    // typed `let q: i32; q = 42;` is reported as a second write (E2010).
    typeInferredByAssignment_.clear();
    typed_map.forInRangeLiteral.clear();
    typed_map.forInNext.clear();
    typed_map.containsCall.clear();
    typed_map.forInElementIndex.clear();
    typed_map.forInEndIndex.clear();
    typed_map.forInUnionType.clear();
    typed_map.forInOptionalType.clear();
    typed_map.traitQualifiedReceiverBase.clear();
    typed_map.opaqueSourceTypes.clear();
    typed_map.dynSourceTypes.clear();
    typed_map.provenNonNullExprs.clear();
    typed_map.provenNonNullOptionalExprs.clear();
    typed_map.optionalPayloadReads.clear();
    typed_map.variadicCallPlans.clear();
    typed_map.variadicStmtPlans.clear();
    call_targets_.clear();
}

bool SemaPipeline::inferFunctionReturnTypes() {
    struct FunctionNode {
        PerModuleSema *sema = nullptr;
        const frontend::Declaration *decl = nullptr;
        std::vector<size_t> callees;
        bool inferReturn = false;
        bool recursiveInference = false;
    };

    const auto keyFor = [](std::string_view module, uint32_t decl_id) {
        std::string key(module);
        key.push_back('\x1f');
        key += std::to_string(decl_id);
        return key;
    };
    const auto needsInference = [](const frontend::Declaration &decl) {
        return decl.kind == frontend::DeclKind::Function && decl.body && !decl.declaredType &&
               !decl.isExtern && decl.functionKind != frontend::FunctionKind::State &&
               decl.functionKind != frontend::FunctionKind::Const &&
               decl.functionKind != frontend::FunctionKind::Extern;
    };

    std::vector<FunctionNode> functions;
    std::unordered_map<std::string, size_t> functionByKey;
    struct LocalTypeSnapshot {
        PerModuleSema *sema = nullptr;
        memory::FlatMap<uint32_t, TypeId> localTypes;
    };
    std::vector<LocalTypeSnapshot> local_type_snapshots;
    for (auto *sema : modules_) {
        sema->prepareStateMachineGrouping();
        sema->resetProvisionalInferenceResults();
        local_type_snapshots.push_back({sema, sema->typed_map.localTypes});
        for (const auto &decl : sema->snapshot.declarations()) {
            if (decl.kind != frontend::DeclKind::Function || !decl.body || decl.isExtern)
                continue;
            const size_t index = functions.size();
            functions.push_back(
                FunctionNode{sema, &decl, {}, needsInference(decl), false});
            functionByKey.emplace(keyFor(sema->module, decl.id.value), index);
        }
    }
    for (auto *sema : modules_)
        sema->resetProvisionalInferenceResults();
    for (const auto &function : functions)
        function.sema->probeFunctionCallTargets(*function.decl);

    const auto collectCalls = [&](const FunctionNode &function, std::vector<size_t> &out) {
        const auto &snapshot = function.sema->snapshot;
        std::vector<uint8_t> visited(snapshot.expressions().size() + 1U, 0U);
        std::function<void(frontend::ExprId)> walk = [&](const frontend::ExprId id) {
            if (!id || id.value > snapshot.expressions().size() || visited[id.value] != 0U)
                return;
            visited[id.value] = 1U;
            const auto &expr = snapshot.expressions()[id.value - 1U];
            if (expr.kind == frontend::ExprKind::Call && !expr.operands.empty()) {
                if (const auto *call_target =
                        function.sema->resolvedCallTarget(expr.operands[0])) {
                    const auto target = functionByKey.find(
                        keyFor(call_target->module, call_target->decl.value));
                    if (target != functionByKey.end())
                        out.push_back(target->second);
                } else if (const auto *resolved =
                               function.sema->findResolvedExpr(expr.operands[0])) {
                    auto target_module = resolved->target.module.empty()
                                             ? function.sema->module
                                             : resolved->target.module;
                    frontend::DeclId target_decl = resolved->declaration;
                    if (!target_decl && resolved->target.localSymbol)
                        target_decl = frontend::DeclId{resolved->target.localSymbol.value};
                    const auto target = functionByKey.find(keyFor(target_module,
                                                                   target_decl.value));
                    if (target_decl && target != functionByKey.end())
                        out.push_back(target->second);
                }
            }
            for (const auto operand : expr.operands)
                walk(operand);
            for (const auto condition : expr.conditions)
                walk(condition);
            if (expr.expansion)
                walk(expr.expansion);
            for (const auto stmt_id : expr.statements) {
                if (!stmt_id || stmt_id.value > snapshot.statements().size())
                    continue;
                const auto &stmt = snapshot.statements()[stmt_id.value - 1U];
                walk(stmt.expression);
                walk(stmt.binding.initializer);
                for (const auto argument : stmt.arguments)
                    walk(argument);
            }
        };
        walk(function.decl->body);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    };
    for (auto &function : functions)
        collectCalls(function, function.callees);
    for (auto &snapshot : local_type_snapshots) {
        snapshot.sema->resetProvisionalInferenceResults();
        snapshot.sema->typed_map.localTypes = std::move(snapshot.localTypes);
    }

    std::vector<uint8_t> visitState(functions.size(), 0U);
    std::vector<size_t> stack;
    std::function<void(size_t)> visit = [&](const size_t index) {
        if (visitState[index] == 2U)
            return;
        if (visitState[index] == 1U)
            return;
        visitState[index] = 1U;
        stack.push_back(index);
        for (const size_t callee : functions[index].callees) {
            if (visitState[callee] == 1U) {
                const auto cycle = std::find(stack.begin(), stack.end(), callee);
                for (auto member = cycle; member != stack.end(); ++member) {
                    auto &recursive = functions[*member];
                    if (recursive.inferReturn && !recursive.recursiveInference) {
                        recursive.recursiveInference = true;
                        recursive.sema->report(
                            recursive.decl->span,
                            "recursive call cycle requires an explicit return type for '" +
                                recursive.decl->name + "'",
                            diagnostics::err::CannotInfer);
                    }
                }
            } else {
                visit(callee);
            }
        }
        stack.pop_back();
        visitState[index] = 2U;
        auto &function = functions[index];
        if (function.inferReturn && !function.recursiveInference)
            function.sema->inferFunctionReturnType(*function.decl);
    };
    for (size_t index = 0; index < functions.size(); ++index)
        visit(index);

    return std::none_of(modules_.begin(), modules_.end(),
                        [](const PerModuleSema *sema) { return sema->hasErrors(); });
}

bool PerModuleSema::hasErrors() const noexcept {
    for (const auto &d : diagnostics)
        if (d.severity == diagnostics::Severity::Error)
            return true;
    return false;
}

TypeId PerModuleSema::typeOfExpr(frontend::ExprId id) const noexcept {
    if (!id)
        return kInvalidTypeId;
    const auto *value = typed_map.exprTypes.get(id.value);
    return value ? *value : kInvalidTypeId;
}

TypeId PerModuleSema::typeOfDecl(frontend::DeclId id) const noexcept {
    if (!id)
        return kInvalidTypeId;
    const auto *value = typed_map.declTypes.get(id.value);
    return value ? *value : kInvalidTypeId;
}

TypeId PerModuleSema::typeOfLocal(frontend::LocalId id) const noexcept {
    if (!id)
        return kInvalidTypeId;
    const auto *value = typed_map.localTypes.get(id.value);
    return value ? *value : kInvalidTypeId;
}

void PerModuleSema::setExprType(frontend::ExprId id, TypeId type) {
    if (id)
        typed_map.exprTypes.insert(id.value, type);
}

void PerModuleSema::setDeclType(frontend::DeclId id, TypeId type) {
    if (id)
        typed_map.declTypes.insert(id.value, type);
}

void PerModuleSema::setLocalType(frontend::LocalId id, TypeId type) {
    if (id)
        typed_map.localTypes.insert(id.value, type);
}

void PerModuleSema::report(frontend::TextSpan span, std::string message, uint32_t code) {
    if (suppressDiagnostics_)
        return;
    diagnostics.emplace(arena, span, std::move(message), diagnostics::Severity::Error, code);
}

void PerModuleSema::reportNote(frontend::TextSpan span, std::string message) {
    if (suppressDiagnostics_)
        return;
    diagnostics.emplace(arena, span, std::move(message), diagnostics::Severity::Note,
                        static_cast<uint32_t>(0));
}

void PerModuleSema::reportWarning(frontend::TextSpan span, std::string message, uint32_t code) {
    if (suppressDiagnostics_)
        return;
    diagnostics.emplace(arena, span, std::move(message), diagnostics::Severity::Warning, code);
}

void PerModuleSema::semaProbe(const char *fmt, ...) const {
    if (!debugSema)
        return;
    std::va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
}

SemaPipeline::SemaPipeline(memory::Arena &arena, diagnostics::DiagnosticEngine &diags,
                           const session::CompilationSnapshot &snapshot, bool debugSema)
    : arena_(arena), diags_(diags), snapshot_(snapshot), type_table_(arena), typed_maps_(),
      modules_(arena), has_errors_(false), debugSema_(debugSema) {}

bool SemaPipeline::run() {
    for (const auto &artifact_ptr : snapshot_.modules()) {
        const auto &artifact   = *artifact_ptr;
        const auto *resolution = snapshot_.findResolution(artifact.key);
        if (!resolution || !artifact.frontend)
            continue;

        auto &typed_map = typedMap(artifact.key);
        auto *sema =
            arena_.make<PerModuleSema>(artifact.key, *artifact.frontend, *resolution, type_table_,
                                       typed_map, arena_, artifact.fileId, this);
        sema->debugSema      = debugSema_;
        sema->instantiations = instantiation_pass_;
        modules_.push(sema);
    }
    for (auto *sema : modules_) {
        if (!sema->prepareTypes())
            has_errors_ = true;
    }
    if (!inferFunctionReturnTypes())
        has_errors_ = true;
    for (auto *sema : modules_) {
        sema->resetProvisionalInferenceResults();
    }
    for (auto *sema : modules_) {
        if (!sema->checkExpressions())
            has_errors_ = true;
    }

    for (const auto *sema : modules_) {
        for (const auto &d : sema->diagnostics) {
            diags_.report(d.severity, d.code, d.message, sema->toMemorySpan(d.primary_span));
        }
    }

    return !has_errors_;
}

bool SemaPipeline::hasErrors() const noexcept {
    return has_errors_;
}

PerModuleSema *SemaPipeline::findModuleSema(session::ModuleKey module) const noexcept {
    for (const auto *sema : modules_) {
        if (sema->module == module)
            return const_cast<PerModuleSema *>(sema);
    }
    return nullptr;
}

const TypedMap *SemaPipeline::findTypedMap(session::ModuleKey module) const noexcept {
    auto *value = typed_maps_.get(module);
    if (value == nullptr)
        return nullptr;
    return *value;
}

TypedMap &SemaPipeline::typedMap(session::ModuleKey module) noexcept {
    auto *value = typed_maps_.get(module);
    if (value && *value)
        return **value;
    auto *tm = arena_.make<TypedMap>(arena_);
    typed_maps_.insert(module, tm);
    return *tm;
}

bool SemaPipeline::isSelfReceiver(session::ModuleKey module, frontend::ExprId id) const noexcept {
    const auto *sema = findModuleSema(module);
    return sema != nullptr && sema->isSelfReceiver(id);
}

bool SemaPipeline::isBorrowParameter(session::ModuleKey module,
                                     frontend::ExprId id) const noexcept {
    const auto *sema = findModuleSema(module);
    return sema != nullptr && sema->isBorrowParameter(id);
}

} // namespace zith::sema::modern
