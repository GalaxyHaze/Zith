#include "cli/actions.hpp"

#include "cli/cli.hpp"

#include <cstdio>

int show_help() {
    std::printf("zith build tool\n");
    return 0;
}

int show_version() {
    std::printf("zith 0.1.0\n");
    return 0;
}

namespace cmd {

int check(const generated_cli::Options &) {
    return 0;
}

int fmt(const generated_cli::Options &) {
    return 0;
}

int run(const generated_cli::Options &) {
    return 0;
}

int build(const generated_cli::Options &) {
    return 0;
}

int depsAdd() {
    return 0;
}

int depsCheck() {
    return 0;
}

int depsRemove() {
    return 0;
}

} // namespace cmd
