// zith_cli.cpp — CLI entry point for the Zith compiler
//
// Requires C++17 (structured bindings, if-initializers).
// Depends on: CLI11, Zith/zith.h

#include <CLI/CLI.hpp>
#include <zith/zith.hpp>
#include <ankerl/unordered_dense.h>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include "../lexer/debug.h"
#include "../ast/ast.h"

static const char *zith_version = ZITH_VERSION;

// ============================================================================
// Output helpers
// ============================================================================

static void print_error(const std::string &msg) {
    std::cerr << "[error] " << msg << "\n";
}

static void print_info(const std::string &msg) {
    std::cout << "[*] " << msg << "\n";
}

static void print_success(const std::string &action, const std::string &target) {
    std::cout << "[ok] " << action << ": " << target << "\n";
}

static void print_not_implemented(const std::string &command) {
    std::cerr << "\n[!] Command '" << command << "' is not implemented yet.\n"
            << "    This feature will be available in a future version of Zith.\n"
            << "    Track progress at: https://github.com/GalaxyHaze/Zith\n\n";
}

// ============================================================================
// ZithProject — representa o ZithProject.toml
// ============================================================================

struct ZithProject {
    // -- Identidade --
    std::string name = "project";
    std::string version = "0.1.0";
    std::string description;
    std::string authors;
    std::string license;
    std::string homepage;

    // -- Compilação --
    std::string entry = "src/main.zith";
    std::string output = "bin/project";
    std::string mode = "debug";
    std::string target_triple;
    std::string edition = "2024";

    // -- Diretórios --
    std::string src_dir = "src";
    std::string bin_dir = "bin";
    std::string lib_dir = "lib";
    std::string docs_dir = "docs";
    std::string test_dir = "examples";
    std::string cache_dir = ".zith_cache";

    // -- Includes & Links --
    std::vector<std::string> include_dirs;
    std::vector<std::string> lib_paths;
    std::vector<std::string> link_libs;
    std::vector<std::string> link_flags;

    // -- Features & Dependências --
    std::vector<std::string> features;
    std::vector<std::string> dependencies; // TODO: formato "libname@1.0" a definir

    // -- Comportamento --
    bool emit_ir = false;
    bool emit_asm = false;
    bool strip_debug = false;
    bool lto = false;
    int opt_level = 0; // 0–3, mapeado para LLVM opt passes
    int debug_level = 2; // 0–3, mapeado para DWARF debug info
};

// Retorna false se ZithProject.toml não existir ou falhar ao ler.
// Preenche 'proj' com defaults enquanto o parser TOML não está implementado.
static bool try_load_project(ZithProject &proj) {
    if (!zith_file_exists("ZithProject.toml")) return false;

    // TODO: integrar toml++ (https://github.com/marzer/tomlplusplus)
    // TODO: validar campos obrigatórios: name, version, entry
    // TODO: reportar campos desconhecidos como warning
    // TODO: suportar herança de perfis, ex: [profile.release] opt_level = 3
    // TODO: suportar array de targets para cross-compilation
    // TODO: resolver paths relativos à localização do .toml

    proj = ZithProject{}; // garante defaults mesmo que a leitura seja parcial
    return true;
}

// ============================================================================
// Pipeline
// ============================================================================

// Tokeniza 'src_path' numa arena nova.
// Preenche 'out_stream', 'out_source' e 'out_source_len' — o source fica na
// arena para ser reutilizado pelo parser sem second load.
// Retorna a arena (o chamador destrói); nullptr em caso de erro.
static ZithArena *tokenize_file(const std::string &src_path,
                                    ZithTokenStream &out_stream,
                                    const char **out_source,
                                    size_t *out_source_len,
                                    bool verbose) {
    ZithArena *arena = zith_arena_create(64 * 1024);
    if (!arena) {
        print_error("Failed to create memory arena");
        return nullptr;
    }

    size_t file_size = 0;
    const char *source = zith_load_file_to_arena(arena, src_path.c_str(), &file_size);
    if (!source) {
        print_error("Failed to load file: " + src_path);
        zith_arena_destroy(arena);
        return nullptr;
    }

    out_stream = zith_tokenize(arena, source, file_size);
    if (!out_stream.data) {
        zith_arena_destroy(arena);
        return nullptr;
    }

    if (verbose)
        print_info("Tokenized " + std::to_string(out_stream.len) + " tokens from " + src_path);

    // Return source pointer — already in arena, no extra allocation needed
    if (out_source) *out_source = source;
    if (out_source_len) *out_source_len = file_size;

    return arena;
}

