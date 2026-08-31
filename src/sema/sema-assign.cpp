#include "sema/sema-modern-utils.hpp"
#include "sema/sema-modern.hpp"
#include <algorithm>

namespace zith::sema::modern {

frontend::ExprId PerModuleSema::assignmentRoot(frontend::ExprId id) const noexcept {
    // Walk the place expression down to the name it is rooted at, so a write to
    // `p.inner.x` is attributed to the binding `p`.
    for (unsigned guard = 0; guard < 64U; ++guard) {
        if (!id || id.value > snapshot.expressions().size())
            return {};
        const auto &expr = snapshot.expressions()[id.value - 1U];
        switch (expr.kind) {
        case frontend::ExprKind::Name:
            return id;
        case frontend::ExprKind::Field:
        case frontend::ExprKind::Arrow:
        case frontend::ExprKind::Index:
            if (expr.operands.empty())
                return {};
            id = expr.operands[0];
            break;
        case frontend::ExprKind::Unary:
            if (expr.text != "*" || expr.operands.empty())
                return {};
            id = expr.operands[0];
            break;
        default:
            return {};
        }
    }
    return {};
}
void PerModuleSema::checkAssignableOwnership(frontend::ExprId target, frontend::TextSpan span) {
    frontend::ExprId root = assignmentRoot(target);
    if (!root)
        return;

    // `self: view Owner` / `p: view Owner` lower to `*view Owner`, so both
    // `self->x = ...` and dot-style `p.x = ...` report a read-only write.
    [&]() {
        for (frontend::ExprId current = target;
             current && current.value <= snapshot.expressions().size() && current != root;) {
            const auto &cursor = snapshot.expressions()[current.value - 1U];
            if (cursor.kind == frontend::ExprKind::Arrow) {
                root = cursor.operands[0];
                return true;
            }
            if (cursor.kind != frontend::ExprKind::Field &&
                cursor.kind != frontend::ExprKind::Index)
                break;
            if (cursor.operands.empty())
                break;
            current = cursor.operands[0];
        }
        return false;
    }();

    const TypeId declared = typeOfExpr(root) ? typeOfExpr(root) : typeOfResolvedName(root);
    if (!declared)
        return;

    const auto *qual = type_table.qualified(type_table.canonical(declared));
    bool is_view     = qual != nullptr && qual->ownership == types::OwnershipKind::View;
    if (!is_view && type_table.kindOf(resolve(declared)) == TypeKind::Pointer) {
        // The root may be `*view Owner`; strip the pointer layer before
        // checking the pointee qualifier.
        const TypeId resolved_root = resolve(declared);
        const auto *pointer        = type_table.pointer(resolved_root);
        if (pointer != nullptr) {
            const auto *pointee_qual = type_table.qualified(type_table.canonical(pointer->pointee));
            is_view =
                pointee_qual != nullptr && pointee_qual->ownership == types::OwnershipKind::View;
        }
    }
    if (!is_view)
        return;
    const auto &root_expr = snapshot.expressions()[root.value - 1U];
    report(span, "cannot write through '" + root_expr.text + "': a 'view' binding is read-only",
           diagnostics::err::WriteThroughView);
}
void PerModuleSema::checkImmutableRootFieldWrite(frontend::ExprId target, frontend::TextSpan span) {
    const frontend::ExprId root = assignmentRoot(target);
    const auto *root_resolved   = root ? findResolvedExpr(root) : nullptr;
    if (root_resolved == nullptr || (root_resolved->bindingKind != frontend::BindingKind::Let &&
                                     root_resolved->bindingKind != frontend::BindingKind::Const))
        return;
    // Parameters are immutable by default. `var p` is locally mutable; `lend`
    // and explicit pointer/`view` receivers keep their own ownership checks.
    if (root_resolved->local && root_resolved->declKind == frontend::DeclKind::Function &&
        root_resolved->bindingKind == frontend::BindingKind::Let) {
        TypeId param_type     = typeOfLocal(root_resolved->local);
        const TypeId stripped = type_table.stripQualifiers(param_type);
        if (type_table.kindOf(resolve(stripped)) == TypeKind::Pointer) {
            // Explicit `self: *Owner` and `self: lend/view Owner` stay mutable
            // through the pointer path. Bare `self`/`var self` map to *Owner
            // too, but bare self is read-only and var self is mutable; the
            // binding kind distinguishes them.
            if (isBorrowParamType(param_type))
                return;
            bool explicit_pointer = false;
            for (const auto &decl : snapshot.declarations()) {
                if (decl.kind != frontend::DeclKind::Function)
                    continue;
                for (size_t index = 0; index < decl.parameters.size(); ++index) {
                    if (decl.parameters[index].id != root_resolved->local)
                        continue;
                    // Bare `self` is read-only even though it lowers to *Owner.
                    explicit_pointer =
                        !(decl.parameters[index].name == "self" && !decl.parameters[index].type);
                    break;
                }
                if (explicit_pointer)
                    break;
            }
            if (explicit_pointer)
                return;
        } else if (const auto *param_qual =
                       type_table.qualified(type_table.canonical(param_type))) {
            // `lend`/`view` parameters opt out of the default immutable local
            // binding; view is still reported by `checkAssignableOwnership`.
            if (param_qual->ownership == types::OwnershipKind::Lend ||
                param_qual->ownership == types::OwnershipKind::View)
                return;
        }
    }
    bool has_field_path = false;
    for (frontend::ExprId current = target;
         current && current.value <= snapshot.expressions().size() && current != root;) {
        const auto &cursor = snapshot.expressions()[current.value - 1U];
        if (cursor.kind != frontend::ExprKind::Field && cursor.kind != frontend::ExprKind::Arrow &&
            cursor.kind != frontend::ExprKind::Index)
            break;
        if (cursor.operands.empty())
            break;
        has_field_path = true;
        current        = cursor.operands[0];
    }
    if (!has_field_path)
        return;
    report(span, "Zith--: cannot write through immutable binding '" + root_resolved->name + "'",
           diagnostics::err::UnsupportedSyntax);
}
TypeId PerModuleSema::inferAssign(frontend::ExprId id) {
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.operands.size() < 2)
        return error_type;
    checkMovedRoot(expr);
    // Mark a direct bind as initialized before re-inferring the left name on
    // the standalone sweep; the later read (in the same expression statement)
    // is then allowed without an extra flow pass.
    if (const auto *lhs_resolved = findResolvedExpr(expr.operands[0]);
        lhs_resolved != nullptr && lhs_resolved->local)
        uninitializedLocals_.erase(lhs_resolved->local.value);
    TypeId left_type = kInvalidTypeId;
    prepareLValueIndexTypes(expr.operands[0]);
    const auto *left_resolved = findResolvedExpr(expr.operands[0]);
    if (left_resolved != nullptr) {
        const auto isFirstAssignmentForLet = [&]() {
            if (!left_resolved || !left_resolved->local)
                return false;
            if (std::find(typeInferredByAssignment_.begin(), typeInferredByAssignment_.end(),
                          left_resolved->local.value) != typeInferredByAssignment_.end())
                return false;
            for (const auto &statement : snapshot.statements()) {
                if (statement.kind == frontend::StmtKind::Binding &&
                    statement.binding.id == left_resolved->local &&
                    !statement.binding.initializer) {
                    return true;
                }
            }
            return false;
        };
        const bool first_assignment = isFirstAssignmentForLet();
        if ((left_resolved->bindingKind == frontend::BindingKind::Let ||
             left_resolved->bindingKind == frontend::BindingKind::Const)) {
            if (first_assignment && left_resolved->bindingKind == frontend::BindingKind::Let) {
                // `let x; x = e;` is allowed once; it supplies the variable's type.
                typeInferredByAssignment_.push_back(left_resolved->local.value);
            } else {
                report(expr.span, "Zith--: cannot assign to an immutable let/const binding",
                       diagnostics::err::UnsupportedSyntax);
            }
        }
        if (left_resolved->foreignConstant != nullptr) {
            report(expr.span, "Zith--: cannot assign to an imported C constant",
                   diagnostics::err::UnsupportedSyntax);
        } else if (left_resolved->declaration &&
                   left_resolved->declKind == frontend::DeclKind::Variable &&
                   left_resolved->bindingKind == frontend::BindingKind::Const) {
            report(expr.span, "Zith--: cannot assign to a const global",
                   diagnostics::err::UnsupportedSyntax);
        }
    }
    if (left_resolved != nullptr && left_resolved->local)
        left_type = typeOfLocal(left_resolved->local);
    if (!left_type && left_resolved != nullptr && left_resolved->declaration)
        left_type = typeOfDecl(left_resolved->declaration);
    if (!left_type)
        left_type = inferExpr(expr.operands[0]);
    if (!left_type)
        left_type = error_type;

