// impl/lexer/debug.h — Debug utilities for ZithToken
#pragma once
#include <zith/zith.hpp>
#include "../diagnostics/diagnostics.hpp"

// ============================================================================
// Nome de cada token
// ============================================================================

inline const char *zith_token_type_name(ZithTokenType type) {
    switch (type) {
        // -- Literais e identificadores ---------------------------------------
        case ZITH_TOKEN_STRING: return "STRING";
        case ZITH_TOKEN_NUMBER: return "NUMBER";
        case ZITH_TOKEN_HEXADECIMAL: return "HEX";
        case ZITH_TOKEN_OCTAL: return "OCTAL";
        case ZITH_TOKEN_BINARY: return "BINARY";
        case ZITH_TOKEN_FLOAT: return "FLOAT";
        case ZITH_TOKEN_IDENTIFIER: return "IDENTIFIER";

        // -- Aritméticos e lógicos --------------------------------------------
        case ZITH_TOKEN_PLUS: return "PLUS";
        case ZITH_TOKEN_MINUS: return "MINUS";
        case ZITH_TOKEN_MULTIPLY: return "MULTIPLY";
        case ZITH_TOKEN_DIVIDE: return "DIVIDE";
        case ZITH_TOKEN_MOD: return "MOD";
        case ZITH_TOKEN_AND: return "AND";
        case ZITH_TOKEN_OR: return "OR";
        case ZITH_TOKEN_NOT: return "NOT";

        // -- Comparação -------------------------------------------------------
        case ZITH_TOKEN_EQUAL: return "EQUAL";
        case ZITH_TOKEN_NOT_EQUAL: return "NOT_EQUAL";
        case ZITH_TOKEN_LESS_THAN: return "LESS_THAN";
        case ZITH_TOKEN_GREATER_THAN: return "GREATER_THAN";
        case ZITH_TOKEN_LESS_THAN_OR_EQUAL: return "LESS_THAN_OR_EQUAL";
        case ZITH_TOKEN_GREATER_THAN_OR_EQUAL: return "GREATER_THAN_OR_EQUAL";

        // -- Atribuição -------------------------------------------------------
        case ZITH_TOKEN_ASSIGNMENT: return "ASSIGNMENT";
        case ZITH_TOKEN_DECLARATION: return "DECLARATION";
        case ZITH_TOKEN_PLUS_EQUAL: return "PLUS_EQUAL";
        case ZITH_TOKEN_MINUS_EQUAL: return "MINUS_EQUAL";
        case ZITH_TOKEN_MULTIPLY_EQUAL: return "MULTIPLY_EQUAL";
        case ZITH_TOKEN_DIVIDE_EQUAL: return "DIVIDE_EQUAL";

        // -- Especiais --------------------------------------------------------
        case ZITH_TOKEN_QUESTION: return "QUESTION";
        case ZITH_TOKEN_BANG: return "BANG";
        case ZITH_TOKEN_ARROW: return "ARROW";

        // -- Delimitadores ----------------------------------------------------
        case ZITH_TOKEN_LPAREN: return "LPAREN";
        case ZITH_TOKEN_RPAREN: return "RPAREN";
        case ZITH_TOKEN_LBRACE: return "LBRACE";
        case ZITH_TOKEN_RBRACE: return "RBRACE";
        case ZITH_TOKEN_LBRACKET: return "LBRACKET";
        case ZITH_TOKEN_RBRACKET: return "RBRACKET";
        case ZITH_TOKEN_DOT: return "DOT";
        case ZITH_TOKEN_DOTS: return "DOTS";
        case ZITH_TOKEN_COMMA: return "COMMA";
        case ZITH_TOKEN_COLON: return "COLON";
        case ZITH_TOKEN_SEMICOLON: return "SEMICOLON";

        // -- Controle de fluxo ------------------------------------------------
        case ZITH_TOKEN_IF: return "IF";
        case ZITH_TOKEN_ELSE: return "ELSE";
        case ZITH_TOKEN_FOR: return "FOR";
        case ZITH_TOKEN_IN: return "IN";
        case ZITH_TOKEN_WHILE: return "WHILE";
        case ZITH_TOKEN_SWITCH: return "SWITCH";
        case ZITH_TOKEN_RETURN: return "RETURN";
        case ZITH_TOKEN_BREAK: return "BREAK";
        case ZITH_TOKEN_CONTINUE: return "CONTINUE";
        case ZITH_TOKEN_GOTO: return "GOTO";
        case ZITH_TOKEN_MARKER: return "MARKER";
        case ZITH_TOKEN_SCENE: return "SCENE";
        case ZITH_TOKEN_IMPORT: return "IMPORT";
        case ZITH_TOKEN_EXPORT: return "EXPORT";
        case ZITH_TOKEN_FROM: return "FROM";
        case ZITH_TOKEN_AS: return "AS";

        // -- Concorrência -----------------------------------------------------
        case ZITH_TOKEN_SPAWN: return "SPAWN";
        case ZITH_TOKEN_JOINED: return "JOINED";
        case ZITH_TOKEN_AWAIT: return "AWAIT";

        // -- Erros ------------------------------------------------------------
        case ZITH_TOKEN_TRY: return "TRY";
        case ZITH_TOKEN_CATCH: return "CATCH";
        case ZITH_TOKEN_MUST: return "MUST";

        // -- Modificadores de propriedade / escopo ----------------------------
        case ZITH_TOKEN_CONST: return "CONST";
        case ZITH_TOKEN_MUTABLE: return "MUTABLE";
        case ZITH_TOKEN_VAR: return "VAR";
        case ZITH_TOKEN_LET: return "LET";
        case ZITH_TOKEN_AUTO: return "AUTO";
        case ZITH_TOKEN_GLOBAL: return "GLOBAL";
        case ZITH_TOKEN_PERSISTENT: return "PERSISTENT";
        case ZITH_TOKEN_LOCAL: return "LOCAL";
        case ZITH_TOKEN_LEND: return "LEND";
        case ZITH_TOKEN_SHARED: return "SHARED";
        case ZITH_TOKEN_VIEW: return "VIEW";
        case ZITH_TOKEN_UNIQUE: return "UNIQUE";
        case ZITH_TOKEN_PACK: return "PACK";

        // -- Modificadores de acesso ------------------------------------------
        case ZITH_TOKEN_MODIFIER: return "MODIFIER";

        // -- Declarações de tipo ----------------------------------------------
        case ZITH_TOKEN_TYPE: return "TYPE";
        case ZITH_TOKEN_STRUCT: return "STRUCT";
        case ZITH_TOKEN_COMPONENT: return "COMPONENT";
        case ZITH_TOKEN_ENUM: return "ENUM";
        case ZITH_TOKEN_UNION: return "UNION";
        case ZITH_TOKEN_FAMILY: return "FAMILY";
        case ZITH_TOKEN_ENTITY: return "ENTITY";
        case ZITH_TOKEN_TRAIT: return "TRAIT";
        case ZITH_TOKEN_TYPEDEF: return "TYPEDEF";
        case ZITH_TOKEN_IMPLEMENT: return "IMPLEMENT";

        // -- Funções ----------------------------------------------------------
        case ZITH_TOKEN_FN: return "FN";
        case ZITH_TOKEN_ASYNC: return "ASYNC";
        case ZITH_TOKEN_RECURSE: return "RECURSE";
        case ZITH_TOKEN_YIELD: return "YIELD";
        case ZITH_TOKEN_ENTRY: return "ENTRY";
        case ZITH_TOKEN_NORETURN: return "NORETURN";
        case ZITH_TOKEN_FLOWING: return "FLOWING";

        // -- Controle interno -------------------------------------------------
        case ZITH_TOKEN_END: return "END";
        case ZITH_TOKEN_UNKNOWN: return "UNKNOWN";

        default: return "<?>";
    }
}

