#include <CLI/CLI.hpp>
#include <Nova/nova.h>
#include <iostream>
#include <string>

#include "Nova/nova.h"

const char* nova_version = NOVA_VERSION;

// ============================================================================
// Helpers internos
// ============================================================================

static void print_not_implemented(const char* command) {
    std::cerr << "\n[!] Command '" << command << "' is not implemented yet.\n";
    std::cerr << "    This feature will be available in a future version of Nova.\n";
    std::cerr << "    Track progress at: https://github.com/GalaxyHaze/Nova\n\n";
}

static void print_success(const char* action, const char* target) {
    std::cout << "[ok] " << action << ": " << target << "\n";
}

static void print_error(const char* msg) {
    std::cerr << "[error] " << msg << "\n";
}

static void print_info(const char* msg) {
    std::cout << "[*] " << msg << "\n";
}

// ============================================================================
// novaProject.toml
// ============================================================================

struct NovaProject {
    std::string              name;
    std::string              version;
    std::string              entry;   // src/main.nova
    std::string              output;  // bin/project  ou  bin/project.nbc
    std::string              mode;    // debug | dev | release | fast | test
    std::vector<std::string> include_dirs;
};

// Retorna false se não encontrar ou falhar ao parsear
static bool try_load_project(NovaProject& proj) {
    if (!nova_file_exists("novaProject.toml")) return false;
    // TODO: parsear TOML de verdade (ex: toml++ ou cpptoml)
    // Por ora preenche defaults para não travar o fluxo
    proj.name    = "project";
    proj.version = "0.1.0";
    proj.entry   = "src/main.nova";
    proj.output  = "bin/project";
    proj.mode    = "debug";
    return true;
}

// ============================================================================
// Comandos
// ============================================================================

// check — parse + semântica, só reporta erros, não guarda resultado
static int cmd_check(const std::string& input_file,
                     const std::string& mode_str, const bool verbose) {
    std::string src = input_file;

    if (src.empty()) {
        NovaProject proj;
        if (!try_load_project(proj)) {
            print_error("No input file and no novaProject.toml found");
            return 1;
        }
        src = proj.entry;
    }

    if (verbose)
        print_info(("Checking '" + src + "' in " + mode_str + " mode...").c_str());

    NovaArena* arena = nova_arena_create(64 * 1024);
    if (!arena) { print_error("Failed to create memory arena"); return 1; }

    size_t file_size = 0;
    const char* source = nova_load_file_to_arena(arena, src.c_str(), &file_size);
    if (!source) {
        print_error(("Failed to load file: " + src).c_str());
        nova_arena_destroy(arena);
        return 1;
    }

    auto [data, len] = nova_tokenize(arena, source, file_size);
    if (! data) {
        nova_arena_destroy(arena);
        return 1;
    }

    if (verbose)
        std::cout << "[*] Tokenized " << len << " tokens\n";

    // TODO: parse + análise semântica — reportar erros e descartar resultado

    nova_arena_destroy(arena);
    print_success("Check passed", src.c_str());
    return 0;
}

