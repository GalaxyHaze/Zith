#include "zirl-code-section.hpp"

namespace zith::zirl {

namespace {

void writeCompactExpr(const cache::CompactExpr &e, ByteWriter &w) {
    w.writeU8(static_cast<uint8_t>(e.kind));
    w.writeU8(e.op);
    w.writeU8(e.flags);
    w.writeU8(0); // reserved
    w.writeU32(e.type_id);
    w.writeU32(e.ref_a);
    w.writeU32(e.ref_b);
    w.writeU32(e.ref_c);
    w.writeU32(e.ref_d);
    w.writeU32(e.ref_e);
    w.writeU32(e.ref_f);
    w.writeU32(e.name_id);
    w.writeI64(e.int_val);
    w.writeF64(e.flt_val);
    w.writeU32(static_cast<uint32_t>(e.args.size()));
    for (auto a : e.args)
        w.writeU32(a);
    w.writeU32(static_cast<uint32_t>(e.arg_types.size()));
    for (auto t : e.arg_types)
        w.writeU32(t);
}

bool readCompactExpr(ByteReader &r, cache::CompactExpr &out) {
    uint8_t kind = 0, op = 0, flags = 0, reserved = 0;
    if (!r.readU8(kind) || !r.readU8(op) || !r.readU8(flags) || !r.readU8(reserved))
        return false;
    out.kind  = static_cast<cache::CompactExprKind>(kind);
    out.op    = op;
    out.flags = flags;
    if (!r.readU32(out.type_id) || !r.readU32(out.ref_a) || !r.readU32(out.ref_b) ||
        !r.readU32(out.ref_c) || !r.readU32(out.ref_d) || !r.readU32(out.ref_e) ||
        !r.readU32(out.ref_f) || !r.readU32(out.name_id))
        return false;
    if (!r.readI64(out.int_val) || !r.readF64(out.flt_val))
        return false;
    uint32_t n = 0;
    if (!r.readU32(n))
        return false;
    out.args.resize(n);
    for (auto &a : out.args)
        if (!r.readU32(a))
            return false;
    if (!r.readU32(n))
        return false;
    out.arg_types.resize(n);
    for (auto &t : out.arg_types)
        if (!r.readU32(t))
            return false;
    return true;
}

} // namespace

bool encodeCode(const cache::Artifact &artifact, ByteWriter &w) {
    w.writeU32(static_cast<uint32_t>(artifact.functions.size()));
    for (const auto &fn : artifact.functions) {
        w.writeU32(fn.name_id);
        w.writeU8(fn.is_extern ? 1 : 0);
        w.writeU8(fn.is_variadic ? 1 : 0);
        w.writeU8(fn.instance_index != ~uint32_t{0} ? 1 : 0);
        w.writeU8(0);
        w.writeU32(fn.return_type_id);
        w.writeU32(fn.instance_index);
        w.writeU32(static_cast<uint32_t>(fn.param_type_ids.size()));
        for (auto id : fn.param_type_ids)
            w.writeU32(id);
        w.writeU32(static_cast<uint32_t>(fn.param_name_ids.size()));
        for (auto id : fn.param_name_ids)
            w.writeU32(id);
        w.writeU32(static_cast<uint32_t>(fn.blocks.size()));
        for (const auto &blk : fn.blocks) {
            w.writeU32(static_cast<uint32_t>(blk.insts.size()));
            for (auto id : blk.insts)
                w.writeU32(id);
            w.writeU32(blk.terminator);
        }
        w.writeU32(static_cast<uint32_t>(fn.exprs.size()));
        for (const auto &e : fn.exprs)
            writeCompactExpr(e, w);
    }
    w.writeU32(static_cast<uint32_t>(artifact.markers.size()));
    for (const auto &marker : artifact.markers) {
        w.writeU32(marker.name_id);
        w.writeU32(marker.marker_id);
        w.writeU8(marker.stackful ? 1 : 0);
        w.writeU8(0);
        w.writeU8(0);
        w.writeU8(0);
        w.writeU32(marker.blob_offset);
        w.writeU32(marker.body_expr);
        w.writeU32(marker.module_name_id);
        w.writeU32(static_cast<uint32_t>(marker.params.size()));
        for (const auto &param : marker.params) {
            w.writeU32(param.name_id);
            w.writeU32(param.type_id);
            w.writeU32(param.offset);
        }
    }
    return true;
}

bool decodeCode(ByteReader &r, cache::Artifact &out) {
    uint32_t n = 0;
    if (!r.readU32(n))
        return false;
    out.functions.resize(n);
    for (auto &fn : out.functions) {
        uint8_t ext = 0, a = 0, b = 0, c = 0;
        if (!r.readU32(fn.name_id) || !r.readU8(ext) || !r.readU8(a) || !r.readU8(b) ||
            !r.readU8(c) || !r.readU32(fn.return_type_id) || !r.readU32(fn.instance_index))
            return false;
        if (fn.name_id < out.strings.size())
            fn.name = out.strings[fn.name_id];
        fn.is_extern   = ext != 0;
        fn.is_variadic = a != 0;
        uint32_t k     = 0;
        if (!r.readU32(k))
            return false;
        fn.param_type_ids.resize(k);
        for (auto &id : fn.param_type_ids)
            if (!r.readU32(id))
                return false;
        if (!r.readU32(k))
            return false;
        fn.param_name_ids.resize(k);
        for (auto &id : fn.param_name_ids)
            if (!r.readU32(id))
                return false;
        if (!r.readU32(k))
            return false;
        fn.blocks.resize(k);
        for (auto &blk : fn.blocks) {
            uint32_t m = 0;
            if (!r.readU32(m))
                return false;
            blk.insts.resize(m);
            for (auto &id : blk.insts)
                if (!r.readU32(id))
                    return false;
            if (!r.readU32(blk.terminator))
                return false;
        }
        if (!r.readU32(k))
            return false;
        fn.exprs.resize(k);
        for (auto &e : fn.exprs)
            if (!readCompactExpr(r, e))
                return false;
    }
    if (!r.readU32(n))
        return false;
    out.markers.resize(n);
    for (auto &marker : out.markers) {
        uint8_t f = 0, a = 0, b = 0, d = 0;
        if (!r.readU32(marker.name_id) || !r.readU32(marker.marker_id) || !r.readU8(f) ||
            !r.readU8(a) || !r.readU8(b) || !r.readU8(d))
            return false;
        marker.stackful = f != 0;
        if (!r.readU32(marker.blob_offset) || !r.readU32(marker.body_expr) ||
            !r.readU32(marker.module_name_id))
            return false;
        uint32_t m = 0;
        if (!r.readU32(m))
            return false;
        marker.params.resize(m);
        for (auto &param : marker.params)
            if (!r.readU32(param.name_id) || !r.readU32(param.type_id) || !r.readU32(param.offset))
                return false;
    }
    return true;
}

} // namespace zith::zirl
