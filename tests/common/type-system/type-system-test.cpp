#include "common/type-system/type-system.hpp"

#include "common/memory/arena.hpp"
#include "common/memory/string-interner.hpp"

#include <cstdlib>
#include <array>
#include <iostream>
#include <span>

namespace {
using common::memory::Arena;
using common::memory::StringInterner;
using toolkit::type_system::BinaryResolver;
using toolkit::type_system::CastKind;
using toolkit::type_system::TypeContext;
using toolkit::type_system::TypeId;
using toolkit::type_system::TypeKind;

bool comparisonCalled = false;
toolkit::type_system::TypeId comparisonResult =
    toolkit::type_system::kInvalidTypeId;

toolkit::type_system::TypeId comparisonResolver(
    void *, const TypeContext &, TypeId, TypeId) {
  comparisonCalled = true;
  return comparisonResult;
}

void fail(const char *message) {
  std::cerr << "type-system-test: " << message << '\n';
  std::exit(1);
}

void check(bool condition, const char *message) {
  if (!condition)
    fail(message);
}
} // namespace

int main() {
  Arena arena;
  StringInterner interner(arena);
  TypeContext ctx(arena, interner);

  const auto *i32 = ctx.find("i32");
  check(i32 != nullptr, "find(\"i32\") must find builtin i32");
  check(i32->kind == TypeKind::I32, "i32 must have TypeKind::I32");

  const auto *i8 = ctx.find("i8");
  const auto *i16 = ctx.find("i16");
  const auto *i64 = ctx.find("i64");
  const auto *f64 = ctx.find("f64");
  check(i8 && i16 && i64 && f64, "core builtins must be present");

  check(ctx.canImplicitCoerce(i8->id, i32->id),
        "i8 must implicitly coerce to i32");
  check(!ctx.canImplicitCoerce(i64->id, f64->id),
        "i64 -> f64 must be explicit, not implicit");
  check(ctx.canCoerce(i64->id, f64->id),
        "i64 must explicitly coerce to f64");
  check(ctx.castKind(i32->id, i8->id) == CastKind::Truncate,
        "i32 -> i8 must be a truncating cast");

  TypeId common = toolkit::type_system::kInvalidTypeId;
  check(ctx.commonType(i8->id, i16->id, common) && common == i16->id,
        "commonType(i8, i16) must be i16");

  const TypeId i1 = ctx.find("i1")->id;
  comparisonResult = i1;

  const std::array<TypeId, 2> pointComponents = {i32->id, i32->id};
  const auto point =
      ctx.registerType<TypeKind::Struct>(interner.intern("Point"),
                                         pointComponents);
  check(ctx.byId(point) != nullptr &&
            ctx.byId(point)->componentCount == 2,
        "registerType must store composite components");
  check(ctx.find("Point") == ctx.byId(point),
        "custom type must be discoverable by name and id");

  ctx.addCmp(
      point, point,
      comparisonResolver,
      nullptr);

  const auto *cmp = ctx.binaryResult("cmp", point, point);
  check(cmp != nullptr, "addCmp must register a cmp binary rule");
  const TypeId result = cmp->resolver ? cmp->resolver(cmp->userData, ctx, point, point)
                                     : cmp->result;
  check(comparisonCalled && result == i1,
        "cmp resolver must run and return i1");

  return 0;
}
