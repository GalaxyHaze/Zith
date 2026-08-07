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
#include "sema/nra-facts.hpp"
#include "sema/sema-modern.hpp"
#include "types/type-kind.hpp"
#include "types/type-lower.hpp"

#include "cache/artifact-builder.hpp"
#include "cache/cache-paths.hpp"
#include "zirl/zirl-reader.hpp"

#include "support/stdlib-discovery.hpp"

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
    case frontend::DeclKind::Macro:
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

[[maybe_unused]] bool isValidLibraryName(const std::string_view name) {
    if (name.empty())
        return false;
    return std::all_of(name.begin(), name.end(), [](const unsigned char character) {
        return std::isalnum(character) != 0 || character == '_' || character == '+' ||
               character == '-' || character == '.';
    });
}

[[maybe_unused]] int runProgram(const std::vector<std::string> &arguments) {
    if (arguments.empty())
        return -1;

    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1U);
    for (const auto &argument : arguments)
        argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);

#ifdef _WIN32
    const auto spawned = _spawnvp(_P_WAIT, argv.front(), argv.data());
    if (spawned == -1)
        return -1;
    return static_cast<int>(spawned);
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

[[maybe_unused]] int captureProgram(const std::vector<std::string> &arguments,
                                    std::string &output) {
#if defined(_WIN32) || defined(ZITH_IS_WASM)
    (void)arguments;
    (void)output;
    return -1;
#else
    if (arguments.empty())
        return -1;

    int pipefd[2] = {-1, -1};
    if (pipe(pipefd) != 0)
        return -1;

    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1U);
    for (const auto &argument : arguments)
        argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);

    const pid_t child = fork();
    if (child < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (child == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execvp(argv.front(), argv.data());
        _exit(127);
    }

    close(pipefd[1]);
    char buffer[4096];
    ssize_t bytes = 0;
    while ((bytes = read(pipefd[0], buffer, sizeof(buffer))) != 0) {
        if (bytes < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        output.append(buffer, static_cast<size_t>(bytes));
    }
    close(pipefd[0]);

    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return status;
#endif
}

// Runs a FILE*-based dump into an in-memory string so dump text can be routed
// through the session's own output band instead of the process stdout. The
// temporary file is used because the dump APIs still take a FILE*; it is
// removed as soon as the captured bytes have been copied out.
template <typename Fn> std::string captureStdioDump(Fn &&dump) {
    std::string result;

    const auto uniqueSuffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path tmpPath =
        std::filesystem::temp_directory_path() /
        ("zithc-dump-" + std::to_string(uniqueSuffix) + ".tmp");

    FILE *tmp = nullptr;
#ifdef _WIN32
    if (_wfopen_s(&tmp, tmpPath.c_str(), L"wb+") != 0)
        return result;
#else
    tmp = std::fopen(tmpPath.c_str(), "wb+");
    if (tmp == nullptr)
        return result;
#endif

    dump(tmp);
    std::fflush(tmp);
    std::rewind(tmp);
    char buffer[4096];
    size_t bytes = 0;
    while ((bytes = std::fread(buffer, 1, sizeof(buffer), tmp)) > 0)
        result.append(buffer, bytes);
    std::fclose(tmp);
    std::error_code ignored;
    std::filesystem::remove(tmpPath, ignored);
    return result;
}

[[maybe_unused]] std::string displayCommand(const std::vector<std::string> &arguments) {
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

    config.useSystemIncludeRoots = mOpts.get().systemIncludes;

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

    // Auto-discover stdlib relative to the compiler binary.
    for (auto &root : support::findStdlibRoots())
        config.stdlibRoots.push_back(std::move(root));

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
                decl.kind == frontend::DeclKind::Macro || decl.name.empty()) {
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
    if (!lowerStage())
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

    // --emit-tokens: dump tokens from the modern frontend snapshot. Dumps are
    // compiler output, so they go through writeOutput() and never touch the
    // program's stdout during `zithc run`.
    if (mOpts.get().flags.emitTokens()) {
        const auto &tokens = root->frontend->tokens();
        writeOutput("--- Tokens ---\n");
        const auto &source = root->frontend->source();
        for (size_t i = 0; i < tokens.size(); ++i) {
            const auto &tok = tokens[i];
            auto text =
                std::string_view(source).substr(tok.span.start, tok.span.end - tok.span.start);
            writeOutput("[%3zu] %-12s '%.*s'\n", i,
                        tok.kind == frontend::TokenKind::Keyword       ? "keyword"
                        : tok.kind == frontend::TokenKind::Identifier  ? "identifier"
                        : tok.kind == frontend::TokenKind::Literal     ? "literal"
                        : tok.kind == frontend::TokenKind::Operator    ? "operator"
                        : tok.kind == frontend::TokenKind::Punctuation ? "punctuation"
                        : tok.kind == frontend::TokenKind::End         ? "end"
                                                                       : "unknown",
                        static_cast<int>(text.size()), text.data());
        }
        writeOutput("---\n");
    }

    // --emit-ast: dump AST from the modern frontend snapshot
    if (mOpts.get().flags.emitAst()) {
        writeOutput("--- AST ---\n");
        for (const auto &decl : root->frontend->declarations()) {
            writeOutput("decl %s (kind=%d, vis=%d, span=%u..%u)\n", decl.name.c_str(),
                        static_cast<int>(decl.kind), static_cast<int>(decl.visibility),
                        decl.span.start, decl.span.end);
        }
        writeOutput("--- Symbols ---\n");
        writeOutput("%s",
                    captureStdioDump([this](FILE *out) { mSyms.dump(out, nullptr); }).c_str());
        writeOutput("---\n");
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
                                       *mInterner, mNraFacts.get());
    if (!lower.run()) {
        mDiags.emit();
        return false;
    }

    mHirModule = lower.takeHir();
    // Keep the semantic pipeline alive through HIR lowering. The solver still
    // runs after lowering until generic instantiation moves onto sema/HIR.

    if (mOpts.get().flags.emitHir()) {
        const std::string hir_text =
            captureStdioDump([this](FILE *out) { mHirModule.dump(out, *mInterner); });
        writeOutput("--- HIR ---\n%s---\n", hir_text.c_str());
    }

    comptime::Solver solver(mTypes, nullptr, nullptr, mSyms, mDiags, mHirArena,
                            mModernTypeTable.get());
    if (!solver.solve(mHirModule)) {
        mDiags.emit();
        return false;
    }
    mModernTypeTable.reset();

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
    // The semantic solver is intentionally conservative while generic
    // instantiation still consumes HIR. The documented boundary is
    // `sema -> comptime/solve -> NTA/NRA -> HIR`; the residual ownership
    // facts are computed here and the current HIR-based solver remains a
    // post-lowering compatibility pass below.
    mModernTypeTable =
        std::make_unique<sema::modern::TypeTable>(sema::modern::TypeTable(mHirArena));
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
    if (mDiags.hasErrors()) {
        mDiags.emit();
        return false;
    }

    sema::modern::NraFacts nra(mHirArena, mDiags, *mSnapshot, *mModernSemaPipeline);
    if (!nra.run()) {
        mDiags.emit();
        return false;
    }
    mNraFacts = std::make_unique<sema::modern::NraFacts>(std::move(nra));

    auto nraDt =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (mModernTypeTable && mOpts.get().flags.verbose()) {
        writeOutput("  [nra] modern types: %zu  (%5.1fms)\n", mModernTypeTable->size(), nraDt);
    }
    mStageDurations[static_cast<size_t>(StageIndex::Nra)] = nraDt;
    if (mOpts.get().flags.verbose()) {
        writeOutput("  [nra] residual facts: %zu locals, %zu calls  (%5.1fms)\n",
                    mNraFacts->localCount(), mNraFacts->callCount(), nraDt);
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

        // `emit` may have reported an IR verification failure. Stop before any consumer
        // hands the module to a TargetMachine, which crashes on invalid IR.
        if (cg.hasInvalidIR() || mDiags.hasErrors()) {
            mDiags.emit();
            return false;
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
            // Object always goes to cache/; -o controls the final executable
            // path, not the object file.
            namespace fs       = std::filesystem;
            std::string objDir = (fs::path(mProjectRoot) / "cache").string();
            fs::create_directories(objDir);
            std::string objPath = objDir + "/" + fs::path(mFilePath).filename().string() + ".o";
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

bool CompilationSession::performLink(std::string &exePath, bool &isWasm) {
#ifndef ZITH_IS_WASM
    isWasm = false;
    if (mObjectPath.empty())
        return false;

    namespace fs = std::filesystem;
    if (!mOpts.get().outputFile.empty()) {
        exePath = mOpts.get().outputFile;
        fs::create_directories(fs::path(exePath).parent_path());
    } else {
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
    isWasm =
        triple.find("wasm32") != std::string::npos || triple.find("wasm64") != std::string::npos;

    if (isWasm)
        exePath += ".wasm";
#ifdef _WIN32
    else if (mOpts.get().outputFile.empty())
        exePath += ".exe";
#endif

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

    mExecutablePath = exePath;
    return true;
#else
    (void)exePath;
    (void)isWasm;
    (void)mObjectPath;
    writeOutput("%s[error]%s cannot link on WASM target\n", ansicolor("\033[31m"),
                ansicolor("\033[0m"));
    return false;
#endif
}

bool CompilationSession::link() {
    std::string exePath;
    bool isWasm = false;
    return performLink(exePath, isWasm);
}

namespace {
// Normalizes a waitpid()/spawn status into a process exit code.
[[maybe_unused]] int normalizeExitStatus(const int status) {
#ifdef _WIN32
    return status;
#else
    int waitStatus = status;
    if (WIFEXITED(waitStatus))
        return WEXITSTATUS(waitStatus);
    if (WIFSIGNALED(waitStatus))
        return 128 + WTERMSIG(waitStatus);
    return 1;
#endif
}
} // namespace

bool CompilationSession::execAfterLink(const bool capture) {
#ifndef ZITH_IS_WASM
    std::string exePath;
    bool isWasm = false;
    if (!performLink(exePath, isWasm))
        return false;

    if (mOpts.get().flags.verbose())
        writeOutput("  [exec] %s\n", exePath.c_str());

    if (isWasm) {
        if (mOpts.get().flags.verbose())
            writeOutput("  [exec] skipped (cannot natively execute WASM)\n");
        mChildExitCode = 0;
        return true;
    }

    // Avoid duplicating any pending parent stdio buffers into the forked child.
    std::fflush(nullptr);
    const int execResult =
        capture ? captureProgram({exePath}, mChildOutput) : runProgram({exePath});
    if (execResult == -1) {
        writeOutput("%s[error]%s failed to launch executable\n", ansicolor("\033[31m"),
                    ansicolor("\033[0m"));
        return false;
    }

    mChildExitCode = normalizeExitStatus(execResult);
    return true;
#else
    (void)capture;
    writeOutput("%s[error]%s cannot execute on WASM target\n", ansicolor("\033[31m"),
                ansicolor("\033[0m"));
    return false;
#endif
}

bool CompilationSession::linkAndExec() {
    return execAfterLink(/*capture=*/true);
}

bool CompilationSession::linkAndExecDirect() {
    return execAfterLink(/*capture=*/false);
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

std::string CompilationSession::takeChildOutput() {
    auto result = std::move(mChildOutput);
    mChildOutput.clear();
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

    mHydratedEntry = mCacheStore->loadEntry(mCanonicalPath, mSourceFingerprint);
    if (!mHydratedEntry) {
        if (mOpts.get().flags.verbose())
            writeOutput("  [cache] miss for %s\n", mCanonicalPath.c_str());
        return false;
    }

    hydrateFromArtifact(mHydratedEntry->artifact);
    mCacheHydrated = true;

    if (mOpts.get().flags.verbose())
        writeOutput("  [cache] hit for %s (%zu decls, %zu fns)\n", mCanonicalPath.c_str(),
                    mHydratedEntry->artifact.decls.size(),
                    mHydratedEntry->artifact.functions.size());

    return true;
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
    // Recreate the exported surface first so downstream lookup (including
    // methods that point at Fn declarations) sees stable symbol ids.
    std::vector<symbols::SymId> decl_sym_ids;
    decl_sym_ids.reserve(art.decls.size());
    for (const auto &decl : art.decls) {
        decl_sym_ids.push_back(mSyms.declare(decl.name, decl.visibility, decl.mod_depth,
                                             static_cast<symbols::SymKind>(decl.kind),
                                             ast::kInvalidDecl, {}, {}, {}));
    }

    for (size_t di = 0; di < art.decls.size(); ++di) {
        const auto &decl = art.decls[di];
        if (decl.kind != cache::CompactSymKind::Struct &&
            decl.kind != cache::CompactSymKind::Union &&
            decl.kind != cache::CompactSymKind::Component)
            continue;
        auto &owner = mSyms.get(decl_sym_ids[di]);
        for (const auto method_index : decl.method_decl_indices) {
            if (method_index < decl_sym_ids.size())
                owner.members.push(decl_sym_ids[method_index]);
        }
    }

    // Compact types are emitted in dependency order: refs always have a lower
    // compact id, so a single forward pass can restore the type table.
    std::vector<types::TypeId> compact_type_ids(art.types.size(), types::kErrorType);
    std::vector<types::TypeId> struct_tids(art.struct_defs.size(), types::kErrorType);
    std::vector<types::TypeId> enum_tids(art.enum_defs.size(), types::kErrorType);
    std::vector<types::TypeId> union_tids(art.union_defs.size(), types::kErrorType);

    auto compactType = [&](uint32_t id) -> types::TypeId {
        return id < compact_type_ids.size() ? compact_type_ids[id] : types::kErrorType;
    };

    // Restore composite definitions by compact def id before resolving the
    // compact type table, so private/internal types referenced by HIR are
    // recreated with the same names, fields, variants, and discriminants.
    for (size_t si = 0; si < art.struct_defs.size(); ++si)
        struct_tids[si] = mTypes.defineStruct(art.struct_defs[si].name);
    for (size_t ei = 0; ei < art.enum_defs.size(); ++ei)
        enum_tids[ei] = mTypes.defineEnum(art.enum_defs[ei].name, types::kErrorType);
    for (size_t ui = 0; ui < art.union_defs.size(); ++ui)
        union_tids[ui] = mTypes.defineUnion(art.union_defs[ui].name, art.union_defs[ui].is_raw);

    for (size_t i = 0; i < art.types.size(); ++i) {
        const auto &ct = art.types[i];
        switch (ct.kind) {
        case cache::CompactTypeKind::Error:
            compact_type_ids[i] = types::kErrorType;
            break;
        case cache::CompactTypeKind::Never:
            compact_type_ids[i] = types::kNeverType;
            break;
        case cache::CompactTypeKind::Void:
            compact_type_ids[i] = types::kVoidType;
            break;
        case cache::CompactTypeKind::Bool:
            compact_type_ids[i] = types::kBoolType;
            break;
        case cache::CompactTypeKind::Char:
            compact_type_ids[i] = types::kCharType;
            break;
        case cache::CompactTypeKind::Int:
            compact_type_ids[i] = mTypes.internInt(static_cast<types::IntWidth>(ct.int_width));
            break;
        case cache::CompactTypeKind::Float:
            compact_type_ids[i] = mTypes.internFloat(static_cast<types::FloatWidth>(ct.int_width));
            break;
        case cache::CompactTypeKind::Ptr:
            compact_type_ids[i] = mTypes.internPtr(compactType(ct.ref0), (ct.flags & 1U) != 0);
            break;
        case cache::CompactTypeKind::Array:
            compact_type_ids[i] = mTypes.internArray(compactType(ct.ref0), ct.ref1);
            break;
        case cache::CompactTypeKind::Struct: {
            compact_type_ids[i] =
                ct.ref0 < struct_tids.size() ? struct_tids[ct.ref0] : types::kErrorType;
            break;
        }
        case cache::CompactTypeKind::Fn: {
            std::vector<types::TypeId> params;
            params.reserve(ct.args.size());
            for (auto id : ct.args)
                params.push_back(compactType(id));
            compact_type_ids[i] = mTypes.internFn(params, compactType(ct.ref0));
            break;
        }
        case cache::CompactTypeKind::Optional:
            compact_type_ids[i] = mTypes.internOptional(compactType(ct.ref0));
            break;
        case cache::CompactTypeKind::Failable:
            compact_type_ids[i] = mTypes.internFailable(compactType(ct.ref0));
            break;
        case cache::CompactTypeKind::Slice:
            compact_type_ids[i] = mTypes.internSlice(compactType(ct.ref0));
            break;
        case cache::CompactTypeKind::Enum: {
            compact_type_ids[i] =
                ct.ref0 < enum_tids.size() ? enum_tids[ct.ref0] : types::kErrorType;
            break;
        }
        case cache::CompactTypeKind::Union: {
            compact_type_ids[i] =
                ct.ref0 < union_tids.size() ? union_tids[ct.ref0] : types::kErrorType;
            break;
        }
        case cache::CompactTypeKind::TypeVar:
            compact_type_ids[i] = mTypes.internTypeVar();
            break;
        case cache::CompactTypeKind::GenericParam:
            compact_type_ids[i] = mTypes.internGenericParam(ct.ref0, ct.ref1);
            break;
        case cache::CompactTypeKind::Incomplete: {
            std::vector<types::TypeId> args;
            args.reserve(ct.args.size());
            for (auto id : ct.args)
                args.push_back(compactType(id));
            compact_type_ids[i] = mTypes.internIncomplete(compactType(ct.ref0), args);
            break;
        }
        case cache::CompactTypeKind::Opaque:
            compact_type_ids[i] = mTypes.internUnknown();
            break;
        }
    }

    for (size_t si = 0; si < art.struct_defs.size(); ++si) {
        const auto &s  = art.struct_defs[si];
        const auto tid = struct_tids[si];
        for (size_t fi = 0; fi < s.field_name_ids.size() && fi < s.field_type_ids.size(); ++fi) {
            const auto &name = art.strings[s.field_name_ids[fi]];
            mTypes.addField(tid, name, compactType(s.field_type_ids[fi]));
        }
    }
    for (size_t ei = 0; ei < art.enum_defs.size(); ++ei) {
        const auto &e  = art.enum_defs[ei];
        const auto tid = enum_tids[ei];
        if (e.underlying_id != ~uint32_t{0})
            mTypes.setEnumUnderlying(tid, compactType(e.underlying_id));
        for (const auto &v : e.variants)
            mTypes.addEnumVariant(tid, v.name, v.discriminant);
    }
    for (size_t ui = 0; ui < art.union_defs.size(); ++ui) {
        const auto &u  = art.union_defs[ui];
        const auto tid = union_tids[ui];
        for (auto member_type_id : u.member_type_ids)
            mTypes.addUnionMember(tid, compactType(member_type_id));
    }

    // Rebuild the module-level expression pool from the first function's
    // serialized table. Function blocks reference these global HirExprIds.
    for (const auto &ce :
         art.functions.empty() ? std::vector<cache::CompactExpr>{} : art.functions.front().exprs) {
        hir::HirExpr expr;
        switch (ce.kind) {
        case cache::CompactExprKind::Literal: {
            hir::HirLiteral lit;
            lit.type = compactType(ce.type_id);
            if (ce.flags == 1) {
                lit.f = ce.flt_val;
            } else if (ce.flags == 2) {
                lit.b = ce.int_val != 0;
            } else if (ce.flags == 3) {
                const auto text = art.strings[ce.name_id];
                lit.str_val     = mInterner->intern(text);
            } else {
                lit.i = ce.int_val;
            }
            expr = lit;
            break;
        }
        case cache::CompactExprKind::Binary: {
            hir::HirBinary bin;
            bin.lhs          = ce.ref_a;
            bin.rhs          = ce.ref_b;
            bin.op           = static_cast<hir::HirBinaryOp>(ce.op);
            bin.type         = compactType(ce.type_id);
            bin.operand_type = compactType(ce.ref_e);
            expr             = bin;
            break;
        }
        case cache::CompactExprKind::Unary: {
            hir::HirUnary un;
            un.op      = static_cast<hir::HirUnaryOp>(ce.op);
            un.operand = ce.ref_a;
            un.type    = compactType(ce.type_id);
            expr       = un;
            break;
        }
        case cache::CompactExprKind::Let: {
            hir::HirLet let;
            let.name = mInterner->intern(art.strings[ce.name_id]);
            let.type = compactType(ce.type_id);
            let.init = ce.ref_a;
            expr     = let;
            break;
        }
        case cache::CompactExprKind::Var: {
            hir::HirVar var;
            var.name    = mInterner->intern(art.strings[ce.name_id]);
            var.version = ce.ref_c;
            expr        = var;
            break;
        }
        case cache::CompactExprKind::Call: {
            memory::DynArray<hir::HirExprId> args(mHirArena);
            for (auto id : ce.args)
                args.push(id);
            memory::DynArray<types::TypeId> arg_types(mHirArena);
            for (auto id : ce.arg_types)
                arg_types.push(compactType(id));
            hir::HirCall call(ce.ref_a, std::move(args), std::move(arg_types));
            call.resolved_fn = ce.ref_b;
            expr             = std::move(call);
            break;
        }
        case cache::CompactExprKind::Ret: {
            hir::HirRet ret;
            ret.value = ce.ref_a;
            expr      = ret;
            break;
        }
        case cache::CompactExprKind::Branch: {
            hir::HirBranch branch;
            branch.cond       = ce.ref_a;
            branch.then_block = ce.ref_c;
            branch.else_block = ce.ref_d;
            expr              = branch;
            break;
        }
        case cache::CompactExprKind::Jump: {
            hir::HirJump jump;
            jump.target = ce.ref_c;
            expr        = jump;
            break;
        }
        case cache::CompactExprKind::Phi: {
            memory::DynArray<hir::HirExprId> incoming(mHirArena);
            for (auto id : ce.args)
                incoming.push(id);
            hir::HirPhi phi(mHirArena);
            phi.incoming = std::move(incoming);
            expr         = std::move(phi);
            break;
        }
        case cache::CompactExprKind::Assign: {
            hir::HirAssign assign;
            assign.target = ce.ref_a;
            assign.value  = ce.ref_b;
            expr          = assign;
            break;
        }
        case cache::CompactExprKind::Index: {
            hir::HirIndex idx;
            idx.object   = ce.ref_a;
            idx.index    = ce.ref_b;
            idx.type     = compactType(ce.type_id);
            idx.obj_type = compactType(ce.ref_e);
            idx.is_array = (ce.flags & 1U) != 0;
            expr         = idx;
            break;
        }
        case cache::CompactExprKind::Field: {
            hir::HirField field;
            field.object      = ce.ref_a;
            field.index       = ce.ref_c;
            field.type        = compactType(ce.type_id);
            field.object_type = compactType(ce.ref_e);
            expr              = field;
            break;
        }
        case cache::CompactExprKind::StructLiteral: {
            memory::DynArray<hir::HirExprId> values(mHirArena);
            for (auto id : ce.args)
                values.push(id);
            hir::HirStructLiteral lit(mHirArena);
            lit.values = std::move(values);
            lit.type   = compactType(ce.type_id);
            expr       = std::move(lit);
            break;
        }
        case cache::CompactExprKind::ArrayLiteral: {
            memory::DynArray<hir::HirExprId> elements(mHirArena);
            for (auto id : ce.args)
                elements.push(id);
            hir::HirArrayLiteral lit(mHirArena);
            lit.elements = std::move(elements);
            lit.type     = compactType(ce.type_id);
            expr         = std::move(lit);
            break;
        }
        case cache::CompactExprKind::EnumValue: {
            hir::HirEnumValue ev;
            ev.value = ce.int_val;
            ev.type  = compactType(ce.type_id);
            expr     = ev;
            break;
        }
        case cache::CompactExprKind::SlotAlloca: {
            hir::HirSlotAlloca sa;
            sa.slot = ce.ref_a;
            sa.type = compactType(ce.type_id);
            expr    = sa;
            break;
        }
        case cache::CompactExprKind::SlotStore: {
            hir::HirSlotStore ss;
            ss.slot  = ce.ref_a;
            ss.value = ce.ref_b;
            expr     = ss;
            break;
        }
        case cache::CompactExprKind::SlotLoad: {
            hir::HirSlotLoad sl;
            sl.slot = ce.ref_a;
            sl.type = compactType(ce.type_id);
            expr    = sl;
            break;
        }
        case cache::CompactExprKind::SlotAddr: {
            hir::HirSlotAddr sa;
            sa.slot = ce.ref_a;
            sa.type = compactType(ce.type_id);
            expr    = sa;
            break;
        }
        case cache::CompactExprKind::MakeNone: {
            hir::HirMakeNone mn;
            mn.type = compactType(ce.type_id);
            expr    = mn;
            break;
        }
        case cache::CompactExprKind::MakeSome: {
            hir::HirMakeSome ms;
            ms.value = ce.ref_a;
            ms.type  = compactType(ce.type_id);
            expr     = ms;
            break;
        }
        case cache::CompactExprKind::Cast: {
            hir::HirCast cast;
            cast.value = ce.ref_a;
            cast.from  = compactType(ce.ref_e);
            cast.to    = compactType(ce.ref_b);
            expr       = cast;
            break;
        }
        case cache::CompactExprKind::LayoutIntrinsic: {
            hir::HirLayoutIntrinsic li;
            li.which       = static_cast<hir::HirLayoutIntrinsic::Which>(ce.ref_e);
            li.type        = compactType(ce.type_id);
            li.field_index = ce.ref_f;
            expr           = li;
            break;
        }
        }
        mHirModule.addExpr(std::move(expr));
    }

    for (const auto &cfn : art.functions) {
        auto &fn       = mHirModule.addFn(mInterner->intern(cfn.name));
        fn.return_type = compactType(cfn.return_type_id);
        fn.isVariadic  = cfn.is_variadic;
        for (size_t pi = 0; pi < cfn.param_type_ids.size() && pi < cfn.param_name_ids.size();
             ++pi) {
            fn.params.push(compactType(cfn.param_type_ids[pi]));
            fn.param_names.push(mInterner->intern(art.strings[cfn.param_name_ids[pi]]));
        }
        for (const auto &cblk : cfn.blocks) {
            auto &blk = fn.blocks.emplace(mHirArena);
            for (auto id : cblk.insts)
                blk.insts.push(id);
            blk.terminator = cblk.terminator;
        }
    }

    for (const auto &slot : art.attrs_slots) {
        auto &attrs     = mHirModule.attrs().slot(static_cast<hir::HirSlotId>(slot.slot));
        attrs.ownership = static_cast<hir::HirOwnership>(slot.ownership);
        attrs.consumed  = static_cast<hir::HirConsumedState>(slot.consumed);
        attrs.nonNull   = slot.nonNull;
    }
    for (const auto &call : art.attrs_calls) {
        auto &attrs      = mHirModule.attrs().call(call.expr_id);
        attrs.returnsArg = call.returns_arg;
        for (auto escape : call.arg_escapes)
            attrs.args.emplace(hir::HirCallArgAttr{static_cast<hir::HirCallEscape>(escape)});
    }
    for (const auto &fn_attrs : art.attrs_fns) {
        auto &attrs          = mHirModule.attrs().fn(fn_attrs.fn_index);
        attrs.returnConsumed = static_cast<hir::HirConsumedState>(fn_attrs.return_consumed);
        attrs.nonNull        = fn_attrs.nonNull;
        attrs.noAlias        = fn_attrs.noAlias;
        attrs.readOnly       = fn_attrs.readOnly;
        attrs.noCapture      = fn_attrs.noCapture;
    }
}

} // namespace zith::session
