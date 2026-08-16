#include "config/project/project-config.hpp"

#include "common/memory/arena.hpp"
#include "common/memory/string-interner.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

using common::memory::Arena;
using common::memory::StringInterner;

int main() {
    constexpr std::string_view toml = R"(
[project]
name = "demo"
version = "0.2.0"
authors = ["Demo Authors"]

[build]
entry = "src/demo.zith"
mode = "release"
opt_level = 2
verbose = true

[paths]
src_dir = "src"
bin_dir = "bin"

[ffi]
include_dirs = ["vendor/include"]
)";

    Arena arena;
    StringInterner strings(arena);
    toolkit::ProjectConfig config(arena, strings);
    const auto result = toolkit::loadFromToml(toml, arena, strings, config);
    if (!result.isOk()) {
        std::cerr << "config-demo: " << result.error().msg << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "project.name=" << strings.lookup(config.name) << "\n";
    std::cout << "project.version=" << strings.lookup(config.version) << "\n";
    std::cout << "build.entry=" << strings.lookup(config.entry) << "\n";
    std::cout << "build.mode=" << strings.lookup(config.mode) << "\n";
    std::cout << "build.optLevel=" << config.optLevel << "\n";
    std::cout << "build.verbose=" << (config.verbose ? "true" : "false") << "\n";
    std::cout << "paths.srcDir=" << strings.lookup(config.srcDir) << "\n";
    std::cout << "paths.binDir=" << strings.lookup(config.binDir) << "\n";
    std::cout << "ffi.includeDirs[0]="
              << (config.includeDirs.size() > 0
                      ? strings.lookup(config.includeDirs[0])
                      : std::string_view{"<empty>"})
              << "\n";

    std::cout << "config-demo: TOML loading checks passed\n";
    return EXIT_SUCCESS;
}
