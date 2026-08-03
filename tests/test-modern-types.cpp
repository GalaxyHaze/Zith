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

static void test_named_registry() {
    memory::Arena arena;
    sema::modern::TypeTable table(arena);

    auto i32 = table.internInteger({32, true});
    table.registerNamed("i32", i32);
    CHECK_EQ(table.lookupNamed("i32"), i32, "named lookup returns registered type");
    CHECK(!table.lookupNamed("missing"), "missing named type is invalid");
    auto created = table.findOrCreateNamed("u32", sema::modern::TypeKind::Unknown);
    CHECK(created, "findOrCreateNamed returns a valid id");
    CHECK_EQ(table.lookupNamed("u32"), created, "findOrCreateNamed registers on miss");
}

static void test_enum_union_trait_alias() {
    memory::Arena arena;
    sema::modern::TypeTable table(arena);

    auto i32            = table.internInteger({32, true});
    auto &variant_names = table.makeStringStorage();
    auto &discs         = table.makeDiscStorage();
    variant_names.push("Red");
    variant_names.push("Green");
    discs.push(0);
    discs.push(1);
    auto en = table.internEnum("Color", i32, variant_names, discs);
    CHECK_EQ(table.kindOf(en), sema::modern::TypeKind::Enum, "enum type detected");
    const auto *en_data = table.enum_type(en);
    CHECK(en_data != nullptr, "enum data is retrievable");
    CHECK_EQ(en_data->name, std::string_view("Color"), "enum name preserved");
    CHECK_EQ(en_data->underlying, i32, "enum underlying type preserved");
    CHECK_EQ(en_data->variant_names.size(), 2u, "enum variant names preserved");
    CHECK_EQ(en_data->discriminants[1], 1, "enum discriminants preserved");

    memory::DynArray<sema::modern::TypeId> members(arena);
    members.push(i32);
    auto un = table.internUnion("Value", members);
    CHECK_EQ(table.kindOf(un), sema::modern::TypeKind::Union, "union type detected");
    const auto *un_data = table.union_type(un);
    CHECK(un_data != nullptr, "union data is retrievable");
    CHECK_EQ(un_data->name, std::string_view("Value"), "union name preserved");

    auto tr = table.internTrait("Drawable");
    CHECK_EQ(table.kindOf(tr), sema::modern::TypeKind::Trait, "trait type detected");
    CHECK_EQ(table.trait(tr)->name, std::string_view("Drawable"), "trait name preserved");

    auto alias = table.internAlias(i32);
    CHECK_EQ(table.kindOf(alias), sema::modern::TypeKind::Alias, "alias type detected");
    CHECK_EQ(table.alias(alias)->target, i32, "alias target is correct");
}

static void test_composite_variants() {
    memory::Arena arena;
    sema::modern::TypeTable table(arena);

    auto i32 = table.internInteger({32, true});
    auto f64 = table.internFloat({64});

    memory::DynArray<sema::modern::TypeId> sum_members(arena);
    sum_members.push(i32);
    sum_members.push(f64);
    auto sum = table.internSum(sum_members);
    CHECK_EQ(table.kindOf(sum), sema::modern::TypeKind::Sum, "sum type detected");
    CHECK_EQ(table.sum(sum)->members.size(), 2u, "sum has two members");

    auto slice = table.internSlice(i32);
    CHECK_EQ(table.kindOf(slice), sema::modern::TypeKind::Slice, "slice type detected");
    CHECK_EQ(table.slice(slice)->element, i32, "slice element is correct");

    auto fail = table.internFailable(i32);
    CHECK_EQ(table.kindOf(fail), sema::modern::TypeKind::Failable, "failable type detected");
    CHECK_EQ(table.failable(fail)->inner, i32, "failable inner is correct");

    memory::DynArray<sema::modern::TypeId> pack_members(arena);
    pack_members.push(i32);
    pack_members.push(f64);
    memory::DynArray<std::string_view> pack_names(arena);
    pack_names.push("x");
    pack_names.push("y");
    auto pack = table.internPack(pack_members, pack_names);
    CHECK_EQ(table.kindOf(pack), sema::modern::TypeKind::Pack, "pack type detected");
    CHECK_EQ(table.pack(pack)->members.size(), 2u, "pack has two members");
    CHECK_EQ(table.pack(pack)->names.size(), 2u, "pack has two names");

    auto var = table.internTypeVar();
    CHECK_EQ(table.kindOf(var), sema::modern::TypeKind::TypeVar, "type variable detected");
    CHECK(table.type_var(var) != nullptr, "type variable data is retrievable");

    auto unknown = table.internUnknown();
    CHECK_EQ(table.kindOf(unknown), sema::modern::TypeKind::Unknown, "unknown type detected");

    memory::DynArray<sema::modern::TypeId> args(arena);
    args.push(i32);
    auto incomplete = table.internIncomplete(fail, args);
    CHECK_EQ(table.kindOf(incomplete), sema::modern::TypeKind::Incomplete,
             "incomplete type detected");
    CHECK_EQ(table.incomplete(incomplete)->base, fail, "incomplete base is correct");
    CHECK_EQ(table.incomplete(incomplete)->args.size(), 1u, "incomplete has one arg");
}

static void test_modern_types() {
    test_primitive_types();
    test_function_and_struct();
    test_identity_is_monotonic();
    test_named_registry();
    test_enum_union_trait_alias();
    test_composite_variants();
}

TEST_MAIN(modern_types)
