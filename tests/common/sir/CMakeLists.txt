add_executable(sir-test
    sir-test.cpp
)

target_link_libraries(sir-test PRIVATE
    zct_sir
    zct_common
)

add_test(
    NAME sir-basics
    COMMAND sir-test
)
