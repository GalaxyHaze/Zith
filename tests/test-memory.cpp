#include "memory/arena.hpp"
#include "memory/dyn-array.hpp"
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

} // namespace

static void test_memory() {
    test_arena_destroys_owned_objects();
    test_arena_markpoint_rolls_back_destructors();
    test_arena_alignment_and_overflow();
    test_dynarray_moves_nontrivial_elements();
    test_result_void_and_movable_chains();
}

TEST_MAIN(memory)
