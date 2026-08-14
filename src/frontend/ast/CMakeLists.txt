set(_zct_frontend_ast_generated
    "${CMAKE_BINARY_DIR}/src/frontend/ast/ast.cpp"
    "${CMAKE_BINARY_DIR}/src/frontend/ast/ast.hpp"
    "${CMAKE_BINARY_DIR}/src/frontend/ast/walk.hpp"
)

add_custom_command(
    OUTPUT ${_zct_frontend_ast_generated}
    BYPRODUCTS "${CMAKE_BINARY_DIR}/src/frontend/ast/.gitignore"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/ast.rules"
            --out "${CMAKE_BINARY_DIR}/src/frontend/ast"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/ast.rules"
        "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/types.hpp"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/__init__.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/errors.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/runtime.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/text.py"
    VERBATIM
    COMMENT "Generating frontend AST sources from rules"
)

add_custom_target(zct_frontend_ast_generated DEPENDS ${_zct_frontend_ast_generated})

add_library(zct_frontend_ast STATIC
    "${CMAKE_BINARY_DIR}/src/frontend/ast/ast.cpp"
)

target_include_directories(zct_frontend_ast PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(zct_frontend_ast PUBLIC
    zct_common
)

add_dependencies(zct_frontend_ast zct_frontend_ast_generated)

add_executable(ast-demo
    "${PROJECT_SOURCE_DIR}/tests/frontend/ast-demo.cpp"
)

target_link_libraries(ast-demo PRIVATE
    zct_frontend_ast
    zct_common
)

add_dependencies(ast-demo zct_frontend_ast_generated)

add_test(NAME ast-demo COMMAND ast-demo)
