#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstring>

// ============================================================================
// Global debug flags - set via command line args
// ============================================================================

bool g_debug_tokens = false;  // --tokens: print token stream
bool g_debug_dump = false;    // --dump: print AST

// ============================================================================
// Custom main to parse command-line arguments
// ============================================================================

int main(int argc, char *argv[]) {
    // Parse custom flags before passing to Catch2
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--tokens") == 0) {
            g_debug_tokens = true;
        } else if (std::strcmp(argv[i], "--dump") == 0) {
            g_debug_dump = true;
        }
    }

    // Initialize Catch2 session
    return Catch::Session().run(argc, argv);
}

// ============================================================================
// Placeholder test
// ============================================================================

TEST_CASE("Kalidous language tests placeholder", "[placeholder]") {
    REQUIRE(true);
}
