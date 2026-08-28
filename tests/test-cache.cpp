#include "cache/artifact-builder.hpp"
#include "cache/cache.hpp"
#include "hir/hir-expr.hpp"
#include "memory/arena.hpp"
#include "memory/string-interner.hpp"
#include "session/frontend-context.hpp"
#include "symbols/symbol-table.hpp"
#include "test-common.hpp"
#include "types/type-intern.hpp"
#include "zirl/zirl-buffer.hpp"
#include "zirl/zirl-reader.hpp"
#include "zirl/zirl-writer.hpp"

#include <algorithm>
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
    art.strings        = {"main", "println", "i32"};
    art.paths          = {std::string(path)};
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
    ret.kind       = CompactExprKind::Ret;
    ret.ref_a      = ~uint32_t{0};
    blk.terminator = 0;
    fn.blocks.push_back(blk);
    art.functions.push_back(fn);
    art.exprs.push_back(ret);
    return art;
}

// ── Binary round-trip ─────────────────────────────────────────────
static void test_binary_round_trip() {
    auto original = makeMinimalArtifact("/test/main.zith", "main");

    ByteWriter writer;
    (void)Writer::write(original, writer);

    auto decoded =
        Reader::read(std::string_view(reinterpret_cast<const char *>(writer.ptr()), writer.size()));
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
    CHECK_EQ(decoded->exprs.size(), original.exprs.size(), "expression pool size preserved");
    CHECK_EQ(decoded->strings.size(), original.strings.size(), "string table size preserved");
    CHECK_EQ(decoded->types.size(), original.types.size(), "type table size preserved");
    if (!decoded->decls.empty()) {
        const auto &name_str = decoded->strings[decoded->decls[0].name_id];
        CHECK_EQ(name_str, "main", "first decl name preserved via string table");
        CHECK_EQ(static_cast<int>(decoded->decls[0].kind), static_cast<int>(CompactSymKind::Fn),
                 "first decl kind preserved");
    }
}

// ── Round-trip with dependency records ────────────────────────────
static void test_deps_round_trip() {
    auto original = makeMinimalArtifact("/test/dep_main.zith", "main");
    original.deps.push_back({"/test/dep_a.zith", "depA", 0xDEADBEEFu, 0xCAFEBABEu});
    original.deps.push_back({"/test/dep_b.zith", "depB", 0x11223344u, 0x55667788u});

    ByteWriter writer;
    (void)Writer::write(original, writer);

    auto decoded =
        Reader::read(std::string_view(reinterpret_cast<const char *>(writer.ptr()), writer.size()));
    CHECK(decoded.has_value(), "artifact with deps round-trips through binary format");
    if (!decoded)
        return;

    CHECK_EQ(decoded->deps.size(), 2u, "dependency count preserved");
    CHECK_EQ(decoded->deps[0].canonical_path, "/test/dep_a.zith", "first dep path preserved");
    CHECK_EQ(decoded->deps[0].import_key, "depA", "first dep import key preserved");
    CHECK_EQ(decoded->deps[0].public_abi_hi, 0xDEADBEEFu, "first dep ABI hi preserved");
    CHECK_EQ(decoded->deps[0].public_abi_lo, 0xCAFEBABEu, "first dep ABI lo preserved");
    CHECK_EQ(decoded->deps[1].canonical_path, "/test/dep_b.zith", "second dep path preserved");
    CHECK_EQ(decoded->deps[1].import_key, "depB", "second dep import key preserved");
    CHECK_EQ(decoded->deps[1].public_abi_hi, 0x11223344u, "second dep ABI hi preserved");
    CHECK_EQ(decoded->deps[1].public_abi_lo, 0x55667788u, "second dep ABI lo preserved");
}

// ── header_size must account for dep records ──────────────────────
static void test_header_size_covered_by_writer() {
    auto original = makeMinimalArtifact("/test/header_size.zith", "main");
    original.deps.push_back({"/test/dep_short.zith", "s", 1, 2});
    original.deps.push_back(
        {"/test/a_much_longer_dependency_path/file.zith", "importKeyLong", 3, 4});

    ByteWriter writer;
    (void)Writer::write(original, writer);

    // The first SectionEntry (section 0, metadata) offset is stored as a u64
    // right after the canonical path and dep records.  It must equal header_size
    // as stored in the FileHeader, which means header_size includes the deps.
    uint32_t header_size  = 0;
    uint64_t first_offset = 0;
    {
        zirl::ByteReader r(writer.ptr(), writer.size());
        // Skip FileHeader field-wise: magic, version, 4 x u8, then u32 fields.
        uint32_t magic = 0, version = 0;
        uint8_t a = 0, b = 0, c = 0, sc = 0;
        CHECK(r.readU32(magic) && r.readU32(version) && r.readU8(a) && r.readU8(b) && r.readU8(c) &&
                  r.readU8(sc),
              "reader walks the FileHeader");
        CHECK_EQ(magic, kMagic, "magic is readable");
        CHECK_EQ(version, kFormatVersion, "version is readable");
        uint32_t checksum = 0, cache_key_hash = 0, mod_hi = 0, mod_lo = 0, src_hi = 0, src_lo = 0;
        uint32_t abi_hi = 0, abi_lo = 0, dep_count = 0, decl_count = 0, template_count = 0;
        uint32_t fn_count = 0, path_len = 0;
        CHECK(r.readU32(header_size) && r.readU32(checksum) && r.readU32(cache_key_hash) &&
                  r.readU32(mod_hi) && r.readU32(mod_lo) && r.readU32(src_hi) &&
                  r.readU32(src_lo) && r.readU32(abi_hi) && r.readU32(abi_lo) &&
                  r.readU32(dep_count) && r.readU32(decl_count) && r.readU32(template_count) &&
                  r.readU32(fn_count) && r.readU32(path_len),
              "header u32 fields are readable");
        CHECK_EQ(dep_count, 2u, "dep count matches");
        // Skip canonical path + both dep records, then read the section table.
        for (uint32_t i = 0; i < path_len; ++i) {
            uint8_t byte = 0;
            CHECK(r.readU8(byte), "canonical path bytes skipped");
        }
        for (uint32_t i = 0; i < dep_count; ++i) {
            std::string path, key;
            uint32_t hi = 0, lo = 0;
            CHECK(r.readBlob(path) && r.readBlob(key) && r.readU32(hi) && r.readU32(lo),
                  "dep record skipped");
        }
        CHECK(r.readU64(first_offset), "first section offset is readable");
    }
    CHECK_EQ(first_offset, static_cast<uint64_t>(header_size),
             "first section offset equals header_size, so header_size covers the deps");
}

