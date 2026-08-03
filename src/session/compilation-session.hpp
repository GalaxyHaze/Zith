#pragma once

#include "cli/options.hpp"
#include "cli/project-config.hpp"
#include "diagnostics/diagnostic-engine.hpp"
#include "hir/hir-module.hpp"
#include "memory/source-map.hpp"
#include "memory/string-interner.hpp"
#include "sema/hir-lower-modern.hpp"
#include "sema/sema-modern.hpp"
#include "session/frontend-context.hpp"
#include "session/pipeline-plan.hpp"
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

/// Snapshot of arena capacity and usage across the four compiler arenas.
struct ArenaMemoryUsage {
    size_t scratchAllocatedBytes = 0;
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
    memory::Arena mSymArena;
    memory::Arena mTypeArena;
    memory::Arena mHirArena;
    diagnostics::DiagnosticEngine mDiags;

    std::unique_ptr<memory::StringInterner> mInterner;
    symbols::SymbolTable mSyms;
    types::TypeIntern mTypes;
    hir::HirModule mHirModule;

    memory::FileId mFileId = 0;
    std::unique_ptr<sema::modern::SemaPipeline> mModernSemaPipeline;
    std::unique_ptr<sema::modern::TypeTable> mModernTypeTable;

    std::string mOutputBuffer;
    bool mBufferedOutput   = false;
    bool mAlwaysEmitObject = false;
    std::string mContentOverride;
    std::shared_ptr<FrontendContext> mFrontendContext;
    std::shared_ptr<const CompilationSnapshot> mSnapshot;
    std::unique_ptr<cache::Store> mCacheStore;
    bool mCacheHydrated = false;
    std::string mCanonicalPath;
    session::ContentFingerprint mSourceFingerprint;

    bool tryLoadPersistentCache();
    void writePersistentCache();
    void hydrateFromArtifact(const cache::Artifact &art);
    void ensureFrontendContext();
    bool materializeFrontendSymbols();
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

    const symbols::SymbolTable &symbolTable() const {
        return mSyms;
    }
    symbols::SymbolTable &symbolTable() {
        return mSyms;
    }
    const types::TypeIntern &types() const {
        return mTypes;
    }
    const hir::HirModule &hirModule() const {
        return mHirModule;
    }
    memory::StringInterner &interner() {
        return *mInterner;
    }

    std::string fmtStage();

    std::unordered_map<std::string, double> getStageDurationsMs() const;
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
    void forwardSnapshotDiagnostics();
    bool mSnapshotDiagsForwarded = false;
};

} // namespace zith::session
