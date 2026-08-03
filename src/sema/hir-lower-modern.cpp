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
    if (text == "&")
        return hir::HirUnaryOp::Ref;
    if (text == "*")
        return hir::HirUnaryOp::Deref;
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
    marker_blocks_.clear();
    local_slots_.clear();
    local_slots_.resize(1U);

    current_fn_->blocks.emplace(arena_);
    current_fn_->blocks[0].insts = memory::DynArray<hir::HirExprId>(arena_);

    collectMarkers(info.decl->body);

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
    // Nominal placeholders must lower to the completed type, not to Unknown.
    type = sema_.typeTable().canonical(type);
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
        if (structure == nullptr) {
            lowered = types::kErrorType;
            break;
        }
        lowered = types_.registerNamedType(structure->name, types::TypeKind::Struct);
        // Register the name (done above) before lowering field types so self-referential
        // structs (`next: *Node`) terminate. Fields are copied once, on first lowering.
        if (types_.fieldCount(lowered) == 0U && structure->fields.size() != 0U) {
            lowered_types_.insert(type.intern_seq, lowered);
            for (size_t index = 0; index < structure->fields.size(); ++index) {
                const auto field_name = index < structure->field_names.size()
                                            ? structure->field_names[index]
                                            : std::string_view{};
                types_.addField(lowered, field_name, lowerType(structure->fields[index]));
            }
        }
        break;
    }
    case TypeKind::Enum: {
        const auto *enumeration = sema_.typeTable().enum_type(type);
        if (enumeration == nullptr) {
            lowered = types::kErrorType;
            break;
        }
        // Register the named enum with its underlying type and variants so codegen can
        // lower it to the underlying integer instead of `void` (a plain registerNamedType
        // would leave the underlying as kErrorType).
        lowered = types_.defineEnum(enumeration->name, lowerType(enumeration->underlying));
        for (size_t i = 0; i < enumeration->variant_names.size(); ++i)
            types_.addEnumVariant(lowered, enumeration->variant_names[i],
                                  enumeration->discriminants[i]);
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
    case frontend::ExprKind::OptionalProp:
        return lowerOptionalProp(expr, type);
    case frontend::ExprKind::Index:
        return lowerIndex(expr, type);
    case frontend::ExprKind::Field:
        return lowerField(expr, type);
    case frontend::ExprKind::Arrow:
        return lowerArrow(expr, type);
    case frontend::ExprKind::StructLiteral:
        return lowerStructLiteral(expr, type);
    case frontend::ExprKind::ArrayLiteral:
        return lowerArrayLiteral(expr, type);
    case frontend::ExprKind::Cast:
        return lowerCast(expr, type);
    case frontend::ExprKind::IsNull:
        return lowerIsNull(expr);
    case frontend::ExprKind::LayoutIntrinsic:
        return lowerLayoutIntrinsic(expr);
    case frontend::ExprKind::Return:
    case frontend::ExprKind::Placeholder:
    case frontend::ExprKind::Error:
        return hir::kInvalidHirExpr;
    }
    return hir::kInvalidHirExpr;
}

