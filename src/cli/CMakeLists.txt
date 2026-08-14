set(_zct_cli_generated
    "${CMAKE_BINARY_DIR}/src/cli/cli.cpp"
    "${CMAKE_BINARY_DIR}/src/cli/cli.hpp"
    "${CMAKE_BINARY_DIR}/src/cli/actions.hpp"
)

add_custom_command(
    OUTPUT ${_zct_cli_generated}
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
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/runtime.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/text.py"
    VERBATIM
    COMMENT "Generating CLI sources from rules"
)

add_custom_target(zct_cli_generated DEPENDS ${_zct_cli_generated})

add_library(zct_cli STATIC
    "${CMAKE_BINARY_DIR}/src/cli/cli.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/handlers.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/terminal.cpp"
)

target_include_directories(zct_cli PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
)

target_link_libraries(zct_cli PRIVATE
    zct_session
)

add_dependencies(zct_cli zct_cli_generated)

add_executable(cli-demo
    "${PROJECT_SOURCE_DIR}/tests/cli/cli-demo.cpp"
)

target_link_libraries(cli-demo PRIVATE
    zct_cli
    zct_common
)

add_dependencies(cli-demo zct_cli_generated)

add_test(NAME cli-demo COMMAND cli-demo)
