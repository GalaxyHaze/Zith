#pragma once

#include "common/memory/result.hpp"
#include "common/memory/span.hpp"
#include "frontend/ast/ast.hpp"

#include <cstdint>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

using Span = common::memory::Span;

namespace sample {

enum class DeclKind : int {
    Error = 0,
    Import = 1,
    Function = 2,
    Variable = 3,
    Struct = 4,
    Enum = 5,
    Union = 6,
    TypeAlias = 7,
    Trait = 8,
    Interface = 9,
    State = 10,
    Macro = 11,
    Word = 12,
    Context = 13,
};

enum class VisibilityKind : int { Private = 0, Public = 1, Module = 2 };

enum class FunctionKind : int {
    Standard = 0,
    Extern = 1,
    Const = 2,
    Raw = 3,
    Flow = 4,
};

enum class TypeExprKind : int {
    Error = 0,
    Name = 1,
    Pointer = 2,
    Optional = 3,
    Slice = 4,
    Array = 5,
    Function = 6,
    Opaque = 7,
};

enum class ExprKind : int {
    Error = 0,
    Literal = 1,
    Name = 2,
    Unary = 3,
    Binary = 4,
    Assign = 5,
    Call = 6,
    Index = 7,
    Field = 8,
    Arrow = 9,
    OptionalProp = 10,
    Cast = 11,
    StructLiteral = 12,
    ArrayLiteral = 13,
    Block = 14,
    If = 15,
    For = 16,
    When = 17,
    Range = 18,
    Placeholder = 19,
    MacroCall = 20,
    LayoutIntrinsic = 21,
    IsNull = 22,
};

enum class StmtKind : int {
    Expression = 0,
    Binding = 1,
    Return = 2,
    Break = 3,
    Continue = 4,
    Enter = 5,
    Leave = 6,
    Jump = 7,
};

struct ParserDiagnostic : common::memory::Error {
    Span span = Span(0, 0);
    std::string message;
};

struct ImportSelector {
    std::string_view name;
    std::string_view alias;
    Span span = Span(0, 0);
    Span aliasSpan = Span(0, 0);
};

struct ImportDecl {
    std::vector<std::string_view> path;
    std::vector<Span> pathSpans;
    std::vector<ImportSelector> selectors;
    std::string_view rawPath;
    std::string_view headerPath;
    std::string_view alias;
    bool isFrom = false;
    bool isExport = false;
    bool isAsset = false;
    bool isHeader = false;
    int32_t depth = 1;
    Span pathSpan = Span(0, 0);
    Span aliasSpan = Span(0, 0);
    Span span = Span(0, 0);
};

struct ParseOutput {
    common::memory::Arena arena;
    generated_ast::AstRoot ast;
    std::string_view source;
    std::vector<ImportDecl> imports;
    std::vector<ParserDiagnostic> diagnostics;

    ParseOutput() : ast(arena) {}
    explicit ParseOutput(std::string_view sourceValue)
        : ast(arena), source(sourceValue) {}
    ParseOutput(ParseOutput &&other)
        : arena(std::move(other.arena)),
          ast(std::move(other.ast)),
          source(other.source),
          imports(std::move(other.imports)),
          diagnostics(std::move(other.diagnostics)) {
        // AstRoot keeps a pointer to the arena it was constructed with. When
        // ParseOutput itself moves, that pointer must track the new location
        // of the arena member or destruction dereferences a moved-from object.
        ast.arena = &arena;
    }
    ParseOutput &operator=(ParseOutput &&other) {
        if (this != &other) {
            arena = std::move(other.arena);
            ast = std::move(other.ast);
            ast.arena = &arena;
            source = other.source;
            imports = std::move(other.imports);
            diagnostics = std::move(other.diagnostics);
        }
        return *this;
    }
    ParseOutput(const ParseOutput &) = delete;
    ParseOutput &operator=(const ParseOutput &) = delete;
};

struct ImportBuilder {
    std::vector<std::string_view> path;
    std::vector<Span> pathSpans;
    std::vector<ImportSelector> selectors;
    std::string alias;
    Span aliasSpan = Span(0, 0);
    bool isFrom = false;
    bool isExport = false;
    bool isAsset = false;
    bool isHeader = false;
    bool expectingAlias = false;
    int32_t depth = 1;
    Span pathSpan = Span(0, 0);
    Span span = Span(0, 0);
};

} // namespace sample
