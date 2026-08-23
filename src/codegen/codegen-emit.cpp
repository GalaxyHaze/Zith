#include "codegen-emit.hpp"
#include "common/overloaded.hpp"

#include "types/type-kind.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

#include <optional>

namespace zith::codegen {

CodeGenEmit::CodeGenEmit(llvm::IRBuilderBase &builder, CodeGenType &typeGen,
                         const memory::StringInterner &interner, const types::TypeIntern &types)
    : builder_(builder), typeGen_(typeGen), interner_(interner), types_(types) {}

llvm::Value *CodeGenEmit::emitExpr(hir::HirExprId id, const hir::HirModule &mod) {
    auto &expr = mod.getExpr(id);
    return hir::visitExpr(
        expr,
        common::overloaded{
            [&](const hir::HirLiteral &lit) { return emitLiteral(lit); },
            [&](const hir::HirBinary &bin) { return emitBinary(bin, mod); },
            [&](const hir::HirUnary &un) { return emitUnary(un, mod); },
            [&](const hir::HirLet &let) { return emitLet(let, mod); },
            [&](const hir::HirVar &var) { return emitVar(var); },
            [&](const hir::HirCall &call) { return emitCall(call, mod); },
            [&](const hir::HirRet &ret) { return emitRet(ret, mod); },
            [&](const hir::HirBranch &branch) { return emitBranch(branch, mod); },
            [&](const hir::HirJump &jump) { return emitJump(jump, mod); },
            [&](const hir::HirAssign &assign) -> llvm::Value * {
                auto *addr = emitLValueAddr(assign.target, mod);
                auto *val  = emitExpr(assign.value, mod);
                if (!addr || !val)
                    return nullptr;
                builder_.CreateStore(val, addr);
                return val;
            },
            [&](const hir::HirIndex &idx) -> llvm::Value * {
                auto *addr = emitIndexAddr(idx, mod);
                if (!addr)
                    return nullptr;
                return builder_.CreateLoad(typeGen_.lower(idx.type), addr);
            },
            [&](const hir::HirField &field) -> llvm::Value * {
                auto *addr = emitFieldAddr(field, mod);
                if (!addr)
                    return nullptr;
                return builder_.CreateLoad(typeGen_.lower(field.type), addr);
            },
            [&](const hir::HirStructLiteral &literal) -> llvm::Value * {
                llvm::Value *result =
                    llvm::ConstantAggregateZero::get(typeGen_.lower(literal.type));
                for (size_t i = 0; i < literal.values.size(); ++i) {
                    if (literal.values[i] == hir::kInvalidHirExpr)
                        continue;
                    auto *value = emitExpr(literal.values[i], mod);
                    if (!value)
                        return nullptr;
                    result = builder_.CreateInsertValue(result, value, {static_cast<unsigned>(i)});
                }
                return result;
            },
            [&](const hir::HirArrayLiteral &literal) -> llvm::Value * {
                llvm::Value *result = llvm::UndefValue::get(typeGen_.lower(literal.type));
                for (size_t i = 0; i < literal.elements.size(); ++i) {
                    auto *value = emitExpr(literal.elements[i], mod);
                    if (!value)
                        return nullptr;
                    result = builder_.CreateInsertValue(result, value, {static_cast<unsigned>(i)});
                }
                return result;
            },
            [&](const hir::HirEnumValue &value) -> llvm::Value * {
                auto *type = typeGen_.lower(value.type);
                return llvm::ConstantInt::get(type, static_cast<uint64_t>(value.value), true);
            },
            [](const hir::HirPhi &) -> llvm::Value * {
                llvm::errs() << "FATAL: HirPhi not supported in memory-variable codegen model\n";
                std::abort();
            },
            [&](const hir::HirSlotAlloca &s) -> llvm::Value * {
                if (s.slot >= slots_.size())
                    slots_.resize(s.slot + 1);
                auto *alloc    = builder_.CreateAlloca(typeGen_.lower(s.type));
                slots_[s.slot] = alloc;
                return alloc;
            },
            [&](const hir::HirSlotStore &s) -> llvm::Value * {
                if (s.slot >= slots_.size())
                    return nullptr;
                const auto &val_expr       = mod.getExpr(s.value);
                const auto *union_cast     = std::get_if<hir::HirUnionCast>(&val_expr);
                llvm::Value *union_storage = nullptr;
                if (union_cast != nullptr &&
                    types_.kindOf(union_cast->from) != types::TypeKind::Union &&
                    types_.kindOf(union_cast->to) == types::TypeKind::Union) {
                    union_storage = emitAddrOf(s.value, mod);
                }
                auto *val = emitExpr(s.value, mod);
                if (!val)
                    return nullptr;
                // A member -> union cast writes one member's bytes into union
                // storage. It is represented by a temporary aggregate load, so
                // reconstruct the store from the cast's own storage instead of
                // letting LLVM reload a loaded aggregate.
                if (union_storage != nullptr) {
                    llvm::Value *dest = builder_.CreateBitCast(
                        slots_[s.slot], llvm::PointerType::get(builder_.getContext(), 0));
                    builder_.CreateStore(
                        llvm::ConstantAggregateZero::get(typeGen_.lower(union_cast->to)),
                        slots_[s.slot]);
                    auto *src_bytes = builder_.CreateStructGEP(
                        typeGen_.lower(union_cast->to),
                        builder_.CreateBitCast(union_storage,
                                               llvm::PointerType::get(builder_.getContext(), 0)),
                        0U);
                    builder_.CreateStore(
                        llvm::ConstantExpr::getBitCast(
                            llvm::ConstantAggregateZero::get(typeGen_.lower(union_cast->to)),
                            llvm::ArrayType::get(llvm::Type::getInt8Ty(builder_.getContext()),
                                                 typeGen_.sizeOf(union_cast->to))),
                        dest);
                    builder_.CreateMemCpy(
                        dest, llvm::MaybeAlign(1),
                        builder_.CreateBitCast(src_bytes,
                                               llvm::PointerType::get(builder_.getContext(), 0)),
                        llvm::MaybeAlign(1), typeGen_.sizeOf(union_cast->from));
                    return val;
                }
                builder_.CreateStore(val, slots_[s.slot]);
                return val;
            },
            [&](const hir::HirSlotLoad &s) -> llvm::Value * {
                if (s.slot >= slots_.size())
                    return nullptr;
                return builder_.CreateLoad(typeGen_.lower(s.type), slots_[s.slot]);
            },
            [&](const hir::HirSlotAddr &s) -> llvm::Value * {
                if (s.slot >= slots_.size())
                    return nullptr;
                return slots_[s.slot];
            },
            [&](const hir::HirMakeNone &m) -> llvm::Value * {
                auto *llvm_type = typeGen_.lower(m.type);
                // For optional pointers, None == nullptr
                if (llvm_type->isPointerTy())
                    return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(llvm_type));
                return llvm::ConstantAggregateZero::get(llvm_type);
            },
            [&](const hir::HirUnionCast &cast) -> llvm::Value * {
                auto *value = emitExpr(cast.value, mod);
                if (!value)
                    return nullptr;
                const auto from_kind  = types_.kindOf(cast.from);
                const auto to_kind    = types_.kindOf(cast.to);
                const auto union_type = from_kind == types::TypeKind::Union ? cast.from : cast.to;
                auto *union_ll        = typeGen_.lower(union_type);
                llvm::Value *storage  = nullptr;
                if (from_kind == types::TypeKind::Union) {
                    // Keep the source storage address so member extraction
                    // never round-trips an aggregate through registers.
                    storage = emitAddrOf(cast.value, mod);
                } else {
                    storage = builder_.CreateAlloca(union_ll);
                    builder_.CreateStore(llvm::ConstantAggregateZero::get(union_ll), storage);
                    auto *bytes = builder_.CreateStructGEP(union_ll, storage, 0U);
                    builder_.CreateStore(
                        value, builder_.CreateBitCast(
                                   bytes, llvm::PointerType::get(builder_.getContext(), 0)));
                }
                if (storage == nullptr)
                    return nullptr;
                if (to_kind == types::TypeKind::Union) {
                    return builder_.CreateLoad(
                        union_ll, builder_.CreateBitCast(
                                      storage, llvm::PointerType::get(builder_.getContext(), 0)));
                }
                auto *bytes = builder_.CreateStructGEP(union_ll, storage, 0U);
                return builder_.CreateLoad(
                    typeGen_.lower(cast.to),
                    builder_.CreateBitCast(bytes,
                                           llvm::PointerType::get(builder_.getContext(), 0)));
            },
            [&](const hir::HirCast &cast) -> llvm::Value * {
                auto *value = emitExpr(cast.value, mod);
                if (!value)
                    return nullptr;
                auto *to_type          = typeGen_.lower(cast.to);
                const auto from_kind   = types_.kindOf(cast.from);
                const auto to_kind     = types_.kindOf(cast.to);
                const bool from_signed = isSignedType(cast.from);
                const bool to_signed   = isSignedType(cast.to);
                const bool from_int =
                    from_kind == types::TypeKind::Int || from_kind == types::TypeKind::Char;
                const bool to_int =
                    to_kind == types::TypeKind::Int || to_kind == types::TypeKind::Char;
                if (from_int && to_int) {
                    const unsigned from_bits = value->getType()->getIntegerBitWidth();
                    const unsigned to_bits   = to_type->getIntegerBitWidth();
                    if (to_bits == from_bits)
                        return value;
                    if (to_bits < from_bits)
                        return builder_.CreateTrunc(value, to_type);
                    return from_signed ? builder_.CreateSExt(value, to_type)
                                       : builder_.CreateZExt(value, to_type);
                }
                if (from_kind == types::TypeKind::Float && to_kind == types::TypeKind::Float) {
                    const auto from_bits = value->getType()->getPrimitiveSizeInBits();
                    const auto to_bits   = to_type->getPrimitiveSizeInBits();
                    if (to_bits == from_bits)
                        return value;
                    return to_bits < from_bits ? builder_.CreateFPTrunc(value, to_type)
                                               : builder_.CreateFPExt(value, to_type);
                }
                if (from_kind == types::TypeKind::Int && to_kind == types::TypeKind::Float) {
                    return from_signed ? builder_.CreateSIToFP(value, to_type)
                                       : builder_.CreateUIToFP(value, to_type);
                }
                if (from_kind == types::TypeKind::Float && to_kind == types::TypeKind::Int) {
                    return to_signed ? builder_.CreateFPToSI(value, to_type)
                                     : builder_.CreateFPToUI(value, to_type);
                }
                return value;
            },
            [&](const hir::HirMakeSome &m) -> llvm::Value * {
                auto *val = emitExpr(m.value, mod);
                if (!val)
                    return nullptr;
                auto *llvm_type = typeGen_.lower(m.type);
                // For optional pointers, Some is just the pointer itself
                if (llvm_type->isPointerTy())
                    return val;
                llvm::Value *result = llvm::ConstantAggregateZero::get(llvm_type);
                result              = builder_.CreateInsertValue(result, val, {0u});
                result              = builder_.CreateInsertValue(
                    result,
                    llvm::ConstantInt::get(llvm::Type::getInt1Ty(builder_.getContext()), 1u), {1u});
                return result;
            },
            [&](const hir::HirLayoutIntrinsic &i) -> llvm::Value * {
                uint64_t value = 0;
                if (i.which == hir::HirLayoutIntrinsic::Which::OffsetOf) {
                    value = typeGen_.fieldOffset(i.type, i.field_index);
                } else if (i.which == hir::HirLayoutIntrinsic::Which::AlignOf) {
                    value = typeGen_.alignOf(i.type);
                } else {
                    value = typeGen_.sizeOf(i.type);
                }
                return llvm::ConstantInt::get(llvm::Type::getInt32Ty(builder_.getContext()), value);
            },
            [&](const hir::HirMarkerStore &store) { return emitMarkerStore(store, mod); },
            [&](const hir::HirMarkerLoad &load) { return emitMarkerLoad(load, mod); },
            [&](const hir::HirMarkerDock &dock) { return emitMarkerDock(dock, mod); },
            [&](const hir::HirMarkerJump &jump) { return emitMarkerJump(jump, mod); },
            [&](const hir::HirMarkerRet &ret) { return emitMarkerRet(ret, mod); },
        });
}

