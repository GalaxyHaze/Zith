// impl/import/symbol_table.hpp — Global symbol table for Zith
//
// Provides centralized symbol resolution using unordered_dense_map.
// Thread-unsafe (single-threaded compiler context).
#pragma once

#include "import.hpp"
#include <unordered_dense_map.hpp>

#include <vector>
#include <memory>
#include <mutex>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
}

namespace zith {
namespace import {

// ============================================================================
// Symbol Table Entry
// ============================================================================

struct SymbolTableEntry {
    Import* import_;
    ZithImportVisibility visibility_;
    uint32_t flags_;

    SymbolTableEntry(Import* imp = nullptr, Visibility vis = Visibility::Public, uint32_t flags = 0)
        : import_(imp), visibility_(static_cast<ZithImportVisibility>(vis)), flags_(flags) {}
};

// ============================================================================
// Symbol Resolution Result
// ============================================================================

class SymbolResolution {
public:
    SymbolResolution() : entry_(nullptr), sym_index_(0) {}
    SymbolResolution(SymbolTableEntry* entry, uint32_t idx)
        : entry_(entry), sym_index_(idx) {}

    explicit operator bool() const { return entry_ != nullptr; }

    Import* import() const { return entry_ ? entry_->import_ : nullptr; }
    uint32_t index() const { return sym_index_; }
    Symbol get_symbol() const;

private:
    SymbolTableEntry* entry_;
    uint32_t sym_index_;
};

// ============================================================================
// Global Symbol Table (Singleton)
// ============================================================================

class SymbolTable {
public:
    static SymbolTable& instance() {
        static SymbolTable inst;
        return inst;
    }

    bool register_import(Import& imp) {
        std::string name = imp.name();
        if (name.empty()) {
            return false;
        }

        if (imports_.contains(name)) {
            return false;
        }

        SymbolTableEntry entry(&imp);
        imports_.emplace(name, entry);
        index_import(imp);
        return true;
    }

    void unregister_import(const std::string& name) {
        auto it = imports_.find(name);
        if (it != imports_.end()) {
            unindex_import(*it->second.import_);
            imports_.erase(it);
        }
    }

    SymbolResolution resolve(const std::string& fully_qualified_name) {
        auto dot_pos = fully_qualified_name.find('.');
        if (dot_pos == std::string::npos) {
            return resolve_local(fully_qualified_name);
        }

        std::string import_name = fully_qualified_name.substr(0, dot_pos);
        std::string symbol_name = fully_qualified_name.substr(dot_pos + 1);

        auto imp_it = imports_.find(import_name);
        if (imp_it == imports_.end()) {
            return SymbolResolution();
        }

        Import* imp = imp_it->second.import_;
        return resolve_in_import(*imp, symbol_name);
    }

    SymbolResolution resolve_local(const std::string& name) {
        for (auto& kv : imports_) {
            auto result = resolve_in_import(*kv.second.import_, name);
            if (result) {
                return result;
            }
        }
        return SymbolResolution();
    }

    std::vector<SymbolResolution> resolve_all(const std::string& fully_qualified_name) {
        std::vector<SymbolResolution> results;

        auto dot_pos = fully_qualified_name.find('.');
        if (dot_pos == std::string::npos) {
            for (auto& kv : imports_) {
                auto result = resolve_in_import(*kv.second.import_, fully_qualified_name);
                if (result) {
                    results.push_back(result);
                }
            }
            return results;
        }

        std::string import_name = fully_qualified_name.substr(0, dot_pos);
        auto imp_it = imports_.find(import_name);
        if (imp_it != imports_.end()) {
            Import* imp = imp_it->second.import_;
            auto result = resolve_in_import(*imp, fully_qualified_name.substr(dot_pos + 1));
            if (result) {
                results.push_back(result);
            }
        }

        return results;
    }

    bool is_registered(const std::string& name) const {
        return imports_.contains(name);
    }

    std::vector<std::string> list_imports() const {
        std::vector<std::string> names;
        names.reserve(imports_.size());
        for (const auto& kv : imports_) {
            names.push_back(kv.first);
        }
        return names;
    }

    Import* get_import(const std::string& name) const {
        auto it = imports_.find(name);
        return it != imports_.end() ? it->second.import_ : nullptr;
    }

