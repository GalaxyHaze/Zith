#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace common::text {

bool parseBool(std::string_view s, bool &out) noexcept;
bool parseInt(std::string_view s, int &out) noexcept;
bool parseLong(const char *text, long &value) noexcept;
bool parseString(std::string_view value, std::string &out) noexcept;
bool parseStringList(std::string_view value, std::vector<std::string> &out) noexcept;

} // namespace common::text
