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

add_executable(common-diagnostic-test
    diagnostic-test.cpp
)

target_link_libraries(common-diagnostic-test PRIVATE
    zith_common
)

add_test(
    NAME common-diagnostic-basics
    COMMAND common-diagnostic-test
)

add_executable(common-diagnostic-render-test
    diagnostic-render-test.cpp
)

target_link_libraries(common-diagnostic-render-test PRIVATE
    zith_common
)

add_test(
    NAME common-diagnostic-render-basics
    COMMAND common-diagnostic-render-test
)

add_executable(common-text-test
    text-test.cpp
)

target_link_libraries(common-text-test PRIVATE
    zith_common
)

add_test(
    NAME common-text-basics
    COMMAND common-text-test
)

add_executable(ast-helpers-test
    ast-helpers-test.cpp
)

target_link_libraries(ast-helpers-test PRIVATE
    zith_common
)

add_test(
    NAME ast-helpers-basics
    COMMAND ast-helpers-test
)
