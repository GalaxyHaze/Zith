#include "zithc-capi.h"
#include "cli/options.hpp"
#include "diagnostics/diagnostic-engine.hpp"
#include "diagnostics/diagnostic.hpp"
#include "diagnostics/error-codes.hpp"
#include "sema/heuristic-engine.hpp"
#include "session/compilation-session.hpp"

#include "legacy-zith/ast/ast-nodes.hpp"
#include "legacy-zith/ast/type-expr.hpp"
#include "legacy-zith/lexer/token.hpp"
#include "memory/arena.hpp"
#include "memory/dyn-array.hpp"

#include <cstring>
#include <new>
#include <string>

struct zithc_session {
    zith::memory::Arena arena;
    zith::memory::StringInterner pool;
    zith::Options opts;
    zith::session::CompilationSession session;
    std::string last_error;
    std::string hover_result_;
    std::string output_result_;

    zithc_session(const char *file_path)
        : arena(), pool(arena), opts(arena), session(opts, file_path) {
        session.setBuffered(true);
    }
    zithc_session(const char *uri, const char *content, size_t length)
        : arena(), pool(arena), opts(arena), session(opts, uri) {
        session.setBuffered(true);
        session.setContent(std::string(content, length));
    }
};

static_assert(static_cast<int>(zith::diagnostics::Severity::Note) == ZITHC_SEVERITY_NOTE,
              "severity enum mismatch");
static_assert(static_cast<int>(zith::session::Stage::Source) == ZITHC_STAGE_SOURCE,
              "stage enum mismatch");
static_assert(static_cast<int>(zith::session::Stage::NraResolved) == ZITHC_STAGE_NRA_RESOLVED,
              "stage enum mismatch");
static_assert(static_cast<int>(zith::session::Stage::CodegenReady) == ZITHC_STAGE_CODEGEN_READY,
              "stage enum mismatch");
static_assert(static_cast<int>(zith::session::Stage::Cached) == ZITHC_STAGE_CACHED,
              "stage enum mismatch");

namespace {

std::string typeExprToShortString(const zith::ast::AstBuilder &bld, zith::ast::TypeExprId id) {
    if (id == zith::ast::kInvalidTypeExpr)
        return "_";
    auto &node = bld.getTypeExpr(id);
    return std::visit(
        [&](const auto &v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, zith::ast::TypeBuiltin>) {
                switch (v.kind) {
                case zith::ast::BuiltinType::I8:
                    return "i8";
                case zith::ast::BuiltinType::I16:
                    return "i16";
                case zith::ast::BuiltinType::I32:
                    return "i32";
                case zith::ast::BuiltinType::I64:
                    return "i64";
                case zith::ast::BuiltinType::I128:
                    return "i128";
                case zith::ast::BuiltinType::U8:
                    return "u8";
                case zith::ast::BuiltinType::U16:
                    return "u16";
                case zith::ast::BuiltinType::U32:
                    return "u32";
                case zith::ast::BuiltinType::U64:
                    return "u64";
                case zith::ast::BuiltinType::U128:
                    return "u128";
                case zith::ast::BuiltinType::F32:
                    return "f32";
                case zith::ast::BuiltinType::F64:
                    return "f64";
                case zith::ast::BuiltinType::Bool:
                    return "bool";
                case zith::ast::BuiltinType::Char:
                    return "char";
                case zith::ast::BuiltinType::Void:
                    return "void";
                case zith::ast::BuiltinType::Never:
                    return "never";
                default:
                    return "?";
                }
            } else if constexpr (std::is_same_v<T, zith::ast::TypePath>) {
                std::string result;
                for (size_t i = 0; i < v.segments.size(); ++i) {
                    if (i > 0)
                        result += "::";
                    result += std::string(v.segments[i]);
                }
                return result.empty() ? "?" : result;
            } else if constexpr (std::is_same_v<T, zith::ast::TypeFnExpr>) {
                std::string result = "(";
                for (size_t i = 0; i < v.params.size(); ++i) {
                    if (i > 0)
                        result += ", ";
                    result += typeExprToShortString(bld, v.params[i]);
                }
                result += ") -> " + typeExprToShortString(bld, v.ret);
                return result;
            } else if constexpr (std::is_same_v<T, zith::ast::TypePtrExpr>) {
                return (v.is_mut ? std::string("*mut ") : std::string("*")) +
                       typeExprToShortString(bld, v.pointee);
            } else if constexpr (std::is_same_v<T, zith::ast::TypeSlice>) {
                return "[]" + typeExprToShortString(bld, v.elem);
            } else if constexpr (std::is_same_v<T, zith::ast::TypeArray>) {
                return "[" + typeExprToShortString(bld, v.count) + "]" +
                       typeExprToShortString(bld, v.elem);
            } else if constexpr (std::is_same_v<T, zith::ast::TypeInfer>) {
                return "_";
            } else if constexpr (std::is_same_v<T, zith::ast::TypeMut>) {
                return "mut " + typeExprToShortString(bld, v.inner);
            } else {
                return "?";
            }
        },
        node);
}

} // anonymous namespace

