#include "workflow.h"
#include "slice.h"
#include <stdio.h>
#include <string.h>

typedef struct name_buffer { char storage[256]; usize len; } name_buffer;
static string_slice NameView(const name_buffer *name) { return (string_slice){name->storage, name->len}; }
static name_buffer Snake(string_slice name, bool upper)
{
    name_buffer result = {0};
    SliceFor(name, i) {
        char c = name.ptr[i];
        if (c >= 'A' && c <= 'Z') {
            if (i && name.ptr[i - 1] != '_' && result.len < sizeof(result.storage)) result.storage[result.len++] = '_';
            c += 'a' - 'A';
        }
        if (upper && c >= 'a' && c <= 'z') c -= 'a' - 'A';
        if (result.len == sizeof(result.storage)) return (name_buffer){0};
        result.storage[result.len++] = c;
    }
    return result;
}
static void Text(FILE *file, string_slice text) { fwrite(text.ptr, 1, text.len, file); }
static void SnakeName(FILE *file, string_slice name, bool upper) { name_buffer converted = Snake(name, upper); Text(file, NameView(&converted)); }
static name_buffer PascalName(string_slice name)
{
    name_buffer result = {0};
    bool upper = true;
    SliceFor(name, i) {
        char c = name.ptr[i];
        if (c == '_') { upper = true; continue; }
        if (upper && c >= 'a' && c <= 'z') c -= 'a' - 'A';
        if (result.len == sizeof(result.storage)) return (name_buffer){0};
        result.storage[result.len++] = c;
        upper = false;
    }
    return result;
}
static void Pascal(FILE *file, string_slice name) { name_buffer converted = PascalName(name); Text(file, NameView(&converted)); }
static string_slice Route(const declaration_ir *ir, usize r) { return ir->strings.ptr[ir->routes.names.ptr[r]]; }
static string_slice Query(const declaration_ir *ir, usize q) { return ir->strings.ptr[ir->queries.names.ptr[q]]; }
static void Operation(FILE *f, const declaration_ir *ir, usize q)
{
    fputs("BROKER_OPERATION_", f); SnakeName(f, Route(ir, ir->queries.data.ptr[q].a.index), true);
    fputc('_', f); SnakeName(f, Query(ir, q), true);
}
static void QueryType(FILE *f, const declaration_ir *ir, usize q)
{
    SnakeName(f, Route(ir, ir->queries.data.ptr[q].a.index), false);
    fputc('_', f); SnakeName(f, Query(ir, q), false);
}
static void ShapeName(FILE *f, bool selection, usize index) { fprintf(f, "%s_%zu", selection ? "selection" : "query", index); }
static bool FieldOwned(const declaration_ir *ir, usize field, u32 collection)
{ return ir->fields.data.ptr[field].a.table == IR_COLLECTION && ir->fields.data.ptr[field].a.index == collection; }
static string_slice ScalarType(ir_value_kind kind)
{
    switch (kind) {
    case IR_ID: return StringSlice("query_id");
    case IR_STRING: return StringSlice("query_text");
    case IR_BOOLEAN: return StringSlice("query_bool");
    case IR_NUMBER: return StringSlice("query_number");
    case IR_TIMESTAMP: return StringSlice("query_timestamp");
    default: return StringSlice("");
    }
}
static string_slice ScalarTag(ir_value_kind kind)
{
    switch (kind) {
    case IR_ID: return StringSlice("QUERY_ID");
    case IR_STRING: return StringSlice("QUERY_TEXT");
    case IR_BOOLEAN: return StringSlice("QUERY_BOOL");
    case IR_NUMBER: return StringSlice("QUERY_NUMBER");
    case IR_TIMESTAMP: return StringSlice("QUERY_TIMESTAMP");
    default: return StringSlice("");
    }
}
static bool Identifier(string_slice name)
{
    if (!name.len || name.len > 80) return false;
    SliceFor(name, i) {
        char c = name.ptr[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (i && c >= '0' && c <= '9'))) return false;
    }
    return true;
}
static bool Same(string_slice a, string_slice b)
{
    if (a.len != b.len) return false;
    SliceFor(a, i) { if (a.ptr[i] != b.ptr[i]) return false; }
    return true;
}
/* The initial policy compiler admits a single nominal ID equality from a route.
 * Selection filters stay part of the precompiled query; fake results are fixtures. */
