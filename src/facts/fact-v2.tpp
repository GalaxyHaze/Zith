#pragma once

#include <limits>

namespace toolkit::facts {

template <IntegralSigned Number>
constexpr bool ValueDomain<Number>::contains(Number value) const noexcept {
  if (kind == DomainKind::Unknown)
    return true;
  if (kind == DomainKind::Null)
    return false;
  if (lower && (value < *lower || (value == *lower && !lowerInclusive)))
    return false;
  if (upper && (value > *upper || (value == *upper && !upperInclusive)))
    return false;
  return true;
}
template <IntegralSigned Number>
constexpr bool
ValueDomain<Number>::contains(const ValueDomain &other) const noexcept {
  if (other.kind == DomainKind::Unknown)
    return true;
  if (other.kind == DomainKind::Null || kind == DomainKind::Null)
    return false;
  if (kind == DomainKind::Unknown)
    return true;
  if (other.exact)
    return contains(*other.exact);
  if (other.kind != DomainKind::Range || (!other.lower && !other.upper))
    return false;
  if (other.lower) {
    if (!lower) {
      if (!other.lowerInclusive)
        return false;
    } else {
      const bool belowLower = *lower < *other.lower;
      const bool atLower = *lower == *other.lower;
      if (other.lowerInclusive
              ? !(belowLower || (atLower && lowerInclusive))
              : !(belowLower || atLower))
        return false;
    }
  }
  if (other.upper) {
    if (!upper) {
      if (!other.upperInclusive)
        return false;
    } else {
      const bool aboveUpper = *upper > *other.upper;
      const bool atUpper = *upper == *other.upper;
      if (other.upperInclusive ? !(aboveUpper || (atUpper && upperInclusive))
                               : !(aboveUpper || atUpper))
        return false;
    }
  }
  return true;
}

template <IntegralSigned Number>
constexpr ValueDomain<Number>
ValueDomain<Number>::intersect(const ValueDomain &other) const noexcept {
  if (kind == DomainKind::Unknown || other.kind == DomainKind::Unknown)
    return kind == DomainKind::Unknown ? other : *this;
  if (kind == DomainKind::Null || other.kind == DomainKind::Null)
    return {DomainKind::Null};

  const auto pickLower = [](const ValueDomain &a,
                            const ValueDomain &b) -> std::optional<Number> {
    if (!a.lower)
      return b.lower;
    if (!b.lower)
      return a.lower;
    return *a.lower > *b.lower ? a.lower : b.lower;
  };
  const auto pickUpper = [](const ValueDomain &a,
                            const ValueDomain &b) -> std::optional<Number> {
    if (!a.upper)
      return b.upper;
    if (!b.upper)
      return a.upper;
    return *a.upper < *b.upper ? a.upper : b.upper;
  };

  const std::optional<Number> lower = pickLower(*this, other);
  const std::optional<Number> upper = pickUpper(*this, other);
  bool lowerInclusive = true;
  bool upperInclusive = true;
  if (lower) {
    lowerInclusive = false;
    if (this->lower && *this->lower == *lower && this->lowerInclusive)
      lowerInclusive = true;
    if (other.lower && *other.lower == *lower && other.lowerInclusive)
      lowerInclusive = true;
  }
  if (upper) {
    upperInclusive = false;
    if (this->upper && *this->upper == *upper && this->upperInclusive)
      upperInclusive = true;
    if (other.upper && *other.upper == *upper && other.upperInclusive)
      upperInclusive = true;
  }

  if (lower && upper) {
    if (*lower > *upper || (*lower == *upper &&
                            (!lowerInclusive || !upperInclusive)))
      return {DomainKind::Null};
  }

  if (lower && upper && *lower == *upper && lowerInclusive && upperInclusive)
    return ValueDomain<Number>::exactValue(*lower);
  if (!lower && !upper)
    return ValueDomain<Number>::unknown();
  return {DomainKind::Range, {}, lower, upper, lowerInclusive, upperInclusive};
}

template <IntegralSigned Number> struct FactStore<Number>::WorldNode {
  enum class Kind : std::uint8_t { Root, SplitChild, Merge };
  explicit WorldNode(common::memory::Arena &arena, Kind nodeKind)
      : kind(nodeKind), alternatives(arena), constraints(arena) {}
  Kind kind;
  WorldId parent{};
  common::memory::DynArray<WorldId> alternatives;
  common::memory::DynArray<Constraint> constraints;
  bool alive = true;
};

template <IntegralSigned Number> struct FactStore<Number>::Branch {
  explicit Branch(common::memory::Arena &arena) : nodes(arena) {}
  common::memory::DynArray<WorldId> nodes;
};

template <IntegralSigned Number> struct FactStore<Number>::Solver {
  enum class Truth : std::uint8_t { True, False, Unknown };
  explicit Solver(common::memory::Arena &arena, std::size_t factCount)
      : domains(arena), parent(arena), rank(arena), constraints(arena) {
    domains.resize(factCount);
    rank.resize(factCount, 0);
    for (std::size_t i = 0; i < factCount; ++i)
      parent.push(static_cast<std::uint32_t>(i));
  }
  common::memory::DynArray<ValueDomain<Number>> domains;
  common::memory::DynArray<std::uint32_t> parent;
  common::memory::DynArray<std::uint8_t> rank;
  common::memory::DynArray<Constraint> constraints;
  bool conflict = false;

  std::uint32_t find(std::uint32_t id) {
    while (parent[id] != id) {
      parent[id] = parent[parent[id]];
      id = parent[id];
    }
    return id;
  }
  void unite(std::uint32_t lhs, std::uint32_t rhs) {
    lhs = find(lhs);
    rhs = find(rhs);
    if (lhs == rhs)
      return;
    if (rank[lhs] < rank[rhs]) {
      const auto temp = lhs;
      lhs = rhs;
      rhs = temp;
    }
    parent[rhs] = lhs;
    if (rank[lhs] == rank[rhs])
      ++rank[lhs];
  }
  static bool add(Number lhs, Number rhs, Number &out) {
    if constexpr (std::is_integral_v<Number>) {
      if ((rhs > 0 && lhs > std::numeric_limits<Number>::max() - rhs) ||
          (rhs < 0 && lhs < std::numeric_limits<Number>::lowest() - rhs))
        return false;
    }
    out = static_cast<Number>(lhs + rhs);
    return true;
  }
  static bool subtract(Number lhs, Number rhs, Number &out) {
    if constexpr (std::is_integral_v<Number>) {
      if ((rhs > 0 && lhs < std::numeric_limits<Number>::lowest() + rhs) ||
          (rhs < 0 && lhs > std::numeric_limits<Number>::max() + rhs))
        return false;
    }
    out = static_cast<Number>(lhs - rhs);
    return true;
  }
  ValueDomain<Number> exprDomain(const Expr &expr) {
    if (expr.kind == Expr::Kind::Constant)
      return ValueDomain<Number>::exactValue(expr.constant);
    ValueDomain<Number> value = domains[find(expr.fact.id.value)];
    if (expr.kind == Expr::Kind::Fact)
      return value;
    if (value.lower) {
      Number n{};
      if (!add(*value.lower, expr.constant, n)) {
        conflict = true;
        return {};
      }
      value.lower = n;
    }
    if (value.upper) {
      Number n{};
      if (!add(*value.upper, expr.constant, n)) {
        conflict = true;
        return {};
      }
      value.upper = n;
    }
    if (value.exact) {
      Number n{};
      if (!add(*value.exact, expr.constant, n)) {
        conflict = true;
        return {};
      }
      value.exact = n;
    }
    return value;
  }
  Truth evaluate(const Constraint &c) {
    const auto sameExpr = [](const Expr &lhs, const Expr &rhs) {
      return lhs.kind == rhs.kind && lhs.fact == rhs.fact &&
             lhs.constant == rhs.constant;
    };
    const auto reverse = [](Rel relation) {
      switch (relation) {
      case Rel::Greater:
        return Rel::Lesser;
      case Rel::GreaterEq:
        return Rel::LesserEq;
      case Rel::Lesser:
        return Rel::Greater;
      case Rel::LesserEq:
        return Rel::GreaterEq;
      case Rel::Equal:
        return Rel::Equal;
      case Rel::NotEqual:
        return Rel::NotEqual;
      }
      return Rel::Equal;
    };
    const auto complement = [](Rel relation) {
      switch (relation) {
      case Rel::Equal:
        return Rel::NotEqual;
      case Rel::NotEqual:
        return Rel::Equal;
      case Rel::Greater:
        return Rel::LesserEq;
      case Rel::GreaterEq:
        return Rel::Lesser;
      case Rel::Lesser:
        return Rel::GreaterEq;
      case Rel::LesserEq:
        return Rel::Greater;
      }
      return Rel::NotEqual;
    };
    for (const auto &known : constraints) {
      Rel relation = known.relation;
      bool matches = sameExpr(known.lhs, c.lhs) && sameExpr(known.rhs, c.rhs);
      if (!matches && sameExpr(known.lhs, c.rhs) &&
          sameExpr(known.rhs, c.lhs)) {
        relation = reverse(relation);
        matches = true;
      }
      if (matches) {
        if (relation == c.relation)
          return Truth::True;
        if (relation == complement(c.relation))
          return Truth::False;
      }
    }
    const auto lhs = exprDomain(c.lhs);
    const auto rhs = exprDomain(c.rhs);
    if (conflict)
      return Truth::False;
    if (lhs.exact && rhs.exact) {
      const auto a = *lhs.exact, b = *rhs.exact;
      switch (c.relation) {
      case Rel::Equal:
        return a == b ? Truth::True : Truth::False;
      case Rel::NotEqual:
        return a != b ? Truth::True : Truth::False;
      case Rel::Greater:
        return a > b ? Truth::True : Truth::False;
      case Rel::GreaterEq:
        return a >= b ? Truth::True : Truth::False;
      case Rel::Lesser:
        return a < b ? Truth::True : Truth::False;
      case Rel::LesserEq:
        return a <= b ? Truth::True : Truth::False;
      }
    }
    if (lhs.lower && rhs.upper &&
        (*lhs.lower > *rhs.upper ||
         (*lhs.lower == *rhs.upper &&
          (!lhs.lowerInclusive || !rhs.upperInclusive)))) {
      return c.relation == Rel::Greater || c.relation == Rel::GreaterEq ||
                     c.relation == Rel::NotEqual
                 ? Truth::True
                 : Truth::False;
    }
    if (lhs.upper && rhs.lower &&
        (*lhs.upper < *rhs.lower ||
         (*lhs.upper == *rhs.lower &&
          (!lhs.upperInclusive || !rhs.lowerInclusive)))) {
      return c.relation == Rel::Lesser || c.relation == Rel::LesserEq ||
                     c.relation == Rel::NotEqual
                 ? Truth::True
                 : Truth::False;
    }
    return Truth::Unknown;
  }
  bool refine(FactRef fact, ValueDomain<Number> source, Number offset) {
    auto &target = domains[find(fact.id.value)];
    if (source.kind == DomainKind::Unknown)
      return false;
    if (source.exact) {
      Number n{};
      if (!add(*source.exact, offset, n)) {
        conflict = true;
        return false;
      }
      if (target.exact && *target.exact != n) {
        conflict = true;
        return false;
      }
      if (target.exact)
        return false;
      target = ValueDomain<Number>::exactValue(n);
      return true;
    }
    return false;
  }
  void run() {
    for (const auto &c : constraints)
      if (c.relation == Rel::Equal && c.lhs.kind != Expr::Kind::Constant &&
          c.rhs.kind != Expr::Kind::Constant &&
          c.lhs.kind == Expr::Kind::Fact && c.rhs.kind == Expr::Kind::Fact)
        unite(c.lhs.fact.id.value, c.rhs.fact.id.value);
    bool changed = true;
    while (changed && !conflict) {
      changed = false;
      for (const auto &c : constraints) {
        if (c.relation == Rel::Equal) {
          if (c.lhs.kind != Expr::Kind::Constant &&
              c.rhs.kind != Expr::Kind::Constant) {
            const Number lo = c.lhs.kind == Expr::Kind::AddConstant
                                  ? c.lhs.constant
                                  : Number{};
            const Number ro = c.rhs.kind == Expr::Kind::AddConstant
                                  ? c.rhs.constant
                                  : Number{};
            Number lhsOffset{};
            Number rhsOffset{};
            if (!subtract(ro, lo, lhsOffset) || !subtract(lo, ro, rhsOffset)) {
              conflict = true;
              break;
            }
            changed |= refine(c.lhs.fact, domains[find(c.rhs.fact.id.value)],
                              lhsOffset);
            changed |= refine(c.rhs.fact, domains[find(c.lhs.fact.id.value)],
                              rhsOffset);
          } else if (c.lhs.kind != Expr::Kind::Constant) {
            Number offset{};
            if (!subtract(Number{},
                          c.lhs.kind == Expr::Kind::AddConstant ? c.lhs.constant
                                                                : Number{},
                          offset)) {
              conflict = true;
              break;
            }
            changed |= refine(c.lhs.fact, exprDomain(c.rhs), offset);
          } else if (c.rhs.kind != Expr::Kind::Constant) {
            Number offset{};
            if (!subtract(Number{},
                          c.rhs.kind == Expr::Kind::AddConstant ? c.rhs.constant
                                                                : Number{},
                          offset)) {
              conflict = true;
              break;
            }
            changed |= refine(c.rhs.fact, exprDomain(c.lhs), offset);
          }
        }
        if (evaluate(c) == Truth::False) {
          conflict = true;
          break;
        }
      }
    }
  }
};

template <IntegralSigned Number>
FactStore<Number>::FactStore(common::memory::Arena &arena)
    : arena_(arena), worlds_(arena), factOwners_(arena), conflicts_(arena) {
  static_assert(IntegralSigned<Number>,
                "FactStore requires a signed integral value type");
  worlds_.emplace(arena_, WorldNode::Kind::Root);
}
template <IntegralSigned Number>
FactResult<FactRef> FactStore<Number>::fact(WorldId world) {
  if (!validWorld_(world))
    return FactResult<FactRef>{FactError{"FactStore::fact received an invalid WorldId"}};
  FactRef result{FactId{static_cast<std::uint32_t>(factOwners_.size())}};
  factOwners_.push(world);
  return result;
}
template <IntegralSigned Number>
auto FactStore<Number>::value(FactRef fact) const noexcept -> Expr {
  return {Expr::Kind::Fact, fact, {}};
}
template <IntegralSigned Number>
auto FactStore<Number>::constant(Number value) const noexcept -> Expr {
  return {Expr::Kind::Constant, {}, value};
}
template <IntegralSigned Number>
auto FactStore<Number>::add(FactRef fact, Number offset) const noexcept
    -> Expr {
  return {Expr::Kind::AddConstant, fact, offset};
}
template <IntegralSigned Number>
const typename FactStore<Number>::Formula &
FactStore<Number>::atom(Expr lhs, Rel relation, Expr rhs) {
  return *arena_.make<Formula>(
      Formula{Formula::Kind::Atom, lhs, relation, rhs});
}
template <IntegralSigned Number>
const typename FactStore<Number>::Formula &
FactStore<Number>::and_(const Formula &lhs, const Formula &rhs) {
  return *arena_.make<Formula>(
      Formula{Formula::Kind::And, {}, Rel::Equal, {}, &lhs, &rhs});
}
template <IntegralSigned Number>
const typename FactStore<Number>::Formula &
FactStore<Number>::or_(const Formula &lhs, const Formula &rhs) {
  return *arena_.make<Formula>(
      Formula{Formula::Kind::Or, {}, Rel::Equal, {}, &lhs, &rhs});
}
template <IntegralSigned Number>
WorldId FactStore<Number>::makeSplit_(WorldId parent) {
  const WorldId id{static_cast<std::uint32_t>(worlds_.size())};
  auto &node = worlds_.emplace(arena_, WorldNode::Kind::SplitChild);
  node.parent = parent;
  return id;
}
template <IntegralSigned Number>
WorldId FactStore<Number>::makeMerge_(std::span<const WorldId> alternatives) {
  const WorldId id{static_cast<std::uint32_t>(worlds_.size())};
  auto &node = worlds_.emplace(arena_, WorldNode::Kind::Merge);
  node.alternatives.appendRange(alternatives);
  return id;
}
template <IntegralSigned Number>
FactResult<WorldId>
FactStore<Number>::merge(std::span<const WorldId> alternatives) {
  if (alternatives.empty())
    return FactResult<WorldId>{FactError{"FactStore::merge requires at least one alternative"}};
  for (std::size_t index = 0; index < alternatives.size(); ++index) {
    if (!validWorld_(alternatives[index]))
      return FactResult<WorldId>{
          FactError{"FactStore::merge received an invalid WorldId"}};
    for (std::size_t previous = 0; previous < index; ++previous)
      if (alternatives[previous] == alternatives[index])
        return FactResult<WorldId>{
            FactError{"FactStore::merge rejects repeated alternatives"}};
  }
  return makeMerge_(alternatives);
}
template <IntegralSigned Number>
bool FactStore<Number>::isVisible_(WorldId world, WorldId owner) const {
  const auto &node = worlds_[world.value];
  if (world == owner)
    return true;
  if (node.kind == WorldNode::Kind::Root)
    return false;
  if (node.kind == WorldNode::Kind::SplitChild)
    return isVisible_(node.parent, owner);
  for (const auto alternative : node.alternatives)
    if (!isVisible_(alternative, owner))
      return false;
  return true;
}
template <IntegralSigned Number>
bool FactStore<Number>::visible_(WorldId world, FactRef fact) const {
  return fact.id.value < factOwners_.size() &&
         isVisible_(world, factOwners_[fact.id.value]);
}
template <IntegralSigned Number>
FactResult<bool> FactStore<Number>::visible(WorldId world, FactRef fact) const {
  if (!validWorld_(world))
    return FactResult<bool>{FactError{"FactStore::visible received an invalid WorldId"}};
  return visible_(world, fact);
}
template <IntegralSigned Number>
void FactStore<Number>::addConstraint_(WorldId world, Constraint constraint) {
  auto valid = [&](Expr e) {
    return e.kind == Expr::Kind::Constant || visible_(world, e.fact);
  };
  if (!valid(constraint.lhs) || !valid(constraint.rhs)) {
    conflicts_.push(
        {world, constraint.lhs.fact, constraint.rhs.fact, constraint.relation});
    worlds_[world.value].alive = false;
    return;
  }
  worlds_[world.value].constraints.push(constraint);
}
template <IntegralSigned Number>
void FactStore<Number>::lower_(WorldId world, const Formula &formula,
                               bool expected,
                               common::memory::DynArray<WorldId> &out) {
  if (!worlds_[world.value].alive)
    return;
  if (formula.kind == Formula::Kind::Atom) {
    Constraint c{formula.lhs, formula.relation, formula.rhs};
    if (!expected) {
      switch (c.relation) {
      case Rel::Equal:
        c.relation = Rel::NotEqual;
        break;
      case Rel::NotEqual:
        c.relation = Rel::Equal;
        break;
      case Rel::Greater:
        c.relation = Rel::LesserEq;
        break;
      case Rel::GreaterEq:
        c.relation = Rel::Lesser;
        break;
      case Rel::Lesser:
        c.relation = Rel::GreaterEq;
        break;
      case Rel::LesserEq:
        c.relation = Rel::Greater;
        break;
      }
    }
    addConstraint_(world, c);
    common::memory::DynArray<WorldId> suffix(arena_);
    common::memory::DynArray<Branch> branches(arena_);
    branches_(world, suffix, branches);
    bool any = false;
    for (const auto &branch : branches) {
      auto solver = solve_(branch);
      if (!solver.conflict)
        any = true;
    }
    if (!any) {
      worlds_[world.value].alive = false;
      conflicts_.push({world, c.lhs.fact, c.rhs.fact, c.relation});
    }
    if (worlds_[world.value].alive)
      out.push(world);
    return;
  }
  if ((formula.kind == Formula::Kind::And && expected) ||
      (formula.kind == Formula::Kind::Or && !expected)) {
    common::memory::DynArray<WorldId> first(arena_);
    lower_(world, *formula.left, expected, first);
    for (const auto candidate : first)
      lower_(candidate, *formula.right, expected, out);
    return;
  }
  const Formula *first = formula.left;
  const Formula *second = formula.right;
  lower_(makeSplit_(world), *first, expected, out);
  lower_(makeSplit_(world), *second, expected, out);
}
template <IntegralSigned Number>
auto FactStore<Number>::assume(WorldId world, const Formula &formula,
                               bool expected)
    -> FactResult<common::memory::DynArray<WorldId>> {
  if (!validWorld_(world))
    return FactResult<common::memory::DynArray<WorldId>>{
        FactError{"FactStore::assume received an invalid WorldId"}};
  common::memory::DynArray<WorldId> result(arena_);
  lower_(world, formula, expected, result);
  return result;
}
template <IntegralSigned Number>
void FactStore<Number>::branches_(
    WorldId world, const common::memory::DynArray<WorldId> &suffix,
    common::memory::DynArray<Branch> &out) const {
  const auto &node = worlds_[world.value];
  if (!node.alive)
    return;
  common::memory::DynArray<WorldId> next(arena_);
  next.appendRange(suffix.data(), suffix.size());
  next.push(world);
  if (node.kind == WorldNode::Kind::Root) {
    auto &branch = out.emplace(arena_);
    for (std::size_t i = next.size(); i > 0; --i)
      branch.nodes.push(next[i - 1]);
    return;
  }
  if (node.kind == WorldNode::Kind::SplitChild) {
    branches_(node.parent, next, out);
    return;
  }
  for (const auto alternative : node.alternatives)
    branches_(alternative, next, out);
}
template <IntegralSigned Number>
auto FactStore<Number>::solve_(const Branch &branch) const -> Solver {
  Solver solver(arena_, factOwners_.size());
  for (const auto nodeId : branch.nodes)
    for (const auto &constraint : worlds_[nodeId.value].constraints)
      solver.constraints.push(constraint);
  solver.run();
  return solver;
}
template <IntegralSigned Number>
auto FactStore<Number>::join_(WorldId world, const Formula &formula) const
    -> Query {
  common::memory::DynArray<WorldId> suffix(arena_);
  common::memory::DynArray<Branch> branches(arena_);
  branches_(world, suffix, branches);
  bool trueSeen = false, falseSeen = false, unknownSeen = false;
  for (const auto &branch : branches) {
    auto solver = solve_(branch);
    if (solver.conflict)
      continue;
    const auto eval = [&](const auto &self, const Formula &f) ->
        typename Solver::Truth {
          if (f.kind == Formula::Kind::Atom)
            return solver.evaluate({f.lhs, f.relation, f.rhs});
          const auto a = self(self, *f.left), b = self(self, *f.right);
          if (f.kind == Formula::Kind::And) {
            if (a == Solver::Truth::False || b == Solver::Truth::False)
              return Solver::Truth::False;
            if (a == Solver::Truth::True && b == Solver::Truth::True)
              return Solver::Truth::True;
          } else {
            if (a == Solver::Truth::True || b == Solver::Truth::True)
              return Solver::Truth::True;
            if (a == Solver::Truth::False && b == Solver::Truth::False)
              return Solver::Truth::False;
          }
          return Solver::Truth::Unknown;
        };
    switch (eval(eval, formula)) {
    case Solver::Truth::True:
      trueSeen = true;
      break;
    case Solver::Truth::False:
      falseSeen = true;
      break;
    case Solver::Truth::Unknown:
      unknownSeen = true;
      break;
    }
  }
  const unsigned kinds =
      unsigned(trueSeen) + unsigned(falseSeen) + unsigned(unknownSeen);
  if (kinds > 1)
    return Query::Maybe;
  if (trueSeen)
    return Query::True;
  if (falseSeen)
    return Query::False;
  return Query::Unknown;
}
template <IntegralSigned Number>
FactResult<Query> FactStore<Number>::status(WorldId world,
                                           const Formula &formula) const {
  if (!validWorld_(world))
    return FactResult<Query>{FactError{"FactStore::status received an invalid WorldId"}};
  return join_(world, formula);
}
template <IntegralSigned Number>
FactResult<Query> FactStore<Number>::status(WorldId world, Expr lhs,
                                           Rel relation, Expr rhs) const {
  if (!validWorld_(world))
    return FactResult<Query>{FactError{"FactStore::status received an invalid WorldId"}};
  return join_(world, Formula{Formula::Kind::Atom, lhs, relation, rhs});
}
template <IntegralSigned Number>
FactResult<Query> FactStore<Number>::evaluate(WorldId world,
                                             const Formula &formula) const {
  if (!validWorld_(world))
    return FactResult<Query>{FactError{"FactStore::evaluate received an invalid WorldId"}};
  return join_(world, formula);
}
template <IntegralSigned Number>
FactResult<Query> FactStore<Number>::evaluate(WorldId world, Expr lhs,
                                             Rel relation, Expr rhs) const {
  if (!validWorld_(world))
    return FactResult<Query>{FactError{"FactStore::evaluate received an invalid WorldId"}};
  return join_(world, Formula{Formula::Kind::Atom, lhs, relation, rhs});
}
template <IntegralSigned Number>
FactResult<ValueDomain<Number>>
FactStore<Number>::domain(WorldId world, FactRef fact) const {
  if (!validWorld_(world))
    return FactResult<ValueDomain<Number>>{
        FactError{"FactStore::domain received an invalid WorldId"}};
  if (!visible_(world, fact))
    return ValueDomain<Number>{DomainKind::Null};
  common::memory::DynArray<WorldId> suffix(arena_);
  common::memory::DynArray<Branch> branches(arena_);
  branches_(world, suffix, branches);
  if (branches.size() != 1)
    return ValueDomain<Number>{DomainKind::Null};
  auto solver = solve_(branches[0]);
  return solver.conflict ? ValueDomain<Number>{DomainKind::Null}
                         : solver.domains[solver.find(fact.id.value)];
}
template <IntegralSigned Number>
std::span<const ConflictV2> FactStore<Number>::conflicts() const noexcept {
  return {conflicts_.data(), conflicts_.size()};
}

} // namespace toolkit::facts
