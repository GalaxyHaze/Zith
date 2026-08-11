#include "common/text/parse.hpp"

#include <charconv>
#include <cstdlib>
#include <string_view>
#include <system_error>

namespace common::text {

bool parseBool(std::string_view value, bool &out) noexcept {
    if (value == "true") { out = true; return true; }
    if (value == "false") { out = false; return true; }
    return false;
}

bool parseString(std::string_view value, std::string &out) noexcept {
    if (value.size() < 2 || value.front() != '"' || value.back() != '"')
        return false;
    out.clear();
    const std::string_view body = value.substr(1, value.size() - 2);
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '\\') {
            if (i + 1 >= body.size()) return false;
            const char escaped = body[++i];
            switch (escaped) {
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case '\\': out.push_back('\\'); break;
            case '"': out.push_back('"'); break;
            default: return false;
            }
        } else {
            out.push_back(body[i]);
        }
    }
    return true;
}

bool parseInt(std::string_view value, int &out) noexcept {
    const char *first = value.data();
    const char *last = value.data() + value.size();
    const auto result = std::from_chars(first, last, out, 10);
    return result.ec == std::errc{} && result.ptr == last;
}

bool parseLong(const char *text, long &value) noexcept {
    char *end = nullptr;
    value = std::strtol(text, &end, 10);
    return end != nullptr && *end == '\0';
}

bool parseStringList(std::string_view value, std::vector<std::string> &out) noexcept {
    if (value.size() < 2 || value.front() != '[' || value.back() != ']')
        return false;
    out.clear();
    std::string_view inner = value.substr(1, value.size() - 2);
    std::size_t cursor = 0;
    while (cursor < inner.size()) {
        while (cursor < inner.size() && (inner[cursor] == ' ' || inner[cursor] == '\t'))
            ++cursor;
        if (cursor == inner.size()) break;
        std::size_t at = cursor;
        bool inString = false;
        for (; at < inner.size(); ++at) {
            const char c = inner[at];
            if (c == '"' && (at == cursor || inner[at - 1] != '\\'))
                inString = !inString;
            if (!inString && c == ',') break;
        }
        if (inString) return false;
        std::size_t end = at;
        while (end > cursor && (inner[end - 1] == ' ' || inner[end - 1] == '\t'))
            --end;
        std::string itemText(inner.substr(cursor, end - cursor));
        std::string item;
        if (!parseString(itemText, item)) return false;
        out.push_back(item);
        if (at == inner.size()) return true;
        cursor = at + 1;
    }
    return true;
}

} // namespace common::text