// ── Corrupted artifact is rejected ────────────────────────────────
static void test_corrupted_artifact_rejected() {
    auto art = makeMinimalArtifact("/test/bad.zith", "bad");
    ByteWriter writer;
    (void)Writer::write(art, writer);

    // Truncate the buffer.
    auto truncated =
        std::string_view(reinterpret_cast<const char *>(writer.ptr()), writer.size() / 2);
    auto decoded = Reader::read(truncated);
    CHECK(!decoded.has_value(), "truncated artifact is rejected");

    // Flip a byte in the payload to break the checksum.
    std::string corrupted(reinterpret_cast<const char *>(writer.ptr()), writer.size());
    if (corrupted.size() > 10)
        corrupted[corrupted.size() - 5] ^= 0xFF;
    auto decoded2 = Reader::read(corrupted);
    CHECK(!decoded2.has_value(), "checksum-mismatched artifact is rejected");
}

// ── Corrupted header_size is rejected as a miss ───────────────────
static void test_corrupted_header_size_rejected() {
    auto art = makeMinimalArtifact("/test/bad_hdr.zith", "bad");
    art.deps.push_back({"/test/dep.zith", "dep", 0x1111u, 0x2222u});
    ByteWriter writer;
    (void)Writer::write(art, writer);

    // Overwrite header_size in the written bytes with a wrong value.  The
    // reader must reject the artifact instead of misparsing it.
    const size_t header_size_offset = 4u + 4u + 4u; // magic + version + 4 x u8
    std::string corrupted(reinterpret_cast<const char *>(writer.ptr()), writer.size());
    corrupted[header_size_offset]     = 0x00;
    corrupted[header_size_offset + 1] = 0x00;
    corrupted[header_size_offset + 2] = 0x01;
    corrupted[header_size_offset + 3] = 0x00; // header_size = 0x10000
    auto decoded                      = Reader::read(corrupted);
    CHECK(!decoded.has_value(), "corrupted header_size is rejected as a miss");
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
    auto missed      = store.load("/test/store.zith", wrong_fp);
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
    main_fp.primary =
        (static_cast<uint64_t>(dependent.source_fp_hi) << 32u) | dependent.source_fp_lo;

    auto dep_loaded = store.load("/test/dep.zith", dep_fp);
    CHECK(dep_loaded.has_value(), "dependency loads before invalidation");

    // Invalidate the dependency; the dependent should also be evicted.
    store.invalidate("/test/dep.zith");
    auto dep_after = store.load("/test/dep.zith", dep_fp);
    CHECK(!dep_after.has_value(), "dependency is gone after invalidation");

    auto main_after = store.load("/test/main.zith", main_fp);
    CHECK(!main_after.has_value(), "dependent is also evicted after invalidation");

    fs::remove_all(root);
}

static void test_variadic_slice_param_round_trip() {
    auto original                              = makeMinimalArtifact("/test/variadic.zith", "main");
    original.functions[0].is_variadic          = true;
    original.functions[0].variadic_slice_param = 1;

    ByteWriter writer;
    (void)Writer::write(original, writer);

    auto decoded =
        Reader::read(std::string_view(reinterpret_cast<const char *>(writer.ptr()), writer.size()));
    CHECK(decoded.has_value(), "variadic slice artifact round-trips");
    if (!decoded)
        return;
    CHECK(decoded->functions[0].is_variadic, "variadic flag survives round-trip");
    CHECK_EQ(decoded->functions[0].variadic_slice_param, 1u,
             "variadic slice parameter survives round-trip");
}