enum class RtValKind { Void, Int, Float, String, Bool };
struct RtValue {
    RtValKind kind = RtValKind::Void;
    int64_t i = 0;
    double f = 0.0;
    std::string s;
    bool b = false;
};

struct RtContext {
    ankerl::unordered_dense::map<std::string, ZithFuncPayload *> funcs;
    std::vector<ankerl::unordered_dense::map<std::string, RtValue>> scopes;
};

static RtValue eval_expr(RtContext &ctx, ZithNode *expr);
static RtValue exec_block(RtContext &ctx, ZithNode *block);

static bool is_truthy(const RtValue &v) {
    switch (v.kind) {
        case RtValKind::Bool: return v.b;
        case RtValKind::Int: return v.i != 0;
        case RtValKind::Float: return v.f != 0.0;
        case RtValKind::String: return !v.s.empty();
        default: return false;
    }
}

static RtValue lookup_var(RtContext &ctx, const std::string &name) {
    for (auto it = ctx.scopes.rbegin(); it != ctx.scopes.rend(); ++it) {
        auto f = it->find(name);
        if (f != it->end()) return f->second;
    }
    return {};
}

static RtValue eval_call(RtContext &ctx, ZithCallPayload *call) {
    if (!call || !call->callee || call->callee->type != ZITH_NODE_IDENTIFIER) return {};
    const std::string cname(call->callee->data.ident.str, call->callee->data.ident.len);
    if (cname == "print" || cname == "println") {
        for (size_t i = 0; i < call->arg_count; ++i) {
            RtValue arg = eval_expr(ctx, call->args[i]);
            if (arg.kind == RtValKind::String) std::cout << arg.s;
            else if (arg.kind == RtValKind::Int) std::cout << arg.i;
            else if (arg.kind == RtValKind::Float) std::cout << arg.f;
            else if (arg.kind == RtValKind::Bool) std::cout << (arg.b ? "true" : "false");
        }
        if (cname == "println") std::cout << "\n";
        return {};
    }
    auto fn_it = ctx.funcs.find(cname);
    if (fn_it == ctx.funcs.end()) return {};
    auto *fn = fn_it->second;
    ctx.scopes.push_back({});
    for (size_t i = 0; i < fn->param_count && i < call->arg_count; ++i) {
        auto *param = static_cast<ZithParamPayload *>(fn->params[i]->data.list.ptr);
        ctx.scopes.back()[std::string(param->name, param->name_len)] = eval_expr(ctx, call->args[i]);
    }
    RtValue ret = exec_block(ctx, fn->body);
    ctx.scopes.pop_back();
    return ret;
}

