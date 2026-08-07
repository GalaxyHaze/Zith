#include "op-mapping.hpp"

#include "sema/modern-types.hpp"
#include "types/type-kind.hpp"

namespace zith::sema {

hir::HirBinaryOp mapBinaryOp(std::string_view text) noexcept {
    if (text == "+")
        return hir::HirBinaryOp::Add;
    if (text == "-")
        return hir::HirBinaryOp::Sub;
    if (text == "*")
        return hir::HirBinaryOp::Mul;
    if (text == "/")
        return hir::HirBinaryOp::Div;
    if (text == "%")
        return hir::HirBinaryOp::Rem;
    if (text == "==")
        return hir::HirBinaryOp::Eq;
    if (text == "!=")
        return hir::HirBinaryOp::Ne;
    if (text == "<")
        return hir::HirBinaryOp::Lt;
    if (text == "<=")
        return hir::HirBinaryOp::Le;
    if (text == ">")
        return hir::HirBinaryOp::Gt;
    if (text == ">=")
        return hir::HirBinaryOp::Ge;
    // The keyword forms and the `.`-suffixed bitwise operators share one HIR op each:
    // `and`/`&.`, `or`/`|.`, `xor`/`^.`.
    if (text == "and" || text == "&.")
        return hir::HirBinaryOp::And;
    if (text == "or" || text == "|.")
        return hir::HirBinaryOp::Or;
    if (text == "xor" || text == "^.")
        return hir::HirBinaryOp::Xor;
    if (text == "<<")
        return hir::HirBinaryOp::Shl;
    if (text == ">>")
        return hir::HirBinaryOp::Shr;
    return hir::HirBinaryOp::Add;
}

hir::HirUnaryOp mapUnaryOp(std::string_view text) noexcept {
    if (text == "-")
        return hir::HirUnaryOp::Neg;
    if (text == "!" || text == "not")
        return hir::HirUnaryOp::Not;
    if (text == "&")
        return hir::HirUnaryOp::Ref;
    if (text == "*")
        return hir::HirUnaryOp::Deref;
    if (text == "~")
        return hir::HirUnaryOp::BitNot;
    return hir::HirUnaryOp::Neg;
}

bool isComparisonOp(std::string_view text) noexcept {
    return text == "==" || text == "!=" || text == "<" || text == ">" || text == "<=" ||
           text == ">=";
}

bool isArithmeticOp(std::string_view text) noexcept {
    return text == "+" || text == "-" || text == "*" || text == "/" || text == "%";
}

bool isShiftOp(std::string_view text) noexcept {
    return text == "<<" || text == ">>";
}

/// The base bitwise operators keep the `.` suffix of the spec grammar so they are
/// distinct from the address-of `&` and the attribute delimiter `|`.
bool isBitwiseOp(std::string_view text) noexcept {
    return text == "&." || text == "|." || text == "^.";
}

types::IntWidth mapIntegerWidth(uint8_t bits, bool is_signed) noexcept {
    switch (bits) {
    case 8:
        return is_signed ? types::IntWidth::I8 : types::IntWidth::U8;
    case 16:
        return is_signed ? types::IntWidth::I16 : types::IntWidth::U16;
    case 32:
        return is_signed ? types::IntWidth::I32 : types::IntWidth::U32;
    case 64:
        return is_signed ? types::IntWidth::I64 : types::IntWidth::U64;
    case 128:
        return is_signed ? types::IntWidth::I128 : types::IntWidth::U128;
    default:
        return types::IntWidth::Literal;
    }
}

types::FloatWidth mapFloatWidth(uint8_t bits) noexcept {
    if (bits <= 32)
        return types::FloatWidth::F32;
    if (bits <= 64)
        return types::FloatWidth::F64;
    return types::FloatWidth::F128;
}

} // namespace zith::sema
