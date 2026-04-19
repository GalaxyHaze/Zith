// impl/import/import.hpp — C++ wrapper for Zith's import system
//
// Provides RAII wrappers, convenience methods, and STL-compatible
// interfaces for the import system.
#pragma once

#include "import.h"

#include <memory>
#include <string>
#include <vector>
#include <utility>
#include <algorithm>
#include <cstring>
#include <unordered_dense_map>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
}

namespace zith {
namespace import {

// ============================================================================
// Forward declarations
// ============================================================================

class Import;
class Symbol;
class Change;
class SymbolTable;

// ============================================================================
// Enums (C++ wrappers)
// ============================================================================

enum class SymbolKind : uint8_t {
    Struct    = ZITH_SYM_STRUCT,
    Union     = ZITH_SYM_UNION,
    Component = ZITH_SYM_COMPONENT,
    Entity    = ZITH_SYM_ENTITY,
    Enum      = ZITH_SYM_ENUM,
    Function  = ZITH_SYM_FUNCTION,
    Trait     = ZITH_SYM_TRAIT,
    Family    = ZITH_SYM_FAMILY,
};

enum class Visibility : uint8_t {
    Private   = ZITH_VIS_PRIVATE,
    Public    = ZITH_VIS_PUBLIC,
    Protected = ZITH_VIS_PROTECTED,
};

enum class ChangeKind : uint8_t {
    Add    = 0,
    Remove = 1,
    Modify = 2,
};

// ============================================================================
// Span-like helpers
// ============================================================================

template <typename T>
class Span {
public:
    Span() : data_(nullptr), length_(0) {}
    Span(T* data, uint32_t length) : data_(data), length_(length) {}

    T* data() const { return data_; }
    uint32_t length() const { return length_; }

    T& operator[](uint32_t i) const { return data_[i]; }

    auto begin() const { return data_; }
    auto end() const { return data_ + length_; }

private:
    T* data_;
    uint32_t length_;
};

using SymbolSpan = Span<ZithSymbol>;
using ChangeSpan = Span<ZithChange>;

// ============================================================================
// Symbol (C++ wrapper)
// ============================================================================

class Symbol {
public:
    Symbol() : name_(nullptr), kind_(SymbolKind::Struct), visibility_(Visibility::Public), decl_(nullptr) {}
    Symbol(std::string name, SymbolKind kind, Visibility vis, void* decl = nullptr)
        : name_(std::move(name)), kind_(kind), visibility_(vis), decl_(decl) {}

    const std::string& name() const { return name_; }
    SymbolKind kind() const { return kind_; }
    Visibility visibility() const { return visibility_; }
    void* decl() const { return decl_; }

    void set_decl(void* d) { decl_ = d; }

    ZithSymbol to_c() const {
        ZithSymbol s;
        s.name = const_cast<char*>(name_.c_str());
        s.kind = static_cast<ZithSymbolKind>(kind_);
        s.visibility = static_cast<ZithImportVisibility>(visibility_);
        s.decl = decl_;
        return s;
    }

    static Symbol from_c(ZithSymbol s) {
        return Symbol(s.name ? s.name : "", static_cast<SymbolKind>(s.kind),
                       static_cast<Visibility>(s.visibility), s.decl);
    }

private:
    std::string name_;
    SymbolKind kind_;
    Visibility visibility_;
    void* decl_;
};

// ============================================================================
// Change (C++ wrapper)
// ============================================================================

class Change {
public:
    Change() : kind_(ChangeKind::Add), symbol_name_(nullptr), symbol_kind_(SymbolKind::Struct) {}
    Change(ChangeKind kind, std::string name, SymbolKind sym_kind)
        : kind_(kind), symbol_name_(std::move(name)), symbol_kind_(sym_kind) {}

    ChangeKind kind() const { return kind_; }
    const std::string& symbol_name() const { return symbol_name_; }
    SymbolKind symbol_kind() const { return symbol_kind_; }

    ZithChange to_c() const {
        ZithChange c;
        c.kind = static_cast<uint8_t>(kind_);
        c.symbol_name = const_cast<char*>(symbol_name_.c_str());
        c.symbol_kind = static_cast<ZithSymbolKind>(symbol_kind_);
        return c;
    }

    static Change from_c(ZithChange c) {
        return Change(static_cast<ChangeKind>(c.kind), c.symbol_name ? c.symbol_name : "",
                      static_cast<SymbolKind>(c.symbol_kind));
    }

private:
    ChangeKind kind_;
    std::string symbol_name_;
    SymbolKind symbol_kind_;
};

// ============================================================================
// Helper: Ownership-managed arrays
// ============================================================================

namespace detail {
    template <typename T>
    struct ManagedArray {
        std::vector<T> vec;

        void init(uint32_t cap) { vec.reserve(cap); }
        void free() { vec.clear(); vec.shrink_to_fit(); }

