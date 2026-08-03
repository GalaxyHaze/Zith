#include "cache/cache-paths.hpp"
#include "cli/commands.hpp"
#include "cli/terminal.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
#include <toml++/toml.hpp>

namespace zith::cli::commands {

namespace {

int stubCommand(const Options &opts, const char *name) {
    auto TERM = term::init(opts);
    term::UsagePrinter err{stderr, TERM.cerrOn};
    err.yellow("[soon]");
    std::fprintf(stderr, " %s not implemented yet\n", name);
    return 1;
}

std::string findBuildDir(const Options &opts) {
    namespace fs = std::filesystem;
    std::string projectRoot;

    // If a file/dir was specified, use its parent
    if (!opts.subcommandStr.empty()) {
        auto path = fs::weakly_canonical(fs::path(opts.subcommandStr));
        if (fs::is_directory(path))
            projectRoot = path.string();
        else
            projectRoot = path.parent_path().string();
    } else {
        // Default to current directory
        projectRoot = fs::current_path().string();
    }

    // Check for ZithProject.toml to get binDir
    auto toml_path = fs::path(projectRoot) / "ZithProject.toml";
    if (fs::exists(toml_path)) {
        auto tbl = toml::parse_file(toml_path.string());
        if (auto *paths = tbl["paths"].as_table()) {
            if (auto v = paths->get("bin_dir"))
                if (auto s = v->value<std::string>())
                    return (fs::path(projectRoot) / *s).string();
        }
    }

    return (fs::path(projectRoot) / "build").string();
}

} // namespace

int test(const Options &opts) {
    return stubCommand(opts, "test");
}

int docs(const Options &opts) {
    return stubCommand(opts, "docs");
}

int repl(const Options &opts) {
    return stubCommand(opts, "repl");
}

int clean(const Options &opts) {
    auto TERM = term::init(opts);
    term::UsagePrinter out{stdout, TERM.coutOn};
    term::UsagePrinter err{stderr, TERM.cerrOn};

    namespace fs = std::filesystem;

    // Determine project root from subcommand arg or current dir
    std::string projectRoot;
    if (!opts.subcommandStr.empty()) {
        auto path   = fs::weakly_canonical(fs::path(opts.subcommandStr));
        projectRoot = fs::is_directory(path) ? path.string() : path.parent_path().string();
    } else {
        projectRoot = fs::current_path().string();
    }

    // Compute build dir and cache dir
    std::string buildDir = findBuildDir(opts);
    auto cacheDir        = fs::path(projectRoot) / cache::kPersistentCacheDirName;

    bool cleaned = false;

    if (fs::exists(buildDir)) {
        fs::remove_all(buildDir);
        out.green("[ok]");
        std::printf(" removed '%s'\n", buildDir.c_str());
        cleaned = true;
    }

    if (fs::exists(cacheDir)) {
        fs::remove_all(cacheDir);
        out.green("[ok]");
        std::printf(" removed '%s'\n", cacheDir.string().c_str());
        cleaned = true;
    }

    if (!cleaned) {
        err.yellow("[info]");
        std::fprintf(stderr, " nothing to clean\n");
    }

    return cleaned ? 0 : 1;
}

int deps(const Options &opts) {
    auto TERM = term::init(opts);
    term::UsagePrinter err{stderr, TERM.cerrOn};
    err.yellow("[soon]");
    std::fprintf(stderr, " deps not implemented yet\n");
    if (opts.subcommandStr.empty()) {
        std::fprintf(stderr,
                     "usage: zithc deps (list|add|remove|publish|unpublish|update) [args]\n");
    }
    return 1;
}

} // namespace zith::cli::commands
