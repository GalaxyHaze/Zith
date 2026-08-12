#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace zith::cache {

// In-memory byte cache with optional persistence through a small binary file.
// The file layout is intentionally simple and stable for v1.
class ByteCache {
public:
    explicit ByteCache(std::string path = {});

    bool contains(std::string_view key) const;
    std::optional<std::string> get(std::string_view key) const;
    void put(std::string_view key, std::string_view value);
    bool remove(std::string_view key);
    void clear();

    std::size_t size() const noexcept {
        return entries_.size();
    }

    bool load();
    bool save() const;

private:
    static std::uint64_t fnv1a64(std::string_view bytes) noexcept;

    std::string path_;
    std::unordered_map<std::string, std::string> entries_;
};

} // namespace zith::cache
