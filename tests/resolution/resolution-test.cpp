#include "resolution/resolution.hpp"
#include "session/session.hpp"

#include "frontend/parser/parse.hpp"
#include "frontend/lexer/lexer.hpp"
#include "frontend/parser/parser.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

using common::memory::Arena;
using common::memory::SourceMap;
using common::memory::StringInterner;
using toolkit::resolution::ImportInfo;
using toolkit::resolution::ResolvedInfo;
using toolkit::resolution::ScanInfo;
using toolkit::session::CompilationSession;
using toolkit::session::Stage;
using toolkit::session::ZithSessionContext;
using toolkit::symbols::SymKind;
using toolkit::symbols::SymbolVisibilityKind;

namespace {

bool check(bool condition, std::string_view message) {
    if (!condition)
        std::fprintf(stderr, "FAIL: %.*s\n",
                     static_cast<int>(message.size()), message.data());
    return condition;
}

sample::ParseOutput parseCopy(Arena &arena, StringInterner &interner,
                              std::string_view source) {
    (void)interner;
    auto tokens = generated_lexer::tokenize(source);
    generated_parser::Parser<sample::ParseOutput> parser(arena);
    return hooks::parser::parseSource(parser, tokens, source);
}

bool recordHas(const ScanInfo &info, std::string_view name,
               SymKind kind, SymbolVisibilityKind visibility) {
    const auto *record = info.lookup(name);
    return record != nullptr && record->kind == kind &&
           record->visibility.kind == visibility;
}

bool ensureMakeDirs(const std::filesystem::path &dir) {
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    return !error;
}

bool writeFile(const std::filesystem::path &path, std::string_view content) {
    std::ofstream out(path);
    out << content;
    return static_cast<bool>(out);
}

bool testScanner() {
    Arena arena;
    StringInterner interner(arena);
    ScanInfo info(arena);
    const auto source =
        "fn local_fn() { }\n"
        "pub fn global_fn() { }\n"
        "pub(0..) fn sibling_fn() { }\n"
        "struct Point { }\n"
        "import helper;";
    auto output = parseCopy(arena, interner, source);
    if (output.ast.root == nullptr || !output.diagnostics.empty())
        return check(false, "scanner source parses");

    const auto result =
        toolkit::resolution::scanProgram(*output.ast.root, 7, info);
    if (result.isError())
        return check(false, "scanner succeeds");
    if (info.records.size() != 4)
        return check(false, "scanner records non-import declarations");
    if (!recordHas(info, "local_fn", SymKind::Fn, SymbolVisibilityKind::Private) ||
        !recordHas(info, "global_fn", SymKind::Fn, SymbolVisibilityKind::Public) ||
        !recordHas(info, "sibling_fn", SymKind::Fn, SymbolVisibilityKind::Module) ||
        !recordHas(info, "Point", SymKind::Struct, SymbolVisibilityKind::Private)) {
        for (const auto &record : info.records)
            std::fprintf(stderr, "scan record %.*s kind=%d vis=%d\n",
                         static_cast<int>(record.name.size()),
                         record.name.data(),
                         static_cast<int>(record.kind),
                         static_cast<int>(record.visibility.kind));
        return check(false, "scanner records visibility and kind");
    }
    if (info.lookup("helper") != nullptr)
        return check(false, "scanner skips import declarations");

    ScanInfo duplicate(arena);
    const std::string duplicateSource = "struct Twice { }\nstruct Twice { }\n";
    auto repeated = parseCopy(arena, interner, duplicateSource);
    const auto duplicateResult =
        toolkit::resolution::scanProgram(*repeated.ast.root, 8, duplicate);
    if (duplicateResult.isOk() ||
        duplicateResult.error().msg.find("duplicate declaration") ==
            std::string::npos)
        return check(false, "duplicate declarations fail scanning");
    return true;
}

bool testImports() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "zith-resolution-imports-test";
    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    cleanupError.clear();
    if (!ensureMakeDirs(root / "lib"))
        return check(false, "create import test directories");

    if (!writeFile(root / "app.zith", "import lib/a;\n")
        || !writeFile(root / "lib" / "a.zith",
                      "import lib/child;\npub fn fromA() { }\n")
        || !writeFile(root / "lib" / "child.zith",
                      "pub fn fromChild() { }\n"))
        return check(false, "write import test files");

    Arena arena;
    StringInterner interner(arena);
    SourceMap sourceMap;
    ImportInfo info(arena, interner);
    const std::string rootPath = (root / "app.zith").string();
    const auto rootFile = sourceMap.addFile(rootPath, "import lib/a;\n");
    if (!rootFile)
        return check(false, "add root source to import map");
    auto rootOutput = parseCopy(arena, interner, "import lib/a;\n");
    const auto result = toolkit::resolution::importProgram(
        rootOutput, rootFile.value(), root.string(), sourceMap, info);
    if (result.isError()) {
        std::fprintf(stderr, "import failed: %s\n",
                     result.error().msg.c_str());
        return false;
    }
    if (info.module("zith-resolution-imports-test") == nullptr ||
        info.module("zith-resolution-imports-test/lib/a") == nullptr ||
        info.module("zith-resolution-imports-test/lib/child") == nullptr)
        return check(false, "recursive imports create modules");
    if (info.outputs.size() != 3)
        return check(false, "recursive imports load all files");
    if (info.deferredImports.size() != 0)
        return check(false, "normal imports are not deferred");
    return true;
}

