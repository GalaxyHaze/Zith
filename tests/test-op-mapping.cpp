#include "sema/op-mapping.hpp"
#include "test-common.hpp"

using namespace zith;

static void test_binary_mapping() {
    using hir::HirBinaryOp;

    CHECK_EQ(static_cast<int>(sema::mapBinaryOp("+")), static_cast<int>(HirBinaryOp::Add),
             "plus maps to add");
    CHECK_EQ(static_cast<int>(sema::mapBinaryOp("==")), static_cast<int>(HirBinaryOp::Eq),
             "equality maps to eq");
    CHECK_EQ(static_cast<int>(sema::mapBinaryOp("<=")), static_cast<int>(HirBinaryOp::Le),
             "less-or-equal maps to le");
    CHECK_EQ(static_cast<int>(sema::mapBinaryOp("and")), static_cast<int>(HirBinaryOp::And),
             "keyword and maps to logical and");
    CHECK_EQ(static_cast<int>(sema::mapBinaryOp("&.")), static_cast<int>(HirBinaryOp::And),
             "suffixed bitwise and maps to and");
    CHECK_EQ(static_cast<int>(sema::mapBinaryOp("<<")), static_cast<int>(HirBinaryOp::Shl),
             "shift left maps to shl");
}

static void test_unary_mapping() {
    using hir::HirUnaryOp;

    CHECK_EQ(static_cast<int>(sema::mapUnaryOp("-")), static_cast<int>(HirUnaryOp::Neg),
             "unary minus maps to neg");
    CHECK_EQ(static_cast<int>(sema::mapUnaryOp("not")), static_cast<int>(HirUnaryOp::Not),
             "keyword not maps to logical not");
    CHECK_EQ(static_cast<int>(sema::mapUnaryOp("~")), static_cast<int>(HirUnaryOp::BitNot),
             "bitwise not maps to bitnot");
    CHECK_EQ(static_cast<int>(sema::mapUnaryOp("&")), static_cast<int>(HirUnaryOp::Ref),
             "address maps to ref");
}

static void test_operator_predicates() {
    CHECK(sema::isComparisonOp(">="), ">= is a comparison");
    CHECK(!sema::isComparisonOp("+"), "+ is not a comparison");

    CHECK(sema::isArithmeticOp("%"), "% is arithmetic");
    CHECK(!sema::isArithmeticOp("=="), "equality is not arithmetic");

    CHECK(sema::isShiftOp(">>"), ">> is a shift");
    CHECK(!sema::isShiftOp("+"), "+ is not a shift");

    CHECK(sema::isBitwiseOp("^."), "^. is bitwise");
    CHECK(!sema::isBitwiseOp("1"), "a literal is not bitwise");
}

static void test_width_mapping() {
    CHECK_EQ(sema::mapIntegerWidth(32, true), types::IntWidth::I32, "32-bit signed maps to i32");
    CHECK_EQ(sema::mapIntegerWidth(16, false), types::IntWidth::U16, "16-bit unsigned maps to u16");
    CHECK_EQ(sema::mapIntegerWidth(128, true), types::IntWidth::I128,
             "128-bit signed maps to i128");
    CHECK_EQ(sema::mapFloatWidth(32), types::FloatWidth::F32, "32-bit float maps to f32");
    CHECK_EQ(sema::mapFloatWidth(64), types::FloatWidth::F64, "64-bit float maps to f64");
}

static void test_op_mapping() {
    test_binary_mapping();
    test_unary_mapping();
    test_operator_predicates();
    test_width_mapping();
}

TEST_MAIN(op_mapping)
