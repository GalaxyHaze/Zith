add_executable(lexer-test
    lexer-test.cpp
)

target_link_libraries(lexer-test PRIVATE
    zith_frontend_lexer
    zith_common
)

add_dependencies(lexer-test zith_frontend_lexer_generated)

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
    zith_frontend_lexer
    zith_common
)

add_dependencies(lexer-perf-smoke zith_frontend_lexer_generated)

add_executable(ast-test
    ast-test.cpp
)

target_link_libraries(ast-test PRIVATE
    zith_frontend_ast
    zith_common
)

add_dependencies(ast-test zith_frontend_ast_generated)

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
    zith_frontend_parser
    zith_frontend_lexer
    zith_common
)

add_dependencies(parser-test zith_frontend_parser_generated)

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
