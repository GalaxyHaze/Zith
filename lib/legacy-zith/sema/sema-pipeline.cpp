#include "sema-pipeline.hpp"
#include "legacy-zith/ast/ast-node-utils.hpp"
#ifdef ZITH_HAS_LLVM
#include "codegen/codegen-type.hpp"
#include <llvm/IR/LLVMContext.h>
#endif
#include "common/overloaded.hpp"
#include "diagnostics/error-codes.hpp"
#include "types/type-id.hpp"
#include "types/type-lower.hpp"

#include <cstdlib>
#include <string>

namespace zith::sema {

namespace {

using ast::LitKind;
using diagnostics::Severity;
using diagnostics::err::AmbiguousCall;
using diagnostics::err::NoMatchingFn;
using diagnostics::err::TypeMismatch;
using diagnostics::err::UndefinedIdent;
using diagnostics::err::WrongArity;

types::TypeId builtinTypeFromName(std::string_view name, types::TypeIntern &types) {
    if (name == "i8")
        return types.internInt(types::IntWidth::I8);
    if (name == "i16")
        return types.internInt(types::IntWidth::I16);
    if (name == "i32")
        return types.internInt(types::IntWidth::I32);
    if (name == "i64")
        return types.internInt(types::IntWidth::I64);
    if (name == "i128")
        return types.internInt(types::IntWidth::I128);
    if (name == "u8")
        return types.internInt(types::IntWidth::U8);
    if (name == "u16")
        return types.internInt(types::IntWidth::U16);
    if (name == "u32")
        return types.internInt(types::IntWidth::U32);
    if (name == "u64")
        return types.internInt(types::IntWidth::U64);
    if (name == "u128")
        return types.internInt(types::IntWidth::U128);
    if (name == "f32")
        return types.internFloat(types::FloatWidth::F32);
    if (name == "f64")
        return types.internFloat(types::FloatWidth::F64);
    if (name == "bool")
        return types::kBoolType;
    if (name == "char")
        return types::kCharType;
    if (name == "void")
        return types::kVoidType;
    if (name == "never")
        return types::kNeverType;
    return types::kErrorType;
}

types::TypeId defaultTypeForLit(ast::LitKind kind, types::TypeIntern &types) {
    switch (kind) {
    case LitKind::Int:
        return types.internInt(types::IntWidth::Literal);
    case LitKind::Float:
        return types.internFloat(types::FloatWidth::F64);
    case LitKind::Bool:
        return types::kBoolType;
    case LitKind::Char:
        return types::kCharType;
    case LitKind::String:
        return types.internPtr(types::kCharType);
    case LitKind::Nil:
        return types::kVoidType;
    }
    return types::kErrorType;
}

const char *checkIntOverflow(int64_t value, types::IntWidth target) {
    unsigned bits = types::intWidthBits(target);
    if (bits == 0)
        return nullptr;
    if (types::isSignedWidth(target)) {
        if (bits >= 64)
            return nullptr;
        int64_t min = -(INT64_C(1) << (bits - 1));
        int64_t max = (INT64_C(1) << (bits - 1)) - 1;
        if (value < min || value > max)
            return "value does not fit in the target integer type";
    } else {
        if (value < 0)
            return "value does not fit in the target unsigned integer type";
        if (bits >= 64)
            return nullptr;
        uint64_t max = (bits >= 64) ? ~0ULL : (1ULL << bits) - 1;
        if (static_cast<uint64_t>(value) > max)
            return "value does not fit in the target unsigned integer type";
    }
    return nullptr;
}

bool tryEvalIntLiteral(const ast::AstBuilder &builder, ast::ExprId id, int64_t &value) {
    if (id == ast::kInvalidExpr)
        return false;
    const auto &expr = builder.getExpr(id);
    if (const auto *lit = std::get_if<ast::LitValue>(&expr)) {
        if (lit->kind != LitKind::Int)
            return false;
        value = std::strtoll(std::string(lit->raw).data(), nullptr, 10);
        return true;
    }
    return false;
}

} // namespace

SemaPipeline::SemaPipeline(symbols::SymbolTable &syms, types::TypeIntern &types,
                           diagnostics::DiagnosticEngine &diags, ast::AstBuilder &builder,
                           memory::Arena &arena, const memory::DynArray<symbols::SymId> *resolved,
                           symbols::ImportManager *import_mgr, std::string_view target_triple)
    : ctx_(syms, types, diags, builder), unifier_(types, diags, arena), arena_(arena),
      resolved_(resolved), typed_ast_(arena), import_mgr_(import_mgr), worklist_(arena),
      active_builder_(&builder), allowed_files_(arena), var_types_(arena), checked_fns_(arena),
      target_triple_(target_triple) {
    typed_ast_.setActiveBuilder(active_builder_);
}

types::TypeId SemaPipeline::astExprType(ast::ExprId id) const {
    return typed_ast_.get(active_builder_, id);
}

ast::AstBuilder &SemaPipeline::builder() const {
    return *active_builder_;
}

bool SemaPipeline::isSymAccessible(symbols::SymId sym_id) const {
    if (allowed_files_.empty())
        return true;
    auto origin =
        import_mgr_ ? import_mgr_->originOf(sym_id) : memory::Optional<symbols::SymOrigin>{};
    if (!origin)
        return true;
    for (auto af : allowed_files_)
        if (af == origin->file_idx)
            return true;
    return false;
}

ast::AstBuilder *SemaPipeline::builderForSym(symbols::SymId fn_sym) {
    if (import_mgr_) {
        auto origin = import_mgr_->originOf(fn_sym);
        if (origin)
            return import_mgr_->get(origin->file_idx).builder;
    }
    return &ctx_.builder();
}

const symbols::SymbolTable *SemaPipeline::symsForSym(symbols::SymId fn_sym) {
    if (import_mgr_) {
        auto origin = import_mgr_->originOf(fn_sym);
        if (origin)
            return &import_mgr_->get(origin->file_idx).symbols;
    }
    return &ctx_.syms();
}

symbols::SymId SemaPipeline::localSymFor(symbols::SymId fn_sym) const {
    if (import_mgr_) {
        auto origin = import_mgr_->originOf(fn_sym);
        if (origin)
            return origin->local_sym;
    }
    return fn_sym;
}

const ast::FnDeclNode *SemaPipeline::fnDeclForSym(symbols::SymId fn_sym,
                                                  ast::AstBuilder **source_bld,
                                                  const symbols::SymbolTable **source_syms) {
    auto *bld  = builderForSym(fn_sym);
    auto *syms = symsForSym(fn_sym);
    if (source_bld)
        *source_bld = bld;
    if (source_syms)
        *source_syms = syms;
    if (!bld || !syms)
        return nullptr;

    auto local_sym   = localSymFor(fn_sym);
    const auto &data = syms->get(local_sym);
    if (data.kind != symbols::SymKind::Fn || data.decl_id == ast::kInvalidDecl)
        return nullptr;
    return ast::asFn(bld->getDecl(data.decl_id));
}

types::TypeId SemaPipeline::lowerFnReturnType(symbols::SymId fn_sym) {
    ast::AstBuilder *source_bld = nullptr;
    auto *fn                    = fnDeclForSym(fn_sym, &source_bld, nullptr);
    if (!fn || !source_bld)
        return types::kErrorType;
    types::TypeLower lower(*source_bld, ctx_.types(), ctx_.diags(), ctx_.syms());
    return lower.lower(fn->return_type);
}

types::TypeId SemaPipeline::lowerFnParamType(symbols::SymId fn_sym, size_t index) {
    ast::AstBuilder *source_bld = nullptr;
    auto *fn                    = fnDeclForSym(fn_sym, &source_bld, nullptr);
    if (!fn || !source_bld || index >= fn->params.size())
        return types::kErrorType;
    types::TypeLower lower(*source_bld, ctx_.types(), ctx_.diags(), ctx_.syms());
    return lower.lower(fn->params[index].type);
}

types::TypeId SemaPipeline::lowerIntrinsicType(const ast::ExprId id, const memory::Span span) {
    if (id == ast::kInvalidExpr)
        return types::kErrorType;

    const auto &expr = builder().getExpr(id);
    if (const auto *ident = std::get_if<ast::IdentNode>(&expr)) {
        auto builtin = builtinTypeFromName(ident->name, ctx_.types());
        if (builtin != types::kErrorType)
            return builtin;

        auto named = ctx_.types().lookupNamedType(ident->name);
        if (named != types::kErrorType)
            return named;

        ctx_.diags().report(Severity::Error, UndefinedIdent,
                            "undefined type '" + std::string(ident->name) + "'", span);
        return types::kErrorType;
    }

    ctx_.diags().report(Severity::Error, TypeMismatch,
                        "layout intrinsics require a type name argument", span);
    return types::kErrorType;
}

const ast::StructDeclNode *SemaPipeline::structDeclForType(const types::TypeId type,
                                                           ast::AstBuilder **source_bld) {
    if (ctx_.types().kindOf(type) != types::TypeKind::Struct)
        return nullptr;

    const auto &def = ctx_.types().getStructDef(type);
    auto name       = ctx_.types().interner().lookup(def.name);
    auto sym_id     = ctx_.syms().lookup(name);
    if (sym_id == symbols::kInvalidSym)
        return nullptr;

    auto *bld  = builderForSym(sym_id);
    auto *syms = symsForSym(sym_id);
    if (source_bld)
        *source_bld = bld;
    if (!bld || !syms)
        return nullptr;

    auto local_sym   = localSymFor(sym_id);
    const auto &data = syms->get(local_sym);
    if (data.decl_id == ast::kInvalidDecl)
        return nullptr;
    return std::get_if<ast::StructDeclNode>(&bld->getDecl(data.decl_id));
}

bool SemaPipeline::materializeLayoutIntrinsic(const ast::ExprId id, ast::IntrinsicNode &node) {
    uint64_t value   = 0;
    auto int_literal = ctx_.types().internInt(types::IntWidth::Literal);

    switch (node.kind) {
    case ast::IntrinsicKind::SizeOf:
    case ast::IntrinsicKind::AlignOf: {
        if (node.args.size() != 1) {
            ctx_.diags().report(Severity::Error, WrongArity,
                                "layout intrinsics expect exactly one type argument", node.span);
            typed_ast_.set(id, types::kErrorType);
            return false;
        }

        auto type = lowerIntrinsicType(node.args[0], node.span);
        if (type == types::kErrorType) {
            typed_ast_.set(id, types::kErrorType);
            return false;
        }

#ifdef ZITH_HAS_LLVM
        auto layout = codegen::makeTargetDataLayout(target_triple_);
        if (!layout) {
            ctx_.diags().report(Severity::Error, TypeMismatch,
                                "failed to construct target layout for layout intrinsic",
                                node.span);
            typed_ast_.set(id, types::kErrorType);
            return false;
        }

        llvm::LLVMContext llvm_ctx;
        codegen::CodeGenType type_gen(llvm_ctx, ctx_.types(), &*layout);
        value = node.kind == ast::IntrinsicKind::SizeOf ? type_gen.sizeOf(type)
                                                        : type_gen.alignOf(type);
#else
        ctx_.diags().report(Severity::Error, TypeMismatch,
                            "layout intrinsics require an LLVM-enabled build", node.span);
        typed_ast_.set(id, types::kErrorType);
        return false;
#endif
        break;
    }

    case ast::IntrinsicKind::OffsetOf: {
        if (node.args.size() != 2) {
            ctx_.diags().report(Severity::Error, WrongArity,
                                "@offsetOf expects a type and a field name", node.span);
            typed_ast_.set(id, types::kErrorType);
            return false;
        }

        auto type = lowerIntrinsicType(node.args[0], node.span);
        if (type == types::kErrorType) {
            typed_ast_.set(id, types::kErrorType);
            return false;
        }
        if (ctx_.types().kindOf(type) != types::TypeKind::Struct) {
            ctx_.diags().report(Severity::Error, TypeMismatch, "@offsetOf requires a struct type",
                                node.span);
            typed_ast_.set(id, types::kErrorType);
            return false;
        }

        const auto *field_ident = std::get_if<ast::IdentNode>(&builder().getExpr(node.args[1]));
        if (!field_ident) {
            ctx_.diags().report(Severity::Error, TypeMismatch,
                                "@offsetOf requires a field name identifier", node.span);
            typed_ast_.set(id, types::kErrorType);
            return false;
        }

        if (!ctx_.types().hasField(type, field_ident->name)) {
            ctx_.diags().report(Severity::Error, diagnostics::err::NoMember,
                                "struct has no field '" + std::string(field_ident->name) + "'",
                                node.span);
            typed_ast_.set(id, types::kErrorType);
            return false;
        }

#ifdef ZITH_HAS_LLVM
        auto layout = codegen::makeTargetDataLayout(target_triple_);
        if (!layout) {
            ctx_.diags().report(Severity::Error, TypeMismatch,
                                "failed to construct target layout for @offsetOf", node.span);
            typed_ast_.set(id, types::kErrorType);
            return false;
        }

        llvm::LLVMContext llvm_ctx;
        codegen::CodeGenType type_gen(llvm_ctx, ctx_.types(), &*layout);
        value = type_gen.fieldOffset(type, field_ident->name);
#else
        ctx_.diags().report(Severity::Error, TypeMismatch,
                            "layout intrinsics require an LLVM-enabled build", node.span);
        typed_ast_.set(id, types::kErrorType);
        return false;
#endif
        break;
    }

    default:
        return false;
    }

    auto raw = ctx_.syms().interner().lookup(ctx_.syms().interner().intern(std::to_string(value)));
    builder().getExpr(id) = ast::LitValue{raw, node.span, ast::LitKind::Int};
    typed_ast_.set(id, int_literal);
    return true;
}

bool SemaPipeline::normalizeStructLiteral(const ast::ExprId id, ast::StructLiteralNode &node,
                                          const types::TypeId type) {
    if (ctx_.types().kindOf(type) != types::TypeKind::Struct) {
        ctx_.diags().report(Severity::Error, TypeMismatch, "struct literal requires a struct type",
                            node.span);
        typed_ast_.set(id, types::kErrorType);
        return false;
    }

    const auto &struct_def      = ctx_.types().getStructDef(type);
    ast::AstBuilder *struct_bld = nullptr;
    const auto *struct_decl     = structDeclForType(type, &struct_bld);

    bool has_named      = false;
    bool has_positional = false;
    for (const auto &field : node.fields) {
        has_named      = has_named || !field.name.empty();
        has_positional = has_positional || field.name.empty();
    }
    if (has_named && has_positional) {
        ctx_.diags().report(Severity::Error, TypeMismatch,
                            "cannot mix positional and named struct initializers", node.span);
        typed_ast_.set(id, types::kErrorType);
        return false;
    }

    if (!has_named) {
        if (node.fields.size() != struct_def.fields.size()) {
            ctx_.diags().report(Severity::Error, TypeMismatch,
                                "positional struct literal has the wrong number of fields",
                                node.span);
            typed_ast_.set(id, types::kErrorType);
            return false;
        }
        return true;
    }

    memory::DynArray<ast::StructFieldInit> ordered{builder().arena()};
    memory::DynArray<uint8_t> seen{builder().arena()};
    for (size_t i = 0; i < struct_def.fields.size(); ++i) {
        ordered.push(
            {ctx_.types().interner().lookup(struct_def.fields[i].name), ast::kInvalidExpr});
        seen.push(0);
    }

    bool ok = true;
    for (const auto &field : node.fields) {
        const auto index = ctx_.types().fieldIndex(type, field.name);
        if (index == static_cast<size_t>(-1)) {
            ctx_.diags().report(Severity::Error, diagnostics::err::NoMember,
                                "struct has no field '" + std::string(field.name) + "'", node.span);
            ok = false;
            continue;
        }
        if (seen[index] != 0) {
            ctx_.diags().report(Severity::Error, TypeMismatch,
                                "duplicate struct initializer for field '" +
                                    std::string(field.name) + "'",
                                node.span);
            ok = false;
            continue;
        }

        seen[index]    = 1;
        ordered[index] = field;
        if (ordered[index].value == ast::kInvalidExpr) {
            if (!struct_decl || !struct_bld || index >= struct_decl->fields.size() ||
                struct_decl->fields[index].default_value == ast::kInvalidExpr) {
                ctx_.diags().report(Severity::Error, TypeMismatch,
                                    "field '" + std::string(field.name) + "' has no default value",
                                    node.span);
                ok = false;
                continue;
            }
            if (struct_bld != &builder()) {
                ctx_.diags().report(
                    Severity::Error, TypeMismatch,
                    "struct field defaults from imported modules are not supported yet", node.span);
                ok = false;
                continue;
            }
            ordered[index].value = struct_decl->fields[index].default_value;
        }
    }

    for (size_t i = 0; i < struct_def.fields.size(); ++i) {
        if (seen[i] == 0) {
            ctx_.diags().report(
                Severity::Error, TypeMismatch,
                "missing initializer for field '" +
                    std::string(ctx_.types().interner().lookup(struct_def.fields[i].name)) + "'",
                node.span);
            ok = false;
        }
    }

    if (!ok) {
        typed_ast_.set(id, types::kErrorType);
        return false;
    }

    node.fields = std::move(ordered);
    return true;
}

void SemaPipeline::ensureVarTypeCapacity(symbols::SymId sym_id) {
    if (sym_id >= var_types_.capacity())
        var_types_.reserve(sym_id + 1);
    while (var_types_.size() <= sym_id)
        var_types_.push(types::kErrorType);
}

void SemaPipeline::ensureCheckedCapacity(symbols::SymId sym_id) {
    if (sym_id >= checked_fns_.capacity())
        checked_fns_.reserve(sym_id + 1);
    while (checked_fns_.size() <= sym_id)
        checked_fns_.push(0);
}

bool SemaPipeline::isBodyChecked(symbols::SymId sym_id) const {
    return sym_id < checked_fns_.size() && checked_fns_[sym_id] != 0;
}

void SemaPipeline::markBodyChecked(symbols::SymId sym_id) {
    ensureCheckedCapacity(sym_id);
    checked_fns_[sym_id] = 1;
}

bool SemaPipeline::concretizeLiteralExpr(ast::ExprId id, types::TypeId concrete_type,
                                         memory::Span span) {
    if (id == ast::kInvalidExpr || concrete_type == types::kErrorType)
        return false;

    auto &node = builder().getExpr(id);
    if (auto *unary = std::get_if<ast::UnaryNode>(&node)) {
        if (unary->op != ast::UnaryOp::Neg)
            return false;

        auto current_type = astExprType(id);
        if (current_type == types::kErrorType ||
            ctx_.types().kindOf(current_type) != types::TypeKind::Int)
            return false;

        auto *current_int = std::get_if<types::TypeInt>(&ctx_.types().lookup(current_type));
        if (!current_int || current_int->width != types::IntWidth::Literal)
            return false;

        auto &operand_node = builder().getExpr(unary->operand);
        auto *operand_lit  = std::get_if<ast::LitValue>(&operand_node);
        if (!operand_lit || operand_lit->kind != ast::LitKind::Int)
            return false;

        typed_ast_.set(id, concrete_type);

        if (ctx_.types().kindOf(concrete_type) != types::TypeKind::Int)
            return true;

        auto *int_t = std::get_if<types::TypeInt>(&ctx_.types().lookup(concrete_type));
        auto value  = -std::strtoll(std::string(operand_lit->raw).data(), nullptr, 10);
        if (auto *err = checkIntOverflow(value, int_t->width))
            ctx_.diags().report(Severity::Error, TypeMismatch, err, span);

        typed_ast_.set(unary->operand, concrete_type);
        return true;
    }

    auto *lit = std::get_if<ast::LitValue>(&node);
    if (!lit || lit->kind != ast::LitKind::Int)
        return false;

    auto current_type = astExprType(id);
    if (current_type == types::kErrorType ||
        ctx_.types().kindOf(current_type) != types::TypeKind::Int)
        return false;

    auto *current_int = std::get_if<types::TypeInt>(&ctx_.types().lookup(current_type));
    if (!current_int || current_int->width != types::IntWidth::Literal)
        return false;

    typed_ast_.set(id, concrete_type);

    if (ctx_.types().kindOf(concrete_type) != types::TypeKind::Int)
        return true;

    auto *int_t = std::get_if<types::TypeInt>(&ctx_.types().lookup(concrete_type));
    auto value  = std::strtoll(std::string(lit->raw).data(), nullptr, 10);
    if (auto *err = checkIntOverflow(value, int_t->width))
        ctx_.diags().report(Severity::Error, TypeMismatch, err, span);
    return true;
}

ast::ExprId SemaPipeline::implicitReturnExpr(ast::ExprId body_id) const {
    if (body_id == ast::kInvalidExpr)
        return ast::kInvalidExpr;
    auto &node = builder().getExpr(body_id);
    if (auto *block = std::get_if<ast::BlockNode>(&node))
        return block->trailing;
    return body_id;
}

void SemaPipeline::ensureBodyChecked(symbols::SymId fn_sym) {
    if (isBodyChecked(fn_sym))
        return;
    markBodyChecked(fn_sym);

    ast::AstBuilder *source_bld             = nullptr;
    const symbols::SymbolTable *source_syms = nullptr;
    auto *fn                                = fnDeclForSym(fn_sym, &source_bld, &source_syms);
    if (!fn || !source_bld || !source_syms || fn->is_extern || fn->body == ast::kInvalidExpr)
        return;

    auto previous_allowed = std::move(allowed_files_);
    allowed_files_.clear();
    if (import_mgr_) {
        auto origin = import_mgr_->originOf(fn_sym);
        if (origin) {
            allowed_files_.push(origin->file_idx);
            auto &file = import_mgr_->get(origin->file_idx);
            for (auto dep : file.dep_files)
                allowed_files_.push(dep);
        } else {
            for (auto dep : import_mgr_->rootDeps())
                allowed_files_.push(dep);
        }
    }

    auto *previous_builder = active_builder_;
    auto previous_ret      = current_fn_ret_type_;
    active_builder_        = source_bld;
    typed_ast_.setActiveBuilder(active_builder_);
    current_fn_ret_type_ = lowerFnReturnType(fn_sym);
    labelMap_            = {};

    auto scope = syms().enterScope();

    types::TypeLower lower(*source_bld, ctx_.types(), ctx_.diags(), ctx_.syms());
    for (const auto &param : fn->params) {
        auto param_sym = syms().declareInScope(scope, param.name);
        ensureVarTypeCapacity(param_sym);
        var_types_[param_sym] = lower.lower(param.type);
    }

    auto body_type        = visitExpr(fn->body);
    auto implicit_expr_id = implicitReturnExpr(fn->body);
    if (implicit_expr_id != ast::kInvalidExpr && current_fn_ret_type_ != types::kErrorType &&
        body_type != types::kErrorType) {
        if (unifier_.unify(body_type, current_fn_ret_type_, fn->span)) {
            types::TypeId concrete = current_fn_ret_type_;
            if (ctx_.types().kindOf(current_fn_ret_type_) == types::TypeKind::Int) {
                auto *type =
                    std::get_if<types::TypeInt>(&ctx_.types().lookup(current_fn_ret_type_));
                if (type && type->width == types::IntWidth::Literal)
                    concrete = body_type;
            }
            concretizeLiteralExpr(implicit_expr_id, concrete, fn->span);
        }
    }

    syms().exitScope();
    current_fn_ret_type_ = previous_ret;
    active_builder_      = previous_builder;
    typed_ast_.setActiveBuilder(active_builder_);
    allowed_files_ = std::move(previous_allowed);
}

void SemaPipeline::warnNotImplemented(std::string_view feature, memory::Span span) {
    ctx_.diags().report(diagnostics::Severity::Warning, diagnostics::err::NotImplemented,
                        std::string(feature) + " is not implemented yet", span);
}

void SemaPipeline::reportUnsupportedSyntax(std::string_view syntax, memory::Span span) {
    ctx_.diags().report(diagnostics::Severity::Error, diagnostics::err::UnsupportedSyntax,
                        std::string(syntax) + " is parsed but not supported yet", span);
}

bool SemaPipeline::run(const ast::ProgramNode &program) {
    for (auto decl_id : program.decls) {
        auto &decl = ctx_.builder().getDecl(decl_id);
        std::visit(common::overloaded{
                       [&](const ast::StructDeclNode &n) {
                           ctx_.types().registerNamedType(n.name, types::TypeKind::Struct);
                       },
                       [&](const ast::EnumDeclNode &n) {
                           ctx_.types().registerNamedType(n.name, types::TypeKind::Enum);
                       },
                       [&](const ast::UnionDeclNode &n) {
                           ctx_.types().registerNamedType(n.name, types::TypeKind::Union);
                       },
                       [&](const ast::TypeAliasDeclNode &n) {
                           types::TypeLower lower(ctx_.builder(), ctx_.types(), ctx_.diags(),
                                                  ctx_.syms());
                           auto target = lower.lower(n.target_type);
                           ctx_.types().registerTypeAlias(n.name, target);
                       },
                       [&](const ast::WordDeclNode &n) {
                           reportUnsupportedSyntax("word declarations", n.span);
                       },
                       [&](const ast::ContextDeclNode &n) {
                           reportUnsupportedSyntax("context declarations", n.span);
                       },
                       [](const auto &) {},
                   },
                   decl);
    }

    types::TypeLower lower(ctx_.builder(), ctx_.types(), ctx_.diags(), ctx_.syms());
    for (auto decl_id : program.decls) {
        auto &decl = ctx_.builder().getDecl(decl_id);
        std::visit(
            common::overloaded{
                [&](const ast::StructDeclNode &n) {
                    auto struct_type = ctx_.types().lookupNamedType(n.name);
                    if (struct_type == types::kErrorType)
                        return;
                    auto &def = ctx_.types().getStructDef(struct_type);
                    if (!def.fields.empty())
                        return;
                    for (const auto &field : n.fields) {
                        auto field_type = lower.lower(field.type_expr);
                        ctx_.types().addField(struct_type, field.name, field_type);
                    }
                    for (size_t i = 0; i < n.fields.size(); ++i) {
                        if (n.fields[i].default_value == ast::kInvalidExpr)
                            continue;
                        auto field_type = def.fields[i].type;
                        auto value_type = visitExpr(n.fields[i].default_value);
                        if (field_type != types::kErrorType && value_type != types::kErrorType &&
                            unifier_.unify(value_type, field_type, n.span)) {
                            concretizeLiteralExpr(n.fields[i].default_value, field_type, n.span);
                        }
                    }
                },
                [&](const ast::EnumDeclNode &n) {
                    auto enum_type = ctx_.types().lookupNamedType(n.name);
                    if (enum_type == types::kErrorType)
                        return;
                    auto &def = ctx_.types().getEnumDef(enum_type);
                    if (!def.variants.empty())
                        return;

                    def.underlying = n.underlying_type != ast::kInvalidTypeExpr
                                         ? lower.lower(n.underlying_type)
                                         : ctx_.types().internInt(types::IntWidth::I32);

                    int64_t next_discriminant = 0;
                    for (const auto &variant : n.variants) {
                        int64_t value = next_discriminant;
                        if (variant.discriminant != ast::kInvalidExpr &&
                            !tryEvalIntLiteral(ctx_.builder(), variant.discriminant, value)) {
                            ctx_.diags().report(Severity::Error, TypeMismatch,
                                                "enum discriminant must be an integer literal",
                                                n.span);
                            value = next_discriminant;
                        }

                        if (ctx_.types().kindOf(def.underlying) == types::TypeKind::Int) {
                            auto *int_type =
                                std::get_if<types::TypeInt>(&ctx_.types().lookup(def.underlying));
                            if (int_type) {
                                if (const auto *err = checkIntOverflow(value, int_type->width)) {
                                    ctx_.diags().report(Severity::Error, TypeMismatch, err, n.span);
                                }
                            }
                        }

                        ctx_.types().addEnumVariant(enum_type, variant.name, value);
                        next_discriminant = value + 1;
                    }
                },
                [&](const ast::UnionDeclNode &n) {
                    auto union_type = ctx_.types().lookupNamedType(n.name);
                    if (union_type == types::kErrorType)
                        return;
                    auto &def = ctx_.types().getUnionDef(union_type);
                    if (!def.members.empty())
                        return;
                    def.is_raw = n.is_raw;
                    for (const auto &variant : n.variants) {
                        if (variant.type_expr == ast::kInvalidTypeExpr)
                            continue;
                        ctx_.types().addUnionMember(union_type, lower.lower(variant.type_expr));
                    }
                },
                [](const auto &) {},
            },
            decl);
    }

    for (auto decl_id : program.decls) {
        auto &decl = ctx_.builder().getDecl(decl_id);
        if (!ast::asFn(decl))
            continue;
        for (symbols::SymId sym = 0; sym < ctx_.syms().symbolCount(); ++sym) {
            const auto &data = ctx_.syms().get(sym);
            if (data.kind == symbols::SymKind::Fn && data.decl_id == decl_id &&
                (!import_mgr_ || !import_mgr_->originOf(sym))) {
                ensureBodyChecked(sym);
                break;
            }
        }
    }

    for (size_t i = 0; i < worklist_.size(); ++i)
        ensureBodyChecked(worklist_[i]);

    return !ctx_.diags().hasErrors();
}

types::TypeId SemaPipeline::visitExpr(ast::ExprId id) {
    if (id == ast::kInvalidExpr)
        return types::kErrorType;

    auto &node = builder().getExpr(id);
    return std::visit(
        common::overloaded{
            [&](const ast::LitValue &n) { return visitLiteral(id, n); },
            [&](const ast::IdentNode &n) { return visitIdent(n, id); },
            [&](const ast::BinaryNode &n) { return visitBinary(id, n); },
            [&](const ast::UnaryNode &n) { return visitUnary(id, n); },
            [&](const ast::CallNode &n) { return visitCall(id, n); },
            [&](const ast::BlockNode &n) { return visitBlock(id, n); },
            [&](const ast::IfNode &n) { return visitIf(id, n); },
            [&](const ast::WhileNode &n) { return visitWhile(id, n); },
            [&](const ast::FieldNode &n) {
                auto object_type = visitExpr(n.object);
                if (object_type == types::kErrorType) {
                    typed_ast_.set(id, types::kErrorType);
                    return types::kErrorType;
                }
                if (ctx_.types().kindOf(object_type) == types::TypeKind::Struct) {
                    auto field_type = ctx_.types().fieldType(object_type, n.field);
                    if (field_type == types::kErrorType) {
                        ctx_.diags().report(Severity::Error, diagnostics::err::NoMember,
                                            "struct has no field '" + std::string(n.field) + "'",
                                            n.span);
                    }
                    typed_ast_.set(id, field_type);
                    return field_type;
                }
                ctx_.diags().report(Severity::Error, TypeMismatch, "field access requires a struct",
                                    n.span);
                typed_ast_.set(id, types::kErrorType);
                return types::kErrorType;
            },
            [&](const ast::IndexNode &n) {
                auto obj_type = visitExpr(n.object);
                auto idx_type = visitExpr(n.index);
                if (obj_type == types::kErrorType || idx_type == types::kErrorType) {
                    typed_ast_.set(id, types::kErrorType);
                    return types::kErrorType;
                }
                if (ctx_.types().kindOf(idx_type) != types::TypeKind::Int) {
                    ctx_.diags().report(Severity::Error, TypeMismatch,
                                        "index expression must have integer type", n.span);
                    typed_ast_.set(id, types::kErrorType);
                    return types::kErrorType;
                }
                if (ctx_.types().kindOf(obj_type) == types::TypeKind::Array) {
                    auto *array_type =
                        std::get_if<types::TypeArray>(&ctx_.types().lookup(obj_type));
                    auto result_type = array_type ? array_type->elem : types::kErrorType;
                    typed_ast_.set(id, result_type);
                    return result_type;
                }
                if (ctx_.types().kindOf(obj_type) == types::TypeKind::Slice) {
                    auto *slice_type =
                        std::get_if<types::TypeSlice>(&ctx_.types().lookup(obj_type));
                    auto result_type = slice_type ? slice_type->elem : types::kErrorType;
                    typed_ast_.set(id, result_type);
                    return result_type;
                }
                if (ctx_.types().kindOf(obj_type) == types::TypeKind::Ptr) {
                    auto *ptr_type   = std::get_if<types::TypePtr>(&ctx_.types().lookup(obj_type));
                    auto result_type = ptr_type ? ptr_type->pointee : types::kErrorType;
                    typed_ast_.set(id, result_type);
                    return result_type;
                }
                ctx_.diags().report(Severity::Error, TypeMismatch,
                                    "cannot index into non-array/slice", n.span);
                typed_ast_.set(id, types::kErrorType);
                return types::kErrorType;
            },
            [&](ast::StructLiteralNode &n) {
                auto type = ctx_.types().lookupNamedType(n.type_name);
                if (type == types::kErrorType) {
                    typed_ast_.set(id, types::kErrorType);
                    return types::kErrorType;
                }
                if (!normalizeStructLiteral(id, n, type))
                    return types::kErrorType;

                const auto &struct_def = ctx_.types().getStructDef(type);
                for (size_t i = 0; i < n.fields.size(); ++i) {
                    const auto &field = n.fields[i];
                    if (field.value != ast::kInvalidExpr) {
                        auto val_type               = visitExpr(field.value);
                        types::TypeId expected_type = i < struct_def.fields.size()
                                                          ? struct_def.fields[i].type
                                                          : types::kErrorType;
                        if (expected_type != types::kErrorType) {
                            unifier_.unify(val_type, expected_type, n.span);
                            auto *type_int =
                                std::get_if<types::TypeInt>(&ctx_.types().lookup(val_type));
                            if (type_int && type_int->width == types::IntWidth::Literal) {
                                concretizeLiteralExpr(field.value, expected_type, n.span);
                            }
                        }
                    }
                }
                typed_ast_.set(id, type);
                return type;
            },
            [&](const ast::EnumValueNode &n) {
                auto type = ctx_.types().lookupNamedType(n.enum_name);
                if (type == types::kErrorType) {
                    typed_ast_.set(id, types::kErrorType);
                    return types::kErrorType;
                }
                int64_t value = 0;
                if (!ctx_.types().enumValue(type, n.variant_name, value)) {
                    ctx_.diags().report(Severity::Error, diagnostics::err::NoMember,
                                        "enum has no variant '" + std::string(n.variant_name) + "'",
                                        n.span);
                    typed_ast_.set(id, types::kErrorType);
                    return types::kErrorType;
                }
                typed_ast_.set(id, type);
                return type;
            },
            [&](const ast::ArrayLiteralNode &n) {
                // Infer element type from first element, default to i32 if empty
                types::TypeId elem_type = types::kErrorType;
                for (auto elem : n.elements) {
                    auto t = visitExpr(elem);
                    if (elem_type == types::kErrorType)
                        elem_type = t;
                }
                if (elem_type == types::kErrorType)
                    elem_type = ctx_.types().internInt(types::IntWidth::I32);
                auto type =
                    ctx_.types().internArray(elem_type, static_cast<uint32_t>(n.elements.size()));
                typed_ast_.set(id, type);
                return type;
            },
            [&](const ast::RangeNode &) {
                // Range nodes are only valid inside when-case conditions; standalone ranges
                // are not yet materializable values.
                warnNotImplemented("standalone range expressions", builder().exprSpan(id));
                typed_ast_.set(id, types::kErrorType);
                return types::kErrorType;
            },
            [&](const ast::WhenNode &n) { return visitWhen(id, n); },
            [&](const ast::UnbodyNode &) {
                warnNotImplemented("compiler intrinsics in expressions", builder().exprSpan(id));
                typed_ast_.set(id, types::kErrorType);
                return types::kErrorType;
            },
            [&](ast::IntrinsicNode &n) {
                switch (n.kind) {
                case ast::IntrinsicKind::SizeOf:
                case ast::IntrinsicKind::AlignOf:
                case ast::IntrinsicKind::OffsetOf: {
                    if (!materializeLayoutIntrinsic(id, n))
                        return types::kErrorType;
                    const auto *lit = std::get_if<ast::LitValue>(&builder().getExpr(id));
                    if (!lit) {
                        typed_ast_.set(id, types::kErrorType);
                        return types::kErrorType;
                    }
                    return visitLiteral(id, *lit);
                }
                default:
                    warnNotImplemented("compiler intrinsics in expressions",
                                       builder().exprSpan(id));
                    typed_ast_.set(id, types::kErrorType);
                    return types::kErrorType;
                }
            },
            [&](const ast::MacroCallNode &) {
                warnNotImplemented("macro calls in expressions", builder().exprSpan(id));
                typed_ast_.set(id, types::kErrorType);
                return types::kErrorType;
            },
            [&](const ast::SeqNode &) {
                reportUnsupportedSyntax("word operator sequences", builder().exprSpan(id));
                typed_ast_.set(id, types::kErrorType);
                return types::kErrorType;
            },
            [&](const ast::WordCallNode &) {
                reportUnsupportedSyntax("word calls", builder().exprSpan(id));
                typed_ast_.set(id, types::kErrorType);
                return types::kErrorType;
            },
            [&](const ast::ErrorExprNode &) {
                // Error recovery node — propagate kErrorType silently.
                typed_ast_.set(id, types::kErrorType);
                return types::kErrorType;
            },
        },
        node);
}

types::TypeId SemaPipeline::visitLiteral(ast::ExprId id, const ast::LitValue &n) {
    auto default_type = defaultTypeForLit(n.kind, ctx_.types());
    typed_ast_.set(id, default_type);
    return default_type;
}

types::TypeId SemaPipeline::visitIdent(const ast::IdentNode &n, ast::ExprId id) {
    symbols::SymId sym = symbols::kInvalidSym;
    if (active_builder_ == &ctx_.builder() && resolved_ && id < resolved_->size())
        sym = (*resolved_)[id];
    if (sym == symbols::kInvalidSym)
        sym = ctx_.syms().lookup(n.name);
    if (sym != symbols::kInvalidSym && !isSymAccessible(sym)) {
        ctx_.diags().report(Severity::Error, UndefinedIdent,
                            "undefined identifier '" + std::string(n.name) + "'", n.span);
        sym = symbols::kInvalidSym;
    }
    if (sym == symbols::kInvalidSym) {
        const bool main_builder = active_builder_ == &ctx_.builder();
        if (!main_builder || !resolved_ || id >= resolved_->size() ||
            (*resolved_)[id] != symbols::kInvalidSym)
            ctx_.diags().report(Severity::Error, UndefinedIdent,
                                std::string("undefined identifier '") + std::string(n.name) + "'",
                                n.span);
        typed_ast_.set(id, types::kErrorType);
        return types::kErrorType;
    }

    auto &data               = ctx_.syms().get(sym);
    types::TypeId ident_type = types::kErrorType;
    if (data.kind == symbols::SymKind::Fn) {
        ident_type = ctx_.types().internUnknown();
    } else if (data.kind == symbols::SymKind::Variable && sym < var_types_.size()) {
        ident_type = var_types_[sym];
    }

    typed_ast_.set(id, ident_type);
    return ident_type;
}

types::TypeId SemaPipeline::visitBinary(ast::ExprId id, const ast::BinaryNode &n) {
    if (n.op == ast::BinaryOp::Is || n.op == ast::BinaryOp::As) {
        reportUnsupportedSyntax(n.op == ast::BinaryOp::Is ? "'is' expressions" : "'as' expressions",
                                n.span);
        typed_ast_.set(id, types::kErrorType);
        return types::kErrorType;
    }

    auto lhs_type = visitExpr(n.lhs);
    auto rhs_type = visitExpr(n.rhs);
    if (lhs_type == types::kErrorType || rhs_type == types::kErrorType) {
        typed_ast_.set(id, types::kErrorType);
        return types::kErrorType;
    }

    if (!unifier_.unify(lhs_type, rhs_type, n.span)) {
        typed_ast_.set(id, types::kErrorType);
        return types::kErrorType;
    }

    types::TypeId result_type = lhs_type;
    types::TypeId concrete    = result_type;
    if (ctx_.types().kindOf(result_type) == types::TypeKind::Int) {
        auto *type = std::get_if<types::TypeInt>(&ctx_.types().lookup(result_type));
        if (type && type->width == types::IntWidth::Literal)
            concrete = rhs_type;
    }

    concretizeLiteralExpr(n.lhs, concrete, n.span);
    concretizeLiteralExpr(n.rhs, concrete, n.span);
    if (ctx_.types().kindOf(result_type) == types::TypeKind::Int) {
        auto *type = std::get_if<types::TypeInt>(&ctx_.types().lookup(result_type));
        if (type && type->width == types::IntWidth::Literal)
            result_type = concrete;
    }

    typed_ast_.set(id, result_type);
    return result_type;
}

types::TypeId SemaPipeline::visitUnary(ast::ExprId id, const ast::UnaryNode &n) {
    switch (n.op) {
    case ast::UnaryOp::FallbackOpt:
    case ast::UnaryOp::FallbackRes:
    case ast::UnaryOp::PropagateOpt:
    case ast::UnaryOp::PropagateRes:
        reportUnsupportedSyntax("fallback and propagation operators", n.span);
        typed_ast_.set(id, types::kErrorType);
        return types::kErrorType;
    default:
        break;
    }

    auto operand_type = visitExpr(n.operand);
    if (operand_type == types::kErrorType) {
        typed_ast_.set(id, types::kErrorType);
        return types::kErrorType;
    }

    auto result_type = operand_type;
    switch (n.op) {
    case ast::UnaryOp::Neg:
    case ast::UnaryOp::Not:
        break;
    case ast::UnaryOp::Ref:
        if (ast::exprKind(builder().getExpr(n.operand)) != ast::ExprKind::Identifier)
            warnNotImplemented("references to non-identifiers", n.span);
        result_type = ctx_.types().internPtr(operand_type);
        break;
    case ast::UnaryOp::Deref:
        if (ctx_.types().kindOf(operand_type) != types::TypeKind::Ptr) {
            ctx_.diags().report(Severity::Error, TypeMismatch,
                                "cannot dereference a non-pointer value", n.span);
            result_type = types::kErrorType;
        } else {
            auto *ptr   = std::get_if<types::TypePtr>(&ctx_.types().lookup(operand_type));
            result_type = ptr ? ptr->pointee : types::kErrorType;
        }
        break;
    case ast::UnaryOp::FallbackOpt:
    case ast::UnaryOp::FallbackRes:
    case ast::UnaryOp::PropagateOpt:
    case ast::UnaryOp::PropagateRes:
        break;
    }

    typed_ast_.set(id, result_type);
    return result_type;
}

types::TypeId SemaPipeline::visitCall(ast::ExprId id, const ast::CallNode &n) {
    auto callee_type = visitExpr(n.callee);

    memory::DynArray<types::TypeId> arg_types{arena_};
    for (auto arg : n.args)
        arg_types.push(visitExpr(arg));

    symbols::SymId resolved_fn = symbols::kInvalidSym;
    types::TypeId result_type  = types::kErrorType;
    size_t match_count         = 0;

    auto &callee_expr = builder().getExpr(n.callee);
    std::string_view callee_name;
    if (auto *ident = std::get_if<ast::IdentNode>(&callee_expr))
        callee_name = ident->name;
    if (!callee_name.empty()) {
        auto candidates           = syms().lookupAll(callee_name, arena_);
        size_t fn_candidate_count = 0;

        for (auto sym_id : candidates) {
            auto &data = syms().get(sym_id);
            if (data.kind != symbols::SymKind::Fn)
                continue;
            if (!isSymAccessible(sym_id))
                continue;
            fn_candidate_count++;

            auto *fn_decl = fnDeclForSym(sym_id, nullptr, nullptr);
            if (!fn_decl || fn_decl->params.size() != n.args.size())
                continue;

            bool match = true;
            for (size_t i = 0; i < fn_decl->params.size(); ++i) {
                auto param_type = lowerFnParamType(sym_id, i);
                if (param_type == types::kErrorType || i >= arg_types.size() ||
                    arg_types[i] == types::kErrorType)
                    continue;
                if (!unifier_.isCoercible(param_type, arg_types[i])) {
                    match = false;
                    break;
                }
            }

            if (match) {
                match_count++;
                if (match_count == 1) {
                    resolved_fn = sym_id;
                    result_type = lowerFnReturnType(sym_id);
                    if (!fn_decl->is_extern && fn_decl->body != ast::kInvalidExpr)
                        worklist_.push(sym_id);
                }
            }
        }

        if (match_count == 0 && fn_candidate_count > 0) {
            if (!n.args.empty()) {
                ctx_.diags().report(
                    Severity::Error, NoMatchingFn,
                    "no matching function for call to '" + std::string(callee_name) + "'", n.span);
            } else {
                ctx_.diags().report(Severity::Error, WrongArity,
                                    "wrong number of arguments in call", n.span);
            }
        } else if (match_count > 1) {
            ctx_.diags().report(Severity::Error, AmbiguousCall,
                                "ambiguous call '" + std::string(callee_name) +
                                    "' — multiple functions match",
                                n.span);
            resolved_fn = symbols::kInvalidSym;
            result_type = types::kErrorType;
        }
    }

    if (resolved_fn != symbols::kInvalidSym) {
        auto *fn_decl = fnDeclForSym(resolved_fn, nullptr, nullptr);
        if (fn_decl) {
            for (size_t i = 0; i < fn_decl->params.size() && i < n.args.size(); ++i) {
                auto param_type = lowerFnParamType(resolved_fn, i);
                if (param_type != types::kErrorType && i < arg_types.size() &&
                    arg_types[i] != types::kErrorType)
                    concretizeLiteralExpr(n.args[i], param_type, n.span);
            }
        }
    } else if (callee_type != types::kErrorType) {
        auto &fn_type = ctx_.types().lookup(callee_type);
        auto fn_ptr   = std::get_if<types::TypeFn>(&fn_type);
        if (fn_ptr) {
            if (arg_types.size() != fn_ptr->param_count) {
                ctx_.diags().report(Severity::Error, WrongArity,
                                    "wrong number of arguments in call", {});
            } else {
                for (size_t i = 0; i < arg_types.size(); ++i) {
                    if (arg_types[i] != types::kErrorType)
                        unifier_.unify(arg_types[i], fn_ptr->params[i], n.span);
                    concretizeLiteralExpr(n.args[i], fn_ptr->params[i], n.span);
                }
                result_type = fn_ptr->ret;
            }
        }
    }

    typed_ast_.set(id, result_type);
    return result_type;
}

types::TypeId SemaPipeline::visitBlock(ast::ExprId id, const ast::BlockNode &n) {
    for (auto stmt_id : n.stmts)
        visitStmt(stmt_id);

    types::TypeId result_type = types::kVoidType;
    if (n.trailing != ast::kInvalidExpr)
        result_type = visitExpr(n.trailing);

    typed_ast_.set(id, result_type);
    return result_type;
}

types::TypeId SemaPipeline::visitIf(ast::ExprId id, const ast::IfNode &n) {
    visitExpr(n.cond);

    auto then_type = visitExpr(n.then_branch);
    if (n.else_branch == ast::kInvalidExpr) {
        typed_ast_.set(id, types::kVoidType);
        return types::kVoidType;
    }

    auto else_type = visitExpr(n.else_branch);
    if (then_type == types::kErrorType || else_type == types::kErrorType ||
        !unifier_.unify(then_type, else_type, n.span)) {
        typed_ast_.set(id, types::kErrorType);
        return types::kErrorType;
    }

    types::TypeId result_type = then_type;
    types::TypeId concrete    = result_type;
    if (ctx_.types().kindOf(result_type) == types::TypeKind::Int) {
        auto *type = std::get_if<types::TypeInt>(&ctx_.types().lookup(result_type));
        if (type && type->width == types::IntWidth::Literal)
            concrete = else_type;
    }

    concretizeLiteralExpr(implicitReturnExpr(n.then_branch), concrete, n.span);
    concretizeLiteralExpr(implicitReturnExpr(n.else_branch), concrete, n.span);
    if (ctx_.types().kindOf(result_type) == types::TypeKind::Int) {
        auto *type = std::get_if<types::TypeInt>(&ctx_.types().lookup(result_type));
        if (type && type->width == types::IntWidth::Literal)
            result_type = concrete;
    }

    typed_ast_.set(id, result_type);
    return result_type;
}

types::TypeId SemaPipeline::visitWhile(ast::ExprId id, const ast::WhileNode &n) {
    visitExpr(n.cond);
    visitExpr(n.body);
    typed_ast_.set(id, types::kVoidType);
    return types::kVoidType;
}

types::TypeId SemaPipeline::visitWhen(ast::ExprId id, const ast::WhenNode &n) {
    auto subject_type = visitExpr(n.subject);
    if (subject_type == types::kErrorType) {
        typed_ast_.set(id, types::kErrorType);
        return types::kErrorType;
    }

    types::TypeId result_type = types::kVoidType;
    bool has_value_body = false;
    bool has_void_body = false;
    bool has_default = false;

    for (size_t i = 0; i < n.cases.size(); i++) {
        auto &c = n.cases[i];

        if (auto *ident = std::get_if<ast::IdentNode>(&builder().getExpr(c.condition))) {
            if (ident->name == "_") {
                if (has_default) {
                    ctx_.diags().report(diagnostics::Severity::Error, diagnostics::err::ExpectedExpr,
                                        "duplicate default case", c.span);
                    typed_ast_.set(id, types::kErrorType);
                    return types::kErrorType;
                }
                if (i + 1 < n.cases.size()) {
                    ctx_.diags().report(diagnostics::Severity::Error, diagnostics::err::ExpectedExpr,
                                        "default case must be the last case", c.span);
                    typed_ast_.set(id, types::kErrorType);
                    return types::kErrorType;
                }
                has_default = true;
            }
        }

        auto cond_type = visitExpr(c.condition);
        if (cond_type != types::kErrorType && cond_type != types::kBoolType) {
            if (!unifier_.unify(cond_type, types::kBoolType, c.span)) {
                typed_ast_.set(id, types::kErrorType);
                return types::kErrorType;
            }
        }

        auto body_type = visitExpr(c.body);
        if (body_type == types::kErrorType) {
            typed_ast_.set(id, types::kErrorType);
            return types::kErrorType;
        }

        if (body_type == types::kVoidType) {
            has_void_body = true;
        } else {
            has_value_body = true;
            if (result_type == types::kVoidType) {
                result_type = body_type;
            } else if (!unifier_.unify(result_type, body_type, c.span)) {
                typed_ast_.set(id, types::kErrorType);
                return types::kErrorType;
            }
        }
    }

    if (!has_value_body) {
        result_type = types::kVoidType;
    } else if (has_void_body || !has_default) {
        result_type = ctx_.types().internOptional(result_type);
    }

    typed_ast_.set(id, result_type);
    return result_type;
}

void SemaPipeline::visitMarker(const ast::MarkerNode &n) {
    labelMap_.insert(n.name, labelMap_.size());
    for (auto stmt_id : n.body)
        visitStmt(stmt_id);
}

void SemaPipeline::visitGoto(const ast::GotoNode &n) {
    if (!labelMap_.get(n.target)) {
        ctx_.diags().report(Severity::Error, UndefinedIdent,
                            "undefined marker '" + std::string(n.target) + "'", n.span);
    }
}

void SemaPipeline::visitStmt(ast::StmtId id) {
    if (id == ast::kInvalidStmt)
        return;

    auto &node = builder().getStmt(id);
    std::visit(
        common::overloaded{
            [&](const ast::LetNode &n) {
                types::TypeId decl_type = types::kErrorType;
                if (n.type_annot != ast::kInvalidTypeExpr) {
                    types::TypeLower lower(builder(), ctx_.types(), ctx_.diags(), ctx_.syms());
                    decl_type = lower.lower(n.type_annot);
                }

                types::TypeId init_type = types::kErrorType;
                if (n.init != ast::kInvalidExpr)
                    init_type = visitExpr(n.init);

                if (n.type_annot != ast::kInvalidTypeExpr && n.init != ast::kInvalidExpr &&
                    decl_type != types::kErrorType && init_type != types::kErrorType &&
                    unifier_.unify(decl_type, init_type, n.span)) {
                    types::TypeId concrete = decl_type;
                    if (ctx_.types().kindOf(decl_type) == types::TypeKind::Int) {
                        auto *type = std::get_if<types::TypeInt>(&ctx_.types().lookup(decl_type));
                        if (type && type->width == types::IntWidth::Literal)
                            concrete = init_type;
                    }
                    concretizeLiteralExpr(n.init, concrete, n.span);
                }

                types::TypeId var_type = init_type;
                if (n.type_annot != ast::kInvalidTypeExpr && decl_type != types::kErrorType)
                    var_type = decl_type;
                else if (init_type != types::kErrorType)
                    var_type = init_type;

                for (auto var_name : n.names) {
                    auto sym_id = syms().declare(var_name, symbols::SymbolVisibility::Private, 0,
                                                 symbols::SymKind::Variable, ast::kInvalidDecl,
                                                 memory::Span{});
                    if (sym_id != symbols::kInvalidSym) {
                        ensureVarTypeCapacity(sym_id);
                        var_types_[sym_id] = var_type;
                    }
                }
            },
            [&](const ast::AssignNode &n) {
                auto target_type = visitExpr(n.target);
                auto value_type  = visitExpr(n.value);
                if (target_type != types::kErrorType && value_type != types::kErrorType &&
                    unifier_.unify(target_type, value_type, n.span)) {
                    types::TypeId concrete = target_type;
                    if (ctx_.types().kindOf(target_type) == types::TypeKind::Int) {
                        auto *type = std::get_if<types::TypeInt>(&ctx_.types().lookup(target_type));
                        if (type && type->width == types::IntWidth::Literal)
                            concrete = value_type;
                    }
                    concretizeLiteralExpr(n.value, concrete, n.span);
                }
            },
            [&](const ast::RetNode &n) {
                auto val_type = types::kVoidType;
                if (n.value != ast::kInvalidExpr)
                    val_type = visitExpr(n.value);

                if (val_type != types::kErrorType && current_fn_ret_type_ != types::kErrorType &&
                    unifier_.unify(val_type, current_fn_ret_type_, n.span)) {
                    types::TypeId concrete = current_fn_ret_type_;
                    if (ctx_.types().kindOf(current_fn_ret_type_) == types::TypeKind::Int) {
                        auto *type =
                            std::get_if<types::TypeInt>(&ctx_.types().lookup(current_fn_ret_type_));
                        if (type && type->width == types::IntWidth::Literal)
                            concrete = val_type;
                    }
                    concretizeLiteralExpr(n.value, concrete, n.span);
                }
            },
            [&](const ast::GotoNode &n) { visitGoto(n); },
            [&](const ast::MarkerNode &n) { visitMarker(n); },
            [&](const ast::ExprStmtNode &n) { visitExpr(n.expr); },
            [&](const ast::UseNode &n) { reportUnsupportedSyntax("use statements", n.span); },
            [&](const ast::ErrorStmtNode &) { /* silently ignore error-recovery statement */ },
        },
        node);
}

} // namespace zith::sema
