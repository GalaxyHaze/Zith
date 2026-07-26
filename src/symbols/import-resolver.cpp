#include "import-resolver.hpp"

#include <filesystem>

namespace zith::symbols::import_resolver {
namespace fs = std::filesystem;

namespace {

bool pathUnderRoot(const fs::path &path, const fs::path &root) {
    auto normalized_root = root.lexically_normal().generic_string();
    if (normalized_root.empty())
        return false;
    if (normalized_root.back() != '/')
        normalized_root += '/';

    auto normalized_path = path.lexically_normal().generic_string();
    return normalized_path.size() >= normalized_root.size() &&
           normalized_path.substr(0, normalized_root.size()) == normalized_root;
}

} // namespace

auto computeNamespace(const memory::DynArray<std::string_view> &path, std::string_view alias)
    -> std::string {
    if (!alias.empty())
        return std::string(alias);

    std::vector<std::string_view> parts;
    for (auto &seg : path) {
        if (seg == ".." && !parts.empty())
            parts.pop_back();
        else if (seg != ".." && seg != ".")
            parts.push_back(seg);
    }

    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0)
            result += '.';
        result.append(parts[i].data(), parts[i].size());
    }
    return result;
}

auto findFile(const std::string &import_key, std::string_view source_file,
              const std::vector<std::string> &visible_roots) -> memory::Optional<std::string> {
    static const char *extensions[] = {".zith", ".h", ".hpp"};

    auto check_file = [](const fs::path &path) { return fs::is_regular_file(path); };

    auto under_visible_root = [&](const fs::path &path) -> bool {
        for (auto &root : visible_roots)
            if (pathUnderRoot(path, fs::path(root)))
                return true;
        return false;
    };

    if (import_key.size() >= 2 && import_key[0] == '.' && import_key[1] == '.') {
        if (source_file.empty())
            return {};

        fs::path source_path(source_file);
        auto absolute = fs::weakly_canonical(source_path.parent_path() / import_key);

        for (auto ext : extensions) {
            auto path = absolute.string() + ext;
            if (check_file(path) && under_visible_root(path))
                return path;
        }

        auto mod_path = (absolute / "mod.zith").string();
        if (check_file(mod_path) && under_visible_root(mod_path))
            return mod_path;

        if (fs::is_directory(absolute) && under_visible_root(absolute))
            return absolute.string();
        return {};
    }

    for (auto &root : visible_roots) {
        auto base = fs::path(root) / import_key;

        for (auto ext : extensions) {
            auto path = base.string() + ext;
            if (check_file(path) && under_visible_root(path))
                return path;
        }

        auto mod_path = (base / "mod.zith").string();
        if (check_file(mod_path) && under_visible_root(mod_path))
            return mod_path;

        if (fs::is_directory(base) && under_visible_root(base))
            return base.string();
    }

    return {};
}

auto findAssetFile(const std::string &import_key, const std::vector<std::string> &asset_roots)
    -> memory::Optional<std::string> {
    for (auto &root : asset_roots) {
        auto path = fs::path(root) / import_key;
        if (fs::is_regular_file(path))
            return path.string();
    }

    return {};
}

void collectDirFiles(const std::string &dir_path, const std::string &base_dir, int max_depth,
                     int current_depth, std::vector<DirEntry> &out) {
    if (max_depth != -1 && current_depth > max_depth)
        return;

    fs::path dir(dir_path);
    if (!fs::is_directory(dir))
        return;

    for (auto &entry : fs::directory_iterator(dir)) {
        auto &path = entry.path();
        auto ext   = path.extension();
        if (entry.is_regular_file() && (ext == ".zith" || ext == ".h" || ext == ".hpp")) {
            auto rel = fs::relative(path, fs::path(base_dir)).generic_string();
            if (ext == ".zith") {
                if (rel.size() >= 5)
                    rel = rel.substr(0, rel.size() - 5);
            } else {
                auto ext_str = ext.string();
                if (rel.size() >= ext_str.size())
                    rel = rel.substr(0, rel.size() - ext_str.size());
            }
            out.push_back({path.string(), rel, current_depth});
        } else if (entry.is_directory()) {
            collectDirFiles(path.string(), base_dir, max_depth, current_depth + 1, out);
        }
    }
}

auto planDirectoryImports(const std::string &import_key, const std::string &dir_path,
                          const memory::DynArray<std::string_view> &path, std::string_view alias,
                          bool is_from, int32_t import_depth)
    -> memory::Result<std::vector<DirectoryImport>> {
    std::vector<DirEntry> files;
    collectDirFiles(dir_path, dir_path, import_depth, 1, files);

    if (files.empty())
        return memory::Error{"directory '" + dir_path + "' contains no .zith files"};

    std::vector<DirectoryImport> planned;
    planned.reserve(files.size());

    auto base_ns = computeNamespace(path, is_from ? std::string_view{} : alias);

    for (auto &entry : files) {
        std::string ns;
        if (!alias.empty()) {
            ns = std::string(alias);
        } else if (is_from) {
            ns.clear();
        } else {
            ns = base_ns;
            if (!ns.empty())
                ns += '.';
        }

        for (char c : entry.relative_stem)
            ns += (c == '/') ? '.' : c;

        planned.push_back(DirectoryImport{
            entry.full_path,
            import_key + "/" + entry.relative_stem,
            std::move(ns),
        });
    }

    return planned;
}

} // namespace zith::symbols::import_resolver
