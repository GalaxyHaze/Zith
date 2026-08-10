#pragma once

#include "common/arena.hpp"
#include "common/source-map.hpp"
#include "common/string-interner.hpp"
#include "frontend/lexer/lexer.hpp"

#include <string_view>

namespace generated_cli {
struct Options;
}

namespace zith::session {

struct ZithSessionContext {
    memory::Arena arena{};
    memory::SourceMap sourceMap{};
    memory::StringInterner interner;
    generated_cli::Options *options = nullptr;
    std::string_view filePath{};
    std::string_view projectRoot{};
    memory::FileId fileId = 0;

    ZithSessionContext() : arena(), sourceMap(), interner(arena) {}
};

} // namespace zith::session
