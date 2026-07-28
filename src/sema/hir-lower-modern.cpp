#include "sema/hir-lower-modern.hpp"

#include "common/overloaded.hpp"
#include "diagnostics/error-codes.hpp"
#include "types/type-kind.hpp"

#include <cctype>
#include <cstdlib>
#include <span>
#include <string>

namespace zith::sema::modern {

namespace {

hir::HirBinaryOp mapBinaryOp(std::string_view text) {
    if (text == "+")
        return hir::HirBinaryOp::Add;
    if (text == "-")
        return hir::HirBinaryOp::Sub;
    if (text == "*")
        return hir::HirBinaryOp::Mul;
    if (text == "/")
        return hir::HirBinaryOp::Div;
    if (text == "%")
        return hir::HirBinaryOp::Rem;
    if (text == "==")
        return hir::HirBinaryOp::Eq;
    if (text == "!=")
        return hir::HirBinaryOp::Ne;
    if (text == "<")
        return hir::HirBinaryOp::Lt;
    if (text == "<=")
        return hir::HirBinaryOp::Le;
    if (text == ">")
        return hir::HirBinaryOp::Gt;
    if (text == ">=")
        return hir::HirBinaryOp::Ge;
    if (text == "and")
        return hir::HirBinaryOp::And;
    if (text == "or")
        return hir::HirBinaryOp::Or;
    if (text == "xor")
        return hir::HirBinaryOp::Xor;
    if (text == "<<")
        return hir::HirBinaryOp::Shl;
    if (text == ">>")
        return hir::HirBinaryOp::Shr;
    return hir::HirBinaryOp::Add;
}

hir::HirUnaryOp mapUnaryOp(std::string_view text) {
    if (text == "-")
        return hir::HirUnaryOp::Neg;
    if (text == "!" || text == "not")
        return hir::HirUnaryOp::Not;
    return hir::HirUnaryOp::Neg;
}

types::IntWidth mapIntegerWidth(const IntegerType &integer) {
    if (integer.bits == 8)
        return integer.isSigned ? types::IntWidth::I8 : types::IntWidth::U8;
    if (integer.bits == 16)
        return integer.isSigned ? types::IntWidth::I16 : types::IntWidth::U16;
    if (integer.bits == 32)
        return integer.isSigned ? types::IntWidth::I32 : types::IntWidth::U32;
    if (integer.bits == 64)
        return integer.isSigned ? types::IntWidth::I64 : types::IntWidth::U64;
    if (integer.bits == 128)
        return integer.isSigned ? types::IntWidth::I128 : types::IntWidth::U128;
    return types::IntWidth::Literal;
}

types::FloatWidth mapFloatWidth(const FloatType &floating) {
    if (floating.bits <= 32)
        return types::FloatWidth::F32;
    if (floating.bits <= 64)
        return types::FloatWidth::F64;
    return types::FloatWidth::F128;
}

std::string functionKey(std::string_view module, frontend::DeclId decl) {
    return std::string(module) + "#" + std::to_string(decl.value);
}

} // namespace

HirLowerModern::HirLowerModern(memory::Arena &arena, diagnostics::DiagnosticEngine &diags,
                               const session::CompilationSnapshot &snapshot,
                               const SemaPipeline &sema, types::TypeIntern &types,
                               memory::StringInterner &interner)
    : arena_(arena), diags_(diags), snapshot_(snapshot), sema_(sema), types_(types),
      interner_(interner), hir_(arena), lowered_types_() {}

bool HirLowerModern::run() {
    return predeclareFunctions() && lowerFunctionBodies() && !diags_.hasErrors();
}

bool HirLowerModern::predeclareFunctions() {
    for (const auto &module_ptr : snapshot_.modules()) {
        const auto &module = *module_ptr;
        auto *module_sema  = sema_.findModuleSema(module.key);
        if (module_sema == nullptr)
            continue;

        for (const auto &decl : module.frontend->declarations()) {
            if (decl.kind != frontend::DeclKind::Function || decl.name.empty())
                continue;

            auto &hir_fn   = hir_.addFn(interner_.intern(decl.name));
            hir_fn.sym_id  = static_cast<symbols::SymId>(functions_.size() + next_sym_id_);
            hir_fn.decl_id = static_cast<ast::DeclId>(decl.id.value);

            const auto fn_type = module_sema->typeOfDecl(decl.id);
            if (const auto *fn = sema_.typeTable().function(fn_type)) {
                hir_fn.return_type = lowerType(fn->result);
                for (size_t index = 0; index < decl.parameters.size(); ++index) {
                    const auto &parameter = decl.parameters[index];
                    const auto param_type = index < fn->params.size() ? lowerType(fn->params[index])
                                                                      : types::kErrorType;
                    hir_fn.params.push(param_type);
                    hir_fn.param_names.push(interner_.intern(parameter.name));
                }
            } else {
                hir_fn.return_type = types::kVoidType;
                for (const auto &parameter : decl.parameters) {
                    hir_fn.params.push(types::kErrorType);
                    hir_fn.param_names.push(interner_.intern(parameter.name));
                }
            }

            functions_.push_back({functionKey(module.key, decl.id), module_ptr.get(), &decl,
                                  nullptr, hir_fn.sym_id, hir_.getFnCount() - 1U});
        }
    }
    for (const auto &header : snapshot_.cHeaders()) {
        if (header == nullptr)
            continue;
        for (const auto &foreign : header->functions) {
            auto &hir_fn       = hir_.addFn(interner_.intern(foreign.linkageName));
            hir_fn.sym_id      = static_cast<symbols::SymId>(functions_.size() + next_sym_id_);
            hir_fn.return_type = lowerForeignType(foreign.result);
            for (const auto &parameter : foreign.parameters) {
                hir_fn.params.push(lowerForeignType(parameter));
                hir_fn.param_names.push({});
            }
            functions_.push_back({foreign.linkageName, nullptr, nullptr, &foreign, hir_fn.sym_id,
                                  hir_.getFnCount() - 1U});
        }
    }
    return true;
}

bool HirLowerModern::lowerFunctionBodies() {
    for (auto &function : functions_) {
        if (function.decl != nullptr && function.decl->body && !lowerFunctionBody(function))
            return false;
    }
    return !diags_.hasErrors();
}

bool HirLowerModern::lowerFunctionBody(FunctionInfo &info) {
    current_module_     = info.module;
    current_resolution_ = snapshot_.findResolution(info.module->key);
    current_types_      = sema_.findTypedMap(info.module->key);
    if (current_resolution_ == nullptr || current_types_ == nullptr) {
        diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                      "missing semantic data for module '" + info.module->key + "'", {});
        return false;
    }

