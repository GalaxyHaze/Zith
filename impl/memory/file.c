// src/utils/file.c
#include <zith/zith.hpp>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
#endif
#include <sys/stat.h>

#include <ctype.h>
#include <stdbool.h>

#ifdef _WIN32
#include <io.h>
#define access _access
#define F_OK 0
#else
#include <unistd.h>
#endif
#ifdef _MSC_VER
#pragma warning(disable: 4200)
#endif

// Diagnostics — only I/O error reporting needed
extern void zith_io_error(const char *fmt, ...);

// Extensão canónica dos ficheiros fonte Zith
#define ZITH_SOURCE_EXT ".kali"

// ============================================================================
// Helpers internos
// ============================================================================

static int is_regular_file(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode);
}

// ============================================================================
// API pública
// ============================================================================

bool zith_file_exists(const char *path) {
    return access(path, F_OK) == 0;
}

bool zith_file_is_regular(const char *path) {
    return is_regular_file(path);
}

size_t zith_file_size(const char *path) {
    if (!is_regular_file(path)) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (size_t) st.st_size;
}

// Comparação de extensão case-insensitive
int zith_extension_matches(const char *path, const char *expected_ext) {
    if (!path || !expected_ext) return 0;

    const char *ext = strrchr(path, '.');
    if (!ext) return 0;

    const size_t len1 = strlen(ext);
    const size_t len2 = strlen(expected_ext);
    if (len1 != len2) return 0;

    for (size_t i = 0; i < len1; ++i) {
        if (tolower((unsigned char) ext[i]) != tolower((unsigned char) expected_ext[i]))
            return 0;
    }
    return 1;
}

// Verifica se o ficheiro tem a extensão canónica (.kali)
bool zith_is_source_file(const char *path) {
    return zith_extension_matches(path, ZITH_SOURCE_EXT);
}

// Carrega um ficheiro fonte (.kali) para a arena.
// Falha com erro descritivo se a extensão não for .kali,
// o ficheiro não existir, não for regular, ou a leitura falhar.
char *zith_load_file_to_arena(struct ZithArena *arena,
                                  const char *path, size_t *out_size) {
    if (!arena || !path || !out_size) {
        if (out_size) *out_size = 0;
        return NULL;
    }

    // Verificação de extensão — rejeita antes de abrir o ficheiro
    if (!zith_is_source_file(path)) {
        zith_io_error("'%s' is not a Zith source file (expected '%s')",
                path, ZITH_SOURCE_EXT);
        *out_size = 0;
        return NULL;
    }

    if (!is_regular_file(path)) {
        zith_io_error("'%s' is not a regular file", path);
        *out_size = 0;
        return NULL;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        zith_io_error("Failed to open '%s'", path);
        *out_size = 0;
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) goto error;
    const long size = ftell(f);
    if (size < 0) goto error;
    if (fseek(f, 0, SEEK_SET) != 0) goto error;

    // Ficheiro vazio — válido, retorna buffer com '\0'
    if (size == 0) {
        fclose(f);
        *out_size = 0;
        char *empty = zith_arena_alloc(arena, 1);
        if (empty) empty[0] = '\0';
        return empty;
    }

    {
        char *buffer = zith_arena_alloc(arena, (size_t) size);
        if (!buffer) goto error;

        const size_t read = fread(buffer, 1, (size_t) size, f);
        fclose(f);

        if (read != (size_t) size) {
            zith_io_error("Failed to read '%s' (read %zu of %ld bytes)",
                    path, read, size);
            *out_size = 0;
            return NULL;
        }

        *out_size = (size_t) size;
        return buffer;
    }

error:
    fclose(f);
    zith_io_error("I/O error while loading '%s'", path);
    *out_size = 0;
    return NULL;
}