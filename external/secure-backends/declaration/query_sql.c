#define _GNU_SOURCE
#include "query_sql.h"
#include "sql.h"
#include "slice.h"
#include <stdlib.h>

typedef struct procedure_plan {
    u8 name_storage[63]; usize name_length;
    declaration_u32_slice parameters; /* binding ID -> input ordinal, 0 if unused */
    u32 parameter_count;
} procedure_plan;
typedef struct row_scope { u32 collection, alias; } row_scope;
typedef struct query_writer {
    const declaration_ir *ir;
    u32 query, next_alias;
    procedure_plan *plan;
    FILE *stream;
    bool planning, io_error;
    u32 offset;
    string_slice error;
    byte_slice dependencies;
} query_writer;
static void Reject(query_writer *w, string_slice message) { w->error = message; }
#define Check(condition, message) do { if (!(condition)) { Reject(w, StringSlice(message)); return failure; } } while (0)
#define Take(name, type, call, okay) type name = call; if (name.tag != okay) return failure
#define Result(type, prefix) typedef enum type##_tag { prefix##_OK, prefix##_ERROR } type##_tag; typedef struct type { type##_tag tag; } type
Result(text_result, TEXT);
Result(identifier_result, IDENTIFIER);
Result(literal_result, LITERAL);
Result(number_result, NUMBER);
Result(column_result, COLUMN);
Result(expression_sql_result, EXPRESSION_SQL);
Result(value_column_result, VALUE_COLUMN);
Result(projection_result, PROJECTION);
Result(selection_sql_result, SELECTION_SQL);
Result(body_sql_result, BODY_SQL);
Result(signature_result, SIGNATURE);
Result(line_result, LINE);
#undef Result
static text_result Text(query_writer *w, string_slice text)
{
    if (w->stream && text.len && fwrite(text.ptr, 1, text.len, w->stream) != text.len) {
        w->io_error = true; Reject(w, StringSlice("could not write procedure SQL")); return (text_result){TEXT_ERROR};
    }
    return (text_result){TEXT_OK};
}
#define Put(value) do { if (Text(w, value).tag != TEXT_OK) return failure; } while (0)
static line_result Line(query_writer *w, usize depth)
{
    line_result failure = {LINE_ERROR};
    Put(StringSlice("\n"));
    for (usize i = 0; i <= depth; ++i) Put(StringSlice("    "));
    return (line_result){LINE_OK};
}
#define NewLine(depth) do { if (Line(w, depth).tag != LINE_OK) return failure; } while (0)
static number_result Number(query_writer *w, u32 number)
{
    number_result failure = {NUMBER_ERROR};
    char text[16];
    i32 length = snprintf(text, sizeof(text), "%u", number);
    Check(length > 0 && (usize)length < sizeof(text), "cannot format SQL number");
    Put(((string_slice){text, (usize)length}));
    return (number_result){NUMBER_OK};
}
#define Num(value) do { if (Number(w, value).tag != NUMBER_OK) return failure; } while (0)
static identifier_result Identifier(query_writer *w, string_slice text)
{
    identifier_result failure = {IDENTIFIER_ERROR};
    Check(DeclarationSqlIdentifier(text), "SQL identifier is invalid or exceeds 63 bytes");
    Put(StringSlice("\""));
    SliceFor(text, i) {
        if (text.ptr[i] == '"') Put(StringSlice("\""));
        Put(((string_slice){&text.ptr[i], 1}));
    }
    Put(StringSlice("\""));
    return (identifier_result){IDENTIFIER_OK};
}
#define Ident(value) do { if (Identifier(w, value).tag != IDENTIFIER_OK) return failure; } while (0)
static literal_result Literal(query_writer *w, string_slice text)
{
    literal_result failure = {LITERAL_ERROR};
    Check(DeclarationSqlText(text), "SQL text literal contains NUL or invalid UTF-8");
    Put(StringSlice("E'"));
    SliceFor(text, i) {
        u8 c = (u8)text.ptr[i];
        if (c == '\'' || c == '\\') Put(((string_slice){&text.ptr[i], 1}));
        if (c < 32 || c == 127) {
            char octal[5];
            i32 length = snprintf(octal, sizeof(octal), "\\%03o", c);
            Check(length == 4, "cannot encode SQL string byte");
            Put(((string_slice){octal, 4}));
        } else Put(((string_slice){&text.ptr[i], 1}));
    }
    Put(StringSlice("'"));
    return (literal_result){LITERAL_OK};
}
static bool Equal(string_slice a, string_slice b)
{
    if (a.len != b.len) return false;
    SliceFor(a, i) { if (a.ptr[i] != b.ptr[i]) return false; }
    return true;
}
static string_slice Name(const declaration_ir *ir, ir_table_kind kind, u32 index)
{
    const ir_table *table = DeclarationIrTable(ir, kind);
    return ir->strings.ptr[table->names.ptr[index]];
}
static string_slice ScalarType(ir_value_kind kind)
{
    switch (kind) {
    case IR_STRING: return StringSlice("text"); case IR_ID: return StringSlice("bigint");
    case IR_NUMBER: return StringSlice("double precision"); case IR_BOOLEAN: return StringSlice("boolean");
    case IR_TIMESTAMP: return StringSlice("timestamp with time zone"); default: return (string_slice){0};
    }
}
static column_result Column(query_writer *w, u32 alias, string_slice column)
{
    column_result failure = {COLUMN_ERROR};
    Put(StringSlice("\"r")); Num(alias); Put(StringSlice("\".")); Ident(column);
    return (column_result){COLUMN_OK};
}
#define Col(alias, name) do { if (Column(w, alias, name).tag != COLUMN_OK) return failure; } while (0)
static bool HasRef(const declaration_ir *ir, ir_ref ref, ir_table_kind kind)
{
    const ir_table *table = DeclarationIrTable(ir, kind);
    return ref.table == kind && table && ref.index < table->data.len;
}
static bool Range(const declaration_ir *ir, ir_range range)
{ return range.start <= ir->extra.len && range.count <= ir->extra.len - range.start; }
/* Empty filters resolve to a boolean true literal. Keep it in the IR, but do
 * not emit a SQL predicate for it. A field comparison to true is not omitted. */
