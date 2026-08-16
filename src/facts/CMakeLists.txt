cmake_minimum_required(VERSION 3.15)
project(ZithFacts LANGUAGES CXX)

set(_zct_facts_generated
    "${CMAKE_BINARY_DIR}/src/facts/facts.hpp"
)

add_custom_command(
    OUTPUT ${_zct_facts_generated}
    BYPRODUCTS "${CMAKE_BINARY_DIR}/src/facts/.gitignore"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/facts.rules"
            --out "${CMAKE_BINARY_DIR}/src/facts"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/facts.rules"
        "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
        "${CMAKE_SOURCE_DIR}/tools/rules_kit/__init__.py"
        "${CMAKE_SOURCE_DIR}/tools/rules_kit/errors.py"
        "${CMAKE_SOURCE_DIR}/tools/rules_kit/runtime.py"
        "${CMAKE_SOURCE_DIR}/tools/rules_kit/text.py"
    VERBATIM
    COMMENT "Generating typed fact domains from rules"
)

add_custom_target(zct_facts_generated
    DEPENDS ${_zct_facts_generated}
)

add_executable(facts-v2-test
    ${CMAKE_SOURCE_DIR}/tests/facts/facts-v2-test.cpp
    ${_zct_facts_generated}
)
target_link_libraries(facts-v2-test PRIVATE
    zct_common
)
add_dependencies(facts-v2-test zct_facts_generated)
add_test(NAME facts-v2-basics COMMAND facts-v2-test --basic)
add_test(NAME facts-v2-intensive COMMAND facts-v2-test --intensive)

add_executable(facts-v2-demo
    ${CMAKE_SOURCE_DIR}/tests/facts/facts-v2-demo.cpp
    ${_zct_facts_generated}
)
target_link_libraries(facts-v2-demo PRIVATE
    zct_common
)
add_dependencies(facts-v2-demo zct_facts_generated)
add_test(NAME facts-v2-demo COMMAND facts-v2-demo)

add_test(
    NAME facts-generator-regression
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/facts/facts-generator-test.py"
            "${CMAKE_SOURCE_DIR}"
            "${CMAKE_CXX_COMPILER}"
    DEPENDS
        "${CMAKE_SOURCE_DIR}/tools/test_kit/__init__.py"
        "${CMAKE_SOURCE_DIR}/tools/test_kit/asserts.py"
        "${CMAKE_SOURCE_DIR}/tools/test_kit/smoke.py"
)

target_include_directories(facts-v2-test PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_BINARY_DIR}/src"
)
target_include_directories(facts-v2-demo PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_BINARY_DIR}/src"
)
