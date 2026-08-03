#include "compilation-session.hpp"
#include "cli/terminal.hpp"
#ifdef ZITH_HAS_LLVM
#include "codegen/codegen.hpp"
#include <llvm/TargetParser/Host.h>
#endif
#include "comptime/solver.hpp"
#include "diagnostics/error-codes.hpp"
#include "formatter/fmt-visitor.hpp"
#include "memory/source-map.hpp"
#include "sema/heuristic-engine.hpp"
#include "sema/hir-lower-modern.hpp"
#include "sema/sema-modern.hpp"
#include "types/type-lower.hpp"

#include "cache/artifact-builder.hpp"
#include "cache/cache-paths.hpp"
#include "zirl/zirl-reader.hpp"

#ifdef ZITH_HAS_LLVM
#include "codegen/codegen.hpp"
#endif

#include "common/ast-ids.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <toml++/toml.hpp>
#include <unordered_set>
#include <vector>
#ifdef _WIN32
#include <Windows.h>
#include <process.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/wait.h>
#elif !defined(ZITH_IS_WASM)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace zith::session {

namespace {

symbols::SymKind mapFrontendDeclKind(const frontend::DeclKind kind) {
    switch (kind) {
    case frontend::DeclKind::Function:
        return symbols::SymKind::Fn;
    case frontend::DeclKind::TypeAlias:
        return symbols::SymKind::Alias;
    case frontend::DeclKind::Struct:
        return symbols::SymKind::Struct;
    case frontend::DeclKind::Enum:
        return symbols::SymKind::Enum;
    case frontend::DeclKind::Union:
        return symbols::SymKind::Union;
    case frontend::DeclKind::Trait:
        return symbols::SymKind::Trait;
    case frontend::DeclKind::Interface:
        return symbols::SymKind::Interface;
    case frontend::DeclKind::Variable:
        return symbols::SymKind::Variable;
    case frontend::DeclKind::Context:
        return symbols::SymKind::Context;
    case frontend::DeclKind::Word:
        return symbols::SymKind::Word;
    case frontend::DeclKind::Import:
        return symbols::SymKind::Module;
    case frontend::DeclKind::Error:
        break;
    }
    return symbols::SymKind::Variable;
}

symbols::SymbolVisibility mapFrontendVisibility(const frontend::Visibility visibility) {
    switch (visibility) {
    case frontend::Visibility::Public:
        return symbols::SymbolVisibility::Public;
    case frontend::Visibility::Module:
        return symbols::SymbolVisibility::Module;
    case frontend::Visibility::Private:
        return symbols::SymbolVisibility::Private;
    }
    return symbols::SymbolVisibility::Private;
}

bool isValidLibraryName(const std::string_view name) {
    if (name.empty())
        return false;
    return std::all_of(name.begin(), name.end(), [](const unsigned char character) {
        return std::isalnum(character) != 0 || character == '_' || character == '+' ||
               character == '-' || character == '.';
    });
}

int runProgram(const std::vector<std::string> &arguments) {
    if (arguments.empty())
        return -1;

    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1U);
    for (const auto &argument : arguments)
        argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);

#ifdef _WIN32
    return _spawnvp(_P_WAIT, argv.front(), argv.data());
#elif defined(ZITH_IS_WASM)
    (void)argv;
    return -1;
#else
    const pid_t child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        execvp(argv.front(), argv.data());
        _exit(127);
    }

    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return status;
#endif
}

std::string displayCommand(const std::vector<std::string> &arguments) {
    std::string command;
    for (const auto &argument : arguments) {
        if (!command.empty())
            command += ' ';
        command += argument;
    }
    return command;
}

} // namespace

