#include "frontend/lexer/lexer.hpp"
#include "common/arena.hpp"
#include "common/string-interner.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

using generated_lexer::Lexer;

namespace {

std::string make_input() {
    std::string out;
    out.reserve(4 * 1024 * 1024);
    constexpr std::string_view line = "fn main += - ; { } if when 1234 5678 alpha beta gamma\n";
    for (int i = 0; i < 50000; ++i)
        out.append(line);
    return out;
}

} // namespace

int main() {
    const std::string source = make_input();
    Lexer lexer;
    memory::Arena arena;
    memory::StringInterner strings(arena);
    size_t checksum = 0;
    for (int i = 0; i < 50; ++i) {
        const auto tokens = lexer.run(source, strings);
        checksum += tokens.size();
    }
    std::printf("%zu\n", checksum);
    return checksum == 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
