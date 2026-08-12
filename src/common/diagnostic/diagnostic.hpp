#pragma once
#include "common/memory/source-map.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace common::diagnostic {

enum class Severity { Note, Warning, Error };

struct Note {
    std::string message;
};

struct Diagnostic {
    memory::SourceSpan span{0, {0, 0}};
    Severity severity = Severity::Error;
    uint32_t code = 0;
    std::string message;
    std::vector<Note> notes;
};

std::size_t levenshteinDistance(std::string_view a, std::string_view b);
bool hasPlausiblePrefix(const char *arg, const char *candidate);
const char *bestSuggestion(const char *arg, const char *const *candidates);

} // namespace common::diagnostic
