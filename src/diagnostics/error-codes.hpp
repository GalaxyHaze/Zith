#pragma once

#include "memory/optional.hpp"
#include <cstdint>
#include <string_view>

namespace zith::diagnostics {

using ErrCode = uint32_t;

namespace err {
// Lexical (1-999)
// DAMN I forget how weird sometimes C++ is
inline constexpr ErrCode const UnknownToken      = 1;
inline constexpr ErrCode const UnclosedString    = 2;
inline constexpr ErrCode const InvalidEscape     = 3;
inline constexpr ErrCode const InvalidIntLiteral = 4;
inline constexpr ErrCode const UnclosedComment   = 5;
// Know let me thik what to add

// Parse (1001-1999)
inline constexpr ErrCode const ExpectedExpr          = 1001;
inline constexpr ErrCode const ExpectedSemicolon     = 1002;
inline constexpr ErrCode const UnclosedParen         = 1003;
inline constexpr ErrCode const ExpectedIdent         = 1004;
inline constexpr ErrCode const InvalidImportDepth    = 1005;
inline constexpr ErrCode const ImportError           = 1006;
inline constexpr ErrCode const TopLevelLetNotAllowed = 1007;
inline constexpr ErrCode const DeprecatedSyntax      = 1008;

// Semantic (2001-2999)
inline constexpr ErrCode const UndefinedIdent    = 2001;
inline constexpr ErrCode const DuplicateDecl     = 2002;
inline constexpr ErrCode const WrongArity        = 2003;
inline constexpr ErrCode const UnusedDecl        = 2004;
inline constexpr ErrCode const NotNamespace      = 2005;
inline constexpr ErrCode const NoMember          = 2006;
inline constexpr ErrCode const NoMatchingFn      = 2007;
inline constexpr ErrCode const AmbiguousCall     = 2008;
inline constexpr ErrCode const NotImplemented    = 2009;
inline constexpr ErrCode const UnsupportedSyntax = 2010;

// Macro (2011-2020)
inline constexpr ErrCode const MacroUnknown              = 2011;
inline constexpr ErrCode const MacroArity                = 2012;
inline constexpr ErrCode const MacroArgKind              = 2013;
inline constexpr ErrCode const MacroRecursion            = 2014;
inline constexpr ErrCode const MacroDuplicate            = 2015;
inline constexpr ErrCode const MacroRawValue             = 2016;
inline constexpr ErrCode const MacroTagValue             = 2017;
inline constexpr ErrCode const MacroTagMismatch          = 2018;
inline constexpr ErrCode const MacroAttrUnknown          = 2019;
inline constexpr ErrCode const MacroAttrNotAllowed       = 2020;
inline constexpr ErrCode const NotATrait                 = 2023;
inline constexpr ErrCode const InterfaceMethodNotAllowed = 2025;

// Types (3001-3999)
inline constexpr ErrCode const TypeMismatch       = 3001;
inline constexpr ErrCode const CannotInfer        = 3002;
inline constexpr ErrCode const InvalidCast        = 3003;
inline constexpr ErrCode const CyclicType         = 3004;
inline constexpr ErrCode const NullDerefUnproven  = 3005;
inline constexpr ErrCode const CoercionFailure    = 3006;
inline constexpr ErrCode const WidthMismatch      = 3007;
inline constexpr ErrCode const OptionalViolation  = 3008;
inline constexpr ErrCode const GenericArity       = 3010;
inline constexpr ErrCode const GenericCannotInfer = 3011;
inline constexpr ErrCode const GenericExplosion   = 3012;
inline constexpr ErrCode const GenericStructInfer = 3013;

// NRA / Ownership (4001-4999)
inline constexpr ErrCode const UseAfterMove     = 4001;
inline constexpr ErrCode const BorrowConflict   = 4002;
inline constexpr ErrCode const DoubleBorrow     = 4003;
inline constexpr ErrCode const WriteThroughView = 4004;

// MIR / Lowering (5001-5999)
inline constexpr ErrCode const InvalidIR   = 5001;
inline constexpr ErrCode const Unreachable = 5002;

// Runtime (10001+)
inline constexpr ErrCode const IndexOutOfBounds = 10001;
inline constexpr ErrCode const DivisionByZero   = 10002;
inline constexpr ErrCode const NullDeref        = 10003;
inline constexpr ErrCode const Panic            = 10004;
} // namespace err

struct ErrorInfo {
    ErrCode code;
    char prefix;
    std::string_view category;
    std::string_view title;
    std::string_view note;
    std::string_view why;
};

memory::Optional<ErrorInfo> lookupError(ErrCode code) noexcept;

} // namespace zith::diagnostics
