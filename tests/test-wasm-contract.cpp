#include "capi/zithc-capi.h"
#include "test-common.hpp"

#include <cstring>

namespace {
constexpr const char *kValidSource =
    "fn main() {\n"
    "}\n";

constexpr const char *kInvalidSource =
    "fn main() {\n"
    "    let x: MissingType = 1;\n"
    "}\n";
} // namespace

void test_wasm_contract() {
    zithc_session *session = zithc_session_create_from_buffer("playground.zith", kValidSource,
                                                              std::strlen(kValidSource));
    CHECK(session != nullptr, "session creation succeeds");
    if (!session)
        return;

    zithc_session_set_mode(session, 0);
    zithc_session_set_opt_level(session, 0);
    zithc_session_set_emit_tokens(session, true);
    zithc_session_set_emit_flags(session, false, true, false, false);
    const bool ok = zithc_run_to(session, ZITHC_STAGE_HIR_LOWERED);
    CHECK(ok, "valid source lowers to HIR");

    const char *output = zithc_session_flush_output(session);
    CHECK(output != nullptr, "buffered output is accessible");
    if (output)
        CHECK(std::strlen(output) > 0, "buffered output is non-empty");
    CHECK(zithc_diag_count(session) == 0, "valid source has no diagnostics");
    zithc_session_destroy(session);

    session = zithc_session_create_from_buffer("playground.zith", kInvalidSource,
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
        const char *rendered = diag.message;
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
