add_executable(codegen-test
    codegen-test.cpp
)

target_link_libraries(codegen-test PRIVATE
    zct_codegen
    zct_common
)

add_test(
    NAME codegen-basics
    COMMAND codegen-test
)
