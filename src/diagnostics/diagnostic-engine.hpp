#pragma once
#include "diagnostics/color.hpp"
#include "diagnostics/diagnostic.hpp"
#include "memory/dyn-array.hpp"

#include <cstdint>
#include <span>

namespace zith::memory {
class SourceMap;
}

namespace zith::diagnostics {

class DiagnosticEngine {
    memory::DynArray<Diagnostic> diagnostics_;
    bool use_color_                = false;
    ColorTheme theme_              = colorTheme;
    memory::SourceMap *source_map_ = nullptr;
    mutable bool suppress_emit_    = false;

public:
    explicit DiagnosticEngine(memory::Arena &arena) : diagnostics_(arena) {}
    void report(Diagnostic diag);
    void report(Severity sev, uint32_t code, std::string msg, memory::Span span);
    void report(Severity sev, uint32_t code, std::string msg, memory::Span span,
                std::initializer_list<std::string> suggestions);

    void reportError(uint32_t code, std::string msg, memory::Span span);
    void reportError(uint32_t code, std::string msg, memory::Span span,
                     std::initializer_list<std::string> suggestions);

    void setColor(bool enabled) {
        use_color_ = enabled;
    }
    bool useColor() const noexcept {
        return use_color_;
    }

    void setSuppressEmit(bool s) const noexcept {
        suppress_emit_ = s;
    }

    void setSourceMap(memory::SourceMap *sm) noexcept {
        source_map_ = sm;
    }
    memory::SourceMap *sourceMap() const noexcept {
        return source_map_;
    }

    void emit() const;
    void emitTo(std::string_view source_text) const;

private:
    void emitLabelLine(const char *label, std::string_view msg) const;
    void emitOne(const Diagnostic &d, std::string_view source, const char *path, memory::Loc loc,
                 bool has_secondary_labels) const;

public:
    memory::DynArray<Diagnostic> &diagnostics() noexcept {
        return diagnostics_;
    }

    [[nodiscard]] bool hasErrors() const noexcept;
    [[nodiscard]] size_t errorCount() const noexcept;
    [[nodiscard]] std::span<const Diagnostic> all() const noexcept;
    void clear();
};

} // namespace zith::diagnostics
