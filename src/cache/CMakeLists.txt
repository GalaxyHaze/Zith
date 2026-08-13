set(_zith_cache_generated
    "${CMAKE_BINARY_DIR}/src/cache/cache-section.hpp"
    "${CMAKE_BINARY_DIR}/src/cache/cache-section.cpp"
    "${CMAKE_BINARY_DIR}/src/cache/cache-codec.gen.hpp"
    "${CMAKE_BINARY_DIR}/src/cache/cache-codec.gen.cpp"
)

add_custom_command(
    OUTPUT ${_zith_cache_generated}
    BYPRODUCTS "${CMAKE_BINARY_DIR}/src/cache/.gitignore"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/cache.rules"
            --out "${CMAKE_BINARY_DIR}/src/cache"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/cache.rules"
        "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/__init__.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/errors.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/runtime.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/text.py"
    VERBATIM
    COMMENT "Generating cache section surface from rules"
)

add_custom_target(zith_cache_generated DEPENDS ${_zith_cache_generated})

add_library(zith_cache STATIC
    "${CMAKE_BINARY_DIR}/src/cache/cache-section.cpp"
    "${CMAKE_BINARY_DIR}/src/cache/cache-codec.gen.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/cache-buffer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/cache-codec.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/cache-entry.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/manifest.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/cache.cpp"
)

add_dependencies(zith_cache zith_cache_generated)

target_include_directories(zith_cache PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
)

target_link_libraries(zith_cache PRIVATE
    zith_common
)
