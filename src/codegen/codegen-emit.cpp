#include "codegen-emit.hpp"

namespace zith::codegen {

CodeGenEmit::CodeGenEmit(llvm::IRBuilderBase &builder, CodeGenType &typeGen,
                         const memory::StringInterner &interner, const types::TypeIntern &types)
    : builder_(builder), typeGen_(typeGen), interner_(interner), types_(types) {}

llvm::AllocaInst *CodeGenEmit::emitHoistedAlloca(llvm::Type *type) {
    return createHoistedAlloca(builder_, type, entryBlock_);
}

} // namespace zith::codegen
