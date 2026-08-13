#include "facts/fact-v2.hpp"

#include <climits>
#include <cstdlib>
#include <iostream>

namespace {
using toolkit::facts::FactStore;
using toolkit::facts::Query;

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

  const auto split = store.assume(store.root(), formula, true);
  check(split.size() == 2, "or-true must split into two worlds");

  const auto joined = store.merge({split.data(), split.size()});
  check(store.status(joined, formula) == Query::True,
        "join of all-true branches is true");

  const auto local = store.fact(split[0]);
  check(store.visible(split[0], local) && !store.visible(split[1], local),
        "a branch-local fact is visible only in its own branch");

  const auto afterMerge = store.fact(joined);
  check(store.visible(joined, afterMerge),
        "a fact created after a merge is visible in the merged world");
}

void demonstrateDomainsAndAffine(FactStore<int> &store) {
  const auto balance = store.globalFact();
  const auto limit = store.globalFact();
  const auto &offset = store.equal(balance, store.add(limit, 3));
  const auto &limitIsTen = store.equal(limit, store.constant(10));

  const auto offsetWorlds = store.assume(store.root(), offset, true);
  check(offsetWorlds.size() == 1, "affine equality keeps one world");
  auto world = offsetWorlds[0];
  const auto limitWorlds = store.assume(world, limitIsTen, true);
  check(limitWorlds.size() == 1, "a constant equality keeps one world");
  world = limitWorlds[0];
  check(store.domain(world, balance).exact == 13,
        "affine propagation computes balance as limit + 3");
  check(store.status(world, store.equal(balance, store.constant(13))) ==
            Query::True,
        "the inferred value answers equality queries");

  const auto &beltAndSuspenders =
      store.and_(store.equal(balance, store.constant(0)),
                 store.equal(limit, store.constant(0)));
  const auto bad = store.assume(world, beltAndSuspenders, true);
  check(bad.empty() && store.hasConflicts(),
        "an infeasible branch is discarded and recorded as a conflict");
}

void demonstrateOverflow(FactStore<int> &store) {
  const auto a = store.globalFact();
  const auto b = store.globalFact();
  const auto &overflow = store.equal(a, store.add(b, 1));
  const auto &bIsMax = store.equal(b, store.constant(INT_MAX));
  const auto worlds = store.assume(store.root(), overflow, true);
  check(worlds.size() == 1, "an unresolved affine equality keeps one world");
  const auto result = store.assume(worlds[0], bIsMax, true);
  check(result.empty() && store.hasConflicts(),
        "overflowing an add discards the path instead of wrapping");
}

void demonstrateMaybe(FactStore<int> &store) {
  const auto a = store.globalFact();
  const auto b = store.globalFact();
  const auto &A = store.equal(a, store.constant(1));
  const auto &B = store.equal(b, store.constant(2));
  const auto &formula = store.or_(A, B);
  const auto split = store.assume(store.root(), formula, true);
  const auto joined = store.merge({split.data(), split.size()});
  check(store.status(joined, A) == Query::Maybe,
        "mixed branch answers join to maybe");
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
