// impl/diagnostics/diagnostics.hpp — Centralized diagnostic system
//
// Replaces scattered fprintf/printf calls with a unified DiagManager
// that tracks errors, warnings, and notes with source context.
#pragma once

#include <kalidous/kalidous.hpp>
#include <cstddef>
#include <cstdio>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Diagnostic severity and structure
// ============================================================================

typedef enum KalidousDiagSeverity {
    KALIDOUS_DIAG_ERROR   = 0,
    KALIDOUS_DIAG_WARNING = 1,
    KALIDOUS_DIAG_NOTE    = 2,
    KALIDOUS_DIAG_INFO    = 3,
} KalidousDiagSeverity;

typedef struct KalidousDiagnostic {
    const char *message;           // interned in arena
    KalidousSourceLoc loc;
    KalidousDiagSeverity severity;
} KalidousDiagnostic;

typedef struct KalidousDiagList {
    KalidousDiagnostic *items;
    size_t count;
    size_t capacity;
} KalidousDiagList;

// ============================================================================
// C API — diagnostic emission and printing
// ============================================================================

// Print all diagnostics with source context to stderr
void kalidous_diag_print_all(const KalidousDiagList *diags,
                             const char *source, size_t source_len,
                             const char *filename);

#ifdef __cplusplus
} // extern "C"

// ============================================================================
// C++ DiagManager — replaces direct fprintf/printf calls
// ============================================================================

class DiagManager {
public:
    DiagManager() : diags_{nullptr, 0, 0}, had_error_(false) {}

    // Emit an error at the given location
    void error(KalidousSourceLoc loc, const char *msg);

    // Emit a warning at the given location
    void warning(KalidousSourceLoc loc, const char *msg);

    // Emit a note at the given location
    void note(KalidousSourceLoc loc, const char *msg);

    // Emit a generic info message (no source location)
    void info(const char *msg);

    // Print all accumulated diagnostics with source context
    void print_all(const char *source, size_t source_len,
                   const char *filename = "<input>") const;

    // Print summary (e.g., "3 error(s), 1 warning(s)")
    void print_summary(const char *filename = "<input>") const;

    // Check if any errors were emitted
    bool had_error() const { return had_error_; }

    // Access to raw list (for C API compatibility)
    const KalidousDiagList &list() const { return diags_; }

    // Arena-backed storage — diagnostics live as long as the arena
    void set_arena(KalidousArena *arena) { arena_ = arena; }

private:
    KalidousDiagList diags_;
    KalidousArena *arena_ = nullptr;
    bool had_error_;

    // Internal: emit a single diagnostic with arena-backed message
    void emit(KalidousSourceLoc loc, KalidousDiagSeverity severity, const char *msg);
};

// ============================================================================
// Convenience macros — use in place of fprintf(stderr, ...) and printf
// ============================================================================

// For use in C++ code — provides diag.error(), diag.warning(), etc.
// Usage:  DIAG_ERROR(loc, "expected ';'");
#define DIAG_ERROR(dm, loc, msg)   (dm).error(loc, msg)
#define DIAG_WARN(dm, loc, msg)    (dm).warning(loc, msg)
#define DIAG_NOTE(dm, loc, msg)    (dm).note(loc, msg)
#define DIAG_INFO(dm, msg)         (dm).info(msg)

#endif // __cplusplus

// ============================================================================
// Debug output helpers — unified printf replacements for debug utilities
// These route through a single point so they can be disabled in release builds
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

#ifndef KALIDOUS_NO_DEBUG
void debug_print(const char *fmt, ...);
void debug_println(const char *fmt, ...);
void debug_error(const char *fmt, ...);  // debug errors go to stderr
#else
static inline void debug_print(const char *fmt, ...) { (void)fmt; }
static inline void debug_println(const char *fmt, ...) { (void)fmt; }
static inline void debug_error(const char *fmt, ...) { (void)fmt; }
#endif

// ============================================================================
// I/O error reporting — used by file.c
// ============================================================================

void kalidous_io_error(const char *fmt, ...);

#ifdef __cplusplus
} // extern "C"
#endif
