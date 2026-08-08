#pragma once

#include "memory/arena.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace zith::frontend {

template <typename Tag> struct Id {
    uint32_t value = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return value != 0;
    }
    friend constexpr bool operator==(Id, Id) = default;
};

struct ModuleTag {};
struct TokenTag {};
struct SyntaxNodeTag {};
struct ExprTag {};
struct DeclTag {};
struct StmtTag {};
struct TypeExprTag {};
struct LocalTag {};
struct ScopeTag {};
struct SymbolTag {};

using ModuleId     = Id<ModuleTag>;
using TokenId      = Id<TokenTag>;
using SyntaxNodeId = Id<SyntaxNodeTag>;
using ExprId       = Id<ExprTag>;
using DeclId       = Id<DeclTag>;
using StmtId       = Id<StmtTag>;
using TypeExprId   = Id<TypeExprTag>;
using LocalId      = Id<LocalTag>;
using ScopeId      = Id<ScopeTag>;
using SymbolId     = Id<SymbolTag>;

struct TextSpan {
    uint32_t start = 0;
    uint32_t end   = 0;

    [[nodiscard]] constexpr uint32_t size() const noexcept {
        return end - start;
    }
    friend constexpr bool operator==(TextSpan, TextSpan) = default;
};

enum class TriviaKind : uint8_t { Whitespace, LineComment, BlockComment, DocLine, DocBlock };

struct Trivia {
    TriviaKind kind;
    TextSpan span;
};

enum class TokenKind : uint8_t {
    Identifier,
    Keyword,
    Literal,
    Operator,
    Punctuation,
    Unknown,
    End,
};

struct Token {
    TokenKind kind = TokenKind::Unknown;
    TextSpan span;
    uint32_t leadingTriviaStart = 0;
    uint32_t leadingTriviaCount = 0;
};

struct Diagnostic {
    TextSpan span;
    std::string message;
    /// When set the session reports this as a warning instead of an error.
    bool isWarning = false;
    /// `diagnostics::ErrCode` for this message; the session propagates it verbatim.
    /// Defaults to `err::UnknownToken` so an unclassified site stays reportable.
    uint32_t code = 1;
};

enum class SyntaxKind : uint8_t { Root, Token, Error };

enum class Visibility : uint8_t { Private, Public, Module };

enum class DeclKind : uint8_t {
    Error,
    /// `macro name(...) { body }` — source-level macro declaration.
    Macro,
    Import,
    Function,
    TypeAlias,
    Struct,
    Enum,
    Union,
    Trait,
    Interface,
    Variable,
    Context,
    Word,
};

/// Parse-level function kind for `fn`, `const fn`, `raw fn`, `extern fn`, and
/// `flow fn`.  All five share `DeclKind::Function`; this metadata is retained for
/// frontend tooling and formatter output.
enum class FunctionKind : uint8_t {
    Standard,
    Const,
    Raw,
    Extern,
    Flow,
};

enum class ExprKind : uint8_t {
    Error,
    Name,
    Literal,
    Unary,
    /// `@name(args)` call to a user-defined macro; expansion is in `Expression::expansion`.
    MacroCall,
    Binary,
    Call,
    Block,
    If,
    /// `for { }` (infinite) and `for (cond) { }` (conditional).
    While,
    /// 3-clause `for (init; cond; step) { body }`: operands are [cond, body, step];
    /// `init` is desugared into a preceding statement of the enclosing block.
    For,
    Return,
    Assign,
    OptionalProp,
    Index,
    Field,
    Arrow,
    StructLiteral,
    ArrayLiteral,
    Cast,
    IsNull,
    /// `when (subject) { (cond) ~> body, (_) ~> default }` — match is a synonym.
    When,
    /// A range pattern `lo..hi`, valid only inside a `when` case condition.
    Range,
    /// `_` as a struct-literal field value: `Pair{left: _, right: 2}`.
    Placeholder,
    /// The offsetOf / alignOf layout intrinsics.
    LayoutIntrinsic,
};