llvm::Value *CodeGenEmit::emitBody(const hir::HirFunction &fn, const hir::HirModule &mod) {
    if (fn.blocks.empty())
        return nullptr;
    if (!blocks_ || blocks_->empty())
        return nullptr;

    llvm::Value *last = nullptr;
    for (size_t i = 0; i < fn.blocks.size(); i++) {
        auto &block  = fn.blocks[i];
        auto *llvmBB = (*blocks_)[i];
        // Move builder to this block if it's not already inserted
        // (avoid moving if the block already has a terminator)
        builder_.SetInsertPoint(llvmBB);
        emitBodyLastId_    = hir::kInvalidHirExpr;
        emitBodyLastValue_ = nullptr;

        for (auto inst_id : block.insts) {
            last               = emitExpr(inst_id, mod);
            emitBodyLastId_    = inst_id;
            emitBodyLastValue_ = last;
        }
        if (block.terminator != hir::kInvalidHirExpr) {
            emitExpr(block.terminator, mod);
        }
    }
    return last;
}

namespace {

/// Rounds `value` up to a power-of-two alignment (ABI alignment, in bytes).
uint64_t alignUp(uint64_t value, uint64_t align) {
    if (align == 0)
        return value;
    const uint64_t mask = align - 1;
    return (value + mask) & ~mask;
}

} // namespace

