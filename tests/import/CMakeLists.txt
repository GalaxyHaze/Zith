add_executable(import-test
    import-test.cpp
)

add_executable(import-demo
    demo.cpp
)

target_link_libraries(import-test PRIVATE
    zith_import
    zith_symbols
    zith_common
)

target_link_libraries(import-demo PRIVATE
    zith_import
    zith_symbols
    zith_common
)

add_test(
    NAME import-graph-basics
    COMMAND import-test
)
