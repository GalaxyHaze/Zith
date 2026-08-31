#include "sema/sema-modern-utils.hpp"
#include "sema/sema-modern.hpp"
#include <string>

namespace zith::sema::modern {

TypeId PerModuleSema::typeOfLocalByName(frontend::ScopeId scope, std::string_view name) {
    if (!scope)
        return kInvalidTypeId;
    if (scope.value > snapshot.scopes().size())
        return kInvalidTypeId;
    const auto *resolved = findResolvedBinding(name, scope);
    if (resolved) {
        if (resolved->local)
            return typeOfLocal(resolved->local);
        if (resolved->declaration)
            return typeOfDecl(resolved->declaration);
    }
    return kInvalidTypeId;
}
const frontend::Expression *PerModuleSema::nameExpression(frontend::ScopeId scope,
                                                          std::string_view name) const noexcept {
    for (const auto &expression : snapshot.expressions()) {
        if (expression.kind == frontend::ExprKind::Name && expression.text == name &&
            expression.scope == scope && findResolvedExpr(expression.id) != nullptr &&
            findResolvedExpr(expression.id)->local) {
            return &expression;
        }
    }
    return nullptr;
}
TypeId PerModuleSema::typeOfResolvedName(frontend::ExprId id) {
    const auto *resolved = findResolvedExpr(id);
    if (!resolved)
        return kInvalidTypeId;
    if (resolved->foreignConstant != nullptr)
        return lowerForeignConstantType(*resolved->foreignConstant);
    if (resolved->foreignFunction != nullptr) {
        auto &parameters = type_table.makeTypeStorage();
        for (const auto &parameter : resolved->foreignFunction->parameters)
            parameters.push(lowerForeignType(parameter));
        return type_table.internFunction(parameters,
                                         lowerForeignType(resolved->foreignFunction->result));
    }
    if (resolved->declaration) {
        if (!resolved->target.module.empty() && resolved->target.module != module) {
            const TypeId resolved_target = typeOfDeclInModule(
                resolved->target.module,
                resolved->declaration ? resolved->declaration
                                      : frontend::DeclId{resolved->target.localSymbol.value});
            if (resolved->declKind == frontend::DeclKind::Struct ||
                resolved->declKind == frontend::DeclKind::Enum ||
                resolved->declKind == frontend::DeclKind::Union ||
                resolved->declKind == frontend::DeclKind::TypeAlias) {
                if (const auto *decl = declarationForResolved(*resolved)) {
                    if (const TypeId named = type_table.lookupNamed(decl->name))
                        return named;
                }
            }
            return resolved_target;
        }
        return typeOfDecl(resolved->declaration);
    }
    if (resolved->local) {
        if (!resolved->target.module.empty() && resolved->target.module != module)
            return typeOfDeclInModule(resolved->target.module,
                                      frontend::DeclId{resolved->target.localSymbol.value});
        return typeOfLocal(resolved->local);
    }
    if (!resolved->target.module.empty() && resolved->target.localSymbol) {
        const TypeId resolved_target = typeOfDeclInModule(
            resolved->target.module, frontend::DeclId{resolved->target.localSymbol.value});
        if (resolved->declKind == frontend::DeclKind::Struct ||
            resolved->declKind == frontend::DeclKind::Enum ||
            resolved->declKind == frontend::DeclKind::Union ||
            resolved->declKind == frontend::DeclKind::TypeAlias) {
            if (const auto *decl = declarationForResolved(*resolved)) {
                if (const TypeId named = type_table.lookupNamed(decl->name))
                    return named;
            }
        }
        if (resolved->target.module == module)
            return typeOfDecl(frontend::DeclId{resolved->target.localSymbol.value});
        return resolved_target;
    }
    return kInvalidTypeId;
}
TypeId PerModuleSema::typeOfDeclInModule(session::ModuleKey target_module,
                                         frontend::DeclId id) const noexcept {
    if (!owner)
        return kInvalidTypeId;
    const auto *target = owner->findModuleSema(target_module);
    if (!target)
        return kInvalidTypeId;
    const auto *target_decl = id && id.value <= target->snapshot.declarations().size()
                                  ? &target->snapshot.declarations()[id.value - 1U]
                                  : nullptr;
    const bool named_type =
        target_decl != nullptr && (target_decl->kind == frontend::DeclKind::Struct ||
                                   target_decl->kind == frontend::DeclKind::Enum ||
                                   target_decl->kind == frontend::DeclKind::Union ||
                                   target_decl->kind == frontend::DeclKind::TypeAlias);
    TypeId current = kInvalidTypeId;
    if (named_type && target_decl != nullptr)
        current = type_table.lookupNamed(target_decl->name);
    if (target->typed_map.declTypes.get(id.value) == nullptr ||
        (named_type && (target->typeOfDecl(id) == current ||
                        type_table.kindOf(resolve(target->typeOfDecl(id))) == TypeKind::Unknown))) {
        // Cross-module lookups can run before the consumer module's checks,
        // but module type lowering is done in order. If a consumer is checked
        // before its import target, lower the target's declarations eagerly so
        // imported type symbols have concrete TypeIds.
        const_cast<PerModuleSema *>(target)->prepareTypes();
    }
    const TypeId result = target->typeOfDecl(id);
    return result;
}
const session::ResolvedName *PerModuleSema::findResolvedExpr(frontend::ExprId id) const noexcept {
    if (!id || id.value > snapshot.expressions().size())
        return nullptr;
    return session::lookupExprResolution(resolution, id);
}
std::string_view PerModuleSema::sourceText(frontend::TextSpan span) const noexcept {
    if (span.end <= span.start || span.end > snapshot.source().size())
        return {};
    return std::string_view(snapshot.source()).substr(span.start, span.size());
}
memory::Span PerModuleSema::toMemorySpan(frontend::TextSpan span) const noexcept {
    return memory::Span{fileId, span.start, span.end};
}

} // namespace zith::sema::modern