enum class StmtKind : uint8_t {
    Error,
    Expression,
    Binding,
    Return,
    Break,
    Continue,
    Dock,
    Marker,
    Jump,
};

/// `Opaque` is the parsed form of `raw opaque`, the C-interop spelling of an
/// untyped pointer. It lowers to pointer-to-void; a literal `*void` stays rejected.
enum class TypeExprKind : uint8_t {
    Error,
    Name,
    Pointer,
    Optional,
    Array,
    Function,
    Slice,
    Opaque
};

/// Memory-model qualifier written as a prefix on a type (`lend T`, `view T`, ...).
/// `Default` means the type carried no ownership prefix.
enum class OwnershipKind : uint8_t { Default, Unique, Share, Lend, View, Belong };

struct TypeExpression {
    TypeExprId id;
    TypeExprKind kind = TypeExprKind::Error;
    TextSpan span;
    std::string name;
    std::vector<TypeExprId> arguments;
    uint64_t arrayLength = 0;
    /// Ownership qualifier written before the type, if any.
    OwnershipKind ownership = OwnershipKind::Default;
    /// Resolved mutability: `lend`/`unique`/`share`/`belong` are mutable, `view` is
    /// immutable, `default` is mutable only when written with `mut`.
    bool isMut = false;
    /// True when the type was written with an explicit `mut` prefix.
    bool hasMutKeyword = false;
};

struct Binding {
    LocalId id;
    std::string name;
    bool mutableBinding = false;
    TextSpan span;
    TypeExprId type;
    ExprId initializer;
};

struct Statement {
    StmtId id;
    StmtKind kind = StmtKind::Error;
    TextSpan span;
    ExprId expression;
    Binding binding;
    /// Name of a marker (StmtKind::Marker) or jump target (StmtKind::Jump).
    std::string label;
    /// True for `stackful marker name { }`.
    bool isStackful = false;
};

struct Expression {
    ExprId id;
    ExprKind kind = ExprKind::Error;
    TextSpan span;
    std::string text;
    std::vector<ExprId> operands;
    std::vector<StmtId> statements;
    ScopeId scope;
    // Used by ExprKind::StructLiteral: parallel field name per operand
    std::vector<std::string> field_names;
    // Used by ExprKind::When: parallel case condition per operand (operand[0] is
    // the subject; operands[1..] are case bodies). An empty id marks the default case.
    std::vector<ExprId> conditions;
    // Used by ExprKind::Cast: the target type written after `as`
    TypeExprId cast_type;
    // Used by ExprKind::Call: explicit generic arguments `name<A, B>(...)`.
    std::vector<TypeExprId> genericArgs;
    /// For MacroCall: the result of expansion (a Block expression for normal
    /// macros; zero remains when expansion fails or did not run).
    ExprId expansion;
    /// True when `expansion` is the result of a `raw` macro (the expansion
    /// statements splice directly without a wrapping Block scope).
    bool expansionIsRaw = false;
    /// For MacroCall operands: when true, the argument was prefixed with =,
    /// requesting pass-by-AST (unevaluated expression) instead of pass-by-value.
    std::vector<bool> argIsUnevaluated;
    /// For MacroCall operands: the source span of each argument, recorded so a
    /// later phase can re-splice the argument at token level (Phase 2).
    std::vector<TextSpan> argSpans;
    /// For MacroCall: the call-site attributes list. `attributes` holds one
    /// expression per entry; `attributeNames` holds the parallel name for a
    /// `name: expr` entry and an empty string for a positional entry.
    std::vector<std::string> attributeNames;
    std::vector<ExprId> attributes;
};

struct Scope {
    ScopeId id;
    ScopeId parent;
    TextSpan span;
};

struct Parameter {
    LocalId id;
    std::string name;
    TextSpan span;
    TypeExprId type;
    /// Optional default expression for a struct field: `left: i32 = 3`.
    ExprId defaultValue;
};

struct ImportSelector {
    std::string name;
    std::string alias;
    TextSpan span;
    TextSpan aliasSpan;
};