// Categoria legível para agrupamento visual na tabela.
// Usa casos explícitos para os tokens cujo valor numérico não segue
// a ordem lógica das categorias (END, UNKNOWN, RECURSE, YIELD, ASYNC, FN
// foram adicionados no fim do enum após tokens de controlo).
static const char *token_category(ZithTokenType type) {
    switch (type) {
        case ZITH_TOKEN_END:
        case ZITH_TOKEN_UNKNOWN: return "control";
        case ZITH_TOKEN_FN:
        case ZITH_TOKEN_ASYNC:
        case ZITH_TOKEN_RECURSE:
        case ZITH_TOKEN_YIELD:
        case ZITH_TOKEN_ENTRY:
        case ZITH_TOKEN_NORETURN:
        case ZITH_TOKEN_FLOWING: return "function";
        default: break;
    }
    if (type <= ZITH_TOKEN_IDENTIFIER) return "literal";
    if (type <= ZITH_TOKEN_NOT) return "operator";
    if (type <= ZITH_TOKEN_GREATER_THAN_OR_EQUAL) return "comparison";
    if (type <= ZITH_TOKEN_DIVIDE_EQUAL) return "assignment";
    if (type <= ZITH_TOKEN_ARROW) return "special";
    if (type <= ZITH_TOKEN_SEMICOLON) return "delimiter";
    if (type <= ZITH_TOKEN_SCENE) return "flow";
    if (type <= ZITH_TOKEN_AWAIT) return "concurrency";
    if (type <= ZITH_TOKEN_MUST) return "error";
    if (type <= ZITH_TOKEN_PACK) return "binding";
    if (type == ZITH_TOKEN_MODIFIER) return "access";
    if (type == ZITH_TOKEN_IMPORT || type == ZITH_TOKEN_EXPORT || type == ZITH_TOKEN_FROM) return "module";
    if (type == ZITH_TOKEN_AS) return "module";
    if (type <= ZITH_TOKEN_IMPLEMENT) return "type-decl";
    return "control";
}

