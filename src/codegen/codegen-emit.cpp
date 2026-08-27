#include "codegen-emit.hpp"

#include "common/overloaded.hpp"
#include "llvm/IR/CallingConv.h"

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
    if (auto *cached = emittedValues_.get(id))
        return *cached;

    auto &expr    = mod.getExpr(id);
    auto *emitted = hir::visitExpr(
        expr,
        common::overloaded{
            [&](const hir::HirLiteral &lit) { return emitLiteral(lit); },
            [&](const hir::HirBinary &bin) { return emitBinary(bin, mod); },
            [&](const hir::HirUnary &un) { return emitUnary(un, mod); },
            [&](const hir::HirLet &let) { return emitLet(let, mod); },
            [&](const hir::HirVar &var) { return emitVar(var); },
            [&](const hir::HirGlobalConstLoad &load) -> llvm::Value * {
                const auto name = interner_.lookup(load.name);
                auto *global =
                    module_ != nullptr
                        ? module_->getNamedGlobal(llvm::StringRef(name.data(), name.size()))
                        : nullptr;
                if (!global)
                    return nullptr;
                return builder_.CreateLoad(typeGen_.lower(load.type), global);
            },
            [&](const hir::HirCall &call) { return emitCall(call, mod); },
            [&](const hir::HirMakeDyn &make) { return emitMakeDyn(make, mod); },
            [&](const hir::HirDynCall &call) { return emitDynCall(call, mod); },
            [&](const hir::HirRet &ret) { return emitRet(ret, mod); },
            [&](const hir::HirStateTailCall &tail) { return emitStateTailCall(tail, mod); },
            [&](const hir::HirCleanup &cleanup) { return emitCleanup(cleanup, mod); },
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
                // storage and, for tagged unions, also stores the member tag.
                // It is represented by a temporary aggregate load, so rebuild
                // the store directly instead of letting LLVM reload the cast.
                if (union_storage != nullptr) {
                    llvm::Value *dest = builder_.CreateBitCast(
                        slots_[s.slot], llvm::PointerType::get(builder_.getContext(), 0));
                    builder_.CreateStore(
                        llvm::ConstantAggregateZero::get(typeGen_.lower(union_cast->to)),
                        slots_[s.slot]);
                    auto *to_type = typeGen_.lower(union_cast->to);
                    auto *payload = builder_.CreateStructGEP(to_type, dest, 0U);
                    builder_.CreateStore(
                        emitExpr(union_cast->value, mod),
                        builder_.CreateBitCast(payload,
                                               llvm::PointerType::get(builder_.getContext(), 0)));
                    if (union_cast->member_index != ~0U && [&]() {
                            const auto *ud =
                                std::get_if<types::TypeUnion>(&types_.lookup(union_cast->to));
                            const auto *def =
                                ud != nullptr ? types_.lookupUnionDef(ud->def_id) : nullptr;
                            return def != nullptr && def->is_tagged;
                        }()) {
                        auto *tag = builder_.CreateStructGEP(to_type, dest, 1U);
                        const auto *tag_def =
                            std::get_if<types::TypeUnion>(&types_.lookup(union_cast->to));
                        const auto *def =
                            tag_def != nullptr ? types_.lookupUnionDef(tag_def->def_id) : nullptr;
                        const auto tag_width =
                            def != nullptr && def->members.size() > 0xFFFFU
                                ? 32U
                                : (def != nullptr && def->members.size() > 0xFFU ? 16U : 8U);
                        builder_.CreateStore(builder_.getIntN(tag_width, union_cast->member_index),
                                             tag);
                    }
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
                const auto from_kind  = types_.kindOf(cast.from);
                const auto to_kind    = types_.kindOf(cast.to);
                const auto union_type = from_kind == types::TypeKind::Union ? cast.from : cast.to;
                auto *union_ll        = typeGen_.lower(union_type);
                const bool to_tagged  = [&]() {
                    if (to_kind != types::TypeKind::Union)
                        return false;
                    const auto *ud  = std::get_if<types::TypeUnion>(&types_.lookup(union_type));
                    const auto *def = ud != nullptr ? types_.lookupUnionDef(ud->def_id) : nullptr;
                    return def != nullptr && def->is_tagged;
                }();
                auto *value = emitExpr(cast.value, mod);
                if (!value)
                    return nullptr;
                llvm::Value *storage = nullptr;
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
                    if (to_tagged && cast.member_index != ~0U) {
                        auto *tag_type =
                            llvm::cast<llvm::IntegerType>(union_ll->getStructElementType(1));
                        auto *tag = builder_.CreateStructGEP(union_ll, storage, 1U);
                        builder_.CreateStore(llvm::ConstantInt::get(tag_type, cast.member_index),
                                             tag);
                    }
                }
                if (storage == nullptr)
                    return nullptr;
                if (to_kind == types::TypeKind::Union) {
                    return builder_.CreateLoad(
                        union_ll, builder_.CreateBitCast(
                                      storage, llvm::PointerType::get(builder_.getContext(), 0)));
                }
                if (from_kind == types::TypeKind::Union) {
                    const auto *from_union =
                        std::get_if<types::TypeUnion>(&types_.lookup(cast.from));
                    const auto *from_def =
                        from_union != nullptr ? types_.lookupUnionDef(from_union->def_id) : nullptr;
                    const bool from_tagged = from_def != nullptr && from_def->is_tagged;
                    if (from_tagged && cast.checked && cast.member_index != ~0U) {
                        auto *tag_addr = builder_.CreateStructGEP(union_ll, storage, 1U);
                        auto *tag      = builder_.CreateLoad(
                            llvm::cast<llvm::IntegerType>(union_ll->getStructElementType(1)),
                            tag_addr);
                        auto *expected = llvm::ConstantInt::get(
                            llvm::cast<llvm::IntegerType>(union_ll->getStructElementType(1)),
                            cast.member_index);
                        auto *match = builder_.CreateICmpEQ(tag, expected);
                        llvm::BasicBlock *fail =
                            llvm::BasicBlock::Create(builder_.getContext(), "union_tag_fail",
                                                     builder_.GetInsertBlock()->getParent());
                        llvm::BasicBlock *cont =
                            llvm::BasicBlock::Create(builder_.getContext(), "union_tag_ok",
                                                     builder_.GetInsertBlock()->getParent());
                        builder_.CreateCondBr(match, cont, fail);
                        builder_.SetInsertPoint(fail);
                        builder_.CreateUnreachable();
                        builder_.SetInsertPoint(cont);
                    }
                }
                auto *bytes = builder_.CreateStructGEP(union_ll, storage, 0U);
                return builder_.CreateLoad(
                    typeGen_.lower(cast.to),
                    builder_.CreateBitCast(bytes,
                                           llvm::PointerType::get(builder_.getContext(), 0)));
            },
            [&](const hir::HirUnionCheck &check) -> llvm::Value * {
                auto *tag = emitExpr(check.value, mod);
                if (tag == nullptr)
                    return nullptr;
                if (!tag->getType()->isIntegerTy())
                    return nullptr;
                auto *expected = llvm::ConstantInt::get(
                    llvm::cast<llvm::IntegerType>(tag->getType()), check.member_index);
                return builder_.CreateICmpEQ(tag, expected);
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
                const bool from_int    = from_kind == types::TypeKind::Int ||
                                      from_kind == types::TypeKind::Char ||
                                      from_kind == types::TypeKind::Enum;
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
            [&](const hir::HirMakeSlice &slice) -> llvm::Value * {
                auto *lo = emitExpr(slice.lo, mod);
                auto *hi = emitExpr(slice.hi, mod);
                if (!lo || !hi)
                    return nullptr;
                auto *i64 = llvm::Type::getInt64Ty(builder_.getContext());
                if (lo->getType()->isIntOrIntVectorTy() && !lo->getType()->isIntegerTy(64))
                    lo = builder_.CreateSExtOrTrunc(lo, i64);
                if (hi->getType()->isIntOrIntVectorTy() && !hi->getType()->isIntegerTy(64))
                    hi = builder_.CreateSExtOrTrunc(hi, i64);

                const auto *elem_type = std::get_if<types::TypeSlice>(&types_.lookup(slice.type));
                const auto elem_ll =
                    elem_type != nullptr ? typeGen_.lower(elem_type->elem) : nullptr;
                llvm::Value *data    = nullptr;
                llvm::Value *len_val = nullptr;
                if (slice.is_array) {
                    auto *addr = emitAddrOf(slice.object, mod);
                    if (!addr || !elem_ll)
                        return nullptr;
                    auto *zero = llvm::ConstantInt::get(builder_.getContext(), llvm::APInt(32, 0));
                    data = builder_.CreateGEP(typeGen_.lower(slice.object_type), addr, {zero, lo});
                } else if (slice.is_pointer) {
                    // A raw pointer slice reinterprets C-owned storage as a
                    // pointer/length view; the pointer itself is the data slot.
                    data = emitExpr(slice.object, mod);
                    if (!data || !elem_ll)
                        return nullptr;
                    data = builder_.CreateGEP(elem_ll, data, lo);
                } else {
                    auto *agg = emitExpr(slice.object, mod);
                    if (!agg)
                        return nullptr;
                    data = builder_.CreateExtractValue(agg, {0U});
                    if (!elem_ll)
                        return nullptr;
                    data = builder_.CreateGEP(elem_ll, data, lo);
                }
                if (len_val == nullptr)
                    len_val = builder_.CreateSub(hi, lo);
                auto *agg_type      = typeGen_.lower(slice.type);
                llvm::Value *result = llvm::ConstantAggregateZero::get(agg_type);
                result              = builder_.CreateInsertValue(result, data, {0U});
                result              = builder_.CreateInsertValue(result, len_val, {1U});
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
        });
    if (emitted != nullptr)
        emittedValues_.insert(id, emitted);
    return emitted;
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
        emittedValues_.clear();

        for (auto inst_id : block.insts) {
            last = emitExpr(inst_id, mod);
        }
        if (block.terminator != hir::kInvalidHirExpr) {
            emitExpr(block.terminator, mod);
        }
    }
    return last;
}

