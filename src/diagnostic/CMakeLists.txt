set(_zith_diagnostic_generated
    "${CMAKE_BINARY_DIR}/src/diagnostic/error-info.hpp"
    "${CMAKE_BINARY_DIR}/src/diagnostic/error-info.cpp"
)

add_custom_command(
    OUTPUT ${_zith_diagnostic_generated}
    BYPRODUCTS "${CMAKE_BINARY_DIR}/src/diagnostic/.gitignore"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/error.rules"
            --out "${CMAKE_BINARY_DIR}/src/diagnostic"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/error.rules"
        "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/__init__.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/errors.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/text.py"
    VERBATIM
    COMMENT "Generating diagnostic error tables from rules"
)

add_custom_target(zith_diagnostic_generated
    DEPENDS ${_zith_diagnostic_generated}
)

add_library(zith_diagnostic STATIC
    "${CMAKE_BINARY_DIR}/src/diagnostic/error-info.cpp"
)

add_dependencies(zith_diagnostic zith_diagnostic_generated)

target_include_directories(zith_diagnostic PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
)

target_link_libraries(zith_diagnostic PRIVATE
    mio::mio
)