static bool Unfiltered(const declaration_ir *ir, ir_ref predicate)
{
    if (!HasRef(ir, predicate, IR_EXPRESSION) ||
        predicate.index >= ir->expressions.kinds.len || predicate.index >= ir->expressions.types.len ||
        predicate.index >= ir->expressions.names.len ||
        ir->expressions.kinds.ptr[predicate.index] != IR_LITERAL ||
        ir->expressions.types.ptr[predicate.index].kind != IR_BOOLEAN) return false;
    u32 name = ir->expressions.names.ptr[predicate.index];
    return name < ir->strings.len && Equal(ir->strings.ptr[name], StringSlice("true"));
}
static expression_sql_result ExpressionSql(query_writer *w, ir_ref expression, row_scope row, usize depth);
static projection_result Projection(query_writer *w, row_scope row, ir_range selections, usize depth);

/* Read a scalar member through a record expression. Nested relationship paths
 * use correlated scalar subqueries; a forward relationship's ID reads its stored
 * key directly. Missing optional rows yield NULL; duplicate to-one rows error. */
static value_column_result ValueColumn(query_writer *w, ir_ref receiver, ir_ref member, row_scope row, usize depth)
{
    value_column_result failure = {VALUE_COLUMN_ERROR};
    const declaration_ir *ir = w->ir;
    Check(depth <= 256 && HasRef(ir, receiver, IR_EXPRESSION), "invalid or excessively deep record receiver");
    Check(HasRef(ir, member, IR_FIELD) || HasRef(ir, member, IR_RELATIONSHIP), "invalid scalar member reference");
    const ir_table *members = DeclarationIrTable(ir, member.table);
    ir_data member_data = members->data.ptr[member.index], data = ir->expressions.data.ptr[receiver.index];
    u32 kind = ir->expressions.kinds.ptr[receiver.index];
    if (kind == IR_LOAD_BINDING) {
        Check(HasRef(ir, data.a, IR_BINDING) && ir->bindings.kinds.ptr[data.a.index] == IR_BINDING_LOCAL, "unsupported record binding in query");
        Take(value, value_column_result, ValueColumn(w, ir->bindings.data.ptr[data.a.index].b, member, row, depth + 1), VALUE_COLUMN_OK); (void)value;
    } else if (kind == IR_CURRENT_RECORD) {
        Check(data.a.table == IR_COLLECTION && data.a.index == row.collection && member_data.a.table == IR_COLLECTION && member_data.a.index == row.collection, "member receiver belongs to a different query row");
        Col(row.alias, Name(ir, member.table, member.index));
    } else if (kind == IR_RELATION_ACCESS) {
        Check(HasRef(ir, data.b, IR_RELATIONSHIP), "invalid relationship receiver");
        ir_data relationship = ir->relationships.data.ptr[data.b.index];
        Check(member_data.a.table == IR_COLLECTION && member_data.a.index == relationship.b.index, "member does not belong to relationship target");
        bool reverse = (ir->relationships.kinds.ptr[data.b.index] & IR_RELATION_REVERSE) != 0;
        Check(!(ir->relationships.kinds.ptr[data.b.index] & IR_RELATION_MANY), "cannot read scalar member through to-many relationship");
        if (!reverse && member.table == IR_FIELD && ir->fields.types.ptr[member.index].kind == IR_ID) {
            Take(key, value_column_result, ValueColumn(w, data.a, data.b, row, depth + 1), VALUE_COLUMN_OK); (void)key;
        } else {
            u32 alias = w->next_alias++;
            Put(StringSlice("(SELECT ")); Col(alias, Name(ir, member.table, member.index));
            if (w->dependencies.len) w->dependencies.ptr[relationship.b.index] = 1;
            Put(StringSlice(" FROM ")); Ident(Name(ir, IR_COLLECTION, relationship.b.index));
            Put(StringSlice(" AS \"r")); Num(alias); Put(StringSlice("\" WHERE "));
            if (reverse) {
                Check(HasRef(ir, relationship.c, IR_RELATIONSHIP), "missing inverse relationship");
                Col(alias, Name(ir, IR_RELATIONSHIP, relationship.c.index)); Put(StringSlice(" = "));
                ir_ref id = {IR_NONE, 0};
                SliceFor(ir->fields.names, i) {
                    if (ir->fields.data.ptr[i].a.index == relationship.a.index && ir->fields.types.ptr[i].kind == IR_ID) { id = (ir_ref){IR_FIELD, (u32)i}; break; }
                }
                Check(id.table != IR_NONE, "missing collection ID");
                Take(parent, value_column_result, ValueColumn(w, data.a, id, row, depth + 1), VALUE_COLUMN_OK); (void)parent;
            } else {
                Col(alias, StringSlice("id")); Put(StringSlice(" = "));
                Take(parent, value_column_result, ValueColumn(w, data.a, data.b, row, depth + 1), VALUE_COLUMN_OK); (void)parent;
            }
            Put(StringSlice(")"));
        }
    } else { Reject(w, StringSlice("record expression cannot be lowered to SQL in this query")); return failure; }
    return (value_column_result){VALUE_COLUMN_OK};
}
static expression_sql_result ExpressionSql(query_writer *w, ir_ref expression, row_scope row, usize depth)
{
    expression_sql_result failure = {EXPRESSION_SQL_ERROR};
    const declaration_ir *ir = w->ir;
    Check(depth <= 256 && HasRef(ir, expression, IR_EXPRESSION), "invalid or excessively deep query expression");
    w->offset = ir->expressions.sources.ptr[expression.index];
    ir_data data = ir->expressions.data.ptr[expression.index];
    ir_type type = ir->expressions.types.ptr[expression.index];
    switch ((ir_expression_kind)ir->expressions.kinds.ptr[expression.index]) {
    case IR_LITERAL: {
        string_slice text = Name(ir, IR_EXPRESSION, expression.index);
        if (type.kind == IR_STRING) { Take(literal, literal_result, Literal(w, text), LITERAL_OK); (void)literal; }
        else if (type.kind == IR_BOOLEAN) {
            Check(Equal(text, StringSlice("true")) || Equal(text, StringSlice("false")), "invalid boolean literal"); Put(text);
        } else if (type.kind == IR_NULL) Put(StringSlice("NULL"));
        else if (type.kind == IR_NUMBER) {
            bool dot = false;
            Check(text.len, "empty number literal");
            SliceFor(text, i) {
                if (text.ptr[i] == '.' && !dot && i && i + 1 < text.len) dot = true;
                else Check(text.ptr[i] >= '0' && text.ptr[i] <= '9', "invalid number literal");
            }
            Put(StringSlice("CAST(")); Put(text); Put(StringSlice(" AS double precision)"));
        } else { Reject(w, StringSlice("unsupported SQL literal type")); return failure; }
        break;
    }
    case IR_ROUTE_PARAMETER: {
        Check(HasRef(ir, data.a, IR_BINDING) && data.a.index < w->plan->parameters.len, "invalid route parameter");
        ir_ref owner = ir->bindings.data.ptr[data.a.index].a, scope = ir->queries.data.ptr[w->query].a;
        Check(ir->bindings.kinds.ptr[data.a.index] == IR_BINDING_PATH_PARAMETER && owner.table == IR_ROUTE && scope.table == IR_ROUTE && owner.index == scope.index, "query references another route's parameter");
        if (w->planning) w->plan->parameters.ptr[data.a.index] = 1;
        Check(w->plan->parameters.ptr[data.a.index] != 0, "route parameter has no procedure argument");
        Put(StringSlice("$")); Num(w->plan->parameters.ptr[data.a.index]); break;
    }
    case IR_LOAD_BINDING:
        Check(HasRef(ir, data.a, IR_BINDING) && ir->bindings.kinds.ptr[data.a.index] == IR_BINDING_LOCAL, "only scalar local bindings can be inlined into queries");
        { Take(value, expression_sql_result, ExpressionSql(w, ir->bindings.data.ptr[data.a.index].b, row, depth + 1), EXPRESSION_SQL_OK); (void)value; } break;
    case IR_PARSE_PARAMETER: {
        string_slice sql_type = ScalarType(type.kind);
        Check(sql_type.len, "unsupported checked parameter conversion");
        Put(StringSlice("CAST("));
        Take(value, expression_sql_result, ExpressionSql(w, data.a, row, depth + 1), EXPRESSION_SQL_OK); (void)value;
        Put(StringSlice(" AS ")); Put(sql_type); Put(StringSlice(")")); break;
    }
    case IR_FIELD_ACCESS: {
        Check(HasRef(ir, data.b, IR_FIELD), "invalid field access");
        Take(value, value_column_result, ValueColumn(w, data.a, data.b, row, depth + 1), VALUE_COLUMN_OK); (void)value; break;
    }
    case IR_EQUAL: case IR_AND: {
        Put(StringSlice("("));
        Take(left, expression_sql_result, ExpressionSql(w, data.a, row, depth + 1), EXPRESSION_SQL_OK); (void)left;
        if (ir->expressions.kinds.ptr[expression.index] == IR_AND) Put(StringSlice(" AND "));
        else {
            Check(HasRef(ir, data.b, IR_EXPRESSION), "invalid equality operand");
            ir_type left_type = ir->expressions.types.ptr[data.a.index], right_type = ir->expressions.types.ptr[data.b.index];
            if (left_type.optional || right_type.optional || left_type.kind == IR_NULL || right_type.kind == IR_NULL)
                Put(StringSlice(" IS NOT DISTINCT FROM "));
            else Put(StringSlice(" = "));
        }
        Take(right, expression_sql_result, ExpressionSql(w, data.b, row, depth + 1), EXPRESSION_SQL_OK); (void)right;
        Put(StringSlice(")")); break;
    }
    case IR_EXISTS: {
        Check(HasRef(ir, data.a, IR_COLLECTION), "invalid EXISTS collection");
        row_scope nested = {data.a.index, w->next_alias++};
        if (w->dependencies.len) w->dependencies.ptr[nested.collection] = 1;
        Put(StringSlice("EXISTS (SELECT 1 FROM ")); Ident(Name(ir, IR_COLLECTION, nested.collection));
        Put(StringSlice(" AS \"r")); Num(nested.alias); Put(StringSlice("\""));
        if (!Unfiltered(ir, data.b)) {
            Put(StringSlice(" WHERE "));
            Take(predicate, expression_sql_result, ExpressionSql(w, data.b, nested, depth + 1), EXPRESSION_SQL_OK); (void)predicate;
        }
        Put(StringSlice(")")); break;
    }
    default: Reject(w, StringSlice("query expression is not supported by SQL lowering")); return failure;
    }
    return (expression_sql_result){EXPRESSION_SQL_OK};
}
static selection_sql_result SelectionSql(query_writer *w, ir_ref selection, row_scope parent, usize depth)
{
    selection_sql_result failure = {SELECTION_SQL_ERROR};
    const declaration_ir *ir = w->ir;
    Check(depth <= 256 && HasRef(ir, selection, IR_SELECTION), "invalid or excessively deep query selection");
    w->offset = ir->selections.sources.ptr[selection.index];
    ir_data data = ir->selections.data.ptr[selection.index];
    Check(HasRef(ir, data.a, IR_RELATIONSHIP), "invalid selected relationship");
    ir_data relationship = ir->relationships.data.ptr[data.a.index];
    Check(relationship.a.table == IR_COLLECTION && relationship.a.index == parent.collection && HasRef(ir, relationship.b, IR_COLLECTION), "selection does not belong to its parent collection");
    u32 flags = ir->relationships.kinds.ptr[data.a.index];
    bool many = (flags & IR_RELATION_MANY) != 0;
    row_scope target = {relationship.b.index, w->next_alias++};
    Put(StringSlice("(SELECT "));
    if (many) Put(StringSlice("COALESCE(pg_catalog.jsonb_agg("));
    Take(projection, projection_result, Projection(w, target, data.items, depth + 1), PROJECTION_OK); (void)projection;
    if (many) { Put(StringSlice(" ORDER BY ")); Col(target.alias, StringSlice("id")); Put(StringSlice("), '[]'::jsonb)")); }
    if (w->dependencies.len) w->dependencies.ptr[target.collection] = 1;
    NewLine(depth + 1); Put(StringSlice("FROM ")); Ident(Name(ir, IR_COLLECTION, target.collection));
    Put(StringSlice(" AS \"r")); Num(target.alias); Put(StringSlice("\""));
    NewLine(depth + 1); Put(StringSlice("WHERE "));
    if (flags & IR_RELATION_REVERSE) {
        Check(HasRef(ir, relationship.c, IR_RELATIONSHIP), "selected reverse relationship has no forward key");
        Col(target.alias, Name(ir, IR_RELATIONSHIP, relationship.c.index)); Put(StringSlice(" = ")); Col(parent.alias, StringSlice("id"));
    } else {
        Col(target.alias, StringSlice("id")); Put(StringSlice(" = ")); Col(parent.alias, Name(ir, IR_RELATIONSHIP, data.a.index));
    }
    if (!Unfiltered(ir, data.b)) {
        Put(StringSlice(" AND "));
        Take(predicate, expression_sql_result, ExpressionSql(w, data.b, target, depth + 1), EXPRESSION_SQL_OK); (void)predicate;
    }
    Put(StringSlice(")"));
    return (selection_sql_result){SELECTION_SQL_OK};
}
static projection_result Projection(query_writer *w, row_scope row, ir_range selections, usize depth)
{
    projection_result failure = {PROJECTION_ERROR};
    const declaration_ir *ir = w->ir;
    Check(depth <= 256 && Range(ir, selections), "invalid projection selection range");
    Put(StringSlice("(pg_catalog.to_jsonb(\"r")); Num(row.alias); Put(StringSlice("\")"));
    ir_ref_slice selected = {selections.count ? ir->extra.ptr + selections.start : NULL, selections.count};
    SliceFor(selected, i) {
        Check(HasRef(ir, selected.ptr[i], IR_SELECTION), "invalid projection selection");
        ir_ref relationship = ir->selections.data.ptr[selected.ptr[i].index].a;
        Check(HasRef(ir, relationship, IR_RELATIONSHIP), "invalid projection relationship");
        /* One pair per object call avoids PostgreSQL's function argument limit.
         * Selected relationships replace their raw key with the nested value. */
        NewLine(depth + 1); Put(StringSlice("|| pg_catalog.jsonb_build_object("));
        Take(key, literal_result, Literal(w, Name(ir, IR_RELATIONSHIP, relationship.index)), LITERAL_OK); (void)key;
        Put(StringSlice(", "));
        Take(value, selection_sql_result, SelectionSql(w, selected.ptr[i], row, depth + 1), SELECTION_SQL_OK); (void)value;
        Put(StringSlice(")"));
    }
    Put(StringSlice(")"));
    return (projection_result){PROJECTION_OK};
}
static body_sql_result BodySql(query_writer *w)
{
    body_sql_result failure = {BODY_SQL_ERROR};
    const declaration_ir *ir = w->ir;
    Check(w->query < ir->queries.data.len, "invalid query ID");
    ir_data query = ir->queries.data.ptr[w->query];
    Check(HasRef(ir, query.b, IR_COLLECTION), "invalid query source collection");
    w->next_alias = 1;
    row_scope row = {query.b.index, 0};
    Put(StringSlice("    SELECT COALESCE(pg_catalog.jsonb_agg("));
    Take(projection, projection_result, Projection(w, row, query.items, 0), PROJECTION_OK); (void)projection;
    Put(StringSlice(" ORDER BY \"r0\".\"id\"), '[]'::jsonb) AS \"result\"\n    FROM "));
    if (w->dependencies.len) w->dependencies.ptr[row.collection] = 1;
    Ident(Name(ir, IR_COLLECTION, row.collection));
    Put(StringSlice(" AS \"r0\""));
    if (!Unfiltered(ir, query.c)) {
        Put(StringSlice("\n    WHERE "));
        Take(predicate, expression_sql_result, ExpressionSql(w, query.c, row, 0), EXPRESSION_SQL_OK); (void)predicate;
    }
    Put(StringSlice(";\n"));
    return (body_sql_result){BODY_SQL_OK};
}
typedef enum plan_query_tag { PLAN_QUERY_OK, PLAN_QUERY_UNSUPPORTED, PLAN_QUERY_NO_SPACE } plan_query_tag;
typedef struct plan_query_result { plan_query_tag tag; } plan_query_result;
static plan_query_result PlanQuery(query_writer *w)
{
    plan_query_result failure = {PLAN_QUERY_UNSUPPORTED};
    const declaration_ir *ir = w->ir;
    procedure_plan *plan = w->plan;
    Check(ir->version && w->query < ir->queries.names.len, "query requires a valid application version");
    string_slice name = Name(ir, IR_QUERY, w->query);
    ir_ref owner = ir->queries.data.ptr[w->query].a;
    string_slice scope = {0};
    if (owner.table != IR_NONE) {
        Check(HasRef(ir, owner, IR_ROUTE), "query scope must be a route or file scope");
        scope = Name(ir, IR_ROUTE, owner.index);
    }
    char version[16];
    i32 size = snprintf(version, sizeof(version), "_v%u", ir->version);
    Check(size > 0 && (usize)size < sizeof(version), "cannot format procedure version");
    byte_slice destination = {plan->name_storage, sizeof(plan->name_storage)};
    const string_slice parts_storage[] = {scope, scope.len ? StringSlice("_") : (string_slice){0}, name, {version, (usize)size}};
    struct parts_slice { const string_slice *ptr; usize len; } parts = {parts_storage, sizeof(parts_storage) / sizeof(parts_storage[0])};
    SliceFor(parts, i) {
        Check(ByteSliceCopy(destination, plan->name_length, (const_byte_slice){(const u8 *)parts.ptr[i].ptr, parts.ptr[i].len}).tag == BYTE_SLICE_COPY_OK, "procedure name including scope and version exceeds PostgreSQL's 63-byte limit");
        plan->name_length += parts.ptr[i].len;
    }
    Check(DeclarationSqlIdentifier((string_slice){(const char *)plan->name_storage, plan->name_length}), "invalid procedure identifier");
    if (ir->bindings.names.len) {
        plan->parameters.ptr = calloc(ir->bindings.names.len, sizeof(*plan->parameters.ptr));
        if (!plan->parameters.ptr) { Reject(w, StringSlice("cannot allocate query argument map")); return (plan_query_result){PLAN_QUERY_NO_SPACE}; }
    }
    plan->parameters.len = ir->bindings.names.len;
    w->planning = true;
    if (BodySql(w).tag != BODY_SQL_OK) return failure;
    w->planning = false;
    SliceFor(plan->parameters, i) {
        if (!plan->parameters.ptr[i]) continue;
        Check(plan->parameter_count < 99, "procedure exceeds PostgreSQL's argument limit (including OUT result)");
        string_slice parameter = Name(ir, IR_BINDING, (u32)i);
        w->offset = ir->bindings.sources.ptr[i];
        Check(parameter.len <= 61 && DeclarationSqlIdentifier(parameter), "route parameter name is invalid or too long for its p_ argument prefix");
        plan->parameters.ptr[i] = ++plan->parameter_count;
    }
    return (plan_query_result){PLAN_QUERY_OK};
}
static signature_result Signature(query_writer *w, bool dropping)
{
    signature_result failure = {SIGNATURE_ERROR};
    procedure_plan *plan = w->plan;
    Ident(((string_slice){(const char *)plan->name_storage, plan->name_length}));
    Put(StringSlice("("));
    bool first = true;
    SliceFor(plan->parameters, i) {
        if (!plan->parameters.ptr[i]) continue;
        if (!first) Put(StringSlice(", "));
        first = false;
        if (!dropping) {
            u8 storage[63];
            byte_slice bytes = {storage, sizeof(storage)};
            string_slice original = Name(w->ir, IR_BINDING, (u32)i);
            string_slice prefix = StringSlice("p_");
            Check(ByteSliceCopy(bytes, 0, (const_byte_slice){(const u8 *)prefix.ptr, prefix.len}).tag == BYTE_SLICE_COPY_OK &&
                ByteSliceCopy(bytes, prefix.len, (const_byte_slice){(const u8 *)original.ptr, original.len}).tag == BYTE_SLICE_COPY_OK, "invalid argument name bounds");
            Put(StringSlice("IN ")); Ident(((string_slice){(const char *)storage, original.len + prefix.len})); Put(StringSlice(" "));
        }
        Put(StringSlice("text"));
    }
    if (!dropping) { if (!first) Put(StringSlice(", ")); Put(StringSlice("OUT \"result\" jsonb")); }
    Put(StringSlice(")"));
    return (signature_result){SIGNATURE_OK};
}
typedef struct plan_slice { procedure_plan *ptr; usize len; } plan_slice;
static void PlansDestroy(plan_slice plans)
{
    SliceFor(plans, i) { free(plans.ptr[i].parameters.ptr); }
    free(plans.ptr);
}
#undef Check
#undef Take
#undef Put
#undef Num
#undef Ident
#undef Col
#undef NewLine

