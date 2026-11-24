#include "ide/lsp-ide-api.hpp"

#include "cli/options.hpp"
#include "diagnostics/diagnostic.hpp"
#include "frontend/frontend.hpp"
#include "memory/source-file.hpp"
#include "memory/arena.hpp"
#include "memory/span.hpp"
#include "sema/modern-types.hpp"
#include "sema/sema-modern.hpp"
#include "session/compilation-session.hpp"
#include "session/frontend-context.hpp"
#include "symbols/symbol-table.hpp"
#include "symbols/symbol-id.hpp"
#include "types/type-kind.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <set>
#include <utility>
#include <unordered_map>

namespace zith::ide {

namespace {

Range rangeFromSource(const std::string &source, memory::FileId file_id,
                      uint32_t start, uint32_t end) {
    Range result{};
    uint32_t line = 0;
    uint32_t column = 0;
    const uint32_t source_size = static_cast<uint32_t>(source.size());
    for (uint32_t i = 0; i < start && i < source_size; ++i) {
        if (source[i] == '\n') {
            ++line;
            column = 0;
        } else {
            ++column;
        }
    }
    result.start.line = line;
    result.start.character = column;
    for (uint32_t i = start; i < end && i < source_size; ++i) {
        if (source[i] == '\n') {
            ++line;
            column = 0;
        } else {
            ++column;
        }
    }
    result.end.line = line;
    result.end.character = column;
    (void)file_id;
    return result;
}

Severity toIdeSeverity(diagnostics::Severity severity) {
    switch (severity) {
    case diagnostics::Severity::Note:
        return Severity::Note;
    case diagnostics::Severity::Warning:
        return Severity::Warning;
    case diagnostics::Severity::Bug:
        return Severity::Bug;
    case diagnostics::Severity::Error:
        return Severity::Error;
    }
    return Severity::Error;
}

std::vector<Diagnostic> collectDiagnostics(const session::CompilationSession &session,
                                           std::string_view source) {
    std::vector<Diagnostic> result;
    const auto &diagnostics = session.diags().all();
    result.reserve(diagnostics.size());
    for (const auto &diag : diagnostics) {
        Diagnostic out;
        out.range = rangeFromSource(std::string(source), diag.primary.file,
                                    diag.primary.start, diag.primary.end);
        out.severity = toIdeSeverity(diag.severity);
        out.code = diag.code;
        out.message = diag.message;
        if (!diag.suggestions.empty()) {
            out.suggestions.assign(diag.suggestions.begin(), diag.suggestions.end());
        }
        result.push_back(std::move(out));
    }
    return result;
}

std::string displayedFunctionName(const frontend::Declaration &decl);
int documentSymbolKind(const frontend::Declaration &decl);
void addMemberToSymbol(DocumentSymbol &symbol, const std::string &source,
                       memory::FileId file_id,
                       const frontend::Declaration &member);

/// Builds the outline label for a named block's marker, listing the marker
/// name and its resource arguments. A plain named block has no detail.
std::string namedBlockDetail(const frontend::FrontendSnapshot &frontend,
                             const std::string &source, const frontend::Expression &block) {
    if (!block.marker || block.marker.value > frontend.expressions().size()) {
        return {};
    }
    const auto &marker = frontend.expressions()[block.marker.value - 1U];
    if (marker.kind != frontend::ExprKind::MacroCall) {
        return {};
    }
    std::string detail = "@" + marker.text + "(";
    for (size_t i = 0; i < marker.operands.size(); ++i) {
        if (i != 0U) {
            detail += ", ";
        }
        const frontend::ExprId arg = marker.operands[i];
        if (arg && arg.value <= frontend.expressions().size()) {
            const auto &arg_expr = frontend.expressions()[arg.value - 1U];
            const uint32_t source_size = static_cast<uint32_t>(source.size());
            const uint32_t start = std::min(arg_expr.span.start, source_size);
            const uint32_t end   = std::min(arg_expr.span.end, source_size);
            detail.append(source, start, end - start);
        }
    }
    detail += ")";
    return detail;
}

/// Appends the named blocks declared in `body` (direct statements and named
/// pipeline stages) to `out`, in source order. A named block is a `Block`
/// expression carrying a descriptive `block_name`; its detail lists the marker
/// resources so the outline shows what each phase brackets.
void collectNamedBlockSymbols(const frontend::FrontendSnapshot &frontend,
                              const std::string &source, memory::FileId file_id,
                              frontend::ExprId body, std::vector<DocumentSymbol> &out) {
    if (!body || body.value > frontend.expressions().size()) {
        return;
    }
    std::function<void(frontend::ExprId)> visit = [&](frontend::ExprId id) {
        if (!id || id.value > frontend.expressions().size() ||
            frontend.isMacroTemplateExpr(id)) {
            return;
        }
        const auto &expr = frontend.expressions()[id.value - 1U];
        if (expr.kind == frontend::ExprKind::Block && !expr.block_name.empty()) {
            DocumentSymbol symbol;
            symbol.name           = expr.block_name;
            symbol.kind           = 3; // LSP SymbolKind.Namespace: a named scope.
            symbol.detail         = namedBlockDetail(frontend, source, expr);
            symbol.range = rangeFromSource(source, file_id, expr.span.start, expr.span.end);
            symbol.selectionRange = symbol.range;
            out.push_back(std::move(symbol));
            // Named blocks never nest, so there is nothing further to collect.
            return;
        }
        for (const auto operand : expr.operands) {
            visit(operand);
        }
        for (const auto stmt_id : expr.statements) {
            if (!stmt_id || stmt_id.value > frontend.statements().size()) {
                continue;
            }
            const auto &stmt = frontend.statements()[stmt_id.value - 1U];
            visit(stmt.expression);
        }
    };
    visit(body);
}

std::vector<DocumentSymbol> collectDocumentSymbols(const session::CompilationSession &session) {
    std::vector<DocumentSymbol> result;
    const auto &snapshot = session.snapshot();
    if (!snapshot || snapshot->modules().empty()) {
        return result;
    }
    const auto file_id = session.fileId();
    const frontend::FrontendSnapshot *frontend = nullptr;
    for (const auto &module : snapshot->modules()) {
        if (module && module->fileId == file_id && module->frontend) {
            frontend = module->frontend.get();
            break;
        }
    }
    if (!frontend) {
        return result;
    }
    const std::string &source = frontend->source();
    for (const auto &decl : frontend->declarations()) {
        if (decl.parentScope) {
            continue;
        }
        DocumentSymbol symbol;
        symbol.name = decl.name.empty() ? std::string("(unnamed)")
                                        : displayedFunctionName(decl);
        symbol.kind = documentSymbolKind(decl);
        symbol.range = rangeFromSource(source, file_id, decl.span.start, decl.span.end);
        symbol.selectionRange = symbol.range;
        if (decl.kind == frontend::DeclKind::Struct ||
            decl.kind == frontend::DeclKind::Union) {
            for (const auto &field : decl.parameters) {
                DocumentSymbol child;
                child.name = field.name.empty() ? std::string("(unnamed)") : field.name;
                child.kind = 6;
                child.range = rangeFromSource(source, file_id, field.span.start, field.span.end);
                child.selectionRange = child.range;
                symbol.children.push_back(std::move(child));
            }
        } else if (decl.kind == frontend::DeclKind::Function && decl.body) {
            // Named blocks are phases of a fn/state body, so they appear as
            // outline children of their function.
            collectNamedBlockSymbols(*frontend, source, file_id, decl.body, symbol.children);
        } else if (decl.kind == frontend::DeclKind::Enum) {
            for (const auto &variant : decl.parameters) {
                DocumentSymbol child;
                child.name = variant.name.empty() ? std::string("(unnamed)") : variant.name;
                child.kind = 13;
                child.range = rangeFromSource(source, file_id, variant.span.start, variant.span.end);
                child.selectionRange = child.range;
                symbol.children.push_back(std::move(child));
            }
        }
        result.push_back(std::move(symbol));
    }
    for (const auto &member : frontend->declarations()) {
        if (!member.parentScope || member.ownerName.empty()) {
            continue;
        }
        for (auto &owner : result) {
            if (owner.name.size() >= member.ownerName.size() &&
                owner.name.substr(owner.name.size() - member.ownerName.size()) ==
                    member.ownerName) {
                addMemberToSymbol(owner, source, file_id, member);
                break;
            }
        }
    }
    return result;
}

void addMemberToSymbol(DocumentSymbol &symbol, const std::string &source,
                       memory::FileId file_id,
                       const frontend::Declaration &member) {
    DocumentSymbol child;
    child.name = member.name.empty() ? std::string("(unnamed)")
                                     : displayedFunctionName(member);
    child.kind = documentSymbolKind(member);
    child.range = rangeFromSource(source, file_id, member.span.start, member.span.end);
    child.selectionRange = child.range;
    symbol.children.push_back(std::move(child));
}

std::string displayedFunctionName(const frontend::Declaration &decl) {
    if (decl.kind != frontend::DeclKind::Function) {
        return decl.name;
    }
    switch (decl.functionKind) {
    case frontend::FunctionKind::Const:
        return "const fn " + decl.name;
    case frontend::FunctionKind::Raw:
        return "raw fn " + decl.name;
    case frontend::FunctionKind::Extern:
        return "extern fn " + decl.name;
    case frontend::FunctionKind::State:
        return "state " + decl.name;
    case frontend::FunctionKind::Standard:
        return decl.name;
    }
}

int documentSymbolKind(const frontend::Declaration &decl) {
    switch (decl.kind) {
    case frontend::DeclKind::Function:
        return 12;
    case frontend::DeclKind::Struct:
    case frontend::DeclKind::Union:
        return 23;
    case frontend::DeclKind::Trait:
    case frontend::DeclKind::Interface:
        return 11;
    case frontend::DeclKind::Enum:
        return 10;
    case frontend::DeclKind::TypeAlias:
        return 3;
    case frontend::DeclKind::Variable:
        return 13;
    case frontend::DeclKind::Context:
        return 9;
    case frontend::DeclKind::Word:
        return 23;
    default:
        return 6;
    }
}

std::vector<CompletionItem> collectCompletionItems(const session::CompilationSession &session) {
    std::vector<CompletionItem> result;
    const auto &snapshot = session.snapshot();
    if (!snapshot) {
        return result;
    }
    const auto file_id = session.fileId();
    for (const auto &module : snapshot->modules()) {
        if (!module || module->fileId != file_id || !module->frontend) {
            continue;
        }
        for (const auto &decl : module->frontend->declarations()) {
            if (decl.name.empty()) {
                continue;
            }
            CompletionItem item;
            item.label = decl.name;
            item.kind = decl.kind == frontend::DeclKind::Function ? 3 : 6;
            item.detail = decl.kind == frontend::DeclKind::Function ? "fn" : "declaration";
            item.insertText = decl.name;
            item.dataKind = decl.kind == frontend::DeclKind::Function
                                ? "function"
                                : (decl.kind == frontend::DeclKind::Struct ? "struct"
                                                                           : "declaration");
            item.dataName = decl.name;
            result.push_back(std::move(item));
        }
        constexpr std::array<std::string_view, 14> keywords = {
            "state", "dock", "jump", "in", "and", "or", "xor",
            "fn",    "let",  "var",  "if", "when", "return", "struct",
        };
        for (const auto keyword : keywords) {
            CompletionItem item;
            item.label = std::string(keyword);
            item.kind = 14;
            item.detail = "keyword";
            item.dataKind = "keyword";
            result.push_back(std::move(item));
        }
        break;
    }
    return result;
}

SignatureHelp collectSignatureHelp(const session::CompilationSession &,
                                   const frontend::FrontendSnapshot &frontend,
                                   const std::string &source, uint32_t line,
                                   uint32_t character) {
    SignatureHelp result;
    uint32_t byte_offset = 0;
    uint32_t current_line = 0;
    for (uint32_t i = 0; i < source.size(); ++i) {
        if (current_line == line) {
            break;
        }
        if (source[i] == '\n') {
            ++current_line;
        }
        byte_offset = i + 1;
    }
    byte_offset += character;

    for (const auto &expr : frontend.expressions()) {
        if (expr.kind != frontend::ExprKind::Call || expr.operands.empty()) {
            continue;
        }
        if (expr.span.start > byte_offset || byte_offset > expr.span.end) {
            continue;
        }
        const auto callee_index = expr.operands.front().value;
        if (callee_index == 0 || callee_index > frontend.expressions().size()) {
            continue;
        }
        const auto &callee = frontend.expressions()[callee_index - 1U];
        const std::string callee_name =
            callee.text.empty()
                ? source.substr(callee.span.start, callee.span.size())
                : callee.text;
        if (callee_name.empty()) {
            continue;
        }

        const frontend::Declaration *decl = nullptr;
        for (const auto &candidate : frontend.declarations()) {
            if (candidate.name == callee_name &&
                candidate.kind == frontend::DeclKind::Function) {
                decl = &candidate;
                break;
            }
        }
        if (decl == nullptr) {
            continue;
        }

        SignatureInfo info;
        std::ostringstream label;
        label << frontend::functionSignature(frontend, *decl);
        info.label = label.str();
        for (const auto &parameter : decl->parameters) {
            ParameterInfo parameter_info;
            std::ostringstream parameter_label;
            if (!parameter.name.empty()) {
                parameter_label << parameter.name << ": ";
            }
            if (parameter.type) {
                parameter_label
                    << frontend::canonicalTypeString(frontend, parameter.type);
            } else {
                parameter_label << "?";
            }
            parameter_info.label = parameter_label.str();
            info.parameters.push_back(std::move(parameter_info));
        }
        result.signatures.push_back(std::move(info));
        if (expr.operands.size() > 1) {
            uint32_t arg_index = 0;
            for (size_t i = 1; i < expr.operands.size(); ++i) {
                const auto &arg =
                    frontend.expressions()[expr.operands[i].value - 1U];
                if (byte_offset >= arg.span.start && byte_offset <= arg.span.end) {
                    arg_index = static_cast<uint32_t>(i - 1U);
                    break;
                }
            }
            result.activeParameter = static_cast<int>(arg_index);
        }
        result.activeSignature = 0;
        return result;
    }
    return result;
}

Position positionAt(const std::string &source, uint32_t offset) {
    Position result;
    const uint32_t limit = static_cast<uint32_t>(source.size());
    for (uint32_t i = 0; i < offset && i < limit; ++i) {
        if (source[i] == '\n') {
            ++result.line;
            result.character = 0;
        } else {
            ++result.character;
        }
    }
    return result;
}

Range rangeAt(const std::string &source, const frontend::TextSpan &span) {
    return Range{positionAt(source, span.start),
                 positionAt(source, span.end)};
}

uint32_t offsetAt(const std::string &source, uint32_t line,
                  uint32_t character) {
    uint32_t offset = 0;
    uint32_t current_line = 0;
    while (current_line < line && offset < source.size()) {
        if (source[offset] == '\n') {
            ++current_line;
        }
        ++offset;
    }
    offset += character;
    if (offset > source.size()) {
        offset = static_cast<uint32_t>(source.size());
    }
    return offset;
}

std::optional<frontend::TextSpan> identifierAt(
    const frontend::FrontendSnapshot &frontend, uint32_t line,
    uint32_t character) {
    const auto &source = frontend.source();
    for (const auto &token : frontend.tokens()) {
        if (token.kind == frontend::TokenKind::End) {
            break;
        }
        if (token.kind != frontend::TokenKind::Identifier) {
            continue;
        }
        const auto start = positionAt(source, token.span.start);
        const auto end = positionAt(source, token.span.end);
        if (start.line == line && start.character <= character &&
            character < end.character) {
            return token.span;
        }
    }
    return std::nullopt;
}

struct IdeSemanticView {
    session::ModuleKey moduleKey;
    const session::CompilationSnapshot *snapshot = nullptr;
    const session::ModuleArtifact *module = nullptr;
    const frontend::FrontendSnapshot *frontend = nullptr;
    const session::ModuleResolution *resolution = nullptr;
    sema::modern::PerModuleSema *sema = nullptr;
    const sema::modern::TypedMap *typedMap = nullptr;

