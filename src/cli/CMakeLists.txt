set(_zith_cli_generated
    "${CMAKE_BINARY_DIR}/src/cli/cli.cpp"
    "${CMAKE_BINARY_DIR}/src/cli/cli.hpp"
    "${CMAKE_BINARY_DIR}/src/cli/actions.hpp"
)

add_custom_command(
    OUTPUT ${_zith_cli_generated}
    BYPRODUCTS "${CMAKE_BINARY_DIR}/src/cli/.gitignore"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/cli.rules"
            --out "${CMAKE_BINARY_DIR}/src/cli"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/cli.rules"
        "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/__init__.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/errors.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/text.py"
    VERBATIM
    COMMENT "Generating CLI sources from rules"
)

add_custom_target(zith_cli_generated DEPENDS ${_zith_cli_generated})

add_library(zith_cli STATIC
    "${CMAKE_BINARY_DIR}/src/cli/cli.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/handlers.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/terminal.cpp"
)

target_include_directories(zith_cli PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
)

target_link_libraries(zith_cli PRIVATE
    zith_session
)

add_dependencies(zith_cli zith_cli_generated)
