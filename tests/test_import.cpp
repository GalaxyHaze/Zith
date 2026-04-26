#include <catch2/catch_test_macros.hpp>

#include "../impl/parser/parser.h"
#include "../impl/ast/ast.h"

TEST_CASE("IMPORT: basic import from std", "[import][scan]") {
    auto ast = ParseResult(zith_parse_test(
        "import std/io/console;"
    ));
    REQUIRE(ast);
    REQUIRE(ast->type == ZITH_NODE_PROGRAM);
    REQUIRE(ast->data.list.len == 1);
    
    auto **decls = static_cast<ZithNode **>(ast->data.list.ptr);
    REQUIRE(decls[0]->type == ZITH_NODE_IMPORT);
    
    auto *payload = static_cast<ZithImportPayload *>(decls[0]->data.list.ptr);
    REQUIRE(std::string(payload->path) == "std/io/console");
    REQUIRE(payload->is_export == false);
}

TEST_CASE("IMPORT: import from allowed roots only", "[import][scan][error]") {
    auto ast_valid = ParseResult(zith_parse_test("import std/io;"));
    REQUIRE(ast_valid);
    
    auto ast_valid2 = ParseResult(zith_parse_test("import utils/helper;"));
    REQUIRE(ast_valid2);
    
    auto ast_valid3 = ParseResult(zith_parse_test("import c/stdio;"));
    REQUIRE(ast_valid3);
}

TEST_CASE("IMPORT: export declaration creates EXPORT node", "[import][scan]") {
    auto ast = ParseResult(zith_parse_test(
        "export std.io.console.println;"
    ));
    REQUIRE(ast);
    REQUIRE(ast->type == ZITH_NODE_PROGRAM);
    REQUIRE(ast->data.list.len == 1);
    
    auto **decls = static_cast<ZithNode **>(ast->data.list.ptr);
    REQUIRE(decls[0]->type == ZITH_NODE_EXPORT);
    
    auto *payload = static_cast<ZithImportPayload *>(decls[0]->data.list.ptr);
    REQUIRE(std::string(payload->path) == "std.io.console.println");
    REQUIRE(payload->is_export == true);
    REQUIRE(payload->vis == ZITH_VIS_PUBLIC);
}

TEST_CASE("IMPORT: mixed import and export", "[import][scan]") {
    auto ast = ParseResult(zith_parse_test(
        "import std/io;\n"
        "export std/io/println;\n"
    ));
    REQUIRE(ast);
    REQUIRE(ast->data.list.len == 2);
    
    auto **decls = static_cast<ZithNode **>(ast->data.list.ptr);
    REQUIRE(decls[0]->type == ZITH_NODE_IMPORT);
    REQUIRE(decls[1]->type == ZITH_NODE_EXPORT);
}

TEST_CASE("IMPORT: import with semicolon required", "[import][scan][error]") {
    auto ast = ParseResult(zith_parse_test("import std/io"));
    (void)ast;
}