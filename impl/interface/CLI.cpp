// kalidous_cli.cpp — CLI entry point for the Kalidous compiler
//
// Requires C++17 (structured bindings, if-initializers).
// Depends on: CLI11, Kalidous/kalidous.h

#include <CLI/CLI.hpp>
#include <kalidous/kalidous.h>
#include <iostream>
#include <string>
#include <vector>
#include "../utils/debug.h"
#include "../ast/ast.h"

static const char* kalidous_version = KALIDOUS_VERSION;

// ============================================================================
// Output helpers
// ============================================================================

static void print_error(const std::string& msg) {
    std::cerr << "[error] " << msg << "\n";
}

static void print_info(const std::string& msg) {
    std::cout << "[*] " << msg << "\n";
}

static void print_success(const std::string& action, const std::string& target) {
    std::cout << "[ok] " << action << ": " << target << "\n";
}

static void print_not_implemented(const std::string& command) {
    std::cerr << "\n[!] Command '" << command << "' is not implemented yet.\n"
              << "    This feature will be available in a future version of Kalidous.\n"
              << "    Track progress at: https://github.com/GalaxyHaze/Kalidous\n\n";
}

// ============================================================================
// KalidousProject — representa o KalidousProject.toml
// ============================================================================

struct KalidousProject {
    // -- Identidade --
    std::string name        = "project";
    std::string version     = "0.1.0";
    std::string description;
    std::string authors;
    std::string license;
    std::string homepage;

    // -- Compilação --
    std::string entry        = "src/main.kalidous";
    std::string output       = "bin/project";
    std::string mode         = "debug";
    std::string target_triple;
    std::string edition      = "2024";

    // -- Diretórios --
    std::string src_dir      = "src";
    std::string bin_dir      = "bin";
    std::string lib_dir      = "lib";
    std::string docs_dir     = "docs";
    std::string test_dir     = "tests";
    std::string cache_dir    = ".kalidous_cache";

    // -- Includes & Links --
    std::vector<std::string> include_dirs;
    std::vector<std::string> lib_paths;
    std::vector<std::string> link_libs;
    std::vector<std::string> link_flags;

    // -- Features & Dependências --
    std::vector<std::string> features;
    std::vector<std::string> dependencies;  // TODO: formato "libname@1.0" a definir

    // -- Comportamento --
    bool emit_ir     = false;
    bool emit_asm    = false;
    bool strip_debug = false;
    bool lto         = false;
    int  opt_level   = 0;    // 0–3, mapeado para LLVM opt passes
    int  debug_level = 2;    // 0–3, mapeado para DWARF debug info
};

// Retorna false se KalidousProject.toml não existir ou falhar ao ler.
// Preenche 'proj' com defaults enquanto o parser TOML não está implementado.
static bool try_load_project(KalidousProject& proj) {
    if (!kalidous_file_exists("KalidousProject.toml")) return false;

    // TODO: integrar toml++ (https://github.com/marzer/tomlplusplus)
    // TODO: validar campos obrigatórios: name, version, entry
    // TODO: reportar campos desconhecidos como warning
    // TODO: suportar herança de perfis, ex: [profile.release] opt_level = 3
    // TODO: suportar array de targets para cross-compilation
    // TODO: resolver paths relativos à localização do .toml

    proj = KalidousProject{}; // garante defaults mesmo que a leitura seja parcial
    return true;
}

// ============================================================================
// Pipeline
// ============================================================================

// Tokeniza 'src_path' numa arena nova.
// Preenche 'out_stream', 'out_source' e 'out_source_len' — o source fica na
// arena para ser reutilizado pelo parser sem second load.
// Retorna a arena (o chamador destrói); nullptr em caso de erro.
static KalidousArena* tokenize_file(const std::string& src_path,
                                    KalidousTokenStream& out_stream,
                                    const char** out_source,
                                    size_t* out_source_len,
                                    bool verbose) {
    KalidousArena* arena = kalidous_arena_create(64 * 1024);
    if (!arena) { print_error("Failed to create memory arena"); return nullptr; }

    size_t file_size = 0;
    const char* source = kalidous_load_file_to_arena(arena, src_path.c_str(), &file_size);
    if (!source) {
        print_error("Failed to load file: " + src_path);
        kalidous_arena_destroy(arena);
        return nullptr;
    }

    out_stream = kalidous_tokenize(arena, source, file_size);
    if (!out_stream.data) {
        kalidous_arena_destroy(arena);
        return nullptr;
    }

    if (verbose)
        print_info("Tokenized " + std::to_string(out_stream.len) + " tokens from " + src_path);

    // Return source pointer — already in arena, no extra allocation needed
    if (out_source)     *out_source     = source;
    if (out_source_len) *out_source_len = file_size;

    return arena;
}

