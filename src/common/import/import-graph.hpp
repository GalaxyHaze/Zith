#pragma once

#include "common/memory/arena.hpp"
#include "common/memory/dyn-array.hpp"
#include "common/memory/flat-map.hpp"
#include "common/memory/optional.hpp"
#include "common/memory/result.hpp"
#include "common/memory/string-interner.hpp"
#include "symbols/symbols.hpp"

#include <cstdint>
#include <string_view>

namespace zith::import {

using ModuleId = uint32_t;

class Module;
class ImportGraph;

struct ResolveGuard {
    ImportGraph &graph;
    Module &module;
    ModuleId moduleId;
    bool active = false;

    ResolveGuard() = delete;
    explicit ResolveGuard(ImportGraph &graph_, Module &module_, ModuleId moduleId_)
        : graph(graph_), module(module_), moduleId(moduleId_), active(true) {}
    ~ResolveGuard();

    ResolveGuard(const ResolveGuard &)            = delete;
    ResolveGuard &operator=(const ResolveGuard &) = delete;
    ResolveGuard &operator=(ResolveGuard &&)      = delete;

    ResolveGuard(ResolveGuard &&other) noexcept
        : graph(other.graph), module(other.module), moduleId(other.moduleId),
          active(other.active) {
        other.active = false;
    }

    bool valid() const noexcept { return active; }
    explicit operator bool() const noexcept { return valid(); }
};

class Module {
public:
    using SymId       = symbols::SymId;
    using SymbolData  = symbols::SymbolData;
    using ScopeId     = symbols::ScopeId;
    using InternedId  = symbols::InternedId;

    [[nodiscard]] SymId declare(std::string_view name, symbols::SymKind kind,
                                symbols::SymbolVisibility visibility = symbols::SymbolVisibility{});
    [[nodiscard]] common::memory::Optional<SymId> lookup(std::string_view name) const;
    [[nodiscard]] common::memory::Optional<SymId> lookupLocal(std::string_view name) const;
    [[nodiscard]] symbols::DynArray<SymId> lookupAll(std::string_view name) const;

    void enterScope();
    void exitScope();
    [[nodiscard]] ScopeId currentScope() const noexcept;

    [[nodiscard]] SymbolData &get(SymId id);
    [[nodiscard]] const SymbolData &get(SymId id) const;

    [[nodiscard]] size_t symbolCount() const noexcept;
    [[nodiscard]] size_t scopeCount() const noexcept;

    template <typename Fn>
    void forEachLocal(Fn &&fn) const;

    template <typename Fn>
    void forEachAll(Fn &&fn) const;

    [[nodiscard]] std::string_view name() const noexcept;

    void dump() const;

private:
    friend class ImportGraph;

    ImportGraph &graph_;
    ModuleId id_ = symbols::kInvalidModule;

    Module(ImportGraph &graph, ModuleId id) : graph_(graph), id_(id) {}

    [[nodiscard]] common::memory::Optional<uint32_t> find_(
        InternedId name, int startScope, int endScope) const;
    [[nodiscard]] symbols::ScopeId scopeAt_(int index) const noexcept;
};

class ImportGraph {
public:
    struct ScopeEntry {
        uint32_t symbolStart = 0;
        common::memory::FlatMap<symbols::InternedId, uint32_t> names;
    };

    explicit ImportGraph(common::memory::Arena &arena,
                         common::memory::StringInterner &interner);

    [[nodiscard]] common::memory::Optional<Module &> addModule(std::string_view name = {});
    [[nodiscard]] common::memory::Optional<Module &> lookupModule(std::string_view name) const;
    void addDependency(Module &from, Module &to);

    [[nodiscard]] common::memory::Result<void> finalize();

    [[nodiscard]] bool isAncestor(const Module &parent, const Module &child) const;
    [[nodiscard]] int32_t distance(const Module &parent, const Module &child) const;
    [[nodiscard]] int32_t depthOf(const Module &id) const;

    [[nodiscard]] bool isLoaded(const Module &key) const;
    [[nodiscard]] common::memory::Result<ResolveGuard> beginResolve(const Module &key);

    [[nodiscard]] size_t nodeCount() const noexcept;
    [[nodiscard]] common::memory::StringInterner &interner() noexcept { return interner_; }
    [[nodiscard]] common::memory::Arena &arena() noexcept { return arena_; }

private:
    struct NodeData {
        int32_t tin   = 0;
        int32_t tout  = 0;
        int32_t depth = 0;
        bool finalized = false;
        bool inResolve = false;
        bool moduleCreated = false;
        Module *module = nullptr;
        common::memory::DynArray<ModuleId> edges;
        symbols::DynArray<symbols::SymbolData> symbols;
        symbols::DynArray<ScopeEntry> scopes;
        common::memory::Optional<symbols::InternedId> name;

