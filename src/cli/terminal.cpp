#include "cli/terminal.hpp"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace generated_cli::term {

Terminal init() {
    enableVirtualTerminal();

    Terminal terminal;
#ifdef _WIN32
    terminal.stderrColor = _isatty(_fileno(stderr)) != 0;
    terminal.stdoutColor = _isatty(_fileno(stdout)) != 0;
#else
    terminal.stderrColor = isatty(fileno(stderr)) != 0;
    terminal.stdoutColor = isatty(fileno(stdout)) != 0;
#endif
    return terminal;
}

} // namespace generated_cli::term
