#include "ide/lsp-ide-api.hpp"
#include "test-common.hpp"

#include <string_view>

namespace {

bool hasLabel(const std::vector<zith::ide::CompletionItem> &items,
              std::string_view label) {
    for (const auto &item : items) {
        if (item.label == label) {
            return true;
        }
    }
    return false;
}

} // namespace

void test_ide_api_named_blocks_in_outline();

void test_ide_api() {
    zith::ide::WorkspaceConfig config;
    config.workspaceRoot = "/tmp/zith-ide-tests";
    config.stdlibPath = ZITH_STDLIB_DIR;

    zith::ide::Workspace workspace(config);
    const auto snapshot =
        workspace.openDocument("file:///tmp/main.zithm", "/tmp/main.zithm",
                               "struct Counter {\n"
                               "  value: i32,\n"
                               "  private_field: i32,\n"
                               "  fn bump(self, by: i32): i32 { return self -> value + by; }\n"
                               "}\n"
                               "enum Color { Red, Green, Blue }\n"
                               "fn pick(c: Color): Color { return c; }\n"
                               "fn helper(x: i32): i32 { return x; }\n"
                               "state Worker(): i32 { return 42; }\n"
                               "fn main(): i32 {\n"
                               "  let counter = Counter { value: 1, private_field: 2 };\n"
                               "  let doubled = counter.bump(2);\n"
                               "  let color = Color.Red;\n"
                               "  let matched = pick(color);\n"
                               "  _ = helper(doubled);\n"
                               "  return dock Worker();\n"
                               "}\n",
                               1);

    zith::ide::Query query;
    const auto result = workspace.analyze(snapshot, query);

    CHECK(result.kind == zith::ide::QueryKind::Diagnostics,
          "diagnostics query is preserved in the result");
    CHECK(result.ok, "well-formed snapshot analyzes without errors");
    CHECK_EQ(result.analyzedRevision, snapshot.revision,
             "analyzed revision matches the in-memory document revision");
    for (const auto &diag : result.diagnostics) {
        std::printf("  IDE diagnostic: %s\n", diag.message.c_str());
    }
    CHECK(result.diagnostics.empty(), "open document has no diagnostics");
    CHECK_EQ(result.error, std::string(), "no error text is reported");

    zith::ide::Query symbols_query;
    symbols_query.kind = zith::ide::QueryKind::DocumentSymbols;
    const auto symbols = workspace.analyze(snapshot, symbols_query);
    CHECK(symbols.ok, "document symbols query succeeds");
    CHECK(!symbols.documentSymbols.empty(), "document symbols are collected");
    bool counter_children = false;
    for (const auto &symbol : symbols.documentSymbols) {
        if (symbol.name == "Counter" && !symbol.children.empty()) {
            counter_children = true;
            break;
        }
    }
    CHECK(counter_children, "document symbols include struct members");

    zith::ide::Query completion_query;
    completion_query.kind = zith::ide::QueryKind::Completion;
    completion_query.line = 3;
    completion_query.character = 1;
    const auto completions = workspace.analyze(snapshot, completion_query);
    CHECK(completions.ok, "completion query succeeds");
    CHECK(!completions.completions.empty(), "completion items are collected");
    bool saw_keyword = false;
    for (const auto &item : completions.completions) {
        if (item.label == "state") {
            saw_keyword = true;
            break;
        }
    }
    CHECK(saw_keyword, "state keyword is exposed by the facade");

    zith::ide::Query member_query;
    member_query.kind = zith::ide::QueryKind::Completion;
    member_query.line = 11;
    member_query.character = 24;
    const auto member = workspace.analyze(snapshot, member_query);
    CHECK(member.ok, "member completion query succeeds");
    CHECK(hasLabel(member.completions, "bump"),
          "member completion exposes the owned method");
    CHECK(hasLabel(member.completions, "value"),
          "member completion exposes the public field");
    CHECK(hasLabel(member.completions, "private_field"),
          "member completion exposes a same-module private field");

    zith::ide::Query resolve_query;
    resolve_query.kind = zith::ide::QueryKind::CompletionResolve;
    resolve_query.dataKind = "method";
    resolve_query.dataName = "bump";
    resolve_query.dataOwner = "Counter";
    const auto resolve = workspace.analyze(snapshot, resolve_query);
    CHECK(resolve.ok, "completion resolve query succeeds");
    CHECK(!resolve.completions.empty(),
          "completion resolve returns a completion item");
    CHECK(!resolve.completions.front().documentation.empty(),
          "completion resolve returns documentation");

    zith::ide::Query hover_query;
    hover_query.kind = zith::ide::QueryKind::Hover;
    hover_query.line = 11;
    hover_query.character = 19;
    const auto hover = workspace.analyze(snapshot, hover_query);
    CHECK(hover.ok, "hover query succeeds");
    CHECK(hover.hover.has_value(), "hover result is produced");
    CHECK(hover.hover->markdown.find("variable counter") != std::string::npos,
          "hover describes the local variable");
    CHECK(hover.hover->markdown.find("type: Counter") != std::string::npos,
          "hover reports the inferred type");

    zith::ide::Query signature_query;
    signature_query.kind = zith::ide::QueryKind::SignatureHelp;
    signature_query.line = 13;
    signature_query.character = 26;
    const auto signature = workspace.analyze(snapshot, signature_query);
    CHECK(signature.ok, "signature help query succeeds");
    CHECK(signature.signatureHelp.has_value(),
          "signature help is produced");
    CHECK(!signature.signatureHelp->signatures.empty(),
          "signature help contains a signature");

    zith::ide::Query inlay_query;
    inlay_query.kind = zith::ide::QueryKind::InlayHints;
    const auto inlays = workspace.analyze(snapshot, inlay_query);
    CHECK(inlays.ok, "inlay hint query succeeds");
    bool saw_type = false;
    bool saw_parameter = false;
    for (const auto &hint : inlays.inlayHints) {
        if (hint.label.rfind(": ", 0) == 0) {
            saw_type = true;
        }
        if (hint.label == "by:") {
            saw_parameter = true;
        }
    }
    CHECK(saw_type, "inlay hints include binding types");
    CHECK(saw_parameter, "inlay hints include call parameter names");

    zith::ide::Query highlight_query;
    highlight_query.kind = zith::ide::QueryKind::Highlights;
    highlight_query.line = 9;
    highlight_query.character = 6;
    const auto highlights = workspace.analyze(snapshot, highlight_query);
    CHECK(highlights.ok, "highlight query succeeds");
    CHECK(!highlights.highlights.empty(),
          "identifier highlights are collected");

    zith::ide::Query folding_query;
    folding_query.kind = zith::ide::QueryKind::Folding;
    const auto folding = workspace.analyze(snapshot, folding_query);
    CHECK(folding.ok, "folding query succeeds");
    CHECK(!folding.folding.empty(), "folding ranges are collected");

    zith::ide::Query definition_query;
    definition_query.kind = zith::ide::QueryKind::Definition;
    definition_query.line = 13;
    definition_query.character = 17;
    const auto definition = workspace.analyze(snapshot, definition_query);
    CHECK(definition.ok, "definition query succeeds");
    CHECK(!definition.definitions.empty(), "definition location is collected");

    test_ide_api_named_blocks_in_outline();
}

