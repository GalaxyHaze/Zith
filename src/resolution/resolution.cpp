#include "resolution/resolution.hpp"

#include "frontend/lexer/lexer.hpp"
#include "frontend/parser/parse.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace toolkit::resolution {
namespace {

using common::memory::DynArray;
using common::memory::Error;
using common::memory::FileId;
using common::memory::Result;
using common::memory::Span;
using generated_ast::AstNode;
using generated_ast::Declaration;
using generated_ast::Program;
using sample::DeclKind;
using toolkit::symbols::SymKind;
using toolkit::symbols::SymbolVisibility;
using toolkit::symbols::SymbolVisibilityKind;

[[nodiscard]] SymKind symKindFor(DeclKind kind) {
    switch (kind) {
    case DeclKind::Function:
    case DeclKind::State:
    case DeclKind::Macro:
        return SymKind::Fn;
    case DeclKind::Variable:
        return SymKind::Variable;
    case DeclKind::Struct:
        return SymKind::Struct;
    case DeclKind::Enum:
        return SymKind::Enum;
    case DeclKind::Union:
        return SymKind::Union;
    case DeclKind::TypeAlias:
        return SymKind::Alias;
    case DeclKind::Trait:
        return SymKind::Trait;
    case DeclKind::Interface:
        return SymKind::Interface;
    case DeclKind::Word:
        return SymKind::Word;
    case DeclKind::Context:
        return SymKind::Context;
    default:
        return SymKind::Fn;
    }
}

[[nodiscard]] SymbolVisibility visibilityFrom(const Declaration *node) {
    SymbolVisibility visibility;
    switch (static_cast<sample::VisibilityKind>(node->visibility)) {
    case sample::VisibilityKind::Public:
        visibility.kind = SymbolVisibilityKind::Public;
        break;
    case sample::VisibilityKind::Private:
        visibility.kind = SymbolVisibilityKind::Private;
        break;
    case sample::VisibilityKind::Module:
        visibility.kind = SymbolVisibilityKind::Module;
        break;
    }
    if (node->visibility == static_cast<int>(sample::VisibilityKind::Module)) {
        visibility.range.ancestors = node->visibilityAncestors;
        visibility.range.descendants = node->visibilityDescendants;
    }
    return visibility;
}

[[nodiscard]] bool sameParentSegment(
    std::string_view left, std::string_view right) {
    const std::size_t leftSlash = left.rfind('/');
    const std::size_t rightSlash = right.rfind('/');
    if (leftSlash == std::string_view::npos ||
        rightSlash == std::string_view::npos)
        return false;
    return left.substr(0, leftSlash) == right.substr(0, rightSlash);
}

[[nodiscard]] bool visibilityAllows(
    const toolkit::import::ImportGraph &graph,
    const toolkit::import::Module &viewer,
    const toolkit::import::Module &owner,
    std::string_view viewerName,
    std::string_view ownerName,
    const SymbolVisibility &visibility) {
    if (visibility.kind == SymbolVisibilityKind::Public)
        return true;
    if (visibility.kind == SymbolVisibilityKind::Private)
        return viewerName == ownerName;
    if (viewerName == ownerName)
        return true;

    const int32_t ancestorBound = visibility.range.ancestors;
    const int32_t descendantBound = visibility.range.descendants;

    if (ancestorBound >= 0 && graph.isAncestor(viewer, owner)) {
        const int32_t distance = graph.distance(viewer, owner);
        if (distance >= 0 && distance <= ancestorBound)
            return true;
    }

    if (descendantBound >= 0 && graph.isAncestor(owner, viewer)) {
        const int32_t distance = graph.distance(owner, viewer);
        if (distance >= 0 && distance <= descendantBound)
            return true;
    }

    if (ancestorBound == 0 &&
        sameParentSegment(viewerName, ownerName))
        return true;

    return false;
}

[[nodiscard]] std::string normalizeImportPath(std::string_view raw) {
    std::string path(raw);
    if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
        path = path.substr(1, path.size() - 2);
    if (path.size() >= 5 && path.substr(path.size() - 5) == ".zith")
        path.erase(path.size() - 5);
    for (char &ch : path)
        if (ch == '\\')
            ch = '/';
    while (path.size() >= 2 && path.substr(0, 2) == "./")
        path.erase(0, 2);
    while (path.size() >= 3 && path.substr(0, 3) == "../")
        path.erase(0, 3);
    return path;
}

[[nodiscard]] std::string rootModuleName(
    std::string_view projectRoot,
    std::string_view filePath) {
    if (!projectRoot.empty()) {
        const std::string name =
            std::filesystem::path(std::string(projectRoot)).filename().string();
        if (!name.empty())
            return name;
    }
    if (!filePath.empty()) {
        std::string name =
            std::filesystem::path(std::string(filePath)).stem().string();
        if (!name.empty())
            return name;
    }
    return "root";
}

[[nodiscard]] std::string moduleForImport(
    std::string_view rootName, std::string_view raw) {
    const std::string normalized = normalizeImportPath(raw);
    if (normalized.empty())
        return std::string(rootName);
    return std::string(rootName) + "/" + normalized;
}

void pushDiagnostic(
    DynArray<common::diagnostic::Diagnostic> &diagnostics,
    FileId file,
    Span span,
    std::string message) {
    diagnostics.push(common::diagnostic::Diagnostic{
        .span = common::memory::SourceSpan{file, span},
        .severity = common::diagnostic::Severity::Error,
        .code = 0,
        .message = std::move(message),
    });
}

struct PendingLoad {
    std::string moduleName;
    std::string parentName;
    std::string path;