static ir_ref Parameter(const declaration_ir *ir, usize query)
{
    ir_ref filter = ir->queries.data.ptr[query].c;
    if (filter.table != IR_EXPRESSION || ir->expressions.kinds.ptr[filter.index] != IR_EQUAL) return (ir_ref){0};
    ir_data equal = ir->expressions.data.ptr[filter.index];
    ir_ref field = equal.a, value = equal.b;
    if (field.table != IR_EXPRESSION || value.table != IR_EXPRESSION ||
        ir->expressions.kinds.ptr[field.index] != IR_FIELD_ACCESS ||
        ir->expressions.kinds.ptr[value.index] != IR_PARSE_PARAMETER || ir->expressions.types.ptr[value.index].kind != IR_ID) return (ir_ref){0};
    ir_ref column = ir->expressions.data.ptr[field.index].b;
    if (column.table != IR_FIELD || !Same(ir->strings.ptr[ir->fields.names.ptr[column.index]], StringSlice("id"))) return (ir_ref){0};
    ir_ref text = ir->expressions.data.ptr[value.index].a;
    if (text.table != IR_EXPRESSION || ir->expressions.kinds.ptr[text.index] != IR_ROUTE_PARAMETER) return (ir_ref){0};
    return ir->expressions.data.ptr[text.index].a;
}
typedef struct route_queries { u32 storage[8]; usize len; bool valid; } route_queries;
static route_queries Queries(const declaration_ir *ir, usize route)
{
    route_queries result = {.valid = true};
    ir_range statements = ir->routes.data.ptr[route].items;
    ir_ref_slice items = {statements.count ? ir->extra.ptr + statements.start : NULL, statements.count};
    SliceFor(items, i) {
        ir_ref statement = items.ptr[i];
        u32 kind = ir->statements.kinds.ptr[statement.index];
        if (kind == IR_RETURN) { result.valid = false; break; }
        ir_ref value = ir->statements.data.ptr[statement.index].a;
        if (kind == IR_LET) value = ir->bindings.data.ptr[value.index].b;
        if (value.table != IR_EXPRESSION || ir->expressions.kinds.ptr[value.index] != IR_RUN_QUERY || result.len == 8) { result.valid = false; break; }
        result.storage[result.len++] = ir->expressions.data.ptr[value.index].a.index;
    }
    return result;
}
static void Row(FILE *f, const declaration_ir *ir, bool selection, usize index, bool header)
{
    ir_data data = selection ? ir->selections.data.ptr[index] : ir->queries.data.ptr[index];
    u32 collection = selection ? ir->relationships.data.ptr[data.a.index].b.index : data.b.index;
    ir_ref_slice selections = {data.items.count ? ir->extra.ptr + data.items.start : NULL, data.items.count};
    if (header) {
        fputs("typedef struct ", f); ShapeName(f, selection, index); fputs("_row {\n", f);
        SliceFor(ir->fields.data, field) {
            if (!FieldOwned(ir, field, collection)) continue;
            fputs("    ", f); Text(f, ScalarType(ir->fields.types.ptr[field].kind)); fputs(" field_", f);
            SnakeName(f, ir->strings.ptr[ir->fields.names.ptr[field]], false); fputs(";\n", f);
        }
        SliceFor(selections, s) {
            u32 rel = ir->selections.data.ptr[selections.ptr[s].index].a.index;
            fputs("    query_records relation_", f); SnakeName(f, ir->strings.ptr[ir->relationships.names.ptr[rel]], false); fputs(";\n", f);
        }
        fputs("} ", f); ShapeName(f, selection, index); fputs("_row;\n\n", f);
        return;
    }
    fputs("static const query_field ", f); ShapeName(f, selection, index); fputs("_fields[] = {\n", f);
    SliceFor(ir->fields.data, field) {
        if (!FieldOwned(ir, field, collection)) continue;
        fputs("    { .name = StringSliceInitializer(\"", f); Text(f, ir->strings.ptr[ir->fields.names.ptr[field]]);
        fputs("\"), .kind = ", f); Text(f, ScalarTag(ir->fields.types.ptr[field].kind));
        fputs(", .offset = offsetof(", f); ShapeName(f, selection, index); fputs("_row, field_", f);
        SnakeName(f, ir->strings.ptr[ir->fields.names.ptr[field]], false);
        fprintf(f, "), .optional = %s },\n", ir->fields.types.ptr[field].optional ? "true" : "false");
    }
    SliceFor(selections, s) {
        u32 sel = selections.ptr[s].index, rel = ir->selections.data.ptr[sel].a.index;
        fputs("    { .name = StringSliceInitializer(\"", f); Text(f, ir->strings.ptr[ir->relationships.names.ptr[rel]]);
        fputs("\"), .kind = QUERY_RELATION, .offset = offsetof(", f); ShapeName(f, selection, index);
        fputs("_row, relation_", f); SnakeName(f, ir->strings.ptr[ir->relationships.names.ptr[rel]], false);
        fprintf(f, "), .optional = %s, .many = %s, .nested = &selection_%u_shape },\n",
            ir->relationships.types.ptr[rel].optional ? "true" : "false", ir->relationships.types.ptr[rel].kind == IR_RECORDS ? "true" : "false", sel);
    }
    fputs("};\nstatic const query_shape ", f); ShapeName(f, selection, index);
    fputs("_shape = {\n    .row_size = sizeof(", f); ShapeName(f, selection, index);
    fputs("_row),\n    .fields = {", f); ShapeName(f, selection, index);
    fputs("_fields, sizeof(", f); ShapeName(f, selection, index);
    fputs("_fields) / sizeof(query_field)},\n};\n\n", f);
}
static void HandlerName(FILE *f, const declaration_ir *ir, usize route, const u32 *query)
{
    fputs("Route", f); Pascal(f, Route(ir, route)); fputc('_', f);
    if (query) { Pascal(f, Query(ir, *query)); fputs("_Result", f); } else fputs("Start", f);
}
static void HandlerType(FILE *f, const declaration_ir *ir, usize route, const u32 *query, bool upper)
{
    fputs(upper ? "ROUTE_" : "route_", f); SnakeName(f, Route(ir, route), upper); fputc('_', f);
    if (query) { SnakeName(f, Query(ir, *query), upper); fputs(upper ? "_RESULT" : "_result", f); }
    else fputs(upper ? "START" : "start", f);
}
static void HandlerDeclaration(FILE *f, const declaration_ir *ir, usize route, const u32 *query, bool next)
{
    fputs("typedef enum ", f); HandlerType(f, ir, route, query, false); fputs("_tag {\n", f);
    if (next) { fputs("    ", f); HandlerType(f, ir, route, query, true); fputs("_QUERY,\n", f); }
    fputs("    ", f); HandlerType(f, ir, route, query, true); fputs("_RESPONSE,\n    ", f);
    HandlerType(f, ir, route, query, true); fputs("_FAILED,\n} ", f); HandlerType(f, ir, route, query, false); fputs("_tag;\n", f);
    fputs("typedef struct ", f); HandlerType(f, ir, route, query, false); fputs("_outcome {\n    ", f);
    HandlerType(f, ir, route, query, false); fputs("_tag tag;\n    const_byte_slice response;\n} ", f);
    HandlerType(f, ir, route, query, false); fputs("_outcome;\n", f);
    HandlerType(f, ir, route, query, false); fputs("_outcome ", f); HandlerName(f, ir, route, query);
    fputs("(route_", f); SnakeName(f, Route(ir, route), false); fputs("_context *context", f);
    if (query) { fputs(", ", f); QueryType(f, ir, *query); fputs("_result result", f); }
    fputs(");\n\n", f);
}
static void RouteHeader(FILE *f, const declaration_ir *ir, usize route)
{
    route_queries queries = Queries(ir, route);
    fputs("/* Generated interface. Implement handlers in src/business; do not edit. */\n#pragma once\n#include \"app-queries.h\"\n#include \"business_handler.h\"\n\n", f);
    fputs("typedef struct route_", f); SnakeName(f, Route(ir, route), false); fputs("_state {\n", f);
    if (!queries.len) fputs("    u32 reserved;\n", f);
    typedef struct u32s { u32 *ptr; usize len; } u32s;
    u32s list = {queries.storage, queries.len};
    SliceFor(list, i) { fputs("    const_byte_slice ", f); SnakeName(f, Query(ir, list.ptr[i]), false); fputs(";\n", f); }
    fputs("} route_", f); SnakeName(f, Route(ir, route), false); fputs("_state;\n\n", f);
    fputs("typedef struct route_", f); SnakeName(f, Route(ir, route), false);
    fputs("_context {\n    bump_allocator *arena;\n    const_byte_slice request;\n    route_", f); SnakeName(f, Route(ir, route), false);
    fputs("_state *state;\n} route_", f); SnakeName(f, Route(ir, route), false); fputs("_context;\n\n", f);
    HandlerDeclaration(f, ir, route, NULL, queries.len != 0);
    SliceFor(list, i) { HandlerDeclaration(f, ir, route, &list.ptr[i], i + 1 < list.len); }
}
static void QueriesHeader(FILE *f, const declaration_ir *ir)
{
    fputs("/* Generated query ABI and synthetic-backend shapes. */\n#pragma once\n#include \"query.h\"\n#include \"app-routes.h\"\n\n", f);
    fputs("enum { BROKER_OPERATION_NONE = 0", f);
    SliceFor(ir->queries.data, q) {
        if (ir->queries.data.ptr[q].a.table != IR_ROUTE) continue;
        fputs(",\n    ", f); Operation(f, ir, q); fprintf(f, " = %zu", q + 1);
    }
    fputs("\n};\n\n", f);
    SliceFor(ir->selections.data, s) { Row(f, ir, true, s, true); }
    SliceFor(ir->queries.data, q) {
        if (ir->queries.data.ptr[q].a.table != IR_ROUTE) continue;
        Row(f, ir, false, q, true);
        fputs("typedef struct ", f); QueryType(f, ir, q);
        fputs("_result {\n    const_byte_slice message;\n    query_result root;\n    const query_shape *shape;\n} ", f);
        QueryType(f, ir, q); fputs("_result;\n\n", f);
    }
    fputs("typedef enum app_query_shape_tag { APP_QUERY_SHAPE_OK, APP_QUERY_SHAPE_UNKNOWN } app_query_shape_tag;\n"
        "typedef struct app_query_shape_result { app_query_shape_tag tag; const query_shape *shape; } app_query_shape_result;\n"
        "app_query_shape_result AppQueryShape(u32 operation);\n", f);
}
static void QueriesSource(FILE *f, const declaration_ir *ir)
{
    fputs("/* Generated read-only query result metadata. */\n#include \"app-queries.h\"\n\n", f);
    SliceFor(ir->selections.data, s) { fprintf(f, "static const query_shape selection_%zu_shape;\n", s); }
    fputc('\n', f);
    SliceFor(ir->selections.data, s) { Row(f, ir, true, s, false); }
    SliceFor(ir->queries.data, q) { if (ir->queries.data.ptr[q].a.table == IR_ROUTE) Row(f, ir, false, q, false); }
    fputs("app_query_shape_result AppQueryShape(u32 operation)\n{\n    switch (operation) {\n", f);
    SliceFor(ir->queries.data, q) {
        if (ir->queries.data.ptr[q].a.table != IR_ROUTE) continue;
        fputs("    case ", f); Operation(f, ir, q);
        fprintf(f, ":\n        return (app_query_shape_result){APP_QUERY_SHAPE_OK, &query_%zu_shape};\n", q);
    }
    fputs("    default:\n        return (app_query_shape_result){.tag = APP_QUERY_SHAPE_UNKNOWN};\n    }\n}\n", f);
}
static void BrokerHeader(FILE *f)
{
    fputs("/* Generated independent broker policy. Synthetic anonymous reads only. */\n#pragma once\n#include \"app-queries.h\"\n\n"
        "typedef enum app_broker_expected_tag { APP_BROKER_EXPECTED_QUERY, APP_BROKER_EXPECTED_DONE, APP_BROKER_EXPECTED_INVALID } app_broker_expected_tag;\n"
        "typedef struct app_broker_expected_result { app_broker_expected_tag tag; u32 operation; } app_broker_expected_result;\n"
        "app_broker_expected_result AppBrokerExpected(u32 route, u32 completed);\n"
        "typedef enum app_broker_arguments_tag { APP_BROKER_ARGUMENTS_OK, APP_BROKER_ARGUMENTS_INVALID_PARAMETER, APP_BROKER_ARGUMENTS_DENIED } app_broker_arguments_tag;\n"
        "typedef struct app_broker_arguments_result { app_broker_arguments_tag tag; query_arguments arguments; } app_broker_arguments_result;\n"
        "app_broker_arguments_result AppBrokerArguments(const_byte_slice request, const app_route_payload *route, query_call call);\n", f);
}
static void BrokerSource(FILE *f, const declaration_ir *ir)
{
    fputs("/* Generated broker policy: mutable state lives only in the broker guest. */\n#include \"app-broker.h\"\n\n"
        "app_broker_expected_result AppBrokerExpected(u32 route, u32 completed)\n{\n    (void)completed;\n    switch (route) {\n", f);
    SliceFor(ir->routes.data, r) {
        route_queries queries = Queries(ir, r);
        fputs("    case APP_ROUTE_", f); Text(f, Route(ir, r)); fputs(":\n        switch (completed) {\n", f);
        typedef struct query_list { u32 *ptr; usize len; } query_list;
        query_list list = {queries.storage, queries.len};
        SliceFor(list, i) {
            fprintf(f, "        case %zu: return (app_broker_expected_result){APP_BROKER_EXPECTED_QUERY, ", i);
            Operation(f, ir, list.ptr[i]); fputs("};\n", f);
        }
        fprintf(f, "        case %zu: return (app_broker_expected_result){.tag = APP_BROKER_EXPECTED_DONE};\n", list.len);
        fputs("        default: break;\n        }\n        break;\n", f);
    }
    fputs("    default: break;\n    }\n    return (app_broker_expected_result){.tag = APP_BROKER_EXPECTED_INVALID};\n}\n\n"
        "app_broker_arguments_result AppBrokerArguments(const_byte_slice request, const app_route_payload *route, query_call call)\n{\n"
        "    app_broker_arguments_result result = {.tag = APP_BROKER_ARGUMENTS_DENIED};\n"
        "    if (!route || !call.sequence) return result;\n"
        "    byte_range capture = {0};\n    switch (call.operation) {\n", f);
    SliceFor(ir->queries.data, q) {
        if (ir->queries.data.ptr[q].a.table != IR_ROUTE) continue;
        u32 r = ir->queries.data.ptr[q].a.index;
        ir_ref parameter = Parameter(ir, q);
        fputs("    case ", f); Operation(f, ir, q); fputs(":\n        if (route->tag != APP_ROUTE_", f); Text(f, Route(ir, r));
        fputs(") return result;\n        capture = route->captures.route_", f); Text(f, Route(ir, r)); fputs(".capture_", f);
        Text(f, ir->strings.ptr[ir->bindings.names.ptr[parameter.index]]); fputs(";\n        break;\n", f);
    }
    fputs("    default: return result;\n    }\n"
        "    const_byte_range_resolve_result text = ConstByteRangeResolve(request, capture);\n"
        "    if (text.tag != CONST_BYTE_RANGE_RESOLVE_OK) return result;\n"
        "    query_id_parse_result parsed = QueryIdParse(text.bytes);\n"
        "    if (parsed.tag != QUERY_ID_PARSE_OK) {\n        result.tag = APP_BROKER_ARGUMENTS_INVALID_PARAMETER;\n        return result;\n    }\n"
        "    result.tag = APP_BROKER_ARGUMENTS_OK;\n"
        "    result.arguments = (query_arguments){call.operation, call.sequence, parsed.value};\n    return result;\n}\n", f);
}
static void BusinessHeader(FILE *f, const declaration_ir *ir)
{
    fputs("/* Generated dispatch state; route implementations are handwritten. */\n#pragma once\n#include \"app-queries.h\"\n#include \"broker_protocol.h\"\n", f);
    SliceFor(ir->routes.data, r) { fputs("#include \"route_", f); SnakeName(f, Route(ir, r), false); fputs(".h\"\n", f); }
    fputs("\ntypedef struct app_business_machine {\n    u32 route, sequence;\n    bool waiting, done;\n    query_call call;\n    union {\n        u32 reserved;\n", f);
    SliceFor(ir->routes.data, r) {
        fputs("        route_", f); SnakeName(f, Route(ir, r), false); fputs("_state route_", f); SnakeName(f, Route(ir, r), false); fputs(";\n", f);
    }
    fputs("    } state;\n} app_business_machine;\n\n"
        "typedef enum app_business_step_tag { APP_BUSINESS_STEP_OK, APP_BUSINESS_STEP_INVALID } app_business_step_tag;\n"
        "typedef struct app_business_step_result { app_business_step_tag tag; u32 type; const_byte_slice output; } app_business_step_result;\n"
        "app_business_step_result AppBusinessStep(app_business_machine *machine, const app_route_payload *route,\n"
        "    const_byte_slice request, u32 event, const_byte_slice input, bump_allocator *arena);\n", f);
}
static void HandlerCall(FILE *f, const declaration_ir *ir, usize route, const u32 *query, const u32 *next)
{
    fputs("            ", f); HandlerType(f, ir, route, query, false); fputs("_outcome handled = ", f);
    HandlerName(f, ir, route, query); fputs("(&context", f);
    if (query) fputs(", value", f);
    fputs(");\n            switch (handled.tag) {\n", f);
    if (next) {
        fputs("            case ", f); HandlerType(f, ir, route, query, true);
        fputs("_QUERY:\n                machine->call.operation = ", f); Operation(f, ir, *next);
        fputs(";\n                machine->call.sequence = ++machine->sequence;\n"
            "                machine->waiting = true;\n"
            "                result.type = BUSINESS_QUERY_REQUEST;\n"
            "                result.output = BytesOf(machine->call);\n                break;\n", f);
    }
    fputs("            case ", f); HandlerType(f, ir, route, query, true);
    fputs("_RESPONSE:\n                machine->done = true;\n                result.type = BUSINESS_RESULT;\n"
        "                result.output = handled.response;\n                break;\n            case ", f);
    HandlerType(f, ir, route, query, true);
    fputs("_FAILED:\n                machine->done = true;\n                result.type = BUSINESS_FAILED;\n                break;\n"
        "            default: return result;\n            }\n            result.tag = APP_BUSINESS_STEP_OK;\n            return result;\n", f);
}
static void BusinessSource(FILE *f, const declaration_ir *ir)
{
    fputs("/* Generated continuations. Only the expected result can resume a handler. */\n#include \"app-business.h\"\n\n"
        "app_business_step_result AppBusinessStep(app_business_machine *machine, const app_route_payload *route,\n"
        "    const_byte_slice request, u32 event, const_byte_slice input, bump_allocator *arena)\n{\n"
        "    app_business_step_result result = {.tag = APP_BUSINESS_STEP_INVALID};\n"
        "    if (!machine || !route || !arena || machine->done) return result;\n"
        "    if (event == BUSINESS_REQUEST) {\n"
        "        if (machine->route || machine->waiting || machine->sequence) return result;\n"
        "        machine->route = route->tag;\n"
        "    } else if (event != BROKER_QUERY_RESULT || !machine->waiting || machine->route != route->tag) return result;\n"
        "    (void)request;\n    (void)input;\n    switch (route->tag) {\n", f);
    SliceFor(ir->routes.data, r) {
        route_queries queries = Queries(ir, r);
        fputs("    case APP_ROUTE_", f); Text(f, Route(ir, r)); fputs(": {\n        route_", f); SnakeName(f, Route(ir, r), false);
        fputs("_context context = {arena, request, &machine->state.route_", f); SnakeName(f, Route(ir, r), false);
        fputs("};\n        if (event == BUSINESS_REQUEST) {\n", f);
        HandlerCall(f, ir, r, NULL, queries.len ? &queries.storage[0] : NULL);
        fputs("        }\n", f);
        typedef struct query_list { u32 *ptr; usize len; } query_list;
        query_list list = {queries.storage, queries.len};
        SliceFor(list, i) {
            u32 q = list.ptr[i];
            fputs("        if (machine->call.operation == ", f); Operation(f, ir, q);
            fprintf(f, " && machine->sequence == %zu) {\n", i + 1);
            fputs("            app_query_shape_result shape = AppQueryShape(", f); Operation(f, ir, q); fputs(");\n", f);
            fputs("            if (shape.tag != APP_QUERY_SHAPE_OK) return result;\n"
                "            query_validate_result checked = QueryValidate(input, machine->call.operation, machine->sequence, shape.shape);\n"
                "            if (checked.tag != QUERY_VALIDATE_OK) return result;\n"
                "            machine->waiting = false;\n            context.state->", f);
            SnakeName(f, Query(ir, q), false); fputs(" = input;\n            ", f); QueryType(f, ir, q);
            fputs("_result value = {input, checked.root, shape.shape};\n", f);
            HandlerCall(f, ir, r, &q, i + 1 < list.len ? &list.ptr[i + 1] : NULL);
            fputs("        }\n", f);
        }
        fputs("        break;\n    }\n", f);
    }
    fputs("    default: break;\n    }\n    return result;\n}\n", f);
}