CompilationSession::CompilationSession(const Options &options, std::string filePath,
                                       std::shared_ptr<FrontendContext> frontend_context)
    : mOpts(options), mFilePath(std::move(filePath)), mProjectRoot(), mProjectConfig(mScratchArena),
      mScratchArena(), mSymArena(), mTypeArena(), mHirArena(), mDiags(mScratchArena),
      mInterner(std::make_unique<memory::StringInterner>(mScratchArena)),
      mSyms(mSymArena, mInterner.get()), mTypes(mTypeArena, *mInterner), mHirModule(mHirArena),
      mFrontendContext(std::move(frontend_context)) {
    mPlan.target = mOpts.get().targetStage;
    mDiags.setColor(term::useColor(mOpts));
    mDiags.setSourceMap(&mSourceMap);

#ifndef ZITH_IS_WASM
    namespace fs = std::filesystem;
    if (fs::is_directory(mFilePath))
        mProjectRoot = fs::weakly_canonical(fs::path(mFilePath)).string();
    else
        mProjectRoot = fs::weakly_canonical(fs::path(mFilePath).parent_path()).string();

    auto toml_path = fs::path(mProjectRoot) / "ZithProject.toml";
    if (fs::exists(toml_path)) {
#if TOML_EXCEPTIONS
        try {
            auto tbl = toml::parse_file(toml_path.string());
            if (auto *build = tbl["build"].as_table()) {
                if (auto v = build->get("entry"))
                    if (auto s = v->value<std::string>())
                        mProjectConfig.entry = *s;
                if (auto v = build->get("output"))
                    if (auto s = v->value<std::string>())
                        mProjectConfig.output = *s;
            }
            if (auto *paths = tbl["paths"].as_table()) {
                if (auto v = paths->get("bin_dir"))
                    if (auto s = v->value<std::string>())
                        mProjectConfig.binDir = *s;
            }
            if (auto *ffi = tbl["ffi"].as_table()) {
                const auto loadArray = [](const toml::table &table, const char *key,
                                          memory::DynArray<std::string> &destination) {
                    if (const auto *values = table[key].as_array())
                        for (const auto &value : *values)
                            if (const auto text = value.value<std::string>())
                                destination.push(*text);
                };
                loadArray(*ffi, "include_dirs", mProjectConfig.includeDirs);
                loadArray(*ffi, "library_dirs", mProjectConfig.libraryDirs);
                loadArray(*ffi, "libraries", mProjectConfig.libraries);
                loadArray(*ffi, "defines", mProjectConfig.defines);
            }
            if (auto *proj = tbl["project"].as_table()) {
                if (auto v = proj->get("name"))
                    if (auto s = v->value<std::string>())
                        mProjectConfig.name = *s;
            }
        } catch (...) {
        }
#else
        auto result = toml::parse_file(toml_path.string());
        if (result) {
            if (auto *build = result["build"].as_table()) {
                if (auto v = build->get("entry"))
                    if (auto s = v->value<std::string>())
                        mProjectConfig.entry = *s;
                if (auto v = build->get("output"))
                    if (auto s = v->value<std::string>())
                        mProjectConfig.output = *s;
            }
            if (auto *paths = result["paths"].as_table()) {
                if (auto v = paths->get("bin_dir"))
                    if (auto s = v->value<std::string>())
                        mProjectConfig.binDir = *s;
            }
            if (auto *ffi = result["ffi"].as_table()) {
                const auto loadArray = [](const toml::table &table, const char *key,
                                          memory::DynArray<std::string> &destination) {
                    if (const auto *values = table[key].as_array())
                        for (const auto &value : *values)
                            if (const auto text = value.value<std::string>())
                                destination.push(*text);
                };
                loadArray(*ffi, "include_dirs", mProjectConfig.includeDirs);
                loadArray(*ffi, "library_dirs", mProjectConfig.libraryDirs);
                loadArray(*ffi, "libraries", mProjectConfig.libraries);
                loadArray(*ffi, "defines", mProjectConfig.defines);
            }
            if (auto *proj = result["project"].as_table()) {
                if (auto v = proj->get("name"))
                    if (auto s = v->value<std::string>())
                        mProjectConfig.name = *s;
            }
        }
#endif
    }
#endif
}

void CompilationSession::ensureFrontendContext() {
    if (mFrontendContext)
        return;

    FrontendConfig config;
    config.maxFrontendWorkers = 1;
    config.workspaceRoot      = mProjectRoot;
#ifdef ZITH_VERSION
    config.compilerVersion = ZITH_VERSION;
#else
    config.compilerVersion = "dev";
#endif
    config.targetTriple = mOpts.get().targetTriple;
    config.parseFlags   = mOpts.get().flags.strict() ? "strict" : "";
    config.sysroot      = mOpts.get().sysroot;

    for (const auto &dir : mProjectConfig.includeDirs)
        config.includeRoots.push_back(
            (std::filesystem::path(mProjectRoot) / dir).lexically_normal().string());
    for (const auto &dir : mOpts.get().includeDirs)
        config.includeRoots.push_back(dir);
    for (const auto &define : mProjectConfig.defines)
        config.cDefines.push_back(define);
    for (const auto &define : mOpts.get().defines)
        config.cDefines.push_back(define);
    for (const auto &dir : mOpts.get().assetDirs)
        config.assetRoots.push_back(dir);
    if (!mProjectConfig.assetDir.empty())
        config.assetRoots.push_back(
            (std::filesystem::path(mProjectRoot) / mProjectConfig.assetDir).string());

    mFrontendContext = std::make_shared<FrontendContext>(std::move(config));
}

