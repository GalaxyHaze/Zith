#include "capi/zithc-capi.h"
#include "wasm/abi-hir.hpp"
#include "test-common.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
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

void test_hir_dyn_vtable_round_trip() {
    zith::memory::Arena arena;
    zith::memory::StringInterner interner(arena);
    zith::types::TypeIntern types(arena, interner);
    zith::hir::HirModule module(arena);
    const auto i32 = types.internInt(zith::types::IntWidth::I32);

    const auto first_type = types.defineStruct("First");
    types.addField(first_type, "base", i32, 8);
    const std::array first_args{i32};
    types.setTypeArgs(first_type, first_args);
    types.setDefiningModule(first_type, "wasm-dyn-test");
    types.setForeignLayout(first_type, 16, 8, false);
    types.setExplicitAlignment(first_type, 8);
    const auto second_type = types.defineStruct("Second");
    types.addField(second_type, "base", i32, 4);

    auto &first_fn        = module.addFn(interner.intern("First.value"));
    first_fn.return_type  = i32;
    auto &second_fn       = module.addFn(interner.intern("Second.value"));
    second_fn.return_type = i32;

    const auto first_name = interner.intern("Biased::First");
    auto &first_vtable    = module.addVTable(first_name);
    first_vtable.slots.push(0);
    const auto second_name = interner.intern("Biased::Second");
    auto &second_vtable    = module.addVTable(second_name);
    second_vtable.slots.push(1);

    zith::hir::HirLiteral value;
    value.type          = i32;
    const auto value_id = module.addExpr(value);

    zith::hir::HirMakeDyn first_make;
    first_make.value         = value_id;
    first_make.source_type   = first_type;
    first_make.dyn_type      = i32;
    first_make.vtable_name   = first_name;
    const auto first_make_id = module.addExpr(first_make);

    zith::hir::HirDynCall first_call(arena);
    first_call.receiver      = first_make_id;
    first_call.vtable_name   = first_name;
    first_call.result_type   = i32;
    const auto first_call_id = module.addExpr(std::move(first_call));

    zith::hir::HirMakeDyn second_make;
    second_make.value         = value_id;
    second_make.source_type   = second_type;
    second_make.dyn_type      = i32;
    second_make.vtable_name   = second_name;
    const auto second_make_id = module.addExpr(second_make);

    zith::hir::HirDynCall second_call(arena);
    second_call.receiver      = second_make_id;
    second_call.vtable_name   = second_name;
    second_call.result_type   = i32;
    const auto second_call_id = module.addExpr(std::move(second_call));

    const auto encoded = zith::wasm::encodeHir(module, interner, types);
    CHECK(encoded.ok, "HIR with dyn calls and vtables encodes");
    if (!encoded.ok)
        return;

    zith::wasm::DecodedHir decoded;
    CHECK(zith::wasm::decodeHir(encoded.blob.bytes, decoded),
          "flat HIR with dyn calls and vtables decodes");
    if (!decoded.ok)
        return;

    CHECK_EQ(decoded.module.getVTableCount(), 2U, "flat HIR preserves both vtable rows");
    if (decoded.module.getVTableCount() == 2U) {
        const auto &decoded_first  = decoded.module.getVTable(0);
        const auto &decoded_second = decoded.module.getVTable(1);
        CHECK(decoded.interner.lookup(decoded_first.name) == "Biased::First",
              "flat HIR preserves the first vtable name");
        CHECK_EQ(decoded_first.slots.size(), 1U, "flat HIR preserves the first slot count");
        CHECK_EQ(decoded_first.slots[0], 0U, "flat HIR preserves the first function slot");
        CHECK(decoded.interner.lookup(decoded_second.name) == "Biased::Second",
              "flat HIR preserves the second vtable name");
        CHECK_EQ(decoded_second.slots.size(), 1U, "flat HIR preserves the second slot count");
        CHECK_EQ(decoded_second.slots[0], 1U, "flat HIR preserves the second function slot");
    }

    const auto *decoded_first_type =
        std::get_if<zith::types::TypeStruct>(&decoded.types.lookup(first_type));
    CHECK(decoded_first_type != nullptr, "flat HIR preserves the first struct type row");
    const auto *decoded_first_def = decoded_first_type != nullptr
                                        ? decoded.types.lookupStructDef(decoded_first_type->def_id)
                                        : nullptr;
    CHECK(decoded_first_def != nullptr, "flat HIR restores the first struct definition");
    if (decoded_first_def != nullptr && decoded_first_def->fields.size() == 1U) {
        CHECK(decoded.interner.lookup(decoded_first_def->name) == "First",
              "flat HIR preserves the first struct name");
        CHECK(decoded.interner.lookup(decoded_first_def->fields[0].name) == "base",
              "flat HIR preserves the first struct field name");
        CHECK_EQ(decoded_first_def->fields[0].type, i32,
                 "flat HIR preserves the first struct field type");
        CHECK_EQ(decoded_first_def->fields[0].foreignOffsetBytes, 8U,
                 "flat HIR preserves the first struct field offset");
        CHECK_EQ(decoded_first_def->args.size(), 1U,
                 "flat HIR preserves the first struct type-argument count");
        if (decoded_first_def->args.size() == 1U)
            CHECK_EQ(decoded_first_def->args[0], i32,
                     "flat HIR preserves the first struct type argument");
        CHECK(decoded_first_def->defining_module == "wasm-dyn-test",
              "flat HIR preserves the defining module name");
        CHECK(decoded_first_def->hasForeignLayout,
              "flat HIR preserves the validated foreign-layout flag");
        CHECK(!decoded_first_def->foreignAbiIsSingleI64,
              "flat HIR preserves the foreign ABI shape");
        CHECK_EQ(decoded_first_def->foreignSizeBytes, 16U,
                 "flat HIR preserves the foreign struct size");
        CHECK_EQ(decoded_first_def->foreignAlignBytes, 8U,
                 "flat HIR preserves the foreign struct alignment");
        CHECK_EQ(decoded_first_def->explicitAlignmentBytes, 8U,
                 "flat HIR preserves the explicit struct alignment");
    }

    const auto *decoded_second_type =
        std::get_if<zith::types::TypeStruct>(&decoded.types.lookup(second_type));
    CHECK(decoded_second_type != nullptr, "flat HIR preserves the second struct type row");
    const auto *decoded_second_def =
        decoded_second_type != nullptr ? decoded.types.lookupStructDef(decoded_second_type->def_id)
                                       : nullptr;
    CHECK(decoded_second_def != nullptr, "flat HIR restores the second struct definition");
    if (decoded_second_def != nullptr && decoded_second_def->fields.size() == 1U) {
        CHECK(decoded.interner.lookup(decoded_second_def->name) == "Second",
              "flat HIR preserves the second struct name");
        CHECK(decoded.interner.lookup(decoded_second_def->fields[0].name) == "base",
              "flat HIR preserves the second struct field name");
        CHECK_EQ(decoded_second_def->fields[0].type, i32,
                 "flat HIR preserves the second struct field type");
        CHECK_EQ(decoded_second_def->fields[0].foreignOffsetBytes, 4U,
                 "flat HIR preserves the second struct field offset");
    }

    const auto *decoded_first_make =
        std::get_if<zith::hir::HirMakeDyn>(&decoded.module.getExpr(first_make_id));
    CHECK(decoded_first_make != nullptr &&
              decoded.interner.lookup(decoded_first_make->vtable_name) == "Biased::First",
          "flat HIR preserves the first make-dyn vtable reference");
    const auto *decoded_first_call =
        std::get_if<zith::hir::HirDynCall>(&decoded.module.getExpr(first_call_id));
    CHECK(decoded_first_call != nullptr &&
              decoded.interner.lookup(decoded_first_call->vtable_name) == "Biased::First",
          "flat HIR preserves the first dyn-call vtable reference");
    const auto *decoded_second_make =
        std::get_if<zith::hir::HirMakeDyn>(&decoded.module.getExpr(second_make_id));
    CHECK(decoded_second_make != nullptr &&
              decoded.interner.lookup(decoded_second_make->vtable_name) == "Biased::Second",
          "flat HIR preserves the second make-dyn vtable reference");
    const auto *decoded_second_call =
        std::get_if<zith::hir::HirDynCall>(&decoded.module.getExpr(second_call_id));
    CHECK(decoded_second_call != nullptr &&
              decoded.interner.lookup(decoded_second_call->vtable_name) == "Biased::Second",
          "flat HIR preserves the second dyn-call vtable reference");

    auto version_six_blob = encoded.blob.bytes;
    version_six_blob[4]   = 6;
    version_six_blob[5]   = 0;
    version_six_blob[6]   = 0;
    version_six_blob[7]   = 0;
    zith::wasm::DecodedHir version_six;
    CHECK(!zith::wasm::decodeHir(version_six_blob, version_six),
          "flat HIR decoder rejects version 6 blobs");
    CHECK(version_six.message == "invalid flat HIR header",
          "old flat HIR version reports an invalid header");

    auto truncated_struct_blob = encoded.blob.bytes;
    truncated_struct_blob.pop_back();
    zith::wasm::DecodedHir truncated_struct;
    CHECK(!zith::wasm::decodeHir(truncated_struct_blob, truncated_struct),
          "flat HIR decoder rejects a truncated struct-definition row");

    auto oversized_vtable_blob = encoded.blob.bytes;
    const auto read_u32        = [&oversized_vtable_blob](const std::size_t offset) {
        return static_cast<uint32_t>(oversized_vtable_blob[offset]) |
               (static_cast<uint32_t>(oversized_vtable_blob[offset + 1U]) << 8U) |
               (static_cast<uint32_t>(oversized_vtable_blob[offset + 2U]) << 16U) |
               (static_cast<uint32_t>(oversized_vtable_blob[offset + 3U]) << 24U);
    };
    std::size_t slot_count_offset = 0;
    bool found_vtable_section     = false;
    for (std::size_t offset = 0; offset + 29U <= oversized_vtable_blob.size(); ++offset) {
        if (oversized_vtable_blob[offset] == 6U && read_u32(offset + 1U) == 2U &&
            read_u32(offset + 5U) == first_name && read_u32(offset + 9U) == 1U &&
            read_u32(offset + 13U) == 0U && read_u32(offset + 17U) == second_name &&
            read_u32(offset + 21U) == 1U && read_u32(offset + 25U) == 1U) {
            slot_count_offset    = offset + 21U;
            found_vtable_section = true;
            break;
        }
    }
    CHECK(found_vtable_section, "flat HIR vtable rows are locatable in the encoded blob");
    if (!found_vtable_section)
        return;
    oversized_vtable_blob[slot_count_offset]      = 0xFF;
    oversized_vtable_blob[slot_count_offset + 1U] = 0xFF;
    oversized_vtable_blob[slot_count_offset + 2U] = 0xFF;
    oversized_vtable_blob[slot_count_offset + 3U] = 0xFF;
    zith::wasm::DecodedHir oversized_vtable;
    CHECK(!zith::wasm::decodeHir(oversized_vtable_blob, oversized_vtable),
          "flat HIR decoder rejects an oversized vtable slot count");
    CHECK(oversized_vtable.message == "flat HIR vtable slot count overflow",
          "oversized vtable slot count reports the rejected field");
}
} // namespace

void test_wasm_contract() {
    test_hir_safety_round_trip();
    test_hir_dyn_vtable_round_trip();

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
