add_executable(sema-test
    sema-test.cpp
)

target_link_libraries(sema-test PRIVATE
    zct_session
    zct_sema
    zct_frontend_parser
    zct_frontend_lexer
    zct_common
)

add_test(
    NAME sema-typechecked
    COMMAND sema-test
)
