#include <cstring>
#include <string>
#include <vector>

#include "capi/zithc-capi.h"

extern "C" {
// Import from JS: zith.host_write
__attribute__((import_module("zith"), import_name("host_write"))) void
host_write(int stream, const char *ptr, int len);
}

static std::string last_error;
static std::string last_output;
static std::vector<std::string> rendered_errors;

extern "C" __attribute__((export_name("zith_alloc"))) void *zith_alloc(int size) {
    return new char[size];
}

extern "C" __attribute__((export_name("zith_free"))) void zith_free(void *ptr, int size) {
    delete[] static_cast<char *>(ptr);
}

extern "C" __attribute__((export_name("zith_last_error_ptr"))) const char *zith_last_error_ptr() {
    return last_error.data();
}

extern "C" __attribute__((export_name("zith_last_error_len"))) int zith_last_error_len() {
    return static_cast<int>(last_error.size());
}

extern "C" __attribute__((export_name("zith_last_output_ptr"))) const char *zith_last_output_ptr() {
    return last_output.data();
}

extern "C" __attribute__((export_name("zith_last_output_len"))) int zith_last_output_len() {
    return static_cast<int>(last_output.size());
}

extern "C" __attribute__((export_name("zith_error_count"))) unsigned zith_error_count() {
    return static_cast<unsigned>(rendered_errors.size());
}

extern "C" __attribute__((export_name("zith_error_at"))) const char *zith_error_at(unsigned index) {
    if (index >= rendered_errors.size())
        return nullptr;
    return rendered_errors[index].data();
}

extern "C" __attribute__((export_name("zith_compiler_version_ptr"))) const char *
zith_compiler_version_ptr() {
    return ZITH_VERSION;
}

extern "C" __attribute__((export_name("zith_compiler_version_len"))) int
zith_compiler_version_len() {
    return static_cast<int>(std::strlen(ZITH_VERSION));
}

namespace {

constexpr int kPlaygroundStatusOk            = 0;
constexpr int kPlaygroundStatusCompileFailed = 1;
constexpr int kPlaygroundStatusInvalidParam  = 2;

constexpr bool isPlaygroundMode(int mode) {
    return mode == 0 || mode == 1;
}

constexpr bool isPlaygroundOptLevel(int opt_level) {
    return opt_level >= 0 && opt_level <= 3;
}

const char *severityName(zithc_severity severity) {
    switch (severity) {
    case ZITHC_SEVERITY_ERROR:
        return "error";
    case ZITHC_SEVERITY_WARNING:
        return "warning";
    case ZITHC_SEVERITY_NOTE:
        return "note";
    case ZITHC_SEVERITY_BUG:
        return "bug";
    }
    return "bug";
}

std::string renderDiagnostic(const zithc_diagnostic &diag) {
    std::string line;
    line += severityName(diag.severity);
    line += ": ";
    line += diag.message ? diag.message : "";
    line += "\n";
    return line;
}

void setErrorMessage(const std::string &message) {
    last_error = message;
    host_write(2, last_error.data(), last_error.size());
}

int runPlayground(const char *ptr, int len, bool is_compile, int mode = 0, int opt_level = 0,
                  int emit_mask = 0) {
    last_error.clear();
    last_output.clear();
    rendered_errors.clear();

    if (!ptr || len < 0)
        setErrorMessage("invalid source buffer");
    if (is_compile && !isPlaygroundMode(mode))
        setErrorMessage("invalid mode (must be 0 or 1)");
    if (is_compile && !isPlaygroundOptLevel(opt_level))
        setErrorMessage("invalid optimization level (must be 0-3)");
    if (last_error.empty() && (emit_mask & ~31) != 0)
        setErrorMessage("invalid emit_mask (bits 0-4 only)");
    if (!last_error.empty())
        return kPlaygroundStatusInvalidParam;

    auto *session =
        zithc_session_create_from_buffer("playground.zith", ptr, static_cast<size_t>(len));
    if (!session) {
        setErrorMessage("failed to create session");
        return kPlaygroundStatusCompileFailed;
    }

    zithc_session_set_mode(session, static_cast<uint8_t>(mode));
    zithc_session_set_opt_level(session, static_cast<uint8_t>(opt_level));
    zithc_session_set_emit_tokens(session, emit_mask & 1);
    zithc_session_set_emit_flags(session, emit_mask & 2, emit_mask & 4, emit_mask & 8,
                                 emit_mask & 16);

    // The browser build has no LLVM backend, so emission stops at HIR lowering.
    const bool ok = zithc_run_to(session, ZITHC_STAGE_HIR_LOWERED);

    const char *buffered_out = zithc_session_flush_output(session);
    if (buffered_out && buffered_out[0] != '\0') {
        last_output = buffered_out;
        host_write(1, last_output.data(), last_output.size());
    }

    const size_t count = zithc_diag_count(session);
    for (size_t i = 0; i < count; ++i) {
        const zithc_diagnostic diag  = zithc_diag_get(session, i);
        const std::string render_str = renderDiagnostic(diag);
        rendered_errors.push_back(render_str);
        host_write(2, render_str.data(), render_str.size());
        last_error += render_str;
    }

    zithc_session_destroy(session);
    if (last_error.empty() && (emit_mask & (8 | 16)) != 0) {
        setErrorMessage("IR/ASM emission is not available in the WASM playground build");
        return kPlaygroundStatusCompileFailed;
    }
    return ok ? kPlaygroundStatusOk : kPlaygroundStatusCompileFailed;
}

} // namespace

extern "C" __attribute__((export_name("zith_compile_source"))) int
zith_compile_source(const char *ptr, int len, int mode, int opt_level, int emit_mask) {
    return runPlayground(ptr, len, true, mode, opt_level, emit_mask);
}

extern "C" __attribute__((export_name("zith_run_source"))) int zith_run_source(const char *ptr,
                                                                               int len) {
    // Run is documented as check + HIR emission; the browser does not execute the program.
    return runPlayground(ptr, len, false, 1, 0, 0);
}
