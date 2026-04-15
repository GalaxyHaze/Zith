// impl/diagnostics/diagnostics.cpp — Diagnostic system implementation
//
// Centralizes all diagnostic emission and printing logic.
// Replaces scattered fprintf/printf calls in parser_utils.cpp and elsewhere.
#include "diagnostics.hpp"
#include <cstring>
#include <cstdio>
#include <cstdlib>

// ============================================================================
// Internal Helpers
// ============================================================================

// Finds the start and length of a specific line number (1-based)
static bool find_source_line(const char *source, size_t source_len,
                             size_t line_num, const char **out_start, size_t *out_len) {
    const char *p = source;
    const char *end = source + source_len;
    size_t cur = 1;

    // Move pointer to the start of the target line
    while (p < end && cur < line_num) {
        if (*p++ == '\n') cur++;
    }
    if (cur != line_num) return false;

    const char *line_start = p;
    // Find the end of the line
    while (p < end && *p != '\n') p++;

    *out_start = line_start;
    *out_len = (size_t)(p - line_start);
    return true;
}

static const char *severity_label(ZithDiagSeverity s) {
    switch (s) {
        case ZITH_DIAG_ERROR:   return "error";
        case ZITH_DIAG_WARNING: return "warning";
        case ZITH_DIAG_NOTE:    return "note";
        case ZITH_DIAG_INFO:    return "info";
        default:                    return "info";
    }
}

// ============================================================================
// C API — zith_diag_print_all
// ============================================================================

void zith_diag_print_all(const ZithDiagList *diags, const char *source,
                             size_t source_len, const char *filename) {
    if (!diags || diags->count == 0) return;

    for (size_t i = 0; i < diags->count; ++i) {
        const ZithDiagnostic *d = &diags->items[i];

        // 1. Print the Header: file:line:col: severity: message
        fprintf(stderr, "%s:%zu:%zu: %s: %s\n",
                filename ? filename : "<input>",
                d->loc.line, d->loc.index,
                severity_label(d->severity), d->message);

        // 2. Print the Source Line and Caret
        if (source && source_len > 0) {
            const char *line_ptr = nullptr;
            size_t line_len = 0;

            if (find_source_line(source, source_len, d->loc.line, &line_ptr, &line_len)) {
                // Print the actual code line
                fprintf(stderr, "  %.*s\n", (int)line_len, line_ptr);

                // Print the caret (^^^) pointing to the error
                fprintf(stderr, "  ");
                size_t col = d->loc.index;
                for (size_t c = 0; c < col && c < line_len; ++c) {
                    fputc(line_ptr[c] == '\t' ? '\t' : ' ', stderr);
                }
                fprintf(stderr, "^\n");
            }
        }
    }

    // 3. Print Summary
    size_t errors = 0, warnings = 0;
    for (size_t i = 0; i < diags->count; ++i) {
        if (diags->items[i].severity == ZITH_DIAG_ERROR)   errors++;
        else if (diags->items[i].severity == ZITH_DIAG_WARNING) warnings++;
    }

    if (errors > 0 || warnings > 0) {
        fprintf(stderr, "\n%s: ", filename ? filename : "<input>");
        if (errors)   fprintf(stderr, "%zu error(s)", errors);
        if (errors && warnings) fprintf(stderr, ", ");
        if (warnings) fprintf(stderr, "%zu warning(s)", warnings);
        fprintf(stderr, "\n\n");
    }
}

// ============================================================================
// C++ DiagManager Implementation
// ============================================================================

void DiagManager::emit(ZithSourceLoc loc, ZithDiagSeverity severity, const char *msg) {
    // Grow the diagnostic list if needed
    if (diags_.count >= diags_.capacity) {
        size_t new_cap = diags_.capacity == 0 ? 8 : diags_.capacity * 2;
        auto *buf = static_cast<ZithDiagnostic *>(
            arena_ ? zith_arena_alloc(arena_, new_cap * sizeof(ZithDiagnostic))
                   : std::malloc(new_cap * sizeof(ZithDiagnostic)));
        if (!buf) return;

        if (diags_.items) {
            if (arena_) {
                std::memcpy(buf, diags_.items, diags_.count * sizeof(ZithDiagnostic));
            } else {
                std::memcpy(buf, diags_.items, diags_.count * sizeof(ZithDiagnostic));
                std::free(diags_.items);
            }
        }
        diags_.items = buf;
        diags_.capacity = new_cap;
    }

    ZithDiagnostic d;
    d.message = (arena_ && msg) ? zith_arena_strdup(arena_, msg) : msg;
    d.loc = loc;
    d.severity = severity;
    diags_.items[diags_.count++] = d;

    if (severity == ZITH_DIAG_ERROR) had_error_ = true;
}

void DiagManager::error(ZithSourceLoc loc, const char *msg) {
    emit(loc, ZITH_DIAG_ERROR, msg);
}

void DiagManager::warning(ZithSourceLoc loc, const char *msg) {
    emit(loc, ZITH_DIAG_WARNING, msg);
}

void DiagManager::note(ZithSourceLoc loc, const char *msg) {
    emit(loc, ZITH_DIAG_NOTE, msg);
}

void DiagManager::info(const char *msg) {
    // Info messages don't have source location — print directly
    printf("[*] %s\n", msg);
}

void DiagManager::print_all(const char *source, size_t source_len,
                            const char *filename) const {
    zith_diag_print_all(&diags_, source, source_len, filename);
}

void DiagManager::print_summary(const char *filename) const {
    size_t errors = 0, warnings = 0;
    for (size_t i = 0; i < diags_.count; ++i) {
        if (diags_.items[i].severity == ZITH_DIAG_ERROR)   errors++;
        else if (diags_.items[i].severity == ZITH_DIAG_WARNING) warnings++;
    }

    if (errors > 0 || warnings > 0) {
        fprintf(stderr, "\n%s: ", filename);
        if (errors)   fprintf(stderr, "%zu error(s)", errors);
        if (errors && warnings) fprintf(stderr, ", ");
        if (warnings) fprintf(stderr, "%zu warning(s)", warnings);
        fprintf(stderr, "\n\n");
    }
}

// ============================================================================
// Debug output helpers
// ============================================================================

#ifndef ZITH_NO_DEBUG

#include <cstdarg>

void debug_print(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

void debug_println(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
}

void debug_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

#endif // ZITH_NO_DEBUG

// ============================================================================
// I/O error reporting
// ============================================================================

void zith_io_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[zith] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}
