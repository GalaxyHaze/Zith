add_executable(source-map-test
    source-map-test.cpp
)

target_link_libraries(source-map-test PRIVATE
    zith_common
)

add_test(
    NAME source-map-basics
    COMMAND source-map-test
)

add_executable(common-diagnostic-test
    diagnostic-test.cpp
)

target_link_libraries(common-diagnostic-test PRIVATE
    zith_common
)

add_test(
    NAME common-diagnostic-basics
    COMMAND common-diagnostic-test
)

add_executable(common-diagnostic-render-test
    diagnostic-render-test.cpp
)

target_link_libraries(common-diagnostic-render-test PRIVATE
    zith_common
)

add_test(
    NAME common-diagnostic-render-basics
    COMMAND common-diagnostic-render-test
)

add_executable(common-diagnostic-error-test
    diagnostic-error-test.cpp
)

target_link_libraries(common-diagnostic-error-test PRIVATE
    zith_common
)

add_test(
    NAME common-diagnostic-error-basics
    COMMAND common-diagnostic-error-test
)

add_executable(project-scaffold-test
    project-scaffold-test.cpp
)

add_test(
    NAME project-scaffold-basics
    COMMAND project-scaffold-test
            "${PROJECT_SOURCE_DIR}"
)

add_test(
    NAME diagnostic-generator-regression
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/diagnostic-generator-test.py"
            "${PROJECT_SOURCE_DIR}"
            "${CMAKE_CXX_COMPILER}"
    DEPENDS
        "${PROJECT_SOURCE_DIR}/tools/test_kit/__init__.py"
        "${PROJECT_SOURCE_DIR}/tools/test_kit/asserts.py"
        "${PROJECT_SOURCE_DIR}/tools/test_kit/smoke.py"
)

add_executable(common-text-test
    text-test.cpp
)

target_link_libraries(common-text-test PRIVATE
    zith_common
)

add_test(
    NAME common-text-basics
    COMMAND common-text-test
)

add_executable(ast-helpers-test
    ast-helpers-test.cpp
)

target_link_libraries(ast-helpers-test PRIVATE
    zith_common
)

add_test(
    NAME ast-helpers-basics
    COMMAND ast-helpers-test
)
