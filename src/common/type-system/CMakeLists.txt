set(_zct_type_system_generated
    "${CMAKE_BINARY_DIR}/src/common/type-system/type-system-table.hpp"
)

add_custom_command(
    OUTPUT ${_zct_type_system_generated}
    BYPRODUCTS "${CMAKE_BINARY_DIR}/src/common/type-system/.gitignore"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/type-system.rules"
            --out "${CMAKE_BINARY_DIR}/src/common/type-system"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/type-system.rules"
        "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/__init__.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/errors.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/runtime.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/text.py"
    VERBATIM
    COMMENT "Generating type-system tables from rules"
)

add_custom_target(zct_type_system_generated
    DEPENDS ${_zct_type_system_generated}
)

add_library(zct_type_system STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/type-system.cpp"
)

add_dependencies(zct_type_system zct_type_system_generated)

target_include_directories(zct_type_system PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
)

target_link_libraries(zct_type_system PUBLIC
    zct_common
)
