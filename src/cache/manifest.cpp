#include "cache/manifest.hpp"

#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>

namespace toolkit::cache {

namespace {

namespace fs = std::filesystem;

std::string escapeField(std::string_view value) {
    std::string out;
    for (const char c : value) {
        if (c == '\n' || c == '\x1f' || c == '\x1e')
            out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

std::string unescapeField(std::string_view value) {
    std::string out;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
            out.push_back(value[++i]);
        } else {
            out.push_back(value[i]);
        }
    }
    return out;
}

bool parseHex32(std::string_view text, std::uint32_t &out) {
    const auto *begin = text.data();
    const auto *end = begin + text.size();
    auto [ptr, ec] = std::from_chars(begin, end, out, 16);
    return ec == std::errc{} && ptr == end;
}

} // namespace

void Manifest::upsert(ManifestEntry entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto &slot = by_path_[entry.canonical_path];
    for (const auto &dep : slot.dependencies)
        reverse_deps_[dep].erase(entry.canonical_path);
    slot = std::move(entry);
    for (const auto &dep : slot.dependencies)
        reverse_deps_[dep].insert(slot.canonical_path);
}

void Manifest::remove(std::string_view canonical_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = by_path_.find(std::string(canonical_path));
    if (it == by_path_.end())
        return;
    for (const auto &dep : it->second.dependencies)
        reverse_deps_[dep].erase(it->first);
    by_path_.erase(it);
}

std::optional<ManifestEntry> Manifest::find(std::string_view canonical_path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = by_path_.find(std::string(canonical_path));
    if (it == by_path_.end())
        return std::nullopt;
    return it->second;
}

std::vector<std::string> Manifest::dependentsOf(std::string_view canonical_path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    std::unordered_set<std::string> visited;
    std::vector<std::string> stack{std::string(canonical_path)};
    while (!stack.empty()) {
        const auto current = std::move(stack.back());
        stack.pop_back();
        if (!visited.insert(current).second)
            continue;
        const auto it = reverse_deps_.find(current);
        if (it == reverse_deps_.end())
            continue;
        for (const auto &dependent : it->second) {
            result.push_back(dependent);
            stack.push_back(dependent);
        }
    }
    return result;
}

void Manifest::save() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code ec;
    fs::create_directories(root_, ec);
    const auto path = fs::path(root_) / "manifest";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        return;
    for (const auto &[key, entry] : by_path_) {
        output << escapeField(entry.canonical_path) << '\x1f'
               << escapeField(entry.artifact_path) << '\x1f' << std::hex
               << std::setfill('0') << std::setw(8) << entry.public_abi_hi << '\x1f'
               << std::setw(8) << entry.public_abi_lo << '\x1f' << std::setw(8)
               << entry.source_fp_hi << '\x1f' << std::setw(8) << entry.source_fp_lo << '\x1e';
        for (size_t i = 0; i < entry.dependencies.size(); ++i) {
            if (i != 0)
                output << '\x1d';
            output << escapeField(entry.dependencies[i]);
        }
        output << '\n';
    }
}

void Manifest::load() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto path = fs::path(root_) / "manifest";
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return;
    std::string line;
    while (std::getline(input, line)) {
        std::string_view raw(line);
        if (raw.empty())
            continue;
        const auto sep = raw.find('\x1e');
        const std::string_view header = (sep == std::string_view::npos) ? raw : raw.substr(0, sep);
        const std::string_view deps = (sep == std::string_view::npos) ? "" : raw.substr(sep + 1);
        std::vector<std::string_view> fields;
        size_t start = 0;
        for (size_t i = 0; i <= header.size(); ++i) {
            if (i == header.size() || header[i] == '\x1f') {
                fields.push_back(header.substr(start, i - start));
                start = i + 1;
            }
        }
        if (fields.size() < 6)
            continue;
        ManifestEntry entry;
        entry.canonical_path = unescapeField(fields[0]);
        entry.artifact_path = unescapeField(fields[1]);
        if (!parseHex32(fields[2], entry.public_abi_hi) ||
            !parseHex32(fields[3], entry.public_abi_lo) ||
            !parseHex32(fields[4], entry.source_fp_hi) ||
            !parseHex32(fields[5], entry.source_fp_lo))
            continue;
        if (!deps.empty()) {
            size_t dstart = 0;
            for (size_t i = 0; i <= deps.size(); ++i) {
                if (i == deps.size() || deps[i] == '\x1d') {
                    const auto dependency = deps.substr(dstart, i - dstart);
                    if (!dependency.empty())
                        entry.dependencies.push_back(unescapeField(dependency));
                    dstart = i + 1;
                }
            }
        }
        for (const auto &dependency : entry.dependencies)
            reverse_deps_[dependency].insert(entry.canonical_path);
        by_path_[entry.canonical_path] = std::move(entry);
    }
}

} // namespace toolkit::cache