    current_fn_    = &hir_.getFn(info.hir_index);
    current_block_ = 0;
    next_slot_     = 0;
    loop_stack_.clear();
    local_slots_.clear();
    local_slots_.resize(1U);

    current_fn_->blocks.emplace(arena_);
    current_fn_->blocks[0].insts = memory::DynArray<hir::HirExprId>(arena_);

    for (size_t index = 0; index < info.decl->parameters.size(); ++index) {
        const auto &parameter = info.decl->parameters[index];
        const auto slot       = localSlot(parameter.id);
        current_fn_->blocks[0].insts.push(emitSlotAlloca(slot, current_fn_->params[index]));

        hir::HirVar param_var;
        param_var.name        = current_fn_->param_names[index];
        param_var.version     = 0;
        const auto param_expr = addExpr(std::move(param_var));
        current_fn_->blocks[0].insts.push(emitSlotStore(slot, param_expr));
    }

    const auto body_expr = lowerExpr(info.decl->body);
    if (current_fn_->blocks[current_block_].terminator == hir::kInvalidHirExpr) {
        hir::HirRet ret;
        if (current_fn_->return_type != types::kVoidType && body_expr != hir::kInvalidHirExpr)
            ret.value = body_expr;
        current_fn_->blocks[current_block_].terminator = addExpr(std::move(ret));
    }

