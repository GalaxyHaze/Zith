#include <cstring>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "capi/zithc-capi.h"
#include "memory/arena.hpp"
#include "session/compilation-session.hpp"
#include "vm/hir-to-vm.hpp"
#include "vm/typed-ir.hpp"
#include "vm/vm-v2.hpp"
#include "wasm/abi-hir.hpp"

extern "C" {
// Import from JS: zith.host_write
__attribute__((import_module("zith"), import_name("host_write"))) void
host_write(int stream, const char *ptr, int len);
// Import from JS: zith.host_read_byte
__attribute__((import_module("zith"), import_name("host_read_byte"))) int host_read_byte();
}

static std::string last_error;
static std::string last_output;
static std::string last_blob;
static std::vector<std::string> rendered_errors;
static std::string last_diagnostics_json  = "{\"diagnostics\":[]}";
static size_t structured_diagnostic_count = 0;
static int64_t last_exit_code             = 0;
static std::vector<std::pair<std::string, std::string>> stdlib_sources;

// Compile-once HIR artifact cache. The browser can compile a source buffer
// once, restore the persisted flat HIR blob, and replay it without re-entering
// the compiler pipeline. Entries are keyed by the source bytes plus the
// registered stdlib pack fingerprint, so a new pack invalidates stale HIR.
struct CachedHirBlob {
    uint64_t key = 0;
    std::string bytes;
};

constexpr size_t kHirCacheCapacity = 32U;
static std::vector<CachedHirBlob> hir_cache;
static uint64_t stdlib_pack_fingerprint = 0;
static int64_t hir_cache_hits           = 0;
static int64_t hir_cache_misses         = 0;
static int64_t hir_cache_stale          = 0;

#ifdef ZITH_IS_WASM
constexpr const char *kWasmStdioHeader = R"(#[discardable] pub extern fn getchar(): i32
#[discardable] pub extern fn putchar(c: i32): i32
#[discardable] pub extern fn sscanf(input: ?*char, format: *char, ...): i32
#[discardable] pub extern fn strncmp(left: ?*char, right: ?*char, count: u64): i32
)";

constexpr const char *kWasmStdlibHeader = R"(#[discardable] pub extern fn malloc(size: u64): raw opaque
#[discardable] pub extern fn calloc(count: u64, size: u64): raw opaque
#[discardable] pub extern fn realloc(ptr: raw opaque, size: u64): raw opaque
#[discardable] pub extern fn free(ptr: raw opaque)
)";

constexpr const char *kWasmStringHeader = R"(#[discardable] pub extern fn strlen(value: ?*char): u64
#[discardable] pub extern fn memcpy(destination: ?*char, source: ?*char, size: u64): raw opaque
#[discardable] pub extern fn snprintf(destination: *char, size: u64, format: *char, ...): i32
)";
#endif

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

extern "C" __attribute__((export_name("zith_last_buffer_ptr"))) const char *
zith_last_buffer_ptr() {
    return last_blob.data();
}

extern "C" __attribute__((export_name("zith_last_buffer_len"))) int zith_last_buffer_len() {
    return static_cast<int>(last_blob.size());
}

extern "C" __attribute__((export_name("zith_exit_code"))) int64_t zith_exit_code() {
    return last_exit_code;
}

extern "C" __attribute__((export_name("zith_error_count"))) unsigned zith_error_count() {
    return static_cast<unsigned>(rendered_errors.size());
}

extern "C" __attribute__((export_name("zith_error_at"))) const char *zith_error_at(unsigned index) {
    if (index >= rendered_errors.size())
        return nullptr;
    return rendered_errors[index].data();
}

extern "C" __attribute__((export_name("zith_last_diagnostics_json_ptr"))) const char *
zith_last_diagnostics_json_ptr() {
    return last_diagnostics_json.data();
}