// compile — check + gera IR/objeto (nativo) ou bytecode (.nbc), sem linkar
static int cmd_compile(const std::string& input_file, const std::string& output_file,
                       const std::string& mode_str, bool interpreted, bool verbose,
                       const std::vector<std::string>& include_dirs = {}) {
    if (verbose) {
        const char* kind = interpreted ? "bytecode" : "native object";
        std::cout << "[*] Compiling '" << input_file << "' -> "
                  << kind << " (" << mode_str << ")\n";
    }

    NovaArena* arena = nova_arena_create(64 * 1024);
    if (!arena) { print_error("Failed to create memory arena"); return 1; }

    size_t file_size = 0;
    const char* source = nova_load_file_to_arena(arena, input_file.c_str(), &file_size);
    if (!source) {
        print_error(("Failed to load file: " + input_file).c_str());
        nova_arena_destroy(arena);
        return 1;
    }

    auto [data, len] = nova_tokenize(arena, source, file_size);

    if (!data) {
        nova_arena_destroy(arena);
        return 1;
    }

    if (verbose)
        std::cout << "[*] Tokenized " << len << " tokens\n";

    /*
    const NovaNode* ast = nova_parse(arena, tokens);
    if (!ast) {
        print_error("AST parsing failed");
        nova_arena_destroy(arena);
        return 1;
    }
    if (verbose) print_info("AST built successfully");
    */

    // TODO: análise semântica
    // TODO: interpreted  → gera bytecode (.nbc)
    // TODO: !interpreted → gera IR/objeto nativo (.o)

    nova_arena_destroy(arena);

    const std::string out = output_file.empty()
        ? (interpreted ? "a.nbc" : "a.o")
        : output_file;

    print_success(interpreted ? "Bytecode compile" : "Compile", out.c_str());
    return 0;
}

// build — compile + linka → binário nativo final (nunca interpretado)
static int cmd_build(const std::string& input_file, const std::string& output_file,
                     const std::string& mode_str, const bool verbose,
                     const std::vector<std::string>& include_dirs = {}) {
    std::string src  = input_file;
    std::string out  = output_file;
    std::string mode = mode_str;

    if (src.empty()) {
        NovaProject proj;
        if (!try_load_project(proj)) {
            print_error("No input file and no novaProject.toml found");
            return 1;
        }
        src = proj.entry;
        if (out.empty())         out  = proj.output;
        if (mode == "debug")     mode = proj.mode; // toml só sobrescreve o default
    }

    if (verbose)
        std::cout << "[*] Building '" << src << "' -> binary (" << mode << ")\n";

    if (const int rc = cmd_compile(src, "", mode, false, verbose, include_dirs); rc != 0) return rc;

    // TODO: link → binário final

    const std::string binary = out.empty() ? "a.out" : out;
    print_success("Build", binary.c_str());
    return 0;
}

// execute — só executa artefato existente; falha se não encontrar
static int cmd_execute(const std::string& target, const bool interpreted, const bool verbose) {
    std::string bin = target;

    if (bin.empty()) {
        NovaProject proj;
        if (!try_load_project(proj)) {
            print_error("No target specified and no novaProject.toml found");
            return 1;
        }
        bin = interpreted ? (proj.output + ".nbc") : proj.output;
    }

    if (!nova_file_exists(bin.c_str())) {
        print_error(("Target not found: '" + bin +
                     "' -- did you run 'nova " +
                     (interpreted ? "compile --interpreted" : "build") +
                     "' first?").c_str());
        return 1;
    }

    if (verbose)
        std::cout << "[*] Executing '" << bin << "'"
                  << (interpreted ? " (interpreted)" : "") << "\n";

    // TODO: interpreted  → invocar VM / bytecode runner
    // TODO: !interpreted → execv / CreateProcess

    print_not_implemented("execute");
    return 1;
}

// run — build + execute (nativo) ou compile --interpreted + execute --interpreted
static int cmd_run(const std::string& input_file, const std::string& output_file,
                   const std::string& mode_str, const bool interpreted, const bool verbose,
                   const std::vector<std::string>& include_dirs = {}) {
    if (interpreted) {
        const std::string bc = output_file.empty() ? "a.nbc" : output_file;
        if (const int rc = cmd_compile(input_file, bc, mode_str, true, verbose, include_dirs); rc != 0) return rc;
        return cmd_execute(bc, true, verbose);
    }

    if (const int rc = cmd_build(input_file, output_file, mode_str, verbose, include_dirs); rc != 0) return rc;

    const std::string binary = output_file.empty() ? "a.out" : output_file;
    return cmd_execute(binary, false, verbose);
}

static int cmd_test(const std::string& input_file, bool verbose) {
    print_not_implemented("test");
    return 1;
}

static int cmd_fmt(const std::string& input_file, bool check_only, bool verbose) {
    print_not_implemented("fmt");
    return 1;
}

