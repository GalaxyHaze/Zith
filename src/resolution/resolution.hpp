#pragma once

#include "common/import/import-graph.hpp"
#include "common/diagnostic/diagnostic.hpp"
#include "common/memory/arena.hpp"
#include "common/memory/dyn-array.hpp"
#include "common/memory/flat-map.hpp"
#include "common/memory/source-map.hpp"
#include "common/memory/string-interner.hpp"
#include "frontend/ast/ast.hpp"
#include "frontend/parser/types.hpp"
#include "symbols/symbols.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace toolkit::resolution {

struct ScanRecord {
    const generated_ast::Declaration *declaration = nullptr;
    std::string_view name{};
    common::memory::FileId file = 0;
    common::memory::Span span{};
    toolkit::symbols::SymKind kind = toolkit::symbols::SymKind::Fn;
    toolkit::symbols::SymbolVisibility visibility{};

    ScanRecord() = default;

    ScanRecord(const generated_ast::Declaration *declaration_,
               std::string_view name_,
               common::memory::FileId file_,
               common::memory::Span span_,
               toolkit::symbols::SymKind kind_,
               toolkit::symbols::SymbolVisibility visibility_)
        : declaration(declaration_), name(name_), file(file_), span(span_),
          kind(kind_), visibility(visibility_) {}
};

struct ScanInfo {
    common::memory::DynArray<ScanRecord> records;

    explicit ScanInfo(common::memory::Arena &arena) : records(arena) {}

    [[nodiscard]] const ScanRecord *lookup(std::string_view name) const;
};

struct DeferredImport {
    std::string_view moduleName{};
    std::string_view path{};
    std::string_view alias{};
    std::vector<sample::ImportSelector> selectors;
    bool isExport = false;
    bool isAsset = false;
    bool isHeader = false;
    common::memory::Span span{};
};

struct ImportInfo {
    common::memory::Arena &arena;
    common::memory::StringInterner &interner;
    toolkit::import::ImportGraph graph;
    common::memory::DynArray<sample::ParseOutput> outputs;
    common::memory::DynArray<std::vector<sample::ImportDecl>> outputImports;
    common::memory::DynArray<common::memory::FileId> outputFiles;
    common::memory::DynArray<std::string_view> moduleNames;
    common::memory::DynArray<DeferredImport> deferredImports;
    common::memory::FlatMap<std::string, std::size_t> outputByModule;
    std::string_view rootModuleName{};
    common::memory::DynArray<common::diagnostic::Diagnostic> diagnostics;

    ImportInfo(common::memory::Arena &arena_, common::memory::StringInterner &interner_)
        : arena(arena_), interner(interner_), graph(arena_, interner_),
          outputs(arena_), outputImports(arena_), outputFiles(arena_), moduleNames(arena_),
          deferredImports(arena_), diagnostics(arena_) {}

    [[nodiscard]] const generated_ast::Program *program(
        std::string_view module) const;
    [[nodiscard]] const toolkit::import::Module *module(
        std::string_view name) const;

    [[nodiscard]] common::memory::DynArray<common::diagnostic::Diagnostic> &
    diagnosticSink() noexcept {
        return diagnostics;
    }
};

struct ResolvedSymbol {
    std::string_view name{};
    std::string_view moduleName{};
    std::string_view ownerModuleName{};
    const generated_ast::Declaration *declaration = nullptr;
    toolkit::symbols::SymKind kind = toolkit::symbols::SymKind::Fn;
    toolkit::symbols::SymbolVisibility visibility{};
    common::memory::FileId file = 0;
    common::memory::Span span{};
};

struct ResolvedInfo {
    common::memory::DynArray<ResolvedSymbol> symbols;

    explicit ResolvedInfo(common::memory::Arena &arena) : symbols(arena) {}

    [[nodiscard]] const ResolvedSymbol *lookup(
        std::string_view module, std::string_view name) const;
};

[[nodiscard]] common::memory::Result<void> scanProgram(
    const generated_ast::Program &program,
    common::memory::FileId file,
    ScanInfo &out);

[[nodiscard]] common::memory::Result<void> importProgram(
    const sample::ParseOutput &root,
    common::memory::FileId rootFile,
    std::string_view projectRoot,
    common::memory::SourceMap &sourceMap,
    ImportInfo &out);

[[nodiscard]] common::memory::Result<void> resolveModules(
    const ImportInfo &imports,
    ResolvedInfo &out);

} // namespace toolkit::resolution
