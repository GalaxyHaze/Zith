cmake_minimum_required(VERSION 3.20)
project(zith LANGUAGES C CXX)

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
# Project Version (Fixed)
# =============================================================================
# Use 'set' with CACHE STRING, NOT 'option' (which is boolean)
set(ZITH_VERSION "v0.0.1" CACHE STRING "Project version")

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

# =============================================================================
# Compiler Flags
# =============================================================================
if (MSVC)
    add_compile_options(/W4 /WX /wd4100) # /wd4100: unreferenced formal parameter
else ()
    # add_compile_options(-Wall -Wextra -Wpedantic -Werror)

    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        # Only probe for lld on native builds — cross toolchains (Zig, aarch64-gnu)
        # may not expose lld at the same path and would fail to link.
        if(NOT CMAKE_CROSSCOMPILING)
            find_program(LLD_EXE NAMES ld.lld lld)
            if(LLD_EXE)
                set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld")
                set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fuse-ld=lld")
                message(STATUS "Using LLD linker: ${LLD_EXE}")
            endif()
        endif()
    endif ()
endif ()

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

# Note: Usually we don't glob headers in add_executable, 
# but kept here if you rely on it for IDE project generation.
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
)
target_link_libraries(ZithCore PUBLIC tomlplusplus::tomlplusplus)

# ────────
add_library(ZithParse STATIC ${PARSER_CPP_SOURCES})
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

target_include_directories(zith PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(zith
    PRIVATE
        ZithParse
        CLI11::CLI11
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
# include(CTest) owns the BUILD_TESTING variable (defaults ON).
# Pass -DBUILD_TESTING=OFF to disable tests, e.g. in CI cross-compile builds.
include(CTest)

if(BUILD_TESTING)
    # --- Catch2 ---
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
        target_link_libraries(zith_tests PRIVATE ZithParse Catch2::Catch2WithMain)

        include(Catch)

        # ======================================================================
        # Cross-Compilation Fix
        # ======================================================================
        if(CMAKE_CROSSCOMPILING)
            # Cannot run the executable on the host to discover tests at build time.
            # Register a single generic test entry instead; requires an emulator to run.
            message(STATUS "Cross-compiling detected. Skipping test discovery.")
            add_test(NAME zith_tests COMMAND zith_tests)
        else()
            # Native build: automatically parse and register all Catch2 test cases.
            catch_discover_tests(zith_tests)
        endif()

        message(STATUS "💡 Tests target 'zith_tests' configured.")
    endif()
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
        target_compile_options(zith-wasm PRIVATE /W4 /WX /wd4100)
    else ()
        target_compile_options(zith-wasm PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()