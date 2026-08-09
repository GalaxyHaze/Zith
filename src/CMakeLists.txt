add_subdirectory(common)
add_subdirectory(cli)
add_subdirectory(frontend)
add_subdirectory(config)

add_custom_target(zith_generate_sources
    DEPENDS
        zith_cli_generated
        zith_frontend_lexer_generated
        zith_project_config_generated
)

add_subdirectory(app)
