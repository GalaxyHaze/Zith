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

add_executable(sir-flat-test
    "${PROJECT_SOURCE_DIR}/tests/common/sir/sir-flat-test.cpp"
)

target_link_libraries(sir-flat-test PRIVATE
    zct_sir_flat
    zct_sir
    zct_cache
    zct_common
)

add_test(
    NAME sir-flat-basics
    COMMAND sir-flat-test
)

add_executable(sir-flat-demo
    "${PROJECT_SOURCE_DIR}/tests/common/sir/sir-flat-demo.cpp"
)

target_link_libraries(sir-flat-demo PRIVATE
    zct_sir_flat
    zct_sir
    zct_cache
    zct_common
)

add_test(
    NAME sir-flat-demo
    COMMAND sir-flat-demo
)
