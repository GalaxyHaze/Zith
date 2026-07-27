#pragma once

#include "legacy-zith/lexer/token.hpp"

#include <initializer_list>

namespace zith::parser::recovery {

using SyncSet = std::initializer_list<lexer::TokenKind>;

inline void panic(lexer::TokenStream &stream, SyncSet sync_tokens = {}) {
    while (!stream.is_empty()) {
        auto &t = stream.peek();
        if (t.is_eof())
            return;
        if (t.punc == ';' || t.punc == '}')
            return;
        for (auto k : sync_tokens)
            if (t.kind == k)
                return;
        stream.advance();
    }
}

} // namespace zith::parser::recovery
