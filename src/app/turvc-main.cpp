#include "cli/cli.hpp"

int main(int argc, char **argv) {
    generated_cli::Cli cli;
    const int parse_status = cli.parseArgs(argc, argv);
    if (parse_status != 0)
        return parse_status;
    return cli.dispatch();
}
