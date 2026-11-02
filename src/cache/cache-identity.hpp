#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace zith::cache {

inline constexpr uint32_t kHirSchemaVersion = 3;

inline constexpr uint64_t kFeatureCanonicalReferences = uint64_t{1} << 0u;
inline constexpr uint64_t kFeatureHirDeclarations = uint64_t{1} << 1u;
inline constexpr uint64_t kFeatureSafetyObligations = uint64_t{1} << 2u;
inline constexpr uint64_t kRequiredHirFeatures = kFeatureCanonicalReferences |
                                                  kFeatureHirDeclarations |
                                                  kFeatureSafetyObligations;

inline constexpr uint64_t kOptionalFeatureInstantiationSummary = uint64_t{1} << 0u;
inline constexpr uint64_t kOptionalFeatureFrontendProvenance = uint64_t{1} << 1u;
inline constexpr uint64_t kOptionalFeatureDerivedVir = uint64_t{1} << 2u;
inline constexpr uint64_t kOptionalFeatureTestDebug = uint64_t{1} << 3u;
inline constexpr uint8_t kFunctionSignatureVariadic = 1U << 0U;
inline constexpr uint8_t kFunctionSignatureState = 1U << 1U;
inline constexpr uint8_t kFunctionSignatureForeignC = 1U << 2U;
inline constexpr uint8_t kFunctionSignatureTailCC = 1U << 3U;

// These identities deliberately distinguish the producer from the semantic
// contract. A different frontend may produce reusable HIR when its profile
// and canonical module match.
struct FrontendIdentity {
    std::string dialect;
    std::string implementation;
    std::string version;

    bool operator==(const FrontendIdentity &) const = default;
};

struct SemanticCompatibilityProfile {
    std::string name;
    uint32_t version = 0;

    bool operator==(const SemanticCompatibilityProfile &) const = default;
};

struct CanonicalModuleIdentity {
    std::string path;

    bool operator==(const CanonicalModuleIdentity &) const = default;
};

struct ContentFingerprint {
    uint64_t primary   = 0;
    uint64_t secondary = 0;

    bool operator==(const ContentFingerprint &) const = default;
};

// The name and version are constexpr views so the identity has a single,
// testable source of truth. The profile itself is built by accessor rather
// than exposed as a namespace-scope object: SemanticCompatibilityProfile owns
// its name in a std::string, and a namespace-scope std::string needs a dynamic
// initializer and an exit-time destructor, which the strict Clang warning set
// (-Wglobal-constructors, -Wexit-time-destructors) rejects in the WebAssembly
// build. A constexpr std::string is not an option either, because emscripten's
// libc++ 18 std::string is not constexpr-constructible. Returning the profile
// by value has neither a global constructor nor an exit-time destructor.
inline constexpr std::string_view kSharedHirProfileName = "zith-shared-hir";
inline constexpr uint32_t kSharedHirProfileVersion      = 1;

inline SemanticCompatibilityProfile sharedHirProfile() {
    return SemanticCompatibilityProfile{std::string(kSharedHirProfileName),
                                        kSharedHirProfileVersion};
}

} // namespace zith::cache
