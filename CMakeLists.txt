cmake_minimum_required(VERSION 3.20)
project(turv LANGUAGES CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

include(FetchContent)

option(ZCT_HAS_MIO "Enable memory-mapped file support" ON)
option(ZCT_HAS_LLVM "Enable optional LLVM-related cache build paths" OFF)
option(ZCT_CODEGEN "Build the experimental codegen subsystem and tests" OFF)

set(ZCT_CODEGEN_BACKEND "VM" CACHE STRING
    "Reserved codegen backends: TINY, TINY_JIT, VM, LLVM, LLVM_JIT (only VM is currently valid)")

if(NOT ZCT_HAS_MIO)
    # Cache and source-map fall back to buffered reads; no network dependency.
endif()

if(NOT ZCT_CODEGEN_BACKEND STREQUAL "VM")
    message(FATAL_ERROR
        "ZCT_CODEGEN_BACKEND is a reserved selector for future milestones; "
        "VM is the only backend implemented in this phase")
endif()

if(ZCT_HAS_MIO)
    set(MIO_TAG master CACHE STRING "mio header-only library revision")
    FetchContent_Declare(
        mio
        GIT_REPOSITORY https://github.com/mandreyel/mio.git
        GIT_TAG "${MIO_TAG}"
    )
    FetchContent_MakeAvailable(mio)
endif()

add_subdirectory(src)
add_subdirectory(tests)
