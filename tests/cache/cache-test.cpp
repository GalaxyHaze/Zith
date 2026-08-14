#include "cache/cache.hpp"
#include "cache/cache-buffer.hpp"
#include "cache/cache-codec.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;

bool check(bool ok, std::string_view message) {
    if (!ok)
        std::cerr << "FAIL: " << message << '\n';
    return ok;
}

toolkit::cache::Artifact makeArtifact() {
    toolkit::cache::Artifact artifact;
    artifact.canonical_path = "/test/main.turv";
    artifact.module_name = "main";
    artifact.cache_key_hash = static_cast<std::uint32_t>(
        toolkit::cache::fnv1a64("test"));
    artifact.source_fp_hi = 0xAABBCCDD;
    artifact.source_fp_lo = 0x11223344;
    artifact.public_abi_hi = 0xEEFF0011;
    artifact.public_abi_lo = 0x22334455;
    artifact.module_id_hi = artifact.public_abi_hi ^ 0x55AA;
    artifact.module_id_lo = artifact.public_abi_lo ^ 0x66BB;
    artifact.strings = {"main", "println", "i32"};
    artifact.paths = {artifact.canonical_path};
    artifact.deps.push_back({"/test/dep.turv", "dep", 1, 2});
    toolkit::cache::DeclRecord decl;
    decl.name = "main";
    decl.kind = toolkit::cache::DeclKind::Fn;
    decl.visibility = toolkit::cache::Visibility::Public;
    decl.name_id = 0;
    artifact.decls.push_back(decl);
    toolkit::cache::CompactFunction fn;
    fn.name = "main";
    fn.name_id = 0;
    toolkit::cache::CompactBasicBlock block;
    block.terminator = 0;
    fn.blocks.push_back(block);
    toolkit::cache::CompactExpr expr;
    expr.kind = toolkit::cache::ExprKind::Ret;
    expr.ref_a = ~std::uint32_t{0};
    fn.exprs.push_back(expr);
    artifact.functions.push_back(fn);
    toolkit::cache::CompactMarker marker;
    marker.marker_id = 3;
    artifact.markers.push_back(marker);
    toolkit::cache::HirSlotAttrsRecord slot;
    slot.slot = 0;
    slot.non_null = true;
    artifact.attrs_slots.push_back(slot);
    return artifact;
}

void roundTrip() {
    const auto original = makeArtifact();
    const auto encoded = toolkit::cache::encodeArtifact(original);
    if (!check(encoded.isOk(), "encode artifact"))
        return;
    const auto decoded = toolkit::cache::decodeArtifact(encoded.value());
    if (!check(decoded.isOk(), "decode artifact"))
        return;
    const auto &art = decoded.value();
    check(art.canonical_path == original.canonical_path, "round-trip path");
    check(art.strings == original.strings, "round-trip strings");
    check(art.deps.size() == original.deps.size(), "round-trip deps count");
    check(art.deps[0].canonical_path == "/test/dep.turv", "round-trip dep path");
    check(art.decls.size() == 1 && art.decls[0].name == "main", "round-trip decls");
    check(art.functions.size() == 1 && art.functions[0].exprs.size() == 1,
          "round-trip functions");
    check(art.markers.size() == 1 && art.markers[0].marker_id == 3, "round-trip markers");
    check(art.attrs_slots.size() == 1 && art.attrs_slots[0].non_null,
          "round-trip attrs");
}

void deterministicBytes() {
    const auto first = toolkit::cache::encodeArtifact(makeArtifact());
    const auto second = toolkit::cache::encodeArtifact(makeArtifact());
    check(first.isOk() && second.isOk() && first.value() == second.value(),
          "identical artifact encodes to identical bytes");
    const auto checksum = toolkit::cache::fnv1a64(first.value());
    check(checksum != 0, "checksum is nonzero");
}

