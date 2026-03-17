// impl/parser/token_debug.cpp — Debug utilities for KalidousToken
#pragma once
#include "Kalidous/kalidous.h"
#include <cstdio>
#include <cstring>

// ============================================================================
// Nome de cada token
// ============================================================================

inline const char* kalidous_token_type_name(KalidousTokenType type) {
    switch (type) {
        // -- Literais e identificadores ---------------------------------------
        case KALIDOUS_TOKEN_STRING:                 return "STRING";
        case KALIDOUS_TOKEN_NUMBER:                 return "NUMBER";
        case KALIDOUS_TOKEN_HEXADECIMAL:            return "HEX";
        case KALIDOUS_TOKEN_OCTAL:                  return "OCTAL";
        case KALIDOUS_TOKEN_BINARY:                 return "BINARY";
        case KALIDOUS_TOKEN_FLOAT:                  return "FLOAT";
        case KALIDOUS_TOKEN_IDENTIFIER:             return "IDENTIFIER";

        // -- Aritméticos e lógicos --------------------------------------------
        case KALIDOUS_TOKEN_PLUS:                   return "PLUS";
        case KALIDOUS_TOKEN_MINUS:                  return "MINUS";
        case KALIDOUS_TOKEN_MULTIPLY:               return "MULTIPLY";
        case KALIDOUS_TOKEN_DIVIDE:                 return "DIVIDE";
        case KALIDOUS_TOKEN_MOD:                    return "MOD";
        case KALIDOUS_TOKEN_AND:                    return "AND";
        case KALIDOUS_TOKEN_OR:                     return "OR";
        case KALIDOUS_TOKEN_NOT:                    return "NOT";

        // -- Comparação -------------------------------------------------------
        case KALIDOUS_TOKEN_EQUAL:                  return "EQUAL";
        case KALIDOUS_TOKEN_NOT_EQUAL:              return "NOT_EQUAL";
        case KALIDOUS_TOKEN_LESS_THAN:              return "LESS_THAN";
        case KALIDOUS_TOKEN_GREATER_THAN:           return "GREATER_THAN";
        case KALIDOUS_TOKEN_LESS_THAN_OR_EQUAL:     return "LESS_THAN_OR_EQUAL";
        case KALIDOUS_TOKEN_GREATER_THAN_OR_EQUAL:  return "GREATER_THAN_OR_EQUAL";

        // -- Atribuição -------------------------------------------------------
        case KALIDOUS_TOKEN_ASSIGNMENT:             return "ASSIGNMENT";
        case KALIDOUS_TOKEN_DECLARATION:            return "DECLARATION";
        case KALIDOUS_TOKEN_PLUS_EQUAL:             return "PLUS_EQUAL";
        case KALIDOUS_TOKEN_MINUS_EQUAL:            return "MINUS_EQUAL";
        case KALIDOUS_TOKEN_MULTIPLY_EQUAL:         return "MULTIPLY_EQUAL";
        case KALIDOUS_TOKEN_DIVIDE_EQUAL:           return "DIVIDE_EQUAL";

        // -- Especiais --------------------------------------------------------
        case KALIDOUS_TOKEN_QUESTION:               return "QUESTION";
        case KALIDOUS_TOKEN_BANG:                   return "BANG";
        case KALIDOUS_TOKEN_ARROW:                  return "ARROW";

        // -- Delimitadores ----------------------------------------------------
        case KALIDOUS_TOKEN_LPAREN:                 return "LPAREN";
        case KALIDOUS_TOKEN_RPAREN:                 return "RPAREN";
        case KALIDOUS_TOKEN_LBRACE:                 return "LBRACE";
        case KALIDOUS_TOKEN_RBRACE:                 return "RBRACE";
        case KALIDOUS_TOKEN_LBRACKET:               return "LBRACKET";
        case KALIDOUS_TOKEN_RBRACKET:               return "RBRACKET";
        case KALIDOUS_TOKEN_DOT:                    return "DOT";
        case KALIDOUS_TOKEN_DOTS:                   return "DOTS";
        case KALIDOUS_TOKEN_COMMA:                  return "COMMA";
        case KALIDOUS_TOKEN_COLON:                  return "COLON";
        case KALIDOUS_TOKEN_SEMICOLON:              return "SEMICOLON";

        // -- Controle de fluxo ------------------------------------------------
        case KALIDOUS_TOKEN_IF:                     return "IF";
        case KALIDOUS_TOKEN_ELSE:                   return "ELSE";
        case KALIDOUS_TOKEN_FOR:                    return "FOR";
        case KALIDOUS_TOKEN_IN:                     return "IN";
        case KALIDOUS_TOKEN_WHILE:                  return "WHILE";
        case KALIDOUS_TOKEN_SWITCH:                 return "SWITCH";
        case KALIDOUS_TOKEN_RETURN:                 return "RETURN";
        case KALIDOUS_TOKEN_BREAK:                  return "BREAK";
        case KALIDOUS_TOKEN_CONTINUE:               return "CONTINUE";
        case KALIDOUS_TOKEN_GOTO:                   return "GOTO";
        case KALIDOUS_TOKEN_MARKER:                 return "MARKER";
        case KALIDOUS_TOKEN_SCENE:                  return "SCENE";

        // -- Concorrência -----------------------------------------------------
        case KALIDOUS_TOKEN_SPAWN:                  return "SPAWN";
        case KALIDOUS_TOKEN_JOINED:                 return "JOINED";
        case KALIDOUS_TOKEN_AWAIT:                  return "AWAIT";

        // -- Erros ------------------------------------------------------------
        case KALIDOUS_TOKEN_TRY:                    return "TRY";
        case KALIDOUS_TOKEN_CATCH:                  return "CATCH";
        case KALIDOUS_TOKEN_MUST:                   return "MUST";

        // -- Modificadores de propriedade / escopo ----------------------------
        case KALIDOUS_TOKEN_CONST:                  return "CONST";
        case KALIDOUS_TOKEN_MUTABLE:                return "MUTABLE";
        case KALIDOUS_TOKEN_VAR:                    return "VAR";
        case KALIDOUS_TOKEN_LET:                    return "LET";
        case KALIDOUS_TOKEN_AUTO:                   return "AUTO";
        case KALIDOUS_TOKEN_GLOBAL:                 return "GLOBAL";
        case KALIDOUS_TOKEN_PERSISTENT:             return "PERSISTENT";
        case KALIDOUS_TOKEN_LOCAL:                  return "LOCAL";
        case KALIDOUS_TOKEN_LEND:                   return "LEND";
        case KALIDOUS_TOKEN_SHARED:                 return "SHARED";
        case KALIDOUS_TOKEN_VIEW:                   return "VIEW";
        case KALIDOUS_TOKEN_UNIQUE:                 return "UNIQUE";
        case KALIDOUS_TOKEN_PACK:                   return "PACK";

        // -- Modificadores de acesso ------------------------------------------
        case KALIDOUS_TOKEN_MODIFIER:               return "MODIFIER";

        // -- Declarações de tipo ----------------------------------------------
        case KALIDOUS_TOKEN_TYPE:                   return "TYPE";
        case KALIDOUS_TOKEN_STRUCT:                 return "STRUCT";
        case KALIDOUS_TOKEN_COMPONENT:              return "COMPONENT";
        case KALIDOUS_TOKEN_ENUM:                   return "ENUM";
        case KALIDOUS_TOKEN_UNION:                  return "UNION";
        case KALIDOUS_TOKEN_FAMILY:                 return "FAMILY";
        case KALIDOUS_TOKEN_ENTITY:                 return "ENTITY";
        case KALIDOUS_TOKEN_TRAIT:                  return "TRAIT";
        case KALIDOUS_TOKEN_TYPEDEF:                return "TYPEDEF";
        case KALIDOUS_TOKEN_IMPLEMENT:              return "IMPLEMENT";

        // -- Funções ----------------------------------------------------------
        case KALIDOUS_TOKEN_FN:                     return "FN";
        case KALIDOUS_TOKEN_ASYNC:                  return "ASYNC";
        case KALIDOUS_TOKEN_RECURSE:                return "RECURSE";
        case KALIDOUS_TOKEN_YIELD:                  return "YIELD";
        case KALIDOUS_TOKEN_ENTRY:                  return "ENTRY";
        case KALIDOUS_TOKEN_NORETURN:               return "NORETURN";
        case KALIDOUS_TOKEN_FLOWING:                return "FLOWING";

        // -- Controle interno -------------------------------------------------
        case KALIDOUS_TOKEN_END:                    return "END";
        case KALIDOUS_TOKEN_UNKNOWN:                return "UNKNOWN";

        default:                                    return "<?>";
    }
}

