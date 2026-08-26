#pragma once

#include "diagnostics/diagnostic-engine.hpp"
#include "frontend/frontend.hpp"
#include "memory/arena.hpp"
#include "memory/flat-map.hpp"
#include "memory/string-interner.hpp"
#include "sema/modern-types.hpp"
#include "sema/sema-modern.hpp"
#include "session/frontend-context.hpp"
#include "types/type-kind.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace zith::sema::modern {

enum class NraArgEscape : uint8_t {
    None,
    Borrow,
    Capture,
    Escape,
    Move,
};

struct NraLocalFact {
    TypeId declaredType            = kInvalidTypeId;
    types::OwnershipKind ownership = types::OwnershipKind::Default;
    bool knownAlive                = true;
    bool nonNull                   = false;

    [[nodiscard]] constexpr bool hasResidual() const noexcept {
        return ownership != types::OwnershipKind::Default || nonNull || !knownAlive;
    }
};

struct NraCallFact {
    uint32_t returnsArgument = ~0U;
    std::vector<NraArgEscape> argEscapes;
    std::vector<uint8_t> argRepeated;
    bool duplicatedShareOrView = false;
    bool returnsNonNull        = false;

    [[nodiscard]] bool hasResidual() const noexcept {
        if (returnsArgument != ~0U || returnsNonNull || duplicatedShareOrView)
            return true;
        for (const auto escape : argEscapes) {
            if (escape != NraArgEscape::Borrow)
                return true;
        }
        return false;
    }
};

struct NraFunctionFact {
    bool allReturnsParameter = false;
    uint32_t parameterIndex  = ~0U;

    [[nodiscard]] constexpr bool hasResidual() const noexcept {
        return allReturnsParameter;
    }
};

struct NraNarrowingFact {
    bool isNullChecked = false;
    bool nonNull       = false;
    bool knownNull     = false;
};

/// Pre-lowering residual ownership accumulator.
///
/// This is deliberately not the full alive/dead/lent proof machine. It computes
/// the facts that the stable HIR boundary promises to carry and reports the
/// qualifier shapes that are already semantically representable before lowering.
/// Lowering turns these facts into `hir::HirAttrs` side tables only.
class NraFacts {
public:
    NraFacts(memory::Arena &arena, diagnostics::DiagnosticEngine &diagnostics,
             const session::CompilationSnapshot &snapshot, const SemaPipeline &sema,
             memory::StringInterner &interner);

    bool run();

    [[nodiscard]] const NraLocalFact *localFact(frontend::LocalId id) const noexcept {
        return local_facts_.get(id.value);
    }
    [[nodiscard]] const NraCallFact *callFact(frontend::ExprId id) const noexcept {
        return call_facts_.get(id.value);
    }
    [[nodiscard]] const NraFunctionFact *functionFact(session::ModuleKey module,
                                                      frontend::DeclId id) const noexcept {
        return function_facts_.get(internFunctionKey(*interner_, module, id));
    }
    [[nodiscard]] const NraNarrowingFact *narrowingFact(frontend::LocalId id) const noexcept {
        const auto found = narrowing_facts_.find(id.value);
        return found == narrowing_facts_.end() ? nullptr : &found->second;
    }
    [[nodiscard]] size_t localCount() const noexcept {
        return local_facts_.size();
    }
    [[nodiscard]] size_t callCount() const noexcept {
        return call_facts_.size();
    }
    [[nodiscard]] bool hasErrors() const noexcept {
        return diagnostics_.hasErrors() || has_errors_;
    }

private:
    static uint64_t internFunctionKey(memory::StringInterner &interner, std::string_view module,
                                      frontend::DeclId id) noexcept {
        const auto module_id = interner.intern(module);
        return (static_cast<uint64_t>(module_id) << 32U) | id.value;
    }

    void analyzeModule(const session::ModuleArtifact &module, const PerModuleSema &module_sema,
                       const TypedMap &typed);
    void analyzeCall(const frontend::Expression &call);
    void analyzeReturn(const frontend::Expression &ret);
    void walkExpr(frontend::ExprId id);
    void walkStatement(frontend::StmtId id);
    void walkFunction(const frontend::Declaration &decl);
    void walkBody(const frontend::Expression &body);
    void walkConditionExpression(frontend::ExprId id, std::optional<frontend::ExprId> negative);
    void applyCurrentNarrowing();
    void analyzeImplicitReturn(const frontend::Expression &body);
    void resolveCallsInFunction(const frontend::Declaration &decl);
    void collectFunctionFact();
    void collectNarrowing(const frontend::FrontendSnapshot &frontend);

    [[nodiscard]] const frontend::Expression *expr(frontend::ExprId id) const noexcept;
    [[nodiscard]] frontend::LocalId localOfName(const frontend::Expression &name) const noexcept;
    [[nodiscard]] frontend::LocalId localOfArgument(const frontend::Expression &arg) const noexcept;
    [[nodiscard]] types::OwnershipKind ownershipOfLocal(frontend::LocalId id) const noexcept;
    [[nodiscard]] types::OwnershipKind
    ownershipOfArgument(const frontend::Expression &arg) const noexcept;
    [[nodiscard]] const session::ResolvedName *resolved(frontend::ExprId id) const noexcept;

    diagnostics::DiagnosticEngine &diagnostics_;
    const session::CompilationSnapshot &snapshot_;
    const SemaPipeline &sema_;
    memory::StringInterner *interner_;
    memory::FlatMap<uint32_t, NraLocalFact> local_facts_;
    memory::FlatMap<uint32_t, NraCallFact> call_facts_;
    std::unordered_map<uint32_t, NraNarrowingFact> narrowing_facts_;
    memory::FlatMap<uint64_t, NraFunctionFact> function_facts_;

    const session::ModuleArtifact *current_module_ = nullptr;
    const PerModuleSema *current_sema_             = nullptr;
    const TypedMap *current_typed_                 = nullptr;
    uint64_t current_key_                          = 0;
    std::vector<frontend::LocalId> current_params_;
    frontend::ExprId current_condition_ = {};
    bool any_return_                    = false;
    bool all_returns_same_parameter     = true;
    uint32_t returned_parameter_        = ~0U;
    bool has_errors_                    = false;
};

} // namespace zith::sema::modern
