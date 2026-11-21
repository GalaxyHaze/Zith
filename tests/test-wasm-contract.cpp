#include "capi/zithc-capi.h"
#include "wasm/abi-hir.hpp"
#include "test-common.hpp"

#include <cstring>

namespace {
constexpr const char *kValidSource = "fn main() {\n"
                                     "    let x: i32 = 1;\n"
                                     "}\n";

constexpr const char *kInvalidSource = "fn main() {\n"
                                       "    let x: MissingType = 1;\n"
                                       "}\n";

void test_hir_safety_round_trip() {
    zith::memory::Arena arena;
    zith::memory::StringInterner interner(arena);
    zith::types::TypeIntern types(arena, interner);
    zith::hir::HirModule module(arena);

    const auto i32 = types.internInt(zith::types::IntWidth::I32);
    const auto array_type = types.internArray(i32, 4);
    const auto generic_type = types.internGenericParam("module::Owner", 2);
    auto &fn = module.addFn(interner.intern("main"));
    fn.hasBody = true;
    fn.return_type = i32;
    fn.blocks.emplace(arena);
    zith::hir::HirLiteral object;
    object.type = array_type;
    const auto object_id = module.addExpr(object);
    zith::hir::HirLiteral index;
    index.type = i32;
    index.i = 1;
    const auto index_id = module.addExpr(index);

    const auto add_index = [&](const zith::hir::HirSafetyMode mode) {
        zith::hir::HirIndex expr;
        expr.object = object_id;
        expr.index = index_id;
        expr.type = i32;
        expr.obj_type = array_type;
        expr.is_array = true;
        expr.safety = mode;
        return module.addExpr(expr);
    };
    const auto required_id = add_index(zith::hir::HirSafetyMode::RequiredCheck);
    const auto guarded_id = add_index(zith::hir::HirSafetyMode::Guarded);
    const auto intrinsic_id = add_index(zith::hir::HirSafetyMode::Intrinsic);
    const auto raw_id = add_index(zith::hir::HirSafetyMode::Raw);
    fn.blocks[0].insts.push(required_id);
    fn.blocks[0].terminator = module.addExpr(zith::hir::HirRet{required_id});
    auto &declaration = module.addFn(interner.intern("external"));
    declaration.return_type = i32;

    auto &proof = module.attrs().safetyProof(required_id);
    proof.kind = zith::hir::HirSafetyProofKind::BoundsValid;
    proof.operand_a = object_id;
    proof.operand_b = index_id;
    proof.present = true;

    const auto encoded = zith::wasm::encodeHir(module, interner, types);
    CHECK(encoded.ok, "completed HIR with safety facts encodes to the WASM envelope");
    if (!encoded.ok)
        return;
    zith::wasm::DecodedHir decoded;
    CHECK(zith::wasm::decodeHir(encoded.blob.bytes, decoded),
          "WASM HIR envelope decodes safety modes and proof facts");
    if (!decoded.ok)
        return;

    const auto safety_of = [&](const zith::hir::HirExprId id) {
        const auto *expr = std::get_if<zith::hir::HirIndex>(&decoded.module.getExpr(id));
        CHECK(expr != nullptr, "round-trip preserves index expressions");
        return expr != nullptr ? expr->safety : zith::hir::HirSafetyMode::RequiredCheck;
    };
    CHECK(safety_of(required_id) == zith::hir::HirSafetyMode::RequiredCheck,
          "WASM round-trip preserves RequiredCheck");
    CHECK(safety_of(guarded_id) == zith::hir::HirSafetyMode::Guarded,
          "WASM round-trip preserves Guarded");
    CHECK(safety_of(intrinsic_id) == zith::hir::HirSafetyMode::Intrinsic,
          "WASM round-trip preserves Intrinsic");
    CHECK(safety_of(raw_id) == zith::hir::HirSafetyMode::Raw,
          "WASM round-trip preserves Raw");
    const auto *decoded_proof = decoded.module.attrs().trySafetyProof(required_id);
    CHECK(decoded_proof != nullptr && decoded_proof->present &&
              decoded_proof->kind == zith::hir::HirSafetyProofKind::BoundsValid &&
              decoded_proof->operand_a == object_id && decoded_proof->operand_b == index_id,
          "WASM round-trip preserves proof kind and exact operand identities");
    CHECK(decoded.module.getFn(0).hasBody,
          "WASM HIR round-trip preserves the function definition classification");
    const auto *decoded_generic =
        std::get_if<zith::types::TypeGenericParam>(&decoded.types.lookup(generic_type));
    CHECK(decoded_generic != nullptr &&
              decoded.interner.lookup(decoded_generic->owner_name) == "module::Owner" &&
              decoded_generic->param_index == 2,
          "WASM HIR round-trip preserves canonical generic parameter owner identity");
    CHECK(!decoded.module.getFn(1).hasBody,
          "WASM HIR round-trip preserves the function declaration classification");
}
} // namespace