    PendingLoad(std::string moduleName_, std::string parentName_,
                std::string path_)
        : moduleName(std::move(moduleName_)),
          parentName(std::move(parentName_)), path(std::move(path_)) {}
};

void enqueue(
    std::vector<PendingLoad> &queue,
    std::string moduleName,
    std::string parentName,
    std::string path) {
    queue.emplace_back(std::move(moduleName), std::move(parentName),
                       std::move(path));
}

[[nodiscard]] std::string importPathFromNode(
    const sample::ImportDecl *import) {
    if (!import->rawPath.empty())
        return normalizeImportPath(import->rawPath);
    std::string path;
    for (std::string_view segment : import->path) {
        path += segment;
        path.push_back('/');
    }
    if (!path.empty())
        path.pop_back();
    return path;
}

[[nodiscard]] bool isHeaderOrAsset(
    std::string_view raw,
    bool isHeader,
    bool isAsset) {
    if (isHeader || isAsset)
        return true;
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
        return true;
    const std::string normalized = normalizeImportPath(raw);
    const std::size_t dot = normalized.rfind('.');
    if (dot != std::string::npos && normalized.find('/', dot) == std::string::npos)
        return normalized.substr(dot) != ".zith";
    return false;
}

} // namespace

const ScanRecord *ScanInfo::lookup(std::string_view name) const {
    for (const ScanRecord &record : records) {
        if (record.name == name)
            return &record;
    }
    return nullptr;
}

const generated_ast::Program *ImportInfo::program(
    std::string_view module) const {
    const auto *index = outputByModule.get(std::string(module));
    if (index == nullptr || *index >= outputs.size())
        return nullptr;
    return outputs[*index].ast.root;
}

const toolkit::import::Module *ImportInfo::module(
    std::string_view name) const {
    const auto found = graph.lookupModule(std::string(name));
    return found.isValid() ? &found.value() : nullptr;
}

const ResolvedSymbol *ResolvedInfo::lookup(
    std::string_view module, std::string_view name) const {
    for (const ResolvedSymbol &symbol : symbols) {
        if (symbol.moduleName == module && symbol.name == name)
            return &symbol;
    }
    return nullptr;
}

Result<void> scanProgram(
    const Program &program,
    FileId file,
    ScanInfo &info) {
    for (AstNode *raw : program.body) {
        if (raw == nullptr || raw->kind != generated_ast::NodeKind::Declaration)
            continue;
        auto *declaration = static_cast<Declaration *>(raw);
        const DeclKind kind = static_cast<DeclKind>(declaration->kind);
        if (kind == DeclKind::Import || declaration->name.empty())
            continue;

        if (info.lookup(declaration->name) != nullptr) {
            if (kind == DeclKind::Function)
                continue;
            return Error{
                "duplicate declaration '" +
                std::string(declaration->name) + "' in module"};
        }

        info.records.push(ScanRecord{
            declaration,
            declaration->name,
            file,
            declaration->span,
            symKindFor(kind),
            visibilityFrom(declaration),
        });
    }
    return {};
}

