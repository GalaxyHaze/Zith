#include "legacy-zith/lexer/token.hpp"
#include "memory/flat-map.hpp"
#include "parser.hpp"

#include "diagnostics/error-codes.hpp"
#include "legacy-zith/lexer/lexer.hpp"
#include "legacy-zith/parser/recovery.hpp"
#include "legacy-zith/parser/scan-helpers.hpp"
#include "symbols/symbol-table.hpp"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace zith::parser {

// Parse fn/method parameters with real types (single-pass).
// Advances past closing ')'.
static memory::DynArray<ast::FnParam> parseFnParams(Parser &parser) {
    auto &tok   = *parser.tok;
    auto &diag  = *parser.diag;
    auto result = memory::DynArray<ast::FnParam>{parser.bld->arena()};

    while (!tok.is_empty()) {
        if (tok.peek().is_eof() || tok.peek().punc == ')')
            break;

        // destructured params: [a, b, c]: Type
        if (tok.peek().punc == '[') {
            tok.advance();
            memory::DynArray<std::string_view> names{parser.bld->arena()};
            while (!tok.is_empty() && tok.peek().punc != ']') {
                if (tok.peek().is(lexer::TokenKind::Identifier)) {
                    names.push(tok.lexeme());
                    tok.advance();
                } else {
                    tok.advance();
                }
                if (tok.peek().punc == ',')
                    tok.advance();
            }
            if (tok.peek().punc == ']')
                tok.advance();
            auto type_expr = parser.parseOptTypeAnnotation();
            for (auto &n : names)
                result.push({n, type_expr});
            if (tok.peek().punc == ',')
                tok.advance();
            continue;
        }

        if (!tok.peek().is(lexer::TokenKind::Identifier)) {
            std::string msg = "expected parameter name but got ";
            if (tok.peek().kind == lexer::TokenKind::Punctuation)
                msg += "'" + std::string(1, tok.peek().punc) + "'";
            else
                msg += lexer::tokenKindName(tok.peek().kind);
            diag.report(diagnostics::Severity::Error, diagnostics::err::ExpectedIdent,
                        std::move(msg), tok.peek().span);
            tok.advance();
            continue;
        }
        auto name = tok.lexeme();
        tok.advance();
        auto type_expr = parser.parseOptTypeAnnotation();
        result.push({name, type_expr});

        if (tok.peek().punc == ',')
            tok.advance();
    }

    if (tok.peek().punc == ')')
        tok.advance();

    return result;
}

static memory::DynArray<ast::GenericParam> tryParseGenericParams(Parser &parser,
                                                                 memory::Arena &arena) {
    auto &tok   = *parser.tok;
    auto &diag  = *parser.diag;
    auto params = memory::DynArray<ast::GenericParam>(arena);
    if (!parser.consume('<'))
        return params;
    while (!tok.is_empty() && !tok.peek().is_eof()) {
        if (tok.peek().punc == '>') {
            diag.report(diagnostics::Severity::Error, diagnostics::err::ExpectedIdent,
                        "expected generic parameter name before '>'", tok.peek().span);
            break;
        }
        if (!tok.peek().is(lexer::TokenKind::Identifier)) {
            std::string msg = "expected generic parameter name but got ";
            msg += lexer::tokenKindName(tok.peek().kind);
            diag.report(diagnostics::Severity::Error, diagnostics::err::ExpectedIdent,
                        std::move(msg), tok.peek().span);
            break;
        }
        auto param_name = tok.lexeme();
        tok.advance();

        if (parser.consume(':'))
            scan_detail::skipTypeExpr(tok);

        params.push(ast::GenericParam{param_name, memory::DynArray<ast::TypeExprId>(arena)});

        if (!parser.consume(','))
            break;
    }
    if (!parser.consume('>')) {
        {
            std::string msg = "expected '>' to close generic parameters but got ";
            msg += lexer::tokenKindName(tok.peek().kind);
            diag.report(diagnostics::Severity::Error, diagnostics::err::ExpectedExpr,
                        std::move(msg), tok.peek().span);
        }
        recovery::panic(tok, {lexer::TokenKind::End});
    }
    return params;
}

// ── import path parsing ──────────────────────────────────────────────
static void parseImportPath(lexer::TokenStream &tok, memory::DynArray<std::string_view> &path) {
    if (tok.peek().is(lexer::TokenKind::LitVal)) {
        const auto literal = tok.lexeme();
        if (literal.size() >= 2U && literal.front() == '"' && literal.back() == '"') {
            path.push(std::string_view{literal.data() + 1, literal.size() - 2U});
            tok.advance();
            return;
        }
    }
    while (!tok.is_empty() && !tok.peek().is_eof()) {
        if (tok.peek().is(lexer::TokenKind::Identifier)) {
            auto first            = tok.lexeme();
            const char *seg_start = first.data();
            const char *seg_end   = first.data() + first.size();
            tok.advance();
            while (!tok.is_empty() && !tok.peek().is_eof() && tok.peek().punc == '.') {
                if (tok.peek(1).is(lexer::TokenKind::Punctuation) && tok.peek(1).punc == '.')
                    break;
                tok.advance();
                if (tok.peek().is(lexer::TokenKind::Identifier)) {
                    auto ext = tok.lexeme();
                    seg_end  = ext.data() + ext.size();
                    tok.advance();
                } else {
                    break;
                }
            }
            path.push(std::string_view{seg_start, static_cast<size_t>(seg_end - seg_start)});
        } else if (tok.peek().punc == '/') {
            tok.advance();
        } else if (tok.peek().is(lexer::TokenKind::Punctuation) && tok.peek().punc == '.') {
            if (tok.peek(1).is(lexer::TokenKind::Punctuation) && tok.peek(1).punc == '.') {
                path.push(std::string_view{"..", 2});
                tok.advance(2);
            } else {
                break;
            }
        } else {
            break;
        }
    }
}