// ============================================================================
// Formatação do lexeme para display
//
// Trunca lexemes longos e substitui caracteres de controlo por '?'
// para não corromper o terminal.
// ============================================================================

static void format_lexeme(char *out, size_t out_size,
                          const char *data, size_t len) {
    if (!data || len == 0) {
        out[0] = '-';
        out[1] = '\0';
        return;
    }

    constexpr size_t MAX_DISPLAY = 24;
    const bool truncated = len > MAX_DISPLAY;
    const size_t copy_len = truncated ? MAX_DISPLAY : len;

    size_t pos = 0;
    for (size_t i = 0; i < copy_len && pos < out_size - 4; ++i) {
        const unsigned char c = static_cast<unsigned char>(data[i]);
        out[pos++] = (c < 0x20 || c == 0x7f) ? '?' : static_cast<char>(c);
    }
    if (truncated) {
        out[pos++] = '.';
        out[pos++] = '.';
        out[pos++] = '.';
    }
    out[pos] = '\0';
}

// ============================================================================
// zith_debug_tokens
// ============================================================================

inline void zith_debug_tokens(const ZithToken *tokens, size_t count) {
    if (!tokens) {
        debug_error("[zith_debug_tokens] null token array\n");
        return;
    }

    // ── Header ───────────────────────────────────────────────────────────────
    debug_print(
            "\n"
            " %-5s  %-4s  %-4s  %-12s  %-13s  %s\n"
            " %-5s  %-4s  %-4s  %-12s  %-13s  %s\n",
            "#", "Line", "Col", "Type", "Category", "Lexeme",
            "-----", "----", "----", "------------", "-------------", "------------------------"
    );

    // ── Rows ─────────────────────────────────────────────────────────────────
    for (size_t i = 0; i < count; ++i) {
        const ZithToken &t = tokens[i];

        char lexeme_buf[32];
        format_lexeme(lexeme_buf, sizeof(lexeme_buf), t.lexeme.data, t.lexeme.len);

        debug_print(
                " %-5zu  %-4zu  %-4zu  %-12s  %-13s  %s\n",
                i,
                t.loc.line,
                t.loc.index,
                zith_token_type_name(t.type),
                token_category(t.type),
                lexeme_buf
        );
    }

    // ── Footer ───────────────────────────────────────────────────────────────
    debug_print("\n Total: %zu token%s\n\n", count, count == 1 ? "" : "s");
}