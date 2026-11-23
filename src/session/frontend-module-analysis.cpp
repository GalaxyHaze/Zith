#include "session/frontend-context-internal.hpp"
#include "session/frontend-context.hpp"

#include "diagnostics/error-codes.hpp"
#include "support/source-extension.hpp"

#ifdef ZITH_HAS_LLVM
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
#endif

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zith::session {
namespace {

namespace fs = std::filesystem;

struct TargetComponents {
    std::optional<std::string> arch;
    std::optional<std::string> os;
};

#ifdef ZITH_HAS_LLVM
[[nodiscard]] TargetComponents targetComponents(const std::string &target_triple) {
#ifdef ZITH_HAS_LLVM
    const std::string effective =
        target_triple.empty() ? llvm::sys::getDefaultTargetTriple() : target_triple;
    const auto triple    = llvm::Triple(effective);
    const auto arch_name = triple.getArchName().str();
    const auto os_name   = triple.getOSName().str();
    TargetComponents result;
    if (!arch_name.empty() && arch_name != "unknown")
        result.arch = arch_name;
    if (!os_name.empty() && os_name != "unknown")
        result.os = os_name;
    return result;
#else
    (void)target_triple;
    return {};
#endif
}
#endif

#ifdef ZITH_HAS_LLVM
[[nodiscard]] std::vector<std::string> platformVariantSuffixes(const TargetComponents &components) {
    std::vector<std::string> suffixes;
    if (components.arch && components.os)
        suffixes.emplace_back("." + *components.arch + "." + *components.os);
    if (components.arch)
        suffixes.emplace_back("." + *components.arch);
    if (components.os)
        suffixes.emplace_back("." + *components.os);
    return suffixes;
}
#endif

[[maybe_unused]] [[nodiscard]] bool isZithmFile(const fs::path &path) {
    return support::isZithmSource(path);
}

[[maybe_unused]] [[nodiscard]] bool isHeaderFile(const fs::path &path) {
    return path.extension() == ".h";
}

[[maybe_unused]] [[nodiscard]] bool isCppHeaderFile(const fs::path &path) {
    return path.extension() == ".hpp";
}

[[maybe_unused]] [[nodiscard]] bool isWithinRoots(const fs::path &path,
                                                  const std::vector<std::string> &roots) {
    const auto normalized_path = fs::weakly_canonical(path).lexically_normal();
    for (const auto &root : roots) {
        std::error_code error;
        const auto relative = fs::relative(normalized_path, fs::path(root), error);
        if (!error && (relative.empty() || *relative.begin() != ".."))
            return true;
    }
    return false;
}

} // namespace

ModuleDiagnostic makeImportDiagnostic(const ModuleArtifact &artifact, const ImportRequest &request,
                                      std::string message) {
    return {
        diagnostics::Severity::Error, diagnostics::err::ImportError,
        std::move(message),           artifact.fileId,
        request.span.start,           request.span.end,
    };
}

std::vector<frontend::ImportedMacroRecord>
FrontendContext::importedMacrosFor(const ModuleArtifact &module,
                                   const std::vector<ModuleArtifactPtr> &modules,
                                   const std::vector<ImportEdge> &import_graph) {
    memory::FlatMap<ModuleKey, const ModuleArtifact *> module_by_key;
    for (const auto &candidate : modules)
        module_by_key.insert(candidate->key, candidate.get());

    std::vector<frontend::ImportedMacroRecord> result;

    auto appendMacros = [&](const ModuleArtifact *target, const ImportRequest &request,
                            const ImportSelectorRequest *selector,
                            const std::string &namespace_prefix) {
        (void)request;
        if (target->frontend == nullptr)
            return;
        for (const auto &decl : target->frontend->declarations()) {
            if (decl.kind != frontend::DeclKind::Macro)
                continue;
            if (selector != nullptr && decl.name != selector->name)
                continue;
            if (decl.visibility != frontend::Visibility::Public)
                continue;

            frontend::ImportedMacroRecord record;
            if (selector != nullptr) {
                record.name = selector->alias.empty() ? selector->name : selector->alias;
            } else if (!namespace_prefix.empty()) {
                record.name = namespace_prefix + "." + decl.name;
            } else {
                record.name = decl.name;
            }
            record.span               = decl.span;
            record.aliasSpan          = selector != nullptr && !selector->alias.empty()
                                            ? selector->aliasSpan
                                            : frontend::TextSpan{};
            record.isRawMacro         = decl.isRawMacro;
            record.isTagMacro         = decl.isTagMacro;
            record.hasAttributesParam = decl.hasAttributesParam;
            for (const auto &attribute : decl.attributes) {
                if (attribute.kind == frontend::AttributeKind::Scope) {
                    record.isScopeMacro = true;
                    break;
                }
            }
            record.parameters         = decl.parameters;
            record.body               = decl.body;
            record.source             = target->frontend.get();
            result.push_back(std::move(record));
        }
    };

    for (const auto &edge : import_graph) {
        if (edge.importer != module.key || !edge.error.empty())
            continue;
        if (edge.targetKind != ImportTargetKind::Zith &&
            edge.targetKind != ImportTargetKind::Directory) {
            continue;
        }

        for (const auto &target_key : edge.targets) {
            const auto *target = module_by_key.get(target_key);
            if (!target)
                continue;
            if ((*target)->frontend == nullptr)
                continue;

            if (!edge.request.selectors.empty()) {
                for (const auto &selector : edge.request.selectors)
                    appendMacros(*target, edge.request, &selector, {});
                continue;
            }
            if (edge.request.isFrom) {
                appendMacros(*target, edge.request, nullptr, {});
            } else if (!edge.request.alias.empty()) {
                appendMacros(*target, edge.request, nullptr, edge.request.alias);
            } else if (!edge.request.path.empty()) {
                appendMacros(*target, edge.request, nullptr, edge.request.path.back());
            }
        }
    }
    return result;
}

std::vector<std::string> FrontendContext::visibleRootsFor(const std::string_view root_path) const {
    (void)root_path;
    std::vector<std::string> roots;
    roots.reserve(config_.stdlibRoots.size() + config_.includeRoots.size() +
                  config_.systemIncludeRoots.size() + 2U);
    roots.insert(roots.end(), config_.stdlibRoots.begin(), config_.stdlibRoots.end());
    roots.insert(roots.end(), config_.includeRoots.begin(), config_.includeRoots.end());
    if (!config_.workspaceRoot.empty())
        roots.push_back(config_.workspaceRoot);
#ifndef ZITH_IS_WASM
    roots.push_back(fs::path(root_path).parent_path().generic_string());
#endif
    // System headers resolve last so project and -I roots always shadow them.
    roots.insert(roots.end(), config_.systemIncludeRoots.begin(), config_.systemIncludeRoots.end());
    normalizeRoots(roots);
    return roots;
}

std::vector<std::string> FrontendContext::collectDirectoryModules(const std::string_view directory,
                                                                  const int32_t depth) {
    std::vector<std::string> paths;
#ifndef ZITH_IS_WASM
    std::error_code error;
    const fs::path base(directory);
    if (!fs::is_directory(base, error))
        return paths;

    const auto options = fs::directory_options::skip_permission_denied;
    for (fs::recursive_directory_iterator iterator(base, options, error), end;
         iterator != end && !error; iterator.increment(error)) {
        if (depth != -1) {
            const auto relative = iterator.depth() + 1;
            if (relative > depth) {
                iterator.disable_recursion_pending();
                continue;
            }
        }
        if (iterator->is_regular_file(error) && isZithmFile(iterator->path()))
            paths.push_back(SourceCatalog::canonicalPath(iterator->path().generic_string()));
    }
#else
    (void)directory;
    (void)depth;
#endif
    std::sort(paths.begin(), paths.end());
    return paths;
}

FrontendContext::ResolvedImport
FrontendContext::resolveImport(const ModuleArtifact &artifact, const ImportRequest &request,
                               const std::vector<std::string> &visible_roots) const {
#ifdef ZITH_IS_WASM
    if (request.isAsset || request.isHeader) {
        if (request.isAsset) {
            (void)artifact;
            (void)visible_roots;
            return {};
        }
        const std::string header_path =
            request.headerPath + std::string(support::kZithmSourceExtension);
        for (const auto &root : visible_roots) {
            const auto path = root + "/" + header_path;
            if (sourceForPath(path))
                return {{path}, ImportTargetKind::Zith, true};
        }
        return {};
    }
    const std::string import_path = request.importKey();
    const std::string source_extension(support::kZithmSourceExtension);
    const std::vector<std::string> candidates = {
        import_path + source_extension,
        import_path + "/mod" + source_extension,
    };
    for (const auto &root : visible_roots) {
        for (const auto &candidate : candidates) {
            const auto path = root + "/" + candidate;
            if (sourceForPath(path))
                return {{path}, ImportTargetKind::Zith, true};
        }
    }
    return {};
#else
    if (request.isAsset) {
        std::vector<std::string> asset_roots = config_.assetRoots;
        if (!config_.workspaceRoot.empty())
            asset_roots.push_back((fs::path(config_.workspaceRoot) / "assets").generic_string());
        asset_roots.push_back(fs::path(artifact.key).parent_path().generic_string());
        normalizeRoots(asset_roots);

        std::string asset_key = request.rawPath.empty() ? request.importKey() : request.rawPath;
        if (asset_key.starts_with("assets/"))
            asset_key.erase(0, std::string_view{"assets/"}.size());
        const fs::path asset_path(asset_key);
        for (const auto &root : asset_roots) {
            const auto candidate = fs::weakly_canonical(fs::path(root) / asset_path);
            std::error_code error;
            if (fs::is_regular_file(candidate, error) && isWithinRoots(candidate, asset_roots))
                return {{SourceCatalog::canonicalPath(candidate.generic_string())},
                        ImportTargetKind::Asset,
                        true};
        }
        return {{}, ImportTargetKind::Asset, false};
    }

#ifdef ZITH_HAS_LLVM
    const auto components = targetComponents(config_.targetTriple);
    const auto suffixes   = platformVariantSuffixes(components);
#else
    const std::vector<std::string> suffixes;
#endif
    std::optional<fs::path> imported;
    const fs::path import_path(request.isHeader ? request.headerPath : request.importKey());
    std::vector<fs::path> variant_paths;
    bool considered_platform_variants = false;
    if (!request.isHeader && !request.path.empty()) {
        for (const auto &suffix : suffixes) {
            const auto &last = request.path.back();
            auto variant     = import_path;
            variant = variant.parent_path() /
                      fs::path(last + suffix + std::string(support::kZithmSourceExtension));
            variant_paths.push_back(variant);
            considered_platform_variants = true;
        }
    }

    std::vector<fs::path> search_roots{fs::path(artifact.key).parent_path()};
    for (const auto &root : visible_roots)
        search_roots.push_back(fs::path(root));
    for (const auto &root : search_roots) {
        std::error_code error;
        if (request.isHeader) {
            const auto header = root / import_path;
            if (fs::is_regular_file(header, error)) {
                imported = header;
                break;
            }
        } else {
            const auto literal = root / import_path;
            if (fs::is_regular_file(literal, error)) {
                imported = literal;
                break;
            }
            for (const auto &variant : variant_paths) {
                const auto candidate = root / variant;
                error.clear();
                if (fs::is_regular_file(candidate, error)) {
                    imported = candidate;
                    break;
                }
            }
            if (imported)
                break;
            error.clear();
            const auto generic =
                root / (import_path.string() + std::string(support::kZithmSourceExtension));
            if (fs::is_regular_file(generic, error)) {
                imported = generic;
                break;
            }
            error.clear();
            // A sibling module file is more specific than aggregating a
            // same-named directory of modules.
            if (fs::is_directory(literal, error)) {
                imported = literal;
                break;
            }
            error.clear();
            const auto module_file =
                root / import_path / ("mod" + std::string(support::kZithmSourceExtension));
            if (fs::is_regular_file(module_file, error)) {
                imported = module_file;
                break;
            }
        }
    }
    if (!imported)
        return {{}, ImportTargetKind::Zith, false, considered_platform_variants};
    const fs::path resolved(*imported);
    if (!isWithinRoots(resolved, visible_roots))
        return {};
    if (isHeaderFile(resolved))
        return {{SourceCatalog::canonicalPath(resolved.generic_string())},
                ImportTargetKind::CHeader,
                true};
    if (isCppHeaderFile(resolved))
        return {{SourceCatalog::canonicalPath(resolved.generic_string())},
                ImportTargetKind::CppHeader,
                true};
    if (fs::is_directory(resolved))
        return {collectDirectoryModules(resolved.generic_string(), request.depth),
                ImportTargetKind::Directory, true};
    if (!isZithmFile(resolved))
        return {};
    return {
        {SourceCatalog::canonicalPath(resolved.generic_string())}, ImportTargetKind::Zith, true};
#endif
}

ModuleArtifactPtr FrontendContext::buildModule(
    SourceCatalog::SourcePtr source,
    const std::vector<frontend::ImportedMacroRecord> &imported_macros) const {
    auto artifact         = std::make_shared<ModuleArtifact>();
    artifact->key         = source->canonicalPath;
    artifact->fileId      = source->id;
    artifact->fingerprint = source->fingerprint;
    artifact->source      = source;

    const auto parse_start = std::chrono::steady_clock::now();
    artifact->frontend     = std::make_shared<const frontend::FrontendSnapshot>(
        frontend::parseWithImports(source->text, imported_macros));
    artifact->timings.lexMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - parse_start)
            .count();
    artifact->timings.expandMs = artifact->frontend->expandMs();

    for (const auto &diagnostic : artifact->frontend->diagnostics()) {
        artifact->diagnostics.push_back({
            diagnostic.isWarning ? diagnostics::Severity::Warning : diagnostics::Severity::Error,
            diagnostic.code,
            diagnostic.message,
            source->id,
            diagnostic.span.start,
            diagnostic.span.end,
        });
    }
    for (const auto &declaration : artifact->frontend->declarations()) {
        if (declaration.kind == frontend::DeclKind::Import) {
            ImportRequest request;
            request.path       = declaration.import.path;
            request.pathSpans  = declaration.import.pathSpans;
            request.rawPath    = declaration.import.rawPath;
            request.headerPath = declaration.import.headerPath;
            request.alias      = declaration.import.alias;
            request.isFrom     = declaration.import.isFrom;
            request.isExport   = declaration.import.isExport;
            request.isAsset    = declaration.import.isAsset;
            request.isHeader   = declaration.import.isHeader;
            request.depth      = declaration.import.depth;
            request.span       = declaration.span;
            request.pathSpan   = declaration.import.pathSpan;
            request.aliasSpan  = declaration.import.aliasSpan;
            for (const auto &selector : declaration.import.selectors) {
                request.selectors.push_back(
                    {selector.name, selector.alias, selector.span, selector.aliasSpan});
            }
            artifact->imports.push_back(std::move(request));
            continue;
        }
        // Methods live in the owner type's member table, not in the module
        // namespace.  Keeping them out of top-level bindings prevents
        // same-named methods on different structs/traits/interfaces from
        // being reported as duplicate top-level declarations.
        if (!declaration.ownerName.empty())
            continue;
        if (declaration.kind == frontend::DeclKind::Error ||
            declaration.visibility == frontend::Visibility::Private)
            continue;
        LocalSymbolInfo symbol{frontend::SymbolId{declaration.id.value},
                               declaration.name,
                               declaration.visibility,
                               declaration.kind,
                               declaration.span,
                               declaration.kind == frontend::DeclKind::Function
                                   ? frontend::functionSignature(*artifact->frontend, declaration)
                                   : std::string{},
                               declaration.isExtern,
                               declaration.externalSymbol,
                               declaration.isVariadic,
                               !declaration.parameters.empty() &&
                                   declaration.parameters.back().isVariadicSlice,
                               declaration.discardable(),
                               declaration.kind == frontend::DeclKind::Variable
                                   ? declaration.bindingKind
                                   : frontend::BindingKind::Let};
        if (symbol.visibility == frontend::Visibility::Public)
            artifact->publicSymbols.push_back(std::move(symbol));
        else
            artifact->moduleSymbols.push_back(std::move(symbol));
    }
    return artifact;
}

} // namespace zith::session
