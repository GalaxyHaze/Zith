#include "zirl-attrs-section.hpp"

namespace zith::zirl {

bool encodeAttrs(const cache::Artifact &artifact, ByteWriter &w) {
    w.writeU32(static_cast<uint32_t>(artifact.attrs_slots.size()));
    for (const auto &slot : artifact.attrs_slots) {
        w.writeU32(slot.slot);
        w.writeU8(slot.ownership);
        w.writeU8(slot.consumed);
        w.writeU8(slot.nonNull ? 1 : 0);
        w.writeU8(0); // reserved
    }

    w.writeU32(static_cast<uint32_t>(artifact.attrs_calls.size()));
    for (const auto &call : artifact.attrs_calls) {
        w.writeU32(call.expr_id);
        w.writeU32(call.returns_arg);
        w.writeU32(static_cast<uint32_t>(call.arg_escapes.size()));
        for (auto escape : call.arg_escapes)
            w.writeU32(escape);
    }

    w.writeU32(static_cast<uint32_t>(artifact.attrs_fns.size()));
    for (const auto &fn : artifact.attrs_fns) {
        w.writeU32(fn.fn_index);
        w.writeU8(fn.return_consumed);
        w.writeU8(fn.nonNull ? 1 : 0);
        w.writeU8(fn.noAlias ? 1 : 0);
        w.writeU8(fn.readOnly ? 1 : 0);
        w.writeU8(fn.noCapture ? 1 : 0);
        w.writeU8(0);
        w.writeU8(0);
        w.writeU8(0);
    }
    return true;
}

bool decodeAttrs(ByteReader &r, cache::Artifact &out) {
    uint32_t n = 0;
    if (!r.readU32(n))
        return false;
    out.attrs_slots.resize(n);
    for (auto &slot : out.attrs_slots) {
        uint8_t a = 0, b = 0, c = 0, reserved = 0;
        if (!r.readU32(slot.slot) || !r.readU8(a) || !r.readU8(b) || !r.readU8(c) ||
            !r.readU8(reserved))
            return false;
        slot.ownership = a;
        slot.consumed  = b;
        slot.nonNull   = c != 0;
    }

    if (!r.readU32(n))
        return false;
    out.attrs_calls.resize(n);
    for (auto &call : out.attrs_calls) {
        if (!r.readU32(call.expr_id) || !r.readU32(call.returns_arg))
            return false;
        uint32_t count = 0;
        if (!r.readU32(count))
            return false;
        call.arg_escapes.resize(count);
        for (auto &escape : call.arg_escapes)
            if (!r.readU32(escape))
                return false;
    }

    if (!r.readU32(n))
        return false;
    out.attrs_fns.resize(n);
    for (auto &fn : out.attrs_fns) {
        uint8_t a = 0, b = 0, c = 0, d = 0, e = 0, f = 0, g = 0, h = 0;
        if (!r.readU32(fn.fn_index) || !r.readU8(a) || !r.readU8(b) || !r.readU8(c) ||
            !r.readU8(d) || !r.readU8(e) || !r.readU8(f) || !r.readU8(g) || !r.readU8(h))
            return false;
        fn.return_consumed = a;
        fn.nonNull         = b != 0;
        fn.noAlias         = c != 0;
        fn.readOnly        = d != 0;
        fn.noCapture       = e != 0;
    }
    return true;
}

} // namespace zith::zirl