static int32_t parseImportDepth(lexer::TokenStream &tok, diagnostics::DiagnosticEngine &diag) {
    if (tok.peek().punc == '(') {
        tok.advance();
        if (tok.peek().punc == '.') {
            tok.advance();
            tok.advance();
            if (tok.peek().punc == ')')
                tok.advance();
            return -1;
        } else if (tok.peek().is(lexer::TokenKind::LitVal)) {
            auto n = tok.lexeme();
            tok.advance();
            char *end = nullptr;
            long val  = std::strtol(n.data(), &end, 10);
            if (end == n.data() || val <= 0 || val > INT32_MAX) {
                diag.report(diagnostics::Severity::Error, diagnostics::err::InvalidImportDepth,
                            "import depth must be a positive integer or '..'", tok.peek().span);
                val = 1;
            }
            if (tok.peek().punc == ')')
                tok.advance();
            return static_cast<int32_t>(val);
        } else {
            diag.report(diagnostics::Severity::Error, diagnostics::err::InvalidImportDepth,
                        "expected import depth: positive integer or '..'", tok.peek().span);
            if (tok.peek().punc == ')')
                tok.advance();
            return 1;
        }
    }
    return 1;
}

static memory::DynArray<ast::ImportSymbol> parseImportSymbols(lexer::TokenStream &tok,
                                                              memory::Arena &arena,
                                                              diagnostics::DiagnosticEngine &diag) {
    auto symbols = memory::DynArray<ast::ImportSymbol>{arena};
    if (tok.peek().punc != '{')
        return symbols;
    tok.advance(); // consume '{'
    while (!tok.is_empty() && !tok.peek().is_eof() && tok.peek().punc != '}') {
        if (!tok.peek().is(lexer::TokenKind::Identifier)) {
            {
                std::string msg = "expected symbol name in import list but got ";
                msg += lexer::tokenKindName(tok.peek().kind);
                diag.report(diagnostics::Severity::Error, diagnostics::err::ExpectedIdent,
                            std::move(msg), tok.peek().span);
            }
            break;
        }
        auto name = tok.lexeme();
        tok.advance();
        std::string_view alias{};
        if (tok.peek().is(lexer::TokenKind::As)) {
            tok.advance();
            if (tok.peek().is(lexer::TokenKind::Identifier)) {
                alias = tok.lexeme();
                tok.advance();
            } else {
                std::string alias_msg = "expected alias name after 'as' but got ";
                alias_msg += lexer::tokenKindName(tok.peek().kind);
                diag.report(diagnostics::Severity::Error, diagnostics::err::ExpectedIdent,
                            std::move(alias_msg), tok.peek().span);
            }
        }
        symbols.push({name, alias});
        if (tok.peek().punc == ',')
            tok.advance();
    }
    if (tok.peek().punc == '}')
        tok.advance();
    else {
        std::string msg = "expected '}' to close import list but got ";
        msg += lexer::tokenKindName(tok.peek().kind);
        diag.report(diagnostics::Severity::Error, diagnostics::err::ExpectedExpr, std::move(msg),
                    tok.peek().span);
    }
    return symbols;
}

static void scanImportDecl(Parser &parser, symbols::SymbolTable & /*syms*/,
                           ast::ProgramNode &program, memory::Span kw_span, bool is_from,
                           bool is_export) {
    using namespace diagnostics::err;
    auto &tok  = *parser.tok;
    auto &bld  = *parser.bld;
    auto &diag = *parser.diag;

    memory::DynArray<std::string_view> path{bld.arena()};
    parseImportPath(tok, path);

    if (path.empty()) {
        std::string kw = is_from ? "from" : is_export ? "export" : "import";
        diag.report(diagnostics::Severity::Error, ImportError,
                    "expected import path after '" + kw + "'", tok.peek().span);
        return;
    }

    auto import_depth = parseImportDepth(tok, diag);
    auto syms_list    = parseImportSymbols(tok, bld.arena(), diag);

    bool is_asset = (is_from || is_export) && path[0] == "assets";

    std::string_view alias{};
    if (tok.peek().is(lexer::TokenKind::As)) {
        tok.advance();
        if (tok.peek().is(lexer::TokenKind::Identifier)) {
            alias = tok.lexeme();
            tok.advance();
        }
    }

    if (is_asset && alias.empty())
        diag.report(diagnostics::Severity::Error, ImportError,
                    "assets import requires an alias using 'as'", kw_span);

    auto decl = bld.importDecl(std::move(path), std::move(syms_list), alias, is_from, is_export,
                               is_asset, import_depth, kw_span);
    program.decls.push(decl);
}

// ── method scanning context ─────────────────────────────────────────
struct MethodScanContext {
    lexer::TokenStream &tok;
    ast::AstBuilder &bld;
    diagnostics::DiagnosticEngine &diag;
    symbols::SymbolTable &syms;
    Parser &parser;
    symbols::SymbolVisibility current_vis;
    int32_t current_mod_depth;
    symbols::SymId struct_sym;
    ast::DeclId decl;
    ast::ProgramNode &program;
    ScanResult &result;
};