llvm::Value *CodeGenEmit::emitStateTailCall(const hir::HirStateTailCall &tail,
                                            const hir::HirModule &mod) {
    if (tail.call.resolved_fn == symbols::kInvalidSym || tail.call.callee != hir::kInvalidHirExpr)
        return nullptr;

    llvm::Function *fn = nullptr;
    for (size_t i = 0; i < mod.getFnCount(); ++i) {
        if (mod.getFn(i).sym_id != tail.call.resolved_fn)
            continue;
        const auto name = interner_.lookup(mod.getFn(i).name);
        fn              = module_->getFunction(llvm::StringRef(name.data(), name.size()));
        break;
    }
    if (fn == nullptr)
        return nullptr;

    llvm::SmallVector<llvm::Value *, 8> args;
    for (auto arg_id : tail.call.args) {
        auto *value = emitExpr(arg_id, mod);
        if (value == nullptr)
            return nullptr;
        args.push_back(value);
    }

    const auto *current = builder_.GetInsertBlock();
    if (current == nullptr || current->getTerminator() != nullptr ||
        fn->arg_size() != args.size() || fn->isVarArg()) {
        return nullptr;
    }

    const auto *current_fn = current->getParent();
    if (current_fn == nullptr || fn->getReturnType() != current_fn->getReturnType()) {
        return nullptr;
    }

    auto *call = builder_.CreateCall(fn, args);
    call->setTailCallKind(llvm::CallInst::TCK_MustTail);
    call->setCallingConv(llvm::CallingConv::Tail);
    if (fn->getReturnType()->isVoidTy())
        return builder_.CreateRetVoid();
    return builder_.CreateRet(call);
}

