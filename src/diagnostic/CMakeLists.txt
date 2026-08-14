set(_zct_diagnostic_generated
    "${CMAKE_BINARY_DIR}/src/diagnostic/error-info.hpp"
    "${CMAKE_BINARY_DIR}/src/diagnostic/error-info.cpp"
)

add_custom_command(
    OUTPUT ${_zct_diagnostic_generated}
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
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/runtime.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/text.py"
    VERBATIM
    COMMENT "Generating diagnostic error tables from rules"
)

add_custom_target(zct_diagnostic_generated
    DEPENDS ${_zct_diagnostic_generated}
)

add_library(zct_diagnostic STATIC
    "${CMAKE_BINARY_DIR}/src/diagnostic/error-info.cpp"
)

add_dependencies(zct_diagnostic zct_diagnostic_generated)

target_include_directories(zct_diagnostic PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
)

if(ZCT_HAS_MIO)
    target_compile_definitions(zct_diagnostic PUBLIC ZCT_HAS_MIO=1)
    target_include_directories(zct_diagnostic SYSTEM PUBLIC
        "${PROJECT_BINARY_DIR}/_deps/mio-src/include"
    )
endif()

add_executable(diagnostic-demo
    "${PROJECT_SOURCE_DIR}/tests/diagnostic/diagnostic-demo.cpp"
)

target_link_libraries(diagnostic-demo PRIVATE
    zct_diagnostic
    zct_common
)

add_dependencies(diagnostic-demo zct_diagnostic_generated)

add_test(NAME diagnostic-demo COMMAND diagnostic-demo)
