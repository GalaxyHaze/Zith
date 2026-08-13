#pragma once

#include "common/memory/arena.hpp"
#include "common/memory/dyn-array.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

namespace toolkit::facts {

template <typename T>
concept Arithmetic = std::is_arithmetic_v<T>;

struct FactId {
  std::uint32_t value = 0;
  [[nodiscard]] friend constexpr bool operator==(FactId, FactId) = default;
};

struct WorldId {
  std::uint32_t value = 0;
  [[nodiscard]] friend constexpr bool operator==(WorldId, WorldId) = default;
};

struct FactRef {
  FactId id{};
  [[nodiscard]] friend constexpr bool operator==(FactRef, FactRef) = default;
};

enum class DomainKind : std::uint8_t { Unknown, Exact, Range, Runtime, Null };
enum class Query : std::uint8_t { True, False, Unknown, Maybe };
enum class Rel : std::uint8_t {
  Equal,
  NotEqual,
  Greater,
  GreaterEq,
  Lesser,
  LesserEq
};

template <Arithmetic Number> struct ValueDomain {
  DomainKind kind = DomainKind::Unknown;
  std::optional<Number> exact;
  std::optional<Number> lower;
  std::optional<Number> upper;
  bool lowerInclusive = false;
  bool upperInclusive = false;

  [[nodiscard]] static constexpr ValueDomain unknown() noexcept { return {}; }
  [[nodiscard]] static constexpr ValueDomain exactValue(Number value) noexcept {
    return {DomainKind::Exact, value, value, value, true, true};
  }
  [[nodiscard]] constexpr bool contains(Number value) const noexcept;
  [[nodiscard]] constexpr bool
  contains(const ValueDomain &other) const noexcept;
};

struct ConflictV2 {
  WorldId world{};
  FactRef lhs{};
  FactRef rhs{};
  Rel relation = Rel::Equal;
};

namespace detail {
template <Arithmetic Number> struct ValueExpr {
  enum class Kind : std::uint8_t { Fact, Constant, AddConstant };
  Kind kind = Kind::Constant;
  FactRef fact{};
  Number constant{};
};

template <Arithmetic Number> struct Formula {
  enum class Kind : std::uint8_t { Atom, And, Or };
  Kind kind = Kind::Atom;
  ValueExpr<Number> lhs{};
  Rel relation = Rel::Equal;
  ValueExpr<Number> rhs{};
  const Formula *left = nullptr;
  const Formula *right = nullptr;
};
} // namespace detail

template <Arithmetic Number> class FactStore {
public:
  using Expr = detail::ValueExpr<Number>;
  using Formula = detail::Formula<Number>;

  explicit FactStore(common::memory::Arena &arena);

  [[nodiscard]] WorldId root() const noexcept { return WorldId{}; }
  [[nodiscard]] FactRef fact(WorldId world);
  [[nodiscard]] FactRef globalFact() { return fact(root()); }
  [[nodiscard]] Expr value(FactRef fact) const noexcept;
  [[nodiscard]] Expr constant(Number value) const noexcept;
  [[nodiscard]] Expr add(FactRef fact, Number offset) const noexcept;
  [[nodiscard]] const Formula &atom(Expr lhs, Rel relation, Expr rhs);
  [[nodiscard]] const Formula &equal(FactRef lhs, Expr rhs) {
    return atom(value(lhs), Rel::Equal, rhs);
  }
  [[nodiscard]] const Formula &and_(const Formula &lhs, const Formula &rhs);
  [[nodiscard]] const Formula &or_(const Formula &lhs, const Formula &rhs);

  [[nodiscard]] common::memory::DynArray<WorldId>
  assume(WorldId world, const Formula &formula, bool expected = true);
  [[nodiscard]] WorldId merge(std::span<const WorldId> alternatives);

  [[nodiscard]] Query status(WorldId world, const Formula &formula) const;
  [[nodiscard]] Query status(WorldId world, Expr lhs, Rel relation,
                             Expr rhs) const;
  [[nodiscard]] ValueDomain<Number> domain(WorldId world, FactRef fact) const;
  [[nodiscard]] bool visible(WorldId world, FactRef fact) const;
  [[nodiscard]] std::span<const ConflictV2> conflicts() const noexcept;
  [[nodiscard]] bool hasConflicts() const noexcept {
    return !conflicts_.empty();
  }

private:
  struct Constraint {
    Expr lhs;
    Rel relation = Rel::Equal;
    Expr rhs;
  };
  struct WorldNode;
  struct Solver;
  struct Branch;

  [[nodiscard]] WorldId makeSplit_(WorldId parent);
  [[nodiscard]] WorldId makeMerge_(std::span<const WorldId> alternatives);
  void lower_(WorldId world, const Formula &formula, bool expected,
              common::memory::DynArray<WorldId> &out);
  void addConstraint_(WorldId world, Constraint constraint);
  [[nodiscard]] bool isVisible_(WorldId world, WorldId owner) const;
  void branches_(WorldId world, const common::memory::DynArray<WorldId> &suffix,
                 common::memory::DynArray<Branch> &out) const;
  [[nodiscard]] Solver solve_(const Branch &branch) const;
  [[nodiscard]] Query join_(WorldId world, const Formula &formula) const;

  common::memory::Arena &arena_;
  common::memory::DynArray<WorldNode> worlds_;
  common::memory::DynArray<WorldId> factOwners_;
  common::memory::DynArray<ConflictV2> conflicts_;
};

} // namespace toolkit::facts

#include "facts/fact-v2.tpp"