    current_module_     = nullptr;
    current_resolution_ = nullptr;
    current_types_      = nullptr;
    current_fn_         = nullptr;
    return true;
}

types::TypeId HirLowerModern::lowerType(sema::modern::TypeId type) {
    if (!type)
        return types::kErrorType;
    if (const auto *cached = lowered_types_.get(type.intern_seq))
        return *cached;

    types::TypeId lowered = types::kErrorType;
    switch (sema_.typeTable().kindOf(type)) {
    case TypeKind::Error:
        lowered = types::kErrorType;
        break;
    case TypeKind::Void:
        lowered = types::kVoidType;
        break;
    case TypeKind::Never:
        lowered = types::kNeverType;
        break;
    case TypeKind::Bool:
        lowered = types::kBoolType;
        break;
    case TypeKind::Char:
        lowered = types::kCharType;
        break;
    case TypeKind::Integer: {
        const auto *integer = sema_.typeTable().integer(type);
        lowered =
            integer != nullptr ? types_.internInt(mapIntegerWidth(*integer)) : types::kErrorType;
        break;
    }
    case TypeKind::Float: {
        const auto *floating = sema_.typeTable().float_kind(type);
        lowered =
            floating != nullptr ? types_.internFloat(mapFloatWidth(*floating)) : types::kErrorType;
        break;
    }
    case TypeKind::String:
        lowered = types_.internPtr(types::kCharType);
        break;
    case TypeKind::Pointer: {
        const auto *pointer = sema_.typeTable().pointer(type);
        lowered =
            pointer != nullptr ? types_.internPtr(lowerType(pointer->pointee)) : types::kErrorType;
        break;
    }
    case TypeKind::Optional: {
        const auto *optional = sema_.typeTable().optional(type);
        lowered = optional != nullptr ? types_.internOptional(lowerType(optional->inner))
                                      : types::kErrorType;
        break;
    }
    case TypeKind::Array: {
        const auto *array = sema_.typeTable().array(type);
        lowered           = array != nullptr ? types_.internArray(lowerType(array->element),
                                                                  static_cast<uint32_t>(array->size))
                                             : types::kErrorType;
        break;
    }
    case TypeKind::Function: {
        const auto *fn = sema_.typeTable().function(type);
        if (fn == nullptr) {
            lowered = types::kErrorType;
            break;
        }
        std::vector<types::TypeId> params;
        params.reserve(fn->params.size());
        for (const auto param : fn->params)
            params.push_back(lowerType(param));
        lowered = types_.internFn(std::span<const types::TypeId>(params.data(), params.size()),
                                  lowerType(fn->result));
        break;
    }
    case TypeKind::Struct: {
        const auto *structure = sema_.typeTable().struct_type(type);
        lowered               = structure != nullptr
                                    ? types_.registerNamedType(structure->name, types::TypeKind::Struct)
                                    : types::kErrorType;
        break;
    }
    case TypeKind::Enum: {
        const auto *enumeration = sema_.typeTable().enum_type(type);
        lowered                 = enumeration != nullptr
                                      ? types_.registerNamedType(enumeration->name, types::TypeKind::Enum)
                                      : types::kErrorType;
        break;
    }
    case TypeKind::Union: {
        const auto *union_type = sema_.typeTable().union_type(type);
        lowered                = union_type != nullptr
                                     ? types_.registerNamedType(union_type->name, types::TypeKind::Union)
                                     : types::kErrorType;
        break;
    }
    case TypeKind::Trait:
    case TypeKind::TypeVar:
    case TypeKind::Unknown:
        lowered = types_.internUnknown();
        break;
    case TypeKind::Incomplete: {
        const auto *incomplete = sema_.typeTable().incomplete(type);
        if (incomplete == nullptr) {
            lowered = types::kErrorType;
            break;
        }
        std::vector<types::TypeId> args;
        args.reserve(incomplete->args.size());
        for (const auto arg : incomplete->args)
            args.push_back(lowerType(arg));
        lowered = types_.internIncomplete(lowerType(incomplete->base),
                                          std::span<const types::TypeId>(args.data(), args.size()));
        break;
    }
    case TypeKind::Sum: {
        const auto *sum = sema_.typeTable().sum(type);
        if (sum == nullptr) {
            lowered = types::kErrorType;
            break;
        }
        std::vector<types::TypeId> members;
        members.reserve(sum->members.size());
        for (const auto member : sum->members)
            members.push_back(lowerType(member));
        lowered = types_.internSum(std::span<const types::TypeId>(members.data(), members.size()));
        break;
    }
    case TypeKind::Slice: {
        const auto *slice = sema_.typeTable().slice(type);
        lowered =
            slice != nullptr ? types_.internSlice(lowerType(slice->element)) : types::kErrorType;
        break;
    }
    case TypeKind::Failable: {
        const auto *failable = sema_.typeTable().failable(type);
        lowered = failable != nullptr ? types_.internFailable(lowerType(failable->inner))
                                      : types::kErrorType;
        break;
    }
    case TypeKind::Pack: {
        const auto *pack = sema_.typeTable().pack(type);
        if (pack == nullptr) {
            lowered = types::kErrorType;
            break;
        }
        std::vector<types::TypeId> members;
        std::vector<std::string_view> names;
        members.reserve(pack->members.size());
        names.reserve(pack->names.size());
        for (const auto member : pack->members)
            members.push_back(lowerType(member));
        for (const auto name : pack->names)
            names.push_back(name);
        lowered = types_.internPack(std::span<const types::TypeId>(members.data(), members.size()),
                                    std::span<const std::string_view>(names.data(), names.size()));
        break;
    }
    case TypeKind::Alias: {
        const auto *alias = sema_.typeTable().alias(type);
        lowered           = alias != nullptr ? lowerType(alias->target) : types::kErrorType;
        break;
    }
    }

    lowered_types_.insert(type.intern_seq, lowered);
    return lowered;
}