// Categoria legível para agrupamento visual na tabela.
// Usa casos explícitos para os tokens cujo valor numérico não segue
// a ordem lógica das categorias (END, UNKNOWN, RECURSE, YIELD, ASYNC, FN
// foram adicionados no fim do enum após tokens de controlo).
static const char* token_category(KalidousTokenType type) {
    switch (type) {
        case KALIDOUS_TOKEN_END:
        case KALIDOUS_TOKEN_UNKNOWN:   return "control";
        case KALIDOUS_TOKEN_FN:
        case KALIDOUS_TOKEN_ASYNC:
        case KALIDOUS_TOKEN_RECURSE:
        case KALIDOUS_TOKEN_YIELD:
        case KALIDOUS_TOKEN_ENTRY:
        case KALIDOUS_TOKEN_NORETURN:
        case KALIDOUS_TOKEN_FLOWING:  return "function";
        default: break;
    }
    if (type <= KALIDOUS_TOKEN_IDENTIFIER)             return "literal";
    if (type <= KALIDOUS_TOKEN_NOT)                    return "operator";
    if (type <= KALIDOUS_TOKEN_GREATER_THAN_OR_EQUAL)  return "comparison";
    if (type <= KALIDOUS_TOKEN_DIVIDE_EQUAL)           return "assignment";
    if (type <= KALIDOUS_TOKEN_ARROW)                  return "special";
    if (type <= KALIDOUS_TOKEN_SEMICOLON)              return "delimiter";
    if (type <= KALIDOUS_TOKEN_SCENE)                  return "flow";
    if (type <= KALIDOUS_TOKEN_AWAIT)                  return "concurrency";
    if (type <= KALIDOUS_TOKEN_MUST)                   return "error";
    if (type <= KALIDOUS_TOKEN_PACK)                   return "binding";
    if (type == KALIDOUS_TOKEN_MODIFIER)               return "access";
    if (type <= KALIDOUS_TOKEN_IMPLEMENT)              return "type-decl";
    return "control";
}