    [[nodiscard]] const session::CompilationSnapshot *
    resolveSnapshot() const noexcept {
        return snapshot;
    }

    [[nodiscard]] bool valid() const noexcept {
        return frontend != nullptr && sema != nullptr;
    }
};

IdeSemanticView semanticView(const session::CompilationSession &session) {
    IdeSemanticView result;
    const auto &snapshot = session.snapshot();
    if (!snapshot) {
        return result;
    }
    const auto file_id = session.fileId();
    for (const auto &module : snapshot->modules()) {
        if (!module || module->fileId != file_id || !module->frontend) {
            continue;
        }
        result.moduleKey = module->key;
        result.snapshot = snapshot.get();
        result.module = module.get();
        result.frontend = module->frontend.get();
        result.resolution = snapshot->findResolution(module->key);
        auto *pipeline = session.semaPipeline();
        if (pipeline != nullptr) {
            result.sema = pipeline->findModuleSema(module->key);
            result.typedMap = pipeline->findTypedMap(module->key);
        }
        break;
    }
    return result;
}

const frontend::Expression *expressionAtOffset(
    const frontend::FrontendSnapshot &frontend, uint32_t offset) {
    const frontend::Expression *best = nullptr;
    for (const auto &expr : frontend.expressions()) {
        if (expr.kind == frontend::ExprKind::Error ||
            expr.kind == frontend::ExprKind::MacroCall ||
            expr.kind == frontend::ExprKind::WhenGuard) {
            continue;
        }
        if (!(expr.span.start <= offset && offset <= expr.span.end)) {
            continue;
        }
        if (best == nullptr || expr.span.size() < best->span.size()) {
            best = &expr;
        }
    }
    return best;
}

const frontend::Statement *localBindingAtOffset(
    const frontend::FrontendSnapshot &frontend, uint32_t offset) {
    for (const auto &stmt : frontend.statements()) {
        if (stmt.kind == frontend::StmtKind::Binding &&
            stmt.binding.span.start <= offset &&
            offset <= stmt.binding.span.end) {
            return &stmt;
        }
    }
    return nullptr;
}

const session::ModuleArtifact *moduleFor(
    const session::CompilationSnapshot &snapshot, std::string_view key) {
    return snapshot.findModule(key);
}

bool sameSpan(const memory::Span &a, const memory::Span &b) noexcept {
    return a.file == b.file && a.start == b.start && a.end == b.end;
}

const session::ResolvedName *resolvedForExpr(
    const IdeSemanticView &view, const frontend::Expression &expr) {
    if (view.resolution == nullptr ||
        expr.kind != frontend::ExprKind::Name) {
        return nullptr;
    }
    return session::lookupExprResolution(*view.resolution, expr.id);
}

std::optional<memory::Span> definitionSpan(const IdeSemanticView &view,
                                           uint32_t offset) {
    if (!view.valid()) {
        return std::nullopt;
    }
    for (const auto &expr : view.frontend->expressions()) {
        if (expr.kind != frontend::ExprKind::Name ||
            !(expr.span.start <= offset && offset <= expr.span.end)) {
            continue;
        }
        const auto *resolved = resolvedForExpr(view, expr);
        if (resolved == nullptr) {
            continue;
        }
        if (resolved->declaration) {
            (void)0;
        }
        if (resolved->local) {
            for (const auto &stmt : view.frontend->statements()) {
                if (stmt.kind == frontend::StmtKind::Binding &&
                    stmt.binding.id == resolved->local) {
                    const auto *root_module =
                        view.snapshot != nullptr
                            ? moduleFor(*view.snapshot, view.moduleKey)
                            : nullptr;
                    const auto source_record =
                        view.snapshot != nullptr
                            ? view.snapshot->sourceCatalog().find(
                                  root_module != nullptr
                                      ? root_module->fileId
                                      : 0)
                            : nullptr;
                    return memory::Span{
                        source_record != nullptr && root_module != nullptr
                            ? root_module->fileId
                            : 0,
                        stmt.binding.span.start,
                        stmt.binding.span.end};
                }
            }
        }
        if (!resolved->declaration) {
            if (resolved->span.start != resolved->span.end) {
                const auto *root_module =
                    view.snapshot != nullptr
                        ? moduleFor(*view.snapshot, view.moduleKey)
                        : nullptr;
                const auto source_record =
                    view.snapshot != nullptr
                        ? view.snapshot->sourceCatalog().find(
                              root_module != nullptr ? root_module->fileId : 0)
                        : nullptr;
                return memory::Span{
                    source_record != nullptr && root_module != nullptr
                        ? root_module->fileId
                        : 0,
                    resolved->span.start, resolved->span.end};
            }
            continue;
        }
        const std::string_view target_module =
            resolved->target.module.empty() ? view.moduleKey
                                            : resolved->target.module;
        const auto *module = view.snapshot != nullptr
                                 ? moduleFor(*view.snapshot, target_module)
                                 : nullptr;
        if (module != nullptr && module->frontend != nullptr) {
            const auto &decls = module->frontend->declarations();
            if (resolved->declaration.value <= decls.size()) {
                const auto &decl = decls[resolved->declaration.value - 1U];
                return memory::Span{module->fileId, decl.span.start,
                                    decl.span.end};
            }
        }
        if (resolved->span.start != resolved->span.end) {
            const auto *root_module =
                view.snapshot != nullptr
                    ? moduleFor(*view.snapshot, view.moduleKey)
                    : nullptr;
            const auto source_record =
                view.snapshot != nullptr
                    ? view.snapshot->sourceCatalog().find(
                          root_module != nullptr ? root_module->fileId : 0)
                    : nullptr;
            return memory::Span{
                source_record != nullptr && root_module != nullptr
                    ? root_module->fileId
                    : 0,
                resolved->span.start, resolved->span.end};
        }
    }
    return std::nullopt;
}

std::string moduleUriFor(const IdeSemanticView &view,
                         const session::CompilationSnapshot *snapshot,
                         const memory::Span &span,
                         const std::string &fallback_uri) {
    if (snapshot != nullptr) {
        const auto source = snapshot->sourceCatalog().find(span.file);
        if (source && !source->canonicalPath.empty()) {
            return "file://" + source->canonicalPath;
        }
    }
    (void)view;
    return fallback_uri;
}

bool fieldVisible(size_t field_index,
                  const sema::modern::StructType &st,
                  const session::ModuleKey &module_key) {
    const auto &meta = field_index < st.field_meta.size()
                           ? st.field_meta[field_index]
                           : sema::modern::FieldMeta{
                                 frontend::Visibility::Private, 0,
                                 module_key};
    if (meta.visibility == frontend::Visibility::Public) {
        return true;
    }
    if (meta.visibility == frontend::Visibility::Private) {
        return meta.owner.empty() || module_key == meta.owner;
    }
    if (meta.owner.empty() || module_key == meta.owner) {
        return true;
    }
    if (meta.modDepth < 0) {
        return true;
    }

    const std::string_view owner_path = meta.owner;
    const auto owner_dir = owner_path.substr(0, owner_path.find_last_of('/'));
    if (!std::string_view(module_key).starts_with(owner_dir) ||
        owner_dir.empty()) {
        return false;
    }
    const auto relative =
        std::string_view(module_key).substr(owner_dir.size() + 1U);
    int32_t depth = 0;
    for (const char ch : relative) {
        if (ch == '/') {
            ++depth;
        }
    }
    return depth <= meta.modDepth;
}

std::string ownerBaseName(std::string_view owner_name) {
    if (const size_t angle = owner_name.find('<'); angle != std::string_view::npos) {
        owner_name = owner_name.substr(0, angle);
    }
    return std::string(owner_name);
}

sema::modern::TypeId receiverType(const IdeSemanticView &view,
                                  const frontend::Expression *base_expr) {
    if (base_expr == nullptr || view.sema == nullptr ||
        view.typedMap == nullptr) {
        return {};
    }
    auto type = view.sema->typeOfExpr(base_expr->id);
    if (!type) {
        if (const auto *mapped = view.typedMap->exprTypes.get(base_expr->id.value);
            mapped != nullptr) {
            type = *mapped;
        }
    }
    if (!type && view.resolution != nullptr && base_expr->id) {
        const auto *resolved =
            session::lookupExprResolution(*view.resolution, base_expr->id);
        if (resolved != nullptr && resolved->local) {
            type = view.sema->typeOfLocal(resolved->local);
        }
    }
    if (!type) {
        return {};
    }
    return view.sema->type_table.stripQualifiers(type);
}

std::vector<CompletionItem> memberCompletions(
    const IdeSemanticView &view, sema::modern::TypeId type) {
    std::vector<CompletionItem> result;
    if (!type || view.sema == nullptr) {
        return result;
    }
    const auto &table = view.sema->type_table;
    auto resolved = table.stripQualifiers(type);
    if (const auto *ptr = table.pointer(resolved); ptr != nullptr) {
        resolved = table.stripQualifiers(ptr->pointee);
    } else if (const auto *opt = table.optional(resolved); opt != nullptr) {
        resolved = table.stripQualifiers(opt->inner);
    }
    const std::string owner = ownerBaseName(table.typeToString(resolved));
    if (const auto *st = table.struct_type(resolved); st != nullptr) {
        for (size_t index = 0; index < st->field_names.size(); ++index) {
            if (!fieldVisible(index, *st, view.moduleKey)) {
                continue;
            }
            CompletionItem item;
            item.label = std::string(st->field_names[index]);
            item.kind = 6;
            item.detail = owner + "." + item.label + " (field)";
            item.insertText = item.label;
            item.dataKind = "field";
            item.dataName = item.label;
            item.dataOwner = owner;
            result.push_back(std::move(item));
        }
    } else if (const auto *et = table.enum_type(resolved); et != nullptr) {
        for (const auto variant : et->variant_names) {
            CompletionItem item;
            item.label = std::string(variant);
            item.kind = 13;
            item.detail = owner + "." + item.label + " (enum member)";
            item.insertText = item.label;
            item.dataKind = "enum-member";
            item.dataName = item.label;
            item.dataOwner = owner;
            result.push_back(std::move(item));
        }
        return result;
    }

    for (const auto &decl : view.frontend->declarations()) {
        if (decl.kind != frontend::DeclKind::Function ||
            decl.ownerName.empty() || ownerBaseName(decl.ownerName) != owner) {
            continue;
        }
        CompletionItem item;
        item.label = decl.name;
        item.kind = 3;
        item.detail = owner + "." + item.label + "()";
        item.dataKind = "method";
        item.dataName = decl.name;
        item.dataOwner = owner;
        std::ostringstream snippet;
        snippet << decl.name << "(";
        const size_t start =
            !decl.parameters.empty() && decl.parameters.front().name == "self"
                ? 1U
                : 0U;
        for (size_t index = start; index < decl.parameters.size(); ++index) {
            if (index > start) {
                snippet << ", ";
            }
            snippet << "${" << (index - start + 1U) << ":"
                    << decl.parameters[index].name << "}";
        }
        snippet << ")";
        item.insertText = snippet.str();
        item.insertTextFormat = 2;
        result.push_back(std::move(item));
    }
    return result;
}

struct MemberContext {
    bool active = false;
    std::string baseText;
    std::string partial;
    uint32_t baseOffset = 0;
};

MemberContext memberContextAt(const std::string &source, uint32_t line,
                              uint32_t character) {
    MemberContext result;
    const uint32_t cursor = offsetAt(source, line, character);
    size_t start = cursor;
    while (start > 0) {
        const char c = source[start - 1];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t' ||
            c == '(' || c == ',' || c == ';') {
            break;
        }
        --start;
    }
    const std::string_view before(source.data() + start, cursor - start);
    size_t op = before.size();
    while (op > 0 && before[op - 1] != '.' && before[op - 1] != '>') {
        --op;
    }
    if (op == 0) {
        return result;
    }
    if (before[op - 1] == '.') {
        result.active = true;
        result.baseText = std::string(before.substr(0, op - 1));
        result.partial = std::string(before.substr(op));
        result.baseOffset =
            static_cast<uint32_t>(start + op - 1U);
    } else {
        if (op < 2 || before[op - 2] != '-') {
            return result;
        }
        result.active = true;
        result.baseText = std::string(before.substr(0, op - 2));
        result.partial = std::string(before.substr(op));
        result.baseOffset = static_cast<uint32_t>(start + op - 2U);
    }
    return result;
}

void appendFunctionItem(std::vector<CompletionItem> &result,
                        const frontend::FrontendSnapshot &frontend,
                        const frontend::Declaration &decl) {
    CompletionItem item;
    item.label = decl.name;
    item.kind = 3;
    item.detail = "fn";
    item.insertText = decl.name;
    item.dataKind = "function";
    item.dataName = decl.name;
    std::ostringstream snippet;
    snippet << decl.name << "(";
    for (size_t index = 0; index < decl.parameters.size(); ++index) {
        if (index > 0) {
            snippet << ", ";
        }
        snippet << "${" << (index + 1U) << ":"
                << decl.parameters[index].name << "}";
    }
    snippet << ")";
    item.insertText = snippet.str();
    item.insertTextFormat = 2;
    item.detail = frontend::functionSignature(frontend, decl);
    result.push_back(std::move(item));
}

std::string functionMarkdown(const frontend::FrontendSnapshot &frontend,
                             const frontend::Declaration &decl) {
    std::ostringstream out;
    out << "```zith\n";
    if (decl.kind == frontend::DeclKind::Function) {
        switch (decl.functionKind) {
        case frontend::FunctionKind::Const:
            out << "const fn ";
            break;
        case frontend::FunctionKind::Raw:
            out << "raw fn ";
            break;
        case frontend::FunctionKind::Extern:
            out << "extern fn ";
            break;
        case frontend::FunctionKind::State:
            out << "state ";
            break;
        case frontend::FunctionKind::Standard:
            out << "fn ";
            break;
        }
    } else {
        out << "declaration ";
    }
    out << decl.name;
    if (!decl.genericParams.empty()) {
        out << "[";
        for (size_t index = 0; index < decl.genericParams.size(); ++index) {
            if (index > 0) {
                out << ", ";
            }
            out << decl.genericParams[index].name;
        }
        out << "]";
    }
    out << "(";
    for (size_t index = 0; index < decl.parameters.size(); ++index) {
        if (index > 0) {
            out << ", ";
        }
        out << decl.parameters[index].name;
        if (decl.parameters[index].type) {
            out << ": "
                << frontend::canonicalTypeString(
                       frontend, decl.parameters[index].type);
        }
    }
    out << ")";
    if (decl.declaredType) {
        out << ": "
            << frontend::canonicalTypeString(frontend, decl.declaredType);
    }
    out << "\n```";
    return out.str();
}

std::string completionDocumentation(
    const IdeSemanticView &view, const Query &query) {
    if (view.frontend == nullptr ||
        (query.dataKind.empty() && query.dataName.empty())) {
        return {};
    }
    const std::string owner = ownerBaseName(query.dataOwner);
    for (const auto &decl : view.frontend->declarations()) {
        if (!query.dataName.empty() && decl.name != query.dataName) {
            continue;
        }
        if (!owner.empty() && decl.ownerName != owner) {
            continue;
        }
        if (query.dataKind == "method") {
            return "method " + owner + "." + decl.name + "\n\n" +
                   functionMarkdown(*view.frontend, decl);
        }
        if (query.dataKind == "function" ||
            decl.kind == frontend::DeclKind::Function) {
            return "function " + decl.name + "\n\n" +
                   functionMarkdown(*view.frontend, decl);
        }
        return "declaration " + decl.name;
    }
    return {};
}

std::string hoverAt(const IdeSemanticView &view,
                    const std::string &source, uint32_t line,
                    uint32_t character) {
    if (!view.valid()) {
        return {};
    }
    const auto token = identifierAt(*view.frontend, line, character);
    if (!token) {
        return {};
    }
    const uint32_t offset =
        offsetAt(source, line, character);
    const std::string lexeme = source.substr(token->start, token->size());

    if (const auto *binding = localBindingAtOffset(*view.frontend, offset);
        binding != nullptr && !binding->binding.name.empty()) {
        const auto type = view.sema->typeOfLocal(binding->binding.id);
        std::ostringstream out;
        out << "```zith\nvariable " << binding->binding.name << "\n";
        if (type) {
            out << "\ntype: "
                << view.sema->type_table.typeToString(type) << "\n";
        }
        out << "```";
        return out.str();
    }

    const frontend::Declaration *fn_decl = nullptr;
    const frontend::FrontendSnapshot *fn_frontend = view.frontend;
    if (view.resolution != nullptr) {
        for (const auto &expr : view.frontend->expressions()) {
            if (expr.kind != frontend::ExprKind::Name ||
                expr.span.start != token->start ||
                expr.span.end != token->end) {
                continue;
            }
            const auto *resolved =
                session::lookupExprResolution(*view.resolution, expr.id);
            if (resolved == nullptr) {
                continue;
            }
            const std::string_view target_module =
                resolved->target.module.empty() ? view.moduleKey
                                                : resolved->target.module;
            const auto *snapshot = view.resolveSnapshot();
            const auto *module =
                snapshot != nullptr ? moduleFor(*snapshot, target_module)
                                    : nullptr;
            if (module != nullptr && module->frontend != nullptr &&
                resolved->declaration &&
                resolved->declaration.value <=
                    module->frontend->declarations().size()) {
                const auto &decl =
                    module->frontend
                        ->declarations()[resolved->declaration.value - 1U];
                if (decl.kind == frontend::DeclKind::Function) {
                    fn_decl = &decl;
                    fn_frontend = module->frontend.get();
                }
            }
            break;
        }
    }
    if (fn_decl == nullptr) {
        for (const auto &decl : view.frontend->declarations()) {
            if (decl.name == lexeme &&
                decl.kind == frontend::DeclKind::Function) {
                fn_decl = &decl;
                break;
            }
        }
    }
    if (fn_decl != nullptr) {
        return functionMarkdown(*fn_frontend, *fn_decl);
    }

    const auto *expr = expressionAtOffset(*view.frontend, offset);
    if (expr == nullptr) {
        return {};
    }
    if (const auto *resolved = resolvedForExpr(view, *expr);
        resolved != nullptr && resolved->local) {
        const auto type = view.sema->typeOfLocal(resolved->local);
        std::ostringstream out;
        out << "```zith\nvariable " << lexeme << "\n";
        if (type) {
            out << "\ntype: "
                << view.sema->type_table.typeToString(type) << "\n";
        }
        out << "```";
        return out.str();
    }
    const auto type = view.sema->typeOfExpr(expr->id);
    if (type) {
        std::ostringstream out;
        out << "```zith\nvariable " << lexeme << "\n\ntype: "
            << view.sema->type_table.typeToString(type) << "\n```";
        return out.str();
    }
    return {};
}

void walkExpr(const frontend::FrontendSnapshot &frontend,
              frontend::ExprId id,
              const std::function<void(const frontend::Expression &)> &visit) {
    if (!id || id.value > frontend.expressions().size()) {
        return;
    }
    const auto &expr = frontend.expressions()[id.value - 1U];
    visit(expr);
    for (const auto operand : expr.operands) {
        walkExpr(frontend, operand, visit);
    }
    for (const auto stmt_id : expr.statements) {
        if (stmt_id && stmt_id.value <= frontend.statements().size()) {
            const auto &stmt = frontend.statements()[stmt_id.value - 1U];
            if (stmt.expression) {
                walkExpr(frontend, stmt.expression, visit);
            }
            if (stmt.binding.initializer) {
                walkExpr(frontend, stmt.binding.initializer, visit);
            }
        }
    }
    if (expr.expansion) {
        walkExpr(frontend, expr.expansion, visit);
    }
}

std::vector<frontend::TextSpan> matchingSpans(
    const frontend::FrontendSnapshot &frontend,
    const frontend::TextSpan &source_span,
    const IdeSemanticView *view) {
    const std::string_view lexeme(frontend.source().data() + source_span.start,
                                  source_span.size());
    std::vector<frontend::TextSpan> result;
    memory::Span target_span{};
    if (view != nullptr && view->resolution != nullptr) {
        for (const auto &expr : frontend.expressions()) {
            if (expr.kind == frontend::ExprKind::Name &&
                expr.span == source_span) {
                    if (const auto *resolved =
                            session::lookupExprResolution(*view->resolution, expr.id);
                        resolved != nullptr) {
                        const auto *root_module =
                            view->snapshot != nullptr
                                ? moduleFor(*view->snapshot, view->moduleKey)
                                : nullptr;
                        target_span =
                            memory::Span{
                                root_module != nullptr ? root_module->fileId
                                                       : 0,
                                resolved->span.start, resolved->span.end};
                    }
                break;
            }
        }
    }

    std::function<void(const frontend::Expression &)> visit =
        [&](const frontend::Expression &expr) {
            if (expr.kind != frontend::ExprKind::Name) {
                return;
            }
            const auto name_sv =
                std::string_view(frontend.source().data() + expr.span.start,
                                 expr.span.size());
            if (target_span.end == 0) {
                if (name_sv == lexeme) {
                    result.push_back(expr.span);
                }
                return;
            }
            if (view != nullptr && view->resolution != nullptr) {
                const auto *resolved =
                    session::lookupExprResolution(*view->resolution, expr.id);
                const auto *root_module =
                    view->snapshot != nullptr
                        ? moduleFor(*view->snapshot, view->moduleKey)
                        : nullptr;
                const memory::Span candidate{
                    resolved != nullptr && root_module != nullptr
                        ? root_module->fileId
                        : 0,
                    resolved != nullptr ? resolved->span.start : 0,
                    resolved != nullptr ? resolved->span.end : 0};
                if (sameSpan(candidate, target_span)) {
                    result.push_back(expr.span);
                }
            }
        };
    for (const auto &decl : frontend.declarations()) {
        if (decl.initializer) {
            walkExpr(frontend, decl.initializer, visit);
        }
        if (decl.body) {
            walkExpr(frontend, decl.body, visit);
        }
    }
    for (const auto &stmt : frontend.statements()) {
        if (stmt.expression) {
            walkExpr(frontend, stmt.expression, visit);
        }
        if (stmt.binding.initializer) {
            walkExpr(frontend, stmt.binding.initializer, visit);
        }
    }
    if (target_span.end == 0) {
        for (const auto &stmt : frontend.statements()) {
            if (stmt.kind == frontend::StmtKind::Binding &&
                stmt.binding.name == lexeme) {
                result.push_back(stmt.binding.span);
            }
        }
        for (const auto &decl : frontend.declarations()) {
            if (decl.name == lexeme) {
                result.push_back(decl.span);
            }
        }
    }
    std::sort(result.begin(), result.end(),
              [](const auto &a, const auto &b) { return a.start < b.start; });
    result.erase(
        std::unique(result.begin(), result.end(),
                    [](const auto &a, const auto &b) {
                        return a.start == b.start && a.end == b.end;
                    }),
        result.end());
    return result;
}

std::vector<InlayHint> inlayHints(const IdeSemanticView &view,
                                  const std::string &source,
                                  const Range &requested_range) {
    std::vector<InlayHint> result;
    if (!view.valid() || view.typedMap == nullptr) {
        return result;
    }
    auto within = [&](const frontend::TextSpan &span) {
        const Range r = rangeAt(source, span);
        const bool before = r.start.line < requested_range.start.line ||
                            (r.start.line == requested_range.start.line &&
                             r.start.character <
                                 requested_range.start.character);
        const bool after = r.end.line > requested_range.end.line ||
                           (r.end.line == requested_range.end.line &&
                            r.end.character > requested_range.end.character);
        return !before && !after;
    };

    for (const auto &stmt : view.frontend->statements()) {
        if (stmt.kind != frontend::StmtKind::Binding ||
            stmt.binding.type || !stmt.binding.initializer ||
            stmt.binding.name.empty() || !within(stmt.binding.span)) {
            continue;
        }
        auto type = view.sema->typeOfLocal(stmt.binding.id);
        if (!type) {
            if (const auto *mapped =
                    view.typedMap->localTypes.get(stmt.binding.id.value);
                mapped != nullptr) {
                type = *mapped;
            }
        }
        if (!type ||
            type == view.sema->error_type ||
            type == view.sema->invalid_type) {
            continue;
        }
        const auto name_end = stmt.binding.span.start +
                              static_cast<uint32_t>(stmt.binding.name.size());
        result.push_back(
            InlayHint{positionAt(source, name_end),
                      ": " + view.sema->type_table.typeToString(type)});
    }

    for (const auto &expr : view.frontend->expressions()) {
        if (expr.kind != frontend::ExprKind::Call || expr.operands.empty() ||
            !expr.id) {
            continue;
        }
        const auto *callee =
            &view.frontend->expressions()[expr.operands[0].value - 1U];
        const auto *target = view.sema->resolvedCallTarget(callee->id);
        if (target == nullptr) {
            continue;
        }
        const std::string_view module_key = target->module.empty()
                                                ? view.moduleKey
                                                : target->module;
        const auto *snapshot = view.resolveSnapshot();
        const auto *module =
            snapshot != nullptr ? moduleFor(*snapshot, module_key) : nullptr;
        if (module == nullptr || !module->frontend ||
            !target->decl ||
            target->decl.value > module->frontend->declarations().size() ||
            !within(expr.span)) {
            continue;
        }
        const auto &decl =
            module->frontend->declarations()[target->decl.value - 1U];
        const bool has_receiver =
            !decl.parameters.empty() && decl.parameters.front().name == "self";
        for (size_t index = 1; index < expr.operands.size(); ++index) {
            const auto &arg =
                view.frontend->expressions()[expr.operands[index].value - 1U];
            const size_t param_index = has_receiver ? index : index - 1U;
            if (param_index >= decl.parameters.size()) {
                continue;
            }
            if (decl.parameters[param_index].name.empty()) {
                continue;
            }
            result.push_back(
                InlayHint{positionAt(source, arg.span.start),
                          decl.parameters[param_index].name + ":"});
        }
    }
    return result;
}

std::vector<FoldingRange> foldingRangesFor(const std::string &content) {
    std::vector<FoldingRange> result;
    std::vector<uint32_t> stack;
    uint32_t line = 0;
    bool in_string = false;
    bool in_line_comment = false;
    for (size_t i = 0; i < content.size(); ++i) {
        const char c = content[i];
        if (c == '\n') {
            ++line;
            in_string = false;
            in_line_comment = false;
            continue;
        }
        if (in_line_comment) {
            continue;
        }
        if (c == '"') {
            in_string = !in_string;
            continue;
        }
        if (in_string) {
            continue;
        }
        if (c == '/' && i + 1 < content.size() && content[i + 1] == '/') {
            in_line_comment = true;
            continue;
        }
        if (c == '{') {
            stack.push_back(line);
        } else if (c == '}' && !stack.empty()) {
            const uint32_t open_line = stack.back();
            stack.pop_back();
            if (line > open_line) {
                FoldingRange range;
                range.startLine = open_line;
                range.endLine = line;
                result.push_back(range);
            }
        }
    }
    return result;
}

int semanticTokenType(const session::CompilationSession &session,
                      const frontend::Token &token,
                      std::string_view text) {
    using K = frontend::TokenKind;
    if (token.kind == K::Identifier) {
        const auto sym_id = session.symbolTable().lookup(text);
        if (sym_id == symbols::kInvalidSym) {
            return 3;
        }
        switch (session.symbolTable().get(sym_id).kind) {
        case symbols::SymKind::Fn:
            return 0;
        case symbols::SymKind::Struct:
        case symbols::SymKind::Enum:
        case symbols::SymKind::Union:
        case symbols::SymKind::Asset:
        case symbols::SymKind::Word:
        case symbols::SymKind::Context:
            return 1;
        case symbols::SymKind::Trait:
        case symbols::SymKind::Interface:
        case symbols::SymKind::Alias:
            return 2;
        default:
            return 3;
        }
    }
    if (token.kind == K::Keyword) {
        if (text == "fn" || text == "macro" || text == "const fn" ||
            text == "raw fn" || text == "extern fn" || text == "state") {
            return 0;
        }
        if (text == "struct" || text == "enum" || text == "union") {
            return 1;
        }
        if (text == "trait" || text == "interface" || text == "type" ||
            text == "alias") {
            return 2;
        }
        if (text == "mut" || text == "const" || text == "extern" ||
            text == "pub" || text == "lend" || text == "view" ||
            text == "share" || text == "unique" || text == "belong" ||
            text == "raw") {
            return 8;
        }
        return 7;
    }
    if (token.kind == K::Literal) {
        return 5;
    }
    return token.kind == K::Operator || token.kind == K::Punctuation ||
                   token.kind == K::Dots
               ? 5
               : 3;
}

std::vector<SemanticToken> collectSymbolAwareTokens(
    const session::CompilationSession &session) {
    std::vector<SemanticToken> result;
    const auto &snapshot = session.snapshot();
    if (!snapshot) {
        return result;
    }
    const auto file_id = session.fileId();
    for (const auto &module : snapshot->modules()) {
        if (!module || module->fileId != file_id || !module->frontend) {
            continue;
        }
        const auto &frontend = *module->frontend;
        uint32_t last_line = 0;
        uint32_t last_start = 0;
        for (const auto &token : frontend.tokens()) {
            if (token.kind == frontend::TokenKind::End) {
                break;
            }
            if (token.kind == frontend::TokenKind::Unknown) {
                continue;
            }
            const auto range = rangeAt(frontend.source(), token.span);
            SemanticToken semantic;
            semantic.deltaLine = range.start.line - last_line;
            if (semantic.deltaLine == 0) {
                semantic.deltaStart = range.start.character - last_start;
            } else {
                semantic.deltaStart = range.start.character;
            }
            semantic.length = token.span.size();
            const std::string_view text(
                frontend.source().data() + token.span.start, token.span.size());
            semantic.tokenType = static_cast<uint32_t>(
                semanticTokenType(session, token, text));
            semantic.tokenModifiers = 0;
            result.push_back(semantic);
            last_line = range.start.line;
            last_start = range.start.character;
        }
        break;
    }
    return result;
}

std::string sourceFromSnapshot(
    const std::shared_ptr<const session::CompilationSnapshot> &snapshot,
    session::CompilationSession &session,
    const std::string &fallback) {
    if (!snapshot) {
        return fallback;
    }
    const auto file_id = session.fileId();
    for (const auto &module : snapshot->modules()) {
        if (module && module->fileId == file_id && module->frontend) {
            return module->frontend->source();
        }
    }
    return fallback;
}

} // namespace