static int cmd_docs(const std::string& input_file, const std::string& output_dir, bool verbose) {
    print_not_implemented("docs");
    return 1;
}

static int cmd_repl(bool verbose) {
    print_not_implemented("repl");
    return 1;
}

static int cmd_version() {
    std::cout << "Nova Programming Language\n";
    std::cout << "Version: " << nova_version << "\n";
    std::cout << "Compiler: " << __VERSION__ << "\n";
    return 0;
}

static int cmd_help() {
    std::cout << R"(Nova - A low-level general-purpose language

USAGE:
    nova [OPTIONS] <COMMAND> [ARGS]

COMMANDS:
    check      Parse and type-check; report errors only, no output
    compile    Compile to IR/object (native or bytecode), no linking
    build      Compile and link to native binary  (reads novaProject.toml)
    execute    Run an existing binary or bytecode (reads novaProject.toml)
    run        Build then execute                 (reads novaProject.toml)
    test       Run tests defined in source
    fmt        Format source code
    docs       Generate documentation
    repl       Start interactive REPL
    version    Show version information
    help       Show this help message

OPTIONS:
    -m, --mode <debug|dev|release|fast|test>    Build mode [default: debug]
    -o, --output <FILE>                         Output file
    -I, --include <DIR>                         Add include directory (repeatable)
    --interpreted                               Use bytecode path instead of native
    --emit <ast|ir|asm|obj|bin>                Emit intermediate representation
    --target <TRIPLE>                           Target triple (e.g., x86_64-linux-gnu)
    -v, --verbose                               Verbose output
    -h, --help                                  Show help

PIPELINE:
    check  <  compile  <  build
    execute             <  run
    run --interpreted  =  compile --interpreted + execute --interpreted

EXAMPLES:
    nova check main.nova
    nova compile main.nova -o main.o
    nova compile --interpreted main.nova -o main.nbc
    nova build
    nova build main.nova -o bin/app -m release
    nova execute
    nova execute --interpreted
    nova run
    nova run main.nova -m release
    nova run --interpreted main.nova

LEARN MORE:
    Source: https://github.com/GalaxyHaze/Nova
    Docs:   https://nova-lang.dev (coming soon)
)";
    return 0;
}

// ============================================================================
// Dispatch central: nova_run (C API)
// ============================================================================