    void clear() {
        imports_.clear();
        symbols_by_name_.clear();
        symbols_by_visibility_.clear();
        symbols_by_kind_.clear();
    }

private:
    SymbolTable() = default;

    void index_import(Import& imp) {
        auto index_arr = [this, &imp](const auto& arr, Visibility vis) {
            for (uint32_t i = 0; i < arr.length(); ++i) {
                const Symbol& sym = arr.vec[i];
                std::string fully_qualified = imp.name() + "." + sym.name();

                symbols_by_name_[fully_qualified] = SymbolTableEntry(&imp, vis, i);

                uint8_t vis_key = static_cast<uint8_t>(vis);
                symbols_by_visibility_[vis_key].push_back(&sym);

                uint8_t kind_key = static_cast<uint8_t>(sym.kind());
                symbols_by_kind_[kind_key].push_back(&sym);
            }
        };

        index_arr(imp.public_types(), Visibility::Public);
        index_arr(imp.public_functions(), Visibility::Public);
        index_arr(imp.public_traits(), Visibility::Public);
        index_arr(imp.protected_types(), Visibility::Protected);
        index_arr(imp.protected_functions(), Visibility::Protected);
        index_arr(imp.protected_traits(), Visibility::Protected);
        index_arr(imp.private_types(), Visibility::Private);
        index_arr(imp.private_functions(), Visibility::Private);
        index_arr(imp.private_traits(), Visibility::Private);
    }

    void unindex_import(Import& imp) {
        (void)imp;
    }

    SymbolResolution resolve_in_import(Import& imp, const std::string& symbol_name) {
        auto search_arr = [&symbol_name](auto& arr) -> SymbolResolution {
            for (uint32_t i = 0; i < arr.length(); ++i) {
                if (arr.vec[i].name() == symbol_name) {
                    return SymbolResolution(nullptr, i);
                }
            }
            return SymbolResolution();
        };

        auto result = search_arr(imp.public_types());
        if (result) return result;
        result = search_arr(imp.public_functions());
        if (result) return result;
        result = search_arr(imp.public_traits());
        if (result) return result;

        result = search_arr(imp.protected_types());
        if (result) return result;
        result = search_arr(imp.protected_functions());
        if (result) return result;
        result = search_arr(imp.protected_traits());
        if (result) return result;

        result = search_arr(imp.private_types());
        if (result) return result;
        result = search_arr(imp.private_functions());
        if (result) return result;
        result = search_arr(imp.private_traits());
        if (result) return result;

        return SymbolResolution();
    }

    using ImportMap = ankerl::unordered_dense_map<std::string, SymbolTableEntry>;
    using SymbolMap = ankerl::unordered_dense_map<std::string, SymbolTableEntry>;
    using VisibilityIndex = ankerl::unordered_dense_map<uint8_t, std::vector<Symbol*>>;
    using KindIndex = ankerl::unordered_dense_map<uint8_t, std::vector<Symbol*>>;

    ImportMap imports_;
    SymbolMap symbols_by_name_;
    VisibilityIndex symbols_by_visibility_;
    KindIndex symbols_by_kind_;
};

// ============================================================================
// Symbol Implementation
// ============================================================================

Symbol SymbolResolution::get_symbol() const {
    if (!entry_) return Symbol();

    Import* imp = entry_->import_;
    uint32_t idx = sym_index_;

    auto& arr = imp->public_types();
    if (idx < arr.length()) return arr.vec[idx];
    idx -= arr.length();

    auto& funcs = imp->public_functions();
    if (idx < funcs.length()) return funcs.vec[idx];
    idx -= funcs.length();

    auto& traits = imp->public_traits();
    if (idx < traits.length()) return traits.vec[idx];

    return Symbol();
}

// ============================================================================
// Convenience functions
// ============================================================================

inline Import* resolve_import(const std::string& name) {
    return SymbolTable::instance().get_import(name);
}

inline Symbol resolve_symbol(const std::string& fully_qualified_name) {
    auto result = SymbolTable::instance().resolve(fully_qualified_name);
    return result.get_symbol();
}

} // namespace import
} // namespace zith
#endif