extern "C" __attribute__((export_name("zith_last_diagnostics_json_len"))) int
zith_last_diagnostics_json_len() {
    return static_cast<int>(last_diagnostics_json.size());
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
constexpr int kPlaygroundStatusTrap          = 3;
constexpr int kPlaygroundStatusOom           = 4;
constexpr int kPlaygroundStatusUnsupported   = 5;
constexpr int kPlaygroundStatusCancelled     = 6;
constexpr std::string_view kStdlibPackMagic  = "ZSTDLIB2";
constexpr uint32_t kStdlibPackAbi            = 3;

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

void appendJsonString(std::string &json, std::string_view text) {
    constexpr char hex[] = "0123456789abcdef";
    json.push_back('"');
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        switch (byte) {
        case '"':
            json += "\\\"";
            break;
        case '\\':
            json += "\\\\";
            break;
        case '\b':
            json += "\\b";
            break;
        case '\f':
            json += "\\f";
            break;
        case '\n':
            json += "\\n";
            break;
        case '\r':
            json += "\\r";
            break;
        case '\t':
            json += "\\t";
            break;
        default:
            if (byte < 0x20U) {
                json += "\\u00";
                json.push_back(hex[byte >> 4U]);
                json.push_back(hex[byte & 0x0fU]);
            } else {
                json.push_back(character);
            }
            break;
        }
    }
    json.push_back('"');
}

void resetStructuredDiagnostics() {
    last_diagnostics_json       = "{\"diagnostics\":[]}";
    structured_diagnostic_count = 0;
}

void appendStructuredDiagnostic(std::string_view severity, std::string_view message,
                                const zithc_diagnostic *diagnostic = nullptr) {
    last_diagnostics_json.resize(last_diagnostics_json.size() - 2U);
    if (structured_diagnostic_count > 0U)
        last_diagnostics_json.push_back(',');

    last_diagnostics_json += "{\"severity\":";
    appendJsonString(last_diagnostics_json, severity);
    last_diagnostics_json += ",\"message\":";
    appendJsonString(last_diagnostics_json, message);
    if (diagnostic != nullptr) {
        last_diagnostics_json += ",\"code\":";
        last_diagnostics_json += std::to_string(diagnostic->code);
        last_diagnostics_json += ",\"span\":{\"start\":";
        last_diagnostics_json += std::to_string(diagnostic->span.start);
        last_diagnostics_json += ",\"end\":";
        last_diagnostics_json += std::to_string(diagnostic->span.end);
        last_diagnostics_json.push_back('}');
    }
    last_diagnostics_json += "]}";
    ++structured_diagnostic_count;
}

void resetDiagnostics() {
    rendered_errors.clear();
    resetStructuredDiagnostics();
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
    if (last_error.empty() || last_error.back() != '\n')
        last_error.push_back('\n');
    appendStructuredDiagnostic("error", message);
    host_write(2, last_error.data(), last_error.size());
}

// FNV-1a over the source bytes and the active stdlib fingerprint. The source
// length is mixed in explicitly so distinct buffers cannot collide through
// trailing-zero differences.
uint64_t hirCacheKey(const char *ptr, int len) {
    constexpr uint64_t kOffset = 1469598103934665603ULL;
    constexpr uint64_t kPrime  = 1099511628211ULL;
    uint64_t hash = kOffset;
    const auto mix = [&](uint8_t byte) {
        hash ^= byte;
        hash *= kPrime;
    };
    for (int i = 0; i < 8; ++i)
        mix(static_cast<uint8_t>(static_cast<uint64_t>(len) >> (i * 8)));
    for (int i = 0; i < 8; ++i)
        mix(static_cast<uint8_t>(stdlib_pack_fingerprint >> (i * 8)));
    for (int i = 0; i < len; ++i)
        mix(static_cast<uint8_t>(ptr[i]));
    return hash;
}

const std::string *lookupCachedHir(uint64_t key) {
    for (const auto &entry : hir_cache) {
        if (entry.key == key)
            return &entry.bytes;
    }
    return nullptr;
}

