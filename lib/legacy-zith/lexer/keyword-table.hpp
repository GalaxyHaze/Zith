#pragma once
#include "legacy-zith/lexer/token.hpp"

#include <array>
#include <cstdint>

namespace zith::lexer {

namespace detail {

constexpr uint64_t hash64(std::string_view sv) {
    uint64_t h = 14695981039346656037ULL;
    for (auto c : sv) {
        h ^= static_cast<uint8_t>(c);
        h *= 1099511628211ULL;
    }
    return h;
}

constexpr uint64_t mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static constexpr auto TokenTable = std::to_array<std::pair<std::string_view, TokenKind>>({

    {"i8", TokenKind::Type},
    {"i16", TokenKind::Type},
    {"i32", TokenKind::Type},
    {"i64", TokenKind::Type},
    {"i128", TokenKind::Type},

    {"u8", TokenKind::Type},
    {"u16", TokenKind::Type},
    {"u32", TokenKind::Type},
    {"u64", TokenKind::Type},
    {"u128", TokenKind::Type},

    {"f32", TokenKind::Type},
    {"f64", TokenKind::Type},

    {"bool", TokenKind::Type},
    {"char", TokenKind::Type},
    {"void", TokenKind::Type},
    {"invalid", TokenKind::Type},
    {"unknown", TokenKind::Type},
    {"null", TokenKind::LitVal},
    {"true", TokenKind::LitVal},
    {"false", TokenKind::LitVal},

    {"type", TokenKind::Typedef},
    {"struct", TokenKind::Struct},
    {"component", TokenKind::Struct},
    {"enum", TokenKind::Struct},
    {"raw", TokenKind::Raw},
    {"unsafe", TokenKind::Raw},
    {"union", TokenKind::Struct},
    {"trait", TokenKind::Trait},
    {"interface", TokenKind::Interface},
    {"extends", TokenKind::Trait},
    {"dyn", TokenKind::Trait},
    {"implement", TokenKind::Implement},
    {"fn", TokenKind::Fn},
    {"import", TokenKind::Module},
    {"use", TokenKind::Using},
    {"context", TokenKind::Context},
    {"macro", TokenKind::Macro},
    {"export", TokenKind::Module},
    {"extern", TokenKind::Extern},
    {"from", TokenKind::Module},
    {"alias", TokenKind::Module},
    {"as", TokenKind::As},

    {"let", TokenKind::Variable},
    {"var", TokenKind::Variable},
    {"auto", TokenKind::Variable},
    {"const", TokenKind::Variable},
    {"mut", TokenKind::Mutable},
    {"global", TokenKind::Variable},
    {"lend", TokenKind::Ownership},
    {"share", TokenKind::Ownership},
    {"view", TokenKind::Ownership},
    {"unique", TokenKind::Ownership},
    {"belong", TokenKind::Ownership},
    {"yield", TokenKind::Yield},
    {"async", TokenKind::Fn},
    {"flow", TokenKind::Fn},
    {"dock", TokenKind::Label},
    {"never", TokenKind::Type},

    {"pub", TokenKind::Visibility},
    {"mod", TokenKind::Visibility},

    {"if", TokenKind::If},
    {"else", TokenKind::If},
    {"for", TokenKind::For},
    {"in", TokenKind::In},
    {"when", TokenKind::Match},
    {"match", TokenKind::Match},
    {"return", TokenKind::Control},
    {"break", TokenKind::Control},
    {"continue", TokenKind::Control},
    {"jump", TokenKind::Control},
    {"while", TokenKind::Control},
    {"marker", TokenKind::Label},

    {"spawn", TokenKind::Thread},
    {"await", TokenKind::Thread},

    {"with", TokenKind::Error},
    {"catch", TokenKind::Error},
    {"must", TokenKind::Must},
    {"throw", TokenKind::Error},
    {"fail", TokenKind::Error},
    {"drop", TokenKind::Drop},

    {"require", TokenKind::Require},
    {"is", TokenKind::Is},
    {"prefix", TokenKind::Word},
    {"suffix", TokenKind::Word},
    {"infix", TokenKind::Word},
    {"nop", TokenKind::Word},
    {"and", TokenKind::Logical},
    {"or", TokenKind::Logical},
    {"not", TokenKind::Logical},
    {"xor", TokenKind::Logical}});

static constexpr size_t N           = TokenTable.size();
static constexpr size_t BucketCount = 128;
static constexpr size_t TableSize   = 256;

struct PerfectHash {
    std::array<int16_t, TableSize> table{};
    std::array<uint8_t, BucketCount> bucketSeed{};

