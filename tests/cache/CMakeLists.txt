add_executable(cache-test
    cache-test.cpp
)

target_link_libraries(cache-test PRIVATE
    zct_cache
    zct_common
)

add_dependencies(cache-test zct_cache_generated)

add_test(
    NAME cache-generated-basics
    COMMAND cache-test
)

add_test(
    NAME cache-generator-regression
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/cache-generator-test.py"
            "${PROJECT_SOURCE_DIR}"
            "${CMAKE_CXX_COMPILER}"
    DEPENDS
        "${PROJECT_SOURCE_DIR}/tools/test_kit/__init__.py"
        "${PROJECT_SOURCE_DIR}/tools/test_kit/asserts.py"
        "${PROJECT_SOURCE_DIR}/tools/test_kit/smoke.py"
)
