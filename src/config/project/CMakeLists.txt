set(_zith_project_config_generated
    "${CMAKE_BINARY_DIR}/src/config/project/project-config.cpp"
    "${CMAKE_BINARY_DIR}/src/config/project/project-config.hpp"
    "${CMAKE_BINARY_DIR}/src/config/project/actions.hpp"
)

add_custom_command(
    OUTPUT ${_zith_project_config_generated}
    BYPRODUCTS "${CMAKE_BINARY_DIR}/src/config/project/.gitignore"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/default.toml"
            --out "${CMAKE_BINARY_DIR}/src/config/project"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/default.toml"
        "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
    VERBATIM
    COMMENT "Generating project configuration sources from default.toml"
)

add_custom_target(zith_project_config_generated DEPENDS ${_zith_project_config_generated})

add_library(zith_project_config STATIC
    "${CMAKE_BINARY_DIR}/src/config/project/project-config.cpp"
)

target_include_directories(zith_project_config PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
)

add_dependencies(zith_project_config zith_project_config_generated)
