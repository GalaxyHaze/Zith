#include "options.hpp"
#include "cli/commands.hpp"
#include "cli/terminal.hpp"
#include "cli/zith-flags.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#ifdef _WIN32
#include <io.h>
#elif !defined(ZITH_IS_WASM)
#include <unistd.h>
#endif
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <toml++/toml.hpp>
#include <type_traits>
#include <utility>

namespace zith {

struct ModeDefaults {
    uint8_t optLevel;
    uint8_t debugLevel;
    bool stripDebug;
    bool lto;
};

// Custom, Debug, Develop, Release, Fast, Small
static constexpr ModeDefaults modeDefaults[] = {
    {0, 2, false, false}, // Custom
    {0, 3, false, false}, // Debug
    {1, 2, false, false}, // Develop
    {2, 1, true, false},  // Release
    {3, 0, true, true},   // Fast
    {2, 0, true, true},   // Small
};

static ModeDefaults getDefaults(Options::Mode mode) {
    auto idx = static_cast<size_t>(mode);
    if (idx >= std::size(modeDefaults))
        idx = 1; // fallback to Debug
    return modeDefaults[idx];
}

void Options::deriveTargetStage() {
    // Run/Execute need a binary; Build needs at least an object file, so all
    // three go through codegen unless an explicit --emit target says otherwise.
    if (command == Command::Run || command == Command::Execute) {
        targetStage = session::Stage::Cached;
        return;
    }
    if (command == Command::Build && emitTarget == EmitTarget::None && !flags.emitHir() &&
        !flags.emitIr() && !flags.emitAsm()) {
        targetStage = session::Stage::Cached;
        return;
    }

    // emitAst is handled inside semaStage (body expansion + print) — no short-circuit
    if (flags.emitHir())
        targetStage = session::Stage::HirLowered;
    else if (flags.emitIr() || flags.emitAsm())
        targetStage = session::Stage::CodegenReady;
    else {
        switch (emitTarget) {
        case EmitTarget::Ast:
            targetStage = session::Stage::Imported;
            break;
        case EmitTarget::Hir:
            targetStage = session::Stage::HirLowered;
            break;
        case EmitTarget::Ir:
        case EmitTarget::Asm:
            targetStage = session::Stage::CodegenReady;
            break;
        case EmitTarget::Obj:
        case EmitTarget::Bin:
            targetStage = session::Stage::Cached;
            break;
        default:
            break;
        }
    }
}

static bool compare(const char *a, const char *b) {
    return std::strcmp(a, b) == 0;
}
template <class... Args> static bool compare(const char *a, Args &&...args) {
    static_assert((std::is_same_v<std::decay_t<Args>, const char *> && ...),
                  "All 'args' must be 'const char*'");
    return ((std::strcmp(a, args) == 0) || ...);
}

static size_t levenshteinDistance(const char *a, const char *b) {
    size_t n = std::strlen(a);
    size_t m = std::strlen(b);
    if (n == 0)
        return m;
    if (m == 0)
        return n;
    std::vector<size_t> prev(m + 1);
    std::vector<size_t> curr(m + 1);
    for (size_t j = 0; j <= m; ++j)
        prev[j] = j;
    for (size_t i = 1; i <= n; ++i) {
        curr[0] = i;
        for (size_t j = 1; j <= m; ++j) {
            size_t cost  = (a[i - 1] == b[j - 1]) ? 0 : 1;
            size_t a_val = prev[j] + 1;
            size_t b_val = curr[j - 1] + 1;
            size_t c_val = prev[j - 1] + cost;
            curr[j] =
                (a_val < b_val) ? (a_val < c_val ? a_val : c_val) : (b_val < c_val ? b_val : c_val);
        }
        std::swap(prev, curr);
    }
    return prev[m];
}

static void suggestCommand(const char *arg, term::UsagePrinter & /*err*/) {
    static const char *suggestCmds[] = {"build", "run",  "check", "execute", "test",
                                        "fmt",   "docs", "repl",  "create",  "clean",
                                        "deps",  "help", nullptr};
    const char *best                 = nullptr;
    size_t best_dist                 = static_cast<size_t>(-1);
    for (size_t i = 0; suggestCmds[i]; ++i) {
        size_t d = levenshteinDistance(arg, suggestCmds[i]);
        if (d < best_dist) {
            best_dist = d;
            best      = suggestCmds[i];
        }
    }
    if (best && best_dist <= (std::max(std::strlen(arg), std::strlen(best)) + 1) / 2) {
#ifdef ZITH_IS_WASM
        bool color = false;
#elif defined(_WIN32)
        bool color = _isatty(_fileno(stderr)) != 0;
#else
        bool color = isatty(fileno(stderr)) != 0;
#endif
        std::fprintf(stderr, "%s[error]%s unknown command '%s'\n", color ? "\033[31m" : "",
                     color ? "\033[0m" : "", arg);
        std::fprintf(stderr, "%s  help: did you mean '%s'?%s\n", color ? "\033[31m" : "", best,
                     color ? "\033[0m" : "");
        std::exit(1);
    }
}

static bool isSubcommand(const char *arg) {
    static const char *cmds[] = {"build", "run",    "check", "execute", "test", "fmt",  "docs",
                                 "repl",  "create", "clean", "deps",    "help", nullptr};
    for (auto cmd : cmds) {
        if (cmd && compare(cmd, arg))
            return true;
    }
    return false;
}

using Command = Options::Command;
static Command subcommandToEnum(const char *arg) {
    if (compare(arg, "build"))
        return Command::Build;
    if (compare(arg, "run"))
        return Command::Run;
    if (compare(arg, "check"))
        return Command::Check;
    if (compare(arg, "execute"))
        return Command::Execute;
    if (compare(arg, "test"))
        return Command::Test;
    if (compare(arg, "fmt"))
        return Command::Fmt;
    if (compare(arg, "docs"))
        return Command::Docs;
    if (compare(arg, "repl"))
        return Command::Repl;
    if (compare(arg, "create"))
        return Command::Create;
    if (compare(arg, "clean"))
        return Command::Clean;
    if (compare(arg, "deps"))
        return Command::Deps;
    if (compare(arg, "help"))
        return Command::Help;
    return Command::None;
}

void Cli::parseArgs(int argc, char **argv) {
    this->args = std::make_pair(argc, argv);

    for (int &i = this->current; i < argc; ++i) {
        if (isSubcommand(argv[i]))
            // Consume next arg as subcommand_arg for commands that take one
            switch (this->opts.command = subcommandToEnum(argv[i])) {
            case Options::Command::Create:
            case Options::Command::Clean:
            case Options::Command::Deps:
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    this->opts.subcommandArg = this->stringPool.intern(argv[i + 1]);
                    this->opts.subcommandStr = argv[i + 1];
                    ++i;
                }
                continue;
            default:
                continue;
            }

        // Unknown command check: if it looks like a typo for a known command
        if (opts.command == Options::Command::None && argv[i][0] != '-')
            suggestCommand(argv[i], err);

        if (compare(argv[i], "-h", "--help")) {
            opts.command = Options::Command::Help;
            continue;
        }

        if (compare(argv[i], "--version")) {
            opts.command = Options::Command::Version;
            continue;
        }

        if (compare(argv[i], "--emit-tokens")) {
            opts.flags.emitTokens(true);
            continue;
        }

        if (compare(argv[i], "--emit-ast")) {
            opts.flags.emitAst(true);
            continue;
        }

        if (compare(argv[i], "--emit-hir")) {
            opts.flags.emitHir(true);
            continue;
        }

        if (compare(argv[i], "--emit-ir")) {
            opts.flags.emitIr(true);
            continue;
        }

        if (compare(argv[i], "--emit-asm")) {
            opts.flags.emitAsm(true);
            continue;
        }

        if (compare(argv[i], "--emit-all")) {
            opts.flags.emitTokens(true);
            opts.flags.emitAst(true);
            opts.flags.emitHir(true);
            opts.flags.emitIr(true);
            opts.flags.emitAsm(true);
            continue;
        }

        if (compare(argv[i], "--interpreted")) {
            opts.flags.interpreted(true);
            continue;
        }

        if (compare(argv[i], "--verbose", "-v")) {
            opts.flags.verbose(true);
            continue;
        }

        if (compare(argv[i], "--strict", "-s")) {
            opts.flags.strict(true);
            continue;
        }

        if (compare(argv[i], "--lto")) {
            opts.flags.lto(true);
            opts.cliFields |= Options::kCliLto;
            continue;
        }

        if (compare(argv[i], "--strip-debug")) {
            opts.flags.stripDebug(true);
            opts.cliFields |= Options::kCliStripDebug;
            continue;
        }

        if (compare(argv[i], "-m", "--mode")) {
            requireValue(i, "--mode/-m");
            auto val = argv[++i];
            Options::Mode m;
            if (std::strcmp(val, "debug") == 0)
                m = Options::Mode::Debug;
            else if (compare(val, "dev"))
                m = Options::Mode::Develop;
            else if (compare(val, "release"))
                m = Options::Mode::Release;
            else if (compare(val, "fast"))
                m = Options::Mode::Fast;
            else if (compare(val, "small"))
                m = Options::Mode::Small;
            else {
                err.red("[error] invalid mode, expected: debug|dev|release|fast|small\ngot: ");
                err.red(val);
                err.red("\n");
                std::exit(1);
            }
            opts.flags.mode(m);
            continue;
        }

        if (compare(argv[i], "-o", "--output")) {
            requireValue(i, "--output/-o");
            opts.outputFile = argv[++i];
            continue;
        }

        if (compare(argv[i], "-I", "--include")) {
            requireValue(i, "--include/-I");
            std::string_view arg = argv[++i];

            std::string_view rest = arg;
            while (!rest.empty()) {
                auto pos   = rest.find(',');
                auto token = rest.substr(0, pos);
                if (!token.empty())
                    opts.includeDirs.push(std::string(token));
                if (pos == std::string_view::npos)
                    break;
                rest = rest.substr(pos + 1);
            }
            continue;
        }

        if (std::strcmp(argv[i], "--c-source-dir") == 0) {
            requireValue(i, "--c-source-dir");
            opts.cSourceDirs.push(std::string(argv[++i]));
            continue;
        }

        if (std::strcmp(argv[i], "-L") == 0) {
            requireValue(i, "-L");
            opts.libraryDirs.push(std::string(argv[++i]));
            continue;
        }

        if (std::strcmp(argv[i], "-l") == 0) {
            requireValue(i, "-l");
            opts.libraries.push(std::string(argv[++i]));
            continue;
        }

        if (std::strcmp(argv[i], "-D") == 0) {
            requireValue(i, "-D");
            opts.defines.push(std::string(argv[++i]));
            continue;
        }

        if (compare(argv[i], "-A", "--assets")) {
            requireValue(i, "--assets/-A");
            std::string_view arg = argv[++i];

            std::string_view rest = arg;
            while (!rest.empty()) {
                auto pos   = rest.find(',');
                auto token = rest.substr(0, pos);
                if (!token.empty())
                    opts.assetDirs.push(std::string(token));
                if (pos == std::string_view::npos)
                    break;
                rest = rest.substr(pos + 1);
            }
            continue;
        }

        // --emit
        if (std::strcmp(argv[i], "--emit") == 0) {
            requireValue(i, "--emit");
            const char *val = argv[++i];
            Options::EmitTarget target;
            if (std::strcmp(val, "ast") == 0)
                target = Options::EmitTarget::Ast;
            else if (std::strcmp(val, "hir") == 0)
                target = Options::EmitTarget::Hir;
            else if (std::strcmp(val, "ir") == 0)
                target = Options::EmitTarget::Ir;
            else if (std::strcmp(val, "asm") == 0)
                target = Options::EmitTarget::Asm;
            else if (std::strcmp(val, "obj") == 0)
                target = Options::EmitTarget::Obj;
            else if (std::strcmp(val, "bin") == 0)
                target = Options::EmitTarget::Bin;
            else {
                err.red("[error]");
                std::fprintf(stderr,
                             " invalid emit target '%s' (expected ast|hir|ir|asm|obj|bin)\n", val);
                std::exit(1);
            }
            opts.emitTarget = target;
            continue;
        }

        // --target
        if (std::strcmp(argv[i], "--target") == 0) {
            requireValue(i, "--target");
            opts.targetTriple = argv[++i];
            continue;
        }

        // --sysroot
        if (std::strcmp(argv[i], "--sysroot") == 0) {
            requireValue(i, "--sysroot");
            opts.sysroot = argv[++i];
            continue;
        }

        // --no-system-includes
        if (std::strcmp(argv[i], "--no-system-includes") == 0) {
            opts.systemIncludes = false;
            continue;
        }

        // --opt-level
        if (std::strcmp(argv[i], "--opt-level") == 0) {
            requireValue(i, "--opt-level");
            int val = std::atoi(argv[++i]);
            if (val < 0 || val > 3) {
                err.red("[error]");
                std::fprintf(stderr, " --opt-level must be 0-3\n");
                std::exit(1);
            }
            opts.flags.optLevel(static_cast<uint8_t>(val));
            opts.cliFields |= Options::kCliOptLevel;
            continue;
        }

        // --debug-level
        if (std::strcmp(argv[i], "--debug-level") == 0) {
            requireValue(i, "--debug-level");
            int val = std::atoi(argv[++i]);
            if (val < 0 || val > 3) {
                err.red("[error]");
                std::fprintf(stderr, " --debug-level must be 0-3\n");
                std::exit(1);
            }
            opts.flags.debugLevel(static_cast<uint8_t>(val));
            opts.cliFields |= Options::kCliDebugLevel;
            continue;
        }

        // --color / -c
        if (std::strcmp(argv[i], "--color") == 0 || std::strcmp(argv[i], "-c") == 0) {
            requireValue(i, "--color/-c");
            const char *val = argv[++i];
            Options::Color c;
            if (std::strcmp(val, "auto") == 0)
                c = Options::Color::Auto;
            else if (std::strcmp(val, "on") == 0)
                c = Options::Color::On;
            else if (std::strcmp(val, "off") == 0)
                c = Options::Color::Off;
            else {
                err.red("[error]");
                std::fprintf(stderr, " --color must be auto|on|off\n");
                std::exit(1);
            }
            opts.flags.color(c);
            continue;
        }

        // --check (fmt)
        if (std::strcmp(argv[i], "--check") == 0) {
            opts.flags.fmtCheck(true);
            continue;
        }

        // -i / --in-place (fmt)
        if (std::strcmp(argv[i], "-i") == 0 || std::strcmp(argv[i], "--in-place") == 0) {
            opts.flags.fmtInPlace(true);
            continue;
        }

        if (std::strcmp(argv[i], "--cache-stats") == 0) {
            opts.flags.cacheStats(true);
            continue;
        }

        // "-" is a positional arg meaning stdin, not a flag
        if (std::strcmp(argv[i], "-") == 0) {
            opts.inputFiles.push(std::string("-"));
            continue;
        }

        // Unknown flag
        if (argv[i][0] == '-') {
            err.red("[error]");
            std::fprintf(stderr, " unknown flag '%s'\n", argv[i]);
            cli::commands::help(stderr);
            std::exit(1);
        }

        // Positional: input file
        opts.inputFiles.push(std::string(argv[i]));
    }

