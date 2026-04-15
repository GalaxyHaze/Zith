// impl/diagnostics/diagnostics.hpp — Centralized diagnostic system
//
// Replaces scattered fprintf/printf calls with a unified DiagManager
// that tracks errors, warnings, and notes with source context.
#pragma once

#include <zith/zith.hpp>
#include <cstddef>
#include <cstdio>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Diagnostic severity and structure
// ============================================================================

typedef enum ZithDiagSeverity {
    ZITH_DIAG_ERROR   = 0,
    ZITH_DIAG_WARNING = 1,
    ZITH_DIAG_NOTE    = 2,
    ZITH_DIAG_INFO    = 3,
} ZithDiagSeverity;

typedef struct ZithDiagnostic {
    const char *message;           // interned in arena
    ZithSourceLoc loc;
    ZithDiagSeverity severity;
} ZithDiagnostic;

typedef struct ZithDiagList {
    ZithDiagnostic *items;
    size_t count;
    size_t capacity;
} ZithDiagList;

// ============================================================================
// C API — diagnostic emission and printing
// ============================================================================

// Print all diagnostics with source context to stderr
void zith_diag_print_all(const ZithDiagList *diags,
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
    void error(ZithSourceLoc loc, const char *msg);

    // Emit a warning at the given location
    void warning(ZithSourceLoc loc, const char *msg);

    // Emit a note at the given location
    void note(ZithSourceLoc loc, const char *msg);

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
    const ZithDiagList &list() const { return diags_; }

    // Arena-backed storage — diagnostics live as long as the arena
    void set_arena(ZithArena *arena) { arena_ = arena; }

private:
    ZithDiagList diags_;
    ZithArena *arena_ = nullptr;
    bool had_error_;

    // Internal: emit a single diagnostic with arena-backed message
    void emit(ZithSourceLoc loc, ZithDiagSeverity severity, const char *msg);
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

#ifndef ZITH_NO_DEBUG
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

void zith_io_error(const char *fmt, ...);

#ifdef __cplusplus
} // extern "C"
#endif
