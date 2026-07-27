#pragma once

#include "legacy-zith/ast/ast-builder.hpp"
#include "cli/options.hpp"
#include "cli/project-config.hpp"
#include "diagnostics/diagnostic-engine.hpp"
#include "hir/hir-module.hpp"
#include "legacy-zith/lexer/token.hpp"
#include "memory/source-map.hpp"
#include "memory/string-interner.hpp"
#include "legacy-zith/parser/scan-result.hpp"
#include "legacy-zith/sema/hir-lower.hpp"
#include "sema/hir-lower-modern.hpp"
#include "sema/sema-modern.hpp"
#include "sema/typed-ast.hpp"
#include "session/frontend-context.hpp"
#include "session/pipeline-plan.hpp"
#include "symbols/import-manager.hpp"
#include "symbols/symbol-table.hpp"
#include "types/type-intern.hpp"

#include "cache/cache.hpp"
#include <array>
#include <cstdarg>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace zith::session {

/// Per-stage index used for timing telemetry.
enum class StageIndex : uint8_t {
    Lex,
    Scan,
    Import,
    Resolve,
    Sema,
    Lower,
    Solve,
    Nra,
    Codegen,
    Cache,
    Count,
};

/// Snapshot of arena capacity and usage across the five compiler arenas.
struct ArenaMemoryUsage {
    size_t scratchAllocatedBytes = 0;
    size_t astAllocatedBytes     = 0;
    size_t symbolAllocatedBytes  = 0;
    size_t typeAllocatedBytes    = 0;
    size_t hirAllocatedBytes     = 0;
};

class CompilationSession {
    std::reference_wrapper<const Options> mOpts;
    std::string mFilePath;
    std::string mProjectRoot;
    std::string mObjectPath;
    int mChildExitCode = 0;
    ProjectConfig mProjectConfig;
    PipelinePlan mPlan;

    memory::SourceMap mSourceMap;
    memory::Arena mScratchArena;
    memory::Arena mAstArena;
    memory::Arena mSymArena;
    memory::Arena mTypeArena;
    memory::Arena mHirArena;
    diagnostics::DiagnosticEngine mDiags;

    std::unique_ptr<memory::StringInterner> mInterner;
    ast::AstBuilder mAstBuilder;
    symbols::ImportManager mImportMgr;
    symbols::SymbolTable mSyms;
    memory::DynArray<symbols::SymId> mResolvedSyms;
    types::TypeIntern mTypes;
    hir::HirModule mHirModule;
    sema::TypedAst mTypedAst;

    memory::FileId mFileId = 0;
    std::unique_ptr<sema::modern::SemaPipeline> mModernSemaPipeline;
    std::unique_ptr<sema::modern::TypeTable> mModernTypeTable;
    lexer::TokenStream mTokens{};
    ast::ProgramNode mProgram{mAstArena};
    parser::ScanResult mScanResult{mAstArena};

    std::string mOutputBuffer;
    bool mBufferedOutput   = false;
    bool mAlwaysEmitObject = false;
    std::string mContentOverride;
    std::shared_ptr<FrontendContext> mFrontendContext;
    std::shared_ptr<const CompilationSnapshot> mSnapshot;
    std::unique_ptr<cache::Store> mCacheStore;
    bool mCacheHydrated = false; // true when session state was restored from artifact
    std::string mCanonicalPath;
    session::ContentFingerprint mSourceFingerprint;

    bool tryLoadPersistentCache();
    void writePersistentCache();
    void hydrateFromArtifact(const cache::Artifact &art);
    void ensureFrontendContext();
    bool materializeFrontendSymbols();
    // Per-stage wall-clock durations in milliseconds; 0.0 = stage not executed.
    std::array<double, static_cast<size_t>(StageIndex::Count)> mStageDurations{};

#if defined(__GNUC__) || defined(__clang__)
    void writeOutput(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#else
    void writeOutput(const char *fmt, ...);
#endif

    const char *ansicolor(const char *code) const {
        return mDiags.useColor() ? code : "";
    }

public:
    /// `frontend_context` is optional so existing CLI/C API callers keep the
    /// session-local behavior.  LSP callers share one context per workspace.
    CompilationSession(const Options &opts, std::string filePath,
                       std::shared_ptr<FrontendContext> frontend_context = {});

    CompilationSession(CompilationSession &&)                 = default;
    CompilationSession &operator=(CompilationSession &&)      = delete;
    CompilationSession(const CompilationSession &)            = delete;
    CompilationSession &operator=(const CompilationSession &) = delete;

    bool run();
    bool runTo(Stage target);

    const diagnostics::DiagnosticEngine &diags() const {
        return mDiags;
    }
    diagnostics::DiagnosticEngine &diags() {
        return mDiags;
    }
    const std::string &filePath() const {
        return mFilePath;
    }
    bool hasErrors() const {
        return mDiags.hasErrors();
    }
    const ProjectConfig &projectConfig() const {
        return mProjectConfig;
    }
    const memory::SourceMap &sourceMap() const {
        return mSourceMap;
    }
    [[nodiscard]] const std::shared_ptr<const CompilationSnapshot> &snapshot() const noexcept {
        return mSnapshot;
    }
    memory::FileId fileId() const {
        return mFileId;
    }

    void setBuffered(bool b) {
        mBufferedOutput = b;
        mDiags.setSuppressEmit(b);
    }
    void setContent(std::string content) {
        mContentOverride = std::move(content);
    }
    void setAlwaysEmitObject(bool v) {
        mAlwaysEmitObject = v;
    }
    std::string flushOutput();
    void emitDiagnostics();
    bool linkAndExec();
    int childExitCode() const {
        return mChildExitCode;
    }

    const lexer::TokenStream &tokens() const {
        return mTokens;
    }
    const parser::ScanResult &scanResult() const {
        return mScanResult;
    }
    const symbols::SymbolTable &symbolTable() const {
        return mSyms;
    }
    symbols::SymbolTable &symbolTable() {
        return mSyms;
    }
    const memory::DynArray<symbols::SymId> &resolvedSyms() const {
        return mResolvedSyms;
    }
    const ast::AstBuilder &astBuilder() const {
        return mAstBuilder;
    }
    ast::AstBuilder &astBuilder() {
        return mAstBuilder;
    }
    const ast::ProgramNode &program() const {
        return mProgram;
    }
    const types::TypeIntern &types() const {
        return mTypes;
    }
    const sema::TypedAst &typedAst() const {
        return mTypedAst;
    }
    const hir::HirModule &hirModule() const {
        return mHirModule;
    }
    memory::StringInterner &interner() {
        return *mInterner;
    }

    std::string fmtStage();

    /// Return a map from stage-name -> milliseconds for each executed stage.
    /// Stages not yet executed return 0.0.
    std::unordered_map<std::string, double> getStageDurationsMs() const;

    /// Return current arena capacity across all five compiler arenas.
    ArenaMemoryUsage getArenaMemoryUsage() const;

private:
    void setTarget(Stage s) {
        mPlan.target = s;
    }
    bool lexStage();
    bool scanStage();
    bool importStage();
    bool resolveStage();
    bool semaStage();
    bool lowerStage();
    bool solveStage();
    bool nraStage();
    bool codegenStage();
    bool cacheStage();
};

} // namespace zith::session
