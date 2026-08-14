add_library(zct_import STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/import-graph.cpp"
)

target_include_directories(zct_import PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
)

target_link_libraries(zct_import PUBLIC
    zct_common
    zct_symbols
)
