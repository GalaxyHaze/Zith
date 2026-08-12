#include "common/import/import-graph.hpp"

#include <cstdio>
#include <string_view>

namespace zith::import {

// --- Module ---

ResolveGuard::~ResolveGuard() {
    if (active)
        graph.finishResolve(moduleId);
}

symbols::SymId Module::declare(std::string_view name, symbols::SymKind kind,
                               symbols::SymbolVisibility visibility) {
    const InternedId interned = graph_.interner_.intern(name);
    auto data = symbols::makeSymbol(graph_.arena_, interned, kind, visibility);
    auto &node = graph_.nodes_[id_];
    const uint32_t index = static_cast<uint32_t>(node.symbols.size());
    node.scopes.back().names.insert(interned, index);
    auto &stored = node.symbols.emplace(std::move(data));
    return SymId{id_, index};
}

common::memory::Optional<symbols::SymId> Module::lookup(std::string_view name) const {
    const auto interned = graph_.interner_.findId(name);
    if (interned.isEmpty())
        return {};
    const auto &node = graph_.nodes_[id_];
    const int last = static_cast<int>(node.scopes.size()) - 1;
    const auto found = find_(interned.value(), last, -1);
    if (found.isValid())
        return SymId{id_, found.value()};
    return {};
}

common::memory::Optional<symbols::SymId> Module::lookupLocal(std::string_view name) const {
    return lookup(name);
}

void Module::enterScope() {
    auto &node = graph_.nodes_[id_];
    const uint32_t start = static_cast<uint32_t>(node.symbols.size());
    node.scopes.push(ImportGraph::ScopeEntry{start, {}});
}

void Module::exitScope() {
    auto &node = graph_.nodes_[id_];
    if (node.scopes.size() <= 1)
        return;

    const uint32_t popStart = node.scopes.back().symbolStart;
    node.scopes.pop_back();

    while (node.symbols.size() > popStart)
        node.symbols.pop_back();
}

symbols::ScopeId Module::currentScope() const noexcept {
    return scopeAt_(static_cast<int>(graph_.nodes_[id_].scopes.size()) - 1);
}

symbols::DynArray<symbols::SymId> Module::lookupAll(std::string_view name) const {
    const auto interned = graph_.interner_.findId(name);
    const auto &node = graph_.nodes_[id_];
    symbols::DynArray<SymId> results{graph_.arena_};
    if (interned.isEmpty())
        return results;
    for (size_t s = 0; s < node.scopes.size(); ++s) {
        const auto *value = node.scopes[s].names.get(interned.value());
        if (value)
            results.push(SymId{id_, *value});
    }
    return results;
}

Module::SymbolData &Module::get(SymId symId) {
    return graph_.nodes_[symId.module].symbols[symId.local];
}

const Module::SymbolData &Module::get(SymId symId) const {
    return graph_.nodes_[symId.module].symbols[symId.local];
}

size_t Module::symbolCount() const noexcept {
    return graph_.nodes_[id_].symbols.size();
}

size_t Module::scopeCount() const noexcept {
    return graph_.nodes_[id_].scopes.size();
}

void Module::dump() const {
    const auto &node = graph_.nodes_[id_];
    std::fprintf(stderr, "=== Module %.*s (%zu symbols, %zu scopes) ===\n",
                 static_cast<int>(name().size()), name().data(),
                 node.symbols.size(), node.scopes.size());
    for (size_t s = 0; s < node.scopes.size(); ++s) {
        std::fprintf(stderr, "  scope %zu (start=%u, names=%zu):\n",
                     s, node.scopes[s].symbolStart, node.scopes[s].names.size());
        for (const auto &entry : node.scopes[s].names) {
            const auto nameView = graph_.interner_.lookup(entry.first);
            std::fprintf(stderr, "    %.*s -> sym[%u]\n",
                         static_cast<int>(nameView.size()), nameView.data(),
                         entry.second);
        }
    }
    std::fprintf(stderr, "============================================\n");
}

std::string_view Module::name() const noexcept {
    const auto &node = graph_.nodes_[id_];
    if (node.name.isEmpty())
        return {};
    return graph_.interner_.lookup(node.name.value());
}

common::memory::Optional<uint32_t> Module::find_(
    InternedId name, int startScope, int endScope) const {
    const auto &node = graph_.nodes_[id_];
    const int step = startScope > endScope ? -1 : 1;
    for (int s = startScope; s != endScope + step; s += step) {
        if (s < 0 || static_cast<size_t>(s) >= node.scopes.size())
            continue;
        const auto *value = node.scopes[s].names.get(name);
        if (value)
            return *value;
    }
    return {};
}

symbols::ScopeId Module::scopeAt_(int index) const noexcept {
    if (index < 0)
        return symbols::kInvalidScopeId;
    return static_cast<symbols::ScopeId>(index);
}

// --- ImportGraph ---

ImportGraph::ImportGraph(common::memory::Arena &arena,
                         common::memory::StringInterner &interner)
    : arena_(arena), interner_(interner), nodes_(arena) {}

common::memory::Optional<Module &> ImportGraph::addModule(std::string_view name) {
    if (!name.empty()) {
        if (lookupModule(name).isValid())
            return {};
    }

    ModuleId id = nextId_++;
    common::memory::Optional<symbols::InternedId> moduleName;
    if (!name.empty())
        moduleName = interner_.intern(name);
    ensure_(id, std::move(moduleName));
    auto &node = nodes_[id];
    node.moduleCreated = true;
    node.scopes.push(ScopeEntry{0, {}});
    auto *storage = static_cast<Module *>(arena_.alloc(sizeof(Module), alignof(Module)));
    node.module = new (storage) Module(*this, id);
    return *node.module;
}

common::memory::Optional<Module &> ImportGraph::lookupModule(std::string_view name) const {
    const auto interned = interner_.findId(name);
    if (interned.isEmpty())
        return {};
    for (size_t i = 0; i < nodes_.size(); ++i) {
        const auto &node = nodes_[i];
        if (node.name.isEmpty() || !node.moduleCreated)
            continue;
        if (node.name.value() == interned.value())
            return *node.module;
    }
    return {};
}

void ImportGraph::ensure_(ModuleId id,
                          common::memory::Optional<symbols::InternedId> name) {
    while (nodes_.size() <= id) {
        const bool isTarget = nodes_.size() == id;
        if (isTarget)
            nodes_.push(NodeData{arena_, std::move(name)});
        else
            nodes_.push(NodeData{arena_, {}});
    }
}

void ImportGraph::addDependency(Module &from, Module &to) {
    nodes_[idOf_(from)].edges.push(idOf_(to));
}

common::memory::Result<void> ImportGraph::finalize() {
    if (finalized_)
        return {};

    common::memory::DynArray<uint8_t> color{arena_};
    color.resizeEmplace(nodes_.size(), static_cast<uint8_t>(0));

    int32_t timer = 0;
    for (ModuleId id = 0; id < nodes_.size(); ++id) {
        if (nodes_[id].moduleCreated && color[id] == 0) {
            const auto result = dfs_(id, timer, color);
            if (result.isError())
                return result;
        }
    }

    finalized_ = true;
    return {};
}

common::memory::Result<void> ImportGraph::dfs_(
    ModuleId id, int32_t &timer,
    common::memory::DynArray<uint8_t> &color) {

    color[id] = 1; // gray — visiting
    nodes_[id].tin = ++timer;
    int32_t child_height = 0;

    for (const ModuleId child : nodes_[id].edges) {
        if (color[child] == 1)
            return common::memory::Error{"circular import"};
        if (color[child] == 0) {
            const auto result = dfs_(child, timer, color);
            if (result.isError())
                return result;
        }
        if (nodes_[child].depth + 1 > child_height)
            child_height = nodes_[child].depth + 1;
    }

    color[id] = 2; // black — done
    nodes_[id].tout   = ++timer;
    nodes_[id].finalized = true;
    nodes_[id].depth  = child_height;

    return {};
}

bool ImportGraph::isAncestor(const Module &parent, const Module &child) const {
    const ModuleId parentId = idOf_(parent);
    const ModuleId childId  = idOf_(child);
    if (parentId >= nodes_.size() || childId >= nodes_.size())
        return false;
    const auto &p = nodes_[parentId];
    const auto &c = nodes_[childId];
    return p.tin < c.tin && c.tout < p.tout;
}

int32_t ImportGraph::distance(const Module &parent, const Module &child) const {
    if (!isAncestor(parent, child))
        return -1;
    return nodes_[idOf_(parent)].depth - nodes_[idOf_(child)].depth;
}

int32_t ImportGraph::depthOf(const Module &id) const {
    const ModuleId key = idOf_(id);
    if (key >= nodes_.size() || !nodes_[key].moduleCreated)
        return -1;
    return nodes_[key].depth;
}

bool ImportGraph::isLoaded(const Module &key) const {
    const ModuleId id = idOf_(key);
    if (id >= nodes_.size() || !nodes_[id].moduleCreated)
        return false;
    return nodes_[id].finalized;
}

common::memory::Result<ResolveGuard> ImportGraph::beginResolve(const Module &key) {
    const ModuleId id = idOf_(key);
    if (id >= nodes_.size() || !nodes_[id].moduleCreated)
        return common::memory::Error{"module not created"};
    if (nodes_[id].inResolve)
        return common::memory::Error{"module already being resolved"};
    nodes_[id].inResolve = true;
    return ResolveGuard{*this, *nodes_[id].module, id};
}

size_t ImportGraph::nodeCount() const noexcept {
    return nodes_.size();
}

void ImportGraph::finishResolve(ModuleId id) noexcept {
    if (id < nodes_.size())
        nodes_[id].inResolve = false;
}

ModuleId ImportGraph::idOf_(const Module &module) const noexcept {
    for (size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].module == &module)
            return static_cast<ModuleId>(i);
    }
    return static_cast<ModuleId>(nodes_.size());
}

} // namespace zith::import
