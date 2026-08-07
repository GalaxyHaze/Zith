#pragma once

#include "diagnostics/diagnostic-engine.hpp"
#include "hir/hir-module.hpp"
#include "legacy-zith/ast/ast-builder.hpp"
#include "legacy-zith/ast/ast-nodes.hpp"
#include "memory/arena.hpp"
#include "symbols/symbol-table.hpp"
#include "types/type-intern.hpp"

#include "sema/modern-types.hpp"

#include <string_view>

namespace zith::comptime {

struct GenericParamInfo {
    std::string_view name;
};

struct GenericFnInfo {
    std::string_view name;
    bool is_generic           = false;
    types::TypeId return_type = types::kErrorType;
};

struct MonomorphEntry {
    std::string mangled_name;
    hir::HirFunction hir_fn;
    types::TypeId return_type = types::kErrorType;
};

class Solver {
    types::TypeIntern &types_;
    ast::AstBuilder *ast_{};
    ast::ProgramNode *program_{};
    symbols::SymbolTable &syms_ [[maybe_unused]];
    diagnostics::DiagnosticEngine &diags_;
    sema::modern::TypeTable *modern_types_ [[maybe_unused]];
    memory::Arena &hir_arena_ [[maybe_unused]];

    memory::DynArray<GenericFnInfo> generic_fns_;
    memory::DynArray<MonomorphEntry> monomorphs_;

    bool collectGenerics();
    bool checkNumericBounds(types::TypeId t);
    bool checkNumericBoundsInFn(hir::HirFunction &fn);
    bool resolveIncompleteInFn(hir::HirFunction &fn);
    bool resolveIncompleteType(types::TypeId &t);
    bool verifyGenericConstraints(hir::HirModule &hir);
    std::string mangleName(std::string_view base, types::TypeId arg);

public:
    explicit Solver(types::TypeIntern &types, ast::AstBuilder *ast, ast::ProgramNode *program,
                    symbols::SymbolTable &syms, diagnostics::DiagnosticEngine &diags,
                    memory::Arena &hir_arena, sema::modern::TypeTable *modern_types = nullptr);

    bool solve(hir::HirModule &hir);
    /// Runs only the residual post-lowering compatibility checks. Generic
    /// instantiation itself remains scheduled for 0.7.0 step-04.
    bool runPostLower(hir::HirModule &hir);
};

} // namespace zith::comptime
