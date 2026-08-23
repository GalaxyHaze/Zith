#pragma once

#include "frontend/frontend.hpp"
#include "memory/arena.hpp"
#include "sema/modern-types.hpp"
#include "session/frontend-context.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace zith::comptime {

struct InstantiationInstance {
    session::ModuleKey module;
    frontend::DeclId decl{};
    std::vector<sema::modern::TypeId> args;
    std::string mangled;
};

struct GenericCallBinding {
    session::ModuleKey module;
    frontend::ExprId callee{};
    size_t instance = ~size_t{0};
};

enum class GenericResolveStatus : uint8_t {
    Ok,
    Arity,
    CannotInfer,
    Explosion,
};

class GenericInstantiationPass {
public:
    GenericInstantiationPass(const session::CompilationSnapshot &snapshot,
                             sema::modern::TypeTable &type_table);

    [[nodiscard]] const GenericCallBinding *callBinding(const session::ModuleKey &module,
                                                        frontend::ExprId call) const noexcept;
    [[nodiscard]] const InstantiationInstance *at(size_t index) const noexcept;
    [[nodiscard]] size_t instanceCount() const noexcept {
        return instances_.size();
    }

    [[nodiscard]] GenericResolveStatus
    resolveArgs(const sema::modern::FunctionType &fn, size_t degree, uint32_t decl_id,
                const std::vector<sema::modern::TypeId> &explicit_args,
                const std::vector<sema::modern::TypeId> &argument_types,
                std::vector<sema::modern::TypeId> &out_args) const;

    size_t bindCall(const session::ModuleKey &module, frontend::ExprId callee,
                    const session::ModuleKey &target_module, frontend::DeclId decl,
                    std::vector<sema::modern::TypeId> args);

    [[nodiscard]] sema::modern::TypeId
    substituteType(sema::modern::TypeId type, const std::vector<sema::modern::TypeId> &args) const;

    [[nodiscard]] sema::modern::TypeId
    substituteFunction(const sema::modern::FunctionType &fn,
                       const std::vector<sema::modern::TypeId> &args) const;

    [[nodiscard]] std::string mangledName(const session::ModuleKey &module, frontend::DeclId decl,
                                          const std::vector<sema::modern::TypeId> &args) const;

    [[nodiscard]] bool hasErrors() const noexcept {
        return has_errors_;
    }

private:
    static uint64_t callKey(const session::ModuleKey &module, frontend::ExprId call) noexcept;
    static constexpr size_t kMaxInstances = 1024;

    const session::CompilationSnapshot &snapshot_;
    sema::modern::TypeTable &type_table_;
    bool has_errors_ = false;
    std::unordered_map<uint64_t, GenericCallBinding> calls_;
    std::vector<InstantiationInstance> instances_;
};

} // namespace zith::comptime
