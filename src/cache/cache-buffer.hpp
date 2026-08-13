#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace toolkit::cache {

class ByteWriter {
public:
    void writeBytes(std::string_view bytes);
    void writeU8(std::uint8_t value);
    void writeU32(std::uint32_t value);
    void writeU64(std::uint64_t value);
    void writeI64(std::int64_t value);
    void writeDouble(double value);
    void writeString(std::string_view value);

    [[nodiscard]] std::uint64_t size() const noexcept {
        return bytes_.size();
    }

    [[nodiscard]] const std::uint8_t *data() const noexcept {
        return bytes_.data();
    }

    [[nodiscard]] const std::vector<std::uint8_t> &bytes() const noexcept {
        return bytes_;
    }

    [[nodiscard]] std::vector<std::uint8_t> takeBytes() {
        return std::move(bytes_);
    }

private:
    std::vector<std::uint8_t> bytes_;
};

class ByteReader {
public:
    explicit ByteReader(std::string_view bytes) : bytes_(bytes) {}

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - position_;
    }

    [[nodiscard]] std::string_view view(std::size_t count);
    bool readBytes(std::size_t count, std::string_view &out);
    bool readU8(std::uint8_t &value);
    bool readU32(std::uint32_t &value);
    bool readU64(std::uint64_t &value);
    bool readI64(std::int64_t &value);
    bool readDouble(double &value);
    bool readString(std::string &value);

private:
    std::string_view bytes_;
    std::size_t position_ = 0;
};

} // namespace toolkit::cache
