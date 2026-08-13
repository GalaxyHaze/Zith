add_library(zith_sir STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/sir.cpp"
)

target_include_directories(zith_sir PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
)

target_link_libraries(zith_sir PUBLIC
    zith_common
)
