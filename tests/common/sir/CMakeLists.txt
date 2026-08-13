add_executable(sir-test
    sir-test.cpp
)

target_link_libraries(sir-test PRIVATE
    zith_sir
    zith_common
)

add_test(
    NAME sir-basics
    COMMAND sir-test
)
