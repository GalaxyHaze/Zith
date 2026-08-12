#include "common/diagnostic/render.hpp"

#include "common/diagnostic/diagnostic.hpp"
#include "diagnostic/error-info.hpp"
#include "common/memory/dyn-array.hpp"
#include "common/memory/source-file.hpp"
#include "common/memory/source-map.hpp"
#include "common/memory/span.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

namespace common::diagnostic {
namespace {

constexpr std::string_view resetColor = "\x1b[0m";
constexpr std::string_view errorColor = "\x1b[31m";
constexpr std::string_view warningColor = "\x1b[33m";
constexpr std::string_view noteColor = "\x1b[36m";

auto severityName(Severity severity) -> std::string_view {
    switch (severity) {
    case Severity::Note:
        return "note";
    case Severity::Warning:
        return "warning";
    case Severity::Error:
        return "error";
    }
    return "error";
}

auto severityColor(Severity severity) -> std::string_view {
    switch (severity) {
    case Severity::Note:
        return noteColor;
    case Severity::Warning:
        return warningColor;
    case Severity::Error:
        return errorColor;
    }
    return errorColor;
}

void writeSeverity(FILE *out, Severity severity, bool useColor) {
    if (useColor) {
        const std::string_view color = severityColor(severity);
        std::fwrite(color.data(), 1, color.size(), out);
        std::fputs(severityName(severity).data(), out);
        std::fwrite(resetColor.data(), 1, resetColor.size(), out);
    } else {
        std::fputs(severityName(severity).data(), out);
    }
}

auto lineWidth(unsigned line) -> unsigned {
    unsigned width = 1;
    while (line >= 10) {
        line /= 10;
        ++width;
    }
    return width;
}

void padLineNo(FILE *out, unsigned line, unsigned width) {
    const unsigned digits = lineWidth(line);
    for (unsigned i = digits; i < width; ++i)
        std::fputc(' ', out);
    std::fprintf(out, "%u", line);
}

void writeLine(FILE *out, std::string_view line) {
    if (const auto newline = line.find('\n'); newline != std::string_view::npos)
        line = line.substr(0, newline);
    if (!line.empty() && line.back() == '\r')
        line.remove_suffix(1);
    std::fwrite(line.data(), 1, line.size(), out);
}

void writeSourceContext(
    FILE *out,
    const memory::SourceLoc &source,
    const memory::SourceSpan &fileSpan,
    memory::Loc loc,
    unsigned contextLines,
    std::string_view afterCaret = {}
) {
    const std::string_view content = source.slice();
    if (loc.line == 0 || loc.col == 0)
        return;

    const size_t lineIndex = static_cast<size_t>(loc.line - 1);
    if (lineIndex >= source.line_starts.size() || source.line_starts.empty())
        return;

    const size_t first = lineIndex > contextLines ? lineIndex - contextLines : 0;
    const size_t last = std::min(lineIndex + contextLines, source.line_starts.size() - 1);
    const unsigned width = lineWidth(static_cast<unsigned>(last + 1));

    for (size_t i = first; i <= last; ++i) {
        const size_t start = source.line_starts[i];
        const std::string_view line = content.substr(start);
        std::fputs("  ", out);
        padLineNo(out, static_cast<unsigned>(i + 1), width);
        std::fputs(" | ", out);
        writeLine(out, line);
        std::fputc('\n', out);

        if (i == lineIndex) {
            const size_t consumed = loc.col > 1 ? loc.col - 1 : 0;
            const size_t lineEnd = line.find('\n');
            const size_t lineLength = lineEnd == std::string_view::npos ? line.size() : lineEnd;
            const size_t remaining = lineLength > consumed ? lineLength - consumed : 0;
            const size_t caretWidth = std::max<std::size_t>(1, std::min<std::size_t>(fileSpan.span.len(), remaining));

            std::fputs("  ", out);
            std::fprintf(out, "%*s", static_cast<int>(width), "");
            std::fputs(" | ", out);
            for (size_t j = 0; j < consumed; ++j)
                std::fputc(' ', out);
            std::fputc('^', out);
            for (size_t j = 1; j < caretWidth; ++j)
                std::fputc('~', out);
            if (!afterCaret.empty()) {
                std::fputc(' ', out);
                std::fwrite(afterCaret.data(), 1, afterCaret.size(), out);
            }
            std::fputc('\n', out);
        }
    }
}

} // namespace

void renderDiagnostic(
    FILE *out,
    const memory::SourceMap &sourceMap,
    const Diagnostic &diag,
    const RenderOptions &options
) {
    const memory::Loc loc = sourceMap.loc(diag.span);
    const auto pathView = sourceMap.view(diag.span.file);
    const std::string_view path = pathView ? pathView.value() : std::string_view{"<unknown>"};
    const auto snippet = sourceMap.snippet(diag.span);
    const std::string_view lexeme = snippet ? snippet.value() : std::string_view{"<invalid span>"};

    std::fprintf(out, "%.*s:%u:%u: ", static_cast<int>(path.size()), path.data(), loc.line, loc.col);
    writeSeverity(out, diag.severity, options.useColor);
    if (diag.code != 0) {
        const ErrorInfo &info = errorInfo(diag.code);
        const std::string rendered = ErrorTemplate{&info}.render(diag.message, lexeme);
        std::fprintf(out, ": E%u: %s\n", diag.code, rendered.c_str());
        std::fprintf(
            out,
            "  --> %.*s:%u:%u\n",
            static_cast<int>(path.size()),
            path.data(),
            loc.line,
            loc.col
        );
        if (options.contextLines != 0) {
            const auto file = sourceMap.get(diag.span.file);
            if (file)
                writeSourceContext(out, file->get(), diag.span, loc, options.contextLines, rendered);
        }
        if (!info.note.empty())
            std::fprintf(out, "  = note: %.*s\n", static_cast<int>(info.note.size()), info.note.data());
        for (const Note &note : diag.notes)
            if (!note.message.empty())
                std::fprintf(out, "  = note: %s\n", note.message.c_str());
    } else {
        std::fprintf(out, ": %s\n", diag.message.c_str());
        for (const Note &note : diag.notes)
            std::fprintf(out, "  note: %s\n", note.message.c_str());
        if (options.contextLines != 0) {
            const auto file = sourceMap.get(diag.span.file);
            if (file)
                writeSourceContext(out, file->get(), diag.span, loc, options.contextLines);
        }
    }
}

void renderDiagnostics(
    FILE *out,
    const memory::SourceMap &sourceMap,
    const memory::DynArray<Diagnostic> &diagnostics,
    const RenderOptions &options,
    unsigned maxErrors
) {
    unsigned rendered = 0;
    for (const Diagnostic &diag : diagnostics) {
        if (maxErrors != 0 && rendered >= maxErrors)
            break;
        if (rendered != 0)
            std::fputc('\n', out);
        renderDiagnostic(out, sourceMap, diag, options);
        ++rendered;
    }
}

} // namespace common::diagnostic
