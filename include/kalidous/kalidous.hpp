// kalidous.h - Unified header for Kalidous programming language core
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
} KalidousSourceLoc;

typedef struct {
    const void *data;
    size_t len;
} KalidousSlice;

typedef struct {
    const char *data;
    size_t len;
} KalidousStr;

// ============================================================================
// Token System
// ============================================================================

typedef enum {
    // ------------------------------------------------------------------------
    // Literais e identificadores
    // ------------------------------------------------------------------------
    KALIDOUS_TOKEN_STRING,
    KALIDOUS_TOKEN_NUMBER,
    KALIDOUS_TOKEN_HEXADECIMAL,
    KALIDOUS_TOKEN_OCTAL,
    KALIDOUS_TOKEN_BINARY,
    KALIDOUS_TOKEN_FLOAT,
    KALIDOUS_TOKEN_IDENTIFIER,

    // ------------------------------------------------------------------------
    // Operadores aritméticos e lógicos
    // ------------------------------------------------------------------------
    KALIDOUS_TOKEN_PLUS,
    KALIDOUS_TOKEN_MINUS,
    KALIDOUS_TOKEN_MULTIPLY,
    KALIDOUS_TOKEN_DIVIDE,
    KALIDOUS_TOKEN_MOD,

    KALIDOUS_TOKEN_AND,
    KALIDOUS_TOKEN_OR,
    KALIDOUS_TOKEN_NOT,

    // ------------------------------------------------------------------------
    // Operadores de comparação
    // ------------------------------------------------------------------------
    KALIDOUS_TOKEN_EQUAL,
    KALIDOUS_TOKEN_NOT_EQUAL,
    KALIDOUS_TOKEN_LESS_THAN,
    KALIDOUS_TOKEN_GREATER_THAN,
    KALIDOUS_TOKEN_LESS_THAN_OR_EQUAL,
    KALIDOUS_TOKEN_GREATER_THAN_OR_EQUAL,

    // ------------------------------------------------------------------------
    // Operadores de atribuição
    // ------------------------------------------------------------------------
    KALIDOUS_TOKEN_ASSIGNMENT,
    KALIDOUS_TOKEN_DECLARATION, // :=
    KALIDOUS_TOKEN_PLUS_EQUAL,
    KALIDOUS_TOKEN_MINUS_EQUAL,
    KALIDOUS_TOKEN_MULTIPLY_EQUAL,
    KALIDOUS_TOKEN_DIVIDE_EQUAL,

    // ------------------------------------------------------------------------
    // Operadores especiais
    // ------------------------------------------------------------------------
    KALIDOUS_TOKEN_QUESTION, // ?  optional
    KALIDOUS_TOKEN_BANG, // !  type may fail
    KALIDOUS_TOKEN_ARROW, // -> encadeamento de funções

    // ------------------------------------------------------------------------
    // Delimitadores
    // ------------------------------------------------------------------------
    KALIDOUS_TOKEN_LPAREN,
    KALIDOUS_TOKEN_RPAREN,
    KALIDOUS_TOKEN_LBRACE,
    KALIDOUS_TOKEN_RBRACE,
    KALIDOUS_TOKEN_LBRACKET,
    KALIDOUS_TOKEN_RBRACKET,
    KALIDOUS_TOKEN_DOT,
    KALIDOUS_TOKEN_DOTS, // ...
    KALIDOUS_TOKEN_COMMA,
    KALIDOUS_TOKEN_COLON,
    KALIDOUS_TOKEN_SEMICOLON,

    // ------------------------------------------------------------------------
    // Keywords: controle de fluxo
    // ------------------------------------------------------------------------
    KALIDOUS_TOKEN_IF,
    KALIDOUS_TOKEN_ELSE,
    KALIDOUS_TOKEN_FOR,
    KALIDOUS_TOKEN_IN,
    KALIDOUS_TOKEN_WHILE, // reservado na ABI, não usado como keyword ativa
    KALIDOUS_TOKEN_SWITCH,
    KALIDOUS_TOKEN_RETURN,
    KALIDOUS_TOKEN_BREAK,
    KALIDOUS_TOKEN_CONTINUE,
    KALIDOUS_TOKEN_GOTO,
    KALIDOUS_TOKEN_MARKER,
    KALIDOUS_TOKEN_SCENE,

    // ------------------------------------------------------------------------
    // Keywords: concorrência / fluxo assíncrono
    // ------------------------------------------------------------------------
    KALIDOUS_TOKEN_SPAWN,
    KALIDOUS_TOKEN_JOINED,
    KALIDOUS_TOKEN_AWAIT,

    // ------------------------------------------------------------------------
    // Keywords: tratamento de erros
    // ------------------------------------------------------------------------
    KALIDOUS_TOKEN_TRY,
    KALIDOUS_TOKEN_CATCH,
    KALIDOUS_TOKEN_MUST, // "must!" — o ! é semântico, o Parser resolve

    // ------------------------------------------------------------------------
    // Modificadores de propriedade e escopo
    // ------------------------------------------------------------------------
    KALIDOUS_TOKEN_CONST,
    KALIDOUS_TOKEN_MUTABLE, // keyword: 'mut'
    KALIDOUS_TOKEN_VAR,
    KALIDOUS_TOKEN_LET,
    KALIDOUS_TOKEN_AUTO,

    KALIDOUS_TOKEN_GLOBAL,
    KALIDOUS_TOKEN_PERSISTENT,
    KALIDOUS_TOKEN_LOCAL,
    KALIDOUS_TOKEN_LEND,
    KALIDOUS_TOKEN_SHARED,
    KALIDOUS_TOKEN_VIEW,
    KALIDOUS_TOKEN_UNIQUE,
    KALIDOUS_TOKEN_PACK, // reservado na ABI; [] é resolvido pelo Parser

    // ------------------------------------------------------------------------
    // Modificadores de acesso
    // ------------------------------------------------------------------------
    KALIDOUS_TOKEN_MODIFIER, // public / private / protected

    // ------------------------------------------------------------------------
    // Declarações de tipo
    // ------------------------------------------------------------------------
    KALIDOUS_TOKEN_TYPE,
    KALIDOUS_TOKEN_STRUCT,
    KALIDOUS_TOKEN_COMPONENT,
    KALIDOUS_TOKEN_ENUM,
    KALIDOUS_TOKEN_UNION,
    KALIDOUS_TOKEN_RAW,
    KALIDOUS_TOKEN_FAMILY,
    KALIDOUS_TOKEN_ENTITY,
    KALIDOUS_TOKEN_TRAIT,
    KALIDOUS_TOKEN_TYPEDEF,
    KALIDOUS_TOKEN_IMPLEMENT,
    KALIDOUS_TOKEN_NULL,

    // ------------------------------------------------------------------------
    // Tokens especiais / controle
    // ------------------------------------------------------------------------
    KALIDOUS_TOKEN_END,
    KALIDOUS_TOKEN_UNKNOWN, KALIDOUS_TOKEN_RECURSE,
    KALIDOUS_TOKEN_YIELD, KALIDOUS_TOKEN_ASYNC, KALIDOUS_TOKEN_FN,
    KALIDOUS_TOKEN_FLOWING, KALIDOUS_TOKEN_ENTRY, KALIDOUS_TOKEN_NORETURN, KALIDOUS_TOKEN_IMPORT,
    KALIDOUS_TOKEN_USE, KALIDOUS_TOKEN_CONTEXT, KALIDOUS_TOKEN_MACRO
} KalidousTokenType;

