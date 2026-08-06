#include "cli/terminal.hpp"
#include "diagnostic-engine.hpp"
#include "diagnostics/diagnostic.hpp"
#include "diagnostics/error-codes.hpp"
#include "memory/source-file.hpp"
#include "memory/source-map.hpp"

#include <cstdio>
#include <string>

using namespace zith::term;

namespace zith::diagnostics {
namespace {

struct LineInfo {
    size_t line_num;
    size_t line_start;
    size_t line_end;
    std::string_view text;
};

// O(log N) line lookup via the precomputed line_starts in a SourceLoc.
LineInfo lineFromSourceLoc(const memory::SourceLoc &src, size_t line, std::string_view source) {
    auto &ls = src.line_starts;
    if (line < 1 || line > ls.size())
        return {};
    size_t line_start = ls[line - 1];
    size_t line_end   = (line < ls.size()) ? ls[line] : source.size();
    return {line, line_start, line_end, source.substr(line_start, line_end - line_start)};
}

// Fallback O(n) line lookup when no SourceMap is available.
LineInfo findLine(std::string_view source, memory::ByteOffset offset) {
    size_t start = offset;
    while (start > 0 && source[start - 1] != '\n')
        start--;

    size_t end = offset;
    while (end < source.size() && source[end] != '\n')
        end++;

    size_t line_num = 1;
    for (size_t i = 0; i < start; i++) {
        if (source[i] == '\n')
            line_num++;
    }

    return {line_num, start, end, source.substr(start, end - start)};
}

static const char *severityName(Severity sev) {
    switch (sev) {
    case Severity::Error:
        return "error";
    case Severity::Warning:
        return "warning";
    case Severity::Bug:
        return "bug";
    case Severity::Note:
        return "note";
    }
    return "???";
}

} // anonymous namespace

void DiagnosticEngine::emitLabelLine(const char *label, std::string_view msg) const {
    if (use_color_)
        std::fputs(theme_.note_prefix.data(), stderr);
    std::fprintf(stderr, "   %s %.*s\n", label, static_cast<int>(msg.size()), msg.data());
    if (use_color_)
        std::fputs(ansi::reset.data(), stderr);
}

void DiagnosticEngine::emitOne(const Diagnostic &d, std::string_view source, const char *path,
                               memory::Loc loc, bool has_secondary_labels) const {
    auto info            = lookupError(d.code);
    char prefix          = info ? info->prefix : '?';
    const char *sev_name = severityName(d.severity);

    // Machine-parseable one-line format: path:line:col: severity[CODE]: message
    if (path) {
        std::fprintf(stderr, "%s:%u:%u: %s[%c%04u]: %s\n", path, loc.line, loc.col, sev_name,
                     prefix, d.code, d.message.c_str());
    } else {
        std::fprintf(stderr, "%s[%c%04u]: %s\n", sev_name, prefix, d.code, d.message.c_str());
    }

    // Rich source output (skipped when no path or invalid primary span)
    if (path && !source.empty() && d.primary.start < source.size() &&
        d.primary.end <= source.size()) {

        // File:line:col header
        if (use_color_)
            std::fputs(theme_.location.data(), stderr);
        std::fprintf(stderr, "  --> %s:%u:%u\n", path, loc.line, loc.col);
        if (use_color_)
            std::fputs(ansi::reset.data(), stderr);

        // Get line info: prefer SourceLoc (O(log N)), fallback to findLine (O(n))
        LineInfo line{};
        if (source_map_) {
            auto maybe_src = source_map_->get(d.primary.file);
            if (maybe_src.isValid())
                line = lineFromSourceLoc(maybe_src.value().get(), loc.line, source);
        }
        if (line.text.empty())
            line = findLine(source, d.primary.start);

        // Source line
        std::fputs("   |\n", stderr);
        if (use_color_)
            std::fputs(theme_.line_no.data(), stderr);
        std::fprintf(stderr, " %zu | ", line.line_num);
        if (use_color_)
            std::fputs(ansi::reset.data(), stderr);
        std::fprintf(stderr, "%.*s\n", static_cast<int>(line.text.size()), line.text.data());

        // Caret + tildes
        size_t col     = d.primary.start >= line.line_start ? d.primary.start - line.line_start : 0;
        size_t end_col = d.primary.end >= line.line_start ? d.primary.end - line.line_start : 0;
        if (end_col > line.text.size())
            end_col = line.text.size();
        if (col > line.text.size())
            col = line.text.size();

        std::fputs("   | ", stderr);
        if (use_color_)
            std::fputs(theme_.underline.data(), stderr);
        for (size_t i = 0; i < col; i++)
            std::fputc(' ', stderr);
        for (size_t i = col; i < col + 1 && i < line.text.size(); i++)
            std::fputc('^', stderr);
        for (size_t i = col + 1; i < end_col; i++)
            std::fputc('~', stderr);

        // Primary label message on caret line
        for (auto &lbl : d.labels) {
            if (lbl.span.start == d.primary.start && lbl.span.end == d.primary.end) {
                std::fprintf(stderr, " %s", lbl.message.c_str());
                break;
            }
        }
        if (use_color_)
            std::fputs(ansi::reset.data(), stderr);
        std::fputc('\n', stderr);

        // Secondary labels with their own source snippet + caret
        if (has_secondary_labels && source_map_) {
            for (auto &lbl : d.labels) {
                if (lbl.span.start == d.primary.start && lbl.span.end == d.primary.end)
                    continue;

                auto lbl_src = source_map_->get(lbl.span.file);
                if (!lbl_src.isValid())
                    continue;

                auto &lbl_src_ref = lbl_src.value().get();
                auto lbl_loc      = source_map_->loc(lbl.span);
                auto lbl_slice    = lbl_src_ref.getSlice();

                // Machine-parseable line for secondary
                std::fprintf(stderr, "%s:%u:%u: note: %s\n", lbl_src_ref.path.c_str(), lbl_loc.line,
                             lbl_loc.col, lbl.message.c_str());

                // Rich output for secondary
                auto lbl_line = lineFromSourceLoc(lbl_src_ref, lbl_loc.line, lbl_slice);
                if (lbl_line.text.empty()) {
                    lbl_line = findLine(lbl_slice, lbl.span.start);
                }

                std::fprintf(stderr, "     |\n");
                if (use_color_)
                    std::fputs(theme_.line_no.data(), stderr);
                std::fprintf(stderr, " %zu | ", lbl_line.line_num);
                if (use_color_)
                    std::fputs(ansi::reset.data(), stderr);
                std::fprintf(stderr, "%.*s\n", static_cast<int>(lbl_line.text.size()),
                             lbl_line.text.data());

                // Secondary caret
                size_t lcol = lbl.span.start >= lbl_line.line_start
                                  ? lbl.span.start - lbl_line.line_start
                                  : 0;
                size_t lend =
                    lbl.span.end >= lbl_line.line_start ? lbl.span.end - lbl_line.line_start : 0;
                if (lend > lbl_line.text.size())
                    lend = lbl_line.text.size();
                if (lcol > lbl_line.text.size())
                    lcol = lbl_line.text.size();

                std::fprintf(stderr, "     | ");
                if (use_color_)
                    std::fputs(theme_.underline.data(), stderr);
                for (size_t i = 0; i < lcol; i++)
                    std::fputc(' ', stderr);
                for (size_t i = lcol; i < lcol + 1 && i < lbl_line.text.size(); i++)
                    std::fputc('^', stderr);
                for (size_t i = lcol + 1; i < lend; i++)
                    std::fputc('~', stderr);
                std::fprintf(stderr, " %s", lbl.message.c_str());
                if (use_color_)
                    std::fputs(ansi::reset.data(), stderr);
                std::fputc('\n', stderr);
            }
        }
    }

    if (info && !info->why.empty())
        emitLabelLine("= why:", info->why);
    if (info && !info->note.empty())
        emitLabelLine("= note:", info->note);
    for (auto &s : d.suggestions)
        emitLabelLine("= help:", s);

    std::fputc('\n', stderr);
}

void DiagnosticEngine::emit() const {
    if (suppress_emit_)
        return;
    for (auto &d : this->all()) {
        if (source_map_) {
            auto maybe_src = source_map_->get(d.primary.file);
            if (maybe_src.isValid()) {
                auto &src = maybe_src.value().get();
                auto loc  = source_map_->loc(d.primary);
                emitOne(d, src.getSlice(), src.path.c_str(), loc, true);
                continue;
            }
        }
        emitOne(d, {}, nullptr, memory::Loc{}, false);
    }
}

void DiagnosticEngine::emitTo(std::string_view source_text) const {
    if (source_map_) {
        emit();
        return;
    }
    for (auto &d : this->all()) {
        auto line = findLine(source_text, d.primary.start);
        memory::Loc loc{static_cast<uint32_t>(line.line_num),
                        static_cast<uint32_t>(d.primary.start >= line.line_start
                                                  ? d.primary.start - line.line_start + 1
                                                  : 1)};
        emitOne(d, source_text, "<input>", loc, false);
    }
}

} // namespace zith::diagnostics
