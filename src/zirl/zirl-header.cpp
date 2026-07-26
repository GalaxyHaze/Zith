#include "zirl-header.hpp"

namespace zith::zirl {

uint32_t fnv1a32(std::string_view data) noexcept {
    return fnv1a32(reinterpret_cast<const uint8_t *>(data.data()), data.size());
}

uint32_t fnv1a32(const uint8_t *data, size_t len) noexcept {
    uint32_t hash = 0x811c9dc5u;
    for (size_t i = 0; i < len; ++i) {
        hash ^= data[i];
        hash *= 0x01000193u;
    }
    return hash;
}

} // namespace zith::zirl
