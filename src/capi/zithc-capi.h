#ifndef ZITHC_CAPI_H
#define ZITHC_CAPI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zithc_session zithc_session;

typedef enum {
    ZITHC_SEVERITY_NOTE    = 0,
    ZITHC_SEVERITY_WARNING = 1,
    ZITHC_SEVERITY_ERROR   = 2,
    ZITHC_SEVERITY_BUG     = 3
} zithc_severity;

typedef enum {
    ZITHC_STAGE_SOURCE        = 0,
    ZITHC_STAGE_LEXED         = 1,
    ZITHC_STAGE_SCANNED       = 2,
    ZITHC_STAGE_IMPORTED      = 3,
    ZITHC_STAGE_RESOLVED      = 4,
    ZITHC_STAGE_TYPE_CHECKED  = 5,
    ZITHC_STAGE_SOLVED        = 6,
    ZITHC_STAGE_NRA_RESOLVED  = 7,
    ZITHC_STAGE_HIR_LOWERED   = 8,
    ZITHC_STAGE_CODEGEN_READY = 9,
    ZITHC_STAGE_CACHED        = 10
} zithc_stage;

typedef struct {
    uint32_t line;
    uint32_t col;
} zithc_position;

typedef struct {
    uint32_t start;
    uint32_t end;
} zithc_span;

typedef struct {
    zithc_severity severity;
    uint32_t code;
    const char *message;
    zithc_span span;
} zithc_diagnostic;

zithc_session *zithc_session_create(const char *file_path);
zithc_session *zithc_session_create_from_buffer(const char *uri, const char *content,
                                                size_t length);
void zithc_session_destroy(zithc_session *session);

void zithc_session_add_include_dir(zithc_session *session, const char *dir);

void zithc_session_set_opt_level(zithc_session *session, uint8_t level);
void zithc_session_set_mode(zithc_session *session, uint8_t mode);
void zithc_session_set_emit_tokens(zithc_session *session, bool tokens);
void zithc_session_set_emit_flags(zithc_session *session, bool ast, bool hir, bool ir, bool asm_);
void zithc_session_set_target(zithc_session *session, int stage);

bool zithc_run(zithc_session *session);
bool zithc_run_to(zithc_session *session, int stage);

size_t zithc_diag_count(zithc_session *session);
zithc_diagnostic zithc_diag_get(zithc_session *session, size_t index);
size_t zithc_diag_suggestion_count(zithc_session *session, size_t diag_index);
const char *zithc_diag_suggestion_get(zithc_session *session, size_t diag_index, size_t sug_index);
bool zithc_has_errors(zithc_session *session);
void zithc_emit_diagnostics(zithc_session *session);

const char *zithc_hover(zithc_session *session, uint32_t offset);
const char *zithc_run_to_json(zithc_session *session, int stage);
zithc_position zithc_offset_to_position(zithc_session *session, uint32_t offset);
const char *zithc_session_flush_output(zithc_session *session);
const char *zithc_last_error(zithc_session *session);

#ifdef __cplusplus
}
#endif

#endif // ZITHC_CAPI_H
