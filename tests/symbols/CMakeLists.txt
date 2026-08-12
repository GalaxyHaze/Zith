add_executable(symbols-test
    symbols-test.cpp
)

target_link_libraries(symbols-test PRIVATE
    zith_import
    zith_symbols
    zith_common
)

add_dependencies(symbols-test zith_symbols_generated)

add_test(
    NAME symbols-basics
    COMMAND symbols-test
)

add_test(
    NAME symbols-generator-regression
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/symbols-generator-test.py"
            "${PROJECT_SOURCE_DIR}"
            "${CMAKE_CXX_COMPILER}"
    DEPENDS
        "${PROJECT_SOURCE_DIR}/tools/test_kit/__init__.py"
        "${PROJECT_SOURCE_DIR}/tools/test_kit/asserts.py"
        "${PROJECT_SOURCE_DIR}/tools/test_kit/smoke.py"
)
