#include "frontend/macro-expand.hpp"
#include "diagnostics/error-codes.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace zith::frontend {

namespace {

/// Depth-first walk over the template body, flagging every node it reaches.
struct TemplateMarker {
    FrontendSnapshot &snapshot;
    std::vector<bool> &exprs;
    std::vector<bool> &stmts;

    TemplateMarker(FrontendSnapshot &snap, std::vector<bool> &e, std::vector<bool> &s)
        : snapshot(snap), exprs(e), stmts(s) {}

    void expr(ExprId id) {
        if (!id || id.value > snapshot.expressions().size())
            return;
        if (id.value >= exprs.size())
            exprs.resize(id.value + 1U, false);
        if (exprs[id.value])
            return;
        exprs[id.value] = true;
        const auto &e   = snapshot.expressions()[id.value - 1U];
        for (const auto op : e.operands)
            expr(op);
        for (const auto cond : e.conditions)
            expr(cond);
        for (const auto attr : e.attributes)
            expr(attr);
        for (const auto sid : e.statements)
            stmt(sid);
    }

    void stmt(StmtId id) {
        if (!id || id.value > snapshot.statements().size())
            return;
        if (id.value >= stmts.size())
            stmts.resize(id.value + 1U, false);
        if (stmts[id.value])
            return;
        stmts[id.value] = true;
        const auto &s   = snapshot.statements()[id.value - 1U];
        expr(s.expression);
        if (s.kind == StmtKind::Binding)
            expr(s.binding.initializer);
        for (const auto argument : s.arguments)
            expr(argument);
        // Local marker declarations are blocks whose jump/dock statements and
        // parameter expressions are still template nodes.
        if (s.kind == StmtKind::Marker)
            expr(s.expression);
        for (const auto &parameter : s.parameters)
            if (parameter.defaultValue)
                expr(parameter.defaultValue);
    }
};

} // namespace

void markMacroTemplates(FrontendSnapshot &snapshot) {
    TemplateMarker marker{snapshot, snapshot.macro_template_exprs_, snapshot.macro_template_stmts_};
    for (const auto &decl : snapshot.declarations()) {
        if (decl.kind != DeclKind::Macro)
            continue;
        marker.expr(decl.body);
        marker.expr(decl.initializer);
    }
}

void expandMacros(FrontendSnapshot &snapshot,
                  const std::vector<ImportedMacroRecord> &imported) {
    MacroExpander expander(snapshot);
    expander.run(imported);
}

MacroExpander::MacroExpander(FrontendSnapshot &snapshot) : snapshot_(snapshot) {}

ExprId MacroExpander::addExpression(Expression expr) {
    const auto index = static_cast<uint32_t>(snapshot_.expressions_.size());
    expr.id          = ExprId{index + 1U};
    snapshot_.expressions_.push_back(std::move(expr));
    return expr.id;
}

StmtId MacroExpander::addStatement(Statement stmt) {
    const auto index = static_cast<uint32_t>(snapshot_.statements_.size());
    stmt.id          = StmtId{index + 1U};
    snapshot_.statements_.push_back(std::move(stmt));
    return stmt.id;
}

ScopeId MacroExpander::addScope(Scope scope) {
    const auto index = static_cast<uint32_t>(snapshot_.scopes_.size());
    scope.id         = ScopeId{index + 1U};
    snapshot_.scopes_.push_back(std::move(scope));
    return scope.id;
}

TypeExprId MacroExpander::addTypeExpression(TypeExpression te) {
    const auto index = static_cast<uint32_t>(snapshot_.type_expressions_.size());
    te.id            = TypeExprId{index + 1U};
    snapshot_.type_expressions_.push_back(std::move(te));
    return te.id;
}

static void prependTrace(std::string &msg, const std::vector<std::string> &stack) {
    for (size_t i = stack.size(); i > 0; --i) {
        msg = "in expansion of macro '" + stack[i - 1U] + "': " + msg;
    }
}

