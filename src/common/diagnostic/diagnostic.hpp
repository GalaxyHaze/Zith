#pragma once
#include "common/memory/source-map.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace common::diagnostic {

enum class Severity { Note, Warning, Error };

struct Diagnostic {
    memory::SourceSpan span{0, {0, 0}};
    Severity severity = Severity::Error;
    std::string message;
};

std::size_t levenshteinDistance(std::string_view a, std::string_view b);
bool hasPlausiblePrefix(const char *arg, const char *candidate);
const char *bestSuggestion(const char *arg, const char *const *candidates);

} // namespace common::diagnostic
