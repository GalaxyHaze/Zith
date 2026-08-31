#include "frontend/frontend.hpp"
#include "diagnostics/error-codes.hpp"
#include "frontend/ast-lowerer.hpp"
#include "frontend/macro-expand.hpp"
#include "support/int-literal.hpp"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>

namespace zith::frontend {

void lowerAst(FrontendSnapshot &snapshot) {
    AstLowerer(snapshot).run();
}

const Token &SyntaxToken::token() const noexcept {
    return (*tokens_)[id_.value - 1];
}

std::string_view SyntaxToken::text() const noexcept {
    const TextSpan span = token().span;
    return std::string_view(*source_).substr(span.start, span.size());
}

const GreenElement &SyntaxNode::child(uint32_t index) const noexcept {
    return green_->children[index];
}

SyntaxToken SyntaxNode::token(uint32_t index) const noexcept {
    return SyntaxToken(*tokens_, *source_, child(index).token);
}

FrontendSnapshot::FrontendSnapshot(std::string source) : source_(std::move(source)) {}

SyntaxNode FrontendSnapshot::root() const noexcept {
    return SyntaxNode(*root_, tokens_, source_);
}

std::string FrontendSnapshot::reconstruct() const {
    std::string result;
    result.reserve(source_.size());
    for (const Token &token : tokens_) {
        for (uint32_t index = 0; index < token.leadingTriviaCount; ++index) {
            const TextSpan span = trivia_[token.leadingTriviaStart + index].span;
            result.append(source_, span.start, span.size());
        }
        if (token.kind != TokenKind::End) {
            result.append(source_, token.span.start, token.span.size());
        }
    }
    return result;
}

FrontendSnapshot parseWithImports(std::string source,
                                  const std::vector<ImportedMacroRecord> &imported) {
    FrontendSnapshot snapshot(std::move(source));
    lex(snapshot);
    parseCst(snapshot);
    lowerAst(snapshot);
    // Flag template bodies *before* expansion, so only the original template
    // nodes are inert and their clones stay analysable.
    markMacroTemplates(snapshot);
    const auto expand_start = std::chrono::steady_clock::now();
    expandMacros(snapshot, imported);
    snapshot.expandMs_ =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - expand_start)
            .count();
    return snapshot;
}

FrontendSnapshot parse(std::string source) {
    return parseWithImports(std::move(source), {});
}

std::string canonicalTypeString(const FrontendSnapshot &snapshot, const TypeExprId id) {
    if (!id || id.value > snapshot.typeExpressions().size())
        return "?";
    const auto &type  = snapshot.typeExpressions()[id.value - 1U];
    const auto nested = [&](const size_t index) {
        return index < type.arguments.size() ? canonicalTypeString(snapshot, type.arguments[index])
                                             : std::string("?");
    };
    switch (type.kind) {
    case TypeExprKind::Name:
        if (type.arguments.empty())
            return type.name;
        {
            std::string result = type.name + "<";
            for (size_t index = 0; index < type.arguments.size(); ++index) {
                if (index != 0)
                    result += ",";
                result += canonicalTypeString(snapshot, type.arguments[index]);
            }
            result += ">";
            return result;
        }
    case TypeExprKind::Pointer:
        return "*" + nested(0);
    case TypeExprKind::Optional:
        return "?" + nested(0);
    case TypeExprKind::Parenthesized:
        return "(" + nested(0) + ")";
    case TypeExprKind::Slice:
        return (type.isVariadicSlice ? "[...]" : "[]") + nested(0);
    case TypeExprKind::Opaque:
        return "raw opaque";
    case TypeExprKind::OpaqueTagged:
        return "opaque";
    case TypeExprKind::Pack: {
        std::string result = "|";
        for (size_t index = 0; index < type.arguments.size(); ++index) {
            if (index != 0)
                result += ",";
            if (index < type.member_names.size()) {
                result += type.member_names[index];
                result += ":";
            }
            result += canonicalTypeString(snapshot, type.arguments[index]);
        }
        result += "|";
        return result;
    }
    case TypeExprKind::Dyn:
        return "dyn " + nested(0);
    case TypeExprKind::Array:
        return "[" + std::to_string(type.arrayLength) + "]" + nested(0);
    case TypeExprKind::Function: {
        std::string result = type.isStateFunctionType ? "state(" : "fn(";
        for (size_t index = 0; index + 1 < type.arguments.size(); ++index) {
            if (index != 0)
                result += ",";
            result += canonicalTypeString(snapshot, type.arguments[index]);
        }
        result += ")";
        if (!type.arguments.empty())
            result += ":" + canonicalTypeString(snapshot, type.arguments.back());
        return result;
    }
    case TypeExprKind::Error:
        break;
    }
    return "?";
}

std::string functionSignature(const FrontendSnapshot &snapshot, const Declaration &decl) {
    std::string result = "(";
    bool first         = true;
    const auto append  = [&](const std::string &text) {
        if (!first)
            result += ",";
        result += text;
        first = false;
    };
    // Methods without a receiver are static in this compiler. A `self`
    // parameter without an explicit type is spelled `*Owner` so signatures
    // stay comparable with the sema function type.
    const bool implicit_self = !decl.ownerName.empty() && !decl.parameters.empty() &&
                               decl.parameters.front().name == "self" &&
                               !decl.parameters.front().type;
    if (implicit_self)
        append("*" + decl.ownerName);
    for (size_t index = 0; index < decl.parameters.size(); ++index) {
        if (implicit_self && index == 0)
            continue;
        append(canonicalTypeString(snapshot, decl.parameters[index].type));
    }
    if (decl.isVariadic) {
        if (!first)
            result += ",";
        result += "...";
    }
    if (!decl.ownerName.empty() && decl.genericParams.empty()) {
        for (const auto &generic_decl : snapshot.declarations()) {
            if (generic_decl.name != decl.ownerName || generic_decl.genericParams.empty())
                continue;
            if (!first)
                result += ",";
            std::string owner_args = "<";
            for (size_t index = 0; index < generic_decl.genericParams.size(); ++index) {
                if (index != 0)
                    owner_args += ",";
                owner_args += generic_decl.genericParams[index].name;
            }
            owner_args += ">";
            result += owner_args;
            first = false;
            break;
        }
    }
    result += ")";
    return result;
}

} // namespace zith::frontend
