set(_zith_session_generated
    "${CMAKE_BINARY_DIR}/src/session/session.cpp"
    "${CMAKE_BINARY_DIR}/src/session/session.hpp"
    "${CMAKE_BINARY_DIR}/src/session/dispatch.hpp"
)

add_custom_command(
    OUTPUT ${_zith_session_generated}
    BYPRODUCTS "${CMAKE_BINARY_DIR}/src/session/.gitignore"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/session.rules"
            --out "${CMAKE_BINARY_DIR}/src/session"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/session.rules"
        "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/__init__.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/errors.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/runtime.py"
        "${PROJECT_SOURCE_DIR}/tools/rules_kit/text.py"
    VERBATIM
    COMMENT "Generating session sources from rules"
)

add_custom_target(zith_session_generated DEPENDS ${_zith_session_generated})

add_library(zith_session_core_objects OBJECT
    "${CMAKE_BINARY_DIR}/src/session/session.cpp"
)

target_include_directories(zith_session_core_objects PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
    "${PROJECT_SOURCE_DIR}/src/frontend/lexer"
    "${PROJECT_BINARY_DIR}/src/frontend/lexer"
)

target_link_libraries(zith_session_core_objects PUBLIC
    zith_common
)

add_library(zith_session_core STATIC
    $<TARGET_OBJECTS:zith_session_core_objects>
)

target_link_libraries(zith_session_core PUBLIC
    zith_common
)

target_include_directories(zith_session_core PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
    "${PROJECT_SOURCE_DIR}/src/frontend/lexer"
    "${PROJECT_BINARY_DIR}/src/frontend/lexer"
)

add_library(zith_session STATIC
    $<TARGET_OBJECTS:zith_session_core_objects>
    "${CMAKE_CURRENT_SOURCE_DIR}/dispatch.cpp"
)

target_link_libraries(zith_session PUBLIC
    zith_common
)

target_link_libraries(zith_session PRIVATE
    zith_frontend_lexer
)

target_include_directories(zith_session PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
    "${PROJECT_SOURCE_DIR}/src/frontend/lexer"
    "${PROJECT_BINARY_DIR}/src/frontend/lexer"
)

if(ZITH_HAS_MIO)
    target_include_directories(zith_session SYSTEM PUBLIC
        "${PROJECT_BINARY_DIR}/_deps/mio-src/include"
    )
endif()

add_dependencies(zith_session_core_objects zith_session_generated)
add_dependencies(zith_session_core zith_session_generated)
add_dependencies(zith_session zith_session_generated)