hir::HirExprId HirLowerModern::lowerLiteral(const frontend::Expression &expr,
                                            const types::TypeId type) {
    // null literal maps to HirMakeNone when the target type is optional
    if (expr.text == "null" && types_.kindOf(type) == types::TypeKind::Optional) {
        hir::HirMakeNone make_none;
        make_none.type = type;
        return addExpr(std::move(make_none));
    }
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
    // Prefer the per-expression resolution: it is keyed by span, so it cannot
    // pick up a same-named binding from another function.
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

    if (current_resolution_ != nullptr && current_module_ != nullptr &&
        current_module_->frontend != nullptr) {
        if (const auto *binding = session::lookupBinding(
                *current_resolution_, expr.text, expr.scope, current_module_->frontend->scopes())) {
            if (binding->local) {
                const auto slot = localSlot(binding->local);
                return emitSlotLoad(slot, typeOfLocal(binding->local));
            }
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
    binary.lhs          = lhs;
    binary.rhs          = rhs;
    binary.op           = mapBinaryOp(expr.text);
    binary.type         = type;
    binary.operand_type = typeOfExpr(expr.operands[0]);
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
    // For field/arrow lvalue targets, lower the lhs normally (produces HirField) then assign.
    if (lhs_expr.kind == frontend::ExprKind::Field || lhs_expr.kind == frontend::ExprKind::Arrow) {
        const auto target_type = typeOfExpr(expr.operands[0]);
        const auto target      = lhs_expr.kind == frontend::ExprKind::Field
                                     ? lowerField(lhs_expr, target_type)
                                     : lowerArrow(lhs_expr, target_type);
        const auto value       = lowerExpr(expr.operands[1]);
        if (target == hir::kInvalidHirExpr || value == hir::kInvalidHirExpr)
            return hir::kInvalidHirExpr;
        hir::HirAssign assign;
        assign.target = target;
        assign.value  = value;
        return addExpr(std::move(assign));
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

hir::HirExprId HirLowerModern::lowerOptionalProp(const frontend::Expression &expr,
                                                 const types::TypeId type) {
    if (expr.operands.empty())
        return hir::kInvalidHirExpr;
    const auto operand = lowerExpr(expr.operands[0]);
    if (operand == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;

    const auto operand_type = typeOfExpr(expr.operands[0]);
    if (types_.kindOf(operand_type) != types::TypeKind::Optional)
        return hir::kInvalidHirExpr;

    // Spill the optional so its payload and tag can be addressed as fields.
    const auto slot = next_slot_++;
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(slot, operand_type));
    current_fn_->blocks[current_block_].insts.push(emitSlotStore(slot, operand));

    // The result of `x?` is stored here so both paths agree on one location.
    const auto result_slot = next_slot_++;
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(result_slot, type));

    const auto is_some = addExpr(hir::HirField{addExpr(hir::HirSlotAddr{slot, operand_type}), 1U,
                                               types::kBoolType, operand_type});

    const auto some_block = newBlock();
    const auto none_block = newBlock();

    hir::HirBranch branch;
    branch.cond       = is_some;
    branch.then_block = static_cast<hir::HirDeclId>(some_block);
    branch.else_block = static_cast<hir::HirDeclId>(none_block);
    setTerminator(addExpr(std::move(branch)));

    // None: propagate by returning None from the enclosing function.
    setCurrentBlock(none_block);
    current_fn_->blocks[none_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    {
        hir::HirRet ret;
        hir::HirMakeNone make_none;
        make_none.type = current_fn_->return_type;
        ret.value      = addExpr(std::move(make_none));
        setTerminator(addExpr(std::move(ret)));
    }

    // Some: unwrap the payload into the result slot and carry on.
    setCurrentBlock(some_block);
    current_fn_->blocks[some_block].insts = memory::DynArray<hir::HirExprId>(arena_);
    const auto payload                    = addExpr(
        hir::HirField{addExpr(hir::HirSlotAddr{slot, operand_type}), 0U, type, operand_type});
    current_fn_->blocks[some_block].insts.push(emitSlotStore(result_slot, payload));
    return emitSlotLoad(result_slot, type);
}

hir::HirExprId HirLowerModern::lowerCast(const frontend::Expression &expr,
                                         const types::TypeId type) {
    if (expr.operands.empty())
        return hir::kInvalidHirExpr;
    const auto value = lowerExpr(expr.operands[0]);
    if (value == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    const auto from = typeOfExpr(expr.operands[0]);
    if (from == type)
        return value;
    return addExpr(hir::HirCast{value, from, type});
}

/// `x is null` lowers to a tag/pointer comparison; no dedicated HIR node is needed.
hir::HirExprId HirLowerModern::lowerIsNull(const frontend::Expression &expr) {
    if (expr.operands.empty())
        return hir::kInvalidHirExpr;
    const auto operand = lowerExpr(expr.operands[0]);
    if (operand == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    const auto operand_type = typeOfExpr(expr.operands[0]);
    if (types_.kindOf(operand_type) != types::TypeKind::Optional)
        return hir::kInvalidHirExpr;

    const auto *optional = std::get_if<types::TypeOptional>(&types_.lookup(operand_type));
    const bool niche =
        optional != nullptr && types_.kindOf(optional->inner) == types::TypeKind::Ptr;
    if (niche) {
        // ?*T uses nullptr as the None sentinel: compare against MakeNone directly.
        hir::HirMakeNone none;
        none.type = operand_type;
        return addExpr(hir::HirBinary{operand, addExpr(std::move(none)), hir::HirBinaryOp::Eq,
                                      types::kBoolType});
    }

    // {payload, tag} layout: spill, read the tag, and negate it.
    const auto slot = next_slot_++;
    current_fn_->blocks[current_block_].insts.push(emitSlotAlloca(slot, operand_type));
    current_fn_->blocks[current_block_].insts.push(emitSlotStore(slot, operand));
    const auto tag = addExpr(hir::HirField{addExpr(hir::HirSlotAddr{slot, operand_type}), 1U,
                                           types::kBoolType, operand_type});
    return addExpr(hir::HirUnary{hir::HirUnaryOp::Not, tag, types::kBoolType});
}

hir::HirExprId HirLowerModern::lowerLayoutIntrinsic(const frontend::Expression &expr) {
    hir::HirLayoutIntrinsic intrinsic;
    intrinsic.which = expr.text == "alignOf" ? hir::HirLayoutIntrinsic::Which::AlignOf
                                             : hir::HirLayoutIntrinsic::Which::OffsetOf;
    if (current_module_ == nullptr || current_module_->frontend == nullptr || !expr.cast_type)
        return hir::kInvalidHirExpr;
    const auto &type_exprs = current_module_->frontend->typeExpressions();
    if (expr.cast_type.value > type_exprs.size())
        return hir::kInvalidHirExpr;
    const auto &type_expr           = type_exprs[expr.cast_type.value - 1U];
    const types::TypeId struct_type = lowerType(sema_.typeTable().lookupNamed(type_expr.name));
    if (struct_type == types::kErrorType)
        return hir::kInvalidHirExpr;
    intrinsic.type = struct_type;
    if (!expr.field_names.empty()) {
        const size_t field_index = types_.fieldIndex(struct_type, expr.field_names[0]);
        if (field_index == static_cast<size_t>(-1))
            return hir::kInvalidHirExpr;
        intrinsic.field_index = static_cast<uint32_t>(field_index);
    }
    return addExpr(std::move(intrinsic));
}

hir::HirExprId HirLowerModern::lowerIndex(const frontend::Expression &expr,
                                          const types::TypeId type) {
    if (expr.operands.size() < 2U)
        return hir::kInvalidHirExpr;
    const auto object = lowerExpr(expr.operands[0]);
    const auto index  = lowerExpr(expr.operands[1]);
    if (object == hir::kInvalidHirExpr || index == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;

    const auto object_type = typeOfExpr(expr.operands[0]);
    hir::HirIndex indexing;
    indexing.object   = object;
    indexing.index    = index;
    indexing.type     = type;
    indexing.obj_type = object_type;
    indexing.is_array = types_.kindOf(object_type) == types::TypeKind::Array;
    return addExpr(std::move(indexing));
}

memory::Optional<int64_t> HirLowerModern::enumVariantValue(frontend::ExprId operand,
                                                           std::string_view variant) {
    if (!operand || current_module_ == nullptr || current_module_->frontend == nullptr ||
        operand.value > current_module_->frontend->expressions().size()) {
        return {};
    }
    const auto &op = current_module_->frontend->expressions()[operand.value - 1U];
    if (op.kind != frontend::ExprKind::Name)
        return {};
    const auto *resolved = findResolvedExpr(operand);
    if (resolved == nullptr || !resolved->declaration)
        return {};
    const auto *decl = findDecl(*current_module_, resolved->declaration);
    if (decl == nullptr || decl->kind != frontend::DeclKind::Enum)
        return {};
    const auto enum_type = sema_.typeTable().canonical(sema_.typeTable().lookupNamed(decl->name));
    const auto *et       = sema_.typeTable().enum_type(enum_type);
    if (et == nullptr)
        return {};
    for (size_t i = 0; i < et->variant_names.size(); ++i) {
        if (et->variant_names[i] == variant)
            return et->discriminants[i];
    }
    return {};
}

hir::HirExprId HirLowerModern::lowerField(const frontend::Expression &expr,
                                          const types::TypeId type) {
    if (expr.operands.empty())
        return hir::kInvalidHirExpr;
    // `console.println` where `console` is an import alias: the field expression resolves
    // to the imported symbol, so emit the same HirVar a plain name would produce and never
    // lower the alias base (which would fail to resolve 'console').
    if (const auto *resolved = findResolvedExpr(expr.id);
        resolved != nullptr && resolved->kind == session::ResolutionKind::Import) {
        const frontend::Declaration *decl = resolvedFunctionDecl(*resolved);
        if (decl != nullptr) {
            hir::HirVar var;
            var.name    = interner_.intern(decl->name);
            var.version = 0;
            return addExpr(std::move(var));
        }
        if (resolved->foreignFunction != nullptr) {
            hir::HirVar var;
            var.name    = interner_.intern(resolved->foreignFunction->linkageName);
            var.version = 0;
            return addExpr(std::move(var));
        }
        return hir::kInvalidHirExpr;
    }
    // `Color.Green` resolves to an enum variant constant, not a struct field read.
    if (const auto variant = enumVariantValue(expr.operands[0], expr.text))
        return addExpr(hir::HirEnumValue{*variant, type});
    const auto object      = lowerExpr(expr.operands[0]);
    const auto object_type = typeOfExpr(expr.operands[0]);
    if (object == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    // Resolve the sema struct type to find the field index by name
    const auto sema_type = sema_.typeTable().canonical(semaTypeOfExpr(expr.operands[0]));
    const int idx        = sema_.typeTable().fieldIndex(sema_type, expr.text);
    if (idx < 0)
        return hir::kInvalidHirExpr;
    return addExpr(hir::HirField{object, static_cast<uint32_t>(idx), type, object_type});
}

hir::HirExprId HirLowerModern::lowerArrow(const frontend::Expression &expr,
                                          const types::TypeId type) {
    if (expr.operands.empty())
        return hir::kInvalidHirExpr;
    const auto ptr = lowerExpr(expr.operands[0]);
    if (ptr == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    // Deref the pointer first
    auto sema_ptr_type = sema_.typeTable().canonical(semaTypeOfExpr(expr.operands[0]));
    // Match sema: `?*T` behaves as `*T` here because None is represented as nullptr.
    if (const auto *opt = sema_.typeTable().optional(sema_ptr_type))
        sema_ptr_type = sema_.typeTable().canonical(opt->inner);
    const auto *pt = sema_.typeTable().pointer(sema_ptr_type);
    if (pt == nullptr)
        return hir::kInvalidHirExpr;
    const auto sema_struct = sema_.typeTable().canonical(pt->pointee);
    const auto struct_type = lowerType(sema_struct);
    const auto deref       = addExpr(hir::HirUnary{hir::HirUnaryOp::Deref, ptr, struct_type});
    const int idx          = sema_.typeTable().fieldIndex(sema_struct, expr.text);
    if (idx < 0)
        return hir::kInvalidHirExpr;
    return addExpr(hir::HirField{deref, static_cast<uint32_t>(idx), type, struct_type});
}

hir::HirExprId HirLowerModern::lowerStructLiteral(const frontend::Expression &expr,
                                                  const types::TypeId type) {
    hir::HirStructLiteral lit(arena_);
    lit.type = type;
    const size_t field_count =
        types_.kindOf(type) == types::TypeKind::Struct ? types_.fieldCount(type) : 0U;
    // Values are emitted in declaration order, not in the order the literal was written.
    std::vector<hir::HirExprId> ordered(field_count == 0U ? expr.operands.size() : field_count,
                                        hir::kInvalidHirExpr);
    for (size_t i = 0; i < expr.operands.size(); ++i) {
        auto value = lowerExpr(expr.operands[i]);
        if (value == hir::kInvalidHirExpr)
            continue;
        size_t slot_index = i;
        if (field_count != 0U && i < expr.field_names.size()) {
            slot_index = types_.fieldIndex(type, expr.field_names[i]);
            if (slot_index >= field_count)
                continue;
        }
        if (field_count != 0U) {
            // A bare `T` value assigned to a `?T` field must be wrapped in Some.
            const auto field_type = types_.getField(type, slot_index).type;
            const auto value_type = typeOfExpr(expr.operands[i]);
            if (types_.kindOf(field_type) == types::TypeKind::Optional &&
                types_.kindOf(value_type) != types::TypeKind::Optional) {
                value = lowerCoerceToOptional(field_type, value);
            }
        }
        if (slot_index < ordered.size())
            ordered[slot_index] = value;
    }
    // Fill slots left empty by a `_` placeholder or an omitted field with the
    // struct declaration's default; slots without a default stay zero-initialized.
    if (field_count != 0U) {
        for (size_t slot_index = 0; slot_index < field_count; ++slot_index) {
            if (ordered[slot_index] != hir::kInvalidHirExpr)
                continue;
            const auto default_id = lowerFieldDefault(expr.text, slot_index);
            if (!default_id)
                continue;
            const auto default_value = lowerExpr(default_id);
            if (default_value == hir::kInvalidHirExpr)
                continue;
            const auto field_type = types_.getField(type, slot_index).type;
            ordered[slot_index] =
                types_.kindOf(field_type) == types::TypeKind::Optional &&
                        types_.kindOf(typeOfExpr(default_id)) != types::TypeKind::Optional
                    ? lowerCoerceToOptional(field_type, default_value)
                    : default_value;
        }
    }
    // Keep every slot (missing ones are zero at codegen); the array is index-aligned.
    for (const auto value : ordered)
        lit.values.push(value);
    return addExpr(std::move(lit));
}

hir::HirExprId HirLowerModern::lowerArrayLiteral(const frontend::Expression &expr,
                                                 const types::TypeId type) {
    hir::HirArrayLiteral lit(arena_);
    lit.type = type;
    for (const auto operand : expr.operands) {
        const auto value = lowerExpr(operand);
        if (value == hir::kInvalidHirExpr)
            return hir::kInvalidHirExpr;
        lit.elements.push(value);
    }
    return addExpr(std::move(lit));
}

frontend::ExprId HirLowerModern::lowerFieldDefault(std::string_view struct_name,
                                                   size_t field_index) const noexcept {
    if (current_module_ == nullptr || current_module_->frontend == nullptr)
        return {};
    const auto &decls = current_module_->frontend->declarations();
    for (const auto &decl : decls) {
        if (decl.kind != frontend::DeclKind::Struct || decl.name != struct_name)
            continue;
        if (field_index < decl.parameters.size())
            return decl.parameters[field_index].defaultValue;
        break;
    }
    return {};
}

hir::HirExprId HirLowerModern::lowerCoerceToOptional(types::TypeId target, hir::HirExprId value) {
    if (value == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;
    // This is a simple wrapper: wrap any T value into ?T (Some)
    hir::HirMakeSome some;
    some.type  = target;
    some.value = value;
    return addExpr(std::move(some));
}

sema::modern::TypeId HirLowerModern::semaTypeOfExpr(frontend::ExprId id) {
    if (!id || current_types_ == nullptr)
        return kInvalidTypeId;
    const auto *sema_id_ptr = current_types_->exprTypes.get(id.value);
    if (!sema_id_ptr)
        return kInvalidTypeId;
    return *sema_id_ptr;
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
            auto init = lowerExpr(statement.binding.initializer);
            // Coerce T → ?T if the annotation is optional but init is not
            if (init != hir::kInvalidHirExpr && types_.kindOf(type) == types::TypeKind::Optional) {
                const auto init_type = typeOfExpr(statement.binding.initializer);
                if (types_.kindOf(init_type) != types::TypeKind::Optional) {
                    init = lowerCoerceToOptional(type, init);
                }
            }
            if (init != hir::kInvalidHirExpr)
                current_fn_->blocks[current_block_].insts.push(emitSlotStore(slot, init));
        }
        last_value = hir::kInvalidHirExpr;
        return true;
    }
    case frontend::StmtKind::Return: {
        hir::HirRet ret;
        if (statement.expression) {
            auto value = lowerExpr(statement.expression);
            // Coerce T → ?T if return type is optional but value is not
            if (value != hir::kInvalidHirExpr &&
                types_.kindOf(current_fn_->return_type) == types::TypeKind::Optional) {
                const auto val_type = typeOfExpr(statement.expression);
                if (types_.kindOf(val_type) != types::TypeKind::Optional) {
                    value = lowerCoerceToOptional(current_fn_->return_type, value);
                }
            }
            ret.value = value;
        }
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
    case frontend::StmtKind::Marker: {
        const auto *target = marker_blocks_.get(statement.label);
        if (target == nullptr) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                          "marker has no block: '" + statement.label + "'", {});
            return false;
        }
        const size_t marker_block = *target;
        // Fall through into the marker block.
        if (current_fn_->blocks[current_block_].terminator == hir::kInvalidHirExpr)
            emitJump(marker_block);
        setCurrentBlock(marker_block);
        current_fn_->blocks[marker_block].insts = memory::DynArray<hir::HirExprId>(arena_);
        if (statement.expression)
            (void)lowerExpr(statement.expression);
        // Continue after the marker body.
        const size_t continuation = newBlock();
        if (current_fn_->blocks[current_block_].terminator == hir::kInvalidHirExpr)
            emitJump(continuation);
        setCurrentBlock(continuation);
        current_fn_->blocks[continuation].insts = memory::DynArray<hir::HirExprId>(arena_);
        last_value                              = hir::kInvalidHirExpr;
        return true;
    }
    case frontend::StmtKind::Jump: {
        const auto *target = marker_blocks_.get(statement.label);
        if (target == nullptr) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::InvalidIR,
                          "jump to undefined marker: '" + statement.label + "'", {});
            return false;
        }
        emitJump(*target);
        // Anything after a jump is unreachable; give it a fresh block.
        setCurrentBlock(newBlock());
        current_fn_->blocks[current_block_].insts = memory::DynArray<hir::HirExprId>(arena_);
        last_value                                = hir::kInvalidHirExpr;
        return true;
    }
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

void HirLowerModern::collectMarkers(frontend::ExprId id) {
    if (!id || current_module_ == nullptr ||
        id.value > current_module_->frontend->expressions().size())
        return;
    const auto &expr = current_module_->frontend->expressions()[id.value - 1U];
    if (expr.kind == frontend::ExprKind::Block) {
        for (const auto &stmt_id : expr.statements) {
            if (!stmt_id || stmt_id.value > current_module_->frontend->statements().size())
                continue;
            const auto &stmt = current_module_->frontend->statements()[stmt_id.value - 1U];
            if (stmt.kind == frontend::StmtKind::Marker && !stmt.label.empty() &&
                !marker_blocks_.contains(stmt.label))
                marker_blocks_.insert(stmt.label, newBlock());
            if (stmt.expression)
                collectMarkers(stmt.expression);
        }
    } else {
        for (const auto operand : expr.operands)
            collectMarkers(operand);
    }
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
