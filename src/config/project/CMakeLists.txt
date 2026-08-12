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
            "${CMAKE_CURRENT_SOURCE_DIR}/../flags/default.toml"
            --out "${CMAKE_BINARY_DIR}/src/config/project"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../flags/default.toml"
        "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/__init__.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/errors.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/runtime.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/text.py"
    VERBATIM
    COMMENT "Generating flags configuration sources from default.toml"
)

add_custom_target(zith_flags_generated DEPENDS ${_zith_project_config_generated})

add_library(zith_flags STATIC
    "${CMAKE_BINARY_DIR}/src/config/project/project-config.cpp"
)

target_include_directories(zith_flags PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
)

add_dependencies(zith_flags zith_flags_generated)
