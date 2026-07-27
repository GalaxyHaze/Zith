#include "codegen.hpp"

#include "legacy-zith/ast/ast-builder.hpp"
#include "legacy-zith/ast/ast-nodes.hpp"
#include "diagnostics/error-codes.hpp"

#include <llvm/ADT/SmallString.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>

#include <llvm/TargetParser/Host.h>

namespace zith::codegen {

CodeGen::CodeGen(const memory::StringInterner &interner, const types::TypeIntern &types,
                 std::string_view targetTriple, uint8_t optLevel,
                 diagnostics::DiagnosticEngine *diags)
    : ctx_(std::make_unique<llvm::LLVMContext>()), interner_(interner), types_(types),
      diags_(diags), targetTriple_(targetTriple), optLevel_(optLevel) {
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmParser();
    LLVMInitializeX86AsmPrinter();
    LLVMInitializeWebAssemblyTargetInfo();
    LLVMInitializeWebAssemblyTarget();
    LLVMInitializeWebAssemblyTargetMC();
    LLVMInitializeWebAssemblyAsmParser();
    LLVMInitializeWebAssemblyAsmPrinter();
}

void CodeGen::llvmError(const std::string &msg) {
    if (diags_)
        diags_->report(diagnostics::Severity::Error, diagnostics::err::InvalidIR, msg,
                       memory::Span{});
    else
        llvm::errs() << msg << "\n";
}

bool CodeGen::verifyCurrentFunction(llvm::Function *llvmFn) {
    std::string buf;
    llvm::raw_string_ostream os(buf);
    if (llvm::verifyFunction(*llvmFn, &os)) {
        llvmError("IR verification failed for function '" + std::string(llvmFn->getName()) +
                  "': " + buf);
        return false;
    }
    return true;
}

std::string CodeGen::effectiveTriple() const {
    return targetTriple_.empty() ? llvm::sys::getDefaultTargetTriple() : targetTriple_;
}

int CodeGen::llvmOptLevel() const {
    return optLevel_;
}

void CodeGen::ensureTargetInfo() {
    auto tripleStr = effectiveTriple();
    auto triple    = llvm::Triple(tripleStr);
#if LLVM_VERSION_MAJOR >= 19
    module_->setTargetTriple(triple);
#else
    module_->setTargetTriple(tripleStr);
#endif
    // Create a throwaway TargetMachine just to get the correct data layout.
    std::string error;
    auto *target = llvm::TargetRegistry::lookupTarget(tripleStr, error);
    if (target) {
        llvm::TargetOptions options;
        auto tm = std::unique_ptr<llvm::TargetMachine>(target->createTargetMachine(
            triple, "generic", "", options, llvm::Reloc::PIC_, std::nullopt,
            static_cast<llvm::CodeGenOptLevel>(llvmOptLevel())));
        if (tm)
            module_->setDataLayout(tm->createDataLayout());
    }
}

void CodeGen::optimize() {
    if (optLevel_ == 0)
        return;

    std::string error;
    auto tripleStr = effectiveTriple();
    auto triple    = llvm::Triple(tripleStr);
    auto *target   = llvm::TargetRegistry::lookupTarget(tripleStr, error);
    std::unique_ptr<llvm::TargetMachine> tm;
    if (target) {
        llvm::TargetOptions options;
        tm.reset(target->createTargetMachine(triple, "generic", "", options, llvm::Reloc::PIC_,
                                             std::nullopt,
                                             static_cast<llvm::CodeGenOptLevel>(llvmOptLevel())));
    }

    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;
    llvm::PassBuilder pb(tm.get());
    pb.registerModuleAnalyses(MAM);
    pb.registerCGSCCAnalyses(CGAM);
    pb.registerFunctionAnalyses(FAM);
    pb.registerLoopAnalyses(LAM);
    pb.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    llvm::ModulePassManager mpm =
        pb.buildPerModuleDefaultPipeline(optLevel_ >= 3   ? llvm::OptimizationLevel::O3
                                         : optLevel_ == 2 ? llvm::OptimizationLevel::O2
                                                          : llvm::OptimizationLevel::O1);
    mpm.run(*module_, MAM);

    llvm::ModulePassManager dce;
    dce.addPass(llvm::GlobalDCEPass());
    dce.run(*module_, MAM);
}

CodeGen::~CodeGen() = default;

void CodeGen::emit(const hir::HirModule &hirModule, std::string_view moduleName) {
    module_ = std::make_unique<llvm::Module>(llvm::StringRef(moduleName.data(), moduleName.size()),
                                             *ctx_);
    ensureTargetInfo();

    // First pass: declare all functions (so forward references resolve)
    for (size_t i = 0; i < hirModule.getFnCount(); i++)
        declareFn(hirModule.getFn(i));

    // Second pass: emit bodies for non-extern functions
    for (size_t i = 0; i < hirModule.getFnCount(); i++) {
        auto &fn = hirModule.getFn(i);
        if (!fn.blocks.empty())
            emitFnBody(fn, hirModule);
    }
}

llvm::Function *CodeGen::declareFn(const hir::HirFunction &fn) {
    auto name = interner_.lookup(fn.name);

    CodeGenType typeGen(*ctx_, types_, &module_->getDataLayout());
    llvm::SmallVector<llvm::Type *, 8> paramTypes;
    for (auto param_type : fn.params)
        paramTypes.push_back(typeGen.lower(param_type));
    auto *retType = typeGen.lower(fn.return_type);

    auto *fnType = llvm::FunctionType::get(retType, paramTypes, false);
    auto *llvmFn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage,
                                          llvm::StringRef(name.data(), name.size()), module_.get());
    if (fn.blocks.empty())
        llvmFn->setDoesNotThrow();
    return llvmFn;
}