bool testMissingImport() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "zith-resolution-missing-test";
    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    cleanupError.clear();
    if (!ensureMakeDirs(root))
        return check(false, "create missing-import test directory");
    if (!writeFile(root / "app.zith", "import nope;\n"))
        return check(false, "write missing-import root");

    Arena arena;
    StringInterner interner(arena);
    SourceMap sourceMap;
    ImportInfo info(arena, interner);
    const std::string rootPath = (root / "app.zith").string();
    const auto rootFile = sourceMap.addFile(rootPath, "import nope;\n");
    auto rootOutput = parseCopy(arena, interner, "import nope;\n");
    const auto result = toolkit::resolution::importProgram(
        rootOutput, rootFile.value(), root.string(), sourceMap, info);
    if (result.isOk() ||
        result.error().msg.find("failed to load import") == std::string::npos)
        return check(false, "missing imports fail importing");
    return true;
}

bool testCircularImport() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        "zith-resolution-circular-test";
    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    cleanupError.clear();
    if (!ensureMakeDirs(root))
        return check(false, "create circular-import test directory");
    if (!writeFile(root / "app.zith", "import a;\n")
        || !writeFile(root / "a.zith", "import b;\n")
        || !writeFile(root / "b.zith", "import a;\n"))
        return check(false, "write circular import files");

    Arena arena;
    StringInterner interner(arena);
    SourceMap sourceMap;
    ImportInfo info(arena, interner);
    const std::string rootPath = (root / "app.zith").string();
    const auto rootFile = sourceMap.addFile(rootPath, "import a;\n");
    auto rootOutput = parseCopy(arena, interner, "import a;\n");
    const auto result = toolkit::resolution::importProgram(
        rootOutput, rootFile.value(), root.string(), sourceMap, info);
    if (result.isOk() ||
        result.error().msg.find("circular import") == std::string::npos)
        return check(false, "circular imports fail finalization");
    return true;
}

std::filesystem::path makeResolverRoot(std::string_view leaf) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        std::string(leaf.data(), leaf.size());
    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    cleanupError.clear();
    std::filesystem::create_directories(root, cleanupError);
    return root;
}

bool resolveRootWithChild(std::string_view childSource,
                          std::string_view symbol,
                          bool expectVisible,
                          std::string_view message) {
    const std::filesystem::path root =
        makeResolverRoot("zith-resolver-scope");
    if (!writeFile(root / "app.zith", "import child;\n"))
        return check(false, "write resolver root");
    if (!writeFile(root / "child.zith", childSource))
        return check(false, "write resolver child");

    Arena arena;
    StringInterner interner(arena);
    SourceMap sourceMap;
    ImportInfo info(arena, interner);
    const std::string rootPath = (root / "app.zith").string();
    const auto rootFile = sourceMap.loadFile(rootPath);
    if (!rootFile)
        return check(false, "load resolver root");
    auto rootOutput =
        parseCopy(arena, interner, "import child;\n");
    const auto imported = toolkit::resolution::importProgram(
        rootOutput, rootFile.value(), root.string(), sourceMap, info);
    if (imported.isError())
        return check(false, "resolver import succeeds");

    ResolvedInfo resolved(arena);
    const auto result =
        toolkit::resolution::resolveModules(info, resolved);
    if (result.isError())
        return check(false, "resolver succeeds");
    const auto *symbolFound =
        resolved.lookup(root.filename().string(), symbol);
    return check((symbolFound != nullptr) == expectVisible, message);
}

bool resolveSiblingScope(std::string_view siblingSource,
                         std::string_view symbol,
                         bool childSees,
                         bool rootSees,
                         std::string_view message) {
    const std::filesystem::path root =
        makeResolverRoot("zith-resolver-siblings");
    if (!writeFile(root / "app.zith", "import child;\n")
        || !writeFile(root / "child.zith", "import sibling;\n")
        || !writeFile(root / "sibling.zith", siblingSource))
        return check(false, "write sibling resolver files");

    Arena arena;
    StringInterner interner(arena);
    SourceMap sourceMap;
    ImportInfo info(arena, interner);
    const std::string rootPath = (root / "app.zith").string();
    const auto rootFile = sourceMap.loadFile(rootPath);
    if (!rootFile)
        return check(false, "load sibling resolver root");
    auto rootOutput = parseCopy(arena, interner, "import child;\n");
    const auto imported = toolkit::resolution::importProgram(
        rootOutput, rootFile.value(), root.string(), sourceMap, info);
    if (imported.isError())
        return check(false, "sibling resolver import succeeds");

    ResolvedInfo resolved(arena);
    const auto result = toolkit::resolution::resolveModules(info, resolved);
    if (result.isError())
        return check(false, "sibling resolver succeeds");
    const std::string rootName = root.filename().string();
    const std::string childName = rootName + "/child";
    if (!check((resolved.lookup(childName, symbol) != nullptr) == childSees,
               message))
        return false;
    return check((resolved.lookup(rootName, symbol) != nullptr) == rootSees,
                 message);
}

