#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

namespace fs = std::filesystem;

std::string readFile(const fs::path &path) {
    std::ifstream in(path);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

bool writeRules(const fs::path &path, const std::string &text) {
    if (path.has_parent_path() && !fs::exists(path.parent_path())) {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        if (ec)
            return false;
    }
    std::ofstream out(path);
    out << text;
    return static_cast<bool>(out);
}

} // namespace

int main(int argc, char **argv) {
    using namespace std::filesystem;

    if (argc < 2)
        return EXIT_FAILURE;
    const std::string scaffoldScript =
        std::string(argv[1]) + "/src/config/project/scaffold.py";
    path base = temp_directory_path() / "turv-scaffold-test";
    remove_all(base);
    const path rules = base / "scaffold.toml";
    if (!writeRules(
            rules,
            "[blueprints]\nturv = \"basic\"\n\n"
            "[blueprints.basic]\nname = \"basic\"\n\n"
            "[blueprints.basic.files]\n"
            "README.md = \"# {project}\\n\"\n"
            "src/main.turv = \"fn main() {}\\n\"\n"))
        return EXIT_FAILURE;

    const std::string cmd =
        "python3 \"" + scaffoldScript + "\" --rules \"" +
        rules.string() + "\" --out \"" + (base / "out").string() +
        "\" --blueprint basic --project demo";
    if (std::system(cmd.c_str()) != 0)
        return EXIT_FAILURE;
    if (!exists(base / "out" / "README.md"))
        return EXIT_FAILURE;
    if (readFile(base / "out" / "README.md") != "# demo\n")
        return EXIT_FAILURE;
    if (!exists(base / "out" / "src" / "main.turv"))
        return EXIT_FAILURE;

    const std::string conflictCmd =
        "python3 \"" + scaffoldScript + "\" --rules \"" +
        rules.string() + "\" --out \"" + (base / "out").string() +
        "\" --blueprint basic --project demo";
    const int conflict = std::system(conflictCmd.c_str());
    if (conflict == 0)
        return EXIT_FAILURE;

    const std::string missingCmd =
        "python3 \"" + scaffoldScript + "\" --rules \"" +
        rules.string() + "\" --out \"" + (base / "out2").string() +
        "\" --blueprint nope --project demo";
    const int missing = std::system(missingCmd.c_str());
    if (missing == 0)
        return EXIT_FAILURE;

    remove_all(base);
    return EXIT_SUCCESS;
}