void storeCachedHir(uint64_t key, const std::string &bytes) {
    for (auto &entry : hir_cache) {
        if (entry.key == key) {
            entry.bytes = bytes;
            return;
        }
    }
    if (hir_cache.size() >= kHirCacheCapacity)
        hir_cache.erase(hir_cache.begin());
    hir_cache.push_back(CachedHirBlob{key, bytes});
}

uint32_t readU32(const uint8_t *data, size_t size, size_t &offset, bool &ok) {
    if (offset > size || size - offset < 4U) {
        ok = false;
        return 0;
    }
    const uint32_t value = static_cast<uint32_t>(data[offset]) |
                           (static_cast<uint32_t>(data[offset + 1U]) << 8U) |
                           (static_cast<uint32_t>(data[offset + 2U]) << 16U) |
                           (static_cast<uint32_t>(data[offset + 3U]) << 24U);
    offset += 4U;
    return value;
}

bool isSafeStdlibPath(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string_view::npos)
        return false;
    size_t start = 0;
    while (start < path.size()) {
        const size_t end = path.find('/', start);
        const auto component = path.substr(start, end == std::string_view::npos
                                                   ? path.size() - start
                                                   : end - start);
        if (component.empty() || component == "." || component == "..")
            return false;
        start = end == std::string_view::npos ? path.size() : end + 1U;
    }
    return true;
}

bool registerStdlibPack(const char *ptr, int len) {
    stdlib_sources.clear();
    stdlib_pack_fingerprint = 0;
    if (!ptr || len < static_cast<int>(kStdlibPackMagic.size() + 8U))
        return false;

    const auto *data = reinterpret_cast<const uint8_t *>(ptr);
    const size_t size = static_cast<size_t>(len);
    if (std::string_view(reinterpret_cast<const char *>(data), kStdlibPackMagic.size()) !=
        kStdlibPackMagic)
        return false;

    size_t offset = kStdlibPackMagic.size();
    bool ok = true;
    const uint32_t abi = readU32(data, size, offset, ok);
    const uint32_t count = readU32(data, size, offset, ok);
    if (!ok || abi != kStdlibPackAbi || count > 10000U)
        return false;

    stdlib_sources.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        const uint32_t path_len = readU32(data, size, offset, ok);
        const uint32_t text_len = readU32(data, size, offset, ok);
        if (!ok || path_len == 0U || path_len > size || text_len > size ||
            offset > size - path_len || offset + path_len > size - text_len)
            return false;
        std::string path(reinterpret_cast<const char *>(data + offset), path_len);
        offset += path_len;
        std::string text(reinterpret_cast<const char *>(data + offset), text_len);
        offset += text_len;
        if (!isSafeStdlibPath(path))
            return false;
        for (const auto &[registered_path, registered_text] : stdlib_sources)
            if (registered_path == path)
                return false;
        stdlib_sources.emplace_back(std::move(path), std::move(text));
    }
    if (offset != size)
        return false;

    // Fingerprint the whole pack so cached HIR is invalidated when the
    // canonical stdlib changes.
    constexpr uint64_t kOffset = 1469598103934665603ULL;
    constexpr uint64_t kPrime  = 1099511628211ULL;
    uint64_t hash = kOffset;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= kPrime;
    }
    stdlib_pack_fingerprint = hash;
    return true;
}

void registerStdlibSources(zithc_session *session) {
    for (const auto &[path, text] : stdlib_sources) {
        const std::string virtual_path = "stdlib/" + path;
        zithc_session_register_virtual_source(session, virtual_path.c_str(), text.c_str());
    }
#ifdef ZITH_IS_WASM
    zithc_session_register_virtual_source(session, "stdlib/stdio.h.zithm", kWasmStdioHeader);
    zithc_session_register_virtual_source(session, "stdlib/stdlib.h.zithm", kWasmStdlibHeader);
    zithc_session_register_virtual_source(session, "stdlib/string.h.zithm", kWasmStringHeader);
#endif
}