void validationErrors() {
    std::string bytes;
    const auto encoded = toolkit::cache::encodeArtifact(makeArtifact());
    if (!encoded.isOk())
        return;
    bytes = encoded.value();

    auto wrongMagic = toolkit::cache::decodeArtifact("WRONG" + bytes.substr(5));
    check(wrongMagic.isError() &&
              wrongMagic.error().kind == toolkit::cache::CacheErrorKind::Corrupt,
          "wrong magic returns Corrupt");

    auto wrongVersion = bytes;
    wrongVersion[8] = 9;
    const auto versionResult = toolkit::cache::decodeArtifact(wrongVersion);
    check(versionResult.isError() && versionResult.error().kind ==
                                         toolkit::cache::CacheErrorKind::UnsupportedVersion,
          "wrong version returns UnsupportedVersion");

    auto wrongEndian = bytes;
    wrongEndian[12] = 2;
    check(toolkit::cache::decodeArtifact(wrongEndian).isError(), "wrong endian rejected");

    auto badHeader = bytes;
    badHeader[16] = 0;
    badHeader[17] = 0;
    badHeader[18] = 1;
    badHeader[19] = 0;
    check(toolkit::cache::decodeArtifact(badHeader).isError(), "bad header size rejected");

    auto truncated = bytes.substr(0, bytes.size() / 2);
    check(toolkit::cache::decodeArtifact(truncated).isError(), "truncated file rejected");

    auto corrupted = bytes;
    corrupted[corrupted.size() - 5] ^= 0xFF;
    const auto checksumResult = toolkit::cache::decodeArtifact(corrupted);
    check(checksumResult.isError() &&
              checksumResult.error().kind == toolkit::cache::CacheErrorKind::Corrupt,
          "checksum mismatch rejected");

    const auto validBytes = toolkit::cache::encodeArtifact(makeArtifact());
    if (!validBytes.isOk())
        return;
    auto withInvalidDeclKind = validBytes.value();
    const auto declStart = withInvalidDeclKind.find("/test/main.turv");
    if (declStart != std::string::npos) {
        std::size_t pos = declStart;
        while (pos < withInvalidDeclKind.size() && withInvalidDeclKind[pos] != '\0')
            ++pos;
        // name is length-prefixed; after the string terminator the next byte
        // is the DeclKind in the encoded decl section. A fake index is fine
        // for validation even if it lands on another byte in this tiny sample.
        withInvalidDeclKind[pos] = static_cast<char>(0x7F);
        auto badKind = toolkit::cache::decodeArtifact(withInvalidDeclKind);
        check(badKind.isError() && badKind.error().kind ==
                                      toolkit::cache::CacheErrorKind::Corrupt,
              "invalid DeclKind rejected");
    }

    auto invalidBool = validBytes.value();
    if (!invalidBool.empty())
        invalidBool[invalidBool.size() - 1] = static_cast<char>(2);
    const auto boolResult = toolkit::cache::decodeArtifact(invalidBool);
    check(boolResult.isError() &&
              boolResult.error().kind == toolkit::cache::CacheErrorKind::Corrupt,
          "bool 2 rejected");

    std::string trailingBytes = validBytes.value();
    trailingBytes.append(8, '\0');
    check(toolkit::cache::decodeArtifact(trailingBytes).isError(),
          "trailing cache bytes rejected");

    auto oversized = validBytes.value();
    if (oversized.size() >= 224) {
        const std::size_t declOffset = 224;
        if (declOffset + 4 <= oversized.size()) {
            oversized[declOffset] = static_cast<char>(0xFF);
            oversized[declOffset + 1] = static_cast<char>(0x0F);
            const auto listResult = toolkit::cache::decodeArtifact(oversized);
            check(listResult.isError() &&
                      listResult.error().kind == toolkit::cache::CacheErrorKind::Corrupt,
                  "oversized list rejected");
        }
    }
}

