#include "sema/sema.hpp"

#include "sema/errors.hpp"
#include "session/session.hpp"

#include <cctype>
#include <cstdlib>
#include <string>
#include <cstring>
#include <string_view>
#include <vector>

namespace toolkit::sema {
namespace {

using common::memory::DynArray;
using generated_ast::AstNode;
using common::memory::FlatMap;
using common::memory::Span;
using generated_ast::Binding;
using generated_ast::Declaration;
using generated_ast::Expr;
using generated_ast::ExprField;
using generated_ast::Parameter;
using generated_ast::Program;
using generated_ast::Stmt;
using generated_ast::TypeExpr;

[[nodiscard]] bool looksInteger(std::string_view text) {
    if (text.empty())
        return false;
    std::size_t start = 0;
    if (text[0] == '-' || text[0] == '+')
        ++start;
    if (start == text.size())
        return false;
    if (text.size() - start >= 2 && text[start] == '0') {
        const char marker = text[start + 1];
        if (marker == 'x' || marker == 'X' || marker == 'b' || marker == 'B' ||
            marker == 'c' || marker == 'C' || marker == 'o' || marker == 'O') {
            start += 2;
            if (start == text.size())
                return false;
        }
    }
    for (std::size_t index = start; index < text.size(); ++index) {
        const char c = text[index];
        if (!std::isdigit(static_cast<unsigned char>(c)) && c != '_' &&
            !(c >= 'a' && c <= 'f') && !(c >= 'A' && c <= 'F'))
            return false;
    }
    return true;
}

[[nodiscard]] bool looksFloat(std::string_view text) {
    if (text.empty())
        return false;
    bool sawDigit = false;
    bool sawDot = false;
    std::size_t index = 0;
    if (text[0] == '-' || text[0] == '+')
        ++index;
    for (; index < text.size(); ++index) {
        const char c = text[index];
        if (std::isdigit(static_cast<unsigned char>(c))) {
            sawDigit = true;
        } else if (c == '.') {
            if (sawDot)
                return false;
            sawDot = true;
        } else {
            return false;
        }
    }
    return sawDigit && sawDot;
}

[[nodiscard]] bool looksBool(std::string_view text) {
    return text == "true" || text == "false";
}

[[nodiscard]] bool looksString(std::string_view text) {
    return text.size() >= 2 && text.front() == '"' && text.back() == '"';
}

[[nodiscard]] bool looksChar(std::string_view text) {
    return text.size() >= 3 && text.front() == '\'' && text.back() == '\'';
}

[[nodiscard]] bool isComparisonOp(std::string_view op) {
    return op == "==" || op == "!=" || op == "<" || op == ">" ||
           op == "<=" || op == ">=";
}

[[nodiscard]] bool isShiftOp(std::string_view op) {
    return op == "<<" || op == ">>";
}

[[nodiscard]] bool isBitwiseOp(std::string_view op) {
    return op == "&" || op == "|" || op == "^";
}

[[nodiscard]] bool isArithmeticOp(std::string_view op) {
    return op == "+" || op == "-" || op == "*" || op == "/" || op == "%";
}

[[nodiscard]] std::int64_t parseDiscriminant(const Expr *expr) {
    if (expr == nullptr)
        return 0;
    if (expr->kind == static_cast<int>(sample::ExprKind::Unary) &&
        expr->op == "-" && !expr->operands.empty()) {
        const auto *operand = static_cast<const Expr *>(expr->operands[0]);
        if (operand == nullptr || operand->kind != static_cast<int>(sample::ExprKind::Literal))
            return 0;
        const std::string text(operand->text);
        char *end = nullptr;
        const long long value = std::strtoll(text.c_str(), &end, 0);
        if (end == nullptr || end == text.c_str() || *end != '\0')
            return 0;
        return -value;
    }
    if (expr->kind != static_cast<int>(sample::ExprKind::Literal))
        return 0;
    const std::string text(expr->text);
    char *end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 0);
    if (end == nullptr || end == text.c_str() || *end != '\0')
        return 0;
    return value;
}

[[nodiscard]] bool isExplicitZero(const Expr *expr) {
    return expr != nullptr && expr->kind == static_cast<int>(sample::ExprKind::Literal) &&
           expr->text == "0";
}

struct Local {
    const AstNode *node = nullptr;
    std::string_view name;
    TypeId type = kInvalidTypeId;
    Ownership ownership = Ownership::Default;
    bool isMut = false;
    bool isView = false;
};

struct Scope {
    FlatMap<std::string_view, Local> locals;
};

struct DeclInfo {
    const Declaration *decl = nullptr;
    TypeId type = kInvalidTypeId;
    bool lowered = false;
};

struct Candidate {
    const Declaration *decl = nullptr;
    TypeId type = kInvalidTypeId;
};

struct FunctionInfo {
    const Declaration *decl = nullptr;
    TypeId type = kInvalidTypeId;
    bool lowered = false;
};

class ScopeStack {
public:
    explicit ScopeStack(common::memory::Arena &arena) : scopes_(arena) {
        push();
    }

    void push() {
        scopes_.push(Scope{});
    }

    void pop() {
        if (scopes_.size() > 1)
            scopes_.pop_back();
    }

    Local *find(std::string_view name) {
        for (std::size_t index = scopes_.size(); index-- > 0;) {
            if (Local *local = scopes_[index].locals.get(name))
                return local;
        }
        return nullptr;
    }

    void declare(std::string_view name, Local local) {
        scopes_.back().locals.insert(name, local);
    }

    [[nodiscard]] bool containsInCurrent(std::string_view name) const {
        return scopes_.back().locals.contains(name);
    }

private:
    DynArray<Scope> scopes_;
};

class TypeChecker final {
public:
    TypeChecker(Program &program, common::memory::FileId fileId,
                common::memory::Arena &arena,
                common::memory::StringInterner &interner,
                DynArray<common::diagnostic::Diagnostic> &diagnostics,
                TypeCheckedInfo &info)
        : program_(program), fileId_(fileId), arena_(arena), interner_(interner),
          diagnostics_(diagnostics), info_(info), scopes_(arena),
          decls_(), declOrder_(arena), functions_(arena) {}

    bool run() {
        collectDeclarations();
        prepareTypes();
        if (hasErrors())
            return false;
        inferDeclarations();
        info_.success = !hasErrors();
        return info_.success;
    }

private:
    Program &program_;
    common::memory::FileId fileId_;
    common::memory::Arena &arena_;
    common::memory::StringInterner &interner_;
    DynArray<common::diagnostic::Diagnostic> &diagnostics_;
    TypeCheckedInfo &info_;
    ScopeStack scopes_;
    FlatMap<std::string_view, DeclInfo> decls_;
    DynArray<DeclInfo> declOrder_;
    DynArray<FunctionInfo> functions_;
    TypeId currentReturnType_ = kInvalidTypeId;
    const Declaration *currentDecl_ = nullptr;
    bool hasDiagnostics_ = false;

    void report(Span span, std::string message, Err code) {
        hasDiagnostics_ = true;
        diagnostics_.push(common::diagnostic::Diagnostic{
            .span = common::memory::SourceSpan{fileId_, span},
            .severity = common::diagnostic::Severity::Error,
            .code = static_cast<std::uint32_t>(code),
            .message = std::move(message),
        });
    }

    [[nodiscard]] bool hasErrors() const noexcept {
        return hasDiagnostics_;
    }

    [[nodiscard]] TypeId errorType() const noexcept {
        return info_.types.error();
    }

    [[nodiscard]] TypeId invalidType() const noexcept {
        return info_.types.invalid();
    }

    [[nodiscard]] TypeId voidType() const noexcept {
        return info_.types.voidType();
    }

    [[nodiscard]] TypeId boolType() const noexcept {
        return info_.types.boolType();
    }

    [[nodiscard]] TypeId i32Type() const noexcept {
        return info_.types.i32Type();
    }

    [[nodiscard]] TypeId i64Type() const noexcept {
        return info_.types.i64Type();
    }

    [[nodiscard]] TypeId f32Type() const noexcept {
        return info_.types.f32Type();
    }

    [[nodiscard]] TypeId f64Type() const noexcept {
        return info_.types.f64Type();
    }

