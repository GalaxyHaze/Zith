#include "resolver.hpp"
#include "legacy-zith/ast/ast-node-utils.hpp"
#include "common/format.hpp"
#include "common/overloaded.hpp"
#include "diagnostics/error-codes.hpp"

namespace zith::symbols {

static std::string joinWith(const memory::DynArray<std::string_view> &path) {
    std::string result;
    for (size_t i = 0; i < path.size(); ++i) {
        if (i > 0)
            result += '.';
        result.append(path[i].data(), path[i].size());
    }
    return result;
}

Resolver::Resolver(SymbolTable &syms, ImportManager &import_mgr, ast::AstBuilder &builder,
                   diagnostics::DiagnosticEngine &diags)
    : syms_(syms), import_mgr_(import_mgr), builder_(builder), diags_(diags),
      resolved_(builder.arena()) {}

void Resolver::resolveProgram(ast::ProgramNode &program) {
    // First pass: process import declarations (validate & register aliases)
    for (auto decl_id : program.decls) {
        auto &decl = builder_.getDecl(decl_id);
        if (auto *import = ast::asImport(decl)) {
            auto import_key = joinWith(import->path);
            if (!import_mgr_.isLoaded(import_key))
                continue;
            if (!import->alias.empty()) {
                auto mod_sym = syms_.lookup(import_key);
                if (mod_sym != kInvalidSym)
                    syms_.declare(import->alias, SymbolVisibility::Public, 0, SymKind::Alias,
                                  ast::kInvalidDecl, {}, mod_sym);
            }
        }
    }

    // Second pass: resolve identifiers in all declaration bodies
    for (auto decl_id : program.decls) {
        auto &decl = builder_.getDecl(decl_id);
        if (!ast::asImport(decl))
            resolveDecl(decl_id);
    }
}

void Resolver::resolveImportMembers(ast::ProgramNode &program) {
    aliases_only_ = true;
    for (auto decl_id : program.decls) {
        auto &decl = builder_.getDecl(decl_id);
        if (!ast::asImport(decl))
            resolveDecl(decl_id);
    }
    aliases_only_ = false;
}

void Resolver::resolveDecl(ast::DeclId id) {
    auto &decl = builder_.getDecl(id);
    std::visit(common::overloaded{
                   [&](const ast::FnDeclNode &n) {
                       if (!aliases_only_) {
                           auto fn_scope = syms_.enterScope();
                           for (auto &p : n.params)
                               syms_.declareInScope(fn_scope, p.name);
                       }
                       if (n.body != ast::kInvalidExpr)
                           resolveExpr(n.body);
                       if (!aliases_only_)
                           syms_.exitScope();
                   },
                   [&](const ast::WordDeclNode &n) {
                       if (n.body != ast::kInvalidExpr)
                           resolveExpr(n.body);
                   },
                   [&](const ast::ContextDeclNode &n) {
                       for (auto d : n.decls)
                           resolveDecl(d);
                       if (n.body != ast::kInvalidExpr)
                           resolveExpr(n.body);
                   },
                   [](const auto &) {},
               },
               decl);
}

void Resolver::resolveStmt(ast::StmtId id) {
    auto &node = builder_.getStmt(id);
    std::visit(common::overloaded{
                   [&](const ast::LetNode &n) {
                       if (n.init != ast::kInvalidExpr)
                           resolveExpr(n.init);
                   },
                   [&](const ast::AssignNode &n) {
                       resolveExpr(n.target);
                       resolveExpr(n.value);
                   },
                   [&](const ast::RetNode &n) {
                       if (n.value != ast::kInvalidExpr)
                           resolveExpr(n.value);
                   },
                   [](const ast::GotoNode &) {},
                   [&](const ast::MarkerNode &n) {
                       for (auto stmt_id : n.body)
                           resolveStmt(stmt_id);
                   },
                   [&](const ast::ExprStmtNode &n) { resolveExpr(n.expr); },
                   [&](const ast::UseNode &n) {
                       if (n.block != ast::kInvalidExpr)
                           resolveExpr(n.block);
                   },
                   [](const ast::ErrorStmtNode &) {},
               },
               node);
}

void Resolver::resolveExpr(ast::ExprId id) {
    if (id == ast::kInvalidExpr)
        return;

    auto &node = builder_.getExpr(id);
    if (auto *field = std::get_if<ast::FieldNode>(&node)) {
        const auto &object = builder_.getExpr(field->object);
        if (const auto *ident = std::get_if<ast::IdentNode>(&object)) {
            auto qualified = std::string(ident->name) + "." + std::string(field->field);
            auto sym       = lookupUnqualified(qualified);
            if (sym != kInvalidSym) {
                auto name = syms_.interner().lookup(syms_.get(sym).name);
                node      = ast::IdentNode{name, field->span, false};
                setResolved(id, sym);
                return;
            }
        }
    }
    std::visit(common::overloaded{
                   [](const ast::LitValue &) {},
                   [](const ast::UnbodyNode &) {},
                   [&](const ast::IdentNode &n) {
                       if (aliases_only_)
                           return;
                       SymId sym =
                           n.scope_escape ? syms_.lookupEscaped(n.name) : lookupQualified(n.name);
                       if (sym == kInvalidSym) {
                           auto dot = n.name.find('.');
                           if (dot == std::string_view::npos) {
                               diags_.report(
                                   diagnostics::Severity::Error, diagnostics::err::UndefinedIdent,
                                   common::format("undefined identifier '%.*s'",
                                                  static_cast<int>(n.name.size()), n.name.data()),
                                   {});
                           }
                       }
                       setResolved(id, sym);
                   },
                   [&](const ast::BinaryNode &n) {
                       resolveExpr(n.lhs);
                       resolveExpr(n.rhs);
                   },
                   [&](const ast::UnaryNode &n) { resolveExpr(n.operand); },
                   [&](const ast::CallNode &n) {
                       resolveExpr(n.callee);
                       for (auto arg : n.args)
                           resolveExpr(arg);
                   },
                   [&](const ast::BlockNode &n) {
                       if (!aliases_only_)
                           syms_.enterScope();
                       for (auto stmt_id : n.stmts)
                           resolveStmt(stmt_id);
                       if (n.trailing != ast::kInvalidExpr)
                           resolveExpr(n.trailing);
                       if (!aliases_only_)
                           syms_.exitScope();
                   },
                   [&](const ast::IfNode &n) {
                       resolveExpr(n.cond);
                       resolveExpr(n.then_branch);
                       if (n.else_branch != ast::kInvalidExpr)
                           resolveExpr(n.else_branch);
                   },
                   [&](const ast::WhileNode &n) {
                       resolveExpr(n.cond);
                       resolveExpr(n.body);
                   },
                   [&](const ast::FieldNode &n) { resolveExpr(n.object); },
                   [&](const ast::IndexNode &n) {
                       resolveExpr(n.object);
                       resolveExpr(n.index);
                   },
                   [&](const ast::StructLiteralNode &n) {
                       for (const auto &field : n.fields) {
                           if (field.value != ast::kInvalidExpr)
                               resolveExpr(field.value);
                       }
                   },
                   [&](const ast::ArrayLiteralNode &n) {
                       for (auto elem : n.elements)
                           resolveExpr(elem);
                   },
                   [](const ast::EnumValueNode &) {},
                   [&](const ast::RangeNode &n) {
                       resolveExpr(n.lhs);
                       resolveExpr(n.rhs);
                   },
                   [&](const ast::WhenNode &n) {
                       resolveExpr(n.subject);
                       for (auto &c : n.cases) {
                           resolveExpr(c.condition);
                           resolveExpr(c.body);
                       }
                   },
                   [&](const ast::IntrinsicNode &n) {
                       switch (n.kind) {
                       case ast::IntrinsicKind::Fields:
                       case ast::IntrinsicKind::SizeOf:
                       case ast::IntrinsicKind::AlignOf:
                       case ast::IntrinsicKind::OffsetOf:
                           break;
                       default:
                           for (auto arg : n.args)
                               resolveExpr(arg);
                           break;
                       }
                   },
                   [&](const ast::MacroCallNode &n) {
                       for (auto arg : n.args)
                           resolveExpr(arg);
                   },
                   [&](const ast::SeqNode &n) {
                       for (auto op : n.operands)
                           resolveExpr(op);
                   },
                   [&](const ast::WordCallNode &n) {
                       for (auto arg : n.args)
                           resolveExpr(arg);
                   },
                   [](const ast::ErrorExprNode &) {},
               },
               node);
}

SymId Resolver::followAliases(SymId id) const {
    while (id != kInvalidSym) {
        auto &data = syms_.get(id);
        if (data.kind != SymKind::Alias || data.target == kInvalidSym)
            break;
        id = data.target;
    }
    return id;
}

SymId Resolver::lookupQualified(std::string_view name) {
    auto dot = name.find('.');
    if (dot == std::string_view::npos)
        return lookupUnqualified(name);

    auto first = name.substr(0, dot);
    auto rest  = name.substr(dot + 1);

    auto sym = lookupUnqualified(first);
    if (sym == kInvalidSym) {
        diags_.report(diagnostics::Severity::Error, diagnostics::err::UndefinedIdent,
                      common::format("undefined identifier '%.*s'", static_cast<int>(first.size()),
                                     first.data()),
                      {});
        return kInvalidSym;
    }

    sym = followAliases(sym);
    if (sym == kInvalidSym)
        return kInvalidSym;

    auto &data = syms_.get(sym);
    if (!isNamespaceLike(data.kind)) {
        diags_.report(diagnostics::Severity::Error, diagnostics::err::NotNamespace,
                      common::format("'%.*s' is not a namespace, so '%.*s' is invalid",
                                     static_cast<int>(first.size()), first.data(),
                                     static_cast<int>(name.size()), name.data()),
                      {});
        return kInvalidSym;
    }

    auto scope     = data.scope;
    auto remaining = rest;
    std::string prefix(first);
    for (;;) {
        auto next_dot = remaining.find('.');
        auto segment =
            next_dot == std::string_view::npos ? remaining : remaining.substr(0, next_dot);
        auto found = syms_.lookupInScope(segment, scope);
        if (found == kInvalidSym) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::NoMember,
                          common::format("no member '%.*s' in '%s'",
                                         static_cast<int>(segment.size()), segment.data(),
                                         prefix.c_str()),
                          {});
            return kInvalidSym;
        }
        found = followAliases(found);
        if (found == kInvalidSym)
            return kInvalidSym;
        if (next_dot == std::string_view::npos)
            return found;
        auto &next_data = syms_.get(found);
        if (!isNamespaceLike(next_data.kind)) {
            auto current = common::format("%s.%.*s", prefix.c_str(),
                                          static_cast<int>(segment.size()), segment.data());
            diags_.report(
                diagnostics::Severity::Error, diagnostics::err::NotNamespace,
                common::format("'%s' is not a namespace, so the remainder of '%.*s' cannot be "
                               "resolved",
                               current.c_str(), static_cast<int>(name.size()), name.data()),
                {});
            return kInvalidSym;
        }
        scope = next_data.scope;
        prefix += "." + std::string(segment);
        remaining = remaining.substr(next_dot + 1);
    }
}

