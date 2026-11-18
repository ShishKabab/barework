#ifndef DECLARATION_LEXER_H
#define DECLARATION_LEXER_H
#include "types.h"

typedef enum declaration_token_tag {
    TOKEN_EOF, TOKEN_IDENTIFIER, TOKEN_STRING, TOKEN_NUMBER,
    TOKEN_TRUE, TOKEN_FALSE, TOKEN_NULL, TOKEN_LET, TOKEN_RETURN,
    TOKEN_LEFT_PAREN, TOKEN_RIGHT_PAREN, TOKEN_LEFT_BRACE, TOKEN_RIGHT_BRACE,
    TOKEN_LEFT_BRACKET, TOKEN_RIGHT_BRACKET, TOKEN_COMMA, TOKEN_COLON,
    TOKEN_SEMICOLON, TOKEN_DOT, TOKEN_EQUAL, TOKEN_ARROW,
} declaration_token_tag;
typedef struct declaration_token { declaration_token_tag tag; u32 start, end; } declaration_token;
/* Borrows source; initialized with {source, 0}. No sentinel or allocation. */
typedef struct declaration_lexer { const_byte_slice source; usize cursor; } declaration_lexer;
typedef enum declaration_lex_tag {
    DECLARATION_LEX_OK, DECLARATION_LEX_INVALID_INPUT, DECLARATION_LEX_INVALID_BYTE,
    DECLARATION_LEX_UNTERMINATED_STRING, DECLARATION_LEX_INVALID_ESCAPE,
    DECLARATION_LEX_UNTERMINATED_COMMENT, DECLARATION_LEX_INVALID_NUMBER,
} declaration_lex_tag;
typedef struct declaration_lex_result {
    declaration_lex_tag tag;
    declaration_token token; /* Only valid on OK, including EOF. */
    u32 offset; /* Source location on failure. */
} declaration_lex_result;
declaration_lex_result DeclarationLexNext(declaration_lexer *lexer);
string_slice DeclarationTokenName(declaration_token_tag tag);
#endif
