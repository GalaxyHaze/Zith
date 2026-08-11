set(_zith_parser_generated
    "${CMAKE_BINARY_DIR}/src/frontend/parser/parser.cpp"
    "${CMAKE_BINARY_DIR}/src/frontend/parser/parser.hpp"
    "${CMAKE_BINARY_DIR}/src/frontend/parser/actions.hpp"
)

add_custom_command(
    OUTPUT ${_zith_parser_generated}
    BYPRODUCTS "${CMAKE_BINARY_DIR}/src/frontend/parser/.gitignore"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/parser.rules"
            --out "${CMAKE_BINARY_DIR}/src/frontend/parser"
            --types "${CMAKE_CURRENT_SOURCE_DIR}/types.hpp"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/parser.rules"
        "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/types.hpp"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/__init__.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/errors.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/text.py"
    VERBATIM
    COMMENT "Generating parser sources from rules"
)

add_custom_target(zith_frontend_parser_generated DEPENDS ${_zith_parser_generated})

add_library(zith_frontend_parser STATIC
    "${CMAKE_BINARY_DIR}/src/frontend/parser/parser.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/actions.cpp"
)

target_include_directories(zith_frontend_parser PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_BINARY_DIR}/src"
)

target_link_libraries(zith_frontend_parser PUBLIC
    zith_frontend_lexer
    zith_common
)

add_dependencies(zith_frontend_parser zith_frontend_parser_generated)
