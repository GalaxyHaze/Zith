// zith.h - Unified header for Zith programming language core
#pragma once

#include <stddef.h>
#ifdef __cplusplus
#include <string>
#include <stdexcept>
#endif 
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Core Types & Utilities
// ============================================================================

typedef struct {
    size_t index;
    size_t line;
} ZithSourceLoc;

typedef struct {
    const void *data;
    size_t len;
} ZithSlice;

typedef struct {
    const char *data;
    size_t len;
} ZithStr;

// ============================================================================
// Token System
// ============================================================================

typedef enum {
    // ------------------------------------------------------------------------
    // Literais e identificadores
    // ------------------------------------------------------------------------
    ZITH_TOKEN_STRING,
    ZITH_TOKEN_NUMBER,
    ZITH_TOKEN_HEXADECIMAL,
    ZITH_TOKEN_OCTAL,
    ZITH_TOKEN_BINARY,
    ZITH_TOKEN_FLOAT,
    ZITH_TOKEN_IDENTIFIER,

    // ------------------------------------------------------------------------
    // Operadores aritméticos e lógicos
    // ------------------------------------------------------------------------
    ZITH_TOKEN_PLUS,
    ZITH_TOKEN_MINUS,
    ZITH_TOKEN_MULTIPLY,
    ZITH_TOKEN_DIVIDE,
    ZITH_TOKEN_MOD,

    ZITH_TOKEN_AND,
    ZITH_TOKEN_OR,
    ZITH_TOKEN_NOT,

    // ------------------------------------------------------------------------
    // Operadores de comparação
    // ------------------------------------------------------------------------
    ZITH_TOKEN_EQUAL,
    ZITH_TOKEN_NOT_EQUAL,
    ZITH_TOKEN_LESS_THAN,
    ZITH_TOKEN_GREATER_THAN,
    ZITH_TOKEN_LESS_THAN_OR_EQUAL,
    ZITH_TOKEN_GREATER_THAN_OR_EQUAL,

    // ------------------------------------------------------------------------
    // Operadores de atribuição
    // ------------------------------------------------------------------------
    ZITH_TOKEN_ASSIGNMENT,
    ZITH_TOKEN_DECLARATION, // :=
    ZITH_TOKEN_PLUS_EQUAL,
    ZITH_TOKEN_MINUS_EQUAL,
    ZITH_TOKEN_MULTIPLY_EQUAL,
    ZITH_TOKEN_DIVIDE_EQUAL,

    // ------------------------------------------------------------------------
    // Operadores especiais
    // ------------------------------------------------------------------------
    ZITH_TOKEN_QUESTION, // ?  optional
    ZITH_TOKEN_BANG, // !  type may fail
    ZITH_TOKEN_ARROW, // -> encadeamento de funções

    // ------------------------------------------------------------------------
    // Delimitadores
    // ------------------------------------------------------------------------
    ZITH_TOKEN_LPAREN,
    ZITH_TOKEN_RPAREN,
    ZITH_TOKEN_LBRACE,
    ZITH_TOKEN_RBRACE,
    ZITH_TOKEN_LBRACKET,
    ZITH_TOKEN_RBRACKET,
    ZITH_TOKEN_DOT,
    ZITH_TOKEN_DOTS, // ...
    ZITH_TOKEN_SLASH, // / path separator (also division in expression context)
    ZITH_TOKEN_COMMA,
    ZITH_TOKEN_COLON,
    ZITH_TOKEN_SEMICOLON,

    // ------------------------------------------------------------------------
    // Keywords: controle de fluxo
    // ------------------------------------------------------------------------
    ZITH_TOKEN_IF,
    ZITH_TOKEN_ELSE,
    ZITH_TOKEN_FOR,
    ZITH_TOKEN_IN,
    ZITH_TOKEN_WHILE, // reservado na ABI, não usado como keyword ativa
    ZITH_TOKEN_SWITCH,
    ZITH_TOKEN_RETURN,
    ZITH_TOKEN_BREAK,
    ZITH_TOKEN_CONTINUE,
    ZITH_TOKEN_GOTO,
    ZITH_TOKEN_MARKER,
    ZITH_TOKEN_SCENE,

    // ------------------------------------------------------------------------
    // Keywords: concorrência / fluxo assíncrono
    // ------------------------------------------------------------------------
    ZITH_TOKEN_SPAWN,
    ZITH_TOKEN_JOINED,
    ZITH_TOKEN_AWAIT,
    ZITH_TOKEN_JOIN,

    // ------------------------------------------------------------------------
    // Keywords: tratamento de erros
    // ------------------------------------------------------------------------
    ZITH_TOKEN_TRY,
    ZITH_TOKEN_CATCH,
    ZITH_TOKEN_MUST, // "must!" — o ! é semântico, o Parser resolve
    ZITH_TOKEN_THROW,
    ZITH_TOKEN_DO,
    ZITH_TOKEN_DROP,

    // ------------------------------------------------------------------------
    // Modificadores de propriedade e escopo
    // ------------------------------------------------------------------------
    ZITH_TOKEN_CONST,
    ZITH_TOKEN_MUTABLE, // keyword: 'mut'
    ZITH_TOKEN_VAR,
    ZITH_TOKEN_LET,
    ZITH_TOKEN_AUTO,

    ZITH_TOKEN_GLOBAL,
    ZITH_TOKEN_LEND,
    ZITH_TOKEN_SHARED,
    ZITH_TOKEN_VIEW,
    ZITH_TOKEN_UNIQUE,
    ZITH_TOKEN_EXTENSION,
    ZITH_TOKEN_PACK, // reservado na ABI; [] é resolvido pelo Parser

    // ------------------------------------------------------------------------
    // Modificadores de acesso
    // ------------------------------------------------------------------------
    ZITH_TOKEN_MODIFIER, // public / private / protected

    // ------------------------------------------------------------------------
    // Declarações de tipo
    // ------------------------------------------------------------------------
    ZITH_TOKEN_TYPE,
    ZITH_TOKEN_STRUCT,
    ZITH_TOKEN_COMPONENT,
    ZITH_TOKEN_ENUM,
    ZITH_TOKEN_UNION,
    ZITH_TOKEN_RAW,
    ZITH_TOKEN_FAMILY,
    ZITH_TOKEN_ENTITY,
    ZITH_TOKEN_TRAIT,
    ZITH_TOKEN_TYPEDEF,
    ZITH_TOKEN_IMPLEMENT,
    ZITH_TOKEN_NULL,

    // ------------------------------------------------------------------------
    // Tokens especiais / controle
    // ------------------------------------------------------------------------
    ZITH_TOKEN_END,
    ZITH_TOKEN_UNKNOWN, ZITH_TOKEN_RECURSE,
    ZITH_TOKEN_YIELD, ZITH_TOKEN_ASYNC, ZITH_TOKEN_FN,
    ZITH_TOKEN_FLOWING, ZITH_TOKEN_ENTRY, ZITH_TOKEN_NORETURN, ZITH_TOKEN_IMPORT,
    ZITH_TOKEN_USE, ZITH_TOKEN_CONTEXT, ZITH_TOKEN_MACRO,
    ZITH_TOKEN_EXPORT, ZITH_TOKEN_FROM, ZITH_TOKEN_AS,
    ZITH_TOKEN_REQUIRE, ZITH_TOKEN_IS,
    ZITH_TOKEN_PREFIX, ZITH_TOKEN_SUFIX,
    ZITH_TOKEN_INFIX
} ZithTokenType;