        void push(T&& item) { vec.push_back(std::move(item)); }
        void push(const T& item) { vec.push_back(item); }

        uint32_t length() const { return static_cast<uint32_t>(vec.size()); }
        uint32_t capacity() const { return static_cast<uint32_t>(vec.capacity()); }

        T* data() { return vec.data(); }
        const T* data() const { return vec.data(); }
    };
} // namespace detail

// ============================================================================
// Import (main C++ wrapper with RAII)
// ============================================================================

class Import {
public:
    Import() : Import("", 0) {}

    Import(std::string name, uint32_t version = 0)
        : name_(std::move(name)), version_(version), is_dirty_(true) {
        auto init_arr = [](auto& arr) { arr.vec.clear(); arr.vec.shrink_to_fit(); };
        init_arr(public_types_);
        init_arr(public_functions_);
        init_arr(public_traits_);
        init_arr(protected_types_);
        init_arr(protected_functions_);
        init_arr(protected_traits_);
        init_arr(private_types_);
        init_arr(private_functions_);
        init_arr(private_traits_);
    }

    ~Import() = default;

    Import(const Import&) = delete;
    Import& operator=(const Import&) = delete;

    Import(Import&& other) noexcept
        : name_(std::move(other.name_))
        , version_(other.version_)
        , is_dirty_(other.is_dirty_)
        , public_types_(std::move(other.public_types_))
        , public_functions_(std::move(other.public_functions_))
        , public_traits_(std::move(other.public_traits_))
        , protected_types_(std::move(other.protected_types_))
        , protected_functions_(std::move(other.protected_functions_))
        , protected_traits_(std::move(other.protected_traits_))
        , private_types_(std::move(other.private_types_))
        , private_functions_(std::move(other.private_functions_))
        , private_traits_(std::move(other.private_traits_))
        , changes_(std::move(other.changes_)) {
        other.is_dirty_ = false;
    }

    Import& operator=(Import&& other) noexcept {
        if (this != &other) {
            name_ = std::move(other.name_);
            version_ = other.version_;
            is_dirty_ = other.is_dirty_;
            public_types_ = std::move(other.public_types_);
            public_functions_ = std::move(other.public_functions_);
            public_traits_ = std::move(other.public_traits_);
            protected_types_ = std::move(other.protected_types_);
            protected_functions_ = std::move(other.protected_functions_);
            protected_traits_ = std::move(other.protected_traits_);
            private_types_ = std::move(other.private_types_);
            private_functions_ = std::move(other.private_functions_);
            private_traits_ = std::move(other.private_traits_);
            changes_ = std::move(other.changes_);
            other.is_dirty_ = false;
        }
        return *this;
    }

    // Accessors
    const std::string& name() const { return name_; }
    uint32_t version() const { return version_; }

    // Symbol addition
    void add_type(std::string name, SymbolKind kind, Visibility vis = Visibility::Public) {
        add_symbol(std::move(name), kind, vis, symbol_array_for(kind, vis));
    }

    void add_function(std::string name, Visibility vis = Visibility::Public) {
        add_symbol(std::move(name), SymbolKind::Function, vis, functions_array_for(vis));
    }

    void add_trait(std::string name, SymbolKind kind, Visibility vis = Visibility::Public) {
        add_symbol(std::move(name), kind, vis, traits_array_for(kind, vis));
    }

    void add_family(std::string name, Visibility vis = Visibility::Public) {
        add_symbol(std::move(name), SymbolKind::Family, vis, traits_array_for(SymbolKind::Family, vis));
    }

    // Accessors by visibility
    auto& public_types() { return public_types_; }
    auto& protected_types() { return protected_types_; }
    auto& private_types() { return private_types_; }

    auto& public_functions() { return public_functions_; }
    auto& protected_functions() { return protected_functions_; }
    auto& private_functions() { return private_functions_; }

    auto& public_traits() { return public_traits_; }
    auto& protected_traits() { return protected_traits_; }
    auto& private_traits() { return private_traits_; }

    const auto& public_types() const { return public_types_; }
    const auto& protected_types() const { return protected_types_; }
    const auto& private_types() const { return private_types_; }

    const auto& public_functions() const { return public_functions_; }
    const auto& protected_functions() const { return protected_functions_; }
    const auto& private_functions() const { return private_functions_; }

    const auto& public_traits() const { return public_traits_; }
    const auto& protected_traits() const { return protected_traits_; }
    const auto& private_traits() const { return private_traits_; }

    // Dirty tracking
    bool is_dirty() const { return is_dirty_; }
    void mark_dirty() { is_dirty_ = true; }
    void mark_clean() { is_dirty_ = false; }

    // Change log
    void log_change(ChangeKind kind, std::string symbol_name, SymbolKind sym_kind) {
        changes_.push(Change(kind, std::move(symbol_name), sym_kind));
        mark_dirty();
    }

