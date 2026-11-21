#include "cli/commands.hpp"
#include "cli/terminal.hpp"
#include "session/compilation-session.hpp"
#include "support/source-extension.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#elif !defined(ZITH_IS_WASM)
#include <unistd.h>
#endif

namespace zith::cli::commands {

namespace fs = std::filesystem;

static bool isStdinTerminal() {
#ifdef ZITH_IS_WASM
    return false;
#elif defined(_WIN32)
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

static std::vector<std::string> findZithFiles(const std::string &dir) {
    std::vector<std::string> files;
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return files;
    for (const auto &entry : fs::recursive_directory_iterator(dir, ec)) {
        if (entry.is_regular_file() && support::isZithmSource(entry.path()))
            files.push_back(entry.path().string());
    }
    return files;
}

static std::string readStdin() {
    std::string content;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0)
        content.append(buf, n);
    return content;
}

static std::string readFile(const std::string &path) {
    std::ifstream file(path);
    if (!file)
        return {};
    std::stringstream buf;
    buf << file.rdbuf();
    return buf.str();
}

static bool writeFile(const std::string &path, const std::string &content) {
    std::ofstream file(path);
    if (!file)
        return false;
    file << content;
    return file.good();
}

struct FmtFileResult {
    std::string formatted;
    bool ok;
};

int fmt(const Options &opts) {
    auto TERM = term::init(opts);
    term::UsagePrinter out{stdout, TERM.coutOn};
    term::UsagePrinter err{stderr, TERM.cerrOn};
    std::vector<std::string> files;
    bool has_stdin = false;

    if (opts.inputFiles.empty()) {
        if (!isStdinTerminal()) {
            has_stdin = true;
        } else {
            files = findZithFiles(".");
            if (files.empty()) {
                err.red("[error]");
                std::fprintf(stderr, " no %s files found\n",
                             std::string(support::kZithmSourceExtension).c_str());
                return 1;
            }
        }
    } else {
        for (const auto &f : opts.inputFiles) {
            if (f == "-")
                has_stdin = true;
            else
                files.push_back(f);
        }
    }

    // Stdin mode
    if (has_stdin) {
        if (opts.flags.fmtCheck()) {
            err.red("[error]");
            std::fprintf(stderr, " --check is not supported with stdin\n");
            return 1;
        }
        if (opts.flags.fmtInPlace()) {
            err.red("[error]");
            std::fprintf(stderr, " -i/--in-place is not supported with stdin\n");
            return 1;
        }

        session::CompilationSession session(opts, "<stdin>");
        session.setContent(readStdin());
        session.setBuffered(true);

        std::string formatted = session.fmtStage();
        if (session.hasErrors()) {
            session.emitDiagnostics();
            return 1;
        }
        std::printf("%s", formatted.c_str());
        return 0;
    }

    // File mode
    int exit_code = 0;
    bool any_diff = false;

    std::vector<std::future<FmtFileResult>> futures;
    futures.reserve(files.size());
    for (const auto &file : files) {
        futures.push_back(std::async(std::launch::async, [&opts, file]() -> FmtFileResult {
            session::CompilationSession session(opts, file);
            session.setBuffered(true);
            std::string formatted = session.fmtStage();
            bool ok               = !session.hasErrors();
            return {std::move(formatted), ok};
        }));
    }

    for (size_t i = 0; i < files.size(); i++) {
        auto result = futures[i].get();

        if (!result.ok) {
            err.red("[error]");
            std::fprintf(stderr, " failed to format %s\n", files[i].c_str());
            exit_code = 1;
            continue;
        }

        if (opts.flags.fmtCheck()) {
            std::string original = readFile(files[i]);
            if (result.formatted != original) {
                err.yellow("[check]");
                std::fprintf(stderr, " %s would be reformatted\n", files[i].c_str());
                any_diff  = true;
                exit_code = 1;
            } else {
                out.green("[ok]");
                std::printf(" %s\n", files[i].c_str());
            }
        } else if (opts.flags.fmtInPlace()) {
            if (!writeFile(files[i], result.formatted)) {
                err.red("[error]");
                std::fprintf(stderr, " failed to write %s\n", files[i].c_str());
                exit_code = 1;
            } else if (opts.flags.verbose()) {
                out.green("[fmt]");
                std::printf(" %s\n", files[i].c_str());
            }
        } else {
            if (i > 0)
                std::printf("\n");
            std::printf("%s", result.formatted.c_str());
        }
    }

    if (any_diff && opts.flags.fmtCheck()) {
        err.yellow("[check]");
        std::fprintf(stderr, " some files need reformatting\n");
        err.yellow("[check]");
        std::fprintf(stderr, " run `zithc fmt -i` to reformat\n");
    }

    return exit_code;
}

} // namespace zith::cli::commands