types::TypeId HirLowerModern::lowerForeignType(const cinterop::Type &type) {
    switch (type.kind) {
    case cinterop::TypeKind::Void:
        return types::kVoidType;
    case cinterop::TypeKind::Bool:
        return types::kBoolType;
    case cinterop::TypeKind::Integer:
        return types_.internInt(type.isSigned ? mapIntegerWidth({type.bits, true})
                                              : mapIntegerWidth({type.bits, false}));
    case cinterop::TypeKind::Float:
        return types_.internFloat(mapFloatWidth({type.bits}));
    case cinterop::TypeKind::Pointer:
        return types_.internPtr(type.pointee ? lowerForeignType(*type.pointee) : types::kErrorType);
    case cinterop::TypeKind::Record:
        return types_.registerNamedType(type.name, types::TypeKind::Struct);
    case cinterop::TypeKind::Enum:
        return types_.registerNamedType(type.name, types::TypeKind::Enum);
    }
    return types::kErrorType;
}

types::TypeId HirLowerModern::typeOfExpr(frontend::ExprId id) {
    if (!id || current_types_ == nullptr)
        return types::kErrorType;
    const auto *type = current_types_->exprTypes.get(id.value);
    return type != nullptr ? lowerType(*type) : types::kErrorType;
}

types::TypeId HirLowerModern::typeOfLocal(frontend::LocalId id) {
    if (!id || current_types_ == nullptr)
        return types::kErrorType;
    const auto *type = current_types_->localTypes.get(id.value);
    return type != nullptr ? lowerType(*type) : types::kErrorType;
}

const frontend::Declaration *HirLowerModern::findDecl(const session::ModuleArtifact &module,
                                                      frontend::DeclId id) const noexcept {
    if (!id || module.frontend == nullptr || id.value > module.frontend->declarations().size())
        return nullptr;
    return &module.frontend->declarations()[id.value - 1U];
}

