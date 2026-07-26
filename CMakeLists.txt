cmake_minimum_required(VERSION 3.20)
project(zithc LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ZITH_BUILD_CLI "Build the zithc CLI executable" ON)
option(ZITH_BUILD_CAPI "Build the C ABI library" ON)
option(ZITH_IS_WASM "Build for WebAssembly target" OFF)
option(ZITH_HAS_LLVM "Enable LLVM codegen backend" ON)

if(ZITH_HAS_LLVM AND NOT ZITH_IS_WASM)
    find_package(ZLIB QUIET)

    if(NOT DEFINED LLVM_DIR AND NOT DEFINED ENV{LLVM_DIR})
        find_program(LLVM_CONFIG_EXE NAMES llvm-config llvm-config-21 llvm-config-20 llvm-config-19 llvm-config-18 llvm-config-17)
        if(LLVM_CONFIG_EXE)
            execute_process(
                COMMAND ${LLVM_CONFIG_EXE} --cmakedir
                OUTPUT_VARIABLE LLVM_CONFIG_CMAKEDIR
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            if(EXISTS "${LLVM_CONFIG_CMAKEDIR}")
                set(LLVM_DIR "${LLVM_CONFIG_CMAKEDIR}" CACHE PATH "LLVM Directory" FORCE)
            endif()
        endif()
    endif()
    find_package(LLVM CONFIG QUIET)
    if(NOT LLVM_FOUND)
        message(WARNING "LLVM 18+ not found — disabling codegen backend")
        set(ZITH_HAS_LLVM OFF)
    elseif(LLVM_PACKAGE_VERSION VERSION_LESS 18)
        message(WARNING "LLVM ${LLVM_PACKAGE_VERSION} found, but LLVM 18+ is required — disabling codegen backend")
        set(ZITH_HAS_LLVM OFF)
    endif()
endif()

set(ZITH_VERSION "v0.0.1" CACHE STRING "Project version")

include(GNUInstallDirs)
include(FetchContent)

if(NOT ZITH_IS_WASM)
    FetchContent_Declare(
        mio
        GIT_REPOSITORY https://github.com/vimpunk/mio.git
        GIT_TAG 8b6b7d878c89e81614d05edca7936de41ccdd2da
    )
    FetchContent_MakeAvailable(mio)
endif()

FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
)
FetchContent_MakeAvailable(tomlplusplus)

add_subdirectory(src/memory)