struct ImportDecl {
    std::vector<std::string> path;
    std::vector<TextSpan> pathSpans;
    std::vector<ImportSelector> selectors;
    std::string rawPath;
    std::string headerPath;
    std::string alias;
    bool isFrom   = false;
    bool isExport = false;
    bool isAsset  = false;
    bool isHeader = false;
    int32_t depth = 1;
    TextSpan pathSpan;
    TextSpan aliasSpan;
};

struct GenericParam {
    std::string name;
    TextSpan span;
    /// Optional `: Constraint` type — parsed but not enforced.
    TypeExprId constraint;
};

struct Declaration {
    DeclId id;
    DeclKind kind         = DeclKind::Error;
    Visibility visibility = Visibility::Private;
    TextSpan span;
    /// Parse-level function kind when `kind == DeclKind::Function`; other
    /// declarations keep `FunctionKind::Standard`.
    FunctionKind functionKind = FunctionKind::Standard;
    /// True when declared with `tag macro`: invoked as `<Name attr: v> ... </Name>`
    /// instead of `@name(...)`, and never produces a value.
    bool isTagMacro = false;
    /// True when declared with `raw macro` (hygiene disabled, splices statements).
    bool isRawMacro = false;
    /// True when the first parameter is named `attributes` (no type), signalling
    /// the macro accepts `|name: expr, ...|` call-site attributes.
    bool hasAttributesParam = false;
    /// True when declared with `extern fn`: the C ABI fixes its linkage name, so it
    /// is never name-qualified and never participates in overloading.
    bool isExtern = false;
    /// True when the declaration ends its parameter list with `...` (`extern fn` only).
    bool isVariadic = false;
    /// True for `type Name = T`; the declaration creates a nominal wrapper.
    /// `alias Name = T` remains a transparent type alias.
    bool isNominalType = false;
    /// Non-empty only for methods lowered from `implement Type as Trait`: the
    /// trait name written after `as`/`for`. Not enforced for dispatch; kept for
    /// context and for resolving `Self` to the implemented type.
    std::string traitName;
    std::string name;
    ImportDecl import;
    std::vector<Parameter> parameters;
    std::vector<GenericParam> genericParams;
    TypeExprId declaredType;
    ExprId initializer;
    ExprId body;
    /// Non-empty for methods: the name of the type that owns this method
    /// (e.g. "Counter" for `fn inc(self: *Counter)` inside `struct Counter`).
    /// Used to mangle the symbol name and to supply the implicit self parameter.
    std::string ownerName;
};

struct GreenNode;

struct GreenElement {
    const GreenNode *node = nullptr;
    TokenId token;

    [[nodiscard]] constexpr bool isNode() const noexcept {
        return node != nullptr;
    }
};

struct GreenNode {
    SyntaxKind kind = SyntaxKind::Error;
    TextSpan span;
    const GreenElement *children = nullptr;
    uint32_t childCount          = 0;
};

class SyntaxToken {
public:
    SyntaxToken(const std::vector<Token> &tokens, const std::string &source, TokenId id)
        : tokens_(&tokens), source_(&source), id_(id) {}

    [[nodiscard]] TokenId id() const noexcept {
        return id_;
    }
    [[nodiscard]] const Token &token() const noexcept;
    [[nodiscard]] std::string_view text() const noexcept;

private:
    const std::vector<Token> *tokens_;
    const std::string *source_;
    TokenId id_;
};

class SyntaxNode {
public:
    SyntaxNode(const GreenNode &green, const std::vector<Token> &tokens, const std::string &source)
        : green_(&green), tokens_(&tokens), source_(&source) {}

    [[nodiscard]] SyntaxKind kind() const noexcept {
        return green_->kind;
    }
    [[nodiscard]] TextSpan span() const noexcept {
        return green_->span;
    }
    [[nodiscard]] uint32_t childCount() const noexcept {
        return green_->childCount;
    }
    [[nodiscard]] const GreenElement &child(uint32_t index) const noexcept;
    [[nodiscard]] SyntaxToken token(uint32_t index) const noexcept;

private:
    const GreenNode *green_;
    const std::vector<Token> *tokens_;
    const std::string *source_;
};

