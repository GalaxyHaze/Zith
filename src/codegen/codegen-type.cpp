#include "codegen-type.hpp"

#include "types/type-kind.hpp"

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Type.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>

#include <algorithm>
#include <optional>
#include <string>

namespace zith::codegen {

namespace {

void initializeCodeGenTargets() {
    static const bool initialized = [] {
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
        return true;
    }();
    (void)initialized;
}

} // namespace

CodeGenType::CodeGenType(llvm::LLVMContext &ctx, const types::TypeIntern &types,
                         const llvm::DataLayout *layout)
    : ctx_(ctx), types_(types), layout_(layout) {}

llvm::Type *CodeGenType::lower(types::TypeId id) {
    if (id == types::kInvalidType || id == types::kErrorType)
        return llvm::Type::getVoidTy(ctx_);

    auto &data = types_.lookup(id);

    return std::visit(
        [this, id](const auto &t) -> llvm::Type * {
            using T = std::decay_t<decltype(t)>;
            if constexpr (std::is_same_v<T, types::TypeVoid>) {
                return llvm::Type::getVoidTy(ctx_);
            } else if constexpr (std::is_same_v<T, types::TypeBool>) {
                return llvm::Type::getInt1Ty(ctx_);
            } else if constexpr (std::is_same_v<T, types::TypeChar>) {
                return llvm::Type::getInt8Ty(ctx_);
            } else if constexpr (std::is_same_v<T, types::TypeInt>) {
                return llvm::Type::getIntNTy(ctx_, types::intWidthBits(t.width));
            } else if constexpr (std::is_same_v<T, types::TypeFloat>) {
                switch (t.width) {
                case types::FloatWidth::F32:
                    return llvm::Type::getFloatTy(ctx_);
                case types::FloatWidth::F64:
                    return llvm::Type::getDoubleTy(ctx_);
                default:
                    return llvm::Type::getDoubleTy(ctx_);
                }
            } else if constexpr (std::is_same_v<T, types::TypePtr>) {
                return llvm::PointerType::get(ctx_, 0);
            } else if constexpr (std::is_same_v<T, types::TypeArray>) {
                auto *elem = lower(t.elem);
                return llvm::ArrayType::get(elem, t.count);
            } else if constexpr (std::is_same_v<T, types::TypeStruct>) {
                const auto &def  = types_.getStructDef(id);
                auto struct_name = "zith.struct." + std::to_string(t.def_id);
                auto *result     = llvm::StructType::getTypeByName(ctx_, struct_name);
                if (!result)
                    result = llvm::StructType::create(ctx_, struct_name);
                if (result->isOpaque()) {
                    llvm::SmallVector<llvm::Type *, 8> fields;
                    for (const auto &field : def.fields)
                        fields.push_back(lower(field.type));
                    result->setBody(fields, false);
                }
                return result;
            } else if constexpr (std::is_same_v<T, types::TypeFn>) {
                auto *ret = lower(t.ret);
                llvm::SmallVector<llvm::Type *, 8> params;
                for (size_t i = 0; i < t.param_count; i++)
                    params.push_back(lower(t.params[i]));
                return llvm::FunctionType::get(ret, params, false);
            } else if constexpr (std::is_same_v<T, types::TypeNever>) {
                return llvm::Type::getVoidTy(ctx_);
            } else if constexpr (std::is_same_v<T, types::TypeEnum>) {
                return lower(types_.getEnumDef(id).underlying);
            } else if constexpr (std::is_same_v<T, types::TypeUnion>) {
                const auto &def                = types_.getUnionDef(id);
                uint64_t max_bytes             = 1;
                llvm::Type *max_aligned_member = llvm::Type::getInt8Ty(ctx_);
                for (auto member : def.members) {
                    auto *member_type = lower(member);
                    uint64_t size = layout_ ? layout_->getTypeAllocSize(member_type).getFixedValue()
                                            : (member_type->getPrimitiveSizeInBits() + 7U) / 8U;
                    if (size > max_bytes)
                        max_bytes = size;
                    if (layout_ && layout_->getABITypeAlign(member_type) >
                                       layout_->getABITypeAlign(max_aligned_member))
                        max_aligned_member = member_type;
                }
                // The zero-length trailing member raises ABI alignment without consuming storage.
                // LLVM rounds the struct size to that alignment, yielding untagged union storage.
                return llvm::StructType::get(
                    ctx_,
                    {llvm::ArrayType::get(llvm::Type::getInt8Ty(ctx_), max_bytes),
                     llvm::ArrayType::get(max_aligned_member, 0)},
                    false);
            } else if constexpr (std::is_same_v<T, types::TypeFailable>) {
                return llvm::PointerType::get(ctx_, 0);
            } else if constexpr (std::is_same_v<T, types::TypeOptional>) {
                auto *inner = lower(t.inner);
                // Niche optimization: ?*T uses nullptr as None sentinel
                if (inner->isPointerTy())
                    return inner;
                return llvm::StructType::get(ctx_, {inner, llvm::Type::getInt1Ty(ctx_)});
            } else if constexpr (std::is_same_v<T, types::TypeSlice>) {
                return llvm::StructType::get(
                    ctx_, {llvm::PointerType::get(ctx_, 0), llvm::Type::getInt64Ty(ctx_)});
            } else {
                return llvm::PointerType::get(ctx_, 0);
            }
        },
        data);
}

llvm::Type *CodeGenType::lowerPtr(types::TypeId pointee, bool is_mut) {
    (void)pointee;
    (void)is_mut;
    return llvm::PointerType::get(ctx_, 0);
}

uint64_t CodeGenType::sizeOf(types::TypeId id) const {
    if (!layout_)
        return 0;
    return layout_->getTypeAllocSize(const_cast<CodeGenType *>(this)->lower(id)).getFixedValue();
}

uint64_t CodeGenType::alignOf(types::TypeId id) const {
    if (!layout_)
        return 0;
    return layout_->getABITypeAlign(const_cast<CodeGenType *>(this)->lower(id)).value();
}

uint64_t CodeGenType::fieldOffset(types::TypeId struct_type, const size_t field_index) const {
    if (!layout_)
        return 0;
    auto *type =
        llvm::dyn_cast<llvm::StructType>(const_cast<CodeGenType *>(this)->lower(struct_type));
    if (!type)
        return 0;
    auto *struct_layout = layout_->getStructLayout(type);
    if (field_index >= type->getNumElements())
        return 0;
    return struct_layout->getElementOffset(static_cast<unsigned>(field_index));
}

uint64_t CodeGenType::fieldOffset(types::TypeId struct_type, std::string_view field_name) const {
    const auto index = types_.fieldIndex(struct_type, field_name);
    if (index == static_cast<size_t>(-1))
        return 0;
    return fieldOffset(struct_type, index);
}

std::optional<llvm::DataLayout> makeTargetDataLayout(const std::string_view target_triple) {
    initializeCodeGenTargets();

    const auto triple_str =
        target_triple.empty() ? llvm::sys::getDefaultTargetTriple() : std::string(target_triple);
    const auto triple = llvm::Triple(triple_str);
    std::string error;
    auto *target = llvm::TargetRegistry::lookupTarget(triple_str, error);
    if (!target)
        return std::nullopt;

    llvm::TargetOptions options;
#if LLVM_VERSION_MAJOR >= 19
    auto machine = std::unique_ptr<llvm::TargetMachine>(
        target->createTargetMachine(triple, "generic", "", options, llvm::Reloc::PIC_, std::nullopt,
                                    llvm::CodeGenOptLevel::None));
#else
    auto machine = std::unique_ptr<llvm::TargetMachine>(
        target->createTargetMachine(triple_str, "generic", "", options, llvm::Reloc::PIC_,
                                    std::nullopt, llvm::CodeGenOptLevel::None));
#endif
    if (!machine)
        return std::nullopt;

    return machine->createDataLayout();
}

} // namespace zith::codegen
