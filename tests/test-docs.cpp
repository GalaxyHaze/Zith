#include "cli/docs/doc-model.hpp"
#include "cli/docs/markdown-renderer.hpp"
#include "cli/docs/output-writer.hpp"
#include "cli/docs/project-graph.hpp"
#include "cli/options.hpp"
#include "session/compilation-session.hpp"
#include "session/pipeline-plan.hpp"
#include "test-common.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

using namespace zith;

namespace {

namespace fs = std::filesystem;

struct Workspace {
    fs::path root = fs::temp_directory_path() /
                    ("zith-docs-tests-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    Workspace() {
        fs::create_directories(root);
    }

    ~Workspace() {
        fs::remove_all(root);
    }

    void write(const std::string &relative_path, const std::string &contents) const {
        const auto destination = root / relative_path;
        fs::create_directories(destination.parent_path());
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        output << contents;
    }

    void writeProjectManifest() const {
        write("ZithProject.toml", "[build]\n"
                                  "entry = \"src/main.zithm\"\n"
                                  "\n"
                                  "[paths]\n"
                                  "src_dir = [\"src\"]\n");
    }

    void writeGraphFixture() const {
        writeProjectManifest();
        write("src/main.zithm", "from middle\npub fn entry() { }\n");
        write("src/middle.zithm", "from leaf\npub fn middle_fn() { }\n");
        write("src/leaf.zithm", "pub fn leaf_fn() { }\n");
        write("src/orphan.zithm", "pub fn orphan_fn() { }\n");
    }
};

std::string readText(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

cli::docs::DocModel makeOutputModel() {
    cli::docs::DocModel model;
    model.entryModule = "src/main.zithm";

    cli::docs::DocSymbol reexport;
    reexport.kind         = frontend::DeclKind::Function;
    reexport.visibility   = frontend::Visibility::Public;
    reexport.name         = "exposed";
    reexport.signature    = "fn exposed(): i32";
    reexport.originModule = "vendor/dep.zithm";
    reexport.isReexport   = true;

    cli::docs::DocSymbol entry;
    entry.kind         = frontend::DeclKind::Function;
    entry.visibility   = frontend::Visibility::Public;
    entry.name         = "main";
    entry.signature    = "fn main(): i32";
    entry.originModule = "src/main.zithm";

    cli::docs::DocSymbol dependencySymbol = reexport;
    dependencySymbol.isReexport           = false;

    model.modules = {
        {"src/main.zithm", false, {entry, reexport}},
        {"vendor/dep.zithm", true, {dependencySymbol}},
    };
    return model;
}

void test_project_graph_contains_only_reachable_zith_modules() {
    Workspace workspace;
    workspace.writeGraphFixture();

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::Imported;

    session::CompilationSession compilation(options, workspace.root.string());
    compilation.setBuffered(true);
    CHECK(compilation.runTo(session::Stage::Imported),
          "configured project entry and its import closure compile through import");

    const auto &snapshot = compilation.snapshot();
    CHECK(snapshot != nullptr, "frontend snapshot is available after import");
    if (snapshot == nullptr)
        return;

    const auto graph = cli::docs::buildProjectGraph(*snapshot);
    CHECK(graph.ok(), "project graph builds without an error");
    CHECK_EQ(graph.entryModule, snapshot->rootModuleKey(), "graph entry is the snapshot root");
    CHECK_EQ(graph.modules.size(), 3u, "graph includes entry and two transitive imports");

    std::vector<std::string> keys;
    for (const auto &module : graph.modules) {
        keys.push_back(module.artifact->key);
        CHECK(!module.isDependency, "modules inside the workspace are project modules");
    }
    CHECK(std::find(keys.begin(), keys.end(), snapshot->rootModuleKey()) != keys.end(),
          "graph contains the configured entry module");
    CHECK(std::any_of(keys.begin(), keys.end(),
                      [](const std::string &key) { return key.ends_with("/middle.zithm"); }),
          "graph contains the direct import");
    CHECK(std::any_of(keys.begin(), keys.end(),
                      [](const std::string &key) { return key.ends_with("/leaf.zithm"); }),
          "graph contains the transitive import");
    CHECK(std::none_of(keys.begin(), keys.end(),
                       [](const std::string &key) { return key.ends_with("/orphan.zithm"); }),
          "graph excludes an unreachable source under src_dir");
}

void test_docs_defaults_to_configured_project_entry() {
    Workspace workspace;
    workspace.writeProjectManifest();
    workspace.write("src/main.zithm", "pub fn configured_entry() { }\n");

    const auto original_directory = fs::current_path();
    fs::current_path(workspace.root);

    char program[] = "zithc";
    char command[] = "docs";
    char *args[]   = {program, command};
    Cli cli;
    cli.parseArgs(2, args);
    cli.loadProject();
    const int result = cli.dispatch();

    fs::current_path(original_directory);
    CHECK_EQ(result, 0, "docs succeeds without an explicit input in a configured project");
    CHECK_EQ(cli.opts.inputFiles.size(), 1u, "dispatch supplies the discovered project root");
    if (!cli.opts.inputFiles.empty())
        CHECK(cli.opts.inputFiles[0] == workspace.root.string(),
              "docs receives the configured project root as its entry source");
}

void test_doc_model_preserves_docs_overloads_and_visibility_modes() {
    Workspace workspace;
    workspace.write("main.zithm", "/** Converts a value.\n"
                                 " * Keeps **Markdown** intact.\n"
                                 " *     example();\n"
                                 " */\n"
                                 "pub fn convert<T>(value: T): T { return value; }\n"
                                 "/// Private overload documentation.\n"
                                 "fn convert(value: i32): i32 { return value; }\n"
                                 "pub struct Box<T> { pub value: T, hidden: i32 }\n"
                                 "fn run(): i32 {\n"
                                 "    state Loop(n: i32): i32 { return n; }\n"
                                 "    return 0;\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::Imported;

    session::CompilationSession compilation(options, workspace.root.string() + "/main.zithm");
    compilation.setBuffered(true);
    CHECK(compilation.runTo(session::Stage::Imported),
          "document model fixture compiles through frontend import");

    const auto &snapshot = compilation.snapshot();
    CHECK(snapshot != nullptr, "document model fixture has a compilation snapshot");
    if (snapshot == nullptr)
        return;

    const auto graph = cli::docs::buildProjectGraph(*snapshot);
    CHECK(graph.ok(), "document model fixture has a valid project graph");
    const auto interface_model =
        cli::docs::buildDocModel(*snapshot, graph, cli::docs::DocsMode::Interface);
    const auto spec_model = cli::docs::buildDocModel(*snapshot, graph, cli::docs::DocsMode::Spec);

    CHECK_EQ(interface_model.modules.size(), 1u, "interface model has the entry module");
    CHECK_EQ(spec_model.modules.size(), 1u, "spec model has the entry module");
    if (interface_model.modules.empty() || spec_model.modules.empty())
        return;

    const auto &interface_symbols = interface_model.modules.front().symbols;
    const auto &spec_symbols      = spec_model.modules.front().symbols;
    CHECK_EQ(interface_symbols.size(), 2u, "interface includes public declarations only");
    CHECK_EQ(spec_symbols.size(), 5u,
             "spec includes private declarations and local state declarations");
    const auto findSymbol = [](const std::vector<cli::docs::DocSymbol> &symbols,
                               const std::string_view name) -> const cli::docs::DocSymbol * {
        const auto found =
            std::find_if(symbols.begin(), symbols.end(),
                         [&](const cli::docs::DocSymbol &symbol) { return symbol.name == name; });
        return found == symbols.end() ? nullptr : &*found;
    };
    const auto *convert = findSymbol(interface_symbols, "convert");
    const auto *box     = findSymbol(interface_symbols, "Box");
    CHECK(findSymbol(interface_symbols, "Loop") == nullptr,
          "interface omits private local state declarations");
    if (convert != nullptr) {
        CHECK_EQ(convert->name, std::string("convert"), "function name is retained");
        CHECK(convert->signature.find("<T>") != std::string::npos,
              "generic parameters are retained in the signature");
        CHECK(convert->signature.find("value: T") != std::string::npos,
              "parameter names and types are retained in the signature");
        CHECK(convert->documentation.find("**Markdown**") != std::string::npos,
              "DocBlock content is preserved as Markdown");
        CHECK(convert->documentation.find("    example();") != std::string::npos,
              "DocBlock normalization preserves Markdown code indentation");
    }
    CHECK(box != nullptr, "public struct is represented in the model");
    if (box != nullptr) {
        CHECK_EQ(box->members.size(), 1u, "interface exposes only public struct fields");
        if (!box->members.empty())
            CHECK_EQ(box->members.front().type, std::string("T"),
                     "generic struct field type is retained");
    }
    const auto *specBox = findSymbol(spec_symbols, "Box");
    if (specBox != nullptr)
        CHECK_EQ(specBox->members.size(), 2u, "spec retains private struct fields");
    const auto *localState = findSymbol(spec_symbols, "Loop");
    CHECK(localState != nullptr, "spec includes local state declarations");
    if (localState != nullptr)
        CHECK_EQ(localState->ownerName, std::string("run"),
                 "local state records its containing function");
    const bool hasLineDocumentation = std::any_of(
        spec_symbols.begin(), spec_symbols.end(), [](const cli::docs::DocSymbol &symbol) {
            return symbol.name == "convert" &&
                   symbol.documentation.find("Private overload documentation.") !=
                       std::string::npos;
        });
    CHECK(hasLineDocumentation, "DocLine content is preserved");
}

void test_doc_model_preserves_declaration_categories_and_members() {
    Workspace workspace;
    workspace.write("main.zithm", "pub type UserId = i32\n"
                                 "pub alias Callback = fn(i32): i32\n"
                                 "pub extern fn puts(msg: *char): i32\n"
                                 "pub raw macro swap(a: identifier, b: identifier) { }\n"
                                 "pub enum Color { Red, Green = 1 }\n"
                                 "pub union Number { i32, f64 }\n"
                                 "pub trait Printable { fn print(self); }\n"
                                 "pub interface Point { x: f32, y: f32, fn getX(self): f32; }\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::Imported;

    session::CompilationSession compilation(options, workspace.root.string() + "/main.zithm");
    compilation.setBuffered(true);
    CHECK(compilation.runTo(session::Stage::Imported),
          "declaration-category fixture parses through frontend import");
    const auto &snapshot = compilation.snapshot();
    CHECK(snapshot != nullptr, "declaration-category fixture has a compilation snapshot");
    if (snapshot == nullptr)
        return;

    const auto graph = cli::docs::buildProjectGraph(*snapshot);
    CHECK(graph.ok(), "declaration-category fixture has a valid project graph");
    const auto spec = cli::docs::buildDocModel(*snapshot, graph, cli::docs::DocsMode::Spec);
    const auto api  = cli::docs::buildDocModel(*snapshot, graph, cli::docs::DocsMode::Interface);
    CHECK_EQ(spec.modules.size(), 1u, "spec model contains the declaration fixture module");
    CHECK_EQ(api.modules.size(), 1u, "interface model contains the declaration fixture module");
    if (spec.modules.empty() || api.modules.empty())
        return;

    const auto findSymbol = [](const cli::docs::DocModule &module,
                               const std::string_view name) -> const cli::docs::DocSymbol * {
        const auto found =
            std::find_if(module.symbols.begin(), module.symbols.end(),
                         [&](const cli::docs::DocSymbol &symbol) { return symbol.name == name; });
        return found == module.symbols.end() ? nullptr : &*found;
    };
    const auto *userId    = findSymbol(spec.modules.front(), "UserId");
    const auto *alias     = findSymbol(spec.modules.front(), "Callback");
    const auto *puts      = findSymbol(spec.modules.front(), "puts");
    const auto *swap      = findSymbol(spec.modules.front(), "swap");
    const auto *color     = findSymbol(spec.modules.front(), "Color");
    const auto *number    = findSymbol(spec.modules.front(), "Number");
    const auto *printable = findSymbol(spec.modules.front(), "Printable");
    const auto *point     = findSymbol(api.modules.front(), "Point");

    CHECK(userId != nullptr, "nominal type alias is included");
    if (userId != nullptr) {
        CHECK_EQ(userId->signature, std::string("type UserId"),
                 "nominal alias is distinguished from a transparent alias");
        CHECK_EQ(userId->type, std::string("i32"), "nominal alias target type is retained");
    }
    CHECK(alias != nullptr, "transparent type alias is included");
    if (alias != nullptr) {
        CHECK_EQ(alias->signature, std::string("alias Callback"),
                 "transparent alias kind appears in its signature");
        CHECK_EQ(alias->type, std::string("fn(i32): i32"),
                 "function type alias target is retained");
    }
    CHECK(puts != nullptr && puts->isExtern,
          "extern declarations retain their external linkage marker");
    if (puts != nullptr) {
        CHECK(puts->signature.starts_with("extern fn puts("),
              "extern function signature retains parameters and type");
        CHECK(puts->signature.find("msg: *char") != std::string::npos &&
                  puts->signature.find(": i32") != std::string::npos,
              "extern function signature retains parameter and return types");
    }
    CHECK(swap != nullptr, "raw macro declaration is included");
    if (swap != nullptr)
        CHECK(swap->signature.starts_with("raw macro swap("),
              "raw macro modifier and parameters are retained");
    CHECK(color != nullptr, "enum declaration is included");
    if (color != nullptr) {
        CHECK_EQ(color->signature, std::string("enum Color"), "enum kind appears in its signature");
        CHECK_EQ(color->members.size(), 2u, "enum variants are retained");
        const auto red =
            std::find_if(color->members.begin(), color->members.end(),
                         [](const cli::docs::DocMember &member) { return member.name == "Red"; });
        const auto green =
            std::find_if(color->members.begin(), color->members.end(),
                         [](const cli::docs::DocMember &member) { return member.name == "Green"; });
        CHECK(red != color->members.end(), "enum includes the Red variant");
        CHECK(green != color->members.end(), "enum includes the Green variant");
        if (green != color->members.end())
            CHECK_EQ(green->defaultValue, std::string("1"), "enum variant immediate is retained");
    }
    CHECK(number != nullptr, "union declaration is included");
    if (number != nullptr) {
        CHECK_EQ(number->signature, std::string("union Number"),
                 "union kind appears in its signature");
        CHECK_EQ(number->members.size(), 2u, "union alternatives are retained");
        CHECK(std::any_of(number->members.begin(), number->members.end(),
                          [](const cli::docs::DocMember &member) { return member.name == "i32"; }),
              "union includes the i32 alternative with a readable type name");
        CHECK(std::any_of(number->members.begin(), number->members.end(),
                          [](const cli::docs::DocMember &member) { return member.name == "f64"; }),
              "union includes the f64 alternative with a readable type name");
    }
    CHECK(printable != nullptr, "trait declaration is included");
    if (printable != nullptr) {
        CHECK_EQ(printable->signature, std::string("trait Printable"),
                 "trait kind appears in its signature");
        CHECK_EQ(printable->members.size(), 1u, "trait method is attached to its owner");
        if (!printable->members.empty()) {
            CHECK(printable->members.front().signature.starts_with("fn print("),
                  "trait member retains its method signature");
            CHECK(printable->members.front().signature.find("self") != std::string::npos,
                  "trait member signature retains its receiver");
        }
    }
    CHECK(point != nullptr, "public interface declaration is included");
    if (point != nullptr) {
        CHECK_EQ(point->signature, std::string("interface Point"),
                 "interface kind appears in its signature");
        const auto fieldCount = std::count_if(
            point->members.begin(), point->members.end(),
            [](const cli::docs::DocMember &member) { return member.kind == "field"; });
        CHECK_EQ(fieldCount, 2, "interface mode retains all interface fields");
        const auto method = std::find_if(
            point->members.begin(), point->members.end(),
            [](const cli::docs::DocMember &member) { return member.kind == "method"; });
        CHECK(method != point->members.end(), "interface method is attached to its owner");
        if (method != point->members.end()) {
            CHECK(method->signature.starts_with("fn getX("),
                  "interface method retains its name and parameter list");
            CHECK(method->signature.find("self") != std::string::npos &&
                      method->signature.find(": f32") != std::string::npos,
                  "interface method retains its receiver and return type");
        }
        CHECK(
            std::none_of(api.modules.front().symbols.begin(), api.modules.front().symbols.end(),
                         [](const cli::docs::DocSymbol &symbol) { return symbol.name == "getX"; }),
            "interface methods are not duplicated as top-level symbols");
    }
}

void test_doc_model_preserves_implemented_methods_and_owners() {
    Workspace workspace;
    workspace.write("main.zithm", "pub struct Counter { value: i32 }\n"
                                 "implement Counter {\n"
                                 "    /// Returns the current value.\n"
                                 "    pub fn get(self): i32 { return self.value; }\n"
                                 "}\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::Imported;

    session::CompilationSession compilation(options, workspace.root.string() + "/main.zithm");
    compilation.setBuffered(true);
    CHECK(compilation.runTo(session::Stage::Imported),
          "implemented-method fixture parses through frontend import");
    const auto &snapshot = compilation.snapshot();
    CHECK(snapshot != nullptr, "implemented-method fixture has a compilation snapshot");
    if (snapshot == nullptr)
        return;

    const auto graph = cli::docs::buildProjectGraph(*snapshot);
    CHECK(graph.ok(), "implemented-method fixture has a valid project graph");
    const auto model = cli::docs::buildDocModel(*snapshot, graph, cli::docs::DocsMode::Interface);
    CHECK_EQ(model.modules.size(), 1u, "interface model contains the owner module");
    if (model.modules.empty())
        return;

    const auto method =
        std::find_if(model.modules.front().symbols.begin(), model.modules.front().symbols.end(),
                     [](const cli::docs::DocSymbol &symbol) { return symbol.name == "get"; });
    CHECK(method != model.modules.front().symbols.end(),
          "public method from an implement block is included");
    if (method != model.modules.front().symbols.end()) {
        CHECK_EQ(method->ownerName, std::string("Counter"),
                 "implemented method retains its owning type");
        CHECK(method->signature.starts_with("fn get("),
              "implemented method retains its complete function signature");
        CHECK_EQ(method->documentation, std::string("Returns the current value."),
                 "implemented method retains its documentation comment");
    }
}

void test_doc_model_limits_dependency_symbols_to_used_ones() {
    Workspace workspace;
    Workspace dependency_workspace;
    workspace.write("main.zithm", "from dep\n"
                                 "pub fn entry(): i32 { return used(); }\n");
    dependency_workspace.write("dep.zithm", "pub fn used(): i32 { return 1; }\n"
                                           "pub fn unused(): i32 { return 2; }\n"
                                           "fn private_dep(): i32 { return 3; }\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::Imported;
    options.includeDirs.push(dependency_workspace.root.string());

    session::CompilationSession compilation(options, workspace.root.string() + "/main.zithm");
    compilation.setBuffered(true);
    CHECK(compilation.runTo(session::Stage::Imported),
          "dependency usage fixture compiles through frontend import");
    const auto &snapshot = compilation.snapshot();
    CHECK(snapshot != nullptr, "dependency usage fixture has a snapshot");
    if (snapshot == nullptr)
        return;
    const auto *resolution = snapshot->findResolution(snapshot->rootModuleKey());
    CHECK(resolution != nullptr, "entry module has a name-resolution record");
    if (resolution == nullptr)
        return;
    const auto resolvedDependencyUses =
        std::count_if(resolution->expressions.begin(), resolution->expressions.end(),
                      [](const session::ResolvedName &name) {
                          return name.name == "used" && !name.target.module.empty() &&
                                 static_cast<bool>(name.target.localSymbol);
                      });
    CHECK_EQ(resolvedDependencyUses, 1,
             "expression resolution points at the imported dependency symbol");

    const auto graph      = cli::docs::buildProjectGraph(*snapshot);
    const auto model      = cli::docs::buildDocModel(*snapshot, graph, cli::docs::DocsMode::Spec);
    const auto dependency = std::find_if(
        model.modules.begin(), model.modules.end(),
        [](const cli::docs::DocModule &module) { return module.key.ends_with("/dep.zithm"); });
    CHECK(dependency != model.modules.end(), "dependency is present in the reachable graph");
    if (dependency == model.modules.end())
        return;
    CHECK(dependency->isDependency, "external include root is classified as a dependency");
    CHECK_EQ(dependency->symbols.size(), 1u, "spec emits only dependency symbols used by project");
    if (!dependency->symbols.empty())
        CHECK_EQ(dependency->symbols.front().name, std::string("used"),
                 "resolved call identifies the dependency symbol in use");
}

void test_doc_model_includes_dependency_types_used_in_signatures() {
    Workspace workspace;
    Workspace dependency_workspace;
    workspace.write("main.zithm", "from dep\n"
                                 "import dep as d\n"
                                 "pub fn entry(value: UsedType, items: []d.OtherUsedType): i32 {\n"
                                 "    return 0;\n"
                                 "}\n");
    dependency_workspace.write("dep.zithm", "pub struct UsedType { value: i32 }\n"
                                           "pub struct OtherUsedType { value: i32 }\n"
                                           "pub struct UnusedType { value: i32 }\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::Imported;
    options.includeDirs.push(dependency_workspace.root.string());

    session::CompilationSession compilation(options, workspace.root.string() + "/main.zithm");
    compilation.setBuffered(true);
    CHECK(compilation.runTo(session::Stage::Imported),
          "dependency type-usage fixture compiles through frontend import");
    const auto &snapshot = compilation.snapshot();
    CHECK(snapshot != nullptr, "dependency type-usage fixture has a snapshot");
    if (snapshot == nullptr)
        return;

    const auto graph      = cli::docs::buildProjectGraph(*snapshot);
    const auto model      = cli::docs::buildDocModel(*snapshot, graph, cli::docs::DocsMode::Spec);
    const auto dependency = std::find_if(
        model.modules.begin(), model.modules.end(),
        [](const cli::docs::DocModule &module) { return module.key.ends_with("/dep.zithm"); });
    CHECK(dependency != model.modules.end(), "type dependency is present in the reachable graph");
    if (dependency == model.modules.end())
        return;

    CHECK_EQ(dependency->symbols.size(), 2u,
             "spec includes only dependency types referenced by declaration signatures");
    CHECK(std::any_of(dependency->symbols.begin(), dependency->symbols.end(),
                      [](const cli::docs::DocSymbol &symbol) { return symbol.name == "UsedType"; }),
          "simple imported type reference selects its dependency symbol");
    CHECK(std::any_of(
              dependency->symbols.begin(), dependency->symbols.end(),
              [](const cli::docs::DocSymbol &symbol) { return symbol.name == "OtherUsedType"; }),
          "qualified nested type reference selects its dependency symbol");
}

void test_doc_model_records_reexports_with_their_origin() {
    Workspace workspace;
    workspace.write("main.zithm", "export middle\n");
    workspace.write("middle.zithm", "export dep\n");
    workspace.write("dep.zithm", "pub fn reexported(): i32 { return 1; }\n");

    memory::Arena arena;
    Options options(arena);
    options.targetStage = session::Stage::Imported;

    session::CompilationSession compilation(options, workspace.root.string() + "/main.zithm");
    compilation.setBuffered(true);
    CHECK(compilation.runTo(session::Stage::Imported),
          "reexport fixture compiles through frontend import");
    const auto &snapshot = compilation.snapshot();
    CHECK(snapshot != nullptr, "reexport fixture has a snapshot");
    if (snapshot == nullptr)
        return;

    const auto graph = cli::docs::buildProjectGraph(*snapshot);
    const auto model = cli::docs::buildDocModel(*snapshot, graph, cli::docs::DocsMode::Interface);
    const auto entry = std::find_if(
        model.modules.begin(), model.modules.end(),
        [&](const cli::docs::DocModule &module) { return module.key == graph.entryModule; });
    CHECK(entry != model.modules.end(), "entry module is present in the model");
    if (entry == model.modules.end())
        return;
    const auto reexport = std::find_if(entry->symbols.begin(), entry->symbols.end(),
                                       [](const cli::docs::DocSymbol &symbol) {
                                           return symbol.name == "reexported" && symbol.isReexport;
                                       });
    CHECK(reexport != entry->symbols.end(), "interface contains the reexported symbol");
    if (reexport != entry->symbols.end())
        CHECK(reexport->originModule.ends_with("/dep.zithm"),
              "reexport records the defining module as its origin");
}

void test_markdown_renderer_is_complete_and_deterministic() {
    cli::docs::DocModel model;
    model.entryModule = "src/main.zithm";

    cli::docs::DocSymbol sum;
    sum.kind          = frontend::DeclKind::Function;
    sum.visibility    = frontend::Visibility::Public;
    sum.name          = "sum";
    sum.signature     = "fn sum(left: i32, right: i32): i32";
    sum.documentation = "Sum two integers.";
    sum.originModule  = "src/math.zithm";
    sum.isReexport    = true;

    cli::docs::DocSymbol main;
    main.kind          = frontend::DeclKind::Function;
    main.visibility    = frontend::Visibility::Public;
    main.name          = "main";
    main.signature     = "fn main(): i32";
    main.documentation = "Entry point.";
    main.originModule  = "src/main.zithm";

    cli::docs::DocSymbol box;
    box.kind          = frontend::DeclKind::Struct;
    box.visibility    = frontend::Visibility::Public;
    box.name          = "Box";
    box.signature     = "Box<T>";
    box.documentation = "A value container.";
    box.originModule  = "src/main.zithm";
    box.members.push_back(
        {"field", "value", "T", {}, "Stored value.", frontend::Visibility::Public});

    cli::docs::DocSymbol mathSum = sum;
    mathSum.isReexport           = false;
    mathSum.originModule         = "src/math.zithm";

    model.modules = {
        {"src/math.zithm", true, {mathSum}},
        {"src/main.zithm", false, {box, sum, main}},
    };

    const std::string expected = "# API Reference\n"
                                 "\n"
                                 "Entry module: `src/main.zithm`\n"
                                 "\n"
                                 "## Reachable Modules\n"
                                 "\n"
                                 "- `src/main.zithm` (entry)\n"
                                 "- `src/math.zithm` (dependency)\n"
                                 "\n"
                                 "## Modules\n"
                                 "\n"
                                 "### `src/main.zithm` (entry)\n"
                                 "\n"
                                 "#### Functions\n"
                                 "\n"
                                 "##### `fn main(): i32`\n"
                                 "\n"
                                 "- Visibility: public\n"
                                 "- Origin: local to `src/main.zithm`\n"
                                 "\n"
                                 "Entry point.\n"
                                 "\n"
                                 "##### `fn sum(left: i32, right: i32): i32`\n"
                                 "\n"
                                 "- Visibility: public\n"
                                 "- Origin: re-exported from `src/math.zithm`\n"
                                 "\n"
                                 "Sum two integers.\n"
                                 "\n"
                                 "#### Structs\n"
                                 "\n"
                                 "##### `Box<T>`\n"
                                 "\n"
                                 "- Visibility: public\n"
                                 "- Origin: local to `src/main.zithm`\n"
                                 "\n"
                                 "A value container.\n"
                                 "\n"
                                 "###### field `value`: `T`\n"
                                 "\n"
                                 "- Visibility: public\n"
                                 "\n"
                                 "Stored value.\n"
                                 "\n"
                                 "### `src/math.zithm` (dependency)\n"
                                 "\n"
                                 "#### Functions\n"
                                 "\n"
                                 "##### `fn sum(left: i32, right: i32): i32`\n"
                                 "\n"
                                 "- Visibility: public\n"
                                 "- Origin: local to `src/math.zithm`\n"
                                 "\n"
                                 "Sum two integers.\n";

    const auto rendered = cli::docs::renderMarkdown(model);
    CHECK_EQ(rendered, expected, "renderer produces the complete stable Markdown contract");

    std::swap(model.modules[0], model.modules[1]);
    std::reverse(model.modules.front().symbols.begin(), model.modules.front().symbols.end());
    CHECK_EQ(cli::docs::renderMarkdown(model), rendered,
             "equivalent models render identically regardless of input order");
}

void test_output_writer_resolves_default_destinations() {
    Workspace project;
    project.writeProjectManifest();
    project.write("src/main.zithm", "pub fn main() { }\n");
    CHECK_EQ(cli::docs::resolveDocsOutputDirectory(project.root / "src/main.zithm"),
             project.root / "docs", "project input defaults to the project docs directory");

    Workspace isolated;
    isolated.write("main.zithm", "pub fn main() { }\n");
    CHECK_EQ(cli::docs::resolveDocsOutputDirectory(isolated.root / "main.zithm"),
             isolated.root / "docs", "isolated input defaults to a sibling docs directory");

    const auto explicitPath = project.root / "generated";
    CHECK_EQ(cli::docs::resolveDocsOutputDirectory(project.root / "src/main.zithm", explicitPath),
             explicitPath, "an explicit output path is used as the output directory");
}

void test_output_writer_enforces_overwrite_policy() {
    Workspace workspace;
    const auto outputDirectory = workspace.root / "generated";
    const auto apiPath         = outputDirectory / "API.md";
    const auto model           = makeOutputModel();

    const auto initialResult = cli::docs::writeDocsOutput(outputDirectory, model, false, false);
    CHECK(initialResult.ok, "writer creates the aggregate API.md output");
    CHECK_EQ(readText(apiPath), cli::docs::renderMarkdown(model),
             "API.md contains the renderer output byte for byte");

    {
        std::ofstream existing(apiPath, std::ios::binary | std::ios::trunc);
        existing << "preserve this file\n";
    }
    const auto refusedResult = cli::docs::writeDocsOutput(outputDirectory, model, false, false);
    CHECK(!refusedResult.ok, "writer refuses to overwrite an existing output without force");
    CHECK_EQ(readText(apiPath), std::string("preserve this file\n"),
             "refused overwrite preserves existing contents");

    const std::string errorMarkdown = "## Errors\n\n- [0..0] `example diagnostic`\n\n";
    const auto forcedResult =
        cli::docs::writeDocsOutput(outputDirectory, model, false, true, errorMarkdown);
    CHECK(forcedResult.ok, "force permits replacing the generated API.md");
    CHECK_EQ(readText(apiPath), cli::docs::renderMarkdown(model) + errorMarkdown,
             "forced API.md contains the document and optional error section");
}

void test_output_writer_index_preserves_unrelated_files_and_detects_collisions() {
    Workspace workspace;
    const auto outputDirectory = workspace.root / "generated";
    const auto entryPage       = outputDirectory / "modules/module-src-main-zithm.md";
    fs::create_directories(entryPage.parent_path());
    const auto unrelatedPath = outputDirectory / "notes.txt";
    {
        std::ofstream unrelated(unrelatedPath, std::ios::binary | std::ios::trunc);
        unrelated << "keep me\n";
    }
    {
        std::ofstream existing(entryPage, std::ios::binary | std::ios::trunc);
        existing << "old generated page\n";
    }

    const auto model   = makeOutputModel();
    const auto refused = cli::docs::writeDocsOutput(outputDirectory, model, true, false);
    CHECK(!refused.ok, "index mode refuses existing pages without force");
    CHECK(!fs::exists(outputDirectory / "README.md"),
          "overwrite refusal occurs before creating the index");
    CHECK_EQ(readText(entryPage), std::string("old generated page\n"),
             "overwrite refusal preserves an existing module page");

    const std::string errorMarkdown = "## Errors\n\n- [0..0] `example diagnostic`\n\n";
    const auto result =
        cli::docs::writeDocsOutput(outputDirectory, model, true, true, errorMarkdown);
    CHECK(result.ok, "force writes an index and one page per module");
    CHECK(fs::exists(outputDirectory / "README.md"), "index mode creates README.md");
    CHECK(fs::exists(entryPage), "index mode creates a safe entry-module page");
    CHECK(fs::exists(outputDirectory / "modules/module-vendor-dep-zithm.md"),
          "index mode creates a safe dependency page");
    const auto readme = readText(outputDirectory / "README.md");
    CHECK(readme.find("modules/module-src-main-zithm.md") != std::string::npos,
          "README links to generated module pages");
    CHECK(readme.find("exposed") != std::string::npos, "README includes re-exported symbol links");
    CHECK(readme.find("## Errors") != std::string::npos,
          "index output includes the optional error section");
    CHECK_EQ(readText(unrelatedPath), std::string("keep me\n"),
             "index generation leaves unrelated files untouched");

    cli::docs::DocModel collidingModel;
    collidingModel.entryModule = "alpha/beta";
    collidingModel.modules     = {
        {"alpha/beta", false, {}},
        {"alpha-beta", false, {}},
    };
    const auto collisionDirectory = workspace.root / "collision";
    const auto collision =
        cli::docs::writeDocsOutput(collisionDirectory, collidingModel, true, true);
    CHECK(!collision.ok, "sanitized module filename collisions fail the output plan");
    CHECK(!fs::exists(collisionDirectory),
          "filename collisions are detected before creating output directories");
}

} // namespace

static void test_docs() {
    test_project_graph_contains_only_reachable_zith_modules();
    test_docs_defaults_to_configured_project_entry();
    test_doc_model_preserves_docs_overloads_and_visibility_modes();
    test_doc_model_preserves_declaration_categories_and_members();
    test_doc_model_preserves_implemented_methods_and_owners();
    test_doc_model_limits_dependency_symbols_to_used_ones();
    test_doc_model_includes_dependency_types_used_in_signatures();
    test_doc_model_records_reexports_with_their_origin();
    test_markdown_renderer_is_complete_and_deterministic();
    test_output_writer_resolves_default_destinations();
    test_output_writer_enforces_overwrite_policy();
    test_output_writer_index_preserves_unrelated_files_and_detects_collisions();
}

TEST_MAIN(docs)