static void test_zero_abi_dependency_skips_validation() {
    auto root = fs::temp_directory_path() / "zith-cache-test-zero-abi";
    fs::remove_all(root);
    fs::create_directories(root);

    session::CacheKey key;
    key.compilerVersion = "test";
    const auto key_hash = static_cast<uint32_t>(zirl::fnv1a32(key.identity()));
    Store store(root.string(), key);

    auto dep = makeMinimalArtifact("/test/dep.zith", "dep", key_hash);
    store.store(dep);

    Artifact dependent = makeMinimalArtifact("/test/main.zith", "main", key_hash);
    dependent.deps.push_back({"/test/dep.zith", "dep", 0, 0});
    store.store(dependent);

    session::ContentFingerprint main_fp;
    main_fp.primary =
        (static_cast<uint64_t>(dependent.source_fp_hi) << 32u) | dependent.source_fp_lo;

    auto loaded = store.load("/test/main.zith", main_fp);
    CHECK(loaded.has_value(), "zero ABI dependency does not invalidate the artifact");

    fs::remove_all(root);
}

// ── Non-zero dep ABI validation ───────────────────────────────────
static void test_dep_abi_validation() {
    auto root = fs::temp_directory_path() / "zith-cache-test-dep-abi";
    fs::remove_all(root);
    fs::create_directories(root);

    session::CacheKey key;
    key.compilerVersion = "test";
    const auto key_hash = static_cast<uint32_t>(zirl::fnv1a32(key.identity()));
    Store store(root.string(), key);

    auto dep = makeMinimalArtifact("/test/dep.zith", "dep", key_hash);
    store.store(dep);

    Artifact matching = makeMinimalArtifact("/test/match.zith", "match", key_hash);
    matching.deps.push_back({"/test/dep.zith", "dep", dep.public_abi_hi, dep.public_abi_lo});
    store.store(matching);

    session::ContentFingerprint match_fp;
    match_fp.primary =
        (static_cast<uint64_t>(matching.source_fp_hi) << 32u) | matching.source_fp_lo;
    auto match_loaded = store.load("/test/match.zith", match_fp);
    CHECK(match_loaded.has_value(), "dependent with matching non-zero dep ABI loads as a hit");

    Artifact mismatched = makeMinimalArtifact("/test/mismatch.zith", "mismatch", key_hash);
    mismatched.deps.push_back({"/test/dep.zith", "dep", 0xDEADBEEFu, 0xCAFEBABEu});
    store.store(mismatched);

    session::ContentFingerprint mismatch_fp;
    mismatch_fp.primary =
        (static_cast<uint64_t>(mismatched.source_fp_hi) << 32u) | mismatched.source_fp_lo;
    auto mismatch_loaded = store.load("/test/mismatch.zith", mismatch_fp);
    CHECK(!mismatch_loaded.has_value(), "dependent with mismatched dep ABI is invalidated");

    fs::remove_all(root);
}

static void test_manifest_load_skips_malformed_record() {
    auto root = fs::temp_directory_path() / "zith-cache-test-manifest";
    fs::remove_all(root);
    fs::create_directories(root / "modules");

    {
        std::ofstream out(root / "modules" / "manifest", std::ios::binary | std::ios::trunc);
        out << "/test/a.zith\x1f"
               "/test/a.zirl\x1f"
               "NOTHEX\x1f"
               "00000001\x1f"
               "00000002\x1f"
               "00000003\x1e\n";
    }

    session::CacheKey key;
    key.compilerVersion = "test";
    Store store(root.string(), key);
    CHECK(!store.manifestEntry("/test/a.zith").has_value(), "malformed manifest record is ignored");

    fs::remove_all(root);
}

