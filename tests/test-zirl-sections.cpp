#include "cache/cache-types.hpp"
#include "hir/hir-attrs.hpp"
#include "test-common.hpp"
#include "zirl/zirl-attrs-section.hpp"
#include "zirl/zirl-buffer.hpp"
#include "zirl/zirl-code-section.hpp"
#include "zirl/zirl-debug-section.hpp"
#include "zirl/zirl-decl-section.hpp"
#include "zirl/zirl-header.hpp"
#include "zirl/zirl-instantiation-section.hpp"
#include "zirl/zirl-reader.hpp"
#include "zirl/zirl-type-section.hpp"
#include "zirl/zirl-writer.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace zith;
using namespace zith::cache;
using namespace zith::zirl;

namespace {

// ── Section-level round-trip helpers ──────────────────────────────

Artifact makeMinimalArtifact() {
    Artifact art;
    art.canonical_path = "/test/main.zith";
    art.module_name    = "main";
    art.cache_key_hash = 0x12345678u;
    art.module_id_hi   = 0xAABBCCDDu;
    art.module_id_lo   = 0x11223344u;
    art.source_fp_hi   = 0xEEFF0011u;
    art.source_fp_lo   = 0x22334455u;
    art.public_abi_hi  = 0x99887766u;
    art.public_abi_lo  = 0x55443322u;

    // strings and paths
    art.strings = {"main", "puts", "i32", "Node"};
    art.paths   = {"/test/main.zith"};

    // one primitive i32
    CompactType int_type;
    int_type.kind      = CompactTypeKind::Int;
    int_type.int_width = 2;
    int_type.flags     = 1; // is_signed
    art.types.push_back(int_type);

    // one struct
    CompactType struct_type;
    struct_type.kind = CompactTypeKind::Struct;
    struct_type.ref0 = 0;
    art.types.push_back(struct_type);

    // function and slice types used by function-pointer bootstrap programs
    CompactType fn_type;
    fn_type.kind = CompactTypeKind::Fn;
    fn_type.ref0 = 0;   // i32 return
    fn_type.args = {0}; // one i32 parameter
    art.types.push_back(fn_type);
    CompactType slice_type;
    slice_type.kind = CompactTypeKind::Slice;
    slice_type.ref0 = 0;
    art.types.push_back(slice_type);

    // one exported fn declaration
    DeclRecord decl;
    decl.name       = "main";
    decl.name_id    = 0;
    decl.kind       = CompactSymKind::Fn;
    decl.visibility = symbols::SymbolVisibility::Public;
    decl.mod_depth  = 0;
    decl.type_id    = 0;
    art.decls.push_back(decl);

    // one dependency record
    DependencyRecord dep;
    dep.canonical_path = "/test/dep.zith";
    dep.import_key     = "dep";
    dep.public_abi_hi  = 0xABCD0001u;
    dep.public_abi_lo  = 0xABCD0002u;
    art.deps.push_back(dep);

    return art;
}

void addCode(Artifact &art) {
    CompactFunction fn;
    fn.name           = "main";
    fn.name_id        = 0;
    fn.is_extern      = false;
    fn.is_variadic    = true;
    fn.return_type_id = 0;

    CompactExpr lit;
    lit.kind    = CompactExprKind::Literal;
    lit.type_id = 0;
    lit.int_val = 42;

    CompactExpr ret;
    ret.kind    = CompactExprKind::Ret;
    ret.type_id = 0;
    ret.ref_a   = 0;

    CompactBasicBlock blk;
    blk.terminator = 1;

    fn.blocks.push_back(blk);
    art.functions.push_back(fn);
    art.exprs.push_back(lit);
    art.exprs.push_back(ret);
    art.canonical_mappings.push_back({0x1122334455667788ULL, 0x99AABBCCDDEEFF00ULL, 0xCAFEBABEu});
}

void addModernCode(Artifact &art) {
    CompactFunction fn;
    fn.name                   = "modern";
    fn.name_id                = 0;
    fn.is_extern              = false;
    fn.is_variadic            = false;
    fn.is_state               = true;
    fn.uses_tailcc            = true;
    fn.machine_id             = 7;
    fn.machine_return_type_id = 0;
    fn.return_type_id         = 0;
    fn.param_type_ids         = {0};
    fn.param_name_ids         = {0};
    fn.param_slot_ids         = {1};

    CompactExpr lit;
    lit.kind    = CompactExprKind::Literal;
    lit.type_id = 0;
    lit.int_val = 7;
    art.functions.push_back(fn);

    art.exprs.push_back(lit);

    CompactExpr cast;
    cast.kind    = CompactExprKind::Cast;
    cast.ref_a   = 0;
    cast.ref_b   = 0;
    cast.ref_e   = 0;
    cast.type_id = 0;
    art.exprs.push_back(cast);

    { // Indirect call keeps the call's lowered function type.
        CompactExpr call;
        call.kind      = CompactExprKind::Call;
        call.ref_a     = 0;
        call.ref_e     = 2;
        call.args      = {0};
        call.arg_types = {0};
        art.exprs.push_back(call);
    }

    { // Slice-range HIR keeps object/bounds/types and the checking flags.
        CompactExpr make_slice;
        make_slice.kind      = CompactExprKind::MakeSlice;
        make_slice.ref_a     = 0;
        make_slice.ref_b     = 0;
        make_slice.ref_c     = 0;
        make_slice.type_id   = 3; // slice type
        make_slice.ref_e     = 0;
        make_slice.ref_f     = 0;
        make_slice.flags     = 3; // array + runtime-checked
        make_slice.arg_types = {};
        art.exprs.push_back(make_slice);
    }

    CompactExpr layout;
    layout.kind    = CompactExprKind::LayoutIntrinsic;
    layout.type_id = 0;
    layout.ref_e   = 0;
    layout.ref_f   = 2;
    art.exprs.push_back(layout);

    CompactExpr tail;
    tail.kind      = CompactExprKind::StateTailCall;
    tail.ref_b     = 9;
    tail.ref_e     = 2;
    tail.args      = {0};
    tail.arg_types = {0};
    art.exprs.push_back(tail);

    CompactExpr canonical;
    canonical.kind    = CompactExprKind::CanonicalType;
    canonical.type_id = 0;
    canonical.ints    = {0x1122334455667788ULL, 0x99AABBCCDDEEFF00ULL};
    art.exprs.push_back(canonical);

    CompactBasicBlock blk;
    blk.terminator = 5;
    fn.blocks.push_back(blk);
}

void addAttrs(Artifact &art) {
    HirSlotAttrsRecord slot;
    slot.slot      = 1;
    slot.ownership = static_cast<uint8_t>(hir::HirOwnership::Unique);
    slot.consumed  = static_cast<uint8_t>(hir::HirConsumedState::NonConsumed);
    slot.nonNull   = true;
    art.attrs_slots.push_back(slot);

    HirCallAttrsRecord call;
    call.expr_id     = 3;
    call.returns_arg = 1;
    call.arg_escapes = {2, 4};
    art.attrs_calls.push_back(call);

    HirFnAttrsRecord fn;
    fn.fn_index        = 0;
    fn.return_consumed = 1;
    fn.nonNull         = true;
    fn.noAlias         = true;
    art.attrs_fns.push_back(fn);
}

void addInstantiations(Artifact &art) {
    InstantiationRecord first;
    first.module        = "main";
    first.mangled       = "main.id<i32>";
    first.template_name = "id";
    first.decl_id       = 1;
    first.arg_types     = {"i32"};
    art.instantiations.push_back(first);

    InstantiationRecord second;
    second.module        = "main";
    second.mangled       = "main.pair<i32,f64>";
    second.template_name = "pair";
    second.decl_id       = 2;
    second.arg_types     = {"i32", "f64"};
    art.instantiations.push_back(second);
}

// ── Type/Metadata section round-trip ──────────────────────────────

static void test_type_section_round_trip() {
    Artifact original = makeMinimalArtifact();

    original.strings.push_back("x");
    original.strings.push_back("y");
    CompactType named_pack;
    named_pack.kind      = CompactTypeKind::Pack;
    named_pack.args      = {0};
    named_pack.arg_names = {2, 3};
    named_pack.ref1      = 1;
    original.types.push_back(named_pack);

    CompactType positional_pack;
    positional_pack.kind = CompactTypeKind::Pack;
    positional_pack.args = {0};
    positional_pack.ref1 = 1;
    original.types.push_back(positional_pack);

    ByteWriter w;
    bool encoded = encodeTypes(original, w);
    CHECK(encoded, "encodeTypes succeeds");
    CHECK(w.size() > 0, "encodeTypes produces non-empty output");

    Artifact decoded;
    ByteReader r(w.ptr(), w.size());
    bool ok = decodeTypes(r, decoded);
    CHECK(ok, "decodeTypes succeeds after encodeTypes");
    CHECK(decoded.strings.size() == original.strings.size(), "decodeTypes preserves string count");
    CHECK(decoded.types.size() == original.types.size(), "decodeTypes preserves type count");
    if (decoded.types.size() >= 4u) {
        CHECK(decoded.types[2].kind == CompactTypeKind::Fn, "decodeTypes preserves fn type kind");
        CHECK_EQ(decoded.types[2].args.size(), 1u, "decodeTypes preserves fn parameter count");
        CHECK(decoded.types[3].kind == CompactTypeKind::Slice,
              "decodeTypes preserves slice type kind");
    }
    CHECK_EQ(decoded.types.size(), original.types.size(), "decodeTypes preserves pack count");
    if (decoded.types.size() >= 6u) {
        CHECK(decoded.types[4].kind == CompactTypeKind::Pack,
              "decodeTypes preserves named pack kind");
        CHECK_EQ(decoded.types[4].arg_names.size(), 2u,
                 "decodeTypes preserves named pack member metadata");
        CHECK_EQ(decoded.types[4].arg_names[0], 2u,
                 "decodeTypes preserves first pack member name id");
        CHECK(decoded.types[5].kind == CompactTypeKind::Pack,
              "decodeTypes preserves positional pack kind");
        CHECK(decoded.types[5].arg_names.empty(),
              "positional pack keeps empty name metadata after decode");
    }
}

// ── Declarations section round-trip ───────────────────────────────

static void test_decl_section_round_trip() {
    Artifact original = makeMinimalArtifact();

    ByteWriter w;
    bool encoded = encodeDecls(original, w);
    CHECK(encoded, "encodeDecls succeeds");
    CHECK(w.size() > 0, "encodeDecls produces non-empty output");

    Artifact decoded;
    ByteReader r(w.ptr(), w.size());
    bool ok = decodeDecls(r, decoded);
    CHECK(ok, "decodeDecls succeeds after encodeDecls");
    CHECK(decoded.decls.size() == original.decls.size(), "decodeDecls preserves decl count");
}

// ── Code section round-trip ───────────────────────────────────────

static void test_code_section_round_trip() {
    Artifact original = makeMinimalArtifact();
    addCode(original);

    ByteWriter w;
    bool encoded = encodeCode(original, w);
    CHECK(encoded, "encodeCode succeeds");
    CHECK(w.size() > 0, "encodeCode produces non-empty output");

    Artifact decoded;
    ByteReader r(w.ptr(), w.size());
    bool ok = decodeCode(r, decoded);
    CHECK(ok, "decodeCode succeeds after encodeCode");
    CHECK(decoded.functions.size() == original.functions.size(),
          "decodeCode preserves function count");
    if (decoded.functions.size() == 1u) {
        CHECK(decoded.functions[0].is_variadic, "decodeCode preserves the variadic flag");
        CHECK(decoded.functions[0].name_id == 0, "decodeCode preserves the function name id");
    }
    CHECK_EQ(decoded.canonical_mappings.size(), 1u, "decodeCode preserves canonical mappings");
    if (decoded.canonical_mappings.size() == 1u) {
        CHECK_EQ(decoded.canonical_mappings[0].hi, 0x1122334455667788ULL,
                 "canonical mapping hi preserved");
        CHECK_EQ(decoded.canonical_mappings[0].lo, 0x99AABBCCDDEEFF00ULL,
                 "canonical mapping lo preserved");
        CHECK_EQ(decoded.canonical_mappings[0].runtime_id, 0xCAFEBABEu,
                 "canonical mapping runtime id preserved");
    }
}

static void test_modern_code_section_round_trip() {
    Artifact original = makeMinimalArtifact();
    addModernCode(original);

    ByteWriter w;
    CHECK(encodeCode(original, w), "encodeCode succeeds for modern variants");

    Artifact decoded;
    ByteReader r(w.ptr(), w.size());
    CHECK(decodeCode(r, decoded), "decodeCode succeeds for modern variants");
    CHECK_EQ(decoded.functions.size(), 1u, "function count preserved");
    if (decoded.functions.empty())
        return;
    CHECK(decoded.functions[0].is_state, "state flag preserved");
    CHECK(decoded.functions[0].uses_tailcc, "state tailcc flag preserved");
    CHECK_EQ(decoded.functions[0].machine_id, 7u, "state machine id preserved");
    CHECK_EQ(decoded.functions[0].machine_return_type_id, 0u, "machine return type preserved");
    CHECK_EQ(decoded.functions[0].param_slot_ids.size(), 1u,
             "function parameter slot mapping preserved");
    if (!decoded.functions[0].param_slot_ids.empty())
        CHECK_EQ(decoded.functions[0].param_slot_ids[0], 1u,
                 "function parameter slot value preserved");
    CHECK_EQ(decoded.exprs.size(), 7u, "modern expression pool preserved");
    if (decoded.exprs.size() == 7u) {
        CHECK(decoded.exprs[0].kind == CompactExprKind::Literal, "literal kind preserved");
        CHECK(decoded.exprs[1].kind == CompactExprKind::Cast, "cast kind preserved");
        CHECK_EQ(decoded.exprs[1].ref_e, 0u, "cast from-type preserved");
        CHECK(decoded.exprs[2].kind == CompactExprKind::Call, "indirect call kind preserved");
        CHECK_EQ(decoded.exprs[2].ref_e, 2u, "call fn_type preserved");
        CHECK(decoded.exprs[3].kind == CompactExprKind::MakeSlice, "slice range kind preserved");
        CHECK_EQ(decoded.exprs[3].ref_f, 0u, "slice bound type preserved");
        CHECK_EQ(decoded.exprs[3].flags, 3u, "slice flags preserved");
        CHECK(decoded.exprs[4].kind == CompactExprKind::LayoutIntrinsic,
              "layout intrinsic kind preserved");
        CHECK_EQ(decoded.exprs[4].ref_f, 2u, "layout field index preserved");
        CHECK(decoded.exprs[5].kind == CompactExprKind::StateTailCall,
              "state tail call kind preserved");
        CHECK_EQ(decoded.exprs[5].ref_b, 9u, "state tail direct callee symbol preserved");
        CHECK_EQ(decoded.exprs[5].arg_types.size(), 1u, "state tail call arg types preserved");
        CHECK(decoded.exprs[6].kind == CompactExprKind::CanonicalType,
              "canonical type kind preserved");
        CHECK_EQ(decoded.exprs[6].type_id, 0u, "canonical type id expression type preserved");
        CHECK_EQ(decoded.exprs[6].ints.size(), 2u, "canonical type id words preserved");
        CHECK_EQ(decoded.exprs[6].ints[0], 0x1122334455667788ULL,
                 "canonical type id high word preserved");
        CHECK_EQ(decoded.exprs[6].ints[1], 0x99AABBCCDDEEFF00ULL,
                 "canonical type id low word preserved");
    }
}

static void test_attrs_section_round_trip() {
    Artifact original = makeMinimalArtifact();
    addAttrs(original);

    ByteWriter w;
    CHECK(encodeAttrs(original, w), "encodeAttrs succeeds");

    Artifact decoded;
    ByteReader r(w.ptr(), w.size());
    CHECK(decodeAttrs(r, decoded), "decodeAttrs succeeds");
    CHECK_EQ(decoded.attrs_slots.size(), 1u, "slot attrs preserved");
    CHECK_EQ(decoded.attrs_calls.size(), 1u, "call attrs preserved");
    CHECK_EQ(decoded.attrs_fns.size(), 1u, "fn attrs preserved");
    if (decoded.attrs_calls.size() == 1u)
        CHECK_EQ(decoded.attrs_calls[0].arg_escapes.size(), 2u, "call escape count preserved");
}

static void test_instantiation_section_round_trip() {
    Artifact original = makeMinimalArtifact();
    addInstantiations(original);

    ByteWriter w;
    CHECK(encodeInstantiations(original, w), "encodeInstantiations succeeds");
    CHECK(w.size() > 0, "encodeInstantiations produces non-empty output");

    Artifact decoded;
    ByteReader r(w.ptr(), w.size());
    CHECK(decodeInstantiations(r, decoded), "decodeInstantiations succeeds after encode");
    CHECK_EQ(decoded.instantiations.size(), 2u, "instance count preserved");
    CHECK_EQ(decoded.instantiations[0].mangled, std::string("main.id<i32>"),
             "first mangled instance name preserved");
    CHECK_EQ(decoded.instantiations[0].arg_types.size(), 1u,
             "first instance type-argument count preserved");
    CHECK_EQ(decoded.instantiations[0].arg_types[0], std::string("i32"),
             "first instance type argument preserved");
    CHECK_EQ(decoded.instantiations[1].arg_types.size(), 2u,
             "second instance type-argument count preserved");
    CHECK_EQ(decoded.instantiations[1].arg_types[1], std::string("f64"),
             "second instance second type argument preserved");
}

// ── Debug section round-trip (payload empty, reader ignores) ──────

static void test_debug_section_round_trip() {
    Artifact art = makeMinimalArtifact();

    ByteWriter w;
    bool encoded = encodeDebug(art, w);
    CHECK(encoded, "encodeDebug succeeds");
    CHECK(w.size() == 0, "encodeDebug writes empty payload");

    Artifact decoded;
    ByteReader r(w.ptr(), w.size());
    bool ok = decodeDebug(r, decoded);
    CHECK(ok, "decodeDebug succeeds on empty payload");
}

// ── Full artifact round-trip (bytes stable) ───────────────────────

static void test_full_artifact_round_trip() {
    Artifact original = makeMinimalArtifact();
    addCode(original);

    ByteWriter w;
    uint32_t csum1 = Writer::write(original, w);
    CHECK(csum1 != 0, "Writer::write returns non-zero checksum");

    std::string bytes{reinterpret_cast<const char *>(w.ptr()), w.size()};
    auto round = Reader::read(bytes);
    CHECK(round.has_value(), "Reader::read decodes full artifact");
    CHECK(round->module_name == original.module_name, "Reader::read preserves module_name");
    CHECK(round->canonical_path == original.canonical_path,
          "Reader::read preserves canonical_path");
    CHECK(round->decls.size() == original.decls.size(), "Reader::read preserves decl count");
    CHECK(round->functions.size() == original.functions.size(),
          "Reader::read preserves function count");
    if (round->functions.size() == 1u)
        CHECK(round->functions[0].name == "main",
              "reader restores the function name from the string table");

    // deterministic: same input → same output
    ByteWriter w2;
    uint32_t csum2 = Writer::write(original, w2);
    CHECK(csum1 == csum2, "Writer::write is deterministic (same checksum)");
    CHECK(w.size() == w2.size(), "Writer::write is deterministic (same size)");
    CHECK(std::memcmp(w.ptr(), w2.ptr(), w.size()) == 0,
          "Writer::write is deterministic (same bytes)");
}

// ── Truncation rejection ──────────────────────────────────────────

static void test_truncated_artifact_rejected() {
    Artifact original = makeMinimalArtifact();
    addCode(original);

    ByteWriter w;
    (void)Writer::write(original, w);

    // truncate at various points
    for (size_t trunc = 1; trunc < w.size() && trunc < 64; ++trunc) {
        std::string partial{reinterpret_cast<const char *>(w.ptr()), trunc};
        auto result = Reader::read(partial);
        CHECK(!result.has_value(), "Reader::read rejects truncated artifact");
    }
}

// ── ByteReader truncation detection ───────────────────────────────

static void test_reader_truncation() {
    const uint8_t data[] = {0x01, 0x02};
    ByteReader r(data, sizeof(data));

    uint8_t u8 = 0;
    CHECK(r.readU8(u8) && u8 == 0x01, "readU8 on first byte");

    uint16_t u16 = 0;
    CHECK(!r.readU16(u16), "readU16 fails on truncation (1 byte remaining, need 2)");
    CHECK(r.remaining() == 1, "remaining unchanged after failed read");

    ByteReader empty{nullptr, 0};
    CHECK(empty.empty(), "empty() true for zero-length reader");
    uint32_t u32 = 0;
    CHECK(!empty.readU32(u32), "readU32 fails on empty reader");
}

// ── Corrupt count rejection ───────────────────────────────────────

static void test_corrupt_count_rejected() {
    // An absurd element count must be rejected before the decoder can resize
    // the destination with an untrusted allocation.
    ByteWriter w;
    w.writeU32(0x7FFFFFFFu);
    Artifact decoded;
    ByteReader r(w.ptr(), w.size());
    CHECK(!decodeDecls(r, decoded), "decodeDecls rejects an absurd count");
    CHECK(decoded.decls.empty(), "decodeDecls does not resize for an absurd count");
}

// ── Checksum verification ─────────────────────────────────────────

static void test_checksum_validation() {
    Artifact original = makeMinimalArtifact();
    addCode(original);

    ByteWriter w;
    (void)Writer::write(original, w);

    // corrupt a section payload byte (skip the header)
    std::vector<uint8_t> corrupted(w.data());
    size_t header_size = sizeof(FileHeader) + original.canonical_path.size();
    if (corrupted.size() > header_size) {
        corrupted[header_size] ^= 0xFFu; // flip bits in first section
        std::string bad{reinterpret_cast<const char *>(corrupted.data()), corrupted.size()};
        auto result = Reader::read(bad);
        CHECK(!result.has_value(), "Reader::read rejects artifact with corrupted payload");
    }
}

// ── Empty artifact edge case ──────────────────────────────────────

static void test_empty_artifact_round_trip() {
    Artifact empty_art;
    empty_art.canonical_path = "/test/empty.zith";
    empty_art.module_name    = "empty";
    empty_art.cache_key_hash = 0;

    ByteWriter w;
    (void)Writer::write(empty_art, w);
    CHECK(w.size() > 0, "Writer::write produces output even for empty artifact");

    std::string bytes{reinterpret_cast<const char *>(w.ptr()), w.size()};
    auto result = Reader::read(bytes);
    CHECK(result.has_value(), "Reader::read decodes empty artifact");
}

// ── All test aggregation ──────────────────────────────────────────

static void test_zirl_sections() {
    test_type_section_round_trip();
    test_decl_section_round_trip();
    test_code_section_round_trip();
    test_modern_code_section_round_trip();
    test_attrs_section_round_trip();
    test_instantiation_section_round_trip();
    test_debug_section_round_trip();
    test_full_artifact_round_trip();
    test_truncated_artifact_rejected();
    test_reader_truncation();
    test_corrupt_count_rejected();
    test_checksum_validation();
    test_empty_artifact_round_trip();
}

} // namespace

TEST_MAIN(zirl_sections)
