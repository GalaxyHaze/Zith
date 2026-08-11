#pragma once

#include "common/memory/arena.hpp"
#include "common/memory/source-map.hpp"
#include "common/memory/string-interner.hpp"
#include "frontend/lexer/lexer.hpp"

#include <string_view>

namespace generated_cli {
struct Options;
}

namespace zith::session {

struct ZithSessionContext {
    common::memory::Arena arena{};
    common::memory::SourceMap sourceMap{};
    common::memory::StringInterner interner;
    generated_cli::Options *options = nullptr;
    std::string_view filePath{};
    std::string_view projectRoot{};
    common::memory::FileId fileId = 0;

    ZithSessionContext() : arena(), sourceMap(), interner(arena) {}
};

} // namespace zith::session
