add_library(zct_codegen STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/codegen.cpp"
)

add_library(zct_codegen_vm STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/vm/vm.cpp"
)

target_include_directories(zct_codegen PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
)

target_link_libraries(zct_codegen PUBLIC
    zct_common
    zct_sir
    zct_codegen_vm
)

target_include_directories(zct_codegen_vm PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
)

target_link_libraries(zct_codegen_vm PUBLIC
    zct_common
    zct_sir
)

add_executable(codegen-demo
    "${PROJECT_SOURCE_DIR}/tests/codegen/codegen-demo.cpp"
)

target_link_libraries(codegen-demo PRIVATE
    zct_codegen
    zct_common
    zct_sir
)

add_test(NAME codegen-demo COMMAND codegen-demo)
