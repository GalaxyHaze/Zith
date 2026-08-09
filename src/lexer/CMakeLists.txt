set(_zith_lexer_generated
    "${CMAKE_CURRENT_BINARY_DIR}/lexer.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/lexer.hpp"
    "${CMAKE_CURRENT_BINARY_DIR}/actions.hpp"
    "${CMAKE_CURRENT_BINARY_DIR}/keyword-table.hpp"
)

add_custom_command(
    OUTPUT ${_zith_lexer_generated}
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/.gitignore"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/lexer.rules"
            --out "${CMAKE_CURRENT_BINARY_DIR}"
            --types "${CMAKE_CURRENT_SOURCE_DIR}/types.hpp"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/lexer.rules"
        "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/types.hpp"
    VERBATIM
    COMMENT "Generating lexer sources from rules"
)

add_custom_target(lexer-generated DEPENDS ${_zith_lexer_generated})

add_library(lexer STATIC
    "${CMAKE_CURRENT_BINARY_DIR}/lexer.cpp"
)

target_include_directories(lexer PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}"
)

set(lexer ${CMAKE_CURRENT_SOURCE_DIR} PARENT_SCOPE)