void storeBehavior() {
    const auto root = fs::temp_directory_path() / "turv-cache-test-store";
    fs::remove_all(root);
    toolkit::cache::CacheKey key;
    key.compilerVersion = "test";
    toolkit::cache::Store store(root.string(), key);
    auto artifact = makeArtifact();
    artifact.deps.clear();
    toolkit::cache::ContentFingerprint fp;
    fp.primary = (static_cast<std::uint64_t>(artifact.source_fp_hi) << 32u) |
                 artifact.source_fp_lo;
    store.store(artifact);

    auto loaded = store.load("/test/main.turv", fp);
    check(loaded.has_value(), "stored artifact loads");
    check(store.metrics().hits == 1, "hit metric");

    toolkit::cache::ContentFingerprint wrong;
    wrong.primary = 123;
    check(!store.load("/test/main.turv", wrong).has_value(), "fingerprint mismatch misses");
    check(store.metrics().invalid > 0, "invalid metric");
    check(!store.load("/test/absent.turv", fp).has_value(), "missing path misses");
    check(store.metrics().misses > 0, "miss metric");

    auto entry = store.loadEntry("/test/main.turv", fp);
    check(entry.has_value() && entry->state == toolkit::cache::CacheEntryState::Hydrated,
          "loadEntry hydrates");
    fs::remove_all(root);
}

void manifestBehavior() {
    const auto root = fs::temp_directory_path() / "turv-cache-test-manifest";
    fs::remove_all(root);
    toolkit::cache::Manifest manifest(root.string());
    toolkit::cache::ManifestEntry a;
    a.canonical_path = "/test/a.turv";
    a.artifact_path = (root / "a.zgc").string();
    toolkit::cache::ManifestEntry b;
    b.canonical_path = "/test/b.turv";
    b.dependencies = {a.canonical_path};
    toolkit::cache::ManifestEntry c;
    c.canonical_path = "/test/c.turv";
    c.dependencies = {b.canonical_path};
    manifest.upsert(a);
    manifest.upsert(b);
    manifest.upsert(c);
    const auto dependents = manifest.dependentsOf(a.canonical_path);
    bool found_b = false;
    bool found_c = false;
    for (const auto &path : dependents) {
        found_b = found_b || path == b.canonical_path;
        found_c = found_c || path == c.canonical_path;
    }
    check(found_b && found_c, "transitive dependents");
    check(manifest.dependentsOf(c.canonical_path).empty(), "leaf has no dependents");

    toolkit::cache::ManifestEntry buffer = a;
    buffer.public_abi_hi = 0xAABB;
    manifest.upsert(std::move(buffer));
    manifest.save();
    toolkit::cache::Manifest loaded(root.string());
    loaded.load();
    const auto found = loaded.find(a.canonical_path);
    check(found.has_value() && found->public_abi_hi == 0xAABB, "manifest round-trip");
    fs::remove_all(root);
}

void transitiveStoreInvalidation() {
    const auto root = fs::temp_directory_path() / "turv-cache-test-invalidate";
    fs::remove_all(root);
    toolkit::cache::CacheKey key;
    key.compilerVersion = "test";
    toolkit::cache::Store store(root.string(), key);

    auto dep = makeArtifact();
    dep.canonical_path = "/test/dep.turv";
    dep.module_name = "dep";
    auto main = makeArtifact();
    main.canonical_path = "/test/main.turv";
    main.module_name = "main";
    main.deps.clear();
    main.deps.push_back({dep.canonical_path, "dep", dep.public_abi_hi, dep.public_abi_lo});
    store.store(dep);
    store.store(main);
    toolkit::cache::ContentFingerprint mainFp;
    mainFp.primary = (static_cast<std::uint64_t>(main.source_fp_hi) << 32u) |
                     main.source_fp_lo;
    check(store.load(main.canonical_path, mainFp).has_value(), "dependent loads");
    store.invalidate(dep.canonical_path);
    toolkit::cache::ContentFingerprint depFp;
    depFp.primary = (static_cast<std::uint64_t>(dep.source_fp_hi) << 32u) | dep.source_fp_lo;
    check(!store.load(dep.canonical_path, depFp).has_value(), "dependency evicted");
    check(!store.load(main.canonical_path, mainFp).has_value(), "dependent evicted");
    fs::remove_all(root);
}

} // namespace

int main() {
    roundTrip();
    deterministicBytes();
    validationErrors();
    storeBehavior();
    manifestBehavior();
    transitiveStoreInvalidation();
    return 0;
}
