#include "common/diagnostic/diagnostic.hpp"

#include <cstdio>
#include <cstdlib>

int main() {
    using common::diagnostic::bestSuggestion;
    using common::diagnostic::levenshteinDistance;

    const char *candidates[] = {"build", "run", "fmt", nullptr};
    bool ok = true;

    ok = ok && (levenshteinDistance("abc", "adc") == 1);
    ok = ok && bestSuggestion("buid", candidates) == candidates[0];
    ok = ok && (bestSuggestion("", candidates) == nullptr);
    ok = ok && bestSuggestion("arrrrrrrrrrrrrrrrrr", candidates) == nullptr;

    if (!ok) {
        std::fprintf(stderr, "diagnostic basics failed\n");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