void CodeGenEmit::setMarkerRuntime(llvm::Module *module,
                                   const hir::HirModuleMarkerLayout &markers) {
    module_  = module;
    markers_ = &markers;
}

const hir::HirMarker *CodeGenEmit::markerFor(const uint32_t marker_id) const {
    if (markers_ == nullptr)
        return nullptr;
    for (const auto &marker : markers_->markers)
        if (marker.marker_id == marker_id)
            return &marker;
    return nullptr;
}

std::optional<uint64_t> CodeGenEmit::markerOffset(const hir::HirMarker &marker,
                                                  const uint32_t param_index) const {
    if (param_index >= marker.params.size())
        return std::nullopt;
    uint64_t offset = 0;
    for (uint32_t index = 0; index < param_index; ++index) {
        const auto size = typeGen_.sizeOf(marker.params[index].type);
        if (size == 0)
            return std::nullopt;
        const auto align = typeGen_.alignOf(marker.params[index].type);
        offset           = alignUp(offset, align);
        offset += size;
    }
    offset = alignUp(offset, typeGen_.alignOf(marker.params[param_index].type));
    return offset;
}

llvm::Value *CodeGenEmit::emitMarkerStore(const hir::HirMarkerStore &store,
                                          const hir::HirModule &mod) {
    if (module_ == nullptr || markers_ == nullptr)
        return nullptr;
    const auto *marker = markerFor(store.marker);
    if (marker == nullptr)
        return nullptr;
    const auto offset = markerOffset(*marker, store.param_index);
    if (!offset)
        return nullptr;
    auto *value = emitExpr(store.value, mod);
    if (value == nullptr)
        return nullptr;
    auto *blob = module_->getNamedGlobal("__zith_marker_blob");
    if (blob == nullptr)
        return nullptr;
    auto *ptr = builder_.CreateInBoundsGEP(
        blob->getValueType(), blob,
        {builder_.getInt64(0), builder_.getInt64(static_cast<uint64_t>(*offset))});
    builder_.CreateStore(value, ptr, typeGen_.alignOf(marker->params[store.param_index].type));
    return value;
}

