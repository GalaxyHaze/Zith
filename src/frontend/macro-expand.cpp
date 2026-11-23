#include "frontend/macro-expand.hpp"
#include "diagnostics/error-codes.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace zith::frontend {

namespace {

constexpr size_t kNoVariadicParam = static_cast<size_t>(-1);

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

void expandMacros(FrontendSnapshot &snapshot, const std::vector<ImportedMacroRecord> &imported) {
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

LocalId MacroExpander::cloneLocal(const LocalId src, LocalMap &map) {
    if (!src)
        return {};
    if (src.value >= map.size())
        map.resize(src.value + 1U);
    if (!map[src.value])
        map[src.value] = LocalId{nextLocalId_++};
    return map[src.value];
}

static void prependTrace(std::string &msg, const std::vector<std::string> &stack) {
    for (size_t i = stack.size(); i > 0; --i) {
        msg = "in expansion of macro '" + stack[i - 1U] + "': " + msg;
    }
}

void MacroExpander::run(const std::vector<ImportedMacroRecord> &imported) {
    nextLocalId_            = 1U;
    const auto observeLocal = [this](const LocalId id) {
        if (id && id.value >= nextLocalId_)
            nextLocalId_ = id.value + 1U;
    };
    for (const auto &statement : snapshot_.statements_)
        observeLocal(statement.binding.id);
    for (const auto &expression : snapshot_.expressions_)
        observeLocal(expression.forInBinding);
    for (const auto &decl : snapshot_.declarations_) {
        for (const auto &parameter : decl.parameters)
            observeLocal(parameter.id);
    }
    for (const auto &statement : snapshot_.statements_) {
        if (statement.isMacroRepetition && !snapshot_.isMacroTemplateStmt(statement.id)) {
            snapshot_.diagnostics_.push_back(
                Diagnostic{statement.span, "statement repetition is only valid in a macro template",
                           false, diagnostics::err::MacroArgKind});
        }
    }

    std::vector<MacroInfo> macros;
    for (const auto &decl : snapshot_.declarations_) {
        if (decl.kind != DeclKind::Macro)
            continue;
        bool duplicate = false;
        for (const auto &existing : macros) {
            if (existing.name == decl.name) {
                snapshot_.diagnostics_.push_back(
                    Diagnostic{decl.span, "duplicate macro '" + decl.name + "'", false,
                               diagnostics::err::MacroDuplicate});
                duplicate = true;
                break;
            }
        }
        if (duplicate)
            continue;

        MacroInfo info;
        info.id                       = decl.id;
        info.name                     = decl.name;
        info.isRaw                    = decl.isRawMacro;
        info.isTag                    = decl.isTagMacro;
        info.hasAttributes            = decl.hasAttributesParam;
        bool validVariadicDeclaration = true;
        for (size_t i = 0; i < decl.parameters.size(); ++i) {
            const auto &parameter = decl.parameters[i];
            if (parameter.isVariadicMacro) {
                if (info.variadicParamIndex != kNoVariadicParam ||
                    i + 1U != decl.parameters.size()) {
                    validVariadicDeclaration = false;
                } else {
                    info.variadicParamIndex = i;
                }
            }
            if (parameter.type) {
                const auto ti = parameter.type.value - 1U;
                info.paramMetaTypes.push_back(ti < snapshot_.type_expressions_.size()
                                                  ? snapshot_.type_expressions_[ti].name
                                                  : std::string("expr"));
            } else {
                info.paramMetaTypes.push_back("expr");
            }
            info.paramNames.push_back(parameter.name);
        }
        if (!validVariadicDeclaration)
            continue;
        info.body   = decl.body;
        info.span   = decl.span;
        info.source = &snapshot_;
        macros.push_back(std::move(info));
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
        info.name                     = record.name;
        info.isRaw                    = record.isRawMacro;
        info.isTag                    = record.isTagMacro;
        info.hasAttributes            = record.hasAttributesParam;
        info.span                     = record.span;
        info.source                   = record.source;
        bool validVariadicDeclaration = true;
        for (size_t i = 0; i < record.parameters.size(); ++i) {
            const auto &parameter = record.parameters[i];
            if (parameter.isVariadicMacro) {
                if (info.variadicParamIndex != kNoVariadicParam ||
                    i + 1U != record.parameters.size()) {
                    validVariadicDeclaration = false;
                } else {
                    info.variadicParamIndex = i;
                }
            }
            if (parameter.type && record.source &&
                parameter.type.value <= record.source->typeExpressions().size()) {
                info.paramMetaTypes.push_back(
                    record.source->typeExpressions()[parameter.type.value - 1U].name);
            } else {
                info.paramMetaTypes.push_back("expr");
            }
            info.paramNames.push_back(parameter.name);
        }
        if (!validVariadicDeclaration)
            continue;
        info.body = record.body;
        macros.push_back(std::move(info));
    }

    for (const auto &macro : macros) {
        if (!macro.source || !macro.body)
            continue;
        std::vector<std::pair<ExprId, bool>> pendingExprs{{macro.body, false}};
        std::vector<std::pair<StmtId, bool>> pendingStmts;
        while (!pendingExprs.empty() || !pendingStmts.empty()) {
            if (!pendingStmts.empty()) {
                const auto [id, repeated] = pendingStmts.back();
                pendingStmts.pop_back();
                if (!id || id.value > macro.source->statements().size())
                    continue;
                const auto &statement          = macro.source->statements()[id.value - 1U];
                const bool inRepeatedStatement = repeated || statement.isMacroRepetition;
                if (statement.isMacroRepetition && macro.variadicParamIndex == kNoVariadicParam) {
                    snapshot_.diagnostics_.push_back(Diagnostic{
                        statement.span, "statement repetition requires a variadic macro parameter",
                        false, diagnostics::err::MacroArgKind});
                }
                if (statement.expression)
                    pendingExprs.emplace_back(statement.expression, inRepeatedStatement);
                if (statement.kind == StmtKind::Binding && statement.binding.initializer)
                    pendingExprs.emplace_back(statement.binding.initializer, inRepeatedStatement);
                for (const auto argument : statement.arguments)
                    pendingExprs.emplace_back(argument, inRepeatedStatement);
                continue;
            }

            const auto [id, repeated] = pendingExprs.back();
            pendingExprs.pop_back();
            if (!id || id.value > macro.source->expressions().size())
                continue;
            const auto &expression = macro.source->expressions()[id.value - 1U];
            if (macro.variadicParamIndex != kNoVariadicParam &&
                macro.variadicParamIndex < macro.paramNames.size() &&
                expression.kind == ExprKind::Name &&
                expression.text == macro.paramNames[macro.variadicParamIndex] && !repeated) {
                snapshot_.diagnostics_.push_back(Diagnostic{
                    expression.span,
                    "variadic macro parameter '" + macro.paramNames[macro.variadicParamIndex] +
                        "' may only be used in a repeated statement",
                    false, diagnostics::err::MacroArgKind});
            }
            for (const auto operand : expression.operands)
                pendingExprs.emplace_back(operand, repeated);
            for (const auto condition : expression.conditions)
                pendingExprs.emplace_back(condition, repeated);
            for (const auto attribute : expression.attributes)
                pendingExprs.emplace_back(attribute, repeated);
            for (const auto statement : expression.statements)
                pendingStmts.emplace_back(statement, repeated);
        }
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
    const size_t exprIndex = id.value - 1U;
    auto &expr             = snapshot_.expressions_[exprIndex];

    if (expr.kind == ExprKind::MacroCall && !expr.expansion) {
        const Expression call = expr;
        // Resolve macro.
        const MacroInfo *macro = nullptr;
        for (const auto &m : macros) {
            if (m.name == call.text) {
                macro = &m;
                break;
            }
        }
        if (!macro) {
            std::string msg = "unknown macro '" + call.text + "'";
            prependTrace(msg, stack);
            snapshot_.diagnostics_.push_back(
                Diagnostic{call.span, std::move(msg), false, diagnostics::err::MacroUnknown});
            return;
        }

        // Arity (ignore the special `attributes` param if present).
        {
            const size_t fixedArgs = macro->variadicParamIndex == kNoVariadicParam
                                         ? macro->paramNames.size()
                                         : macro->variadicParamIndex;
            const bool wrongArity =
                macro->variadicParamIndex == kNoVariadicParam
                    ? call.operands.size() != fixedArgs
                    : call.operands.size() < fixedArgs;
            if (wrongArity) {
                std::string msg = "macro '" + call.text +
                                  (macro->variadicParamIndex == kNoVariadicParam
                                       ? "' expects "
                                       : "' expects at least ") +
                                  std::to_string(fixedArgs) + " arguments but " +
                                  std::to_string(call.operands.size()) + " were provided";
                prependTrace(msg, stack);
                snapshot_.diagnostics_.push_back(
                    Diagnostic{call.span, std::move(msg), false, diagnostics::err::MacroArity});
                return;
            }
        }

        // Recursion depth limit.
        if (stack.size() >= static_cast<size_t>(kMaxExpansionDepth)) {
            std::string msg = "macro recursion limit exceeded at '" + call.text + "'";
            prependTrace(msg, stack);
            snapshot_.diagnostics_.push_back(
                Diagnostic{call.span, std::move(msg), false, diagnostics::err::MacroRecursion});
            return;
        }

        // Direct recursion check.
        for (const auto &name : stack) {
            if (name == macro->name) {
                std::string msg = "in expansion of macro '" + macro->name +
                                  "': recursive macro expansion detected";
                prependTrace(msg, stack);
                snapshot_.diagnostics_.push_back(
                    Diagnostic{call.span, std::move(msg), false, diagnostics::err::MacroRecursion});
                return;
            }
        }

        // Validate argument kinds.
        for (size_t i = 0; i < call.operands.size(); ++i) {
            const size_t parameterIndex =
                macro->variadicParamIndex != kNoVariadicParam && i >= macro->variadicParamIndex
                    ? macro->variadicParamIndex
                    : i;
            const auto &mt   = macro->paramMetaTypes[parameterIndex];
            const auto argId = call.operands[i];
            if (!argId || argId.value > snapshot_.expressions_.size())
                return;
            const auto &arg          = snapshot_.expressions_[argId.value - 1U];
            const bool isUnevaluated = i < call.argIsUnevaluated.size() && call.argIsUnevaluated[i];
            std::string expected;
            if (mt == "identifier" &&
                (arg.kind != ExprKind::Name || arg.isRawName || isUnevaluated)) {
                expected = "an identifier";
            } else if (mt == "val" && isUnevaluated) {
                expected = "a value argument without '='";
            } else if (mt == "ast" && !isUnevaluated) {
                expected = "an AST argument written as '=expr'";
            } else if ((mt == "block" || mt == "body") && arg.kind != ExprKind::Block) {
                expected = "a block";
            }
            if (!expected.empty()) {
                std::string msg = "expected " + expected + " for parameter '" +
                                  macro->paramNames[parameterIndex] + "' of macro '" + macro->name +
                                  "'";
                prependTrace(msg, stack);
                snapshot_.diagnostics_.push_back(
                    Diagnostic{arg.span, std::move(msg), false, diagnostics::err::MacroArgKind});
                return;
            }
        }

        // Attributes require an `attributes` parameter on the declaration.
        if (!call.attributes.empty() && !macro->hasAttributes) {
            std::string msg =
                "macro '" + macro->name + "' does not declare an 'attributes' parameter";
            prependTrace(msg, stack);
            snapshot_.diagnostics_.push_back(Diagnostic{call.span, std::move(msg), false,
                                                        diagnostics::err::MacroAttrNotAllowed});
            return;
        }

        // A tag macro produces no value; it only expands as a statement.
        if (macro->isTag && contextIsValue(call.id)) {
            std::string msg = "tag macro '" + call.text + "' cannot be used as a value";
            prependTrace(msg, stack);
            snapshot_.diagnostics_.push_back(
                Diagnostic{call.span, std::move(msg), false, diagnostics::err::MacroTagValue});
            return;
        }

        // raw macro cannot be used as a value expression.
        if (macro->isRaw && contextIsValue(call.id)) {
            std::string msg = "raw macro '" + call.text + "' cannot be used as a value";
            prependTrace(msg, stack);
            snapshot_.diagnostics_.push_back(
                Diagnostic{call.span, std::move(msg), false, diagnostics::err::MacroRawValue});
            return;
        }

        // Collect hygiene names (raw macros skip hygiene).
        std::vector<std::string> hygieneNames;
        const FrontendSnapshot *source = macro->source != nullptr ? macro->source : &snapshot_;
        if (!macro->isRaw)
            collectHygieneNames(macro->body, hygieneNames, *source);

        memory::FlatMap<std::string, std::string> hygieneMap;
        for (const auto &name : hygieneNames)
            hygieneMap[name] = hygieneName(name);

        // Clone the template body, substituting parameters.
        scopeMap_.clear();
        const auto attrNames       = call.attributeNames;
        const auto attrExprs       = call.attributes;
        callAttrNames_             = &attrNames;
        callAttrExprs_             = &attrExprs;
        callSpanForAttrs_          = call.span;
        const size_t origExprCount = snapshot_.expressions_.size();
        const size_t origStmtCount = snapshot_.statements_.size();
        ExprMap exprMap(source->expressions().size() + 1U);
        StmtMap stmtMap(source->statements().size() + 1U);
        LocalMap localMap;
        MacroInfo invocation = *macro;
        std::vector<ExprId> args(macro->paramNames.size());
        const size_t fixedArgs = macro->variadicParamIndex == kNoVariadicParam
                                     ? macro->paramNames.size()
                                     : macro->variadicParamIndex;
        for (size_t i = 0; i < fixedArgs; ++i)
            args[i] = call.operands[i];
        if (macro->variadicParamIndex != kNoVariadicParam) {
            invocation.variadicArgs.assign(
                call.operands.begin() + static_cast<ptrdiff_t>(fixedArgs), call.operands.end());
        } else {
            args = call.operands;
        }
        macro               = &invocation;
        ExprId expandedBody = cloneExpr(macro->body, args, call.span, call.scope, macro, exprMap,
                                        stmtMap, localMap, *source);

        // Apply hygiene renaming to template-derived nodes only.
        for (size_t ei = origExprCount; ei < snapshot_.expressions_.size(); ++ei) {
            if (isNonHygienicExpr(ei + 1U))
                continue;
            auto &e = snapshot_.expressions_[ei];
            if (e.kind == ExprKind::Name) {
                if (const auto *renamed = hygieneMap.get(e.text))
                    e.text = *renamed;
            }
        }
        for (size_t si = origStmtCount; si < snapshot_.statements_.size(); ++si) {
            if (isNonHygienicStmt(si + 1U))
                continue;
            auto &s = snapshot_.statements_[si];
            if (s.kind == StmtKind::Binding) {
                if (const auto *renamed = hygieneMap.get(s.binding.name))
                    s.binding.name = *renamed;
            }
        }

        // Wrap in Block (normal macros) or prepare splice (raw macros).
        Expression block;
        block.kind  = ExprKind::Block;
        block.span  = call.span;
        block.scope = macro->isRaw ? call.scope : addScope(Scope{{}, call.scope, call.span});
        ExprId result;

        if (expandedBody) {
            const auto &expanded = snapshot_.expressions_[expandedBody.value - 1U];
            if (expanded.kind == ExprKind::Block && !macro->isRaw) {
                result = expandedBody;
                if (expanded.scope) {
                    auto &innerScope  = snapshot_.scopes_[expanded.scope.value - 1U];
                    innerScope.parent = call.scope;
                }
            } else if (expanded.kind == ExprKind::Block) {
                block.statements = expanded.statements;
            } else {
                Statement stmt;
                stmt.kind       = StmtKind::Expression;
                stmt.expression = expandedBody;
                stmt.span       = call.span;
                block.statements.push_back(addStatement(std::move(stmt)));
            }
        }

        callAttrNames_ = nullptr;
        callAttrExprs_ = nullptr;

        if (!result)
            result = addExpression(std::move(block));
        snapshot_.expressions_[exprIndex].expansion      = result;
        snapshot_.expressions_[exprIndex].expansionIsRaw = macro->isRaw;

        // Walk expansion for nested macros.
        stack.push_back(macro->name);
        expandExpr(result, macros, stack);
        stack.pop_back();
        return;
    }

    // Walk child expressions.
    const auto operands   = expr.operands;
    const auto conditions = expr.conditions;
    for (auto op : operands)
        expandExpr(op, macros, stack);

    // Reacquire the parent expression by index because recursive expansion can
    // append expressions and reallocate the snapshot vectors.
    for (size_t si = 0; si < snapshot_.expressions_[exprIndex].statements.size(); ++si) {
        const auto sid = snapshot_.expressions_[exprIndex].statements[si];
        if (!sid || sid.value > snapshot_.statements_.size())
            continue;
        const Statement stmt = snapshot_.statements_[sid.value - 1U];
        if (stmt.kind == StmtKind::Expression && stmt.expression) {
            expandExpr(stmt.expression, macros, stack);

            const auto eid = stmt.expression;
            const auto es  = snapshot_.expressions_[eid.value - 1U];
            if (es.kind == ExprKind::MacroCall && es.expansion && es.expansionIsRaw) {
                // Splice the expansion statements directly into the parent block.
                const auto expandedStatements =
                    snapshot_.expressions_[es.expansion.value - 1U].statements;
                auto &statements = snapshot_.expressions_[exprIndex].statements;
                statements.erase(statements.begin() + static_cast<ptrdiff_t>(si));
                ptrdiff_t pos = static_cast<ptrdiff_t>(si);
                for (auto s : expandedStatements)
                    statements.insert(statements.begin() + (pos++), s);
                --si; // re-process at current position
                continue;
            }
        }
        if (stmt.kind == StmtKind::Binding && stmt.binding.initializer)
            expandExpr(stmt.binding.initializer, macros, stack);
        for (const auto argument : stmt.arguments)
            expandExpr(argument, macros, stack);
    }

    for (auto cond : conditions)
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
                                StmtMap &stmtMap, LocalMap &localMap,
                                const FrontendSnapshot &source) {
    if (!src || src.value > source.expressions().size())
        return {};
    if (src.value < map.size() && map[src.value])
        return map[src.value];

    const auto orig = source.expressions()[src.value - 1U];

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
                StmtMap argStmtMap(snapshot_.statements_.size() + 1U);
                LocalMap argLocalMap;
                // Everything cloned from a call-site argument belongs to the
                // caller, so hygiene must leave those names alone.
                const size_t firstExpr = snapshot_.expressions_.size();
                const size_t firstStmt = snapshot_.statements_.size();
                ExprId result          = cloneExpr(args[i], {}, callSpan, callScope, nullptr, dummy,
                                                   argStmtMap, argLocalMap, snapshot_);
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
        copy.operands[i] = cloneExpr(copy.operands[i], args, callSpan, callScope, macro, map,
                                     stmtMap, localMap, source);

    const auto templateStatements = copy.statements;
    copy.statements.clear();
    for (const StmtId statementId : templateStatements) {
        if (!statementId || statementId.value > source.statements().size())
            continue;
        const auto &templateStatement = source.statements()[statementId.value - 1U];
        if (macro != nullptr && templateStatement.isMacroRepetition) {
            if (macro->variadicParamIndex == kNoVariadicParam ||
                macro->variadicParamIndex >= args.size()) {
                continue;
            }
            const auto scopeMapBase = scopeMap_;
            for (const ExprId repeatedArg : macro->variadicArgs) {
                auto repeatedArgs                       = args;
                repeatedArgs[macro->variadicParamIndex] = repeatedArg;
                ExprMap repeatedExprMap(source.expressions().size() + 1U);
                StmtMap repeatedStmtMap(source.statements().size() + 1U);
                LocalMap repeatedLocalMap;
                scopeMap_ = scopeMapBase;
                const StmtId repeatedStatement =
                    cloneStmt(statementId, repeatedArgs, callSpan, callScope, macro,
                              repeatedExprMap, repeatedStmtMap, repeatedLocalMap, source);
                if (repeatedStatement)
                    copy.statements.push_back(repeatedStatement);
            }
            scopeMap_ = scopeMapBase;
            continue;
        }
        const StmtId cloned = cloneStmt(statementId, args, callSpan, callScope, macro, map, stmtMap,
                                        localMap, source);
        if (cloned)
            copy.statements.push_back(cloned);
    }

    for (size_t i = 0; i < copy.conditions.size(); ++i)
        copy.conditions[i] = cloneExpr(copy.conditions[i], args, callSpan, callScope, macro, map,
                                       stmtMap, localMap, source);

    if (macro != nullptr && copy.forInBinding)
        copy.forInBinding = cloneLocal(copy.forInBinding, localMap);
    if (copy.forInBindingStmt && copy.forInBindingStmt.value < stmtMap.size() &&
        stmtMap[copy.forInBindingStmt.value])
        copy.forInBindingStmt = stmtMap[copy.forInBindingStmt.value];

    if (copy.cast_type)
        copy.cast_type = cloneTypeExpr(copy.cast_type, source);

    for (size_t i = 0; i < copy.genericArgs.size(); ++i)
        copy.genericArgs[i] = cloneTypeExpr(copy.genericArgs[i], source);

    copy.whenIslandOps = orig.whenIslandOps;

    ExprId result = addExpression(std::move(copy));
    if (src.value >= map.size())
        map.resize(src.value + 1U);
    map[src.value] = result;
    return result;
}

StmtId MacroExpander::cloneStmt(StmtId src, const std::vector<ExprId> &args, TextSpan callSpan,
                                ScopeId, const MacroInfo *macro, ExprMap &map, StmtMap &stmtMap,
                                LocalMap &localMap, const FrontendSnapshot &source) {
    if (!src || src.value > source.statements().size())
        return {};
    if (src.value < stmtMap.size() && stmtMap[src.value])
        return stmtMap[src.value];

    const auto orig = source.statements()[src.value - 1U];
    if (macro != nullptr && macro->variadicArgs.empty() && orig.kind == StmtKind::Defer &&
        orig.expression && orig.expression.value <= source.expressions().size()) {
        const auto &deferBody = source.expressions()[orig.expression.value - 1U];
        if (deferBody.kind == ExprKind::Block && !deferBody.statements.empty() &&
            std::all_of(deferBody.statements.begin(), deferBody.statements.end(),
                        [&source](const StmtId statementId) {
                            return statementId && statementId.value <= source.statements().size() &&
                                   source.statements()[statementId.value - 1U].isMacroRepetition;
                        })) {
            return {};
        }
    }

    Statement copy         = orig;
    copy.id                = {};
    copy.span              = callSpan;
    copy.isMacroRepetition = false;
    if (macro != nullptr && copy.kind == StmtKind::Binding)
        copy.binding.id = cloneLocal(copy.binding.id, localMap);

    if (copy.expression)
        copy.expression = cloneExpr(
            copy.expression, args, callSpan,
            orig.expression ? source.expressions()[orig.expression.value - 1U].scope : ScopeId{},
            macro, map, stmtMap, localMap, source);

    if (copy.kind == StmtKind::Binding) {
        if (copy.binding.initializer)
            copy.binding.initializer =
                cloneExpr(copy.binding.initializer, args, callSpan,
                          copy.binding.initializer
                              ? source.expressions()[copy.binding.initializer.value - 1U].scope
                              : ScopeId{},
                          macro, map, stmtMap, localMap, source);
        if (copy.binding.type)
            copy.binding.type = cloneTypeExpr(copy.binding.type, source);
    }
    if (copy.kind == StmtKind::Declaration) {
        // Local state declarations are not template expansion targets in this
        // iteration; keep the marker inert so an unsupported template does not
        // leak into resolution.
        copy.kind        = StmtKind::Error;
        copy.declaration = {};
    }

    for (size_t i = 0; i < copy.arguments.size(); ++i)
        copy.arguments[i] =
            cloneExpr(copy.arguments[i], args, callSpan,
                      copy.arguments[i] ? source.expressions()[copy.arguments[i].value - 1U].scope
                                        : ScopeId{},
                      macro, map, stmtMap, localMap, source);

    const StmtId result = addStatement(std::move(copy));
    if (src.value >= stmtMap.size())
        stmtMap.resize(src.value + 1U);
    stmtMap[src.value] = result;
    return result;
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
    const auto field = source.expressions()[src.value - 1U];
    if (field.operands.empty())
        return {};
    const auto baseId = field.operands[0];
    if (!baseId || baseId.value > source.expressions().size())
        return {};
    const auto base = source.expressions()[baseId.value - 1U];
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
            StmtMap stmtMap(snapshot_.statements_.size() + 1U);
            LocalMap localMap;
            const size_t firstExpr = snapshot_.expressions_.size();
            const size_t firstStmt = snapshot_.statements_.size();
            *outClone = cloneExpr((*exprs)[i], {}, callSpanForAttrs_, base.scope, nullptr, dummy,
                                  stmtMap, localMap, snapshot_);
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
    const auto *found = scopeMap_.get(templateScope.value);
    return found ? *found : callScope;
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
