#include "cache/cache.hpp"
#include "cache/cache-codec.hpp"

#include "common/memory/arena.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>

using toolkit::cache::Artifact;
using toolkit::cache::CacheKey;
using toolkit::cache::ContentFingerprint;
using toolkit::cache::Store;

Artifact makeArtifact() {
    Artifact artifact;
    artifact.canonical_path = "/demo/main.zith";
    artifact.module_name = "main";
    artifact.cache_key_hash = static_cast<std::uint32_t>(toolkit::cache::fnv1a64("demo"));
    artifact.source_fp_hi = 0xD00D0000;
    artifact.source_fp_lo = 0x00C0FFEE;
    artifact.strings = {"main", "i32"};
    artifact.paths = {artifact.canonical_path};
    artifact.decls.push_back({.name = "main"});
    return artifact;
}

bool expect(bool condition, std::string_view message) {
    if (!condition)
        std::cerr << "cache-demo: " << message << '\n';
    return condition;
}

int main() {
    const auto root = std::filesystem::temp_directory_path() / "zith-cache-demo";
    std::filesystem::remove_all(root);

    CacheKey key;
    key.compilerVersion = "demo";
    Store store(root.string(), key);

    Artifact artifact = makeArtifact();
    ContentFingerprint fingerprint;
    fingerprint.primary = (static_cast<std::uint64_t>(artifact.source_fp_hi) << 32u) |
                          artifact.source_fp_lo;
    store.store(artifact);

    const auto hit = store.load(artifact.canonical_path, fingerprint);
    if (!expect(hit.has_value(), "stored artifact loads"))
        return EXIT_FAILURE;

    ContentFingerprint missFingerprint;
    missFingerprint.primary = 0x5EED;
    const auto miss = store.load(artifact.canonical_path, missFingerprint);
    (void)miss;

    const auto metrics = store.metrics();
    std::cout << "cache-demo: hit=" << metrics.hits
              << " miss=" << metrics.misses
              << " invalid=" << metrics.invalid
              << " writes=" << metrics.writes
              << "\n";

    std::filesystem::remove_all(root);
    return expect(metrics.hits == 1 && metrics.writes == 1,
                  "hit and write metrics tracked")
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