const session::ResolvedName *HirLowerModern::findResolvedExpr(frontend::ExprId id) const noexcept {
    if (current_module_ == nullptr || current_resolution_ == nullptr || !id ||
        id.value > current_module_->frontend->expressions().size()) {
        return nullptr;
    }

    const auto &expr = current_module_->frontend->expressions()[id.value - 1U];
    for (const auto &resolved : current_resolution_->expressions) {
        if (resolved.span.start == expr.span.start && resolved.span.end == expr.span.end)
            return &resolved;
    }
    return nullptr;
}

const frontend::Declaration *
HirLowerModern::resolvedFunctionDecl(const session::ResolvedName &resolved,
                                     const session::ModuleArtifact **module_out) const noexcept {
    const session::ModuleArtifact *module = current_module_;
    frontend::DeclId decl                 = resolved.declaration;
    if (!resolved.target.module.empty()) {
        module = snapshot_.findModule(resolved.target.module);
        if (!decl && resolved.target.localSymbol)
            decl = frontend::DeclId{resolved.target.localSymbol.value};
    }
    if (module_out != nullptr)
        *module_out = module;
    if (module == nullptr)
        return nullptr;
    return findDecl(*module, decl);
}

symbols::SymId
HirLowerModern::resolvedFunctionSym(const session::ResolvedName &resolved) const noexcept {
    if (resolved.foreignFunction != nullptr) {
        for (const auto &function : functions_) {
            if (function.foreign == resolved.foreignFunction)
                return function.sym_id;
        }
        return symbols::kInvalidSym;
    }
    const session::ModuleArtifact *module = nullptr;
    const auto *decl                      = resolvedFunctionDecl(resolved, &module);
    if (decl == nullptr || module == nullptr)
        return symbols::kInvalidSym;
    const auto key = functionKey(module->key, decl->id);
    for (const auto &function : functions_) {
        if (function.key == key)
            return function.sym_id;
    }
    return symbols::kInvalidSym;
}

hir::HirSlotId HirLowerModern::localSlot(frontend::LocalId id) {
    if (!id)
        return hir::kInvalidHirSlot;
    if (id.value >= local_slots_.size())
        local_slots_.resize(id.value + 1U, hir::kInvalidHirSlot);
    if (local_slots_[id.value] == hir::kInvalidHirSlot)
        local_slots_[id.value] = next_slot_++;
    return local_slots_[id.value];
}

hir::HirExprId HirLowerModern::lowerExpr(frontend::ExprId id) {
    if (!id || current_module_ == nullptr ||
        id.value > current_module_->frontend->expressions().size())
        return hir::kInvalidHirExpr;

    const auto &expr = current_module_->frontend->expressions()[id.value - 1U];
    const auto type  = typeOfExpr(id);
    switch (expr.kind) {
    case frontend::ExprKind::Literal:
        return lowerLiteral(expr, type);
    case frontend::ExprKind::Name:
        return lowerName(expr);
    case frontend::ExprKind::Unary:
        return lowerUnary(expr, type);
    case frontend::ExprKind::Binary:
        return lowerBinary(expr, type);
    case frontend::ExprKind::Call:
        return lowerCall(expr);
    case frontend::ExprKind::Block:
        return lowerBlock(expr);
    case frontend::ExprKind::If:
        return lowerIf(expr, type);
    case frontend::ExprKind::While:
        return lowerWhile(expr);
    case frontend::ExprKind::Assign:
        return lowerAssign(expr, type);
    case frontend::ExprKind::Return:
    case frontend::ExprKind::Error:
        return hir::kInvalidHirExpr;
    }
    return hir::kInvalidHirExpr;
}