struct Workspace::Impl {
    explicit Impl(WorkspaceConfig config)
        : config_(std::move(config)), arena_() {
        if (!config_.compilerVersion.empty()) {
            frontend_config_.compilerVersion = config_.compilerVersion;
        }
        frontend_config_.workspaceRoot = config_.workspaceRoot;
        frontend_config_.maxFrontendWorkers =
            config_.maxWorkers == 0 ? 1 : config_.maxWorkers;
        if (!config_.stdlibPath.empty()) {
            frontend_config_.stdlibRoots.push_back(config_.stdlibPath);
        }
        frontend_context_ =
            std::make_shared<session::FrontendContext>(frontend_config_);
    }

    Options makeOptions() {
        Options opts(arena_);
        opts.targetStage = session::Stage::TypeChecked;
        opts.noCache = true;
        if (!config_.stdlibPath.empty()) {
            opts.includeDirs.push(config_.stdlibPath);
        }
        return opts;
    }

    WorkspaceConfig config_;
    session::FrontendConfig frontend_config_;
    std::shared_ptr<session::FrontendContext> frontend_context_;
    memory::Arena arena_;
    std::unordered_map<std::string, DocumentSnapshot> documents_;
};

Workspace::Workspace(WorkspaceConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

Workspace::~Workspace() = default;

DocumentSnapshot Workspace::openDocument(std::string uri, std::string filePath,
                                         std::string text, int64_t version) {
    DocumentSnapshot snapshot;
    snapshot.uri = std::move(uri);
    snapshot.filePath = std::move(filePath);
    snapshot.text = std::move(text);
    snapshot.version = version;
    snapshot.revision = 1;
    impl_->documents_[snapshot.uri] = snapshot;
    impl_->frontend_context_->setOverlay(snapshot.filePath, snapshot.text);
    return snapshot;
}

DocumentSnapshot Workspace::changeDocument(const std::string &uri,
                                           const std::string &text,
                                           int64_t version) {
    auto existing = impl_->documents_.find(uri);
    if (existing == impl_->documents_.end()) {
        return {};
    }
    existing->second.text = text;
    existing->second.version = version;
    ++existing->second.revision;
    impl_->frontend_context_->setOverlay(existing->second.filePath,
                                         existing->second.text);
    return existing->second;
}

void Workspace::closeDocument(const std::string &uri) noexcept {
    const auto existing = impl_->documents_.find(uri);
    if (existing == impl_->documents_.end()) {
        return;
    }
    impl_->frontend_context_->removeOverlay(existing->second.filePath);
    impl_->documents_.erase(existing);
}

std::optional<DocumentSnapshot> Workspace::snapshotFor(const std::string &uri) const {
    const auto it = impl_->documents_.find(uri);
    if (it == impl_->documents_.end()) {
        return std::nullopt;
    }
    return it->second;
}

AnalysisResult Workspace::analyze(const DocumentSnapshot &snapshot,
                                  const Query &query) {
    AnalysisResult result;
    result.kind = query.kind;
    result.analyzedRevision = snapshot.revision;

    Options opts = impl_->makeOptions();
    auto session = std::make_unique<session::CompilationSession>(
        opts, snapshot.filePath, impl_->frontend_context_);
    session->setContent(snapshot.text);
    session->setBuffered(true);
    const bool success =
        session->runTo(session::Stage::TypeChecked);
    result.ok = success || !session->hasErrors();

    const auto &compiler_snapshot = session->snapshot();
    const std::string source =
        sourceFromSnapshot(compiler_snapshot, *session, snapshot.text);
    const IdeSemanticView view = semanticView(*session);

    switch (query.kind) {
    case QueryKind::Diagnostics:
        result.diagnostics = collectDiagnostics(*session, source);
        break;
    case QueryKind::Completion:
        {
            const auto member = memberContextAt(source, query.line,
                                                query.character);
            if (member.active) {
                const frontend::Expression *base = nullptr;
                if (view.frontend != nullptr) {
                    for (const auto &expr : view.frontend->expressions()) {
                        const auto text =
                            std::string_view(source.data() + expr.span.start,
                                             expr.span.size());
                        if (expr.span.end == member.baseOffset &&
                            text == member.baseText) {
                            base = &expr;
                            break;
                        }
                    }
                    if (base == nullptr) {
                        const auto *best = expressionAtOffset(
                            *view.frontend, member.baseOffset);
                        if (best != nullptr &&
                            best->span.end == member.baseOffset) {
                            base = best;
                        }
                    }
                }
                auto items = memberCompletions(
                    view, receiverType(view, base));
                for (auto &item : items) {
                    if (!member.partial.empty() &&
                        !(item.label.rfind(member.partial, 0) == 0)) {
                        continue;
                    }
                    result.completions.push_back(std::move(item));
                }
            } else {
                result.completions = collectCompletionItems(*session);
                if (view.frontend != nullptr) {
                    for (const auto &decl : view.frontend->declarations()) {
                        if (decl.kind == frontend::DeclKind::Function &&
                            decl.ownerName.empty()) {
                            appendFunctionItem(result.completions,
                                               *view.frontend, decl);
                        }
                    }
                }
            }
        }
        break;
    case QueryKind::CompletionResolve:
        {
            const std::string docs = completionDocumentation(view, query);
            if (!docs.empty()) {
                CompletionItem item;
                item.label = query.dataName.empty() ? query.text
                                                     : query.dataName;
                item.documentation = docs;
                result.completions.push_back(std::move(item));
            }
        }
        break;
    case QueryKind::DocumentSymbols:
        result.documentSymbols = collectDocumentSymbols(*session);
        break;
    case QueryKind::SemanticTokensFull:
    case QueryKind::SemanticTokensDelta:
        result.semanticTokens = collectSymbolAwareTokens(*session);
        break;
    case QueryKind::Hover:
        {
            const std::string text = hoverAt(view, source, query.line,
                                             query.character);
            if (!text.empty()) {
                result.hover = HoverResult{text};
            }
        }
        break;
    case QueryKind::Highlights:
        {
            const auto token =
                view.frontend != nullptr
                    ? identifierAt(*view.frontend, query.line,
                                   query.character)
                    : std::optional<frontend::TextSpan>{};
            if (token && view.frontend != nullptr) {
                for (const auto &span :
                     matchingSpans(*view.frontend, *token, &view)) {
                    Highlight highlight;
                    highlight.range = rangeAt(source, span);
                    highlight.kind = 1;
                    result.highlights.push_back(highlight);
                }
            }
        }
        break;
    case QueryKind::Definition:
        {
            const uint32_t offset = offsetAt(source, query.line,
                                              query.character);
            if (const auto span = definitionSpan(view, offset);
                span.has_value()) {
                Location location;
                location.uri = moduleUriFor(
                    view, compiler_snapshot.get(), *span, snapshot.uri);
                const std::string &span_source =
                    view.frontend != nullptr ? view.frontend->source()
                                             : source;
                location.range = rangeAt(
                    span_source,
                    frontend::TextSpan{span->start, span->end});
                result.definitions.push_back(std::move(location));
            }
        }
        break;
    case QueryKind::References:
        {
            const auto token =
                view.frontend != nullptr
                    ? identifierAt(*view.frontend, query.line,
                                   query.character)
                    : std::optional<frontend::TextSpan>{};
            if (token && view.frontend != nullptr) {
                for (const auto &span :
                     matchingSpans(*view.frontend, *token, &view)) {
                    Location location;
                    location.uri = snapshot.uri;
                    location.range = rangeAt(source, span);
                    result.references.push_back(std::move(location));
                }
            }
        }
        break;
    case QueryKind::PrepareRename:
        {
            const auto token =
                view.frontend != nullptr
                    ? identifierAt(*view.frontend, query.line,
                                   query.character)
                    : std::optional<frontend::TextSpan>{};
            if (token) {
                result.prepareRename = rangeAt(source, *token);
            }
        }
        break;
    case QueryKind::Rename:
        {
            const auto token =
                view.frontend != nullptr
                    ? identifierAt(*view.frontend, query.line,
                                   query.character)
                    : std::optional<frontend::TextSpan>{};
            if (token && !query.text.empty() && view.frontend != nullptr) {
                std::vector<TextEdit> changes;
                for (const auto &span :
                     matchingSpans(*view.frontend, *token, &view)) {
                    TextEdit edit;
                    edit.range = rangeAt(source, span);
                    edit.newText = query.text;
                    changes.push_back(std::move(edit));
                }
                if (!changes.empty()) {
                    result.renameChanges[snapshot.uri] = std::move(changes);
                }
            }
        }
        break;
    case QueryKind::SignatureHelp:
        {
            if (view.frontend != nullptr) {
                result.signatureHelp =
                    collectSignatureHelp(*session, *view.frontend, source,
                                         query.line, query.character);
                if (result.signatureHelp->signatures.empty()) {
                    result.signatureHelp.reset();
                }
            }
        }
        break;
    case QueryKind::InlayHints:
        {
            Range range = query.range;
            if (range.end.line == 0 && range.end.character == 0) {
                range.end =
                    positionAt(source, static_cast<uint32_t>(source.size()));
            }
            result.inlayHints = inlayHints(view, source, range);
        }
        break;
    case QueryKind::Formatting:
        {
            if (result.ok) {
                const std::string formatted = session->fmtStage();
                if (!formatted.empty() && formatted != source) {
                    TextEdit edit;
                    edit.range.start = Position{0, 0};
                    edit.range.end =
                        positionAt(source,
                                   static_cast<uint32_t>(source.size()));
                    edit.newText = formatted;
                    result.formatting.push_back(std::move(edit));
                }
            }
        }
        break;
    case QueryKind::Folding:
        result.folding = foldingRangesFor(source);
        break;
    default:
        result.ok = true;
        break;
    }
    return result;
}

FeatureStability Workspace::stability(QueryKind kind) const noexcept {
    switch (kind) {
    case QueryKind::SemanticTokensDelta:
    case QueryKind::Commands:
        return FeatureStability::Experimental;
    case QueryKind::Configuration:
        return FeatureStability::Planned;
    default:
        return FeatureStability::Stable;
    }
}

} // namespace zith::ide
