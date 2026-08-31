#pragma once

#include "frontend/frontend.hpp"
#include "sema/sema-modern.hpp"

#include <string_view>

namespace zith::sema::modern {

/// Accepts every integer literal the lexer produces, including the explicit radix
/// forms `0x`/`0c`/`0b`. Keeping this in sync with the lexer matters: a literal that
/// is lexed but not recognised here infers as `error` and silently produces no value.
bool looksInteger(std::string_view text);

bool looksFloat(std::string_view text);
bool looksBool(std::string_view text);
bool looksString(std::string_view text);
bool looksChar(std::string_view text);

/// The single decision point for `as` conversions. User-defined casts will become a new
/// branch here rather than a new call site.
enum class CastKind : uint8_t {
    Invalid,
    Identity,
    IntToInt,
    IntToFloat,
    FloatToInt,
    FloatToFloat,
    PtrToPtr
};

[[nodiscard]] CastKind classifyCast(TypeKind from, TypeKind to);

const frontend::Declaration *findDeclarationForResolved(const PerModuleSema &sema,
                                                        const session::ResolvedName &resolved);

} // namespace zith::sema::modern