void MacroExpander::run(const std::vector<ImportedMacroRecord> &imported) {
    std::vector<MacroInfo> macros;
    for (const auto &decl : snapshot_.declarations_) {
        if (decl.kind != DeclKind::Macro)
            continue;
        for (const auto &existing : macros) {
            if (existing.name == decl.name) {
                snapshot_.diagnostics_.push_back(
                    Diagnostic{decl.span, "duplicate macro '" + decl.name + "'", false,
                               diagnostics::err::MacroDuplicate});
                goto next_decl;
            }
        }
        {
            MacroInfo info;
            info.id            = decl.id;
            info.name          = decl.name;
            info.isRaw         = decl.isRawMacro;
            info.isTag         = decl.isTagMacro;
            info.hasAttributes = decl.hasAttributesParam;
            for (const auto &p : decl.parameters) {
                if (p.type) {
                    const auto ti = p.type.value - 1U;
                    info.paramMetaTypes.push_back(ti < snapshot_.type_expressions_.size()
                                                      ? snapshot_.type_expressions_[ti].name
                                                      : std::string("expr"));
                } else {
                    info.paramMetaTypes.push_back("expr");
                }
                info.paramNames.push_back(p.name);
            }
            info.body = decl.body;
            info.span = decl.span;
            info.source = &snapshot_;
            macros.push_back(std::move(info));
        }
    next_decl:;
    }

    for (const auto &record : imported) {
        bool duplicate = false;
        for (const auto &existing : macros) {
            if (existing.name == record.name) {
                duplicate = true;
                snapshot_.diagnostics_.push_back(
                    Diagnostic{record.aliasSpan.size() != 0U ? record.aliasSpan : record.span,
                               "duplicate macro '" + record.name + "'", false,
                               diagnostics::err::MacroDuplicate});
                break;
            }
        }
        if (duplicate)
            continue;
        MacroInfo info;
        info.name          = record.name;
        info.isRaw         = record.isRawMacro;
        info.isTag         = record.isTagMacro;
        info.hasAttributes = record.hasAttributesParam;
        info.span          = record.span;
        info.source        = record.source;
        for (const auto &parameter : record.parameters) {
            if (parameter.type && record.source &&
                parameter.type.value <= record.source->typeExpressions().size()) {
                info.paramMetaTypes.push_back(
                    record.source->typeExpressions()[parameter.type.value - 1U].name);
            } else {
                info.paramMetaTypes.push_back("expr");
            }
            info.paramNames.push_back(parameter.name);
        }
        info.body = record.body;
        macros.push_back(std::move(info));
    }

    std::vector<std::string> stack;
    for (const auto &decl : snapshot_.declarations_) {
        if (decl.kind == DeclKind::Macro)
            continue;
        if (decl.body)
            expandExpr(decl.body, macros, stack);
        if (decl.initializer)
            expandExpr(decl.initializer, macros, stack);
    }
}

