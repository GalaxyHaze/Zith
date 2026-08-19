add_library(zct_resolution STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/resolution.cpp"
)

target_include_directories(zct_resolution PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
)

target_link_libraries(zct_resolution PUBLIC
    zct_common
    zct_import
)

target_link_libraries(zct_resolution PRIVATE
    zct_frontend_parser
    zct_frontend_ast
    zct_frontend_lexer
)