// ============================================================================
// Comandos
// ============================================================================

// check — parse + semântica, só reporta erros, não produz artefacto
static int cmd_check(const std::string& input_file,
                     const std::string& mode_str, bool verbose) {
    std::string src = input_file;

    if (src.empty()) {
        KalidousProject proj;
        if (!try_load_project(proj)) {
            print_error("No input file and no KalidousProject.toml found");
            return 1;
        }
        src = proj.entry;
    }

    if (verbose) print_info("Checking '" + src + "' in " + mode_str + " mode...");

    KalidousTokenStream stream{};
    const char* source   = nullptr;
    size_t      src_size = 0;
    KalidousArena* arena = tokenize_file(src, stream, &source, &src_size, verbose);
    if (!arena) return 1;

    kalidous_debug_tokens(stream.data, stream.len);

    KalidousNode* ast = kalidous_parse_with_source(arena,
                                                    source, src_size,
                                                    src.c_str(), stream);

    // Debug: AST dump
    if (ast) {
        /*fprintf(stderr, "\n── AST ──────────────────────────────────────────\n");
        kalidous_ast_print(ast, 0);
        fprintf(stderr, "─────────────────────────────────────────────────\n\n");
        fflush(stderr); // ensure debug output appears before stdout*/
    } else {
        print_error("Parse failed — null AST");
        kalidous_arena_destroy(arena);
        return 1;
    }

    // TODO: kalidous_sema(arena, ast) — name resolution, types, borrow checker
    // TODO: report errors/warnings with full span (line:col start+end)

    kalidous_arena_destroy(arena);
    print_success("Check passed", src);
    return 0;
}

// compile — check + gera objeto nativo (.o) ou bytecode (.nbc), sem linkar
static int cmd_compile(const std::string& input_file,
                       const std::string& output_file,
                       const std::string& mode_str,
                       bool interpreted,
                       bool verbose,
                       const std::vector<std::string>& include_dirs) {
    if (verbose) {
        const std::string kind = interpreted ? "bytecode" : "LLVM IR / native object";
        print_info("Compiling '" + input_file + "' → " + kind + " (" + mode_str + ")");
    }

    KalidousTokenStream stream{};
    const char* source   = nullptr;
    size_t      src_size = 0;
    KalidousArena* arena = tokenize_file(input_file, stream, &source, &src_size, verbose);
    if (!arena) return 1;

    // TODO: kalidous_parse(arena, stream)  → AST
    // TODO: kalidous_sema(arena, ast)       → AST anotada

    // TODO [LLVM — caminho nativo]:
    //   - Inicializar LLVMContext + LLVMModule + LLVMBuilder
    //   - Percorrer AST e emitir LLVMValueRef por nó (codegen)
    //   - Aplicar passes de optimização via LLVMPassManager (opt_level 0–3)
    //   - LLVMTargetMachineEmitToFile → .o  (ou .ll se --emit ir)
    //   - Se --emit asm → emitir .s via LLVMAssemblyFile
    //   - Considerar llvm::orc::LLJIT para JIT futuro (REPL)

    // TODO [Bytecode — caminho interpretado]:
    //   - Definir formato .nbc:
    //       header: magic + versão + nº de constantes + nº de instruções
    //       pool de constantes (strings, números)
    //       array de instruções (opcode 1 byte + operandos variáveis)
    //   - kalidous_bytecode_emit(arena, ast) → KalidousBytecode*
    //   - Serializar para ficheiro .nbc

    // TODO: usar include_dirs para resolver imports no parse/sema

    kalidous_arena_destroy(arena);

    const std::string out = !output_file.empty() ? output_file
                                                  : (interpreted ? "a.nbc" : "a.o");
    print_success(interpreted ? "Bytecode compile" : "Compile", out);
    return 0;
}