file(GLOB_RECURSE ZITH_LIB_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(REMOVE_ITEM ZITH_LIB_SOURCES "${CMAKE_SOURCE_DIR}/src/cli/zithc-main.cpp")
if(ZITH_IS_WASM)
    file(GLOB_RECURSE CLI_CMD_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/cli/cmd/*.cpp")
    list(REMOVE_ITEM ZITH_LIB_SOURCES ${CLI_CMD_SOURCES})
    list(REMOVE_ITEM ZITH_LIB_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/wasm/playground.cpp")
else()
    file(GLOB_RECURSE WASM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/wasm/*.cpp")
    list(REMOVE_ITEM ZITH_LIB_SOURCES ${WASM_SOURCES})
endif()

if(NOT ZITH_HAS_LLVM OR ZITH_IS_WASM)
    file(GLOB_RECURSE CODEGEN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/codegen/*.cpp")
    list(REMOVE_ITEM ZITH_LIB_SOURCES ${CODEGEN_SOURCES})
endif()

add_library(zithcLib STATIC ${ZITH_LIB_SOURCES})

target_include_directories(zithcLib PUBLIC src)

if(ZITH_IS_WASM)
    target_compile_definitions(zithcLib PUBLIC ZITH_IS_WASM=1)
    target_compile_definitions(zithcLib PRIVATE TOML_EXCEPTIONS=0)
    target_compile_options(zithcLib PRIVATE -Wno-unknown-attributes -Wno-missing-prototypes -fno-exceptions)
endif()

if(CMAKE_CXX_FLAGS MATCHES "-fno-exceptions" OR CMAKE_CXX_FLAGS MATCHES "-fno-cxx-exceptions")
    target_compile_definitions(zithcLib PRIVATE TOML_EXCEPTIONS=0)
    target_compile_options(zithcLib PRIVATE -Wno-unknown-attributes -Wno-missing-prototypes)
endif()

if(ZITH_IS_WASM)
    add_executable(zith-playground src/wasm/playground.cpp src/wasm/wasi-stubs.cpp)
    target_link_libraries(zith-playground PRIVATE zithcLib)
    set_target_properties(zith-playground PROPERTIES OUTPUT_NAME zith-playground SUFFIX ".wasm")
    target_link_options(zith-playground PRIVATE -Wl,--no-entry -Wl,--export=zith_alloc -Wl,--export=zith_free -Wl,--export=zith_run_source -Wl,--export=zith_compile_source -Wl,--export=zith_last_error_ptr -Wl,--export=zith_last_error_len )
    target_compile_options(zith-playground PRIVATE -Wno-unknown-attributes -Wno-missing-prototypes)
endif()

target_link_libraries(zithcLib PUBLIC
    zith-core
    tomlplusplus::tomlplusplus
)

if(NOT ZITH_IS_WASM)
    if(ZITH_IS_WASM)
    add_executable(zith-playground src/wasm/playground.cpp src/wasm/wasi-stubs.cpp)
    target_link_libraries(zith-playground PRIVATE zithcLib)
    set_target_properties(zith-playground PROPERTIES OUTPUT_NAME zith-playground SUFFIX ".wasm")
    target_link_options(zith-playground PRIVATE -Wl,--no-entry -Wl,--export=zith_alloc -Wl,--export=zith_free -Wl,--export=zith_run_source -Wl,--export=zith_compile_source -Wl,--export=zith_last_error_ptr -Wl,--export=zith_last_error_len )
    target_compile_options(zith-playground PRIVATE -Wno-unknown-attributes -Wno-missing-prototypes)
endif()

target_link_libraries(zithcLib PUBLIC mio)
    get_target_property(_mio_includes mio INTERFACE_INCLUDE_DIRECTORIES)
    target_include_directories(zithcLib SYSTEM PUBLIC ${_mio_includes})
endif()

if(ZITH_HAS_LLVM AND NOT ZITH_IS_WASM)
        llvm_map_components_to_libnames(LLVM_LIBS core support irreader passes target X86CodeGen X86AsmParser X86Desc X86Info MC MCDisassembler MCParser AsmPrinter SelectionDAG GlobalISel WebAssemblyCodeGen WebAssemblyAsmParser WebAssemblyDesc WebAssemblyInfo)
        if(ZITH_IS_WASM)
    add_executable(zith-playground src/wasm/playground.cpp src/wasm/wasi-stubs.cpp)
    target_link_libraries(zith-playground PRIVATE zithcLib)
    set_target_properties(zith-playground PROPERTIES OUTPUT_NAME zith-playground SUFFIX ".wasm")
    target_link_options(zith-playground PRIVATE -Wl,--no-entry -Wl,--export=zith_alloc -Wl,--export=zith_free -Wl,--export=zith_run_source -Wl,--export=zith_compile_source -Wl,--export=zith_last_error_ptr -Wl,--export=zith_last_error_len )
    target_compile_options(zith-playground PRIVATE -Wno-unknown-attributes -Wno-missing-prototypes)
endif()

target_link_libraries(zithcLib PUBLIC ${LLVM_LIBS})
        target_include_directories(zithcLib SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS})
        target_compile_definitions(zithcLib PUBLIC ZITH_HAS_LLVM=1)
    endif()
target_compile_definitions(zithcLib PUBLIC ZITH_VERSION="${ZITH_VERSION}")

if(ZITH_BUILD_CLI AND NOT ZITH_IS_WASM)
    add_executable(zithc src/cli/zithc-main.cpp)
    target_link_libraries(zithc PRIVATE zithcLib)
endif()

include(CTest)

if(BUILD_TESTING AND NOT ZITH_IS_WASM)
    function(add_zith_test NAME SOURCE)
        add_executable(${NAME} ${SOURCE})
        target_link_libraries(${NAME} PRIVATE zithcLib)
        add_test(NAME ${NAME} COMMAND ${NAME})
    endfunction()

    add_zith_test(test-lexer tests/test-lexer.cpp)
    add_zith_test(test-import tests/test-import.cpp)
    add_zith_test(test-alias tests/test-alias.cpp)
    add_zith_test(test-struct tests/test-struct.cpp)
    add_zith_test(test-scan tests/test-scan.cpp)
    add_zith_test(test-sema tests/test-sema.cpp)
    add_zith_test(test-formatter tests/test-formatter.cpp)
    add_zith_test(test-recovery tests/test-recovery.cpp)
    add_zith_test(test-telemetry tests/test-telemetry.cpp)
    add_zith_test(test-frontend-context tests/test-frontend-context.cpp)
    add_zith_test(test-parser-expr tests/test-parser-expr.cpp)
    add_zith_test(test-cache tests/test-cache.cpp)
    if(ZITH_HAS_LLVM)
        add_zith_test(test-codegen tests/test-codegen.cpp)
    endif()
endif()

if(ZITH_BUILD_CLI AND NOT ZITH_IS_WASM)
    install(TARGETS zithc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(ZITH_BUILD_CAPI)
    install(TARGETS zithcLib
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zithc
    )
endif()

install(DIRECTORY stdlib/
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/zith/stdlib
    FILES_MATCHING PATTERN "*.zith"
)

find_program(CLANG_FORMAT_EXE NAMES clang-format clang-format-18 clang-format-17 clang-format-16)

if(CLANG_FORMAT_EXE)
    file(GLOB_RECURSE ZITH_FORMAT_SOURCES
        "src/*.cpp" "src/*.hpp" "src/*.h" "src/*.c"
        "tests/*.cpp" "tests/*.h"
    )
    add_custom_target(fmt COMMAND ${CLANG_FORMAT_EXE} -i ${ZITH_FORMAT_SOURCES})
    add_custom_target(fmt-check COMMAND ${CLANG_FORMAT_EXE} --dry-run --Werror ${ZITH_FORMAT_SOURCES})
endif()
# ── Compiler flags: -Werror -Weverything for Clang ────────────────────────

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(ZITH_CLANG_WARNINGS
        -Wall
        -Werror
        -Weverything
        -Wno-unknown-warning-option
        -Wno-disabled-macro-expansion
        -Wno-switch-enum
        -Wno-c++98-compat
        -Wno-switch-default
        -Wno-c++98-compat-pedantic
        -Wno-reserved-module-identifier
        -Wno-unsafe-buffer-usage
        -Wno-padded
        -Wno-unsafe-buffer-usage-in-libc-call
        -Wno-old-style-cast
        -Wno-format-nonliteral
        -Wno-missing-field-initializers
    )
    target_compile_options(zithcLib PRIVATE ${ZITH_CLANG_WARNINGS})
    if(TARGET zithc)
        target_compile_options(zithc PRIVATE ${ZITH_CLANG_WARNINGS})
    endif()
    if(BUILD_TESTING AND NOT ZITH_IS_WASM)
        foreach(TARGET test-lexer test-import test-alias test-struct test-scan test-sema test-formatter test-codegen test-parser-expr)
            if(TARGET ${TARGET})
                target_compile_options(${TARGET} PRIVATE ${ZITH_CLANG_WARNINGS})
            endif()
        endforeach()
    endif()
endif()