static RtValue eval_expr(RtContext &ctx, ZithNode *expr) {
    if (!expr) return {};
    switch (expr->type) {
        case ZITH_NODE_LITERAL: {
            auto *lit = static_cast<ZithLiteral *>(expr->data.list.ptr);
            RtValue v{};
            if (!lit) return v;
            if (lit->kind == ZITH_LIT_INT || lit->kind == ZITH_LIT_UINT) { v.kind = RtValKind::Int; v.i = lit->value.i64; }
            else if (lit->kind == ZITH_LIT_FLOAT) { v.kind = RtValKind::Float; v.f = lit->value.f64; }
            else if (lit->kind == ZITH_LIT_STRING) { v.kind = RtValKind::String; v.s.assign(lit->value.string.ptr, lit->value.string.len); }
            else if (lit->kind == ZITH_LIT_BOOL) { v.kind = RtValKind::Bool; v.b = lit->value.boolean; }
            return v;
        }
        case ZITH_NODE_IDENTIFIER:
            return lookup_var(ctx, std::string(expr->data.ident.str, expr->data.ident.len));
        case ZITH_NODE_CALL:
            return eval_call(ctx, static_cast<ZithCallPayload *>(expr->data.list.ptr));
        case ZITH_NODE_BINARY_OP: {
            RtValue a = eval_expr(ctx, expr->data.kids.a);
            RtValue b = eval_expr(ctx, expr->data.kids.c);
            RtValue out{};
            if (expr->data.list.len == ZITH_TOKEN_PLUS && a.kind == RtValKind::String && b.kind == RtValKind::String) {
                out.kind = RtValKind::String; out.s = a.s + b.s; return out;
            }
            if (a.kind == RtValKind::Int && b.kind == RtValKind::Int) {
                out.kind = RtValKind::Int;
                switch (expr->data.list.len) {
                    case ZITH_TOKEN_PLUS: out.i = a.i + b.i; break;
                    case ZITH_TOKEN_MINUS: out.i = a.i - b.i; break;
                    case ZITH_TOKEN_MULTIPLY: out.i = a.i * b.i; break;
                    case ZITH_TOKEN_DIVIDE: out.i = (b.i == 0 ? 0 : a.i / b.i); break;
                    case ZITH_TOKEN_EQUAL: out.kind = RtValKind::Bool; out.b = (a.i == b.i); break;
                    case ZITH_TOKEN_NOT_EQUAL: out.kind = RtValKind::Bool; out.b = (a.i != b.i); break;
                    case ZITH_TOKEN_LESS_THAN: out.kind = RtValKind::Bool; out.b = (a.i < b.i); break;
                    case ZITH_TOKEN_LESS_THAN_OR_EQUAL: out.kind = RtValKind::Bool; out.b = (a.i <= b.i); break;
                    case ZITH_TOKEN_GREATER_THAN: out.kind = RtValKind::Bool; out.b = (a.i > b.i); break;
                    case ZITH_TOKEN_GREATER_THAN_OR_EQUAL: out.kind = RtValKind::Bool; out.b = (a.i >= b.i); break;
                    default: break;
                }
            }
            return out;
        }
        case ZITH_NODE_UNARY_OP: {
            RtValue val = eval_expr(ctx, expr->data.kids.a);
            if (expr->data.list.len == ZITH_TOKEN_BANG) {
                RtValue out{};
                out.kind = RtValKind::Bool;
                out.b = !is_truthy(val);
                return out;
            }
            return val;
        }
        default:
            return {};
    }
}

static RtValue exec_stmt(RtContext &ctx, ZithNode *stmt, bool &did_return) {
    if (!stmt) return {};
    switch (stmt->type) {
        case ZITH_NODE_VAR_DECL: {
            auto *var = static_cast<ZithVarPayload *>(stmt->data.list.ptr);
            ctx.scopes.back()[std::string(var->name, var->name_len)] = eval_expr(ctx, var->initializer);
            return {};
        }
        case ZITH_NODE_RETURN:
            did_return = true;
            return eval_expr(ctx, stmt->data.kids.a);
        case ZITH_NODE_IF: {
            RtValue cond = eval_expr(ctx, stmt->data.kids.a);
            if (is_truthy(cond)) return exec_block(ctx, stmt->data.kids.b);
            if (stmt->data.kids.c) return exec_block(ctx, stmt->data.kids.c);
            return {};
        }
        case ZITH_NODE_FOR: {
            auto *payload = static_cast<ZithForPayload *>(stmt->data.list.ptr);
            while (true) {
                if (payload && payload->condition) {
                    RtValue cond = eval_expr(ctx, payload->condition);
                    if (!is_truthy(cond)) break;
                }
                RtValue loop_ret = exec_block(ctx, payload ? payload->body : nullptr);
                if (loop_ret.kind != RtValKind::Void) {
                    did_return = true;
                    return loop_ret;
                }
                if (!payload || !payload->condition) break;
            }
            return {};
        }
        case ZITH_NODE_CALL:
        case ZITH_NODE_BINARY_OP:
        case ZITH_NODE_UNARY_OP:
        case ZITH_NODE_IDENTIFIER:
        case ZITH_NODE_LITERAL:
            (void)eval_expr(ctx, stmt);
            return {};
        case ZITH_NODE_BLOCK:
            return exec_block(ctx, stmt);
        default:
            return {};
    }
}

static RtValue exec_block(RtContext &ctx, ZithNode *block) {
    RtValue ret{};
    if (!block || block->type != ZITH_NODE_BLOCK) return ret;
    auto **stmts = static_cast<ZithNode **>(block->data.list.ptr);
    for (size_t i = 0; i < block->data.list.len; ++i) {
        bool did_return = false;
        RtValue v = exec_stmt(ctx, stmts[i], did_return);
        if (did_return) return v;
    }
    return ret;
}

