#pragma once

#include <cstdarg>
#include <cstdio>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace generated_cli::term {

namespace ansi {
inline constexpr std::string_view reset = "\033[0m";
inline constexpr std::string_view bold = "\033[1m";
inline constexpr std::string_view red = "\033[31m";
inline constexpr std::string_view cyan = "\033[36m";
} // namespace ansi

struct Terminal {
    bool stderrColor = false;
    bool stdoutColor = false;
};

Terminal init();

inline void enableVirtualTerminal() {
#ifdef _WIN32
    HANDLE handles[] = {GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE)};
    for (HANDLE handle : handles) {
        if (handle == INVALID_HANDLE_VALUE || handle == nullptr)
            continue;
        DWORD mode = 0;
        if (GetConsoleMode(handle, &mode)) {
            mode |= 0x0004;
            SetConsoleMode(handle, mode);
        }
    }
#endif
}

struct UsagePrinter {
    FILE *out = nullptr;
    bool colorOn = false;

    UsagePrinter(FILE *stream, bool color) : out(stream), colorOn(color) {}
    UsagePrinter() = default;

    void print(const char *ansiCode, const char *fmt, ...) const {
        if (colorOn)
            std::fputs(ansiCode, out);
        va_list args;
        va_start(args, fmt);
        std::vfprintf(out, fmt, args);
        va_end(args);
        if (colorOn)
            std::fputs(ansi::reset.data(), out);
    }

    void bold(const char *text) const {
        print(ansi::bold.data(), "%s", text);
    }

    void cyan(const char *text) const {
        print(ansi::cyan.data(), "%s", text);
    }

    void red(const char *text) const {
        print(ansi::red.data(), "%s", text);
    }

    void section(const char *title) const {
        bold(title);
        std::fputc('\n', out);
    }

    void flag(const char *flags, const char *description) const {
        std::fputs("  ", out);
        print(ansi::cyan.data(), "%-28s", flags);
        std::fprintf(out, " %s\n", description);
    }
};

} // namespace generated_cli::term
