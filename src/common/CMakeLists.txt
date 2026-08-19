set(_zct_common_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/memory/arena.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/memory/file-source.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/memory/source-file.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/memory/source-map.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/memory/string-interner.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/diagnostic/levenshtein.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/diagnostic/render.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/text/parse.cpp"
)

add_library(zct_common STATIC ${_zct_common_sources})

add_subdirectory(import)

add_subdirectory(sir)

add_subdirectory(type-system)

target_link_libraries(zct_common PUBLIC
    zct_diagnostic
)

if(ZCT_HAS_MIO)
    target_compile_definitions(zct_common PUBLIC ZCT_HAS_MIO=1)
    target_include_directories(zct_common SYSTEM PUBLIC
        "${PROJECT_BINARY_DIR}/_deps/mio-src/include"
    )
endif()

target_include_directories(zct_common PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
)

add_executable(common-demo
    "${PROJECT_SOURCE_DIR}/tests/common/common-demo.cpp"
)

target_link_libraries(common-demo PRIVATE
    zct_common
)

add_test(NAME common-demo COMMAND common-demo)
