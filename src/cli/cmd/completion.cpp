#include "cli/commands.hpp"
#include <cstdio>
#include <string>

namespace zith::cli::commands {

int completion(const Options &opts) {
    const std::string &shell = opts.subcommandStr;

    if (shell.empty()) {
        std::fprintf(stderr,
                     "Error: shell name required. Supported shells are: bash, zsh, fish.\n");
        return 1;
    }

    if (shell == "bash") {
        std::printf(R"###(# bash completion for zithc

_zithc() {
    local cur prev opts cmds
    COMPREPLY=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    cmds="build run execute check fmt create deps test docs repl clean completion"

    opts="-h --help --version -m --mode -o --output -I --include -L -l -D -A --assets --check -i --in-place --emit --target --sysroot --emit-tokens --emit-ast --emit-hir --emit-ir --emit-asm --emit-all --interpreted --opt-level --debug-level -s --strict --lto --strip-debug -c --color -v --verbose"

    # Specific option completions
    case "$prev" in
        -m|--mode)
            COMPREPLY=( $(compgen -W "debug dev release fast small" -- "$cur") )
            return 0
            ;;
        --emit)
            COMPREPLY=( $(compgen -W "ast hir ir asm obj bin" -- "$cur") )
            return 0
            ;;
        -c|--color)
            COMPREPLY=( $(compgen -W "auto on off" -- "$cur") )
            return 0
            ;;
        -o|--output|--sysroot|-I|--include|-L|-A|--assets)
            if declare -F _filedir &>/dev/null; then
                _filedir
            else
                COMPREPLY=( $(compgen -f -- "$cur") )
            fi
            return 0
            ;;
    esac

    # Complete commands or general options
    if [[ "$cur" == -* ]]; then
        COMPREPLY=( $(compgen -W "$opts" -- "$cur") )
    else
        # Search if any command is already specified
        local cmd_found=""
        local word
        for word in "${COMP_WORDS[@]:1:COMP_CWORD-1}"; do
            if [[ " $cmds " == *" $word "* ]]; then
                cmd_found="$word"
                break
            fi
        done

        if [[ -z "$cmd_found" ]]; then
            COMPREPLY=( $(compgen -W "$cmds" -- "$cur") )
        else
            # We are inside a command, e.g. "zithc build <file>".
            # Provide file/directory completion
            if declare -F _filedir &>/dev/null; then
                _filedir
            else
                COMPREPLY=( $(compgen -f -- "$cur") )
            fi
        fi
    fi
}

complete -F _zithc zithc
)###");
        return 0;
    } else if (shell == "zsh") {
        std::printf(R"###(# zsh completion for zithc
#compdef zithc

autoload -U +X bashcompinit && bashcompinit

_zithc() {
    local cur prev opts cmds
    COMPREPLY=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    cmds="build run execute check fmt create deps test docs repl clean completion"

    opts="-h --help --version -m --mode -o --output -I --include -L -l -D -A --assets --check -i --in-place --emit --target --sysroot --emit-tokens --emit-ast --emit-hir --emit-ir --emit-asm --emit-all --interpreted --opt-level --debug-level -s --strict --lto --strip-debug -c --color -v --verbose"

    case "$prev" in
        -m|--mode)
            COMPREPLY=( $(compgen -W "debug dev release fast small" -- "$cur") )
            return 0
            ;;
        --emit)
            COMPREPLY=( $(compgen -W "ast hir ir asm obj bin" -- "$cur") )
            return 0
            ;;
        -c|--color)
            COMPREPLY=( $(compgen -W "auto on off" -- "$cur") )
            return 0
            ;;
        -o|--output|--sysroot|-I|--include|-L|-A|--assets)
            COMPREPLY=( $(compgen -f -- "$cur") )
            return 0
            ;;
    esac

    if [[ "$cur" == -* ]]; then
        COMPREPLY=( $(compgen -W "$opts" -- "$cur") )
    else
        local cmd_found=""
        local word
        for word in "${COMP_WORDS[@]:1:COMP_CWORD-1}"; do
            if [[ " $cmds " == *" $word "* ]]; then
                cmd_found="$word"
                break
            fi
        done

        if [[ -z "$cmd_found" ]]; then
            COMPREPLY=( $(compgen -W "$cmds" -- "$cur") )
        else
            COMPREPLY=( $(compgen -f -- "$cur") )
        fi
    fi
}

