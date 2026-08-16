#include "session/session.hpp"

#include "frontend/lexer/lexer.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

using toolkit::session::CompilationSession;
using toolkit::session::Stage;
using toolkit::session::ZithSessionContext;

int main() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    const std::filesystem::path path = dir / "zith-session-demo.zith";

    {
        std::ofstream out(path);
        out << "fn main(x) { return 42; }\n";
    }

    ZithSessionContext context;
    {
        const std::string filePath = path.string();
        context.filePath = filePath;
        CompilationSession session(context);

        const auto result = session.runTo(Stage::Lexed);
        if (!result) {
            std::cerr << "session-demo: " << result.error().msg << '\n';
            return EXIT_FAILURE;
        }
        if (!session.hasStageResult<Stage::Lexed>()) {
            std::cerr << "session-demo: missing Lexed stage result\n";
            return EXIT_FAILURE;
        }

        const auto &tokens = session.stageResult<Stage::Lexed>().value();
        std::cout << "session-demo: ran to "
                  << toolkit::session::stageLabel(Stage::Lexed)
                  << ", " << tokens.size() << " tokens\n";
    }

    std::error_code cleanupError;
    std::filesystem::remove(path, cleanupError);
    return EXIT_SUCCESS;
}
