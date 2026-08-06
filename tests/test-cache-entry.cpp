#include "cache/cache-types.hpp"
#include "cache/cache.hpp"
#include "cache/manifest.hpp"
#include "session/frontend-context.hpp"
#include "test-common.hpp"

#include <filesystem>
#include <string>
#include <string_view>

using namespace zith;
using namespace zith::cache;

namespace {

namespace fs = std::filesystem;

// ── helpers ───────────────────────────────────────────────────────

session::ContentFingerprint makeFingerprint(uint64_t primary) {
    session::ContentFingerprint fp;
    fp.primary = primary;
    return fp;
}

std::string makeCacheKeyIdentity() {
    return "zithc-test-cache-entry";
}

// ── Transitive invalidation: B depends on A, C depends on B ───────
//   invalidating A should also invalidate B and C transitively.

static void test_transitive_invalidation() {
    auto root = fs::temp_directory_path() / "zith-test-transitive";
    fs::remove_all(root);
    fs::create_directories(root);

    session::CacheKey key;
    key.compilerVersion = "test";
    Store store(root.string(), key);

    // Module A (no dependencies)
    ManifestEntry entryA;
    entryA.canonical_path = "/test/a.zith";
    entryA.artifact_path  = (root / "a.zirl").string();
    entryA.public_abi_hi  = 1;
    entryA.public_abi_lo  = 2;

    // Module B (depends on A)
    ManifestEntry entryB;
    entryB.canonical_path = "/test/b.zith";
    entryB.artifact_path  = (root / "b.zirl").string();
    entryB.public_abi_hi  = 3;
    entryB.public_abi_lo  = 4;
    entryB.dependencies   = {"/test/a.zith"};

    // Module C (depends on B)
    ManifestEntry entryC;
    entryC.canonical_path = "/test/c.zith";
    entryC.artifact_path  = (root / "c.zirl").string();
    entryC.public_abi_hi  = 5;
    entryC.public_abi_lo  = 6;
    entryC.dependencies   = {"/test/b.zith"};

    // Build manifest
    Manifest manifest(root.string());
    manifest.upsert(entryA);
    manifest.upsert(entryB);
    manifest.upsert(entryC);

    // dependentsOf should return transitive dependents
    auto deps_of_a = manifest.dependentsOf("/test/a.zith");
    CHECK(deps_of_a.size() >= 2, "A has at least B and C as transitive dependents");

    bool found_b = false;
    bool found_c = false;
    for (auto &d : deps_of_a) {
        if (d == "/test/b.zith")
            found_b = true;
        if (d == "/test/c.zith")
            found_c = true;
    }
    CHECK(found_b, "B is a transitive dependent of A");
    CHECK(found_c, "C is a transitive dependent of A");

    // dependentsOf for C should have nothing (leaf)
    auto deps_of_c = manifest.dependentsOf("/test/c.zith");
    CHECK(deps_of_c.empty(), "C has no dependents (leaf module)");

    fs::remove_all(root);
}

// ── Cycle idempotence: if A depends on B and B depends on A,       │
//   invalidation must terminate (not recurse infinitely).           │

static void test_cycle_idempotence() {
    auto root = fs::temp_directory_path() / "zith-test-cycles";
    fs::remove_all(root);
    fs::create_directories(root);

    Manifest manifest(root.string());

    ManifestEntry entryA;
    entryA.canonical_path = "/test/cycle_a.zith";
    entryA.artifact_path  = (root / "cycle_a.zirl").string();
    entryA.dependencies   = {"/test/cycle_b.zith"};

    ManifestEntry entryB;
    entryB.canonical_path = "/test/cycle_b.zith";
    entryB.artifact_path  = (root / "cycle_b.zirl").string();
    entryB.dependencies   = {"/test/cycle_a.zith"};

    manifest.upsert(entryA);
    manifest.upsert(entryB);

    auto deps_of_a = manifest.dependentsOf("/test/cycle_a.zith");
    CHECK(deps_of_a.size() <= 2, "cycle dependents terminate (no unbounded growth)");

    auto deps_of_b = manifest.dependentsOf("/test/cycle_b.zith");
    CHECK(deps_of_b.size() <= 2, "cycle dependents terminate for B too");

    fs::remove_all(root);
}

// ── Manifest save/load round-trip ─────────────────────────────────

static void test_manifest_round_trip() {
    auto root = fs::temp_directory_path() / "zith-test-manifest-rt";
    fs::remove_all(root);
    fs::create_directories(root);

    {
        Manifest manifest(root.string());
        ManifestEntry e;
        e.canonical_path = "/test/mod.zith";
        e.artifact_path  = (root / "mod.zirl").string();
        e.public_abi_hi  = 0xAABB;
        e.public_abi_lo  = 0xCCDD;
        e.source_fp_hi   = 0x1111;
        e.source_fp_lo   = 0x2222;
        e.dependencies   = {"/test/dep.zith"};
        manifest.upsert(e);
        manifest.save();
    }

    {
        Manifest manifest(root.string());
        manifest.load();
        auto found = manifest.find("/test/mod.zith");
        CHECK(found.has_value(), "Manifest round-trip: entry found after load");
        CHECK(found->canonical_path == "/test/mod.zith", "Manifest round-trip: path preserved");
        CHECK(found->public_abi_hi == 0xAABB, "Manifest round-trip: abi_hi preserved");
        CHECK(found->public_abi_lo == 0xCCDD, "Manifest round-trip: abi_lo preserved");
    }

    fs::remove_all(root);
}

// ── Manifest remove and dependents ────────────────────────────────

static void test_manifest_remove_clears_dependents() {
    auto root = fs::temp_directory_path() / "zith-test-remove";
    fs::remove_all(root);
    fs::create_directories(root);

    Manifest manifest(root.string());

    ManifestEntry dep;
    dep.canonical_path = "/test/dep.zith";
    dep.artifact_path  = (root / "dep.zirl").string();
    manifest.upsert(dep);

    ManifestEntry user;
    user.canonical_path = "/test/user.zith";
    user.artifact_path  = (root / "user.zirl").string();
    user.dependencies   = {"/test/dep.zith"};
    manifest.upsert(user);

    auto deps = manifest.dependentsOf("/test/dep.zith");
    CHECK(deps.size() == 1, "one dependent before remove");

    manifest.remove("/test/user.zith");

    deps = manifest.dependentsOf("/test/dep.zith");
    CHECK(deps.empty(), "no dependents after removing user");

    // Removing a non-existent entry is a no-op (no crash)
    manifest.remove("/test/does-not-exist.zith");

    fs::remove_all(root);
}

// ── Store metrics exposure ────────────────────────────────────────

static void test_store_metrics_exposed() {
    auto root = fs::temp_directory_path() / "zith-test-metrics";
    fs::remove_all(root);
    fs::create_directories(root);

    session::CacheKey key;
    key.compilerVersion = "test";
    Store store(root.string(), key);

    auto m = store.metrics();
    CHECK(m.hits == 0, "metrics start at zero hits");
    CHECK(m.misses == 0, "metrics start at zero misses");
    CHECK(m.invalid == 0, "metrics start at zero invalid");
    CHECK(m.writes == 0, "metrics start at zero writes");
    CHECK(m.evictions == 0, "metrics start at zero evictions");

    auto fp = makeFingerprint(0xAAAA);
    store.load("/test/nonexistent.zith", fp);
    m = store.metrics();
    CHECK(m.misses > 0, "miss increments after failed load");

    fs::remove_all(root);
}

// ── All test aggregation ──────────────────────────────────────────

static void test_cache_entry() {
    test_transitive_invalidation();
    test_cycle_idempotence();
    test_manifest_round_trip();
    test_manifest_remove_clears_dependents();
    test_store_metrics_exposed();
}

} // namespace

TEST_MAIN(cache_entry)
