#include "sema-context.hpp"

namespace zith::sema {

SemaContext::SemaContext(symbols::SymbolTable &syms, types::TypeIntern &types,
                         diagnostics::DiagnosticEngine &diags, ast::AstBuilder &builder)
    : syms_(syms), types_(types), diags_(diags), builder_(builder) {}

symbols::SymbolTable &SemaContext::syms() noexcept {
    return syms_;
}
types::TypeIntern &SemaContext::types() noexcept {
    return types_;
}
diagnostics::DiagnosticEngine &SemaContext::diags() noexcept {
    return diags_;
}
ast::AstBuilder &SemaContext::builder() noexcept {
    return builder_;
}
types::TypeId SemaContext::currentFnRet() const noexcept {
    return current_fn_ret_;
}
void SemaContext::setCurrentFnRet(types::TypeId t) noexcept {
    current_fn_ret_ = t;
}

} // namespace zith::sema
