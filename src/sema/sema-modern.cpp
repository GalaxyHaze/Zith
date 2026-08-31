#include "sema/sema-modern.hpp"

#include "diagnostics/error-codes.hpp"
#include "sema/op-mapping.hpp"
#include "sema/sema-modern-utils.hpp"
#include "support/int-literal.hpp"

#include <algorithm>
#include <cstring>
#include <functional>

#include <cstdlib>
#include <limits>
#include <string>

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
    checkStructFieldDefaults();
    checkFunctionDefaults();
    checkConstFieldAssignments();
    checkZithDeclarations();
    checkReturnsAndCalls();
    return !hasErrors();
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
    diagnostics.emplace(arena, span, std::move(message), diagnostics::Severity::Error, code);
}

void PerModuleSema::reportNote(frontend::TextSpan span, std::string message) {
    diagnostics.emplace(arena, span, std::move(message), diagnostics::Severity::Note,
                        static_cast<uint32_t>(0));
}

SemaPipeline::SemaPipeline(memory::Arena &arena, diagnostics::DiagnosticEngine &diags,
                           const session::CompilationSnapshot &snapshot)
    : arena_(arena), diags_(diags), snapshot_(snapshot), type_table_(arena), typed_maps_(),
      modules_(arena), has_errors_(false) {}

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
        sema->instantiations = instantiation_pass_;
        modules_.push(sema);
        if (!sema->prepareTypes())
            has_errors_ = true;
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
