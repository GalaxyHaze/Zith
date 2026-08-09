add_executable(lexer-test
    lexer-test.cpp
)

target_link_libraries(lexer-test PRIVATE
    lexer
)

add_executable(source-map-test
    source-map-test.cpp
)

target_link_libraries(source-map-test PRIVATE
    common
)

add_test(
    NAME lexer-generated-basics
    COMMAND lexer-test
)

add_test(
    NAME source-map-basics
    COMMAND source-map-test
)

add_test(
    NAME cli-no-args-help
    COMMAND "${CMAKE_COMMAND}"
            -DPROGRAM=$<TARGET_FILE:zithc>
            -DEXPECTED_EXIT=0
            -DEXPECT_STDOUT=Usage
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run-cli-check.cmake"
)

add_test(
    NAME cli-unknown-command-suggestion
    COMMAND "${CMAKE_COMMAND}"
            -DPROGRAM=$<TARGET_FILE:zithc>
            -DARGS=bulid
            -DEXPECTED_EXIT=2
            -DEXPECT_STDERR_1=unknown\ command\ 'bulid'
            -DEXPECT_STDERR_2=did\ you\ mean\ 'build'?
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run-cli-check.cmake"
)

add_test(
    NAME cli-unknown-subcommand-suggestion
    COMMAND "${CMAKE_COMMAND}"
            -DPROGRAM=$<TARGET_FILE:zithc>
            -DARGS=deps\;chekc
            -DEXPECTED_EXIT=2
            -DEXPECT_STDERR_1=unknown\ subcommand\ for\ deps:\ chekc
            -DEXPECT_STDERR_2=did\ you\ mean\ 'check'?
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run-cli-check.cmake"
)

add_test(
    NAME cli-unknown-global-flag
    COMMAND "${CMAKE_COMMAND}"
            -DPROGRAM=$<TARGET_FILE:zithc>
            -DARGS=--verbsoe
            -DEXPECTED_EXIT=2
            -DEXPECT_STDERR_1=unknown\ flag\ '--verbsoe'
            -DEXPECT_STDERR_2=did\ you\ mean\ '--verbose'?
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run-cli-check.cmake"
)

add_test(
    NAME cli-unknown-command-flag-suggestion
    COMMAND "${CMAKE_COMMAND}"
            -DPROGRAM=$<TARGET_FILE:zithc>
            -DARGS=fmt\;--colro
            -DEXPECTED_EXIT=2
            -DEXPECT_STDERR_1=unknown\ flag\ '--colro'
            -DEXPECT_STDERR_2=did\ you\ mean\ '--color'?
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run-cli-check.cmake"
)

add_test(
    NAME cli-unknown-subcommand-flag-suggestion
    COMMAND "${CMAKE_COMMAND}"
            -DPROGRAM=$<TARGET_FILE:zithc>
            -DARGS=deps\;add\;--froce
            -DEXPECTED_EXIT=2
            -DEXPECT_STDERR_1=unknown\ flag\ '--froce'
            -DEXPECT_STDERR_2=did\ you\ mean\ '--force'?
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run-cli-check.cmake"
)

add_test(
    NAME cli-valid-dispatch-path
    COMMAND "${CMAKE_COMMAND}"
            -DPROGRAM=$<TARGET_FILE:zithc>
            -DARGS=deps\;add\;pkg
            -DEXPECTED_EXIT=0
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run-cli-check.cmake"
)

add_test(
    NAME cli-help-dispatch-output
    COMMAND "${CMAKE_COMMAND}"
            -DPROGRAM=$<TARGET_FILE:zithc>
            -DARGS=--help
            -DEXPECTED_EXIT=0
            -DEXPECT_STDOUT=zith\ build\ tool
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run-cli-check.cmake"
)

add_test(
    NAME cli-version
    COMMAND "${CMAKE_COMMAND}"
            -DPROGRAM=$<TARGET_FILE:zithc>
            -DARGS=--version
            -DEXPECTED_EXIT=0
            -DEXPECT_STDOUT=0.1.0
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run-cli-check.cmake"
)
