#pragma once

#include "codegen-type.hpp"
#include "hir/hir-expr.hpp"
#include "hir/hir-module.hpp"
#include "hir/hir-types.hpp"
#include "memory/flat-map.hpp"
#include "memory/string-interner.hpp"
#include "types/type-intern.hpp"

#include <optional>
#include <string_view>
#include <vector>

namespace llvm {
class IRBuilderBase;
class Value;
class Type;
class Function;
class BasicBlock;
class Module;
} // namespace llvm

namespace zith::codegen {

struct NamedValue {
    llvm::Value *value;
    llvm::Type *elementType;
    bool isAlloca;
};

class CodeGenEmit {
public:
    CodeGenEmit(llvm::IRBuilderBase &builder, CodeGenType &typeGen,
                const memory::StringInterner &interner, const types::TypeIntern &types);

    /// Emits a direct LLVM `musttail` state call followed immediately by `ret`.
    llvm::Value *emitStateTailCall(const hir::HirStateTailCall &tail, const hir::HirModule &mod);
    llvm::Value *emitCleanup(const hir::HirCleanup &cleanup, const hir::HirModule &mod);

    void setBlocks(const std::vector<llvm::BasicBlock *> *blocks) {
        blocks_ = blocks;
    }
    void setModule(llvm::Module *module) {
        module_ = module;
    }

    llvm::Value *emitExpr(hir::HirExprId id, const hir::HirModule &mod);
    llvm::Value *emitBody(const hir::HirFunction &fn, const hir::HirModule &mod);
    void registerParams(const hir::HirFunction &fn, llvm::Function *llvmFn,
                        const hir::HirModule &mod);

private:
    llvm::Value *emitLiteral(const hir::HirLiteral &lit);
    llvm::Value *emitBinary(const hir::HirBinary &bin, const hir::HirModule &mod);
    llvm::Value *emitUnary(const hir::HirUnary &un, const hir::HirModule &mod);
    llvm::Value *emitCall(const hir::HirCall &call, const hir::HirModule &mod);
    llvm::Value *emitMakeDyn(const hir::HirMakeDyn &make, const hir::HirModule &mod);
    llvm::Value *emitDynCall(const hir::HirDynCall &call, const hir::HirModule &mod);
    llvm::Value *emitMakeOpaque(const hir::HirMakeOpaque &make, const hir::HirModule &mod);
    llvm::Value *emitOpaqueCast(const hir::HirOpaqueCast &cast, const hir::HirModule &mod);
    llvm::Value *emitOpaqueCheck(const hir::HirOpaqueCheck &check, const hir::HirModule &mod);
    llvm::Value *emitRet(const hir::HirRet &ret, const hir::HirModule &mod);
    llvm::Value *emitLet(const hir::HirLet &let, const hir::HirModule &mod);
    llvm::Value *emitVar(const hir::HirVar &var);
    llvm::Value *emitVarAddr(const hir::HirVar &var);
    /// Resolves the address of an expression without loading its value. Returns
    /// nullptr when the expression is not addressable in place.
    llvm::Value *emitAddrOf(hir::HirExprId id, const hir::HirModule &mod);
    llvm::Value *emitJump(const hir::HirJump &jump, const hir::HirModule &mod);
    llvm::Value *emitIndexAddr(const hir::HirIndex &idx, const hir::HirModule &mod);
    llvm::Value *emitFieldAddr(const hir::HirField &field, const hir::HirModule &mod);
    llvm::Value *emitLValueAddr(hir::HirExprId target_id, const hir::HirModule &mod);
    llvm::Value *emitBranch(const hir::HirBranch &branch, const hir::HirModule &mod);
    /// True for signed integers; false for unsigned integers and all other types.
    bool isSignedType(types::TypeId id) const;

    /// Values already emitted within the current basic block, keyed by HIR expr.
    /// Re-emitting the same expression in the same block is only used for value
    /// nodes like a call that was emitted as a block instruction and then reused
    /// by the block terminator; the cache is cleared on every block boundary so
    /// it never reuses a value across an LLVM CFG edge.
    memory::FlatMap<hir::HirExprId, llvm::Value *> emittedValues_;

    llvm::IRBuilderBase &builder_;
    CodeGenType &typeGen_;
    const memory::StringInterner &interner_;
    const types::TypeIntern &types_;
    memory::FlatMap<std::string_view, NamedValue> namedValues_;
    std::vector<llvm::Value *> slots_;
    const std::vector<llvm::BasicBlock *> *blocks_ = nullptr;
    llvm::Module *module_                          = nullptr;
};

} // namespace zith::codegen
