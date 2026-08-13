set(_zith_common_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/memory/arena.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/memory/file-source.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/memory/source-file.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/memory/source-map.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/memory/string-interner.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/diagnostic/levenshtein.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/diagnostic/render.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/text/parse.cpp"
)

add_library(zith_common STATIC ${_zith_common_sources})

add_subdirectory(import)

add_subdirectory(sir)

target_link_libraries(zith_common PUBLIC
    zith_diagnostic
)

if(ZITH_HAS_MIO)
    target_compile_definitions(zith_common PUBLIC ZITH_HAS_MIO=1)
    target_include_directories(zith_common SYSTEM PUBLIC
        "${PROJECT_BINARY_DIR}/_deps/mio-src/include"
    )
endif()

target_include_directories(zith_common PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
)
