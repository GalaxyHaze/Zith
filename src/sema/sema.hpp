#pragma once

#include "common/diagnostic/diagnostic.hpp"
#include "common/memory/arena.hpp"
#include "common/memory/dyn-array.hpp"
#include "common/memory/flat-map.hpp"
#include "common/memory/string-interner.hpp"
#include "frontend/ast/ast.hpp"
#include "sema/type-table.hpp"

#include <cstdint>

namespace toolkit::session {
class CompilationSession;
}

namespace toolkit::sema {

struct TypeAnnotation {
    TypeId type = kInvalidTypeId;
    Ownership ownership = Ownership::Default;
    bool isMut = false;
};

/// Sema outcome attached to the session context. The stage itself stores a
/// `void` result, so this is the consultable annotation surface for later
/// phases and tests.
struct TypeCheckedInfo {
    TypeTable types;
    bool success = false;

    TypeCheckedInfo(common::memory::Arena &arena,
                    common::memory::StringInterner &interner)
        : types(arena, interner) {}

    [[nodiscard]] const TypeAnnotation *annotation(
        const generated_ast::AstNode *node) const noexcept;
    [[nodiscard]] TypeId typeOf(
        const generated_ast::AstNode *node) const noexcept;
    [[nodiscard]] Ownership ownershipOf(
        const generated_ast::AstNode *node) const noexcept;
    [[nodiscard]] bool isMutOf(
        const generated_ast::AstNode *node) const noexcept;
    void set(const generated_ast::AstNode *node, TypeAnnotation annotation);

private:
    common::memory::FlatMap<const void *, TypeAnnotation> annotations_;
};

/// Runs the semantic checker over a parsed program. Diagnostics are appended
/// to `diagnostics`; the outcome is also copied into `info`.
[[nodiscard]] bool typeCheckProgram(
    generated_ast::Program &program,
    common::memory::FileId fileId,
    common::memory::Arena &arena,
    common::memory::StringInterner &interner,
    common::memory::DynArray<common::diagnostic::Diagnostic> &diagnostics,
    TypeCheckedInfo &info);

[[nodiscard]] TypeCheckedInfo &checkedInfo(
    toolkit::session::CompilationSession &session);

[[nodiscard]] Ownership ownershipFromTypeExpr(
    const generated_ast::TypeExpr *type) noexcept;

} // namespace toolkit::sema