llvm::Value *CodeGenEmit::emitCleanup(const hir::HirCleanup &cleanup, const hir::HirModule &mod) {
    llvm::Value *last = nullptr;
    for (const auto expr_id : cleanup.exprs) {
        auto *value = emitExpr(expr_id, mod);
        if (value != nullptr)
            last = value;
    }
    return last;
}

void CodeGenEmit::registerParams(const hir::HirFunction &fn, llvm::Function *llvmFn,
                                 const hir::HirModule &mod) {
    auto argIt = llvmFn->arg_begin();
    for (size_t i = 0; i < fn.param_names.size() && argIt != llvmFn->arg_end(); i++, ++argIt) {
        auto paramName = interner_.lookup(fn.param_names[i]);
        argIt->setName(llvm::StringRef(paramName.data(), paramName.size()));
        if (i < fn.param_slots.size()) {
            const auto *slotAttrs =
                mod.attrs().trySlot(static_cast<hir::HirSlotId>(fn.param_slots[i]));
            const bool borrow_attr =
                slotAttrs != nullptr && (slotAttrs->ownership == hir::HirOwnership::Lend ||
                                         slotAttrs->ownership == hir::HirOwnership::View);
            const bool is_pointer = argIt->getType()->isPointerTy();
            // Borrow facts only apply when the ABI passed a pointer for this
            // parameter. Generic/interface arguments can be by-value aggregates
            // with a residual qualifier; applying readonly/nocapture there is an
            // LLVM type error.
            if (borrow_attr && is_pointer) {
                argIt->addAttr(llvm::Attribute::getWithCaptureInfo(argIt->getContext(),
                                                                   llvm::CaptureInfo::none()));
                if (slotAttrs->ownership == hir::HirOwnership::View)
                    argIt->addAttr(llvm::Attribute::ReadOnly);
            }
        }
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
    const auto kind = types_.kindOf(id);
    if (kind == types::TypeKind::Enum) {
        return isSignedType(types_.getEnumDef(id).underlying);
    }
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
    llvm::Function *fn = nullptr;
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
    // Imported externs and callable fields may be represented only as a callee
    // value. Resolve through the linkage name when emitCall has a callee var
    // and no symbol target.
    if (fn == nullptr && call.callee != hir::kInvalidHirExpr) {
        if (const auto *as_var = std::get_if<hir::HirVar>(&mod.getExpr(call.callee)))
            fn = module->getFunction(llvm::StringRef(interner_.lookup(as_var->name).data(),
                                                     interner_.lookup(as_var->name).size()));
    }

    if (fn == nullptr && call.fn_type == types::kInvalidType) {
        llvm::errs() << "ERROR: emitCall failed to resolve callee (resolved_fn=" << call.resolved_fn
                     << ")\n";
        return nullptr;
    }

    // C default argument promotions apply only to the variadic tail: float -> double,
    // and bool/char/small ints -> int. Fixed parameters keep their declared ABI types.
    llvm::FunctionType *fn_type = nullptr;
    if (fn != nullptr) {
        fn_type = fn->getFunctionType();
    } else {
        const auto *lowered = std::get_if<types::TypeFn>(&types_.lookup(call.fn_type));
        if (lowered == nullptr) {
            llvm::errs() << "ERROR: emitCall has an indirect callee without a function type\n";
            return nullptr;
        }
        llvm::SmallVector<llvm::Type *, 8> param_types;
        for (size_t index = 0; index < lowered->param_count; ++index)
            param_types.push_back(typeGen_.lower(lowered->params[index]));
        fn_type = llvm::FunctionType::get(typeGen_.lower(lowered->ret), param_types, false);
    }
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

    if (fn == nullptr) {
        auto *callee = emitExpr(call.callee, mod);
        if (callee == nullptr)
            return nullptr;
        auto *fn_ptr_type = llvm::PointerType::get(builder_.getContext(),
                                                   callee->getType()->getPointerAddressSpace());
        if (callee->getType() != fn_ptr_type)
            callee = builder_.CreateBitCast(callee, fn_ptr_type);
        return builder_.CreateCall(fn_type, callee, args);
    }

    const bool tailcc =
        call.usesTailCC || (fn != nullptr && fn->getCallingConv() == llvm::CallingConv::Tail);
    auto *llvm_call = builder_.CreateCall(fn, args);
    if (tailcc)
        llvm_call->setCallingConv(llvm::CallingConv::Tail);
    return llvm_call;
}

llvm::Value *CodeGenEmit::emitMakeDyn(const hir::HirMakeDyn &make, const hir::HirModule &mod) {
    auto *value = emitExpr(make.value, mod);
    if (!value)
        return nullptr;

    auto *dyn_type = typeGen_.lower(make.dyn_type);
    if (!dyn_type || !dyn_type->isStructTy() ||
        llvm::cast<llvm::StructType>(dyn_type)->getNumElements() != 2U)
        return nullptr;

    // Keep a stable address for the concrete aggregate. HIR uses memory slots
    // for normal locals already, but literals and call/load expressions may be
    // register values; spill them here so the data pointer outlives the call.
    auto *storage = builder_.CreateAlloca(value->getType());
    builder_.CreateStore(value, storage);
    auto *data =
        builder_.CreateBitCast(storage, llvm::PointerType::get(builder_.getContext(), 0));

    const auto name = interner_.lookup(make.vtable_name);
    auto *global =
        module_ != nullptr
            ? module_->getNamedGlobal(llvm::StringRef(name.data(), name.size()))
            : nullptr;
    if (global == nullptr)
        return nullptr;
    auto *data_field = builder_.CreateInsertValue(
        llvm::UndefValue::get(dyn_type), data, {0U});
    auto *vtable =
        builder_.CreateBitCast(global, llvm::PointerType::get(builder_.getContext(), 0));
    return builder_.CreateInsertValue(data_field, vtable, {1U});
}

llvm::Value *CodeGenEmit::emitDynCall(const hir::HirDynCall &call, const hir::HirModule &mod) {
    auto *receiver = emitExpr(call.receiver, mod);
    if (!receiver || !receiver->getType()->isStructTy())
        return nullptr;

    auto *data = builder_.CreateExtractValue(receiver, {0U});
    auto *vtable = builder_.CreateExtractValue(receiver, {1U});
    if (!vtable)
        return nullptr;
    auto *slot_addr = builder_.CreateGEP(
        llvm::ArrayType::get(llvm::PointerType::get(builder_.getContext(), 0),
                             std::max<size_t>(static_cast<size_t>(call.slot_index) + 1U, 1U)),
        vtable, {builder_.getInt32(0), builder_.getInt32(static_cast<uint32_t>(call.slot_index))});
    auto *fn_ptr =
        builder_.CreateLoad(llvm::PointerType::get(builder_.getContext(), 0), slot_addr);
    if (!fn_ptr)
        return nullptr;

    const auto *fn_type = std::get_if<types::TypeFn>(&types_.lookup(call.fn_type));
    if (fn_type == nullptr)
        return nullptr;
    llvm::SmallVector<llvm::Type *, 8> param_types;
    if (call.has_receiver)
        param_types.push_back(llvm::PointerType::get(builder_.getContext(), 0));
    for (size_t index = 0; index < fn_type->param_count; ++index)
        param_types.push_back(typeGen_.lower(fn_type->params[index]));
    auto *llvm_fn_type =
        llvm::FunctionType::get(typeGen_.lower(call.result_type), param_types, false);

    llvm::SmallVector<llvm::Value *, 8> args;
    if (call.has_receiver)
        args.push_back(data);
    for (auto arg_id : call.args) {
        auto *value = emitExpr(arg_id, mod);
        if (!value)
            return nullptr;
        args.push_back(value);
    }
    return builder_.CreateCall(llvm_fn_type, fn_ptr, args);
}

llvm::Value *CodeGenEmit::emitRet(const hir::HirRet &ret, const hir::HirModule &mod) {
    if (ret.value == hir::kInvalidHirExpr)
        return builder_.CreateRetVoid();
    // The implicit-return path lowers the trailing expression both as an
    // instruction and as the Ret value. Reuse the value produced earlier in the
    // same block when available; the block-scoped cache avoids re-evaluating
    // calls while never reusing values from a different CFG edge.
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
    // Function references lower to HirVar using their linkage name, so a direct
    // module lookup lets them participate as first-class function-pointer values.
    if (module_ != nullptr) {
        if (auto *fn = module_->getFunction(llvm::StringRef(name.data(), name.size())))
            return fn;
    }
    return nullptr;
}

llvm::Value *CodeGenEmit::emitVarAddr(const hir::HirVar &var) {
    auto name = interner_.lookup(var.name);
    auto *nv  = namedValues_.get(name);
    if (nv)
        return nv->value;
    if (module_ != nullptr) {
        if (auto *fn = module_->getFunction(llvm::StringRef(name.data(), name.size())))
            return fn;
    }
    return nullptr;
}

llvm::Value *CodeGenEmit::emitJump(const hir::HirJump &jump, const hir::HirModule &mod) {
    (void)mod;
    if (!blocks_ || jump.target >= blocks_->size())
        return nullptr;
    auto *target = (*blocks_)[jump.target];
    // Ordinary jumps are terminator-only edges between lowered CFG blocks.
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
