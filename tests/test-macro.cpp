#include "diagnostics/error-codes.hpp"
#include "frontend/frontend.hpp"
#include "frontend/macro-expand.hpp"
#include "test-common.hpp"

#include <string>
#include <vector>

using namespace zith;

static void test_macro() {
    // 1. macro declaration
    {
        auto snap = frontend::parse("macro mylog(msg: expr) { @println(msg); }");
        CHECK(snap.declarations().size() >= 1, "macro declaration is parsed");
        CHECK(snap.declarations()[0].kind == frontend::DeclKind::Macro, "decl kind is Macro");
        CHECK_EQ(snap.declarations()[0].name, std::string("mylog"), "macro name is 'mylog'");
        CHECK(!snap.declarations()[0].isRawMacro, "not raw by default");
        CHECK_EQ(snap.declarations()[0].parameters.size(), 1U, "one parameter");
        CHECK(static_cast<bool>(snap.declarations()[0].body), "macro has a body");
    }
    // 2. raw macro
    {
        auto snap = frontend::parse(
            "raw macro swap(a: identifier, b: identifier) { let tmp = a; a = b; b = tmp; }");
        CHECK(snap.declarations().size() >= 1, "raw macro declaration is parsed");
        CHECK_EQ(snap.declarations()[0].name, std::string("swap"), "raw macro name is 'swap'");
        CHECK(snap.declarations()[0].isRawMacro, "is raw macro");
        CHECK_EQ(snap.declarations()[0].parameters.size(), 2U, "two parameters");
    }
    // 3. pub macro
    {
        auto snap = frontend::parse("pub macro debug(msg: expr) { }");
        CHECK_EQ(snap.declarations()[0].visibility, frontend::Visibility::Public,
                 "pub macro visibility is public");
    }
    // 4. reserved `fail` can be used as a macro name without changing its token kind
    {
        auto snap = frontend::parse("pub macro fail(message: expr) { @assert(false); }\n"
                                    "fn main() { @fail(\"intentional\"); }\n");
        CHECK_EQ(snap.tokens()[2].kind, frontend::TokenKind::Keyword,
                 "`fail` remains a reserved keyword");
        CHECK(snap.diagnostics().empty(), "reserved `fail` macro declaration parses and expands");
        CHECK_EQ(snap.declarations()[0].name, std::string("fail"),
                 "reserved `fail` is retained as the macro name");
        bool expanded = false;
        for (const auto &expr : snap.expressions()) {
            if (expr.kind == frontend::ExprKind::MacroCall && expr.text == "fail") {
                expanded = static_cast<bool>(expr.expansion);
                break;
            }
        }
        CHECK(expanded, "`@fail` expands as a user macro");
    }
    // 5. Macro call expands
    {
        auto snap = frontend::parse(
            "macro mylog(msg: expr) { @println(msg); }\nfn main() { @mylog(\"hello\"); }");
        CHECK(snap.declarations().size() >= 2, "module has macro + fn declarations");
        const auto &fnDecl = snap.declarations()[1];
        CHECK_EQ(fnDecl.kind, frontend::DeclKind::Function, "second decl is a function");
        CHECK(static_cast<bool>(fnDecl.body), "function has a body");
        const auto &bodyExpr = snap.expressions()[fnDecl.body.value - 1U];
        CHECK_EQ(bodyExpr.kind, frontend::ExprKind::Block, "function body is a block");
        bool found = false;
        for (auto sid : bodyExpr.statements) {
            if (!sid)
                continue;
            const auto &stmt = snap.statements()[sid.value - 1U];
            if (stmt.kind != frontend::StmtKind::Expression)
                continue;
            if (!stmt.expression)
                continue;
            const auto &expr = snap.expressions()[stmt.expression.value - 1U];
            if (expr.kind == frontend::ExprKind::MacroCall) {
                found = true;
                CHECK_EQ(expr.text, std::string("mylog"), "MacroCall text is 'mylog'");
                CHECK(static_cast<bool>(expr.expansion), "MacroCall has expansion set");
                const auto &exp = snap.expressions()[expr.expansion.value - 1U];
                CHECK_EQ(exp.kind, frontend::ExprKind::Block, "expansion is a Block");
            }
        }
        CHECK(found, "found a MacroCall in function body");
    }
    // 5. @offsetOf still intrinsic
    {
        auto snap           = frontend::parse("fn foo() { @offsetOf(Foo, bar) }");
        bool foundIntrinsic = false, foundMacroCall = false;
        for (const auto &expr : snap.expressions()) {
            if (expr.kind == frontend::ExprKind::LayoutIntrinsic)
                foundIntrinsic = true;
            if (expr.kind == frontend::ExprKind::MacroCall)
                foundMacroCall = true;
        }
        CHECK(foundIntrinsic, "@offsetOf is still a LayoutIntrinsic");
        CHECK(!foundMacroCall, "@offsetOf is NOT a MacroCall");
    }
    // 6. unknown macro
    {
        auto snap = frontend::parse("fn main() { @unknown(1); }");
        bool has  = false;
        for (const auto &d : snap.diagnostics())
            if (d.code == diagnostics::err::MacroUnknown) {
                has = true;
                break;
            }
        CHECK(has, "unknown macro produces MacroUnknown error");
    }
    // 7. arity error
    {
        auto snap = frontend::parse("macro f(a: expr) { a; }\nfn main() { @f(1, 2); }");
        bool has  = false;
        for (const auto &d : snap.diagnostics())
            if (d.code == diagnostics::err::MacroArity) {
                has = true;
                break;
            }
        CHECK(has, "wrong arity produces MacroArity error");
    }
    // 8. recursion
    {
        auto snap = frontend::parse("macro a() { @a(); }\nfn main() { @a(); }");
        bool has  = false;
        for (const auto &d : snap.diagnostics())
            if (d.code == diagnostics::err::MacroRecursion) {
                has = true;
                break;
            }
        CHECK(has, "recursive macro produces MacroRecursion error");
    }
    // 9. arg kind
    {
        auto snap = frontend::parse("macro set(v: identifier) { v = 1; }\nfn main() { @set(42); }");
        bool has  = false;
        for (const auto &d : snap.diagnostics())
            if (d.code == diagnostics::err::MacroArgKind) {
                has = true;
                break;
            }
        CHECK(has, "non-Name to identifier param produces MacroArgKind");
    }
    // 10. body meta-type
    {
        auto snap = frontend::parse("macro run(b: body) { b; }\nfn main() { @run(42); }");
        bool has  = false;
        for (const auto &d : snap.diagnostics())
            if (d.code == diagnostics::err::MacroArgKind) {
                has = true;
                break;
            }
        CHECK(has, "non-Block to body param produces MacroArgKind");
    }
    // 11. duplicate macro
    {
        auto snap = frontend::parse("macro f() { 1; }\nmacro f() { 2; }\nfn main() { @f(); }");
        bool has  = false;
        for (const auto &d : snap.diagnostics())
            if (d.code == diagnostics::err::MacroDuplicate) {
                has = true;
                break;
            }
        CHECK(has, "duplicate macro produces MacroDuplicate");
    }
    // 12. top-level @ tolerated
    {
        auto snap = frontend::parse("@appendField Custom, x: i32;");
        CHECK(snap.declarations().empty(), "top-level @ is still tolerated");
        CHECK(snap.diagnostics().empty(), "no diagnostics for tolerated @appendField");
    }

    // 13. Other intrinsic names are LayoutIntrinsic, never MacroCall
    {
        auto snap           = frontend::parse("fn foo() { @fields(Foo); @hasTrait(Foo, Bar); }");
        bool foundIntrinsic = false, foundMacroCall = false;
        for (const auto &expr : snap.expressions()) {
            if (expr.kind == frontend::ExprKind::LayoutIntrinsic &&
                (expr.text == "fields" || expr.text == "hasTrait"))
                foundIntrinsic = true;
            if (expr.kind == frontend::ExprKind::MacroCall)
                foundMacroCall = true;
        }
        CHECK(foundIntrinsic, "@fields/@hasTrait are LayoutIntrinsic");
        CHECK(!foundMacroCall, "intrinsics from table are NOT MacroCall");
    }
    // 14. Unknown @name is MacroCall
    {
        auto snap  = frontend::parse("fn main() { @myThing(42); }");
        bool found = false;
        for (const auto &expr : snap.expressions()) {
            if (expr.kind == frontend::ExprKind::MacroCall && expr.text == "myThing")
                found = true;
        }
        CHECK(found, "unknown @name falls through to MacroCall");
    }
    // 15. Attributes: named + positional + empty list
    {
        auto snap  = frontend::parse("fn main() { @closure|x: 50, y: 50, value|() }");
        bool found = false;
        for (const auto &expr_ : snap.expressions()) {
            if (expr_.kind != frontend::ExprKind::MacroCall || expr_.text != "closure")
                continue;
            const auto &expr = expr_;
            found            = true;
            CHECK_EQ(expr.attributes.size(), 3U, "3 attribute expressions");
            CHECK_EQ(expr.attributeNames.size(), 3U, "3 attribute names");
            CHECK_EQ(expr.attributeNames[0], std::string("x"), "first attr named x");
            CHECK_EQ(expr.attributeNames[1], std::string("y"), "second attr named y");
            CHECK_EQ(expr.attributeNames[2], std::string(""), "third attr positional");
        }
        CHECK(found, "MacroCall with attributes found");
    }
    // 16. Attributes empty list `||`
    {
        auto snap  = frontend::parse("fn main() { @nome||(a) }");
        bool found = false;
        for (const auto &expr : snap.expressions()) {
            if (expr.kind != frontend::ExprKind::MacroCall || expr.text != "nome")
                continue;
            found = true;
            CHECK_EQ(expr.attributes.size(), 0U, "empty attributes");
            CHECK_EQ(expr.operands.size(), 1U, "one argument");
        }
        CHECK(found, "MacroCall with empty attributes found");
    }
    // 17. Macro with `attributes` param skips arity
    {
        auto snap = frontend::parse("macro closure(attributes, body1: block) { body1; }\n"
                                    "fn main() { @closure|k: 1|({ 42 }) }");
        bool has  = false;
        for (const auto &d : snap.diagnostics())
            if (d.code == diagnostics::err::MacroArity) {
                has = true;
                break;
            }
        CHECK(!has, "attributes param does not count toward arity");
    }
    // 18. `=expr` stores uneval flag and arg span
    {
        auto snap  = frontend::parse("macro mylog(msg: expr) { @println(msg); }\n"
                                     "fn main() { @mylog(=5 + 5); }");
        bool found = false;
        for (const auto &expr_ : snap.expressions()) {
            if (expr_.kind != frontend::ExprKind::MacroCall || expr_.text != "mylog")
                continue;
            const auto &expr = expr_;
            found            = true;
            CHECK_EQ(expr.argIsUnevaluated.size(), 1U, "one uneval flag");
            CHECK(expr.argIsUnevaluated[0], "first arg is unevaluated");
            CHECK_EQ(expr.argSpans.size(), 1U, "one arg span");
            CHECK(expr.argSpans[0].size() > 0, "argSpan is non-empty");
        }
        CHECK(found, "=expr arg recorded correctly");
    }
    // 19. raw macro splices statements
    {
        auto snap = frontend::parse(
            "raw macro swap(a: identifier, b: identifier) { let _tmp = a; a = b; b = _tmp; }\n"
            "fn main() { var x = 1; var y = 2; @swap(x, y); }");
        bool foundCall = false;
        for (const auto &expr : snap.expressions()) {
            if (expr.kind == frontend::ExprKind::MacroCall && expr.text == "swap" &&
                expr.expansion) {
                foundCall = true;
                CHECK(expr.expansionIsRaw, "raw expansion flagged");
            }
        }
        CHECK(foundCall, "raw macro call expanded");
    }
    // 20. raw macro as value produces MacroRawValue
    {
        auto snap = frontend::parse(
            "raw macro swap(a: identifier, b: identifier) { let _tmp = a; a = b; b = _tmp; }\n"
            "fn main() { let y = @swap(x, y); }");
        bool has = false;
        for (const auto &d : snap.diagnostics())
            if (d.code == diagnostics::err::MacroRawValue) {
                has = true;
                break;
            }
        CHECK(has, "raw macro as value produces MacroRawValue");
    }
    // 21. Normal macro in value position does NOT produce MacroRawValue
    {
        auto snap = frontend::parse("macro answer() { 42; }\n"
                                    "fn main() { let x = @answer(); }");
        bool has  = false;
        for (const auto &d : snap.diagnostics())
            if (d.code == diagnostics::err::MacroRawValue) {
                has = true;
                break;
            }
        CHECK(!has, "normal macro as value is fine");
    }
    // 22. block meta-type accepts block, rejects non-block
    {
        auto snap = frontend::parse("macro run(b: block) { b; }\n"
                                    "fn main() { @run({ 1; }); }");
        bool has  = false;
        for (const auto &d : snap.diagnostics())
            if (d.code == diagnostics::err::MacroArgKind) {
                has = true;
                break;
            }
        CHECK(!has, "block meta-type accepts a block");
    }
    // 23. Unknown meta-type is diagnosed at declaration
    {
        auto snap = frontend::parse("macro bad(p: Xyzzy) { }");
        CHECK(!snap.diagnostics().empty(), "unknown meta-type diagnosed at declaration");
    }
    // 24. tag macro declaration
    {
        auto snap = frontend::parse("tag macro Section(attributes, content: body) { content }");
        CHECK(snap.declarations().size() >= 1, "tag macro declaration is parsed");
        CHECK(snap.declarations()[0].kind == frontend::DeclKind::Macro, "tag decl kind is Macro");
        CHECK_EQ(snap.declarations()[0].name, std::string("Section"),
                 "tag macro name is 'Section'");
        CHECK(snap.declarations()[0].isTagMacro, "isTagMacro is true");
        CHECK(snap.declarations()[0].hasAttributesParam,
              "tag macro with attributes parameter is recorded");
        CHECK(!snap.diagnostics().empty(), "tag macro declaration is rejected in Zith--");
    }
    // 25. tag macro call parses attributes and body
    {
        auto snap  = frontend::parse("tag macro Section(attributes, content: body) { content }\n"
                                     "fn main() { <Section title: 7> let x = 1; </Section> }");
        bool found = false;
        for (const auto &expr : snap.expressions()) {
            if (expr.kind != frontend::ExprKind::MacroCall || expr.text != "Section")
                continue;
            found = true;
            CHECK_EQ(expr.attributeNames.size(), 1U, "one attribute");
            CHECK_EQ(expr.attributeNames[0], std::string("title"), "attribute name is title");
            CHECK_EQ(expr.operands.size(), 1U, "one body argument");
            CHECK(expr.argSpans.size() == 1U && expr.argSpans[0].size() > 0,
                  "body argument span is recorded");
        }
        CHECK(found, "found tag macro MacroCall");
        CHECK(!snap.diagnostics().empty(), "tag macro body and call are rejected in Zith--");
    }
    // 26. mismatched closing tag
    {
        auto snap = frontend::parse("tag macro Section(attributes, content: body) { content }\n"
                                    "fn main() { <Section> </Other> }");
        bool has  = false;
        for (const auto &d : snap.diagnostics())
            if (d.code == diagnostics::err::MacroTagMismatch) {
                has = true;
                break;
            }
        CHECK(has, "mismatched closing tag produces MacroTagMismatch");
    }
    // 27. tag macro used as a value
    {
        auto snap = frontend::parse("tag macro Section(content: body) { content }\n"
                                    "fn main() -> i32 { let x = <Section> </Section>; }");
        bool has  = false;
        for (const auto &d : snap.diagnostics())
            if (d.code == diagnostics::err::MacroTagValue) {
                has = true;
                break;
            }
        CHECK(has, "tag macro value use produces MacroTagValue");
    }
    // 28. attributes.x substitution and unknown attribute
    {
        auto snap = frontend::parse(
            "tag macro Box(attributes, content: body) { let t = attributes.title; content }\n"
            "fn main() { <Box title: 2, color: \"red\"> </Box> }");
        CHECK(!snap.diagnostics().empty(), "tag macro source is rejected before expansion");

        auto missing =
            frontend::parse("tag macro Box(attributes, content: body) { attributes.missing }\n"
                            "fn main() { <Box> </Box> }");
        bool hasMissing = false;
        for (const auto &d : missing.diagnostics())
            if (d.code == diagnostics::err::MacroAttrUnknown) {
                hasMissing = true;
                break;
            }
        CHECK(hasMissing, "unknown attribute produces MacroAttrUnknown");
    }
    // 29. attributes on a macro without the attributes parameter
    {
        auto snap = frontend::parse("tag macro Plain(content: body) { content }\n"
                                    "fn main() { <Plain title: 1> </Plain> }");
        bool has  = false;
        for (const auto &d : snap.diagnostics())
            if (d.code == diagnostics::err::MacroAttrNotAllowed) {
                has = true;
                break;
            }
        CHECK(has, "undeclared tag attributes produce MacroAttrNotAllowed");
    }
    // 30. `<` comparisons and generics remain non-tag syntax
    {
        auto snap     = frontend::parse("fn f(a: i32, b: i32) -> bool { a < b }\n"
                                        "fn g<T>(x: T) -> T { x }\n"
                                        "fn main() -> i32 { g<i32>(1) }");
        bool foundTag = false;
        for (const auto &expr : snap.expressions())
            if (expr.kind == frontend::ExprKind::MacroCall && expr.text == "T")
                foundTag = true;
        CHECK(!foundTag, "comparison/generic angle brackets are not tag calls");
        CHECK(snap.diagnostics().empty(), "non-tag expression source parses cleanly");
    }
    // 31. macro body with dock/jump arguments does not diagnose arguments as code
    {
        auto snap             = frontend::parse("state target(v: i32): i32 { return v; }\n"
                                                "macro jump_to(v: expr) { jump target(v); }\n"
                                                "macro dock_to(v: expr) { dock target(v); }\n"
                                                "fn main(): i32 { return @dock_to(2); }");
        bool macro_unknown    = false;
        bool template_unknown = false;
        for (const auto &d : snap.diagnostics()) {
            if (d.code == diagnostics::err::MacroUnknown)
                macro_unknown = true;
            if (d.message.find("unknown identifier 'v'") != std::string::npos)
                template_unknown = true;
        }
        CHECK(!macro_unknown, "docked/jumped macros expand without unknown-macro errors");
        CHECK(!template_unknown, "dock/jump macro arguments are template material");
    }
    // 32. macros imported into a snapshot expand by name and alias
    {
        auto dep = frontend::parse("pub macro twice(v: expr) { v + v }\n"
                                   "pub macro greet() { @println(\"hi\"); }\n");
        frontend::ImportedMacroRecord imported;
        imported.name       = "twice";
        imported.span       = dep.declarations()[0].span;
        imported.parameters = dep.declarations()[0].parameters;
        imported.body       = dep.declarations()[0].body;
        imported.source     = &dep;

        auto snap = frontend::parseWithImports("fn main(): i32 { @twice(21) }", {imported});
        CHECK(snap.diagnostics().empty(), "imported macro expansion produces no diagnostics");
    }
    // 33. separate macro expansions receive distinct local identities
    {
        auto snap = frontend::parse("macro capture(value: expr) { let captured = value; }\n"
                                    "fn main() { @capture(1); @capture(2); }");
        std::vector<frontend::LocalId> locals;
        for (const auto &expr : snap.expressions()) {
            if (expr.kind != frontend::ExprKind::MacroCall || expr.text != "capture" ||
                !expr.expansion)
                continue;
            const auto &block = snap.expressions()[expr.expansion.value - 1U];
            for (const auto stmt_id : block.statements) {
                if (!stmt_id)
                    continue;
                const auto &stmt = snap.statements()[stmt_id.value - 1U];
                if (stmt.kind == frontend::StmtKind::Binding)
                    locals.push_back(stmt.binding.id);
            }
        }
        CHECK_EQ(locals.size(), 2U, "both macro expansions retain their local binding");
        if (locals.size() == 2U) {
            CHECK(locals[0], "first expansion receives a valid local identity");
            CHECK(locals[1], "second expansion receives a valid local identity");
            CHECK(locals[0] != locals[1], "macro expansions do not share local identities");
        }
    }
    // 34. empty and populated variadic tails parse and expand cleanly
    {
        auto snap = frontend::parse("macro call_all(headArg: val, names: identifier...) { "
                                    "names()...; }\n"
                                    "fn main() { @call_all(1); @call_all(2, first, second); }\n");
        CHECK(snap.diagnostics().empty(), "variadic macro calls parse and expand cleanly");
    }
    // 35. every argument in the variadic tail obeys its declared kind
    {
        auto snap = frontend::parse("macro call_all(names: identifier...) { names()...; }\n"
                                    "fn main() { @call_all(first, 2); @call_all(=named); }\n");
        size_t kindErrors = 0U;
        for (const auto &diagnostic : snap.diagnostics())
            kindErrors += diagnostic.code == diagnostics::err::MacroArgKind ? 1U : 0U;
        CHECK_EQ(kindErrors, 2U,
                 "identifier tails reject non-names and explicit unevaluated arguments");
    }
    // 36. variadic declarations must be unique and final
    {
        auto snap     = frontend::parse("macro not_last(names: identifier..., extra: expr) { }\n"
                                        "macro two_tails(a: expr..., b: identifier...) { }\n");
        size_t errors = 0U;
        for (const auto &diagnostic : snap.diagnostics())
            errors += diagnostic.code == diagnostics::err::MacroArity ? 1U : 0U;
        CHECK(errors >= 2U, "non-final and multiple variadic parameters are diagnosed");
    }
    // 37. variadic parameters accept only supported macro argument kinds
    {
        auto snap = frontend::parse("macro invalid_kind(values: expr...) { values()...; }\n");
        bool hasKindError = false;
        for (const auto &diagnostic : snap.diagnostics())
            hasKindError |= diagnostic.code == diagnostics::err::MacroArgKind;
        CHECK(hasKindError, "unsupported variadic argument kind is diagnosed");
    }
    // 38. val and ast variadic arguments enforce their call-site syntax
    {
        auto snap = frontend::parse("macro value_tail(values: val...) { consume(values)...; }\n"
                                    "macro ast_tail(nodes: ast...) { consume(nodes)...; }\n"
                                    "fn main() { @value_tail(1); @ast_tail(=2); "
                                    "@value_tail(=3); @ast_tail(4); }\n");
        size_t kindErrors = 0U;
        for (const auto &diagnostic : snap.diagnostics())
            kindErrors += diagnostic.code == diagnostics::err::MacroArgKind ? 1U : 0U;
        CHECK_EQ(kindErrors, 2U, "val rejects =expr and ast requires =expr");
    }
    // 39. fixed macro arguments remain required before the variadic tail
    {
        auto snap          = frontend::parse("macro with_fixed(head: val, tail: identifier...) { "
                                             "tail()...; }\n"
                                             "fn main() { @with_fixed(); }\n");
        bool hasArityError = false;
        for (const auto &diagnostic : snap.diagnostics())
            hasArityError |= diagnostic.code == diagnostics::err::MacroArity;
        CHECK(hasArityError, "missing fixed argument before a variadic tail is diagnosed");
    }
    // 40. a variadic parameter can only be used in a repeated expression statement
    {
        auto snap        = frontend::parse("macro bad_macro(names: identifier...) { names(); }\n"
                                           "fn main() { @bad_macro(item); }\n");
        bool hasUseError = false;
        for (const auto &diagnostic : snap.diagnostics())
            hasUseError |= diagnostic.code == diagnostics::err::MacroArgKind;
        if (!hasUseError) {
            for (const auto &diagnostic : snap.diagnostics())
                std::printf("  non-repeated variadic diagnostic: %s\n", diagnostic.message.c_str());
        }
        CHECK(hasUseError, "variadic parameter use without repetition is diagnosed");
    }
    // 41. repetition outside a macro template is rejected
    {
        auto snap            = frontend::parse("fn main() { invoke()...; }\n");
        bool hasContextError = false;
        for (const auto &diagnostic : snap.diagnostics())
            hasContextError |= diagnostic.code == diagnostics::err::MacroArgKind;
        CHECK(hasContextError, "statement repetition outside a macro is diagnosed");
    }
    // 42. imported macro metadata preserves the variadic parameter
    {
        auto dep = frontend::parse("pub macro call_all(names: identifier...) { names()...; }\n");
        frontend::ImportedMacroRecord imported;
        imported.name       = "call_all";
        imported.span       = dep.declarations()[0].span;
        imported.parameters = dep.declarations()[0].parameters;
        imported.body       = dep.declarations()[0].body;
        imported.source     = &dep;
        auto snap =
            frontend::parseWithImports("fn main() { @call_all(first, second); }\n", {imported});
        CHECK(snap.diagnostics().empty(), "imported variadic macro expands cleanly");
    }
    // 43. a plain named block parses with its descriptive name
    {
        auto snap = frontend::parse("fn main() {\n"
                                    "    setup: {\n"
                                    "    }\n"
                                    "}\n");
        bool found = false;
        for (const auto &expr : snap.expressions()) {
            if (expr.kind == frontend::ExprKind::Block && expr.block_name == "setup") {
                found = true;
                CHECK(!expr.marker, "a plain named block carries no marker");
            }
        }
        CHECK(found, "the 'setup' named block is parsed");
    }
    // 44. a marked named block parses with the marker MacroCall retained
    {
        auto snap = frontend::parse("#[scope] pub macro scoped(resources: identifier...) { }\n"
                                    "fn main() {\n"
                                    "    render: @scoped(a, b) {\n"
                                    "    }\n"
                                    "}\n");
        bool found = false;
        for (const auto &expr : snap.expressions()) {
            if (expr.kind != frontend::ExprKind::Block || expr.block_name != "render")
                continue;
            found = true;
            CHECK(static_cast<bool>(expr.marker), "the marked named block keeps its marker");
            if (expr.marker) {
                const auto &marker = snap.expressions()[expr.marker.value - 1U];
                CHECK_EQ(marker.kind, frontend::ExprKind::MacroCall,
                         "the marker is a MacroCall");
                CHECK_EQ(marker.text, std::string("scoped"), "the marker names the scope macro");
            }
            CHECK(expr.markerScopeAware, "the marker is recorded as scope-aware");
        }
        CHECK(found, "the 'render' marked named block is parsed");
    }
    // 45. #[scope] on a macro classifies as AttributeKind::Scope
    {
        auto snap = frontend::parse("#[scope] pub macro scoped(resources: identifier...) { }\n");
        bool classified = false;
        for (const auto &decl : snap.declarations()) {
            if (decl.kind != frontend::DeclKind::Macro)
                continue;
            for (const auto &attribute : decl.attributes)
                classified |= attribute.kind == frontend::AttributeKind::Scope;
        }
        CHECK(classified, "#[scope] classifies as AttributeKind::Scope");
    }
    // 46. a non-scope macro used as a marker is rejected by sema, not the parser
    {
        auto snap = frontend::parse("pub macro plain(x: identifier) { }\n"
                                    "fn main() {\n"
                                    "    render: @plain(a) {\n"
                                    "    }\n"
                                    "}\n");
        CHECK(snap.diagnostics().empty(),
              "the marker form parses for any macro; sema rejects the non-scope one");
    }
}

TEST_MAIN(macro)
