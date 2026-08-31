#include "frontend/ast-lowerer.hpp"

#include "diagnostics/error-codes.hpp"
#include "support/int-literal.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zith::frontend {

void AstLowerer::lowerImport(uint32_t start, Visibility visibility) {
    Declaration declaration;
    declaration.id         = DeclId{static_cast<uint32_t>(snapshot_.declarations_.size() + 1U)};
    declaration.kind       = DeclKind::Import;
    declaration.visibility = visibility;
    declaration.import.isExport = text(index_) == "export";
    declaration.import.isFrom   = text(index_) == "from";
    ++index_;
    if (index_ < token_count_ && text(index_) == "asset") {
        declaration.import.isAsset = true;
        ++index_;
    }
    const uint32_t path_start = index_;
    parseImportPath(declaration.import);
    declaration.import.pathSpan = range(path_start, index_);
    if (!declaration.import.isHeader) {
        declaration.import.rawPath = std::string(snapshot_.source_.substr(
            declaration.import.pathSpan.start, declaration.import.pathSpan.size()));
    }
    if (!declaration.import.path.empty() && declaration.import.path.front() == "assets")
        declaration.import.isAsset = true;
    parseImportDepth(declaration.import);
    parseImportSelectors(declaration.import);
    if (index_ < token_count_ && text(index_) == "as") {
        ++index_;
        if (index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Identifier) {
            declaration.import.alias     = std::string(text(index_));
            declaration.import.aliasSpan = tokenSpan(index_++);
        } else {
            snapshot_.diagnostics_.push_back(
                {range(start, index_), "expected an import alias after 'as'"});
        }
    }
    declaration.span = range(start, index_);
    if (declaration.import.path.empty() && !declaration.import.isHeader) {
        snapshot_.diagnostics_.push_back({declaration.span, "expected an import path"});
        declaration.kind = DeclKind::Error;
    } else if (declaration.import.isAsset && declaration.import.alias.empty()) {
        snapshot_.diagnostics_.push_back(
            {declaration.span, "assets import requires an alias using 'as'"});
    }
    if (declaration.import.isHeader) {
        if (declaration.import.headerPath.ends_with(".hpp")) {
            snapshot_.diagnostics_.push_back(
                {declaration.import.pathSpan, "C++ headers are not supported in this version"});
        }
        if (declaration.import.isFrom || declaration.import.isExport ||
            !declaration.import.selectors.empty() || declaration.import.depth != 1) {
            snapshot_.diagnostics_.push_back(
                {declaration.span,
                 "C header imports only support 'import \"header.h\"' and an optional 'as' "
                 "alias"});
        }
    }
    snapshot_.declarations_.push_back(std::move(declaration));
}
void AstLowerer::parseImportPath(ImportDecl &import) {
    if (index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Literal) {
        const auto literal = text(index_);
        if (literal.size() >= 2U && literal.front() == '"' && literal.back() == '"') {
            import.isHeader   = true;
            import.headerPath = std::string(literal.substr(1U, literal.size() - 2U));
            import.rawPath    = import.headerPath;
            import.pathSpans.push_back(tokenSpan(index_++));
            return;
        }
    }
    bool expect_segment = true;
    while (index_ < token_count_) {
        const auto segment = text(index_);
        const auto kind    = snapshot_.tokens_[index_].kind;
        if (kind == TokenKind::Identifier) {
            import.path.emplace_back(segment);
            import.pathSpans.push_back(tokenSpan(index_++));
            expect_segment = false;
            continue;
        }
        if (segment == "." || segment == "/") {
            if (segment == "." && expect_segment && index_ + 1U < token_count_ &&
                text(index_ + 1U) == ".") {
                import.path.emplace_back("..");
                import.pathSpans.push_back(range(index_, index_ + 2U));
                index_ += 2U;
                expect_segment = false;
                continue;
            }
            if (segment == "." && expect_segment) {
                import.path.emplace_back(".");
                import.pathSpans.push_back(tokenSpan(index_++));
                expect_segment = false;
                continue;
            }
            ++index_;
            expect_segment = true;
            continue;
        }
        break;
    }
}
void AstLowerer::parseImportDepth(ImportDecl &import) {
    if (!punctuation(index_, '('))
        return;
    const uint32_t depth_start = index_++;
    if (index_ < token_count_ && text(index_) == "." && index_ + 1U < token_count_ &&
        text(index_ + 1U) == ".") {
        import.depth = -1;
        index_ += 2U;
    } else if (index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Literal) {
        std::int64_t depth = 1;
        if (support::parseIntegerLiteral(text(index_), depth) != support::IntLiteralStatus::Ok ||
            depth < std::numeric_limits<int32_t>::min() ||
            depth > std::numeric_limits<int32_t>::max()) {
            snapshot_.diagnostics_.push_back({tokenSpan(index_), "invalid import depth"});
        } else {
            import.depth = static_cast<int32_t>(depth);
        }
        ++index_;
    } else {
        snapshot_.diagnostics_.push_back({range(depth_start, index_), "expected import depth"});
    }
    if (punctuation(index_, ')'))
        ++index_;
    else
        snapshot_.diagnostics_.push_back(
            {range(depth_start, index_), "expected ')' after import depth"});
}
void AstLowerer::parseImportSelectors(ImportDecl &import) {
    if (!punctuation(index_, '{'))
        return;
    ++index_;
    while (index_ < token_count_ && !punctuation(index_, '}')) {
        if (punctuation(index_, ',')) {
            ++index_;
            continue;
        }
        if (snapshot_.tokens_[index_].kind != TokenKind::Identifier &&
            snapshot_.tokens_[index_].kind != TokenKind::Keyword) {
            snapshot_.diagnostics_.push_back(
                {tokenSpan(index_), "expected symbol name in import selector"});
            ++index_;
            continue;
        }
        ImportSelector selector;
        selector.name = std::string(text(index_));
        selector.span = tokenSpan(index_++);
        if (index_ < token_count_ && text(index_) == "as") {
            ++index_;
            if (index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Identifier) {
                selector.alias     = std::string(text(index_));
                selector.aliasSpan = tokenSpan(index_++);
                selector.span.end  = selector.aliasSpan.end;
            } else {
                snapshot_.diagnostics_.push_back(
                    {selector.span, "expected alias in import selector"});
            }
        }
        import.selectors.push_back(std::move(selector));
        if (punctuation(index_, ','))
            ++index_;
    }
    if (punctuation(index_, '}'))
        ++index_;
    else
        snapshot_.diagnostics_.push_back({import.pathSpan, "expected '}' after import selectors"});
}
void AstLowerer::lowerMacroDeclaration(uint32_t start, Visibility visibility, bool isRaw,
                                       const bool isTag) {
    ++index_; // consume `macro`
    Declaration declaration;
    declaration.kind       = DeclKind::Macro;
    declaration.visibility = visibility;
    declaration.isRawMacro = isRaw;
    declaration.isTagMacro = isTag;
    if (isTag) {
        snapshot_.diagnostics_.push_back(
            {tokenSpan(start), "Zith--: tag macros are not supported; use a normal or raw macro",
             false, diagnostics::err::UnsupportedSyntax});
    }

    if (index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Identifier) {
        declaration.name = std::string(text(index_++));
    } else {
        snapshot_.diagnostics_.push_back({tokenSpan(index_), "expected a macro name"});
        declaration.kind = DeclKind::Error;
    }

    // Parameter list: `(p: identifier, msg: expr, ...)`.
    // Known meta-types: identifier, expr, condition, block, body.
    // The special first parameter `attributes` (no type) signals that the
    // call site may supply `|attrs|` and is not counted toward arity.
    if (punctuation(index_, '(')) {
        ++index_;
        bool first = true;
        while (index_ < token_count_ && !punctuation(index_, ')')) {
            Parameter parameter;
            if (punctuation(index_, ',')) {
                ++index_;
                first = false;
                continue;
            }
            if (snapshot_.tokens_[index_].kind == TokenKind::Identifier) {
                const auto param_name = std::string(text(index_));
                const auto param_span = tokenSpan(index_);
                ++index_;
                // Handle `attributes` — special first param with no type.
                if (first && !punctuation(index_, ':') && param_name == "attributes") {
                    declaration.hasAttributesParam = true;
                } else {
                    parameter.id   = LocalId{statementCountLocals_++};
                    parameter.name = param_name;
                    parameter.span = param_span;
                    if (punctuation(index_, ':')) {
                        ++index_;
                        parameter.type = parseType();
                        // Diagnose unknown meta-type at declaration time.
                        if (parameter.type) {
                            const auto ti = parameter.type.value - 1U;
                            if (ti < snapshot_.type_expressions_.size()) {
                                const auto &te = snapshot_.type_expressions_[ti];
                                if (te.kind == frontend::TypeExprKind::Name &&
                                    te.name != "identifier" && te.name != "expr" &&
                                    te.name != "condition" && te.name != "block" &&
                                    te.name != "body") {
                                    snapshot_.diagnostics_.push_back(
                                        {te.span, "unknown meta-type '" + te.name + "'"});
                                }
                            }
                        }
                    }
                    declaration.parameters.push_back(std::move(parameter));
                }
            } else {
                snapshot_.diagnostics_.push_back({tokenSpan(index_), "expected a parameter name"});
                ++index_;
                first = false;
            }
            if (punctuation(index_, ','))
                ++index_;
            else if (!punctuation(index_, ')'))
                break;
            first = false;
        }
        if (punctuation(index_, ')'))
            ++index_;
        else
            snapshot_.diagnostics_.push_back(
                {range(index_ - 5, index_), "expected ')' after macro parameters"});
    }

    // Body: `{ ... }`
    if (punctuation(index_, '{')) {
        declaration.body = parseBlock();
    } else {
        snapshot_.diagnostics_.push_back({tokenSpan(index_), "expected a body block for macro"});
    }

    if (punctuation(index_, ';'))
        ++index_;
    declaration.span = range(start, index_);
    declaration.id   = DeclId{static_cast<uint32_t>(snapshot_.declarations_.size() + 1U)};
    snapshot_.declarations_.push_back(std::move(declaration));
}
void AstLowerer::lowerImplementBlock(const uint32_t start, const Visibility visibility) {
    ++index_; // consume `implement` or `impl`

    const TypeExprId owner_type = parseType();
    if (!owner_type) {
        snapshot_.diagnostics_.push_back({tokenSpan(index_), "expected a type after 'implement'"});
        return;
    }
    std::string owner_name = canonicalTypeString(snapshot_, owner_type);
    // `implement Box<T>` is accepted but methods still attach to the template
    // name; concrete instances are resolved through explicit receiver types.
    const auto &owner_expr = snapshot_.typeExpressions()[owner_type.value - 1U];
    if (const size_t angle = owner_name.find('<'); angle != std::string::npos)
        owner_name.resize(angle);
    const bool pointer_char_owner =
        owner_expr.kind == frontend::TypeExprKind::Pointer && owner_expr.arguments.size() == 1U &&
        canonicalTypeString(snapshot_, owner_expr.arguments[0]) == "char";
    if ((owner_expr.kind == frontend::TypeExprKind::Pointer && !pointer_char_owner) ||
        owner_expr.kind == frontend::TypeExprKind::Array) {
        snapshot_.diagnostics_.push_back(
            {owner_expr.span,
             "implement targets are primitives, optionals, slices and named types "
             "in this iteration; only the '*char' pointer owner is supported currently",
             false, diagnostics::err::UnsupportedSyntax});
    }

    std::vector<GenericParam> ownerGenericParams;
    if (owner_expr.kind == frontend::TypeExprKind::Name && !owner_expr.arguments.empty()) {
        for (const TypeExprId arg : owner_expr.arguments) {
            const auto &arg_expr = snapshot_.typeExpressions()[arg.value - 1U];
            if (arg_expr.kind != frontend::TypeExprKind::Name)
                continue;
            GenericParam param;
            param.name = arg_expr.name;
            param.span = arg_expr.span;
            ownerGenericParams.push_back(std::move(param));
        }
    }

    std::string trait_name;
    // Optional `as TraitName` or `for TraitName` — parsed but not enforced.
    if (index_ < token_count_ && isKeywordToken("as")) {
        ++index_;
        if (index_ < token_count_ && (snapshot_.tokens_[index_].kind == TokenKind::Identifier ||
                                      snapshot_.tokens_[index_].kind == TokenKind::Keyword)) {
            trait_name = std::string(text(index_));
            ++index_;
        } else {
            snapshot_.diagnostics_.push_back(
                {tokenSpan(index_), "expected a trait name after 'as'/'for'"});
        }
    }

    if (!punctuation(index_, '{')) {
        snapshot_.diagnostics_.push_back(
            {tokenSpan(index_), "expected '{' after implement block header"});
        return;
    }
    if (!trait_name.empty()) {
        bool name_exists      = false;
        bool valid_trait_kind = false;
        for (const auto &known : snapshot_.declarations_) {
            if (known.name != trait_name)
                continue;
            name_exists      = true;
            valid_trait_kind = known.kind == DeclKind::Trait || known.kind == DeclKind::Interface;
            break;
        }
        if (name_exists && !valid_trait_kind) {
            snapshot_.diagnostics_.push_back({range(start, index_),
                                              "'" + trait_name + "' is not a trait", false,
                                              diagnostics::err::NotATrait});
        }
        snapshot_.implement_records_.push_back(
            ImplementRecord{owner_name, owner_type, trait_name, range(start, index_)});
    }
    ++index_;
    Visibility method_visibility = visibility;
    while (index_ < token_count_ && !punctuation(index_, '}')) {
        if (punctuation(index_, ',')) {
            ++index_;
            continue;
        }
        if (isVisibilityPrefix()) {
            const auto visibility_word = text(index_);
            method_visibility = visibility_word == "pub" ? Visibility::Public : Visibility::Module;
            ++index_;
            continue;
        }
        if (const auto function_kind = functionKindPrefix()) {
            const auto method_start = index_;
            lowerDeclaration(method_start, DeclKind::Function, method_visibility, owner_name,
                             trait_name, false, *function_kind, ownerGenericParams);
            method_visibility = visibility;
            continue;
        }
        snapshot_.diagnostics_.push_back(
            {tokenSpan(index_), "expected a method declaration or '}'"});
        ++index_;
    }
    if (punctuation(index_, '}'))
        ++index_;
    else
        snapshot_.diagnostics_.push_back(
            {range(start, index_), "expected '}' after implement block"});
}
DeclId AstLowerer::lowerDeclaration(uint32_t start, DeclKind kind, Visibility visibility,
                                    std::string ownerName, std::string traitName, bool isExtern,
                                    FunctionKind functionKind,
                                    const std::vector<GenericParam> &inheritedParams,
                                    bool isRawUnion, bool suppressTopLevelBindingCheck,
                                    ScopeId parentScope, const std::string &parentName) {
    Declaration declaration;
    declaration.id            = DeclId{static_cast<uint32_t>(snapshot_.declarations_.size() + 1U)};
    declaration.kind          = kind;
    declaration.visibility    = visibility;
    declaration.functionKind  = functionKind;
    declaration.ownerName     = std::move(ownerName);
    declaration.traitName     = std::move(traitName);
    declaration.isExtern      = isExtern;
    declaration.isRawUnion    = isRawUnion;
    declaration.isNominalType = declaration_is_nominal_;
    declaration.parentScope   = parentScope;
    declaration.parentName    = parentName;
    if (kind == DeclKind::Variable)
        declaration.bindingKind = BindingKind::Let;
    if (kind == DeclKind::Function && functionKind == FunctionKind::Extern)
        declaration.isExtern = true;
    if (functionKind == FunctionKind::Const) {
        snapshot_.diagnostics_.push_back(
            {tokenSpan(start), "Zith--: 'const fn' is not supported; declare an ordinary 'fn'",
             false, diagnostics::err::UnsupportedSyntax});
    }
    ++index_;
    if (index_ < token_count_ && snapshot_.tokens_[index_].kind == TokenKind::Identifier) {
        if (kind == DeclKind::Variable)
            declaration.bindingKind = bindingKind(text(index_ - 1U));
        declaration.name = std::string(text(index_++));
    } else {
        snapshot_.diagnostics_.push_back({tokenSpan(start), "expected a declaration name"});
        declaration.kind = DeclKind::Error;
    }
    if (kind == DeclKind::Variable && declaration.bindingKind != BindingKind::Const &&
        !suppressTopLevelBindingCheck) {
        snapshot_.diagnostics_.push_back(
            {range(start, index_),
             "Zith--: top-level variables must be declared with 'const NAME: T = value'; "
             "let/var are local bindings",
             false, diagnostics::err::UnsupportedSyntax});
    }
    // Generic parameter list `<T, U>` (constraints parse but are not enforced).
    if (isOperatorToken("<")) {
        ++index_;
        while (index_ < token_count_ && !isOperatorToken(">")) {
            if (snapshot_.tokens_[index_].kind == TokenKind::Identifier) {
                GenericParam param;
                param.name = std::string(text(index_));
                param.span = tokenSpan(index_++);
                if (punctuation(index_, ':')) {
                    ++index_;
                    param.constraint = parseType();
                    param.constraints.push_back(param.constraint);
                    while (isOperatorToken("+")) {
                        ++index_;
                        const TypeExprId constraint = parseType();
                        if (!constraint)
                            break;
                        param.constraints.push_back(constraint);
                    }
                }
                declaration.genericParams.push_back(std::move(param));
            } else {
                snapshot_.diagnostics_.push_back(
                    {tokenSpan(index_), "expected a generic parameter name"});
                ++index_;
            }
            if (punctuation(index_, ','))
                ++index_;
            else if (!isOperatorToken(">"))
                break;
        }
        if (isOperatorToken(">"))
            ++index_;
        else
            snapshot_.diagnostics_.push_back({range(start, index_), "expected '>'"});
    } else if (!inheritedParams.empty()) {
        declaration.genericParams = inheritedParams;
    }

    if (kind == DeclKind::Function && punctuation(index_, '(')) {
        ++index_;
        while (index_ < token_count_ && !punctuation(index_, ')')) {
            if (matchesToken(snapshot_, index_, "...")) {
                if (!isExtern && functionKind != FunctionKind::Extern) {
                    snapshot_.diagnostics_.push_back(
                        {tokenSpan(index_), "only 'extern fn' may declare variadic parameters",
                         false, diagnostics::err::ExpectedExpr});
                }
                declaration.isVariadic = true;
                ++index_;
                break;
            }
            Parameter parameter;
            if (isKeywordToken("var")) {
                parameter.bindingKind = BindingKind::Var;
                ++index_;
            } else if (isKeywordToken("let")) {
                parameter.bindingKind = BindingKind::Let;
                ++index_;
            }
            if (snapshot_.tokens_[index_].kind == TokenKind::Identifier) {
                parameter.id   = LocalId{statementCountLocals_++};
                parameter.name = std::string(text(index_));
                parameter.span = tokenSpan(index_++);
                if (punctuation(index_, ':')) {
                    ++index_;
                    parameter.type = parseType();
                    // `fn f(p: T = expr)` keeps the default inside the
                    // parameter list.  A `= extern` alias is handled after
                    // the return type and never reaches this branch.
                    if (index_ < token_count_ && text(index_) == "=") {
                        ++index_;
                        parameter.defaultValue = parseExpression();
                    }
                }
                if (parameter.type && parameter.type.value <= snapshot_.typeExpressions().size()) {
                    parameter.isVariadicSlice =
                        snapshot_.typeExpressions()[parameter.type.value - 1U].isVariadicSlice;
                }
                // A variadic slice is only meaningful when no parameter follows it.
                if (parameter.isVariadicSlice && punctuation(index_, ',')) {
                    snapshot_.diagnostics_.push_back(
                        {tokenSpan(index_), "variadic slice parameter must be the last parameter",
                         false, diagnostics::err::ExpectedExpr});
                }
                declaration.parameters.push_back(std::move(parameter));
            } else {
                snapshot_.diagnostics_.push_back({tokenSpan(index_), "expected a parameter name"});
                ++index_;
            }
            if (punctuation(index_, ','))
                ++index_;
            else if (!punctuation(index_, ')'))
                break;
        }
        if (punctuation(index_, ')'))
            ++index_;
        else
            snapshot_.diagnostics_.push_back({range(start, index_), "expected ')'"});
    }
    if (punctuation(index_, ':') || isOperatorToken("->")) {
        ++index_; // `:` or `->` arrow return type
        declaration.declaredType = parseType();
    } else if (kind == DeclKind::TypeAlias && index_ < token_count_ && text(index_) == "=") {
        ++index_;
        declaration.declaredType = parseType();
    }
    if (kind == DeclKind::Function) {
        // `fn zithName(...): T = extern CSymbol;` keeps the Zith declaration
        // as a normal overloadable function/method but links to the native
        // symbol on the right. There is no body to lower.
        if (text(index_) == "=" && text(index_ + 1U) == "extern") {
            const bool forbidden_kind =
                functionKind == FunctionKind::Const || functionKind == FunctionKind::State;
            const bool forbidden_owner =
                !ownerName.empty() &&
                (traitName == declaration.name || declaration.traitName == declaration.name);
            if (forbidden_kind || forbidden_owner) {
                snapshot_.diagnostics_.push_back(
                    {range(start, index_ + 2U),
                     "external symbol aliases are not allowed on const/state functions "
                     "or trait/interface requirements",
                     false, diagnostics::err::UnsupportedSyntax});
            }
            index_ += 2U; // consume `= extern`
            if (index_ >= token_count_ || snapshot_.tokens_[index_].kind != TokenKind::Identifier) {
                snapshot_.diagnostics_.push_back(
                    {range(start, index_),
                     "external symbol alias requires an identifier after '= extern'", false,
                     diagnostics::err::ExpectedExpr});
            } else {
                declaration.externalSymbol     = std::string(text(index_));
                declaration.externalSymbolSpan = tokenSpan(index_);
                ++index_;
            }
        } else if (text(index_) == "=") {
            snapshot_.diagnostics_.push_back(
                {tokenSpan(index_), "function assignment syntax requires '= extern <identifier>'",
                 false, diagnostics::err::UnsupportedSyntax});
        }
        // A function body owns any flat `state` declarations parsed inside
        // it.  Top-level state declarations also carry this state so nested
        // state-inside-state stays rejected and HIR sees the local owner.
        const bool saved_parent_is_state    = current_local_parent_is_state_;
        const std::string saved_parent_name = std::move(current_local_parent_name_);
        const ScopeId saved_body_scope      = current_function_body_scope_;
        if (parentScope) {
            current_local_parent_is_state_ = functionKind == FunctionKind::State;
            current_local_parent_name_     = parentName.empty() ? declaration.name : parentName;
        } else {
            current_local_parent_is_state_ = functionKind == FunctionKind::State;
            current_local_parent_name_     = declaration.name;
        }
        expecting_function_body_scope_ = true;
        if (punctuation(index_, '{'))
            declaration.body = parseBlock();
        expecting_function_body_scope_ = false;
        current_function_body_scope_   = {};
        current_local_parent_is_state_ = saved_parent_is_state;
        current_local_parent_name_     = std::move(saved_parent_name);
        current_function_body_scope_   = saved_body_scope;
    } else if (kind == DeclKind::Variable && index_ < token_count_ && text(index_) == "=") {
        ++index_;
        declaration.initializer = parseExpression();
    } else if (kind == DeclKind::Variable && declaration.bindingKind == BindingKind::Const) {
        snapshot_.diagnostics_.push_back({range(start, index_),
                                          "Zith--: const declaration requires an initializer",
                                          false, diagnostics::err::UnsupportedSyntax});
    } else if ((kind == DeclKind::Struct || kind == DeclKind::Interface || kind == DeclKind::Enum ||
                kind == DeclKind::Union) &&
               punctuation(index_, '{')) {
        // Struct bodies contain fields and methods; interface bodies contain
        // fields and declaration-only method requirements. Enum and union
        // bodies keep their positional members/variants and may also declare
        // inline methods, which lower exactly like struct methods.
        ++index_;
        while (index_ < token_count_ && !punctuation(index_, '}')) {
            if (punctuation(index_, ',')) {
                ++index_;
                continue;
            }
            if (const auto function_kind = functionKindPrefix()) {
                const auto method_start = index_;
                const auto method_id    = lowerDeclaration(
                    method_start, DeclKind::Function, Visibility::Private, declaration.name,
                    kind == DeclKind::Interface ? declaration.name : std::string{}, false,
                    *function_kind, declaration.genericParams);
                if (kind == DeclKind::Interface && method_id &&
                    method_id.value <= snapshot_.declarations_.size()) {
                    const auto &method = snapshot_.declarations_[method_id.value - 1U];
                    if (method.body) {
                        snapshot_.diagnostics_.push_back(
                            {range(method_start, index_),
                             "interface method requirements cannot have a default body", false,
                             diagnostics::err::UnsupportedSyntax});
                    }
                }
                continue;
            }
            if (kind == DeclKind::Enum) {
                if (snapshot_.tokens_[index_].kind != TokenKind::Identifier) {
                    snapshot_.diagnostics_.push_back(
                        {tokenSpan(index_), "expected a variant name"});
                    ++index_;
                    continue;
                }
                Parameter variant;
                variant.name = std::string(text(index_));
                variant.span = tokenSpan(index_++);
                if (index_ < token_count_ && text(index_) == "=") {
                    if (punctuation(index_ + 1, '{')) {
                        snapshot_.diagnostics_.push_back(
                            {range(index_, index_ + 2),
                             "struct-backed enum variants are not supported in this version", false,
                             diagnostics::err::UnsupportedSyntax});
                        ++index_;
                    } else {
                        ++index_;
                        variant.defaultValue = parseExpression();
                    }
                }
                declaration.parameters.push_back(std::move(variant));
            } else if (kind == DeclKind::Union) {
                // Positional union body: `union Name { T, U, ... }`. Each member is
                // stored as an unnamed Parameter whose `type` names the member type;
                // named-variant unions remain a future extension.
                Parameter member;
                member.span = tokenSpan(index_);
                member.type = parseType();
                declaration.parameters.push_back(std::move(member));
            } else if (kind == DeclKind::Interface) {
                if (!parseInterfaceField(declaration.parameters))
                    break;
            } else if (!parseStructField(declaration.parameters))
                break;
        }
        if (punctuation(index_, '}'))
            ++index_;
        else
            snapshot_.diagnostics_.push_back(
                {range(start, index_), "expected '}' after composite members"});
    } else if (kind == DeclKind::Trait && punctuation(index_, '{')) {
        ++index_;
        while (index_ < token_count_ && !punctuation(index_, '}')) {
            if (punctuation(index_, ',')) {
                ++index_;
                continue;
            }
            if (const auto function_kind = functionKindPrefix()) {
                const auto method_start = index_;
                lowerDeclaration(method_start, DeclKind::Function, Visibility::Private,
                                 declaration.name, declaration.name, false, *function_kind,
                                 declaration.genericParams);
                continue;
            }
            snapshot_.diagnostics_.push_back(
                {tokenSpan(index_), "expected a trait method declaration or '}'"});
            ++index_;
        }
        if (punctuation(index_, '}'))
            ++index_;
        else
            snapshot_.diagnostics_.push_back(
                {range(start, index_), "expected '}' after trait methods"});
    } else if (punctuation(index_, '{')) {
        skipDelimited('{', '}');
    }
    if (kind == DeclKind::Word || kind == DeclKind::Context) {
        const auto what = kind == DeclKind::Word ? "word declarations" : "context declarations";
        snapshot_.diagnostics_.push_back({range(start, index_),
                                          std::string(what) + " are not supported in this version",
                                          false, diagnostics::err::UnsupportedSyntax});
    }
    if (punctuation(index_, ';'))
        ++index_;
    declaration.span = range(start, index_);
    // Re-stamp the id at push time: a struct or implement body pushes its
    // nested method declarations while this one is still being parsed, so
    // the id reserved on entry would collide with the first nested method.
    declaration.id = DeclId{static_cast<uint32_t>(snapshot_.declarations_.size() + 1U)};
    snapshot_.declarations_.push_back(std::move(declaration));
    return declaration.id;
}
bool AstLowerer::parseStructField(std::vector<Parameter> &out) {
    Visibility field_visibility = Visibility::Private;
    int32_t field_mod_depth     = 0;
    if (isVisibilityPrefix()) {
        const auto visibility_word = text(index_);
        field_visibility = visibility_word == "pub" ? Visibility::Public : Visibility::Module;
        ++index_;
        if (field_visibility == Visibility::Module && punctuation(index_, '(')) {
            const uint32_t depth_start = index_++;
            if (punctuation(index_, '.') && punctuation(index_ + 1U, '.') &&
                punctuation(index_ + 2U, ')')) {
                field_mod_depth = -1;
                index_ += 3;
            } else {
                std::int64_t depth = 0;
                if (snapshot_.tokens_[index_].kind == TokenKind::Literal &&
                    support::parseIntegerLiteral(text(index_), depth) ==
                        support::IntLiteralStatus::Ok &&
                    depth >= std::numeric_limits<int32_t>::min() &&
                    depth <= std::numeric_limits<int32_t>::max()) {
                    field_mod_depth = static_cast<int32_t>(depth);
                    ++index_;
                } else {
                    snapshot_.diagnostics_.push_back(
                        {tokenSpan(index_), "invalid mod depth for struct field"});
                }
                if (!punctuation(index_, ')')) {
                    snapshot_.diagnostics_.push_back(
                        {range(depth_start, index_),
                         "expected ')' after struct field module depth"});
                } else {
                    ++index_;
                }
            }
        }
    }

    if (index_ < token_count_ && text(index_) == "const") {
        const uint32_t const_start = index_++;
        if (snapshot_.tokens_[index_].kind != TokenKind::Identifier) {
            snapshot_.diagnostics_.push_back({tokenSpan(index_),
                                              "expected a field name after 'const'", false,
                                              diagnostics::err::UnsupportedSyntax});
            ++index_;
            if (punctuation(index_, ',') || punctuation(index_, '}'))
                return true;
            return false;
        }
        Parameter field;
        field.id           = LocalId{statementCountLocals_++};
        field.isConstField = true;
        field.visibility   = field_visibility;
        field.modDepth     = field_mod_depth;
        field.name         = std::string(text(index_));
        field.span         = tokenSpan(index_++);
        if (!punctuation(index_, ':')) {
            snapshot_.diagnostics_.push_back({range(const_start, index_),
                                              "expected ':' after const field name", false,
                                              diagnostics::err::UnsupportedSyntax});
        } else {
            ++index_;
            field.type = parseType();
            if (index_ < token_count_ && text(index_) == "=") {
                ++index_;
                field.defaultValue = parseExpression();
            } else {
                snapshot_.diagnostics_.push_back(
                    {range(const_start, index_),
                     "Zith--: const struct field requires an initializer", false,
                     diagnostics::err::UnsupportedSyntax});
            }
        }
        out.push_back(std::move(field));
        if (punctuation(index_, ','))
            ++index_;
        else if (!punctuation(index_, '}'))
            return false;
        return true;
    }

    // Grouped field syntax: `{ [x, y, z]: Type, ... }`. Expanding the
    // list into one Parameter per name keeps sema, HIR and codegen on
    // the existing per-field path.
    if (punctuation(index_, '[')) {
        const auto group_start = index_++;
        std::vector<Parameter> grouped;
        while (index_ < token_count_ && !punctuation(index_, '[') && !punctuation(index_, ']') &&
               !punctuation(index_, '}')) {
            if (punctuation(index_, ',')) {
                ++index_;
                continue;
            }
            if (snapshot_.tokens_[index_].kind != TokenKind::Identifier) {
                snapshot_.diagnostics_.push_back(
                    {tokenSpan(index_), "expected a field name in grouped field list"});
                ++index_;
                continue;
            }
            Parameter field;
            field.id         = LocalId{statementCountLocals_++};
            field.visibility = field_visibility;
            field.modDepth   = field_mod_depth;
            field.name       = std::string(text(index_));
            field.span       = tokenSpan(index_++);
            grouped.push_back(std::move(field));
        }
        if (!punctuation(index_, ']')) {
            snapshot_.diagnostics_.push_back(
                {range(group_start, index_), "expected ']' after grouped field names"});
            while (index_ < token_count_ && !punctuation(index_, '}') &&
                   !punctuation(index_, ',')) {
                if (punctuation(index_, ']'))
                    break;
                ++index_;
            }
        } else {
            ++index_; // consume ']'
        }
        if (!punctuation(index_, ':')) {
            snapshot_.diagnostics_.push_back({range(group_start, index_),
                                              "expected ':' after grouped field names", false,
                                              diagnostics::err::UnsupportedSyntax});
        } else {
            ++index_;
            const auto type_id = parseType();
            ExprId default_value;
            if (index_ < token_count_ && text(index_) == "=") {
                ++index_;
                default_value = parseExpression();
            }
            for (auto &field : grouped) {
                field.type = type_id;
                if (default_value)
                    field.defaultValue = default_value;
                out.push_back(std::move(field));
            }
        }
        if (!punctuation(index_, ',') && !punctuation(index_, '}')) {
            snapshot_.diagnostics_.push_back(
                {range(group_start, index_), "expected ',' after grouped fields"});
            ++index_;
        }
        return true;
    }

    if (snapshot_.tokens_[index_].kind != TokenKind::Identifier) {
        snapshot_.diagnostics_.push_back({tokenSpan(index_), "expected a field name"});
        ++index_;
        return false;
    }
    Parameter field;
    field.id         = LocalId{statementCountLocals_++};
    field.visibility = field_visibility;
    field.modDepth   = field_mod_depth;
    field.name       = std::string(text(index_));
    field.span       = tokenSpan(index_++);
    if (punctuation(index_, ':')) {
        ++index_;
        field.type = parseType();
        if (index_ < token_count_ && text(index_) == "=") {
            ++index_;
            field.defaultValue = parseExpression();
        }
    } else if (index_ < token_count_ && text(index_) == "=") {
        // This is a rejected field syntax, not a field type: consume the
        // expression so we do not also leave its tokens for top-level recovery.
        ++index_;
        (void)parseExpression();
        snapshot_.diagnostics_.push_back(
            {TextSpan{field.span.start, index_ > 0U ? tokenSpan(index_ - 1U).end : field.span.end},
             "unsupported: field '" + field.name +
                 " = <expr>'; use 'name: Type' or 'name: Type = default'",
             false, diagnostics::err::UnsupportedSyntax});
        out.push_back(std::move(field));
        if (punctuation(index_, ','))
            ++index_;
        else if (!punctuation(index_, '}'))
            return false;
        return true;
    } else if (index_ < token_count_ && !punctuation(index_, ',')) {
        snapshot_.diagnostics_.push_back({field.span,
                                          "expected ':' after field name '" + field.name + "'",
                                          false, diagnostics::err::UnsupportedSyntax});
    }
    out.push_back(std::move(field));
    if (punctuation(index_, ','))
        ++index_;
    else if (!punctuation(index_, '}'))
        return false;
    return true;
}
bool AstLowerer::parseInterfaceField(std::vector<Parameter> &out) {
    const bool is_grouped = index_ < token_count_ && punctuation(index_, '[');
    const uint32_t start  = index_;
    std::vector<Parameter> fields;
    if (is_grouped) {
        ++index_;
        while (index_ < token_count_ && !punctuation(index_, '[') && !punctuation(index_, ']') &&
               !punctuation(index_, '}')) {
            if (punctuation(index_, ',')) {
                ++index_;
                continue;
            }
            if (snapshot_.tokens_[index_].kind != TokenKind::Identifier) {
                snapshot_.diagnostics_.push_back(
                    {tokenSpan(index_), "expected a field name in grouped interface field list"});
                ++index_;
                continue;
            }
            Parameter field;
            field.id   = LocalId{statementCountLocals_++};
            field.name = std::string(text(index_));
            field.span = tokenSpan(index_++);
            fields.push_back(std::move(field));
        }
        if (!punctuation(index_, ']')) {
            snapshot_.diagnostics_.push_back(
                {range(start, index_), "expected ']' after grouped interface field names"});
            while (index_ < token_count_ && !punctuation(index_, '}') &&
                   !punctuation(index_, ',')) {
                if (punctuation(index_, ']'))
                    break;
                ++index_;
            }
        } else {
            ++index_;
        }
    } else {
        if (snapshot_.tokens_[index_].kind != TokenKind::Identifier) {
            snapshot_.diagnostics_.push_back(
                {tokenSpan(index_), "expected an interface field name"});
            ++index_;
            return false;
        }
        Parameter field;
        field.id   = LocalId{statementCountLocals_++};
        field.name = std::string(text(index_));
        field.span = tokenSpan(index_++);
        fields.push_back(std::move(field));
    }
    if (!punctuation(index_, ':')) {
        snapshot_.diagnostics_.push_back({range(start, index_),
                                          "expected ':' after interface field name(s)", false,
                                          diagnostics::err::UnsupportedSyntax});
    } else {
        ++index_;
        const auto type_id = parseType();
        for (auto &field : fields) {
            field.type = type_id;
            out.push_back(std::move(field));
        }
    }
    if (!punctuation(index_, ',') && !punctuation(index_, '}')) {
        snapshot_.diagnostics_.push_back(
            {range(start, index_), "expected ',' after interface fields"});
        ++index_;
    }
    return true;
}
void AstLowerer::skipDelimited(const char open, const char close) {
    if (!punctuation(index_, open))
        return;
    uint32_t depth = 0;
    do {
        if (punctuation(index_, open))
            ++depth;
        else if (punctuation(index_, close))
            --depth;
        ++index_;
    } while (index_ < token_count_ && depth != 0);
}
void AstLowerer::skipNestedUnsupportedDeclaration() {
    if (index_ >= token_count_)
        return;
    bool saw_open        = false;
    uint32_t brace_depth = 0;
    while (index_ < token_count_) {
        if (punctuation(index_, '{')) {
            saw_open = true;
            ++brace_depth;
            ++index_;
            continue;
        }
        if (punctuation(index_, '}')) {
            if (!saw_open || brace_depth != 0U) {
                if (brace_depth != 0U)
                    --brace_depth;
                ++index_;
                if (saw_open && brace_depth == 0U) {
                    if (punctuation(index_, ';'))
                        ++index_;
                    return;
                }
                continue;
            }
            break;
        }
        if (!saw_open && (punctuation(index_, ';') || punctuation(index_, '}'))) {
            if (punctuation(index_, ';'))
                ++index_;
            return;
        }
        ++index_;
    }
}
} // namespace zith::frontend
