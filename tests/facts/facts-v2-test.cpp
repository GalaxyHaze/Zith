#include "facts/fact-v2.hpp"

#include <array>
#include <climits>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>

namespace {
using toolkit::facts::DomainKind;
using toolkit::facts::FactRef;
using toolkit::facts::FactStore;
using toolkit::facts::Query;
using toolkit::facts::Rel;
using toolkit::facts::ValueDomain;
using toolkit::facts::WorldId;

void fail(const std::string &message) {
  std::cerr << "facts-v2-test: " << message << '\n';
  std::exit(1);
}

void check(bool condition, const char *message) {
  if (!condition)
    fail(message);
}

void checkStatus(const FactStore<int> &store, WorldId world,
                 const FactStore<int>::Formula &formula, Query expected,
                 const char *message) {
  const auto result = store.status(world, formula);
  check(result.isOk() && result.value() == expected, message);
}

auto assumeOne(FactStore<int> &store, WorldId world,
               const FactStore<int>::Formula &formula, bool expected,
               const char *message) -> WorldId {
  const auto result = store.assume(world, formula, expected);
  check(result.isOk() && result.value().size() == 1, message);
  return result.value()[0];
}

auto assumeValue(FactStore<int> &store, WorldId world,
                 const FactStore<int>::Formula &formula, bool expected,
                 const char *message)
    -> common::memory::DynArray<WorldId> {
  auto result = store.assume(world, formula, expected);
  check(result.isOk(), message);
  return std::move(result).value();
}

auto mergeValue(FactStore<int> &store, std::span<const WorldId> alternatives)
    -> WorldId {
  auto result = store.merge(alternatives);
  check(result.isOk(), "a valid merge must return a world");
  return result.value();
}

auto domainValue(const FactStore<int> &store, WorldId world, FactRef fact)
    -> ValueDomain<int> {
  auto result = store.domain(world, fact);
  check(result.isOk(), "a valid domain query must return a domain");
  return result.value();
}

auto visibleValue(const FactStore<int> &store, WorldId world, FactRef fact)
    -> bool {
  auto result = store.visible(world, fact);
  check(result.isOk(), "a valid visibility query must return a bool");
  return result.value();
}

constexpr auto complement(Rel relation) -> Rel {
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
  return Rel::Equal;
}

constexpr auto inverse(Rel relation) -> Rel {
  switch (relation) {
  case Rel::Equal:
  case Rel::NotEqual:
    return relation;
  case Rel::Greater:
    return Rel::Lesser;
  case Rel::GreaterEq:
    return Rel::LesserEq;
  case Rel::Lesser:
    return Rel::Greater;
  case Rel::LesserEq:
    return Rel::GreaterEq;
  }
  return Rel::Equal;
}

void testValueDomains() {
  const auto unknown = ValueDomain<int>::unknown();
  const auto exact = ValueDomain<int>::exactValue(5);
  const ValueDomain<int> closed{.kind = DomainKind::Range,
                                .lower = 2,
                                .upper = 7,
                                .lowerInclusive = true,
                                .upperInclusive = true};
  const ValueDomain<int> open{.kind = DomainKind::Range,
                              .lower = 2,
                              .upper = 7,
                              .lowerInclusive = false,
                              .upperInclusive = false};
  const ValueDomain<int> nullValue{.kind = DomainKind::Null};

  check(unknown.contains(-99), "unknown domains contain every concrete value");
  check(exact.contains(5) && !exact.contains(4),
        "exact domains contain only their exact value");
  check(closed.contains(2) && closed.contains(7),
        "closed ranges contain both boundaries");
  check(!open.contains(2) && !open.contains(7),
        "open ranges exclude both boundaries");
  check(!nullValue.contains(0), "null domains contain no concrete values");
  check(closed.contains(exact), "a range contains an exact value inside it");
  check(!closed.contains(ValueDomain<int>::exactValue(9)),
        "a range rejects an exact value outside it");
  check(closed.contains(open),
        "contains(domain) accepts an open range inside a closed range");
  check(exact.contains(unknown), "every domain contains unknown as top");
  check(!closed.contains(nullValue), "a range does not contain the null domain");
}

void testValueDomainIntersection() {
  const ValueDomain<int> closed{.kind = DomainKind::Range,
                                .lower = 2,
                                .upper = 7,
                                .lowerInclusive = true,
                                .upperInclusive = true};
  const ValueDomain<int> upperOpen{.kind = DomainKind::Range,
                                   .lower = 4,
                                   .upper = 9,
                                   .lowerInclusive = false,
                                   .upperInclusive = false};
  const ValueDomain<int> point{.kind = DomainKind::Exact,
                               .exact = 5,
                               .lower = 5,
                               .upper = 5,
                               .lowerInclusive = true,
                               .upperInclusive = true};
  const ValueDomain<int> partialLower{.kind = DomainKind::Range,
                                      .lower = 6,
                                      .lowerInclusive = false};
  const ValueDomain<int> partialUpper{.kind = DomainKind::Range,
                                      .upper = 7,
                                      .upperInclusive = true};
  const ValueDomain<int> right{.kind = DomainKind::Range,
                               .lower = 8,
                               .upper = 20,
                               .lowerInclusive = true,
                               .upperInclusive = true};
  const ValueDomain<int> nullValue{.kind = DomainKind::Null};
  const auto unknown = ValueDomain<int>::unknown();

  const auto mid = closed.intersect(upperOpen);
  check(mid.kind == DomainKind::Range && *mid.lower == 4 &&
            *mid.upper == 7 && !mid.lowerInclusive && mid.upperInclusive,
        "closed/open intersection keeps the tighter endpoints");
  check(closed.intersect(point).kind == DomainKind::Exact &&
            closed.intersect(point).exact == 5,
        "intersection that converges to one point is exact");
  check(closed.intersect(right).kind == DomainKind::Null,
        "disjoint ranges intersect to null");
  check(partialLower.intersect(partialUpper).kind == DomainKind::Range &&
            *partialLower.intersect(partialUpper).lower == 6 &&
            *partialLower.intersect(partialUpper).upper == 7 &&
            !partialLower.intersect(partialUpper).lowerInclusive &&
            partialLower.intersect(partialUpper).upperInclusive,
        "one-sided ranges keep each side and inclusive flags");
  const auto unknownClosed = unknown.intersect(closed);
  check(unknownClosed.kind == DomainKind::Range &&
            unknownClosed.lower == closed.lower &&
            unknownClosed.upper == closed.upper &&
            unknownClosed.lowerInclusive == closed.lowerInclusive &&
            unknownClosed.upperInclusive == closed.upperInclusive,
        "unknown intersection returns the other domain");
  const auto closedUnknown = closed.intersect(unknown);
  check(closedUnknown.kind == DomainKind::Range &&
            closedUnknown.lower == closed.lower &&
            closedUnknown.upper == closed.upper &&
            closedUnknown.lowerInclusive == closed.lowerInclusive &&
            closedUnknown.upperInclusive == closed.upperInclusive,
        "intersection with unknown returns the known domain");
  check(nullValue.intersect(closed).kind == DomainKind::Null,
        "null intersection is null");
  check(unknown.intersect(nullValue).kind == DomainKind::Null &&
            nullValue.intersect(unknown).kind == DomainKind::Null,
        "unknown and null still intersect to null because null is empty");
}

void testValueDomainSubset() {
  const ValueDomain<int> closed{.kind = DomainKind::Range,
                                .lower = 2,
                                .upper = 7,
                                .lowerInclusive = true,
                                .upperInclusive = true};
  const ValueDomain<int> open{.kind = DomainKind::Range,
                              .lower = 2,
                              .upper = 7,
                              .lowerInclusive = false,
                              .upperInclusive = false};
  const ValueDomain<int> inside{.kind = DomainKind::Range,
                                .lower = 3,
                                .upper = 6,
                                .lowerInclusive = true,
                                .upperInclusive = true};
  const ValueDomain<int> partialUpper{.kind = DomainKind::Range,
                                      .upper = 10,
                                      .upperInclusive = true};
  const ValueDomain<int> nullValue{.kind = DomainKind::Null};
  const auto unknown = ValueDomain<int>::unknown();

  check(closed.contains(closed), "identical ranges are subsets");
  check(closed.contains(inside), "ranges inside a closed range are subsets");
  check(closed.contains(open), "open boundary inside inclusive range is a subset");
  check(!open.contains(closed),
        "inclusive boundary is not inside an open range");
  check(!closed.contains(partialUpper),
        "one-sided wider range is not a subset");
  check(closed.contains(ValueDomain<int>::exactValue(5)),
        "exact point inside a range is a subset");
  check(closed.contains(unknown), "unknown is a superset of every domain");
  check(!closed.contains(nullValue), "null is not inside a value domain");
}

void testAtomicRelationsAndNegation() {
  constexpr std::array relations = {Rel::Equal,   Rel::NotEqual,
                                    Rel::Greater, Rel::GreaterEq,
                                    Rel::Lesser,  Rel::LesserEq};
  for (const auto relation : relations) {
    common::memory::Arena arena;
    FactStore<int> store(arena);
    const auto lhs = store.globalFact();
    const auto rhs = store.globalFact();
    const auto &atom = store.atom(store.value(lhs), relation, store.value(rhs));
    const auto world = assumeOne(store, store.root(), atom, true,
                                 "an atomic assumption must keep one world");
    checkStatus(store, world, atom, Query::True,
                "an assumed atom must be true");
    const auto &opposite =
        store.atom(store.value(lhs), complement(relation), store.value(rhs));
    checkStatus(store, world, opposite, Query::False,
                "the complementary atom must be false");
    const auto &reversed =
        store.atom(store.value(rhs), inverse(relation), store.value(lhs));
    checkStatus(store, world, reversed, Query::True,
                "the inverse relation in reverse order must be true");
  }

  for (const auto relation : relations) {
    common::memory::Arena arena;
    FactStore<int> store(arena);
    const auto lhs = store.globalFact();
    const auto rhs = store.globalFact();
    const auto &atom = store.atom(store.value(lhs), relation, store.value(rhs));
    const auto world =
        assumeOne(store, store.root(), atom, false,
                  "a negated atomic assumption must keep one world");
    const auto &opposite =
        store.atom(store.value(lhs), complement(relation), store.value(rhs));
    checkStatus(store, world, atom, Query::False,
                "a false expected atom must be false");
    checkStatus(store, world, opposite, Query::True,
                "a false expected atom must prove its exact complement");
  }
}

void testBooleanLoweringMatrix() {
  {
    common::memory::Arena arena;
    FactStore<int> store(arena);
    const auto a = store.globalFact();
    const auto b = store.globalFact();
    const auto &A = store.atom(store.value(a), Rel::Equal, store.constant(1));
    const auto &B = store.atom(store.value(b), Rel::Equal, store.constant(2));
    const auto &formula = store.and_(A, B);
    const auto world = assumeOne(store, store.root(), formula, true,
                                 "true A and B must use one world");
    checkStatus(store, world, formula, Query::True,
                "true A and B must prove the formula");
  }
  {
    common::memory::Arena arena;
    FactStore<int> store(arena);
    const auto a = store.globalFact();
    const auto b = store.globalFact();
    const auto &A = store.atom(store.value(a), Rel::Equal, store.constant(1));
    const auto &B = store.atom(store.value(b), Rel::Equal, store.constant(2));
    const auto &formula = store.or_(A, B);
    const auto world = assumeOne(store, store.root(), formula, false,
                                 "false A or B must use one world");
    checkStatus(store, world, formula, Query::False,
                "false A or B must refute the formula");
  }
  {
    common::memory::Arena arena;
    FactStore<int> store(arena);
    const auto a = store.globalFact();
    const auto b = store.globalFact();
    const auto &A = store.atom(store.value(a), Rel::Equal, store.constant(1));
    const auto &B = store.atom(store.value(b), Rel::Equal, store.constant(2));
    const auto &formula = store.or_(A, B);
    const auto worlds = assumeValue(store, store.root(), formula, true,
                                    "true A or B must split into two worlds");
    check(worlds.size() == 2, "true A or B must split into two worlds");
    for (const auto world : worlds)
      checkStatus(store, world, formula, Query::True,
                  "each true-or branch must prove the formula");
  }
  {
    common::memory::Arena arena;
    FactStore<int> store(arena);
    const auto a = store.globalFact();
    const auto b = store.globalFact();
    const auto &A = store.atom(store.value(a), Rel::Equal, store.constant(1));
    const auto &B = store.atom(store.value(b), Rel::Equal, store.constant(2));
    const auto &formula = store.and_(A, B);
    const auto worlds = assumeValue(store, store.root(), formula, false,
                                    "false A and B must split into two worlds");
    check(worlds.size() == 2, "false A and B must split into two worlds");
    for (const auto world : worlds)
      checkStatus(store, world, formula, Query::False,
                  "each false-and branch must refute the formula");
  }
}

void testKleeneAndJoinMatrix() {
  {
    common::memory::Arena arena;
    FactStore<int> store(arena);
    const auto a = store.globalFact();
    const auto b = store.globalFact();
    const auto c = store.globalFact();
    const auto &A = store.atom(store.value(a), Rel::Equal, store.constant(1));
    const auto &B = store.atom(store.value(b), Rel::Equal, store.constant(2));
    const auto &C = store.atom(store.value(c), Rel::Equal, store.constant(3));
    const auto world = assumeOne(store, store.root(), A, false,
                                 "a false atom must keep one world");
    const auto decided =
        assumeOne(store, world, B, true, "a true atom must keep one world");
    checkStatus(store, decided, store.and_(A, C), Query::False,
                "false and unknown must be false");
    checkStatus(store, decided, store.and_(B, C), Query::Unknown,
                "true and unknown must be unknown");
    checkStatus(store, decided, store.or_(B, C), Query::True,
                "true or unknown must be true");
    checkStatus(store, decided, store.or_(A, C), Query::Unknown,
                "false or unknown must be unknown");
  }
  {
    common::memory::Arena arena;
    FactStore<int> store(arena);
    const auto a = store.globalFact();
    const auto b = store.globalFact();
    const auto c = store.globalFact();
    const auto &A = store.atom(store.value(a), Rel::Equal, store.constant(1));
    const auto &B = store.atom(store.value(b), Rel::Equal, store.constant(2));
    const auto &C = store.atom(store.value(c), Rel::Equal, store.constant(3));
    const auto &orFormula = store.or_(A, B);
    const auto &andFormula = store.and_(A, B);
    const auto split =
        assumeValue(store, store.root(), orFormula, true,
                    "join matrix needs a binary split");
    check(split.size() == 2, "join matrix needs a binary split");
    const auto merged = mergeValue(store, {split.data(), split.size()});
    checkStatus(store, merged, orFormula, Query::True,
                "all true alternatives join to true");
    const auto falseSplit = assumeValue(store, store.root(), andFormula, false,
                                        "false and must split into two");;
    check(falseSplit.size() == 2,
          "false and must create two alternatives for join evaluation");
    const auto falseMerged =
        mergeValue(store, {falseSplit.data(), falseSplit.size()});
    checkStatus(store, falseMerged, andFormula, Query::False,
                "each branch evaluates the whole formula before joining");
    checkStatus(store, merged, C, Query::Unknown,
                "all unknown alternatives join to unknown");
    checkStatus(store, merged, A, Query::Maybe,
                "mixed alternatives join to maybe");
  }
}

void testWorldVisibilityAndPostMergeConstraints() {
  common::memory::Arena arena;
  FactStore<int> store(arena);
  const auto rootFact = store.globalFact();
  const auto a = store.globalFact();
  const auto b = store.globalFact();
  const auto &A = store.atom(store.value(a), Rel::Equal, store.constant(1));
  const auto &B = store.atom(store.value(b), Rel::Equal, store.constant(2));
  const auto split = assumeValue(store, store.root(), store.or_(A, B), true,
                                 "visibility test needs two children");
  check(split.size() == 2, "visibility test needs two children");
  check(visibleValue(store, split[0], rootFact) &&
            visibleValue(store, split[1], rootFact),
        "a root fact is visible in each child");
  const auto localResult = store.fact(split[0]);
  check(localResult.isOk(), "creating a local fact must succeed");
  const auto local = localResult.value();
  check(visibleValue(store, split[0], local) &&
            !visibleValue(store, split[1], local),
        "a child fact is visible only in its own child");
  const auto descendant = assumeOne(
      store, split[0], A, true, "a local child remains usable in descendants");
  check(visibleValue(store, descendant, local),
        "a child fact is visible in descendants");
  const auto merged = mergeValue(store, {split.data(), split.size()});
  check(visibleValue(store, merged, rootFact) &&
            !visibleValue(store, merged, local),
        "merge visibility requires every alternative to own the fact");
  const auto afterMergeResult = store.fact(merged);
  check(afterMergeResult.isOk(), "creating a post-merge fact must succeed");
  const auto afterMerge = afterMergeResult.value();
  check(visibleValue(store, merged, afterMerge),
        "facts created after merge are visible in that merge");
  const auto later = assumeValue(store, merged, store.or_(A, B), true,
                                 "a post-merge split is valid");
  check(later.size() == 2 && visibleValue(store, later[0], afterMerge),
        "a post-merge fact is visible in later children");
  const auto &invalid = store.equal(local, store.constant(9));
  const auto rejected = assumeValue(store, merged, invalid, true,
                                    "a branch-local post-merge constraint is invalid");
  check(rejected.empty() && store.hasConflicts(),
        "a branch-local post-merge constraint must conflict, not become maybe");
}

void testAffineChainsAndOverflow() {
  {
    common::memory::Arena arena;
    FactStore<int> store(arena);
    const auto x = store.globalFact();
    const auto y = store.globalFact();
    const auto z = store.globalFact();
    const auto &zIsFive = store.equal(z, store.constant(5));
    const auto &yIsZMinusTwo = store.equal(y, store.add(z, -2));
    const auto &xIsYPlusFour = store.equal(x, store.add(y, 4));
    auto world =
        assumeOne(store, store.root(), zIsFive, true, "z == 5 is valid");
    world = assumeOne(store, world, yIsZMinusTwo, true, "y == z - 2 is valid");
    world = assumeOne(store, world, xIsYPlusFour, true, "x == y + 4 is valid");
    check(domainValue(store, world, x).exact == 7 &&
              domainValue(store, world, y).exact == 3,
          "affine chains must propagate exact values");
  }
  {
    common::memory::Arena arena;
    FactStore<int> store(arena);
    const auto x = store.globalFact();
    const auto y = store.globalFact();
    const auto &xIsYPlusFour = store.equal(x, store.add(y, 4));
    const auto &yIsFive = store.equal(y, store.constant(5));
    auto world = assumeOne(store, store.root(), xIsYPlusFour, true,
                           "unresolved affine equality is valid");
    world =
        assumeOne(store, world, yIsFive, true, "late affine constant is valid");
    check(domainValue(store, world, x).exact == 9,
          "affine propagation must not depend on assumption order");
  }
  {
    common::memory::Arena arena;
    FactStore<int> store(arena);
    const auto a = store.globalFact();
    const auto b = store.globalFact();
    const auto &same = store.equal(a, store.value(b));
    const auto &bIsFive = store.equal(b, store.constant(5));
    auto world = assumeOne(store, store.root(), same, true, "a == b is valid");
    world = assumeOne(store, world, bIsFive, true, "b == 5 is valid");
    check(domainValue(store, world, a).exact == 5,
          "simple equality must share a known constant");
  }
  for (const auto [value, offset] :
       std::array<std::array<int, 2>, 2>{{{INT_MAX, 1}, {INT_MIN, -1}}}) {
    common::memory::Arena arena;
    FactStore<int> store(arena);
    const auto x = store.globalFact();
    const auto y = store.globalFact();
    const auto &xIsYOffset = store.equal(x, store.add(y, offset));
    const auto &yIsValue = store.equal(y, store.constant(value));
    const auto world = assumeOne(store, store.root(), xIsYOffset, true,
                                 "an unresolved affine equality is valid");
    const auto overflow = assumeValue(store, world, yIsValue, true,
                                      "overflowing add must discard its path");
    check(overflow.empty() && store.hasConflicts(),
          "overflowing add must discard the path and record a conflict");
  }
}

void testBranchLocalConflictIsolation() {
  common::memory::Arena arena;
  FactStore<int> store(arena);
  const auto a = store.globalFact();
  const auto b = store.globalFact();
  const auto &A = store.atom(store.value(a), Rel::Equal, store.constant(1));
  const auto &B = store.atom(store.value(b), Rel::Equal, store.constant(2));
  const auto split = assumeValue(store, store.root(), store.or_(A, B), true,
                                 "conflict isolation needs two worlds");
  check(split.size() == 2, "conflict isolation needs two worlds");
  const auto killed = assumeValue(store, split[0], A, false,
                                  "contradicting one branch must kill that branch");
  check(killed.empty() && store.hasConflicts(),
        "contradicting one branch must kill that branch");
  checkStatus(store, split[1], B, Query::True,
              "the sibling branch must remain queryable");
  const auto merged = mergeValue(store, {split.data(), split.size()});
  checkStatus(store, merged, B, Query::True,
              "a merge must use only its surviving branch");
}

void testFactContractErrors() {
  common::memory::Arena arena;
  FactStore<int> store(arena);
  const auto invalidWorldWorldId = WorldId{999};
  const auto rootFact = store.globalFact();
  const auto &atom = store.equal(rootFact, store.constant(0));

  const auto invalidAssume = store.assume(invalidWorldWorldId, atom, true);
  check(invalidAssume.isError(), "assume rejects an invalid WorldId");

  const auto invalidStatus = store.status(invalidWorldWorldId, atom);
  check(invalidStatus.isError(), "status rejects an invalid WorldId");

  const std::span<const WorldId> none;
  const auto emptyMerge = store.merge(none);
  check(emptyMerge.isError(), "merge rejects an empty alternative list");

  const std::array<WorldId, 2> repeated = {store.root(), store.root()};
  const auto duplicate = store.merge({repeated.data(), repeated.size()});
  check(duplicate.isError(), "merge rejects repeated alternatives");

  const std::array<WorldId, 1> invalidAlternatives = {invalidWorldWorldId};
  const auto invalidMerge =
      store.merge({invalidAlternatives.data(), invalidAlternatives.size()});
  check(invalidMerge.isError(), "merge rejects an invalid alternative");

  const auto invalidDomain = store.domain(invalidWorldWorldId, rootFact);
  check(invalidDomain.isError(), "domain rejects an invalid WorldId");

  const auto invalidVisible = store.visible(invalidWorldWorldId, rootFact);
  check(invalidVisible.isError(), "visible rejects an invalid WorldId");
}

void testEvaluateAfterAssumptions() {
  common::memory::Arena arena;
  FactStore<int> store(arena);
  const auto x = store.globalFact();
  const auto y = store.globalFact();
  const auto &xIsOne = store.equal(x, store.constant(1));
  const auto &yIsTwo = store.equal(y, store.constant(2));
  const auto world = assumeOne(store, store.root(), xIsOne, true,
                               "one atom keeps one world");
  const auto evaluatedAnd = store.evaluate(world, store.and_(xIsOne, yIsTwo));
  check(evaluatedAnd.isOk(), "evaluate accepts a formula after assumptions");
  check(evaluatedAnd.value() == Query::Unknown ||
            evaluatedAnd.value() == Query::Maybe,
        "evaluate does not assume facts as hard constraints");
  const auto evaluatedAtom = store.evaluate(world, yIsTwo);
  check(evaluatedAtom.isOk() && evaluatedAtom.value() == Query::Unknown,
        "evaluate does not create a world for the queried atom");
}

struct DeterministicRng {
  std::uint32_t state = 0xFACADE42U;