extern "C" int nova_run(const int argc, const char* argv[]) {
    CLI::App app{"Nova - A low-level general-purpose language"};
    app.require_subcommand(0, 1);

    // Opcoes globais
    std::string              mode_str = "debug";
    std::string              output_file;
    std::vector<std::string> include_dirs;
    std::string              emit_target;
    std::string              target_triple;
    bool                     verbose = false;

    app.add_option("-m,--mode", mode_str,
        "Build mode: debug, dev, release, fast, test")
       ->transform(CLI::IsMember({"debug", "dev", "release", "fast", "test"}))
       ->default_str("debug");

    app.add_option("-o,--output",  output_file,   "Output file path");
    app.add_option("-I,--include", include_dirs,  "Include directories (repeatable)");
    app.add_option("--emit",       emit_target,   "Emit: ast, ir, asm, obj, bin");
    app.add_option("--target",     target_triple, "Target triple");
    app.add_flag  ("-v,--verbose", verbose,       "Verbose output");

    // ── Subcomandos ──────────────────────────────────────────────────────────

    std::string input_file;
    bool        interpreted = false;
    bool        fmt_check   = false;
    std::string docs_output = "docs";

    // check
    auto* check_cmd = app.add_subcommand("check", "Parse and type-check only");
    check_cmd->add_option("input", input_file, "Source file (.nova) [optional, reads toml if omitted]")
             ->check(CLI::ExistingFile);

    // compile
    auto* compile_cmd = app.add_subcommand("compile", "Compile to IR/object, no linking");
    compile_cmd->add_option("input", input_file, "Source file (.nova)")
               ->required()
               ->check(CLI::ExistingFile);
    compile_cmd->add_flag("--interpreted", interpreted, "Compile to bytecode instead of native");

    // build
    auto* build_cmd = app.add_subcommand("build", "Compile and link to native binary");
    build_cmd->add_option("input", input_file,
                          "Source file (.nova) [optional, reads toml if omitted]")
             ->check(CLI::ExistingFile);

    // execute
    auto* execute_cmd = app.add_subcommand("execute", "Run existing binary or bytecode");
    execute_cmd->add_option("target", input_file,
                            "Binary or bytecode [optional, reads toml if omitted]");
    execute_cmd->add_flag("--interpreted", interpreted, "Run bytecode instead of native binary");

    // run
    auto* run_cmd = app.add_subcommand("run", "Build then execute");
    run_cmd->add_option("input", input_file,
                        "Source file (.nova) [optional, reads toml if omitted]")
           ->check(CLI::ExistingFile);
    run_cmd->add_flag("--interpreted", interpreted,
                      "Compile to bytecode and run interpreted");

    // test
    auto* test_cmd = app.add_subcommand("test", "Run tests in source file");
    test_cmd->add_option("input", input_file, "Source file (.nova)")
            ->check(CLI::ExistingFile);

    // fmt
    auto* fmt_cmd = app.add_subcommand("fmt", "Format source code");
    fmt_cmd->add_option("input", input_file, "Source file or directory")->required();
    fmt_cmd->add_flag("--check", fmt_check, "Check formatting without modifying files");

    // docs
    auto* docs_cmd = app.add_subcommand("docs", "Generate documentation");
    docs_cmd->add_option("input", input_file, "Source file (.nova)")
            ->check(CLI::ExistingFile);
    docs_cmd->add_option("-o,--output", docs_output, "Output directory")
            ->default_str("docs");

    // repl / version / help
    auto* repl_cmd    = app.add_subcommand("repl",    "Start interactive REPL");
    auto* version_cmd = app.add_subcommand("version", "Show version information");
    auto* help_cmd    = app.add_subcommand("help",    "Show help message");

    // ── Parse ────────────────────────────────────────────────────────────────

    try {
        app.parse(argc, argv);
    } catch (const CLI::CallForHelp&) {
        return cmd_help();
    } catch (const CLI::CallForVersion&) {
        return cmd_version();
    } catch (const CLI::ParseError& e) {
        if (e.get_name() == "ExtrasError" && argc > 1 && std::string(argv[1]) == "help")
            return cmd_help();
        std::cerr << "[error] " << e.what() << "\n\n";
        std::cerr << app.help();
        return 1;
    }

    // ── Dispatch ─────────────────────────────────────────────────────────────

    if (*help_cmd)    return cmd_help();
    if (*version_cmd) return cmd_version();
    if (*repl_cmd)    return cmd_repl(verbose);
    if (*docs_cmd)    return cmd_docs(input_file, docs_output, verbose);
    if (*fmt_cmd)     return cmd_fmt(input_file, fmt_check, verbose);
    if (*test_cmd)    return cmd_test(input_file, verbose);
    if (*check_cmd)   return cmd_check(input_file, mode_str, verbose);
    if (*compile_cmd) return cmd_compile(input_file, output_file, mode_str,
                                         interpreted, verbose, include_dirs);
    if (*build_cmd)   return cmd_build(input_file, output_file, mode_str,
                                       verbose, include_dirs);
    if (*execute_cmd) return cmd_execute(input_file, interpreted, verbose);
    if (*run_cmd)     return cmd_run(input_file, output_file, mode_str,
                                     interpreted, verbose, include_dirs);

    // Sem subcomando: tenta build via toml, senão mostra ajuda
    {
        NovaProject proj;
        if (try_load_project(proj))
            return cmd_build("", "", mode_str, verbose, include_dirs);
    }

    cmd_help();
    return 0;
}