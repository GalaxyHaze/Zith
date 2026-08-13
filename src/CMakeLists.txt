add_subdirectory(diagnostic)
add_subdirectory(symbols)
add_subdirectory(common)
add_subdirectory(cache)
add_subdirectory(cli)
add_subdirectory(frontend)
add_subdirectory(config)
add_subdirectory(session)

add_custom_target(zith_generate_sources
    DEPENDS
        zith_diagnostic_generated
        zith_symbols_generated
        zith_cli_generated
        zith_frontend_lexer_generated
        zith_frontend_ast_generated
        zith_frontend_parser_generated
        zith_flags_generated
        zith_cache_generated
        zith_session_generated
)

add_subdirectory(app)