typedef enum query_validate_tag { QUERY_VALIDATE_OK, QUERY_VALIDATE_UNSUPPORTED, QUERY_VALIDATE_NO_SPACE } query_validate_tag;
typedef struct query_validate_result { query_validate_tag tag; string_slice message; } query_validate_result;
static query_validate_result QueryValidate(const declaration_ir *ir)
{
    plan_slice plans = {ir->queries.names.len ? calloc(ir->queries.names.len, sizeof(procedure_plan)) : NULL, ir->queries.names.len};
    if (plans.len && !plans.ptr) return (query_validate_result){QUERY_VALIDATE_NO_SPACE,StringSlice("cannot allocate procedure plans")};
    query_validate_result result = {.tag=QUERY_VALIDATE_OK};
    SliceFor(plans,i) {
        query_writer writer = {.ir=ir,.query=(u32)i,.plan=&plans.ptr[i]};
        plan_query_result planned = PlanQuery(&writer);
        if (planned.tag != PLAN_QUERY_OK) {
            result = (query_validate_result){planned.tag == PLAN_QUERY_NO_SPACE ? QUERY_VALIDATE_NO_SPACE : QUERY_VALIDATE_UNSUPPORTED,writer.error}; break;
        }
        string_slice name = {(const char *)plans.ptr[i].name_storage,plans.ptr[i].name_length};
        plan_slice preceding = {plans.ptr,i};
        SliceFor(preceding,j) {
            if (Equal(name,(string_slice){(const char *)plans.ptr[j].name_storage,plans.ptr[j].name_length})) {
                result=(query_validate_result){QUERY_VALIDATE_UNSUPPORTED,StringSlice("scoped query names collide after procedure naming")}; break;
            }
        }
        if (result.tag != QUERY_VALIDATE_OK) break;
    }
    PlansDestroy(plans); return result;
}
query_physical_result QueryPhysical(const declaration_ir *ir, physical_schema *schema)
{
    query_validate_result validated = QueryValidate(ir);
    if (validated.tag != QUERY_VALIDATE_OK) return (query_physical_result){validated.tag == QUERY_VALIDATE_NO_SPACE ? QUERY_PHYSICAL_NO_SPACE : QUERY_PHYSICAL_UNSUPPORTED,validated.message};
    SliceFor(ir->queries.names,i) {
        procedure_plan plan = {0};
        query_writer writer = {.ir=ir,.query=(u32)i,.plan=&plan};
        query_writer *w = &writer;
        query_physical_result result = {.tag=QUERY_PHYSICAL_OK};
        char *body = NULL, *signature = NULL; usize body_len=0,signature_len=0;
        FILE *stream = NULL;
        plan_query_result planned = PlanQuery(w);
        if (planned.tag != PLAN_QUERY_OK) { result=(query_physical_result){planned.tag == PLAN_QUERY_NO_SPACE ? QUERY_PHYSICAL_NO_SPACE : QUERY_PHYSICAL_UNSUPPORTED,w->error}; goto query_done; }
        w->dependencies = (byte_slice){calloc(ir->collections.names.len,1),ir->collections.names.len};
        if (w->dependencies.len && !w->dependencies.ptr) { result.tag=QUERY_PHYSICAL_NO_SPACE; goto query_done; }
        stream = open_memstream(&signature,&signature_len);
        if (!stream) { result.tag=QUERY_PHYSICAL_NO_SPACE; goto query_done; }
        w->stream=stream;
        bool okay = Signature(w,true).tag == SIGNATURE_OK;
        if (fclose(stream) != 0) okay=false;
        stream=NULL;
        if (!okay) { result.tag=QUERY_PHYSICAL_IO_ERROR; goto query_done; }
        stream=open_memstream(&body,&body_len);
        if (!stream) { result.tag=QUERY_PHYSICAL_NO_SPACE; goto query_done; }
        w->stream=stream; w->next_alias=0;
        okay = Text(w,StringSlice("CREATE PROCEDURE ")).tag == TEXT_OK && Signature(w,false).tag == SIGNATURE_OK &&
            Text(w,StringSlice("\nLANGUAGE SQL\nSECURITY INVOKER\nBEGIN ATOMIC\n")).tag == TEXT_OK && BodySql(w).tag == BODY_SQL_OK && Text(w,StringSlice("END;\n")).tag == TEXT_OK;
        if (fclose(stream) != 0) okay=false;
        stream=NULL;
        if (!okay) { result=(query_physical_result){QUERY_PHYSICAL_IO_ERROR,w->error}; goto query_done; }
        string_slice name={(const char *)plan.name_storage,plan.name_length};
        if (PhysicalAdd(schema,(physical_record){PHYSICAL_PROCEDURE,ir->version,{signature,signature_len},name,{body,body_len}}).tag != PHYSICAL_ADD_OK) { result.tag=QUERY_PHYSICAL_NO_SPACE; goto query_done; }
        SliceFor(w->dependencies,j) {
            if (w->dependencies.ptr[j] && PhysicalAdd(schema,(physical_record){.kind=PHYSICAL_DEPENDENCY,.owner=name,.name=Name(ir,IR_COLLECTION,(u32)j)}).tag != PHYSICAL_ADD_OK) { result.tag=QUERY_PHYSICAL_NO_SPACE; goto query_done; }
        }
query_done:
        if (stream) fclose(stream);
        free(body); free(signature); free(plan.parameters.ptr); free(w->dependencies.ptr);
        if (result.tag != QUERY_PHYSICAL_OK) return result;
    }
    return (query_physical_result){.tag=QUERY_PHYSICAL_OK};
}