complete -F _zithc zithc
)###");
        return 0;
    } else if (shell == "fish") {
        std::printf(R"###(# fish completion for zithc

# Disable file completion by default for commands, but re-enable when appropriate
complete -c zithc -f

# Subcommands
complete -c zithc -f -n "not __fish_use_subcommand" -a build -d "Build project files (uses ZithProject.toml)"
complete -c zithc -f -n "not __fish_use_subcommand" -a run -d "Build and execute"
complete -c zithc -f -n "not __fish_use_subcommand" -a execute -d "Execute a pre-compiled binary"
complete -c zithc -f -n "not __fish_use_subcommand" -a check -d "Type-check files (uses ZithProject.toml)"
complete -c zithc -f -n "not __fish_use_subcommand" -a fmt -d "Format source files"
complete -c zithc -f -n "not __fish_use_subcommand" -a create -d "Create a new project"
complete -c zithc -f -n "not __fish_use_subcommand" -a deps -d "Manage dependencies"
complete -c zithc -f -n "not __fish_use_subcommand" -a test -d "Run tests"
complete -c zithc -f -n "not __fish_use_subcommand" -a docs -d "Generate documentation"
complete -c zithc -f -n "not __fish_use_subcommand" -a repl -d "Start interactive REPL"
complete -c zithc -f -n "not __fish_use_subcommand" -a clean -d "Clean build artifacts"
complete -c zithc -f -n "not __fish_use_subcommand" -a completion -d "Generate shell completion scripts"

# Enable file completion for subcommands that take files
complete -c zithc -F -n "__fish_seen_subcommand_from build run execute check fmt"

# Options
complete -c zithc -s h -l help -d "Show help"
complete -c zithc -l version -d "Show version"
complete -c zithc -s m -l mode -r -f -a "debug dev release fast small" -d "Build mode"
complete -c zithc -s o -l output -r -g -d "Output file path"
complete -c zithc -s I -l include -r -d "Add include directory"
complete -c zithc -s A -l assets -r -d "Add asset directory"
complete -c zithc -l check -d "Check formatting without modifying"
complete -c zithc -s i -l in-place -d "Format files in-place"
complete -c zithc -l emit -r -f -a "ast hir ir asm obj bin" -d "Emit intermediate representation"
complete -c zithc -l target -r -d "Target triple for cross-compilation"
complete -c zithc -l sysroot -r -d "Sysroot for cross-compilation linking"
complete -c zithc -l emit-tokens -d "Print and emit tokens"
complete -c zithc -l emit-ast -d "Emit AST"
complete -c zithc -l emit-hir -d "Emit HIR"
complete -c zithc -l emit-ir -d "Emit LLVM IR"
complete -c zithc -l emit-asm -d "Emit assembly"
complete -c zithc -l emit-all -d "Emit tokens, AST, HIR, IR, and assembly"
complete -c zithc -l interpreted -d "Use bytecode path"
complete -c zithc -l opt-level -r -f -a "0 1 2 3" -d "Optimization level"
complete -c zithc -l debug-level -r -f -a "0 1 2 3" -d "Debug info level"
complete -c zithc -s s -l strict -d "Apply stricter rules"
complete -c zithc -l lto -d "Enable LTO"
complete -c zithc -l strip-debug -d "Strip debug symbols"
complete -c zithc -s c -l color -r -f -a "auto on off" -d "Color output"
complete -c zithc -s v -l verbose -d "Verbose output"
)###");
        return 0;
    }

    std::fprintf(stderr, "Error: Unknown shell '%s'. Supported shells are: bash, zsh, fish.\n",
                 shell.c_str());
    return 1;
}

} // namespace zith::cli::commands
