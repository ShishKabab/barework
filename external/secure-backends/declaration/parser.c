#include "parser.h"
#include "slice.h"
#include <stdlib.h>

typedef struct parser {
    declaration_ast *ast;
    declaration_u32_slice scratch;
    usize scratch_used, nodes_used, extra_used;
    u32 token;
    u32 error_offset;
    string_slice error;
} parser;
typedef enum parse_kind {
    PARSE_FILE, PARSE_STATEMENT, PARSE_EXPRESSION, PARSE_BLOCK, PARSE_OBJECT,
    PARSE_PROPERTY, PARSE_ARRAY, PARSE_ARGUMENTS, PARSE_PARAMETERS, PARSE_NAME,
} parse_kind;
typedef enum parse_node_tag { PARSE_NODE_OK, PARSE_NODE_FAILED } parse_node_tag;
typedef struct parse_node_result { parse_node_tag tag; u32 node; } parse_node_result;
typedef enum add_node_tag { ADD_NODE_OK, ADD_NODE_FULL } add_node_tag;
typedef struct add_node_result { add_node_tag tag; u32 node; } add_node_result;

static declaration_token_tag Current(parser *p) { return (declaration_token_tag)p->ast->token_tags.ptr[p->token]; }
static void Diagnose(parser *p, string_slice message)
{
    p->error = message;
    p->error_offset = p->ast->token_starts.ptr[p->token];
}
static bool At(parser *p, declaration_token_tag tag) { return Current(p) == tag; }
static add_node_result AddNode(parser *p, declaration_node_tag tag, u32 token, usize mark)
{
    usize count = p->scratch_used - mark;
    if (p->nodes_used >= p->ast->node_tags.len || count > p->ast->extra.len - p->extra_used)
        return (add_node_result){.tag = ADD_NODE_FULL};
    u32 node = (u32)p->nodes_used++;
    p->ast->node_tags.ptr[node] = (u8)tag;
    p->ast->main_tokens.ptr[node] = token;
    p->ast->node_data.ptr[node] = (declaration_node_data){(u32)p->extra_used, (u32)count};
    /* Both ranges are checked above; copying typed indices, not raw bytes. */
    declaration_u32_slice children = {count ? p->scratch.ptr + mark : NULL, count};
    SliceFor(children, i) { p->ast->extra.ptr[p->extra_used++] = children.ptr[i]; }
    p->scratch_used = mark;
    return (add_node_result){.tag = ADD_NODE_OK, .node = node};
}
static bool TokenEquals(parser *p, u32 token, string_slice text)
{
    const_byte_slice word;
    u32 start = p->ast->token_starts.ptr[token], end = p->ast->token_ends.ptr[token];
    if (ConstByteSliceSubslice(p->ast->source, start, end - start, &word).tag != CONST_BYTE_SLICE_SUBSLICE_OK || word.len != text.len) return false;
    SliceFor(word, i) { if (word.ptr[i] != (u8)text.ptr[i]) return false; }
    return true;
}
/* The recursive routine has one function-specific result. Diagnostics are
 * retained separately so callers propagate failure without losing its location.
 * Lists use a scratch stack: nested lists finish before their parent's indices
 * are appended contiguously to extra. */
