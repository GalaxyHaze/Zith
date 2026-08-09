set(_zith_common_out "${CMAKE_CURRENT_BINARY_DIR}")

set(_zith_common_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/arena.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/string-interner.cpp"
)

add_library(common STATIC ${_zith_common_sources})

target_include_directories(common PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

set(common ${CMAKE_CURRENT_SOURCE_DIR} PARENT_SCOPE)
