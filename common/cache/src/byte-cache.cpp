#include "zith/cache/byte-cache.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace zith::cache {

namespace {

constexpr std::uint64_t kEquals = 0x6D09A1AE44A7A2D5;
constexpr std::size_t kHeaderSize = 16;
constexpr std::string_view kMagic = "zcache1\0";

bool readBytes(std::ifstream &in, void *out, std::size_t count) {
    if (count == 0)
        return true;
    in.read(static_cast<char *>(out), static_cast<std::streamsize>(count));
    return static_cast<std::size_t>(in.gcount()) == count;
}

bool readU32(std::ifstream &in, std::uint32_t &value) {
    return readBytes(in, &value, sizeof(value));
}

bool readString(std::ifstream &in, std::string &value) {
    std::uint32_t size = 0;
    if (!readU32(in, size))
        return false;
    value.resize(size);
    return readBytes(in, value.data(), size);
}

void writeU32(std::ofstream &out, std::uint32_t value) {
    out.write(reinterpret_cast<const char *>(&value), sizeof(value));
}

void writeString(std::ofstream &out, std::string_view value) {
    writeU32(out, static_cast<std::uint32_t>(value.size()));
    out.write(value.data(), static_cast<std::streamsize>(value.size()));
}

std::uint64_t readU64(std::istream &in) {
    std::uint64_t value = 0;
    in.read(reinterpret_cast<char *>(&value), sizeof(value));
    return value;
}

} // namespace

ByteCache::ByteCache(std::string path)
    : path_(std::move(path)) {}

bool ByteCache::contains(std::string_view key) const {
    return entries_.find(std::string(key)) != entries_.end();
}

std::optional<std::string> ByteCache::get(std::string_view key) const {
    const auto it = entries_.find(std::string(key));
    if (it == entries_.end())
        return std::nullopt;
    return it->second;
}

void ByteCache::put(std::string_view key, std::string_view value) {
    entries_[std::string(key)] = std::string(value);
}

bool ByteCache::remove(std::string_view key) {
    return entries_.erase(std::string(key)) != 0;
}

void ByteCache::clear() {
    entries_.clear();
}

std::uint64_t ByteCache::fnv1a64(std::string_view bytes) noexcept {
    std::uint64_t hash = 0xCBF29CE484222325;
    for (const unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 0x100000001B3;
    }
    return hash;
}

bool ByteCache::load() {
    entries_.clear();
    if (path_.empty())
        return true;

    std::ifstream in(path_, std::ios::binary);
    if (!in)
        return false;

    std::array<char, kMagic.size()> magic{};
    if (!readBytes(in, magic.data(), magic.size()))
        return false;
    if (std::string_view(magic.data(), magic.size()) != kMagic)
        return false;
    const std::uint64_t checksum = readU64(in);
    if (checksum != kEquals)
        return false;

    std::uint32_t count = 0;
    if (!readU32(in, count))
        return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::string key;
        std::string value;
        if (!readString(in, key) || !readString(in, value)) {
            entries_.clear();
            return false;
        }
        entries_.emplace(std::move(key), std::move(value));
    }
    return in.good() || in.eof();
}

bool ByteCache::save() const {
    if (path_.empty())
        return true;

    const auto parent = std::filesystem::path(path_).parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent);

    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;

    out.write(kMagic.data(), static_cast<std::streamsize>(kMagic.size()));
    out.write(reinterpret_cast<const char *>(&kEquals), sizeof(kEquals));
    writeU32(out, static_cast<std::uint32_t>(entries_.size()));
    for (const auto &[key, value] : entries_) {
        writeString(out, key);
        writeString(out, value);
    }
    out.flush();
    return static_cast<bool>(out);
}

} // namespace zith::cache