typedef struct {
    ZithStr lexeme;
    ZithSourceLoc loc;
    ZithTokenType type;
    uint16_t keyword_id;
} ZithToken;

typedef struct {
    const ZithToken *data;
    size_t len;
} ZithTokenStream;

typedef struct ZithArena ZithArena;

ZithTokenStream zith_tokenize(ZithArena *arena, const char *source, size_t source_len);

void zith_debug_tokenize(ZithArena *arena, const char *source, size_t source_len);

// ============================================================================
// AST System
// ============================================================================

typedef uint16_t ZithNodeId;

enum {
    ZITH_NODE_ERROR = 0,

    ZITH_NODE_LITERAL = 100,
    ZITH_NODE_IDENTIFIER = 101,
    ZITH_NODE_BINARY_OP = 102,
    ZITH_NODE_UNARY_OP = 103,
    ZITH_NODE_CALL = 104,
    ZITH_NODE_INDEX = 105,
    ZITH_NODE_MEMBER = 106,

    ZITH_NODE_VAR_DECL = 200,
    ZITH_NODE_FUNC_DECL = 201,
    ZITH_NODE_PARAM = 202,

    ZITH_NODE_BLOCK = 300,
    ZITH_NODE_IF = 301,
    ZITH_NODE_FOR = 302, // unifica for e while
    ZITH_NODE_RETURN = 303,
    ZITH_NODE_EXPR_STMT = 304,
    ZITH_NODE_UNBODY = 305, // corpo não parseado (entre { e }) - scanner mode

