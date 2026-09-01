#include "sema/hir-lower-utils.hpp"

#include <filesystem>

#include <algorithm>
#include <cctype>

namespace zith::sema::modern {

bool decodeEscapes(std::string_view text, std::string &output, bool keep_marker) {
    for (size_t index = 0; index < text.size(); ++index) {
        const char current = text[index];
        if (current != '\\') {
            output.push_back(current);
            continue;
        }
        if (index + 1 >= text.size())
            return false;
        const char escaped = text[++index];
        switch (escaped) {
        case 'n':
            output.push_back('\n');
            break;
        case 'r':
            output.push_back('\r');
            break;
        case 't':
            output.push_back('\t');
            break;
        case '0':
            output.push_back('\0');
            break;
        case '\\':
            output.push_back('\\');
            break;
        case '#':
            output.push_back(keep_marker ? kFormatHashSentinel : '#');
            break;
        case '\'':
            output.push_back('\'');
            break;
        case '"':
            output.push_back('"');
            break;
        case 'x': {
            if (index + 2 >= text.size() ||
                !std::isxdigit(static_cast<unsigned char>(text[index + 1])) ||
                !std::isxdigit(static_cast<unsigned char>(text[index + 2]))) {
                return false;
            }
            const unsigned high = static_cast<unsigned char>(text[index + 1]);
            const unsigned low  = static_cast<unsigned char>(text[index + 2]);
            const auto hex      = [](unsigned c) {
                return c >= '0' && c <= '9'   ? c - '0'
                            : c >= 'a' && c <= 'f' ? c - 'a' + 10U
                                                   : c - 'A' + 10U;
            };
            output.push_back(static_cast<char>((hex(high) << 4U) | hex(low)));
            index += 2U;
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

uint64_t internFunctionKey(memory::StringInterner &interner, std::string_view module,
                           frontend::DeclId decl) {
    const uint64_t module_id = interner.intern(module);
    return (module_id << 32U) | decl.value;
}

std::string moduleNamespace(std::string_view module_key, const session::CacheKey &cache_key) {
    const std::filesystem::path path{module_key};
    // The console module is imported from many roots during a normal stdlib
    // build. Normalize its canonical namespace so function lookup (and the
    // vtable/string symbols that depend on it) is stable.
    if (module_key == "stdlib/std/io/console.zith")
        return "std.io.console";
    std::string best_relative;
    size_t best_root_length = 0;
    const auto consider     = [&](const std::string &root) {
        if (root.empty())
            return;
        std::error_code ec;
        const auto relative = std::filesystem::relative(path, std::filesystem::path(root), ec);
        if (ec || relative.empty())
            return;
        const auto text = relative.generic_string();
        if (text.starts_with(".."))
            return;
        if (root.size() >= best_root_length) {
            best_root_length = root.size();
            best_relative    = text;
        }
    };
    for (const auto &root : cache_key.stdlibRoots)
        consider(root);
    for (const auto &root : cache_key.includeRoots)
        consider(root);
    consider(cache_key.workspaceRoot);

    std::string relative = best_relative.empty() ? path.stem().string() : best_relative;
    if (relative.size() > 5U && relative.compare(relative.size() - 5U, 5U, ".zith") == 0)
        relative.erase(relative.size() - 5U);
    for (auto &character : relative) {
        if (character == '/' || character == '\\')
            character = '.';
    }
    return relative;
}

hir::HirOwnership mapHirOwnership(types::OwnershipKind kind) noexcept {
    switch (kind) {
    case types::OwnershipKind::Lend:
        return hir::HirOwnership::Lend;
    case types::OwnershipKind::View:
        return hir::HirOwnership::View;
    case types::OwnershipKind::Unique:
        return hir::HirOwnership::Unique;
    case types::OwnershipKind::Share:
        return hir::HirOwnership::Share;
    case types::OwnershipKind::Belong:
        return hir::HirOwnership::Belong;
    case types::OwnershipKind::Default:
        break;
    }
    return hir::HirOwnership::Default;
}

hir::HirCallEscape mapHirEscape(NraArgEscape escape) noexcept {
    switch (escape) {
    case NraArgEscape::Borrow:
        return hir::HirCallEscape::Borrow;
    case NraArgEscape::Capture:
        return hir::HirCallEscape::Capture;
    case NraArgEscape::Escape:
        return hir::HirCallEscape::Escape;
    case NraArgEscape::Move:
        return hir::HirCallEscape::Move;
    case NraArgEscape::None:
        break;
    }
    return hir::HirCallEscape::None;
}

} // namespace zith::sema::modern
