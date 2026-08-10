set(_zith_frontend_ast_generated
    "${CMAKE_BINARY_DIR}/src/frontend/ast/ast.cpp"
    "${CMAKE_BINARY_DIR}/src/frontend/ast/ast.hpp"
    "${CMAKE_BINARY_DIR}/src/frontend/ast/walk.hpp"
)

add_custom_command(
    OUTPUT ${_zith_frontend_ast_generated}
    BYPRODUCTS "${CMAKE_BINARY_DIR}/src/frontend/ast/.gitignore"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/ast.rules"
            --out "${CMAKE_BINARY_DIR}/src/frontend/ast"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/ast.rules"
        "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/types.hpp"
    VERBATIM
    COMMENT "Generating frontend AST sources from rules"
)

add_custom_target(zith_frontend_ast_generated DEPENDS ${_zith_frontend_ast_generated})

add_library(zith_frontend_ast STATIC
    "${CMAKE_BINARY_DIR}/src/frontend/ast/ast.cpp"
)

target_include_directories(zith_frontend_ast PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(zith_frontend_ast PUBLIC
    zith_common
)

add_dependencies(zith_frontend_ast zith_frontend_ast_generated)