bool CompilationSession::materializeFrontendSymbols() {
    if (!mSnapshot)
        return true;

    for (const auto &module : mSnapshot->modules()) {
        if (module->frontend == nullptr)
            continue;
        for (const auto &decl : module->frontend->declarations()) {
            if (decl.kind == frontend::DeclKind::Import || decl.kind == frontend::DeclKind::Error ||
                decl.name.empty()) {
                continue;
            }
            mSyms.declare(decl.name, mapFrontendVisibility(decl.visibility), 0,
                          mapFrontendDeclKind(decl.kind), ast::kInvalidDecl, {});
        }
    }
    return true;
}

bool CompilationSession::run() {
    return runTo(mPlan.target);
}

bool CompilationSession::runTo(Stage target) {
    auto t_start = std::chrono::steady_clock::now();
    mPlan.target = target;

    if (mOpts.get().flags.verbose())
        writeOutput("%s[zithc] [starting]%s %s\n", ansicolor("\033[36m"), ansicolor("\033[0m"),
                    mFilePath.c_str());

    if (mPlan.shouldStop())
        return !mDiags.hasErrors();
    if (!lexStage())
        return false;
    mPlan.advance();

    if (mPlan.shouldStop())
        return !mDiags.hasErrors();
    if (!scanStage())
        return false;
    mPlan.advance();

    if (mPlan.shouldStop())
        return !mDiags.hasErrors();
    if (!importStage())
        return false;
    mPlan.advance();

    if (mPlan.shouldStop())
        return !mDiags.hasErrors();
    if (!resolveStage())
        return false;
    mPlan.advance();

    if (mPlan.shouldStop())
        return !mDiags.hasErrors();
    if (!semaStage())
        return false;
    mPlan.advance();

    if (mPlan.shouldStop())
        return !mDiags.hasErrors();
    if (!lowerStage())
        return false;
    mPlan.advance();

    if (mPlan.shouldStop())
        return !mDiags.hasErrors();
    if (!solveStage())
        return false;
    mPlan.advance();

    if (mPlan.shouldStop())
        return !mDiags.hasErrors();
    if (!nraStage())
        return false;
    mPlan.advance();

    if (mPlan.shouldStop())
        return !mDiags.hasErrors();
    if (!codegenStage())
        return false;
    mPlan.advance();

    if (mPlan.shouldStop())
        return !mDiags.hasErrors();
    if (!cacheStage())
        return false;

    bool ok = !mDiags.hasErrors();
    if (mOpts.get().flags.verbose()) {
        auto dt =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_start)
                .count();
        writeOutput("%s[zithc] [done]%s %s %s%s%s (%.1fms)\n", ansicolor("\033[36m"),
                    ansicolor("\033[0m"), mFilePath.c_str(),
                    ansicolor(ok ? "\033[32m" : "\033[31m"), ok ? "\xe2\x9c\x93" : "\xe2\x9c\x97",
                    ansicolor("\033[0m"), dt);
    }
    return ok;
}