static parse_node_result ParseNode(parser *p, parse_kind kind, usize depth)
{
#define Require(condition, message) do { if (!(condition)) { Diagnose(p, StringSlice(message)); return (parse_node_result){.tag = PARSE_NODE_FAILED}; } } while (0)
#define Consume(tag, message) do { Require(At(p, tag), message); ++p->token; } while (0)
#define Child(child_kind) do { \
    parse_node_result child = ParseNode(p, child_kind, depth + 1); \
    if (child.tag != PARSE_NODE_OK) return (parse_node_result){.tag = PARSE_NODE_FAILED}; \
    Require(p->scratch_used < p->scratch.len, "AST scratch capacity exceeded"); \
    p->scratch.ptr[p->scratch_used++] = child.node; \
} while (0)
#define Finish(node_kind, main) do { \
    add_node_result added = AddNode(p, node_kind, main, mark); \
    Require(added.tag == ADD_NODE_OK, "AST capacity exceeded"); \
    return (parse_node_result){.tag = PARSE_NODE_OK, .node = added.node}; \
} while (0)
    Require(depth <= 128, "maximum syntax nesting exceeded (128)");
    usize mark = p->scratch_used;
    u32 token = p->token;
    if (kind == PARSE_NAME) {
        Consume(TOKEN_IDENTIFIER, "expected identifier");
        Finish(NODE_IDENTIFIER, token);
    }
    if (kind == PARSE_STATEMENT) {
        if (At(p, TOKEN_LET)) {
            ++p->token;
            Child(PARSE_NAME);
            Consume(TOKEN_EQUAL, "expected '=' after binding name");
            Child(PARSE_EXPRESSION);
            Consume(TOKEN_SEMICOLON, "expected ';' after binding");
            Finish(NODE_LET, token);
        }
        if (At(p, TOKEN_RETURN)) {
            ++p->token;
            if (!At(p, TOKEN_SEMICOLON)) Child(PARSE_EXPRESSION);
            Consume(TOKEN_SEMICOLON, "expected ';' after return");
            Finish(NODE_RETURN, token);
        }
        parse_node_result expression = ParseNode(p, PARSE_EXPRESSION, depth + 1);
        if (expression.tag != PARSE_NODE_OK) return (parse_node_result){.tag = PARSE_NODE_FAILED};
        Consume(TOKEN_SEMICOLON, "expected ';' after expression");
        return (parse_node_result){.tag = PARSE_NODE_OK, .node = expression.node};
    }
    if (kind == PARSE_PROPERTY) {
        Require(At(p, TOKEN_IDENTIFIER) || At(p, TOKEN_STRING), "expected object property name");
        /* Property names are leaves, not expressions/calls. */
        declaration_node_tag key_tag = At(p, TOKEN_STRING) ? NODE_STRING : NODE_IDENTIFIER;
        ++p->token;
        add_node_result key = AddNode(p, key_tag, token, mark);
        Require(key.tag == ADD_NODE_OK && p->scratch_used < p->scratch.len, "AST capacity exceeded");
        p->scratch.ptr[p->scratch_used++] = key.node;
        Consume(TOKEN_COLON, "expected ':' after property name");
        Child(PARSE_EXPRESSION);
        Finish(NODE_PROPERTY, token);
    }
    if (kind == PARSE_EXPRESSION) {
        declaration_node_tag tag;
        switch (Current(p)) {
        case TOKEN_IDENTIFIER: tag = NODE_IDENTIFIER; ++p->token; break;
        case TOKEN_STRING: tag = NODE_STRING; ++p->token; break;
        case TOKEN_NUMBER: tag = NODE_NUMBER; ++p->token; break;
        case TOKEN_TRUE: case TOKEN_FALSE: tag = NODE_BOOLEAN; ++p->token; break;
        case TOKEN_NULL: tag = NODE_NULL; ++p->token; break;
        case TOKEN_LEFT_PAREN:
            Child(PARSE_PARAMETERS);
            Consume(TOKEN_ARROW, "expected '=>' after callback parameters");
            Child(PARSE_BLOCK);
            tag = NODE_CALLBACK;
            break;
        case TOKEN_LEFT_BRACE: case TOKEN_LEFT_BRACKET: {
            parse_node_result container = ParseNode(p, At(p, TOKEN_LEFT_BRACE) ? PARSE_OBJECT : PARSE_ARRAY, depth + 1);
            if (container.tag != PARSE_NODE_OK) return (parse_node_result){.tag = PARSE_NODE_FAILED};
            /* Containers already have their node; enter the postfix loop below. */
            Require(p->scratch_used < p->scratch.len, "AST scratch capacity exceeded");
            p->scratch.ptr[p->scratch_used++] = container.node;
            goto postfix;
        }
        default:
            Diagnose(p, StringSlice("expected expression"));
            return (parse_node_result){.tag = PARSE_NODE_FAILED};
        }
        add_node_result primary = AddNode(p, tag, token, mark);
        Require(primary.tag == ADD_NODE_OK && p->scratch_used < p->scratch.len, "AST capacity exceeded");
        p->scratch.ptr[p->scratch_used++] = primary.node;
postfix:
        while (At(p, TOKEN_DOT) || At(p, TOKEN_LEFT_PAREN)) {
            token = p->token;
            tag = NODE_MEMBER;
            if (At(p, TOKEN_DOT)) { ++p->token; Child(PARSE_NAME); }
            else {
                tag = NODE_CALL;
                u32 callee = p->scratch.ptr[mark];
                if (p->ast->node_tags.ptr[callee] == NODE_IDENTIFIER) {
                    u32 name = p->ast->main_tokens.ptr[callee];
                    if (TokenEquals(p, name, StringSlice("collection"))) tag = NODE_COLLECTION;
                    else if (TokenEquals(p, name, StringSlice("route"))) tag = NODE_ROUTE;
                    else if (TokenEquals(p, name, StringSlice("query"))) tag = NODE_QUERY;
                    else if (TokenEquals(p, name, StringSlice("version"))) tag = NODE_VERSION;
                }
                Child(PARSE_ARGUMENTS);
            }
            add_node_result combined = AddNode(p, tag, token, mark);
            Require(combined.tag == ADD_NODE_OK && p->scratch_used < p->scratch.len, "AST capacity exceeded");
            p->scratch.ptr[p->scratch_used++] = combined.node;
        }
        u32 node = p->scratch.ptr[mark];
        p->scratch_used = mark;
        return (parse_node_result){.tag = PARSE_NODE_OK, .node = node};
    }
    declaration_token_tag close;
    declaration_node_tag tag;
    parse_kind item;
    bool comma = true;
    switch (kind) {
    case PARSE_FILE: close = TOKEN_EOF; tag = NODE_FILE; item = PARSE_STATEMENT; comma = false; break;
    case PARSE_BLOCK: Consume(TOKEN_LEFT_BRACE, "expected callback body '{'"); close = TOKEN_RIGHT_BRACE; tag = NODE_BLOCK; item = PARSE_STATEMENT; comma = false; break;
    case PARSE_OBJECT: Consume(TOKEN_LEFT_BRACE, "expected '{'"); close = TOKEN_RIGHT_BRACE; tag = NODE_OBJECT; item = PARSE_PROPERTY; break;
    case PARSE_ARRAY: Consume(TOKEN_LEFT_BRACKET, "expected '['"); close = TOKEN_RIGHT_BRACKET; tag = NODE_ARRAY; item = PARSE_EXPRESSION; break;
    case PARSE_ARGUMENTS: Consume(TOKEN_LEFT_PAREN, "expected '('"); close = TOKEN_RIGHT_PAREN; tag = NODE_ARGUMENTS; item = PARSE_EXPRESSION; break;
    case PARSE_PARAMETERS: Consume(TOKEN_LEFT_PAREN, "expected '('"); close = TOKEN_RIGHT_PAREN; tag = NODE_PARAMETERS; item = PARSE_NAME; break;
    default: Diagnose(p, StringSlice("invalid parser context")); return (parse_node_result){.tag = PARSE_NODE_FAILED};
    }
    while (!At(p, close)) {
        Require(!At(p, TOKEN_EOF), "unexpected end of file inside list or block");
        Child(item);
        if (kind == PARSE_FILE) {
            u32 child = p->scratch.ptr[p->scratch_used - 1];
            u8 child_tag = p->ast->node_tags.ptr[child];
            Require(child_tag == NODE_COLLECTION || child_tag == NODE_ROUTE || child_tag == NODE_QUERY || child_tag == NODE_VERSION,
                "expected version, collection, route, or query declaration at file scope");
        }
        if (comma && !At(p, close)) Consume(TOKEN_COMMA, "expected ',' or closing delimiter");
    }
    if (kind != PARSE_FILE) ++p->token;
    Finish(tag, token);
