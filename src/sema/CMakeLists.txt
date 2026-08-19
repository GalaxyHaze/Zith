add_library(zct_sema STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/type-table.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/sema.cpp"
)

target_include_directories(zct_sema PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
    "${PROJECT_SOURCE_DIR}/src/frontend/ast"
    "${PROJECT_BINARY_DIR}/src/frontend/ast"
    "${PROJECT_SOURCE_DIR}/src/frontend/parser"
    "${PROJECT_BINARY_DIR}/src/frontend/parser"
)

add_dependencies(zct_sema zct_session_generated)

target_link_libraries(zct_sema PUBLIC
    zct_common
    zct_type_system
    zct_frontend_ast
    zct_frontend_parser
)
