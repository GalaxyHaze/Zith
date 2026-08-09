add_executable(source-map-test
    source-map-test.cpp
)

target_link_libraries(source-map-test PRIVATE
    zith_common
)

add_test(
    NAME source-map-basics
    COMMAND source-map-test
)