llvm::Value *CodeGenEmit::emitMarkerLoad(const hir::HirMarkerLoad &load,
                                         const hir::HirModule &mod) {
    (void)mod;
    if (module_ == nullptr || markers_ == nullptr)
        return nullptr;
    const auto *marker = markerFor(load.marker);
    if (marker == nullptr)
        return nullptr;
    const auto offset = markerOffset(*marker, load.param_index);
    if (!offset)
        return nullptr;
    auto *blob = module_->getNamedGlobal("__zith_marker_blob");
    if (blob == nullptr)
        return nullptr;
    auto *ptr = builder_.CreateInBoundsGEP(
        blob->getValueType(), blob,
        {builder_.getInt64(0), builder_.getInt64(static_cast<uint64_t>(*offset))});
    return builder_.CreateLoad(typeGen_.lower(load.type), ptr, typeGen_.alignOf(load.type));
}

llvm::Value *CodeGenEmit::emitMarkerDock(const hir::HirMarkerDock &dock,
                                         const hir::HirModule &mod) {
    (void)mod;
    if (module_ == nullptr || blocks_ == nullptr || dock.marker_entry >= blocks_->size())
        return nullptr;
    auto *dockAddress = module_->getNamedGlobal("__zith_dock_address");
    if (dockAddress == nullptr)
        return nullptr;
    builder_.CreateStore(builder_.getInt32(static_cast<uint32_t>(dock.continuation)), dockAddress);
    auto *entry = (*blocks_)[dock.marker_entry];
    if (builder_.GetInsertBlock()->getTerminator() == nullptr)
        builder_.CreateBr(entry);
    return nullptr;
}

llvm::Value *CodeGenEmit::emitMarkerJump(const hir::HirMarkerJump &jump,
                                         const hir::HirModule &mod) {
    (void)mod;
    if (blocks_ == nullptr || jump.marker_entry >= blocks_->size())
        return nullptr;
    auto *entry = (*blocks_)[jump.marker_entry];
    if (builder_.GetInsertBlock()->getTerminator() == nullptr)
        builder_.CreateBr(entry);
    return nullptr;
}

llvm::Value *CodeGenEmit::emitMarkerRet(const hir::HirMarkerRet &ret, const hir::HirModule &mod) {
    (void)mod;
    if (module_ == nullptr || blocks_ == nullptr)
        return nullptr;
    auto *dockAddress = module_->getNamedGlobal("__zith_dock_address");
    if (dockAddress == nullptr)
        return nullptr;
    auto *exitFn = module_->getFunction("__zith_marker_exit");
    if (exitFn == nullptr)
        return nullptr;

    auto *cont = builder_.getInt32(0);
    if (!ret.continuations.empty()) {
        auto *address =
            builder_.CreateLoad(llvm::Type::getInt32Ty(builder_.getContext()), dockAddress);
        llvm::SwitchInst *switchInst  = builder_.CreateSwitch(address, nullptr, 0);
        llvm::BasicBlock *unreachable = llvm::BasicBlock::Create(
            builder_.getContext(), "marker_exit", builder_.GetInsertBlock()->getParent());
        builder_.SetInsertPoint(unreachable);
        builder_.CreateCall(exitFn, {});
        builder_.CreateUnreachable();
        switchInst->setDefaultDest(unreachable);
        for (size_t i = 0; i < ret.continuations.size(); ++i) {
            const auto continuation = ret.continuations[i];
            if (continuation >= blocks_->size())
                continue;
            switchInst->addCase(builder_.getInt32(static_cast<uint32_t>(continuation)),
                                (*blocks_)[continuation]);
        }
    } else {
        builder_.CreateCall(exitFn, {});
        builder_.CreateUnreachable();
        cont = nullptr;
    }
    return cont;
}

void CodeGenEmit::registerParams(const hir::HirFunction &fn, llvm::Function *llvmFn) {
    auto argIt = llvmFn->arg_begin();
    for (size_t i = 0; i < fn.param_names.size() && argIt != llvmFn->arg_end(); i++, ++argIt) {
        auto paramName = interner_.lookup(fn.param_names[i]);
        argIt->setName(llvm::StringRef(paramName.data(), paramName.size()));
        auto *slot = builder_.CreateAlloca(argIt->getType(), nullptr,
                                           llvm::StringRef(paramName.data(), paramName.size()));
        builder_.CreateStore(&*argIt, slot);
        namedValues_[paramName] = {slot, argIt->getType(), true};
    }
}