// build — compile (nativo) + link → binário final
static int cmd_build(const std::string& input_file,
                     const std::string& output_file,
                     const std::string& mode_str,
                     bool verbose,
                     const std::vector<std::string>& include_dirs) {
    std::string src  = input_file;
    std::string out  = output_file;
    std::string mode = mode_str;
    std::vector<std::string> extra_includes = include_dirs;

    if (src.empty()) {
        KalidousProject proj;
        if (!try_load_project(proj)) {
            print_error("No input file and no KalidousProject.toml found");
            return 1;
        }
        src = proj.entry;
        if (out.empty())     out  = proj.output;
        if (mode == "debug") mode = proj.mode;

        // Herdar configurações do projecto
        extra_includes.insert(extra_includes.end(),
                              proj.include_dirs.begin(), proj.include_dirs.end());

        // TODO: propagar proj.lib_paths, proj.link_libs, proj.link_flags
        // TODO: aplicar proj.lto se mode == "release"
        // TODO: aplicar proj.opt_level / proj.debug_level ao LLVMTargetMachine
        // TODO: criar proj.bin_dir e proj.cache_dir se não existirem
    }

    if (verbose) print_info("Building '" + src + "' → binary (" + mode + ")");

    // compile: nativo apenas (interpreted = false)
    if (const int rc = cmd_compile(src, "", mode, /*interpreted=*/false,
                                   verbose, extra_includes); rc != 0) return rc;

    // TODO: invocar linker (LLD embutido ou system linker como fallback)
    //   - recolher todos os .o (suporte multi-ficheiro no futuro)
    //   - adicionar lib_paths e link_libs do projecto
    //   - produzir binário final em `out`
    // TODO: strip de debug se proj.strip_debug == true

    const std::string binary = out.empty() ? "a.out" : out;
    print_success("Build", binary);
    return 0;
}

// execute — executa um binário ou bytecode já existente
static int cmd_execute(const std::string& target, bool interpreted, bool verbose) {
    std::string bin = target;

    if (bin.empty()) {
        KalidousProject proj;
        if (!try_load_project(proj)) {
            print_error("No target specified and no KalidousProject.toml found");
            return 1;
        }
        bin = interpreted ? (proj.output + ".nbc") : proj.output;
    }

    if (!kalidous_file_exists(bin.c_str())) {
        const std::string hint = interpreted ? "compile --interpreted" : "build";
        print_error("Target not found: '" + bin +
                    "' -- did you run 'kalidous " + hint + "' first?");
        return 1;
    }

    if (verbose)
        print_info("Executing '" + bin + "'" + (interpreted ? " (interpreted)" : ""));

    // TODO [nativo]: execv (POSIX) / CreateProcess (Windows)
    //   - passar argv restantes ao processo filho
    //   - retornar exit code do processo filho

    // TODO [interpretado / .nbc]:
    //   - kalidous_vm_create() → KalidousVM*
    //   - kalidous_vm_load(vm, bin)
    //   - kalidous_vm_run(vm) → int exit_code
    //   - kalidous_vm_destroy(vm)

    print_not_implemented("execute");
    return 1;
}

// run — build/compile + execute numa só invocação
static int cmd_run(const std::string& input_file,
                   const std::string& output_file,
                   const std::string& mode_str,
                   bool interpreted,
                   bool verbose,
                   const std::vector<std::string>& include_dirs) {
    if (interpreted) {
        const std::string bc = output_file.empty() ? "a.nbc" : output_file;
        if (const int rc = cmd_compile(input_file, bc, mode_str,
                                       /*interpreted=*/true, verbose, include_dirs); rc != 0)
            return rc;
        return cmd_execute(bc, /*interpreted=*/true, verbose);
    }

    if (const int rc = cmd_build(input_file, output_file, mode_str,
                                  verbose, include_dirs); rc != 0) return rc;

    const std::string binary = output_file.empty() ? "a.out" : output_file;
    return cmd_execute(binary, /*interpreted=*/false, verbose);
}