void MacroExpander::expandExpr(ExprId id, const std::vector<MacroInfo> &macros,
                               std::vector<std::string> &stack) {
    if (!id || id.value > snapshot_.expressions_.size())
        return;
    auto &expr = snapshot_.expressions_[id.value - 1U];

    if (expr.kind == ExprKind::MacroCall && !expr.expansion) {
        // Resolve macro.
        const MacroInfo *macro = nullptr;
        for (const auto &m : macros) {
            if (m.name == expr.text) {
                macro = &m;
                break;
            }
        }
        if (!macro) {
            std::string msg = "unknown macro '" + expr.text + "'";
            prependTrace(msg, stack);
            snapshot_.diagnostics_.push_back(
                Diagnostic{expr.span, std::move(msg), false, diagnostics::err::MacroUnknown});
            return;
        }

        // Arity (ignore the special `attributes` param if present).
        {
            const size_t expectArgs = macro->paramNames.size();
            if (expr.operands.size() != expectArgs) {
                std::string msg = "macro '" + expr.text + "' expects " +
                                  std::to_string(expectArgs) + " arguments but " +
                                  std::to_string(expr.operands.size()) + " were provided";
                prependTrace(msg, stack);
                snapshot_.diagnostics_.push_back(
                    Diagnostic{expr.span, std::move(msg), false, diagnostics::err::MacroArity});
                return;
            }
        }

        // Recursion depth limit.
        if (stack.size() >= static_cast<size_t>(kMaxExpansionDepth)) {
            std::string msg = "macro recursion limit exceeded at '" + expr.text + "'";
            prependTrace(msg, stack);
            snapshot_.diagnostics_.push_back(
                Diagnostic{expr.span, std::move(msg), false, diagnostics::err::MacroRecursion});
            return;
        }

        // Direct recursion check.
        for (const auto &name : stack) {
            if (name == macro->name) {
                std::string msg = "in expansion of macro '" + macro->name +
                                  "': recursive macro expansion detected";
                prependTrace(msg, stack);
                snapshot_.diagnostics_.push_back(
                    Diagnostic{expr.span, std::move(msg), false, diagnostics::err::MacroRecursion});
                return;
            }
        }

        // Validate argument kinds.
        for (size_t i = 0; i < macro->paramNames.size(); ++i) {
            const auto &mt   = macro->paramMetaTypes[i];
            const auto argId = expr.operands[i];
            if (!argId || argId.value > snapshot_.expressions_.size())
                return;
            const auto &arg = snapshot_.expressions_[argId.value - 1U];
            if (mt == "identifier" && arg.kind != ExprKind::Name) {
                std::string msg = "expected an identifier for parameter '" + macro->paramNames[i] +
                                  "' of macro '" + macro->name + "'";
                prependTrace(msg, stack);
                snapshot_.diagnostics_.push_back(
                    Diagnostic{arg.span, std::move(msg), false, diagnostics::err::MacroArgKind});
                return;
            }
            if ((mt == "block" || mt == "body") && arg.kind != ExprKind::Block) {
                std::string msg = "expected a block for parameter '" + macro->paramNames[i] +
                                  "' of macro '" + macro->name + "'";
                prependTrace(msg, stack);
                snapshot_.diagnostics_.push_back(
                    Diagnostic{arg.span, std::move(msg), false, diagnostics::err::MacroArgKind});
                return;
            }
        }

        // Attributes require an `attributes` parameter on the declaration.
        if (!expr.attributes.empty() && !macro->hasAttributes) {
            std::string msg =
                "macro '" + macro->name + "' does not declare an 'attributes' parameter";
            prependTrace(msg, stack);
            snapshot_.diagnostics_.push_back(Diagnostic{expr.span, std::move(msg), false,
                                                        diagnostics::err::MacroAttrNotAllowed});
            return;
        }

        // A tag macro produces no value; it only expands as a statement.
        if (macro->isTag && contextIsValue(expr.id)) {
            std::string msg = "tag macro '" + expr.text + "' cannot be used as a value";
            prependTrace(msg, stack);
            snapshot_.diagnostics_.push_back(
                Diagnostic{expr.span, std::move(msg), false, diagnostics::err::MacroTagValue});
            return;
        }

        // raw macro cannot be used as a value expression.
        if (macro->isRaw && contextIsValue(expr.id)) {
            std::string msg = "raw macro '" + expr.text + "' cannot be used as a value";
            prependTrace(msg, stack);
            snapshot_.diagnostics_.push_back(
                Diagnostic{expr.span, std::move(msg), false, diagnostics::err::MacroRawValue});
            return;
        }

        // Collect hygiene names (raw macros skip hygiene).
        std::vector<std::string> hygieneNames;
        const FrontendSnapshot *source = macro->source != nullptr ? macro->source : &snapshot_;
        if (!macro->isRaw)
            collectHygieneNames(macro->body, hygieneNames, *source);

        std::unordered_map<std::string, std::string> hygieneMap;
        for (const auto &name : hygieneNames)
            hygieneMap[name] = hygieneName(name);

        // Clone the template body, substituting parameters.
        scopeMap_.clear();
        const auto attrNames       = expr.attributeNames;
        const auto attrExprs       = expr.attributes;
        callAttrNames_             = &attrNames;
        callAttrExprs_             = &attrExprs;
        callSpanForAttrs_          = expr.span;
        const size_t origExprCount = snapshot_.expressions_.size();
        const size_t origStmtCount = snapshot_.statements_.size();
        ExprMap exprMap(origExprCount + 1U);
        // Copy operands: expr is a reference into snapshot_.expressions_,
        // and addExpression push_back can reallocate and invalidate it.
        const std::vector<ExprId> args = expr.operands;
        ExprId expandedBody =
            cloneExpr(macro->body, args, expr.span, expr.scope, macro, exprMap, *source);

        // Apply hygiene renaming to template-derived nodes only.
        for (size_t ei = origExprCount; ei < snapshot_.expressions_.size(); ++ei) {
            if (isNonHygienicExpr(ei + 1U))
                continue;
            auto &e = snapshot_.expressions_[ei];
            if (e.kind == ExprKind::Name) {
                auto it = hygieneMap.find(e.text);
                if (it != hygieneMap.end())
                    e.text = it->second;
            }
        }
        for (size_t si = origStmtCount; si < snapshot_.statements_.size(); ++si) {
            if (isNonHygienicStmt(si + 1U))
                continue;
            auto &s = snapshot_.statements_[si];
            if (s.kind == StmtKind::Binding) {
                auto it = hygieneMap.find(s.binding.name);
                if (it != hygieneMap.end())
                    s.binding.name = it->second;
            }
        }

        // Wrap in Block (normal macros) or prepare splice (raw macros).
        Expression block;
        block.kind  = ExprKind::Block;
        block.span  = expr.span;
        block.scope = macro->isRaw ? expr.scope : addScope(Scope{{}, expr.scope, expr.span});

        if (expandedBody) {
            const auto &expanded = snapshot_.expressions_[expandedBody.value - 1U];
            if (expanded.kind == ExprKind::Block) {
                block.statements = expanded.statements;
                if (!macro->isRaw) {
                    auto &innerScope  = snapshot_.scopes_[expanded.scope.value - 1U];
                    innerScope.parent = expr.scope;
                    block.scope       = expanded.scope;
                }
            } else {
                Statement stmt;
                stmt.kind       = StmtKind::Expression;
                stmt.expression = expandedBody;
                stmt.span       = expr.span;
                block.statements.push_back(addStatement(std::move(stmt)));
            }
        }

        callAttrNames_ = nullptr;
        callAttrExprs_ = nullptr;

        ExprId result                                        = addExpression(std::move(block));
        snapshot_.expressions_[id.value - 1U].expansion      = result;
        snapshot_.expressions_[id.value - 1U].expansionIsRaw = macro->isRaw;

        // Walk expansion for nested macros.
        stack.push_back(macro->name);
        expandExpr(result, macros, stack);
        stack.pop_back();
        return;
    }

    // Walk child expressions.
    for (auto op : expr.operands)
        expandExpr(op, macros, stack);

    // Walk child statements by index so raw splicing can mutate the list.
    for (size_t si = 0; si < expr.statements.size(); ++si) {
        const auto sid = expr.statements[si];
        if (!sid || sid.value > snapshot_.statements_.size())
            continue;
        auto &stmt = snapshot_.statements_[sid.value - 1U];
        if (stmt.kind == StmtKind::Expression && stmt.expression) {
            const auto eid = stmt.expression;
            auto &es       = snapshot_.expressions_[eid.value - 1U];
            if (es.kind == ExprKind::MacroCall && es.expansion && es.expansionIsRaw) {
                // Splice the expansion statements directly into the parent block.
                const auto &expBlock = snapshot_.expressions_[es.expansion.value - 1U];
                expr.statements.erase(expr.statements.begin() + static_cast<ptrdiff_t>(si));
                ptrdiff_t pos = static_cast<ptrdiff_t>(si);
                for (auto s : expBlock.statements)
                    expr.statements.insert(expr.statements.begin() + (pos++), s);
                --si; // re-process at current position
                continue;
            }
        }
        if (stmt.expression)
            expandExpr(stmt.expression, macros, stack);
        if (stmt.kind == StmtKind::Binding && stmt.binding.initializer)
            expandExpr(stmt.binding.initializer, macros, stack);
        for (const auto argument : stmt.arguments)
            expandExpr(argument, macros, stack);
    }

    for (auto cond : expr.conditions)
        expandExpr(cond, macros, stack);
}

