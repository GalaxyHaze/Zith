cmake_minimum_required(VERSION 3.20)
project(zith_cache LANGUAGES CXX)

option(ZITH_CACHE_BUILD_TESTS "Build zith-cache tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(zith_cache STATIC
    src/byte-cache.cpp
)

target_include_directories(zith_cache PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

if(ZITH_CACHE_BUILD_TESTS)
    enable_testing()
    add_executable(zith_cache_test
        tests/byte-cache-test.cpp
    )
    target_include_directories(zith_cache_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tests"
    )
    target_link_libraries(zith_cache_test PRIVATE zith_cache)
    add_test(NAME zith_cache_test COMMAND zith_cache_test)
endif()
