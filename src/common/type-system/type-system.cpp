#include "common/type-system/type-system.hpp"

#include <cstring>

namespace toolkit::type_system {

const TypeDesc *TypeContext::find(std::string_view name) const noexcept {
    if (const TypeDesc *entry = staticFindType(name))
        return entry;
    for (const TypeDesc &entry : customTypes_) {
        if (entry.name == name)
            return &entry;
    }
    return nullptr;
}

const TypeDesc *TypeContext::byId(TypeId id) const noexcept {
    if (id < kTypeTableCount)
        return &kTypeTable[id];
    const std::size_t index = id - kTypeTableCount;
    return index < customTypes_.size() ? &customTypes_[index] : nullptr;
}

TypeId TypeContext::byName(common::memory::InternedId name) const noexcept {
    if (const TypeId *id = namedTypes_.get(name))
        return *id;
    for (std::size_t i = 0; i < kTypeTableCount; ++i) {
        if (kTypeTable[i].name == interner_.lookup(name))
            return static_cast<TypeId>(i);
    }
    return kInvalidTypeId;
}

bool TypeContext::canCoerce(TypeId from, TypeId to) const noexcept {
    if (from == to)
        return true;
    if (staticCoercionKind(from, to) != CoercionKind::None)
        return true;
    for (const CoercionRule &rule : customCoercions_) {
        if (rule.from == from && rule.to == to)
            return true;
    }
    return false;
}

bool TypeContext::canImplicitCoerce(TypeId from, TypeId to) const noexcept {
    if (from == to)
        return true;
    if (staticCoercionKind(from, to) == CoercionKind::Implicit)
        return true;
    for (const CoercionRule &rule : customCoercions_) {
        if (rule.from == from && rule.to == to)
            return rule.kind == CoercionKind::Implicit;
    }
    return false;
}

CastKind TypeContext::castKind(TypeId from, TypeId to) const noexcept {
    const CastKind staticKind = staticCastKind(from, to);
    if (staticKind != CastKind::None)
        return staticKind;
    for (const CastRule &rule : customCasts_) {
        if (rule.from == from && rule.to == to)
            return rule.kind;
    }
    return CastKind::None;
}

bool TypeContext::commonType(TypeId left, TypeId right,
                             TypeId &out) const noexcept {
    if (staticCommonType(left, right, out))
        return true;
    for (const CommonRule &rule : customCommons_) {
        if ((rule.left == left && rule.right == right) ||
            (rule.left == right && rule.right == left)) {
            out = rule.result;
            return true;
        }
    }
    return false;
}

const BinaryRule *
TypeContext::binaryResult(std::string_view op, TypeId left,
                          TypeId right) const noexcept {
    for (const BinaryRule &rule : customBinary_) {
        if (rule.op == op && rule.left == left && rule.right == right &&
            rule.resolver)
            return &rule;
    }
    for (const BinaryRule &rule : customBinary_) {
        if (rule.op == op && rule.left == left && rule.right == right)
            return &rule;
    }
    return nullptr;
}

const UnaryRule *
TypeContext::unaryResult(std::string_view name, TypeId operand) const noexcept {
    for (const UnaryRule &rule : customUnary_) {
        if (rule.op == name && rule.operand == operand &&
            rule.resolver)
            return &rule;
    }
    for (const UnaryRule &rule : customUnary_) {
        if (rule.op == name && rule.operand == operand)
            return &rule;
    }
    return nullptr;
}

void TypeContext::addCoercion(TypeId from, TypeId to, CoercionKind kind) {
    if (from == to || kind == CoercionKind::None)
        return;
    if (staticCoercionKind(from, to) != CoercionKind::None)
        return;
    for (const CoercionRule &rule : customCoercions_) {
        if (rule.from == from && rule.to == to)
            return;
    }
    customCoercions_.push(CoercionRule{.from = from, .to = to, .kind = kind});
}

void TypeContext::addCast(TypeId from, TypeId to, CastKind kind) {
    if (from == to || kind == CastKind::None)
        return;
    if (staticCastKind(from, to) != CastKind::None)
        return;
    for (const CastRule &rule : customCasts_) {
        if (rule.from == from && rule.to == to)
            return;
    }
    customCasts_.push(CastRule{.from = from, .to = to, .kind = kind});
}

void TypeContext::addCommon(TypeId left, TypeId right, TypeId result) {
    TypeId existing = kInvalidTypeId;
    if (staticCommonType(left, right, existing))
        return;
    for (const CommonRule &rule : customCommons_) {
        if ((rule.left == left && rule.right == right) ||
            (rule.left == right && rule.right == left))
            return;
    }
    customCommons_.push(
        CommonRule{.left = left, .right = right, .result = result});
}

void TypeContext::addBinary(std::string_view op, TypeId left, TypeId right,
                            BinaryResolver resolver, void *userData) {
    for (const BinaryRule &rule : customBinary_) {
        if (rule.op == op && rule.left == left && rule.right == right && rule.resolver)
            return;
    }
    customBinary_.push(BinaryRule{
        .op = op, .left = left, .right = right, .result = kInvalidTypeId,
        .resolver = resolver, .userData = userData});
}

void TypeContext::addBinary(std::string_view op, TypeId left, TypeId right,
                            TypeId result) {
    for (const BinaryRule &rule : customBinary_) {
        if (rule.op == op && rule.left == left && rule.right == right &&
            !rule.resolver)
            return;
    }
    customBinary_.push(BinaryRule{
        .op = op, .left = left, .right = right, .result = result,
        .resolver = nullptr, .userData = nullptr});
}

void TypeContext::addUnary(std::string_view op, TypeId from, TypeId result,
                           UnaryResolver resolver, void *userData) {
    for (const UnaryRule &rule : customUnary_) {
        if (rule.op == op && rule.operand == from)
            return;
    }
    customUnary_.push(UnaryRule{
        .op = op, .operand = from, .result = result,
        .resolver = resolver, .userData = userData});
}

void TypeContext::addCmp(TypeId left, TypeId right,
                         BinaryResolver resolver, void *userData) {
    addBinary("cmp", left, right, resolver, userData);
}

} // namespace toolkit::type_system
