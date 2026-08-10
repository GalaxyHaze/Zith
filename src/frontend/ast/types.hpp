#pragma once

#include <cstdint>

struct Span {
    uint32_t start;
    uint32_t end;
    Span(uint32_t s, uint32_t e)
        : start(s), end(e) {}
};
