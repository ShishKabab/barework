#ifndef DECLARATION_PARSER_H
#define DECLARATION_PARSER_H
#include "lexer.h"

typedef struct declaration_u32_slice { u32 *ptr; usize len; } declaration_u32_slice;
typedef struct declaration_u8_slice { u8 *ptr; usize len; } declaration_u8_slice;
typedef enum declaration_node_tag {
    NODE_FILE, NODE_COLLECTION, NODE_ROUTE, NODE_QUERY, NODE_CALL, NODE_ARGUMENTS,
    NODE_CALLBACK, NODE_PARAMETERS, NODE_BLOCK, NODE_LET, NODE_RETURN,
    NODE_OBJECT, NODE_PROPERTY, NODE_ARRAY, NODE_MEMBER,
    NODE_IDENTIFIER, NODE_STRING, NODE_NUMBER, NODE_BOOLEAN, NODE_NULL, NODE_VERSION,
} declaration_node_tag;
/* An ordered range of node indices in extra. Leaves have count=0.
 * Calls: callee, arguments. Callback: parameters, block. Let: name, value.
 * Property/member: key/value and receiver/name respectively. Return: 0 or 1 value.
 * File/block/arguments/parameters/object/array: their members in source order.
 * Collection/route/query have the same layout as call, recognized syntactically. */
typedef struct declaration_node_data { u32 start, count; } declaration_node_data;
typedef struct declaration_data_slice { declaration_node_data *ptr; usize len; } declaration_data_slice;
/* Separate columns avoid loading spans or child ranges when scanning tags.
 * Owns all columns; borrows source, which must outlive this AST. No node pointers.
 * A successful parse has root node 0. Destroy even after a failed parse.
 * Treat returned columns as read-only. */
typedef struct declaration_ast {
    const_byte_slice source;
    declaration_u8_slice token_tags;
    declaration_u32_slice token_starts, token_ends;
    declaration_u8_slice node_tags;
    declaration_u32_slice main_tokens;
    declaration_data_slice node_data;
    declaration_u32_slice extra;
} declaration_ast;
typedef enum declaration_parse_tag {
    DECLARATION_PARSE_OK, DECLARATION_PARSE_INVALID_ARGUMENT, DECLARATION_PARSE_NO_SPACE,
    DECLARATION_PARSE_LEXICAL_ERROR, DECLARATION_PARSE_SYNTAX_ERROR,
} declaration_parse_tag;
typedef struct declaration_parse_result {
    declaration_parse_tag tag;
    u32 offset;
    string_slice message; /* Static diagnostic text; no ownership. */
} declaration_parse_result;
/* output must be fresh/destroyed. On failure it is empty. Maximum nesting: 128. */
declaration_parse_result DeclarationParse(const_byte_slice source, declaration_ast *output);
void DeclarationAstDestroy(declaration_ast *ast);
string_slice DeclarationNodeName(declaration_node_tag tag);
#endif