/* Filesystem/stdio adapter. Names are compiler-owned identifiers, never paths
 * from source. Callers own and clean the staging directory after any failure. */
typedef enum open_tag { OPEN_OK, OPEN_FAILED } open_tag;
typedef struct open_result { open_tag tag; FILE *file; } open_result;
static open_result Open(string_slice directory, string_slice filename)
{
    char path[4096];
    if (directory.len > 3000 || filename.len > 512) return (open_result){.tag = OPEN_FAILED};
    i32 length = snprintf(path, sizeof(path), "%.*s/%.*s", (int)directory.len, directory.ptr, (int)filename.len, filename.ptr);
    if (length < 0 || (usize)length >= sizeof(path)) return (open_result){.tag = OPEN_FAILED};
    FILE *file = fopen(path, "wbx");
    return (open_result){.tag = file ? OPEN_OK : OPEN_FAILED, .file = file};
}
typedef enum close_tag { CLOSE_OK, CLOSE_FAILED } close_tag;
typedef struct close_result { close_tag tag; } close_result;
static close_result Close(FILE *file)
{
    bool okay = !ferror(file) && fflush(file) == 0;
    if (fclose(file) != 0) okay = false;
    return (close_result){.tag = okay ? CLOSE_OK : CLOSE_FAILED};
}
workflow_generate_result WorkflowGenerate(const declaration_ir *ir, string_slice directory)
{
    workflow_generate_result unsupported = {WORKFLOW_GENERATE_UNSUPPORTED,
        StringSlice("workflow subset: at most eight sequential named queries per route, each filtered by id == a route parameter; identifier names and scalar fields required")};
    SliceFor(ir->routes.data, r) {
        if (!Identifier(Route(ir, r)) || !Queries(ir, r).valid) return unsupported;
        name_buffer name = Snake(Route(ir, r), false);
        name_buffer handler = PascalName(Route(ir, r));
        SliceFor(ir->routes.data, other) {
            name_buffer peer = Snake(Route(ir, other), false);
            name_buffer peer_handler = PascalName(Route(ir, other));
            if (other < r && (Same(NameView(&name), NameView(&peer)) || Same(NameView(&handler), NameView(&peer_handler))))
                return (workflow_generate_result){WORKFLOW_GENERATE_UNSUPPORTED, StringSlice("route names collide after C name normalization")};
        }
    }
    SliceFor(ir->queries.data, q) {
        if (ir->queries.data.ptr[q].a.table != IR_ROUTE) continue;
        if (!Identifier(Query(ir, q)) || Parameter(ir, q).table != IR_BINDING) return unsupported;
        name_buffer name = Snake(Query(ir, q), false);
        name_buffer handler = PascalName(Query(ir, q));
        SliceFor(ir->queries.data, other) {
            if (other >= q || ir->queries.data.ptr[other].a.table != IR_ROUTE || ir->queries.data.ptr[other].a.index != ir->queries.data.ptr[q].a.index) continue;
            name_buffer peer = Snake(Query(ir, other), false);
            name_buffer peer_handler = PascalName(Query(ir, other));
            if (Same(NameView(&name), NameView(&peer)) || Same(NameView(&handler), NameView(&peer_handler)))
                return (workflow_generate_result){WORKFLOW_GENERATE_UNSUPPORTED, StringSlice("query names collide after C name normalization")};
        }
    }
    SliceFor(ir->fields.data, field) {
        if (!Identifier(ir->strings.ptr[ir->fields.names.ptr[field]]) || !ScalarType(ir->fields.types.ptr[field].kind).len) return unsupported;
        name_buffer name = Snake(ir->strings.ptr[ir->fields.names.ptr[field]], false);
        SliceFor(ir->fields.data, other) {
            if (other >= field || ir->fields.data.ptr[other].a.index != ir->fields.data.ptr[field].a.index) continue;
            name_buffer peer = Snake(ir->strings.ptr[ir->fields.names.ptr[other]], false);
            if (Same(NameView(&name), NameView(&peer)))
                return (workflow_generate_result){WORKFLOW_GENERATE_UNSUPPORTED, StringSlice("field names collide after C name normalization")};
        }
    }
    SliceFor(ir->selections.data, selection) {
        u32 rel = ir->selections.data.ptr[selection].a.index;
        if (!Identifier(ir->strings.ptr[ir->relationships.names.ptr[rel]])) return unsupported;
    }
    const string_slice names[] = {StringSlice("app-queries.h"), StringSlice("app-queries.c"), StringSlice("app-business.h"), StringSlice("app-business.c"), StringSlice("app-broker.h"), StringSlice("app-broker.c")};
    typedef struct file_names { const string_slice *ptr; usize len; } file_names;
    file_names files = {names, sizeof(names) / sizeof(names[0])};
    SliceFor(files, i) {
        open_result opened = Open(directory, files.ptr[i]);
        if (opened.tag != OPEN_OK) goto io_error;
        FILE *f = opened.file;
        switch (i) {
        case 0: QueriesHeader(f, ir); break;
        case 1: QueriesSource(f, ir); break;
        case 2: BusinessHeader(f, ir); break;
        case 3: BusinessSource(f, ir); break;
        case 4: BrokerHeader(f); break;
        case 5: BrokerSource(f, ir); break;
        }
        if (Close(f).tag != CLOSE_OK) goto io_error;
    }
    SliceFor(ir->routes.data, r) {
        name_buffer name = Snake(Route(ir, r), false);
        char filename[300];
        i32 length = snprintf(filename, sizeof(filename), "route_%.*s.h", (int)name.len, name.storage);
        if (length < 0 || (usize)length >= sizeof(filename)) goto io_error;
        open_result opened = Open(directory, (string_slice){filename, (usize)length});
        if (opened.tag != OPEN_OK) goto io_error;
        FILE *f = opened.file;
        RouteHeader(f, ir, r);
        if (Close(f).tag != CLOSE_OK) goto io_error;
    }
    return (workflow_generate_result){.tag = WORKFLOW_GENERATE_OK};
io_error:
    return (workflow_generate_result){WORKFLOW_GENERATE_IO_ERROR, StringSlice("cannot write generated workflow artifacts")};
}