llvm::Value *CodeGenEmit::emitLiteral(const hir::HirLiteral &lit) {
    if (lit.type == types::kErrorType || lit.type == types::kInvalidType)
        return nullptr;
    return types::visitType(
        types_.lookup(lit.type),
        common::overloaded{
            [&](const types::TypeInt &int_t) -> llvm::Value * {
                unsigned bits = 64;
                switch (int_t.width) {
                case types::IntWidth::I8:
                case types::IntWidth::U8:
                    bits = 8;
                    break;
                case types::IntWidth::I16:
                case types::IntWidth::U16:
                    bits = 16;
                    break;
                case types::IntWidth::I32:
                case types::IntWidth::U32:
                    bits = 32;
                    break;
                case types::IntWidth::I64:
                case types::IntWidth::U64:
                    bits = 64;
                    break;
                case types::IntWidth::I128:
                case types::IntWidth::U128:
                    bits = 128;
                    break;
                case types::IntWidth::Literal:
                    bits = 64;
                    break;
                }
                bool is_signed = types::isSignedWidth(int_t.width);
                return llvm::ConstantInt::get(
                    builder_.getContext(),
                    llvm::APInt(bits, static_cast<uint64_t>(lit.i), is_signed));
            },
            [&](const types::TypeBool &) -> llvm::Value * {
                return llvm::ConstantInt::get(builder_.getContext(), llvm::APInt(1, lit.b ? 1 : 0));
            },
            [&](const types::TypeFloat &float_t) -> llvm::Value * {
                if (float_t.width == types::FloatWidth::F32)
                    return llvm::ConstantFP::get(builder_.getContext(),
                                                 llvm::APFloat(static_cast<float>(lit.f)));
                return llvm::ConstantFP::get(builder_.getContext(), llvm::APFloat(lit.f));
            },
            [&](const types::TypeChar &) -> llvm::Value * {
                return llvm::ConstantInt::get(llvm::Type::getInt8Ty(builder_.getContext()),
                                              static_cast<uint64_t>(lit.i), true);
            },
            [&](const types::TypePtr &) -> llvm::Value * {
                auto str_data = interner_.lookup(lit.str_val);
                auto *str     = llvm::ConstantDataArray::getString(
                    builder_.getContext(), llvm::StringRef(str_data.data(), str_data.size()), true);
                auto *module = builder_.GetInsertBlock()->getParent()->getParent();
                auto *global = new llvm::GlobalVariable(
                    *module, str->getType(), true, llvm::GlobalValue::PrivateLinkage, str, ".str");
                auto *zero =
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(builder_.getContext()), 0);
                llvm::Value *indices[] = {zero, zero};
                return llvm::ConstantExpr::getInBoundsGetElementPtr(str->getType(), global,
                                                                    indices);
            },
            [](const auto &) -> llvm::Value * { return nullptr; },
        });
}

bool CodeGenEmit::isSignedType(const types::TypeId id) const {
    const auto *int_type = std::get_if<types::TypeInt>(&types_.lookup(id));
    return int_type != nullptr && types::isSignedWidth(int_type->width);
}

