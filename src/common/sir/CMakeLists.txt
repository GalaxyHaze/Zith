add_library(zct_sir STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/sir.cpp"
)

target_include_directories(zct_sir PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
)

target_link_libraries(zct_sir PUBLIC
    zct_common
)

add_executable(sir-demo
    "${PROJECT_SOURCE_DIR}/tests/common/sir/sir-demo.cpp"
)

target_link_libraries(sir-demo PRIVATE
    zct_sir
    zct_common
)

add_test(NAME sir-demo COMMAND sir-demo)