// Check whether this expression is used as a value (i.e. not in a standalone
// StmtKind::Expression statement).
bool MacroExpander::contextIsValue(ExprId eid) {
    // Walk all statements in all expressions to find the parent statement.
    for (const auto &ex : snapshot_.expressions_) {
        for (size_t si = 0; si < ex.statements.size(); ++si) {
            const auto sid = ex.statements[si];
            if (!sid || sid.value > snapshot_.statements_.size())
                continue;
            const auto &stmt = snapshot_.statements_[sid.value - 1U];
            if (stmt.kind == StmtKind::Expression && stmt.expression == eid) {
                // This expression IS the statement itself — not a value.
                return false;
            }
        }
    }
    // Not found as a standalone statement — it's in a value position (e.g.,
    // `let x = @macro();` or `@macro() + 1`).
    return true;
}

ExprId MacroExpander::cloneExpr(ExprId src, const std::vector<ExprId> &args, TextSpan callSpan,
                                ScopeId callScope, const MacroInfo *macro, ExprMap &map,
                                const FrontendSnapshot &source) {
    if (!src || src.value > source.expressions().size())
        return {};
    if (src.value < map.size() && map[src.value])
        return map[src.value];

    const auto &orig = source.expressions()[src.value - 1U];

    if (orig.kind == ExprKind::Field && macro && macro->hasAttributes) {
        ExprId attrClone;
        if (substituteAttribute(src, macro, &attrClone, source)) {
            if (src.value >= map.size())
                map.resize(src.value + 1U);
            map[src.value] = attrClone;
            return attrClone;
        }
    }

    if (orig.kind == ExprKind::Name && macro) {
        for (size_t i = 0; i < macro->paramNames.size(); ++i) {
            if (orig.text == macro->paramNames[i]) {
                ExprMap dummy(snapshot_.expressions_.size() + 1U);
                // Everything cloned from a call-site argument belongs to the
                // caller, so hygiene must leave those names alone.
                const size_t firstExpr = snapshot_.expressions_.size();
                const size_t firstStmt = snapshot_.statements_.size();
                ExprId result =
                    cloneExpr(args[i], {}, callSpan, callScope, nullptr, dummy, snapshot_);
                markNonHygienic(firstExpr, firstStmt);
                if (src.value >= map.size())
                    map.resize(src.value + 1U);
                map[src.value] = result;
                return result;
            }
        }
    }

    Expression copy     = orig;
    copy.id             = {};
    copy.span           = callSpan;
    copy.expansion      = {};
    copy.expansionIsRaw = false;

    // Resolve this node's scope *before* recursing: children look their parent
    // scope up through scopeMap_, so it has to be registered first.
    if (macro == nullptr) {
        // Cloned from a call-site argument: it already lives in the caller's
        // scope chain, so keep the scope verbatim.
        copy.scope = orig.scope;
    } else if (orig.kind == ExprKind::Block && orig.scope) {
        Scope newScope;
        // Nest under the cloned parent when there is one, else under the call
        // site: a template scope id means nothing in the caller's chain.
        newScope.parent = mappedScope(source.scopes()[orig.scope.value - 1U].parent, callScope);
        newScope.span   = callSpan;
        copy.scope      = addScope(std::move(newScope));
        scopeMap_[orig.scope.value] = copy.scope;
    } else if (orig.scope) {
        copy.scope = mappedScope(orig.scope, callScope);
    }

    for (size_t i = 0; i < copy.operands.size(); ++i)
        copy.operands[i] =
            cloneExpr(copy.operands[i], args, callSpan, callScope, macro, map, source);

    for (size_t i = 0; i < copy.statements.size(); ++i)
        copy.statements[i] =
            cloneStmt(copy.statements[i], args, callSpan, callScope, macro, map, source);

    for (size_t i = 0; i < copy.conditions.size(); ++i)
        copy.conditions[i] =
            cloneExpr(copy.conditions[i], args, callSpan, callScope, macro, map, source);

    if (copy.cast_type)
        copy.cast_type = cloneTypeExpr(copy.cast_type, source);

    for (size_t i = 0; i < copy.genericArgs.size(); ++i)
        copy.genericArgs[i] = cloneTypeExpr(copy.genericArgs[i], source);

    ExprId result = addExpression(std::move(copy));
    if (src.value >= map.size())
        map.resize(src.value + 1U);
    map[src.value] = result;
    return result;
}

