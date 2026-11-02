# WebAssembly Strict Warnings

The WebAssembly build compiles `zithcLib` under the same strict Clang warning
set as the native build (`-Weverything -Werror` when
`ZITH_ENABLE_STRICT_WARNINGS` is on), but against emscripten's bundled libc++
rather than the host libstdc++. Two non-obvious constraints follow.

## Namespace-scope std::string is rejected

A namespace-scope (or `static`) object that owns a `std::string` needs a
dynamic initializer and an exit-time destructor. `-Weverything` enables
`-Wglobal-constructors` and `-Wexit-time-destructors`, and `-Werror` turns both
into build failures. This is why `cache::sharedHirProfile()` is an accessor
that returns the profile by value instead of a
`kSharedHirProfile` global. Any future namespace-scope constant that owns a
`std::string` (or any type with a non-trivial destructor) will fail the
WebAssembly build the same way.

## constexpr std::string is not portable

Making the constant `constexpr` does not fix it. Emscripten 3.1.65 ships libc++
18, whose `std::string` allocates on the heap and so is not
`constexpr`-constructible. `em++ -std=c++23` rejects
`inline constexpr SemanticCompatibilityProfile{std::string{...}}` with
"pointer to subobject of heap-allocated object is not a constant expression".
The host libstdc++ (GCC 14) accepts it, so this only surfaces in the
WebAssembly job.

What does work is a `constexpr std::string_view` for the literal plus a
by-value accessor that materializes the owning profile:

```cpp
inline constexpr std::string_view kSharedHirProfileName = "zith-shared-hir";
inline constexpr uint32_t kSharedHirProfileVersion = 1;

inline SemanticCompatibilityProfile sharedHirProfile() {
    return SemanticCompatibilityProfile{std::string(kSharedHirProfileName),
                                        kSharedHirProfileVersion};
}
```

The `name` field must stay an owning `std::string`, not a `std::string_view`.
`zirl::Reader::read` decodes the profile name out of a temporary byte buffer
in `Store::load`, so a view would dangle once that buffer dies.

## Reproducing locally

emsdk 3.1.65 is at `~/emsdk` and matches the CI version. Source
`~/emsdk/emsdk_env.sh`, configure with `emcmake cmake ... -DZITH_IS_WASM=ON`,
and build. A quick isolated probe with the strict flags is the fastest way to
confirm a candidate design before touching the tree.
