set(_zct_project_config_generated
    "${CMAKE_BINARY_DIR}/src/config/project/project-config.cpp"
    "${CMAKE_BINARY_DIR}/src/config/project/project-config.hpp"
    "${CMAKE_BINARY_DIR}/src/config/project/actions.hpp"
)

add_custom_command(
    OUTPUT ${_zct_project_config_generated}
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

add_custom_target(zct_flags_generated DEPENDS ${_zct_project_config_generated})

add_library(zct_flags STATIC
    "${CMAKE_BINARY_DIR}/src/config/project/project-config.cpp"
)

target_include_directories(zct_flags PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
)

add_dependencies(zct_flags zct_flags_generated)

add_executable(config-demo
    "${PROJECT_SOURCE_DIR}/tests/config/config-demo.cpp"
)

target_link_libraries(config-demo PRIVATE
    zct_flags
    zct_common
)

add_dependencies(config-demo zct_flags_generated)

add_test(NAME config-demo COMMAND config-demo)