    void collectDeclarations() {
        for (AstNode *node : program_.body) {
            if (node == nullptr || node->kind != generated_ast::NodeKind::Declaration)
                continue;
            auto *decl = static_cast<Declaration *>(node);
            const auto kind = static_cast<sample::DeclKind>(decl->kind);
            if (kind == sample::DeclKind::Import || kind == sample::DeclKind::Word ||
                kind == sample::DeclKind::Context) {
                report(decl->span, "import/word/context declarations need resolution",
                       Err::UnsupportedSyntax);
                continue;
            }
            if (decl->name.empty()) {
                report(decl->span, "declaration requires a name", Err::UnsupportedSyntax);
                continue;
            }
            if (kind == sample::DeclKind::Function) {
                functions_.push(
                    FunctionInfo{.decl = decl, .type = invalidType(), .lowered = false});
                declOrder_.push(
                    DeclInfo{.decl = decl, .type = invalidType(), .lowered = false});
            } else if (decls_.contains(decl->name)) {
                report(decl->span, "duplicate declaration '" + std::string(decl->name) + "'",
                       Err::DuplicateDecl);
                continue;
            }
            decls_.insert(decl->name, DeclInfo{.decl = decl, .type = errorType(), .lowered = false});
            declOrder_.push(DeclInfo{.decl = decl, .type = errorType(), .lowered = false});
        }
    }

    void prepareTypes() {
        for (const DeclInfo &declaration : declOrder_) {
            const auto kind = static_cast<sample::DeclKind>(declaration.decl->kind);
            switch (kind) {
            case sample::DeclKind::Struct:
                (void)info_.types.findOrCreateNamed(declaration.decl->name, TypeKind::Struct);
                break;
            case sample::DeclKind::Enum:
                (void)info_.types.findOrCreateNamed(declaration.decl->name, TypeKind::Enum);
                break;
            case sample::DeclKind::Union:
                (void)info_.types.findOrCreateNamed(declaration.decl->name, TypeKind::Union);
                break;
            case sample::DeclKind::Trait:
            case sample::DeclKind::Interface:
                (void)info_.types.findOrCreateNamed(declaration.decl->name, TypeKind::Trait);
                break;
            case sample::DeclKind::TypeAlias:
                (void)info_.types.findOrCreateNamed(declaration.decl->name, TypeKind::Alias);
                break;
            default:
                break;
            }
        }
        for (const DeclInfo &declaration : declOrder_)
            lowerDeclaration(declaration.decl);
        for (std::size_t left = 0; left < functions_.size(); ++left) {
            for (std::size_t right = left + 1; right < functions_.size(); ++right) {
                if (functions_[left].decl->name == functions_[right].decl->name &&
                    sameType(functions_[left].type, functions_[right].type)) {
                    report(functions_[right].decl->span,
                           "duplicate function signature '" +
                               std::string(functions_[right].decl->name) + "'",
                           Err::DuplicateDecl);
                }
            }
        }
    }

    TypeId lowerTypeExpr(const TypeExpr *type) {
        if (type == nullptr)
            return errorType();
        if (type->kind == static_cast<int>(sample::TypeExprKind::Name) &&
            info_.types.lookupNamed(type->name) == kInvalidTypeId) {
            report(type->span, "unknown type '" + std::string(type->name) + "'",
                   Err::UndefinedIdent);
            return errorType();
        }
        if (type->kind == static_cast<int>(sample::TypeExprKind::Pointer) &&
            !type->arguments.empty()) {
            const auto *inner = static_cast<const TypeExpr *>(type->arguments[0]);
            if (inner != nullptr &&
                info_.types.resolve(info_.types.lowerTypeExpr(inner)) ==
                    info_.types.resolve(info_.types.voidType())) {
                report(type->span,
                       "pointer to 'void' is not allowed; use 'raw opaque' for C interop",
                       Err::TypeMismatch);
                return errorType();
            }
        }
        return info_.types.lowerTypeExpr(type);
    }

    TypeId lowerDeclaration(const Declaration *decl) {
        const bool isFunction =
            static_cast<sample::DeclKind>(decl->kind) == sample::DeclKind::Function;
        if (isFunction) {
            const std::size_t functionIndex = findFunction(decl);
            if (functionIndex < functions_.size() && functions_[functionIndex].lowered)
                return functions_[functionIndex].type;
        }
        DeclInfo *entry = decls_.get(decl->name);
        if (!isFunction && entry != nullptr && entry->lowered)
            return entry->type;

        TypeId result;
        (void)result;
        result = errorType();
        switch (static_cast<sample::DeclKind>(decl->kind)) {
        case sample::DeclKind::Function: {
            DynArray<TypeId> &params = info_.types.makeTypeStorage();
            for (AstNode *node : decl->parameters) {
                auto *param = static_cast<Parameter *>(node);
                const TypeId ptype = param->type != nullptr
                                         ? lowerTypeExpr(static_cast<const TypeExpr *>(param->type))
                                         : errorType();
                params.push(ptype);
                const Ownership ownership =
                                     ownershipFromTypeExpr(static_cast<const TypeExpr *>(param->type));
                info_.set(param, TypeAnnotation{ptype, ownership, false});
            }
            const TypeId ret = decl->declaredType != nullptr
                                   ? lowerTypeExpr(static_cast<const TypeExpr *>(decl->declaredType))
                                   : voidType();
            result = info_.types.internFunction(params, ret);
            const std::size_t functionIndex = findFunction(decl);
            if (functionIndex < functions_.size()) {
                functions_[functionIndex].type = result;
                functions_[functionIndex].lowered = true;
            }
            break;
        }
        case sample::DeclKind::Variable:
            result = decl->declaredType != nullptr
                         ? lowerTypeExpr(static_cast<const TypeExpr *>(decl->declaredType))
                         : invalidType();
            break;
        case sample::DeclKind::TypeAlias: {
            if (decl->declaredType == nullptr) {
                report(decl->span, "type alias requires an underlying type", Err::TypeMismatch);
                break;
            }
            const TypeId target = lowerTypeExpr(static_cast<const TypeExpr *>(decl->declaredType));
            result = decl->isNominalType ? info_.types.internNominal(decl->name, target)
                                         : info_.types.internAlias(target);
            info_.types.registerNamed(decl->name, result);
            break;
        }
        case sample::DeclKind::Struct: {
            DynArray<TypeId> &fields = info_.types.makeTypeStorage();
            DynArray<std::string_view> &names = info_.types.makeNameStorage();
            for (AstNode *node : decl->parameters) {
                auto *param = static_cast<Parameter *>(node);
                fields.push(param->type != nullptr
                                ? lowerTypeExpr(static_cast<const TypeExpr *>(param->type))
                                : errorType());
                char *nameStorage = static_cast<char *>(arena_.alloc(param->name.size(), 1));
                if (nameStorage != nullptr && !param->name.empty()) {
                    std::memcpy(nameStorage, param->name.data(), param->name.size());
                    names.push(std::string_view{nameStorage, param->name.size()});
                } else {
                    names.push(param->name);
                }
            }
            result = info_.types.internStruct(decl->name, fields, names);
            info_.types.registerNamed(decl->name, result);
            break;
        }
        case sample::DeclKind::Enum: {
            const TypeId underlying = decl->declaredType != nullptr
                                          ? lowerTypeExpr(static_cast<const TypeExpr *>(decl->declaredType))
                                          : i32Type();
            DynArray<std::string_view> &variantNames = info_.types.makeNameStorage();
            DynArray<std::int64_t> &discriminants = info_.types.makeDiscStorage();
            std::int64_t next = 0;
            for (AstNode *node : decl->parameters) {
                auto *param = static_cast<Parameter *>(node);
                for (std::string_view existing : variantNames) {
                    if (existing == param->name) {
                        report(param->span, "duplicate enum variant '" + std::string(param->name) + "'",
                               Err::DuplicateDecl);
                        break;
                    }
                }
                if (param->defaultValue != nullptr) {
                    const auto *valueExpr = static_cast<const Expr *>(param->defaultValue);
                    const std::int64_t discriminant = parseDiscriminant(valueExpr);
                    if (!isExplicitZero(valueExpr) && discriminant == 0)
                        report(param->span, "enum variant discriminant must be an integer literal",
                               Err::TypeMismatch);
                    else
                        next = discriminant;
                }
                char *nameStorage = static_cast<char *>(arena_.alloc(param->name.size(), 1));
                if (nameStorage != nullptr && !param->name.empty()) {
                    std::memcpy(nameStorage, param->name.data(), param->name.size());
                    variantNames.push(std::string_view{nameStorage, param->name.size()});
                } else {
                    variantNames.push(param->name);
                }
                discriminants.push(next++);
            }
            result = info_.types.internEnum(decl->name, underlying, variantNames, discriminants);
            info_.types.registerNamed(decl->name, result);
            break;
        }
        case sample::DeclKind::Union: {
            DynArray<TypeId> &members = info_.types.makeTypeStorage();
            for (AstNode *node : decl->parameters) {
                auto *param = static_cast<Parameter *>(node);
                members.push(param->type != nullptr
                                 ? lowerTypeExpr(static_cast<const TypeExpr *>(param->type))
                                 : errorType());
            }
            result = info_.types.internUnion(decl->name, members);
            info_.types.registerNamed(decl->name, result);
            break;
        }
        case sample::DeclKind::Trait:
        case sample::DeclKind::Interface:
            result = info_.types.internTrait(decl->name);
            info_.types.registerNamed(decl->name, result);
            break;
        case sample::DeclKind::State: {
            for (AstNode *node : decl->parameters) {
                auto *param = static_cast<Parameter *>(node);
                const TypeId ptype = param->type != nullptr
                                         ? lowerTypeExpr(static_cast<const TypeExpr *>(param->type))
                                         : errorType();
                info_.set(param, TypeAnnotation{
                                     ptype,
                                     ownershipFromTypeExpr(
                                         static_cast<const TypeExpr *>(param->type)),
                                     false,
                                 });
            }
            result = voidType();
            break;
        }
        case sample::DeclKind::Macro:
            result = voidType();
            break;
        default:
            report(decl->span, "unsupported declaration '" + std::string(decl->name) + "'",
                   Err::UnsupportedSyntax);
            break;
        }
        if (entry != nullptr) {
            entry->type = result;
            entry->lowered = true;
        } else {
            decls_.insert(decl->name, DeclInfo{decl, result, true});
        }
        return result;
    }

