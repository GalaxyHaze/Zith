// impl/parser/keywords.cpp
#include "zith/zith.hpp"
#include <string_view>
#include <array>

// ============================================================================
// Hash primitives
// ============================================================================

static constexpr uint64_t mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static constexpr uint64_t hash64(std::string_view sv) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (const unsigned char c: sv) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return mix64(h);
}

// ============================================================================
// Keyword + operator table
// ============================================================================

static constexpr auto TokenTable = std::to_array<std::pair<std::string_view, ZithTokenType> >({

    // --- Inteiros com sinal -------------------------------------------------
    {"i8", ZITH_TOKEN_TYPE}, {"i16", ZITH_TOKEN_TYPE},
    {"i32", ZITH_TOKEN_TYPE}, {"i64", ZITH_TOKEN_TYPE},
    {"i128", ZITH_TOKEN_TYPE}, {"i256", ZITH_TOKEN_TYPE},

    // --- Inteiros sem sinal -------------------------------------------------
    {"u8", ZITH_TOKEN_TYPE}, {"u16", ZITH_TOKEN_TYPE},
    {"u32", ZITH_TOKEN_TYPE}, {"u64", ZITH_TOKEN_TYPE},
    {"u128", ZITH_TOKEN_TYPE}, {"u256", ZITH_TOKEN_TYPE},

    // --- Ponto flutuante ----------------------------------------------------
    {"f32", ZITH_TOKEN_TYPE}, {"f64", ZITH_TOKEN_TYPE},
    {"f128", ZITH_TOKEN_TYPE},

    // --- Primitivos gerais --------------------------------------------------
    {"bool", ZITH_TOKEN_TYPE}, {"void", ZITH_TOKEN_TYPE},
    {"null", ZITH_TOKEN_NULL},

    // --- Declarações de tipo ------------------------------------------------
    {"type", ZITH_TOKEN_TYPE},
    {"struct", ZITH_TOKEN_STRUCT},
    {"component", ZITH_TOKEN_COMPONENT},
    {"enum", ZITH_TOKEN_ENUM},
    {"raw", ZITH_TOKEN_RAW},
    {"union", ZITH_TOKEN_UNION},
    {"family", ZITH_TOKEN_FAMILY},
    {"entity", ZITH_TOKEN_ENTITY},
    {"trait", ZITH_TOKEN_TRAIT},
    {"type", ZITH_TOKEN_TYPEDEF},
    {"implement", ZITH_TOKEN_IMPLEMENT},
    {"fn", ZITH_TOKEN_FN},
    {"import", ZITH_TOKEN_IMPORT},
    {"use", ZITH_TOKEN_USE},
    {"context", ZITH_TOKEN_CONTEXT},
    {"macro", ZITH_TOKEN_MACRO},
    {"export", ZITH_TOKEN_EXPORT},
    {"from", ZITH_TOKEN_FROM},
    {"as", ZITH_TOKEN_AS},

    // --- Bindings / modificadores de ownership ------------------------------
    {"let", ZITH_TOKEN_LET},
    {"var", ZITH_TOKEN_VAR},
    {"auto", ZITH_TOKEN_AUTO},
    {"const", ZITH_TOKEN_CONST},
    {"mut", ZITH_TOKEN_MUTABLE},
    {"global", ZITH_TOKEN_GLOBAL},
    {"persistent", ZITH_TOKEN_PERSISTENT},
    {"local", ZITH_TOKEN_LOCAL},
    {"lend", ZITH_TOKEN_LEND},
    {"shared", ZITH_TOKEN_SHARED},
    {"view", ZITH_TOKEN_VIEW},
    {"unique", ZITH_TOKEN_UNIQUE},
    {"yield", ZITH_TOKEN_YIELD},
    {"async", ZITH_TOKEN_ASYNC},
    {"flowing", ZITH_TOKEN_FLOWING},
    {"entry", ZITH_TOKEN_ENTRY},
    {"noreturn", ZITH_TOKEN_NORETURN},

    // --- Modificadores de acesso --------------------------------------------
    {"public", ZITH_TOKEN_MODIFIER},
    {"private", ZITH_TOKEN_MODIFIER},
    {"protected", ZITH_TOKEN_MODIFIER},

    // --- Controle de fluxo --------------------------------------------------
    {"if", ZITH_TOKEN_IF},
    {"else", ZITH_TOKEN_ELSE},
    {"for", ZITH_TOKEN_FOR},
    {"in", ZITH_TOKEN_IN},
    {"switch", ZITH_TOKEN_SWITCH},
    {"return", ZITH_TOKEN_RETURN},
    {"break", ZITH_TOKEN_BREAK},
    {"continue", ZITH_TOKEN_CONTINUE},
    {"goto", ZITH_TOKEN_GOTO},
    {"marker", ZITH_TOKEN_MARKER},
    {"scene", ZITH_TOKEN_SCENE},
    {"end", ZITH_TOKEN_END},

    // --- Concorrência -------------------------------------------------------
    {"spawn", ZITH_TOKEN_SPAWN},
    {"await", ZITH_TOKEN_AWAIT},

    // --- Tratamento de erros ------------------------------------------------
    {"try", ZITH_TOKEN_TRY},
    {"catch", ZITH_TOKEN_CATCH},
    {"must", ZITH_TOKEN_MUST},

    // --- Operadores multi-caractere -----------------------------------------
    {"and", ZITH_TOKEN_AND},
    {"or", ZITH_TOKEN_OR},
    {"not", ZITH_TOKEN_NOT_EQUAL},
    {"==", ZITH_TOKEN_EQUAL},
    {">=", ZITH_TOKEN_GREATER_THAN_OR_EQUAL},
    {"<=", ZITH_TOKEN_LESS_THAN_OR_EQUAL},
    {"->", ZITH_TOKEN_ARROW},
    {"+=", ZITH_TOKEN_PLUS_EQUAL},
    {"-=", ZITH_TOKEN_MINUS_EQUAL},
    {"*=", ZITH_TOKEN_MULTIPLY_EQUAL},
    {"/=", ZITH_TOKEN_DIVIDE_EQUAL},
    {":=", ZITH_TOKEN_DECLARATION},
    {"...", ZITH_TOKEN_DOTS},

    // --- Operadores simples -------------------------------------------------
    {"+", ZITH_TOKEN_PLUS},
    {"-", ZITH_TOKEN_MINUS},
    {"*", ZITH_TOKEN_MULTIPLY},
    {"/", ZITH_TOKEN_DIVIDE},
    {"%", ZITH_TOKEN_MOD},
    {"=", ZITH_TOKEN_ASSIGNMENT},
    {"<", ZITH_TOKEN_LESS_THAN},
    {">", ZITH_TOKEN_GREATER_THAN},
    {"!", ZITH_TOKEN_BANG},
    {"?", ZITH_TOKEN_QUESTION},

    // --- Delimitadores ------------------------------------------------------
    {"(", ZITH_TOKEN_LPAREN}, {")", ZITH_TOKEN_RPAREN},
    {"{", ZITH_TOKEN_LBRACE}, {"}", ZITH_TOKEN_RBRACE},
    {"[", ZITH_TOKEN_LBRACKET}, {"]", ZITH_TOKEN_RBRACKET},
    {",", ZITH_TOKEN_COMMA}, {";", ZITH_TOKEN_SEMICOLON},
    {":", ZITH_TOKEN_COLON}, {".", ZITH_TOKEN_DOT},
});

