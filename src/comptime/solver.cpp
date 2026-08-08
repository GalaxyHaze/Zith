#include "solver.hpp"
#include "common/overloaded.hpp"

#include "diagnostics/error-codes.hpp"
#include "types/type-kind.hpp"

namespace zith::comptime {

namespace {

inline bool isNumericType(types::TypeId t, types::TypeIntern &types) {
    auto kind = types.kindOf(t);
    return kind == types::TypeKind::Int || kind == types::TypeKind::Float;
}

bool typeHasGenericParam(types::TypeId t, types::TypeIntern &types) {
    auto kind = types.kindOf(t);
    if (kind == types::TypeKind::GenericParam)
        return true;
    if (kind == types::TypeKind::Incomplete) {
        auto *inc = std::get_if<types::TypeIncomplete>(&types.lookup(t));
        for (size_t i = 0; i < inc->arg_count; i++) {
            if (typeHasGenericParam(inc->args[i], types))
                return true;
        }
    }
    if (kind == types::TypeKind::Fn) {
        auto *fn = std::get_if<types::TypeFn>(&types.lookup(t));
        if (typeHasGenericParam(fn->ret, types))
            return true;
        for (size_t i = 0; i < fn->param_count; i++) {
            if (typeHasGenericParam(fn->params[i], types))
                return true;
        }
    }
    return false;
}

} // anonymous namespace

Solver::Solver(types::TypeIntern &types, ast::AstBuilder *ast, ast::ProgramNode *program,
               symbols::SymbolTable &syms, diagnostics::DiagnosticEngine &diags,
               memory::Arena &hir_arena, sema::modern::TypeTable *modern_types)
    : types_(types), ast_(ast), program_(program), syms_(syms), diags_(diags),
      modern_types_(modern_types), hir_arena_(hir_arena), generic_fns_(hir_arena),
      monomorphs_(hir_arena) {}

bool Solver::solve(hir::HirModule &hir) {
    if (!collectGenerics())
        return false;

    if (!verifyGenericConstraints(hir))
        return false;

    for (size_t i = 0; i < hir.getFnCount(); i++) {
        auto &fn = hir.getFn(i);
        if (!resolveIncompleteInFn(fn))
            return false;
    }

    return !diags_.hasErrors();
}

bool Solver::runPostLower(hir::HirModule &hir) {
    for (size_t i = 0; i < hir.getFnCount(); i++) {
        auto &fn = hir.getFn(i);
        if (!resolveIncompleteInFn(fn))
            return false;
    }
    return !diags_.hasErrors();
}

bool Solver::collectGenerics() {
    // AstBuilder removed — Solver not used by modern pipeline.
    (void)ast_; (void)program_;
    return true;
}

bool Solver::checkNumericBounds(types::TypeId t) {
    if (isNumericType(t, types_))
        return true;

    auto kind = types_.kindOf(t);

    if (kind == types::TypeKind::Error)
        return false;

    if (kind == types::TypeKind::Unknown) {
        diags_.report(diagnostics::Severity::Error, diagnostics::err::TypeMismatch,
                      "cannot infer type for generic parameter", {});
        return false;
    }

    if (kind == types::TypeKind::GenericParam) {
        diags_.report(diagnostics::Severity::Error, diagnostics::err::TypeMismatch,
                      "generic parameter T has no concrete type", {});
        return false;
    }

    if (kind == types::TypeKind::Incomplete) {
        auto *inc = std::get_if<types::TypeIncomplete>(&types_.lookup(t));
        for (size_t i = 0; i < inc->arg_count; i++) {
            if (!checkNumericBounds(inc->args[i]))
                return false;
        }
        return true;
    }

    if (kind == types::TypeKind::Fn) {
        auto *fn = std::get_if<types::TypeFn>(&types_.lookup(t));
        if (!checkNumericBounds(fn->ret))
            return false;
        for (size_t i = 0; i < fn->param_count; i++) {
            if (!checkNumericBounds(fn->params[i]))
                return false;
        }
        return true;
    }

    diags_.report(diagnostics::Severity::Error, diagnostics::err::TypeMismatch,
                  "type does not satisfy numeric constraint (expected i32, f32, etc.)", {});
    return false;
}

bool Solver::checkNumericBoundsInFn(hir::HirFunction &fn) {
    if (!checkNumericBounds(fn.return_type))
        return false;
    for (size_t i = 0; i < fn.params.size(); i++) {
        if (!checkNumericBounds(fn.params[i]))
            return false;
    }
    return true;
}

bool Solver::verifyGenericConstraints(hir::HirModule &hir) {
    for (size_t i = 0; i < hir.getFnCount(); i++) {
        auto &fn = hir.getFn(i);

        // Skip functions without generic params — no constraints to verify
        bool has_generics = typeHasGenericParam(fn.return_type, types_);
        if (!has_generics) {
            for (size_t j = 0; j < fn.params.size(); j++) {
                if (typeHasGenericParam(fn.params[j], types_)) {
                    has_generics = true;
                    break;
                }
            }
        }
        if (!has_generics)
            continue;

        if (!checkNumericBoundsInFn(fn))
            return false;
    }
    return true;
}

bool Solver::resolveIncompleteInFn(hir::HirFunction &fn) {
    if (!resolveIncompleteType(fn.return_type))
        return false;
    for (size_t i = 0; i < fn.params.size(); i++) {
        if (!resolveIncompleteType(fn.params[i]))
            return false;
    }
    return true;
}

bool Solver::resolveIncompleteType(types::TypeId &t) {
    auto kind = types_.kindOf(t);
    if (kind == types::TypeKind::Incomplete) {
        auto *inc = std::get_if<types::TypeIncomplete>(&types_.lookup(t));
        for (size_t i = 0; i < inc->arg_count; i++) {
            if (!resolveIncompleteType(const_cast<types::TypeId &>(inc->args[i])))
                return false;
        }
    }
    return true;
}

std::string Solver::mangleName(std::string_view base, types::TypeId arg) {
    return std::string(base) + '<' + std::to_string(arg) + '>';
}

} // namespace zith::comptime
