cmake_minimum_required(VERSION 3.20)
project(Nova LANGUAGES C CXX)

# =============================================================================
# Standards & Policies
# =============================================================================
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

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
        GIT_TAG        v2.4.1
        GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(cli11)

# --- unordered_dense ---
FetchContent_Declare(unordered_dense
        GIT_REPOSITORY https://github.com/martinus/unordered_dense.git
        GIT_TAG        v4.0.0
        GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(unordered_dense)

# --- NovaABI: Provides the 'Nova' INTERFACE target ---
FetchContent_Declare(NovaABI
        GIT_REPOSITORY https://github.com/GalaxyHaze/Nova.git
        GIT_TAG        master
        GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(NovaABI)

#find_package(LLVM REQUIRED CONFIG)
#message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")

# =============================================================================
# Compiler Flags
# =============================================================================
# Prefer target-specific flags over global add_definitions where possible
if(MSVC)
    add_compile_options(/W4 /WX)
else()
    #add_compile_options(-Wall -Wextra -Wpedantic -Werror)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        if(EXISTS "/usr/bin/ld.lld" OR EXISTS "/usr/local/bin/ld.lld")
            set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld")
            set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fuse-ld=lld")
            message(STATUS "Using LLD linker")
        endif()
    endif()
endif()

# =============================================================================
# Sources
# =============================================================================
set(CORE_C_SOURCES
        impl/memory/arena_c_functions.c
        impl/memory/file.c
)

set(PARSER_CPP_SOURCES
        impl/parser/tokenizer.cpp
        impl/parser/keywords.cpp
        impl/parser/parser.cpp
        impl/ast/ast.cpp
)

# REMOVED parser.cpp from CLI_SOURCES to prevent duplicate compilation
set(CLI_SOURCES
        impl/interface/CLI.cpp
)

# =============================================================================
# Targets
# =============================================================================

# Core C library (implements parts of Nova ABI)
add_library(NovaCore STATIC ${CORE_C_SOURCES})
target_include_directories(NovaCore PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# Link against Nova ABI → gets nova.h + C++17 requirement
target_link_libraries(NovaCore PUBLIC Nova::Nova)

# Apply version definition specifically to targets that need it, or globally if preferred
target_compile_definitions(NovaCore PRIVATE NOVA_VERSION="v0.0-alpha")

# Parser library
add_library(NovaParse STATIC ${PARSER_CPP_SOURCES})
target_link_libraries(NovaParse
        PUBLIC
        NovaCore          # Brings in Nova::Nova transitively
        #LLVM       # Using modern CMake target for LLVM is safer
        unordered_dense::unordered_dense
)
target_compile_definitions(NovaParse PRIVATE NOVA_VERSION="v0.0-alpha")

set_target_properties(NovaParse PROPERTIES
        VERSION 0.0.1
        SOVERSION 0
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
)

# Executable
add_executable(nova main.cpp ${CLI_SOURCES})
target_link_libraries(nova
        PRIVATE
        NovaParse
        CLI11::CLI11
        # No need to link LLVM or unordered_dense here directly;
        # NovaParse brings them in transitively if they are PUBLIC,
        # but since they are PRIVATE in NovaParse, we only need NovaParse logic.
)
target_compile_definitions(nova PRIVATE NOVA_VERSION="v0.0-alpha" NOVA_EXECUTABLE)

# =============================================================================
# Install (Optional)
# =============================================================================
include(GNUInstallDirs)
install(TARGETS nova NovaParse NovaCore
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# =============================================================================
# Final Info
# =============================================================================
message("")
message(STATUS "💡 Build Summary")
# message(STATUS "   NovaABI version: ${Nova_ABI_VERSION}")  # Commented out as variable is likely unset
#message(STATUS "   LLVM version: ${LLVM_PACKAGE_VERSION}")
if(TARGET Nova::Nova)
    message(STATUS "   Nova::Nova target found.")
endif()
message("")