#include "cli/cli.hpp"

int main(int argc, char **argv) {
    generated_cli::Cli cli;
    cli.parseArgs(argc, argv);
}