int runStatusToPlaygroundStatus(zith::vm::RunStatus status) {
    switch (status) {
    case zith::vm::RunStatus::Ok:
        return kPlaygroundStatusOk;
    case zith::vm::RunStatus::MissingMain:
    case zith::vm::RunStatus::Trap:
        return kPlaygroundStatusTrap;
    case zith::vm::RunStatus::Unsupported:
        return kPlaygroundStatusUnsupported;
    case zith::vm::RunStatus::Oom:
        return kPlaygroundStatusOom;
    case zith::vm::RunStatus::Cancelled:
        return kPlaygroundStatusCancelled;
    }
    return kPlaygroundStatusTrap;
}

int readHostInputByte(void *) {
    return host_read_byte();
}

void writeHostOutput(void *, std::string_view bytes) {
    if (!bytes.empty())
        host_write(1, bytes.data(), static_cast<int>(bytes.size()));
}

int runPlayground(const char *ptr, int len, bool is_compile, int mode = 0, int opt_level = 0,
                  int emit_mask = 0) {
    last_error.clear();
    last_output.clear();
    resetDiagnostics();

    if (!ptr || len < 0)
        setErrorMessage("invalid source buffer");
    if (is_compile && !isPlaygroundMode(mode))
        setErrorMessage("invalid mode (must be 0 or 1)");
    if (is_compile && !isPlaygroundOptLevel(opt_level))
        setErrorMessage("invalid optimization level (must be 0-3)");
    if (last_error.empty() && (emit_mask & ~127) != 0)
        setErrorMessage("invalid emit_mask (bits 0-6 only)");
    if (!last_error.empty())
        return kPlaygroundStatusInvalidParam;

    auto *session =
        zithc_session_create_from_buffer("playground.zithm", ptr, static_cast<size_t>(len));
    if (!session) {
        setErrorMessage("failed to create session");
        return kPlaygroundStatusCompileFailed;
    }

    zithc_session_set_mode(session, static_cast<uint8_t>(mode));
    zithc_session_set_opt_level(session, static_cast<uint8_t>(opt_level));
    zithc_session_set_emit_tokens(session, emit_mask & 1);
    zithc_session_set_emit_flags(session, emit_mask & 2, emit_mask & 4, emit_mask & 8,
                                 emit_mask & 16);
    zithc_session_set_emit_extra_flags(session, emit_mask & 32, emit_mask & 64);
    zithc_session_add_include_dir(session, "stdlib");
    registerStdlibSources(session);

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
        appendStructuredDiagnostic(severityName(diag.severity),
                                   diag.message != nullptr ? diag.message : "", &diag);
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

// Compile `ptr`/`len` to a flat HIR blob and store it in `last_blob`. This is
// the shared body behind `zith_emit_hir` and the cache's compile-once path.
int emitHir(const char *ptr, int len) {
    last_error.clear();
    last_output.clear();
    last_blob.clear();
    resetDiagnostics();
    last_exit_code = 0;

    if (!ptr || len < 0)
        setErrorMessage("invalid source buffer");
    if (!last_error.empty())
        return kPlaygroundStatusInvalidParam;

    auto *session =
        zithc_session_create_from_buffer("playground.zithm", ptr, static_cast<size_t>(len));
    if (!session) {
        setErrorMessage("failed to create session");
        return kPlaygroundStatusCompileFailed;
    }

    zithc_session_set_mode(session, 1);
    zithc_session_set_opt_level(session, 0);
    zithc_session_add_include_dir(session, "stdlib");
    registerStdlibSources(session);
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
        appendStructuredDiagnostic(severityName(diag.severity),
                                   diag.message != nullptr ? diag.message : "", &diag);
        host_write(2, render_str.data(), render_str.size());
        last_error += render_str;
    }

    auto *compilerContext = static_cast<zith::session::CompilationSession *>(
        zithc_session_compiler_context(session));
    if (ok && last_error.empty()) {
        const auto encoded = zith::wasm::encodeHir(*compilerContext);
        if (!encoded.ok) {
            setErrorMessage(encoded.message.empty() ? "flat HIR encoding failed"
                                                    : encoded.message);
        } else {
            last_blob.assign(encoded.blob.bytes.begin(), encoded.blob.bytes.end());
        }
    }
    zithc_session_destroy(session);
    if (!ok || last_blob.empty()) {
        if (last_error.empty())
            setErrorMessage("compilation failed");
        return kPlaygroundStatusCompileFailed;
    }

    return kPlaygroundStatusOk;
}

