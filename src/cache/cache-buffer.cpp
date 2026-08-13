#include "cache/cache-buffer.hpp"

#include <cstring>
#include <limits>

namespace toolkit::cache {

namespace {

constexpr std::uint32_t kMaxLength = std::numeric_limits<std::uint32_t>::max();

bool fit(std::size_t value) noexcept {
    return value <= kMaxLength;
}

} // namespace

void ByteWriter::writeBytes(std::string_view bytes) {
    if (!bytes.empty())
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
}

void ByteWriter::writeU8(std::uint8_t value) {
    bytes_.push_back(value);
}

void ByteWriter::writeU32(std::uint32_t value) {
    const auto first = static_cast<std::uint8_t>(value & 0xFFu);
    const auto second = static_cast<std::uint8_t>((value >> 8u) & 0xFFu);
    const auto third = static_cast<std::uint8_t>((value >> 16u) & 0xFFu);
    const auto fourth = static_cast<std::uint8_t>((value >> 24u) & 0xFFu);
    bytes_.push_back(first);
    bytes_.push_back(second);
    bytes_.push_back(third);
    bytes_.push_back(fourth);
}

void ByteWriter::writeU64(std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        const auto byte = static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFu);
        bytes_.push_back(byte);
    }
}

void ByteWriter::writeI64(std::int64_t value) {
    writeU64(static_cast<std::uint64_t>(value));
}

void ByteWriter::writeDouble(double value) {
    static_assert(sizeof(value) == sizeof(std::uint64_t));
    std::uint64_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    writeU64(raw);
}

void ByteWriter::writeString(std::string_view value) {
    if (!fit(value.size()))
        return;
    writeU32(static_cast<std::uint32_t>(value.size()));
    writeBytes(value);
}

std::string_view ByteReader::view(std::size_t count) {
    if (count > remaining()) {
        position_ = bytes_.size();
        return {};
    }
    const std::string_view out = bytes_.substr(position_, count);
    position_ += count;
    return out;
}

bool ByteReader::readBytes(std::size_t count, std::string_view &out) {
    const std::string_view result = view(count);
    if (result.empty() && count != 0)
        return false;
    out = result;
    return true;
}

bool ByteReader::readU8(std::uint8_t &value) {
    if (remaining() < 1)
        return false;
    value = static_cast<std::uint8_t>(bytes_[position_]);
    ++position_;
    return true;
}

bool ByteReader::readU32(std::uint32_t &value) {
    if (remaining() < 4)
        return false;
    value = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes_[position_])) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes_[position_ + 1]))
             << 8u) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes_[position_ + 2]))
             << 16u) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes_[position_ + 3]))
             << 24u);
    position_ += 4;
    return true;
}

bool ByteReader::readU64(std::uint64_t &value) {
    if (remaining() < 8)
        return false;
    value = static_cast<std::uint64_t>(
                static_cast<unsigned char>(bytes_[position_ + 7]))
            << 56u |
            (static_cast<std::uint64_t>(
                 static_cast<unsigned char>(bytes_[position_ + 6]))
             << 48u) |
            (static_cast<std::uint64_t>(
                 static_cast<unsigned char>(bytes_[position_ + 5]))
             << 40u) |
            (static_cast<std::uint64_t>(
                 static_cast<unsigned char>(bytes_[position_ + 4]))
             << 32u) |
            (static_cast<std::uint64_t>(
                 static_cast<unsigned char>(bytes_[position_ + 3]))
             << 24u) |
            (static_cast<std::uint64_t>(
                 static_cast<unsigned char>(bytes_[position_ + 2]))
             << 16u) |
            (static_cast<std::uint64_t>(
                 static_cast<unsigned char>(bytes_[position_ + 1]))
             << 8u) |
            static_cast<std::uint64_t>(
                static_cast<unsigned char>(bytes_[position_]));
    position_ += 8;
    return true;
}

bool ByteReader::readI64(std::int64_t &value) {
    std::uint64_t raw = 0;
    if (!readU64(raw))
        return false;
    value = static_cast<std::int64_t>(raw);
    return true;
}

bool ByteReader::readDouble(double &value) {
    std::uint64_t raw = 0;
    if (!readU64(raw))
        return false;
    static_assert(sizeof(value) == sizeof(raw));
    std::memcpy(&value, &raw, sizeof(value));
    return true;
}

bool ByteReader::readString(std::string &value) {
    std::uint32_t size = 0;
    if (!readU32(size))
        return false;
    std::string_view bytes;
    if (!readBytes(size, bytes))
        return false;
    value.assign(bytes);
    return true;
}

} // namespace toolkit::cache