static int cmd_test(const std::string& /*input_file*/, bool /*verbose*/) {
    // TODO: sintaxe de testes: #[test] fn my_test() { ... }
    // TODO: descobrir automaticamente ficheiros *_test.kalidous em test_dir
    // TODO: compilar e executar cada teste isoladamente
    // TODO: reportar: passed / failed / ignored com tempo de execução
    // TODO: filtro por nome: kalidous test foo::bar
    print_not_implemented("test");
    return 1;
}

static int cmd_fmt(const std::string& /*input_file*/, bool /*check_only*/, bool /*verbose*/) {
    // TODO: pretty-printer sobre a AST (canonical form)
    // TODO: suporte a ficheiro único ou directório recursivo
    // TODO: --check → não modifica, retorna 1 se algum ficheiro difere (CI)
    // TODO: .kalidous_fmt para config de estilo
    print_not_implemented("fmt");
    return 1;
}

static int cmd_docs(const std::string& /*input_file*/,
                    const std::string& /*output_dir*/, bool /*verbose*/) {
    // TODO: extrair doc-comments (/// ou /** */) durante o parse
    // TODO: gerar HTML estático ou Markdown por módulo/função/tipo
    // TODO: suporte a exemplos embutidos nos doc-comments (runnable snippets)
    print_not_implemented("docs");
    return 1;
}

static int cmd_repl(bool /*verbose*/) {
    // TODO: llvm::orc::LLJIT para JIT compilation
    // TODO: readline / linenoise — input com histórico
    // TODO: compilar cada linha/bloco incrementalmente
    // TODO: manter contexto de variáveis entre expressões
    // TODO: comandos especiais: :quit, :type <expr>, :help, :load <file>
    print_not_implemented("repl");
    return 1;
}

static int cmd_version() {
    std::cout << "Kalidous Programming Language\n"
              << "Version:  " << kalidous_version   << "\n"
              << "Compiler: " << __VERSION__         << "\n";
    // TODO: LLVMGetVersion() quando o backend estiver linkado
    // TODO: LLVMGetDefaultTargetTriple() para mostrar o host target
    return 0;
}

static int cmd_help() {
    std::cout << R"(Kalidous - A low-level general-purpose language

USAGE:
    kalidous [OPTIONS] <COMMAND> [ARGS]

COMMANDS:
    check      Parse and type-check; report errors only, no output
    compile    Compile to object file or bytecode, no linking
    build      Compile and link to native binary  (reads KalidousProject.toml)
    execute    Run an existing binary or bytecode (reads KalidousProject.toml)
    run        Build then execute                 (reads KalidousProject.toml)
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
        --interpreted                           Use bytecode path instead of native
        --emit <ast|ir|asm|obj|bin>             Emit intermediate representation
        --target <TRIPLE>                       Target triple (e.g. x86_64-linux-gnu)
    -v, --verbose                               Verbose output
    -h, --help                                  Show help

PIPELINE:
    check  <  compile  <  build
    execute             <  run
    run --interpreted  =  compile --interpreted + execute --interpreted

EXAMPLES:
    kalidous check main.kalidous
    kalidous compile main.kalidous -o main.o
    kalidous compile --interpreted main.kalidous -o main.nbc
    kalidous build
    kalidous build main.kalidous -o bin/app -m release
    kalidous execute
    kalidous execute --interpreted
    kalidous run
    kalidous run main.kalidous -m release
    kalidous run --interpreted main.kalidous

LEARN MORE:
    Source: https://github.com/GalaxyHaze/Kalidous-lang
    Docs:   https://galaxyhaze.github.io/Kalidous-Lang/kalidous-docs.html
)";
    return 0;
}

// ============================================================================
// Entry point (C API)
// ============================================================================