    opts.deriveTargetStage();
}

void Cli::loadFlags() {
    namespace fs = std::filesystem;

    // Apply mode defaults first (but don't overwrite CLI-set values)
    auto defaults = getDefaults(opts.flags.mode());
    if (!(opts.cliFields & Options::kCliOptLevel))
        opts.flags.optLevel(defaults.optLevel);
    if (!(opts.cliFields & Options::kCliDebugLevel))
        opts.flags.debugLevel(defaults.debugLevel);
    if (!(opts.cliFields & Options::kCliStripDebug))
        opts.flags.stripDebug(defaults.stripDebug);
    if (!(opts.cliFields & Options::kCliLto))
        opts.flags.lto(defaults.lto);

    // Try to find ZithFlags.toml
#ifndef ZITH_IS_WASM
    fs::path flagsPath;
    if (!find_flags_file(flagsPath))
        return;
#endif

    auto process [[maybe_unused]] = [&](const toml::table &tbl) {
        if (auto v = tbl["mode"].value<std::string>()) {
            if (*v == "debug")
                opts.flags.mode(Options::Mode::Debug);
            else if (*v == "dev")
                opts.flags.mode(Options::Mode::Develop);
            else if (*v == "release")
                opts.flags.mode(Options::Mode::Release);
            else if (*v == "fast")
                opts.flags.mode(Options::Mode::Fast);
            else if (*v == "small")
                opts.flags.mode(Options::Mode::Small);
        }
        if (auto v = tbl["opt_level"].value<int64_t>())
            if (*v >= 0 && *v <= 3)
                if (!(opts.cliFields & Options::kCliOptLevel))
                    opts.flags.optLevel(static_cast<uint8_t>(*v));
        if (auto v = tbl["debug_level"].value<int64_t>())
            if (*v >= 0 && *v <= 3)
                opts.flags.debugLevel(static_cast<uint8_t>(*v));
        if (auto v = tbl["verbose"].value<bool>())
            opts.flags.verbose(*v);
        if (auto v = tbl["strict"].value<bool>())
            opts.flags.strict(*v);
        if (auto v = tbl["strip_debug"].value<bool>())
            opts.flags.stripDebug(*v);
        if (auto v = tbl["lto"].value<bool>())
            opts.flags.lto(*v);
        if (auto v = tbl["color"].value<std::string>()) {
            if (*v == "auto")
                opts.flags.color(Options::Color::Auto);
            else if (*v == "on")
                opts.flags.color(Options::Color::On);
            else if (*v == "off")
                opts.flags.color(Options::Color::Off);
        }
        if (auto arr = tbl["include_dirs"].as_array()) {
            for (auto &elem : *arr)
                if (auto s = elem.value<std::string>())
                    opts.includeDirs.push(*s);
        }
        if (auto v = tbl["emit_target"].value<std::string>()) {
            if (*v == "ast")
                opts.emitTarget = Options::EmitTarget::Ast;
            else if (*v == "hir")
                opts.emitTarget = Options::EmitTarget::Hir;
            else if (*v == "ir")
                opts.emitTarget = Options::EmitTarget::Ir;
            else if (*v == "asm")
                opts.emitTarget = Options::EmitTarget::Asm;
            else if (*v == "obj")
                opts.emitTarget = Options::EmitTarget::Obj;
            else if (*v == "bin")
                opts.emitTarget = Options::EmitTarget::Bin;
        }
    };

    // Re-apply mode defaults after TOML values (but not for CLI-set fields)
    auto applyModeDefaults = [&]() {
        auto d = getDefaults(opts.flags.mode());
        if (!(opts.cliFields & Options::kCliOptLevel))
            opts.flags.optLevel(d.optLevel);
        if (!(opts.cliFields & Options::kCliDebugLevel))
            opts.flags.debugLevel(d.debugLevel);
        if (!(opts.cliFields & Options::kCliStripDebug))
            opts.flags.stripDebug(d.stripDebug);
        if (!(opts.cliFields & Options::kCliLto))
            opts.flags.lto(d.lto);
    };

#ifndef ZITH_IS_WASM
#if TOML_EXCEPTIONS
    try {
        process(toml::parse_file(flagsPath.string()));
    } catch (const toml::parse_error &) {
        return;
    }
#else
    auto result = toml::parse_file(flagsPath.string());
    if (!result)
        return;
    process(result);
#endif
#endif

    applyModeDefaults();
}

