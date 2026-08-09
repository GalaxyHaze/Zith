cmake_minimum_required(VERSION 3.20)
project(zith LANGUAGES CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_subdirectory(src/cli)
add_subdirectory(src/common)
add_subdirectory(src/lexer)
add_subdirectory(src/project-config)

add_custom_target(generate-language-sources
    DEPENDS
        cli-generated
        lexer-generated
        project-config-generated
)

add_executable(zithc
    main.cpp
)

target_link_libraries(zithc PRIVATE
    cli
    lexer
    project-config
    common
)

target_include_directories(zithc PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/lexer"
    "${CMAKE_CURRENT_BINARY_DIR}/src"
)

add_dependencies(zithc generate-language-sources)

add_subdirectory(tests)