    if (resolve(left_type) == invalid_type) {
        // A binding declared `let x;` is typed by its first assignment.
        TypeId narrowed = inferExpr(expr.operands[1]);
        if (narrowed) {
            if (left_resolved != nullptr && left_resolved->local)
                setLocalType(left_resolved->local, narrowed);
            left_type = narrowed;
        }
    }
    if (expr.operands[0] && !typeOfExpr(expr.operands[0])) {
        (void)inferExpr(expr.operands[0]);
    }
    TypeId right_type                = inferExpr(expr.operands[1]);
    const bool rhs_is_escaping_alias = pointerAliasEscapesScope(expr.operands[1]);
    const bool direct_pointer_rebind =
        expr.operands[0] && expr.operands[0].value <= snapshot.expressions().size() &&
        snapshot.expressions()[expr.operands[0].value - 1U].kind == frontend::ExprKind::Name &&
        isPointerStorageType(left_type);
    if (direct_pointer_rebind && left_resolved != nullptr && left_resolved->local)
        escapingPointerLocals_.erase(left_resolved->local.value);
    // A successful `[]char -> *char` coercion makes the assignment an explicit
    // pointer store. Re-check escapes after the coercion so a slice stored
    // through `p: *char = s` reports E4008 even though the source expression
    // itself is not an address-of/ptrOf.
    const bool rhs_can_coerce = coerceValue(expr.operands[1], left_type, right_type);
    const bool rhs_escapes_after_coercion =
        rhs_can_coerce && pointerAliasEscapesScope(expr.operands[1]);
    if ((rhs_is_escaping_alias || rhs_escapes_after_coercion) && !direct_pointer_rebind) {
        report(expr.span, "pointer to local storage cannot escape the current scope",
               diagnostics::err::PointerEscapesScope);
    } else if (pointerAliasEscapesScope(expr.operands[0])) {
        report(expr.span, "pointer to local storage cannot escape the current scope",
               diagnostics::err::PointerEscapesScope);
    }
    checkAssignableOwnership(expr.operands[0], expr.span);
    checkImmutableRootFieldWrite(expr.operands[0], expr.span);
    TypeId result = left_type;
    if (rhs_escapes_after_coercion) {
        // Coercion already ran; keep the reported alias error and still return
        // the declared type so lowering can continue only when the rest of the
        // module has no errors.
        result = left_type;
    } else if (!coerceValue(expr.operands[1], left_type, right_type)) {
        reportCoercionFailure(expr.span, left_type, right_type,
                              "assignment between incompatible types");
        result = error_type;
    }
    return result;
}
void PerModuleSema::checkMovedRoot(const frontend::Expression &target) {
    if (target.kind != frontend::ExprKind::Assign || target.operands.size() < 2U)
        return;
    const frontend::ExprId root = assignmentRoot(target.operands[0]);
    if (!root)
        return;
    const auto *resolved = findResolvedExpr(root);
    if (resolved == nullptr || !resolved->local || !movedLocals_.contains(resolved->local.value))
        return;

    bool direct_rebind = target.operands[0] == root;
    if (direct_rebind) {
        movedLocals_.erase(resolved->local.value);
        return;
    }
    const auto &root_expr = snapshot.expressions()[root.value - 1U];
    if (resolved->local) {
        const TypeId local_type = typeOfLocal(resolved->local);
        const TypeId stripped   = type_table.stripQualifiers(local_type);
        if (type_table.kindOf(resolve(stripped)) == TypeKind::Pointer) {
            // An address-of pointer can be written through without reviving the
            // original binding. A store into the pointer is not a rebind of the
            // move-with-address local itself.
            return;
        }
    }
    report(target.span,
           "cannot assign through '" + root_expr.text + "' after it was moved by a previous call",
           diagnostics::err::UseAfterMove);
}
bool PerModuleSema::pointerAliasEscapesScope(frontend::ExprId id) const {
    if (!id || id.value > snapshot.expressions().size())
        return false;
    if (escapingPointerExprs_.contains(id.value))
        return true;
    const auto &expr = snapshot.expressions()[id.value - 1U];
    if (expr.kind == frontend::ExprKind::Name) {
        const auto *resolved = findResolvedExpr(id);
        if (resolved != nullptr && resolved->local &&
            escapingPointerLocals_.contains(resolved->local.value))
            return true;
    }
    if (expr.kind == frontend::ExprKind::Call || expr.kind == frontend::ExprKind::MacroCall ||
        expr.kind == frontend::ExprKind::DockCall) {
        // A by-value argument is a temporary borrow. The call result is owned
        // by the callee (or a C interop pointer), not by caller storage.
        return false;
    }
    if (expr.text == "&" ||
        (expr.kind == frontend::ExprKind::LayoutIntrinsic && expr.text == "ptrOf"))
        return true;
    if (expr.kind == frontend::ExprKind::Unary && expr.text == "*")
        return false;
    for (const auto operand : expr.operands) {
        if (pointerAliasEscapesScope(operand))
            return true;
    }
    // A local initialized from an escaping pointer remains an alias.
    for (const auto &statement : snapshot.statements()) {
        if (statement.kind != frontend::StmtKind::Binding)
            continue;
        if (statement.binding.id.value == 0U)
            continue;
        if (statement.binding.initializer == id &&
            escapingPointerLocals_.contains(statement.binding.id.value))
            return true;
    }
    return false;
}
bool PerModuleSema::isPointerStorageType(TypeId id) const {
    if (!id)
        return false;
    TypeId resolved = type_table.stripQualifiers(id);
    if (type_table.kindOf(resolved) == TypeKind::Pointer)
        return true;
    const auto *opt = type_table.optional(resolved);
    return opt != nullptr &&
           type_table.kindOf(type_table.stripQualifiers(opt->inner)) == TypeKind::Pointer;
}
bool PerModuleSema::containedInRawRead(frontend::ExprId id) const {
    if (!id)
        return false;
    for (const auto &expr : snapshot.expressions()) {
        if (expr.kind != frontend::ExprKind::Name || !expr.isRawName)
            continue;
        if (rawRootName(expr) == id)
            return true;
    }
    // `raw a[i]` and `raw a[lo..hi]` mark the container expression instead of
    // the underlying name, so re-check those unchecked reads too.
    for (const auto &expr : snapshot.expressions()) {
        if (!expr.is_raw)
            continue;
        if (expr.kind != frontend::ExprKind::Index && expr.kind != frontend::ExprKind::SliceRange &&
            expr.kind != frontend::ExprKind::Cast)
            continue;
        if (expr.operands.empty())
            continue;
        if (assignmentRoot(expr.operands[0]) == id)
            return true;
    }
    return false;
}
frontend::ExprId PerModuleSema::rawRootName(const frontend::Expression &expr) const noexcept {
    const frontend::ExprId root = assignmentRoot(expr.id);
    if (root)
        return root;
    return expr.id;
}

} // namespace zith::sema::modern