Result<void> importProgram(
    const sample::ParseOutput &root,
    FileId rootFile,
    std::string_view projectRoot,
    common::memory::SourceMap &sourceMap,
    ImportInfo &info) {
    if (root.ast.root == nullptr)
        return Error{"cannot import an unparsed program"};

    std::filesystem::path projectRootPath =
        std::filesystem::path(std::string(projectRoot));
    if (!projectRoot.empty() && !std::filesystem::exists(projectRootPath))
        return Error{"project root does not exist: " +
                     std::string(projectRoot)};

    const auto rootFileName = sourceMap.view(rootFile);
    const std::string rootName =
        rootModuleName(projectRoot,
                       rootFileName.isValid()
                           ? rootFileName.value()
                           : std::string_view{});

    const auto rootLoc = sourceMap.get(rootFile);
    if (rootLoc.isEmpty())
        return Error{"root source disappeared"};
    const std::string_view rootSource = rootLoc.value().get().slice();

    generated_parser::Parser<sample::ParseOutput> rootParser(info.arena);
    generated_lexer::TokenStream rootTokens =
        generated_lexer::tokenize(rootSource);
    sample::ParseOutput parsedRoot =
        hooks::parser::parseSource(rootParser, rootTokens, rootSource);
    for (const sample::ParserDiagnostic &diag : parsedRoot.diagnostics) {
        pushDiagnostic(info.diagnosticSink(), rootFile, diag.span, diag.message);
    }

    auto rootModule = info.graph.addModule(rootName);
    if (rootModule.isEmpty())
        return Error{"cannot create root module " + rootName};
    info.rootModuleName = info.interner.lookup(info.interner.intern(rootName));
    info.outputByModule.insert(rootName, 0);
    info.outputs.emplace(std::move(parsedRoot));
    info.outputFiles.push(rootFile);
    const std::string_view storedRootName = info.rootModuleName;
    info.moduleNames.push(storedRootName);
    info.outputImports.emplace(info.outputs.back().imports);

    std::vector<PendingLoad> queue;

    const auto collectImports = [&](std::size_t outputIndex,
                                    std::string_view parentName) {
        const std::vector<sample::ImportDecl> &imports =
            outputIndex < info.outputImports.size()
                ? info.outputImports[outputIndex]
                : info.outputs[outputIndex].imports;
        for (const sample::ImportDecl &import : imports) {
            const std::string path = importPathFromNode(&import);
            if (isHeaderOrAsset(import.rawPath, import.isHeader,
                                import.isAsset)) {
                info.deferredImports.push(DeferredImport{
                    .moduleName =
                        info.interner.lookup(info.interner.intern(
                            import.rawPath.empty() ? path : import.rawPath)),
                    .path = info.interner.lookup(info.interner.intern(path)),
                    .alias = info.interner.lookup(
                        info.interner.intern(import.alias)),
                    .selectors = import.selectors,
                    .isExport = import.isExport,
                    .isAsset = import.isAsset,
                    .isHeader = import.isHeader,
                    .span = import.span,
                });
                continue;
            }
            if (path.empty())
                continue;
            const std::string moduleName =
                moduleForImport(rootName,
                                import.rawPath.empty() ? path
                                                       : import.rawPath);
            enqueue(queue, moduleName, std::string(parentName), path);
        }
    };

    collectImports(0, rootName);

    while (!queue.empty()) {
        PendingLoad load = std::move(queue.back());
        queue.pop_back();
        const bool alreadyLoaded =
            info.outputByModule.contains(load.moduleName);
        const auto importedModule = info.graph.lookupModule(load.moduleName);
        const auto parent = info.graph.lookupModule(load.parentName);
        if (parent.isValid() && importedModule.isValid())
            info.graph.addDependency(parent.value(), importedModule.value());
        if (alreadyLoaded)
            continue;

        std::filesystem::path full =
            projectRootPath / load.path;
        if (!full.has_extension())
            full += ".zith";

        const auto loaded = sourceMap.loadFile(full.string());
        if (!loaded)
            return Error{"failed to load import '" + load.path + "': " +
                         loaded.error().msg};

        const auto loc = sourceMap.get(loaded.value());
        if (loc.isEmpty())
            return Error{"imported source disappeared"};
        const std::string_view source = loc.value().get().slice();

        generated_parser::Parser<sample::ParseOutput> parser(info.arena);
        generated_lexer::TokenStream tokens =
            generated_lexer::tokenize(source);
        sample::ParseOutput output =
            hooks::parser::parseSource(parser, tokens, source);
        for (const sample::ParserDiagnostic &diag : output.diagnostics) {
            pushDiagnostic(info.diagnosticSink(), loaded.value(),
                           diag.span, diag.message);
        }

        const std::size_t index = info.outputs.size();
        info.outputs.emplace(std::move(output));
        info.outputFiles.push(loaded.value());
        const std::string_view storedModuleName =
            info.interner.lookup(info.interner.intern(load.moduleName));
        info.moduleNames.push(storedModuleName);
        info.outputImports.emplace(info.outputs.back().imports);
        info.outputByModule.insert(load.moduleName, index);

        auto createdModule = info.graph.addModule(load.moduleName);
        if (createdModule.isEmpty())
            return Error{"cannot create module " + load.moduleName};
        if (parent.isValid())
            info.graph.addDependency(parent.value(), createdModule.value());

        collectImports(index, load.moduleName);
    }

    const auto finalized = info.graph.finalize();
    if (finalized.isError())
        return finalized.error();
    return {};
}

