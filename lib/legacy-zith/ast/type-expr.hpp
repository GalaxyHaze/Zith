#pragma once

#include "common/ast-ids.hpp"
#include "memory/dyn-array.hpp"

#include <cstdint>
#include <string_view>
#include <variant>

namespace zith::ast {

enum class BuiltinType : uint8_t {
    I8,
    I16,
    I32,
    I64,
    I128,
    U8,
    U16,
    U32,
    U64,
    U128,
    F32,
    F64,
    Bool,
    Char,
    Void,
    Never,
    Unknown,
    Invalid,
    Opaque,
};

enum class OwnershipKw : uint8_t { Default, Unique, Share, Lend, View, Belong };

struct TypePath {
    memory::DynArray<std::string_view> segments;
    memory::Span span{};
};

struct TypeBuiltin {
    BuiltinType kind;
};

struct TypePtrExpr {
    TypeExprId pointee;
    bool is_mut           = false;
    OwnershipKw ownership = OwnershipKw::Default;
};

struct TypeSlice {
    TypeExprId elem;
};

struct TypeArray {
    TypeExprId elem;
    TypeExprId count;
    uint32_t count_val = 0;
};

struct TypeFnExpr {
    memory::DynArray<TypeExprId> params;
    TypeExprId ret;
};

struct TypeOptional {
    TypeExprId inner;
};

struct TypeFailable {
    TypeExprId inner;
    TypeExprId error;
};

struct TypeApp {
    TypeExprId base;
    memory::DynArray<TypeExprId> args;
};

struct TypePackMember {
    std::string_view name;
    TypeExprId type;
};

struct TypePack {
    memory::DynArray<TypePackMember> members;
};

struct TypeSum {
    memory::DynArray<TypeExprId> members;
};

struct TypeInfer {};

struct TypeGenericParamRef {
    std::string_view name;
};

struct TypeMut {
    TypeExprId inner;
};

struct TypeDyn {
    TypeExprId inner;
};

struct TypeUnion {
    // union type hint — compiler creates implicit union from context
};

struct TypeSpecialization {
    TypeExprId base; // kInvalidTypeExpr when standalone (<Args> hint)
    memory::DynArray<TypeExprId> args;
};

using TypeExprNode =
    std::variant<TypePath, TypeBuiltin, TypePtrExpr, TypeSlice, TypeArray, TypeFnExpr, TypeOptional,
                 TypeFailable, TypeApp, TypePack, TypeSum, TypeInfer, TypeGenericParamRef, TypeMut,
                 TypeDyn, TypeUnion, TypeSpecialization>;

} // namespace zith::ast
