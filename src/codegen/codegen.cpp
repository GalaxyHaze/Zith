#include "codegen.hpp"

#include "diagnostics/error-codes.hpp"
#include "legacy-zith/ast/ast-builder.hpp"
#include "legacy-zith/ast/ast-nodes.hpp"

#include <llvm/ADT/SmallString.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/GlobalVariable.h>
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

#include <algorithm>

namespace zith::codegen {

namespace {

uint32_t alignUpU32(uint32_t value, uint32_t align) noexcept {
    if (align == 0)
        return value;
    return static_cast<uint32_t>((value + align - 1U) & ~(align - 1U));
}

} // namespace

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
    if (!llvm::verifyFunction(*llvmFn, &os))
        return true;

    // Keep the primary message on one line; the raw (possibly multi-line) LLVM
    // verifier output is attached as a suggestion so it renders below the note.
    invalidIR_  = true;
    auto msg    = "IR verification failed for function '" + std::string(llvmFn->getName()) + "'";
    auto detail = buf;
    while (!detail.empty() && (detail.back() == '\n' || detail.back() == '\r'))
        detail.pop_back();

    if (diags_) {
        if (detail.empty())
            diags_->report(diagnostics::Severity::Error, diagnostics::err::InvalidIR, msg,
                           currentFnSpan_);
        else
            diags_->report(diagnostics::Severity::Error, diagnostics::err::InvalidIR, msg,
                           currentFnSpan_, {detail});
    } else {
        llvm::errs() << msg << (detail.empty() ? "" : ": " + detail) << "\n";
    }
    return false;
}

bool CodeGen::verifyWholeModule() {
    if (!module_)
        return false;
    // A per-function failure was already reported with a precise span; re-running the
    // module verifier would only duplicate it.
    if (invalidIR_)
        return false;
    std::string buf;
    llvm::raw_string_ostream os(buf);
    if (!llvm::verifyModule(*module_, &os))
        return true;

    invalidIR_         = true;
    std::string detail = buf;
    while (!detail.empty() && (detail.back() == '\n' || detail.back() == '\r'))
        detail.pop_back();
    llvmError("IR verification failed for module" + (detail.empty() ? "" : ": " + detail));
    return false;
}

bool CodeGen::refuseInvalidIR(const char *what) {
    if (!invalidIR_)
        return false;
    // Running LLVM codegen over invalid IR is not a diagnosable error, it is a crash
    // (a block without a terminator segfaults inside MachineBasicBlock construction).
    llvmError(std::string("refusing to ") + what + ": module failed IR verification");
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
#if LLVM_VERSION_MAJOR >= 19
        auto tm = std::unique_ptr<llvm::TargetMachine>(target->createTargetMachine(
            triple, "generic", "", options, llvm::Reloc::PIC_, std::nullopt,
            static_cast<llvm::CodeGenOptLevel>(llvmOptLevel())));
#else
        auto tm = std::unique_ptr<llvm::TargetMachine>(target->createTargetMachine(
            tripleStr, "generic", "", options, llvm::Reloc::PIC_, std::nullopt,
            static_cast<llvm::CodeGenOptLevel>(llvmOptLevel())));
#endif
        if (tm)
            module_->setDataLayout(tm->createDataLayout());
    }
}

