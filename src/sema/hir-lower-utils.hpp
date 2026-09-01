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

/// Internal representation of `\#`. The normal output path converts it back to
/// `#`; a format-message path can keep it to distinguish escaped hashes from
/// the `#` placeholder at runtime.
inline constexpr char kFormatHashSentinel = '\x1f';

/// Decodes the C-like escape set inside string/char literal bodies. Returns false
/// (without touching `output`) when an escape is malformed or unknown. With
/// `keep_marker` false, the `\#` sentinel is restored to `#`; callers that will
/// feed a format scanner should request the sentinel so an escaped hash remains
/// distinguishable from a placeholder.
bool decodeEscapes(std::string_view text, std::string &output, bool keep_marker = false);

uint64_t internFunctionKey(memory::StringInterner &interner, std::string_view module,
                           frontend::DeclId decl);

/// Dot-separated module namespace for `module_key`, derived from the longest
/// configured root that prefixes it: `stdlib/std/io/console.zith` under the
/// stdlib root `stdlib` becomes `std.io.console`.  Falls back to the file stem.
std::string moduleNamespace(std::string_view module_key, const session::CacheKey &cache_key);

hir::HirOwnership mapHirOwnership(types::OwnershipKind kind) noexcept;

hir::HirCallEscape mapHirEscape(NraArgEscape escape) noexcept;

} // namespace zith::sema::modern
