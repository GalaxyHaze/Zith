#include "sema/narrowing.hpp"

namespace zith::sema::modern::narrowing {

FactSet::Entry::Entry(memory::Arena &arena, frontend::LocalId root, Fact fact_)
    : place(arena), fact(fact_) {
    place.root = root;
}

FactSet::FactSet(memory::Arena &arena) : arena_(&arena), entries_(arena) {}

FactSet FactSet::clone() const {
    FactSet copy(*arena_);
    for (const auto &entry : entries_) {
        Entry cloned(*arena_, entry.place.root, entry.fact);
        for (const auto &segment : entry.place.segments)
            cloned.place.segments.push(segment);
        copy.entries_.push(std::move(cloned));
    }
    return copy;
}

void FactSet::assume(Place &&place, Fact fact) {
    if (!place.valid())
        return;
    for (auto &entry : entries_) {
        if (equalPlace(entry.place, place)) {
            entry.fact = fact;
            return;
        }
    }
    Entry entry(*arena_, place.root, fact);
    for (const auto &segment : place.segments)
        entry.place.segments.push(segment);
    entries_.push(std::move(entry));
}

void FactSet::invalidate(const Place &place) {
    memory::DynArray<Entry> kept(*arena_);
    for (auto &entry : entries_) {
        if (isPrefix(place, entry.place))
            continue;
        kept.push(std::move(entry));
    }
    entries_ = std::move(kept);
}

void FactSet::invalidateArrowDependent() {
    memory::DynArray<Entry> kept(*arena_);
    for (auto &entry : entries_) {
        if (containsArrow(entry.place))
            continue;
        kept.push(std::move(entry));
    }
    entries_ = std::move(kept);
}

bool FactSet::has(const Place &place, Fact fact) const {
    for (const auto &entry : entries_) {
        if (entry.fact == fact && equalPlace(entry.place, place))
            return true;
    }
    return false;
}

FactSet FactSet::merge(memory::Arena &arena, const FactSet &a, const FactSet &b) {
    FactSet merged(arena);
    for (const auto &entry : a.entries_) {
        if (b.has(entry.place, entry.fact)) {
            Entry cloned(arena, entry.place.root, entry.fact);
            for (const auto &segment : entry.place.segments)
                cloned.place.segments.push(segment);
            merged.entries_.push(std::move(cloned));
        }
    }
    return merged;
}

bool FactSet::equalPlace(const Place &a, const Place &b) {
    if (a.root != b.root || a.segments.size() != b.segments.size())
        return false;
    for (size_t index = 0; index < a.segments.size(); ++index) {
        if (!(a.segments[index] == b.segments[index]))
            return false;
    }
    return true;
}

bool FactSet::isPrefix(const Place &prefix, const Place &value) {
    if (prefix.root != value.root || prefix.segments.size() > value.segments.size())
        return false;
    for (size_t index = 0; index < prefix.segments.size(); ++index) {
        if (!(prefix.segments[index] == value.segments[index]))
            return false;
    }
    return true;
}

bool FactSet::containsArrow(const Place &place) {
    for (const auto &segment : place.segments) {
        if (segment.via_arrow)
            return true;
    }
    return false;
}

} // namespace zith::sema::modern::narrowing