StmtId MacroExpander::cloneStmt(StmtId src, const std::vector<ExprId> &args, TextSpan callSpan,
                                ScopeId, const MacroInfo *macro, ExprMap &map,
                                const FrontendSnapshot &source) {
    if (!src || src.value > source.statements().size())
        return {};

    const auto &orig = source.statements()[src.value - 1U];
    Statement copy   = orig;
    copy.id          = {};
    copy.span        = callSpan;

    if (copy.expression)
        copy.expression = cloneExpr(
            copy.expression, args, callSpan,
            orig.expression ? source.expressions()[orig.expression.value - 1U].scope : ScopeId{},
            macro, map, source);

    if (copy.kind == StmtKind::Binding) {
        if (copy.binding.initializer)
            copy.binding.initializer =
                cloneExpr(copy.binding.initializer, args, callSpan,
                              copy.binding.initializer
                              ? source.expressions()[copy.binding.initializer.value - 1U].scope
                              : ScopeId{},
                          macro, map, source);
        if (copy.binding.type)
            copy.binding.type = cloneTypeExpr(copy.binding.type, source);
    }

    for (size_t i = 0; i < copy.arguments.size(); ++i)
        copy.arguments[i] = cloneExpr(copy.arguments[i], args, callSpan,
                                      copy.arguments[i]
                                          ? source.expressions()[copy.arguments[i].value - 1U].scope
                                          : ScopeId{},
                                      macro, map, source);

    return addStatement(std::move(copy));
}

