#pragma once

#include "common/ast-ids.hpp"
#include "memory/arena.hpp"
#include "memory/dyn-array.hpp"
#include "memory/flat-map.hpp"
#include "memory/span.hpp"
#include "memory/string-interner.hpp"
#include "symbols/symbol-id.hpp"
#include "symbols/symbol-visibility.hpp"

#include <cstdio>

namespace zith::ast {
class AstBuilder;
}

namespace zith::symbols {

enum class SymKind : uint8_t {
    Fn,
    Struct,
    Trait,
    Interface,
    Enum,
    Alias,
    Variable,
    Module,
    Component,
    Union,
    Asset,
    Word,
    Context,
};

struct SymbolData {
    memory::InternedId name;
    ScopeId scope;
    SymbolVisibility visibility = SymbolVisibility::Private;
    int32_t mod_depth           = 0;
    SymKind kind                = SymKind::Variable;
    ast::DeclId decl_id         = ast::kInvalidDecl;
    memory::Span span{};
    memory::Span doc_span{};
    SymId target = kInvalidSym;
    memory::DynArray<SymId> members;

    SymbolData(memory::InternedId name_, ScopeId scope_, SymbolVisibility vis, int32_t depth,
               SymKind ikind, ast::DeclId did, memory::Span ispan, memory::Span idoc, SymId itarget,
               memory::Arena &arena)
        : name(name_), scope(scope_), visibility(vis), mod_depth(depth), kind(ikind), decl_id(did),
          span(ispan), doc_span(idoc), target(itarget), members(arena) {}
};

struct Scope {
    ScopeId parent = kInvalidScope;
    /// Ordered list preserves declaration order for dumps and deterministic diagnostics.
    memory::DynArray<SymId> syms;
    /// O(1) lookup by interned name within this scope only.
    memory::FlatMap<memory::InternedId, SymId> index;
};

class SymbolTable {
    memory::Arena *arena_;
    memory::StringInterner *interner_;
    memory::DynArray<Scope> scopes_;
    memory::DynArray<SymbolData> symbols_;
    ScopeId current_ = kRootScope;

public:
    SymbolTable(memory::Arena &arena, memory::StringInterner *interner);

    ScopeId enterScope();
    void exitScope();
    ScopeId currentScope() const noexcept;

    SymId declare(memory::InternedId name, SymbolVisibility vis = SymbolVisibility::Private,
                  int32_t depth = 0, SymKind kind = SymKind::Variable,
                  ast::DeclId decl_id = ast::kInvalidDecl, memory::Span span = {},
                  SymId target = kInvalidSym, memory::Span doc_span = {});

    /// Copy all non-root symbols from `other` into the current scope of `this`.
    void emplace(const SymbolTable &other);
    SymId declare(std::string_view name, SymbolVisibility vis = SymbolVisibility::Private,
                  int32_t depth = 0, SymKind kind = SymKind::Variable,
                  ast::DeclId decl_id = ast::kInvalidDecl, memory::Span span = {},
                  SymId target = kInvalidSym, memory::Span doc_span = {});
    SymId declareInScope(ScopeId scope, memory::InternedId name,
                         SymbolVisibility vis = SymbolVisibility::Private, int32_t depth = 0,
                         SymKind kind = SymKind::Variable, ast::DeclId decl_id = ast::kInvalidDecl,
                         memory::Span span = {}, SymId target = kInvalidSym,
                         memory::Span doc_span = {});
    SymId declareInScope(ScopeId scope, std::string_view name,
                         SymbolVisibility vis = SymbolVisibility::Private, int32_t depth = 0,
                         SymKind kind = SymKind::Variable, ast::DeclId decl_id = ast::kInvalidDecl,
                         memory::Span span = {}, SymId target = kInvalidSym,
                         memory::Span doc_span = {});
    SymId lookup(memory::InternedId name) const;
    SymId lookup(std::string_view name) const;
    /// Look up `name` in the parent of the current scope (skip current level).
    SymId lookupEscaped(std::string_view name) const;
    SymId lookupEscaped(memory::InternedId name) const;
    SymId lookupInScope(memory::InternedId name, ScopeId scope) const;
    SymId lookupInScope(std::string_view name, ScopeId scope) const;
    memory::DynArray<SymId> lookupAll(memory::InternedId name, memory::Arena &arena) const;
    memory::DynArray<SymId> lookupAll(std::string_view name, memory::Arena &arena) const;

    // Copy local (non-root) bindings into an existing scope.  Imported files keep
    // their globals at the root; this is intended for bindings belonging to a
    // body being lowered in a temporary scope.
    void emplace(const SymbolTable &other, ScopeId targetScope);

    SymbolData &get(SymId id);
    const SymbolData &get(SymId id) const;
    size_t symbolCount() const noexcept;
    ScopeId scopeCount() const noexcept;

    memory::StringInterner &interner() const {
        return *interner_;
    }

    void dump(FILE *out = stdout, ast::AstBuilder *bld = nullptr) const;
};

} // namespace zith::symbols
