#include "legacy-zith/sema/hir-lower.hpp"
#include "legacy-zith/ast/ast-node-utils.hpp"
#include "common/overloaded.hpp"
#include "diagnostics/error-codes.hpp"
#include "symbols/symbol-id.hpp"
#include "types/type-id.hpp"
#include "types/type-lower.hpp"

#include <cstdlib>
#include <string>

namespace zith::sema {

namespace {

using ast::BinaryOp;
using ast::LitKind;
using ast::UnaryOp;
using diagnostics::Severity;
using diagnostics::err::AmbiguousCall;
using diagnostics::err::NoMatchingFn;
using diagnostics::err::UndefinedIdent;
using diagnostics::err::WrongArity;

hir::HirBinaryOp mapBinaryOp(ast::BinaryOp op) {
    switch (op) {
    case BinaryOp::Add:
        return hir::HirBinaryOp::Add;
    case BinaryOp::Sub:
        return hir::HirBinaryOp::Sub;
    case BinaryOp::Mul:
        return hir::HirBinaryOp::Mul;
    case BinaryOp::Div:
        return hir::HirBinaryOp::Div;
    case BinaryOp::Rest:
        return hir::HirBinaryOp::Rem;
    case BinaryOp::Eq:
        return hir::HirBinaryOp::Eq;
    case BinaryOp::Ne:
        return hir::HirBinaryOp::Ne;
    case BinaryOp::Lt:
        return hir::HirBinaryOp::Lt;
    case BinaryOp::Le:
        return hir::HirBinaryOp::Le;
    case BinaryOp::Gt:
        return hir::HirBinaryOp::Gt;
    case BinaryOp::Ge:
        return hir::HirBinaryOp::Ge;
    case BinaryOp::And:
        return hir::HirBinaryOp::And;
    case BinaryOp::Or:
        return hir::HirBinaryOp::Or;
    case BinaryOp::Xor:
        return hir::HirBinaryOp::Xor;
    case BinaryOp::Is:
    case BinaryOp::As:
        // Semantic analysis rejects these before lowering.
        std::abort();
    case BinaryOp::Shl:
        return hir::HirBinaryOp::Shl;
    case BinaryOp::Shr:
        return hir::HirBinaryOp::Shr;
    }
}

hir::HirUnaryOp mapUnaryOp(ast::UnaryOp op) {
    switch (op) {
    case UnaryOp::Neg:
        return hir::HirUnaryOp::Neg;
    case UnaryOp::Not:
        return hir::HirUnaryOp::Not;
    case UnaryOp::Ref:
        return hir::HirUnaryOp::Ref;
    case UnaryOp::Deref:
        return hir::HirUnaryOp::Deref;
    case UnaryOp::FallbackOpt:
    case UnaryOp::FallbackRes:
    case UnaryOp::PropagateOpt:
    case UnaryOp::PropagateRes:
        // Semantic analysis rejects these before lowering.
        std::abort();
    }
    return hir::HirUnaryOp::Neg;
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

} // namespace

HirLower::HirLower(symbols::SymbolTable &syms, types::TypeIntern &types,
                   diagnostics::DiagnosticEngine &diags, ast::AstBuilder &builder,
                   memory::Arena &hir_arena, const TypedAst &typed_ast,
                   const memory::DynArray<symbols::SymId> *resolved,
                   symbols::ImportManager *import_mgr)
    : ctx_(syms, types, diags, builder), unifier_(types, diags, hir_arena), hir_arena_(hir_arena),
      hir_(hir_arena), typed_ast_(typed_ast), resolved_(resolved), import_mgr_(import_mgr),
      worklist_(hir_arena), fn_syms_(hir_arena), active_builder_(&builder),
      allowed_files_(hir_arena), var_types_(hir_arena) {}

ast::AstBuilder &HirLower::builder() const {
    return *active_builder_;
}

ast::AstBuilder *HirLower::builderForSym(symbols::SymId fn_sym) {
    if (import_mgr_) {
        auto origin = import_mgr_->originOf(fn_sym);
        if (origin)
            return import_mgr_->get(origin->file_idx).builder;
    }
    return &ctx_.builder();
}

const symbols::SymbolTable *HirLower::symsForSym(symbols::SymId fn_sym) {
    if (import_mgr_) {
        auto origin = import_mgr_->originOf(fn_sym);
        if (origin)
            return &import_mgr_->get(origin->file_idx).symbols;
    }
    return &ctx_.syms();
}

size_t HirLower::hirIndexForSym(symbols::SymId fn_sym) const {
    for (size_t i = 0; i < fn_syms_.size(); ++i)
        if (fn_syms_[i] == fn_sym)
            return i;
    return static_cast<size_t>(-1);
}

bool HirLower::isSymAccessible(symbols::SymId sym_id) const {
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

types::TypeId HirLower::astExprType(ast::ExprId id) const {
    return typed_ast_.get(active_builder_, id);
}

hir::HirExprId HirLower::addHirExpr(hir::HirExpr expr) {
    return hir_.addExpr(std::move(expr));
}

void HirLower::ensureStub(symbols::SymId fn_sym) {
    if (hirIndexForSym(fn_sym) != static_cast<size_t>(-1))
        return;
    auto *source_syms = symsForSym(fn_sym);
    auto *source_bld  = builderForSym(fn_sym);
    if (!source_syms || !source_bld)
        return;
    auto local_sym = fn_sym;
    if (import_mgr_) {
        auto origin = import_mgr_->originOf(fn_sym);
        if (origin)
            local_sym = origin->local_sym;
    }
    const auto &data = source_syms->get(local_sym);
    if (data.kind != symbols::SymKind::Fn || data.decl_id == ast::kInvalidDecl)
        return;
    auto *fn = ast::asFn(source_bld->getDecl(data.decl_id));
    if (!fn)
        return;

    auto &hfn   = hir_.addFn(data.name);
    hfn.decl_id = data.decl_id;
    hfn.sym_id  = fn_sym;
    types::TypeLower lower(*source_bld, ctx_.types(), ctx_.diags(), ctx_.syms());
    for (const auto &param : fn->params) {
        hfn.params.push(lower.lower(param.type));
        hfn.param_names.push(ctx_.syms().interner().intern(param.name));
    }
    hfn.return_type = lower.lower(fn->return_type);
    fn_syms_.push(fn_sym);
}

void HirLower::ensureBodyLowered(symbols::SymId fn_sym) {
    ensureStub(fn_sym);
    auto idx = hirIndexForSym(fn_sym);
    if (idx == static_cast<size_t>(-1))
        return;
    auto &hfn = hir_.getFn(idx);
    if (!hfn.blocks.empty())
        return;
    auto *source_bld  = builderForSym(fn_sym);
    auto *source_syms = symsForSym(fn_sym);
    if (!source_bld || !source_syms)
        return;
    auto local_sym = fn_sym;
    if (import_mgr_) {
        auto origin = import_mgr_->originOf(fn_sym);
        if (origin)
            local_sym = origin->local_sym;
    }
    const auto &data = source_syms->get(local_sym);
    auto *fn         = ast::asFn(source_bld->getDecl(data.decl_id));
    if (!fn || fn->is_extern || fn->body == ast::kInvalidExpr)
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
        }
    }

