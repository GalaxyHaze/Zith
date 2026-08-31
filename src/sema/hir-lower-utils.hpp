#pragma once

#include "frontend/frontend.hpp"
#include "hir/hir-attrs.hpp"
#include "memory/string-interner.hpp"
#include "sema/nra-facts.hpp"
#include "session/frontend-context.hpp"
#include "types/type-kind.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace zith::sema::modern {

/// Decodes the C-like escape set inside string/char literal bodies. Returns false
/// (without touching `output`) when an escape is malformed or unknown.
bool decodeEscapes(std::string_view text, std::string &output);

uint64_t internFunctionKey(memory::StringInterner &interner, std::string_view module,
                           frontend::DeclId decl);

/// Dot-separated module namespace for `module_key`, derived from the longest
/// configured root that prefixes it: `stdlib/std/io/console.zith` under the
/// stdlib root `stdlib` becomes `std.io.console`.  Falls back to the file stem.
std::string moduleNamespace(std::string_view module_key, const session::CacheKey &cache_key);

hir::HirOwnership mapHirOwnership(types::OwnershipKind kind) noexcept;

hir::HirCallEscape mapHirEscape(NraArgEscape escape) noexcept;

} // namespace zith::sema::modern