typedef struct {
    KalidousStr lexeme;
    KalidousSourceLoc loc;
    KalidousTokenType type;
    uint16_t keyword_id;
} KalidousToken;

typedef struct {
    const KalidousToken *data;
    size_t len;
} KalidousTokenStream;

typedef struct KalidousArena KalidousArena;

KalidousTokenStream kalidous_tokenize(KalidousArena *arena, const char *source, size_t source_len);

void kalidous_debug_tokenize(KalidousArena *arena, const char *source, size_t source_len);

// ============================================================================
// AST System
// ============================================================================

typedef uint16_t KalidousNodeId;

enum {
    KALIDOUS_NODE_ERROR = 0,

    KALIDOUS_NODE_LITERAL = 100,
    KALIDOUS_NODE_IDENTIFIER = 101,
    KALIDOUS_NODE_BINARY_OP = 102,
    KALIDOUS_NODE_UNARY_OP = 103,
    KALIDOUS_NODE_CALL = 104,
    KALIDOUS_NODE_INDEX = 105,
    KALIDOUS_NODE_MEMBER = 106,

    KALIDOUS_NODE_VAR_DECL = 200,
    KALIDOUS_NODE_FUNC_DECL = 201,
    KALIDOUS_NODE_PARAM = 202,

    KALIDOUS_NODE_BLOCK = 300,
    KALIDOUS_NODE_IF = 301,
    KALIDOUS_NODE_FOR = 302, // unifica for e while
    KALIDOUS_NODE_RETURN = 303,
    KALIDOUS_NODE_EXPR_STMT = 304,
    KALIDOUS_NODE_UNBODY = 305, // corpo não parseado (entre { e }) - scanner mode