// ============================================================================
// Perfect hash (compile-time, dois níveis)
//
// Nível 1 : bucket = hash(str) % BucketCount  →  seleciona um seed
// Nível 2 : slot   = mix64(hash(str) ^ seed) % TableSize
//
// Lookup usa SEMPRE o caminho com seed — sem bifurcação por tamanho de bucket.
// Isso elimina o antigo scan O(N) de countsForBucket() em tempo de execução.
// ============================================================================

static constexpr size_t N = TokenTable.size();
static constexpr size_t BucketCount = 128;
static constexpr size_t TableSize = 256;

namespace {
    struct PerfectHash {
        std::array<int16_t, TableSize> table{};
        std::array<uint8_t, BucketCount> bucketSeed{};

        constexpr PerfectHash() {
            table.fill(-1);
            bucketSeed.fill(0);

            // Agrupar entradas por bucket
            std::array<uint8_t, BucketCount> counts{};
            std::array<std::array<uint16_t, 16>, BucketCount> items{};

            for (size_t i = 0; i < N; ++i) {
                const size_t b = hash64(TokenTable[i].first) % BucketCount;
                items[b][counts[b]++] = static_cast<uint16_t>(i);
            }

            // Resolver cada bucket: encontrar seed sem colisão
            for (size_t b = 0; b < BucketCount; ++b) {
                if (counts[b] == 0) continue;

                for (uint8_t seed = 0; seed < 255; ++seed) {
                    bool ok = true;
                    std::array<size_t, 16> slots{};

                    for (size_t k = 0; k < counts[b]; ++k) {
                        const size_t i = items[b][k];
                        const size_t slot = mix64(hash64(TokenTable[i].first) ^ seed) % TableSize;

                        // Colisão intra-bucket
                        for (size_t j = 0; j < k; ++j) {
                            if (slots[j] == slot) {
                                ok = false;
                                break;
                            }
                        }
                        if (!ok) break;

                        // Colisão cross-bucket (slots já ocupados)
                        if (table[slot] != -1) {
                            ok = false;
                            break;
                        }

                        slots[k] = slot;
                    }

                    if (ok) {
                        bucketSeed[b] = seed;
                        for (size_t k = 0; k < counts[b]; ++k) {
                            const size_t i = items[b][k];
                            const size_t slot = mix64(hash64(TokenTable[i].first) ^ seed) % TableSize;
                            table[slot] = static_cast<int16_t>(i);
                        }
                        break;
                    }
                }
            }
        }

        [[nodiscard]] constexpr ZithTokenType lookup(std::string_view sv) const {
            if (sv.empty()) return ZITH_TOKEN_IDENTIFIER;

            const uint64_t h = hash64(sv);
            const size_t b = h % BucketCount;
            const size_t idx = mix64(h ^ bucketSeed[b]) % TableSize;
            const int16_t id = table[idx];

            if (id < 0) return ZITH_TOKEN_IDENTIFIER;
            return (TokenTable[id].first == sv)
                       ? TokenTable[id].second
                       : ZITH_TOKEN_IDENTIFIER;
        }
    };

    constexpr auto g_hasher = PerfectHash{};
} // anonymous namespace

// ============================================================================
// C API
// ============================================================================

extern "C" ZithTokenType zith_lookup_keyword(const char *str, const size_t len) {
    if (!str || len == 0) return ZITH_TOKEN_IDENTIFIER;
    return g_hasher.lookup(std::string_view(str, len));
}