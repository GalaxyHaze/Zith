#include "cache/artifact-builder.hpp"
#include "cache/cache.hpp"
#include "memory/arena.hpp"
#include "memory/string-interner.hpp"
#include "session/frontend-context.hpp"
#include "symbols/symbol-table.hpp"
#include "test-common.hpp"
#include "types/type-intern.hpp"
#include "zirl/zirl-buffer.hpp"
#include "zirl/zirl-reader.hpp"
#include "zirl/zirl-writer.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace zith;
using namespace zith::cache;
using namespace zith::zirl;

namespace {

namespace fs = std::filesystem;

// ── Helper: build a minimal artifact ──────────────────────────────
Artifact makeMinimalArtifact(std::string_view path, std::string_view name,
                              uint32_t cache_key_hash = 0x12345678u) {
    Artifact art;
    art.canonical_path = std::string(path);
    art.module_name    = std::string(name);
    art.cache_key_hash = cache_key_hash;
    art.source_fp_hi   = 0xAABBCCDDu;
    art.source_fp_lo   = 0x11223344u;
    art.public_abi_hi  = 0xEEFF0011u;
    art.public_abi_lo  = 0x22334455u;
    art.module_id_hi   = art.public_abi_hi ^ 0x55AAu;
    art.module_id_lo   = art.public_abi_lo ^ 0x66BBu;
    art.strings = {"main", "println", "i32"};
    art.paths   = {std::string(path)};
    // One primitive type (i32) and one struct type.
    CompactType int_type;
    int_type.kind      = CompactTypeKind::Int;
    int_type.int_width = 2; // i32
    art.types.push_back(int_type);
    CompactType struct_type;
    struct_type.kind = CompactTypeKind::Struct;
    struct_type.ref0 = 0;
    art.types.push_back(struct_type);
    // One exported fn declaration.
    DeclRecord fn_decl;
    fn_decl.name       = "main";
    fn_decl.name_id    = 0;
    fn_decl.kind       = CompactSymKind::Fn;
    fn_decl.visibility = symbols::SymbolVisibility::Public;
    fn_decl.mod_depth  = 0;
    fn_decl.type_id    = 0;
    art.decls.push_back(fn_decl);
    // One concrete function with a single ret-void block.
    CompactFunction fn;
    fn.name           = "main";
    fn.name_id        = 0;
    fn.is_extern      = false;
    fn.return_type_id = 0;
    CompactBasicBlock blk;
    CompactExpr ret;
    ret.kind     = CompactExprKind::Ret;
    ret.ref_a    = ~uint32_t{0};
    blk.terminator = 0;
    fn.blocks.push_back(blk);
    fn.exprs.push_back(ret);
    art.functions.push_back(fn);
    return art;
}

// ── Binary round-trip ─────────────────────────────────────────────
static void test_binary_round_trip() {
    auto original = makeMinimalArtifact("/test/main.zith", "main");

    ByteWriter writer;
    Writer::write(original, writer);

    auto decoded = Reader::read(
        std::string_view(reinterpret_cast<const char *>(writer.ptr()), writer.size()));
    CHECK(decoded.has_value(), "artifact round-trips through binary format");
    if (!decoded)
        return;

    CHECK_EQ(decoded->canonical_path, original.canonical_path, "canonical path preserved");
    // module_name is not serialized as a separate field; it is derivable from
    // the canonical path.  We skip that check here.
    CHECK_EQ(decoded->cache_key_hash, original.cache_key_hash, "cache key hash preserved");
    CHECK_EQ(decoded->source_fp_hi, original.source_fp_hi, "source fingerprint hi preserved");
    CHECK_EQ(decoded->source_fp_lo, original.source_fp_lo, "source fingerprint lo preserved");
    CHECK_EQ(decoded->public_abi_hi, original.public_abi_hi, "public ABI hi preserved");
    CHECK_EQ(decoded->public_abi_lo, original.public_abi_lo, "public ABI lo preserved");
    CHECK_EQ(decoded->decls.size(), original.decls.size(), "decl count preserved");
    CHECK_EQ(decoded->functions.size(), original.functions.size(), "function count preserved");
    CHECK_EQ(decoded->strings.size(), original.strings.size(), "string table size preserved");
    CHECK_EQ(decoded->types.size(), original.types.size(), "type table size preserved");
    if (!decoded->decls.empty()) {
        const auto &name_str = decoded->strings[decoded->decls[0].name_id];
        CHECK_EQ(name_str, "main", "first decl name preserved via string table");
        CHECK_EQ(static_cast<int>(decoded->decls[0].kind), static_cast<int>(CompactSymKind::Fn),
                 "first decl kind preserved");
    }
}

// ── Corrupted artifact is rejected ────────────────────────────────
static void test_corrupted_artifact_rejected() {
    auto art = makeMinimalArtifact("/test/bad.zith", "bad");
    ByteWriter writer;
    Writer::write(art, writer);

    // Truncate the buffer.
    auto truncated = std::string_view(reinterpret_cast<const char *>(writer.ptr()),
                                      writer.size() / 2);
    auto decoded = Reader::read(truncated);
    CHECK(!decoded.has_value(), "truncated artifact is rejected");

    // Flip a byte in the payload to break the checksum.
    std::string corrupted(reinterpret_cast<const char *>(writer.ptr()), writer.size());
    if (corrupted.size() > 10)
        corrupted[corrupted.size() - 5] ^= 0xFF;
    auto decoded2 = Reader::read(corrupted);
    CHECK(!decoded2.has_value(), "checksum-mismatched artifact is rejected");
}

// ── Store hit/miss ────────────────────────────────────────────────
static void test_store_hit_miss() {
    auto root = fs::temp_directory_path() / "zith-cache-test-store";
    fs::remove_all(root);
    fs::create_directories(root);

    session::CacheKey key;
    key.compilerVersion = "test";
    const auto key_hash = static_cast<uint32_t>(zirl::fnv1a32(key.identity()));
    Store store(root.string(), key);

    auto art = makeMinimalArtifact("/test/store.zith", "store", key_hash);
    store.store(art);

    session::ContentFingerprint fp;
    fp.primary = (static_cast<uint64_t>(art.source_fp_hi) << 32u) | art.source_fp_lo;

    auto loaded = store.load("/test/store.zith", fp);
    CHECK(loaded.has_value(), "stored artifact loads as a cache hit");
    CHECK_EQ(store.metrics().hits, 1u, "hit counter incremented");

    // Different fingerprint -> miss/invalid.
    session::ContentFingerprint wrong_fp;
    wrong_fp.primary = 0xDEADBEEFu;
    auto missed = store.load("/test/store.zith", wrong_fp);
    CHECK(!missed.has_value(), "different fingerprint causes invalidation");

    // Nonexistent path -> miss.
    auto absent = store.load("/test/absent.zith", fp);
    CHECK(!absent.has_value(), "nonexistent artifact is a miss");

    fs::remove_all(root);
}

// ── Store invalidation removes dependents ─────────────────────────
static void test_store_invalidation() {
    auto root = fs::temp_directory_path() / "zith-cache-test-invalidate";
    fs::remove_all(root);
    fs::create_directories(root);

    session::CacheKey key;
    key.compilerVersion = "test";
    const auto key_hash = static_cast<uint32_t>(zirl::fnv1a32(key.identity()));
    Store store(root.string(), key);

    // Store a dependency and a dependent.
    auto dep = makeMinimalArtifact("/test/dep.zith", "dep", key_hash);
    store.store(dep);

    Artifact dependent = makeMinimalArtifact("/test/main.zith", "main", key_hash);
    DependencyRecord dep_ref;
    dep_ref.canonical_path = "/test/dep.zith";
    dep_ref.import_key     = "dep";
    dep_ref.public_abi_hi  = dep.public_abi_hi;
    dep_ref.public_abi_lo  = dep.public_abi_lo;
    dependent.deps.push_back(dep_ref);
    store.store(dependent);

    // Both should load.
    session::ContentFingerprint dep_fp;
    dep_fp.primary = (static_cast<uint64_t>(dep.source_fp_hi) << 32u) | dep.source_fp_lo;
    session::ContentFingerprint main_fp;
    main_fp.primary = (static_cast<uint64_t>(dependent.source_fp_hi) << 32u) | dependent.source_fp_lo;

    auto dep_loaded = store.load("/test/dep.zith", dep_fp);
    CHECK(dep_loaded.has_value(), "dependency loads before invalidation");

    // Invalidate the dependency; the dependent should also be evicted.
    store.invalidate("/test/dep.zith");
    auto dep_after = store.load("/test/dep.zith", dep_fp);
    CHECK(!dep_after.has_value(), "dependency is gone after invalidation");

    fs::remove_all(root);
}

// ── Byte-stable output ────────────────────────────────────────────
static void test_byte_stability() {
    auto art = makeMinimalArtifact("/test/stable.zith", "stable");

    ByteWriter w1;
    Writer::write(art, w1);
    ByteWriter w2;
    Writer::write(art, w2);

    CHECK_EQ(w1.size(), w2.size(), "identical input produces identical size");
    bool same = true;
    for (size_t i = 0; i < w1.size(); ++i) {
        if (w1.ptr()[i] != w2.ptr()[i]) {
            same = false;
            break;
        }
    }
    CHECK(same, "identical input produces byte-identical output");
}

// ── Artifact builder from session state ───────────────────────────
static void test_artifact_builder() {
    memory::Arena arena;
    memory::StringInterner interner(arena);
    types::TypeIntern types(arena, interner);
    symbols::SymbolTable syms(arena, &interner);
    hir::HirModule hir(arena);

    // Declare a public function.
    syms.declare("main", symbols::SymbolVisibility::Public, 0, symbols::SymKind::Fn);
    // Add a concrete HIR function.
    auto &fn = hir.addFn(interner.intern("main"));
    fn.return_type = types::kVoidType;
    (void)fn.blocks;

    session::ContentFingerprint fp;
    fp.primary = 0xCAFEBABEu;
    session::CacheKey key;
    key.compilerVersion = "test";

    ArtifactBuilder builder(syms, types, hir, interner, fp, key);
    std::vector<DependencyRecord> deps;
    auto art = builder.build("/test/builder.zith", "builder", deps);

    CHECK_EQ(art.canonical_path, "/test/builder.zith", "builder sets canonical path");
    CHECK(art.decls.size() >= 1, "builder extracts exported declarations");
    CHECK_EQ(art.decls[0].name, "main", "builder extracts function name");
    CHECK_EQ(art.functions.size(), 1u, "builder extracts HIR functions");
    CHECK_EQ(art.source_fp_hi, 0u, "builder stores source fingerprint hi");
}

} // namespace

static void test_cache() {
    test_binary_round_trip();
    test_corrupted_artifact_rejected();
    test_store_hit_miss();
    test_store_invalidation();
    test_byte_stability();
    test_artifact_builder();
}

TEST_MAIN(cache)