hir::HirExprId HirLowerModern::lowerLiteral(const frontend::Expression &expr,
                                            const types::TypeId type) {
    hir::HirLiteral literal{};
    literal.type = type;
    switch (types_.kindOf(type)) {
    case types::TypeKind::Bool:
        literal.b = expr.text == "true";
        break;
    case types::TypeKind::Float:
        literal.f = std::strtod(expr.text.c_str(), nullptr);
        break;
    case types::TypeKind::Ptr: {
        auto text = std::string_view(expr.text);
        if (text.size() >= 2U && text.front() == '"' && text.back() == '"')
            text = text.substr(1U, text.size() - 2U);
        literal.str_val = interner_.intern(text);
        break;
    }
    default:
        literal.i = std::strtoll(expr.text.c_str(), nullptr, 10);
        break;
    }
    return addExpr(std::move(literal));
}

hir::HirExprId HirLowerModern::lowerName(const frontend::Expression &expr) {
    for (const auto &binding : current_resolution_->bindings) {
        if (binding.name != expr.text)
            continue;
        if (binding.local) {
            const auto slot = localSlot(binding.local);
            return emitSlotLoad(slot, typeOfLocal(binding.local));
        }
    }

    if (const auto *resolved = findResolvedExpr(expr.id)) {
        if (resolved->local) {
            const auto slot = localSlot(resolved->local);
            return emitSlotLoad(slot, typeOfLocal(resolved->local));
        }
        if (resolved->foreignFunction != nullptr) {
            hir::HirVar var;
            var.name    = interner_.intern(resolved->foreignFunction->linkageName);
            var.version = 0;
            return addExpr(std::move(var));
        }
        if (const auto *decl = resolvedFunctionDecl(*resolved)) {
            hir::HirVar var;
            var.name    = interner_.intern(decl->name);
            var.version = 0;
            return addExpr(std::move(var));
        }
    }

    hir::HirVar var;
    var.name    = interner_.intern(expr.text);
    var.version = 0;
    return addExpr(std::move(var));
}