static void scanMethod(MethodScanContext &ctx, bool must_have_body) {
    using namespace diagnostics::err;
    ctx.tok.advance(); // consume 'fn'

    if (ctx.tok.peek().punc == '<')
        scan_detail::skipBalanced(ctx.tok, '<', '>');

    if (!ctx.tok.peek().is(lexer::TokenKind::Identifier)) {
        std::string msg = "expected method name but got ";
        msg += lexer::tokenKindName(ctx.tok.peek().kind);
        ctx.diag.report(diagnostics::Severity::Error, ExpectedIdent, std::move(msg),
                        ctx.tok.peek().span);
        ctx.tok.advance();
        return;
    }
    auto mname_span = ctx.tok.peek().span;
    auto mname      = ctx.tok.lexeme();
    ctx.tok.advance();

    if (ctx.tok.peek().punc != '(') {
        std::string msg = "expected '(' after method name but got ";
        if (ctx.tok.peek().is(lexer::TokenKind::Punctuation))
            msg += "'" + std::string(1, ctx.tok.peek().punc) + "'";
        else if (ctx.tok.peek().is_eof())
            msg += "end of file";
        else
            msg += lexer::tokenKindName(ctx.tok.peek().kind);
        ctx.diag.report(diagnostics::Severity::Error, ExpectedExpr, std::move(msg),
                        ctx.tok.peek().span);
        return;
    }
    ctx.tok.advance();

    auto typed_params = parseFnParams(ctx.parser);

    ast::TypeExprId return_type = ast::kInvalidTypeExpr;
    if (ctx.tok.peek().punc == ':') {
        ctx.tok.advance();
        return_type = ctx.parser.parseTypeExpr();
    }

    ast::ExprId mn_body_node = ast::kInvalidExpr;
    uint32_t mn_body_start   = 0;
    uint32_t mn_body_end     = 0;
    memory::Span mn_span{};
    if (ctx.tok.peek().punc == '{') {
        mn_body_start = ctx.tok.offset;
        mn_span       = ctx.tok.peek().span;
        mn_body_end   = scan_detail::skipBody(ctx.tok);
        mn_span       = {mn_span.file, mn_span.start, mn_body_end};
        mn_body_node  = ctx.bld.unbody(mn_span, mn_body_start, mn_body_end);
    } else if (ctx.tok.peek().punc == ';') {
        if (must_have_body)
            ctx.diag.report(diagnostics::Severity::Error, ExpectedExpr,
                            "methods in component must have a body (got ';')", ctx.tok.peek().span);
        ctx.tok.advance();
    } else {
        ctx.diag.report(diagnostics::Severity::Error, ExpectedExpr,
                        "expected '{' or ';' after method signature", ctx.tok.peek().span);
    }

    if (!scan_detail::reportIfDuplicate(ctx.syms, ctx.diag, mname, mname_span)) {
        auto ms = ctx.syms.declare(mname, ctx.current_vis, ctx.current_mod_depth,
                                   symbols::SymKind::Fn, ast::kInvalidDecl, mname_span);
        for (auto &p : typed_params) {
            auto ps = ctx.syms.declare(p.name, symbols::SymbolVisibility::Private, 0,
                                       symbols::SymKind::Variable, ast::kInvalidDecl, mname_span);
            if (ms != symbols::kInvalidSym)
                ctx.syms.get(ms).members.push(ps);
        }
        auto method_decl =
            ctx.bld.fnDecl(mname, memory::DynArray<ast::GenericParam>{ctx.bld.arena()},
                           std::move(typed_params), return_type, mn_body_node, false, mname_span);
        ctx.program.decls.push(method_decl);
        if (ms != symbols::kInvalidSym)
            ctx.syms.get(ms).decl_id = method_decl;
        if (ctx.struct_sym != symbols::kInvalidSym)
            ctx.syms.get(ctx.struct_sym).members.push(ms);
        if (ctx.decl != ast::kInvalidDecl) {
            auto &decl_node = ctx.bld.getDecl(ctx.decl);
            if (auto *sn = ast::asStruct(decl_node))
                sn->methods.push(method_decl);
            else if (auto *cn = ast::asComponent(decl_node))
                cn->methods.push(method_decl);
        }
        ctx.result.fns.push({mname, mn_span, mn_body_node, 0});
    }
}

static void scanFieldItem(lexer::TokenStream &tok, diagnostics::DiagnosticEngine &diag,
                          memory::FlatMap<std::string, char> &field_names,
                          bool support_destructure) {
    auto consumed = scan_detail::consumeFieldItem(
        tok,
        scan_detail::FieldItemOptions{
            .support_destructure = support_destructure,
            .skip_qualifiers     = support_destructure,
            .parse_bind          = false,
        },
        [&]() {
            if (tok.peek().punc == ':')
                tok.advance();
            scan_detail::skipTypeExpr(tok);
            return false;
        },
        [&]() {
            scan_detail::skipExpr(tok);
            return false;
        },
        []() { return false; },
        [&](std::string_view fname, memory::Span fspan, bool, bool, ast::FieldBind) {
            if (field_names.contains(std::string(fname)))
                diag.report(diagnostics::Severity::Error, diagnostics::err::DuplicateDecl,
                            "duplicate field '" + std::string(fname) + "'", fspan);
            else
                field_names.insert(std::string(fname), char(1));
        });

    if (!consumed)
        tok.advance();
}

static void scanStructLikeFieldBody(lexer::TokenStream &tok, diagnostics::DiagnosticEngine &diag,
                                    ast::AstBuilder &bld, ast::ExprId &body_node,
                                    uint32_t &token_start, memory::Span &body_span,
                                    MethodScanContext &ctx, bool support_destructure,
                                    bool methods_must_have_body) {
    memory::FlatMap<std::string, char> field_names;
    scan_detail::scanBody(
        tok,
        [&](lexer::TokenStream &) { scanFieldItem(tok, diag, field_names, support_destructure); },
        [&](lexer::TokenStream &) { scanMethod(ctx, methods_must_have_body); }, token_start,
        body_span, bld, body_node);
}

static void scanEnumVariantItem(lexer::TokenStream &tok, diagnostics::DiagnosticEngine &diag,
                                symbols::SymbolTable &syms, ast::DeclId decl) {
    if (tok.peek().is(lexer::TokenKind::Identifier)) {
        auto v_span = tok.peek().span;
        auto vname  = tok.lexeme();
        tok.advance();
        if (!scan_detail::reportIfDuplicate(syms, diag, vname, v_span)) {
            auto vs = syms.declare(vname, symbols::SymbolVisibility::Private, 0,
                                   symbols::SymKind::Variable, ast::kInvalidDecl, v_span);
            if (decl != ast::kInvalidDecl && vs != symbols::kInvalidSym)
                syms.get(vs).decl_id = decl;
        }
        if (tok.peek().punc == '(')
            scan_detail::skipBalanced(tok, '(', ')');
        if (tok.peek().punc == '{')
            scan_detail::skipBalanced(tok, '{', '}');
        if (tok.peek().punc == '=') {
            tok.advance();
            scan_detail::skipExpr(tok);
        }
    } else {
        tok.advance();
    }
    if (tok.peek().punc == ',')
        tok.advance();
}

static void scanEnumBody(lexer::TokenStream &tok, diagnostics::DiagnosticEngine &diag,
                         symbols::SymbolTable &syms, ast::DeclId decl, ast::AstBuilder &bld,
                         ast::ExprId &body_node, uint32_t &token_start, memory::Span &body_span,
                         MethodScanContext &ctx) {
    scan_detail::scanBody(
        tok, [&](lexer::TokenStream &) { scanEnumVariantItem(tok, diag, syms, decl); },
        [&](lexer::TokenStream &) { scanMethod(ctx, false); }, token_start, body_span, bld,
        body_node);
}

