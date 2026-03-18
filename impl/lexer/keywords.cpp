// impl/parser/keywords.cpp
#include "kalidous/kalidous.h"
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
    for (const unsigned char c : sv) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return mix64(h);
}

// ============================================================================
// Keyword + operator table
// ============================================================================

static constexpr auto TokenTable = std::to_array<std::pair<std::string_view, KalidousTokenType>>({

    // --- Inteiros com sinal -------------------------------------------------
    {"i8",   KALIDOUS_TOKEN_TYPE}, {"i16",  KALIDOUS_TOKEN_TYPE},
    {"i32",  KALIDOUS_TOKEN_TYPE}, {"i64",  KALIDOUS_TOKEN_TYPE},
    {"i128", KALIDOUS_TOKEN_TYPE}, {"i256", KALIDOUS_TOKEN_TYPE},

    // --- Inteiros sem sinal -------------------------------------------------
    {"u8",   KALIDOUS_TOKEN_TYPE}, {"u16",  KALIDOUS_TOKEN_TYPE},
    {"u32",  KALIDOUS_TOKEN_TYPE}, {"u64",  KALIDOUS_TOKEN_TYPE},
    {"u128", KALIDOUS_TOKEN_TYPE}, {"u256", KALIDOUS_TOKEN_TYPE},

    // --- Ponto flutuante ----------------------------------------------------
    {"f32",  KALIDOUS_TOKEN_TYPE}, {"f64",  KALIDOUS_TOKEN_TYPE},
    {"f128", KALIDOUS_TOKEN_TYPE},

    // --- Primitivos gerais --------------------------------------------------
    {"bool", KALIDOUS_TOKEN_TYPE}, {"void", KALIDOUS_TOKEN_TYPE},
        {"null", KALIDOUS_TOKEN_NULL},

    // --- Declarações de tipo ------------------------------------------------
    {"type",      KALIDOUS_TOKEN_TYPE},
    {"struct",    KALIDOUS_TOKEN_STRUCT},
    {"component", KALIDOUS_TOKEN_COMPONENT},
    {"enum",      KALIDOUS_TOKEN_ENUM},
    {"union",     KALIDOUS_TOKEN_UNION},
    {"family",    KALIDOUS_TOKEN_FAMILY},
    {"entity",    KALIDOUS_TOKEN_ENTITY},
    {"trait",     KALIDOUS_TOKEN_TRAIT},
    {"using",     KALIDOUS_TOKEN_TYPEDEF},
    {"implement", KALIDOUS_TOKEN_IMPLEMENT},
    {"fn", KALIDOUS_TOKEN_FN},

    // --- Bindings / modificadores de ownership ------------------------------
    {"let",        KALIDOUS_TOKEN_LET},
    {"var",        KALIDOUS_TOKEN_VAR},
    {"auto",       KALIDOUS_TOKEN_AUTO},
    {"const",      KALIDOUS_TOKEN_CONST},
    {"mut",        KALIDOUS_TOKEN_MUTABLE},
    {"global",     KALIDOUS_TOKEN_GLOBAL},
    {"persistent", KALIDOUS_TOKEN_PERSISTENT},
    {"local",      KALIDOUS_TOKEN_LOCAL},
    {"lend",       KALIDOUS_TOKEN_LEND},
    {"shared",     KALIDOUS_TOKEN_SHARED},
    {"view",       KALIDOUS_TOKEN_VIEW},
    {"unique",     KALIDOUS_TOKEN_UNIQUE},
    {"yield", KALIDOUS_TOKEN_YIELD},
    {"async", KALIDOUS_TOKEN_ASYNC},
    /*{"flowing", KALIDOUS_TOKEN_FLOWING},*/
    {"entry", KALIDOUS_TOKEN_ENTRY},
    {"noreturn", KALIDOUS_TOKEN_NORETURN},

    // --- Modificadores de acesso --------------------------------------------
    {"public",    KALIDOUS_TOKEN_MODIFIER},
    {"private",   KALIDOUS_TOKEN_MODIFIER},
    {"protected", KALIDOUS_TOKEN_MODIFIER},

    // --- Controle de fluxo --------------------------------------------------
    {"if",       KALIDOUS_TOKEN_IF},
    {"else",     KALIDOUS_TOKEN_ELSE},
    {"for",      KALIDOUS_TOKEN_FOR},
    {"in",       KALIDOUS_TOKEN_IN},
    {"switch",   KALIDOUS_TOKEN_SWITCH},
    {"return",   KALIDOUS_TOKEN_RETURN},
    {"break",    KALIDOUS_TOKEN_BREAK},
    {"continue", KALIDOUS_TOKEN_CONTINUE},
    {"goto",     KALIDOUS_TOKEN_GOTO},
    {"marker",   KALIDOUS_TOKEN_MARKER},
    {"scene",    KALIDOUS_TOKEN_SCENE},
    {"end",      KALIDOUS_TOKEN_END},

    // --- Concorrência -------------------------------------------------------
    {"spawn", KALIDOUS_TOKEN_SPAWN},
    {"await", KALIDOUS_TOKEN_AWAIT},

    // --- Tratamento de erros ------------------------------------------------
    {"try",   KALIDOUS_TOKEN_TRY},
    {"catch", KALIDOUS_TOKEN_CATCH},
    {"must",  KALIDOUS_TOKEN_MUST},

    // --- Operadores multi-caractere -----------------------------------------
    {"and", KALIDOUS_TOKEN_AND},
    {"or",  KALIDOUS_TOKEN_OR},
    {"not", KALIDOUS_TOKEN_NOT_EQUAL},
    {"==",  KALIDOUS_TOKEN_EQUAL},
    {">=",  KALIDOUS_TOKEN_GREATER_THAN_OR_EQUAL},
    {"<=",  KALIDOUS_TOKEN_LESS_THAN_OR_EQUAL},
    {"->",  KALIDOUS_TOKEN_ARROW},
    {"+=",  KALIDOUS_TOKEN_PLUS_EQUAL},
    {"-=",  KALIDOUS_TOKEN_MINUS_EQUAL},
    {"*=",  KALIDOUS_TOKEN_MULTIPLY_EQUAL},
    {"/=",  KALIDOUS_TOKEN_DIVIDE_EQUAL},
    {":=",  KALIDOUS_TOKEN_DECLARATION},
    {"...", KALIDOUS_TOKEN_DOTS},

    // --- Operadores simples -------------------------------------------------
    {"+", KALIDOUS_TOKEN_PLUS},
    {"-", KALIDOUS_TOKEN_MINUS},
    {"*", KALIDOUS_TOKEN_MULTIPLY},
    {"/", KALIDOUS_TOKEN_DIVIDE},
    {"%", KALIDOUS_TOKEN_MOD},
    {"=", KALIDOUS_TOKEN_ASSIGNMENT},
    {"<", KALIDOUS_TOKEN_LESS_THAN},
    {">", KALIDOUS_TOKEN_GREATER_THAN},
    {"!", KALIDOUS_TOKEN_BANG},
    {"?", KALIDOUS_TOKEN_QUESTION},

    // --- Delimitadores ------------------------------------------------------
    {"(", KALIDOUS_TOKEN_LPAREN},   {")", KALIDOUS_TOKEN_RPAREN},
    {"{", KALIDOUS_TOKEN_LBRACE},   {"}", KALIDOUS_TOKEN_RBRACE},
    {"[", KALIDOUS_TOKEN_LBRACKET}, {"]", KALIDOUS_TOKEN_RBRACKET},
    {",", KALIDOUS_TOKEN_COMMA},    {";", KALIDOUS_TOKEN_SEMICOLON},
    {":", KALIDOUS_TOKEN_COLON},    {".", KALIDOUS_TOKEN_DOT},
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

static constexpr size_t N           = TokenTable.size();
static constexpr size_t BucketCount = 128;
static constexpr size_t TableSize   = 256;

namespace {

struct PerfectHash {
    std::array<int16_t, TableSize>   table{};
    std::array<uint8_t, BucketCount> bucketSeed{};

    constexpr PerfectHash() {
        table.fill(-1);
        bucketSeed.fill(0);

        // Agrupar entradas por bucket
        std::array<uint8_t,                     BucketCount> counts{};
        std::array<std::array<uint16_t, 16>,    BucketCount> items{};

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
                    const size_t i    = items[b][k];
                    const size_t slot = mix64(hash64(TokenTable[i].first) ^ seed) % TableSize;

                    // Colisão intra-bucket
                    for (size_t j = 0; j < k; ++j) {
                        if (slots[j] == slot) { ok = false; break; }
                    }
                    if (!ok) break;

                    // Colisão cross-bucket (slots já ocupados)
                    if (table[slot] != -1) { ok = false; break; }

                    slots[k] = slot;
                }

                if (ok) {
                    bucketSeed[b] = seed;
                    for (size_t k = 0; k < counts[b]; ++k) {
                        const size_t i    = items[b][k];
                        const size_t slot = mix64(hash64(TokenTable[i].first) ^ seed) % TableSize;
                        table[slot]       = static_cast<int16_t>(i);
                    }
                    break;
                }
            }
        }
    }

    [[nodiscard]] constexpr KalidousTokenType lookup(std::string_view sv) const {
        if (sv.empty()) return KALIDOUS_TOKEN_IDENTIFIER;

        const uint64_t h   = hash64(sv);
        const size_t   b   = h % BucketCount;
        const size_t   idx = mix64(h ^ bucketSeed[b]) % TableSize;
        const int16_t  id  = table[idx];

        if (id < 0) return KALIDOUS_TOKEN_IDENTIFIER;
        return (TokenTable[id].first == sv) ? TokenTable[id].second
                                            : KALIDOUS_TOKEN_IDENTIFIER;
    }
};

constexpr auto g_hasher = PerfectHash{};

} // anonymous namespace

// ============================================================================
// C API
// ============================================================================

extern "C" KalidousTokenType kalidous_lookup_keyword(const char* str, const size_t len) {
    if (!str || len == 0) return KALIDOUS_TOKEN_IDENTIFIER;
    return g_hasher.lookup(std::string_view(str, len));
}