void Cli::loadProject() {
#ifdef ZITH_IS_WASM
    return;
#else
    namespace fs = std::filesystem;

    fs::path search = fs::current_path();
    while (true) {
        auto tomlPath = search / "ZithProject.toml";
        if (fs::exists(tomlPath)) {
            config.projectRoot = fs::weakly_canonical(search).string();
            auto process       = [&](const toml::table &tbl) {
                if (auto *build = tbl["build"].as_table()) {
                    if (auto v = build->get("entry"))
                        if (auto s = v->value<std::string>())
                            config.entry = *s;
                    if (auto v = build->get("output"))
                        if (auto s = v->value<std::string>())
                            config.output = *s;
                    if (auto v = build->get("mode"))
                        if (auto s = v->value<std::string>()) {
                            if (*s == "debug")
                                config.mode = "debug";
                            else if (*s == "dev")
                                config.mode = "dev";
                            else if (*s == "release")
                                config.mode = "release";
                            else if (*s == "fast")
                                config.mode = "fast";
                            else if (*s == "small")
                                config.mode = "small";
                        }
                    if (auto v = build->get("opt_level"))
                        if (auto i = v->value<int>())
                            config.opt_level = *i;
                }
                if (auto *paths = tbl["paths"].as_table()) {
                    if (auto *src = paths->get("src_dir")) {
                        if (auto *arr = src->as_array()) {
                            for (auto &elem : *arr)
                                if (auto s = elem.value<std::string>())
                                    config.srcDirs.push(*s);
                        } else if (auto s = src->value<std::string>()) {
                            config.srcDirs.push(*s);
                        }
                    }
                    if (auto v = paths->get("bin_dir"))
                        if (auto s = v->value<std::string>())
                            config.binDir = *s;
                    if (auto v = paths->get("mod_dir"))
                        if (auto s = v->value<std::string>())
                            config.modDir = *s;
                    if (auto v = paths->get("docs_dir"))
                        if (auto s = v->value<std::string>())
                            config.docsDir = *s;
                    if (auto v = paths->get("test_dir"))
                        if (auto s = v->value<std::string>())
                            config.testDir = *s;
                    if (auto v = paths->get("asset_dir"))
                        if (auto s = v->value<std::string>())
                            config.assetDir = *s;
                }
                if (auto *ffi = tbl["ffi"].as_table()) {
                    const auto loadArray = [](const toml::table &table, const char *key,
                                              memory::DynArray<std::string> &destination) {
                        if (const auto *values = table[key].as_array())
                            for (const auto &value : *values)
                                if (const auto text = value.value<std::string>())
                                    destination.push(*text);
                    };
                    loadArray(*ffi, "include_dirs", config.includeDirs);
                    loadArray(*ffi, "c_source_dirs", config.cSourceDirs);
                    loadArray(*ffi, "library_dirs", config.libraryDirs);
                    loadArray(*ffi, "libraries", config.libraries);
                    loadArray(*ffi, "defines", config.defines);
                }
                if (auto *proj = tbl["project"].as_table()) {
                    if (auto v = proj->get("name"))
                        if (auto s = v->value<std::string>())
                            config.name = *s;
                    if (auto v = proj->get("version"))
                        if (auto s = v->value<std::string>())
                            config.version = *s;
                    if (auto v = proj->get("description"))
                        if (auto s = v->value<std::string>())
                            config.description = *s;
                    if (auto v = proj->get("authors"))
                        if (auto s = v->value<std::string>())
                            config.authors = *s;
                    if (auto v = proj->get("license"))
                        if (auto s = v->value<std::string>())
                            config.license = *s;
                    if (auto v = proj->get("homepage"))
                        if (auto s = v->value<std::string>())
                            config.homepage = *s;
                }
            };

#if TOML_EXCEPTIONS
            try {
                process(toml::parse_file(tomlPath.string()));
            } catch (const toml::parse_error &) {
                return;
            }
#else
            auto result = toml::parse_file(tomlPath.string());
            if (!result)
                return;
            process(result);
#endif
            return;
        }
        if (search == search.root_path())
            break;
        search = search.parent_path();
    }
#endif
}

int Cli::dispatch() {
    if (opts.inputFiles.empty() && !config.projectRoot.empty()) {
        switch (opts.command) {
        case Command::Build:
        case Command::Check:
        case Command::Run:
        case Command::Execute:
            opts.inputFiles.push(config.projectRoot);
            break;
        default:
            break;
        }
    }

    switch (opts.command) {
    case Command::None:
    case Command::Help:
        return cli::commands::help();
    case Command::Version:
        return cli::commands::version();
    case Command::Build:
        return cli::commands::build(opts);
    case Command::Run:
        return cli::commands::run(opts);
    case Command::Check:
        return cli::commands::check(opts);
    case Command::Execute:
        return cli::commands::execute(opts);
    case Command::Test:
        return cli::commands::test(opts);
    case Command::Fmt:
        return cli::commands::fmt(opts);
    case Command::Docs:
        return cli::commands::docs(opts);
    case Command::Repl:
        return cli::commands::repl(opts);
    case Command::Create:
        return cli::commands::create(opts);
    case Command::Clean:
        return cli::commands::clean(opts);
    case Command::Deps:
        return cli::commands::deps(opts);
    case Command::Completion:
        return cli::commands::completion(opts);
    }
    return 1;
}

} // namespace zith