        NodeData(common::memory::Arena &arena,
                 common::memory::Optional<symbols::InternedId> moduleName)
            : edges(arena), symbols(arena), scopes(arena), name(std::move(moduleName)) {}
    };

    common::memory::Arena &arena_;
    common::memory::StringInterner &interner_;
    common::memory::DynArray<NodeData> nodes_;
    ModuleId nextId_ = 0;
    bool finalized_ = false;

    void ensure_(ModuleId id, common::memory::Optional<symbols::InternedId> name = {});
    [[nodiscard]] common::memory::Result<void> dfs_(
        ModuleId id, int32_t &timer,
        common::memory::DynArray<uint8_t> &color);
    [[nodiscard]] ModuleId idOf_(const Module &module) const noexcept;
    void finishResolve(ModuleId id) noexcept;

    friend class Module;
    friend class ResolveGuard;
};

// --- Module template implementations ---

template <typename Fn>
void Module::forEachLocal(Fn &&fn) const {
    const auto &node = graph_.nodes_[id_];
    for (size_t i = 0; i < node.symbols.size(); ++i) {
        fn(SymId{id_, static_cast<uint32_t>(i)}, node.symbols[i]);
    }
}

template <typename Fn>
void Module::forEachAll(Fn &&fn) const {
    using VisitedEntry = struct { InternedId name; uint32_t collectedIndex; };

    auto &arena = graph_.arena_;
    common::memory::DynArray<SymId> collectedIds{arena};
    common::memory::DynArray<const SymbolData *> collectedData{arena};
    common::memory::DynArray<VisitedEntry> visited{arena};

    auto collectFromNode = [&](ModuleId modId) {
        if (modId >= graph_.nodes_.size() || !graph_.nodes_[modId].moduleCreated)
            return;
        const auto &modNode = graph_.nodes_[modId];
        for (size_t i = 0; i < modNode.symbols.size(); ++i) {
            const auto &data = modNode.symbols[i];

            // Visibility check inline (friend of ImportGraph, accesses graph_)
            if (data.visibility.kind == symbols::SymbolVisibilityKind::Private) {
                if (modId != id_)
                    continue;
            } else if (data.visibility.kind == symbols::SymbolVisibilityKind::Module) {
                const auto &range = data.visibility.range;
                if (range.ancestors >= 0) {
                    if (!graph_.isAncestor(*graph_.nodes_[id_].module,
                                           *graph_.nodes_[modId].module) ||
                        graph_.distance(*graph_.nodes_[id_].module,
                                        *graph_.nodes_[modId].module) > range.ancestors)
                        continue;
                }
                if (range.descendants >= 0) {
                    if (!graph_.isAncestor(*graph_.nodes_[modId].module,
                                           *graph_.nodes_[id_].module) ||
                        graph_.distance(*graph_.nodes_[modId].module,
                                        *graph_.nodes_[id_].module) > range.descendants)
                        continue;
                }
            }
            // Public always visible

            // Already visited?
            bool seen = false;
            for (const auto &v : visited) {
                if (v.name == data.name) {
                    seen = true;
                    break;
                }
            }
            if (seen) continue;

            visited.push(VisitedEntry{data.name, static_cast<uint32_t>(collectedIds.size())});
            collectedIds.push(SymId{modId, static_cast<uint32_t>(i)});
            collectedData.push(&data);
        }
    };

    // Visit self first (closest)
    collectFromNode(id_);

    // DFS over dependencies
    common::memory::DynArray<ModuleId> stack{arena};
    common::memory::DynArray<uint8_t> onStack{arena};
    onStack.resizeEmplace(graph_.nodes_.size(), 0);

    for (ModuleId dep : graph_.nodes_[id_].edges) {
        if (dep < graph_.nodes_.size() && graph_.nodes_[dep].moduleCreated)
            stack.push(dep);
    }

    while (!stack.empty()) {
        ModuleId cur = stack.back();
        stack.pop_back();

        if (cur >= graph_.nodes_.size() || !graph_.nodes_[cur].moduleCreated)
            continue;
        if (onStack[cur])
            continue;
        onStack[cur] = 1;

        collectFromNode(cur);

        for (ModuleId dep : graph_.nodes_[cur].edges) {
            if (dep < graph_.nodes_.size() && graph_.nodes_[dep].moduleCreated && !onStack[dep])
                stack.push(dep);
        }
    }

    for (size_t i = 0; i < collectedIds.size(); ++i) {
        fn(collectedIds[i], *collectedData[i]);
    }
}

} // namespace zith::import
