cmake_minimum_required(VERSION 3.20)
project(zith LANGUAGES C CXX)

include(GNUInstallDirs)

# =============================================================================
# Standards & Policies
# =============================================================================
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if (POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif ()

# =============================================================================
# Output Directories
# =============================================================================
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# =============================================================================
# Project Version (Dynamic via CI or Default v0.0.1)
# =============================================================================
# Use FORCE para permitir sobrescrever via -DZITH_VERSION=... no CI
if(NOT DEFINED ZITH_VERSION OR ZITH_VERSION STREQUAL "")
    set(ZITH_VERSION "v0.0.1")
endif()
set(ZITH_VERSION "${ZITH_VERSION}" CACHE STRING "Project version" FORCE)

# =============================================================================
# Fetch External Dependencies
# =============================================================================
include(FetchContent)

# --- CLI11 ---
FetchContent_Declare(cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG v2.4.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(cli11)

# --- unordered_dense ---
FetchContent_Declare(unordered_dense
    GIT_REPOSITORY https://github.com/martinus/unordered_dense.git
    GIT_TAG v4.0.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(unordered_dense)

# --- tomlplusplus ---
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(tomlplusplus)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFI REQUIRED libffi)

# =============================================================================
# Compiler Flags
# =============================================================================
option(ENABLE_WERROR "Treat warnings as errors" OFF)

if (MSVC)
    add_compile_options(/W4 /wd4100) 
    if(ENABLE_WERROR)
        add_compile_options(/WX)
    endif()
else ()
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(ENABLE_WERROR)
        add_compile_options(-Werror)
    endif()

        # Clang Linker Setup (LLD)
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # Tenta encontrar o binário do linker.
        # Procura versões específicas (ex: ld.lld-18) e os nomes genéricos (ld.lld, lld)
        find_program(LLD_EXE 
            NAMES 
                ld.lld-18 ld.lld-17 ld.lld-16 
                ld.lld 
                lld 
        )

        if(LLD_EXE)
            # Pega apenas o nome do arquivo (ex: "ld.lld" ou "lld")
            get_filename_component(LLD_NAME "${LLD_EXE}" NAME_WE)
            
            # Se o nome encontrado for "lld", removemos o ".exe" se estiver no Windows, 
            # e garantimos que a flag fique correta. 
            # Nota: Se o binário é 'lld', o Clang geralmente aceita '-fuse-ld=lld', 
            # mas 'ld.lld' é o padrão mais seguro para evitar erros de dispacher.
            
            if (NOT CMAKE_EXE_LINKER_FLAGS MATCHES "-fuse-ld=")
                set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=${LLD_NAME}")
                set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fuse-ld=${LLD_NAME}")
                message(STATUS "✓ Using LLD linker: ${LLD_EXE} (flag: -fuse-ld=${LLD_NAME})")
            endif()
        else()
            # Se não encontrar, NÃO força a flag para evitar o erro "invalid linker name"
            message(WARNING "⚠ LLD linker not found in PATH. Build will proceed with default linker.")
        endif()
    endif ()
endif ()

# =============================================================================
# Clang Extension Configuration
# =============================================================================
# Se você tiver um caminho para a extensão, defina aqui.
# Exemplo: -DCLANG_PLUGIN_PATH=/path/to/libMyPlugin.so
if(DEFINED CLANG_PLUGIN_PATH AND EXISTS "${CLANG_PLUGIN_PATH}")
    message(STATUS "Loading Clang Extension: ${CLANG_PLUGIN_PATH}")
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:CXX>:-Xclang>-load"
        "$<$<COMPILE_LANGUAGE:CXX>:-Xclang>${CLANG_PLUGIN_PATH}"
    )
    # Se a extensão adicionar argumentos adicionais, use -Xclang -plugin-arg-<NomePlugin>...
endif()

# =============================================================================
# LLVM Configuration
# =============================================================================
set(LLVM_DIR "" CACHE PATH "Path to LLVMConfig.cmake (optional)")
if(LLVM_DIR)
    find_package(LLVM REQUIRED CONFIG PATHS "${LLVM_DIR}" NO_DEFAULT_PATH)
else()
    find_package(LLVM REQUIRED CONFIG)
endif()
message(STATUS "✓ Found LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")

add_definitions(-DLLVM_VERSION_MAJOR=${LLVM_VERSION_MAJOR})
include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})

llvm_map_components_to_libnames(LLVM_LIBRARIES
    analysis
    core
    instcombine
    scalaropts
    vectorize
    ipo
    irreader
    support
    target
)

# =============================================================================
# Sources
# =============================================================================
file(GLOB_RECURSE CORE_C_SOURCES "impl/memory/*.c")
file(GLOB_RECURSE DIAGNOSTICS_SOURCES "impl/diagnostics/*.cpp")
file(GLOB_RECURSE PARSER_CPP_SOURCES
    "impl/lexer/*.cpp"
    "impl/parser/*.cpp"
    "impl/ast/*.cpp"
)

