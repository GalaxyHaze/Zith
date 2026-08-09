add_executable(lexer-test
    lexer-test.cpp
)

target_link_libraries(lexer-test PRIVATE
    zith_frontend_lexer
)

add_dependencies(lexer-test zith_frontend_lexer_generated)

add_test(
    NAME lexer-generated-basics
    COMMAND lexer-test
)