// ============================================================================
// Formatação do lexeme para display
//
// Trunca lexemes longos e substitui caracteres de controlo por '?'
// para não corromper o terminal.
// ============================================================================

static void format_lexeme(char* out, size_t out_size,
                          const char* data, size_t len) {
    if (!data || len == 0) {
        out[0] = '-'; out[1] = '\0';
        return;
    }

    constexpr size_t MAX_DISPLAY = 24;
    const bool       truncated   = len > MAX_DISPLAY;
    const size_t     copy_len    = truncated ? MAX_DISPLAY : len;

    size_t pos = 0;
    for (size_t i = 0; i < copy_len && pos < out_size - 4; ++i) {
        const unsigned char c = static_cast<unsigned char>(data[i]);
        out[pos++] = (c < 0x20 || c == 0x7f) ? '?' : static_cast<char>(c);
    }
    if (truncated) {
        out[pos++] = '.'; out[pos++] = '.'; out[pos++] = '.';
    }
    out[pos] = '\0';
}

// ============================================================================
// kalidous_debug_tokens
// ============================================================================

inline void kalidous_debug_tokens(const KalidousToken* tokens, size_t count) {
    if (!tokens) {
        fprintf(stderr, "[kalidous_debug_tokens] null token array\n");
        return;
    }

    // ── Header ───────────────────────────────────────────────────────────────
    fprintf(stderr,
        "\n"
        " %-5s  %-4s  %-4s  %-12s  %-13s  %s\n"
        " %-5s  %-4s  %-4s  %-12s  %-13s  %s\n",
        "#", "Line", "Col", "Type", "Category", "Lexeme",
        "-----", "----", "----", "------------", "-------------", "------------------------"
    );

    // ── Rows ─────────────────────────────────────────────────────────────────
    for (size_t i = 0; i < count; ++i) {
        const KalidousToken& t = tokens[i];

        char lexeme_buf[32];
        format_lexeme(lexeme_buf, sizeof(lexeme_buf), t.lexeme.data, t.lexeme.len);

        fprintf(stderr,
            " %-5zu  %-4zu  %-4zu  %-12s  %-13s  %s\n",
            i,
            t.loc.line,
            t.loc.index,
            kalidous_token_type_name(t.type),
            token_category(t.type),
            lexeme_buf
        );
    }

    // ── Footer ───────────────────────────────────────────────────────────────
    fprintf(stderr, "\n Total: %zu token%s\n\n", count, count == 1 ? "" : "s");
}