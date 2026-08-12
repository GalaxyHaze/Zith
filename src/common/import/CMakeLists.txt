add_library(zith_import STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/import-graph.cpp"
)

target_include_directories(zith_import PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
)

target_link_libraries(zith_import PUBLIC
    zith_common
    zith_symbols
)