bool CompilationSession::lexStage() {
    auto t0 = std::chrono::steady_clock::now();

#ifndef ZITH_IS_WASM
    namespace fs = std::filesystem;

    if (fs::is_directory(mFilePath)) {
        if (mProjectConfig.entry.empty()) {
            writeOutput("%s[error]%s no entry file in ZithProject.toml\n", ansicolor("\033[31m"),
                        ansicolor("\033[0m"));
            return false;
        }
        mFilePath = (fs::path(mProjectRoot) / mProjectConfig.entry).string();
    }

    if (!mCacheStore) {
        const auto cache_root = (fs::path(mProjectRoot) / cache::kPersistentCacheDirName).string();
        mCacheStore           = std::make_unique<cache::Store>(
            cache_root,
            mFrontendContext ? mFrontendContext->config().cacheKey() : session::CacheKey{});
    }
    (void)tryLoadPersistentCache();

    if (mOpts.get().flags.verbose()) {
        std::error_code ec;
        auto fsize = fs::file_size(mFilePath, ec);
        if (ec)
            writeOutput("[file] %s\n", mFilePath.c_str());
        else
            writeOutput("[file] %s (%.1f KiB)\n", mFilePath.c_str(),
                        static_cast<double>(fsize) / 1024.0);
    }
#endif

    ensureFrontendContext();
    if (!mSnapshot) {
        auto snapshot = mContentOverride.empty()
                            ? mFrontendContext->analyzeFile(mFilePath)
                            : mFrontendContext->analyzeText(mFilePath, mContentOverride);
        if (!snapshot) {
            writeOutput("%s[error]%s failed to build frontend snapshot for '%s': %s\n",
                        ansicolor("\033[31m"), ansicolor("\033[0m"), mFilePath.c_str(),
                        snapshot.error().msg.c_str());
            return false;
        }
        mSnapshot = std::move(snapshot.value());
    }

    // Materialize all module sources into SourceMap.
    for (const auto &module : mSnapshot->modules()) {
        const auto materialized = mSourceMap.addFile(module->key, module->source->text);
        if (!materialized) {
            writeOutput("%s[error]%s failed to materialize frontend source '%s'\n",
                        ansicolor("\033[31m"), ansicolor("\033[0m"), module->key.c_str());
            return false;
        }
    }
    const auto root_key = SourceCatalog::canonicalPath(mFilePath);
    const auto *root    = mSnapshot->findModule(root_key);
    if (!root) {
        writeOutput("%s[error]%s frontend snapshot has no root module '%s'\n",
                    ansicolor("\033[31m"), ansicolor("\033[0m"), mFilePath.c_str());
        return false;
    }
    const auto root_file = mSourceMap.addFile(root->key, root->source->text);
    if (!root_file) {
        writeOutput("%s[error]%s failed to materialize root source '%s'\n", ansicolor("\033[31m"),
                    ansicolor("\033[0m"), root->key.c_str());
        return false;
    }
    mFileId = root_file.value();

    // --emit-tokens: dump tokens from the modern frontend snapshot
    if (mOpts.get().flags.emitTokens()) {
        const auto &tokens = root->frontend->tokens();
        std::fputs("--- Tokens ---\n", stdout);
        const auto &source = root->frontend->source();
        for (size_t i = 0; i < tokens.size(); ++i) {
            const auto &tok = tokens[i];
            auto text =
                std::string_view(source).substr(tok.span.start, tok.span.end - tok.span.start);
            std::printf("[%3zu] %-12s '%.*s'\n", i,
                        tok.kind == frontend::TokenKind::Keyword       ? "keyword"
                        : tok.kind == frontend::TokenKind::Identifier  ? "identifier"
                        : tok.kind == frontend::TokenKind::Literal     ? "literal"
                        : tok.kind == frontend::TokenKind::Operator    ? "operator"
                        : tok.kind == frontend::TokenKind::Punctuation ? "punctuation"
                        : tok.kind == frontend::TokenKind::End         ? "end"
                                                                       : "unknown",
                        static_cast<int>(text.size()), text.data());
        }
        std::fputs("---\n", stdout);
    }

    // --emit-ast: dump AST from the modern frontend snapshot
    if (mOpts.get().flags.emitAst()) {
        std::fputs("--- AST ---\n", stdout);
        for (const auto &decl : root->frontend->declarations()) {
            std::printf("decl %s (kind=%d, vis=%d, span=%u..%u)\n", decl.name.c_str(),
                        static_cast<int>(decl.kind), static_cast<int>(decl.visibility),
                        decl.span.start, decl.span.end);
        }
        std::fputs("--- Symbols ---\n", stdout);
        mSyms.dump(stdout, nullptr);
        std::fputs("---\n", stdout);
    }

    auto lexDt =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    mStageDurations[static_cast<size_t>(StageIndex::Lex)] = lexDt;
    if (mOpts.get().flags.verbose()) {
        writeOutput("  [lex] %5.1fms\n", lexDt);
    }

    return true;
}

bool CompilationSession::scanStage() {
    auto t0 = std::chrono::steady_clock::now();
    if (mCacheHydrated)
        return true;

    if (mSnapshot->hasErrors())
        forwardSnapshotDiagnostics();
    else
        forwardSnapshotDiagnostics(); // warnings always forwarded

    auto scanDt =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    mStageDurations[static_cast<size_t>(StageIndex::Scan)] = scanDt;
    if (mOpts.get().flags.verbose()) {
        writeOutput("  [scan] %zu top-level decls  (%5.1fms, %zu diagnostics)\n",
                    mSnapshot->modules().empty()
                        ? 0U
                        : mSnapshot->modules().front()->frontend->declarations().size(),
                    scanDt, mSnapshot->diagnostics().size());
    }
    const bool snapshot_failed = mSnapshot->hasErrors();
    forwardSnapshotDiagnostics();
    return !snapshot_failed;
}

bool CompilationSession::importStage() {
    auto t0 = std::chrono::steady_clock::now();
    if (mCacheHydrated)
        return true;

    const auto ok = materializeFrontendSymbols();
    auto importDt =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    mStageDurations[static_cast<size_t>(StageIndex::Import)] = importDt;
    if (mOpts.get().flags.verbose()) {
        writeOutput("  [import] %zu symbols  (%5.1fms)\n", mSyms.symbolCount(), importDt);
    }
    return ok;
}