    auto *previous_builder = active_builder_;
    auto *previous_fn      = current_fn_;
    auto previous_ret      = current_fn_ret_type_;
    active_builder_        = source_bld;
    current_fn_            = &hfn;
    current_fn_ret_type_   = hfn.return_type;
    auto scope             = ctx_.syms().enterScope();
    for (const auto &param : fn->params)
        ctx_.syms().declareInScope(scope, param.name);
    hfn.blocks.emplace(hir_arena_);
    currentBlock_ = 0;
    labelMap_.clear();
    auto &entry = hfn.blocks[0];
    entry.insts = memory::DynArray<hir::HirExprId>(hir_arena_);
    auto body   = visitExpr(fn->body);
    if (entry.terminator == hir::kInvalidHirExpr) {
        hir::HirRet ret;
        if (current_fn_ret_type_ != types::kErrorType &&
            ctx_.types().kindOf(current_fn_ret_type_) != types::TypeKind::Void) {
            if (body != hir::kInvalidHirExpr)
                ret.value = body;
            else
                ret.value = hir::kInvalidHirExpr;
        } else {
            ret.value = hir::kInvalidHirExpr;
        }
        if (current_fn_ret_type_ == types::kErrorType ||
            ctx_.types().kindOf(current_fn_ret_type_) == types::TypeKind::Void ||
            body != hir::kInvalidHirExpr) {
            setTerminator(hir_.addExpr(hir::HirExpr{ret}));
        }
    }
    ctx_.syms().exitScope();
    current_fn_          = previous_fn;
    current_fn_ret_type_ = previous_ret;
    active_builder_      = previous_builder;
    allowed_files_       = std::move(previous_allowed);
}

bool HirLower::run(const ast::ProgramNode &program) {
    for (auto decl_id : program.decls) {
        auto &decl = ctx_.builder().getDecl(decl_id);
        if (ast::asFn(decl)) {
            for (symbols::SymId sym = 0; sym < ctx_.syms().symbolCount(); ++sym) {
                const auto &data = ctx_.syms().get(sym);
                if (data.kind == symbols::SymKind::Fn && data.decl_id == decl_id &&
                    (!import_mgr_ || !import_mgr_->originOf(sym))) {
                    ensureStub(sym);
                    break;
                }
            }
        }
    }

    {
        memory::DynArray<size_t> saved = std::move(allowed_files_);
        if (import_mgr_) {
            for (auto dep : import_mgr_->rootDeps())
                allowed_files_.push(dep);
        }
        for (auto decl_id : program.decls) {
            auto &decl = ctx_.builder().getDecl(decl_id);
            if (ast::asFn(decl)) {
                for (symbols::SymId sym = 0; sym < ctx_.syms().symbolCount(); ++sym) {
                    const auto &data = ctx_.syms().get(sym);
                    if (data.kind == symbols::SymKind::Fn && data.decl_id == decl_id &&
                        (!import_mgr_ || !import_mgr_->originOf(sym))) {
                        ensureBodyLowered(sym);
                        break;
                    }
                }
            }
        }
        allowed_files_ = std::move(saved);
    }

    for (size_t i = 0; i < worklist_.size(); ++i)
        ensureBodyLowered(worklist_[i]);

    return !ctx_.diags().hasErrors();
}

hir::HirExprId HirLower::visitExpr(ast::ExprId id) {
    if (id == ast::kInvalidExpr)
        return hir::kInvalidHirExpr;

    auto &node = builder().getExpr(id);
    return std::visit(
        common::overloaded{
            [&](const ast::LitValue &n) { return visitLiteral(id, n); },
            [&](const ast::IdentNode &n) { return visitIdent(n, id); },
            [&](const ast::BinaryNode &n) { return visitBinary(id, n); },
            [&](const ast::UnaryNode &n) { return visitUnary(id, n); },
            [&](const ast::CallNode &n) { return visitCall(id, n); },
            [&](const ast::BlockNode &n) { return visitBlock(n); },
            [&](const ast::IfNode &n) { return visitIf(n); },
            [&](const ast::WhileNode &n) { return visitWhile(n); },
            [&](const ast::WhenNode &n) { return visitWhen(id, n); },
            [&](const ast::FieldNode &n) -> hir::HirExprId {
                auto object = visitExpr(n.object);
                if (object == hir::kInvalidHirExpr)
                    return hir::kInvalidHirExpr;
                auto object_type = typed_ast_.get(n.object);
                auto index       = ctx_.types().fieldIndex(object_type, n.field);
                if (index == static_cast<size_t>(-1))
                    return hir::kInvalidHirExpr;
                hir::HirField field;
                field.object      = object;
                field.index       = static_cast<uint32_t>(index);
                field.type        = typed_ast_.get(id);
                field.object_type = object_type;
                return addHirExpr(field);
            },
            [&](const ast::IndexNode &n) -> hir::HirExprId {
                auto object = visitExpr(n.object);
                auto index  = visitExpr(n.index);
                if (object == hir::kInvalidHirExpr || index == hir::kInvalidHirExpr)
                    return hir::kInvalidHirExpr;
                hir::HirIndex idx;
                idx.object   = object;
                idx.index    = index;
                idx.type     = typed_ast_.get(id);
                idx.obj_type = typed_ast_.get(n.object);
                idx.is_array = (ctx_.types().kindOf(idx.obj_type) == types::TypeKind::Array);
                return addHirExpr(idx);
            },
            [&](const ast::StructLiteralNode &n) { return visitStructLiteral(id, n); },
            [&](const ast::ArrayLiteralNode &n) { return visitArrayLiteral(id, n); },
            [&](const ast::EnumValueNode &n) { return visitEnumValue(id, n); },
            [](const ast::RangeNode &) -> hir::HirExprId { return hir::kInvalidHirExpr; },
            [](const ast::UnbodyNode &) -> hir::HirExprId { return hir::kInvalidHirExpr; },
            [](const ast::IntrinsicNode &) -> hir::HirExprId { return hir::kInvalidHirExpr; },
            [](const ast::MacroCallNode &) -> hir::HirExprId { return hir::kInvalidHirExpr; },
            [](const ast::SeqNode &) -> hir::HirExprId { return hir::kInvalidHirExpr; },
            [](const ast::WordCallNode &) -> hir::HirExprId { return hir::kInvalidHirExpr; },
            [](const ast::ErrorExprNode &) -> hir::HirExprId { return hir::kInvalidHirExpr; },
        },
        node);
}

hir::HirExprId HirLower::visitLiteral(ast::ExprId id, const ast::LitValue &n) {
    auto literal_type = astExprType(id);
    if (literal_type == types::kErrorType)
        literal_type = defaultTypeForLit(n.kind, ctx_.types());

    hir::HirLiteral lit{};
    lit.type = literal_type;
    switch (n.kind) {
    case ast::LitKind::Int:
        lit.i = std::strtoll(std::string(n.raw).data(), nullptr, 10);
        break;
    case ast::LitKind::Float:
        lit.f = std::strtod(std::string(n.raw).data(), nullptr);
        break;
    case ast::LitKind::Bool:
        lit.b = (n.raw == "true");
        break;
    case ast::LitKind::Char: {
        auto raw = n.raw;
        lit.i    = raw.size() >= 3 && raw[0] == '\'' ? raw[1] : 0;
        break;
    }
    case ast::LitKind::String: {
        auto raw = n.raw;
        if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
            raw = raw.substr(1, raw.size() - 2);
        lit.str_val = ctx_.syms().interner().intern(raw);
        break;
    }
    default:
        break;
    }
    return addHirExpr(hir::HirExpr{lit});
}

hir::HirExprId HirLower::visitIdent(const ast::IdentNode &n, ast::ExprId id) {
    symbols::SymId sym = symbols::kInvalidSym;
    if (active_builder_ == &ctx_.builder() && resolved_ && id != ast::kInvalidExpr &&
        id < resolved_->size())
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
        return hir::kInvalidHirExpr;
    }