void test_wasm_contract() {
    test_hir_safety_round_trip();

    zithc_session *session = zithc_session_create_from_buffer("playground.zithm", kValidSource,
                                                              std::strlen(kValidSource));
    CHECK(session != nullptr, "session creation succeeds");
    if (!session)
        return;

    zithc_session_set_mode(session, 0);
    zithc_session_set_opt_level(session, 0);
    zithc_session_set_emit_tokens(session, true);
    zithc_session_set_emit_flags(session, true, true, false, false);
    zithc_session_set_emit_extra_flags(session, true, true);
    const bool ok = zithc_run_to(session, ZITHC_STAGE_HIR_LOWERED);
    CHECK(ok, "valid source lowers to HIR");

    const char *output = zithc_session_flush_output(session);
    CHECK(output != nullptr, "buffered output is accessible");
    if (output) {
        CHECK(std::strlen(output) > 0, "buffered output is non-empty");
        CHECK(std::strstr(output, "--- CST ---") != nullptr, "CST is in the text output buffer");
        CHECK(std::strstr(output, "--- AST ---") != nullptr, "AST is in the text output buffer");
        CHECK(std::strstr(output, "--- HIR ---") != nullptr, "HIR is in the text output buffer");
        CHECK(std::strstr(output, "--- VIR ---") != nullptr, "VIR is in the text output buffer");
        CHECK(std::strstr(output, "Token") != nullptr, "CST contains token rows");
    }
    CHECK(zithc_diag_count(session) == 0, "valid source has no diagnostics");
    zithc_session_destroy(session);

    session = zithc_session_create_from_buffer("playground.zithm", kInvalidSource,
                                               std::strlen(kInvalidSource));
    if (!session)
        return;

    zithc_session_set_mode(session, 0);
    zithc_session_set_opt_level(session, 0);
    zithc_session_set_emit_flags(session, false, false, false, false);
    const bool bad = zithc_run_to(session, ZITHC_STAGE_HIR_LOWERED);
    CHECK(!bad, "invalid source fails HIR lowering");

    const size_t diag_count = zithc_diag_count(session);
    CHECK(diag_count > 0, "invalid source produces diagnostics");
    if (diag_count > 0) {
        const zithc_diagnostic diag = zithc_diag_get(session, 0);
        const char *rendered        = diag.message;
        CHECK(rendered != nullptr && rendered[0] != '\0', "diagnostic message is non-empty");
        CHECK(zithc_diag_get(session, diag_count).message == nullptr,
              "out-of-range diagnostic returns null message");
    }

    zithc_session_destroy(session);

    zithc_session *null_session = nullptr;
    CHECK(zithc_diag_count(null_session) == 0, "null session has no diagnostics");
    CHECK(zithc_diag_get(null_session, 0).message == nullptr, "null session diagnostic is null");
    CHECK(zithc_session_flush_output(null_session)[0] == '\0', "null session flush is empty");
}

TEST_MAIN(wasm_contract)
