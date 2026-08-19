#pragma once

#include <cstdint>

namespace toolkit::sema {

enum class Err : std::uint32_t {
    UndefinedIdent = 2001,
    DuplicateDecl = 2002,
    NoMember = 2006,
    NoMatchingFn = 2007,
    AmbiguousCall = 2008,
    UnsupportedSyntax = 2010,
    TypeMismatch = 3001,
    CannotInfer = 3002,
    InvalidCast = 3003,
    WriteThroughView = 4004,
};

} // namespace toolkit::sema