    std::size_t findFunction(
        const Declaration *decl) noexcept {
        for (std::size_t index = 0; index < functions_.size(); ++index) {
            if (functions_[index].decl == decl)
                return index;
        }
        return functions_.size();
    }

    const FunctionInfo *findFunction(
        std::string_view name) noexcept {
        for (std::size_t index = functions_.size(); index-- > 0;) {
            if (functions_[index].decl->name == name &&
                functions_[index].decl->ownerName.empty() &&
                functionType(functions_[index].type) != nullptr)
                return &functions_[index];
        }
        return nullptr;
    }

    [[nodiscard]] const TypeDesc *functionType(TypeId type) const {
        const TypeDesc *desc = info_.types.find(info_.types.resolve(type));
        return desc != nullptr && desc->kind == TypeKind::Function ? desc : nullptr;
    }

    TypeId functionResultType(const Declaration *decl) {
        const std::size_t functionIndex = findFunction(decl);
        const TypeDesc *signature =
            functionIndex < functions_.size()
                ? functionType(functions_[functionIndex].type)
                : nullptr;
        return signature != nullptr ? signature->inner : voidType();
    }

    void inferDeclarations() {
        for (const DeclInfo &declaration : declOrder_) {
            const Declaration *decl = declaration.decl;
            currentDecl_ = decl;
            switch (static_cast<sample::DeclKind>(decl->kind)) {
            case sample::DeclKind::Function:
                currentReturnType_ = functionResultType(decl);
                inferFunction(decl);
                break;
            case sample::DeclKind::Variable: {
                TypeId declared = decls_.get(decl->name) ? decls_[decl->name].type : invalidType();
                if (decl->initializer != nullptr) {
                    const TypeId value = inferExpr(static_cast<Expr *>(decl->initializer));
                    if (declared == invalidType()) {
                        if (info_.types.resolve(value) == info_.types.null()) {
                            report(decl->span, "null requires an optional type annotation",
                                   Err::TypeMismatch);
                            declared = errorType();
                        } else {
                            declared = value;
                        }
                        decls_[decl->name].type = declared;
                    } else if (!coerceValue(decl->initializer, declared, value)) {
                        reportCoercionFailure(decl->span, declared, value,
                                              "global initializer type mismatch");
                    }
                }
                info_.set(decl, TypeAnnotation{
                                    declared,
                                    ownershipFromTypeExpr(
                                        static_cast<const TypeExpr *>(decl->declaredType)),
                                    false,
                                });
                break;
            }
            default:
                if (decl->body != nullptr)
                    (void)inferExpr(static_cast<Expr *>(decl->body));
                break;
            }
            currentReturnType_ = kInvalidTypeId;
            currentDecl_ = nullptr;
        }
    }

    void inferFunction(const Declaration *decl) {
        scopes_.push();
        for (AstNode *node : decl->parameters) {
            auto *param = static_cast<Parameter *>(node);
            if (param->name.empty()) {
                report(param->span, "parameter requires a name", Err::UnsupportedSyntax);
                continue;
            }
            if (scopes_.containsInCurrent(param->name)) {
                report(param->span, "duplicate parameter '" + std::string(param->name) + "'",
                       Err::DuplicateDecl);
                continue;
            }
            const TypeId ptype = param->type != nullptr
                                     ? lowerTypeExpr(static_cast<const TypeExpr *>(param->type))
                                     : errorType();
            const Ownership ownership =
                ownershipFromTypeExpr(static_cast<const TypeExpr *>(param->type));
            scopes_.declare(param->name, Local{
                                            param,
                                            param->name,
                                            ptype,
                                            ownership,
                                            false,
                                            ownership == Ownership::View,
                                        });
            info_.set(param, TypeAnnotation{ptype, ownership, false});
        }

        if (decl->body != nullptr) {
            const TypeId bodyType = inferExpr(static_cast<Expr *>(decl->body));
            const bool hasReturn = countReturns(decl->body) > 0;
            if (!hasReturn && currentReturnType_ != voidType() &&
                currentReturnType_ != errorType() && bodyType != voidType() &&
                bodyType != errorType() &&
                !coerceValue(decl->body, currentReturnType_, bodyType)) {
                reportCoercionFailure(static_cast<Expr *>(decl->body)->span,
                                      currentReturnType_, bodyType,
                                      "function body type does not match declared return type");
            }
        } else if (currentReturnType_ != voidType() && currentReturnType_ != errorType()) {
            report(decl->span, "function with a declared return type has no body",
                   Err::TypeMismatch);
        }
        scopes_.pop();
    }

    std::size_t countReturns(const AstNode *node) {
        if (node == nullptr)
            return 0;
        std::size_t count = 0;
        if (node->kind == generated_ast::NodeKind::Stmt &&
            static_cast<const Stmt *>(node)->kind == static_cast<int>(sample::StmtKind::Return))
            ++count;
        if (node->kind == generated_ast::NodeKind::Expr) {
            const auto *expr = static_cast<const Expr *>(node);
            for (const AstNode *child : expr->operands)
                count += countReturns(child);
            for (const AstNode *child : expr->statements)
                count += countReturns(child);
            for (const AstNode *child : expr->conditions)
                count += countReturns(child);
            for (const AstNode *child : expr->cases)
                count += countReturns(child);
            if (expr->alternate != nullptr)
                count += countReturns(expr->alternate);
            for (const AstNode *child : expr->fieldNames)
                count += countReturns(child);
            if (expr->castType != nullptr)
                count += countReturns(expr->castType);
        } else if (node->kind == generated_ast::NodeKind::Stmt) {
            const auto *stmt = static_cast<const Stmt *>(node);
            if (stmt->expression != nullptr)
                count += countReturns(stmt->expression);
            if (stmt->returnValue != nullptr)
                count += countReturns(stmt->returnValue);
            if (stmt->binding != nullptr)
                count += countReturns(stmt->binding);
        }
        return count;
    }

    TypeId inferExpr(Expr *expr) {
        if (expr == nullptr)
            return errorType();
        if (const TypeAnnotation *annotation = info_.annotation(expr))
            return annotation->type;

        const TypeId result = [&]() -> TypeId {
            switch (static_cast<sample::ExprKind>(expr->kind)) {
            case sample::ExprKind::Literal:
                return inferLiteral(expr);
            case sample::ExprKind::Name:
                return inferName(expr);
            case sample::ExprKind::Unary:
                return inferUnary(expr);
            case sample::ExprKind::Binary:
                return inferBinary(expr);
            case sample::ExprKind::Assign:
                return inferAssign(expr);
            case sample::ExprKind::Call:
                return inferCall(expr);
            case sample::ExprKind::Index:
                return inferIndex(expr);
            case sample::ExprKind::Field:
                return inferField(expr);
            case sample::ExprKind::Arrow:
                return inferArrow(expr);
            case sample::ExprKind::OptionalProp:
                return inferOptionalProp(expr);
            case sample::ExprKind::Cast:
                return inferCast(expr);
            case sample::ExprKind::StructLiteral:
                return inferStructLiteral(expr);
            case sample::ExprKind::ArrayLiteral:
                return inferArrayLiteral(expr);
            case sample::ExprKind::Block:
                return inferBlock(expr);
            case sample::ExprKind::If:
                return inferIf(expr);
            case sample::ExprKind::For:
                return inferFor(expr);
            case sample::ExprKind::When:
                return inferWhen(expr);
            case sample::ExprKind::Range:
                return inferRange(expr);
            case sample::ExprKind::IsNull:
                return inferIsNull(expr);
            case sample::ExprKind::LayoutIntrinsic:
                return inferLayoutIntrinsic(expr);
            case sample::ExprKind::Placeholder:
                return errorType();
            case sample::ExprKind::MacroCall:
                report(expr->span, "macro calls are not supported in semantic analysis",
                       Err::UnsupportedSyntax);
                return errorType();
            default:
                report(expr->span, "unsupported expression syntax",
                       Err::UnsupportedSyntax);
                return errorType();
            }
        }();
        if (result != kInvalidTypeId &&
            info_.annotation(expr) == nullptr) {
            info_.set(expr, TypeAnnotation{result, Ownership::Default, false});
        }
        return result;
    }