hir::HirExprId HirLowerModern::lowerUnary(const frontend::Expression &expr,
                                          const types::TypeId type) {
    if (expr.operands.empty())
        return hir::kInvalidHirExpr;
    const auto operand = lowerExpr(expr.operands[0]);
    if (operand == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;

    hir::HirUnary unary;
    unary.op      = mapUnaryOp(expr.text);
    unary.operand = operand;
    unary.type    = type;
    return addExpr(std::move(unary));
}

hir::HirExprId HirLowerModern::lowerBinary(const frontend::Expression &expr,
                                           const types::TypeId type) {
    if (expr.operands.size() != 2U)
        return hir::kInvalidHirExpr;
    const auto lhs = lowerExpr(expr.operands[0]);
    const auto rhs = lowerExpr(expr.operands[1]);
    if (lhs == hir::kInvalidHirExpr || rhs == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;

    hir::HirBinary binary;
    binary.lhs  = lhs;
    binary.rhs  = rhs;
    binary.op   = mapBinaryOp(expr.text);
    binary.type = type;
    return addExpr(std::move(binary));
}

hir::HirExprId HirLowerModern::lowerCall(const frontend::Expression &expr) {
    if (expr.operands.empty())
        return hir::kInvalidHirExpr;

    const auto callee = lowerExpr(expr.operands[0]);
    if (callee == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;

    memory::DynArray<hir::HirExprId> args(arena_);
    for (size_t index = 1; index < expr.operands.size(); ++index) {
        const auto argument = lowerExpr(expr.operands[index]);
        if (argument != hir::kInvalidHirExpr)
            args.push(argument);
    }

    hir::HirCall call{callee, std::move(args)};
    if (const auto *resolved = findResolvedExpr(expr.operands[0]))
        call.resolved_fn = resolvedFunctionSym(*resolved);
    return addExpr(std::move(call));
}

hir::HirExprId HirLowerModern::lowerBlock(const frontend::Expression &expr) {
    hir::HirExprId last = hir::kInvalidHirExpr;
    for (const auto statement : expr.statements) {
        if (current_fn_->blocks[current_block_].terminator != hir::kInvalidHirExpr)
            break;
        if (!lowerStatement(statement, last))
            return hir::kInvalidHirExpr;
    }
    return last;
}

hir::HirExprId HirLowerModern::lowerIf(const frontend::Expression &expr, const types::TypeId type) {
    if (expr.operands.size() < 2U)
        return hir::kInvalidHirExpr;

    const auto cond = lowerExpr(expr.operands[0]);
    if (cond == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;

    const bool has_else    = expr.operands.size() >= 3U;
    const bool has_value   = has_else && type != types::kVoidType && type != types::kErrorType;
    const auto result_slot = has_value ? next_slot_++ : hir::kInvalidHirSlot;
    if (has_value)
        current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(result_slot, type));

    const auto then_block  = newBlock();
    const auto else_block  = newBlock();
    const auto merge_block = newBlock();

    hir::HirBranch branch;
    branch.cond       = cond;
    branch.then_block = static_cast<hir::HirDeclId>(then_block);
    branch.else_block = static_cast<hir::HirDeclId>(has_else ? else_block : merge_block);
    setTerminator(addExpr(std::move(branch)));

    setCurrentBlock(then_block);
    current_fn_->blocks[then_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    const auto then_value                 = lowerExpr(expr.operands[1]);
    if (has_value && then_value != hir::kInvalidHirExpr &&
        current_fn_->blocks[then_block].terminator == hir::kInvalidHirExpr) {
        current_fn_->blocks[then_block].insts.push(emitSlotStore(result_slot, then_value));
    }
    if (current_fn_->blocks[then_block].terminator == hir::kInvalidHirExpr)
        emitJump(merge_block);

    if (has_else) {
        setCurrentBlock(else_block);
        current_fn_->blocks[else_block].insts = memory::DynArray<hir::HirExprId>(arena_);
        const auto else_value                 = lowerExpr(expr.operands[2]);
        if (has_value && else_value != hir::kInvalidHirExpr &&
            current_fn_->blocks[else_block].terminator == hir::kInvalidHirExpr) {
            current_fn_->blocks[else_block].insts.push(emitSlotStore(result_slot, else_value));
        }
        if (current_fn_->blocks[else_block].terminator == hir::kInvalidHirExpr)
            emitJump(merge_block);
    }

    setCurrentBlock(merge_block);
    current_fn_->blocks[merge_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    return has_value ? emitSlotLoad(result_slot, type) : hir::kInvalidHirExpr;
}

hir::HirExprId HirLowerModern::lowerWhile(const frontend::Expression &expr) {
    if (expr.operands.size() < 2U)
        return hir::kInvalidHirExpr;

    const auto header_block = newBlock();
    const auto body_block   = newBlock();
    const auto exit_block   = newBlock();

    emitJump(header_block);

    setCurrentBlock(header_block);
    current_fn_->blocks[header_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    const auto cond                         = lowerExpr(expr.operands[0]);
    hir::HirBranch branch;
    branch.cond       = cond;
    branch.then_block = static_cast<hir::HirDeclId>(body_block);
    branch.else_block = static_cast<hir::HirDeclId>(exit_block);
    setTerminator(addExpr(std::move(branch)));

    loop_stack_.push_back({header_block, exit_block});
    setCurrentBlock(body_block);
    current_fn_->blocks[body_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    (void)lowerExpr(expr.operands[1]);
    if (current_fn_->blocks[body_block].terminator == hir::kInvalidHirExpr)
        emitJump(header_block);
    loop_stack_.pop_back();

    setCurrentBlock(exit_block);
    current_fn_->blocks[exit_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    return hir::kInvalidHirExpr;
}

hir::HirExprId HirLowerModern::lowerAssign(const frontend::Expression &expr,
                                           const types::TypeId type) {
    (void)type;
    if (expr.operands.size() != 2U)
        return hir::kInvalidHirExpr;

    const auto &lhs_expr = current_module_->frontend->expressions()[expr.operands[0].value - 1U];
    if (lhs_expr.kind == frontend::ExprKind::Name) {
        if (const auto *resolved = findResolvedExpr(expr.operands[0]);
            resolved != nullptr && resolved->local) {
            const auto value = lowerExpr(expr.operands[1]);
            return value != hir::kInvalidHirExpr ? emitSlotStore(localSlot(resolved->local), value)
                                                 : hir::kInvalidHirExpr;
        }
    }

    const auto target = lowerExpr(expr.operands[0]);
    const auto value  = lowerExpr(expr.operands[1]);
    if (target == hir::kInvalidHirExpr || value == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;

    hir::HirAssign assign;
    assign.target = target;
    assign.value  = value;
    return addExpr(std::move(assign));
}

bool HirLowerModern::lowerStatement(frontend::StmtId id, hir::HirExprId &last_value) {
    if (!id || current_module_ == nullptr ||
        id.value > current_module_->frontend->statements().size())
        return true;

    const auto &statement = current_module_->frontend->statements()[id.value - 1U];
    switch (statement.kind) {
    case frontend::StmtKind::Expression:
        last_value = lowerExpr(statement.expression);
        if (last_value != hir::kInvalidHirExpr)
            current_fn_->blocks[current_block_].insts.push(last_value);
        return true;
    case frontend::StmtKind::Binding: {
        const auto slot = localSlot(statement.binding.id);
        const auto type = typeOfLocal(statement.binding.id);
        current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(slot, type));
        if (statement.binding.initializer) {
            const auto init = lowerExpr(statement.binding.initializer);
            if (init != hir::kInvalidHirExpr)
                current_fn_->blocks[current_block_].insts.push(emitSlotStore(slot, init));
        }
        last_value = hir::kInvalidHirExpr;
        return true;
    }
    case frontend::StmtKind::Return: {
        hir::HirRet ret;
        if (statement.expression)
            ret.value = lowerExpr(statement.expression);
        setTerminator(addExpr(std::move(ret)));
        return true;
    }
    case frontend::StmtKind::Break:
        if (loop_stack_.empty()) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                          "break used outside of a loop", {});
            return false;
        }
        emitJump(loop_stack_.back().break_block);
        setCurrentBlock(newBlock());
        current_fn_->blocks[current_block_].insts = memory::DynArray<hir::HirExprId>(arena_);
        return true;
    case frontend::StmtKind::Continue:
        if (loop_stack_.empty()) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                          "continue used outside of a loop", {});
            return false;
        }
        emitJump(loop_stack_.back().continue_block);
        setCurrentBlock(newBlock());
        current_fn_->blocks[current_block_].insts = memory::DynArray<hir::HirExprId>(arena_);
        return true;
    case frontend::StmtKind::Error:
        return false;
    }
    return true;
}

hir::HirExprId HirLowerModern::addExpr(hir::HirExpr expr) {
    return hir_.addExpr(std::move(expr));
}

hir::HirExprId HirLowerModern::emitSlotAlloca(const hir::HirSlotId slot, const types::TypeId type) {
    return addExpr(hir::HirSlotAlloca{slot, type});
}

hir::HirExprId HirLowerModern::emitSlotStore(const hir::HirSlotId slot,
                                             const hir::HirExprId value) {
    return addExpr(hir::HirSlotStore{slot, value});
}

hir::HirExprId HirLowerModern::emitSlotLoad(const hir::HirSlotId slot, const types::TypeId type) {
    return addExpr(hir::HirSlotLoad{slot, type});
}

size_t HirLowerModern::newBlock() {
    const auto block = current_fn_->blocks.size();
    current_fn_->blocks.emplace(arena_);
    return block;
}

void HirLowerModern::setCurrentBlock(const size_t block) {
    current_block_ = block;
}

void HirLowerModern::setTerminator(const hir::HirExprId term) {
    current_fn_->blocks[current_block_].terminator = term;
}

void HirLowerModern::emitJump(const size_t target) {
    hir::HirJump jump;
    jump.target = static_cast<hir::HirDeclId>(target);
    setTerminator(addExpr(std::move(jump)));
}

} // namespace zith::sema::modern
