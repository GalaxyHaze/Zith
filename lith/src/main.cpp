#include "lith/tokenizer.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: lith <file.lith>\n";
        return 1;
    }

    const char* file_path = argv[1];
    std::ifstream in(file_path);
    if (!in) {
        std::cerr << "Could not open file: " << file_path << "\n";
        return 1;
    }

    std::ostringstream buffer;
    buffer << in.rdbuf();

    const auto tokens = lith::tokenize(buffer.str());
    for (const auto& token : tokens) {
        std::cout
            << token.line << ':' << token.column
            << " [" << lith::token_kind_name(token.kind) << "] "
            << token.lexeme << '\n';
    }

    return 0;
}
