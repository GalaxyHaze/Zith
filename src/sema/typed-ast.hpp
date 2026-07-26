#pragma once

#include "ast/ast-ids.hpp"
#include "memory/arena.hpp"
#include "memory/dyn-array.hpp"
#include "types/type-id.hpp"

namespace zith::ast {
class AstBuilder;
}

namespace zith::sema {

class TypedAst {
public:
    explicit TypedAst(memory::Arena &arena)
        : arena_(&arena), modules_(arena), empty_types_(arena) {}

    void setActiveBuilder(const ast::AstBuilder *builder) {
        active_builder_ = builder;
        if (root_builder_ == nullptr)
            root_builder_ = builder;
        if (builder != nullptr)
            (void)moduleFor(builder);
    }

    void set(ast::ExprId id, types::TypeId type) {
        set(active_builder_, id, type);
    }

    void set(const ast::AstBuilder *builder, ast::ExprId id, types::TypeId type) {
        if (builder == nullptr || id == ast::kInvalidExpr)
            return;
        auto &expr_types = moduleFor(builder).expr_types;
        while (id >= expr_types.size())
            expr_types.push(types::kErrorType);
        expr_types[id] = type;
    }

    types::TypeId get(ast::ExprId id) const {
        return get(root_builder_, id);
    }

    types::TypeId get(const ast::AstBuilder *builder, ast::ExprId id) const {
        if (builder == nullptr || id == ast::kInvalidExpr)
            return types::kErrorType;
        const auto *module = findModule(builder);
        if (module == nullptr || id >= module->expr_types.size())
            return types::kErrorType;
        return module->expr_types[id];
    }

    const memory::DynArray<types::TypeId> &exprTypes() const {
        const auto *module = findModule(root_builder_);
        return module != nullptr ? module->expr_types : empty_types_;
    }

private:
    struct ModuleTypes {
        const ast::AstBuilder *builder;
        memory::DynArray<types::TypeId> expr_types;

        ModuleTypes(memory::Arena &arena, const ast::AstBuilder *source_builder)
            : builder(source_builder), expr_types(arena) {}
    };

    ModuleTypes &moduleFor(const ast::AstBuilder *builder) {
        for (auto &module : modules_) {
            if (module.builder == builder)
                return module;
        }
        return modules_.emplace(*arena_, builder);
    }

    const ModuleTypes *findModule(const ast::AstBuilder *builder) const {
        for (const auto &module : modules_) {
            if (module.builder == builder)
                return &module;
        }
        return nullptr;
    }

    memory::Arena *arena_;
    memory::DynArray<ModuleTypes> modules_;
    memory::DynArray<types::TypeId> empty_types_;
    const ast::AstBuilder *active_builder_ = nullptr;
    const ast::AstBuilder *root_builder_   = nullptr;
};

} // namespace zith::sema
