add_executable(type-system-test
    type-system-test.cpp
)

target_link_libraries(type-system-test PRIVATE
    zct_type_system
    zct_common
)

add_dependencies(type-system-test zct_type_system_generated)

add_test(
    NAME type-system-basics
    COMMAND type-system-test
)

add_test(
    NAME type-system-generator-regression
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/type-system-generator-test.py"
            "${PROJECT_SOURCE_DIR}"
            "${CMAKE_CXX_COMPILER}"
    DEPENDS
        "${PROJECT_SOURCE_DIR}/tools/test_kit/__init__.py"
        "${PROJECT_SOURCE_DIR}/tools/test_kit/asserts.py"
        "${PROJECT_SOURCE_DIR}/tools/test_kit/smoke.py"
)
