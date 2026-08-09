cmake_minimum_required(VERSION 3.20)
project(zith LANGUAGES CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

include(FetchContent)

set(MIO_TAG master CACHE STRING "mio header-only library revision")
FetchContent_Declare(
    mio
    GIT_REPOSITORY https://github.com/mandreyel/mio.git
    GIT_TAG "${MIO_TAG}"
)
FetchContent_MakeAvailable(mio)

add_subdirectory(src)
add_subdirectory(tests)
