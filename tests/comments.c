#include "declaration/lexer.h"
#include <assert.h>
#include <stdio.h>

static declaration_lexer Lexer(string_slice text)
{
    return (declaration_lexer){.source = {(const u8 *)text.ptr, text.len}};
}
static declaration_token Next(declaration_lexer *lexer, declaration_token_tag expected)
{
    declaration_lex_result result = DeclarationLexNext(lexer);
    assert(result.tag == DECLARATION_LEX_OK && result.token.tag == expected);
    return result.token;
}
int main(void)
{
    declaration_lexer lexer = Lexer(StringSlice("# first\r\nversion# inline\n(1);# final"));
    declaration_token token = Next(&lexer, TOKEN_IDENTIFIER);
    assert(token.start == 9 && token.end == 16);
    Next(&lexer, TOKEN_LEFT_PAREN);
    Next(&lexer, TOKEN_NUMBER);
    Next(&lexer, TOKEN_RIGHT_PAREN);
    Next(&lexer, TOKEN_SEMICOLON);
    Next(&lexer, TOKEN_EOF);
    lexer = Lexer(StringSlice("#"));
    Next(&lexer, TOKEN_EOF);
    lexer = Lexer(StringSlice("# comment\rtrue"));
    Next(&lexer, TOKEN_TRUE);
    Next(&lexer, TOKEN_EOF);
    lexer = Lexer(StringSlice("'https://example.test/#fragment' \"#text\""));
    Next(&lexer, TOKEN_STRING);
    Next(&lexer, TOKEN_STRING);
    Next(&lexer, TOKEN_EOF);
    lexer = Lexer(StringSlice("/* retained block comment */ # line\nfalse"));
    Next(&lexer, TOKEN_FALSE);
    lexer = Lexer(StringSlice("// old line comment"));
    declaration_lex_result rejected = DeclarationLexNext(&lexer);
    assert(rejected.tag == DECLARATION_LEX_INVALID_BYTE && rejected.offset == 0);
    puts("PASS: hash comments, line endings, source offsets, literals, block comments, rejected slash comments");
}