    ZITH_NODE_TYPE_REF = 400,
    ZITH_NODE_TYPE_FUNC = 401,

    ZITH_NODE_CUSTOM_START = 1000
};

typedef struct ZithNode ZithNode;

struct ZithNode {
    union {
        struct {
            ZithNode *a;
            ZithNode *b;
            ZithNode *c;
        } kids;

        struct {
            void *ptr;
            size_t len;
        } list;

        struct {
            const char *str;
            size_t len;
        } ident;

        struct {
            double num;
        } number;

        struct {
            bool value;
        } boolean;

        uint64_t custom;
    } data;
    ZithSourceLoc loc;
    ZithNodeId type;



};

ZithNode *zith_parse(ZithArena *arena, ZithTokenStream tokens);

ZithNode *zith_parse_with_source(ZithArena *arena, const char *source,
                                         size_t source_len, const char *filename,
                                         ZithTokenStream tokens);


static inline ZithNodeId zith_node_type(const ZithNode *node) {
    return node ? node->type : (ZithNodeId) ZITH_NODE_ERROR;
}

// ============================================================================
// Memory Arena
// ============================================================================

ZithArena *zith_arena_create(size_t initial_block_size);

void *zith_arena_alloc(ZithArena *arena, size_t size);

char *zith_arena_strdup(ZithArena *arena, const char *str);

void zith_arena_reset(ZithArena *arena);

void zith_arena_destroy(ZithArena *arena);

// ============================================================================
// File Utilities
// ============================================================================

bool zith_file_exists(const char *path);

bool zith_file_is_regular(const char *path);

size_t zith_file_size(const char *path);

bool zith_file_has_extension(const char *path, const char *ext);

char *zith_load_file_to_arena(ZithArena *arena, const char *path, size_t *out_size);

int zith_run(int argc, const char *const argv[]);

ZithTokenType zith_lookup_keyword(const char *src, size_t len);

char *zith_arena_str(ZithArena *arena, const char *str, size_t len);

// ============================================================================
// Error Handling
// ============================================================================

typedef enum {
    ZITH_OK = 0,
    ZITH_ERR_IO,
    ZITH_ERR_PARSE,
    ZITH_ERR_LEX,
    ZITH_ERR_MEMORY,
    ZITH_ERR_INVALID_INPUT
} ZithError;

// ============================================================================
// C++ Extensions
// ============================================================================

#ifdef __cplusplus
}

#include <memory>
#include <string_view>

namespace ZITH {
    class Arena {
        struct Deleter {
            void operator()(ZithArena *a) const { zith_arena_destroy(a); }
        };

        std::unique_ptr<ZithArena, Deleter> handle_;

    public:
        explicit Arena(size_t initial = 65536)
            : handle_(zith_arena_create(initial)) { if (!handle_) throw std::bad_alloc(); }

        [[nodiscard]] void *alloc(size_t size) const { return zith_arena_alloc(handle_.get(), size); }
        char *strdup(const char *s) const { return zith_arena_strdup(handle_.get(), s); }

        [[nodiscard]] char *strdup(std::string_view sv) const {
            char *p = static_cast<char *>(alloc(sv.size() + 1));
            if (p) {
                memcpy(p, sv.data(), sv.size());
                p[sv.size()] = '\0';
            }
            return p;
        }

        [[nodiscard]] ZithArena *get() const { return handle_.get(); }
    };

    inline ZithTokenStream tokenize(const Arena &arena, std::string_view source) {
        return zith_tokenize(arena.get(), source.data(), source.size());
    }

    inline std::pair<char *, size_t> load_file(const Arena &arena, const char *path) {
        size_t size = 0;
        char *data = zith_load_file_to_arena(arena.get(), path, &size);
        if (!data) throw std::runtime_error(std::string("Failed to load file: ") + std::string(path));
        return {data, size};
    }

    namespace debug {
        const char *token_type_name(ZithTokenType t);

        void print_tokens(ZithTokenStream tokens);

        void print_ast(const ZithNode *node, int indent = 0);
    }
} // namespace ZITH
#endif // __cplusplus
