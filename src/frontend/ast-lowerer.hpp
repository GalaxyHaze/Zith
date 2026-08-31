#pragma once

#include "frontend/frontend.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zith::frontend {

void lex(FrontendSnapshot &snapshot);
void parseCst(FrontendSnapshot &snapshot);
void lowerAst(FrontendSnapshot &snapshot);

[[nodiscard]] std::string_view tokenText(const FrontendSnapshot &snapshot, uint32_t index) noexcept;

[[nodiscard]] bool isPunctuation(const FrontendSnapshot &snapshot, uint32_t index,
                                 char character) noexcept;

[[nodiscard]] bool matchesToken(const FrontendSnapshot &snapshot, uint32_t index,
                                std::string_view text) noexcept;

[[nodiscard]] std::optional<DeclKind> declarationKind(std::string_view word) noexcept;

/// Parses a valid function-kind prefix: `fn`, `const fn`, `raw fn`, or `extern fn`.
/// Returns nullopt without consuming tokens when the current token is not `fn` or
/// a kind prefix followed by `fn`.
[[nodiscard]] std::optional<FunctionKind> functionKindPrefix(const FrontendSnapshot &snapshot,
                                                             uint32_t &index,
                                                             uint32_t token_count) noexcept;

[[nodiscard]] BindingKind bindingKind(std::string_view word) noexcept;

/// Lowers the syntax tree from the token stream into the arena-backed snapshot
/// expression/statement/declaration tables. The class is exposed across the
/// frontend parser TUs; only `frontend.cpp` constructs it.
class AstLowerer {
public:
    explicit AstLowerer(FrontendSnapshot &snapshot);

    void run();

    void skipMacroInvocation();

private:
    [[nodiscard]] std::string_view text(uint32_t index) const noexcept;
    [[nodiscard]] bool punctuation(uint32_t index, char character) const noexcept;
    [[nodiscard]] TextSpan tokenSpan(uint32_t index) const noexcept;
    [[nodiscard]] TextSpan range(uint32_t start, uint32_t end) const noexcept;

    [[nodiscard]] ExprId addExpression(Expression expression);
    [[nodiscard]] StmtId addStatement(Statement statement);
    [[nodiscard]] TypeExprId addType(TypeExpression type);
    [[nodiscard]] ScopeId addScope(ScopeId parent, TextSpan span);

    static bool ownershipKeyword(std::string_view word, OwnershipKind &out) noexcept;

    [[nodiscard]] TypeExprId parseType();
    [[nodiscard]] ExprId parseCallArgument();
    [[nodiscard]] ExprId parsePrimary();
    [[nodiscard]] bool isOperatorToken(std::string_view op) const noexcept;
    [[nodiscard]] bool isGenericApplication() const noexcept;
    [[nodiscard]] bool isKeywordToken(std::string_view word) const noexcept;
    [[nodiscard]] bool isVisibilityPrefix() const noexcept;
    [[nodiscard]] std::optional<FunctionKind> functionKindPrefix();
    [[nodiscard]] ExprId parseAttributeValue();
    [[nodiscard]] ExprId parsePostfix(ExprId result, uint32_t start);

    static constexpr const char *kIntrinsicNames[] = {
        "offsetOf", "alignOf",     "sizeOf",      "fields",       "hasTrait", "struct", "component",
        "union",    "enum",        "nullable",    "primitive",    "allocate", "pack",   "toStruct",
        "toPack",   "appendField", "removeField", "appendMethod", "file",     "line",   "fnName",
        "location", "ok",          "err",         "lengthOf",     "ptrOf",
    };

    static bool isIntrinsicName(std::string_view name) noexcept;
    [[nodiscard]] static int precedence(std::string_view op) noexcept;
    static constexpr int kUnaryPrecedence = 12;
    [[nodiscard]] static bool isAssignmentOp(std::string_view op) noexcept;
    [[nodiscard]] static std::string_view compoundBaseOp(std::string_view op) noexcept;

    [[nodiscard]] ExprId parseExpression(int minimum_precedence = 0);
    [[nodiscard]] ExprId parseBlock();
    void parseArgumentList(std::vector<ExprId> &out);
    [[nodiscard]] ExprId parseElseTail(uint32_t start);
    [[nodiscard]] ExprId parseIf();
    [[nodiscard]] ExprId parseWhen();
    void applyLoopLabel(ExprId id, std::string_view label);
    [[nodiscard]] ExprId parseFor();
    [[nodiscard]] ExprId parseWhile();
    [[nodiscard]] ExprId parseConditionExpression();
    [[nodiscard]] bool isOperatorAt(uint32_t offset, std::string_view op) const noexcept;
    [[nodiscard]] bool isTagMacroOpen() const noexcept;
    [[nodiscard]] ExprId parseTagMacroCall();
    [[nodiscard]] std::vector<StmtId> parseStatements();

    void lowerImport(uint32_t start, Visibility visibility);
    void parseImportPath(ImportDecl &import);
    void parseImportDepth(ImportDecl &import);
    void parseImportSelectors(ImportDecl &import);
    void lowerMacroDeclaration(uint32_t start, Visibility visibility, bool isRaw, bool isTag);
    void lowerImplementBlock(uint32_t start, Visibility visibility);
    DeclId lowerDeclaration(uint32_t start, DeclKind kind, Visibility visibility,
                            std::string ownerName = {}, std::string traitName = {},
                            bool isExtern             = false,
                            FunctionKind functionKind = FunctionKind::Standard,
                            const std::vector<GenericParam> &inheritedParams = {},
                            bool isRawUnion = false, bool suppressTopLevelBindingCheck = false,
                            ScopeId parentScope = {}, const std::string &parentName = {});

    bool parseStructField(std::vector<Parameter> &out);
    bool parseInterfaceField(std::vector<Parameter> &out);
    void skipDelimited(char open, char close);
    void skipNestedUnsupportedDeclaration();

    FrontendSnapshot &snapshot_;
    uint32_t token_count_;
    uint32_t index_      = 0;
    uint32_t next_scope_ = 1;
    ScopeId root_scope_;
    ScopeId current_scope_;
    bool declaration_is_nominal_        = false;
    bool range_mode_                    = false;
    bool suppress_struct_literal_       = false;
    uint32_t statementCountLocals_      = 1;
    bool expecting_function_body_scope_ = false;
    ScopeId current_function_body_scope_;
    bool current_local_parent_is_state_ = false;
    std::string current_local_parent_name_;
};

} // namespace zith::frontend
