cmake_minimum_required(VERSION 3.20)
project(lith LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(lith
    src/main.cpp
    src/tokenizer.cpp
)

target_include_directories(lith PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
