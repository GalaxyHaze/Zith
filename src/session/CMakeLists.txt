set(_zct_session_generated
    "${CMAKE_BINARY_DIR}/src/session/session.cpp"
    "${CMAKE_BINARY_DIR}/src/session/session.hpp"
    "${CMAKE_BINARY_DIR}/src/session/dispatch.hpp"
)

add_custom_command(
    OUTPUT ${_zct_session_generated}
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

add_custom_target(zct_session_generated DEPENDS ${_zct_session_generated})

add_library(zct_session_core_objects OBJECT
    "${CMAKE_BINARY_DIR}/src/session/session.cpp"
)

target_include_directories(zct_session_core_objects PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
    "${PROJECT_SOURCE_DIR}/src/frontend/lexer"
    "${PROJECT_BINARY_DIR}/src/frontend/lexer"
    "${PROJECT_SOURCE_DIR}/src/frontend/parser"
    "${PROJECT_BINARY_DIR}/src/frontend/parser"
    "${PROJECT_SOURCE_DIR}/src/frontend/ast"
    "${PROJECT_BINARY_DIR}/src/frontend/ast"
)

target_link_libraries(zct_session_core_objects PUBLIC
    zct_common
)

add_library(zct_session_core STATIC
    $<TARGET_OBJECTS:zct_session_core_objects>
)

target_link_libraries(zct_session_core PUBLIC
    zct_common
)

target_include_directories(zct_session_core PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
    "${PROJECT_SOURCE_DIR}/src/frontend/lexer"
    "${PROJECT_BINARY_DIR}/src/frontend/lexer"
    "${PROJECT_SOURCE_DIR}/src/frontend/parser"
    "${PROJECT_BINARY_DIR}/src/frontend/parser"
    "${PROJECT_SOURCE_DIR}/src/frontend/ast"
    "${PROJECT_BINARY_DIR}/src/frontend/ast"
)

add_library(zct_session STATIC
    $<TARGET_OBJECTS:zct_session_core_objects>
    "${CMAKE_CURRENT_SOURCE_DIR}/dispatch.cpp"
)

target_link_libraries(zct_session PUBLIC
    zct_common
)

target_link_libraries(zct_session PRIVATE
    zct_frontend_parser
    zct_frontend_ast
)

target_include_directories(zct_session PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/src"
    "${PROJECT_SOURCE_DIR}/src/frontend/lexer"
    "${PROJECT_BINARY_DIR}/src/frontend/lexer"
    "${PROJECT_SOURCE_DIR}/src/frontend/parser"
    "${PROJECT_BINARY_DIR}/src/frontend/parser"
    "${PROJECT_SOURCE_DIR}/src/frontend/ast"
    "${PROJECT_BINARY_DIR}/src/frontend/ast"
)

if(ZCT_HAS_MIO)
    target_include_directories(zct_session SYSTEM PUBLIC
        "${PROJECT_BINARY_DIR}/_deps/mio-src/include"
    )
endif()

add_dependencies(zct_session_core_objects zct_session_generated)
add_dependencies(zct_session_core zct_session_generated)
add_dependencies(zct_session zct_session_generated)

add_executable(session-demo
    "${PROJECT_SOURCE_DIR}/tests/session/session-demo.cpp"
)

target_link_libraries(session-demo PRIVATE
    zct_session
    zct_frontend_lexer
    zct_common
)

add_dependencies(session-demo zct_session_generated)

add_test(NAME session-demo COMMAND session-demo)