// ── Byte-stable output ────────────────────────────────────────────
static void test_byte_stability() {
    auto art = makeMinimalArtifact("/test/stable.zith", "stable");

    ByteWriter w1;
    (void)Writer::write(art, w1);
    ByteWriter w2;
    (void)Writer::write(art, w2);

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

static void test_format_version_bump() {
    // The format version is the forward-compat gate for stale caches.  A
    // reader that only knows an older format must reject a newer artifact.
    auto art = makeMinimalArtifact("/test/main.zith", "main");
    ByteWriter writer;
    (void)Writer::write(art, writer);

    std::string bytes(reinterpret_cast<const char *>(writer.ptr()), writer.size());
    CHECK_EQ(kFormatVersion, 13u, "zirl format version is bumped");

    // Simulate an old reader by treating the version field as v3.
    bytes[4]        = 3;
    auto old_reader = Reader::read(bytes);
    CHECK(!old_reader.has_value(), "v13 artifact is rejected by a v3-only reader");
}

static void test_artifact_builder() {
    // A representative HIR module covering the compact expression variants.
    memory::Arena arena;
    memory::StringInterner interner(arena);
    types::TypeIntern types(arena, interner);
    symbols::SymbolTable syms(arena, &interner);
    hir::HirModule hir(arena);

    // Declare a public function.
    const auto main_sym =
        syms.declare("main", symbols::SymbolVisibility::Public, 0, symbols::SymKind::Fn);
    const auto loop_sym =
        syms.declare("Loop", symbols::SymbolVisibility::Public, 0, symbols::SymKind::Fn);
    const auto struct_sym =
        syms.declare("Point", symbols::SymbolVisibility::Public, 0, symbols::SymKind::Struct);
    const auto method_sym =
        syms.declare("scale", symbols::SymbolVisibility::Public, 0, symbols::SymKind::Fn);
    syms.get(struct_sym).members.push(method_sym);

    const auto i32 = types.internInt(types::IntWidth::I32);
    const auto f64 = types.internFloat(types::FloatWidth::F64);
    const auto opt = types.internOptional(i32);

    // Add a concrete HIR function.
    auto &fn              = hir.addFn(interner.intern("main"));
    fn.return_type        = i32;
    fn.isVariadic         = true;
    fn.variadicSliceParam = 1;
    fn.sym_id             = main_sym;

    hir::HirLiteral int_lit;
    int_lit.type      = i32;
    int_lit.i         = 3;
    const auto int_id = hir.addExpr(int_lit);

    hir::HirLiteral float_lit;
    float_lit.type      = f64;
    float_lit.f         = 1.5;
    const auto float_id = hir.addExpr(float_lit);

    hir::HirLiteral bool_lit;
    bool_lit.type      = types::kBoolType;
    bool_lit.b         = true;
    const auto bool_id = hir.addExpr(bool_lit);

    hir::HirLiteral str_lit;
    str_lit.type      = types.internPtr(types::kCharType);
    str_lit.str_val   = interner.intern("hi");
    const auto str_id = hir.addExpr(str_lit);

    hir::HirBinary bin;
    bin.lhs           = int_id;
    bin.rhs           = int_id;
    bin.op            = hir::HirBinaryOp::Add;
    bin.type          = i32;
    bin.operand_type  = i32;
    const auto bin_id = hir.addExpr(bin);

    hir::HirUnary un;
    un.op            = hir::HirUnaryOp::Neg;
    un.operand       = bin_id;
    un.type          = i32;
    const auto un_id = hir.addExpr(un);

    hir::HirLet let;
    let.name          = interner.intern("x");
    let.type          = i32;
    let.init          = int_id;
    const auto let_id = hir.addExpr(let);

    hir::HirVar var;
    var.name          = interner.intern("x");
    var.version       = 1;
    const auto var_id = hir.addExpr(var);

    hir::HirExprId direct_call_id = hir::kInvalidHirExpr;
    { // Call with argument types and a resolved fn id.
        memory::DynArray<hir::HirExprId> args(arena);
        args.push(var_id);
        memory::DynArray<types::TypeId> arg_types(arena);
        arg_types.push(i32);
        hir::HirCall call(var_id, std::move(args), std::move(arg_types));
        call.resolved_fn = loop_sym;
        direct_call_id   = hir.addExpr(std::move(call));
    }

    { // Indirect call with a lowered function type and no direct symbol.
        const types::TypeId params[] = {i32};
        const auto fn_type           = types.internFn(params, i32);
        memory::DynArray<hir::HirExprId> args(arena);
        memory::DynArray<types::TypeId> arg_types(arena);
        hir::HirCall call(var_id, std::move(args), std::move(arg_types));
        call.fn_type = fn_type;
        hir.addExpr(std::move(call));
    }

    hir::HirRet ret;
    ret.value = un_id;
    hir.addExpr(ret);

    hir::HirBranch branch;
    branch.cond       = bool_id;
    branch.then_block = 1;
    branch.else_block = 2;
    hir.addExpr(branch);

    hir::HirJump jump;
    jump.target = 0;
    hir.addExpr(jump);

    {
        memory::DynArray<hir::HirExprId> incoming(arena);
        incoming.push(int_id);
        incoming.push(var_id);
        hir::HirPhi phi(arena);
        phi.incoming = std::move(incoming);
        hir.addExpr(std::move(phi));
    }

    hir::HirAssign assign;
    assign.target = var_id;
    assign.value  = int_id;
    hir.addExpr(assign);

    {
        auto arr = types.internArray(i32, 4);
        hir::HirIndex idx;
        idx.object   = var_id;
        idx.index    = int_id;
        idx.type     = i32;
        idx.obj_type = arr;
        idx.is_array = true;
        hir.addExpr(idx);
    }

    {
        auto ptr = types.internPtr(i32);
        hir::HirField field;
        field.object      = var_id;
        field.index       = 0;
        field.type        = i32;
        field.object_type = ptr;
        hir.addExpr(field);
    }

    {
        hir::HirStructLiteral lit(arena);
        lit.type   = i32;
        lit.values = memory::DynArray<hir::HirExprId>(arena);
        lit.values.push(int_id);
        hir.addExpr(std::move(lit));
    }

    {
        hir::HirArrayLiteral lit(arena);
        lit.type     = types.internArray(i32, 2);
        lit.elements = memory::DynArray<hir::HirExprId>(arena);
        lit.elements.push(int_id);
        hir.addExpr(std::move(lit));
    }

    {
        hir::HirEnumValue ev;
        ev.value = 2;
        ev.type  = i32;
        hir.addExpr(ev);
    }

    {
        hir::HirSlotAlloca alloca;
        alloca.slot = 0;
        alloca.type = i32;
        hir.addExpr(alloca);
    }
    {
        hir::HirSlotStore store;
        store.slot  = 0;
        store.value = int_id;
        hir.addExpr(store);
    }
    {
        hir::HirSlotLoad load;
        load.slot = 0;
        load.type = i32;
        hir.addExpr(load);
    }
    {
        hir::HirSlotAddr addr;
        addr.slot = 0;
        addr.type = i32;
        hir.addExpr(addr);
    }
    {
        hir::HirMakeNone none;
        none.type = opt;
        hir.addExpr(none);
    }
    {
        hir::HirMakeSome some;
        some.value = int_id;
        some.type  = opt;
        hir.addExpr(some);
    }
    {
        const auto array_type = types.internArray(i32, 4);
        const auto slice_type = types.internSlice(i32);
        hir::HirMakeSlice slice;
        slice.object      = var_id;
        slice.lo          = int_id;
        slice.hi          = int_id;
        slice.type        = slice_type;
        slice.object_type = array_type;
        slice.bound_type  = i32;
        slice.is_array    = true;
        slice.checked     = true;
        hir.addExpr(slice);
    }
    {
        hir::HirCast cast;
        cast.value = int_id;
        cast.from  = i32;
        cast.to    = f64;
        hir.addExpr(cast);
    }
    {
        hir::HirLayoutIntrinsic layout;
        layout.which       = hir::HirLayoutIntrinsic::Which::OffsetOf;
        layout.type        = i32;
        layout.field_index = 1;
        hir.addExpr(layout);
    }
    hir::HirExprId state_tail_id = hir::kInvalidHirExpr;
    {
        hir::HirStateTailCall tail(arena);
        tail.call.callee      = hir::kInvalidHirExpr;
        tail.call.resolved_fn = loop_sym;
        tail.call.musttail    = true;
        tail.call.usesTailCC  = true;
        tail.call.args.push(int_id);
        tail.call.argument_types.push(i32);
        state_tail_id = hir.addExpr(std::move(tail));
    }
    hir::HirExprId make_dyn_id = hir::kInvalidHirExpr;
    hir::HirExprId dyn_call_id = hir::kInvalidHirExpr;
    types::TypeId source_type  = types::kErrorType;
    types::TypeId dyn_type     = types::kErrorType;
    types::TypeId fn_type      = types::kErrorType;
    {
        source_type = types.defineStruct("_DynSource");
        types.addField(source_type, "value", i32);
        const auto dyn_target = types.internTrait("_DynTarget");
        dyn_type              = types.internDyn(dyn_target, 1);
        hir::HirMakeDyn make;
        make.value       = var_id;
        make.source_type = source_type;
        make.dyn_type    = dyn_type;
        make.vtable_name = interner.intern("_test.vtable.DynTarget._DynSource");
        make_dyn_id      = hir.addExpr(std::move(make));

        memory::DynArray<types::TypeId> params(arena);
        memory::DynArray<hir::HirExprId> args(arena);
        memory::DynArray<types::TypeId> arg_types(arena);
        args.push(int_id);
        arg_types.push(i32);
        params.push(types.internPtr(source_type));
        fn_type = types.internFn(params, i32);
        hir::HirDynCall call(arena);
        call.receiver     = make_dyn_id;
        call.vtable_name  = interner.intern("DynTarget");
        call.slot_index   = 2;
        call.has_receiver = true;
        call.result_type  = i32;
        call.fn_type      = fn_type;
        call.args         = std::move(args);
        call.arg_types    = std::move(arg_types);
        dyn_call_id       = hir.addExpr(std::move(call));

        auto &vtable = hir.addVTable(interner.intern("_test.vtable.DynTarget._DynSource"));
        vtable.slots.push(main_sym);
        vtable.slots.push(loop_sym);
    }

    hir.attrs().slot(0).ownership = hir::HirOwnership::View;
    hir.attrs().slot(0).consumed  = hir::HirConsumedState::NonConsumed;
    hir.attrs().slot(0).nonNull   = true;
    {
        auto &call_attrs      = hir.attrs().call(direct_call_id);
        call_attrs.returnsArg = 0;
        call_attrs.args.emplace(hir::HirCallArgAttr{hir::HirCallEscape::Borrow});
    }
    hir.attrs().fn(0).noAlias = true;

    fn.blocks.emplace(arena);
    fn.blocks[0].insts.push(int_id);
    fn.blocks[0].terminator = state_tail_id;

    auto &state_fn             = hir.addFn(interner.intern("Loop"));
    state_fn.return_type       = i32;
    state_fn.isState           = true;
    state_fn.usesTailCC        = true;
    state_fn.machineId         = 77;
    state_fn.machineReturnType = i32;
    state_fn.sym_id            = loop_sym;
    state_fn.params.push(i32);
    state_fn.param_names.push(interner.intern("n"));
    state_fn.blocks.emplace(arena);
    state_fn.blocks[0].terminator = state_tail_id;

    // Private composite types and explicit non-series enum discriminants must
    // survive the artifact writer so hydration does not depend on exported decl
    // surface or rederive discriminants from variant position.
    const auto private_struct = types.defineStruct("_CachePrivate");
    types.addField(private_struct, "n", i32);
    const auto private_enum = types.defineEnum("_CacheEnum", i32);
    types.addEnumVariant(private_enum, "First", -3);
    types.addEnumVariant(private_enum, "Second", 4);
    const auto private_union = types.defineUnion("_CacheUnion", false);
    types.addUnionMember(private_union, i32);
    types.addUnionMember(private_union, f64);

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
    CHECK_EQ(art.functions.size(), 2u, "builder extracts HIR functions");
    CHECK_EQ(art.exprs.size(), hir.exprCount(), "builder serializes module expression pool");
    CHECK_EQ(art.source_fp_hi, 0u, "builder stores source fingerprint hi");
    CHECK_EQ(art.functions[0].is_variadic, true, "builder stores HIR variadic flag");
    CHECK_EQ(art.functions[0].variadic_slice_param, 1u,
             "builder stores the HIR variadic slice parameter");
    const auto indirect_call =
        std::find_if(art.exprs.begin(), art.exprs.end(), [](const cache::CompactExpr &expr) {
            return expr.kind == cache::CompactExprKind::Call && expr.ref_b == symbols::kInvalidSym;
        });
    CHECK(indirect_call != art.exprs.end(), "builder serializes an indirect call");
    if (indirect_call != art.exprs.end())
        CHECK(indirect_call->ref_e != 0u, "builder preserves the indirect call's lowered fn type");
    const auto make_slice =
        std::find_if(art.exprs.begin(), art.exprs.end(), [](const cache::CompactExpr &expr) {
            return expr.kind == cache::CompactExprKind::MakeSlice;
        });
    CHECK(make_slice != art.exprs.end(), "builder serializes a slice-range expression");
    if (make_slice != art.exprs.end()) {
        CHECK_EQ(make_slice->ref_a, var_id, "slice range serializes the object");
        CHECK_EQ(make_slice->ref_b, int_id, "slice range serializes the lower bound");
        CHECK_EQ(make_slice->ref_c, int_id, "slice range serializes the upper bound");
        const auto i32_kind = art.types[make_slice->ref_f].kind;
        CHECK(i32_kind == CompactTypeKind::Int && art.types[make_slice->ref_f].int_width ==
                                                      static_cast<uint8_t>(types::IntWidth::I32),
              "slice range serializes an i32 bound type");
        CHECK((make_slice->flags & 1U) != 0, "slice range serializes the array flag");
        CHECK((make_slice->flags & 2U) != 0, "slice range serializes the runtime-check flag");
    }
    const auto point_decl = std::find_if(art.decls.begin(), art.decls.end(),
                                         [](const DeclRecord &d) { return d.name == "Point"; });
    CHECK(point_decl != art.decls.end(), "builder extracts public struct declaration");
    const auto method_decl = std::find_if(art.decls.begin(), art.decls.end(),
                                          [](const DeclRecord &d) { return d.name == "scale"; });
    CHECK(method_decl != art.decls.end(), "builder extracts public method declaration");
    if (point_decl != art.decls.end() && method_decl != art.decls.end())
        CHECK(int(point_decl->method_decl_indices.size()) == 1 &&
                  point_decl->method_decl_indices[0] ==
                      static_cast<uint32_t>(std::distance(art.decls.begin(), method_decl)),
              "method refs use artifact decl indices");
    CHECK_EQ(art.attrs_slots.size(), 1u, "builder serializes slot attrs");
    CHECK_EQ(art.attrs_calls.size(), 1u, "builder serializes call attrs");
    CHECK_EQ(art.attrs_fns.size(), 1u, "builder serializes fn attrs");
    CHECK_EQ(art.struct_defs.size(), 2u, "builder serializes private struct defs");
    CHECK_EQ(art.enum_defs.size(), 1u, "builder serializes private enum defs");
    CHECK_EQ(art.enum_defs[0].variants.size(), 2u, "enum variants are serialized");
    CHECK_EQ(art.enum_defs[0].variants[0].discriminant, -3, "enum discriminants are not reindexed");
    CHECK_EQ(art.union_defs.size(), 1u, "builder serializes private union defs");
    CHECK(art.union_defs[0].name_id != 0u, "builder assigns a name_id for the union");
    CHECK_EQ(art.union_defs[0].is_raw, true, "builder serializes raw union flag");
    CHECK_EQ(art.union_defs[0].member_type_ids.size(), 2u,
             "builder serializes every union member type");
    const auto state_function =
        std::find_if(art.functions.begin(), art.functions.end(),
                     [](const CompactFunction &function) { return function.is_state; });
    CHECK(state_function != art.functions.end(), "builder serializes state function metadata");
    if (state_function != art.functions.end())
        CHECK_EQ(state_function->machine_id, 77u, "builder serializes state machine ids");
    if (state_function != art.functions.end())
        CHECK(state_function->uses_tailcc, "builder serializes state tailcc flags");
    if (state_function != art.functions.end())
        CHECK_EQ(state_function->machine_return_type_id, state_function->return_type_id,
                 "builder serializes the machine return type");
    const auto state_tail =
        std::find_if(art.exprs.begin(), art.exprs.end(), [](const cache::CompactExpr &expr) {
            return expr.kind == cache::CompactExprKind::StateTailCall;
        });
    CHECK(state_tail != art.exprs.end(), "builder serializes state tail calls");
    const auto make_dyn_compact =
        std::find_if(art.exprs.begin(), art.exprs.end(), [](const cache::CompactExpr &expr) {
            return expr.kind == cache::CompactExprKind::MakeDyn;
        });
    CHECK(make_dyn_compact != art.exprs.end(), "builder serializes a MakeDyn expression");
    if (make_dyn_compact != art.exprs.end()) {
        CHECK_EQ(make_dyn_compact->ref_a, var_id, "MakeDyn serializes the coerced value");
        CHECK(make_dyn_compact->ref_b < art.types.size() &&
                  art.types[make_dyn_compact->ref_b].kind == CompactTypeKind::Struct,
              "MakeDyn serializes the source type as a concrete struct type");
        CHECK(make_dyn_compact->type_id < art.types.size() &&
                  art.types[make_dyn_compact->type_id].kind == CompactTypeKind::Dyn,
              "MakeDyn serializes the dyn type");
        if (make_dyn_compact->type_id < art.types.size())
            CHECK_EQ(art.types[make_dyn_compact->type_id].ref1, 1u,
                     "MakeDyn's dyn type serializes the interface method count");
        CHECK(make_dyn_compact->name_id < art.strings.size() &&
                  art.strings[make_dyn_compact->name_id] == "_test.vtable.DynTarget._DynSource",
              "MakeDyn serializes the concrete vtable name");
    }
    const auto dyn_call_compact =
        std::find_if(art.exprs.begin(), art.exprs.end(), [](const cache::CompactExpr &expr) {
            return expr.kind == cache::CompactExprKind::DynCall;
        });
    CHECK(dyn_call_compact != art.exprs.end(), "builder serializes a DynCall expression");
    if (dyn_call_compact != art.exprs.end()) {
        CHECK_EQ(dyn_call_compact->ref_a, make_dyn_id,
                 "DynCall serializes the fat-pointer receiver");
        CHECK_EQ(dyn_call_compact->ref_c, 2u, "DynCall serializes the slot index");
        CHECK(dyn_call_compact->type_id < art.types.size() &&
                  art.types[dyn_call_compact->type_id].kind == CompactTypeKind::Int &&
                  static_cast<types::IntWidth>(art.types[dyn_call_compact->type_id].int_width) ==
                      types::IntWidth::I32,
              "DynCall serializes the i32 result type");
        CHECK(dyn_call_compact->ref_e < art.types.size() &&
                  art.types[dyn_call_compact->ref_e].kind == CompactTypeKind::Fn,
              "DynCall serializes the lowered fn type");
        if (dyn_call_compact->ref_e < art.types.size())
            CHECK_EQ(art.types[dyn_call_compact->ref_e].args.size(), 1u,
                     "DynCall's fn type keeps the receiver pointer parameter");
        CHECK_EQ(dyn_call_compact->args.size(), 1u, "DynCall serializes the argument list");
        CHECK_EQ(dyn_call_compact->arg_types.size(), 1u,
                 "DynCall serializes the argument type list");
        CHECK((dyn_call_compact->flags & 1U) != 0, "DynCall serializes has_receiver");
        CHECK(dyn_call_compact->name_id < art.strings.size() &&
                  art.strings[dyn_call_compact->name_id] == "DynTarget",
              "DynCall serializes the interface vtable requirement name");
    }
    CHECK_EQ(art.vtables.size(), 1u, "builder serializes HIR vtables");
    if (art.vtables.size() == 1u) {
        CHECK(art.vtables[0].name_id < art.strings.size() &&
                  art.strings[art.vtables[0].name_id] == "_test.vtable.DynTarget._DynSource",
              "vtable serializes its concrete pairing name");
        CHECK_EQ(art.vtables[0].slot_sym_ids.size(), 2u,
                 "vtable serializes one slot per interface method requirement");
        if (art.vtables[0].slot_sym_ids.size() == 2u) {
            CHECK_EQ(art.vtables[0].slot_sym_ids[0], static_cast<uint32_t>(main_sym),
                     "first vtable slot points to the concrete method symbol");
            CHECK_EQ(art.vtables[0].slot_sym_ids[1], static_cast<uint32_t>(loop_sym),
                     "second vtable slot points to the concrete method symbol");
        }
    }

    // Full zirl round-trip: the artifact from in-memory state must decode with
    // the same HIR pool, blocks, and residual attrs.
    ByteWriter writer;
    (void)Writer::write(art, writer);
    auto round =
        Reader::read(std::string_view(reinterpret_cast<const char *>(writer.ptr()), writer.size()));
    CHECK(round.has_value(), "builder artifact round-trips through zirl");
    if (!round)
        return;
    CHECK_EQ(round->functions.size(), art.functions.size(), "round-trip preserves function count");
    CHECK_EQ(round->exprs.size(), art.exprs.size(), "round-trip preserves expression pool size");
    CHECK_EQ(round->attrs_slots.size(), art.attrs_slots.size(), "round-trip preserves slot attrs");
    CHECK_EQ(round->attrs_calls.size(), art.attrs_calls.size(), "round-trip preserves call attrs");
    CHECK_EQ(round->attrs_fns.size(), art.attrs_fns.size(), "round-trip preserves fn attrs");
    CHECK_EQ(round->struct_defs.size(), art.struct_defs.size(), "round-trip preserves struct defs");
    CHECK_EQ(round->enum_defs.size(), art.enum_defs.size(), "round-trip preserves enum defs");
    CHECK_EQ(round->enum_defs[0].variants[0].discriminant, -3,
             "round-trip preserves explicit enum discriminants");
    CHECK_EQ(round->union_defs.size(), art.union_defs.size(), "round-trip preserves union defs");
    CHECK_EQ(round->union_defs[0].is_raw, art.union_defs[0].is_raw,
             "round-trip preserves union is_raw flag");
    CHECK_EQ(round->union_defs[0].name_id, art.union_defs[0].name_id,
             "round-trip preserves union name_id");
    CHECK_EQ(round->union_defs[0].member_type_ids, art.union_defs[0].member_type_ids,
             "round-trip preserves union member_type_ids");
    const auto round_state_function =
        std::find_if(round->functions.begin(), round->functions.end(),
                     [](const CompactFunction &function) { return function.is_state; });
    CHECK(round_state_function != round->functions.end(),
          "round-trip preserves state function metadata");
    if (round_state_function != round->functions.end())
        CHECK_EQ(round_state_function->machine_id, 77u, "round-trip preserves state machine ids");
    if (round_state_function != round->functions.end())
        CHECK(round_state_function->uses_tailcc, "round-trip preserves state tailcc flags");
    if (round_state_function != round->functions.end())
        CHECK_EQ(round_state_function->machine_return_type_id, round_state_function->return_type_id,
                 "round-trip preserves the machine return type");
    const auto round_state_tail =
        std::find_if(round->exprs.begin(), round->exprs.end(), [](const cache::CompactExpr &expr) {
            return expr.kind == cache::CompactExprKind::StateTailCall;
        });
    CHECK(round_state_tail != round->exprs.end(), "round-trip preserves state tail calls");
    const auto round_make_dyn =
        std::find_if(round->exprs.begin(), round->exprs.end(), [](const cache::CompactExpr &expr) {
            return expr.kind == cache::CompactExprKind::MakeDyn;
        });
    CHECK(round_make_dyn != round->exprs.end(), "round-trip preserves MakeDyn expressions");
    if (round_make_dyn != round->exprs.end()) {
        CHECK_EQ(round_make_dyn->ref_a, make_dyn_compact->ref_a,
                 "round-trip preserves MakeDyn value");
        CHECK_EQ(round_make_dyn->ref_b, make_dyn_compact->ref_b,
                 "round-trip preserves MakeDyn source type");
        CHECK_EQ(round_make_dyn->type_id, make_dyn_compact->type_id,
                 "round-trip preserves MakeDyn dyn type");
        CHECK_EQ(round_make_dyn->name_id, make_dyn_compact->name_id,
                 "round-trip preserves MakeDyn vtable name");
    }
    const auto round_dyn_call =
        std::find_if(round->exprs.begin(), round->exprs.end(), [](const cache::CompactExpr &expr) {
            return expr.kind == cache::CompactExprKind::DynCall;
        });
    CHECK(round_dyn_call != round->exprs.end(), "round-trip preserves DynCall expressions");
    if (round_dyn_call != round->exprs.end()) {
        CHECK_EQ(round_dyn_call->ref_a, dyn_call_compact->ref_a,
                 "round-trip preserves DynCall receiver");
        CHECK_EQ(round_dyn_call->ref_c, dyn_call_compact->ref_c,
                 "round-trip preserves DynCall slot index");
        CHECK_EQ(round_dyn_call->type_id, dyn_call_compact->type_id,
                 "round-trip preserves DynCall result type");
        CHECK_EQ(round_dyn_call->ref_e, dyn_call_compact->ref_e,
                 "round-trip preserves DynCall fn type");
        CHECK(round_dyn_call->args == dyn_call_compact->args,
              "round-trip preserves DynCall arguments");
        CHECK(round_dyn_call->arg_types == dyn_call_compact->arg_types,
              "round-trip preserves DynCall argument types");
        CHECK_EQ(round_dyn_call->flags, dyn_call_compact->flags,
                 "round-trip preserves DynCall receiver flag");
    }
    CHECK_EQ(round->vtables.size(), art.vtables.size(), "round-trip preserves vtable count");
    if (round->vtables.size() == art.vtables.size() && round->vtables.size() == 1u)
        CHECK(round->vtables[0].slot_sym_ids == art.vtables[0].slot_sym_ids &&
                  round->vtables[0].name_id == art.vtables[0].name_id,
              "round-trip preserves vtable name and concrete slot symbols");
}

} // namespace

static void test_cache() {
    test_binary_round_trip();
    test_deps_round_trip();
    test_variadic_slice_param_round_trip();
    test_header_size_covered_by_writer();
    test_corrupted_artifact_rejected();
    test_corrupted_header_size_rejected();
    test_store_hit_miss();
    test_store_invalidation();
    test_zero_abi_dependency_skips_validation();
    test_dep_abi_validation();
    test_manifest_load_skips_malformed_record();
    test_byte_stability();
    test_format_version_bump();
    test_artifact_builder();
}

TEST_MAIN(cache)