    TypeId inferLiteral(Expr *expr) {
        if (expr->text == "null")
            return info_.types.null();
        if (looksBool(expr->text))
            return boolType();
        if (looksFloat(expr->text))
            return f64Type();
        if (looksInteger(expr->text)) {
            const std::string text(expr->text);
            char *end = nullptr;
            const unsigned long long value = std::strtoull(text.c_str(), &end, 0);
            if (end == nullptr || end == text.c_str() || *end != '\0')
                report(expr->span, "integer literal '" + text + "' does not fit in 64 bits",
                       Err::CannotInfer);
            return value > 0xFFFFFFFFULL ? i64Type() : i32Type();
        }
        if (looksString(expr->text)) {
            TypeId stringType = info_.types.lookupNamed("string");
            if (stringType == kInvalidTypeId)
                stringType = info_.types.internName("string", TypeKind::String);
            return stringType;
        }
        if (looksChar(expr->text))
            return info_.types.charType();
        report(expr->span, "unsupported literal '" + std::string(expr->text) + "'",
               Err::CannotInfer);
        return errorType();
    }

    TypeId inferName(Expr *expr) {
        if (Local *local = scopes_.find(expr->text)) {
            if (info_.types.resolve(local->type) == invalidType()) {
                report(expr->span,
                       "cannot infer type of '" + std::string(expr->text) +
                           "'; assign a value before reading it or add a type annotation",
                       Err::CannotInfer);
                info_.set(expr, TypeAnnotation{errorType(), Ownership::Default, false});
                return errorType();
            }
            info_.set(expr, TypeAnnotation{local->type, local->ownership, local->isMut});
            return local->type;
        }
        if (DeclInfo *decl = decls_.get(expr->text)) {
            const Ownership ownership =
                decl->decl->declaredType != nullptr
                    ? ownershipFromTypeExpr(static_cast<const TypeExpr *>(decl->decl->declaredType))
                    : Ownership::Default;
            info_.set(expr, TypeAnnotation{decl->type, ownership, false});
            return decl->type;
        }
        if (const FunctionInfo *function = findFunction(expr->text)) {
            info_.set(expr, TypeAnnotation{function->type, Ownership::Default, false});
            return function->type;
        }
        if (info_.types.lookupNamed(expr->text) != kInvalidTypeId) {
            const TypeId type = info_.types.lookupNamed(expr->text);
            info_.set(expr, TypeAnnotation{type, Ownership::Default, false});
            return type;
        }
        report(expr->span, "unknown identifier '" + std::string(expr->text) + "'",
               Err::UndefinedIdent);
        return errorType();
    }

    TypeId inferUnary(Expr *expr) {
        if (expr->operands.empty())
            return errorType();
        const TypeId operand = inferExpr(static_cast<Expr *>(expr->operands[0]));
        if (expr->op == "!" || expr->op == "not") {
            if (!sameType(operand, boolType()))
                report(expr->span, "unary 'not' expects a boolean operand", Err::TypeMismatch);
            return boolType();
        }
        if (expr->op == "-") {
            if (!info_.types.isNumeric(operand))
                report(expr->span, "unary '-' expects a numeric operand", Err::TypeMismatch);
            return operand;
        }
        if (expr->op == "~") {
            if (!info_.types.isInteger(operand))
                report(expr->span, "unary '~' expects an integer operand", Err::TypeMismatch);
            return operand;
        }
        if (expr->op == "&")
            return info_.types.internPointer(operand);
        if (expr->op == "*") {
            const TypeDesc *desc = info_.types.find(info_.types.resolve(operand));
            if (desc == nullptr || desc->kind != TypeKind::Pointer) {
                report(expr->span, "unary '*' expects a pointer operand", Err::TypeMismatch);
                return errorType();
            }
            return desc->inner;
        }
        report(expr->span, "unsupported unary operator '" + std::string(expr->op) + "'",
               Err::UnsupportedSyntax);
        return errorType();
    }

    TypeId inferBinary(Expr *expr) {
        if (expr->operands.size() < 2)
            return errorType();
        TypeId left = inferExpr(static_cast<Expr *>(expr->operands[0]));
        TypeId right = inferExpr(static_cast<Expr *>(expr->operands[1]));
        if (!sameType(left, right)) {
            if (adaptNumericLiteral(expr->operands[1], left))
                right = left;
            else if (adaptNumericLiteral(expr->operands[0], right))
                left = right;
        }
        if (isComparisonOp(expr->op)) {
            if (!sameType(left, right))
                report(expr->span, "comparison between incompatible types", Err::TypeMismatch);
            return boolType();
        }
        if (isShiftOp(expr->op) || isBitwiseOp(expr->op)) {
            if (!info_.types.isInteger(left) || !info_.types.isInteger(right)) {
                report(expr->span, "shift/bitwise operator expects integer operands",
                       Err::TypeMismatch);
                return errorType();
            }
            if (!sameType(left, right)) {
                report(expr->span, "shift/bitwise operands have incompatible types",
                       Err::TypeMismatch);
                return errorType();
            }
            return left;
        }
        if (isArithmeticOp(expr->op)) {
            if (!info_.types.isNumeric(left) || !info_.types.isNumeric(right)) {
                report(expr->span, "arithmetic operator expects numeric operands",
                       Err::TypeMismatch);
                return errorType();
            }
            if (!sameType(left, right)) {
                report(expr->span, "arithmetic between incompatible types", Err::TypeMismatch);
                return errorType();
            }
            return left;
        }
        if (expr->op == "&&" || expr->op == "||") {
            if (!sameType(left, boolType()) || !sameType(right, boolType()))
                report(expr->span, "logical operator expects boolean operands",
                       Err::TypeMismatch);
            return boolType();
        }
        report(expr->span, "unsupported binary operator '" + std::string(expr->op) + "'",
               Err::UnsupportedSyntax);
        return errorType();
    }

    TypeId inferAssign(Expr *expr) {
        if (expr->operands.size() < 2)
            return errorType();
        TypeId left = inferExpr(static_cast<Expr *>(expr->operands[0]));
        TypeId right = inferExpr(static_cast<Expr *>(expr->operands[1]));
        if (info_.types.resolve(left) == invalidType())
            left = right;
        checkAssignableOwnership(expr->operands[0], expr->span);
        if (!coerceValue(expr->operands[1], left, right)) {
            reportCoercionFailure(expr->span, left, right,
                                  "assignment between incompatible types");
            return errorType();
        }
        return left;
    }

    void checkAssignableOwnership(const AstNode *target, Span span) {
        const AstNode *root = assignmentRoot(target);
        if (root == nullptr)
            return;
        const TypeAnnotation *annotation = info_.annotation(root);
        if (annotation == nullptr || annotation->ownership != Ownership::View)
            return;
        std::string name = "value";
        if (root->kind == generated_ast::NodeKind::Expr) {
            const auto *expr = static_cast<const Expr *>(root);
            if (!expr->text.empty())
                name = std::string(expr->text);
        }
        report(span, "cannot write through '" + name + "': a 'view' binding is read-only",
               Err::WriteThroughView);
    }

    [[nodiscard]] const AstNode *assignmentRoot(const AstNode *target) const {
        for (int guard = 0; guard < 64 && target != nullptr; ++guard) {
            if (target->kind != generated_ast::NodeKind::Expr)
                return nullptr;
            const auto *expr = static_cast<const Expr *>(target);
            switch (static_cast<sample::ExprKind>(expr->kind)) {
            case sample::ExprKind::Name:
                return target;
            case sample::ExprKind::Field:
            case sample::ExprKind::Arrow:
            case sample::ExprKind::Index:
                target = expr->operands.empty() ? nullptr : expr->operands[0];
                break;
            case sample::ExprKind::Unary:
                if (expr->op != "*" || expr->operands.empty())
                    return nullptr;
                target = expr->operands[0];
                break;
            default:
                return nullptr;
            }
        }
        return nullptr;
    }

