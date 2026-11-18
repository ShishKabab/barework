#include "lexer.h"
#include "slice.h"

static bool IdentifierStart(u8 c)
{ return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$'; }
static bool Digit(u8 c) { return c >= '0' && c <= '9'; }
static bool Hex(u8 c) { return Digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static bool TextEquals(const_byte_slice source, usize start, usize end, string_slice text)
{
    const_byte_slice word;
    if (end < start || ConstByteSliceSubslice(source, start, end - start, &word).tag != CONST_BYTE_SLICE_SUBSLICE_OK || word.len != text.len) return false;
    SliceFor(word, i) { if (word.ptr[i] != (u8)text.ptr[i]) return false; }
    return true;
}
declaration_lex_result DeclarationLexNext(declaration_lexer *lexer)
{
    if (!lexer || (!lexer->source.ptr && lexer->source.len) || lexer->source.len > UINT32_MAX || lexer->cursor > lexer->source.len)
        return (declaration_lex_result){.tag = DECLARATION_LEX_INVALID_INPUT};
    const_byte_slice source = lexer->source;
    usize p = lexer->cursor;
    for (;;) {
        while (p < source.len && (source.ptr[p] == ' ' || source.ptr[p] == '\t' || source.ptr[p] == '\r' || source.ptr[p] == '\n')) ++p;
        if (source.len - p < 2 || source.ptr[p] != '/') break;
        if (source.ptr[p + 1] == '/') {
            p += 2;
            while (p < source.len && source.ptr[p] != '\n') ++p;
        } else if (source.ptr[p + 1] == '*') {
            usize start = p;
            p += 2;
            while (source.len - p >= 2 && !(source.ptr[p] == '*' && source.ptr[p + 1] == '/')) ++p;
            if (source.len - p < 2) return (declaration_lex_result){.tag = DECLARATION_LEX_UNTERMINATED_COMMENT, .offset = (u32)start};
            p += 2;
        } else break;
    }
    usize start = p;
    declaration_token_tag tag = TOKEN_EOF;
    if (p < source.len) {
        u8 c = source.ptr[p++];
        if (IdentifierStart(c)) {
            while (p < source.len && (IdentifierStart(source.ptr[p]) || Digit(source.ptr[p]))) ++p;
            tag = TOKEN_IDENTIFIER;
#define Keyword(text, value) if (TextEquals(source, start, p, StringSlice(text))) tag = value
            Keyword("true", TOKEN_TRUE);
            Keyword("false", TOKEN_FALSE);
            Keyword("null", TOKEN_NULL);
            Keyword("let", TOKEN_LET);
            Keyword("return", TOKEN_RETURN);
#undef Keyword
        } else if (Digit(c)) {
            tag = TOKEN_NUMBER;
            while (p < source.len && Digit(source.ptr[p])) ++p;
            if (p < source.len && source.ptr[p] == '.') {
                ++p;
                if (p == source.len || !Digit(source.ptr[p])) return (declaration_lex_result){.tag = DECLARATION_LEX_INVALID_NUMBER, .offset = (u32)p};
                while (p < source.len && Digit(source.ptr[p])) ++p;
            }
            if (p < source.len && IdentifierStart(source.ptr[p])) return (declaration_lex_result){.tag = DECLARATION_LEX_INVALID_NUMBER, .offset = (u32)p};
        } else if (c == '\'' || c == '"') {
            tag = TOKEN_STRING;
            bool closed = false;
            while (p < source.len) {
                u8 next = source.ptr[p++];
                if (next == c) { closed = true; break; }
                if (next < 32) return (declaration_lex_result){.tag = DECLARATION_LEX_UNTERMINATED_STRING, .offset = (u32)start};
                if (next == '\\') {
                    if (p == source.len) break;
                    u8 escape = source.ptr[p++];
                    if (escape == 'u') {
                        for (usize i = 0; i < 4; ++i) {
                            if (p == source.len || !Hex(source.ptr[p])) return (declaration_lex_result){.tag = DECLARATION_LEX_INVALID_ESCAPE, .offset = (u32)p};
                            ++p;
                        }
                    } else if (escape != '\\' && escape != '\'' && escape != '"' && escape != 'n' && escape != 'r' && escape != 't' && escape != 'b' && escape != 'f' && escape != '/')
                        return (declaration_lex_result){.tag = DECLARATION_LEX_INVALID_ESCAPE, .offset = (u32)(p - 1)};
                }
            }
            if (!closed) return (declaration_lex_result){.tag = DECLARATION_LEX_UNTERMINATED_STRING, .offset = (u32)start};
        } else switch (c) {
        case '(': tag = TOKEN_LEFT_PAREN; break; case ')': tag = TOKEN_RIGHT_PAREN; break;
        case '{': tag = TOKEN_LEFT_BRACE; break; case '}': tag = TOKEN_RIGHT_BRACE; break;
        case '[': tag = TOKEN_LEFT_BRACKET; break; case ']': tag = TOKEN_RIGHT_BRACKET; break;
        case ',': tag = TOKEN_COMMA; break; case ':': tag = TOKEN_COLON; break;
        case ';': tag = TOKEN_SEMICOLON; break; case '.': tag = TOKEN_DOT; break;
        case '=': tag = TOKEN_EQUAL; if (p < source.len && source.ptr[p] == '>') { ++p; tag = TOKEN_ARROW; } break;
        default: return (declaration_lex_result){.tag = DECLARATION_LEX_INVALID_BYTE, .offset = (u32)start};
        }
    }
    lexer->cursor = p;
    return (declaration_lex_result){.tag = DECLARATION_LEX_OK, .token = {tag, (u32)start, (u32)p}};
}
string_slice DeclarationTokenName(declaration_token_tag tag)
{
    const string_slice names[] = {
        StringSlice("end of file"), StringSlice("identifier"), StringSlice("string"), StringSlice("number"),
        StringSlice("true"), StringSlice("false"), StringSlice("null"), StringSlice("let"), StringSlice("return"),
        StringSlice("("), StringSlice(")"), StringSlice("{"), StringSlice("}"), StringSlice("["), StringSlice("]"),
        StringSlice(","), StringSlice(":"), StringSlice(";"), StringSlice("."), StringSlice("="), StringSlice("=>"),
    };
    if ((usize)tag >= sizeof(names) / sizeof(names[0])) return StringSlice("unknown token");
    return names[tag];
}
