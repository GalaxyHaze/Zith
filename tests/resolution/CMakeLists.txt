add_executable(resolution-test
    resolution-test.cpp
)

target_link_libraries(resolution-test PRIVATE
    zct_session
    zct_resolution
    zct_import
    zct_symbols
    zct_sema
    zct_frontend_parser
    zct_frontend_lexer
    zct_frontend_ast
    zct_common
)

target_include_directories(resolution-test PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
    "${PROJECT_SOURCE_DIR}/src/frontend/lexer"
    "${PROJECT_BINARY_DIR}/src/frontend/lexer"
    "${PROJECT_SOURCE_DIR}/src/frontend/parser"
    "${PROJECT_BINARY_DIR}/src/frontend/parser"
    "${PROJECT_SOURCE_DIR}/src/frontend/ast"
    "${PROJECT_BINARY_DIR}/src/frontend/ast"
)

add_test(
    NAME resolution-generated-basics
    COMMAND resolution-test
)
