#ifndef DECLARATION_IR_H
#define DECLARATION_IR_H
#include "parser.h"

/* References carry their table, so collection#3 cannot become field#3. */
typedef enum ir_table_kind {
    IR_NONE, IR_COLLECTION, IR_FIELD, IR_RELATIONSHIP, IR_ROUTE, IR_QUERY,
    IR_CONSTRAINT, IR_BINDING, IR_EXPRESSION, IR_SELECTION, IR_STATEMENT,
} ir_table_kind;
typedef struct ir_ref { ir_table_kind table; u32 index; } ir_ref;
typedef struct ir_ref_slice { ir_ref *ptr; usize len; } ir_ref_slice;
typedef struct ir_range { u32 start, count; } ir_range;
typedef struct ir_string_slice { string_slice *ptr; usize len; } ir_string_slice;
typedef enum ir_value_kind {
    IR_VOID, IR_STRING, IR_BOOLEAN, IR_NUMBER, IR_TIMESTAMP, IR_ID,
    IR_RECORD, IR_RECORDS, IR_ROUTE_CONTEXT, IR_ROUTE_PARAMETERS, IR_UPDATE_CONTEXT, IR_NULL,
} ir_value_kind;
typedef struct ir_type { ir_value_kind kind; ir_ref entity; bool optional; } ir_type;
typedef struct ir_type_slice { ir_type *ptr; usize len; } ir_type_slice;
/* Tag-specific payload. All references are resolved; zero reference means absent.
 * collection: items = fields/relationships/constraints (populated after resolution).
 * field: a = owning collection; type includes nominal ID collection where needed.
 * relationship: a = source, b = target, c = inverse; flags in kind below.
 * route: a = context binding, b = full path binding; items = body statements. Path segments are bindings
 *        (IR_BINDING_PATH_LITERAL/PARAMETER) owned by the route, in path order.
 * query: a = route scope (or absent), b = collection, c = predicate; items = selections.
 * constraint: a = collection, b = predicate (unique) or context binding (check);
 *             items = resolved keys (unique) or statements (check).
 * binding: a = scope (route/constraint), b = initializer; kind distinguishes role.
 * expression: a/b/c are operands/references as described by ir_expression_kind.
 * selection: a = relationship, b = predicate; items = nested selections.
 * statement: a = binding for let, expression for evaluate/return.
 * name: decoded string ID; UINT32_MAX means unnamed. source: original byte offset.
 */
typedef struct ir_data { ir_ref a, b, c; ir_range items; } ir_data;
typedef struct ir_data_slice { ir_data *ptr; usize len; } ir_data_slice;
typedef struct ir_table {
    declaration_u32_slice names, sources, kinds;
    ir_type_slice types;
    ir_data_slice data;
} ir_table;
enum ir_relationship_flags { IR_RELATION_MANY = 1, IR_RELATION_REVERSE = 2, IR_RELATION_OPTIONAL = 4 };
typedef enum ir_constraint_kind { IR_UNIQUE, IR_FILTERED_UNIQUE, IR_CHECK_UPDATE } ir_constraint_kind;
typedef enum ir_binding_kind { IR_BINDING_CONTEXT, IR_BINDING_LOCAL, IR_BINDING_PATH_LITERAL, IR_BINDING_PATH_PARAMETER, IR_BINDING_PATH } ir_binding_kind;
typedef enum ir_statement_kind { IR_LET, IR_EVALUATE, IR_RETURN } ir_statement_kind;
typedef enum ir_expression_kind {
    IR_LITERAL, /* name = decoded literal; type distinguishes string/number/bool/null */
    IR_LOAD_BINDING, /* a = binding */
    IR_CURRENT_RECORD, /* a = collection being filtered */
    IR_NEW_RECORD, /* a = context expression; type.entity = collection */
    IR_ROUTE_VALUES, /* a = context expression; type.entity = route */
    IR_ROUTE_PARAMETER, /* a = path parameter binding */
    IR_FIELD_ACCESS, /* a = receiver, b = field */
    IR_RELATION_ACCESS, /* a = receiver, b = relationship */
    IR_EQUAL, IR_AND, /* a/b = expressions */
    IR_PARSE_PARAMETER, /* a = text expression; type = checked conversion target */
    IR_EXISTS, /* a = collection, b = predicate */
    IR_RUN_QUERY, /* a = query */
} ir_expression_kind;
/* Owns all storage, including decoded names/literals; no dependency on AST lifetime. */
typedef struct declaration_ir {
    u32 version;
    ir_table collections, fields, relationships, routes, queries, constraints;
    ir_table bindings, expressions, selections, statements;
    ir_ref_slice extra;
    ir_string_slice strings;
    byte_slice string_storage;
} declaration_ir;
typedef enum declaration_resolve_tag {
    DECLARATION_RESOLVE_OK, DECLARATION_RESOLVE_INVALID_ARGUMENT,
    DECLARATION_RESOLVE_NO_SPACE, DECLARATION_RESOLVE_SEMANTIC_ERROR,
} declaration_resolve_tag;
typedef struct declaration_resolve_result { declaration_resolve_tag tag; u32 offset; string_slice message; } declaration_resolve_result;
/* Accepts an unmodified successful DeclarationParse AST. output must be fresh.
 * Failure destroys partial output. Diagnostics are static strings + source offsets. */
declaration_resolve_result DeclarationResolve(const declaration_ast *ast, declaration_ir *output);
void DeclarationIrDestroy(declaration_ir *ir);
const ir_table *DeclarationIrTable(const declaration_ir *ir, ir_table_kind kind);
string_slice DeclarationIrTableName(ir_table_kind kind);
string_slice DeclarationIrTypeName(ir_value_kind kind);
string_slice DeclarationIrExpressionName(ir_expression_kind kind);
#endif
