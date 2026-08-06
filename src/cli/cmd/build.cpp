#include "cache/cache.hpp"
#include "cli/commands.hpp"
#include "cli/terminal.hpp"
#include "session/compilation-session.hpp"
#include "session/pipeline-plan.hpp"

#include <cstdio>
#include <future>
#include <string>
#include <vector>

namespace zith::cli::commands {

namespace {

struct Printers {
    term::UsagePrinter out;
    term::UsagePrinter err;
};

Printers initPrinters(const Options &opts) {
    auto TERM = term::init(opts);
    return {{stdout, TERM.coutOn}, {stderr, TERM.cerrOn}};
}

void printCheckResults(const Printers &p, const std::vector<std::string> &files,
                       const std::vector<bool> &results, bool verbose) {
    size_t passed  = countPassed(results);
    bool allPassed = (passed == files.size());

    if (verbose) {
        if (files.size() == 1) {
            results[0] ? p.out.green("[ok]") : p.out.red("[error]");
            std::printf(" %s\n", results[0] ? "check passed" : "check failed");
        } else {
            std::string list;
            for (size_t i = 0; i < files.size(); i++) {
                list += "\n\t";
                list += files[i];
                list += results[i] ? ": passed" : ": failed";
            }
            allPassed ? p.out.green("[ok]") : p.out.red("[error]");
            std::printf(" %zu/%zu passed%s\n", passed, files.size(), list.c_str());
        }
    } else {
        if (files.size() == 1) {
            results[0] ? p.out.green("[ok]") : p.out.red("[error]");
            std::printf(" %s\n", results[0] ? "check passed" : "check failed");
        } else {
            allPassed ? p.out.green("[ok]") : p.out.red("[error]");
            std::printf(" %zu/%zu passed\n", passed, files.size());
        }
    }
}

void printBuildResults(const Printers &p, const std::vector<std::string> &files,
                       const std::vector<bool> &results,
                       const std::vector<std::string> &linkedPaths) {
    for (size_t i = 0; i < files.size(); i++) {
        results[i] ? p.out.green("[ok]") : p.out.red("[error]");
        if (results[i]) {
            std::printf(" created %s", files[i].c_str());
            if (!linkedPaths[i].empty())
                std::printf(" -> %s", linkedPaths[i].c_str());
            std::printf("\n");
        } else {
            std::printf(" build failed for %s\n", files[i].c_str());
        }
    }
}

// True when `build` should link an executable: compilation reaches codegen and
// no non-binary --emit target was requested.
bool shouldLinkExecutable(const Options &opts) {
    if (opts.flags.emitIr() || opts.flags.emitAsm() || opts.flags.emitHir())
        return false;
    switch (opts.emitTarget) {
    case Options::EmitTarget::Ast:
    case Options::EmitTarget::Hir:
    case Options::EmitTarget::Ir:
    case Options::EmitTarget::Asm:
    case Options::EmitTarget::Obj:
        return false;
    default:
        return true;
    }
}

// Compiles one file and, when requested, links the resulting object into an
// executable without running it. Returns the linked executable path in
// outLinkedPath when a link was performed.
bool buildOneFile(const Options &opts, const std::string &file, session::Stage stage,
                  cache::StoreMetrics *outMetrics, std::string *outLinkedPath) {
    const bool doLink = shouldLinkExecutable(opts);

    session::CompilationSession session(opts, file);
    session.setBuffered(true);
    if (doLink)
        session.setAlwaysEmitObject(true);
    bool ok = (stage == session::Stage::Cached) ? session.run() : session.runTo(stage);
    session.emitDiagnostics();

    std::string linkedPath;
    if (ok && doLink && !session.hasErrors()) {
        ok = session.link();
        if (ok)
            linkedPath = session.executablePath();
    }

    std::fputs(session.flushOutput().c_str(), stderr);
    if (!linkedPath.empty() && opts.flags.verbose())
        std::fprintf(stderr, "  [build] %s\n", linkedPath.c_str());
    if (outLinkedPath != nullptr)
        *outLinkedPath = linkedPath;
    if (outMetrics != nullptr)
        *outMetrics = session.cacheMetrics();
    return session.hasErrors() ? false : ok;
}

} // namespace

// ── Shared helpers (non-anonymous, declared in commands.hpp) ───────────────

static bool processFile(const Options &opts, const std::string &file, session::Stage stage) {
    session::CompilationSession session(opts, file);
    session.setBuffered(true);
    bool ok = (stage == session::Stage::Cached) ? session.run() : session.runTo(stage);
    session.emitDiagnostics();
    std::fputs(session.flushOutput().c_str(), stderr);
    return session.hasErrors() ? false : ok;
}

std::vector<std::string> collectFiles(const Options &opts) {
    std::vector<std::string> files;
    if (!opts.inputFiles.empty()) {
        files.reserve(opts.inputFiles.size());
        for (const auto &f : opts.inputFiles)
            files.push_back(f);
    }
    return files;
}

std::vector<bool> runOnFiles(const Options &opts, const std::vector<std::string> &files,
                             session::Stage stage) {
    std::vector<bool> results(files.size());

    if (files.size() == 1) {
        results[0] = processFile(opts, files[0], stage);
    } else {
        std::vector<std::future<bool>> futures;
        futures.reserve(files.size());
        for (const auto &file : files)
            futures.push_back(std::async(std::launch::async, [&opts, &file, stage]() {
                return processFile(opts, file, stage);
            }));
        for (size_t i = 0; i < futures.size(); i++)
            results[i] = futures[i].get();
    }

    return results;
}

size_t countPassed(const std::vector<bool> &results) {
    size_t c = 0;
    for (bool v : results)
        c += v;
    return c;
}

// ── check ──────────────────────────────────────────────────────────────────

int check(const Options &opts) {
    auto p     = initPrinters(opts);
    auto files = collectFiles(opts);
    if (files.empty()) {
        p.err.red("[error]");
        std::fprintf(stderr, " no input files and no ZithProject.toml found\n");
        return 1;
    }
    auto stage = opts.targetStage;
    if (stage == session::Stage::Cached)
        stage = session::Stage::TypeChecked;
    auto results = runOnFiles(opts, files, stage);
    printCheckResults(p, files, results, opts.flags.verbose());
    return countPassed(results) == files.size() ? 0 : 1;
}

// ── build ──────────────────────────────────────────────────────────────────

int build(const Options &opts) {
    auto p     = initPrinters(opts);
    auto files = collectFiles(opts);
    if (files.empty()) {
        p.err.red("[error]");
        std::fprintf(stderr, " no input files and no ZithProject.toml found\n");
        return 1;
    }

    std::vector<bool> results(files.size());
    std::vector<std::string> linkedPaths(files.size());
    cache::StoreMetrics totalMetrics;

    if (opts.flags.cacheStats() && files.size() == 1) {
        results[0] = buildOneFile(opts, files[0], opts.targetStage, &totalMetrics, &linkedPaths[0]);
    } else if (opts.flags.cacheStats()) {
        std::vector<cache::StoreMetrics> perFileMetrics(files.size());
        std::vector<std::future<bool>> futures;
        futures.reserve(files.size());
        for (size_t i = 0; i < files.size(); i++) {
            futures.push_back(
                std::async(std::launch::async, [&opts, &files, &perFileMetrics, &linkedPaths, i,
                                                stage = opts.targetStage]() {
                    return buildOneFile(opts, files[i], stage, &perFileMetrics[i], &linkedPaths[i]);
                }));
        }
        for (size_t i = 0; i < futures.size(); i++) {
            results[i] = futures[i].get();
            totalMetrics.hits += perFileMetrics[i].hits;
            totalMetrics.misses += perFileMetrics[i].misses;
            totalMetrics.invalid += perFileMetrics[i].invalid;
            totalMetrics.writes += perFileMetrics[i].writes;
            totalMetrics.evictions += perFileMetrics[i].evictions;
        }
    } else if (files.size() == 1) {
        results[0] = buildOneFile(opts, files[0], opts.targetStage, nullptr, &linkedPaths[0]);
    } else {
        std::vector<std::future<bool>> futures;
        futures.reserve(files.size());
        for (size_t i = 0; i < files.size(); i++)
            futures.push_back(std::async(
                std::launch::async, [&opts, &files, &linkedPaths, i, stage = opts.targetStage]() {
                    return buildOneFile(opts, files[i], stage, nullptr, &linkedPaths[i]);
                }));
        for (size_t i = 0; i < futures.size(); i++)
            results[i] = futures[i].get();
    }

    printBuildResults(p, files, results, linkedPaths);

    if (opts.flags.cacheStats()) {
        p.out.green("[cache]");
        std::printf(" hits=%zu misses=%zu writes=%zu invalid=%zu evictions=%zu\n",
                    totalMetrics.hits, totalMetrics.misses, totalMetrics.writes,
                    totalMetrics.invalid, totalMetrics.evictions);
    }

    return countPassed(results) == files.size() ? 0 : 1;
}

} // namespace zith::cli::commands