llvm::Value *CodeGenEmit::emitBinary(const hir::HirBinary &bin, const hir::HirModule &mod) {
    auto *lhs = emitExpr(bin.lhs, mod);
    auto *rhs = emitExpr(bin.rhs, mod);
    if (!lhs || !rhs)
        return nullptr;

    bool isFloat    = lhs->getType()->isFloatingPointTy();
    bool isUnsigned = false;
    // Comparisons/arithmetic derive signedness from the operand type, not the
    // result type: a comparator's result is bool.
    const auto signed_type = bin.operand_type != types::kInvalidType ? bin.operand_type : bin.type;
    if (signed_type != types::kInvalidType) {
        auto &ty = types_.lookup(signed_type);
        types::visitType(
            ty, common::overloaded{
                    [&](const types::TypeInt &i) { isUnsigned = !types::isSignedWidth(i.width); },
                    [&](const auto &) {},
                });
    }
    switch (bin.op) {
    case hir::HirBinaryOp::Add:
        return isFloat ? builder_.CreateFAdd(lhs, rhs) : builder_.CreateAdd(lhs, rhs);
    case hir::HirBinaryOp::Sub:
        return isFloat ? builder_.CreateFSub(lhs, rhs) : builder_.CreateSub(lhs, rhs);
    case hir::HirBinaryOp::Mul:
        return isFloat ? builder_.CreateFMul(lhs, rhs) : builder_.CreateMul(lhs, rhs);
    case hir::HirBinaryOp::Div:
        if (isFloat)
            return builder_.CreateFDiv(lhs, rhs);
        if (isUnsigned)
            return builder_.CreateUDiv(lhs, rhs);
        return builder_.CreateSDiv(lhs, rhs);
    case hir::HirBinaryOp::Rem:
        if (isFloat)
            return builder_.CreateFRem(lhs, rhs);
        if (isUnsigned)
            return builder_.CreateURem(lhs, rhs);
        return builder_.CreateSRem(lhs, rhs);
    case hir::HirBinaryOp::Eq:
        return isFloat ? builder_.CreateFCmpOEQ(lhs, rhs) : builder_.CreateICmpEQ(lhs, rhs);
    case hir::HirBinaryOp::Ne:
        return isFloat ? builder_.CreateFCmpONE(lhs, rhs) : builder_.CreateICmpNE(lhs, rhs);
    case hir::HirBinaryOp::Lt:
        if (isFloat)
            return builder_.CreateFCmpOLT(lhs, rhs);
        if (isUnsigned)
            return builder_.CreateICmpULT(lhs, rhs);
        return builder_.CreateICmpSLT(lhs, rhs);
    case hir::HirBinaryOp::Le:
        if (isFloat)
            return builder_.CreateFCmpOLE(lhs, rhs);
        if (isUnsigned)
            return builder_.CreateICmpULE(lhs, rhs);
        return builder_.CreateICmpSLE(lhs, rhs);
    case hir::HirBinaryOp::Gt:
        if (isFloat)
            return builder_.CreateFCmpOGT(lhs, rhs);
        if (isUnsigned)
            return builder_.CreateICmpUGT(lhs, rhs);
        return builder_.CreateICmpSGT(lhs, rhs);
    case hir::HirBinaryOp::Ge:
        if (isFloat)
            return builder_.CreateFCmpOGE(lhs, rhs);
        if (isUnsigned)
            return builder_.CreateICmpUGE(lhs, rhs);
        return builder_.CreateICmpSGE(lhs, rhs);
    case hir::HirBinaryOp::And:
        return builder_.CreateAnd(lhs, rhs);
    case hir::HirBinaryOp::Or:
        return builder_.CreateOr(lhs, rhs);
    case hir::HirBinaryOp::Xor:
        return builder_.CreateXor(lhs, rhs);
    case hir::HirBinaryOp::Shl:
        return builder_.CreateShl(lhs, rhs);
    case hir::HirBinaryOp::Shr:
        return isUnsigned ? builder_.CreateLShr(lhs, rhs) : builder_.CreateAShr(lhs, rhs);
    case hir::HirBinaryOp::Invalid:
        break;
    }
    return nullptr;
}

llvm::Value *CodeGenEmit::emitUnary(const hir::HirUnary &un, const hir::HirModule &mod) {
    // Address-of must not evaluate (load) its operand; it needs the operand's address.
    if (un.op == hir::HirUnaryOp::Ref) {
        auto &operandExpr = mod.getExpr(un.operand);
        if (auto *var = std::get_if<hir::HirVar>(&operandExpr))
            return emitVarAddr(*var);
        if (auto *slot_load = std::get_if<hir::HirSlotLoad>(&operandExpr)) {
            if (slot_load->slot >= slots_.size())
                return nullptr;
            return slots_[slot_load->slot];
        }
        if (auto *field = std::get_if<hir::HirField>(&operandExpr))
            return emitFieldAddr(*field, mod);
        if (auto *index = std::get_if<hir::HirIndex>(&operandExpr))
            return emitIndexAddr(*index, mod);
        if (auto *unary = std::get_if<hir::HirUnary>(&operandExpr);
            unary != nullptr && unary->op == hir::HirUnaryOp::Deref) {
            return emitExpr(unary->operand, mod);
        }
        return nullptr;
    }

    auto *operand = emitExpr(un.operand, mod);
    if (!operand)
        return nullptr;

    switch (un.op) {
    case hir::HirUnaryOp::Neg:
        return operand->getType()->isFloatingPointTy() ? builder_.CreateFNeg(operand)
                                                       : builder_.CreateNeg(operand);
    case hir::HirUnaryOp::Not:
        return builder_.CreateNot(operand);
    case hir::HirUnaryOp::BitNot:
        return builder_.CreateNot(operand);
    case hir::HirUnaryOp::Ref:
        // Address-of must not evaluate (load) its operand; it needs the
        // operand's address.
        return emitAddrOf(un.operand, mod);
    case hir::HirUnaryOp::Deref:
        return builder_.CreateLoad(typeGen_.lower(un.type), operand);
    }
    return nullptr;
}