  [[nodiscard]] auto next() noexcept -> std::uint32_t {
    state = state * 1664525U + 1013904223U;
    return state;
  }
};

auto relationName(Rel relation) -> const char * {
  switch (relation) {
  case Rel::Equal:
    return "Equal";
  case Rel::NotEqual:
    return "NotEqual";
  case Rel::Greater:
    return "Greater";
  case Rel::GreaterEq:
    return "GreaterEq";
  case Rel::Lesser:
    return "Lesser";
  case Rel::LesserEq:
    return "LesserEq";
  }
  return "invalid";
}

void testRandomizedWorldContract() {
  constexpr std::array constants = {-8, -3, 0, 1, 5, 9};
  constexpr std::array relations = {Rel::Equal,   Rel::NotEqual,
                                    Rel::Greater, Rel::GreaterEq,
                                    Rel::Lesser,  Rel::LesserEq};
  DeterministicRng rng;
  for (unsigned scenario = 0; scenario < 256; ++scenario) {
    common::memory::Arena arena;
    FactStore<int> store(arena);
    const std::array<FactRef, 4> facts = {
        store.globalFact(), store.globalFact(), store.globalFact(),
        store.globalFact()};

    const int offset = (scenario & 1U) == 0U ? 1 : 3;
    const int value = (scenario & 2U) == 0U ? 7 : INT_MAX;
    const auto &affine = store.equal(facts[0], store.add(facts[1], offset));
    const auto affineWorld =
        assumeOne(store, store.root(), affine, true,
                  "random affine relation must be valid first");
    const auto &known = store.equal(facts[1], store.constant(value));
    const auto affineResult = assumeValue(store, affineWorld, known, true,
                                          "random affine assumption must be valid");
    if (value <= INT_MAX - offset) {
      check(affineResult.size() == 1 &&
                domainValue(store, affineResult[0], facts[0]).exact ==
                    value + offset,
            "random affine relation must infer its exact value");
    } else {
      check(affineResult.empty() && store.hasConflicts(),
            "random affine overflow must discard its path");
    }

    WorldId current = store.root();
    for (unsigned step = 0; step < 16; ++step) {
      const auto lhs = facts[rng.next() % facts.size()];
      const auto rhs = facts[rng.next() % facts.size()];
      const auto relation = relations[rng.next() % relations.size()];
      const auto constant = constants[rng.next() % constants.size()];
      const auto &first =
          store.atom(store.value(lhs), relation, store.constant(constant));
      const auto &second =
          store.atom(store.value(rhs), relation,
                     store.constant(constants[rng.next() % constants.size()]));
      const auto &formula = step % 4U == 0U   ? store.or_(first, second)
                            : step % 4U == 1U ? store.and_(first, second)
                                              : first;
      const bool expected = (rng.next() & 1U) != 0U;
      const auto worlds = assumeValue(store, current, formula, expected,
                                      "random assumption must return a result");
      const std::string context =
          "scenario=" + std::to_string(scenario) +
          " step=" + std::to_string(step) +
          " seed=0xFACADE42 rel=" + relationName(relation) +
          " constant=" + std::to_string(constant);
      if (worlds.empty()) {
        current = store.root();
        continue;
      }
      check(worlds.size() <= 2,
            (context + " produced more than two worlds").c_str());
      for (const auto world : worlds) {
        checkStatus(store, world, formula,
                    expected ? Query::True : Query::False,
                    (context + " violated the assumed formula").c_str());
        for (const auto fact : facts)
          check(visibleValue(store, world, fact),
                (context + " hid a global fact").c_str());
        const auto status = store.status(world, formula);
        check(status.isOk() &&
                  (status.value() == Query::True ||
                   status.value() == Query::False ||
                   status.value() == Query::Unknown ||
                   status.value() == Query::Maybe),
              (context + " returned an invalid query state").c_str());
      }
      if (worlds.size() == 2) {
        const auto merged = mergeValue(store, {worlds.data(), worlds.size()});
        const std::array<WorldId, 2> reversed = {worlds[1], worlds[0]};
        const auto reverseMerged =
            mergeValue(store, {reversed.data(), reversed.size()});
        const auto firstStatus = store.status(merged, formula);
        const auto secondStatus = store.status(reverseMerged, formula);
        check(firstStatus.isOk() && secondStatus.isOk() &&
                  firstStatus.value() == secondStatus.value(),
              (context + " merge order changed the formula status").c_str());
        current = merged;
      } else {
        current = worlds[0];
      }
    }
  }
}
} // namespace

auto main(int argc, char **argv) -> int {
  bool intensive = false;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--intensive")
      intensive = true;
    else if (argument != "--basic") {
      std::cerr << "facts-v2-test: unknown argument: " << argument << '\n';
      return 2;
    }
  }

  testValueDomains();
  testValueDomainIntersection();
  testValueDomainSubset();
  testAtomicRelationsAndNegation();
  testBooleanLoweringMatrix();
  testKleeneAndJoinMatrix();
  testWorldVisibilityAndPostMergeConstraints();
  testAffineChainsAndOverflow();
  testBranchLocalConflictIsolation();
  testFactContractErrors();
  testEvaluateAfterAssumptions();
  if (intensive)
    testRandomizedWorldContract();
  std::cout << "facts-v2-test: all tests passed";
  if (intensive)
    std::cout << " (intensive)";
  std::cout << '\n';
  return 0;
}
