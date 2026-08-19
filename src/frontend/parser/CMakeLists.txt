set(_zct_parser_generated
    "${CMAKE_BINARY_DIR}/src/frontend/parser/parser.cpp"
    "${CMAKE_BINARY_DIR}/src/frontend/parser/parser.hpp"
    "${CMAKE_BINARY_DIR}/src/frontend/parser/actions.hpp"
)

add_custom_command(
    OUTPUT ${_zct_parser_generated}
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
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/runtime.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/text.py"
    VERBATIM
    COMMENT "Generating parser sources from rules"
)

add_custom_target(zct_frontend_parser_generated DEPENDS ${_zct_parser_generated})

add_library(zct_frontend_parser STATIC
    "${CMAKE_BINARY_DIR}/src/frontend/parser/parser.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/actions.cpp"
)

target_include_directories(zct_frontend_parser PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_BINARY_DIR}/src"
)

target_link_libraries(zct_frontend_parser PUBLIC
    zct_frontend_lexer
    zct_common
)

add_dependencies(zct_frontend_parser zct_frontend_parser_generated)

add_executable(parser-demo
    "${PROJECT_SOURCE_DIR}/tests/frontend/parser-demo.cpp"
)

target_link_libraries(parser-demo PRIVATE
    zct_frontend_parser
    zct_frontend_ast
    zct_frontend_lexer
    zct_common
)

add_dependencies(parser-demo zct_frontend_parser_generated)

add_test(NAME parser-demo COMMAND parser-demo)