llvm::Value *CodeGenEmit::emitAddrOf(hir::HirExprId id, const hir::HirModule &mod) {
    auto &operandExpr = mod.getExpr(id);
    if (auto *var = std::get_if<hir::HirVar>(&operandExpr))
        return emitVarAddr(*var);
    if (auto *slot_load = std::get_if<hir::HirSlotLoad>(&operandExpr)) {
        if (slot_load->slot >= slots_.size())
            return nullptr;
        return slots_[slot_load->slot];
    }
    if (auto *slot_addr = std::get_if<hir::HirSlotAddr>(&operandExpr)) {
        if (slot_addr->slot >= slots_.size())
            return nullptr;
        return slots_[slot_addr->slot];
    }
    if (auto *field = std::get_if<hir::HirField>(&operandExpr))
        return emitFieldAddr(*field, mod);
    if (auto *index = std::get_if<hir::HirIndex>(&operandExpr))
        return emitIndexAddr(*index, mod);
    if (auto *unary = std::get_if<hir::HirUnary>(&operandExpr)) {
        if (unary->op == hir::HirUnaryOp::Deref)
            return emitExpr(unary->operand, mod); // `*p` is addressed by the pointer itself
    }
    if (auto *union_cast = std::get_if<hir::HirUnionCast>(&operandExpr)) {
        if (types_.kindOf(union_cast->from) != types::TypeKind::Union &&
            types_.kindOf(union_cast->to) == types::TypeKind::Union) {
            auto *storage = builder_.CreateAlloca(typeGen_.lower(union_cast->to));
            builder_.CreateStore(llvm::ConstantAggregateZero::get(typeGen_.lower(union_cast->to)),
                                 storage);
            auto *value = emitExpr(union_cast->value, mod);
            if (value == nullptr)
                return nullptr;
            auto *bytes = builder_.CreateStructGEP(
                typeGen_.lower(union_cast->to),
                builder_.CreateBitCast(storage, llvm::PointerType::get(builder_.getContext(), 0)),
                0U);
            builder_.CreateStore(
                value,
                builder_.CreateBitCast(bytes, llvm::PointerType::get(builder_.getContext(), 0)));
            return storage;
        }
    }
    // Not directly addressable (e.g. a call result or literal): spill the value
    // into a temporary so its address can be taken.
    auto *value = emitExpr(id, mod);
    if (!value)
        return nullptr;
    auto *spill = builder_.CreateAlloca(value->getType());
    builder_.CreateStore(value, spill);
    return spill;
}

llvm::Value *CodeGenEmit::emitCall(const hir::HirCall &call, const hir::HirModule &mod) {
    llvm::Function *fn = nullptr;

    // For extern calls, resolve by finding the function in the module
    llvm::SmallVector<llvm::Value *, 8> args;
    for (auto arg_id : call.args) {
        auto *v = emitExpr(arg_id, mod);
        if (!v)
            return nullptr;
        args.push_back(v);
    }

    // Find callee function from the module context
    // The callee is typically the first arg — resolve through IRBuilder's module
    auto *module = builder_.GetInsertBlock()->getParent()->getParent();

    // HIR function names are local to their source module, while call symbols
    // can be namespace-qualified by an import alias. Resolve by symbol identity.
    if (call.resolved_fn != symbols::kInvalidSym) {
        for (size_t i = 0; i < mod.getFnCount(); ++i) {
            const auto &hir_fn = mod.getFn(i);
            if (hir_fn.sym_id != call.resolved_fn)
                continue;
            auto name = interner_.lookup(hir_fn.name);
            fn        = module->getFunction(llvm::StringRef(name.data(), name.size()));
            break;
        }
    }

    if (!fn) {
        llvm::errs() << "ERROR: emitCall failed to resolve callee (resolved_fn=" << call.resolved_fn
                     << ")\n";
        return nullptr;
    }

    // C default argument promotions apply only to the variadic tail: float -> double,
    // and bool/char/small ints -> int. Fixed parameters keep their declared ABI types.
    const auto *fn_type = fn->getFunctionType();
    if (fn_type->isVarArg()) {
        const auto fixed_count = fn_type->getNumParams();
        for (size_t index = fixed_count; index < args.size(); ++index) {
            llvm::Value *value = args[index];
            if (value->getType()->isFloatingPointTy() && value->getType()->isFloatTy()) {
                args[index] =
                    builder_.CreateFPExt(value, llvm::Type::getDoubleTy(builder_.getContext()));
            } else if (value->getType()->isIntegerTy() &&
                       value->getType()->getIntegerBitWidth() < 32U) {
                const auto arg_type = index < call.argument_types.size()
                                          ? call.argument_types[index]
                                          : types::kInvalidType;
                const bool extend   = arg_type != types::kInvalidType && isSignedType(arg_type);
                args[index] =
                    extend
                        ? builder_.CreateSExt(value, llvm::Type::getInt32Ty(builder_.getContext()))
                        : builder_.CreateZExt(value, llvm::Type::getInt32Ty(builder_.getContext()));
            }
        }
    }

    return builder_.CreateCall(fn, args);
}

llvm::Value *CodeGenEmit::emitRet(const hir::HirRet &ret, const hir::HirModule &mod) {
    if (ret.value == hir::kInvalidHirExpr)
        return builder_.CreateRetVoid();
    // The implicit-return path lowers the trailing expression both as an
    // instruction and as the Ret value. Re-emitting calls there would execute
    // them twice; reuse the value produced by the trailing instruction instead.
    if (ret.value == emitBodyLastId_ && emitBodyLastValue_ != nullptr)
        return builder_.CreateRet(emitBodyLastValue_);
    auto *val = emitExpr(ret.value, mod);
    if (!val)
        return nullptr;
    return builder_.CreateRet(val);
}