    hir::HirVar var;
    var.name    = ctx_.syms().interner().intern(n.name);
    var.version = 0;
    return addHirExpr(hir::HirExpr{var});
}

hir::HirExprId HirLower::visitBinary(ast::ExprId id, const ast::BinaryNode &n) {
    auto lhs = visitExpr(n.lhs);
    auto rhs = visitExpr(n.rhs);
    if (lhs == hir::kInvalidHirExpr || rhs == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;

    hir::HirBinary bin;
    bin.lhs  = lhs;
    bin.rhs  = rhs;
    bin.op   = mapBinaryOp(n.op);
    bin.type = astExprType(id);
    return addHirExpr(hir::HirExpr{bin});
}

hir::HirExprId HirLower::visitUnary(ast::ExprId id, const ast::UnaryNode &n) {
    auto operand = visitExpr(n.operand);
    if (operand == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;

    hir::HirUnary un;
    un.op      = mapUnaryOp(n.op);
    un.operand = operand;
    un.type    = astExprType(id);
    return addHirExpr(hir::HirExpr{un});
}

hir::HirExprId HirLower::visitCall(ast::ExprId id, const ast::CallNode &n) {
    auto callee = visitExpr(n.callee);
    if (callee == hir::kInvalidHirExpr)
        return hir::kInvalidHirExpr;

    auto callee_type = astExprType(n.callee);

    memory::DynArray<hir::HirExprId> hir_args{hir_arena_};
    memory::DynArray<types::TypeId> arg_types{hir_arena_};
    for (auto arg : n.args) {
        auto hir_arg = visitExpr(arg);
        if (hir_arg != hir::kInvalidHirExpr) {
            hir_args.push(hir_arg);
            arg_types.push(astExprType(arg));
        }
    }

    symbols::SymId resolved_fn = symbols::kInvalidSym;
    size_t match_count         = 0;

    const auto &callee_expr = hir_.getExpr(callee);
    if (auto *var = std::get_if<hir::HirVar>(&callee_expr)) {
        auto callee_name          = var->name;
        auto candidates           = ctx_.syms().lookupAll(callee_name, hir_arena_);
        size_t fn_candidate_count = 0;
        for (auto sym_id : candidates) {
            auto &data = ctx_.syms().get(sym_id);
            if (data.kind != symbols::SymKind::Fn)
                continue;
            if (!isSymAccessible(sym_id))
                continue;
            fn_candidate_count++;
            if (data.decl_id == ast::kInvalidDecl)
                continue;

            ensureStub(sym_id);
            auto fi = hirIndexForSym(sym_id);
            if (fi == static_cast<size_t>(-1))
                continue;

            auto &hfn = hir_.getFn(fi);
            if (hfn.params.size() != n.args.size())
                continue;

            bool match = true;
            for (size_t i = 0; i < hfn.params.size(); i++) {
                if (i >= arg_types.size() || arg_types[i] == types::kErrorType)
                    continue;
                if (hfn.params[i] == types::kErrorType)
                    continue;
                if (!unifier_.isCoercible(hfn.params[i], arg_types[i])) {
                    match = false;
                    break;
                }
            }

            if (match) {
                match_count++;
                if (match_count == 1) {
                    resolved_fn = sym_id;
                    auto *bld   = builderForSym(sym_id);
                    if (bld) {
                        auto local_sym = sym_id;
                        if (import_mgr_) {
                            auto origin = import_mgr_->originOf(sym_id);
                            if (origin)
                                local_sym = origin->local_sym;
                        }
                        auto *source_syms = symsForSym(sym_id);
                        if (source_syms) {
                            const auto &source_data = source_syms->get(local_sym);
                            auto *fn_decl           = ast::asFn(bld->getDecl(source_data.decl_id));
                            if (fn_decl && !fn_decl->is_extern &&
                                fn_decl->body != ast::kInvalidExpr)
                                worklist_.push(sym_id);
                        }
                    }
                }
            }
        }

        if (match_count == 0 && fn_candidate_count > 0) {
            if (!n.args.empty()) {
                ctx_.diags().report(Severity::Error, NoMatchingFn,
                                    "no matching function for call to '" +
                                        std::string(ctx_.syms().interner().lookup(callee_name)) +
                                        "'",
                                    n.span);
            } else {
                ctx_.diags().report(Severity::Error, WrongArity,
                                    "wrong number of arguments in call", n.span);
            }
        } else if (match_count > 1) {
            ctx_.diags().report(Severity::Error, AmbiguousCall,
                                "ambiguous call '" +
                                    std::string(ctx_.syms().interner().lookup(callee_name)) +
                                    "' — multiple functions match",
                                n.span);
            resolved_fn = symbols::kInvalidSym;
        }
    }

    if (resolved_fn == symbols::kInvalidSym && callee_type != types::kErrorType) {
        auto &fn_type = ctx_.types().lookup(callee_type);
        auto fn_ptr   = std::get_if<types::TypeFn>(&fn_type);
        if (fn_ptr && hir_args.size() != fn_ptr->param_count) {
            ctx_.diags().report(Severity::Error, WrongArity, "wrong number of arguments in call",
                                {});
        }
    }

    hir::HirCall call{callee, std::move(hir_args)};
    call.resolved_fn = resolved_fn;
    (void)id;
    return addHirExpr(hir::HirExpr{std::move(call)});
}

hir::HirExprId HirLower::visitBlock(const ast::BlockNode &n) {
    hir::HirExprId last = hir::kInvalidHirExpr;
    for (auto stmt_id : n.stmts)
        visitStmt(stmt_id);
    if (n.trailing != ast::kInvalidExpr) {
        last = visitExpr(n.trailing);
        if (current_fn_ && !current_fn_->blocks.empty() && last != hir::kInvalidHirExpr) {
            if (ctx_.types().kindOf(current_fn_ret_type_) == types::TypeKind::Void ||
                current_fn_ret_type_ == types::kErrorType) {
                current_fn_->blocks[currentBlock()].insts.push(last);
            }
        }
    }
    return last;
}

hir::HirExprId HirLower::visitIf(const ast::IfNode &n) {
    auto origBlock = currentBlock_;
    auto cond      = visitExpr(n.cond);

    auto then_block = newBlock();
    setCurrentBlock(then_block);
    current_fn_->blocks[then_block].insts = memory::DynArray<hir::HirExprId>(hir_arena_);
    visitExpr(n.then_branch);
    bool thenTerminated = current_fn_->blocks[then_block].terminator != hir::kInvalidHirExpr;

    size_t else_target;

    if (n.else_branch != ast::kInvalidExpr) {
        auto else_block = newBlock();
        setCurrentBlock(else_block);
        current_fn_->blocks[else_block].insts = memory::DynArray<hir::HirExprId>(hir_arena_);
        visitExpr(n.else_branch);
        bool elseTerminated = current_fn_->blocks[else_block].terminator != hir::kInvalidHirExpr;

        if (thenTerminated && elseTerminated) {
            size_t dead = newBlock();
            setCurrentBlock(dead);
            current_fn_->blocks[dead].insts = memory::DynArray<hir::HirExprId>(hir_arena_);
            else_target                     = else_block;
        } else if (thenTerminated) {
            else_target = else_block;
        } else if (elseTerminated) {
            auto merge = newBlock();
            setCurrentBlock(then_block);
            emitJump(merge);
            setCurrentBlock(merge);
            current_fn_->blocks[merge].insts = memory::DynArray<hir::HirExprId>(hir_arena_);
            else_target                      = else_block;
        } else {
            auto merge = newBlock();
            setCurrentBlock(then_block);
            emitJump(merge);
            setCurrentBlock(else_block);
            emitJump(merge);
            setCurrentBlock(merge);
            current_fn_->blocks[merge].insts = memory::DynArray<hir::HirExprId>(hir_arena_);
            else_target                      = else_block;
        }
    } else {
        if (thenTerminated) {
            auto cont = newBlock();
            setCurrentBlock(cont);
            current_fn_->blocks[cont].insts = memory::DynArray<hir::HirExprId>(hir_arena_);
            else_target                     = cont;
        } else {
            auto cont = newBlock();
            setCurrentBlock(then_block);
            emitJump(cont);
            setCurrentBlock(cont);
            current_fn_->blocks[cont].insts = memory::DynArray<hir::HirExprId>(hir_arena_);
            else_target                     = cont;
        }
    }

    hir::HirBranch branch;
    branch.cond                               = cond;
    branch.then_block                         = static_cast<hir::HirDeclId>(then_block);
    branch.else_block                         = static_cast<hir::HirDeclId>(else_target);
    current_fn_->blocks[origBlock].terminator = hir_.addExpr(hir::HirExpr{std::move(branch)});
    return hir::kInvalidHirExpr;
}

hir::HirExprId HirLower::visitWhile(const ast::WhileNode &n) {
    auto header_block = newBlock();
    auto body_block   = newBlock();
    auto exit_block   = newBlock();

    emitJump(header_block);

    setCurrentBlock(header_block);
    current_fn_->blocks[header_block].insts = memory::DynArray<hir::HirExprId>(hir_arena_);
    auto cond                               = visitExpr(n.cond);

    hir::HirBranch branch;
    branch.cond       = cond;
    branch.then_block = static_cast<hir::HirDeclId>(body_block);
    branch.else_block = static_cast<hir::HirDeclId>(exit_block);
    setTerminator(hir_.addExpr(hir::HirExpr{std::move(branch)}));

    setCurrentBlock(body_block);
    current_fn_->blocks[body_block].insts = memory::DynArray<hir::HirExprId>(hir_arena_);
    visitExpr(n.body);
    if (current_fn_->blocks[body_block].terminator == hir::kInvalidHirExpr)
        emitJump(header_block);

    setCurrentBlock(exit_block);
    current_fn_->blocks[exit_block].insts = memory::DynArray<hir::HirExprId>(hir_arena_);
    return hir::kInvalidHirExpr;
}

hir::HirExprId HirLower::visitWhen(ast::ExprId id, const ast::WhenNode &n) {
    auto result_type  = astExprType(id);
    auto subject_type = astExprType(n.subject);

    // Allocate slot for subject — evaluated exactly once
    auto subj_slot = allocSlot();
    auto subj_val  = visitExpr(n.subject);
    current_fn_->blocks[currentBlock_].insts.push(emitSlotAlloca(subj_slot, subject_type));
    if (subj_val != hir::kInvalidHirExpr)
        current_fn_->blocks[currentBlock_].insts.push(emitSlotStore(subj_slot, subj_val));

    // Result slot for value-producing when
    bool has_value   = result_type != types::kVoidType;
    bool is_optional = has_value && ctx_.types().kindOf(result_type) == types::TypeKind::Optional;
    types::TypeId inner_t = types::kErrorType;
    if (is_optional) {
        auto *opt = std::get_if<types::TypeOptional>(&ctx_.types().lookup(result_type));
        if (opt)
            inner_t = opt->inner;
    }
    hir::HirSlotId result_slot = hir::kInvalidHirSlot;
    if (has_value) {
        result_slot = allocSlot();
        current_fn_->blocks[currentBlock_].insts.push(
            emitSlotAlloca(result_slot, is_optional ? inner_t : result_type));
    }

    size_t merge_block       = newBlock();
    size_t no_match_block    = merge_block;
    bool need_no_match_block = is_optional && !n.cases.empty();
    if (need_no_match_block) {
        no_match_block = newBlock();
        setCurrentBlock(no_match_block);
        current_fn_->blocks[no_match_block].insts = memory::DynArray<hir::HirExprId>(hir_arena_);
        emitJump(merge_block);
    }

    for (size_t i = 0; i < n.cases.size(); i++) {
        auto &c         = n.cases[i];
        bool is_last    = (i + 1 == n.cases.size());
        bool is_default = c.is_default;

        auto body_block = newBlock();
        size_t next_block;
        if (is_default) {
            next_block = body_block; // just fall through
        } else if (is_last) {
            next_block = no_match_block;
        } else {
            next_block = newBlock();
        }

        if (is_default) {
            emitJump(body_block);
        } else {
            auto cond = lowerWhenCondition(c.condition, subj_slot, subject_type);
            hir::HirBranch branch;
            branch.cond       = cond;
            branch.then_block = static_cast<hir::HirDeclId>(body_block);
            branch.else_block = static_cast<hir::HirDeclId>(next_block);
            setTerminator(hir_.addExpr(hir::HirExpr{std::move(branch)}));
        }

        // Body block
        setCurrentBlock(body_block);
        current_fn_->blocks[body_block].insts = memory::DynArray<hir::HirExprId>(hir_arena_);
        auto body_val                         = visitExpr(c.body);
        bool body_terminated = current_fn_->blocks[body_block].terminator != hir::kInvalidHirExpr;

        if (!body_terminated) {
            if (result_slot != hir::kInvalidHirSlot && body_val != hir::kInvalidHirExpr) {
                auto body_type = astExprType(c.body);
                if (body_type != types::kVoidType) {
                    hir::HirExprId stored = body_val;
                    if (is_optional) {
                        // Wrap T in ?T: store inner value, mark slot as "has value"
                        stored = body_val;
                    }
                    current_fn_->blocks[body_block].insts.push(emitSlotStore(result_slot, stored));
                }
            }
            emitJump(merge_block);
        }

        if (!is_default && !is_last) {
            setCurrentBlock(next_block);
            current_fn_->blocks[next_block].insts = memory::DynArray<hir::HirExprId>(hir_arena_);
        }
    }

    // Merge block
    setCurrentBlock(merge_block);
    current_fn_->blocks[merge_block].insts = memory::DynArray<hir::HirExprId>(hir_arena_);
    if (result_slot != hir::kInvalidHirSlot) {
        auto result = emitSlotLoad(result_slot, is_optional ? inner_t : result_type);
        current_fn_->blocks[merge_block].insts.push(result);
        return result;
    }

    return hir::kInvalidHirExpr;
}

hir::HirExprId HirLower::lowerWhenCondition(ast::ExprId cond, hir::HirSlotId subj_slot,
                                            types::TypeId subject_type) {
    auto &node = builder().getExpr(cond);

    // Range pattern: expand to comparisons against subject
    if (auto *range = std::get_if<ast::RangeNode>(&node)) {
        auto lhs_val = visitExpr(range->lhs);
        auto rhs_val = visitExpr(range->rhs);
        auto subj    = emitSlotLoad(subj_slot, subject_type);

        bool left_open  = (range->bounds == ast::RangeBounds::OpenLeft ||
                          range->bounds == ast::RangeBounds::Open);
        bool right_open = (range->bounds == ast::RangeBounds::OpenRight ||
                           range->bounds == ast::RangeBounds::Open);

        hir::HirBinary cmp_left;
        cmp_left.lhs  = subj;
        cmp_left.rhs  = lhs_val;
        cmp_left.op   = left_open ? hir::HirBinaryOp::Gt : hir::HirBinaryOp::Ge;
        cmp_left.type = types::kBoolType;
        auto left_id  = addHirExpr(hir::HirExpr{std::move(cmp_left)});

        hir::HirBinary cmp_right;
        cmp_right.lhs  = subj;
        cmp_right.rhs  = rhs_val;
        cmp_right.op   = right_open ? hir::HirBinaryOp::Lt : hir::HirBinaryOp::Le;
        cmp_right.type = types::kBoolType;
        auto right_id  = addHirExpr(hir::HirExpr{std::move(cmp_right)});

        hir::HirBinary and_bin;
        and_bin.lhs  = left_id;
        and_bin.rhs  = right_id;
        and_bin.op   = hir::HirBinaryOp::And;
        and_bin.type = types::kBoolType;
        return addHirExpr(hir::HirExpr{std::move(and_bin)});
    }

    // Regular boolean expression (literal, ident, binary, etc.)
    return visitExpr(cond);
}

hir::HirSlotId HirLower::allocSlot() {
    return slot_counter_++;
}

hir::HirExprId HirLower::emitSlotAlloca(hir::HirSlotId slot, types::TypeId type) {
    return addHirExpr(hir::HirSlotAlloca{slot, type});
}

hir::HirExprId HirLower::emitSlotStore(hir::HirSlotId slot, hir::HirExprId value) {
    return addHirExpr(hir::HirSlotStore{slot, value});
}

hir::HirExprId HirLower::emitSlotLoad(hir::HirSlotId slot, types::TypeId type) {
    return addHirExpr(hir::HirSlotLoad{slot, type});
}

size_t HirLower::newBlock() {
    auto idx = current_fn_->blocks.size();
    current_fn_->blocks.emplace(hir_arena_);
    return idx;
}

void HirLower::setCurrentBlock(size_t idx) {
    currentBlock_ = idx;
}

size_t HirLower::currentBlock() const {
    return currentBlock_;
}

void HirLower::setTerminator(hir::HirExprId term) {
    current_fn_->blocks[currentBlock_].terminator = term;
}

void HirLower::emitJump(size_t target) {
    hir::HirJump jump;
    jump.target = static_cast<hir::HirDeclId>(target);
    setTerminator(hir_.addExpr(hir::HirExpr{std::move(jump)}));
}

void HirLower::visitMarker(const ast::MarkerNode &n) {
    auto prevBlock   = currentBlock_;
    auto markerBlock = newBlock();
    auto postBlock   = newBlock();

    labelMap_.insert(n.name, markerBlock);

    hir::HirJump jump;
    jump.target                               = static_cast<hir::HirDeclId>(postBlock);
    current_fn_->blocks[prevBlock].terminator = hir_.addExpr(hir::HirExpr{std::move(jump)});

    setCurrentBlock(markerBlock);
    current_fn_->blocks[markerBlock].insts = memory::DynArray<hir::HirExprId>(hir_arena_);
    for (auto stmt_id : n.body)
        visitStmt(stmt_id);

    if (current_fn_->blocks[markerBlock].terminator == hir::kInvalidHirExpr) {
        hir::HirJump endJump;
        endJump.target = static_cast<hir::HirDeclId>(postBlock);
        current_fn_->blocks[markerBlock].terminator =
            hir_.addExpr(hir::HirExpr{std::move(endJump)});
    }

    setCurrentBlock(postBlock);
    current_fn_->blocks[postBlock].insts = memory::DynArray<hir::HirExprId>(hir_arena_);
}

void HirLower::visitGoto(const ast::GotoNode &n) {
    auto *targetIdx = labelMap_.get(n.target);
    if (!targetIdx) {
        ctx_.diags().report(Severity::Error, UndefinedIdent,
                            "undefined marker '" + std::string(n.target) + "'", n.span);
        return;
    }
    emitJump(*targetIdx);
    auto dead = newBlock();
    setCurrentBlock(dead);
    current_fn_->blocks[dead].insts = memory::DynArray<hir::HirExprId>(hir_arena_);
}

void HirLower::visitStmt(ast::StmtId id) {
    if (id == ast::kInvalidStmt)
        return;

    auto &node = builder().getStmt(id);
    std::visit(
        common::overloaded{
            [&](const ast::LetNode &n) {
                hir::HirExprId init     = hir::kInvalidHirExpr;
                types::TypeId init_type = types::kErrorType;
                if (n.init != ast::kInvalidExpr) {
                    init = visitExpr(n.init);
                    if (init != hir::kInvalidHirExpr)
                        init_type = astExprType(n.init);
                }

                types::TypeId decl_type = types::kErrorType;
                if (n.type_annot != ast::kInvalidTypeExpr) {
                    types::TypeLower lower(builder(), ctx_.types(), ctx_.diags(), ctx_.syms());
                    decl_type = lower.lower(n.type_annot);
                }

                types::TypeId var_type = init_type;
                if (n.type_annot != ast::kInvalidTypeExpr && decl_type != types::kErrorType)
                    var_type = decl_type;
                else if (init_type != types::kErrorType)
                    var_type = init_type;

                for (auto var_name : n.names) {
                    auto sym_id = ctx_.syms().declare(var_name, symbols::SymbolVisibility::Private,
                                                      0, symbols::SymKind::Variable,
                                                      ast::kInvalidDecl, memory::Span{});
                    if (sym_id != symbols::kInvalidSym) {
                        if (sym_id >= var_types_.size()) {
                            if (sym_id >= var_types_.capacity())
                                var_types_.reserve(sym_id + 1);
                            while (var_types_.size() <= sym_id)
                                var_types_.push(types::kErrorType);
                        }
                        var_types_[sym_id] = var_type;
                    }

                    hir::HirLet hir_let;
                    hir_let.name = ctx_.syms().interner().intern(var_name);
                    hir_let.type = var_type;
                    hir_let.init = init;

                    auto hir_id = addHirExpr(hir::HirExpr{hir_let});
                    if (current_fn_ && !current_fn_->blocks.empty()) {
                        auto &cur = current_fn_->blocks[currentBlock()];
                        cur.insts.push(hir_id);
                    }
                }
            },
            [&](const ast::AssignNode &n) {
                auto target = visitExpr(n.target);
                auto value  = visitExpr(n.value);
                hir::HirAssign assign;
                assign.target = target;
                assign.value  = value;
                auto hir_id   = addHirExpr(hir::HirExpr{assign});
                if (current_fn_ && !current_fn_->blocks.empty()) {
                    auto &cur = current_fn_->blocks[currentBlock()];
                    cur.insts.push(hir_id);
                }
            },
            [&](const ast::RetNode &n) {
                hir::HirExprId val = hir::kInvalidHirExpr;
                if (n.value != ast::kInvalidExpr)
                    val = visitExpr(n.value);

                hir::HirRet ret;
                ret.value   = val;
                auto hir_id = hir_.addExpr(hir::HirExpr{ret});
                if (current_fn_ && !current_fn_->blocks.empty())
                    setTerminator(hir_id);
            },
            [&](const ast::GotoNode &n) { visitGoto(n); },
            [&](const ast::MarkerNode &n) { visitMarker(n); },
            [&](const ast::ExprStmtNode &n) {
                auto result = visitExpr(n.expr);
                if (result != hir::kInvalidHirExpr && current_fn_ && !current_fn_->blocks.empty()) {
                    auto &cur = current_fn_->blocks[currentBlock()];
                    cur.insts.push(result);
                }
            },
            [](const ast::UseNode &) {},
            [](const ast::ErrorStmtNode &) {},
        },
        node);
}

hir::HirExprId HirLower::visitStructLiteral(ast::ExprId id, const ast::StructLiteralNode &n) {
    hir::HirStructLiteral lit(hir_arena_);
    lit.type = astExprType(id);
    for (const auto &field : n.fields) {
        if (field.value != ast::kInvalidExpr)
            lit.values.push(visitExpr(field.value));
    }
    return addHirExpr(std::move(lit));
}

hir::HirExprId HirLower::visitArrayLiteral(ast::ExprId id, const ast::ArrayLiteralNode &n) {
    hir::HirArrayLiteral lit(hir_arena_);
    lit.type = astExprType(id);
    for (auto elem : n.elements) {
        lit.elements.push(visitExpr(elem));
    }
    return addHirExpr(std::move(lit));
}

hir::HirExprId HirLower::visitEnumValue(ast::ExprId id, const ast::EnumValueNode &n) {
    auto type       = astExprType(id);
    const auto &def = ctx_.types().getEnumDef(type);
    int64_t val     = 0;
    for (const auto &v : def.variants) {
        if (ctx_.types().interner().lookup(v.name) == n.variant_name) {
            val = v.discriminant;
            break;
        }
    }
    hir::HirEnumValue lit;
    lit.type  = type;
    lit.value = val;
    return addHirExpr(std::move(lit));
}
} // namespace zith::sema
