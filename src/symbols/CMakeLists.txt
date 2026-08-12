set(_zith_symbols_generated
    "${CMAKE_BINARY_DIR}/src/symbols/symbols.hpp"
)

add_custom_command(
    OUTPUT ${_zith_symbols_generated}
    BYPRODUCTS "${CMAKE_BINARY_DIR}/src/symbols/.gitignore"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/symbols.rules"
            --out "${CMAKE_BINARY_DIR}/src/symbols"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/symbols.rules"
        "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/__init__.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/errors.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/runtime.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/text.py"
    VERBATIM
    COMMENT "Generating symbol-table header from rules"
)

add_custom_target(zith_symbols_generated
    DEPENDS ${_zith_symbols_generated}
)

add_library(zith_symbols INTERFACE)

add_dependencies(zith_symbols zith_symbols_generated)

target_include_directories(zith_symbols INTERFACE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
)