void CompilationSession::forwardSnapshotDiagnostics() {
    if (!mSnapshot || mSnapshotDiagsForwarded)
        return;
    mSnapshotDiagsForwarded = true;
    for (const auto &diagnostic : mSnapshot->diagnostics()) {
        mDiags.report(diagnostic.severity, diagnostic.code, diagnostic.message,
                      memory::Span{diagnostic.file, diagnostic.start, diagnostic.end});
    }
    mDiags.emit();
}

bool CompilationSession::resolveStage() {
    auto t0 = std::chrono::steady_clock::now();
    if (mCacheHydrated)
        return true;

    if (mSnapshot->hasErrors()) {
        forwardSnapshotDiagnostics();
        return false;
    }
    auto resolveDt =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    mStageDurations[static_cast<size_t>(StageIndex::Resolve)] = resolveDt;
    if (mOpts.get().flags.verbose()) {
        writeOutput("  [resolve] %5.1fms\n", resolveDt);
    }
    return true;
}

bool CompilationSession::semaStage() {
    auto t0 = std::chrono::steady_clock::now();
    if (mCacheHydrated)
        return true;

    if (mSnapshot->hasErrors()) {
        forwardSnapshotDiagnostics();
        return false;
    }
    mModernSemaPipeline =
        std::make_unique<sema::modern::SemaPipeline>(mScratchArena, mDiags, *mSnapshot);
    if (!mModernSemaPipeline->run()) {
        mDiags.emit();
        return false;
    }
    auto semaDt =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    mStageDurations[static_cast<size_t>(StageIndex::Sema)] = semaDt;
    if (mOpts.get().flags.verbose()) {
        writeOutput("  [sema] modern types ready  (%5.1fms)\n", semaDt);
    }
    return !mDiags.hasErrors();
}

bool CompilationSession::lowerStage() {
    auto t0 = std::chrono::steady_clock::now();
    if (mCacheHydrated)
        return true;

    if (mDiags.hasErrors()) {
        mDiags.emit();
        return false;
    }

    sema::modern::HirLowerModern lower(mHirArena, mDiags, *mSnapshot, *mModernSemaPipeline, mTypes,
                                       *mInterner);
    if (!lower.run()) {
        mDiags.emit();
        return false;
    }

    mHirModule = lower.takeHir();
    mModernTypeTable =
        std::make_unique<sema::modern::TypeTable>(mModernSemaPipeline->takeTypeTable());
    mModernSemaPipeline.reset();

    if (mOpts.get().flags.emitHir()) {
        std::fputs("--- HIR ---\n", stdout);
        mHirModule.dump(stdout, *mInterner);
        std::fputs("---\n", stdout);
    }

    auto lowerDt =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    mStageDurations[static_cast<size_t>(StageIndex::Lower)] = lowerDt;
    if (mOpts.get().flags.verbose()) {
        writeOutput("  [lower] %zu fns lowered  (%5.1fms)\n", mHirModule.getFnCount(), lowerDt);
    }
    return !mDiags.hasErrors();
}

bool CompilationSession::solveStage() {
    auto t0 = std::chrono::steady_clock::now();
    if (mDiags.hasErrors()) {
        mDiags.emit();
        return false;
    }
    comptime::Solver solver(mTypes, nullptr, nullptr, mSyms, mDiags, mHirArena,
                            mModernTypeTable.get());
    if (!solver.solve(mHirModule)) {
        mDiags.emit();
        return false;
    }
    auto solveDt =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    mStageDurations[static_cast<size_t>(StageIndex::Solve)] = solveDt;
    if (mOpts.get().flags.verbose()) {
        writeOutput("  [solve] \xe2\x80\x94 (stub)  (%5.1fms)\n", solveDt);
    }
    return true;
}

bool CompilationSession::nraStage() {
    auto t0 = std::chrono::steady_clock::now();
    auto nraDt =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (mModernTypeTable && mOpts.get().flags.verbose()) {
        writeOutput("  [nra] modern types: %zu  (%5.1fms)\n", mModernTypeTable->size(), nraDt);
    }
    mStageDurations[static_cast<size_t>(StageIndex::Nra)] = nraDt;
    if (mOpts.get().flags.verbose()) {
        writeOutput("  [nra] \xe2\x80\x94 (stub)  (%5.1fms)\n", nraDt);
    }
    return true;
}

