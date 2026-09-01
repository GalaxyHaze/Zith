#pragma once

#include "memory/flat-map.hpp"

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

// Open-addressing set mirroring FlatMap's storage profile. The slot metadata,
// stored hash and copy/move semantics intentionally match FlatMap so the two
// containers behave identically when mixed in cache-heavy passes.
template <typename Key,
          typename Hash = std::conditional_t<std::is_same_v<Key, std::string> ||
                                                 std::is_same_v<Key, std::string_view>,
                                             TransparentStringHash, std::hash<Key>>,
          typename Eq = std::equal_to<>>
class FlatSet {
    static_assert(std::is_same_v<Key, std::string> || std::is_same_v<Key, std::string_view> ||
                      std::is_arithmetic_v<Key> || std::is_enum_v<Key> || std::is_pointer_v<Key>,
                  "FlatSet supports string, string_view, arithmetic, enum, and pointer keys");

    std::vector<uint8_t> metadata_;
    std::vector<Key> keys_;
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

    template <typename LK> bool insert_slot(LK &&key, size_t hash) {
        if (size_ >= capacity() * 7 / 10)
            rehash(capacity() ? capacity() * 2 : 16);
        size_t idx = probe(hash, capacity());
        while (metadata_[idx] == FM_OCCUPIED) {
            if (hashes_[idx] == hash && eq_(keys_[idx], std::forward<LK>(key)))
                return false;
            idx = (idx + 1) & (capacity() - 1);
        }
        metadata_[idx] = FM_OCCUPIED;
        keys_[idx]     = Key{std::forward<LK>(key)};
        hashes_[idx]   = hash;
        size_++;
        return true;
    }

    void rehash(size_t new_cap) {
        new_cap = std::max(new_cap, size_t(16));
        size_t p = 1;
        while (p < new_cap)
            p <<= 1;
        new_cap = p;

        auto old_keys   = std::move(keys_);
        auto old_meta   = std::move(metadata_);
        auto old_hashes = std::move(hashes_);

        keys_.assign(new_cap, Key{});
        metadata_.assign(new_cap, FM_EMPTY);
        hashes_.assign(new_cap, 0);
        size_ = 0;

        for (size_t i = 0; i < old_meta.size(); i++) {
            if (old_meta[i] == FM_OCCUPIED)
                insert_slot(std::move(old_keys[i]), old_hashes[i]);
        }
    }

public:
    FlatSet() = default;
    explicit FlatSet(size_t initial_capacity) {
        reserve(initial_capacity);
    }

    FlatSet(const FlatSet &o)
        : metadata_(o.metadata_), keys_(o.keys_), hashes_(o.hashes_), size_(o.size_) {}
    FlatSet &operator=(const FlatSet &o) {
        if (this != &o) {
            metadata_ = o.metadata_;
            keys_     = o.keys_;
            hashes_   = o.hashes_;
            size_     = o.size_;
        }
        return *this;
    }
    FlatSet(FlatSet &&o) noexcept
        : metadata_(std::move(o.metadata_)), keys_(std::move(o.keys_)),
          hashes_(std::move(o.hashes_)), size_(o.size_) {
        o.size_ = 0;
    }
    FlatSet &operator=(FlatSet &&o) noexcept {
        if (this != &o) {
            metadata_ = std::move(o.metadata_);
            keys_     = std::move(o.keys_);
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

    // Returns true when a new element was inserted, false when already present.
    template <typename LK> bool insert(LK &&key) {
        return insert_slot(std::forward<LK>(key), hasher_(key));
    }

    template <typename LK> bool contains(LK &&key) const {
        return find_slot(std::forward<LK>(key), hasher_(key)) != SIZE_MAX;
    }

    template <typename LK> void erase(LK &&key) {
        const size_t hash = hasher_(key);
        const size_t idx  = find_slot(std::forward<LK>(key), hash);
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
        FlatSet *set_;
        size_t idx_;
        friend class FlatSet;

    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type        = const Key;
        using reference         = const Key &;
        using pointer           = const Key *;
        using difference_type   = std::ptrdiff_t;

        iterator(FlatSet *set, size_t idx) : set_(set), idx_(idx) {
            if (set_ && idx_ < set_->capacity() && set_->metadata_[idx_] != FM_OCCUPIED)
                advance_past_empty();
        }

        const Key &operator*() const {
            return set_->keys_[idx_];
        }

        iterator &operator++() {
            idx_++;
            advance_past_empty();
            return *this;
        }
        bool operator!=(const iterator &o) const {
            return idx_ != o.idx_ || set_ != o.set_;
        }

    private:
        void advance_past_empty() {
            while (set_ && idx_ < set_->capacity() && set_->metadata_[idx_] != FM_OCCUPIED)
                idx_++;
        }
    };

    class const_iterator {
        const FlatSet *set_;
        size_t idx_;
        friend class FlatSet;

    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type        = const Key;
        using reference         = const Key &;
        using pointer           = const Key *;
        using difference_type   = std::ptrdiff_t;

        const_iterator(const FlatSet *set, size_t idx) : set_(set), idx_(idx) {
            if (set_ && idx_ < set_->capacity() && set_->metadata_[idx_] != FM_OCCUPIED)
                advance_past_empty();
        }

        const Key &operator*() const {
            return set_->keys_[idx_];
        }

        const_iterator &operator++() {
            idx_++;
            advance_past_empty();
            return *this;
        }
        bool operator!=(const const_iterator &o) const {
            return idx_ != o.idx_ || set_ != o.set_;
        }

    private:
        void advance_past_empty() {
            while (set_ && idx_ < set_->capacity() && set_->metadata_[idx_] != FM_OCCUPIED)
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
