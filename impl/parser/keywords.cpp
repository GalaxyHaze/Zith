// impl/parser/keywords.cpp
#include "Nova/nova.h"
#include <string_view>
#include <array>

// --- Hash functions (same as before) ---
static constexpr uint64_t mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static constexpr uint64_t hash64(const std::string_view sv) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (const unsigned char c : sv) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return mix64(h);
}

// --- Keyword table (same as before) ---
static constexpr auto TokenTable = std::to_array<std::pair<std::string_view, NovaTokenType>>({
    // Types
    {"i8",      NOVA_TOKEN_IDENTIFIER}, {"i16",     NOVA_TOKEN_TYPE}, {"i32",     NOVA_TOKEN_TYPE},
    {"i64",     NOVA_TOKEN_TYPE}, {"u8",      NOVA_TOKEN_TYPE}, {"u16",     NOVA_TOKEN_TYPE},
    {"u32",     NOVA_TOKEN_TYPE}, {"u64",     NOVA_TOKEN_TYPE}, {"f32",     NOVA_TOKEN_TYPE},
    {"f64",     NOVA_TOKEN_TYPE}, {"bool",    NOVA_TOKEN_TYPE}, {"void",    NOVA_TOKEN_TYPE},

    // Declarations & control flow
    {"let",     NOVA_TOKEN_LET},       {"mutable", NOVA_TOKEN_MUTABLE},
    {"return",  NOVA_TOKEN_RETURN},    {"if",      NOVA_TOKEN_IF},
    {"else",    NOVA_TOKEN_ELSE},      {"while",   NOVA_TOKEN_WHILE},
    {"for",     NOVA_TOKEN_FOR},       {"in",      NOVA_TOKEN_IN},
    {"break",   NOVA_TOKEN_BREAK},     {"continue", NOVA_TOKEN_CONTINUE},
    {"switch",  NOVA_TOKEN_SWITCH},    {"struct",  NOVA_TOKEN_STRUCT},
    {"enum",    NOVA_TOKEN_ENUM},      {"union",   NOVA_TOKEN_UNION},
    {"family",  NOVA_TOKEN_FAMILY},    {"entity",  NOVA_TOKEN_ENTITY},

    // Modifiers
    {"public",    NOVA_TOKEN_MODIFIER},
    {"private",   NOVA_TOKEN_MODIFIER},
    {"protected", NOVA_TOKEN_MODIFIER},

    // Operators
    {"&&", NOVA_TOKEN_AND}, {"||", NOVA_TOKEN_OR},
    {"==", NOVA_TOKEN_EQUAL}, {"!=", NOVA_TOKEN_NOT_EQUAL},
    {">=", NOVA_TOKEN_GREATER_THAN_OR_EQUAL}, {"<=", NOVA_TOKEN_LESS_THAN_OR_EQUAL},
    {"->", NOVA_TOKEN_ARROW},
    {"+=", NOVA_TOKEN_PLUS_EQUAL}, {"-=", NOVA_TOKEN_MINUS_EQUAL},
    {"*=", NOVA_TOKEN_MULTIPLY_EQUAL}, {"/=", NOVA_TOKEN_DIVIDE_EQUAL},
    {"+", NOVA_TOKEN_PLUS}, {"-", NOVA_TOKEN_MINUS},
    {"*", NOVA_TOKEN_MULTIPLY}, {"/", NOVA_TOKEN_DIVIDE},
    {"=", NOVA_TOKEN_ASSIGNMENT}, {">", NOVA_TOKEN_GREATER_THAN},
    {"<", NOVA_TOKEN_LESS_THAN}, {"!", NOVA_TOKEN_NOT},
    {"%", NOVA_TOKEN_MOD},

    // Delimiters
    {"(", NOVA_TOKEN_LPAREN}, {")", NOVA_TOKEN_RPAREN},
    {"{", NOVA_TOKEN_LBRACE}, {"}", NOVA_TOKEN_RBRACE},
    {"[", NOVA_TOKEN_LBRACKET}, {"]", NOVA_TOKEN_RBRACKET},
    {",", NOVA_TOKEN_COMMA}, {";", NOVA_TOKEN_SEMICOLON},
    {":", NOVA_TOKEN_COLON}, {".", NOVA_TOKEN_DOT}, {"...", NOVA_TOKEN_DOTS}
});

constexpr size_t N = TokenTable.size();
constexpr size_t BucketCount = 64;
constexpr size_t TableSize = 128;

// --- Perfect hash table (computed at compile time) ---
namespace {
    struct PerfectHash {
        std::array<int16_t, TableSize> table{};
        std::array<uint8_t, BucketCount> bucketSeed{};

        constexpr PerfectHash() {
            table.fill(-1);
            bucketSeed.fill(0);

            std::array<size_t, N> bucket{};
            for (size_t i = 0; i < N; ++i)
                bucket[i] = hash64(TokenTable[i].first) % BucketCount;

            std::array<size_t, BucketCount> counts{};
            std::array<std::array<uint16_t, 8>, BucketCount> items{};
            for (size_t i = 0; i < N; ++i) {
                size_t b = bucket[i];
                items[b][counts[b]++] = static_cast<uint16_t>(i);
            }

            for (size_t b = 0; b < BucketCount; ++b) {
                if (counts[b] <= 1) {
                    if (counts[b] == 1) {
                        size_t i = items[b][0];
                        size_t idx = hash64(TokenTable[i].first) % TableSize;
                        table[idx] = static_cast<int16_t>(i);
                    }
                    continue;
                }

                for (uint8_t seed = 0; seed < 255; ++seed) {
                    bool collision = false;
                    std::array<int, 8> used{};
                    for (size_t k = 0; k < counts[b]; ++k) {
                        size_t i = items[b][k];
                        size_t idx = mix64(hash64(TokenTable[i].first) ^ seed) % TableSize;
                        if (table[idx] != -1) {
                            collision = true;
                            break;
                        }
                        used[k] = static_cast<int>(idx);
                    }
                    if (!collision) {
                        bucketSeed[b] = seed;
                        for (size_t k = 0; k < counts[b]; ++k) {
                            table[used[k]] = static_cast<int16_t>(items[b][k]);
                        }
                        break;
                    }
                }
            }
        }

        [[nodiscard]] constexpr NovaTokenType lookup(const std::string_view sv) const {
            if (sv.empty()) return NOVA_TOKEN_IDENTIFIER;

            const uint64_t h = hash64(sv);
            const size_t b = h % BucketCount;
            const uint8_t seed = bucketSeed[b];

            const size_t idx = (countsForBucket(b) <= 1)
                ? (h % TableSize)
                : (mix64(h ^ seed) % TableSize);

            const int16_t id = table[idx];
            if (id == -1) return NOVA_TOKEN_IDENTIFIER;
            return (TokenTable[id].first == sv) ? TokenTable[id].second : NOVA_TOKEN_IDENTIFIER;
        }

    private:
        static constexpr size_t countsForBucket(size_t b) {
            size_t c = 0;
            for (size_t i = 0; i < N; ++i)
                if ((hash64(TokenTable[i].first) % BucketCount) == b)
                    ++c;
            return c;
        }
    };

    constexpr auto g_hasher = PerfectHash{};
}

// --- C API wrapper ---
extern "C" NovaTokenType nova_lookup_keyword(const char* str, const size_t len) {
    if (!str || len == 0) return NOVA_TOKEN_IDENTIFIER;
    return g_hasher.lookup(std::string_view(str, len));
}