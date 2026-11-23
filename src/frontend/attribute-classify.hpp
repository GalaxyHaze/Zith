#pragma once

#include "diagnostics/error-codes.hpp"
#include "frontend/frontend.hpp"

#include <cstddef>

namespace zith::frontend {

inline AttributeKind classifyAttribute(Attribute &attr, AttributeTarget target,
                                       std::vector<Diagnostic> &diagnostics) {
    AttributeKind classified = AttributeKind::Unknown;
    if (attr.text == "alignas") {
        if (target == AttributeTarget::Struct || target == AttributeTarget::Variable)
            classified = AttributeKind::AlignAs;
        else
            diagnostics.push_back({attr.span, "attribute 'alignas' is not applicable here", false,
                                   diagnostics::err::UnsupportedSyntax});
    } else if (attr.text == "noalias") {
        if (target == AttributeTarget::Function)
            classified = AttributeKind::NoAlias;
        else
            diagnostics.push_back({attr.span, "attribute 'noalias' is not applicable here", false,
                                   diagnostics::err::UnsupportedSyntax});
    } else if (attr.text == "likely") {
        classified = AttributeKind::Likely;
    } else if (attr.text == "unlikely") {
        classified = AttributeKind::Unlikely;
    } else if (attr.text == "test" && target == AttributeTarget::Function) {
        classified = AttributeKind::Test;
    } else if (attr.text == "discardable" && target == AttributeTarget::Function) {
        classified = AttributeKind::Discardable;
    } else if ((attr.text == "raw" || attr.text == "safe") && target == AttributeTarget::Function) {
        classified = attr.text == "raw" ? AttributeKind::Raw : AttributeKind::Safe;
    } else if (attr.text == "volatile" && target == AttributeTarget::Variable) {
        classified = AttributeKind::Volatile;
    } else if (attr.text == "scope") {
        // Scope-awareness is a property of a macro declaration; the parser
        // classifies it under Other because macros are not declarations with a
        // dedicated AttributeTarget here.
        classified = AttributeKind::Scope;
    }

    const bool known = attr.text == "alignas" || attr.text == "noalias" || attr.text == "likely" ||
                       attr.text == "unlikely" || attr.text == "test" ||
                       attr.text == "discardable" || attr.text == "volatile" ||
                       attr.text == "raw" || attr.text == "safe" || attr.text == "scope";
    if (classified == AttributeKind::Unknown && !known) {
        diagnostics.push_back({attr.span, "unknown attribute '" + attr.text + "'", true,
                               diagnostics::err::UnknownAttribute});
    } else if (classified == AttributeKind::Unknown && known && attr.text != "alignas" &&
               attr.text != "noalias") {
        diagnostics.push_back({attr.span, "attribute '" + attr.text + "' is not applicable here",
                               true, diagnostics::err::AttributeNotApplicable});
    }
    attr.kind = classified;
    return classified;
}

inline void classifyAttributes(std::vector<Attribute> &attributes, AttributeTarget target,
                               std::vector<Diagnostic> &diagnostics) {
    size_t alignas_count = 0;
    for (auto &attr : attributes) {
        if (classifyAttribute(attr, target, diagnostics) == AttributeKind::AlignAs &&
            ++alignas_count > 1U) {
            diagnostics.push_back({attr.span, "duplicate 'alignas' attribute", false,
                                   diagnostics::err::UnsupportedSyntax});
        }
    }
}

} // namespace zith::frontend