extern "C" {

zithc_session *zithc_session_create(const char *file_path) {
    if (!file_path || file_path[0] == '\0')
        return nullptr;
    auto *s = new (std::nothrow) zithc_session(file_path);
    return s;
}

zithc_session *zithc_session_create_from_buffer(const char *uri, const char *content,
                                                size_t length) {
    if (!uri || uri[0] == '\0' || !content)
        return nullptr;
    auto *s = new (std::nothrow) zithc_session(uri, content, length);
    return s;
}

void zithc_session_destroy(zithc_session *session) {
    delete session;
}

void zithc_session_add_include_dir(zithc_session *session, const char *dir) {
    if (session && dir)
        session->opts.includeDirs.push(std::string(dir));
}

void zithc_session_set_opt_level(zithc_session *session, uint8_t level) {
    if (session)
        session->opts.flags.optLevel(level);
}

void zithc_session_set_mode(zithc_session *session, uint8_t mode) {
    if (session)
        session->opts.flags.mode(static_cast<zith::Options::Mode>(mode));
}

void zithc_session_set_emit_flags(zithc_session *session, bool ast, bool hir, bool ir, bool asm_) {
    if (session) {
        session->opts.flags.emitAst(ast);
        session->opts.flags.emitHir(hir);
        session->opts.flags.emitIr(ir);
        session->opts.flags.emitAsm(asm_);
    }
}

void zithc_session_set_target(zithc_session *session, int stage) {
    if (session)
        session->opts.targetStage = static_cast<zith::session::Stage>(stage);
}

bool zithc_run(zithc_session *session) {
    if (!session)
        return false;
    return session->session.run();
}

bool zithc_run_to(zithc_session *session, int stage) {
    if (!session)
        return false;
    if (stage < ZITHC_STAGE_SOURCE || stage > ZITHC_STAGE_CACHED)
        return false;
    auto s = static_cast<zith::session::Stage>(stage);
    return session->session.runTo(s);
}

size_t zithc_diag_count(zithc_session *session) {
    if (!session)
        return 0;
    return session->session.diags().all().size();
}

zithc_diagnostic zithc_diag_get(zithc_session *session, size_t index) {
    zithc_diagnostic result = {ZITHC_SEVERITY_ERROR, 0, nullptr, {0, 0}};
    if (!session)
        return result;
    auto &all = session->session.diags().diagnostics();
    if (index >= all.size())
        return result;
    auto &d = all[index];
    // Auto-fill suggestions for UndefinedIdent diagnostics
    if (d.code == zith::diagnostics::err::UndefinedIdent && d.suggestions.empty()) {
        zith::sema::HeuristicEngine heuristic;
        heuristic.generate(d, session->session.symbolTable(), d.suggestions);
    }
    // Enrich overload errors with all function signatures
    if ((d.code == zith::diagnostics::err::NoMatchingFn ||
         d.code == zith::diagnostics::err::AmbiguousCall) &&
        d.suggestions.empty()) {
        auto start = d.message.find('\'');
        auto end   = d.message.rfind('\'');
        if (start != std::string::npos && end != std::string::npos && end > start) {
            auto fn_name = d.message.substr(start + 1, end - start - 1);
            zith::memory::Arena tmp_arena;
            auto syms = session->session.symbolTable().lookupAll(fn_name, tmp_arena);
            for (auto sym_id : syms) {
                auto &data = session->session.symbolTable().get(sym_id);
                if (data.kind != zith::symbols::SymKind::Fn ||
                    data.decl_id == zith::ast::kInvalidDecl)
                    continue;
                auto &decl = session->session.astBuilder().getDecl(data.decl_id);
                auto *fn   = std::get_if<zith::ast::FnDeclNode>(&decl);
                if (!fn)
                    continue;
                std::string sig = "   fn " + std::string(fn->name) + "(";
                for (size_t i = 0; i < fn->params.size(); ++i) {
                    if (i > 0)
                        sig += ", ";
                    sig += std::string(fn->params[i].name) + ": ";
                    sig += typeExprToShortString(session->session.astBuilder(), fn->params[i].type);
                }
                sig += ")";
                if (fn->return_type != zith::ast::kInvalidTypeExpr)
                    sig += " -> " +
                           typeExprToShortString(session->session.astBuilder(), fn->return_type);
                d.suggestions.push(std::move(sig));
            }
        }
    }
    result.severity = static_cast<zithc_severity>(static_cast<int>(d.severity));
    result.code     = d.code;
    result.message  = d.message.c_str();
    result.span     = {d.primary.start, d.primary.end};
    return result;
}
size_t zithc_diag_suggestion_count(zithc_session *session, size_t diag_index) {
    if (!session)
        return 0;
    auto &all = session->session.diags().diagnostics();
    if (diag_index >= all.size())
        return 0;
    return all[diag_index].suggestions.size();
}

const char *zithc_diag_suggestion_get(zithc_session *session, size_t diag_index, size_t sug_index) {
    if (!session)
        return nullptr;
    auto &all = session->session.diags().diagnostics();
    if (diag_index >= all.size())
        return nullptr;
    auto &sug = all[diag_index].suggestions;
    if (sug_index >= sug.size())
        return nullptr;
    return sug[sug_index].c_str();
}

bool zithc_has_errors(zithc_session *session) {
    return session && session->session.hasErrors();
}

void zithc_emit_diagnostics(zithc_session *session) {
    if (session)
        session->session.emitDiagnostics();
}

zithc_position zithc_offset_to_position(zithc_session *session, uint32_t offset) {
    zithc_position result = {0, 0};
    if (!session)
        return result;
    auto &s = session->session;
    zith::memory::Span span{s.fileId(), offset, offset + 1};
    auto loc    = s.sourceMap().loc(span);
    result.line = loc.line > 0 ? loc.line - 1 : 0;
    result.col  = loc.col > 0 ? loc.col - 1 : 0;
    return result;
}

const char *zithc_hover(zithc_session *session, uint32_t offset) {
    if (!session)
        return nullptr;
    auto &tokens = session->session.tokens();
    std::string_view ident_name;
    for (uint32_t i = 0; i < tokens.len; ++i) {
        if (tokens.src[i].span.start <= offset && offset < tokens.src[i].span.end) {
            if (tokens.src[i].kind == zith::lexer::TokenKind::Identifier)
                ident_name = tokens.lexeme(tokens.src[i]);
            break;
        }
    }
    if (ident_name.empty())
        return nullptr;

    zith::memory::Arena tmp_arena;
    auto all = session->session.symbolTable().lookupAll(ident_name, tmp_arena);
    std::string result;
    for (auto sym_id : all) {
        auto &data = session->session.symbolTable().get(sym_id);
        if (data.kind != zith::symbols::SymKind::Fn || data.decl_id == zith::ast::kInvalidDecl)
            continue;
        auto &decl = session->session.astBuilder().getDecl(data.decl_id);
        auto *fn   = std::get_if<zith::ast::FnDeclNode>(&decl);
        if (!fn)
            continue;
        if (!result.empty())
            result += "\n---\n";
        result += "**fn** `" + std::string(fn->name) + "(";
        for (size_t i = 0; i < fn->params.size(); ++i) {
            if (i > 0)
                result += ", ";
            result += std::string(fn->params[i].name) + ": ";
            if (fn->params[i].type != zith::ast::kInvalidTypeExpr)
                result += "`" +
                          typeExprToShortString(session->session.astBuilder(), fn->params[i].type) +
                          "`";
        }
        result += ")";
        if (fn->return_type != zith::ast::kInvalidTypeExpr)
            result += " -> `" +
                      typeExprToShortString(session->session.astBuilder(), fn->return_type) + "`";
    }
    if (result.empty())
        return nullptr;
    result                 = "```zith\n" + result + "\n```";
    session->hover_result_ = std::move(result);
    return session->hover_result_.c_str();
}

const char *zithc_last_error(zithc_session *session) {
    if (!session)
        return "null session";
    return session->last_error.c_str();
}

const char *zithc_run_to_json(zithc_session *session, int stage) {
    if (!session)
        return R"({"success":false,"errors":[{"severity":"error","code":0,"message":"null session","span":{"start":0,"end":0}}]})";

    if (stage < 0)
        session->session.run();
    else
        session->session.runTo(static_cast<zith::session::Stage>(stage));

    auto &diags = session->session.diags();
    auto &all   = diags.diagnostics();

    std::string json;
    json += "{\"success\":";
    json += diags.hasErrors() ? "false" : "true";
    json += ",\"errors\":[";

    for (size_t i = 0; i < all.size(); ++i) {
        if (i > 0)
            json += ",";
        auto &d = all[i];
        json += "{\"severity\":\"";
        switch (d.severity) {
        case zith::diagnostics::Severity::Note:
            json += "note";
            break;
        case zith::diagnostics::Severity::Warning:
            json += "warning";
            break;
        case zith::diagnostics::Severity::Error:
            json += "error";
            break;
        case zith::diagnostics::Severity::Bug:
            json += "bug";
            break;
        }
        json += "\",\"code\":";
        json += std::to_string(d.code);
        json += ",\"message\":\"";
        // Escape JSON special chars in message
        for (auto c : d.message) {
            switch (c) {
            case '"':
                json += "\\\"";
                break;
            case '\\':
                json += "\\\\";
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
                json += c;
            }
        }
        json += "\",\"span\":{\"start\":";
        json += std::to_string(d.primary.start);
        json += ",\"end\":";
        json += std::to_string(d.primary.end);
        json += "}}";
    }

    json += "]}";
    session->last_error = std::move(json);
    return session->last_error.c_str();
}

const char *zithc_session_flush_output(zithc_session *session) {
    if (!session)
        return "";
    session->output_result_ = session->session.flushOutput();
    return session->output_result_.c_str();
}
} // extern "C"
