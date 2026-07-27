#pragma once

#include "legacy-zith/ast/ast-builder.hpp"
#include "diagnostics/diagnostic-engine.hpp"
#include "symbols/symbol-table.hpp"
#include "types/type-intern.hpp"

namespace zith::sema {

class SemaContext {
    symbols::SymbolTable &syms_;
    types::TypeIntern &types_;
    diagnostics::DiagnosticEngine &diags_;
    ast::AstBuilder &builder_;
    types::TypeId current_fn_ret_ = types::kVoidType;

public:
    SemaContext(symbols::SymbolTable &syms, types::TypeIntern &types,
                diagnostics::DiagnosticEngine &diags, ast::AstBuilder &builder);

    symbols::SymbolTable &syms() noexcept;
    types::TypeIntern &types() noexcept;
    diagnostics::DiagnosticEngine &diags() noexcept;
    ast::AstBuilder &builder() noexcept;

    types::TypeId currentFnRet() const noexcept;
    void setCurrentFnRet(types::TypeId t) noexcept;
};

} // namespace zith::sema
