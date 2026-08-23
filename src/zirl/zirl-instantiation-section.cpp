#include "zirl-instantiation-section.hpp"

namespace zith::zirl {

bool encodeInstantiations(const cache::Artifact &artifact, ByteWriter &w) {
    w.writeU32(static_cast<uint32_t>(artifact.instantiations.size()));
    for (const auto &instance : artifact.instantiations) {
        w.writeBlob(instance.module);
        w.writeBlob(instance.mangled);
        w.writeBlob(instance.template_name);
        w.writeU32(instance.decl_id);
        w.writeU32(static_cast<uint32_t>(instance.arg_types.size()));
        for (const auto &arg : instance.arg_types)
            w.writeBlob(arg);
    }
    return true;
}

bool decodeInstantiations(ByteReader &r, cache::Artifact &out) {
    uint32_t n = 0;
    if (!r.readU32(n))
        return false;
    out.instantiations.resize(n);
    for (auto &instance : out.instantiations) {
        if (!r.readBlob(instance.module) || !r.readBlob(instance.mangled) ||
            !r.readBlob(instance.template_name) || !r.readU32(instance.decl_id))
            return false;
        uint32_t count = 0;
        if (!r.readU32(count))
            return false;
        instance.arg_types.resize(count);
        for (auto &arg : instance.arg_types)
            if (!r.readBlob(arg))
                return false;
    }
    return true;
}

} // namespace zith::zirl
