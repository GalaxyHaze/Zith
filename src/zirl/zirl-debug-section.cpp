#include "zirl-debug-section.hpp"

namespace zith::zirl {

bool encodeDebug(const cache::Artifact & /*artifact*/, ByteWriter & /*w*/) {
    // Reserved section: always empty payload.  When the format version bumps
    // this will be wired into the section table as section 5.
    return true;
}

bool decodeDebug(ByteReader & /*r*/, cache::Artifact & /*out*/) {
    // Reserved section: ignore the payload without failing.
    return true;
}

} // namespace zith::zirl
