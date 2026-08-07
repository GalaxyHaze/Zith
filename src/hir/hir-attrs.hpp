#pragma once

#include "hir/hir-expr.hpp"
#include "memory/arena.hpp"
#include "memory/dyn-array.hpp"
#include "types/type-kind.hpp"

#include <cstdint>

namespace zith::hir {

/// Residual ownership fact attached to a slot/lowering object.
enum class HirOwnership : uint8_t {
    Default,
    Lend,
    View,
    Unique,
    Share,
    Belong,
};

/// State of a slot after the NRA proof boundary.
enum class HirConsumedState : uint8_t {
    Unknown,
    Consumed,
    NonConsumed,
};

/// Per-argument decision at a call site. The plan keeps these facts without
/// introducing ownership nodes into the expression graph.
enum class HirCallEscape : uint8_t {
    None,
    Borrow,
    Capture,
    Escape,
    Move,
};

struct HirSlotAttrs {
    HirOwnership ownership    = HirOwnership::Default;
    HirConsumedState consumed = HirConsumedState::Unknown;
    bool nonNull              = false;

    [[nodiscard]] constexpr bool hasResidualFacts() const noexcept {
        return ownership != HirOwnership::Default || consumed != HirConsumedState::Unknown ||
               nonNull;
    }
};

struct HirCallArgAttr {
    HirCallEscape escape = HirCallEscape::None;

    [[nodiscard]] constexpr bool hasResidualFacts() const noexcept {
        return escape != HirCallEscape::None;
    }
};

struct HirCallAttrs {
    memory::DynArray<HirCallArgAttr> args;
    /// When the proven return is the same node as an argument, the argument
    /// index is kept so codegen/cache can consume a non-consumed fact lazily.
    uint32_t returnsArg = ~0U;

    explicit HirCallAttrs(memory::Arena &arena) : args(arena) {}

    [[nodiscard]] constexpr bool hasResidualFacts() const noexcept {
        return returnsArg != ~0U;
    }
};

struct HirFnAttrs {
    HirConsumedState returnConsumed = HirConsumedState::Unknown;
    bool nonNull                    = false;
    bool noAlias                    = false;
    bool readOnly                   = false;
    bool noCapture                  = false;

    [[nodiscard]] constexpr bool hasResidualFacts() const noexcept {
        return returnConsumed != HirConsumedState::Unknown || nonNull || noAlias || readOnly ||
               noCapture;
    }
};

/// Parallel residual-fact tables attached to the existing HIR nodes.
///
/// The tables are intentionally side tables: the HIR expression/Fn records stay
/// ownership-free, so code without ownership remains valid with empty tables.
/// `visitExpr`, `dump` and the current emitters ignore these attributes.
class HirAttrs {
    memory::DynArray<HirSlotAttrs> slots_;
    memory::DynArray<HirCallAttrs> calls_;
    memory::DynArray<HirFnAttrs> fns_;
    memory::Arena *arena_ = nullptr;

public:
    explicit HirAttrs(memory::Arena &arena)
        : slots_(arena), calls_(arena), fns_(arena), arena_(&arena) {}

    HirAttrs(const HirAttrs &)                = delete;
    HirAttrs &operator=(const HirAttrs &)     = delete;
    HirAttrs(HirAttrs &&) noexcept            = default;
    HirAttrs &operator=(HirAttrs &&) noexcept = default;

    HirSlotAttrs &slot(HirSlotId id) {
        if (id >= slots_.size())
            slots_.resize(id + 1U);
        return slots_[id];
    }

    [[nodiscard]] const HirSlotAttrs *trySlot(HirSlotId id) const noexcept {
        return id < slots_.size() ? &slots_[id] : nullptr;
    }

    HirCallAttrs &call(HirExprId id) {
        if (id >= calls_.size())
            calls_.resizeEmplace(id + 1U, *arena_);
        return calls_[id];
    }

    [[nodiscard]] const HirCallAttrs *tryCall(HirExprId id) const noexcept {
        return id < calls_.size() ? &calls_[id] : nullptr;
    }

    HirFnAttrs &fn(size_t index) {
        if (index >= fns_.size())
            fns_.resize(index + 1U);
        return fns_[index];
    }

    [[nodiscard]] const HirFnAttrs *tryFn(size_t index) const noexcept {
        return index < fns_.size() ? &fns_[index] : nullptr;
    }

    [[nodiscard]] size_t slotCount() const noexcept {
        return slots_.size();
    }
    [[nodiscard]] size_t callCount() const noexcept {
        return calls_.size();
    }
    [[nodiscard]] size_t fnCount() const noexcept {
        return fns_.size();
    }
};

} // namespace zith::hir
