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

    /// Installs the module-wide marker runtime used by `HirMarkerStore/Load/Dock/Jump/Ret`.
    /// Called by CodeGen before a body is emitted, so marker exprs can resolve globals.
    void setMarkerRuntime(llvm::Module *module, const hir::HirModuleMarkerLayout &markers);

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
    /// Resolves the address of an expression without loading its value. Returns
    /// nullptr when the expression is not addressable in place.
    llvm::Value *emitAddrOf(hir::HirExprId id, const hir::HirModule &mod);
    llvm::Value *emitJump(const hir::HirJump &jump, const hir::HirModule &mod);
    llvm::Value *emitIndexAddr(const hir::HirIndex &idx, const hir::HirModule &mod);
    llvm::Value *emitFieldAddr(const hir::HirField &field, const hir::HirModule &mod);
    llvm::Value *emitLValueAddr(hir::HirExprId target_id, const hir::HirModule &mod);
    llvm::Value *emitBranch(const hir::HirBranch &branch, const hir::HirModule &mod);
    llvm::Value *emitMarkerStore(const hir::HirMarkerStore &store, const hir::HirModule &mod);
    llvm::Value *emitMarkerLoad(const hir::HirMarkerLoad &load, const hir::HirModule &mod);
    llvm::Value *emitMarkerDock(const hir::HirMarkerDock &dock, const hir::HirModule &mod);
    llvm::Value *emitMarkerJump(const hir::HirMarkerJump &jump, const hir::HirModule &mod);
    llvm::Value *emitMarkerRet(const hir::HirMarkerRet &ret, const hir::HirModule &mod);
    const hir::HirMarker *markerFor(uint32_t marker_id) const;
    std::optional<uint64_t> markerOffset(const hir::HirMarker &marker, uint32_t param_index) const;
    /// True for signed integers; false for unsigned integers and all other types.
    bool isSignedType(types::TypeId id) const;

    /// Last non-terminal instruction emitted by `emitBody`, along with the HIR
    /// block it came from. `emitRet` uses this to avoid re-evaluating the same
    /// expression when it was already emitted as the block's trailing `inst`.
    hir::HirExprId emitBodyLastId_  = hir::kInvalidHirExpr;
    llvm::Value *emitBodyLastValue_ = nullptr;

    llvm::IRBuilderBase &builder_;
    CodeGenType &typeGen_;
    const memory::StringInterner &interner_;
    const types::TypeIntern &types_;
    memory::FlatMap<std::string_view, NamedValue> namedValues_;
    std::vector<llvm::Value *> slots_;
    const std::vector<llvm::BasicBlock *> *blocks_ = nullptr;
    llvm::Module *module_                          = nullptr;
    const hir::HirModuleMarkerLayout *markers_     = nullptr;
};

} // namespace zith::codegen