bool CompilationSession::codegenStage() {
    auto t0 = std::chrono::steady_clock::now();

    if (mDiags.hasErrors()) {
        mDiags.emit();
        return false;
    }

#ifdef ZITH_HAS_LLVM
    {
        codegen::CodeGen cg(*mInterner, mTypes, mOpts.get().targetTriple,
                            mOpts.get().flags.optLevel(), &mDiags);
        cg.emit(mHirModule, mFilePath);
        cg.optimize();

        if (mOpts.get().flags.emitIr()) {
            auto ir = cg.printIR();
            writeOutput("%s\n", ir.c_str());
        }

        if (mOpts.get().flags.emitAsm()) {
            auto asm_str = cg.printAsm();
            if (asm_str.empty())
                writeOutput("%s[error]%s failed to generate assembly\n", ansicolor("\033[31m"),
                            ansicolor("\033[0m"));
            else
                writeOutput("%s\n", asm_str.c_str());
        }

        auto emitTarget = mOpts.get().emitTarget;
        if (mAlwaysEmitObject || emitTarget == Options::EmitTarget::Obj ||
            emitTarget == Options::EmitTarget::Bin) {
            std::string objPath = mOpts.get().outputFile;
            if (objPath.empty()) {
                namespace fs       = std::filesystem;
                std::string objDir = (fs::path(mProjectRoot) / "cache").string();
                fs::create_directories(objDir);
                objPath = objDir + "/" + fs::path(mFilePath).filename().string() + ".o";
            }
            if (!cg.emitObject(objPath)) {
                writeOutput("%s[error]%s failed to emit object file\n", ansicolor("\033[31m"),
                            ansicolor("\033[0m"));
                return false;
            }
            mObjectPath = objPath;
        }
    }
#else
    if (mOpts.get().flags.emitIr() || mOpts.get().flags.emitAsm() ||
        mOpts.get().emitTarget == Options::EmitTarget::Obj ||
        mOpts.get().emitTarget == Options::EmitTarget::Bin) {
        writeOutput("%s[error]%s LLVM not available in this build\n", ansicolor("\033[31m"),
                    ansicolor("\033[0m"));
        return false;
    }
#endif

    auto codegenDt =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    mStageDurations[static_cast<size_t>(StageIndex::Codegen)] = codegenDt;
    if (mOpts.get().flags.verbose()) {
        writeOutput("  [codegen] %5.1fms\n", codegenDt);
    }

    return !mDiags.hasErrors();
}

bool CompilationSession::cacheStage() {
#ifndef ZITH_IS_WASM
    auto t0      = std::chrono::steady_clock::now();
    namespace fs = std::filesystem;
    fs::create_directories(fs::path(mProjectRoot) / cache::kPersistentCacheDirName);
    writePersistentCache();
    auto cacheDt =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    mStageDurations[static_cast<size_t>(StageIndex::Cache)] = cacheDt;
    if (mOpts.get().flags.verbose()) {
        const auto m = mCacheStore ? mCacheStore->metrics() : cache::StoreMetrics{};
        writeOutput("  [cache] hits=%zu misses=%zu writes=%zu invalid=%zu  (%5.1fms)\n", m.hits,
                    m.misses, m.writes, m.invalid, cacheDt);
    }
#endif
    return true;
}

std::unordered_map<std::string, double> CompilationSession::getStageDurationsMs() const {
    static constexpr const char *names[] = {
        "lex", "scan", "import", "resolve", "sema", "lower", "solve", "nra", "codegen", "cache",
    };
    std::unordered_map<std::string, double> result;
    for (size_t i = 0; i < static_cast<size_t>(StageIndex::Count); ++i)
        result[names[i]] = mStageDurations[i];
    return result;
}

ArenaMemoryUsage CompilationSession::getArenaMemoryUsage() const {
    return {
        mScratchArena.allocatedBytes(),
        mSymArena.allocatedBytes(),
        mTypeArena.allocatedBytes(),
        mHirArena.allocatedBytes(),
    };
}