static void scanUnionBody(lexer::TokenStream &tok, ast::AstBuilder &bld, ast::ExprId &body_node,
                          uint32_t &token_start, memory::Span &body_span, MethodScanContext &ctx) {
    scan_detail::scanBody(
        tok,
        [&](lexer::TokenStream &) {
            tok.advance();
            if (tok.peek().punc == ',')
                tok.advance();
        },
        [&](lexer::TokenStream &) { scanMethod(ctx, false); }, token_start, body_span, bld,
        body_node);
}

enum class StructLikeKind : uint8_t {
    Struct,
    Enum,
    Union,
    Component,
};

static auto structLikeKindOf(std::string_view kw) -> StructLikeKind {
    if (kw == "enum")
        return StructLikeKind::Enum;
    if (kw == "union")
        return StructLikeKind::Union;
    if (kw == "component")
        return StructLikeKind::Component;
    return StructLikeKind::Struct;
}

static auto symbolKindOf(StructLikeKind kind) -> symbols::SymKind {
    switch (kind) {
    case StructLikeKind::Struct:
        return symbols::SymKind::Struct;
    case StructLikeKind::Enum:
        return symbols::SymKind::Enum;
    case StructLikeKind::Union:
        return symbols::SymKind::Union;
    case StructLikeKind::Component:
        return symbols::SymKind::Component;
    }
    return symbols::SymKind::Struct;
}

static auto createStructLikeDecl(ast::AstBuilder &bld, StructLikeKind kind, std::string_view name,
                                 memory::DynArray<ast::GenericParam> generic_params,
                                 ast::TypeExprId enum_underlying, bool is_raw,
                                 memory::Span name_span) -> ast::DeclId {
    switch (kind) {
    case StructLikeKind::Struct:
        return bld.structDecl(
            name, std::move(generic_params), memory::DynArray<ast::StructField>(bld.arena()),
            memory::DynArray<ast::DeclId>(bld.arena()), ast::kInvalidTypeExpr, name_span);
    case StructLikeKind::Enum:
        return bld.enumDecl(name, std::move(generic_params),
                            memory::DynArray<ast::EnumVariant>(bld.arena()), enum_underlying,
                            name_span);
    case StructLikeKind::Union:
        return bld.unionDecl(name, std::move(generic_params),
                             memory::DynArray<ast::UnionVariant>(bld.arena()), is_raw, name_span);
    case StructLikeKind::Component:
        return bld.componentDecl(name, std::move(generic_params),
                                 memory::DynArray<ast::StructField>(bld.arena()), name_span);
    }
    return ast::kInvalidDecl;
}

static void scanStructLikeBody(StructLikeKind kind, lexer::TokenStream &tok,
                               diagnostics::DiagnosticEngine &diag, symbols::SymbolTable &syms,
                               ast::AstBuilder &bld, ast::DeclId decl, ast::ExprId &body_node,
                               uint32_t &token_start, memory::Span &body_span,
                               MethodScanContext &ctx) {
    switch (kind) {
    case StructLikeKind::Struct:
        scanStructLikeFieldBody(tok, diag, bld, body_node, token_start, body_span, ctx, true,
                                false);
        break;
    case StructLikeKind::Enum:
        scanEnumBody(tok, diag, syms, decl, bld, body_node, token_start, body_span, ctx);
        break;
    case StructLikeKind::Union:
        scanUnionBody(tok, bld, body_node, token_start, body_span, ctx);
        break;
    case StructLikeKind::Component:
        scanStructLikeFieldBody(tok, diag, bld, body_node, token_start, body_span, ctx, false,
                                true);
        break;
    }
}

static void pushStructLikeEntry(ScanResult &result, StructLikeKind kind, std::string_view name,
                                memory::Span body_span, ast::ExprId body_node,
                                uint32_t struct_header_start) {
    ScanEntry entry{name, body_span, body_node, struct_header_start};
    switch (kind) {
    case StructLikeKind::Struct:
        result.structs.push(entry);
        break;
    case StructLikeKind::Enum:
        result.enums.push(entry);
        break;
    case StructLikeKind::Union:
        result.unions.push(entry);
        break;
    case StructLikeKind::Component:
        result.components.push(entry);
        break;
    }
}

ast::DeclId scan_detail::scanGlobalOrConst(Parser &parser, symbols::SymbolTable &syms,
                                           memory::DynArray<ast::DeclId> &program_decls,
                                           bool is_const, memory::Span kw_span,
                                           symbols::SymbolVisibility vis, int32_t mod_depth,
                                           memory::Span doc_span) {
    auto &tok  = *parser.tok;
    auto &bld  = *parser.bld;
    auto &diag = *parser.diag;

    if (!tok.peek().is(lexer::TokenKind::Identifier)) {
        std::string msg = "expected variable name but got ";
        if (tok.peek().kind == lexer::TokenKind::Punctuation)
            msg += "'" + std::string(1, tok.peek().punc) + "'";
        else
            msg += lexer::tokenKindName(tok.peek().kind);
        diag.report(diagnostics::Severity::Error, diagnostics::err::ExpectedIdent, std::move(msg),
                    tok.peek().span);
        tok.advance();
        return ast::kInvalidDecl;
    }

    auto name_span = tok.peek().span;
    auto name      = tok.lexeme();
    tok.advance();

    auto type_annot = parser.parseOptTypeAnnotation();

    if (!parser.consume('=')) {
        diag.report(diagnostics::Severity::Error, diagnostics::err::ExpectedExpr,
                    "expected '=' and initializer", name_span);
        while (!tok.is_empty() && !tok.peek().is_eof() && tok.peek().punc != ';' &&
               tok.peek().punc != '}')
            tok.advance();
        if (tok.peek().punc == ';')
            tok.advance();
        return ast::kInvalidDecl;
    }
    auto init = parser.parseExpr();

    if (!parser.consume(';'))
        diag.report(diagnostics::Severity::Error, diagnostics::err::ExpectedSemicolon,
                    "expected ';' after declaration", tok.peek().span);

    auto decl = bld.globalDecl(name, is_const, type_annot, init,
                               scan_detail::spanFromOffset(kw_span.start, kw_span.end));
    auto sym  = syms.declare(name, vis, mod_depth, symbols::SymKind::Variable, decl, name_span,
                             symbols::kInvalidSym, doc_span);
    if (sym != symbols::kInvalidSym)
        syms.get(sym).decl_id = decl;
    program_decls.push(decl);
    return decl;
}