    TypeId inferCall(Expr *expr) {
        if (expr->operands.empty())
            return errorType();
        auto *callee = static_cast<Expr *>(expr->operands[0]);
        if (callee == nullptr)
            return errorType();

        if (callee->kind == static_cast<int>(sample::ExprKind::Field) ||
            callee->kind == static_cast<int>(sample::ExprKind::Arrow)) {
            if (const TypeId method = inferMethodCall(expr, callee); method != kInvalidTypeId)
                return method;
        }

        if (callee->kind == static_cast<int>(sample::ExprKind::Name)) {
            std::vector<Candidate> candidates;
            for (const FunctionInfo &function : functions_) {
                if (function.decl->name != callee->text ||
                    !function.decl->ownerName.empty())
                    continue;
                if (functionType(function.type) != nullptr)
                    candidates.push_back(Candidate{function.decl, function.type});
            }
            if (!candidates.empty()) {
                const Candidate *chosen = selectOverload(candidates, expr);
                if (chosen == nullptr)
                    return errorType();
                const TypeDesc *signature = functionType(chosen->type);
                if (signature == nullptr)
                    return errorType();
                info_.set(callee, TypeAnnotation{chosen->type, Ownership::Default, false});
                const std::size_t written = expr->operands.size() - 1;
                const std::size_t params =
                    signature->components != nullptr ? signature->components->size() : 0;
                if (written != params) {
                    report(expr->span, "function call arity mismatch", Err::NoMatchingFn);
                    return signature->inner;
                }
                for (std::size_t index = 0; index < written; ++index) {
                    const TypeId argType = inferExpr(static_cast<Expr *>(expr->operands[index + 1]));
                    const TypeId paramType = (*signature->components)[index];
                    if (!coerceValue(expr->operands[index + 1], paramType, argType))
                        reportCoercionFailure(expr->span, paramType, argType,
                                              "function call argument type mismatch");
                }
                return signature->inner;
            }
        }

        const TypeId calleeType = inferExpr(callee);
        const TypeDesc *signature = functionType(calleeType);
        if (signature == nullptr) {
            report(expr->span, "callee is not a function", Err::NoMatchingFn);
            return errorType();
        }
        const std::size_t written = expr->operands.size() - 1;
        const std::size_t params =
            signature->components != nullptr ? signature->components->size() : 0;
        if (written != params) {
            report(expr->span, "function call arity mismatch", Err::NoMatchingFn);
            return signature->inner;
        }
        for (std::size_t index = 0; index < written; ++index) {
            const TypeId argType = inferExpr(static_cast<Expr *>(expr->operands[index + 1]));
            const TypeId paramType = (*signature->components)[index];
            if (!coerceValue(expr->operands[index + 1], paramType, argType))
                reportCoercionFailure(expr->span, paramType, argType,
                                      "function call argument type mismatch");
        }
        return signature->inner;
    }

    const Candidate *selectOverload(const std::vector<Candidate> &candidates,
                                    const Expr *call) {
        std::vector<const Candidate *> viable;
        const std::size_t written = call->operands.size() - 1;
        for (const Candidate &candidate : candidates) {
            const TypeDesc *signature = functionType(candidate.type);
            if (signature == nullptr)
                continue;
            const std::size_t params =
                signature->components != nullptr ? signature->components->size() : 0;
            if (params != written)
                continue;
            bool fits = true;
            for (std::size_t index = 0; index < written && fits; ++index) {
                const TypeId argType = inferExpr(static_cast<Expr *>(call->operands[index + 1]));
                const TypeId paramType = (*signature->components)[index];
                fits = coercesTo(paramType, argType) ||
                       literalAdaptsTo(call->operands[index + 1], paramType);
            }
            if (fits)
                viable.push_back(&candidate);
        }
        if (viable.empty()) {
            report(call->span, "no overload of this function accepts the given arguments",
                   Err::NoMatchingFn);
            return nullptr;
        }
        if (viable.size() > 1) {
            report(call->span, "call is ambiguous between several overloads", Err::AmbiguousCall);
            return nullptr;
        }
        return viable.front();
    }

    bool literalAdaptsTo(const AstNode *value, TypeId target) const {
        if (value == nullptr || value->kind != generated_ast::NodeKind::Expr)
            return false;
        const auto *expr = static_cast<const Expr *>(value);
        if (expr->kind == static_cast<int>(sample::ExprKind::Unary) && expr->op == "-" &&
            !expr->operands.empty())
            return literalAdaptsTo(expr->operands[0], target);
        if (expr->kind != static_cast<int>(sample::ExprKind::Literal))
            return false;
        const bool integerLiteral = looksInteger(expr->text);
        const bool floatLiteral = looksFloat(expr->text);
        if (!integerLiteral && !floatLiteral)
            return false;
        const TypeKind targetKind = info_.types.kindOf(target);
        if (targetKind == TypeKind::Integer)
            return integerLiteral;
        if (targetKind == TypeKind::Float)
            return floatLiteral;
        return false;
    }

    TypeId inferMethodCall(Expr *call, Expr *callee) {
        if (callee->operands.empty())
            return kInvalidTypeId;
        const TypeId base = inferExpr(static_cast<Expr *>(callee->operands[0]));
        TypeId owner = info_.types.resolve(base);
        const TypeDesc *desc = info_.types.find(owner);
        if (desc != nullptr && desc->kind == TypeKind::Pointer)
            owner = info_.types.resolve(desc->inner);
        else if (desc != nullptr && desc->kind == TypeKind::Optional)
            owner = info_.types.resolve(desc->inner);
        const TypeDesc *ownerDesc = info_.types.find(owner);
        if (ownerDesc == nullptr ||
            (ownerDesc->kind != TypeKind::Struct && ownerDesc->kind != TypeKind::Enum))
            return kInvalidTypeId;

        std::vector<const Declaration *> methods;
        for (const FunctionInfo &function : functions_) {
            if (!function.decl->ownerName.empty() &&
                function.decl->ownerName == ownerDesc->name &&
                function.decl->name == callee->text)
                methods.push_back(function.decl);
        }
        if (methods.empty())
            return kInvalidTypeId;

        const Declaration *method = methods.front();
        DynArray<TypeId> &params = info_.types.makeTypeStorage();
        for (AstNode *node : method->parameters) {
            auto *param = static_cast<Parameter *>(node);
            params.push(param->type != nullptr
                            ? lowerTypeExpr(static_cast<const TypeExpr *>(param->type))
                            : errorType());
        }
        const TypeId result = method->declaredType != nullptr
                                  ? lowerTypeExpr(static_cast<const TypeExpr *>(method->declaredType))
                                  : voidType();
        const bool hasSelf = !params.empty() && method->parameters[0] != nullptr &&
                             static_cast<Parameter *>(method->parameters[0])->name == "self";
        if (hasSelf && !params.empty() &&
            static_cast<Parameter *>(method->parameters[0])->type == nullptr)
            params[0] = info_.types.internPointer(owner);
        const TypeId methodType = info_.types.internFunction(params, result);
        info_.set(callee, TypeAnnotation{methodType, Ownership::Default, false});

        const std::size_t provided = call->operands.size() - 1;
        const std::size_t expected = hasSelf ? params.size() - 1 : params.size();
        if (provided != expected) {
            report(call->span, "method call arity mismatch", Err::NoMatchingFn);
            return errorType();
        }
        const std::size_t firstParam = hasSelf ? 1 : 0;
        for (std::size_t index = 0; index < provided; ++index) {
            const TypeId argType = inferExpr(static_cast<Expr *>(call->operands[index + 1]));
            const TypeId paramType =
                firstParam + index < params.size() ? params[firstParam + index] : errorType();
            if (!coerceValue(call->operands[index + 1], paramType, argType))
                reportCoercionFailure(call->span, paramType, argType,
                                      "method call argument type mismatch");
        }
        return result;
    }

    TypeId inferBlock(Expr *expr) {
        TypeId last = voidType();
        for (AstNode *node : expr->statements) {
            if (node == nullptr)
                continue;
            auto *stmt = static_cast<Stmt *>(node);
            switch (static_cast<sample::StmtKind>(stmt->kind)) {
            case sample::StmtKind::Expression:
                if (stmt->expression != nullptr)
                    last = inferExpr(static_cast<Expr *>(stmt->expression));
                break;
            case sample::StmtKind::Binding:
                inferBinding(static_cast<Binding *>(stmt->binding));
                last = voidType();
                break;
            case sample::StmtKind::Return:
                checkReturnStatement(stmt);
                last = voidType();
                break;
            case sample::StmtKind::Break:
            case sample::StmtKind::Continue:
                break;
            case sample::StmtKind::Enter:
            case sample::StmtKind::Leave:
            case sample::StmtKind::Jump:
                report(stmt->span, "flow statements require flow execution lowering",
                       Err::UnsupportedSyntax);
                break;
            default:
                report(stmt->span, "unsupported statement syntax", Err::UnsupportedSyntax);
                break;
            }
        }
        return last;
    }

