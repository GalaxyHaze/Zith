#include "common/diagnostic/diagnostic.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <vector>

namespace common::diagnostic {

std::size_t levenshteinDistance(std::string_view a, std::string_view b) {
    const std::size_t n = a.size();
    const std::size_t m = b.size();
    if (n == 0) return m;
    if (m == 0) return n;
    std::vector<std::size_t> prev(m + 1);
    std::vector<std::size_t> curr(m + 1);
    for (std::size_t j = 0; j <= m; ++j)
        prev[j] = j;
    for (std::size_t i = 1; i <= n; ++i) {
        curr[0] = i;
        for (std::size_t j = 1; j <= m; ++j) {
            const std::size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            const std::size_t aValue = prev[j] + 1;
            const std::size_t bValue = curr[j - 1] + 1;
            const std::size_t cValue = prev[j - 1] + cost;
            curr[j] = std::min({aValue, bValue, cValue});
        }
        std::swap(prev, curr);
    }
    return prev[m];
}

static const char *skipOptionPrefix(const char *text) {
    while (*text == '-')
        ++text;
    return text;
}

bool hasPlausiblePrefix(const char *arg, const char *candidate) {
    const char *lhs = skipOptionPrefix(arg);
    const char *rhs = skipOptionPrefix(candidate);
    if (*lhs == '\0' || *rhs == '\0')
        return false;
    if (lhs[0] != rhs[0])
        return false;
    return true;
}

const char *bestSuggestion(const char *arg, const char *const *candidates) {
    const char *best = nullptr;
    std::size_t bestDistance = static_cast<std::size_t>(-1);
    for (std::size_t i = 0; candidates[i] != nullptr; ++i) {
        if (!hasPlausiblePrefix(arg, candidates[i]))
            continue;
        const std::size_t distance = levenshteinDistance(arg, candidates[i]);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = candidates[i];
        }
    }
    if (best == nullptr)
        return nullptr;
    const std::size_t threshold = (std::max(std::strlen(arg), std::strlen(best)) + 1U) / 2U;
    if (bestDistance < threshold)
        return best;
    return nullptr;
}

} // namespace common::diagnostic
