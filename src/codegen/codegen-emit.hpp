#pragma once

#include "codegen-type.hpp"
#include "hir/hir-expr.hpp"
#include "hir/hir-module.hpp"
#include "hir/hir-types.hpp"
#include "memory/flat-map.hpp"
#include "memory/string-interner.hpp"
#include "types/type-intern.hpp"

#include <string_view>
#include <vector>

namespace llvm {
class IRBuilderBase;
class Value;
class Type;
class Function;
class BasicBlock;
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

    void setBlocks(const std::vector<llvm::BasicBlock *> *blocks) {
        blocks_ = blocks;
    }

    llvm::Value *emitExpr(hir::HirExprId id, const hir::HirModule &mod);
    llvm::Value *emitBody(const hir::HirFunction &fn, const hir::HirModule &mod);
    void registerParams(const hir::HirFunction &fn, llvm::Function *llvmFn);

private:
    llvm::Value *emitLiteral(const hir::HirLiteral &lit);
    llvm::Value *emitBinary(const hir::HirBinary &bin, const hir::HirModule &mod);
    llvm::Value *emitUnary(const hir::HirUnary &un, const hir::HirModule &mod);
    llvm::Value *emitCall(const hir::HirCall &call, const hir::HirModule &mod);
    llvm::Value *emitRet(const hir::HirRet &ret, const hir::HirModule &mod);
    llvm::Value *emitLet(const hir::HirLet &let, const hir::HirModule &mod);
    llvm::Value *emitVar(const hir::HirVar &var);
    llvm::Value *emitVarAddr(const hir::HirVar &var);
    llvm::Value *emitJump(const hir::HirJump &jump, const hir::HirModule &mod);
    llvm::Value *emitIndexAddr(const hir::HirIndex &idx, const hir::HirModule &mod);
    llvm::Value *emitFieldAddr(const hir::HirField &field, const hir::HirModule &mod);
    llvm::Value *emitLValueAddr(hir::HirExprId target_id, const hir::HirModule &mod);
    llvm::Value *emitBranch(const hir::HirBranch &branch, const hir::HirModule &mod);
    /// True for signed integers; false for unsigned integers and all other types.
    bool isSignedType(types::TypeId id) const;

    llvm::IRBuilderBase &builder_;
    CodeGenType &typeGen_;
    const memory::StringInterner &interner_;
    const types::TypeIntern &types_;
    memory::FlatMap<std::string_view, NamedValue> namedValues_;
    std::vector<llvm::Value *> slots_;
    const std::vector<llvm::BasicBlock *> *blocks_ = nullptr;
};

} // namespace zith::codegen