void CodeGen::optimize() {
    if (optLevel_ == 0 || invalidIR_)
        return;

    std::string error;
    auto tripleStr = effectiveTriple();
    auto triple    = llvm::Triple(tripleStr);
    auto *target   = llvm::TargetRegistry::lookupTarget(tripleStr, error);
    std::unique_ptr<llvm::TargetMachine> tm;
    if (target) {
        llvm::TargetOptions options;
#if LLVM_VERSION_MAJOR >= 19
        tm.reset(target->createTargetMachine(triple, "generic", "", options, llvm::Reloc::PIC_,
                                             std::nullopt,
                                             static_cast<llvm::CodeGenOptLevel>(llvmOptLevel())));
#else
        tm.reset(target->createTargetMachine(tripleStr, "generic", "", options, llvm::Reloc::PIC_,
                                             std::nullopt,
                                             static_cast<llvm::CodeGenOptLevel>(llvmOptLevel())));
#endif
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

void CodeGen::emit(hir::HirModule &hirModule, std::string_view moduleName) {
    module_ = std::make_unique<llvm::Module>(llvm::StringRef(moduleName.data(), moduleName.size()),
                                             *ctx_);
    ensureTargetInfo();

    // Marker samples are embedded in the flow fn that reaches them, so the
    // thread-local blob is module-global and shared by all flow fns.
    if (hirModule.getMarkerCount() > 0) {
        auto &markers = hirModule.markers();
        emitMarkerOffsets(markers);
        emitMarkerRuntime(markers);
    }

    // First pass: declare all functions (so forward references resolve)
    for (size_t i = 0; i < hirModule.getFnCount(); i++)
        declareFn(hirModule.getFn(i));

    // Second pass: emit bodies for non-extern functions
    for (size_t i = 0; i < hirModule.getFnCount(); i++) {
        auto &fn = hirModule.getFn(i);
        if (!fn.blocks.empty())
            emitFnBody(fn, hirModule);
    }

    // Invariant: no consumer ever sees a module that fails verifyModule.
    verifyWholeModule();
}

llvm::Function *CodeGen::declareFn(const hir::HirFunction &fn) {
    auto name = interner_.lookup(fn.name);

    CodeGenType typeGen(*ctx_, types_, &module_->getDataLayout());
    llvm::SmallVector<llvm::Type *, 8> paramTypes;
    for (auto param_type : fn.params)
        paramTypes.push_back(typeGen.lower(param_type));
    auto *retType = typeGen.lower(fn.return_type);

    auto *fnType = llvm::FunctionType::get(retType, paramTypes, fn.isVariadic);
    auto *llvmFn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage,
                                          llvm::StringRef(name.data(), name.size()), module_.get());
    if (fn.blocks.empty())
        llvmFn->setDoesNotThrow();
    return llvmFn;
}

void CodeGen::emitMarkerRuntime(hir::HirModuleMarkerLayout &markers) {
    if (markers.blob_align == 0)
        return;

    uint64_t blob_size  = markers.blob_size != 0 ? markers.blob_size : 1;
    uint64_t blob_align = markers.blob_align != 0 ? markers.blob_align : 1;

    auto *i8       = llvm::Type::getInt8Ty(*ctx_);
    auto *blobType = llvm::ArrayType::get(i8, static_cast<uint64_t>(blob_size));
    auto *blob =
        new llvm::GlobalVariable(*module_, blobType, false, llvm::GlobalValue::InternalLinkage,
                                 llvm::ConstantAggregateZero::get(blobType), "__zith_marker_blob");
    blob->setThreadLocal(true);
    blob->setAlignment(llvm::Align(static_cast<uint64_t>(blob_align)));

    auto *contType = llvm::Type::getInt32Ty(*ctx_);
    auto *address =
        new llvm::GlobalVariable(*module_, contType, false, llvm::GlobalValue::InternalLinkage,
                                 llvm::ConstantInt::get(contType, 0), "__zith_dock_address");
    address->setThreadLocal(true);

    auto *exitType = llvm::FunctionType::get(llvm::Type::getVoidTy(*ctx_), false);
    auto *exitFn   = llvm::Function::Create(exitType, llvm::Function::InternalLinkage,
                                            "__zith_marker_exit", module_.get());
    auto *exitBB   = llvm::BasicBlock::Create(*ctx_, "entry", exitFn);
    llvm::IRBuilder<> exitBuilder(exitBB);
    auto exitCallee = module_->getOrInsertFunction(
        "exit", llvm::FunctionType::get(llvm::Type::getVoidTy(*ctx_), llvm::Type::getInt32Ty(*ctx_),
                                        false));
    exitBuilder.CreateCall(exitCallee, exitBuilder.getInt32(1));
    exitBuilder.CreateUnreachable();
}

void CodeGen::emitMarkerOffsets(hir::HirModuleMarkerLayout &markers) {
    CodeGenType typeGen(*ctx_, types_, &module_->getDataLayout());
    uint32_t blob_size  = 0;
    uint32_t blob_align = 1;
    for (auto &marker : markers.markers) {
        uint32_t offset = 0;
        for (size_t index = 0; index < marker.params.size(); ++index) {
            const auto &param = marker.params[index];
            const auto size   = static_cast<uint32_t>(typeGen.sizeOf(param.type));
            const auto align  = static_cast<uint32_t>(typeGen.alignOf(param.type));
            if (size == 0 || align == 0)
                continue;
            offset                      = alignUpU32(offset, align);
            marker.params[index].offset = static_cast<uint32_t>(offset);
            marker.params[index].size   = static_cast<uint32_t>(size);
            marker.params[index].align  = static_cast<uint32_t>(align);
            offset += size;
            blob_align = std::max(blob_align, align);
        }
        marker.blob_offset = static_cast<uint32_t>(offset);
        blob_size          = std::max(blob_size, offset);
    }
    markers.blob_size  = blob_size == 0 ? 1U : blob_size;
    markers.blob_align = blob_align;
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
    emit.setMarkerRuntime(module_.get(), mod.markers());
    emit.setBlocks(&llvmBlocks);
    emit.registerParams(fn, llvmFn);
    emit.emitBody(fn, mod);

    currentFnSpan_ = fn.fnSpan;

    // A HIR block that carries a terminator but produced no LLVM terminator means an
    // operand failed to lower (an expression emitting nullptr). That is an internal
    // compiler error, and the unterminated block would crash LLVM codegen rather than be
    // diagnosed, so report it and mark the module unusable.
    for (size_t i = 0; i < fn.blocks.size(); i++) {
        if (fn.blocks[i].terminator != hir::kInvalidHirExpr && !llvmBlocks[i]->getTerminator()) {
            invalidIR_ = true;
            llvmError("failed to emit the terminator of a block in function '" + std::string(name) +
                      "'");
            break;
        }
    }

    // Terminate every block that still lacks one, so the module stays verifiable even on
    // the failure path above: no consumer may ever see an unterminated block.
    for (auto *bb : llvmBlocks) {
        if (bb->getTerminator())
            continue;
        llvm::IRBuilder<> termBuilder(bb);
        if (retType->isVoidTy())
            termBuilder.CreateRetVoid();
        else
            termBuilder.CreateUnreachable();
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
#if LLVM_VERSION_MAJOR >= 19
    outTM.reset(target->createTargetMachine(triple, "generic", "", options, llvm::Reloc::PIC_,
                                            std::nullopt,
                                            static_cast<llvm::CodeGenOptLevel>(optLevel)));
#else
    outTM.reset(target->createTargetMachine(tripleStr, "generic", "", options, llvm::Reloc::PIC_,
                                            std::nullopt,
                                            static_cast<llvm::CodeGenOptLevel>(optLevel)));
#endif
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
    if (refuseInvalidIR("emit an object file"))
        return false;
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
    if (refuseInvalidIR("emit an assembly file"))
        return false;
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
    if (refuseInvalidIR("print assembly"))
        return "";
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
