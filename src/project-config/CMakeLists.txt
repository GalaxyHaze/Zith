set(_zith_project_config_generated
    "${CMAKE_CURRENT_BINARY_DIR}/project-config.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/project-config.hpp"
    "${CMAKE_CURRENT_BINARY_DIR}/actions.hpp"
)

add_custom_command(
    OUTPUT ${_zith_project_config_generated}
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/.gitignore"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/default.toml"
            --out "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/default.toml"
        "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
    VERBATIM
    COMMENT "Generating project configuration sources from default.toml"
)

add_custom_target(project-config-generated DEPENDS ${_zith_project_config_generated})

add_library(project-config STATIC
    "${CMAKE_CURRENT_BINARY_DIR}/project-config.cpp"
)

target_include_directories(project-config PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
    "${CMAKE_CURRENT_BINARY_DIR}"
)

set(project-config ${CMAKE_CURRENT_SOURCE_DIR} PARENT_SCOPE)
