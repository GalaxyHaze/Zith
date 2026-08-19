add_executable(lexer-test
    lexer-test.cpp
)

target_link_libraries(lexer-test PRIVATE
    zct_frontend_lexer
    zct_common
)

add_dependencies(lexer-test zct_frontend_lexer_generated)

add_test(
    NAME lexer-generated-basics
    COMMAND lexer-test
)

add_test(
    NAME lexer-generator-regression
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/lexer-generator-test.py"
            "${PROJECT_SOURCE_DIR}"
            "${CMAKE_CXX_COMPILER}"
    DEPENDS
        "${PROJECT_SOURCE_DIR}/tools/test_kit/__init__.py"
        "${PROJECT_SOURCE_DIR}/tools/test_kit/asserts.py"
        "${PROJECT_SOURCE_DIR}/tools/test_kit/smoke.py"
)

add_executable(lexer-perf-smoke
    lexer-perf-smoke.cpp
)

target_link_libraries(lexer-perf-smoke PRIVATE
    zct_frontend_lexer
    zct_common
)

add_dependencies(lexer-perf-smoke zct_frontend_lexer_generated)

add_executable(ast-test
    ast-test.cpp
)

target_link_libraries(ast-test PRIVATE
    zct_frontend_ast
    zct_common
)

add_dependencies(ast-test zct_frontend_ast_generated)

add_test(
    NAME ast-generated-basics
    COMMAND ast-test
)

add_test(
    NAME ast-generator-regression
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/ast-generator-test.py"
            "${PROJECT_SOURCE_DIR}"
            "${CMAKE_CXX_COMPILER}"
    DEPENDS
        "${PROJECT_SOURCE_DIR}/tools/test_kit/__init__.py"
        "${PROJECT_SOURCE_DIR}/tools/test_kit/asserts.py"
        "${PROJECT_SOURCE_DIR}/tools/test_kit/smoke.py"
)

add_executable(parser-test
    parser-test.cpp
)

target_link_libraries(parser-test PRIVATE
    zct_frontend_parser
    zct_frontend_ast
    zct_frontend_lexer
    zct_common
)

add_dependencies(parser-test zct_frontend_parser_generated)

add_test(
    NAME parser-generated-basics
    COMMAND parser-test
)

add_test(
    NAME parser-generator-regression
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/parser-generator-test.py"
            "${PROJECT_SOURCE_DIR}"
            "${CMAKE_CXX_COMPILER}"
    DEPENDS
        "${PROJECT_SOURCE_DIR}/tools/test_kit/__init__.py"
        "${PROJECT_SOURCE_DIR}/tools/test_kit/asserts.py"
        "${PROJECT_SOURCE_DIR}/tools/test_kit/smoke.py"
)