static int run_interpreted_ast(ZithNode *ast) {
    if (!ast || ast->type != ZITH_NODE_PROGRAM) return 1;
    RtContext ctx{};
    auto **decls = static_cast<ZithNode **>(ast->data.list.ptr);
    for (size_t i = 0; i < ast->data.list.len; ++i) {
        if (decls[i] && decls[i]->type == ZITH_NODE_FUNC_DECL) {
            auto *fn = static_cast<ZithFuncPayload *>(decls[i]->data.list.ptr);
            ctx.funcs[std::string(fn->name, fn->name_len)] = fn;
        }
    }
    auto it = ctx.funcs.find("main");
    if (it == ctx.funcs.end()) {
        print_error("No 'main' function found for interpreted execution");
        return 1;
    }
    ctx.scopes.push_back({});
    exec_block(ctx, it->second->body);
    ctx.scopes.pop_back();
    return 0;
}

// ============================================================================
// Comandos
// ============================================================================

// check — parse + semântica, só reporta erros, não produz artefacto
static int cmd_check(const std::string &input_file,
                     const std::string &mode_str, bool verbose) {
    std::string src = input_file;

    if (src.empty()) {
        ZithProject proj;
        if (!try_load_project(proj)) {
            print_error("No input file and no ZithProject.toml found");
            return 1;
        }
        src = proj.entry;
    }

    if (verbose) print_info("Checking '" + src + "' in " + mode_str + " mode...");

    ZithTokenStream stream{};
    const char *source = nullptr;
    size_t src_size = 0;
    ZithArena *arena = tokenize_file(src, stream, &source, &src_size, verbose);
    if (!arena) return 1;

    zith_debug_tokens(stream.data, stream.len);

    ZithNode *ast = zith_parse_with_source(arena,
                                                   source, src_size,
                                                   src.c_str(), stream);

    // Debug: AST dump
    if (verbose){
        if (ast) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            printf("\n\n\n-Starting AST\n");
            zith_ast_print(ast, 0);
            printf("Ending AST\n\n\n");
        } else {
            print_error("Parse failed — null AST\n\n");
            zith_arena_destroy(arena);
            return 1;
        }
}

    if (!ast) {
        zith_arena_destroy(arena);
        return 1;
    }

    zith_arena_destroy(arena);
    print_success("Check passed", src);
    return 0;
}

// compile — check + gera objeto nativo (.o) ou bytecode (.nbc), sem linkar
static int cmd_compile(const std::string &input_file,
                       const std::string &output_file,
                       const std::string &mode_str,
                       bool interpreted,
                       bool verbose,
                       const std::vector<std::string> &include_dirs) {
    (void)include_dirs;
    if (verbose) {
        const std::string kind = interpreted ? "bytecode" : "LLVM IR / native object";
        print_info("Compiling '" + input_file + "' → " + kind + " (" + mode_str + ")");
    }

    ZithTokenStream stream{};
    const char *source = nullptr;
    size_t src_size = 0;
    ZithArena *arena = tokenize_file(input_file, stream, &source, &src_size, verbose);
    if (!arena) return 1;

    ZithNode *ast = zith_parse_with_source(arena, source, src_size, input_file.c_str(), stream);
    if (!ast) {
        zith_arena_destroy(arena);
        return 1;
    }

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
    //   - zith_bytecode_emit(arena, ast) → ZithBytecode*
    //   - Serializar para ficheiro .nbc

    // TODO: usar include_dirs para resolver imports no parse/sema

    const std::string out = !output_file.empty()
                                ? output_file
                                : (interpreted ? "a.nbc" : "a.o");
    if (interpreted) {
        std::ofstream ofs(out, std::ios::binary);
        if (!ofs) {
            print_error("Failed to write bytecode file: " + out);
            zith_arena_destroy(arena);
            return 1;
        }
        ofs << source;
    }
    zith_arena_destroy(arena);
    print_success(interpreted ? "Bytecode compile" : "Compile", out);
    return 0;
}

