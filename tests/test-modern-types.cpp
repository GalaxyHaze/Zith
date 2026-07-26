#include "sema/modern-types.hpp"
#include "test-common.hpp"

using namespace zith;

static void test_primitive_types() {
    memory::Arena arena;
    sema::modern::TypeTable table(arena);

    auto i32 = table.internInteger({32, true});
    CHECK(i32, "integer type has a valid id");
    CHECK_EQ(table.kindOf(i32), sema::modern::TypeKind::Integer, "i32 is an integer type");
    const auto *int_data = table.integer(i32);
    CHECK(int_data != nullptr, "integer data is retrievable");
    CHECK_EQ(int_data->bits, 32u, "i32 has 32 bits");
    CHECK(int_data->isSigned, "i32 is signed");

    auto f64 = table.internFloat({64});
    CHECK_EQ(table.kindOf(f64), sema::modern::TypeKind::Float, "f64 is a float type");
    const auto *float_data = table.float_kind(f64);
    CHECK(float_data != nullptr, "float data is retrievable");
    CHECK_EQ(float_data->bits, 64u, "f64 has 64 bits");

    auto ptr = table.internPointer(i32);
    CHECK_EQ(table.kindOf(ptr), sema::modern::TypeKind::Pointer, "pointer type detected");
    const auto *ptr_data = table.pointer(ptr);
    CHECK(ptr_data != nullptr, "pointer data is retrievable");
    CHECK_EQ(ptr_data->pointee, i32, "pointer pointee is correct");

    auto opt = table.internOptional(i32);
    CHECK_EQ(table.kindOf(opt), sema::modern::TypeKind::Optional, "optional type detected");
}

static void test_function_and_struct() {
    memory::Arena arena;
    sema::modern::TypeTable table(arena);

    auto i32 = table.internInteger({32, true});
    auto f64 = table.internFloat({64});

    memory::DynArray<sema::modern::TypeId> params(arena);
    params.push(i32);
    params.push(f64);
    auto fn = table.internFunction(params, i32);
    CHECK_EQ(table.kindOf(fn), sema::modern::TypeKind::Function, "function type detected");
    const auto *fn_data = table.function(fn);
    CHECK(fn_data != nullptr, "function data is retrievable");
    CHECK_EQ(fn_data->result, i32, "function result type is i32");
    CHECK_EQ(fn_data->params.size(), 2u, "function has two params");

    memory::DynArray<sema::modern::TypeId> fields(arena);
    fields.push(i32);
    fields.push(f64);
    auto struct_ty = table.internStruct("Point", fields);
    CHECK_EQ(table.kindOf(struct_ty), sema::modern::TypeKind::Struct, "struct type detected");
    const auto *struct_data = table.struct_type(struct_ty);
    CHECK(struct_data != nullptr, "struct data is retrievable");
    CHECK_EQ(struct_data->name, std::string_view("Point"), "struct name is preserved");
    CHECK_EQ(struct_data->fields.size(), 2u, "struct has two fields");
}

static void test_identity_is_monotonic() {
    memory::Arena arena;
    sema::modern::TypeTable table(arena);

    auto a = table.internInteger({32, true});
    auto b = table.internInteger({64, true});
    CHECK(a.intern_seq < b.intern_seq, "ids are monotonic");
    CHECK(a != b, "different types get different ids");
}

static void test_modern_types() {
    test_primitive_types();
    test_function_and_struct();
    test_identity_is_monotonic();
}

TEST_MAIN(modern_types)
