#pragma once

#include "common/memory/arena.hpp"
#include "common/memory/source-map.hpp"
#include "common/memory/string-interner.hpp"
#include "frontend/lexer/lexer.hpp"
#include "frontend/parser/types.hpp"
#include "resolution/resolution.hpp"
#include "sema/sema.hpp"

#include <string_view>
#include <vector>

namespace generated_cli {
struct Options;
}

namespace toolkit::session {

struct ZithSessionContext {
    common::memory::Arena arena{};
    common::memory::SourceMap sourceMap{};
    common::memory::StringInterner interner;
    generated_cli::Options *options = nullptr;
    std::string_view filePath{};
    std::string_view projectRoot{};
    common::memory::FileId fileId = 0;
    std::vector<std::string> stdlibRoots;
    std::vector<std::string> includeRoots;
    std::vector<std::string> systemIncludeRoots;
    std::vector<std::string> assetRoots;
    common::memory::Arena resolutionArena{};
    common::memory::StringInterner resolutionInterner;
    toolkit::resolution::ScanInfo scan;
    toolkit::resolution::ImportInfo imports;
    toolkit::resolution::ResolvedInfo resolved;
    toolkit::sema::TypeCheckedInfo checked{arena, interner};

    ZithSessionContext()
        : arena{}, sourceMap{}, interner(arena), resolutionArena{},
          resolutionInterner(resolutionArena), scan(resolutionArena),
          imports(resolutionArena, resolutionInterner),
          resolved(resolutionArena), checked(arena, interner) {}
};

} // namespace toolkit::session