bool testVisibility() {
    if (!resolveRootWithChild("pub fn visible() { }\n", "visible", true,
                              "child pub is visible to project root"))
        return false;
    if (!resolveRootWithChild("fn hidden() { }\n", "hidden", false,
                              "private child declaration is invisible"))
        return false;
    if (!resolveRootWithChild("pub(=..0) fn parentOnly() { }\n",
                              "parentOnly", true,
                              "pub(=..0) direct parent visibility allows root"))
        return false;
    if (!resolveRootWithChild("pub(=..=) fn childOnly() { }\n",
                              "childOnly", false,
                              "child pub(=..=) is not visible to parent"))
        return false;
    if (!resolveSiblingScope("pub(0..) fn upward() { }\n", "upward",
                             true, false,
                             "pub(0..) allows same-root siblings"))
        return false;
    if (!resolveSiblingScope("pub(0..=) fn lateral() { }\n", "lateral",
                             true, false,
                             "pub(0..=) sibling-only visibility"))
        return false;
    return true;
}

bool testSelectors() {
    const std::filesystem::path root =
        makeResolverRoot("zith-resolution-selectors");
    if (!writeFile(root / "app.zith",
                   "from child { target as show };\n")
        || !writeFile(root / "child.zith",
                      "pub fn target() { }\npub fn hidden() { }\n"))
        return check(false, "write selector test files");

    Arena arena;
    StringInterner interner(arena);
    SourceMap sourceMap;
    ImportInfo info(arena, interner);
    const std::string rootPath = (root / "app.zith").string();
    const auto rootFile = sourceMap.loadFile(rootPath);
    if (!rootFile)
        return check(false, "load selector root");
    auto rootOutput = parseCopy(arena, interner,
                                "from child { target as show };\n");
    const auto imported = toolkit::resolution::importProgram(
        rootOutput, rootFile.value(), root.string(), sourceMap, info);
    if (imported.isError())
        return check(false, "selector import succeeds");
    ResolvedInfo resolved(arena);
    const auto result = toolkit::resolution::resolveModules(info, resolved);
    if (result.isError())
        return check(false, "selector resolution succeeds");
    const std::string rootName = root.filename().string();
    if (resolved.lookup(rootName, "show") == nullptr)
        return check(false, "from selector alias exposes selected public symbol");
    if (resolved.lookup(rootName, "target") != nullptr ||
        resolved.lookup(rootName, "hidden") != nullptr)
        return check(false, "from selectors hide unselected and unaliased names");
    return true;
}

bool expectSessionResolved() {
    const std::filesystem::path root =
        makeResolverRoot("zith-resolution-session");
    if (!writeFile(root / "app.zith", "pub fn rootFn() { }\nimport lib;\n")
        || !writeFile(root / "lib.zith", "pub fn fromLib() { }\n"))
        return check(false, "write session import file");

    ZithSessionContext context;
    const std::string rootPath = (root / "app.zith").string();
    const std::string projectRootString = root.string();
    const auto rootFile = context.sourceMap.loadFile(rootPath);
    if (!rootFile) {
        return check(false, "session addFile fails");
    }
    context.fileId = rootFile.value();
    context.filePath = rootPath;
    context.projectRoot = projectRootString;

    CompilationSession session(context);
    const auto result = session.runTo(Stage::Resolved);
    if (!result) {
        std::fprintf(stderr, "session resolved failed: %s\n",
                     result.error().msg.c_str());
        return false;
    }
    const std::string rootName = root.filename().string();
    if (context.imports.module(rootName) == nullptr ||
        context.imports.module(rootName + "/lib") == nullptr)
        return check(false, "Imported stores root and imported modules");
    if (context.resolved.lookup(rootName, "fromLib") == nullptr)
        return check(false, "Resolved exposes imported lib symbol to root");
    if (context.scan.lookup("rootFn") == nullptr)
        return check(false, "Scanned stores root scan info");
    return check(context.resolved.lookup(rootName, "rootFn") != nullptr,
                 "Resolved keeps local symbols visible");
}

} // namespace

int main() {
    bool ok = true;
    ok &= testScanner();
    ok &= testImports();
    ok &= testMissingImport();
    ok &= testCircularImport();
    ok &= testVisibility();
    ok &= testSelectors();
    ok &= expectSessionResolved();
    if (!ok)
        return EXIT_FAILURE;
    std::cout << "resolution-test: scan, import, and resolve checks passed\n";
    return EXIT_SUCCESS;
}
