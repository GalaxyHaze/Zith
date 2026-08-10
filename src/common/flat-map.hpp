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

namespace memory {

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
    static_assert(std::is_same_v<Key, std::string> || std::is_same_v<Key, std::string_view> ||
                      std::is_arithmetic_v<Key> || std::is_enum_v<Key> || std::is_pointer_v<Key>,
                  "FlatMap supports string, string_view, arithmetic, enum, and pointer keys");

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
        hash ^= hash >> 16;
        hash *= 0x9e3779b97f4a7c15ULL;
        return hash & (cap - 1);
    }

    template <typename LookupKey> size_t find_slot(LookupKey &&key, size_t hash) const {
        if (capacity() == 0)
            return SIZE_MAX;
        size_t idx = probe(hash, capacity());
        while (metadata_[idx] != FM_EMPTY) {
            if (metadata_[idx] == FM_OCCUPIED && hashes_[idx] == hash &&
                eq_(keys_[idx], std::forward<LookupKey>(key))) {
                return idx;
            }
            idx = (idx + 1) & (capacity() - 1);
        }
        return SIZE_MAX;
    }

    Value &insert_with_hash_(const Key &key, Value value, size_t hash) {
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
        ++size_;
        return values_[idx];
    }

    void rehash(size_t new_cap) {
        new_cap = std::max(new_cap, size_t(16));
        size_t power_of_two = 1;
        while (power_of_two < new_cap)
            power_of_two <<= 1;
        new_cap = power_of_two;

        auto old_keys   = std::move(keys_);
        auto old_values = std::move(values_);
        auto old_meta   = std::move(metadata_);
        auto old_hashes = std::move(hashes_);

        keys_.assign(new_cap, Key{});
        values_.assign(new_cap, Value{});
        metadata_.assign(new_cap, FM_EMPTY);
        hashes_.assign(new_cap, 0);
        size_ = 0;

        for (size_t i = 0; i < old_meta.size(); ++i) {
            if (old_meta[i] == FM_OCCUPIED)
                insert_with_hash_(std::move(old_keys[i]), std::move(old_values[i]), old_hashes[i]);
        }
    }

public:
    FlatMap() = default;

    explicit FlatMap(size_t initial_capacity) {
        reserve(initial_capacity);
    }

    FlatMap(const FlatMap &other)
        : metadata_(other.metadata_), keys_(other.keys_), values_(other.values_),
          hashes_(other.hashes_), size_(other.size_) {}

    auto operator=(const FlatMap &other) -> FlatMap & {
        if (this != &other) {
            metadata_ = other.metadata_;
            keys_     = other.keys_;
            values_   = other.values_;
            hashes_   = other.hashes_;
            size_     = other.size_;
        }
        return *this;
    }

    FlatMap(FlatMap &&other) noexcept
        : metadata_(std::move(other.metadata_)), keys_(std::move(other.keys_)),
          values_(std::move(other.values_)), hashes_(std::move(other.hashes_)), size_(other.size_) {
        other.size_ = 0;
    }

    auto operator=(FlatMap &&other) noexcept -> FlatMap & {
        if (this != &other) {
            metadata_ = std::move(other.metadata_);
            keys_     = std::move(other.keys_);
            values_   = std::move(other.values_);
            hashes_   = std::move(other.hashes_);
            size_     = other.size_;
            other.size_ = 0;
        }
        return *this;
    }

    void reserve(size_t cap) {
        if (cap > capacity())
            rehash(cap);
    }

    Value &insert(const Key &key, Value value) {
        return insert_with_hash_(key, std::move(value), hasher_(key));
    }

    Value &insert(const Key &key) {
        Value value{};
        return insert_with_hash_(key, std::move(value), hasher_(key));
    }

    Value *get(const Key &key, size_t hash) {
        size_t idx = find_slot(key, hash);
        return idx == SIZE_MAX ? nullptr : &values_[idx];
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

    template <typename LookupKey> Value *get(LookupKey &&key) {
        const size_t hash = hasher_(key);
        const size_t idx  = find_slot(std::forward<LookupKey>(key), hash);
        return idx == SIZE_MAX ? nullptr : &values_[idx];
    }

    template <typename LookupKey> const Value *get(LookupKey &&key) const {
        return const_cast<FlatMap *>(this)->get(std::forward<LookupKey>(key));
    }

    bool contains(const Key &key) const {
        return get(key) != nullptr;
    }

    template <typename LookupKey> bool contains(LookupKey &&key) const {
        return get(std::forward<LookupKey>(key)) != nullptr;
    }

    Value &operator[](const Key &key) {
        if (auto *value = get(key))
            return *value;
        return insert(key, Value{});
    }

    template <typename LookupKey> Value &operator[](LookupKey &&key) {
        if (auto *value = get(std::forward<LookupKey>(key)))
            return *value;
        return insert(Key{std::forward<LookupKey>(key)}, Value{});
    }

    void erase(const Key &key) {
        const size_t idx = find_slot(key, hasher_(key));
        if (idx != SIZE_MAX) {
            metadata_[idx] = FM_ERASED;
            --size_;
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
        FlatMap *map_ = nullptr;
        size_t idx_   = 0;
        friend class FlatMap;

    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type        = std::pair<const Key &, Value &>;
        using difference_type   = std::ptrdiff_t;

        iterator(FlatMap *map, size_t idx) : map_(map), idx_(idx) {
            if (map_ && idx_ < map_->capacity() && map_->metadata_[idx_] != FM_OCCUPIED)
                advance_();
        }

        value_type operator*() const {
            return {map_->keys_[idx_], map_->values_[idx_]};
        }

        auto operator++() -> iterator & {
            ++idx_;
            advance_();
            return *this;
        }

        bool operator!=(const iterator &other) const {
            return idx_ != other.idx_ || map_ != other.map_;
        }

    private:
        void advance_() {
            while (map_ && idx_ < map_->capacity() && map_->metadata_[idx_] != FM_OCCUPIED)
                ++idx_;
        }
    };

    class const_iterator {
        const FlatMap *map_ = nullptr;
        size_t idx_         = 0;
        friend class FlatMap;

    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type        = std::pair<const Key &, const Value &>;
        using difference_type   = std::ptrdiff_t;

        const_iterator(const FlatMap *map, size_t idx) : map_(map), idx_(idx) {
            if (map_ && idx_ < map_->capacity() && map_->metadata_[idx_] != FM_OCCUPIED)
                advance_();
        }

        value_type operator*() const {
            return {map_->keys_[idx_], map_->values_[idx_]};
        }

        auto operator++() -> const_iterator & {
            ++idx_;
            advance_();
            return *this;
        }

        bool operator!=(const const_iterator &other) const {
            return idx_ != other.idx_ || map_ != other.map_;
        }

    private:
        void advance_() {
            while (map_ && idx_ < map_->capacity() && map_->metadata_[idx_] != FM_OCCUPIED)
                ++idx_;
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

} // namespace memory