// Decode and execute a flat HIR blob, forwarding program output and exposing
// the guest exit code. Shared by `zith_execute_hir` and the cache replay path.
// `fromCache` only changes how a blob that no longer decodes is reported: a
// cached blob is a stale artifact, while a caller-supplied blob is malformed.
int executeHirBlob(const char *ptr, int len, bool fromCache) {
    last_error.clear();
    last_output.clear();
    resetDiagnostics();
    last_exit_code = 0;

    if (!ptr || len < 0)
        setErrorMessage("invalid HIR buffer");
    if (!last_error.empty())
        return kPlaygroundStatusInvalidParam;

    zith::wasm::DecodedHir decoded;
    std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t *>(ptr),
                                   static_cast<size_t>(len));
    if (!zith::wasm::decodeHir(bytes, decoded)) {
        if (fromCache) {
            ++hir_cache_stale;
            setErrorMessage("stale HIR artifact: recompile the source with zith_compile_hir");
        } else {
            setErrorMessage(decoded.message.empty() ? "invalid flat HIR blob" : decoded.message);
        }
        return kPlaygroundStatusCompileFailed;
    }

    zith::memory::Arena vmArena;
    zith::vm::Module module(vmArena);
    const auto lowered =
        zith::vm::lowerModule(decoded.module, decoded.interner, decoded.types, vmArena, module);
    if (!lowered.ok) {
        setErrorMessage(lowered.message.empty() ? "unsupported program for VM v2"
                                                : lowered.message);
        return kPlaygroundStatusUnsupported;
    }

    zith::vm::Vm vm;
    const auto result = vm.runMain(module, {nullptr, readHostInputByte, writeHostOutput});
    last_exit_code    = result.exitCode;
    last_output       = result.output;
    if (result.status == zith::vm::RunStatus::Ok) {
        return kPlaygroundStatusOk;
    }

    if (result.status == zith::vm::RunStatus::Cancelled)
        return kPlaygroundStatusCancelled;

    setErrorMessage(result.message.empty() ? "VM v2 could not execute the program"
                                           : result.message);
    return runStatusToPlaygroundStatus(result.status);
}

} // namespace

extern "C" int zith_emit_hir(const char *ptr, int len);
extern "C" int zith_execute_hir(const char *ptr, int len);

extern "C" __attribute__((export_name("zith_compile_source"))) int
zith_compile_source(const char *ptr, int len, int mode, int opt_level, int emit_mask) {
    return runPlayground(ptr, len, true, mode, opt_level, emit_mask);
}

extern "C" __attribute__((export_name("zith_run_source"))) int zith_run_source(const char *ptr,
                                                                               int len) {
    const int compile_status = zith_emit_hir(ptr, len);
    if (compile_status != kPlaygroundStatusOk)
        return compile_status;
    return zith_execute_hir(last_blob.data(), static_cast<int>(last_blob.size()));
}

// Compile once and store the flat HIR blob in the module-local artifact cache.
// A replay through `zith_execute_cached` skips the compiler pipeline entirely.
extern "C" __attribute__((export_name("zith_compile_hir"))) int zith_compile_hir(const char *ptr,
                                                                                 int len) {
    const int status = zith_emit_hir(ptr, len);
    if (status == kPlaygroundStatusOk) {
        const uint64_t key = hirCacheKey(ptr, len);
        storeCachedHir(key, last_blob);
    }
    return status;
}