    void inferBinding(Binding *binding) {
        if (binding == nullptr) {
            report(Span{0, 0}, "binding requires a name", Err::UnsupportedSyntax);
            return;
        }
        if (binding->name.empty()) {
            report(binding->span, "binding requires a name", Err::UnsupportedSyntax);
            return;
        }
        if (scopes_.containsInCurrent(binding->name)) {
            report(binding->span, "duplicate local '" + std::string(binding->name) + "'",
                   Err::DuplicateDecl);
            return;
        }
        const TypeId initType = binding->initializer != nullptr
                                    ? inferExpr(static_cast<Expr *>(binding->initializer))
                                    : invalidType();
        const TypeExpr *annotationType =
            binding->type != nullptr ? static_cast<const TypeExpr *>(binding->type) : nullptr;
        const TypeId annotation =
            annotationType != nullptr ? lowerTypeExpr(annotationType) : invalidType();
        const Ownership ownership =
            annotationType != nullptr ? ownershipFromTypeExpr(annotationType)
                                      : Ownership::Default;
        if (annotation != invalidType() && initType != invalidType() &&
            !coerceValue(binding->initializer, annotation, initType)) {
            reportCoercionFailure(binding->span, annotation, initType,
                                  "binding initializer type does not match annotation");
        }
        TypeId finalType = annotation != invalidType() ? annotation : initType;
        if (binding->type == nullptr && info_.types.resolve(initType) == info_.types.null()) {
            report(binding->span, "null requires an optional type annotation", Err::TypeMismatch);
            finalType = errorType();
        } else if (finalType == invalidType()) {
            report(binding->span,
                   "cannot infer type of '" + std::string(binding->name) +
                       "'; assign a value before reading it or add a type annotation",
                   Err::CannotInfer);
            finalType = errorType();
        }
        scopes_.declare(binding->name, Local{
                                           binding,
                                           binding->name,
                                           finalType,
                                           ownership,
                                           binding->isMutable,
                                           ownership == Ownership::View,
                                       });
        info_.set(binding, TypeAnnotation{finalType, ownership, binding->isMutable});
    }

    void checkReturnStatement(Stmt *stmt) {
        if (stmt->expression == nullptr) {
            if (currentReturnType_ != kInvalidTypeId &&
                info_.types.resolve(currentReturnType_) != voidType() &&
                info_.types.resolve(currentReturnType_) != errorType())
                report(stmt->span,
                       "return without a value in a function with a declared return type",
                       Err::TypeMismatch);
            return;
        }
        const TypeId value = inferExpr(static_cast<Expr *>(stmt->expression));
        if (currentReturnType_ == kInvalidTypeId || value == kInvalidTypeId ||
            value == errorType())
            return;
        if (!coerceValue(stmt->expression, currentReturnType_, value))
            reportCoercionFailure(stmt->span, currentReturnType_, value,
                                  "return type does not match declared return type");
    }

    TypeId inferIf(Expr *expr) {
        TypeId condition = boolType();
        if (!expr->conditions.empty())
            condition = inferExpr(static_cast<Expr *>(expr->conditions[0]));
        if (!sameType(condition, boolType()))
            report(expr->span, "if condition must be boolean", Err::TypeMismatch);
        TypeId thenType = errorType();
        TypeId elseType = voidType();
        if (!expr->statements.empty())
            thenType = inferExpr(static_cast<Expr *>(expr->statements[0]));
        if (expr->alternate != nullptr)
            elseType = inferExpr(static_cast<Expr *>(expr->alternate));
        else if (!expr->operands.empty())
            elseType = inferExpr(static_cast<Expr *>(expr->operands[0]));
        return sameType(thenType, elseType) ? thenType : thenType;
    }

    TypeId inferFor(Expr *expr) {
        if (!expr->conditions.empty()) {
            const TypeId condition = inferExpr(static_cast<Expr *>(expr->conditions[0]));
            if (!sameType(condition, boolType()))
                report(expr->span, "loop condition must be boolean", Err::TypeMismatch);
        }
        for (AstNode *node : expr->operands) {
            if (node == nullptr)
                continue;
            if (node->kind == generated_ast::NodeKind::Stmt) {
                const auto *stmt = static_cast<const Stmt *>(node);
                if (stmt->expression != nullptr)
                    (void)inferExpr(static_cast<Expr *>(stmt->expression));
            } else {
                (void)inferExpr(static_cast<Expr *>(node));
            }
        }
        if (!expr->statements.empty())
            (void)inferExpr(static_cast<Expr *>(expr->statements[0]));
        return voidType();
    }

    TypeId inferOptionalProp(Expr *expr) {
        if (expr->operands.empty())
            return errorType();
        const TypeId operand = inferExpr(static_cast<Expr *>(expr->operands[0]));
        const TypeDesc *desc = info_.types.find(info_.types.resolve(operand));
        if (desc == nullptr || desc->kind != TypeKind::Optional) {
            report(expr->span, "'?' operator requires an optional operand", Err::TypeMismatch);
            return errorType();
        }
        if (currentReturnType_ != kInvalidTypeId) {
            const TypeDesc *ret = info_.types.find(info_.types.resolve(currentReturnType_));
            if (ret == nullptr || ret->kind != TypeKind::Optional)
                report(expr->span,
                       "'?' operator used in a function that does not return an optional",
                       Err::TypeMismatch);
        }
        return desc->inner;
    }

    TypeId inferIndex(Expr *expr) {
        if (expr->operands.size() < 2)
            return errorType();
        const TypeId object = inferExpr(static_cast<Expr *>(expr->operands[0]));
        const TypeId index = inferExpr(static_cast<Expr *>(expr->operands[1]));
        if (!info_.types.isInteger(index))
            report(expr->span, "array index must be an integer", Err::TypeMismatch);
        const TypeDesc *desc = info_.types.find(info_.types.resolve(object));
        if (desc == nullptr || desc->kind == TypeKind::Error)
            return errorType();
        switch (desc->kind) {
        case TypeKind::Array:
        case TypeKind::Slice:
        case TypeKind::Pointer:
            return desc->inner;
        default:
            report(expr->span, "type is not indexable", Err::TypeMismatch);
            return errorType();
        }
    }

    TypeId inferField(Expr *expr) {
        if (expr->operands.empty())
            return errorType();
        if (const TypeId enumVariant = enumVariantType(expr->operands[0], expr->text);
            enumVariant != kInvalidTypeId)
            return enumVariant;
        const TypeId object = inferExpr(static_cast<Expr *>(expr->operands[0]));
        const TypeDesc *desc = info_.types.find(info_.types.resolve(object));
        if (desc == nullptr || desc->kind != TypeKind::Struct) {
            report(expr->span,
                   "field access on non-struct type having type '" +
                       info_.types.toString(object) + "'",
                   Err::TypeMismatch);
            return errorType();
        }
        const int index = info_.types.fieldIndex(desc->id, expr->text);
        if (index < 0) {
            report(expr->span, "unknown field '" + std::string(expr->text) + "' on type '" +
                                   info_.types.toString(object) + "'",
                   Err::NoMember);
            return errorType();
        }
        return desc->components->at(static_cast<std::size_t>(index));
    }

    [[nodiscard]] TypeId enumVariantType(const AstNode *operand,
                                         std::string_view variant) {
        if (operand == nullptr || operand->kind != generated_ast::NodeKind::Expr)
            return kInvalidTypeId;
        const auto *nameExpr = static_cast<const Expr *>(operand);
        if (nameExpr->kind != static_cast<int>(sample::ExprKind::Name))
            return kInvalidTypeId;
        const TypeId nameType = inferExpr(const_cast<Expr *>(nameExpr));
        const TypeDesc *desc = info_.types.find(info_.types.resolve(nameType));
        if (desc == nullptr || desc->kind != TypeKind::Enum || desc->names == nullptr)
            return kInvalidTypeId;
        for (std::string_view existing : *desc->names) {
            if (existing == variant)
                return desc->id;
        }
        report(nameExpr->span, "unknown enum variant '" + std::string(variant) + "'",
               Err::NoMember);
        return kInvalidTypeId;
    }

    TypeId inferArrow(Expr *expr) {
        if (expr->operands.empty())
            return errorType();
        const TypeId pointer = inferExpr(static_cast<Expr *>(expr->operands[0]));
        const TypeDesc *desc = info_.types.find(info_.types.resolve(pointer));
        TypeId pointee = kInvalidTypeId;
        if (desc != nullptr && desc->kind == TypeKind::Pointer)
            pointee = desc->inner;
        else if (desc != nullptr && desc->kind == TypeKind::Optional) {
            const TypeDesc *inner = info_.types.find(info_.types.resolve(desc->inner));
            if (inner != nullptr && inner->kind == TypeKind::Pointer)
                pointee = inner->inner;
        }
        const TypeDesc *structDesc = info_.types.find(info_.types.resolve(pointee));
        if (pointee == kInvalidTypeId || structDesc == nullptr ||
            structDesc->kind != TypeKind::Struct) {
            report(expr->span, "'->' requires a pointer to a struct type", Err::TypeMismatch);
            return errorType();
        }
        const int index = info_.types.fieldIndex(structDesc->id, expr->text);
        if (index < 0) {
            report(expr->span, "unknown field '" + std::string(expr->text) + "' on type '" +
                                   info_.types.toString(pointer) + "'",
                   Err::NoMember);
            return errorType();
        }
        return structDesc->components->at(static_cast<std::size_t>(index));
    }

