#pragma once

#include "frontend/frontend.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace zith::frontend {

class MacroExpander {
public:
    explicit MacroExpander(FrontendSnapshot &snapshot);
    void run(const std::vector<ImportedMacroRecord> &imported = {});

private:
    struct MacroInfo {
        DeclId id;
        std::string name;
        bool isRaw         = false;
        bool isTag         = false;
        bool hasAttributes = false;
        std::vector<std::string> paramNames;
        std::vector<std::string> paramMetaTypes;
        ExprId body;
        TextSpan span;
        const FrontendSnapshot *source = nullptr;
    };

    using ExprMap = std::vector<ExprId>;

    ExprId addExpression(Expression expr);
    StmtId addStatement(Statement stmt);
    ScopeId addScope(Scope scope);
    TypeExprId addTypeExpression(TypeExpression te);

    /// Maps a template scope id to the scope cloned for the current expansion.
    ScopeId mappedScope(ScopeId templateScope, ScopeId callScope) const;

    void markNonHygienic(size_t firstExprIndex, size_t firstStmtIndex);
    [[nodiscard]] bool isNonHygienicExpr(size_t idValue) const;
    [[nodiscard]] bool isNonHygienicStmt(size_t idValue) const;

    ExprId cloneExpr(ExprId src, const std::vector<ExprId> &args, TextSpan callSpan,
                     ScopeId callScope, const MacroInfo *macro, ExprMap &map,
                     const FrontendSnapshot &source);
    /// Resolves `attributes.name` inside a macro body to the call-site
    /// attribute expression, or reports MacroAttrUnknown.  Returns an empty id
    /// when `src` is not an attribute access.
    ExprId substituteAttribute(ExprId src, const MacroInfo *macro, ExprId *outClone,
                               const FrontendSnapshot &source);
    StmtId cloneStmt(StmtId src, const std::vector<ExprId> &args, TextSpan callSpan,
                     ScopeId callScope, const MacroInfo *macro, ExprMap &map,
                     const FrontendSnapshot &source);
    TypeExprId cloneTypeExpr(TypeExprId src, const FrontendSnapshot &source);

    void collectHygieneNames(ExprId body, std::vector<std::string> &names,
                             const FrontendSnapshot &source);
    bool contextIsValue(ExprId eid);
    std::string hygieneName(const std::string &original);

    bool expandCall(ExprId callId, const std::vector<MacroInfo> &macros,
                    std::vector<std::string> &stack);
    void expandExpr(ExprId root, const std::vector<MacroInfo> &macros,
                    std::vector<std::string> &stack);

    /// Expression/statement ids cloned from a *call-site argument* rather than
    /// from the macro template.  Hygiene renaming must skip them: they refer to
    /// the caller's symbols, not to bindings introduced by the macro.
    std::vector<bool> nonHygienicExprs_;
    std::vector<bool> nonHygienicStmts_;
    /// Template scope id -> cloned scope id, rebuilt for every expansion.
    std::unordered_map<uint32_t, ScopeId> scopeMap_;

    /// Attribute names/expressions of the call site being expanded.
    const std::vector<std::string> *callAttrNames_ = nullptr;
    const std::vector<ExprId> *callAttrExprs_      = nullptr;
    TextSpan callSpanForAttrs_{};

    FrontendSnapshot &snapshot_;
    uint32_t hygieneCounter_                = 1;
    static constexpr int kMaxExpansionDepth = 64;
};

/// Marks every expression/statement reachable from a `macro` declaration body
/// as template material, so later phases never analyse it as real code.
void markMacroTemplates(FrontendSnapshot &snapshot);

void expandMacros(FrontendSnapshot &snapshot,
                  const std::vector<ImportedMacroRecord> &imported = {});

} // namespace zith::frontend
