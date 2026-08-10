add_executable(session-test
    session-test.cpp
)

target_link_libraries(session-test PRIVATE
    zith_session_core
    zith_frontend_lexer
    zith_common
)

add_dependencies(session-test zith_session_generated)

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
)