    TypeId inferCast(Expr *expr) {
        if (expr->operands.empty() || expr->castType == nullptr)
            return errorType();
        const TypeId source = inferExpr(static_cast<Expr *>(expr->operands[0]));
        const TypeId target = lowerTypeExpr(static_cast<const TypeExpr *>(expr->castType));
        if (target == errorType()) {
            report(expr->span, "unknown target type in 'as' conversion", Err::TypeMismatch);
            return errorType();
        }
        const TypeKind fromKind = info_.types.kindOf(source);
        const TypeKind toKind = info_.types.kindOf(target);
        const bool fromNumeric = fromKind == TypeKind::Integer || fromKind == TypeKind::Char ||
                                 fromKind == TypeKind::Float;
        const bool toNumeric = toKind == TypeKind::Integer || toKind == TypeKind::Char ||
                               toKind == TypeKind::Float;
        if (fromNumeric && toNumeric)
            return target;
        if (fromKind == TypeKind::Pointer && toKind == TypeKind::Pointer)
            return target;
        if (fromKind == TypeKind::Opaque && toKind == TypeKind::Pointer)
            return target;
        if (fromKind == TypeKind::Pointer && toKind == TypeKind::Opaque)
            return target;
        report(expr->span,
               "'as' supports numeric conversions and 'raw opaque' pointer conversions",
               Err::InvalidCast);
        return errorType();
    }

    TypeId inferStructLiteral(Expr *expr) {
        const TypeId type = info_.types.lookupNamed(expr->text);
        if (type == kInvalidTypeId) {
            report(expr->span, "unknown struct type '" + std::string(expr->text) + "'",
                   Err::UndefinedIdent);
            return errorType();
        }
        const TypeDesc *desc = info_.types.find(info_.types.resolve(type));
        if (desc == nullptr || desc->kind != TypeKind::Struct || desc->components == nullptr ||
            desc->names == nullptr) {
            report(expr->span, "'" + std::string(expr->text) + "' is not a struct type",
                   Err::TypeMismatch);
            return errorType();
        }
        const DynArray<TypeId> &fields = *desc->components;
        const DynArray<std::string_view> &names = *desc->names;
        std::vector<bool> seen(fields.size(), false);
        const bool named = !expr->fieldNames.empty();
        if (!named) {
            for (std::size_t index = 0; index < expr->operands.size(); ++index) {
                if (index >= fields.size()) {
                    report(expr->span,
                           "too many fields in struct literal for '" +
                               std::string(expr->text) + "'",
                           Err::TypeMismatch);
                    continue;
                }
                const int fieldIndex = static_cast<int>(index);
                if (seen[static_cast<std::size_t>(fieldIndex)]) {
                    report(expr->span, "duplicate field in struct literal",
                           Err::TypeMismatch);
                    continue;
                }
                seen[static_cast<std::size_t>(fieldIndex)] = true;
                const TypeId valueType =
                    inferExpr(static_cast<Expr *>(expr->operands[index]));
                if (!coerceValue(expr->operands[index],
                                 fields[fieldIndex], valueType))
                    reportCoercionFailure(
                        expr->span, fields[fieldIndex], valueType,
                        "struct literal field type mismatch");
            }
            return desc->id;
        }

        for (AstNode *fieldNode : expr->fieldNames) {
            auto *field = static_cast<ExprField *>(fieldNode);
            const int fieldIndex = info_.types.fieldIndex(desc->id, field->name);
            if (fieldIndex < 0) {
                report(expr->span,
                       "unknown field '" + std::string(field->name) +
                           "' in struct '" + std::string(expr->text) + "'",
                       Err::NoMember);
                continue;
            }
            if (seen[static_cast<std::size_t>(fieldIndex)]) {
                report(expr->span, "duplicate field in struct literal",
                       Err::TypeMismatch);
                continue;
            }
            seen[static_cast<std::size_t>(fieldIndex)] = true;
            if (field->value == nullptr)
                continue;
            const TypeId valueType =
                inferExpr(static_cast<Expr *>(field->value));
            if (!coerceValue(field->value, fields[fieldIndex], valueType))
                reportCoercionFailure(expr->span, fields[fieldIndex],
                                      valueType,
                                      "struct literal field type mismatch");
        }
        return desc->id;
    }

    TypeId inferArrayLiteral(Expr *expr) {
        if (expr->operands.empty())
            return info_.types.internArray(i32Type(), 0);
        TypeId elementType = errorType();
        for (AstNode *node : expr->operands) {
            const TypeId candidate = inferExpr(static_cast<Expr *>(node));
            if (elementType == errorType()) {
                elementType = candidate;
                continue;
            }
            if (!sameType(elementType, candidate)) {
                if (adaptNumericLiteral(node, elementType))
                    continue;
                report(expr->span, "array literal element types do not match",
                       Err::TypeMismatch);
                return errorType();
            }
        }
        return info_.types.internArray(elementType, expr->operands.size());
    }

    TypeId inferWhen(Expr *expr) {
        const TypeId subject =
            expr->conditions.empty() ? errorType()
                                      : inferExpr(static_cast<Expr *>(expr->conditions[0]));
        const std::size_t caseCount = expr->cases.size() / 2;
        TypeId bodyType = voidType();
        bool hasDefault = false;
        for (std::size_t index = 0; index < caseCount; ++index) {
            Expr *condition =
                static_cast<Expr *>(expr->cases[index * 2]);
            Expr *body = static_cast<Expr *>(expr->cases[index * 2 + 1]);
            if (condition == nullptr || body == nullptr)
                continue;
            if (condition->kind == static_cast<int>(sample::ExprKind::Placeholder)) {
                hasDefault = true;
                if (index + 1 != caseCount)
                    report(expr->span,
                           "a default when case ('_') must be the last case",
                           Err::TypeMismatch);
            } else {
                const TypeId condType = inferExpr(condition);
                if (!sameType(condType, boolType()) && condType != errorType() &&
                    !sameType(subject, condType) &&
                    !adaptNumericLiteral(condition, subject))
                    report(expr->span,
                           "when case condition must be a boolean expression or match the subject type",
                           Err::TypeMismatch);
            }
            const TypeId caseType = inferExpr(body);
            if (index == 0)
                bodyType = caseType;
            else if (!sameType(bodyType, caseType) && caseType != errorType())
                report(expr->span, "when case bodies must all have the same type",
                       Err::TypeMismatch);
        }
        if (expr->alternate != nullptr) {
            hasDefault = true;
            const TypeId defaultType = inferExpr(static_cast<Expr *>(expr->alternate));
            if (caseCount == 0)
                bodyType = defaultType;
            else if (!sameType(bodyType, defaultType) && defaultType != errorType())
                report(expr->span, "when case bodies must all have the same type",
                       Err::TypeMismatch);
        }
        if (bodyType != voidType() && bodyType != errorType() && !hasDefault)
            report(expr->span, "non-exhaustive when; add a default case '(_) ~> ...'",
                   Err::TypeMismatch);
        return bodyType;
    }

    TypeId inferRange(Expr *expr) {
        if (expr->operands.size() != 2)
            return errorType();
        const TypeId low = inferExpr(static_cast<Expr *>(expr->operands[0]));
        const TypeId high = inferExpr(static_cast<Expr *>(expr->operands[1]));
        if (!sameType(low, high)) {
            if (!adaptNumericLiteral(expr->operands[1], low))
                report(expr->span, "range pattern bounds must have the same type",
                       Err::TypeMismatch);
        }
        return boolType();
    }

    TypeId inferIsNull(Expr *expr) {
        if (expr->operands.empty())
            return errorType();
        const TypeId operand = inferExpr(static_cast<Expr *>(expr->operands[0]));
        const TypeDesc *desc = info_.types.find(info_.types.resolve(operand));
        if (desc == nullptr || desc->kind != TypeKind::Optional) {
            report(expr->span, "'is null' requires an optional operand ('?T')",
                   Err::TypeMismatch);
            return errorType();
        }
        return boolType();
    }

    TypeId inferLayoutIntrinsic(Expr *expr) {
        if (expr->castType == nullptr)
            return errorType();
        const TypeId target = lowerTypeExpr(static_cast<const TypeExpr *>(expr->castType));
        const TypeDesc *desc = info_.types.find(info_.types.resolve(target));
        if (expr->op == "sizeOf") {
            if (desc == nullptr || desc->kind == TypeKind::Void) {
                report(expr->span, "'@sizeOf' requires a complete type", Err::TypeMismatch);
                return errorType();
            }
            return info_.types.u64Type();
        }
        if (desc == nullptr || desc->kind != TypeKind::Struct) {
            report(expr->span, "'@" + std::string(expr->op) + "' requires a struct type",
                   Err::TypeMismatch);
            return errorType();
        }
        if (expr->op == "offsetOf") {
            if (expr->fieldNames.empty()) {
                report(expr->span, "'@offsetOf' requires a field name", Err::TypeMismatch);
                return errorType();
            }
            const auto *field = static_cast<const ExprField *>(expr->fieldNames[0]);
            if (info_.types.fieldIndex(desc->id, field->name) < 0) {
                report(expr->span, "unknown field '" + std::string(field->name) + "'",
                       Err::NoMember);
                return errorType();
            }
        }
        return i32Type();
    }