SymId Resolver::lookupUnqualified(std::string_view name) {
    auto sym = syms_.lookup(name);
    return sym != kInvalidSym ? followAliases(sym) : kInvalidSym;
}

bool Resolver::isNamespaceLike(SymKind kind) const {
    return kind == SymKind::Module || kind == SymKind::Struct || kind == SymKind::Enum ||
           kind == SymKind::Trait || kind == SymKind::Interface || kind == SymKind::Union ||
           kind == SymKind::Component;
}

void Resolver::setResolved(ast::ExprId id, SymId sym) {
    if (id == ast::kInvalidExpr)
        return;
    if (id >= resolved_.size()) {
        if (id >= resolved_.capacity())
            resolved_.reserve(id + 1);
        while (resolved_.size() <= id)
            resolved_.push(kInvalidSym);
    }
    resolved_[id] = sym;
}

SymId Resolver::resolvedSym(ast::ExprId id) const {
    if (id >= resolved_.size())
        return kInvalidSym;
    return resolved_[id];
}

bool Resolver::hasResolvedSym(ast::ExprId id) const {
    return id < resolved_.size() && resolved_[id] != kInvalidSym;
}

memory::DynArray<SymId> Resolver::takeResolvedTable() {
    return std::move(resolved_);
}

} // namespace zith::symbols
