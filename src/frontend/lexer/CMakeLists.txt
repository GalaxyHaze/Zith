set(_zith_lexer_generated
    "${CMAKE_BINARY_DIR}/src/frontend/lexer/lexer.cpp"
    "${CMAKE_BINARY_DIR}/src/frontend/lexer/lexer.hpp"
    "${CMAKE_BINARY_DIR}/src/frontend/lexer/actions.hpp"
    "${CMAKE_BINARY_DIR}/src/frontend/lexer/keyword-table.hpp"
)

add_custom_command(
    OUTPUT ${_zith_lexer_generated}
    BYPRODUCTS "${CMAKE_BINARY_DIR}/src/frontend/lexer/.gitignore"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/lexer.rules"
            --out "${CMAKE_BINARY_DIR}/src/frontend/lexer"
            --types "${CMAKE_CURRENT_SOURCE_DIR}/types.hpp"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/lexer.rules"
        "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/types.hpp"
    VERBATIM
    COMMENT "Generating lexer sources from rules"
)

add_custom_target(zith_frontend_lexer_generated DEPENDS ${_zith_lexer_generated})

add_library(zith_frontend_lexer STATIC
    "${CMAKE_BINARY_DIR}/src/frontend/lexer/lexer.cpp"
)

target_include_directories(zith_frontend_lexer PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_BINARY_DIR}/src"
)

add_dependencies(zith_frontend_lexer zith_frontend_lexer_generated)