    bool adaptNumericLiteral(AstNode *value, TypeId target) {
        if (value == nullptr)
            return false;
        auto *expr = static_cast<Expr *>(value);
        if (expr->kind == static_cast<int>(sample::ExprKind::Unary) && expr->op == "-" &&
            !expr->operands.empty()) {
            if (!adaptNumericLiteral(expr->operands[0], target))
                return false;
            info_.set(expr, TypeAnnotation{target, Ownership::Default, false});
            return true;
        }
        if (expr->kind != static_cast<int>(sample::ExprKind::Literal))
            return false;
        const bool integerLiteral = looksInteger(expr->text);
        const bool floatLiteral = looksFloat(expr->text);
        if (!integerLiteral && !floatLiteral)
            return false;
        const TypeKind targetKind = info_.types.kindOf(target);
        if (targetKind == TypeKind::Integer && !integerLiteral)
            return false;
        if (floatLiteral && targetKind != TypeKind::Float)
            return false;
        if (integerLiteral && targetKind != TypeKind::Integer &&
            targetKind != TypeKind::Float)
            return false;
        info_.set(expr, TypeAnnotation{target, Ownership::Default, false});
        return true;
    }

    bool coerceValue(AstNode *value, TypeId target, TypeId source) {
        if (coercesTo(target, source)) {
            if (info_.types.resolve(source) == info_.types.null() &&
                info_.types.kindOf(target) == TypeKind::Optional)
                info_.set(value, TypeAnnotation{target, Ownership::Default, false});
            return true;
        }
        return adaptNumericLiteral(value, target);
    }

    [[nodiscard]] bool coercesTo(TypeId target, TypeId source) const noexcept {
        if (sameType(target, source))
            return true;
        const TypeId resolvedTarget = info_.types.resolve(target);
        const TypeDesc *targetDesc = info_.types.find(resolvedTarget);
        if (targetDesc != nullptr && targetDesc->kind == TypeKind::Optional) {
            if (info_.types.resolve(source) == info_.types.null())
                return true;
            return sameType(targetDesc->inner, source);
        }
        // `?*T` sources are accepted wherever `*T` is expected until
        // flow-sensitive nullability refinement lands.
        const TypeDesc *sourceDesc = info_.types.find(info_.types.resolve(source));
        if (sourceDesc != nullptr && sourceDesc->kind == TypeKind::Optional &&
            targetDesc != nullptr && targetDesc->kind == TypeKind::Pointer) {
            const TypeDesc *inner = info_.types.find(info_.types.resolve(sourceDesc->inner));
            return inner != nullptr && inner->kind == TypeKind::Pointer &&
                   sameType(target, inner->inner);
        }
        return false;
    }

    [[nodiscard]] bool sameType(TypeId left, TypeId right) const noexcept {
        if (left == right)
            return true;
        const TypeId resolvedLeft = info_.types.resolve(left);
        const TypeId resolvedRight = info_.types.resolve(right);
        if (resolvedLeft == resolvedRight)
            return true;
        const TypeKind leftKind = info_.types.kindOf(resolvedLeft);
        const TypeKind rightKind = info_.types.kindOf(resolvedRight);
        if (resolvedLeft == info_.types.error() || resolvedRight == info_.types.error())
            return true;
        if (resolvedLeft == info_.types.null() || resolvedRight == info_.types.null())
            return leftKind == TypeKind::Optional || rightKind == TypeKind::Optional;
        if (leftKind != rightKind)
            return false;
        if (leftKind == TypeKind::Integer || leftKind == TypeKind::Float) {
            const TypeDesc *leftDesc = info_.types.find(resolvedLeft);
            const TypeDesc *rightDesc = info_.types.find(resolvedRight);
            return leftDesc != nullptr && rightDesc != nullptr &&
                   leftDesc->bits == rightDesc->bits &&
                   leftDesc->isSigned == rightDesc->isSigned;
        }
        if (leftKind == TypeKind::Pointer || leftKind == TypeKind::Optional ||
            leftKind == TypeKind::Slice || leftKind == TypeKind::Array) {
            const TypeDesc *leftDesc = info_.types.find(resolvedLeft);
            const TypeDesc *rightDesc = info_.types.find(resolvedRight);
            if (leftDesc == nullptr || rightDesc == nullptr)
                return false;
            if (leftKind == TypeKind::Array && leftDesc->length != rightDesc->length)
                return false;
            return leftDesc->inner == rightDesc->inner ||
                   sameType(leftDesc->inner, rightDesc->inner);
        }
        if (leftKind == TypeKind::Function) {
            const TypeDesc *leftDesc = info_.types.find(resolvedLeft);
            const TypeDesc *rightDesc = info_.types.find(resolvedRight);
            if (leftDesc == nullptr || rightDesc == nullptr ||
                leftDesc->components == nullptr || rightDesc->components == nullptr ||
                leftDesc->components->size() != rightDesc->components->size() ||
                !sameType(leftDesc->inner, rightDesc->inner))
                return false;
            for (std::size_t index = 0; index < leftDesc->components->size(); ++index) {
                if (!sameType((*leftDesc->components)[index],
                              (*rightDesc->components)[index]))
                    return false;
            }
            return true;
        }
        if (leftKind == TypeKind::Struct || leftKind == TypeKind::Enum ||
            leftKind == TypeKind::Union || leftKind == TypeKind::Trait) {
            const TypeDesc *leftDesc = info_.types.find(resolvedLeft);
            const TypeDesc *rightDesc = info_.types.find(resolvedRight);
            return leftDesc != nullptr && rightDesc != nullptr &&
                   leftDesc->name == rightDesc->name;
        }
        if (leftKind == TypeKind::Void || leftKind == TypeKind::Bool ||
            leftKind == TypeKind::Char || leftKind == TypeKind::String)
            return true;
        return false;
    }

    void reportCoercionFailure(Span span, TypeId target, TypeId source,
                               std::string_view context) {
        if (info_.types.resolve(source) == info_.types.null()) {
            report(span, "cannot assign 'null' to a non-optional pointer; use '?*T'",
                   Err::TypeMismatch);
            return;
        }
        report(span,
               std::string(context) + ": expected '" + info_.types.toString(target) +
                   "', has type '" + info_.types.toString(source) + "'",
               Err::TypeMismatch);
    }

};

} // namespace toolkit::sema

const TypeAnnotation *TypeCheckedInfo::annotation(
    const generated_ast::AstNode *node) const noexcept {
    if (node == nullptr)
        return nullptr;
    return annotations_.get(node);
}

TypeId TypeCheckedInfo::typeOf(
    const generated_ast::AstNode *node) const noexcept {
    const TypeAnnotation *value = annotation(node);
    return value != nullptr ? value->type : kInvalidTypeId;
}

Ownership TypeCheckedInfo::ownershipOf(
    const generated_ast::AstNode *node) const noexcept {
    const TypeAnnotation *value = annotation(node);
    return value != nullptr ? value->ownership : Ownership::Default;
}

bool TypeCheckedInfo::isMutOf(
    const generated_ast::AstNode *node) const noexcept {
    const TypeAnnotation *value = annotation(node);
    return value != nullptr && value->isMut;
}

void TypeCheckedInfo::set(const generated_ast::AstNode *node,
                          TypeAnnotation annotation) {
    if (node != nullptr)
        annotations_.insert(node, annotation);
}

Ownership ownershipFromTypeExpr(const generated_ast::TypeExpr *type) noexcept {
    if (type == nullptr)
        return Ownership::Default;
    switch (static_cast<Ownership>(type->ownership)) {
    case Ownership::Lend:
    case Ownership::Share:
    case Ownership::View:
    case Ownership::Unique:
    case Ownership::Belong:
        return static_cast<Ownership>(type->ownership);
    default:
        return Ownership::Default;
    }
}

bool typeCheckProgram(
    generated_ast::Program &program,
    common::memory::FileId fileId,
    common::memory::Arena &arena,
    common::memory::StringInterner &interner,
    common::memory::DynArray<common::diagnostic::Diagnostic> &diagnostics,
    TypeCheckedInfo &info) {
    TypeChecker checker(program, fileId, arena, interner, diagnostics, info);
    return checker.run();
}

TypeCheckedInfo &checkedInfo(
    toolkit::session::CompilationSession &session) {
    return session.context().checked;
}

} // namespace toolkit::sema
