#include "facts/fact-v2.hpp"

#include <climits>
#include <cstdlib>
#include <iostream>
#include <span>

namespace {
using toolkit::facts::FactStore;
using toolkit::facts::Query;
using toolkit::facts::WorldId;

void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "facts-v2-demo: " << message << '\n';
    std::exit(1);
  }
}

void demonstrateWorlds(FactStore<int> &store) {
  const auto a = store.globalFact();
  const auto b = store.globalFact();
  const auto &A = store.equal(a, store.constant(1));
  const auto &B = store.equal(b, store.constant(2));
  const auto &formula = store.or_(A, B);

  const auto splitResult = store.assume(store.root(), formula, true);
  check(splitResult.isOk(), "or-true assumption must succeed");
  const auto &split = splitResult.value();
  check(split.size() == 2, "or-true must split into two worlds");

  const auto joinedResult = store.merge({split.data(), split.size()});
  check(joinedResult.isOk(), "a valid merge must succeed");
  const auto joined = joinedResult.value();
  const auto status = store.status(joined, formula);
  check(status.isOk() && status.value() == Query::True,
        "join of all-true branches is true");

  const auto localResult = store.fact(split[0]);
  check(localResult.isOk(), "a branch-local fact must be created");
  const auto local = localResult.value();
  const auto visibleLeft = store.visible(split[0], local);
  const auto visibleRight = store.visible(split[1], local);
  check(visibleLeft.isOk() && visibleRight.isOk() &&
            visibleLeft.value() && !visibleRight.value(),
        "a branch-local fact is visible only in its own branch");

  const auto afterMergeResult = store.fact(joined);
  check(afterMergeResult.isOk(), "a post-merge fact must be created");
  const auto afterMerge = afterMergeResult.value();
  const auto afterMergeVisible = store.visible(joined, afterMerge);
  check(afterMergeVisible.isOk() && afterMergeVisible.value(),
        "a fact created after a merge is visible in the merged world");
}

void demonstrateDomainsAndAffine(FactStore<int> &store) {
  const auto balance = store.globalFact();
  const auto limit = store.globalFact();
  const auto &offset = store.equal(balance, store.add(limit, 3));
  const auto &limitIsTen = store.equal(limit, store.constant(10));

  const auto offsetResult = store.assume(store.root(), offset, true);
  check(offsetResult.isOk() && offsetResult.value().size() == 1,
        "affine equality keeps one world");
  auto world = offsetResult.value()[0];
  const auto limitResult = store.assume(world, limitIsTen, true);
  check(limitResult.isOk() && limitResult.value().size() == 1,
        "a constant equality keeps one world");
  world = limitResult.value()[0];
  const auto domain = store.domain(world, balance);
  check(domain.isOk() && domain.value().exact == 13,
        "affine propagation computes balance as limit + 3");
  const auto status =
      store.status(world, store.equal(balance, store.constant(13)));
  check(status.isOk() && status.value() == Query::True,
        "the inferred value answers equality queries");

  const auto evaluated =
      store.evaluate(world, store.equal(balance, store.constant(13)));
  check(evaluated.isOk() && evaluated.value() == Query::True,
        "evaluate can answer from the collected facts");

  const auto &beltAndSuspenders =
      store.and_(store.equal(balance, store.constant(0)),
                 store.equal(limit, store.constant(0)));
  const auto badResult = store.assume(world, beltAndSuspenders, true);
  check(badResult.isOk() && badResult.value().empty() && store.hasConflicts(),
        "an infeasible branch is discarded and recorded as a conflict");
}

void demonstrateOverflow(FactStore<int> &store) {
  const auto a = store.globalFact();
  const auto b = store.globalFact();
  const auto &overflow = store.equal(a, store.add(b, 1));
  const auto &bIsMax = store.equal(b, store.constant(INT_MAX));
  const auto worldsResult = store.assume(store.root(), overflow, true);
  check(worldsResult.isOk() && worldsResult.value().size() == 1,
        "an unresolved affine equality keeps one world");
  const auto world = worldsResult.value()[0];
  const auto result = store.assume(world, bIsMax, true);
  check(result.isOk() && result.value().empty() && store.hasConflicts(),
        "overflowing an add discards the path instead of wrapping");
}

void demonstrateMaybe(FactStore<int> &store) {
  const auto a = store.globalFact();
  const auto b = store.globalFact();
  const auto &A = store.equal(a, store.constant(1));
  const auto &B = store.equal(b, store.constant(2));
  const auto &formula = store.or_(A, B);
  const auto splitResult = store.assume(store.root(), formula, true);
  check(splitResult.isOk() && splitResult.value().size() == 2,
        "maybe example needs two branches");
  const auto &split = splitResult.value();
  const auto joinedResult = store.merge({split.data(), split.size()});
  check(joinedResult.isOk(), "a valid split merge must succeed");
  const auto joined = joinedResult.value();
  const auto status = store.status(joined, A);
  check(status.isOk() && status.value() == Query::Maybe,
        "mixed branch answers join to maybe");

  const std::span<const WorldId> none;
  check(store.merge(none).isError(), "an empty merge is rejected");
}
} // namespace

auto main() -> int {
  {
    common::memory::Arena arena;
    FactStore<int> store(arena);
    demonstrateWorlds(store);
  }
  {
    common::memory::Arena arena;
    FactStore<int> store(arena);
    demonstrateDomainsAndAffine(store);
  }
  {
    common::memory::Arena arena;
    FactStore<int> store(arena);
    demonstrateOverflow(store);
  }
  {
    common::memory::Arena arena;
    FactStore<int> store(arena);
    demonstrateMaybe(store);
  }
  std::cout << "facts-v2-demo: all examples passed\n";
  return 0;
}
