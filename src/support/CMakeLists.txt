add_library(zct_support STATIC
    resource-discovery.cpp
)

target_include_directories(zct_support PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
)