Result<void> resolveModules(const ImportInfo &imports, ResolvedInfo &out) {
    for (const DeferredImport &deferred : imports.deferredImports) {
        if (deferred.isHeader || deferred.isAsset) {
            return Error{"header/asset imports are not supported yet"};
        }
    }

    for (std::size_t index = 0; index < imports.outputs.size(); ++index) {
        const sample::ParseOutput &output = imports.outputs[index];
        const std::string_view moduleName =
            index < imports.moduleNames.size()
                ? imports.moduleNames[index]
                : std::string_view{};
        if (output.ast.root == nullptr || moduleName.empty())
            continue;

        const auto moduleFound =
            imports.graph.lookupModule(std::string(moduleName));
        if (moduleFound.isEmpty())
            continue;
        const toolkit::import::Module &module = moduleFound.value();

        const std::vector<sample::ImportDecl> &parsedImports =
            index < imports.outputImports.size()
                ? imports.outputImports[index]
                : output.imports;
        const auto importCoversModule = [&](std::string_view target) {
            for (const sample::ImportDecl &import : parsedImports) {
                if (!import.isFrom)
                    continue;
                const std::string targetPath = importPathFromNode(&import);
                const std::string targetName =
                    moduleForImport(imports.rootModuleName,
                                    import.rawPath.empty() ? targetPath
                                                           : import.rawPath);
                if (targetName == target)
                    return true;
            }
            return false;
        };

        for (AstNode *raw : output.ast.root->body) {
            if (raw == nullptr ||
                raw->kind != generated_ast::NodeKind::Declaration)
                continue;
            auto *decl = static_cast<Declaration *>(raw);
            const DeclKind kind = static_cast<DeclKind>(decl->kind);
            if (kind == DeclKind::Import || decl->name.empty())
                continue;
            if (out.lookup(moduleName, decl->name) != nullptr)
                continue;
            out.symbols.push(ResolvedSymbol{
                .name = decl->name,
                .moduleName = moduleName,
                .ownerModuleName = moduleName,
                .declaration = decl,
                .kind = symKindFor(kind),
                .visibility = visibilityFrom(decl),
                .file = index < imports.outputFiles.size()
                            ? imports.outputFiles[index]
                            : 0,
                .span = decl->span,
            });
        }

        for (std::size_t other = 0; other < imports.outputs.size(); ++other) {
            if (other == index)
                continue;
            const std::string_view otherName =
                other < imports.moduleNames.size()
                    ? imports.moduleNames[other]
                    : std::string_view{};
            if (otherName.empty())
                continue;
            const auto otherFound =
                imports.graph.lookupModule(std::string(otherName));
            if (otherFound.isEmpty())
                continue;
            const sample::ParseOutput &otherOutput = imports.outputs[other];
            if (otherOutput.ast.root == nullptr)
                continue;

            for (AstNode *raw : otherOutput.ast.root->body) {
                if (raw == nullptr ||
                    raw->kind != generated_ast::NodeKind::Declaration)
                    continue;
                auto *decl = static_cast<Declaration *>(raw);
                const DeclKind kind = static_cast<DeclKind>(decl->kind);
                if (kind == DeclKind::Import || decl->name.empty())
                    continue;
                if (out.lookup(moduleName, decl->name) != nullptr)
                    continue;
                if (importCoversModule(otherName))
                    continue;
                const SymbolVisibility visibility = visibilityFrom(decl);
                if (!visibilityAllows(imports.graph, module, otherFound.value(),
                                      moduleName, otherName, visibility))
                    continue;
                out.symbols.push(ResolvedSymbol{
                    .name = decl->name,
                    .moduleName = moduleName,
                    .ownerModuleName = otherName,
                    .declaration = decl,
                    .kind = symKindFor(kind),
                    .visibility = visibility,
                    .file = other < imports.outputFiles.size()
                                ? imports.outputFiles[other]
                                : 0,
                    .span = decl->span,
                });
            }
        }

        for (const sample::ImportDecl &parsedImport : parsedImports) {
            if (parsedImport.isHeader || parsedImport.isAsset)
                continue;
            const std::string targetPath =
                importPathFromNode(&parsedImport);
            const std::string targetName =
                moduleForImport(imports.rootModuleName,
                                parsedImport.rawPath.empty()
                                    ? targetPath
                                    : parsedImport.rawPath);
            const auto targetFound =
                imports.graph.lookupModule(std::string(targetName));
            if (targetFound.isEmpty())
                continue;
            const auto targetIndex =
                imports.outputByModule.get(std::string(targetName));
            if (targetIndex == nullptr)
                continue;
            const std::string_view targetModuleName =
                *targetIndex < imports.moduleNames.size()
                    ? imports.moduleNames[*targetIndex]
                    : std::string_view{};
            const Program *targetProgram =
                imports.program(targetName);
            if (targetProgram == nullptr)
                continue;

            for (AstNode *raw : targetProgram->body) {
                if (raw == nullptr ||
                    raw->kind != generated_ast::NodeKind::Declaration)
                    continue;
                auto *decl = static_cast<Declaration *>(raw);
                const DeclKind kind = static_cast<DeclKind>(decl->kind);
                if (kind == DeclKind::Import || decl->name.empty())
                    continue;
                const SymbolVisibility visibility = visibilityFrom(decl);
                if (!visibilityAllows(imports.graph, module,
                                      targetFound.value(), moduleName,
                                      targetModuleName, visibility))
                    continue;

                std::string_view exposed = decl->name;
                if (!parsedImport.selectors.empty()) {
                    bool selected = false;
                    for (const sample::ImportSelector &selector :
                         parsedImport.selectors) {
                        if (selector.name != decl->name)
                            continue;
                        selected = true;
                        if (!selector.alias.empty())
                            exposed = selector.alias;
                        break;
                    }
                    if (parsedImport.isFrom && !selected)
                        continue;
                }
                if (out.lookup(moduleName, exposed) != nullptr)
                    continue;
                out.symbols.push(ResolvedSymbol{
                    .name = exposed,
                    .moduleName = moduleName,
                    .ownerModuleName = targetModuleName,
                    .declaration = decl,
                    .kind = symKindFor(kind),
                    .visibility = visibility,
                    .file = imports.outputFiles[*targetIndex],
                    .span = decl->span,
                });
            }
        }
    }

    return {};
}

} // namespace toolkit::resolution