class FrontendSnapshot {
public:
    explicit FrontendSnapshot(std::string source);
    FrontendSnapshot(FrontendSnapshot &&) noexcept            = default;
    FrontendSnapshot &operator=(FrontendSnapshot &&) noexcept = default;
    FrontendSnapshot(const FrontendSnapshot &)                = delete;
    FrontendSnapshot &operator=(const FrontendSnapshot &)     = delete;

    [[nodiscard]] const std::string &source() const noexcept {
        return source_;
    }
    [[nodiscard]] const std::vector<Trivia> &trivia() const noexcept {
        return trivia_;
    }
    [[nodiscard]] const std::vector<Token> &tokens() const noexcept {
        return tokens_;
    }
    [[nodiscard]] const std::vector<Diagnostic> &diagnostics() const noexcept {
        return diagnostics_;
    }
    [[nodiscard]] const std::vector<Declaration> &declarations() const noexcept {
        return declarations_;
    }
    [[nodiscard]] const std::vector<TypeExpression> &typeExpressions() const noexcept {
        return type_expressions_;
    }
    [[nodiscard]] const std::vector<Expression> &expressions() const noexcept {
        return expressions_;
    }
    [[nodiscard]] const std::vector<Statement> &statements() const noexcept {
        return statements_;
    }
    [[nodiscard]] const std::vector<Scope> &scopes() const noexcept {
        return scopes_;
    }
    /// True when this expression belongs to the *template* body of a `macro`
    /// declaration.  Template nodes are inert: they are not real code, so name
    /// resolution and sema skip them and only their clones are analysed.
    [[nodiscard]] bool isMacroTemplateExpr(ExprId id) const noexcept {
        return id.value < macro_template_exprs_.size() && macro_template_exprs_[id.value];
    }
    [[nodiscard]] bool isMacroTemplateStmt(StmtId id) const noexcept {
        return id.value < macro_template_stmts_.size() && macro_template_stmts_[id.value];
    }
    [[nodiscard]] SyntaxNode root() const noexcept;
    [[nodiscard]] double expandMs() const noexcept {
        return expandMs_;
    }
    [[nodiscard]] std::string reconstruct() const;

private:
    friend FrontendSnapshot parse(std::string source);
    friend void lex(FrontendSnapshot &snapshot);
    friend void parseCst(FrontendSnapshot &snapshot);
    friend void lowerAst(FrontendSnapshot &snapshot);
    friend void markMacroTemplates(FrontendSnapshot &snapshot);
    friend class AstLowerer;
    friend class MacroExpander;
    double expandMs_ = 0.0;

    std::string source_;
    memory::Arena arena_;
    std::vector<Trivia> trivia_;
    std::vector<Token> tokens_;
    std::vector<Diagnostic> diagnostics_;
    std::vector<Declaration> declarations_;
    std::vector<TypeExpression> type_expressions_;
    std::vector<Expression> expressions_;
    std::vector<Statement> statements_;
    std::vector<Scope> scopes_;
    /// Indexed by id value; marks nodes reachable from a macro template body.
    std::vector<bool> macro_template_exprs_;
    std::vector<bool> macro_template_stmts_;
    const GreenNode *root_ = nullptr;
};

[[nodiscard]] FrontendSnapshot parse(std::string source);

/// Canonical textual form of a type expression with memory qualifiers removed:
/// `i32`, `f64`, `*T`, `?T`, `[]T`, `[N]T`, or the written type name.  Shared by
/// overload duplicate detection and by linkage-name mangling so the two agree.
[[nodiscard]] std::string canonicalTypeString(const FrontendSnapshot &snapshot, TypeExprId id);

/// Parenthesised parameter-type list of a function declaration, e.g. `(i32,i32)`.
/// A method's implicit `self` is written as `*Owner`.
[[nodiscard]] std::string functionSignature(const FrontendSnapshot &snapshot,
                                            const Declaration &decl);

} // namespace zith::frontend