// Replay a cached HIR artifact. A miss is reported through the structured
// diagnostics channel with a stable message rather than silently recompiling.
extern "C" __attribute__((export_name("zith_execute_cached"))) int
zith_execute_cached(const char *ptr, int len) {
    if (!ptr || len < 0) {
        last_error.clear();
        last_output.clear();
        resetDiagnostics();
        last_exit_code = 0;
        setErrorMessage("invalid source buffer");
        return kPlaygroundStatusInvalidParam;
    }

    const uint64_t key         = hirCacheKey(ptr, len);
    const std::string *cached  = lookupCachedHir(key);
    if (cached == nullptr) {
        ++hir_cache_misses;
        last_error.clear();
        last_output.clear();
        resetDiagnostics();
        last_exit_code = 0;
        setErrorMessage("HIR artifact cache miss: compile the source with zith_compile_hir");
        return kPlaygroundStatusCompileFailed;
    }

    ++hir_cache_hits;
    return executeHirBlob(cached->data(), static_cast<int>(cached->size()), true);
}

// Restore a flat HIR blob persisted by a previous session (for example from
// browser storage) under the cache key for `ptr`/`len`. The blob is stored
// without validation so that an outdated artifact surfaces as a stale cache
// entry when it is replayed instead of failing at restore time.
extern "C" __attribute__((export_name("zith_restore_cached"))) int
zith_restore_cached(const char *ptr, int len, const char *blob_ptr, int blob_len) {
    last_error.clear();
    last_output.clear();
    resetDiagnostics();
    last_exit_code = 0;
    if (!ptr || len < 0 || !blob_ptr || blob_len <= 0) {
        setErrorMessage("invalid cached HIR artifact");
        return kPlaygroundStatusInvalidParam;
    }
    storeCachedHir(hirCacheKey(ptr, len),
                   std::string(blob_ptr, static_cast<size_t>(blob_len)));
    return kPlaygroundStatusOk;
}

extern "C" __attribute__((export_name("zith_hir_cache_hits"))) int64_t zith_hir_cache_hits() {
    return hir_cache_hits;
}

extern "C" __attribute__((export_name("zith_hir_cache_misses"))) int64_t
zith_hir_cache_misses() {
    return hir_cache_misses;
}

extern "C" __attribute__((export_name("zith_hir_cache_stale"))) int64_t zith_hir_cache_stale() {
    return hir_cache_stale;
}

extern "C" __attribute__((export_name("zith_hir_cache_size"))) int64_t zith_hir_cache_size() {
    return static_cast<int64_t>(hir_cache.size());
}

extern "C" __attribute__((export_name("zith_register_stdlib_pack"))) int
zith_register_stdlib_pack(const char *ptr, int len) {
    last_error.clear();
    last_output.clear();
    last_blob.clear();
    resetDiagnostics();
    last_exit_code = 0;
    if (!registerStdlibPack(ptr, len)) {
        setErrorMessage("invalid stdlib pack");
        return kPlaygroundStatusInvalidParam;
    }
    // A new pack changes the cache key domain, so drop entries compiled
    // against the previous pack instead of retaining unreachable blobs.
    hir_cache.clear();
    hir_cache_hits   = 0;
    hir_cache_misses = 0;
    hir_cache_stale  = 0;
    return kPlaygroundStatusOk;
}

extern "C" __attribute__((export_name("zith_emit_hir"))) int zith_emit_hir(const char *ptr,
                                                                           int len) {
    return emitHir(ptr, len);
}

extern "C" __attribute__((export_name("zith_execute_hir"))) int zith_execute_hir(const char *ptr,
                                                                                 int len) {
    return executeHirBlob(ptr, len, false);
}
