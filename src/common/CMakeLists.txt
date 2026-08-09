set(_zith_common_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/arena.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/source-file.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/source-map.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/string-interner.cpp"
)

add_library(zith_common STATIC ${_zith_common_sources})

target_link_libraries(zith_common PUBLIC
    mio::mio
)

target_include_directories(zith_common PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
)