// build — compile (nativo) + link → binário final
static int cmd_build(const std::string &input_file,
                     const std::string &output_file,
                     const std::string &mode_str,
                     bool verbose,
                     const std::vector<std::string> &include_dirs) {
    std::string src = input_file;
    std::string out = output_file;
    std::string mode = mode_str;
    std::vector<std::string> extra_includes = include_dirs;

    if (src.empty()) {
        ZithProject proj;
        if (!try_load_project(proj)) {
            print_error("No input file and no ZithProject.toml found");
            return 1;
        }
        src = proj.entry;
        if (out.empty()) out = proj.output;
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
                                   verbose, extra_includes); rc != 0)
        return rc;

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
static int cmd_execute(const std::string &target, bool interpreted, bool verbose) {
    std::string bin = target;

    if (bin.empty()) {
        ZithProject proj;
        if (!try_load_project(proj)) {
            print_error("No target specified and no ZithProject.toml found");
            return 1;
        }
        bin = interpreted ? (proj.output + ".nbc") : proj.output;
    }

    if (!zith_file_exists(bin.c_str())) {
        const std::string hint = interpreted ? "compile --interpreted" : "build";
        print_error("Target not found: '" + bin +
                    "' -- did you run 'zith " + hint + "' first?");
        return 1;
    }

    if (verbose)
        print_info("Executing '" + bin + "'" + (interpreted ? " (interpreted)" : ""));

    // TODO [nativo]: execv (POSIX) / CreateProcess (Windows)
    //   - passar argv restantes ao processo filho
    //   - retornar exit code do processo filho

    if (interpreted) {
        std::ifstream ifs(bin, std::ios::binary);
        if (!ifs) {
            print_error("Failed to read bytecode file: " + bin);
            return 1;
        }
        std::string source((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        ZithArena *arena = zith_arena_create(64 * 1024);
        if (!arena) return 1;
        ZithTokenStream stream = zith_tokenize(arena, source.c_str(), source.size());
        if (!stream.data) {
            zith_arena_destroy(arena);
            return 1;
        }
        ZithNode *ast = zith_parse_with_source(arena, source.c_str(), source.size(), bin.c_str(), stream);
        if (!ast) {
            zith_arena_destroy(arena);
            return 1;
        }
        const int rc = run_interpreted_ast(ast);
        zith_arena_destroy(arena);
        return rc;
    }
    print_not_implemented("execute");
    return 1;
}

// run — build/compile + execute numa só invocação
static int cmd_run(const std::string &input_file,
                   const std::string &output_file,
                   const std::string &mode_str,
                   bool interpreted,
                   bool verbose,
                   const std::vector<std::string> &include_dirs) {
    if (interpreted) {
        const std::string bc = output_file.empty() ? "a.nbc" : output_file;
        if (const int rc = cmd_compile(input_file, bc, mode_str,
                                       /*interpreted=*/true, verbose, include_dirs); rc != 0)
            return rc;
        return cmd_execute(bc, /*interpreted=*/true, verbose);
    }

    if (const int rc = cmd_build(input_file, output_file, mode_str,
                                 verbose, include_dirs); rc != 0)
        return rc;

    const std::string binary = output_file.empty() ? "a.out" : output_file;
    return cmd_execute(binary, /*interpreted=*/false, verbose);
}

static int cmd_test(const std::string & /*input_file*/, bool /*verbose*/) {
    // TODO: sintaxe de testes: #[test] fn my_test() { ... }
    // TODO: descobrir automaticamente ficheiros *_test.zith em test_dir
    // TODO: compilar e executar cada teste isoladamente
    // TODO: reportar: passed / failed / ignored com tempo de execução
    // TODO: filtro por nome: zith test foo::bar
    print_not_implemented("test");
    return 1;
}

static int cmd_fmt(const std::string & /*input_file*/, bool /*check_only*/, bool /*verbose*/) {
    // TODO: pretty-printer sobre a AST (canonical form)
    // TODO: suporte a ficheiro único ou directório recursivo
    // TODO: --check → não modifica, retorna 1 se algum ficheiro difere (CI)
    // TODO: .zith_fmt para config de estilo
    print_not_implemented("fmt");
    return 1;
}

static int cmd_docs(const std::string & /*input_file*/,
                    const std::string & /*output_dir*/, bool /*verbose*/) {
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

static int cmd_new(const std::string & /*project_name*/, bool /*verbose*/) {
    // TODO: criar ZithProject.toml com defaults
    // TODO: criar directorios: src/, lib/, examples/
    // TODO: criar ficheiro src/main.zith com template
    print_not_implemented("new");
    return 1;
}

static int cmd_clean(bool /*verbose*/) {
    // TODO: remover bin_dir (bin/)
    // TODO: remover cache_dir (.zith_cache/)
    // TODO: remover ficheiros .o, .nbc, .ll, .s
    // TODO: opção --frozen para remover tudo
    print_not_implemented("clean");
    return 1;
}

#if defined(__VERSION__)
    std::string compiler = __VERSION__;
#elif defined(_MSC_FULL_VER)
    std::string compiler = "MSVC " + std::to_string(_MSC_FULL_VER);
#else
    std::string compiler = "unknown compiler";
#endif

static int cmd_version() {
    std::cout << "Zith Programming Language\n"
            << "Version:  " << zith_version << "\n"
            << "Compiler: " << compiler << "\n";
    // TODO: LLVMGetVersion() quando o backend estiver linkado
    // TODO: LLVMGetDefaultTargetTriple() para mostrar o host target
    return 0;
}

static int cmd_help() {
    std::cout << R"(Zith - A low-level general-purpose language

USAGE:
    zith [OPTIONS] <COMMAND> [ARGS]

COMMANDS:
    build        Compile and link to native binary (reads ZithProject.toml)
    run          Build then execute (reads ZithProject.toml)
    check        Parse and type-check; report errors only, no output
    compile      Compile to object file or bytecode, no linking
    execute      Run an existing binary or bytecode (reads ZithProject.toml)
    test         Run examples in source
    repl         Start interactive REPL
    new          Create a new project
    fmt          Format source code
    docs         Generate documentation
    clean        Remove build artifacts
    version      Show version information
    help         Show this help message

OPTIONS:
    -m, --mode <debug|dev|release|fast|test>    Build mode [default: debug]
    -o, --output <FILE>                         Output file path
    -I, --include <DIR>                         Add include directory (repeatable)
        --interpreted                           Use bytecode path instead of native
        --emit <ast|ir|asm|obj|bin>             Emit intermediate representation
        --target <TRIPLE>                       Target triple (e.g. x86_64-linux-gnu)
    -s, --strict                                Apply stricter rules to the compiler
    -v, --verbose                               Use verbose output
    -c, --color <auto|on|off>                   Set color output [default: auto]
    -h, --help                                  Show help

PIPELINE:
    check  <  compile  <  build
    execute            <  run
    run --interpreted  =  compile --interpreted + execute --interpreted

EXAMPLES:
    zith build
    zith run main.zith -m release
    zith compile --interpreted main.zith -o main.nbc
    zith execute --interpreted

LEARN MORE:
    Source: https://github.com/GalaxyHaze/Zith
    Docs:   https://zith.run.place)";
    return 0;
}

// ============================================================================
// Entry point (C API)
// ============================================================================

extern "C" int zith_run(int argc, const char *const argv[]) {
    CLI::App app{"Zith - A low-level general-purpose language"};
    app.require_subcommand(0, 1);

    // -- Opções globais -------------------------------------------------------
    std::string mode_str = "debug";
    std::string output_file;
    std::vector<std::string> include_dirs;
    std::string emit_target;
    std::string target_triple;
    bool verbose = false;

    app.add_option("-m,--mode", mode_str, "Build mode: debug, dev, release, fast, test")
            ->transform(CLI::IsMember({"debug", "dev", "release", "fast", "test"}))
            ->default_str("debug");
    app.add_option("-o,--output", output_file, "Output file path");
    app.add_option("-I,--include", include_dirs, "Include directories (repeatable)");
    app.add_option("--emit", emit_target, "Emit: ast, ir, asm, obj, bin");
    app.add_option("--target", target_triple, "Target triple");
    app.add_flag("-v,--verbose", verbose, "Verbose output");

    // TODO: propagar emit_target e target_triple para cmd_compile / cmd_build
    // TODO: validar target_triple contra os targets suportados pelo LLVM linkado

    // -- Subcomandos ----------------------------------------------------------
    std::string input_file;
    bool interpreted = false;
    bool fmt_check = false;
    std::string docs_output = "docs";

    auto *check_cmd = app.add_subcommand("check", "Parse and type-check only");
    check_cmd->add_option("input", input_file, "Source file [optional, reads toml if omitted]")
            ->check(CLI::ExistingFile);

    auto *compile_cmd = app.add_subcommand("compile", "Compile to object/bytecode, no linking");
    compile_cmd->add_option("input", input_file, "Source file (.zith)")
            ->required()->check(CLI::ExistingFile);
    compile_cmd->add_flag("--interpreted", interpreted, "Compile to bytecode instead of native");

    auto *build_cmd = app.add_subcommand("build", "Compile and link to native binary");
    build_cmd->add_option("input", input_file, "Source file [optional, reads toml if omitted]")
            ->check(CLI::ExistingFile);

    auto *execute_cmd = app.add_subcommand("execute", "Run existing binary or bytecode");
    execute_cmd->add_option("target", input_file, "Binary or bytecode [optional, reads toml if omitted]");
    execute_cmd->add_flag("--interpreted", interpreted, "Run bytecode instead of native binary");

    auto *run_cmd = app.add_subcommand("run", "Build then execute");
    run_cmd->add_option("input", input_file, "Source file [optional, reads toml if omitted]")
            ->check(CLI::ExistingFile);
    run_cmd->add_flag("--interpreted", interpreted, "Compile to bytecode and run interpreted");

    auto *test_cmd = app.add_subcommand("test", "Run examples in source");
    test_cmd->add_option("input", input_file, "Source file (.zith)")->check(CLI::ExistingFile);

    auto *fmt_cmd = app.add_subcommand("fmt", "Format source code");
    fmt_cmd->add_option("input", input_file, "Source file or directory")->required();
    fmt_cmd->add_flag("--check", fmt_check, "Check formatting only, do not modify files");

    auto *docs_cmd = app.add_subcommand("docs", "Generate documentation");
    docs_cmd->add_option("input", input_file, "Source file (.zith)")->check(CLI::ExistingFile);
    docs_cmd->add_option("-o,--output", docs_output, "Output directory")->default_str("docs");

    auto *repl_cmd = app.add_subcommand("repl", "Start interactive REPL");
    auto *new_cmd = app.add_subcommand("new", "Create a new project");
    new_cmd->add_option("name", input_file, "Project name")->required();

    auto *clean_cmd = app.add_subcommand("clean", "Remove build artifacts");
    auto *version_cmd = app.add_subcommand("version", "Show version information");
    auto *help_cmd = app.add_subcommand("help", "Show help message");

    // -- Parse ----------------------------------------------------------------
    try {
        app.parse(argc, argv);
    } catch (const CLI::CallForHelp &) {
        return cmd_help();
    } catch (const CLI::CallForVersion &) {
        return cmd_version();
    } catch (const CLI::ParseError &e) {
        if (e.get_name() == "ExtrasError" && argc > 1 && std::string(argv[1]) == "help")
            return cmd_help();
        std::cerr << "[error] " << e.what() << "\n\n" << app.help();
        return 1;
    }

    // -- Dispatch -------------------------------------------------------------
    if (*help_cmd) return cmd_help();
    if (*version_cmd) return cmd_version();
    if (*new_cmd) return cmd_new(input_file, verbose);
    if (*clean_cmd) return cmd_clean(verbose);
    if (*repl_cmd) return cmd_repl(verbose);
    if (*docs_cmd) return cmd_docs(input_file, docs_output, verbose);
    if (*fmt_cmd) return cmd_fmt(input_file, fmt_check, verbose);
    if (*test_cmd) return cmd_test(input_file, verbose);
    if (*check_cmd) return cmd_check(input_file, mode_str, verbose);
    if (*compile_cmd)
        return cmd_compile(input_file, output_file, mode_str,
                           interpreted, verbose, include_dirs);
    if (*build_cmd)
        return cmd_build(input_file, output_file, mode_str,
                         verbose, include_dirs);
    if (*execute_cmd) return cmd_execute(input_file, interpreted, verbose);
    if (*run_cmd)
        return cmd_run(input_file, output_file, mode_str,
                       interpreted, verbose, include_dirs);

    // Sem subcomando: tenta build via toml, senão mostra ajuda
    ZithProject proj;
    if (try_load_project(proj))
        return cmd_build("", "", mode_str, verbose, include_dirs);

    return cmd_help();
}
