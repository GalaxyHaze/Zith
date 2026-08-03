#pragma once

#include "frontend/frontend.hpp"
#include "memory/arena.hpp"
#include "memory/dyn-array.hpp"

#include <string>

namespace zith::sema::modern::narrowing {

struct PlaceSegment {
    std::string field;
    bool via_arrow = false;

    friend bool operator==(const PlaceSegment &, const PlaceSegment &) = default;
};

struct Place {
    frontend::LocalId root{};
    memory::DynArray<PlaceSegment> segments;

    explicit Place(memory::Arena &arena) : segments(arena) {}
    Place(Place &&) noexcept            = default;
    Place &operator=(Place &&) noexcept = default;
    Place(const Place &)                = delete;
    Place &operator=(const Place &)     = delete;

    [[nodiscard]] bool valid() const noexcept {
        return static_cast<bool>(root);
    }
};

enum class FactKind : uint8_t { NonNull };

struct Fact {
    FactKind kind = FactKind::NonNull;

    friend bool operator==(Fact, Fact) = default;
};

class FactSet {
public:
    struct Entry {
        Place place;
        Fact fact;

        Entry(memory::Arena &arena, frontend::LocalId root, Fact fact_);
    };

    explicit FactSet(memory::Arena &arena);
    FactSet(FactSet &&) noexcept            = default;
    FactSet &operator=(FactSet &&) noexcept = default;
    FactSet(const FactSet &)                = delete;
    FactSet &operator=(const FactSet &)     = delete;

    [[nodiscard]] FactSet clone() const;
    void assume(Place &&place, Fact fact);
    void invalidate(const Place &place);
    void invalidateArrowDependent();
    [[nodiscard]] bool has(const Place &place, Fact fact) const;
    [[nodiscard]] static FactSet merge(memory::Arena &arena, const FactSet &a, const FactSet &b);

private:
    memory::Arena *arena_ = nullptr;
    memory::DynArray<Entry> entries_;

    [[nodiscard]] static bool equalPlace(const Place &a, const Place &b);
    [[nodiscard]] static bool isPrefix(const Place &prefix, const Place &value);
    [[nodiscard]] static bool containsArrow(const Place &place);
};

} // namespace zith::sema::modern::narrowing
