#include "sema/sema-modern-utils.hpp"

#include "support/int-literal.hpp"

#include <cctype>

namespace zith::sema::modern {

bool looksInteger(std::string_view text) {
    return support::looksIntegerLiteral(text);
}

bool looksFloat(std::string_view text) {
    if (text.empty())
        return false;
    bool saw_dot = false;
    size_t i     = 0;
    if (text[0] == '-' || text[0] == '+')
        i++;
    for (; i < text.size(); ++i) {
        char c = text[i];
        if (c == '.') {
            if (saw_dot)
                return false;
            saw_dot = true;
        } else if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return saw_dot;
}

bool looksBool(std::string_view text) {
    return text == "true" || text == "false";
}

bool looksString(std::string_view text) {
    return text.size() >= 2 && text.front() == '"' && text.back() == '"';
}

bool looksChar(std::string_view text) {
    return text.size() >= 3 && text.front() == '\'' && text.back() == '\'';
}

[[nodiscard]] CastKind classifyCast(TypeKind from, TypeKind to) {
    const bool from_integer = from == TypeKind::Integer || from == TypeKind::Char;
    const bool to_integer   = to == TypeKind::Integer || to == TypeKind::Char;
    const bool from_enum    = from == TypeKind::Enum;
    if (from == to && from == TypeKind::Float)
        return CastKind::FloatToFloat;
    if ((from_integer || from_enum) && to_integer)
        return CastKind::IntToInt;
    if ((from_integer || from_enum) && to == TypeKind::Float)
        return CastKind::IntToFloat;
    if (from == TypeKind::Float && to_integer)
        return CastKind::FloatToInt;
    return CastKind::Invalid;
}

const frontend::Declaration *findDeclarationForResolved(const PerModuleSema &sema,
                                                        const session::ResolvedName &resolved) {
    const session::ModuleKey target_module =
        resolved.target.module.empty() ? sema.module : resolved.target.module;
    PerModuleSema *target =
        sema.owner != nullptr ? sema.owner->findModuleSema(target_module) : nullptr;
    if (target == nullptr)
        return nullptr;
    frontend::DeclId decl_id = resolved.declaration;
    if (!decl_id && resolved.target.localSymbol)
        decl_id = frontend::DeclId{resolved.target.localSymbol.value};
    if (!decl_id || decl_id.value > target->snapshot.declarations().size())
        return nullptr;
    return &target->snapshot.declarations()[decl_id.value - 1U];
}

} // namespace zith::sema::modern
