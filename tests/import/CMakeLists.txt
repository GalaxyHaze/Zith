add_executable(import-test
    import-test.cpp
)

add_executable(import-demo
    demo.cpp
)

target_link_libraries(import-test PRIVATE
    zct_import
    zct_symbols
    zct_common
)

target_link_libraries(import-demo PRIVATE
    zct_import
    zct_symbols
    zct_common
)

add_test(
    NAME import-graph-basics
    COMMAND import-test
)

add_test(
    NAME import-demo
    COMMAND import-demo
)