TypeExprId MacroExpander::cloneTypeExpr(TypeExprId src, const FrontendSnapshot &source) {
    if (!src || src.value > source.typeExpressions().size())
        return {};

    TypeExpression copy = source.typeExpressions()[src.value - 1U];
    copy.id             = {};
    copy.span           = {};

    for (size_t i = 0; i < copy.arguments.size(); ++i)
        copy.arguments[i] = cloneTypeExpr(copy.arguments[i], source);

    return addTypeExpression(std::move(copy));
}

void MacroExpander::collectHygieneNames(ExprId body, std::vector<std::string> &names,
                                        const FrontendSnapshot &source) {
    if (!body || body.value > source.expressions().size())
        return;
    const auto &expr = source.expressions()[body.value - 1U];

    for (auto sid : expr.statements) {
        if (!sid || sid.value > source.statements().size())
            continue;
        const auto &stmt = source.statements()[sid.value - 1U];
        if (stmt.kind == StmtKind::Binding)
            names.push_back(stmt.binding.name);
        for (const auto argument : stmt.arguments)
            collectHygieneNames(argument, names, source);
    }
    for (auto op : expr.operands)
        collectHygieneNames(op, names, source);
}

ExprId MacroExpander::substituteAttribute(ExprId src, const MacroInfo *macro, ExprId *outClone,
                                          const FrontendSnapshot &source) {
    const auto &field = source.expressions()[src.value - 1U];
    if (field.operands.empty())
        return {};
    const auto baseId = field.operands[0];
    if (!baseId || baseId.value > source.expressions().size())
        return {};
    const auto &base = source.expressions()[baseId.value - 1U];
    if (base.kind != ExprKind::Name || base.text != "attributes")
        return {};

    const auto *names = callAttrNames_;
    const auto *exprs = callAttrExprs_;
    if (names != nullptr && exprs != nullptr) {
        for (size_t i = 0; i < names->size() && i < exprs->size(); ++i) {
            if ((*names)[i] != field.text)
                continue;
            // Clone the call-site expression: it belongs to the caller, so it
            // is exempt from hygiene renaming.
            ExprMap dummy(snapshot_.expressions_.size() + 1U);
            const size_t firstExpr = snapshot_.expressions_.size();
            const size_t firstStmt = snapshot_.statements_.size();
            *outClone =
                cloneExpr((*exprs)[i], {}, callSpanForAttrs_, base.scope, nullptr, dummy, snapshot_);
            markNonHygienic(firstExpr, firstStmt);
            return *outClone;
        }
    }

    snapshot_.diagnostics_.push_back(Diagnostic{callSpanForAttrs_,
                                                "macro '" + macro->name + "' has no attribute '" +
                                                    field.text + "' at this call site",
                                                false, diagnostics::err::MacroAttrUnknown});
    // Substitute an error node so expansion continues without a phantom name.
    Expression error_expr;
    error_expr.kind  = ExprKind::Error;
    error_expr.span  = callSpanForAttrs_;
    error_expr.scope = base.scope;
    *outClone        = addExpression(std::move(error_expr));
    return *outClone;
}

ScopeId MacroExpander::mappedScope(ScopeId templateScope, ScopeId callScope) const {
    if (!templateScope)
        return callScope;
    const auto found = scopeMap_.find(templateScope.value);
    return found == scopeMap_.end() ? callScope : found->second;
}

void MacroExpander::markNonHygienic(size_t firstExprIndex, size_t firstStmtIndex) {
    if (snapshot_.expressions_.size() >= nonHygienicExprs_.size())
        nonHygienicExprs_.resize(snapshot_.expressions_.size() + 1U, false);
    for (size_t i = firstExprIndex; i < snapshot_.expressions_.size(); ++i)
        nonHygienicExprs_[i + 1U] = true;
    if (snapshot_.statements_.size() >= nonHygienicStmts_.size())
        nonHygienicStmts_.resize(snapshot_.statements_.size() + 1U, false);
    for (size_t i = firstStmtIndex; i < snapshot_.statements_.size(); ++i)
        nonHygienicStmts_[i + 1U] = true;
}

bool MacroExpander::isNonHygienicExpr(size_t idValue) const {
    return idValue < nonHygienicExprs_.size() && nonHygienicExprs_[idValue];
}

bool MacroExpander::isNonHygienicStmt(size_t idValue) const {
    return idValue < nonHygienicStmts_.size() && nonHygienicStmts_[idValue];
}

std::string MacroExpander::hygieneName(const std::string &original) {
    return original + "$m" + std::to_string(hygieneCounter_++);
}

} // namespace zith::frontend
