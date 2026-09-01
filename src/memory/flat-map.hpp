#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace zith::memory {

struct TransparentStringHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
    size_t operator()(const std::string &s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
    size_t operator()(const char *s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
};

enum : uint8_t { FM_EMPTY = 0, FM_OCCUPIED = 1, FM_ERASED = 2 };

template <typename Key, typename Value,
          typename Hash = std::conditional_t<std::is_same_v<Key, std::string> ||
                                                 std::is_same_v<Key, std::string_view>,
                                             TransparentStringHash, std::hash<Key>>,
          typename Eq   = std::equal_to<>>
class FlatMap {
    static_assert(
        std::is_same_v<Key, std::string> || std::is_same_v<Key, std::string_view> ||
            std::is_arithmetic_v<Key> || std::is_enum_v<Key> || std::is_pointer_v<Key> ||
            std::is_class_v<Key>,
        "FlatMap supports standard keys, custom class keys, arithmetic, enum, and pointer keys");

    std::vector<uint8_t> metadata_;
    std::vector<Key> keys_;
    std::vector<Value> values_;
    std::vector<size_t> hashes_;
    size_t size_ = 0;

    Hash hasher_;
    Eq eq_;

    size_t capacity() const {
        return metadata_.size();
    }

    static size_t probe(size_t hash, size_t cap) {
        if (cap == 0)
            return 0;
        hash = hash ^ (hash >> 16);
        hash *= 0x9e3779b97f4a7c15ULL;
        return hash & (cap - 1);
    }

    // Internal: lookup with precomputed hash
    template <typename LK> size_t find_slot(LK &&key, size_t hash) const {
        if (capacity() == 0)
            return SIZE_MAX;
        size_t idx = probe(hash, capacity());
        while (metadata_[idx] != FM_EMPTY) {
            if (metadata_[idx] == FM_OCCUPIED && hashes_[idx] == hash &&
                eq_(keys_[idx], std::forward<LK>(key)))
                return idx;
            idx = (idx + 1) & (capacity() - 1);
        }
        return SIZE_MAX;
    }

    // Internal: insert with precomputed hash, overwrites existing
    Value &insert(const Key &key, Value value, size_t hash) {
        if (size_ >= capacity() * 7 / 10)
            rehash(capacity() ? capacity() * 2 : 16);
        size_t idx = probe(hash, capacity());
        while (metadata_[idx] == FM_OCCUPIED) {
            if (hashes_[idx] == hash && eq_(keys_[idx], key)) {
                values_[idx] = std::move(value);
                return values_[idx];
            }
            idx = (idx + 1) & (capacity() - 1);
        }
        metadata_[idx] = FM_OCCUPIED;
        keys_[idx]     = key;
        values_[idx]   = std::move(value);
        hashes_[idx]   = hash;
        size_++;
        return values_[idx];
    }

    void rehash(size_t new_cap) {
        new_cap  = std::max(new_cap, size_t(16));
        size_t p = 1;
        while (p < new_cap)
            p <<= 1;
        new_cap = p;

        auto old_keys   = std::move(keys_);
        auto old_values = std::move(values_);
        auto old_meta   = std::move(metadata_);
        auto old_hashes = std::move(hashes_);

        keys_.assign(new_cap, Key{});
        values_.assign(new_cap, Value{});
        metadata_.assign(new_cap, FM_EMPTY);
        hashes_.assign(new_cap, 0);
        size_ = 0;

        for (size_t i = 0; i < old_meta.size(); i++) {
            if (old_meta[i] == FM_OCCUPIED)
                insert(std::move(old_keys[i]), std::move(old_values[i]), old_hashes[i]);
        }
    }

public:
    FlatMap() = default;
    explicit FlatMap(size_t initial_capacity) {
        reserve(initial_capacity);
    }

    FlatMap(const FlatMap &o)
        : metadata_(o.metadata_), keys_(o.keys_), values_(o.values_), hashes_(o.hashes_),
          size_(o.size_) {}
    FlatMap &operator=(const FlatMap &o) {
        if (this != &o) {
            metadata_ = o.metadata_;
            keys_     = o.keys_;
            values_   = o.values_;
            hashes_   = o.hashes_;
            size_     = o.size_;
        }
        return *this;
    }
    FlatMap(FlatMap &&o) noexcept
        : metadata_(std::move(o.metadata_)), keys_(std::move(o.keys_)),
          values_(std::move(o.values_)), hashes_(std::move(o.hashes_)), size_(o.size_) {
        o.size_ = 0;
    }
    FlatMap &operator=(FlatMap &&o) noexcept {
        if (this != &o) {
            metadata_ = std::move(o.metadata_);
            keys_     = std::move(o.keys_);
            values_   = std::move(o.values_);
            hashes_   = std::move(o.hashes_);
            size_     = o.size_;
            o.size_   = 0;
        }
        return *this;
    }

    void reserve(size_t cap) {
        if (cap > capacity())
            rehash(cap);
    }

    Value &insert(const Key &key, Value value) {
        return insert(key, std::move(value), hasher_(key));
    }

    Value &insert(const Key &key) {
        Value v{};
        return insert(key, std::move(v), hasher_(key));
    }

    // Lookup with precomputed hash — lets callers (e.g. StringInterner)
    // compute the hash once and reuse it across get + insert-on-miss
    Value *get(const Key &key, size_t hash) {
        size_t idx = find_slot(key, hash);
        return (idx != SIZE_MAX) ? &values_[idx] : nullptr;
    }
    const Value *get(const Key &key, size_t hash) const {
        return const_cast<FlatMap *>(this)->get(key, hash);
    }

    Value *get(const Key &key) {
        return get(key, hasher_(key));
    }
    const Value *get(const Key &key) const {
        return const_cast<FlatMap *>(this)->get(key);
    }

    template <typename LK> Value *get(LK &&key) {
        size_t h   = hasher_(key);
        size_t idx = find_slot(std::forward<LK>(key), h);
        return (idx != SIZE_MAX) ? &values_[idx] : nullptr;
    }
    template <typename LK> const Value *get(LK &&key) const {
        return const_cast<FlatMap *>(this)->get(std::forward<LK>(key));
    }

    bool contains(const Key &key) const {
        return get(key) != nullptr;
    }
    template <typename LK> bool contains(LK &&key) const {
        return get(std::forward<LK>(key)) != nullptr;
    }

    Value &operator[](const Key &key) {
        Value *v = get(key);
        if (v)
            return *v;
        return insert(key, Value{});
    }

    template <typename LK> Value &operator[](LK &&key) {
        Value *v = get(std::forward<LK>(key));
        if (v)
            return *v;
        return insert(Key{std::forward<LK>(key)}, Value{});
    }

    void erase(const Key &key) {
        size_t idx = find_slot(key, hasher_(key));
        if (idx != SIZE_MAX) {
            metadata_[idx] = FM_ERASED;
            size_--;
        }
    }

    void clear() {
        std::fill(metadata_.begin(), metadata_.end(), FM_EMPTY);
        size_ = 0;
    }

    size_t size() const {
        return size_;
    }
    bool empty() const {
        return size_ == 0;
    }

    class iterator {
        FlatMap *map_;
        size_t idx_;
        friend class FlatMap;

    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type        = std::pair<const Key &, Value &>;
        using difference_type   = std::ptrdiff_t;

        iterator(FlatMap *map, size_t idx) : map_(map), idx_(idx) {
            if (map_ && idx_ < map_->capacity() && map_->metadata_[idx_] != FM_OCCUPIED)
                advance_past_empty();
        }

        value_type operator*() const {
            return {map_->keys_[idx_], map_->values_[idx_]};
        }

        iterator &operator++() {
            idx_++;
            advance_past_empty();
            return *this;
        }
        bool operator!=(const iterator &o) const {
            return idx_ != o.idx_ || map_ != o.map_;
        }

    private:
        void advance_past_empty() {
            while (map_ && idx_ < map_->capacity() && map_->metadata_[idx_] != FM_OCCUPIED)
                idx_++;
        }
    };

    class const_iterator {
        const FlatMap *map_;
        size_t idx_;
        friend class FlatMap;

    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type        = std::pair<const Key &, const Value &>;
        using difference_type   = std::ptrdiff_t;

        const_iterator(const FlatMap *map, size_t idx) : map_(map), idx_(idx) {
            if (map_ && idx_ < map_->capacity() && map_->metadata_[idx_] != FM_OCCUPIED)
                advance_past_empty();
        }

        value_type operator*() const {
            return {map_->keys_[idx_], map_->values_[idx_]};
        }

        const_iterator &operator++() {
            idx_++;
            advance_past_empty();
            return *this;
        }
        bool operator!=(const const_iterator &o) const {
            return idx_ != o.idx_ || map_ != o.map_;
        }

    private:
        void advance_past_empty() {
            while (map_ && idx_ < map_->capacity() && map_->metadata_[idx_] != FM_OCCUPIED)
                idx_++;
        }
    };

    iterator begin() {
        return iterator(this, 0);
    }
    iterator end() {
        return iterator(this, capacity());
    }
    const_iterator begin() const {
        return const_iterator(this, 0);
    }
    const_iterator end() const {
        return const_iterator(this, capacity());
    }
};

} // namespace zith::memory