    constexpr PerfectHash() {
        table.fill(-1);
        bucketSeed.fill(0);

        std::array<uint8_t, BucketCount> counts{};
        std::array<std::array<uint16_t, 16>, BucketCount> items{};

        for (size_t i = 0; i < N; ++i) {
            const size_t b        = hash64(TokenTable[i].first) % BucketCount;
            items[b][counts[b]++] = static_cast<uint16_t>(i);
        }

        for (size_t b = 0; b < BucketCount; ++b) {
            if (counts[b] == 0)
                continue;
            for (uint8_t seed = 0; seed < 255; ++seed) {
                bool ok = true;
                std::array<size_t, 16> slots{};
                for (size_t k = 0; k < counts[b] && ok; ++k) {
                    const size_t i    = items[b][k];
                    const size_t slot = mix64(hash64(TokenTable[i].first) ^ seed) % TableSize;
                    for (size_t j = 0; j < k; ++j) {
                        if (slots[j] == slot) {
                            ok = false;
                            break;
                        }
                    }
                    if (!ok)
                        break;
                    if (table[slot] != -1) {
                        ok = false;
                        break;
                    }
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

    [[nodiscard]] constexpr TokenKind lookup(std::string_view sv) const {
        if (sv.empty())
            return TokenKind::Identifier;

        const uint64_t h = hash64(sv);
        const size_t b   = h % BucketCount;
        const size_t idx = mix64(h ^ bucketSeed[b]) % TableSize;
        const int16_t id = table[idx];

        if (id < 0)
            return TokenKind::Identifier;
        return (TokenTable[static_cast<size_t>(id)].first == sv)
                   ? TokenTable[static_cast<size_t>(id)].second
                   : TokenKind::Identifier;
    }
};

constexpr auto g_hasher = PerfectHash{};

constexpr bool hasDuplicateKeywords() noexcept {
    for (size_t i = 0; i < N; ++i)
        for (size_t j = i + 1; j < N; ++j)
            if (TokenTable[i].first == TokenTable[j].first)
                return true;
    return false;
}
static_assert(!hasDuplicateKeywords(), "TokenTable contains duplicate keywords");

constexpr bool hasBucketOverflow() noexcept {
    std::array<uint8_t, BucketCount> counts{};
    for (size_t i = 0; i < N; ++i) {
        const size_t b = hash64(TokenTable[i].first) % BucketCount;
        if (++counts[b] > 16)
            return true;
    }
    return false;
}
static_assert(!hasBucketOverflow(), "Bucket overflow (>16 keywords per bucket)");

constexpr bool allKeywordsPlaced() noexcept {
    for (size_t i = 0; i < N; ++i) {
        const uint64_t h  = hash64(TokenTable[i].first);
        const size_t b    = h % BucketCount;
        const size_t slot = mix64(h ^ g_hasher.bucketSeed[b]) % TableSize;
        const int16_t id  = g_hasher.table[slot];
        if (id < 0)
            return false;
        if (TokenTable[static_cast<size_t>(id)].first != TokenTable[i].first)
            return false;
    }
    return true;
}
static_assert(allKeywordsPlaced(), "Not all keywords placed in the perfect hash table");

} // namespace detail

inline TokenKind lookupKeyword(std::string_view sv) {
    return detail::g_hasher.lookup(sv);
}

} // namespace zith::lexer