bool CompilationSession::linkAndExec() {
#ifndef ZITH_IS_WASM
    if (mObjectPath.empty())
        return false;

    std::string exePath;
    if (!mOpts.get().outputFile.empty() && mOpts.get().outputFile != mObjectPath) {
        exePath = mOpts.get().outputFile;
    } else {
        namespace fs = std::filesystem;
        std::string binName;
        if (!mProjectConfig.name.empty())
            binName = mProjectConfig.name;
        else
            binName = fs::path(mFilePath).stem().string();

        std::string exeDir = (fs::path(mProjectRoot) / "target").string();
        auto &triple       = mOpts.get().targetTriple;
        if (!triple.empty()) {
#ifdef ZITH_HAS_LLVM
            std::string hostTriple = llvm::sys::getDefaultTargetTriple();
#else
            std::string hostTriple;
#endif
            if (triple != hostTriple)
                exeDir = (fs::path(mProjectRoot) / "target" / triple).string();
        }

        fs::create_directories(exeDir);
        exePath = exeDir + "/" + binName;
    }

    auto &triple = mOpts.get().targetTriple;
    bool isWasm =
        triple.find("wasm32") != std::string::npos || triple.find("wasm64") != std::string::npos;

    if (isWasm && !mOpts.get().outputFile.empty() && mOpts.get().outputFile != mObjectPath) {
        exePath = mOpts.get().outputFile;
    } else if (isWasm) {
        exePath += ".wasm";
    } else {
#ifdef _WIN32
        if (mOpts.get().outputFile.empty() || mOpts.get().outputFile == mObjectPath)
            exePath += ".exe";
#endif
    }

    if (mOpts.get().flags.verbose())
        writeOutput("  [link] %s -> %s\n", mObjectPath.c_str(), exePath.c_str());

    std::vector<std::string> link_args;
    if (isWasm) {
        bool isWasi = triple.find("wasi") != std::string::npos;
        link_args.emplace_back("wasm-ld");
        if (!isWasi)
            link_args.insert(link_args.end(), {"--no-entry", "--export-all"});
        link_args.insert(link_args.end(), {"-o", exePath, mObjectPath});
    } else {
        link_args = {"/usr/bin/cc", "-o", exePath, mObjectPath};
    }

    if (!mOpts.get().sysroot.empty())
        link_args.push_back("--sysroot=" + mOpts.get().sysroot);

    if (!isWasm) {
        for (const auto &directory : mProjectConfig.libraryDirs) {
            const auto path =
                (std::filesystem::path(mProjectRoot) / directory).lexically_normal().string();
            link_args.push_back("-L" + path);
        }
        for (const auto &directory : mOpts.get().libraryDirs)
            link_args.push_back("-L" + directory);

        const auto addLibraries = [this, &link_args](const auto &libraries) {
            for (const auto &library : libraries) {
                if (!isValidLibraryName(library)) {
                    writeOutput("%s[error]%s invalid library name '%s'\n", ansicolor("\033[31m"),
                                ansicolor("\033[0m"), library.c_str());
                    return false;
                }
                link_args.push_back("-l" + library);
            }
            return true;
        };
        if (!addLibraries(mProjectConfig.libraries) || !addLibraries(mOpts.get().libraries))
            return false;
    }

    if (mOpts.get().flags.verbose())
        writeOutput("  [link] %s\n", displayCommand(link_args).c_str());

    const int linkResult = runProgram(link_args);
    if (linkResult != 0) {
        writeOutput("%s[error]%s linking failed (exit code %d)\n", ansicolor("\033[31m"),
                    ansicolor("\033[0m"), linkResult);
        return false;
    }

    if (mOpts.get().flags.verbose())
        writeOutput("  [exec] %s\n", exePath.c_str());

    if (isWasm) {
        if (mOpts.get().flags.verbose())
            writeOutput("  [exec] skipped (cannot natively execute WASM)\n");
        mChildExitCode = 0;
        return true;
    }

    const int execResult = runProgram({exePath});
    if (execResult == -1) {
        writeOutput("%s[error]%s failed to launch executable\n", ansicolor("\033[31m"),
                    ansicolor("\033[0m"));
        return false;
    }

#ifdef _WIN32
    mChildExitCode = execResult;
#else
    if (WIFEXITED(execResult)) {
        mChildExitCode = WEXITSTATUS(execResult);
    } else if (WIFSIGNALED(execResult)) {
        mChildExitCode = 128 + WTERMSIG(execResult);
    } else {
        mChildExitCode = 1;
    }
#endif

    return true;
#else
    (void)mObjectPath;
    writeOutput("%s[error]%s cannot execute on WASM target\n", ansicolor("\033[31m"),
                ansicolor("\033[0m"));
    return false;
#endif
}

std::string CompilationSession::fmtStage() {
    if (!lexStage())
        return {};
    if (!scanStage())
        return {};

    const auto root_key = SourceCatalog::canonicalPath(mFilePath);
    const auto *root    = mSnapshot->findModule(root_key);
    if (root == nullptr || root->frontend == nullptr) {
        writeOutput("%s[error]%s formatter could not find root frontend module '%s'\n",
                    ansicolor("\033[31m"), ansicolor("\033[0m"), mFilePath.c_str());
        return {};
    }

    formatter::FmtVisitor visitor(*root->frontend);
    visitor.format();
    return visitor.result();
}

