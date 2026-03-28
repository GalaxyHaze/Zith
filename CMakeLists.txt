cmake_minimum_required(VERSION 3.20)
project(kalidous LANGUAGES C CXX)

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
# CORREÇÃO: Garante que o FetchContent baixe e construa o tomlplusplus
# mesmo que o sistema não tenha o pacote instalado.
FetchContent_Declare(
        tomlplusplus
        GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
        GIT_TAG v3.4.0
        GIT_SHALLOW TRUE
)
# Tornar disponível antes de tentar usar
FetchContent_MakeAvailable(tomlplusplus)


# =============================================================================
# Compiler Flags
# =============================================================================
if (MSVC)
    add_compile_options(/W4 /WX)
else ()
    # Cuidado com -Werror em compilações cruzadas (CI), pode falhar por warnings de versão do compilador
    # add_compile_options(-Wall -Wextra -Wpedantic -Werror)
    
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        # Verifica se o linker lld existe antes de forçar
        if (EXISTS "/usr/bin/ld.lld" OR EXISTS "/usr/local/bin/ld.lld")
            set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld")
            set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fuse-ld=lld")
            message(STATUS "Using LLD linker")
        endif ()
    endif ()
endif ()

# =============================================================================
# Sources
# =============================================================================
set(CORE_C_SOURCES
        impl/memory/arena_c_functions.c
        impl/memory/file.c
)

# CORREÇÃO: Removido 'impl/ast/ast.h' e duplicata de 'tokenizer.cpp'
set(PARSER_CPP_SOURCES
        impl/lexer/tokenizer.cpp
        impl/lexer/keywords.cpp
        impl/parser/parser.cpp
        impl/ast/ast.cpp
        impl/parser/parser.h
)

set(CLI_SOURCES
        impl/cli/CLI.cpp
        impl/memory/utils.h
        impl/lexer/debug.h
        include/kalidous/kalidous.hpp
)

# ───────────────────────────────────────────────
# Targets
# ───────────────────────────────────────────────

add_library(KalidousCore STATIC ${CORE_C_SOURCES})
target_include_directories(KalidousCore
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_compile_definitions(KalidousCore PRIVATE KALIDOUS_VERSION="v0.0-alpha")
# Agora o tomlplusplus::tomlplusplus deve ser encontrado com segurança
target_link_libraries(KalidousCore PUBLIC tomlplusplus::tomlplusplus)

# ────────
add_library(KalidousParse STATIC ${PARSER_CPP_SOURCES})
target_include_directories(KalidousParse
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(KalidousParse
        PUBLIC
        KalidousCore
        unordered_dense::unordered_dense
)
target_compile_definitions(KalidousParse PRIVATE KALIDOUS_VERSION="v0.0-alpha")

set_target_properties(KalidousParse PROPERTIES
        VERSION 0.0.1
        SOVERSION 0
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
)

# ────────
add_executable(kalidous impl/main.cpp ${CLI_SOURCES})

target_include_directories(kalidous PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(kalidous
        PRIVATE
        KalidousParse
        CLI11::CLI11
)
target_compile_definitions(kalidous PRIVATE KALIDOUS_VERSION="v0.0-alpha" KALIDOUS_EXECUTABLE)

# ───────────────────────────────────────────────
# Install
# ───────────────────────────────────────────────
install(TARGETS kalidous KalidousParse KalidousCore
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# =============================================================================
# Final Info
# =============================================================================
message("")
message(STATUS "💡 Build Summary")
if (TARGET kalidous)
    message(STATUS "   Executable 'kalidous' target configured.")
endif()
message("")

# =============================================================================
# WebAssembly (Emscripten)
# =============================================================================
if(EMSCRIPTEN)
    add_executable(kalidous-wasm impl/main.cpp)

    target_include_directories(kalidous-wasm PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(kalidous-wasm PRIVATE KalidousParse)

    target_compile_definitions(kalidous-wasm PRIVATE
        KALIDOUS_VERSION="v0.0-alpha"
        KALIDOUS_EXECUTABLE
        KALIDOUS_WASM
    )

    # LINK_FLAGS passa como string única — evita o CMake splittar os -s flags
    set_target_properties(kalidous-wasm PROPERTIES
        SUFFIX ".html"
        OUTPUT_NAME "kalidous"
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
        LINK_FLAGS "-O3 -flto --closure 1 -s WASM=1 -s FILESYSTEM=1 -s ENVIRONMENT=web,node -s EXPORTED_RUNTIME_METHODS=['ccall','cwrap'] -s ALLOW_MEMORY_GROWTH=1"
    )
endif()