ScanResult scan(Parser &parser, symbols::SymbolTable &syms) {
    using namespace diagnostics::err;
    auto &tok     = *parser.tok;
    auto &bld     = *parser.bld;
    auto &diag    = *parser.diag;
    auto &program = parser.program;

    ScanResult result{bld.arena()};
    symbols::SymbolVisibility current_vis = symbols::SymbolVisibility::Private;
    int32_t current_mod_depth             = 0;

    memory::Span lastDocSpan{};

    auto resetVis = [&]() {
        current_vis       = symbols::SymbolVisibility::Private;
        current_mod_depth = 0;
        lastDocSpan       = {};
    };

    // Per-file error limit: suppress further scan errors beyond this threshold.
    static constexpr size_t kMaxScanErrors = 100;

    while (!tok.is_empty()) {
        if (tok.peek().is_eof())
            break;

        // Check error budget; if exceeded emit a suppression note and stop scanning.
        if (diag.errorCount() >= kMaxScanErrors) {
            diag.report(diagnostics::Severity::Note, diagnostics::err::ExpectedExpr,
                        "too many errors (scan stopped after 100 syntax errors)", tok.peek().span);
            break;
        }

        // ── capture doc comments ───────────────────────────────────
        if (tok.peek().is(lexer::TokenKind::Docs)) {
            if (lastDocSpan.start == 0 && lastDocSpan.end == 0)
                lastDocSpan = tok.peek().span;
            else
                lastDocSpan.end = tok.peek().span.end;
            tok.advance();
            continue;
        }

        // ── skip plain comments ────────────────────────────────────
        if (tok.peek().is(lexer::TokenKind::Comments)) {
            tok.advance();
            continue;
        }

        // ── visibility modifier (pub / mod) ────────────────────────
        if (tok.peek().is(lexer::TokenKind::Visibility)) {
            auto kw = tok.lexeme();
            tok.advance();

            if (kw == "pub") {
                current_vis = symbols::SymbolVisibility::Public;
                continue;
            }

            if (kw == "mod") {
                current_vis = symbols::SymbolVisibility::Module;
                if (tok.peek().punc == '(') {
                    tok.advance();
                    if (tok.peek().punc == '.') {
                        tok.advance(); // '.'
                        tok.advance(); // '.'
                        current_mod_depth = -1;
                        if (tok.peek().punc == ')')
                            tok.advance();
                    } else if (tok.peek().is(lexer::TokenKind::LitVal)) {
                        auto n = tok.lexeme();
                        tok.advance();
                        char *end = nullptr;
                        long val  = std::strtol(n.data(), &end, 10);
                        if (end == n.data() || val < 1 || val > INT32_MAX) {
                            diag.report(diagnostics::Severity::Error, InvalidImportDepth,
                                        "mod depth must be a positive integer", tok.peek().span);
                            val = 1;
                        }
                        current_mod_depth = static_cast<int32_t>(val);
                        if (tok.peek().punc == ')')
                            tok.advance();
                    } else {
                        current_mod_depth = 1;
                        if (tok.peek().punc == ')')
                            tok.advance();
                    }
                } else {
                    current_mod_depth = 1;
                }
                continue;
            }
        }
        // ── fn declaration ─────────────────────────────────────────
        bool is_extern = tok.peek().is(lexer::TokenKind::Extern);
        if (scan_detail::consumeFnModifiers(tok)) {
            tok.advance(); // consume 'fn'

            if (!tok.peek().is(lexer::TokenKind::Identifier)) {
                std::string msg = "expected function name but got ";
                msg += lexer::tokenKindName(tok.peek().kind);
                diag.report(diagnostics::Severity::Error, ExpectedIdent, std::move(msg),
                            tok.peek().span);
                tok.advance();
                resetVis();
                continue;
            }

            auto name_span = tok.peek().span;
            auto name      = tok.lexeme();
            tok.advance();

            auto generic_params = tryParseGenericParams(parser, bld.arena());

            if (tok.peek().punc != '(') {
                std::string msg = "expected '(' after function name but got ";
                if (tok.peek().is(lexer::TokenKind::Punctuation))
                    msg += "'" + std::string(1, tok.peek().punc) + "'";
                else if (tok.peek().is_eof())
                    msg += "end of file";
                else
                    msg += lexer::tokenKindName(tok.peek().kind);
                diag.report(diagnostics::Severity::Error, ExpectedExpr, std::move(msg),
                            tok.peek().span);
                resetVis();
                continue;
            }
            tok.advance();

            auto typed_params = parseFnParams(parser);

            // parse return type annotation : TypeExpr
            ast::TypeExprId return_type = ast::kInvalidTypeExpr;
            if (tok.peek().punc == ':') {
                tok.advance();
                return_type = parser.parseTypeExpr();
            }

            ast::ExprId body_node = ast::kInvalidExpr;
            uint32_t token_start  = 0;
            uint32_t token_end    = 0;
            memory::Span body_span{};

            if (tok.peek().punc == '{') {
                token_start = tok.offset;
                body_span   = tok.peek().span;
                token_end   = scan_detail::skipBody(tok);
                body_span   = {body_span.file, body_span.start, token_end};
                body_node   = bld.unbody(body_span, token_start, token_end);
            }

            auto fn_sym =
                syms.declare(name, current_vis, current_mod_depth, symbols::SymKind::Fn,
                             ast::kInvalidDecl, name_span, symbols::kInvalidSym, lastDocSpan);
            for (auto &p : typed_params) {
                auto ps = syms.declare(p.name, symbols::SymbolVisibility::Private, 0,
                                       symbols::SymKind::Variable, ast::kInvalidDecl, name_span);
                if (fn_sym != symbols::kInvalidSym)
                    syms.get(fn_sym).members.push(ps);
            }
            lastDocSpan = {};

            auto decl = bld.fnDecl(name, std::move(generic_params), std::move(typed_params),
                                   return_type, body_node, is_extern,
                                   scan_detail::spanFromOffset(name_span.start, name_span.end));
            program.decls.push(decl);
            if (fn_sym != symbols::kInvalidSym)
                syms.get(fn_sym).decl_id = decl;

            result.fns.push({name, body_span, body_node, 0});
            resetVis();
            continue;
        }

        // ── variable declaration
        if (tok.peek().is(lexer::TokenKind::Variable)) {
            auto kw_span = tok.peek().span;
            auto kw      = tok.lexeme();
            tok.advance();
            if (kw == "global" || kw == "const") {
                scan_detail::scanGlobalOrConst(parser, syms, program.decls, kw == "const", kw_span,
                                               current_vis, current_mod_depth, lastDocSpan);
            } else {
                diag.report(
                    diagnostics::Severity::Error, TopLevelLetNotAllowed,
                    "You cant declare variables at top-level", kw_span,
                    {"Use `global` (global variable) or `const` (comptime variable) instead"});
                recovery::panic(tok);
                if (tok.peek().punc == ';')
                    tok.advance();
            }
            resetVis();
            continue;
        }

        // ── struct-like declaration (struct / enum / union / component) ─
        if (tok.peek().is(lexer::TokenKind::Struct) ||
            (tok.peek().is(lexer::TokenKind::Raw) && tok.peek(1).is(lexer::TokenKind::Struct) &&
             tok.lexeme(tok.peek(1)) == "union")) {
            const bool is_raw_union = tok.peek().is(lexer::TokenKind::Raw);
            if (is_raw_union)
                tok.advance();
            auto kw = tok.lexeme();
            tok.advance();

            if (!tok.peek().is(lexer::TokenKind::Identifier)) {
                {
                    std::string msg = "expected name after '" + std::string(kw) + "' but got ";
                    msg += lexer::tokenKindName(tok.peek().kind);
                    diag.report(diagnostics::Severity::Error, ExpectedIdent, std::move(msg),
                                tok.peek().span);
                }
                // Recover the malformed declaration as a whole so the next
                // top-level declaration can still be collected deterministically.
                if (tok.peek().punc == '{') {
                    scan_detail::skipBalanced(tok, '{', '}');
                } else if (!tok.peek().is_eof()) {
                    tok.advance();
                }
                recovery::panic(tok, {lexer::TokenKind::Fn, lexer::TokenKind::Struct,
                                      lexer::TokenKind::Trait, lexer::TokenKind::Interface,
                                      lexer::TokenKind::Implement, lexer::TokenKind::Module,
                                      lexer::TokenKind::Variable, lexer::TokenKind::Extern,
                                      lexer::TokenKind::Word, lexer::TokenKind::Context});
                resetVis();
                continue;
            }

            auto name_span = tok.peek().span;
            auto name      = tok.lexeme();
            tok.advance();

            auto generic_params = tryParseGenericParams(parser, bld.arena());
            auto struct_like    = structLikeKindOf(kw);

            ast::TypeExprId enum_underlying = ast::kInvalidTypeExpr;
            if (struct_like == StructLikeKind::Enum && tok.peek().punc == ':') {
                tok.advance();
                enum_underlying = parser.parseTypeExpr();
            }

            uint32_t struct_header_start = tok.offset;

            // skip extends/traits tokens to reach '{' or ';'
            while (!tok.is_empty() && tok.peek().punc != '{' && tok.peek().punc != ';') {
                if (tok.peek().is_eof())
                    break;
                if (tok.peek().punc == '(') {
                    scan_detail::skipBalanced(tok, '(', ')');
                    continue;
                }
                if (tok.peek().punc == '<') {
                    scan_detail::skipBalanced(tok, '<', '>');
                    continue;
                }
                tok.advance();
            }

            auto decl             = ast::kInvalidDecl;
            ast::ExprId body_node = ast::kInvalidExpr;
            memory::Span body_span{};

            decl = createStructLikeDecl(bld, struct_like, name, std::move(generic_params),
                                        enum_underlying, is_raw_union, name_span);

            // declare struct/union/enum symbol BEFORE body scan
            symbols::SymId struct_sym = symbols::kInvalidSym;
            if (!scan_detail::reportIfDuplicate(syms, diag, name, name_span))
                struct_sym =
                    syms.declare(name, current_vis, current_mod_depth, symbolKindOf(struct_like),
                                 decl, name_span, symbols::kInvalidSym, lastDocSpan);

            // ── scan body: register member names, skip implementations ──
            if (tok.peek().punc == '{') {
                uint32_t token_start = tok.offset;
                body_span            = tok.peek().span;

                MethodScanContext ctx{
                    tok,        bld,  diag,    syms,  parser, current_vis, current_mod_depth,
                    struct_sym, decl, program, result};

                scanStructLikeBody(struct_like, tok, diag, syms, bld, decl, body_node, token_start,
                                   body_span, ctx);
            }

            program.decls.push(decl);
            lastDocSpan = {};
            pushStructLikeEntry(result, struct_like, name, body_span, body_node,
                                struct_header_start);

            resetVis();
            continue;
        }

        // ── trait / interface declaration ────────────────────────────
        if (tok.peek().is(lexer::TokenKind::Trait) || tok.peek().is(lexer::TokenKind::Interface)) {
            auto kw = tok.lexeme();
            tok.advance();

            if (!tok.peek().is(lexer::TokenKind::Identifier)) {
                {
                    std::string msg = "expected name after '" + std::string(kw) + "' but got ";
                    msg += lexer::tokenKindName(tok.peek().kind);
                    diag.report(diagnostics::Severity::Error, ExpectedIdent, std::move(msg),
                                tok.peek().span);
                }
                tok.advance();
                resetVis();
                continue;
            }

            auto name_span = tok.peek().span;
            auto name      = tok.lexeme();
            tok.advance();

            auto generic_params = tryParseGenericParams(parser, bld.arena());

            symbols::SymKind sk =
                (kw == "interface") ? symbols::SymKind::Interface : symbols::SymKind::Trait;
            auto decl =
                (kw == "interface")
                    ? bld.interfaceDecl(name, std::move(generic_params),
                                        memory::DynArray<ast::TraitMethod>(bld.arena()), name_span)
                    : bld.traitDecl(name, std::move(generic_params),
                                    memory::DynArray<ast::TraitMethod>(bld.arena()), name_span);

            ast::ExprId body_node = ast::kInvalidExpr;
            memory::Span body_span{};

            // ── scan trait body: register method names ──
            if (tok.peek().punc == '{') {
                uint32_t token_start = tok.offset;
                body_span            = tok.peek().span;

                scan_detail::scanBody(
                    tok,
                    [&](lexer::TokenStream &) {
                        if (tok.peek().is(lexer::TokenKind::Typedef)) {
                            tok.advance();
                            while (!tok.is_empty() && !tok.peek().is_eof() &&
                                   tok.peek().punc != ';')
                                tok.advance();
                            if (tok.peek().punc == ';')
                                tok.advance();
                            return;
                        }
                        tok.advance();
                    },
                    [&](lexer::TokenStream &) {
                        tok.advance(); // consume 'fn'
                        if (tok.peek().punc == '<')
                            scan_detail::skipBalanced(tok, '<', '>');
                        if (tok.peek().is(lexer::TokenKind::Identifier)) {
                            auto m_span = tok.peek().span;
                            auto mname  = tok.lexeme();
                            tok.advance();
                            if (!scan_detail::reportIfDuplicate(syms, diag, mname, m_span)) {
                                auto ms =
                                    syms.declare(mname, symbols::SymbolVisibility::Private, 0,
                                                 symbols::SymKind::Fn, ast::kInvalidDecl, m_span);
                                if (decl != ast::kInvalidDecl && ms != symbols::kInvalidSym)
                                    syms.get(ms).decl_id = decl;
                            }
                        }
                        if (tok.peek().punc == '(')
                            scan_detail::skipBalanced(tok, '(', ')');
                        if (tok.peek().punc == ':') {
                            tok.advance();
                            scan_detail::skipTypeExpr(tok);
                        }
                        while (!tok.is_empty() && !tok.peek().is_eof()) {
                            if (tok.peek().punc == ';') {
                                tok.advance();
                                break;
                            }
                            if (tok.peek().punc == '{') {
                                scan_detail::skipBalanced(tok, '{', '}');
                                break;
                            }
                            if (tok.peek().punc == '}')
                                break;
                            tok.advance();
                        }
                    },
                    token_start, body_span, bld, body_node);
            }

            program.decls.push(decl);
            if (!scan_detail::reportIfDuplicate(syms, diag, name, name_span))
                syms.declare(name, current_vis, current_mod_depth, sk, decl, name_span,
                             symbols::kInvalidSym, lastDocSpan);
            lastDocSpan = {};

            result.traits.push({name, body_span, body_node});
            resetVis();
            continue;
        }

        // ── implement declaration ────────────────────────────────────
        if (tok.peek().is(lexer::TokenKind::Implement)) {
            tok.advance();

            // syntax: implement Type for Trait { ... }
            // or:      implement Trait for Type { ... }
            // during scan: skip to '{' and create unbody
            while (!tok.is_empty() && !tok.peek().is_eof()) {
                if (tok.peek().punc == '{') {
                    scan_detail::skipBalanced(tok, '{', '}');
                    break;
                }
                if (tok.peek().punc == ';') {
                    tok.advance();
                    break;
                }
                tok.advance();
            }

            resetVis();
            continue;
        }

        // ── import declaration (from / import / export) ─────────────
        if (tok.peek().is(lexer::TokenKind::Module)) {
            auto kw_span = tok.peek().span;
            auto kw      = tok.lexeme();
            tok.advance();

            // ── type alias: alias Name = TypeExpr ──────────────────
            if (kw == "alias") {
                if (!tok.peek().is(lexer::TokenKind::Identifier)) {
                    std::string msg = "expected alias name but got ";
                    msg += lexer::tokenKindName(tok.peek().kind);
                    diag.report(diagnostics::Severity::Error, ExpectedIdent, std::move(msg),
                                tok.peek().span);
                    resetVis();
                    continue;
                }
                auto name_span = tok.peek().span;
                auto name      = tok.lexeme();
                tok.advance();

                auto generic_params = tryParseGenericParams(parser, bld.arena());

                if (tok.peek().punc != '=') {
                    std::string msg = "expected '=' in type alias but got ";
                    msg += lexer::tokenKindName(tok.peek().kind);
                    diag.report(diagnostics::Severity::Error, ExpectedExpr, std::move(msg),
                                tok.peek().span);
                    resetVis();
                    continue;
                }
                tok.advance(); // consume '='

                auto target_type = parser.parseTypeExpr();

                if (tok.peek().punc == ';')
                    tok.advance();

                auto decl =
                    bld.typeAliasDecl(name, std::move(generic_params), target_type, kw_span);
                program.decls.push(decl);

                syms.declare(name, current_vis, current_mod_depth, symbols::SymKind::Alias, decl,
                             name_span, symbols::kInvalidSym, lastDocSpan);

                resetVis();
                continue;
            }

            if (kw == "from")
                scanImportDecl(parser, syms, program, kw_span, true, false);
            else if (kw == "export")
                scanImportDecl(parser, syms, program, kw_span, false, true);
            else
                scanImportDecl(parser, syms, program, kw_span, false, false);
            resetVis();
            continue;
        }

        // ── custom word declaration (prefix / suffix / infix / nop / context) ──
        if (tok.peek().is(lexer::TokenKind::Word) || tok.peek().is(lexer::TokenKind::Context)) {
            // auto kw_span = tok.peek().span;
            auto kw = tok.lexeme();
            if (kw == "prefix" || kw == "suffix" || kw == "infix" || kw == "nop" ||
                kw == "context") {
                tok.advance(); // consume kw

                if (kw == "context") {
                    if (!tok.peek().is(lexer::TokenKind::Identifier)) {
                        diag.report(diagnostics::Severity::Error, ExpectedIdent,
                                    "expected context name", tok.peek().span);
                        resetVis();
                        continue;
                    }
                    auto name_span = tok.peek().span;
                    auto name      = tok.lexeme();
                    tok.advance();

                    ast::ExprId body_node = ast::kInvalidExpr;
                    memory::Span body_span{};

                    if (tok.peek().punc == '{') {
                        uint32_t token_start = tok.offset;
                        body_span            = tok.peek().span;
                        uint32_t token_end   = scan_detail::skipBody(tok);
                        body_span            = {body_span.file, body_span.start, token_end};
                        body_node            = bld.unbody(body_span, token_start, token_end);
                    } else {
                        diag.report(diagnostics::Severity::Error, ExpectedExpr,
                                    "expected { after context name", tok.peek().span);
                    }

                    auto context_sym = syms.declare(name, current_vis, current_mod_depth,
                                                    symbols::SymKind::Context, ast::kInvalidDecl,
                                                    name_span, symbols::kInvalidSym, lastDocSpan);

                    auto decl = bld.contextDecl(
                        name, memory::DynArray<ast::DeclId>{bld.arena()}, body_node,
                        scan_detail::spanFromOffset(name_span.start, name_span.end));
                    program.decls.push(decl);
                    if (context_sym != symbols::kInvalidSym)
                        syms.get(context_sym).decl_id = decl;

                    result.contexts.push({name, body_span, body_node, 0});
                    resetVis();
                    continue;
                }

                // For words (prefix, suffix, infix, nop):
                std::string_view name;
                auto name_span = tok.peek().span;
                if (tok.peek().is(lexer::TokenKind::Identifier) ||
                    tok.peek().is(lexer::TokenKind::Word) ||
                    tok.peek().is(lexer::TokenKind::Operators) ||
                    tok.peek().is(lexer::TokenKind::Punctuation) ||
                    tok.peek().is(lexer::TokenKind::Logical) ||
                    tok.peek().is(lexer::TokenKind::Comparison)) {
                    name = tok.lexeme();
                    tok.advance();
                } else {
                    diag.report(diagnostics::Severity::Error, ExpectedIdent, "expected word name",
                                tok.peek().span);
                    resetVis();
                    continue;
                }

                ast::ExprId body_node = ast::kInvalidExpr;
                memory::Span body_span{};
                memory::DynArray<std::string_view> params{bld.arena()};

                if (kw == "nop") {
                    if (tok.peek().punc == ';') {
                        tok.advance();
                    } else {
                        diag.report(diagnostics::Severity::Error, ExpectedExpr,
                                    "expected ; after nop name", tok.peek().span);
                    }
                } else {
                    if (tok.peek().punc != '(') {
                        diag.report(diagnostics::Severity::Error, ExpectedExpr,
                                    "expected ( after word name", tok.peek().span);
                        resetVis();
                        continue;
                    }
                    tok.advance(); // consume (

                    while (!tok.is_empty() && tok.peek().punc != ')') {
                        if (tok.peek().is(lexer::TokenKind::Identifier) ||
                            tok.peek().is(lexer::TokenKind::Word)) {
                            params.push(tok.lexeme());
                            tok.advance();
                        } else {
                            diag.report(diagnostics::Severity::Error, ExpectedIdent,
                                        "expected parameter name", tok.peek().span);
                            tok.advance();
                        }
                        if (tok.peek().punc == ',') {
                            tok.advance();
                        }
                    }
                    if (tok.peek().punc == ')') {
                        tok.advance();
                    } else {
                        diag.report(diagnostics::Severity::Error, ExpectedExpr, "expected )",
                                    tok.peek().span);
                    }

                    if (kw == "prefix" && params.size() != 1) {
                        diag.report(diagnostics::Severity::Error, ExpectedExpr,
                                    "prefix word must have exactly 1 parameter", name_span);
                    } else if (kw == "suffix" && params.size() != 1) {
                        diag.report(diagnostics::Severity::Error, ExpectedExpr,
                                    "suffix word must have exactly 1 parameter", name_span);
                    } else if (kw == "infix" && params.size() != 2) {
                        diag.report(diagnostics::Severity::Error, ExpectedExpr,
                                    "infix word must have exactly 2 parameters", name_span);
                    }

                    if (tok.peek().punc == '{') {
                        uint32_t token_start = tok.offset;
                        body_span            = tok.peek().span;
                        uint32_t token_end   = scan_detail::skipBody(tok);
                        body_span            = {body_span.file, body_span.start, token_end};
                        body_node            = bld.unbody(body_span, token_start, token_end);
                    } else {
                        diag.report(diagnostics::Severity::Error, ExpectedExpr,
                                    "expected { before word body", tok.peek().span);
                    }
                }

                auto word_sym =
                    syms.declare(name, current_vis, current_mod_depth, symbols::SymKind::Word,
                                 ast::kInvalidDecl, name_span, symbols::kInvalidSym, lastDocSpan);

                ast::WordCategory category = ast::WordCategory::Nop;
                if (kw == "prefix")
                    category = ast::WordCategory::Prefix;
                else if (kw == "suffix")
                    category = ast::WordCategory::Suffix;
                else if (kw == "infix")
                    category = ast::WordCategory::Infix;

                auto decl =
                    bld.wordDecl(name, category, std::move(params), body_node,
                                 scan_detail::spanFromOffset(name_span.start, name_span.end));
                program.decls.push(decl);
                if (word_sym != symbols::kInvalidSym)
                    syms.get(word_sym).decl_id = decl;

                if (body_node != ast::kInvalidExpr) {
                    result.words.push({name, body_span, body_node, 0});
                }

                resetVis();
                continue;
            }
        }

        // ── @annotation — skip ─────────────────────────────────────
        if (tok.peek().is(lexer::TokenKind::Annotation)) {
            tok.advance();
            continue;
        }

        // ── unknown token → skip to ; or next declaration ─────────
        {
            auto &bad       = tok.peek();
            std::string msg = "unexpected ";
            if (bad.kind == lexer::TokenKind::Punctuation)
                msg += "'" + std::string(1, bad.punc) + "'";
            else
                msg += lexer::tokenKindName(bad.kind);
            msg += " in declaration context";
            diag.report(diagnostics::Severity::Error, ExpectedExpr, std::move(msg), bad.span);
        }
        recovery::panic(tok, {lexer::TokenKind::Fn, lexer::TokenKind::Struct,
                              lexer::TokenKind::Trait, lexer::TokenKind::Interface,
                              lexer::TokenKind::Implement, lexer::TokenKind::Module,
                              lexer::TokenKind::Variable, lexer::TokenKind::Extern});
        if (tok.peek().punc == ';')
            tok.advance();
        resetVis();
    }

    return result;
}

} // namespace zith::parser
