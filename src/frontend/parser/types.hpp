#pragma once

#include "common/memory/result.hpp"
#include "common/memory/span.hpp"

#include <cstdint>
#include <string>

using Span = common::memory::Span;

namespace sample {

struct ParserDiagnostic : common::memory::Error {
    Span span = Span(0, 0);
    std::string message;
};

struct ParseOutput {
    int count = 0;
    bool sawEnd = false;
};

} // namespace sample