    Span<const Change> changes() const {
        return {const_cast<Change*>(changes_.vec.data()), changes_.length()};
    }

    void clear_changes() {
        changes_.free();
    }

    // Conversion to C ABI (zero-copy view, caller must not free)
    ZithImport* to_c() {
        ZithImport* imp = new ZithImport{};
        imp->name = const_cast<char*>(name_.c_str());
        imp->version = version_;

        imp->public_types.data = public_types_.data();
        imp->public_types.length = public_types_.length();
        imp->public_types.capacity = public_types_.capacity();

        imp->public_functions.data = public_functions_.data();
        imp->public_functions.length = public_functions_.length();
        imp->public_functions.capacity = public_functions_.capacity();

        imp->public_traits.data = public_traits_.data();
        imp->public_traits.length = public_traits_.length();
        imp->public_traits.capacity = public_traits_.capacity();

        imp->protected_types.data = protected_types_.data();
        imp->protected_types.length = protected_types_.length();
        imp->protected_types.capacity = protected_types_.capacity();

        imp->protected_functions.data = protected_functions_.data();
        imp->protected_functions.length = protected_functions_.length();
        imp->protected_functions.capacity = protected_functions_.capacity();

        imp->protected_traits.data = protected_traits_.data();
        imp->protected_traits.length = protected_traits_.length();
        imp->protected_traits.capacity = protected_traits_.capacity();

        imp->private_types.data = private_types_.data();
        imp->private_types.length = private_types_.length();
        imp->private_types.capacity = private_types_.capacity();

        imp->private_functions.data = private_functions_.data();
        imp->private_functions.length = private_functions_.length();
        imp->private_functions.capacity = private_functions_.capacity();

        imp->private_traits.data = private_traits_.data();
        imp->private_traits.length = private_traits_.length();
        imp->private_traits.capacity = private_traits_.capacity();

        imp->is_dirty = is_dirty_ ? 1 : 0;

        imp->changes.data = const_cast<Change*>(changes_.vec.data());
        imp->changes.length = changes_.length();
        imp->changes.capacity = changes_.capacity();

        return imp;
    }

    static Import from_c(ZithImport* c_imp) {
        Import imp(c_imp->name ? c_imp->name : "", c_imp->version);

        auto copy_arr = [](auto& dest, const ZithSymbolArray& src) {
            for (uint32_t i = 0; i < src.length; ++i) {
                dest.push(Symbol::from_c(src.data[i]));
            }
        };

        copy_arr(imp.public_types_, c_imp->public_types);
        copy_arr(imp.public_functions_, c_imp->public_functions);
        copy_arr(imp.public_traits_, c_imp->public_traits);
        copy_arr(imp.protected_types_, c_imp->protected_types);
        copy_arr(imp.protected_functions_, c_imp->protected_functions);
        copy_arr(imp.protected_traits_, c_imp->protected_traits);
        copy_arr(imp.private_types_, c_imp->private_types);
        copy_arr(imp.private_functions_, c_imp->private_functions);
        copy_arr(imp.private_traits_, c_imp->private_traits);

        for (uint32_t i = 0; i < c_imp->changes.length; ++i) {
            imp.changes_.push(Change::from_c(c_imp->changes.data[i]));
        }

        imp.is_dirty_ = c_imp->is_dirty != 0;
        return imp;
    }

private:
    using Arr = detail::ManagedArray<Symbol>;

    void add_symbol(std::string name, SymbolKind kind, Visibility vis, Arr& arr) {
        arr.push(Symbol{std::move(name), kind, vis, nullptr});
        mark_dirty();
    }

    Arr& symbol_array_for(SymbolKind kind, Visibility vis) {
        switch (vis) {
            case Visibility::Public: return public_types_;
            case Visibility::Protected: return protected_types_;
            case Visibility::Private: return private_types_;
        }
        return public_types_;
    }

    Arr& functions_array_for(Visibility vis) {
        switch (vis) {
            case Visibility::Public: return public_functions_;
            case Visibility::Protected: return protected_functions_;
            case Visibility::Private: return private_functions_;
        }
        return public_functions_;
    }

    Arr& traits_array_for(SymbolKind kind, Visibility vis) {
        switch (vis) {
            case Visibility::Public: return public_traits_;
            case Visibility::Protected: return protected_traits_;
            case Visibility::Private: return private_traits_;
        }
        return public_traits_;
    }

    std::string name_;
    uint32_t version_;
    bool is_dirty_;

    Arr public_types_;
    Arr public_functions_;
    Arr public_traits_;

    Arr protected_types_;
    Arr protected_functions_;
    Arr protected_traits_;

    Arr private_types_;
    Arr private_functions_;
    Arr private_traits_;

    Arr changes_;
};

// ============================================================================
// Aliases for common usage
// ============================================================================

using ImportPtr = std::unique_ptr<Import>;
using ImportView = std::unique_ptr<Import>;

} // namespace import
} // namespace zith
#endif