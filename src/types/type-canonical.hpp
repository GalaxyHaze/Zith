#pragma once

#include "types/type-id.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace zith::types {

class TypeIntern;

/// Resolves a module key (for example `stdlib/std/io/console.zithm`) to the
/// dotted canonical namespace used inside canonical type ids. It is injected so
/// the type layer does not depend on the session/sema namespace policy.
using ModuleNamespaceResolver = std::function<std::string(std::string_view module_key)>;

/// Aggregate byte size of `type` as HIR lowering lays it out. This is the single
/// source of truth shared by lowering and canonical type ids, so the canonical
/// field order cannot drift from the emitted layout.
[[nodiscard]] uint32_t typeByteCount(const TypeIntern &types, TypeId type) noexcept;

/// Aggregate byte alignment of `type` as HIR lowering lays it out.
[[nodiscard]] uint32_t typeAlignBytes(const TypeIntern &types, TypeId type) noexcept;

/// Number of bytes of the member-index tag appended to a tagged union payload.
[[nodiscard]] uint32_t tagByteCount(uint32_t member_count) noexcept;

/// Canonical field order for the struct `type`: indices into its declared field
/// list, sorted by aggregate byte size and then by field name. This is the exact
/// order canonical type ids hash in, exposed so tests can guard the comparator
/// directly. Returns an empty vector when `type` is not a struct.
[[nodiscard]] std::vector<size_t> canonicalFieldOrder(const TypeIntern &types, TypeId type);

/// Canonical 128-bit identity of `type`, derived from the defining module
/// namespace, the structural kind, and (for structs) the fields ordered by a
/// size-then-name canonical field order.
///
/// This is the one shared rule used by the `at-canonicalType` intrinsic lowering
/// and by opaque cache canonization. Both paths must call this function so a
/// canonical field-order change cannot silently desynchronize them.
[[nodiscard]] TypeCanonicalId canonicalTypeId(const TypeIntern &types, TypeId type,
                                              const ModuleNamespaceResolver &resolveNamespace);

} // namespace zith::types
