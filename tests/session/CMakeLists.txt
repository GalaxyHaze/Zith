add_executable(session-test
    session-test.cpp
)

target_link_libraries(session-test PRIVATE
    zct_session_core
    zct_frontend_lexer
    zct_frontend_ast
    zct_frontend_parser
    zct_common
)

add_dependencies(session-test zct_session_generated)

add_test(
    NAME session-generated-basics
    COMMAND session-test
)

add_test(
    NAME session-generator-regression
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/session-generator-test.py"
            "${PROJECT_SOURCE_DIR}"
            "${CMAKE_CXX_COMPILER}"
    DEPENDS
        "${PROJECT_SOURCE_DIR}/tools/test_kit/__init__.py"
        "${PROJECT_SOURCE_DIR}/tools/test_kit/asserts.py"
        "${PROJECT_SOURCE_DIR}/tools/test_kit/smoke.py"
)
