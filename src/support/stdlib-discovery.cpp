#include "support/stdlib-discovery.hpp"

#include <cstdlib>
#include <string>
#include <vector>

#ifndef ZITH_IS_WASM
#include <filesystem>
#endif

#ifdef __linux__
#include <climits>
#include <unistd.h>
#elif defined(__APPLE__)
#include <cstdlib>
#include <limits.h>
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#include <Windows.h>
#endif

namespace zith::support {
namespace {

/// Returns the absolute path to the running executable, or empty on failure.
std::string executablePath() {
#ifdef ZITH_IS_WASM
    return {};
#elif defined(__linux__)
    char buf[PATH_MAX];
    auto len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0)
        return {};
    buf[len] = '\0';
    return buf;
#elif defined(__APPLE__)
    char buf[PATH_MAX];
    uint32_t bufsize = sizeof(buf);
    if (_NSGetExecutablePath(buf, &bufsize) != 0)
        return {};
    char resolved[PATH_MAX];
    if (!::realpath(buf, resolved))
        return {};
    return resolved;
#elif defined(_WIN32)
    wchar_t buf[MAX_PATH];
    auto len = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return {};
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        return {};
    std::string result(static_cast<std::size_t>(needed) - 1U, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, buf, -1, &result[0], static_cast<int>(result.size() + 1U),
                          nullptr, nullptr);
    return result;
#else
    return {};
#endif
}

} // namespace

std::vector<std::string> findStdlibRoots() {
    // ZITH_STDLIB override
    char *env = nullptr;
#ifdef _WIN32
    std::size_t env_size = 0;
    if (_dupenv_s(&env, &env_size, "ZITH_STDLIB") != 0)
        return {};
#else
    env = std::getenv("ZITH_STDLIB");
#endif
    if (env != nullptr) {
        std::string path(env);
#ifdef ZITH_IS_WASM
#ifdef _WIN32
        std::free(env);
#endif
        return {std::move(path)};
#else
        if (std::filesystem::is_directory(path)) {
#ifdef _WIN32
            std::free(env);
#endif
            return {path};
        }
#ifdef _WIN32
        std::free(env);
#endif
        return {};
#endif
    }

    auto exe = executablePath();
    if (exe.empty())
        return {};

#ifdef ZITH_IS_WASM
    return {};
#else
    namespace fs = std::filesystem;

    auto parentDir = fs::path(exe).parent_path();

    std::vector<std::string> roots;
    auto installed = (parentDir / ".." / "share" / "zith" / "stdlib").lexically_normal();
    if (fs::is_directory(installed))
        roots.push_back(installed.string());

    auto dev = (parentDir / ".." / "stdlib").lexically_normal();
    if (fs::is_directory(dev))
        roots.push_back(dev.string());

    return roots;
#endif
}

} // namespace zith::support