#undef Finish
#undef Child
#undef Consume
#undef Require
}
void DeclarationAstDestroy(declaration_ast *ast)
{
    if (!ast) return;
    free(ast->token_tags.ptr); free(ast->token_starts.ptr); free(ast->token_ends.ptr);
    free(ast->node_tags.ptr); free(ast->main_tokens.ptr); free(ast->node_data.ptr); free(ast->extra.ptr);
    *ast = (declaration_ast){0};
}
declaration_parse_result DeclarationParse(const_byte_slice source, declaration_ast *output)
{
    if (!output) return (declaration_parse_result){.tag = DECLARATION_PARSE_INVALID_ARGUMENT, .message = StringSlice("null output")};
    *output = (declaration_ast){0};
    if ((!source.ptr && source.len) || source.len > UINT32_MAX)
        return (declaration_parse_result){.tag = DECLARATION_PARSE_INVALID_ARGUMENT, .message = StringSlice("source exceeds 32-bit offsets or is invalid")};
    /* Count tokens first, then allocate columns once. Each token produces at
     * most two nodes; child edges fit in the same bound. No per-node allocation. */
    declaration_lexer lexer = {source, 0};
    usize tokens = 0;
    for (;;) {
        declaration_lex_result lexed = DeclarationLexNext(&lexer);
        if (lexed.tag != DECLARATION_LEX_OK) {
            string_slice message;
            switch (lexed.tag) {
            case DECLARATION_LEX_UNTERMINATED_STRING: message = StringSlice("unterminated string"); break;
            case DECLARATION_LEX_UNTERMINATED_COMMENT: message = StringSlice("unterminated block comment"); break;
            case DECLARATION_LEX_INVALID_ESCAPE: message = StringSlice("invalid string escape"); break;
            case DECLARATION_LEX_INVALID_NUMBER: message = StringSlice("invalid number"); break;
            default: message = StringSlice("invalid source byte"); break;
            }
            return (declaration_parse_result){.tag = DECLARATION_PARSE_LEXICAL_ERROR, .offset = lexed.offset, .message = message};
        }
        ++tokens;
        if (lexed.token.tag == TOKEN_EOF) break;
    }
    if (tokens > (UINT32_MAX - 1) / 2 || tokens > (SIZE_MAX / sizeof(declaration_node_data) - 1) / 2)
        return (declaration_parse_result){.tag = DECLARATION_PARSE_NO_SPACE, .message = StringSlice("too many tokens")};
    usize capacity = tokens * 2 + 1;
    declaration_ast ast = {.source = source};
#define Allocate(column, count) do { ast.column.ptr = calloc(count, sizeof(*ast.column.ptr)); ast.column.len = count; } while (0)
    Allocate(token_tags, tokens); Allocate(token_starts, tokens); Allocate(token_ends, tokens);
    Allocate(node_tags, capacity); Allocate(main_tokens, capacity); Allocate(node_data, capacity); Allocate(extra, capacity);
#undef Allocate
    declaration_u32_slice scratch = {calloc(capacity, sizeof(u32)), capacity};
    if (!ast.token_tags.ptr || !ast.token_starts.ptr || !ast.token_ends.ptr || !ast.node_tags.ptr || !ast.main_tokens.ptr || !ast.node_data.ptr || !ast.extra.ptr || !scratch.ptr) {
        DeclarationAstDestroy(&ast); free(scratch.ptr);
        return (declaration_parse_result){.tag = DECLARATION_PARSE_NO_SPACE, .message = StringSlice("could not allocate AST columns")};
    }
    lexer.cursor = 0;
    SliceFor(ast.token_tags, i) {
        declaration_lex_result lexed = DeclarationLexNext(&lexer);
        if (lexed.tag != DECLARATION_LEX_OK) {
            DeclarationAstDestroy(&ast); free(scratch.ptr);
            return (declaration_parse_result){.tag = DECLARATION_PARSE_LEXICAL_ERROR, .offset = lexed.offset, .message = StringSlice("source changed during parsing")};
        }
        ast.token_tags.ptr[i] = (u8)lexed.token.tag;
        ast.token_starts.ptr[i] = lexed.token.start; ast.token_ends.ptr[i] = lexed.token.end;
    }
    /* Reserve index zero for the root; move the completed file node there. */
    parser p = {.ast = &ast, .scratch = scratch, .nodes_used = 1};
    parse_node_result parsed = ParseNode(&p, PARSE_FILE, 0);
    free(scratch.ptr);
    if (parsed.tag != PARSE_NODE_OK) {
        DeclarationAstDestroy(&ast);
        return (declaration_parse_result){.tag = DECLARATION_PARSE_SYNTAX_ERROR, .offset = p.error_offset, .message = p.error};
    }
    ast.node_tags.ptr[0] = ast.node_tags.ptr[parsed.node];
    ast.main_tokens.ptr[0] = ast.main_tokens.ptr[parsed.node];
    ast.node_data.ptr[0] = ast.node_data.ptr[parsed.node];
    ast.node_tags.len = ast.main_tokens.len = ast.node_data.len = p.nodes_used - 1;
    ast.extra.len = p.extra_used;
    *output = ast;
    return (declaration_parse_result){.tag = DECLARATION_PARSE_OK};
}
string_slice DeclarationNodeName(declaration_node_tag tag)
{
    const string_slice names[] = {
        StringSlice("file"), StringSlice("collection"), StringSlice("route"), StringSlice("query"), StringSlice("call"), StringSlice("arguments"),
        StringSlice("callback"), StringSlice("parameters"), StringSlice("block"), StringSlice("let"), StringSlice("return"),
        StringSlice("object"), StringSlice("property"), StringSlice("array"), StringSlice("member"),
        StringSlice("identifier"), StringSlice("string"), StringSlice("number"), StringSlice("boolean"), StringSlice("null"), StringSlice("version"),
    };
    if ((usize)tag >= sizeof(names) / sizeof(names[0])) return StringSlice("unknown node");
    return names[tag];
}
