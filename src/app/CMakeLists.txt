add_executable(zithc
    zithc-main.cpp
)

target_link_libraries(zithc PRIVATE
    zith_cli
    zith_frontend_lexer
    zith_project_config
    zith_common
)

add_dependencies(zithc zith_generate_sources)
