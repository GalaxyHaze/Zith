#pragma once

#include "common/memory/arena.hpp"
#include "common/memory/dyn-array.hpp"
#include "common/memory/flat-map.hpp"
#include "common/memory/string-interner.hpp"
#include "common/type-system/type-system-table.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace toolkit::type_system {

template <TypeKind Kind>
constexpr Category categoryForKind() noexcept {
  if constexpr (Kind == TypeKind::Struct || Kind == TypeKind::Enum)
    return Category::Nominal;
  else if constexpr (Kind == TypeKind::Union)
    return Category::Nominal;
  else
    return Category::UserDefined;
}

template <TypeKind Kind>
constexpr bool isCustomKind() noexcept {
  return Kind == TypeKind::Struct || Kind == TypeKind::Enum ||
         Kind == TypeKind::Union || Kind == TypeKind::UserDefined;
}

class TypeContext {
public:
  TypeContext(common::memory::Arena &arena,
              common::memory::StringInterner &interner)
      : arena_(arena), interner_(interner), customTypes_(arena),
        customCoercions_(arena), customCasts_(arena), customCommons_(arena),
        customBinary_(arena), customUnary_(arena) {}

  template <TypeKind Kind>
  TypeId registerType(common::memory::InternedId name,
                      std::span<const TypeId> components = {});

  [[nodiscard]] const TypeDesc *find(std::string_view name) const noexcept;
  [[nodiscard]] const TypeDesc *byId(TypeId id) const noexcept;
  [[nodiscard]] TypeId byName(common::memory::InternedId name) const noexcept;

  [[nodiscard]] bool canCoerce(TypeId from, TypeId to) const noexcept;
  [[nodiscard]] bool canImplicitCoerce(TypeId from, TypeId to) const noexcept;
  [[nodiscard]] CastKind castKind(TypeId from, TypeId to) const noexcept;
  [[nodiscard]] bool commonType(TypeId left, TypeId right,
                                TypeId &out) const noexcept;

  [[nodiscard]] const BinaryRule *binaryResult(std::string_view op,
                                               TypeId left,
                                               TypeId right) const noexcept;
  [[nodiscard]] const UnaryRule *unaryResult(std::string_view name,
                                             TypeId operand) const noexcept;

  void addCoercion(TypeId from, TypeId to, CoercionKind kind);
  void addCast(TypeId from, TypeId to, CastKind kind);
  void addCommon(TypeId left, TypeId right, TypeId result);
  void addBinary(std::string_view op, TypeId left, TypeId right,
                 BinaryResolver resolver, void *userData = nullptr);
  void addBinary(std::string_view op, TypeId left, TypeId right,
                 TypeId result);
  void addUnary(std::string_view op, TypeId from, TypeId result,
                UnaryResolver resolver = nullptr, void *userData = nullptr);
  void addCmp(TypeId left, TypeId right,
              BinaryResolver resolver = nullptr, void *userData = nullptr);

private:
  common::memory::Arena &arena_;
  common::memory::StringInterner &interner_;
  common::memory::DynArray<TypeDesc> customTypes_;
  common::memory::DynArray<CoercionRule> customCoercions_;
  common::memory::DynArray<CastRule> customCasts_;
  common::memory::DynArray<CommonRule> customCommons_;
  common::memory::DynArray<BinaryRule> customBinary_;
  common::memory::DynArray<UnaryRule> customUnary_;
  common::memory::FlatMap<common::memory::InternedId, TypeId> namedTypes_;
};

template <TypeKind Kind>
TypeId TypeContext::registerType(common::memory::InternedId name,
                                 std::span<const TypeId> components) {
  static_assert(isCustomKind<Kind>(),
                "registerType supports Struct, Enum, Union and UserDefined");
  const TypeId id = static_cast<TypeId>(kTypeTableCount + customTypes_.size());
  const TypeId *componentStorage = nullptr;
  if (!components.empty()) {
    componentStorage = static_cast<const TypeId *>(
        arena_.alloc(components.size() * sizeof(TypeId), alignof(TypeId)));
    for (std::size_t i = 0; i < components.size(); ++i) {
      auto *storage = const_cast<TypeId *>(componentStorage);
      storage[i] = components[i];
    }
  }
  customTypes_.push(TypeDesc{
      .name = interner_.lookup(name),
      .id = id,
      .kind = Kind,
      .category = categoryForKind<Kind>(),
      .bits = 0,
      .isSigned = false,
      .components = componentStorage,
      .componentCount = components.size(),
  });
  namedTypes_.insert(name, id);
  return id;
}

} // namespace toolkit::type_system