void CodeGen::emitFnBody(const hir::HirFunction &fn, const hir::HirModule &mod) {
    auto name    = interner_.lookup(fn.name);
    auto *llvmFn = module_->getFunction(llvm::StringRef(name.data(), name.size()));
    if (!llvmFn) {
        llvmError("function '" + std::string(name) + "' not found during body emission");
        return;
    }

    CodeGenType typeGen(*ctx_, types_, &module_->getDataLayout());
    auto *retType = typeGen.lower(fn.return_type);

    // Create LLVM basic blocks for all HIR blocks
    std::vector<llvm::BasicBlock *> llvmBlocks;
    llvmBlocks.reserve(fn.blocks.size());
    for (size_t i = 0; i < fn.blocks.size(); i++) {
        auto bbName = (i == 0) ? "entry" : ("bb" + std::to_string(i));
        llvmBlocks.push_back(llvm::BasicBlock::Create(*ctx_, bbName, llvmFn));
    }

    auto *firstBB = llvmBlocks[0];
    llvm::IRBuilder<> builder(firstBB);

    CodeGenEmit emit(builder, typeGen, interner_, types_);
    emit.setBlocks(&llvmBlocks);
    emit.registerParams(fn, llvmFn);
    emit.emitBody(fn, mod);

    // Add terminators to blocks that lack one
    for (size_t i = 0; i < fn.blocks.size(); i++) {
        if (fn.blocks[i].terminator == hir::kInvalidHirExpr) {
            auto *bb = llvmBlocks[i];
            if (!bb->getTerminator()) {
                llvm::IRBuilder<> termBuilder(bb);
                if (retType->isVoidTy())
                    termBuilder.CreateRetVoid();
                else
                    termBuilder.CreateUnreachable();
            }
        }
    }

    verifyCurrentFunction(llvmFn);
}

llvm::Module *CodeGen::getModule() {
    return module_.get();
}

std::string CodeGen::printIR() {
    std::string ir;
    llvm::raw_string_ostream os(ir);
    module_->print(os, nullptr);
    return ir;
}

static bool setupTargetMachine(llvm::Module *module, const std::string &tripleStr, int optLevel,
                               std::unique_ptr<llvm::TargetMachine> &outTM,
                               diagnostics::DiagnosticEngine *diags = nullptr) {
    std::string error;
    auto triple  = llvm::Triple(tripleStr);
    auto *target = llvm::TargetRegistry::lookupTarget(tripleStr, error);
    if (!target) {
        std::string msg = "target lookup failed: " + error;
        if (diags)
            diags->report(diagnostics::Severity::Error, diagnostics::err::InvalidIR, msg,
                          memory::Span{});
        else
            llvm::errs() << msg << "\n";
        return false;
    }

    llvm::TargetOptions options;
    outTM.reset(target->createTargetMachine(triple, "generic", "", options, llvm::Reloc::PIC_,
                                            std::nullopt,
                                            static_cast<llvm::CodeGenOptLevel>(optLevel)));
    if (!outTM) {
        std::string msg = "failed to create TargetMachine for " + tripleStr;
        if (diags)
            diags->report(diagnostics::Severity::Error, diagnostics::err::InvalidIR, msg,
                          memory::Span{});
        else
            llvm::errs() << msg << "\n";
        return false;
    }

    module->setDataLayout(outTM->createDataLayout());
    return true;
}

bool CodeGen::emitObject(const std::string &outputPath) {
    std::unique_ptr<llvm::TargetMachine> tm;
    if (!setupTargetMachine(module_.get(), effectiveTriple(), llvmOptLevel(), tm, diags_))
        return false;

    std::error_code ec;
    llvm::raw_fd_ostream dest(outputPath, ec, llvm::sys::fs::OF_None);
    if (ec) {
        llvmError("failed to open object output '" + outputPath + "': " + ec.message());
        return false;
    }

    llvm::legacy::PassManager passManager;
    if (tm->addPassesToEmitFile(passManager, dest, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        llvmError("TargetMachine cannot emit object file for this target");
        return false;
    }

    passManager.run(*module_);
    dest.flush();

    return true;
}

bool CodeGen::emitAsm(const std::string &outputPath) {
    std::unique_ptr<llvm::TargetMachine> tm;
    if (!setupTargetMachine(module_.get(), effectiveTriple(), llvmOptLevel(), tm, diags_))
        return false;

    std::error_code ec;
    llvm::raw_fd_ostream dest(outputPath, ec, llvm::sys::fs::OF_None);
    if (ec) {
        llvmError("failed to open assembly output '" + outputPath + "': " + ec.message());
        return false;
    }

    llvm::legacy::PassManager passManager;
    if (tm->addPassesToEmitFile(passManager, dest, nullptr, llvm::CodeGenFileType::AssemblyFile)) {
        llvmError("TargetMachine cannot emit assembly file for this target");
        return false;
    }

    passManager.run(*module_);
    dest.flush();

    return true;
}

std::string CodeGen::printAsm() {
    std::unique_ptr<llvm::TargetMachine> tm;
    if (!setupTargetMachine(module_.get(), effectiveTriple(), llvmOptLevel(), tm, diags_))
        return "";

    llvm::SmallString<0> asm_buf;
    llvm::raw_svector_ostream ros(asm_buf);
    llvm::legacy::PassManager passManager;
    if (tm->addPassesToEmitFile(passManager, ros, nullptr, llvm::CodeGenFileType::AssemblyFile)) {
        llvmError("TargetMachine cannot emit assembly file for this target");
        return "";
    }

    passManager.run(*module_);
    return asm_buf.str().str();
}

} // namespace zith::codegen
