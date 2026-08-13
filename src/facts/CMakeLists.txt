cmake_minimum_required(VERSION 3.15)
project(ZithFacts LANGUAGES CXX)

# --- O seu código atual continua abaixo desta linha ---
add_executable(facts-v2-test
    ${CMAKE_SOURCE_DIR}/tests/facts/facts-v2-test.cpp
)
target_link_libraries(facts-v2-test PRIVATE
    zith_common
)
add_test(NAME facts-v2-basics COMMAND facts-v2-test --basic)
add_test(NAME facts-v2-intensive COMMAND facts-v2-test --intensive)

add_executable(facts-v2-demo
    ${CMAKE_SOURCE_DIR}/tests/facts/facts-v2-demo.cpp
)
target_link_libraries(facts-v2-demo PRIVATE
    zith_common
)
add_test(NAME facts-v2-demo COMMAND facts-v2-demo)
