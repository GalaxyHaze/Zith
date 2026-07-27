#pragma once

#include "codegen-emit.hpp"
#include "codegen-type.hpp"
#include "diagnostics/diagnostic-engine.hpp"
#include "hir/hir-module.hpp"
#include "memory/string-interner.hpp"
#include "types/type-intern.hpp"

#include <memory>
#include <string_view>

namespace llvm {
class LLVMContext;
class Module;
class Function;
} // namespace llvm

namespace zith::codegen {

class CodeGen {
public:
    CodeGen(const memory::StringInterner &interner, const types::TypeIntern &types,
            std::string_view targetTriple = {}, uint8_t optLevel = 0,
            diagnostics::DiagnosticEngine *diags = nullptr);
    ~CodeGen();

    void emit(const hir::HirModule &hirModule, std::string_view moduleName);
    void optimize();

    bool emitObject(const std::string &outputPath);
    bool emitAsm(const std::string &outputPath);
    llvm::Module *getModule();
    std::string printIR();
    std::string printAsm();

private:
    llvm::Function *declareFn(const hir::HirFunction &fn);
    void emitFnBody(const hir::HirFunction &fn, const hir::HirModule &mod);
    void llvmError(const std::string &msg);
    bool verifyCurrentFunction(llvm::Function *llvmFn);

    void ensureTargetInfo();
    std::string effectiveTriple() const;
    int llvmOptLevel() const;

    std::unique_ptr<llvm::LLVMContext> ctx_;
    std::unique_ptr<llvm::Module> module_;
    const memory::StringInterner &interner_;
    const types::TypeIntern &types_;
    diagnostics::DiagnosticEngine *diags_;
    std::string targetTriple_;
    uint8_t optLevel_;
};

} // namespace zith::codegen