    KALIDOUS_NODE_TYPE_REF = 400,
    KALIDOUS_NODE_TYPE_FUNC = 401,

    KALIDOUS_NODE_CUSTOM_START = 1000
};

typedef struct KalidousNode KalidousNode;

struct KalidousNode {
    union {
        struct {
            KalidousNode *a;
            KalidousNode *b;
            KalidousNode *c;
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
    KalidousSourceLoc loc;
    KalidousNodeId type;



};

KalidousNode *kalidous_parse(KalidousArena *arena, KalidousTokenStream tokens);

KalidousNode *kalidous_parse_with_source(KalidousArena *arena, const char *source,
                                         size_t source_len, const char *filename,
                                         KalidousTokenStream tokens);


static inline KalidousNodeId kalidous_node_type(const KalidousNode *node) {
    return node ? node->type : (KalidousNodeId) KALIDOUS_NODE_ERROR;
}

// ============================================================================
// Memory Arena
// ============================================================================

KalidousArena *kalidous_arena_create(size_t initial_block_size);

void *kalidous_arena_alloc(KalidousArena *arena, size_t size);

char *kalidous_arena_strdup(KalidousArena *arena, const char *str);

void kalidous_arena_reset(KalidousArena *arena);

void kalidous_arena_destroy(KalidousArena *arena);

// ============================================================================
// File Utilities
// ============================================================================

bool kalidous_file_exists(const char *path);

bool kalidous_file_is_regular(const char *path);

size_t kalidous_file_size(const char *path);

bool kalidous_file_has_extension(const char *path, const char *ext);

char *kalidous_load_file_to_arena(KalidousArena *arena, const char *path, size_t *out_size);

int kalidous_run(int argc, const char *const argv[]);

KalidousTokenType kalidous_lookup_keyword(const char *src, size_t len);

char *kalidous_arena_str(KalidousArena *arena, const char *str, size_t len);

// ============================================================================
// Error Handling
// ============================================================================

typedef enum {
    KALIDOUS_OK = 0,
    KALIDOUS_ERR_IO,
    KALIDOUS_ERR_PARSE,
    KALIDOUS_ERR_LEX,
    KALIDOUS_ERR_MEMORY,
    KALIDOUS_ERR_INVALID_INPUT
} KalidousError;

// ============================================================================
// C++ Extensions
// ============================================================================

#ifdef __cplusplus
}

#include <memory>
#include <string_view>

namespace KALIDOUS {
    class Arena {
        struct Deleter {
            void operator()(KalidousArena *a) const { kalidous_arena_destroy(a); }
        };

        std::unique_ptr<KalidousArena, Deleter> handle_;

    public:
        explicit Arena(size_t initial = 65536)
            : handle_(kalidous_arena_create(initial)) { if (!handle_) throw std::bad_alloc(); }

        [[nodiscard]] void *alloc(size_t size) const { return kalidous_arena_alloc(handle_.get(), size); }
        char *strdup(const char *s) const { return kalidous_arena_strdup(handle_.get(), s); }

        [[nodiscard]] char *strdup(std::string_view sv) const {
            char *p = static_cast<char *>(alloc(sv.size() + 1));
            if (p) {
                memcpy(p, sv.data(), sv.size());
                p[sv.size()] = '\0';
            }
            return p;
        }

        [[nodiscard]] KalidousArena *get() const { return handle_.get(); }
    };

    inline KalidousTokenStream tokenize(const Arena &arena, std::string_view source) {
        return kalidous_tokenize(arena.get(), source.data(), source.size());
    }

    inline std::pair<char *, size_t> load_file(const Arena &arena, const char *path) {
        size_t size = 0;
        char *data = kalidous_load_file_to_arena(arena.get(), path, &size);
        if (!data) throw std::runtime_error(std::string("Failed to load file: ") + std::string(path));
        return {data, size};
    }

    namespace debug {
        const char *token_type_name(KalidousTokenType t);

        void print_tokens(KalidousTokenStream tokens);

        void print_ast(const KalidousNode *node, int indent = 0);
    }
} // namespace KALIDOUS
#endif // __cplusplus