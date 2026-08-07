#pragma once

#include "common/overloaded.hpp"
#include "type-kind.hpp"

namespace zith::types {

/// Calls `fn` for each direct child TypeId in the type data.
/// All TypeKinds are covered; leaf types (Error, Never, Void, etc.)
/// simply call with no children.
template <typename F> void walkSubTypes(const TypeData &data, F &&fn) {
    visitType(data, common::overloaded{
                        [&](const TypePtr &p) { fn(p.pointee); },
                        [&](const TypeArray &a) { fn(a.elem); },
                        [&](const TypeStruct &s) { fn(s.def_id); },
                        [&](const TypeFn &f) {
                            fn(f.ret);
                            for (size_t i = 0; i < f.param_count; i++)
                                fn(f.params[i]);
                        },
                        [&](const TypeOptional &o) { fn(o.inner); },
                        [&](const TypeFailable &f) { fn(f.inner); },
                        [&](const TypeAlias &a) { fn(a.target); },
                        [&](const TypeNominal &n) {
                            fn(n.target);
                        },
                        [&](const TypeQualified &q) { fn(q.inner); },
                        [&](const TypeTrait &) {},
                        [&](const TypeSlice &s) { fn(s.elem); },
                        [&](const TypePack &p) {
                            for (size_t i = 0; i < p.count; i++)
                                fn(p.members[i]);
                        },
                        [&](const TypeSum &s) {
                            for (size_t i = 0; i < s.count; i++)
                                fn(s.members[i]);
                        },
                        [&](const TypeIncomplete &ic) {
                            fn(ic.base);
                            for (size_t i = 0; i < ic.arg_count; i++)
                                fn(ic.args[i]);
                        },
                        // Error, Never, Void, Bool, Char, Int, Float, TypeVar, Opaque,
                        // Unknown, Enum, Union, GenericParam — no children
                        [](const auto &) {},
                    });
}

} // namespace zith::types