void test_ide_api_named_blocks_in_outline() {
    zith::ide::WorkspaceConfig config;
    config.workspaceRoot = "/tmp/zith-ide-named-blocks-tests";
    config.stdlibPath = ZITH_STDLIB_DIR;

    zith::ide::Workspace workspace(config);
    const auto snapshot = workspace.openDocument(
        "file:///tmp/named.zithm", "/tmp/named.zithm",
        "pub trait Guard { fn leave(var self): void; }\n"
        "pub trait Scope { fn enter(var self): dyn Guard; }\n"
        "struct RG { }\n"
        "struct Renderer { }\n"
        "implement RG as Guard { fn leave(var self): void { } }\n"
        "implement Renderer as Scope {\n"
        "    fn enter(var self): dyn Guard { return RG { }; }\n"
        "}\n"
        "#[scope] pub macro scoped(resources: identifier...) { }\n"
        "fn main() {\n"
        "    setup: {\n"
        "        var v: i32 = 1;\n"
        "        _ = v;\n"
        "    }\n"
        "    var r = Renderer { };\n"
        "    render: @scoped(r) {\n"
        "    }\n"
        "}\n",
        1);

    zith::ide::Query symbols_query;
    symbols_query.kind = zith::ide::QueryKind::DocumentSymbols;
    const auto symbols = workspace.analyze(snapshot, symbols_query);
    CHECK(symbols.ok, "document symbols query succeeds for a body with named blocks");
    bool saw_setup  = false;
    bool saw_render = false;
    for (const auto &symbol : symbols.documentSymbols) {
        if (symbol.name != "main") {
            continue;
        }
        for (const auto &child : symbol.children) {
            if (child.name == "setup" && child.detail.empty()) {
                saw_setup = true;
            }
            if (child.name == "render" && child.detail == "@scoped(r)") {
                saw_render = true;
            }
        }
    }
    CHECK(saw_setup, "the outline lists a plain named block as a function child");
    CHECK(saw_render, "the outline shows the marker resources on a named block");
}

TEST_MAIN(ide_api)