file(GLOB_RECURSE IMPORT_SOURCES "impl/import/*.hpp")

file(GLOB_RECURSE CLI_SOURCES
    "impl/cli/*.cpp"
    "impl/memory/*.h"
    "impl/lexer/*.h"
)

# ───────────────────────────────────────────────
# Targets
# ───────────────────────────────────────────────

add_library(ZithCore STATIC ${CORE_C_SOURCES} ${DIAGNOSTICS_SOURCES})
target_include_directories(ZithCore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        ${LLVM_INCLUDE_DIRS}
        ${FFI_INCLUDE_DIRS}
)
target_link_libraries(ZithCore PUBLIC 
    tomlplusplus::tomlplusplus
    ${LLVM_LIBRARIES}
)

# ────────
add_library(ZithParse STATIC ${PARSER_CPP_SOURCES} ${IMPORT_SOURCES})
target_include_directories(ZithParse
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(ZithParse
    PUBLIC
        ZithCore
        unordered_dense::unordered_dense
)
target_compile_definitions(ZithParse PRIVATE ZITH_VERSION="${ZITH_VERSION}")

set_target_properties(ZithParse PROPERTIES
    VERSION 0.0.1
    SOVERSION 0
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# ────────
add_executable(zith impl/main.cpp ${CLI_SOURCES})

target_include_directories(zith PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${FFI_INCLUDE_DIRS})

target_link_libraries(zith
    PRIVATE
        ZithParse
        CLI11::CLI11
        ${FFI_LIBRARIES}
)
target_compile_definitions(zith PRIVATE ZITH_VERSION="${ZITH_VERSION}" ZITH_EXECUTABLE)

# ───────────────────────────────────────────────
# Install
# ───────────────────────────────────────────────
install(TARGETS zith ZithParse ZithCore
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# =============================================================================
# Tests
# =============================================================================
include(CTest)

if(BUILD_TESTING)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.7.1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(Catch2)

    file(GLOB_RECURSE TEST_SOURCES "tests/*.cpp")

    if(TEST_SOURCES)
        add_executable(zith_tests ${TEST_SOURCES})
        target_include_directories(zith_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        
        # Garante que os testes também usem a versão correta
        target_compile_definitions(zith_tests PRIVATE ZITH_VERSION="${ZITH_VERSION}")

        target_link_libraries(zith_tests PRIVATE ZithParse Catch2::Catch2WithMain)

        include(Catch)

        if(CMAKE_CROSSCOMPILING)
            message(STATUS "Cross-compiling detected. Skipping test discovery.")
            add_test(NAME zith_tests COMMAND zith_tests)
        else()
            catch_discover_tests(zith_tests)
        endif()

        message(STATUS "💡 Tests target 'zith_tests' configured.")
    endif()

    add_test(
        NAME zith_cli_hello_interpreted
        COMMAND $<TARGET_FILE:zith> run --interpreted ${CMAKE_SOURCE_DIR}/examples/start/hello-world.zith
    )
endif()

# =============================================================================
# Final Info
# =============================================================================
message("")
message(STATUS "💡 Build Summary")
message(STATUS "   Version: ${ZITH_VERSION}")
if (TARGET zith)
    message(STATUS "   Executable 'zith' target configured.")
endif()
if (TARGET zith_tests)
    message(STATUS "   Tests executable 'zith_tests' configured.")
endif()
message("")

# =============================================================================
# WebAssembly (Emscripten)
# =============================================================================
if(EMSCRIPTEN)
    add_executable(zith-wasm impl/main.cpp)

    target_include_directories(zith-wasm PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(zith-wasm PRIVATE ZithParse)

    target_compile_definitions(zith-wasm PRIVATE
        ZITH_VERSION="${ZITH_VERSION}"
        ZITH_EXECUTABLE
        ZITH_WASM
    )

    target_link_options(zith-wasm PRIVATE
        "SHELL:-O3"
        "SHELL:-flto"
        "SHELL:--closure 1"
        "SHELL:-s WASM=1"
        "SHELL:-s FILESYSTEM=1"
        "SHELL:-s ENVIRONMENT=web,node"
        "SHELL:-s EXPORTED_RUNTIME_METHODS=['ccall','cwrap']"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
    )

    set_target_properties(zith-wasm PROPERTIES
        SUFFIX ".html"
        OUTPUT_NAME "zith"
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )

    if (MSVC)
        target_compile_options(zith-wasm PRIVATE /W4 /wd4100)
    else ()
        target_compile_options(zith-wasm PRIVATE -Wall -Wextra -Wpedantic -Wno-error)
    endif()
endif()
