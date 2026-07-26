#pragma once

#include "memory/dyn-array.hpp"

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zith::zirl {

// Little-endian byte buffer used by the zirl writer.  All integers are written
// in fixed-width little-endian form so artifacts are portable across hosts.
class ByteWriter {
    std::vector<uint8_t> buf_;

public:
    void writeU8(uint8_t v) { buf_.push_back(v); }
    void writeU16(uint16_t v) {
        buf_.push_back(static_cast<uint8_t>(v & 0xFFu));
        buf_.push_back(static_cast<uint8_t>((v >> 8u) & 0xFFu));
    }
    void writeU32(uint32_t v) {
        for (int i = 0; i < 4; ++i)
            buf_.push_back(static_cast<uint8_t>((v >> (8u * i)) & 0xFFu));
    }
    void writeI32(int32_t v) { writeU32(static_cast<uint32_t>(v)); }
    void writeU64(uint64_t v) {
        for (int i = 0; i < 8; ++i)
            buf_.push_back(static_cast<uint8_t>((v >> (8u * i)) & 0xFFu));
    }
    void writeI64(int64_t v) { writeU64(static_cast<uint64_t>(v)); }
    void writeF64(double v) {
        uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        writeU64(bits);
    }
    void writeBytes(const uint8_t *data, size_t len) {
        buf_.insert(buf_.end(), data, data + len);
    }
    // Length-prefixed (u32) UTF-8 blob.
    void writeBlob(std::string_view s) {
        writeU32(static_cast<uint32_t>(s.size()));
        writeBytes(reinterpret_cast<const uint8_t *>(s.data()), s.size());
    }
    void writeRaw(std::string_view s) {
        writeBytes(reinterpret_cast<const uint8_t *>(s.data()), s.size());
    }

    [[nodiscard]] const std::vector<uint8_t> &data() const noexcept { return buf_; }
    [[nodiscard]] size_t size() const noexcept { return buf_.size(); }
    [[nodiscard]] const uint8_t *ptr() const noexcept { return buf_.data(); }
};

// Little-endian reader.  All methods return false on truncation so callers can
// treat any read failure as a corrupted artifact.
class ByteReader {
    const uint8_t *data_  = nullptr;
    size_t size_          = 0;
    size_t pos_           = 0;

public:
    ByteReader(const uint8_t *data, size_t size) : data_(data), size_(size) {}
    explicit ByteReader(std::string_view blob)
        : data_(reinterpret_cast<const uint8_t *>(blob.data())), size_(blob.size()) {}

    [[nodiscard]] bool empty() const noexcept { return pos_ >= size_; }
    [[nodiscard]] size_t remaining() const noexcept { return size_ - pos_; }
    [[nodiscard]] size_t position() const noexcept { return pos_; }

    bool readU8(uint8_t &out) {
        if (pos_ + 1 > size_)
            return false;
        out = data_[pos_++];
        return true;
    }
    bool readU16(uint16_t &out) {
        if (pos_ + 2 > size_)
            return false;
        out = static_cast<uint16_t>(data_[pos_]) | (static_cast<uint16_t>(data_[pos_ + 1]) << 8u);
        pos_ += 2;
        return true;
    }
    bool readU32(uint32_t &out) {
        if (pos_ + 4 > size_)
            return false;
        out = 0;
        for (int i = 0; i < 4; ++i)
            out |= static_cast<uint32_t>(data_[pos_ + i]) << (8u * i);
        pos_ += 4;
        return true;
    }
    bool readI32(int32_t &out) {
        uint32_t u = 0;
        if (!readU32(u))
            return false;
        out = static_cast<int32_t>(u);
        return true;
    }
    bool readU64(uint64_t &out) {
        if (pos_ + 8 > size_)
            return false;
        out = 0;
        for (int i = 0; i < 8; ++i)
            out |= static_cast<uint64_t>(data_[pos_ + i]) << (8u * i);
        pos_ += 8;
        return true;
    }
    bool readI64(int64_t &out) {
        uint64_t u = 0;
        if (!readU64(u))
            return false;
        out = static_cast<int64_t>(u);
        return true;
    }
    bool readF64(double &out) {
        uint64_t bits = 0;
        if (!readU64(bits))
            return false;
        std::memcpy(&out, &bits, sizeof(out));
        return true;
    }
    bool readBytes(uint8_t *dst, size_t len) {
        if (pos_ + len > size_)
            return false;
        std::memcpy(dst, data_ + pos_, len);
        pos_ += len;
        return true;
    }
    bool readBlob(std::string &out) {
        uint32_t len = 0;
        if (!readU32(len))
            return false;
        if (pos_ + len > size_)
            return false;
        out.assign(reinterpret_cast<const char *>(data_ + pos_), len);
        pos_ += len;
        return true;
    }
    bool readBlob(std::string_view &out) {
        uint32_t len = 0;
        if (!readU32(len))
            return false;
        if (pos_ + len > size_)
            return false;
        out = std::string_view(reinterpret_cast<const char *>(data_ + pos_), len);
        pos_ += len;
        return true;
    }
};

} // namespace zith::zirl
