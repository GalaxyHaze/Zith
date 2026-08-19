add_subdirectory(diagnostic)
add_subdirectory(symbols)
add_subdirectory(common)
add_subdirectory(support)
add_subdirectory(sema)
add_subdirectory(resolution)
add_subdirectory(cache)
add_subdirectory(cli)
add_subdirectory(frontend)
add_subdirectory(config)
add_subdirectory(session)
add_subdirectory(facts)

if(ZCT_CODEGEN)
    add_subdirectory(codegen)
endif()

add_custom_target(zct_generate_sources
    DEPENDS
        zct_diagnostic_generated
        zct_symbols_generated
        zct_cli_generated
        zct_frontend_lexer_generated
        zct_frontend_ast_generated
        zct_frontend_parser_generated
        zct_flags_generated
        zct_cache_generated
        zct_session_generated
        zct_facts_generated
        zct_type_system_generated
)

add_subdirectory(app)