extern "C" int kalidous_run(int argc, const char* const argv[]) {
    CLI::App app{"Kalidous - A low-level general-purpose language"};
    app.require_subcommand(0, 1);

    // -- Opções globais -------------------------------------------------------
    std::string              mode_str = "debug";
    std::string              output_file;
    std::vector<std::string> include_dirs;
    std::string              emit_target;
    std::string              target_triple;
    bool                     verbose = false;

    app.add_option("-m,--mode",    mode_str,     "Build mode: debug, dev, release, fast, test")
       ->transform(CLI::IsMember({"debug", "dev", "release", "fast", "test"}))
       ->default_str("debug");
    app.add_option("-o,--output",  output_file,  "Output file path");
    app.add_option("-I,--include", include_dirs, "Include directories (repeatable)");
    app.add_option("--emit",       emit_target,  "Emit: ast, ir, asm, obj, bin");
    app.add_option("--target",     target_triple,"Target triple");
    app.add_flag  ("-v,--verbose", verbose,      "Verbose output");

    // TODO: propagar emit_target e target_triple para cmd_compile / cmd_build
    // TODO: validar target_triple contra os targets suportados pelo LLVM linkado

    // -- Subcomandos ----------------------------------------------------------
    std::string input_file;
    bool        interpreted = false;
    bool        fmt_check   = false;
    std::string docs_output = "docs";

    auto* check_cmd = app.add_subcommand("check", "Parse and type-check only");
    check_cmd->add_option("input", input_file, "Source file [optional, reads toml if omitted]")
             ->check(CLI::ExistingFile);

    auto* compile_cmd = app.add_subcommand("compile", "Compile to object/bytecode, no linking");
    compile_cmd->add_option("input", input_file, "Source file (.kalidous)")
               ->required()->check(CLI::ExistingFile);
    compile_cmd->add_flag("--interpreted", interpreted, "Compile to bytecode instead of native");

    auto* build_cmd = app.add_subcommand("build", "Compile and link to native binary");
    build_cmd->add_option("input", input_file, "Source file [optional, reads toml if omitted]")
             ->check(CLI::ExistingFile);

    auto* execute_cmd = app.add_subcommand("execute", "Run existing binary or bytecode");
    execute_cmd->add_option("target", input_file, "Binary or bytecode [optional, reads toml if omitted]");
    execute_cmd->add_flag("--interpreted", interpreted, "Run bytecode instead of native binary");

    auto* run_cmd = app.add_subcommand("run", "Build then execute");
    run_cmd->add_option("input", input_file, "Source file [optional, reads toml if omitted]")
           ->check(CLI::ExistingFile);
    run_cmd->add_flag("--interpreted", interpreted, "Compile to bytecode and run interpreted");

    auto* test_cmd = app.add_subcommand("test", "Run tests in source");
    test_cmd->add_option("input", input_file, "Source file (.kalidous)")->check(CLI::ExistingFile);

    auto* fmt_cmd = app.add_subcommand("fmt", "Format source code");
    fmt_cmd->add_option("input", input_file, "Source file or directory")->required();
    fmt_cmd->add_flag("--check", fmt_check,  "Check formatting only, do not modify files");

    auto* docs_cmd = app.add_subcommand("docs", "Generate documentation");
    docs_cmd->add_option("input",      input_file,  "Source file (.kalidous)")->check(CLI::ExistingFile);
    docs_cmd->add_option("-o,--output",docs_output, "Output directory")->default_str("docs");

    auto* repl_cmd    = app.add_subcommand("repl",    "Start interactive REPL");
    auto* version_cmd = app.add_subcommand("version", "Show version information");
    auto* help_cmd    = app.add_subcommand("help",    "Show help message");

    // -- Parse ----------------------------------------------------------------
    try {
        app.parse(argc, argv);
    } catch (const CLI::CallForHelp&) {
        return cmd_help();
    } catch (const CLI::CallForVersion&) {
        return cmd_version();
    } catch (const CLI::ParseError& e) {
        if (e.get_name() == "ExtrasError" && argc > 1 && std::string(argv[1]) == "help")
            return cmd_help();
        std::cerr << "[error] " << e.what() << "\n\n" << app.help();
        return 1;
    }

    // -- Dispatch -------------------------------------------------------------
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
    KalidousProject proj;
    if (try_load_project(proj))
        return cmd_build("", "", mode_str, verbose, include_dirs);

    return cmd_help();
}