void CompilationSession::writeOutput(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    if (mBufferedOutput) {
        va_list args_copy;
        va_copy(args_copy, args);
        int len = std::vsnprintf(nullptr, 0, fmt, args_copy);
        va_end(args_copy);
        if (len > 0) {
            auto old = mOutputBuffer.size();
            mOutputBuffer.resize(old + static_cast<size_t>(len));
            std::vsnprintf(mOutputBuffer.data() + old, static_cast<size_t>(len) + 1, fmt, args);
        }
    } else {
        std::vfprintf(stderr, fmt, args);
    }
    va_end(args);
}

std::string CompilationSession::flushOutput() {
    auto result = std::move(mOutputBuffer);
    mOutputBuffer.clear();
    return result;
}

void CompilationSession::emitDiagnostics() {
    sema::HeuristicEngine heuristic;
    auto &all = mDiags.diagnostics();
    for (size_t i = 0; i < all.size(); i++) {
        heuristic.generate(all[i], mSyms, all[i].suggestions);
    }
    mDiags.setSuppressEmit(false);
    mDiags.emit();
}

bool CompilationSession::tryLoadPersistentCache() {
    if (mCacheStore == nullptr)
        return false;

    mCanonicalPath = SourceCatalog::canonicalPath(mFilePath);

    std::string source_text;
    if (!mContentOverride.empty()) {
        source_text = mContentOverride;
    } else {
        std::ifstream input(mFilePath, std::ios::binary);
        if (!input)
            return false;
        source_text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    mSourceFingerprint = ContentFingerprint::fromText(source_text);

    auto artifact = mCacheStore->load(mCanonicalPath, mSourceFingerprint);
    if (!artifact) {
        if (mOpts.get().flags.verbose())
            writeOutput("  [cache] miss for %s\n", mCanonicalPath.c_str());
        return false;
    }

    if (mOpts.get().flags.verbose())
        writeOutput("  [cache] hit for %s (%zu decls, %zu fns)\n", mCanonicalPath.c_str(),
                    artifact->decls.size(), artifact->functions.size());

    return false;
}

void CompilationSession::writePersistentCache() {
    if (mCacheStore == nullptr || mCacheHydrated)
        return;
    if (mDiags.hasErrors())
        return;

    namespace fs          = std::filesystem;
    const auto cache_root = (fs::path(mProjectRoot) / cache::kPersistentCacheDirName).string();
    if (!mCacheStore || mCacheStore->root() != cache_root) {
        mCacheStore = std::make_unique<cache::Store>(
            cache_root,
            mFrontendContext ? mFrontendContext->config().cacheKey() : session::CacheKey{});
    }

    cache::ArtifactBuilder builder(mSyms, mTypes, mHirModule, *mInterner, mSourceFingerprint,
                                   mFrontendContext ? mFrontendContext->config().cacheKey()
                                                    : session::CacheKey{});
    std::vector<cache::DependencyRecord> deps;
    std::unordered_set<std::string> seen_deps;
    auto append_dep = [&](std::string canonical_path, std::string import_key) {
        if (canonical_path.empty() || !seen_deps.insert(canonical_path).second)
            return;
        cache::DependencyRecord dep;
        dep.canonical_path = std::move(canonical_path);
        dep.import_key     = std::move(import_key);
        if (const auto entry = mCacheStore->manifestEntry(dep.canonical_path)) {
            dep.public_abi_hi = entry->public_abi_hi;
            dep.public_abi_lo = entry->public_abi_lo;
        }
        deps.push_back(std::move(dep));
    };
    for (const auto &edge : mSnapshot->importGraph()) {
        if (edge.importer != mCanonicalPath || edge.targetKind != session::ImportTargetKind::Zith) {
            continue;
        }
        for (const auto &target : edge.targets)
            append_dep(SourceCatalog::canonicalPath(target), target);
    }

    std::string module_name = fs::path(mFilePath).stem().string();
    auto artifact           = builder.build(mCanonicalPath, module_name, deps);
    mCacheStore->store(artifact);

    if (mOpts.get().flags.verbose())
        writeOutput("  [cache] wrote artifact for %s\n", mCanonicalPath.c_str());
}

void CompilationSession::hydrateFromArtifact(const cache::Artifact &art) {
    for (const auto &decl : art.decls) {
        mSyms.declare(decl.name, decl.visibility, decl.mod_depth,
                      static_cast<symbols::SymKind>(decl.kind), ast::kInvalidDecl, {}, {}, {});
    }
}

} // namespace zith::session
