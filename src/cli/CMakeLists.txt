set(_zith_cli_generated
    "${CMAKE_CURRENT_BINARY_DIR}/cli.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/cli.hpp"
    "${CMAKE_CURRENT_BINARY_DIR}/actions.hpp"
)

add_custom_command(
    OUTPUT ${_zith_cli_generated}
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/.gitignore"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/cli.rules"
            --out "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/cli.rules"
        "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
    VERBATIM
    COMMENT "Generating CLI sources from rules"
)

add_custom_target(cli-generated DEPENDS ${_zith_cli_generated})

add_library(cli STATIC
    "${CMAKE_CURRENT_BINARY_DIR}/cli.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/handlers.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/terminal.cpp"
)

target_include_directories(cli PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
    "${CMAKE_CURRENT_BINARY_DIR}"
)

set(cli ${CMAKE_CURRENT_SOURCE_DIR} PARENT_SCOPE)
