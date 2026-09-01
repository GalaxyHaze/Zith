#include "memory/arena.hpp"
#include "memory/dyn-array.hpp"
#include "memory/flat-map.hpp"
#include "memory/flat-set.hpp"
#include "memory/result.hpp"
#include "test-common.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

using namespace zith;

namespace {

struct Tracked {
    static inline int constructed = 0;
    static inline int destroyed   = 0;

    Tracked() {
        ++constructed;
    }
    ~Tracked() {
        ++destroyed;
    }

    static void reset() {
        constructed = 0;
        destroyed   = 0;
    }
};

struct alignas(64) Aligned {
    uint8_t data[64]{};
};

struct CustomKey {
    uint32_t left  = 0;
    uint32_t right = 0;

    friend constexpr bool operator==(CustomKey, CustomKey) noexcept = default;
};

struct CustomKeyHash {
    size_t operator()(CustomKey key) const noexcept {
        return std::hash<uint32_t>{}(key.left) ^ (std::hash<uint32_t>{}(key.right) << 1U);
    }
};

void test_arena_destroys_owned_objects() {
    Tracked::reset();
    {
        memory::Arena arena;
        CHECK(arena.make<Tracked>() != nullptr, "arena creates tracked object");
        CHECK_EQ(Tracked::constructed, 1, "tracked object constructed once");
        CHECK_EQ(Tracked::destroyed, 0, "tracked object remains alive before arena destruction");
    }
    CHECK_EQ(Tracked::destroyed, 1, "arena destroys objects created with make");
}

void test_arena_markpoint_rolls_back_destructors() {
    Tracked::reset();
    memory::Arena arena;
    CHECK(arena.make<Tracked>() != nullptr, "object before mark is created");
    {
        memory::MarkPoint mark(arena);
        CHECK(arena.make<Tracked>() != nullptr, "first marked object is created");
        CHECK(arena.make<Tracked>() != nullptr, "second marked object is created");
    }
    CHECK_EQ(Tracked::destroyed, 2, "rollback destroys objects created after mark");
    arena.clear();
    CHECK_EQ(Tracked::destroyed, 3, "clear preserves objects before mark until final cleanup");
}

void test_arena_alignment_and_overflow() {
    memory::Arena arena;
    auto *aligned = arena.make<Aligned>();
    CHECK(aligned != nullptr, "over-aligned allocation succeeds");
    CHECK(reinterpret_cast<uintptr_t>(aligned) % alignof(Aligned) == 0,
          "arena preserves requested alignment");
    CHECK(arena.alloc(8, 3) == nullptr, "non-power-of-two alignment is rejected");
    CHECK(arena.alloc(std::numeric_limits<size_t>::max()) == nullptr,
          "overflowing allocation is rejected");
}

void test_dynarray_moves_nontrivial_elements() {
    memory::Arena arena;
    memory::DynArray<std::string> values(arena);
    for (int index = 0; index < 64; ++index)
        values.push("value-" + std::to_string(index));

    CHECK_EQ(values.size(), 64u, "DynArray grows through multiple reallocations");
    for (int index = 0; index < 64; ++index)
        CHECK_EQ(values[static_cast<size_t>(index)], "value-" + std::to_string(index),
                 "DynArray preserves moved elements");
}

void test_dynarray_resize_and_at() {
    memory::Arena arena;
    memory::DynArray<int> values(arena);

    values.resize(3);
    CHECK_EQ(values.size(), 3u, "resize grows to the requested size");
    CHECK_EQ(values[1], 0, "resize default-constructs new elements");

    values.resize(5, 7);
    CHECK_EQ(values.size(), 5u, "resize with value grows to the requested size");
    CHECK_EQ(values[3], 7, "resize with value fills appended elements");
    CHECK_EQ(values[4], 7, "resize with value fills all appended elements");

    values.resize(2);
    CHECK_EQ(values.size(), 2u, "resize can shrink");
    CHECK_EQ(values.at(1), 0, "at accesses an in-bounds element");
    CHECK_EQ(values[0], 0, "shrinking preserves the first element");
}

void test_dynarray_append_range() {
    memory::Arena arena;
    memory::DynArray<int> values(arena);
    const int source[] = {11, 22, 33};

    values.appendRange(source, 3);
    CHECK_EQ(values.size(), 3u, "appendRange appends pointer ranges");
    CHECK_EQ(values[0], 11, "appendRange copies the first element");
    CHECK_EQ(values[2], 33, "appendRange copies the last element");

    values.appendRange(std::span<const int>(source, 2));
    CHECK_EQ(values.size(), 5u, "appendRange appends span ranges");
    CHECK_EQ(values[3], 11, "span append starts after existing values");
    CHECK_EQ(values[4], 22, "span append copies values in order");
}

void test_result_void_and_movable_chains() {
    memory::Result<void> ok;
    auto mapped = ok.map([] { return std::string{"ready"}; });
    CHECK(mapped.isOk(), "Result<void>::map returns a value result");
    CHECK_EQ(mapped.value(), "ready", "Result<void>::map preserves result value");

    memory::Result<std::string> movable(std::string{"frontend"});
    auto chained =
        std::move(movable)
            .map([](std::string &&value) { return value + "-snapshot"; })
            .andThen([](std::string &&value) -> memory::Result<size_t> { return value.size(); });
    CHECK(chained.isOk(), "movable Result map and andThen chain succeeds");
    CHECK_EQ(chained.value(), 17u, "movable chain passes ownership to the next stage");

    memory::Result<void> failed(memory::Error{"io failure"});
    auto propagated = failed.andThen([]() -> memory::Result<int> { return 1; });
    CHECK(propagated.isError(), "Result<void>::andThen propagates errors");
    CHECK_EQ(propagated.error().msg, "io failure", "propagated error remains structured");
}

void test_flat_set_insert_contains_erase() {
    memory::FlatSet<int> values;
    CHECK(values.empty(), "FlatSet starts empty");
    CHECK(values.insert(1), "FlatSet::insert reports a new element");
    CHECK(!values.insert(1), "FlatSet::insert reports an existing element");
    CHECK(values.insert(2), "FlatSet::insert another element");
    CHECK_EQ(values.size(), 2u, "FlatSet keeps distinct elements");
    CHECK(values.contains(1), "FlatSet::contains finds an inserted element");
    CHECK(values.contains(2), "FlatSet::contains finds the second element");
    values.erase(1);
    CHECK(!values.contains(1), "FlatSet::erase removes an element");
    CHECK(values.contains(2), "FlatSet::erase keeps other elements");
    CHECK_EQ(values.size(), 1u, "FlatSet size tracks erasures");

    values.clear();
    CHECK(values.empty(), "FlatSet::clear removes all elements");
    CHECK(!values.contains(2), "FlatSet::clear removes stored keys");
}

void test_flat_set_string_and_iteration() {
    memory::FlatSet<std::string> names;
    names.insert("a");
    names.insert("b");
    names.insert("a");
    CHECK_EQ(names.size(), 2u, "FlatSet deduplicates string keys");
    CHECK(names.contains("b"), "FlatSet lookup supports string_view keys");

    size_t seen = 0;
    for (const auto &name : names)
        seen += name == "a" || name == "b";
    CHECK_EQ(seen, 2u, "FlatSet iterates all stored keys");
}

void test_flat_map_custom_key_hash() {
    memory::FlatMap<CustomKey, std::string, CustomKeyHash> values;
    const CustomKey first{1, 2};
    const CustomKey second{3, 4};
    values[first]     = "one";
    values[second]    = "two";
    const auto *found = values.get(first);
    CHECK(found != nullptr && *found == "one", "FlatMap stores custom-key values");
    CHECK(values.contains(second), "FlatMap lookup supports custom hash keys");
    CHECK(!values.contains(CustomKey{9, 9}), "FlatMap misses absent custom keys");
}

} // namespace

static void test_memory() {
    test_arena_destroys_owned_objects();
    test_arena_markpoint_rolls_back_destructors();
    test_arena_alignment_and_overflow();
    test_dynarray_moves_nontrivial_elements();
    test_dynarray_resize_and_at();
    test_dynarray_append_range();
    test_result_void_and_movable_chains();
    test_flat_set_insert_contains_erase();
    test_flat_set_string_and_iteration();
    test_flat_map_custom_key_hash();
}

TEST_MAIN(memory)
