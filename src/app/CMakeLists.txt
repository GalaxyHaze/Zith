add_executable(zithc
    zithc-main.cpp
)

target_link_libraries(zithc PRIVATE
    zct_cli
    zct_frontend_lexer
    zct_flags
    zct_common
    zct_session
)

add_dependencies(zithc zct_generate_sources)
