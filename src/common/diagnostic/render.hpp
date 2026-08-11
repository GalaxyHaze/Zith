#pragma once
#include "common/diagnostic/diagnostic.hpp"
#include "common/memory/dyn-array.hpp"
#include "common/memory/source-map.hpp"

#include <cstdio>

namespace common::diagnostic {

struct RenderOptions {
    bool useColor = false;
    unsigned contextLines = 0;
};

void renderDiagnostic(
    FILE *out,
    const memory::SourceMap &sourceMap,
    const Diagnostic &diag,
    const RenderOptions &options = {}
);

void renderDiagnostics(
    FILE *out,
    const memory::SourceMap &sourceMap,
    const memory::DynArray<Diagnostic> &diagnostics,
    const RenderOptions &options = {},
    unsigned maxErrors = 0
);

} // namespace common::diagnostic