llvm::Value *CodeGenEmit::emitLet(const hir::HirLet &let, const hir::HirModule &mod) {
    llvm::Value *init = nullptr;
    if (let.init != hir::kInvalidHirExpr) {
        init = emitExpr(let.init, mod);
    }

    auto name      = interner_.lookup(let.name);
    auto *elemType = typeGen_.lower(let.type);
    auto *alloca   = builder_.CreateAlloca(elemType);
    if (init) {
        builder_.CreateStore(init, alloca);
    }
    namedValues_[name] = {alloca, elemType, true};
    return alloca;
}

llvm::Value *CodeGenEmit::emitVar(const hir::HirVar &var) {
    auto name = interner_.lookup(var.name);
    auto *nv  = namedValues_.get(name);
    if (nv) {
        if (nv->isAlloca)
            return builder_.CreateLoad(nv->elementType, nv->value);
        return nv->value;
    }
    return nullptr;
}

llvm::Value *CodeGenEmit::emitVarAddr(const hir::HirVar &var) {
    auto name = interner_.lookup(var.name);
    auto *nv  = namedValues_.get(name);
    if (nv)
        return nv->value;
    return nullptr;
}

llvm::Value *CodeGenEmit::emitJump(const hir::HirJump &jump, const hir::HirModule &mod) {
    (void)mod;
    if (!blocks_ || jump.target >= blocks_->size())
        return nullptr;
    auto *target = (*blocks_)[jump.target];
    // Ordinary `jump` is a terminator-only edge. A flow `jump marker` carries the
    // continuation used only when the marker body falls through; that edge is
    // lowered when the marker clone is emitted, so only the transfer itself is
    // needed here.
    return builder_.CreateBr(target);
}

llvm::Value *CodeGenEmit::emitBranch(const hir::HirBranch &branch, const hir::HirModule &mod) {
    if (!blocks_ || branch.then_block >= blocks_->size() || branch.else_block >= blocks_->size())
        return nullptr;
    auto *condVal = emitExpr(branch.cond, mod);
    if (!condVal)
        return nullptr;
    auto *thenBB = (*blocks_)[branch.then_block];
    auto *elseBB = (*blocks_)[branch.else_block];
    return builder_.CreateCondBr(condVal, thenBB, elseBB);
}

llvm::Value *CodeGenEmit::emitIndexAddr(const hir::HirIndex &idx, const hir::HirModule &mod) {
    // A slice is a `{ *T, i64 }` aggregate: index through its data pointer.
    if (idx.obj_type && types_.kindOf(idx.obj_type) == types::TypeKind::Slice) {
        auto *aggregate = emitExpr(idx.object, mod);
        auto *index_val = emitExpr(idx.index, mod);
        if (!aggregate || !index_val)
            return nullptr;
        auto *data = builder_.CreateExtractValue(aggregate, {0U});
        return builder_.CreateGEP(typeGen_.lower(idx.type), data, index_val);
    }

    llvm::Value *addr = nullptr;
    if (idx.is_array) {
        addr = emitAddrOf(idx.object, mod);
    } else {
        addr = emitExpr(idx.object, mod);
    }

    if (!addr)
        return nullptr;

    auto *index_val = emitExpr(idx.index, mod);
    if (!index_val)
        return nullptr;

    if (idx.is_array) {
        llvm::Value *zero = builder_.getInt32(0);
        auto *arr_type    = typeGen_.lower(idx.obj_type);
        return builder_.CreateGEP(arr_type, addr, {zero, index_val});
    } else {
        auto *elem_type = typeGen_.lower(idx.type);
        return builder_.CreateGEP(elem_type, addr, index_val);
    }
}

llvm::Value *CodeGenEmit::emitFieldAddr(const hir::HirField &field, const hir::HirModule &mod) {
    llvm::Value *base = emitAddrOf(field.object, mod);
    if (!base) {
        // Fall back to spilling the aggregate value so its fields can be addressed.
        auto *value = emitExpr(field.object, mod);
        if (!value)
            return nullptr;
        auto *spill = builder_.CreateAlloca(value->getType());
        builder_.CreateStore(value, spill);
        base = spill;
    }
    return builder_.CreateStructGEP(typeGen_.lower(field.object_type), base, field.index);
}

llvm::Value *CodeGenEmit::emitLValueAddr(hir::HirExprId target_id, const hir::HirModule &mod) {
    auto &expr = mod.getExpr(target_id);
    return hir::visitExpr(
        expr,
        common::overloaded{
            [&](const hir::HirVar &var) -> llvm::Value * { return emitVarAddr(var); },
            [&](const hir::HirSlotAddr &s) -> llvm::Value * {
                if (s.slot >= slots_.size())
                    return nullptr;
                return slots_[s.slot];
            },
            [&](const hir::HirIndex &idx) -> llvm::Value * { return emitIndexAddr(idx, mod); },
            [&](const hir::HirField &field) -> llvm::Value * { return emitFieldAddr(field, mod); },
            [&](const hir::HirUnary &un) -> llvm::Value * {
                if (un.op == hir::HirUnaryOp::Deref) {
                    return emitExpr(un.operand, mod);
                }
                return nullptr;
            },
            [](const auto &) -> llvm::Value * { return nullptr; }});
}

} // namespace zith::codegen
