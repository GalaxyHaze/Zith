add_executable(turvc
    turvc-main.cpp
)

target_link_libraries(turvc PRIVATE
    zct_cli
    zct_frontend_lexer
    zct_flags
    zct_common
    zct_session
)

add_dependencies(turvc zct_generate_sources)
